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
"""Utilities for probability distributions."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import functools
import hashlib
import math
import numpy as np

from tensorflow.python.framework import constant_op
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import ops
from tensorflow.python.framework import tensor_shape
from tensorflow.python.framework import tensor_util
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import check_ops
from tensorflow.python.ops import control_flow_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import nn


def assert_close(
    x, y, data=None, summarize=None, message=None, name="assert_close"):
  """Assert that x and y are within machine epsilon of each other.

  Args:
    x: Floating-point `Tensor`
    y: Floating-point `Tensor`
    data: The tensors to print out if the condition is `False`. Defaults to
      error message and first few entries of `x` and `y`.
    summarize: Print this many entries of each tensor.
    message: A string to prefix to the default message.
    name: A name for this operation (optional).

  Returns:
    Op raising `InvalidArgumentError` if |x - y| > machine epsilon.
  """
  message = message or ""
  x = ops.convert_to_tensor(x, name="x")
  y = ops.convert_to_tensor(y, name="y")

  if data is None:
    data = [
        message,
        "Condition x ~= y did not hold element-wise: x = ", x.name, x, "y = ",
        y.name, y
    ]

  if x.dtype.is_integer:
    return check_ops.assert_equal(
        x, y, data=data, summarize=summarize, message=message, name=name)

  with ops.name_scope(name, "assert_close", [x, y, data]):
    tol = np.finfo(x.dtype.as_numpy_dtype).eps
    condition = math_ops.reduce_all(math_ops.less_equal(math_ops.abs(x-y), tol))
    return control_flow_ops.Assert(
        condition, data, summarize=summarize)


def assert_integer_form(
    x, data=None, summarize=None, message=None,
    int_dtype=None, name="assert_integer_form"):
  """Assert that x has integer components (or floats equal to integers).

  Args:
    x: Floating-point `Tensor`
    data: The tensors to print out if the condition is `False`. Defaults to
      error message and first few entries of `x` and `y`.
    summarize: Print this many entries of each tensor.
    message: A string to prefix to the default message.
    int_dtype: A `tf.dtype` used to cast the float to. The default (`None`)
      implies the smallest possible signed int will be used for casting.
    name: A name for this operation (optional).

  Returns:
    Op raising `InvalidArgumentError` if `cast(x, int_dtype) != x`.
  """
  with ops.name_scope(name, values=[x, data]):
    x = ops.convert_to_tensor(x, name="x")
    if x.dtype.is_integer:
      return control_flow_ops.no_op()
    message = message or "{} has non-integer components".format(x.op.name)
    if int_dtype is None:
      try:
        int_dtype = {
            dtypes.float16: dtypes.int16,
            dtypes.float32: dtypes.int32,
            dtypes.float64: dtypes.int64,
        }[x.dtype.base_dtype]
      except KeyError:
        raise TypeError("Unrecognized type {}".format(x.dtype.name))
    return check_ops.assert_equal(
        x, math_ops.cast(math_ops.cast(x, int_dtype), x.dtype),
        data=data, summarize=summarize, message=message, name=name)


def assert_symmetric(matrix):
  matrix_t = array_ops.matrix_transpose(matrix)
  return control_flow_ops.with_dependencies(
      [check_ops.assert_equal(matrix, matrix_t)], matrix)


def embed_check_nonnegative_integer_form(
    x, name="embed_check_nonnegative_integer_form"):
  """Assert x is a non-negative tensor, and optionally of integers."""
  with ops.name_scope(name, values=[x]):
    x = ops.convert_to_tensor(x, name="x")
    assertions = [
        check_ops.assert_non_negative(
            x, message="'{}' must be non-negative.".format(x.op.name)),
    ]
    if not x.dtype.is_integer:
      assertions += [
          assert_integer_form(
              x, message="'{}' cannot contain fractional components.".format(
                  x.op.name)),
      ]
    return control_flow_ops.with_dependencies(assertions, x)


def same_dynamic_shape(a, b):
  """Returns whether a and b have the same dynamic shape.

  Args:
    a: `Tensor`
    b: `Tensor`

  Returns:
    `bool` `Tensor` representing if both tensors have the same shape.
  """
  a = ops.convert_to_tensor(a, name="a")
  b = ops.convert_to_tensor(b, name="b")

  # Here we can't just do math_ops.equal(a.shape, b.shape), since
  # static shape inference may break the equality comparison between
  # shape(a) and shape(b) in math_ops.equal.
  def all_shapes_equal():
    return math_ops.reduce_all(math_ops.equal(
        array_ops.concat([array_ops.shape(a), array_ops.shape(b)], 0),
        array_ops.concat([array_ops.shape(b), array_ops.shape(a)], 0)))

  # One of the shapes isn't fully defined, so we need to use the dynamic
  # shape.
  return control_flow_ops.cond(
      math_ops.equal(array_ops.rank(a), array_ops.rank(b)),
      all_shapes_equal,
      lambda: constant_op.constant(False))


def get_logits_and_probs(logits=None,
                         probs=None,
                         multidimensional=False,
                         validate_args=False,
                         name="get_logits_and_probs"):
  """Converts logit to probabilities (or vice-versa), and returns both.

  Args:
    logits: Floating-point `Tensor` representing log-odds.
    probs: Floating-point `Tensor` representing probabilities.
    multidimensional: Python `bool`, default `False`.
      If `True`, represents whether the last dimension of `logits` or `probs`,
      a `[N1, N2, ...  k]` dimensional tensor, representing the
      logit or probability of `shape[-1]` classes.
    validate_args: Python `bool`, default `False`. When `True`, either assert
      `0 <= probs <= 1` (if not `multidimensional`) or that the last dimension
      of `probs` sums to one.
    name: A name for this operation (optional).

  Returns:
    logits, probs: Tuple of `Tensor`s. If `probs` has an entry that is `0` or
      `1`, then the corresponding entry in the returned logit will be `-Inf` and
      `Inf` respectively.

  Raises:
    ValueError: if neither `probs` nor `logits` were passed in, or both were.
  """
  with ops.name_scope(name, values=[probs, logits]):
    if (probs is None) == (logits is None):
      raise ValueError("Must pass probs or logits, but not both.")

    if probs is None:
      logits = ops.convert_to_tensor(logits, name="logits")
      if not logits.dtype.is_floating:
        raise TypeError("logits must having floating type.")
      # We can early return since we constructed probs and therefore know
      # they're valid.
      if multidimensional:
        if validate_args:
          logits = embed_check_categorical_event_shape(logits)
        return logits, nn.softmax(logits, name="probs")
      return logits, math_ops.sigmoid(logits, name="probs")

    probs = ops.convert_to_tensor(probs, name="probs")
    if not probs.dtype.is_floating:
      raise TypeError("probs must having floating type.")

    if validate_args:
      with ops.name_scope("validate_probs"):
        one = constant_op.constant(1., probs.dtype)
        dependencies = [check_ops.assert_non_negative(probs)]
        if multidimensional:
          probs = embed_check_categorical_event_shape(probs)
          dependencies += [assert_close(math_ops.reduce_sum(probs, -1), one,
                                        message="probs does not sum to 1.")]
        else:
          dependencies += [check_ops.assert_less_equal(
              probs, one, message="probs has components greater than 1.")]
        probs = control_flow_ops.with_dependencies(dependencies, probs)

    with ops.name_scope("logits"):
      if multidimensional:
        # Here we don't compute the multidimensional case, in a manner
        # consistent with respect to the unidimensional case. We do so
        # following the TF convention. Typically, you might expect to see
        # logits = log(probs) - log(probs[pivot]). A side-effect of
        # being consistent with the TF approach is that the unidimensional case
        # implicitly handles the second dimension but the multidimensional case
        # explicitly keeps the pivot dimension.
        return math_ops.log(probs), probs
      return math_ops.log(probs) - math_ops.log1p(-1. * probs), probs


def _is_known_unsigned_by_dtype(dt):
  """Helper returning True if dtype is known to be unsigned."""
  return {
      dtypes.bool: True,
      dtypes.uint8: True,
      dtypes.uint16: True,
  }.get(dt.base_dtype, False)


def _is_known_signed_by_dtype(dt):
  """Helper returning True if dtype is known to be signed."""
  return {
      dtypes.float16: True,
      dtypes.float32: True,
      dtypes.float64: True,
      dtypes.int8: True,
      dtypes.int16: True,
      dtypes.int32: True,
      dtypes.int64: True,
  }.get(dt.base_dtype, False)


def _is_known_dtype(dt):
  """Helper returning True if dtype is known."""
  return _is_known_unsigned_by_dtype(dt) or _is_known_signed_by_dtype(dt)


def _largest_integer_by_dtype(dt):
  """Helper returning the largest integer exactly representable by dtype."""
  if not _is_known_dtype(dt):
    raise TypeError("Unrecognized dtype: {}".format(dt.name))
  if dt.is_floating:
    return int(2**(np.finfo(dt.as_numpy_dtype).nmant + 1))
  if dt.is_integer:
    return np.iinfo(dt.as_numpy_dtype).max
  if dt.base_dtype == dtypes.bool:
    return int(1)
  # We actually can't land here but keep the case for completeness.
  raise TypeError("Unrecognized dtype: {}".format(dt.name))


def _smallest_integer_by_dtype(dt):
  """Helper returning the smallest integer exactly representable by dtype."""
  if not _is_known_dtype(dt):
    raise TypeError("Unrecognized dtype: {}".format(dt.name))
  if _is_known_unsigned_by_dtype(dt):
    return 0
  return -1 * _largest_integer_by_dtype(dt)


def _is_integer_like_by_dtype(dt):
  """Helper returning True if dtype.is_interger or is `bool`."""
  if not _is_known_dtype(dt):
    raise TypeError("Unrecognized dtype: {}".format(dt.name))
  return dt.is_integer or dt.base_dtype == dtypes.bool


def embed_check_categorical_event_shape(
    categorical_param,
    name="embed_check_categorical_event_shape"):
  """Embeds checks that categorical distributions don't have too many classes.

  A categorical-type distribution is one which, e.g., returns the class label
  rather than a one-hot encoding.  E.g., `Categorical(probs)`.

  Since distributions output samples in the same dtype as the parameters, we
  must ensure that casting doesn't lose precision. That is, the
  `parameter.dtype` implies a maximum number of classes. However, since shape is
  `int32` and categorical variables are presumed to be indexes into a `Tensor`,
  we must also ensure that the number of classes is no larger than the largest
  possible `int32` index, i.e., `2**31-1`.

  In other words the number of classes, `K`, must satisfy the following
  condition:

  ```python
  K <= min(
      int(2**31 - 1),  # Largest float as an index.
      {
          dtypes.float16: int(2**11),   # Largest int as a float16.
          dtypes.float32: int(2**24),
          dtypes.float64: int(2**53),
      }.get(categorical_param.dtype.base_dtype, 0))
  ```

  Args:
    categorical_param: Floating-point `Tensor` representing parameters of
      distribution over categories. The rightmost shape is presumed to be the
      number of categories.
    name: A name for this operation (optional).

  Returns:
    categorical_param: Input `Tensor` with appropriate assertions embedded.

  Raises:
    TypeError: if `categorical_param` has an unknown `dtype`.
    ValueError: if we can statically identify `categorical_param` as being too
      large (for being closed under int32/float casting).
  """
  with ops.name_scope(name, values=[categorical_param]):
    x = ops.convert_to_tensor(categorical_param, name="categorical_param")
    # The size must not exceed both of:
    # - The largest possible int32 (since categorical values are presumed to be
    #   indexes into a Tensor).
    # - The largest possible integer exactly representable under the given
    #   floating-point dtype (since we need to cast to/from).
    #
    # The chosen floating-point thresholds are 2**(1 + mantissa_bits).
    # For more details, see:
    # https://en.wikipedia.org/wiki/Floating-point_arithmetic#Internal_representation
    x_dtype = x.dtype.base_dtype
    max_event_size = (_largest_integer_by_dtype(x_dtype)
                      if x_dtype.is_floating else 0)
    if max_event_size is 0:
      raise TypeError("Unable to validate size of unrecognized dtype "
                      "({}).".format(x_dtype.name))
    try:
      x_shape_static = x.get_shape().with_rank_at_least(1)
    except ValueError:
      raise ValueError("A categorical-distribution parameter must have "
                       "at least 1 dimension.")
    if x_shape_static[-1].value is not None:
      event_size = x_shape_static[-1].value
      if event_size < 2:
        raise ValueError("A categorical-distribution parameter must have at "
                         "least 2 events.")
      if event_size > max_event_size:
        raise ValueError(
            "Number of classes exceeds `dtype` precision, i.e., "
            "{} implies shape ({}) cannot exceed {}.".format(
                x_dtype.name, event_size, max_event_size))
      return x
    else:
      event_size = array_ops.shape(x, name="x_shape")[-1]
      return control_flow_ops.with_dependencies([
          check_ops.assert_rank_at_least(
              x, 1, message=("A categorical-distribution parameter must have "
                             "at least 1 dimension.")),
          check_ops.assert_greater_equal(
              array_ops.shape(x)[-1], 2,
              message=("A categorical-distribution parameter must have at "
                       "least 2 events.")),
          check_ops.assert_less_equal(
              event_size, max_event_size,
              message="Number of classes exceeds `dtype` precision, "
                      "i.e., {} dtype cannot exceed {} shape.".format(
                          x_dtype.name, max_event_size)),
      ], x)


def embed_check_integer_casting_closed(
    x,
    target_dtype,
    assert_nonnegative=True,
    name="embed_check_casting_closed"):
  """Ensures integers remain unaffected despite casting to/from int/float types.

  Example integer-types: `uint8`, `int32`, `bool`.
  Example floating-types: `float32`, `float64`.

  The largest possible integer representable by an IEEE754 floating-point is
  `2**(1 + mantissa_bits)` yet the largest possible integer as an int-type is
  `2**(bits - 1) - 1`. This function ensures that a `Tensor` purporting to have
  integer-form values can be cast to some other type without loss of precision.

  The smallest representable integer is the negative of the largest
  representable integer, except for types: `uint8`, `uint16`, `bool`. For these
  types, the smallest representable integer is `0`.

  Args:
    x: `Tensor` representing integer-form values.
    target_dtype: TF `dtype` under which `x` should have identical values.
    assert_nonnegative: `bool` indicating `x` should contain nonnegative values.
    name: A name for this operation (optional).

  Returns:
    x: Input `Tensor` with appropriate assertions embedded.

  Raises:
    TypeError: if `x` is neither integer- nor floating-type.
    TypeError: if `target_dtype` is neither integer- nor floating-type.
    TypeError: if neither `x` nor `target_dtype` are integer-type.
  """

  with ops.name_scope(name, values=[x]):
    x = ops.convert_to_tensor(x, name="x")
    if (not _is_integer_like_by_dtype(x.dtype)
        and not x.dtype.is_floating):
      raise TypeError("{}.dtype must be floating- or "
                      "integer-type.".format(x.dtype.name))
    if (not _is_integer_like_by_dtype(target_dtype)
        and not target_dtype.is_floating):
      raise TypeError("target_dtype ({}) must be floating- or "
                      "integer-type.".format(target_dtype.name))
    if (not _is_integer_like_by_dtype(x.dtype)
        and not _is_integer_like_by_dtype(target_dtype)):
      raise TypeError("At least one of {}.dtype ({}) and target_dtype ({}) "
                      "must be integer-type.".format(
                          x.op.name, x.dtype.name, target_dtype.name))

    assertions = []
    if assert_nonnegative:
      assertions += [
          check_ops.assert_non_negative(
              x, message="Elements must be non-negative."),
      ]

    if x.dtype.is_floating:
      # Being here means _is_integer_like_by_dtype(target_dtype) = True.
      # Since this check implies the magnitude check below, we need only it.
      assertions += [
          assert_integer_form(
              x, int_dtype=target_dtype,
              message="Elements must be {}-equivalent.".format(
                  target_dtype.name)),
      ]
    else:
      if (_largest_integer_by_dtype(x.dtype)
          > _largest_integer_by_dtype(target_dtype)):
        # Cast may lose integer precision.
        assertions += [
            check_ops.assert_less_equal(
                x, _largest_integer_by_dtype(target_dtype),
                message=("Elements cannot exceed {}.".format(
                    _largest_integer_by_dtype(target_dtype)))),
        ]
      if (not assert_nonnegative and
          (_smallest_integer_by_dtype(x.dtype)
           < _smallest_integer_by_dtype(target_dtype))):
        assertions += [
            check_ops.assert_greater_equal(
                x, _smallest_integer_by_dtype(target_dtype),
                message=("Elements cannot be smaller than {}.".format(
                    _smallest_integer_by_dtype(target_dtype)))),
        ]

    if not assertions:
      return x
    return control_flow_ops.with_dependencies(assertions, x)


def log_combinations(n, counts, name="log_combinations"):
  """Multinomial coefficient.

  Given `n` and `counts`, where `counts` has last dimension `k`, we compute
  the multinomial coefficient as:

  ```n! / sum_i n_i!```

  where `i` runs over all `k` classes.

  Args:
    n: Floating-point `Tensor` broadcastable with `counts`. This represents `n`
      outcomes.
    counts: Floating-point `Tensor` broadcastable with `n`. This represents
      counts in `k` classes, where `k` is the last dimension of the tensor.
    name: A name for this operation (optional).

  Returns:
    `Tensor` representing the multinomial coefficient between `n` and `counts`.
  """
  # First a bit about the number of ways counts could have come in:
  # E.g. if counts = [1, 2], then this is 3 choose 2.
  # In general, this is (sum counts)! / sum(counts!)
  # The sum should be along the last dimension of counts. This is the
  # "distribution" dimension. Here n a priori represents the sum of counts.
  with ops.name_scope(name, values=[n, counts]):
    n = ops.convert_to_tensor(n, name="n")
    counts = ops.convert_to_tensor(counts, name="counts")
    total_permutations = math_ops.lgamma(n + 1)
    counts_factorial = math_ops.lgamma(counts + 1)
    redundant_permutations = math_ops.reduce_sum(counts_factorial, axis=[-1])
    return total_permutations - redundant_permutations


def matrix_diag_transform(matrix, transform=None, name=None):
  """Transform diagonal of [batch-]matrix, leave rest of matrix unchanged.

  Create a trainable covariance defined by a Cholesky factor:

  ```python
  # Transform network layer into 2 x 2 array.
  matrix_values = tf.contrib.layers.fully_connected(activations, 4)
  matrix = tf.reshape(matrix_values, (batch_size, 2, 2))

  # Make the diagonal positive. If the upper triangle was zero, this would be a
  # valid Cholesky factor.
  chol = matrix_diag_transform(matrix, transform=tf.nn.softplus)

  # LinearOperatorTriL ignores the upper triangle.
  operator = LinearOperatorTriL(chol)
  ```

  Example of heteroskedastic 2-D linear regression.

  ```python
  # Get a trainable Cholesky factor.
  matrix_values = tf.contrib.layers.fully_connected(activations, 4)
  matrix = tf.reshape(matrix_values, (batch_size, 2, 2))
  chol = matrix_diag_transform(matrix, transform=tf.nn.softplus)

  # Get a trainable mean.
  mu = tf.contrib.layers.fully_connected(activations, 2)

  # This is a fully trainable multivariate normal!
  dist = tf.contrib.distributions.MVNCholesky(mu, chol)

  # Standard log loss. Minimizing this will "train" mu and chol, and then dist
  # will be a distribution predicting labels as multivariate Gaussians.
  loss = -1 * tf.reduce_mean(dist.log_prob(labels))
  ```

  Args:
    matrix:  Rank `R` `Tensor`, `R >= 2`, where the last two dimensions are
      equal.
    transform:  Element-wise function mapping `Tensors` to `Tensors`. To
      be applied to the diagonal of `matrix`. If `None`, `matrix` is returned
      unchanged. Defaults to `None`.
    name:  A name to give created ops.
      Defaults to "matrix_diag_transform".

  Returns:
    A `Tensor` with same shape and `dtype` as `matrix`.
  """
  with ops.name_scope(name, "matrix_diag_transform", [matrix]):
    matrix = ops.convert_to_tensor(matrix, name="matrix")
    if transform is None:
      return matrix
    # Replace the diag with transformed diag.
    diag = array_ops.matrix_diag_part(matrix)
    transformed_diag = transform(diag)
    transformed_mat = array_ops.matrix_set_diag(matrix, transformed_diag)

  return transformed_mat


def rotate_transpose(x, shift, name="rotate_transpose"):
  """Circularly moves dims left or right.

  Effectively identical to:

  ```python
  numpy.transpose(x, numpy.roll(numpy.arange(len(x.shape)), shift))
  ```

  When `validate_args=False` additional graph-runtime checks are
  performed. These checks entail moving data from to GPU to CPU.

  Example:

  ```python
  x = tf.random_normal([1, 2, 3, 4])  # Tensor of shape [1, 2, 3, 4].
  rotate_transpose(x, -1).shape == [2, 3, 4, 1]
  rotate_transpose(x, -2).shape == [3, 4, 1, 2]
  rotate_transpose(x,  1).shape == [4, 1, 2, 3]
  rotate_transpose(x,  2).shape == [3, 4, 1, 2]
  rotate_transpose(x,  7).shape == rotate_transpose(x, 3).shape  # [2, 3, 4, 1]
  rotate_transpose(x, -7).shape == rotate_transpose(x, -3).shape  # [4, 1, 2, 3]
  ```

  Args:
    x: `Tensor`.
    shift: `Tensor`. Number of dimensions to transpose left (shift<0) or
      transpose right (shift>0).
    name: Python `str`. The name to give this op.

  Returns:
    rotated_x: Input `Tensor` with dimensions circularly rotated by shift.

  Raises:
    TypeError: if shift is not integer type.
  """
  with ops.name_scope(name, values=[x, shift]):
    x = ops.convert_to_tensor(x, name="x")
    shift = ops.convert_to_tensor(shift, name="shift")
    # We do not assign back to preserve constant-ness.
    check_ops.assert_integer(shift)
    shift_value_static = tensor_util.constant_value(shift)
    ndims = x.get_shape().ndims
    if ndims is not None and shift_value_static is not None:
      if ndims < 2: return x
      shift_value_static = np.sign(shift_value_static) * (
          abs(shift_value_static) % ndims)
      if shift_value_static == 0: return x
      perm = np.roll(np.arange(ndims), shift_value_static)
      return array_ops.transpose(x, perm=perm)
    else:
      # Consider if we always had a positive shift, and some specified
      # direction.
      # When shifting left we want the new array:
      #   last(x, n-shift) + first(x, shift)
      # and if shifting right then we want:
      #   last(x, shift) + first(x, n-shift)
      # Observe that last(a) == slice(a, n) and first(a) == slice(0, a).
      # Also, we can encode direction and shift as one: direction * shift.
      # Combining these facts, we have:
      #   a = cond(shift<0, -shift, n-shift)
      #   last(x, n-a) + first(x, a) == x[a:n] + x[0:a]
      # Finally, we transform shift by modulo length so it can be specified
      # independently from the array upon which it operates (like python).
      ndims = array_ops.rank(x)
      shift = array_ops.where(math_ops.less(shift, 0),
                              math_ops.mod(-shift, ndims),
                              ndims - math_ops.mod(shift, ndims))
      first = math_ops.range(0, shift)
      last = math_ops.range(shift, ndims)
      perm = array_ops.concat([last, first], 0)
      return array_ops.transpose(x, perm=perm)


def pick_vector(cond,
                true_vector,
                false_vector,
                name="pick_vector"):
  """Picks possibly different length row `Tensor`s based on condition.

  Value `Tensor`s should have exactly one dimension.

  If `cond` is a python Boolean or `tf.constant` then either `true_vector` or
  `false_vector` is immediately returned. I.e., no graph nodes are created and
  no validation happens.

  Args:
    cond: `Tensor`. Must have `dtype=tf.bool` and be scalar.
    true_vector: `Tensor` of one dimension. Returned when cond is `True`.
    false_vector: `Tensor` of one dimension. Returned when cond is `False`.
    name: Python `str`. The name to give this op.

  Example:

  ```python
  pick_vector(tf.less(0, 5), tf.range(10, 12), tf.range(15, 18))  # [10, 11]
  pick_vector(tf.less(5, 0), tf.range(10, 12), tf.range(15, 18))  # [15, 16, 17]
  ```

  Returns:
    true_or_false_vector: `Tensor`.

  Raises:
    TypeError: if `cond.dtype != tf.bool`
    TypeError: if `cond` is not a constant and
      `true_vector.dtype != false_vector.dtype`
  """
  with ops.name_scope(name, values=(cond, true_vector, false_vector)):
    cond = ops.convert_to_tensor(cond, name="cond")
    if cond.dtype != dtypes.bool:
      raise TypeError("%s.dtype=%s which is not %s" %
                      (cond.name, cond.dtype, dtypes.bool))
    cond_value_static = tensor_util.constant_value(cond)
    if cond_value_static is not None:
      return true_vector if cond_value_static else false_vector
    true_vector = ops.convert_to_tensor(true_vector, name="true_vector")
    false_vector = ops.convert_to_tensor(false_vector, name="false_vector")
    if true_vector.dtype != false_vector.dtype:
      raise TypeError(
          "%s.dtype=%s does not match %s.dtype=%s"
          % (true_vector.name, true_vector.dtype,
             false_vector.name, false_vector.dtype))
    n = array_ops.shape(true_vector)[0]
    return array_ops.slice(
        array_ops.concat([true_vector, false_vector], 0),
        [array_ops.where(cond, 0, n)], [array_ops.where(cond, n, -1)])


def gen_new_seed(seed, salt):
  """Generate a new seed, from the given seed and salt."""
  if seed is None:
    return None
  string = (str(seed) + salt).encode("utf-8")
  return int(hashlib.md5(string).hexdigest()[:8], 16) & 0x7FFFFFFF


def fill_lower_triangular(x, validate_args=False, name="fill_lower_triangular"):
  """Creates a (batch of) lower triangular matrix from a vector of inputs.

  If `x.get_shape()` is `[b1, b2, ..., bK, d]` then the output shape is `[b1,
  b2, ..., bK, n, n]` where `n` is such that `d = n(n+1)/2`, i.e.,
  `n = int(0.5 * (math.sqrt(1. + 8. * d) - 1.))`.

  Although the non-batch complexity is O(n**2), large constants and sub-optimal
  vectorization means the complexity of this function is 5x slower than zeroing
  out the upper triangular, i.e., `tf.matrix_band_part(X, -1, 0)`. This
  function becomes competitive only when several matmul/cholesky/etc ops can be
  ellided in constructing the input. Example: wiring a fully connected layer as
  a covariance matrix; this function reduces the final layer by 2x and possibly
  reduces the network arch complexity considerably. In most cases it is better
  to simply build a full matrix and zero out the upper triangular elements,
  e.g., `tril = tf.matrix_band_part(full, -1, 0)`, rather than directly
  construct a lower triangular.

  Warning: This Op is intended for convenience, not efficiency.

  Example:

  ```python
  fill_lower_triangular([1, 2, 3, 4, 5, 6])  # [[1, 0, 0],
                                             #  [2, 3, 0],
                                             #  [4, 5, 6]]
  ```

  For comparison, a pure numpy version of this function can be found in
  `distribution_util_test.py`, function `_fill_lower_triangular`.

  Args:
    x: `Tensor` representing lower triangular elements.
    validate_args: Python `bool`, default `False`. Whether to ensure the shape
      of `x` can be mapped to a lower triangular matrix (controls non-static
      checks only).
    name: Python `str`. The name to give this op.

  Returns:
    tril: `Tensor` with lower triangular elements filled from `x`.

  Raises:
    ValueError: if shape of `x` has static shape which cannot be mapped to a
      lower triangular matrix.
  """
  # TODO(jvdillon): Replace this code with dedicated op when it exists.
  with ops.name_scope(name, values=[x]):
    x = ops.convert_to_tensor(x, name="x")
    if (x.get_shape().ndims is not None and
        x.get_shape()[-1].value is not None):
      d = x.get_shape()[-1].value
      # d = n(n+1)/2 implies n is:
      n = int(0.5 * (math.sqrt(1. + 8. * d) - 1.))
      d_inferred = n * (n + 1) /2
      if d != d_inferred:
        raise ValueError("Input cannot be mapped to a lower triangular; "
                         "n*(n+1)/2 = %d != %d" % (d_inferred, d))
      final_shape = x.get_shape()[:-1].concatenate(
          tensor_shape.TensorShape([n, n]))
    else:
      d = math_ops.cast(array_ops.shape(x)[-1], dtype=dtypes.float32)
      # d = n(n+1)/2 implies n is:
      n = math_ops.cast(0.5 * (dtypes.sqrt(1. + 8. * d) - 1.),
                        dtype=dtypes.int32)
      if validate_args:
        is_valid_input_shape = check_ops.assert_equal(
            n * (n + 1) / 2, d,
            message="Input cannot be mapped to a lower triangular.")
        n = control_flow_ops.with_dependencies([is_valid_input_shape], n)
      final_shape = x.get_shape()[:-1].concatenate(
          tensor_shape.TensorShape([None, None]))

    def tril_ids(n):
      """Internal helper to create vector of linear indices into y."""
      # Build the ids statically; chose 512 because it implies 1MiB.
      if not tensor_util.is_tensor(n) and n <= 512:
        ids = np.arange(n**2, dtype=np.int32)
        rows = (ids / n).astype(np.int32)  # Implicit floor.
        # We need to stop incrementing the index when we encounter
        # upper-triangular elements. The idea here is to compute the
        # lower-right number of zeros then by "symmetry" subtract this from the
        # total number of zeros, n(n-1)/2.
        # Then we note that: n(n-1)/2 - (n-r)*(n-r-1)/2 = r(2n-r-1)/2
        offset = (rows * (2 * n - rows - 1) / 2).astype(np.int32)
        # We could also zero out when (rows < cols) == (rows < ids-n*rows).
        # mask = (ids <= (n + 1) * rows).astype(np.int32)
      else:
        ids = math_ops.range(n**2)
        rows = math_ops.cast(ids / n, dtype=dtypes.int32)
        offset = math_ops.cast(rows * (2 * n - rows - 1) / 2,
                               dtype=dtypes.int32)
      return ids - offset

    # Special-case non-batch case.
    if x.get_shape().ndims == 1:
      y = array_ops.gather(x, array_ops.reshape(tril_ids(n), [n, n]))
      y = array_ops.matrix_band_part(y, -1, 0)
      y.set_shape(y.get_shape().merge_with(final_shape))
      return y

    # Make ids for each batch dim.
    if (x.get_shape().ndims is not None and
        x.get_shape()[:-1].is_fully_defined()):
      batch_shape = np.asarray(x.get_shape()[:-1].as_list(), dtype=np.int32)
      m = np.prod(batch_shape).astype(np.int32)
    else:
      batch_shape = array_ops.shape(x)[:-1]
      m = math_ops.reduce_prod(array_ops.shape(x)[:-1])
    batch_ids = math_ops.range(m)

    # Assemble the tril_ids into batch,tril_id pairs.
    idx = array_ops.stack([
        array_ops.tile(array_ops.expand_dims(batch_ids, 1), [1, n * n]),
        array_ops.tile(array_ops.expand_dims(tril_ids(n), 0), [m, 1])
    ])
    idx = array_ops.transpose(idx, [1, 2, 0])

    # Gather up, reshape, and return.
    y = array_ops.reshape(x, [-1, d])
    y = array_ops.gather_nd(y, idx)
    y = array_ops.reshape(y, array_ops.concat([batch_shape, [n, n]], 0))
    y = array_ops.matrix_band_part(y, -1, 0)
    y.set_shape(y.get_shape().merge_with(final_shape))
    return y


def tridiag(below=None, diag=None, above=None, name=None):
  """Creates a matrix with values set above, below, and on the diagonal.

  Example:

  ```python
  tridiag(below=[1., 2., 3.],
          diag=[4., 5., 6., 7.],
          above=[8., 9., 10.])
  # ==> array([[  4.,   8.,   0.,   0.],
  #            [  1.,   5.,   9.,   0.],
  #            [  0.,   2.,   6.,  10.],
  #            [  0.,   0.,   3.,   7.]], dtype=float32)
  ```

  Warning: This Op is intended for convenience, not efficiency.

  Args:
    below: `Tensor` of shape `[B1, ..., Bb, d-1]` corresponding to the below
      diagonal part. `None` is logically equivalent to `below = 0`.
    diag: `Tensor` of shape `[B1, ..., Bb, d]` corresponding to the diagonal
      part.  `None` is logically equivalent to `diag = 0`.
    above: `Tensor` of shape `[B1, ..., Bb, d-1]` corresponding to the above
      diagonal part.  `None` is logically equivalent to `above = 0`.
    name: Python `str`. The name to give this op.

  Returns:
    tridiag: `Tensor` with values set above, below and on the diagonal.

  Raises:
    ValueError: if all inputs are `None`.
  """

  def _pad(x):
    """Prepends and appends a zero to every vector in a batch of vectors."""
    shape = array_ops.concat([array_ops.shape(x)[:-1], [1]], axis=0)
    z = array_ops.zeros(shape, dtype=x.dtype)
    return array_ops.concat([z, x, z], axis=-1)

  def _add(*x):
    """Adds list of Tensors, ignoring `None`."""
    s = None
    for y in x:
      if y is None:
        continue
      elif s is None:
        s = y
      else:
        s += y
    if s is None:
      raise ValueError("Must specify at least one of `below`, `diag`, `above`.")
    return s

  with ops.name_scope(name, "tridiag", [below, diag, above]):
    if below is not None:
      below = ops.convert_to_tensor(below, name="below")
      below = array_ops.matrix_diag(_pad(below))[..., :-1, 1:]
    if diag is not None:
      diag = ops.convert_to_tensor(diag, name="diag")
      diag = array_ops.matrix_diag(diag)
    if above is not None:
      above = ops.convert_to_tensor(above, name="above")
      above = array_ops.matrix_diag(_pad(above))[..., 1:, :-1]
    # TODO(jvdillon): Consider using scatter_nd instead of creating three full
    # matrices.
    return _add(below, diag, above)


# TODO(jvdillon): Merge this test back into:
# tensorflow/python/ops/softplus_op_test.py
# once TF core is accepting new ops.
def softplus_inverse(x, name=None):
  """Computes the inverse softplus, i.e., x = softplus_inverse(softplus(x)).

  Mathematically this op is equivalent to:

  ```none
  softplus_inverse = log(exp(x) - 1.)
  ```

  Args:
    x: `Tensor`. Non-negative (not enforced), floating-point.
    name: A name for the operation (optional).

  Returns:
    `Tensor`. Has the same type/shape as input `x`.
  """
  with ops.name_scope(name, "softplus_inverse", values=[x]):
    x = ops.convert_to_tensor(x, name="x")
    # We begin by deriving a more numerically stable softplus_inverse:
    # x = softplus(y) = Log[1 + exp{y}], (which means x > 0).
    # ==> exp{x} = 1 + exp{y}                                (1)
    # ==> y = Log[exp{x} - 1]                                (2)
    #       = Log[(exp{x} - 1) / exp{x}] + Log[exp{x}]
    #       = Log[(1 - exp{-x}) / 1] + Log[exp{x}]
    #       = Log[1 - exp{-x}] + x                           (3)
    # (2) is the "obvious" inverse, but (3) is more stable than (2) for large x.
    # For small x (e.g. x = 1e-10), (3) will become -inf since 1 - exp{-x} will
    # be zero. To fix this, we use 1 - exp{-x} approx x for small x > 0.
    #
    # In addition to the numerically stable derivation above, we clamp
    # small/large values to be congruent with the logic in:
    # tensorflow/core/kernels/softplus_op.h
    #
    # Finally, we set the input to one whenever the input is too large or too
    # small. This ensures that no unchosen codepath is +/- inf. This is
    # necessary to ensure the gradient doesn't get NaNs. Recall that the
    # gradient of `where` behaves like `pred*pred_true + (1-pred)*pred_false`
    # thus an `inf` in an unselected path results in `0*inf=nan`. We are careful
    # to overwrite `x` with ones only when we will never actually use this
    # value. Note that we use ones and not zeros since `log(expm1(0.)) = -inf`.
    threshold = np.log(np.finfo(x.dtype.as_numpy_dtype).eps) + 2.
    is_too_small = math_ops.less(x, np.exp(threshold))
    is_too_large = math_ops.greater(x, -threshold)
    too_small_value = math_ops.log(x)
    too_large_value = x
    # This `where` will ultimately be a NOP because we won't select this
    # codepath whenever we used the surrogate `ones_like`.
    x = array_ops.where(math_ops.logical_or(is_too_small, is_too_large),
                        array_ops.ones_like(x), x)
    y = x + math_ops.log(-math_ops.expm1(-x))  # == log(expm1(x))
    return array_ops.where(is_too_small, too_small_value,
                           array_ops.where(is_too_large, too_large_value, y))


# TODO(b/35290280): Add unit-tests.
def dimension_size(x, axis):
  """Returns the size of a specific dimension."""
  # Since tf.gather isn't "constant-in, constant-out", we must first check the
  # static shape or fallback to dynamic shape.
  num_rows = (None if x.get_shape().ndims is None
              else x.get_shape()[axis].value)
  if num_rows is not None:
    return num_rows
  return array_ops.shape(x)[axis]


class AppendDocstring(object):
  """Helper class to promote private subclass docstring to public counterpart.

  Example:

  ```python
  class TransformedDistribution(Distribution):
    @distribution_util.AppendDocstring(
      additional_note="A special note!",
      kwargs_dict={"foo": "An extra arg."})
    def _prob(self, y, foo=None):
      pass
  ```

  In this case, the `AppendDocstring` decorator appends the `additional_note` to
  the docstring of `prob` (not `_prob`) and adds a new `kwargs`
  section with each dictionary item as a bullet-point.

  For a more detailed example, see `TransformedDistribution`.
  """

  def __init__(self, additional_note="", kwargs_dict=None):
    """Initializes the AppendDocstring object.

    Args:
      additional_note: Python string added as additional docstring to public
        version of function.
      kwargs_dict: Python string/string dictionary representing
        specific kwargs expanded from the **kwargs input.

    Raises:
      ValueError: if kwargs_dict.key contains whitespace.
      ValueError: if kwargs_dict.value contains newlines.
    """
    self._additional_note = additional_note
    if kwargs_dict:
      bullets = []
      for key in sorted(kwargs_dict.keys()):
        value = kwargs_dict[key]
        if any(x.isspace() for x in key):
          raise ValueError(
              "Parameter name \"%s\" contains whitespace." % key)
        value = value.lstrip()
        if "\n" in value:
          raise ValueError(
              "Parameter description for \"%s\" contains newlines." % key)
        bullets.append("*  `%s`: %s" % (key, value))
      self._additional_note += ("\n\n##### `kwargs`:\n\n" +
                                "\n".join(bullets))

  def __call__(self, fn):
    @functools.wraps(fn)
    def _fn(*args, **kwargs):
      return fn(*args, **kwargs)
    if _fn.__doc__ is None:
      _fn.__doc__ = self._additional_note
    else:
      _fn.__doc__ += "\n%s" % self._additional_note
    return _fn
