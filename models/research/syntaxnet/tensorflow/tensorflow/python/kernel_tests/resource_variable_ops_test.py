# Copyright 2016 The TensorFlow Authors. All Rights Reserved.
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
"""Tests for tensorflow.ops.resource_variable_ops."""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np

from tensorflow.python.eager import context
from tensorflow.python.framework import constant_op
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import errors
from tensorflow.python.framework import ops
from tensorflow.python.framework import test_util
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import control_flow_ops
from tensorflow.python.ops import init_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import resource_variable_ops
from tensorflow.python.ops import variable_scope
from tensorflow.python.ops import variables
from tensorflow.python.platform import test


class ResourceVariableOpsTest(test_util.TensorFlowTestCase):

  def testHandleDtypeShapeMatch(self):
    with self.test_session():
      handle = resource_variable_ops.var_handle_op(dtype=dtypes.int32, shape=[])
      with self.assertRaises(ValueError):
        resource_variable_ops.assign_variable_op(
            handle, constant_op.constant(0.0, dtype=dtypes.float32)).run()
      with self.assertRaises(ValueError):
        resource_variable_ops.assign_variable_op(handle,
                                                 constant_op.constant(
                                                     [0],
                                                     dtype=dtypes.int32)).run()
      resource_variable_ops.assign_variable_op(handle,
                                               constant_op.constant(
                                                   0,
                                                   dtype=dtypes.int32)).run()

  def testDtypeSurvivesIdentity(self):
    with self.test_session():
      handle = resource_variable_ops.var_handle_op(dtype=dtypes.int32, shape=[])
      id_handle = array_ops.identity(handle)
      resource_variable_ops.assign_variable_op(id_handle,
                                               constant_op.constant(
                                                   0,
                                                   dtype=dtypes.int32)).run()

  def testCreateRead(self):
    with self.test_session():
      handle = resource_variable_ops.var_handle_op(dtype=dtypes.int32, shape=[])
      resource_variable_ops.assign_variable_op(handle,
                                               constant_op.constant(
                                                   1,
                                                   dtype=dtypes.int32)).run()
      value = resource_variable_ops.read_variable_op(
          handle, dtype=dtypes.int32).eval()
      self.assertAllEqual(1, value)

  def testManyAssigns(self):
    with self.test_session() as session:
      handle = resource_variable_ops.var_handle_op(dtype=dtypes.int32, shape=[])
      create = resource_variable_ops.assign_variable_op(handle,
                                                        constant_op.constant(
                                                            1,
                                                            dtype=dtypes.int32))
      with ops.control_dependencies([create]):
        first_read = resource_variable_ops.read_variable_op(
            handle, dtype=dtypes.int32)
      with ops.control_dependencies([first_read]):
        write = resource_variable_ops.assign_variable_op(
            handle, constant_op.constant(2, dtype=dtypes.int32))
      with ops.control_dependencies([write]):
        second_read = resource_variable_ops.read_variable_op(
            handle, dtype=dtypes.int32)
      f, s = session.run([first_read, second_read])
      self.assertEqual(f, 1)
      self.assertEqual(s, 2)

  def testAssignAdd(self):
    with self.test_session():
      handle = resource_variable_ops.var_handle_op(dtype=dtypes.int32, shape=[])
      resource_variable_ops.assign_variable_op(handle,
                                               constant_op.constant(
                                                   1,
                                                   dtype=dtypes.int32)).run()
      resource_variable_ops.assign_add_variable_op(
          handle, constant_op.constant(1, dtype=dtypes.int32)).run()
      read = resource_variable_ops.read_variable_op(handle, dtype=dtypes.int32)
      self.assertEqual(read.eval(), 2)

  def testScatterAdd(self):
    with self.test_session(use_gpu=True):
      handle = resource_variable_ops.var_handle_op(
          dtype=dtypes.int32, shape=[1, 1])
      resource_variable_ops.assign_variable_op(handle,
                                               constant_op.constant(
                                                   [[1]],
                                                   dtype=dtypes.int32)).run()
      resource_variable_ops.resource_scatter_add(handle, [0],
                                                 constant_op.constant(
                                                     [[2]],
                                                     dtype=dtypes.int32)).run()
      read = resource_variable_ops.read_variable_op(handle, dtype=dtypes.int32)
      self.assertEqual(read.eval(), [[3]])

  def testGPU(self):
    with self.test_session(use_gpu=True) as sess:
      abc = variable_scope.get_variable(
          "abc",
          shape=[1],
          initializer=init_ops.ones_initializer(),
          use_resource=True)

      sess.run(variables.global_variables_initializer())
      self.assertEqual(
          resource_variable_ops.var_is_initialized_op(abc.handle).eval(), True)
      print(sess.run(abc))

  def testConstraintArg(self):
    constraint = lambda x: x
    v = resource_variable_ops.ResourceVariable(
        initial_value=lambda: 1, constraint=constraint)
    self.assertEqual(v.constraint, constraint)

    constraint = 0
    with self.assertRaises(ValueError):
      v = resource_variable_ops.ResourceVariable(
          initial_value=lambda: 1, constraint=constraint)

  def testInitFn(self):
    with self.test_session():
      v = resource_variable_ops.ResourceVariable(
          initial_value=lambda: 1, dtype=dtypes.float32)
      self.assertEqual(v.handle.op.colocation_groups(),
                       v.initializer.inputs[1].op.colocation_groups())

  def testInitFnDtype(self):
    with self.test_session():
      v = resource_variable_ops.ResourceVariable(
          initial_value=lambda: 1, dtype=dtypes.float32)
      self.assertEqual(dtypes.float32, v.value().dtype)

  def testInitFnNoDtype(self):
    with self.test_session():
      v = resource_variable_ops.ResourceVariable(initial_value=lambda: 1)
      self.assertEqual(dtypes.int32, v.value().dtype)

  def testInitializeAllVariables(self):
    with self.test_session():
      v = resource_variable_ops.ResourceVariable(1, dtype=dtypes.float32)
      with self.assertRaises(errors.NotFoundError):
        v.value().eval()
      variables.global_variables_initializer().run()
      self.assertEqual(1.0, v.value().eval())

  def testOperatorOverload(self):
    with self.test_session():
      v = resource_variable_ops.ResourceVariable(1.0)
      variables.global_variables_initializer().run()
      self.assertEqual(2.0, (v + v).eval())

  def testAssignMethod(self):
    with self.test_session():
      v = resource_variable_ops.ResourceVariable(1.0)
      variables.global_variables_initializer().run()
      v.assign(2.0).eval()
      self.assertEqual(2.0, v.value().eval())

  def testLoad(self):
    with self.test_session():
      v = resource_variable_ops.ResourceVariable(1.0)
      variables.global_variables_initializer().run()
      v.load(2.0)
      self.assertEqual(2.0, v.value().eval())

  def testSparseRead(self):
    with self.test_session():
      init_value = np.reshape(np.arange(np.power(4, 3)), (4, 4, 4))
      v = resource_variable_ops.ResourceVariable(
          constant_op.constant(init_value, dtype=dtypes.int32))
      variables.global_variables_initializer().run()

      value = v.sparse_read([0, 3, 1, 2]).eval()
      self.assertAllEqual(init_value[[0, 3, 1, 2], ...], value)

  def testToFromProto(self):
    with self.test_session():
      v = resource_variable_ops.ResourceVariable(1.0)
      variables.global_variables_initializer().run()

      w = resource_variable_ops.ResourceVariable.from_proto(v.to_proto())
      self.assertEquals(2, math_ops.add(w, 1).eval())

  def testAssignAddMethod(self):
    with self.test_session():
      v = resource_variable_ops.ResourceVariable(1.0)
      variables.global_variables_initializer().run()
      v.assign_add(1.0).eval()
      self.assertEqual(2.0, v.value().eval())

  def testAssignSubMethod(self):
    with self.test_session():
      v = resource_variable_ops.ResourceVariable(3.0)
      variables.global_variables_initializer().run()
      v.assign_sub(1.0).eval()
      self.assertEqual(2.0, v.value().eval())

  def testDestroyResource(self):
    with self.test_session() as sess:
      v = resource_variable_ops.ResourceVariable(3.0)
      variables.global_variables_initializer().run()
      self.assertEqual(3.0, v.value().eval())
      sess.run(resource_variable_ops.destroy_resource_op(v.handle))
      with self.assertRaises(errors.NotFoundError):
        v.value().eval()
      # Handle to a resource not actually created.
      handle = resource_variable_ops.var_handle_op(dtype=dtypes.int32, shape=[])
      # Should raise no exception
      sess.run(
          resource_variable_ops.destroy_resource_op(
              handle, ignore_lookup_error=True))

  def testAssignDifferentShapes(self):
    with self.test_session() as sess, variable_scope.variable_scope(
        "foo", use_resource=True):
      var = variable_scope.get_variable("x", shape=[1, 1], dtype=dtypes.float32)
      placeholder = array_ops.placeholder(dtypes.float32)
      assign = var.assign(placeholder)
      sess.run(
          [assign],
          feed_dict={placeholder: np.zeros(shape=[2, 2], dtype=np.float32)})

  def testDtypeAfterFromProto(self):
    v = resource_variable_ops.ResourceVariable(2.0)
    w = resource_variable_ops.ResourceVariable.from_proto(v.to_proto())
    self.assertIsInstance(w.dtype, dtypes.DType)
    self.assertEqual(v.dtype, w.dtype)

  def testCachingDevice(self):
    with ops.device("/job:server/task:1"):
      v = resource_variable_ops.ResourceVariable(
          2.0, caching_device="/job:localhost")
      self.assertEqual("/job:localhost", v.value().device)
      with self.assertRaisesRegexp(ValueError, "No attr named '_class'"):
        _ = v.value().op.get_attr("_class")

    with ops.colocate_with(v.op):
      w = resource_variable_ops.ResourceVariable(
          2.0, caching_device="/job:localhost")
      self.assertEqual("/job:localhost", w.value().device)
      with self.assertRaisesRegexp(ValueError, "No attr named '_class'"):
        _ = w.value().op.get_attr("_class")

  def testSharedName(self):
    with self.test_session():
      v = resource_variable_ops.ResourceVariable(300.0, name="var1")
      v.initializer.run()

      w = resource_variable_ops.var_handle_op(
          dtype=v.dtype.base_dtype, shape=v.get_shape(), shared_name="var1")
      w_read = resource_variable_ops.read_variable_op(w, v.dtype.base_dtype)
      self.assertEqual(300.0, w_read.eval())

      x = resource_variable_ops.var_handle_op(
          dtype=v.dtype.base_dtype, shape=v.get_shape(), shared_name="var1/")
      x_read = resource_variable_ops.read_variable_op(x, v.dtype.base_dtype)
      with self.assertRaisesOpError("Resource .*/var1//.* does not exist"):
        _ = x_read.eval()

  def testSharedNameWithNamescope(self):
    with self.test_session():
      with ops.name_scope("foo"):
        v = resource_variable_ops.ResourceVariable(300.0, name="var1")
        v.initializer.run()

      w = resource_variable_ops.var_handle_op(
          dtype=v.dtype.base_dtype, shape=v.get_shape(), shared_name="foo/var1")
      w_read = resource_variable_ops.read_variable_op(w, v.dtype.base_dtype)
      self.assertEqual(300.0, w_read.eval())

  def testShape(self):
    with self.test_session():
      v = resource_variable_ops.ResourceVariable(
          name="var1", initial_value=array_ops.ones(shape=[10, 20, 35]))
      self.assertEqual("(10, 20, 35)", str(v.shape))
      self.assertEqual("(10, 20, 35)", str(v.get_shape()))
      self.assertEqual("(10, 20, 35)", str(v.value().shape))
      self.assertEqual("(3, 20, 35)", str(v.sparse_read([0, 1, 2]).shape))
      self.assertEqual(
          "<unknown>",
          str(v.sparse_read(array_ops.placeholder(dtypes.int32)).shape))

  def testSetInitialValue(self):
    with self.test_session():
      # Initialize variable with a value different from the initial value passed
      # in the constructor.
      v = resource_variable_ops.ResourceVariable(2.0)
      v.initializer.run(feed_dict={v.initial_value: 3.0})
      self.assertEqual(3.0, v.value().eval())

  def testControlFlowInitialization(self):
    """Expects an error if an initializer is in a control-flow scope."""

    def cond(i, _):
      return i < 10

    def body(i, _):
      zero = array_ops.zeros([], dtype=dtypes.int32)
      v = resource_variable_ops.ResourceVariable(initial_value=zero)
      return (i + 1, v.read_value())

    with self.assertRaisesRegexp(ValueError, "inside a control-flow"):
      control_flow_ops.while_loop(cond, body, [0, 0])


