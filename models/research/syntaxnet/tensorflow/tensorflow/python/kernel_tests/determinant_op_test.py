# Copyright 2015 The TensorFlow Authors. All Rights Reserved.
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
"""Tests for tensorflow.ops.tf.MatrixDeterminant."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np

from tensorflow.python.client import session
from tensorflow.python.framework import constant_op
from tensorflow.python.framework import ops
from tensorflow.python.ops import control_flow_ops
from tensorflow.python.ops import linalg_ops
from tensorflow.python.platform import test


class DeterminantOpTest(test.TestCase):

  def _compareDeterminantBase(self, matrix_x, tf_ans):
    out = tf_ans.eval()
    shape = matrix_x.shape
    if shape[-1] == 0 and shape[-2] == 0:
      np_ans = np.ones(shape[:-2]).astype(matrix_x.dtype)
    else:
      np_ans = np.array(np.linalg.det(matrix_x)).astype(matrix_x.dtype)
    self.assertShapeEqual(np_ans, tf_ans)
    self.assertAllClose(np_ans, out, atol=5e-5)

  def _compareDeterminant(self, matrix_x):
    with self.test_session(use_gpu=True):
      self._compareDeterminantBase(matrix_x,
                                   linalg_ops.matrix_determinant(matrix_x))

  def testBasic(self):
    # 2x2 matrices
    self._compareDeterminant(np.array([[2., 3.], [3., 4.]]).astype(np.float32))
    self._compareDeterminant(np.array([[0., 0.], [0., 0.]]).astype(np.float32))
    # 5x5 matrices (Eigen forces LU decomposition)
    self._compareDeterminant(
        np.array([[2., 3., 4., 5., 6.], [3., 4., 9., 2., 0.], [
            2., 5., 8., 3., 8.
        ], [1., 6., 7., 4., 7.], [2., 3., 4., 5., 6.]]).astype(np.float32))
    # A multidimensional batch of 2x2 matrices
    self._compareDeterminant(np.random.rand(3, 4, 5, 2, 2).astype(np.float32))

  def testBasicDouble(self):
    # 2x2 matrices
    self._compareDeterminant(np.array([[2., 3.], [3., 4.]]).astype(np.float64))
    self._compareDeterminant(np.array([[0., 0.], [0., 0.]]).astype(np.float64))
    # 5x5 matrices (Eigen forces LU decomposition)
    self._compareDeterminant(
        np.array([[2., 3., 4., 5., 6.], [3., 4., 9., 2., 0.], [
            2., 5., 8., 3., 8.
        ], [1., 6., 7., 4., 7.], [2., 3., 4., 5., 6.]]).astype(np.float64))
    # A multidimensional batch of 2x2 matrices
    self._compareDeterminant(np.random.rand(3, 4, 5, 2, 2).astype(np.float64))

  def testBasicComplex64(self):
    # 2x2 matrices
    self._compareDeterminant(
        np.array([[2., 3.], [3., 4.]]).astype(np.complex64))
    self._compareDeterminant(
        np.array([[0., 0.], [0., 0.]]).astype(np.complex64))
    self._compareDeterminant(
        np.array([[1. + 1.j, 1. - 1.j], [-1. + 1.j, -1. - 1.j]]).astype(
            np.complex64))
    # 5x5 matrices (Eigen forces LU decomposition)
    self._compareDeterminant(
        np.array([[2., 3., 4., 5., 6.], [3., 4., 9., 2., 0.], [
            2., 5., 8., 3., 8.
        ], [1., 6., 7., 4., 7.], [2., 3., 4., 5., 6.]]).astype(np.complex64))
    # A multidimensional batch of 2x2 matrices
    self._compareDeterminant(np.random.rand(3, 4, 5, 2, 2).astype(np.complex64))

  def testBasicComplex128(self):
    # 2x2 matrices
    self._compareDeterminant(
        np.array([[2., 3.], [3., 4.]]).astype(np.complex128))
    self._compareDeterminant(
        np.array([[0., 0.], [0., 0.]]).astype(np.complex128))
    self._compareDeterminant(
        np.array([[1. + 1.j, 1. - 1.j], [-1. + 1.j, -1. - 1.j]]).astype(
            np.complex128))
    # 5x5 matrices (Eigen forces LU decomposition)
    self._compareDeterminant(
        np.array([[2., 3., 4., 5., 6.], [3., 4., 9., 2., 0.], [
            2., 5., 8., 3., 8.
        ], [1., 6., 7., 4., 7.], [2., 3., 4., 5., 6.]]).astype(np.complex128))
    # A multidimensional batch of 2x2 matrices
    self._compareDeterminant(
        np.random.rand(3, 4, 5, 2, 2).astype(np.complex128))

  def testOverflow(self):
    max_double = np.finfo("d").max
    huge_matrix = np.array([[max_double, 0.0], [0.0, max_double]])
    with self.assertRaisesOpError("not finite"):
      self._compareDeterminant(huge_matrix)

  def testNonSquareMatrix(self):
    # When the determinant of a non-square matrix is attempted we should return
    # an error
    with self.assertRaises(ValueError):
      linalg_ops.matrix_determinant(
          np.array([[1., 2., 3.], [3., 5., 4.]]).astype(np.float32))

  def testWrongDimensions(self):
    # The input to the determinant should be a 2-dimensional tensor.
    tensor1 = constant_op.constant([1., 2.])
    with self.assertRaises(ValueError):
      linalg_ops.matrix_determinant(tensor1)

  def testEmpty(self):
    self._compareDeterminant(np.empty([0, 2, 2]))
    self._compareDeterminant(np.empty([2, 0, 0]))


class MatrixDeterminantBenchmark(test.Benchmark):

  sizes = [
      (4, 4),
      (16, 16),
      (256, 256),
      (1024, 1024),
      (513, 4, 4),
      (513, 16, 16),
      (513, 256, 256),
  ]

  def _GenerateData(self, size):
    batch_shape = size[:-2]
    size = size[-2:]
    assert size[0] == size[1]
    n = size[0]
    data = np.ones(size).astype(np.float32) / (
        2.0 * n) + np.diag(np.ones(n).astype(np.float32))
    return np.tile(data, batch_shape + (1, 1))

  def benchmarkMatrixDeterminantOp(self):
    for size in self.sizes:
      data = self._GenerateData(size)

      with ops.Graph().as_default(), session.Session() as sess, ops.device(
          "/cpu:0"):
        d = linalg_ops.matrix_determinant(data)
        self.run_op_benchmark(
            sess,
            control_flow_ops.group(
                d,),
            min_iters=25,
            name="matrix_determinant_cpu_{size}".format(size=size))

      if test.is_gpu_available(True):
        with ops.Graph().as_default(), session.Session() as sess, ops.device(
            "/gpu:0"):
          d = linalg_ops.matrix_determinant(data)
          self.run_op_benchmark(
              sess,
              control_flow_ops.group(
                  d,),
              min_iters=25,
              name="matrix_determinant_gpu_{size}".format(size=size))


if __name__ == "__main__":
  test.main()
