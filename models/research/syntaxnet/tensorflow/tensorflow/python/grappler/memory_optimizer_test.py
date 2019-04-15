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
"""Tests for the swig wrapper tf_optimizer."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.core.protobuf import config_pb2
from tensorflow.core.protobuf import rewriter_config_pb2
from tensorflow.python.client import session
from tensorflow.python.framework import meta_graph
from tensorflow.python.framework import ops
from tensorflow.python.framework import random_seed
from tensorflow.python.grappler import tf_optimizer
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import nn
from tensorflow.python.ops import variable_scope
from tensorflow.python.ops import variables
from tensorflow.python.platform import test
from tensorflow.python.training import training as train


class MemoryOptimizerSwapTest(test.TestCase):
  """Tests the Grappler memory optimizer."""

  def testNoSwapping(self):
    """Make sure the graph is preserved when there is nothing to swap."""
    a = variables.Variable(10, name='a')
    b = variables.Variable(20, name='b')
    c = math_ops.add_n([a, b], name='c')
    d = math_ops.add_n([b, c], name='d')
    train_op = ops.get_collection_ref(ops.GraphKeys.TRAIN_OP)
    train_op.append(d)
    mg = meta_graph.create_meta_graph_def(graph=ops.get_default_graph())
    graph_size = len(mg.graph_def.node)
    nodes = [node.name for node in mg.graph_def.node]

    rewriter_config = rewriter_config_pb2.RewriterConfig(
        disable_model_pruning=True,
        memory_optimization=rewriter_config_pb2.RewriterConfig.MANUAL)
    graph = tf_optimizer.OptimizeGraph(rewriter_config, mg)

    self.assertEqual(len(graph.node), graph_size)
    self.assertItemsEqual([node.name for node in graph.node], nodes)

  def testSimpleSwap(self):
    """Check that the swap annotations are followed."""
    a = variables.Variable(10, name='a')
    b = variables.Variable(20, name='b')
    c = math_ops.add_n([a, b], name='c')
    d = math_ops.add_n([b, c], name='d')
    train_op = ops.get_collection_ref(ops.GraphKeys.TRAIN_OP)
    train_op.append(d)

    d.op.node_def.attr['_swap_to_host'].i = 0

    mg = meta_graph.create_meta_graph_def(graph=ops.get_default_graph())
    graph_size = len(mg.graph_def.node)

    rewriter_config = rewriter_config_pb2.RewriterConfig(
        disable_model_pruning=True,
        memory_optimization=rewriter_config_pb2.RewriterConfig.MANUAL)
    graph = tf_optimizer.OptimizeGraph(rewriter_config, mg)

    self.assertEqual(len(graph.node), graph_size + 2)
    self.assertTrue(
        set([node.name for node in graph.node]) > set(
            ['a', 'b', 'c', 'd', 'swap_in_d_0', 'swap_out_d_0']))
    for node in graph.node:
      if node.name == 'swap_in_d_0':
        self.assertEqual('swap_out_d_0', node.input[0])
        self.assertEqual('^b/read', node.input[1])
      elif node.name == 'swap_out_d_0':
        self.assertEqual('b/read', node.input[0])
      elif node.name == 'd':
        self.assertEqual('swap_in_d_0', node.input[0])
        self.assertEqual('c', node.input[1])


class MemoryOptimizerRecomputeTest(test.TestCase):
  """Tests the Python interface to recomputation rewrites.

  See core/grappler/optimizers/memory_optimizer_test.cc for functional tests.
  """

  def _GetMetaGraph(self, batch_size=14, image_dim=12, optimizer_scope_name=''):
    """A simple layered graph with conv, an intermediate op, and a ReLU."""
    graph = ops.Graph()
    with graph.as_default():
      random_seed.set_random_seed(1)
      current_activation = variable_scope.get_variable(
          name='start', shape=[batch_size, image_dim, image_dim, 5])
      conv_filter = variable_scope.get_variable(
          name='filter', shape=[5, 5, 5, 5])
      for layer_number in range(10):
        with variable_scope.variable_scope('layer_{}'.format(layer_number)):
          after_conv = nn.conv2d(current_activation, conv_filter, [1, 1, 1, 1],
                                 'SAME')
          current_activation = 2. * after_conv
          current_activation = nn.relu(current_activation)
      loss = math_ops.reduce_mean(current_activation)
      with ops.name_scope(optimizer_scope_name):
        optimizer = train.AdamOptimizer(0.001)
        train_op = optimizer.minimize(loss)
      init_op = variables.global_variables_initializer()
      metagraph = train.export_meta_graph()
    return (metagraph, init_op.name, train_op.name, loss.name)

  def testRewritingDefaultGradientNames(self):
    """Tests that rewriting occurs with default gradient names."""
    (original_metagraph, _, _, _) = self._GetMetaGraph()
    rewritten_graph_def = tf_optimizer.OptimizeGraph(
        rewriter_config_pb2.RewriterConfig(
            disable_model_pruning=True,
            memory_optimization=rewriter_config_pb2.RewriterConfig.HEURISTICS),
        original_metagraph)
    self.assertGreater(
        len(rewritten_graph_def.node),
        len(original_metagraph.graph_def.node))
    self.assertEqual(
        0,
        len([node for node in original_metagraph.graph_def.node
             if 'Recomputed/' in node.name]))
    self.assertEqual(
        20,  # Two per layer
        len([node for node in rewritten_graph_def.node
             if 'Recomputed/' in node.name]))

  def testRewritingNameScopedGradientNames(self):
    """Tests that rewriting occurs with non-standard gradient names."""
    (original_metagraph, _, _, _) = self._GetMetaGraph(
        optimizer_scope_name='optimizer')
    rewritten_graph_def = tf_optimizer.OptimizeGraph(
        rewriter_config_pb2.RewriterConfig(
            disable_model_pruning=True,
            memory_optimization=rewriter_config_pb2.RewriterConfig.HEURISTICS,
            memory_optimizer_target_node_name_prefix='optimizer/gradients/'),
        original_metagraph)
    self.assertGreater(
        len(rewritten_graph_def.node),
        len(original_metagraph.graph_def.node))
    self.assertEqual(
        0,
        len([node for node in original_metagraph.graph_def.node
             if 'Recomputed/' in node.name]))
    self.assertEqual(
        20,  # Two per layer
        len([node for node in rewritten_graph_def.node
             if 'Recomputed/' in node.name]))

  def _GetMemoryOptimizerSessionConfig(self):
    rewrite_options = rewriter_config_pb2.RewriterConfig(
        disable_model_pruning=True,
        memory_optimization=rewriter_config_pb2.RewriterConfig.HEURISTICS)
    graph_options = config_pb2.GraphOptions(rewrite_options=rewrite_options)
    return config_pb2.ConfigProto(graph_options=graph_options)

  def _RunMetaGraphWithConfig(
      self, config, metagraph, init_op_name, train_op_name, loss_op_name):
    graph = ops.Graph()
    with graph.as_default():
      train.import_meta_graph(metagraph)
      init_op = graph.get_operation_by_name(init_op_name)
      train_op = graph.get_operation_by_name(train_op_name)
      loss_op = graph.get_tensor_by_name(loss_op_name)
      with session.Session(config=config, graph=graph) as sess:
        sess.run(init_op)
        sess.run(train_op)
        sess.run(train_op)
        return sess.run(loss_op)

  def testRecomputationRewritingNoErrors(self):
    """Tests that graph output is not significantly different with rewriting."""
    (original_metagraph, init_op_name, train_op_name, loss_op_name
    ) = self._GetMetaGraph()
    original_loss = self._RunMetaGraphWithConfig(
        config=config_pb2.ConfigProto(),
        metagraph=original_metagraph,
        init_op_name=init_op_name,
        train_op_name=train_op_name,
        loss_op_name=loss_op_name)
    memory_optimized_loss = self._RunMetaGraphWithConfig(
        config=self._GetMemoryOptimizerSessionConfig(),
        metagraph=original_metagraph,
        init_op_name=init_op_name,
        train_op_name=train_op_name,
        loss_op_name=loss_op_name)
    self.assertAllClose(original_loss, memory_optimized_loss, rtol=1e-4)


if __name__ == '__main__':
  test.main()