# TODO(agarwal,apassos): Add more comprehensive tests and/or translate the above
# tests to work in both GRAPH and EAGER modes.
# TODO(agarwal): Add tests for sparse_read, scatter_sub
class ResourceVariableOpsEagerTest(test_util.TensorFlowTestCase):

  def testVariable(self):
    with context.eager_mode():
      init = array_ops.ones(shape=[10, 20, 35], dtype=dtypes.int32)
      constraint = lambda x: x
      with ops.name_scope("foo"):
        v = resource_variable_ops.ResourceVariable(
            name="var1",
            initial_value=init,
            caching_device="cpu:0",
            constraint=constraint)
      # Test properties
      self.assertEqual(dtypes.int32, v.dtype)
      self.assertEqual("foo/var1:0", v.name)
      self.assertAllEqual([10, 20, 35], v.shape.as_list())
      self.assertAllEqual(init.device, v.device)
      self.assertTrue(isinstance(v.handle, ops.EagerTensor))
      self.assertEqual(constraint, v.constraint)
      self.assertAllEqual(init.numpy(), v.read_value().numpy())
      self.assertAllEqual(init.numpy(), v.value().numpy())

      # Callable init.
      callable_init = lambda: init * 2
      v2 = resource_variable_ops.ResourceVariable(
          initial_value=callable_init, name="v2")
      self.assertEqual("v2:0", v2.name)
      self.assertAllEqual(2 * init.numpy(), v2.read_value().numpy())

      # Test assign_add.
      new_v2_val = v2.assign_add(v.read_value())
      self.assertAllEqual(v.read_value().numpy() * 3, new_v2_val.numpy())

      # Test assign_sub.
      new_v2_val = v2.assign_sub(v.read_value())
      self.assertAllEqual(v.read_value().numpy() * 2, new_v2_val.numpy())

      # Test assign.
      v2.assign(v.read_value())
      self.assertAllEqual(v.read_value().numpy(), v2.read_value().numpy())

      # Test load
      v2.load(2 * v.read_value())
      self.assertAllEqual(2 * v.read_value().numpy(), v2.read_value().numpy())

      # Test convert_to_tensor
      t = ops.convert_to_tensor(v)
      self.assertAllEqual(t.numpy(), v.read_value().numpy())

      # Test operations
      self.assertAllEqual((v * 2).numpy(), (v + v).numpy())


if __name__ == "__main__":
  test.main()
