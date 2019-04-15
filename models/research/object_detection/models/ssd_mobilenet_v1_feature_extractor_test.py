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

"""Tests for ssd_mobilenet_v1_feature_extractor."""
import numpy as np
import tensorflow as tf

from object_detection.models import ssd_feature_extractor_test
from object_detection.models import ssd_mobilenet_v1_feature_extractor

slim = tf.contrib.slim


class SsdMobilenetV1FeatureExtractorTest(
    ssd_feature_extractor_test.SsdFeatureExtractorTestBase, tf.test.TestCase):

  def _create_feature_extractor(self, depth_multiplier, pad_to_multiple,
                                is_training=True, batch_norm_trainable=True):
    """Constructs a new feature extractor.

    Args:
      depth_multiplier: float depth multiplier for feature extractor
      pad_to_multiple: the nearest multiple to zero pad the input height and
        width dimensions to.
      is_training: whether the network is in training mode.
      batch_norm_trainable: Whether to update batch norm parameters during
        training or not.
    Returns:
      an ssd_meta_arch.SSDFeatureExtractor object.
    """
    min_depth = 32
    with slim.arg_scope([slim.conv2d], normalizer_fn=slim.batch_norm) as sc:
      conv_hyperparams = sc
    return ssd_mobilenet_v1_feature_extractor.SSDMobileNetV1FeatureExtractor(
        is_training, depth_multiplier, min_depth, pad_to_multiple,
        conv_hyperparams, batch_norm_trainable)

  def test_extract_features_returns_correct_shapes_128(self):
    image_height = 128
    image_width = 128
    depth_multiplier = 1.0
    pad_to_multiple = 1
    expected_feature_map_shape = [(4, 8, 8, 512), (4, 4, 4, 1024),
                                  (4, 2, 2, 512), (4, 1, 1, 256),
                                  (4, 1, 1, 256), (4, 1, 1, 128)]
    self.check_extract_features_returns_correct_shape(
        image_height, image_width, depth_multiplier, pad_to_multiple,
        expected_feature_map_shape)

  def test_extract_features_returns_correct_shapes_299(self):
    image_height = 299
    image_width = 299
    depth_multiplier = 1.0
    pad_to_multiple = 1
    expected_feature_map_shape = [(4, 19, 19, 512), (4, 10, 10, 1024),
                                  (4, 5, 5, 512), (4, 3, 3, 256),
                                  (4, 2, 2, 256), (4, 1, 1, 128)]
    self.check_extract_features_returns_correct_shape(
        image_height, image_width, depth_multiplier, pad_to_multiple,
        expected_feature_map_shape)

  def test_extract_features_returns_correct_shapes_enforcing_min_depth(self):
    image_height = 299
    image_width = 299
    depth_multiplier = 0.5**12
    pad_to_multiple = 1
    expected_feature_map_shape = [(4, 19, 19, 32), (4, 10, 10, 32),
                                  (4, 5, 5, 32), (4, 3, 3, 32),
                                  (4, 2, 2, 32), (4, 1, 1, 32)]
    self.check_extract_features_returns_correct_shape(
        image_height, image_width, depth_multiplier, pad_to_multiple,
        expected_feature_map_shape)

  def test_extract_features_returns_correct_shapes_with_pad_to_multiple(self):
    image_height = 299
    image_width = 299
    depth_multiplier = 1.0
    pad_to_multiple = 32
    expected_feature_map_shape = [(4, 20, 20, 512), (4, 10, 10, 1024),
                                  (4, 5, 5, 512), (4, 3, 3, 256),
                                  (4, 2, 2, 256), (4, 1, 1, 128)]
    self.check_extract_features_returns_correct_shape(
        image_height, image_width, depth_multiplier, pad_to_multiple,
        expected_feature_map_shape)

  def test_extract_features_raises_error_with_invalid_image_size(self):
    image_height = 32
    image_width = 32
    depth_multiplier = 1.0
    pad_to_multiple = 1
    self.check_extract_features_raises_error_with_invalid_image_size(
        image_height, image_width, depth_multiplier, pad_to_multiple)

  def test_preprocess_returns_correct_value_range(self):
    image_height = 128
    image_width = 128
    depth_multiplier = 1
    pad_to_multiple = 1
    test_image = np.random.rand(4, image_height, image_width, 3)
    feature_extractor = self._create_feature_extractor(depth_multiplier,
                                                       pad_to_multiple)
    preprocessed_image = feature_extractor.preprocess(test_image)
    self.assertTrue(np.all(np.less_equal(np.abs(preprocessed_image), 1.0)))

  def test_variables_only_created_in_scope(self):
    depth_multiplier = 1
    pad_to_multiple = 1
    scope_name = 'MobilenetV1'
    self.check_feature_extractor_variables_under_scope(
        depth_multiplier, pad_to_multiple, scope_name)

  def test_nofused_batchnorm(self):
    image_height = 40
    image_width = 40
    depth_multiplier = 1
    pad_to_multiple = 1
    image_placeholder = tf.placeholder(tf.float32,
                                       [1, image_height, image_width, 3])
    feature_extractor = self._create_feature_extractor(depth_multiplier,
                                                       pad_to_multiple)
    preprocessed_image = feature_extractor.preprocess(image_placeholder)
    _ = feature_extractor.extract_features(preprocessed_image)
    self.assertFalse(any(op.type == 'FusedBatchNorm'
                         for op in tf.get_default_graph().get_operations()))

if __name__ == '__main__':
  tf.test.main()
