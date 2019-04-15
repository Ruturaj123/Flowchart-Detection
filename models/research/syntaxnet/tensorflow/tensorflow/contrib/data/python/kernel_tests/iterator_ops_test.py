# Copyright 2017 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================
"""Tests for the experimental input pipeline ops."""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np

from tensorflow.contrib.data.python.ops import dataset_ops
from tensorflow.core.protobuf import config_pb2
from tensorflow.python.client import session
from tensorflow.python.framework import constant_op
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import errors
from tensorflow.python.framework import ops
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import gradients_impl
from tensorflow.python.ops import math_ops
from tensorflow.python.platform import test
from tensorflow.python.training import server_lib


class IteratorTest(test.TestCase):

  def testAttemptingGradientsRaiseExceptions(self):
    component = constant_op.constant([1])
    side = constant_op.constant(0)
    add = lambda x: x + side
    dataset = dataset_ops.Dataset.from_tensor_slices(component).map(add)
    value = dataset.make_one_shot_iterator().get_next()
    with self.assertRaisesRegexp(LookupError, "No gradient defined"):
      gradients_impl.gradients(value, component)
    with self.assertRaisesRegexp(LookupError, "No gradient defined"):
      gradients_impl.gradients(value, side)
    with self.assertRaisesRegexp(LookupError, "No gradient defined"):
      gradients_impl.gradients(value, [component, side])

  def testOneShotIterator(self):
    components = (np.arange(7),
                  np.array([[1, 2, 3]]) * np.arange(7)[:, np.newaxis],
                  np.array(37.0) * np.arange(7))

    def _map_fn(x, y, z):
      return math_ops.square(x), math_ops.square(y), math_ops.square(z)

    iterator = (dataset_ops.Dataset.from_tensor_slices(components).map(_map_fn)
                .repeat(14).make_one_shot_iterator())
    get_next = iterator.get_next()

    self.assertEqual([c.shape[1:] for c in components],
                     [t.shape for t in get_next])

    with self.test_session() as sess:
      for _ in range(14):
        for i in range(7):
          result = sess.run(get_next)
          for component, result_component in zip(components, result):
            self.assertAllEqual(component[i]**2, result_component)
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(get_next)

  def testOneShotIteratorCaptureByValue(self):
    components = (np.arange(7),
                  np.array([[1, 2, 3]]) * np.arange(7)[:, np.newaxis],
                  np.array(37.0) * np.arange(7))
    tensor_components = tuple([ops.convert_to_tensor(c) for c in components])

    def _map_fn(x, y, z):
      return math_ops.square(x), math_ops.square(y), math_ops.square(z)

    iterator = (dataset_ops.Dataset.from_tensor_slices(tensor_components)
                .map(_map_fn).repeat(14).make_one_shot_iterator())
    get_next = iterator.get_next()

    self.assertEqual([c.shape[1:] for c in components],
                     [t.shape for t in get_next])

    with self.test_session() as sess:
      for _ in range(14):
        for i in range(7):
          result = sess.run(get_next)
          for component, result_component in zip(components, result):
            self.assertAllEqual(component[i]**2, result_component)
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(get_next)

  def testOneShotIteratorInsideContainer(self):
    components = (np.arange(7),
                  np.array([[1, 2, 3]]) * np.arange(7)[:, np.newaxis],
                  np.array(37.0) * np.arange(7))

    def within_container():
      def _map_fn(x, y, z):
        return math_ops.square(x), math_ops.square(y), math_ops.square(z)
      iterator = (dataset_ops.Dataset.from_tensor_slices(components)
                  .map(_map_fn).repeat(14).make_one_shot_iterator())
      return iterator.get_next()

    server = server_lib.Server.create_local_server()

    # Create two iterators within unique containers, and run them to
    # make sure that the resources aren't shared.
    #
    # The test below would fail if cname were the same across both
    # sessions.
    for i in range(2):
      with session.Session(server.target) as sess:
        cname = "iteration%d" % i
        with ops.container(cname):
          get_next = within_container()

        for _ in range(14):
          for i in range(7):
            result = sess.run(get_next)
            for component, result_component in zip(components, result):
              self.assertAllEqual(component[i]**2, result_component)
        with self.assertRaises(errors.OutOfRangeError):
          sess.run(get_next)

  def testOneShotIteratorNonBlocking(self):
    dataset = dataset_ops.Dataset.from_tensors([1, 2, 3]).map(lambda x: x * x)
    iterator = dataset.make_one_shot_iterator()
    next_element = iterator.get_next()

    # Create a session with a single thread to ensure that the
    # one-shot iterator initializer does not deadlock.
    config = config_pb2.ConfigProto(inter_op_parallelism_threads=1,
                                    use_per_session_threads=True)
    with session.Session(config=config) as sess:
      self.assertAllEqual([1, 4, 9], sess.run(next_element))
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(next_element)

    # Test with multiple threads invoking the one-shot iterator concurrently.
    with session.Session(config=config) as sess:
      results = []
      def consumer_thread():
        try:
          results.append(sess.run(next_element))
        except errors.OutOfRangeError:
          results.append(None)

      num_threads = 8
      threads = [
          self.checkedThread(consumer_thread) for _ in range(num_threads)]
      for t in threads:
        t.start()
      for t in threads:
        t.join()

      self.assertEqual(num_threads, len(results))
      self.assertEqual(num_threads - 1,
                       len([None for r in results if r is None]))
      self.assertAllEqual([[1, 4, 9]], [r for r in results if r is not None])

  def testOneShotIteratorInitializerFails(self):
    # Define a dataset whose initialization will always fail.
    dataset = dataset_ops.Dataset.from_tensors(
        array_ops.check_numerics(
            constant_op.constant(1.0) / constant_op.constant(0.0), "oops"))
    iterator = dataset.make_one_shot_iterator()
    next_element = iterator.get_next()

    with self.test_session() as sess:
      with self.assertRaisesRegexp(errors.InvalidArgumentError, "oops"):
        sess.run(next_element)

      # Test that subsequent attempts to use the iterator also fail.
      with self.assertRaisesRegexp(errors.InvalidArgumentError, "oops"):
        sess.run(next_element)

    with self.test_session() as sess:
      def consumer_thread():
        with self.assertRaisesRegexp(errors.InvalidArgumentError, "oops"):
          sess.run(next_element)

      num_threads = 8
      threads = [
          self.checkedThread(consumer_thread) for _ in range(num_threads)]
      for t in threads:
        t.start()
      for t in threads:
        t.join()

  def testSimpleSharedResource(self):
    components = (
        np.array(1, dtype=np.int64),
        np.array([1, 2, 3], dtype=np.int64),
        np.array(37.0, dtype=np.float64)
    )

    server = server_lib.Server.create_local_server()

    # Create two non-overlapping sessions that share the same iterator
    # resource on the same server, and verify that an action of the
    # first session (initializing the iterator) is visible in the
    # second session.
    with ops.Graph().as_default():
      iterator = (dataset_ops.Dataset.from_tensors(components)
                  .map(lambda x, y, z: (x, y, z)).make_initializable_iterator(
                      shared_name="shared_iterator"))
      init_op = iterator.initializer
      get_next = iterator.get_next()

      with session.Session(server.target) as sess:
        sess.run(init_op)
        results = sess.run(get_next)
        for component, result_component in zip(components, results):
          self.assertAllEqual(component, result_component)
        with self.assertRaises(errors.OutOfRangeError):
          sess.run(get_next)

        # Re-initialize the iterator in the first session.
        sess.run(init_op)

    with ops.Graph().as_default():
      # Re-define the iterator manually, without defining any of the
      # functions in this graph, to ensure that we are not
      # accidentally redefining functions with the same names in the
      # new graph.
      iterator = dataset_ops.Iterator.from_structure(
          shared_name="shared_iterator",
          output_types=(dtypes.int64, dtypes.int64, dtypes.float64),
          output_shapes=([], [3], []))
      get_next = iterator.get_next()

      with session.Session(server.target) as sess:
        # Use the iterator without re-initializing in the second session.
        results = sess.run(get_next)
        for component, result_component in zip(components, results):
          self.assertAllEqual(component, result_component)
        with self.assertRaises(errors.OutOfRangeError):
          sess.run(get_next)

  def testNotInitializedError(self):
    components = (np.array(1), np.array([1, 2, 3]), np.array(37.0))
    iterator = (dataset_ops.Dataset.from_tensors(components)
                .make_initializable_iterator())
    get_next = iterator.get_next()

    with self.test_session() as sess:
      with self.assertRaisesRegexp(errors.FailedPreconditionError,
                                   "iterator has not been initialized"):
        sess.run(get_next)

  def testReinitializableIterator(self):
    dataset_3 = dataset_ops.Dataset.from_tensors(
        constant_op.constant([1, 2, 3]))
    dataset_4 = dataset_ops.Dataset.from_tensors(
        constant_op.constant([4, 5, 6, 7]))
    iterator = dataset_ops.Iterator.from_structure(dataset_3.output_types,
                                                   [None])

    dataset_3_init_op = iterator.make_initializer(dataset_3)
    dataset_4_init_op = iterator.make_initializer(dataset_4)
    get_next = iterator.get_next()

    self.assertEqual(dataset_3.output_types, iterator.output_types)
    self.assertEqual(dataset_4.output_types, iterator.output_types)
    self.assertEqual([None], iterator.output_shapes.as_list())

    with self.test_session() as sess:
      # The iterator is initially uninitialized.
      with self.assertRaises(errors.FailedPreconditionError):
        sess.run(get_next)

      # Initialize with one dataset.
      sess.run(dataset_3_init_op)
      self.assertAllEqual([1, 2, 3], sess.run(get_next))
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(get_next)

      # Initialize with a different dataset.
      sess.run(dataset_4_init_op)
      self.assertAllEqual([4, 5, 6, 7], sess.run(get_next))
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(get_next)

      # Reinitialize with the first dataset.
      sess.run(dataset_3_init_op)
      self.assertAllEqual([1, 2, 3], sess.run(get_next))
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(get_next)

  def testReinitializableIteratorStaticErrors(self):
    # Non-matching structure for types and shapes.
    with self.assertRaises(TypeError):
      iterator = dataset_ops.Iterator.from_structure((dtypes.int64,
                                                      dtypes.float64), [None])

    # Test validation of dataset argument.
    iterator = dataset_ops.Iterator.from_structure((dtypes.int64,
                                                    dtypes.float64))

    # Incompatible structure.
    with self.assertRaises(ValueError):
      iterator.make_initializer(
          dataset_ops.Dataset.from_tensors(((constant_op.constant(
              [1, 2, 3], dtype=dtypes.int64),), (constant_op.constant(
                  [4., 5., 6., 7.], dtype=dtypes.float64),))))

    # Incompatible types.
    with self.assertRaises(TypeError):
      iterator.make_initializer(
          dataset_ops.Dataset.from_tensors((constant_op.constant(
              [1, 2, 3], dtype=dtypes.int32), constant_op.constant(
                  [4., 5., 6., 7.], dtype=dtypes.float32))))

    # Incompatible shapes.
    iterator = dataset_ops.Iterator.from_structure(
        (dtypes.int64, dtypes.float64), ([None], []))
    with self.assertRaises(TypeError):
      iterator.make_initializer(
          dataset_ops.Dataset.from_tensors((constant_op.constant(
              [1, 2, 3], dtype=dtypes.int64), constant_op.constant(
                  [4., 5., 6., 7.], dtype=dtypes.float64))))

  def testIteratorStringHandle(self):
    dataset_3 = dataset_ops.Dataset.from_tensor_slices([1, 2, 3])
    dataset_4 = dataset_ops.Dataset.from_tensor_slices([10, 20, 30, 40])

    iterator_3 = dataset_3.make_one_shot_iterator()
    iterator_4 = dataset_4.make_one_shot_iterator()

    handle_placeholder = array_ops.placeholder(dtypes.string, shape=[])
    feedable_iterator = dataset_ops.Iterator.from_string_handle(
        handle_placeholder, dataset_3.output_types, dataset_3.output_shapes)
    next_element = feedable_iterator.get_next()

    self.assertEqual(dataset_3.output_types, feedable_iterator.output_types)
    self.assertEqual(dataset_4.output_types, feedable_iterator.output_types)
    self.assertEqual([], feedable_iterator.output_shapes)

    with self.test_session() as sess:
      iterator_3_handle = sess.run(iterator_3.string_handle())
      iterator_4_handle = sess.run(iterator_4.string_handle())

      self.assertEqual(
          10, sess.run(next_element,
                       feed_dict={handle_placeholder: iterator_4_handle}))
      self.assertEqual(
          1, sess.run(next_element,
                      feed_dict={handle_placeholder: iterator_3_handle}))
      self.assertEqual(
          20, sess.run(next_element,
                       feed_dict={handle_placeholder: iterator_4_handle}))
      self.assertEqual(
          2, sess.run(next_element,
                      feed_dict={handle_placeholder: iterator_3_handle}))
      self.assertEqual(
          30, sess.run(next_element,
                       feed_dict={handle_placeholder: iterator_4_handle}))
      self.assertEqual(
          3, sess.run(next_element,
                      feed_dict={handle_placeholder: iterator_3_handle}))
      self.assertEqual(
          40, sess.run(next_element,
                       feed_dict={handle_placeholder: iterator_4_handle}))
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(next_element,
                 feed_dict={handle_placeholder: iterator_3_handle})
      with self.assertRaises(errors.OutOfRangeError):
        sess.run(next_element,
                 feed_dict={handle_placeholder: iterator_4_handle})

  def testIteratorStringHandleError(self):
    dataset_int_scalar = (dataset_ops.Dataset.from_tensor_slices([1, 2,
                                                                  3]).repeat())
    dataset_float_vector = (dataset_ops.Dataset.from_tensors([1.0, 2.0, 3.0]))

    handle_placeholder = array_ops.placeholder(dtypes.string, shape=[])

    feedable_int_scalar = dataset_ops.Iterator.from_string_handle(
        handle_placeholder, dtypes.int32, [])
    feedable_int_vector = dataset_ops.Iterator.from_string_handle(
        handle_placeholder, dtypes.int32, [None])
    feedable_int_any = dataset_ops.Iterator.from_string_handle(
        handle_placeholder, dtypes.int32)

    with self.test_session() as sess:
      handle_int_scalar = sess.run(
          dataset_int_scalar.make_one_shot_iterator().string_handle())
      handle_float_vector = sess.run(
          dataset_float_vector.make_one_shot_iterator().string_handle())

      self.assertEqual(1,
                       sess.run(
                           feedable_int_scalar.get_next(),
                           feed_dict={handle_placeholder: handle_int_scalar}))

      self.assertEqual(2,
                       sess.run(
                           feedable_int_any.get_next(),
                           feed_dict={handle_placeholder: handle_int_scalar}))

      with self.assertRaises(errors.InvalidArgumentError):
        print(sess.run(
            feedable_int_vector.get_next(),
            feed_dict={handle_placeholder: handle_int_scalar}))

      with self.assertRaises(errors.InvalidArgumentError):
        print(sess.run(
            feedable_int_vector.get_next(),
            feed_dict={handle_placeholder: handle_float_vector}))


if __name__ == "__main__":
  test.main()
