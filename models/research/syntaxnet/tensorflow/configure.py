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
"""configure script to get build parameters from user."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import errno
import os
import platform
import re
import site
import subprocess
import sys

_TF_BAZELRC = '.tf_configure.bazelrc'
_DEFAULT_CUDA_VERSION = '8.0'
_DEFAULT_CUDNN_VERSION = '6'
_DEFAULT_CUDA_COMPUTE_CAPABILITIES = '3.5,5.2'
_DEFAULT_CUDA_PATH = '/usr/local/cuda'
_DEFAULT_CUDA_PATH_LINUX = '/opt/cuda'
_DEFAULT_CUDA_PATH_WIN = ('C:/Program Files/NVIDIA GPU Computing '
                          'Toolkit/CUDA/v%s' % _DEFAULT_CUDA_VERSION)
_TF_OPENCL_VERSION = '1.2'
_DEFAULT_COMPUTECPP_TOOLKIT_PATH = '/usr/local/computecpp'


def is_windows():
  return platform.system() == 'Windows'


def is_linux():
  return platform.system() == 'Linux'


def is_macos():
  return platform.system() == 'Darwin'


def is_ppc64le():
  return platform.machine() == 'ppc64le'


def get_input(question):
  try:
    try:
      answer = raw_input(question)
    except NameError:
      answer = input(question)  # pylint: disable=bad-builtin
  except EOFError:
    answer = ''
  return answer


def symlink_force(target, link_name):
  """Force symlink, equivalent of 'ln -sf'.

  Args:
    target: items to link to.
    link_name: name of the link.
  """
  try:
    os.symlink(target, link_name)
  except OSError as e:
    if e.errno == errno.EEXIST:
      os.remove(link_name)
      os.symlink(target, link_name)
    else:
      raise e


def sed_in_place(filename, old, new):
  """Replace old string with new string in file.

  Args:
    filename: string for filename.
    old: string to replace.
    new: new string to replace to.
  """
  with open(filename, 'r') as f:
    filedata = f.read()
  newdata = filedata.replace(old, new)
  with open(filename, 'w') as f:
    f.write(newdata)


def remove_line_with(filename, token):
  """Remove lines that contain token from file.

  Args:
    filename: string for filename.
    token: string token to check if to remove a line from file or not.
  """
  with open(filename, 'r') as f:
    filedata = f.read()

  with open(filename, 'w') as f:
    for line in filedata.strip().split('\n'):
      if token not in line:
        f.write(line + '\n')


def write_to_bazelrc(line):
  with open(_TF_BAZELRC, 'a') as f:
    f.write(line + '\n')


def write_action_env_to_bazelrc(var_name, var):
  write_to_bazelrc('build --action_env %s="%s"' % (var_name, str(var)))


def run_shell(cmd):
  return subprocess.check_output(cmd, shell=True).decode('UTF-8').strip()


def cygpath(path):
  """Convert path from posix to windows."""
  return run_shell('cygpath  -m "%s"' % path)


def get_python_path(environ_cp):
  """Get the python site package paths."""
  python_paths = []
  if environ_cp.get('PYTHONPATH'):
    python_paths = environ_cp.get('PYTHONPATH').split(':')
  try:
    library_paths = site.getsitepackages()
  except AttributeError:
    from distutils.sysconfig import get_python_lib  # pylint: disable=g-import-not-at-top
    library_paths = [get_python_lib()]
  all_paths = set(python_paths + library_paths)

  paths = []
  for path in all_paths:
    if os.path.isdir(path):
      paths.append(path)
  return paths


def setup_python(environ_cp, bazel_version):
  """Setup python related env variables."""
  # Get PYTHON_BIN_PATH, default is the current running python.
  default_python_bin_path = sys.executable
  ask_python_bin_path = ('Please specify the location of python. [Default is '
                         '%s]: ') % default_python_bin_path
  while True:
    python_bin_path = get_from_env_or_user_or_default(
        environ_cp, 'PYTHON_BIN_PATH', ask_python_bin_path,
        default_python_bin_path)
    # Check if the path is valid
    if (os.path.isfile(python_bin_path) and os.access(
        python_bin_path, os.X_OK)) or (os.path.isdir(python_bin_path)):
      break
    elif not os.path.exists(python_bin_path):
      print('Invalid python path: %s cannot be found.' % python_bin_path)
    else:
      print('%s is not executable.  Is it the python binary?' % python_bin_path)
    environ_cp['PYTHON_BIN_PATH'] = ''

  # Get PYTHON_LIB_PATH
  python_lib_path = environ_cp.get('PYTHON_LIB_PATH')
  if not python_lib_path:
    python_lib_paths = get_python_path(environ_cp)
    if environ_cp.get('USE_DEFAULT_PYTHON_LIB_PATH') == '1':
      python_lib_path = python_lib_paths[0]
    else:
      print('Found possible Python library paths:\n%s' %
            '\n'.join(python_lib_paths))
      default_python_lib_path = python_lib_paths[0]
      python_lib_path = get_input(
          'Please input the desired Python library path to use.  Default is %s'
          % python_lib_paths[0])
      if not python_lib_path:
        python_lib_path = default_python_lib_path
    environ_cp['PYTHON_LIB_PATH'] = python_lib_path

  python_major_version = sys.version_info[0]
  # Convert python path to Windows style before writing into bazel.rc
  if is_windows():
    python_bin_path = cygpath(python_bin_path)
    python_lib_path = cygpath(python_lib_path)

  # Set-up env variables used by python_configure.bzl
  write_action_env_to_bazelrc('PYTHON_BIN_PATH', python_bin_path)
  write_action_env_to_bazelrc('PYTHON_LIB_PATH', python_lib_path)
  write_to_bazelrc('build --define PYTHON_BIN_PATH="%s"' % python_bin_path)
  write_to_bazelrc('build --define PYTHON_LIB_PATH="%s"' % python_lib_path)
  write_to_bazelrc('build --force_python=py%s' % python_major_version)
  write_to_bazelrc('build --host_force_python=py%s' % python_major_version)
  bazel_version_int = convert_version_to_int(bazel_version)
  version_0_5_3_int = convert_version_to_int('0.5.3')
  # If bazel_version_int is None, we are testing a release Bazel, then the
  # version should be higher than 0.5.3
  # TODO(pcloudy): remove this after required min bazel version is higher
  # than 0.5.3
  if not bazel_version_int or bazel_version_int >= version_0_5_3_int:
    write_to_bazelrc('build --python_path=\"%s"' % python_bin_path)
  else:
    write_to_bazelrc('build --python%s_path=\"%s"' % (python_major_version,
                                                      python_bin_path))
  write_to_bazelrc('test --force_python=py%s' % python_major_version)
  write_to_bazelrc('test --host_force_python=py%s' % python_major_version)
  write_to_bazelrc('test --define PYTHON_BIN_PATH="%s"' % python_bin_path)
  write_to_bazelrc('test --define PYTHON_LIB_PATH="%s"' % python_lib_path)
  write_to_bazelrc('run --define PYTHON_BIN_PATH="%s"' % python_bin_path)
  write_to_bazelrc('run --define PYTHON_LIB_PATH="%s"' % python_lib_path)
  environ_cp['PYTHON_BIN_PATH'] = python_bin_path

  # Write tools/python_bin_path.sh
  with open('tools/python_bin_path.sh', 'w') as f:
    f.write('export PYTHON_BIN_PATH="%s"' % python_bin_path)


def reset_tf_configure_bazelrc():
  """Reset file that contains customized config settings."""
  open(_TF_BAZELRC, 'w').close()

  home = os.path.expanduser('~')
  if not os.path.exists('.bazelrc'):
    if os.path.exists(os.path.join(home, '.bazelrc')):
      with open('.bazelrc', 'a') as f:
        f.write('import %s/.bazelrc\n' % home)
    else:
      open('.bazelrc', 'w').close()

  remove_line_with('.bazelrc', 'tf_configure')
  with open('.bazelrc', 'a') as f:
    f.write('import %workspace%/.tf_configure.bazelrc\n')


def run_gen_git_source(environ_cp):
  """Run the gen_git_source to create links.

  The links are for bazel to track dependencies for git hash propagation.

  Args:
    environ_cp: copy of the os.environ.
  """
  cmd = '"%s" tensorflow/tools/git/gen_git_source.py --configure %s' % (
      environ_cp.get('PYTHON_BIN_PATH'), os.getcwd())
  os.system(cmd)


def cleanup_makefile():
  """Delete any leftover BUILD files from the Makefile build.

  These files could interfere with Bazel parsing.
  """
  makefile_download_dir = 'tensorflow/contrib/makefile/downloads'
  if os.path.isdir(makefile_download_dir):
    for root, _, filenames in os.walk(makefile_download_dir):
      for f in filenames:
        if f.endswith('BUILD'):
          os.remove(os.path.join(root, f))


def get_var(environ_cp,
            var_name,
            query_item,
            enabled_by_default,
            question=None,
            yes_reply=None,
            no_reply=None):
  """Get boolean input from user.

  If var_name is not set in env, ask user to enable query_item or not. If the
  response is empty, use the default.

  Args:
    environ_cp: copy of the os.environ.
    var_name: string for name of environment variable, e.g. "TF_NEED_HDFS".
    query_item: string for feature related to the variable, e.g. "Hadoop File
      System".
    enabled_by_default: boolean for default behavior.
    question: optional string for how to ask for user input.
    yes_reply: optionanl string for reply when feature is enabled.
    no_reply: optional string for reply when feature is disabled.

  Returns:
    boolean value of the variable.
  """
  if not question:
    question = 'Do you wish to build TensorFlow with %s support?' % query_item
  if not yes_reply:
    yes_reply = '%s support will be enabled for TensorFlow.' % query_item
  if not no_reply:
    no_reply = 'No %s' % yes_reply

  yes_reply += '\n'
  no_reply += '\n'

  if enabled_by_default:
    question += ' [Y/n]: '
  else:
    question += ' [y/N]: '

  var = environ_cp.get(var_name)
  while var is None:
    user_input_origin = get_input(question)
    user_input = user_input_origin.strip().lower()
    if user_input == 'y':
      print(yes_reply)
      var = True
    elif user_input == 'n':
      print(no_reply)
      var = False
    elif not user_input:
      if enabled_by_default:
        print(yes_reply)
        var = True
      else:
        print(no_reply)
        var = False
    else:
      print('Invalid selection: %s' % user_input_origin)
  return var


def set_build_var(environ_cp, var_name, query_item, option_name,
                  enabled_by_default):
  """Set if query_item will be enabled for the build.

  Ask user if query_item will be enabled. Default is used if no input is given.
  Set subprocess environment variable and write to .bazelrc if enabled.

  Args:
    environ_cp: copy of the os.environ.
    var_name: string for name of environment variable, e.g. "TF_NEED_HDFS".
    query_item: string for feature related to the variable, e.g. "Hadoop File
      System".
    option_name: string for option to define in .bazelrc.
    enabled_by_default: boolean for default behavior.
  """

  var = str(int(get_var(environ_cp, var_name, query_item, enabled_by_default)))
  environ_cp[var_name] = var
  if var == '1':
    write_to_bazelrc('build --define %s=true' % option_name)


def set_action_env_var(environ_cp,
                       var_name,
                       query_item,
                       enabled_by_default,
                       question=None,
                       yes_reply=None,
                       no_reply=None):
  """Set boolean action_env variable.

  Ask user if query_item will be enabled. Default is used if no input is given.
  Set environment variable and write to .bazelrc.

  Args:
    environ_cp: copy of the os.environ.
    var_name: string for name of environment variable, e.g. "TF_NEED_HDFS".
    query_item: string for feature related to the variable, e.g. "Hadoop File
      System".
    enabled_by_default: boolean for default behavior.
    question: optional string for how to ask for user input.
    yes_reply: optionanl string for reply when feature is enabled.
    no_reply: optional string for reply when feature is disabled.
  """
  var = int(
      get_var(environ_cp, var_name, query_item, enabled_by_default, question,
              yes_reply, no_reply))

  write_action_env_to_bazelrc(var_name, var)
  environ_cp[var_name] = str(var)


def convert_version_to_int(version):
  """Convert a version number to a integer that can be used to compare.

  Version strings of the form X.YZ and X.Y.Z-xxxxx are supported. The
  'xxxxx' part, for instance 'homebrew' on OS/X, is ignored.

  Args:
    version: a version to be converted

  Returns:
    An integer if converted successfully, otherwise return None.
  """
  version = version.split('-')[0]
  version_segments = version.split('.')
  for seg in version_segments:
    if not seg.isdigit():
      return None

  version_str = ''.join(['%03d' % int(seg) for seg in version_segments])
  return int(version_str)


def check_bazel_version(min_version):
  """Check installed bezel version is at least min_version.

  Args:
    min_version: string for minimum bazel version.

  Returns:
    The bazel version detected.
  """
  try:
    curr_version = run_shell('bazel --batch version')
  except subprocess.CalledProcessError:
    print('Cannot find bazel. Please install bazel.')
    sys.exit(0)

  for line in curr_version.split('\n'):
    if 'Build label: ' in line:
      curr_version = line.split('Build label: ')[1]
      break

  min_version_int = convert_version_to_int(min_version)
  curr_version_int = convert_version_to_int(curr_version)

  # Check if current bazel version can be detected properly.
  if not curr_version_int:
    print('WARNING: current bazel installation is not a release version.')
    print('Make sure you are running at least bazel %s' % min_version)
    return curr_version

  print("You have bazel %s installed." % curr_version)

  if curr_version_int < min_version_int:
    print('Please upgrade your bazel installation to version %s or higher to '
          'build TensorFlow!' % min_version)
    sys.exit(0)
  return curr_version


def set_cc_opt_flags(environ_cp):
  """Set up architecture-dependent optimization flags.

  Also append CC optimization flags to bazel.rc..

  Args:
    environ_cp: copy of the os.environ.
  """
  if is_ppc64le():
    # gcc on ppc64le does not support -march, use mcpu instead
    default_cc_opt_flags = '-mcpu=native'
  else:
    default_cc_opt_flags = '-march=native'
  question = ('Please specify optimization flags to use during compilation when'
              ' bazel option "--config=opt" is specified [Default is %s]: '
             ) % default_cc_opt_flags
  cc_opt_flags = get_from_env_or_user_or_default(environ_cp, 'CC_OPT_FLAGS',
                                                 question, default_cc_opt_flags)
  for opt in cc_opt_flags.split():
    write_to_bazelrc('build:opt --cxxopt=%s --copt=%s' % (opt, opt))


def set_tf_cuda_clang(environ_cp):
  """set TF_CUDA_CLANG action_env.

  Args:
    environ_cp: copy of the os.environ.
  """
  question = 'Do you want to use clang as CUDA compiler?'
  yes_reply = 'Clang will be used as CUDA compiler.'
  no_reply = 'nvcc will be used as CUDA compiler.'
  set_action_env_var(
      environ_cp,
      'TF_CUDA_CLANG',
      None,
      False,
      question=question,
      yes_reply=yes_reply,
      no_reply=no_reply)


def get_from_env_or_user_or_default(environ_cp, var_name, ask_for_var,
                                    var_default):
  """Get var_name either from env, or user or default.

  If var_name has been set as environment variable, use the preset value, else
  ask for user input. If no input is provided, the default is used.

  Args:
    environ_cp: copy of the os.environ.
    var_name: string for name of environment variable, e.g. "TF_NEED_HDFS".
    ask_for_var: string for how to ask for user input.
    var_default: default value string.

  Returns:
    string value for var_name
  """
  var = environ_cp.get(var_name)
  if not var:
    var = get_input(ask_for_var)
  if not var:
    var = var_default
  return var


def set_clang_cuda_compiler_path(environ_cp):
  """Set CLANG_CUDA_COMPILER_PATH."""
  default_clang_path = run_shell('which clang || true')
  ask_clang_path = ('Please specify which clang should be used as device and '
                    'host compiler. [Default is %s]: ') % default_clang_path

  while True:
    clang_cuda_compiler_path = get_from_env_or_user_or_default(
        environ_cp, 'CLANG_CUDA_COMPILER_PATH', ask_clang_path,
        default_clang_path)
    if os.path.exists(clang_cuda_compiler_path):
      break

    # Reset and retry
    print('Invalid clang path: %s cannot be found.' % clang_cuda_compiler_path)
    environ_cp['CLANG_CUDA_COMPILER_PATH'] = ''

  # Set CLANG_CUDA_COMPILER_PATH
  environ_cp['CLANG_CUDA_COMPILER_PATH'] = clang_cuda_compiler_path
  write_action_env_to_bazelrc('CLANG_CUDA_COMPILER_PATH',
                              clang_cuda_compiler_path)


def set_gcc_host_compiler_path(environ_cp):
  """Set GCC_HOST_COMPILER_PATH."""
  default_gcc_host_compiler_path = run_shell('which gcc || true')
  cuda_bin_symlink = '%s/bin/gcc' % environ_cp.get('CUDA_TOOLKIT_PATH')

  if os.path.islink(cuda_bin_symlink):
    # os.readlink is only available in linux
    default_gcc_host_compiler_path = run_shell('readlink %s' % cuda_bin_symlink)

  ask_gcc_path = (
      'Please specify which gcc should be used by nvcc as the '
      'host compiler. [Default is %s]: ') % default_gcc_host_compiler_path
  while True:
    gcc_host_compiler_path = get_from_env_or_user_or_default(
        environ_cp, 'GCC_HOST_COMPILER_PATH', ask_gcc_path,
        default_gcc_host_compiler_path)

    if os.path.exists(gcc_host_compiler_path):
      break

    # Reset and retry
    print('Invalid gcc path. %s cannot be found' % gcc_host_compiler_path)
    environ_cp['GCC_HOST_COMPILER_PATH'] = ''

  # Set GCC_HOST_COMPILER_PATH
  environ_cp['GCC_HOST_COMPILER_PATH'] = gcc_host_compiler_path
  write_action_env_to_bazelrc('GCC_HOST_COMPILER_PATH', gcc_host_compiler_path)


def set_tf_cuda_version(environ_cp):
  """Set CUDA_TOOLKIT_PATH and TF_CUDA_VERSION."""
  ask_cuda_version = (
      'Please specify the CUDA SDK version you want to use, '
      'e.g. 7.0. [Leave empty to default to CUDA %s]: ') % _DEFAULT_CUDA_VERSION

  while True:
    # Configure the Cuda SDK version to use.
    tf_cuda_version = get_from_env_or_user_or_default(
        environ_cp, 'TF_CUDA_VERSION', ask_cuda_version, _DEFAULT_CUDA_VERSION)

    # Find out where the CUDA toolkit is installed
    default_cuda_path = _DEFAULT_CUDA_PATH
    if is_windows():
      default_cuda_path = cygpath(
          environ_cp.get('CUDA_PATH', _DEFAULT_CUDA_PATH_WIN))
    elif is_linux():
      # If the default doesn't exist, try an alternative default.
      if (not os.path.exists(default_cuda_path)
         ) and os.path.exists(_DEFAULT_CUDA_PATH_LINUX):
        default_cuda_path = _DEFAULT_CUDA_PATH_LINUX
    ask_cuda_path = ('Please specify the location where CUDA %s toolkit is'
                     ' installed. Refer to README.md for more details. '
                     '[Default is %s]: ') % (tf_cuda_version, default_cuda_path)
    cuda_toolkit_path = get_from_env_or_user_or_default(
        environ_cp, 'CUDA_TOOLKIT_PATH', ask_cuda_path, default_cuda_path)

    if is_windows():
      cuda_rt_lib_path = 'lib/x64/cudart.lib'
    elif is_linux():
      cuda_rt_lib_path = 'lib64/libcudart.so.%s' % tf_cuda_version
    elif is_macos():
      cuda_rt_lib_path = 'lib/libcudart.%s.dylib' % tf_cuda_version

    cuda_toolkit_path_full = os.path.join(cuda_toolkit_path, cuda_rt_lib_path)
    if os.path.exists(cuda_toolkit_path_full):
      break

    # Reset and retry
    print('Invalid path to CUDA %s toolkit. %s cannot be found' %
          (tf_cuda_version, cuda_toolkit_path_full))
    environ_cp['TF_CUDA_VERSION'] = ''
    environ_cp['CUDA_TOOLKIT_PATH'] = ''

  # Set CUDA_TOOLKIT_PATH and TF_CUDA_VERSION
  environ_cp['CUDA_TOOLKIT_PATH'] = cuda_toolkit_path
  write_action_env_to_bazelrc('CUDA_TOOLKIT_PATH', cuda_toolkit_path)
  environ_cp['TF_CUDA_VERSION'] = tf_cuda_version
  write_action_env_to_bazelrc('TF_CUDA_VERSION', tf_cuda_version)


def set_tf_cunn_version(environ_cp):
  """Set CUDNN_INSTALL_PATH and TF_CUDNN_VERSION."""
  ask_cudnn_version = (
      '"Please specify the cuDNN version you want to use. '
      '[Leave empty to default to cuDNN %s.0]: ') % _DEFAULT_CUDNN_VERSION

  while True:
    tf_cudnn_version = get_from_env_or_user_or_default(
        environ_cp, 'TF_CUDNN_VERSION', ask_cudnn_version,
        _DEFAULT_CUDNN_VERSION)

    default_cudnn_path = environ_cp.get('CUDA_TOOLKIT_PATH')
    ask_cudnn_path = (r'Please specify the location where cuDNN %s library is '
                      'installed. Refer to README.md for more details. [Default'
                      ' is %s]:') % (tf_cudnn_version, default_cudnn_path)
    cudnn_install_path = get_from_env_or_user_or_default(
        environ_cp, 'CUDNN_INSTALL_PATH', ask_cudnn_path, default_cudnn_path)

    # Result returned from "read" will be used unexpanded. That make "~"
    # unusable. Going through one more level of expansion to handle that.
    cudnn_install_path = os.path.realpath(
        os.path.expanduser(cudnn_install_path))
    if is_windows():
      cudnn_install_path = cygpath(cudnn_install_path)

    if is_windows():
      cuda_dnn_lib_path = 'lib/x64/cudnn.lib'
      cuda_dnn_lib_alt_path = 'lib/x64/cudnn.lib'
    elif is_linux():
      cuda_dnn_lib_path = 'lib64/libcudnn.so.%s' % tf_cudnn_version
      cuda_dnn_lib_alt_path = 'libcudnn.so.%s' % tf_cudnn_version
    elif is_macos():
      cuda_dnn_lib_path = 'lib/libcudnn.%s.dylib' % tf_cudnn_version
      cuda_dnn_lib_alt_path = 'libcudnn.%s.dylib' % tf_cudnn_version

    cuda_dnn_lib_path_full = os.path.join(cudnn_install_path, cuda_dnn_lib_path)
    cuda_dnn_lib_alt_path_full = os.path.join(cudnn_install_path,
                                              cuda_dnn_lib_alt_path)
    if os.path.exists(cuda_dnn_lib_path_full) or os.path.exists(
        cuda_dnn_lib_alt_path_full):
      break

    # Try another alternative for Linux
    if is_linux():
      if subprocess.call(['which', 'ldconfig']):
        ldconfig_bin = '/sbin/ldconfig'
      else:
        ldconfig_bin = 'ldconfig'
      cudnn_path_from_ldconfig = run_shell(
          r'%s -p | sed -n "s/.*libcudnn.so .* => \(.*\)/\\1/p"' % ldconfig_bin)
      if os.path.exists('%s.%s' % (cudnn_path_from_ldconfig, tf_cudnn_version)):
        cudnn_install_path = os.path.dirname(cudnn_path_from_ldconfig)
        break

    # Reset and Retry
    print(
        'Invalid path to cuDNN %s toolkit. None of the following files can be '
        'found:' % tf_cudnn_version)
    print(cuda_dnn_lib_path_full)
    print(cuda_dnn_lib_alt_path_full)
    if is_linux():
      print('%s.%s' % (cudnn_path_from_ldconfig, tf_cudnn_version))

    environ_cp['TF_CUDNN_VERSION'] = ''

  # Set CUDNN_INSTALL_PATH and TF_CUDNN_VERSION
  environ_cp['CUDNN_INSTALL_PATH'] = cudnn_install_path
  write_action_env_to_bazelrc('CUDNN_INSTALL_PATH', cudnn_install_path)
  environ_cp['TF_CUDNN_VERSION'] = tf_cudnn_version
  write_action_env_to_bazelrc('TF_CUDNN_VERSION', tf_cudnn_version)


def get_native_cuda_compute_capabilities(environ_cp):
  """Get native cuda compute capabilities.

  Args:
    environ_cp: copy of the os.environ.
  Returns:
    string of native cuda compute capabilities, separated by comma.
  """
  device_query_bin = os.path.join(
      environ_cp.get('CUDA_TOOLKIT_PATH'), 'extras/demo_suite/deviceQuery')
  cmd = (r'"%s" | grep "Capability" | grep -o "[0-9]*\.[0-9]*" | sed '
         '":a;{N;s/\\n/,/};ba"') % device_query_bin
  try:
    output = run_shell(cmd)
  except subprocess.CalledProcessError:
    output = ''
  return output


def set_tf_cuda_compute_capabilities(environ_cp):
  """Set TF_CUDA_COMPUTE_CAPABILITIES."""
  while True:
    native_cuda_compute_capabilities = get_native_cuda_compute_capabilities(
        environ_cp)
    if not native_cuda_compute_capabilities:
      default_cuda_compute_capabilities = _DEFAULT_CUDA_COMPUTE_CAPABILITIES
    else:
      default_cuda_compute_capabilities = native_cuda_compute_capabilities

    ask_cuda_compute_capabilities = (
        'Please specify a list of comma-separated '
        'Cuda compute capabilities you want to '
        'build with.\nYou can find the compute '
        'capability of your device at: '
        'https://developer.nvidia.com/cuda-gpus.\nPlease'
        ' note that each additional compute '
        'capability significantly increases your '
        'build time and binary size. [Default is: %s]' %
        default_cuda_compute_capabilities)
    tf_cuda_compute_capabilities = get_from_env_or_user_or_default(
        environ_cp, 'TF_CUDA_COMPUTE_CAPABILITIES',
        ask_cuda_compute_capabilities, default_cuda_compute_capabilities)
    # Check whether all capabilities from the input is valid
    all_valid = True
    for compute_capability in tf_cuda_compute_capabilities.split(','):
      if not re.match('[0-9]+.[0-9]+', compute_capability):
        print('Invalid compute capability: ' % compute_capability)
        all_valid = False

    if all_valid:
      break

    # Reset and Retry
    environ_cp['TF_CUDA_COMPUTE_CAPABILITIES'] = ''

  # Set TF_CUDA_COMPUTE_CAPABILITIES
  environ_cp['TF_CUDA_COMPUTE_CAPABILITIES'] = tf_cuda_compute_capabilities
  write_action_env_to_bazelrc('TF_CUDA_COMPUTE_CAPABILITIES',
                              tf_cuda_compute_capabilities)


def set_other_cuda_vars(environ_cp):
  """Set other CUDA related variables."""
  if is_windows():
    # The following three variables are needed for MSVC toolchain configuration
    # in Bazel
    environ_cp['CUDA_PATH'] = environ_cp.get('CUDA_TOOLKIT_PATH')
    environ_cp['CUDA_COMPUTE_CAPABILITIES'] = environ_cp.get(
        'TF_CUDA_COMPUTE_CAPABILITIES')
    environ_cp['NO_WHOLE_ARCHIVE_OPTION'] = 1
    write_action_env_to_bazelrc('CUDA_PATH', environ_cp.get('CUDA_PATH'))
    write_action_env_to_bazelrc('CUDA_COMPUTE_CAPABILITIE',
                                environ_cp.get('CUDA_COMPUTE_CAPABILITIE'))
    write_action_env_to_bazelrc('NO_WHOLE_ARCHIVE_OPTION',
                                environ_cp.get('NO_WHOLE_ARCHIVE_OPTION'))
    write_to_bazelrc('build --config=win-cuda')
    write_to_bazelrc('test --config=win-cuda')
  else:
    # If CUDA is enabled, always use GPU during build and test.
    if environ_cp.get('TF_CUDA_CLANG') == '1':
      write_to_bazelrc('build --config=cuda_clang')
      write_to_bazelrc('test --config=cuda_clang')
    else:
      write_to_bazelrc('build --config=cuda')
      write_to_bazelrc('test --config=cuda')


def set_host_cxx_compiler(environ_cp):
  """Set HOST_CXX_COMPILER."""
  default_cxx_host_compiler = run_shell('which g++ || true')
  ask_cxx_host_compiler = (
      'Please specify which C++ compiler should be used as'
      ' the host C++ compiler. [Default is %s]: ') % default_cxx_host_compiler

  while True:
    host_cxx_compiler = get_from_env_or_user_or_default(
        environ_cp, 'HOST_CXX_COMPILER', ask_cxx_host_compiler,
        default_cxx_host_compiler)
    if os.path.exists(host_cxx_compiler):
      break

    # Reset and retry
    print('Invalid C++ compiler path. %s cannot be found' % host_cxx_compiler)
    environ_cp['HOST_CXX_COMPILER'] = ''

  # Set HOST_CXX_COMPILER
  environ_cp['HOST_CXX_COMPILER'] = host_cxx_compiler
  write_action_env_to_bazelrc('HOST_CXX_COMPILER', host_cxx_compiler)


def set_host_c_compiler(environ_cp):
  """Set HOST_C_COMPILER."""
  default_c_host_compiler = run_shell('which gcc || true')
  ask_c_host_compiler = (
      'Please specify which C compiler should be used as the'
      ' host C compiler. [Default is %s]: ') % default_c_host_compiler

  while True:
    host_c_compiler = get_from_env_or_user_or_default(
        environ_cp, 'HOST_C_COMPILER', ask_c_host_compiler,
        default_c_host_compiler)
    if os.path.exists(host_c_compiler):
      break

    # Reset and retry
    print('Invalid C compiler path. %s cannot be found' % host_c_compiler)
    environ_cp['HOST_C_COMPILER'] = ''

  # Set HOST_C_COMPILER
  environ_cp['HOST_C_COMPILER'] = host_c_compiler
  write_action_env_to_bazelrc('HOST_C_COMPILER', host_c_compiler)


def set_computecpp_toolkit_path(environ_cp):
  """Set COMPUTECPP_TOOLKIT_PATH."""
  ask_computecpp_toolkit_path = ('Please specify the location where ComputeCpp '
                                 'for SYCL %s is installed. [Default is %s]: '
                                ) % (_TF_OPENCL_VERSION,
                                     _DEFAULT_COMPUTECPP_TOOLKIT_PATH)

  while True:
    computecpp_toolkit_path = get_from_env_or_user_or_default(
        environ_cp, 'COMPUTECPP_TOOLKIT_PATH', ask_computecpp_toolkit_path,
        _DEFAULT_COMPUTECPP_TOOLKIT_PATH)
    if is_linux():
      sycl_rt_lib_path = 'lib/libComputeCpp.so'
    else:
      sycl_rt_lib_path = ''

    sycl_rt_lib_path_full = os.path.join(computecpp_toolkit_path,
                                         sycl_rt_lib_path)
    if os.path.exists(sycl_rt_lib_path_full):
      break

    print('Invalid SYCL %s library path. %s cannot be found' %
          (_TF_OPENCL_VERSION, sycl_rt_lib_path_full))
    environ_cp['COMPUTECPP_TOOLKIT_PATH'] = ''

  # Set COMPUTECPP_TOOLKIT_PATH
  environ_cp['COMPUTECPP_TOOLKIT_PATH'] = computecpp_toolkit_path
  write_action_env_to_bazelrc('COMPUTECPP_TOOLKIT_PATH',
                              computecpp_toolkit_path)


def set_mpi_home(environ_cp):
  """Set MPI_HOME."""
  cmd = ('dirname $(dirname $(which mpirun)) || dirname $(dirname $(which '
         'mpiexec))  || true')
  default_mpi_home = run_shell(cmd)
  ask_mpi_home = ('Please specify the MPI toolkit folder. [Default is %s]: '
                 ) % default_mpi_home
  while True:
    mpi_home = get_from_env_or_user_or_default(environ_cp, 'MPI_HOME',
                                               ask_mpi_home, default_mpi_home)

    if os.path.exists(os.path.join(mpi_home, 'include')) and os.path.exists(
        os.path.join(mpi_home, 'lib')):
      break

    print('Invalid path to the MPI Toolkit. %s or %s cannot be found' %
          (os.path.join(mpi_home, 'include'),
           os.path.exists(os.path.join(mpi_home, 'lib'))))
    environ_cp['MPI_HOME'] = ''

  # Set MPI_HOME
  environ_cp['MPI_HOME'] = str(mpi_home)


def set_other_mpi_vars(environ_cp):
  """Set other MPI related variables."""
  # Link the MPI header files
  mpi_home = environ_cp.get('MPI_HOME')
  symlink_force('%s/include/mpi.h' % mpi_home, 'third_party/mpi/mpi.h')

  # Determine if we use OpenMPI or MVAPICH, these require different header files
  # to be included here to make bazel dependency checker happy
  if os.path.exists(os.path.join(mpi_home, 'include/mpi_portable_platform.h')):
    symlink_force(
        os.path.join(mpi_home, 'include/mpi_portable_platform.h'),
        'third_party/mpi/mpi_portable_platform.h')
    # TODO(gunan): avoid editing files in configure
    sed_in_place('third_party/mpi/mpi.bzl', 'MPI_LIB_IS_OPENMPI=False',
                 'MPI_LIB_IS_OPENMPI=True')
  else:
    # MVAPICH / MPICH
    symlink_force(
        os.path.join(mpi_home, 'include/mpio.h'), 'third_party/mpi/mpio.h')
    symlink_force(
        os.path.join(mpi_home, 'include/mpicxx.h'), 'third_party/mpi/mpicxx.h')
    # TODO(gunan): avoid editing files in configure
    sed_in_place('third_party/mpi/mpi.bzl', 'MPI_LIB_IS_OPENMPI=True',
                 'MPI_LIB_IS_OPENMPI=False')

  if os.path.exists(os.path.join(mpi_home, 'lib/libmpi.so')):
    symlink_force(
        os.path.join(mpi_home, 'lib/libmpi.so'), 'third_party/mpi/libmpi.so')
  else:
    raise ValueError('Cannot find the MPI library file in %s/lib' % mpi_home)


def set_mkl():
  write_to_bazelrc('build:mkl --define with_mkl_support=true')
  write_to_bazelrc('build:mkl --define using_mkl=true')
  write_to_bazelrc('build:mkl -c opt')
  write_to_bazelrc('build:mkl --copt="-DEIGEN_USE_VML"')
  print(
      'Add "--config=mkl" to your bazel command to build with MKL '
      'support.\nPlease note that MKL on MacOS or windows is still not '
      'supported.\nIf you would like to use a local MKL instead of '
      'downloading, please set the environment variable \"TF_MKL_ROOT\" every '
      'time before build.')


def main():
  # Make a copy of os.environ to be clear when functions and getting and setting
  # environment variables.
  environ_cp = dict(os.environ)

  bazel_version = check_bazel_version('0.4.5')

  reset_tf_configure_bazelrc()
  cleanup_makefile()
  setup_python(environ_cp, bazel_version)
  run_gen_git_source(environ_cp)

  if is_windows():
    environ_cp['TF_NEED_GCP'] = '0'
    environ_cp['TF_NEED_HDFS'] = '0'
    environ_cp['TF_NEED_JEMALLOC'] = '0'
    environ_cp['TF_NEED_OPENCL'] = '0'
    environ_cp['TF_CUDA_CLANG'] = '0'

  if is_macos():
    environ_cp['TF_NEED_JEMALLOC'] = '0'

  set_build_var(environ_cp, 'TF_NEED_JEMALLOC', 'jemalloc as malloc',
                'with_jemalloc', True)
  set_build_var(environ_cp, 'TF_NEED_GCP', 'Google Cloud Platform',
                'with_gcp_support', False)
  set_build_var(environ_cp, 'TF_NEED_HDFS', 'Hadoop File System',
                'with_hdfs_support', False)
  set_build_var(environ_cp, 'TF_ENABLE_XLA', 'XLA JIT', 'with_xla_support',
                False)
  set_build_var(environ_cp, 'TF_NEED_GDR', 'GDR', 'with_gdr_support',
                False)
  set_build_var(environ_cp, 'TF_NEED_VERBS', 'VERBS', 'with_verbs_support',
                False)

  set_action_env_var(environ_cp, 'TF_NEED_OPENCL', 'OpenCL', False)
  if environ_cp.get('TF_NEED_OPENCL') == '1':
    set_host_cxx_compiler(environ_cp)
    set_host_c_compiler(environ_cp)
    set_computecpp_toolkit_path(environ_cp)

  set_action_env_var(environ_cp, 'TF_NEED_CUDA', 'CUDA', False)
  if environ_cp.get('TF_NEED_CUDA') == '1':
    set_tf_cuda_version(environ_cp)
    set_tf_cunn_version(environ_cp)
    set_tf_cuda_compute_capabilities(environ_cp)

    set_tf_cuda_clang(environ_cp)
    if environ_cp.get('TF_CUDA_CLANG') == '1':
      # Set up which clang we should use as the cuda / host compiler.
      set_clang_cuda_compiler_path(environ_cp)
    else:
      # Set up which gcc nvcc should use as the host compiler
      # No need to set this on Windows
      if not is_windows():
        set_gcc_host_compiler_path(environ_cp)
    set_other_cuda_vars(environ_cp)

  set_build_var(environ_cp, 'TF_NEED_MPI', 'MPI', 'with_mpi_support', False)
  if environ_cp.get('TF_NEED_MPI') == '1':
    set_mpi_home(environ_cp)
    set_other_mpi_vars(environ_cp)

  set_cc_opt_flags(environ_cp)
  set_mkl()


if __name__ == '__main__':
  main()
