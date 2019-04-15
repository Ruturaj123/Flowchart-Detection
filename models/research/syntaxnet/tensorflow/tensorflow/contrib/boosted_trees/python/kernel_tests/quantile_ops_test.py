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
"""Test for checking quantile related ops."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import math
import os
import tempfile

import numpy as np

from tensorflow.contrib.boosted_trees.proto.quantiles_pb2 import QuantileConfig
from tensorflow.contrib.boosted_trees.python.ops import quantile_ops
from tensorflow.python.framework import constant_op
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import ops
from tensorflow.python.framework import sparse_tensor
from tensorflow.python.framework import test_util
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import resources
from tensorflow.python.platform import googletest
from tensorflow.python.training import saver


class QuantileBucketsOpTest(test_util.TensorFlowTestCase):

  def _gen_config(self, eps, num_quantiles):
    config = QuantileConfig()
    config.eps = eps
    config.num_quantiles = num_quantiles
    return config.SerializeToString()

  def testBasicQuantileBuckets(self):
    """Sets up the quantile summary op test as follows.

    Create a batch of 6 examples having a dense and sparse features.
    The data looks like this
    | Instance | instance weights | Dense 0  | Sparse 0
    | 0        |     10           |   1      |
    | 1        |     1            |   2      |    2
    | 2        |     1            |   3      |    3
    | 3        |     1            |   4      |    4
    | 4        |     1            |   4      |    5
    | 5        |     1            |   5      |    6
    """

    dense_float_tensor_0 = constant_op.constant(
        [1, 2, 3, 4, 4, 5], dtype=dtypes.float32)
    sparse_indices_0 = constant_op.constant(
        [[1, 0], [2, 0], [3, 0], [4, 0], [5, 0]], dtype=dtypes.int64)
    sparse_values_0 = constant_op.constant(
        [2, 3, 4, 5, 6], dtype=dtypes.float32)
    sparse_shape_0 = constant_op.constant([6, 1], dtype=dtypes.int64)
    example_weights = constant_op.constant(
        [10, 1, 1, 1, 1, 1], dtype=dtypes.float32)

    with self.test_session():
      config = self._gen_config(0.33, 3)
      dense_buckets, sparse_buckets = quantile_ops.quantile_buckets(
          [dense_float_tensor_0], [sparse_indices_0], [sparse_values_0],
          [sparse_shape_0],
          example_weights=example_weights,
          dense_config=[config],
          sparse_config=[config])

      self.assertAllEqual([1, 3, 5], dense_buckets[0].eval())
      self.assertAllEqual([2, 4, 6.], sparse_buckets[0].eval())

  def testStreamingQuantileBucketsWithVaryingBatch(self):
    """Sets up the quantile summary op test as follows.

    Creates batches examples with different number of inputs in each batch.
    The input values are dense in the range [1 ... N]
    The data looks like this:
    | Batch | Start | InputList
    |   1   |   1   |  [1]
    |   2   |   2   |  [2, 3]
    |   3   |   4   |  [4, 5, 6]
    |   4   |   7   |  [7, 8, 9, 10]
    |   5   |  11   |  [11, 12, 13, 14, 15]
    |   6   |  16   |  [16, 17, 18, 19, 20, 21]
    """

    with self.test_session() as sess:
      accumulator = quantile_ops.QuantileAccumulator(
          init_stamp_token=0, num_quantiles=3, epsilon=0.001, name="q1")
      resources.initialize_resources(resources.shared_resources()).run()
    input_column = array_ops.placeholder(dtypes.float32)
    weights = array_ops.placeholder(dtypes.float32)
    update = accumulator.add_summary(
        stamp_token=0,
        column=input_column,
        example_weights=weights)

    with self.test_session() as sess:
      for i in range(1, 23):
        # start = 1, 2, 4, 7, 11, 16 ... (see comment above)
        start = int((i * (i-1) / 2) + 1)
        sess.run(update,
                 {input_column: range(start, start+i),
                  weights: [1] * i})

    with self.test_session() as sess:
      sess.run(accumulator.flush(stamp_token=0, next_stamp_token=1))
      are_ready_flush, buckets = (accumulator.get_buckets(stamp_token=1))
      buckets, are_ready_flush = (sess.run(
          [buckets, are_ready_flush]))
      self.assertEqual(True, are_ready_flush)
      self.assertAllEqual([1, 86., 170., 253.], buckets)

  def testStreamingQuantileBuckets(self):
    """Sets up the quantile summary op test as follows.

    100 batches of data is added to the accumulator. The batches are in form:
    [0 1 .. 99]
    [100 101 .. 200]
    ...
    [9900 9901 .. 9999]
    All the batches have 1 for all the example weights.
    """
    with self.test_session() as sess:
      accumulator = quantile_ops.QuantileAccumulator(
          init_stamp_token=0, num_quantiles=3, epsilon=0.01, name="q1")
      resources.initialize_resources(resources.shared_resources()).run()
    weight_placeholder = array_ops.placeholder(dtypes.float32)
    dense_placeholder = array_ops.placeholder(dtypes.float32)
    update = accumulator.add_summary(
        stamp_token=0,
        column=dense_placeholder,
        example_weights=weight_placeholder)
    with self.test_session() as sess:
      for i in range(100):
        dense_float = np.linspace(
            i * 100, (i + 1) * 100 - 1, num=100).reshape(-1, 1)
        sess.run(update, {
            dense_placeholder: dense_float,
            weight_placeholder: np.ones(shape=(100, 1), dtype=np.float32)
        })

    with self.test_session() as sess:
      sess.run(accumulator.flush(stamp_token=0, next_stamp_token=1))
      are_ready_flush, buckets = (accumulator.get_buckets(stamp_token=1))
      buckets, are_ready_flush = (sess.run([buckets, are_ready_flush]))
      self.assertEqual(True, are_ready_flush)
      self.assertAllEqual([0, 3335., 6671., 9999.], buckets)

  def testStreamingQuantileBucketsTwoLevel(self):
    """Sets up the quantile summary op test as follows.

    100 batches of data is added to the accumulator. The batches are in form:
    [0 1 .. 99]
    [100 101 .. 200]
    ...
    [9900 9901 .. 9999]
    All the batches have 1 for all the example weights.
    """
    with self.test_session() as sess:
      accumulator = quantile_ops.QuantileAccumulator(
          init_stamp_token=0, num_quantiles=3, epsilon=0.01, name="q1")
      accumulator_2 = quantile_ops.QuantileAccumulator(
          init_stamp_token=0, num_quantiles=3, epsilon=0.01, name="q2")
      resources.initialize_resources(resources.shared_resources()).run()
    weight_placeholder = array_ops.placeholder(dtypes.float32)
    dense_placeholder = array_ops.placeholder(dtypes.float32)
    update = accumulator.add_summary(
        stamp_token=0,
        column=dense_placeholder,
        example_weights=weight_placeholder)
    with self.test_session() as sess:
      for i in range(100):
        dense_float = np.linspace(
            i * 100, (i + 1) * 100 - 1, num=100).reshape(-1, 1)
        sess.run(update, {
            dense_placeholder: dense_float,
            weight_placeholder: np.ones(shape=(100, 1), dtype=np.float32)
        })

    with self.test_session() as sess:
      summary = sess.run(
          accumulator.flush_summary(stamp_token=0, next_stamp_token=1))
      sess.run(
          accumulator_2.add_prebuilt_summary(
              stamp_token=0, summary=constant_op.constant(summary)))
      sess.run(accumulator_2.flush(stamp_token=0, next_stamp_token=1))
      are_ready_flush, buckets = (accumulator_2.get_buckets(stamp_token=1))
      buckets, are_ready_flush = (sess.run([buckets, are_ready_flush]))
      self.assertEqual(True, are_ready_flush)
      self.assertAllEqual([0, 3337., 6677., 9999.], buckets)

  def testSaveRestoreBeforeFlush(self):
    save_dir = os.path.join(self.get_temp_dir(), "save_restore")
    save_path = os.path.join(tempfile.mkdtemp(prefix=save_dir), "hash")

    with self.test_session(graph=ops.Graph()) as sess:
      accumulator = quantile_ops.QuantileAccumulator(
          init_stamp_token=0, num_quantiles=3, epsilon=0.33, name="q0")

      save = saver.Saver()
      resources.initialize_resources(resources.shared_resources()).run()

      sparse_indices_0 = constant_op.constant(
          [[1, 0], [2, 0], [3, 0], [4, 0], [5, 0]], dtype=dtypes.int64)
      sparse_values_0 = constant_op.constant(
          [2.0, 3.0, 4.0, 5.0, 6.0], dtype=dtypes.float32)
      sparse_shape_0 = constant_op.constant([6, 1], dtype=dtypes.int64)
      example_weights = constant_op.constant(
          [10, 1, 1, 1, 1, 1], dtype=dtypes.float32, shape=[6, 1])
      update = accumulator.add_summary(
          stamp_token=0,
          column=sparse_tensor.SparseTensor(sparse_indices_0, sparse_values_0,
                                            sparse_shape_0),
          example_weights=example_weights)
      update.run()
      save.save(sess, save_path)
      reset = accumulator.flush(stamp_token=0, next_stamp_token=1)
      with ops.control_dependencies([reset]):
        are_ready_flush, buckets = (accumulator.get_buckets(stamp_token=1))
      buckets, are_ready_flush = (sess.run([buckets, are_ready_flush]))
      self.assertEqual(True, are_ready_flush)
      self.assertAllEqual([2, 4, 6.], buckets)

    with self.test_session(graph=ops.Graph()) as sess:
      accumulator = quantile_ops.QuantileAccumulator(
          init_stamp_token=0, num_quantiles=3, epsilon=0.33, name="q0")
      save = saver.Saver()

      # Restore the saved values in the parameter nodes.
      save.restore(sess, save_path)
      are_ready_noflush = accumulator.get_buckets(stamp_token=0)[0]
      with ops.control_dependencies([are_ready_noflush]):
        reset = accumulator.flush(stamp_token=0, next_stamp_token=1)

      with ops.control_dependencies([reset]):
        are_ready_flush, buckets = accumulator.get_buckets(stamp_token=1)
      buckets, are_ready_flush, are_ready_noflush = (sess.run(
          [buckets, are_ready_flush, are_ready_noflush]))
      self.assertFalse(are_ready_noflush)
      self.assertTrue(are_ready_flush)
      self.assertAllEqual([2, 4, 6.], buckets)

  def testSaveRestoreAfterFlush(self):
    save_dir = os.path.join(self.get_temp_dir(), "save_restore")
    save_path = os.path.join(tempfile.mkdtemp(prefix=save_dir), "hash")

    with self.test_session(graph=ops.Graph()) as sess:
      accumulator = quantile_ops.QuantileAccumulator(
          init_stamp_token=0, num_quantiles=3, epsilon=0.33, name="q0")

      save = saver.Saver()
      resources.initialize_resources(resources.shared_resources()).run()

      example_weights = constant_op.constant(
          [10, 1, 1, 1, 1, 1], dtype=dtypes.float32, shape=[6, 1])
      dense_float_tensor_0 = constant_op.constant(
          [1, 2, 3, 4, 4, 5], dtype=dtypes.float32, shape=[6, 1])
      update = accumulator.add_summary(
          stamp_token=0,
          column=dense_float_tensor_0,
          example_weights=example_weights)
      update.run()
      reset = accumulator.flush(stamp_token=0, next_stamp_token=1)
      with ops.control_dependencies([reset]):
        are_ready_flush, buckets = (accumulator.get_buckets(stamp_token=1))
      buckets, are_ready_flush = (sess.run([buckets, are_ready_flush]))
      self.assertEqual(True, are_ready_flush)
      self.assertAllEqual([1, 3, 5], buckets)
      save.save(sess, save_path)

    with self.test_session(graph=ops.Graph()) as sess:
      accumulator = quantile_ops.QuantileAccumulator(
          init_stamp_token=0, num_quantiles=3, epsilon=0.33, name="q0")
      save = saver.Saver()

      # Restore the saved values in the parameter nodes.
      save.restore(sess, save_path)
      are_ready_flush, buckets = (accumulator.get_buckets(stamp_token=1))
      buckets, are_ready_flush = (sess.run([buckets, are_ready_flush]))
      self.assertEqual(True, are_ready_flush)
      self.assertAllEqual([1, 3, 5], buckets)

  def testFixedUniform(self):
    """Sets up the quantile summary op test as follows.

    Creates array dividing range [0, 1] to 1<<16 elements equally spaced
    with weight of 1.0.
    """
    dense_float_tensor_0 = constant_op.constant(
        [(1.0 * i) / math.pow(2.0, 16)
         for i in range(0, int(math.pow(2, 16)) + 1)])
    example_weights = constant_op.constant(
        [1] * (int(math.pow(2, 16)) + 1), dtype=dtypes.float32)
    config = self._gen_config(0.1, 10)

    with self.test_session():
      dense_buckets, _ = quantile_ops.quantile_buckets(
          [dense_float_tensor_0], [], [], [],
          example_weights=example_weights,
          dense_config=[config],
          sparse_config=[])
      self.assertAllClose(
          [0] + [(i + 1.0) / 10 for i in range(0, 10)],
          dense_buckets[0].eval(),
          atol=0.1)

  def testFixedNonUniform(self):
    """Sets up the quantile summary op test as follows.

    Creates array dividing range [0, 1] to 1<<16 elements equally spaced
    with weight same as the value.
    """
    dense_float_tensor_0 = constant_op.constant(
        [(1.0 * i) / math.pow(2.0, 16)
         for i in range(0, int(math.pow(2, 16)) + 1)])
    example_weights = constant_op.constant(
        [(1.0 * i) / math.pow(2.0, 16)
         for i in range(0, int(math.pow(2, 16)) + 1)])

    config = self._gen_config(0.1, 10)

    with self.test_session():
      dense_buckets, _ = quantile_ops.quantile_buckets(
          [dense_float_tensor_0], [], [], [],
          example_weights=example_weights,
          dense_config=[config],
          sparse_config=[])
      self.assertAllClose(
          [0] + [math.sqrt((i + 1.0) / 10) for i in range(0, 10)],
          dense_buckets[0].eval(),
          atol=0.1)


class QuantilesOpTest(test_util.TensorFlowTestCase):

  def setUp(self):
    """Sets up the quantile op tests.

    Create a batch of 4 examples having 2 dense and 3 sparse features.
    The data looks like this
    | Instance | Dense 0 | Dense 1 | Sparse 0 | Sparse 1 | Sparse 2
    | 0        |   -0.1  |  -1     |   -2     |   0.1    |
    | 1        |    0.4  |  -15    |   5.5    |          |   2
    | 2        |    3.2  |  18     |   16     |   3      |
    | 3        |    190  |  1000   |   17.5   |  -3      |   4
    Quantiles are:
    Dense 0: (-inf,0.4], (0.4,5], (5, 190]
    Dense 1: (-inf, -9], (-9,15], (15, 1000)
    Sparse 0: (-inf, 5], (5,16], (16, 100]
    Sparse 1: (-inf, 2], (2, 5]
    Sparse 2: (-inf, 100]
    """
    super(QuantilesOpTest, self).setUp()
    self._dense_float_tensor_0 = constant_op.constant(
        [[-0.1], [0.4], [3.2], [190]], dtype=dtypes.float32)
    self._dense_float_tensor_1 = constant_op.constant(
        [[-1], [-15], [18], [1000]], dtype=dtypes.float32)
    # Sparse feature 0
    self._sparse_indices_0 = constant_op.constant([[0, 0], [1, 0], [2, 0],
                                                   [3, 0]])
    self._sparse_values_0 = constant_op.constant([-2, 5.5, 16, 17.5])
    self._sparse_shape_0 = constant_op.constant([4, 1])
    # Sprase feature 1
    self._sparse_indices_1 = constant_op.constant([[0, 0], [2, 0], [3, 0]])
    self._sparse_values_1 = constant_op.constant([0.1, 3, -3])
    self._sparse_shape_1 = constant_op.constant([4, 1])
    # Sprase feature 2
    self._sparse_indices_2 = constant_op.constant([[1, 0], [3, 0]])
    self._sparse_values_2 = constant_op.constant([2, 4], dtype=dtypes.float32)
    self._sparse_shape_2 = constant_op.constant([4, 1])
    # Quantiles
    self._dense_thresholds_0 = [0.4, 5, 190]
    self._dense_thresholds_1 = [-9, 15, 1000]

    self._sparse_thresholds_0 = [5, 16, 100]
    self._sparse_thresholds_1 = [2, 5]
    self._sparse_thresholds_2 = [100]

  def testDenseFeaturesOnly(self):
    with self.test_session():
      dense_quantiles, _ = quantile_ops.quantiles(
          [self._dense_float_tensor_0, self._dense_float_tensor_1], [],
          [self._dense_thresholds_0, self._dense_thresholds_1], [])

      # Dense feature 0
      self.assertAllEqual([0, 0, 1, 2], dense_quantiles[0].eval())
      # Dense feature 1
      self.assertAllEqual([1, 0, 2, 2], dense_quantiles[1].eval())

  def testSparseFeaturesOnly(self):
    with self.test_session():
      _, sparse_quantiles = quantile_ops.quantiles(
          [],
          [self._sparse_values_0, self._sparse_values_1, self._sparse_values_2],
          [], [self._sparse_thresholds_0, self._sparse_thresholds_1,
               self._sparse_thresholds_2])

      # Sparse feature 0
      self.assertAllEqual([0, 1, 1, 2], sparse_quantiles[0].eval())
      # Sparse feature 1
      self.assertAllEqual([0, 1, 0], sparse_quantiles[1].eval())
      # Sparse feature 2
      self.assertAllEqual([0, 0], sparse_quantiles[2].eval())

  def testDenseAndSparseFeatures(self):
    with self.test_session():
      dense_quantiles, sparse_quantiles = quantile_ops.quantiles(
          [self._dense_float_tensor_0, self._dense_float_tensor_1],
          [self._sparse_values_0, self._sparse_values_1, self._sparse_values_2],
          [self._dense_thresholds_0, self._dense_thresholds_1],
          [self._sparse_thresholds_0, self._sparse_thresholds_1,
           self._sparse_thresholds_2])

      # Dense feature 0
      self.assertAllEqual([0, 0, 1, 2], dense_quantiles[0].eval())
      # Dense feature 1
      self.assertAllEqual([1, 0, 2, 2], dense_quantiles[1].eval())
      # Sparse feature 0
      self.assertAllEqual([0, 1, 1, 2], sparse_quantiles[0].eval())
      # Sparse feature 1
      self.assertAllEqual([0, 1, 0], sparse_quantiles[1].eval())
      # Sparse feature 2
      self.assertAllEqual([0, 0], sparse_quantiles[2].eval())

  def testBucketizeWithInputBoundaries(self):
    with self.test_session():
      buckets = quantile_ops.bucketize_with_input_boundaries(
          input=[1, 2, 3, 4, 5],
          boundaries=[3])
      self.assertAllEqual([0, 0, 1, 1, 1], buckets.eval())

  def testBucketizeWithInputBoundaries2(self):
    with self.test_session():
      boundaries = constant_op.constant([3], dtype=dtypes.float32)
      buckets = quantile_ops.bucketize_with_input_boundaries(
          input=[1, 2, 3, 4, 5],
          boundaries=boundaries)
      self.assertAllEqual([0, 0, 1, 1, 1], buckets.eval())

  def testBucketizeWithInputBoundaries3(self):
    with self.test_session():
      b = array_ops.placeholder(dtypes.float32)
      buckets = quantile_ops.bucketize_with_input_boundaries(
          input=[1, 2, 3, 4, 5],
          boundaries=b)
      self.assertAllEqual([0, 1, 1, 2, 2],
                          buckets.eval(feed_dict={b: [2, 4]}))

if __name__ == "__main__":
  googletest.main()
