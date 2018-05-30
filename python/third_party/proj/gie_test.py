# Copyright 2018 Google Inc. All Rights Reserved.
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
"""Tests the Proj via the gie command line."""

import contextlib
import glob
import os
import subprocess
import unittest

from pyglib import flags
from pyglib import resources

FLAGS = flags.FLAGS


@contextlib.contextmanager
def TempSetEnv(key, value):
  """Temporarily set an environment variable.

  If it is overwriting an existing value, it saves the old value and restores
  it when exiting the context.

  If there was no prior environment variable with that name, it removes that
  variable from the environment when exiting the context.

  Args:
    key: (str) The name of the environment variable to set.
    value: (str) What string value to set the variable to.

  Yields:
    None.  The goal is to provide the side effect in os.environ.
  """
  if key in os.environ:
    # pylint: disable=lost-exception
    original = os.environ[key]
    try:
      os.environ[key] = value
      yield
    finally:
      os.environ[key] = original
  else:
    try:
      os.environ[key] = value
      yield
    finally:
      del os.environ[key]


@contextlib.contextmanager
def TempUnSetEnv(key):
  """Temporarily remove an environment variable.

  If it is overwriting an existing value, it saves the old value and restores
  it when exiting the context.

  If there was no prior environment variable with that name, it does nothing
  when exiting the context.

  Args:
    key: Name of the environment variable.

  Yields:
    None.  Goal is the side effect.
  """
  if key in os.environ:
    # pylint: disable=lost-exception
    original = os.environ[key]
    try:
      del os.environ[key]
      yield
    finally:
      os.environ[key] = original
  else:
    try:
      yield
    finally:
      pass


class GieTest(unittest.TestCase):

  def setUp(self):
    self.gie = os.path.join(resources.GetARootDirWithAllResources(),
                            'third_party/proj4/gie')
    self.test_data = os.path.join(FLAGS.test_srcdir,
                                  'third_party/proj4/proj/test/gie')

  def testAll(self):
    glob_path = os.path.join(FLAGS.test_srcdir,
                             'third_party/proj4/proj/test/gie', '*.gie')
    gie_files = glob.glob(glob_path)
    self.assertTrue(gie_files)
    for filepath in gie_files:
      cmd = [self.gie, filepath]
      try:
        result = subprocess.check_output(cmd, stderr=subprocess.STDOUT)
      except subprocess.CalledProcessError as err:
        self.fail('gie tests failed:\n' + str(err.output) + '\n\n' + str(err))
      self.assertIn('0 tests failed', result)

  def testHelp(self):
    cmd = [self.gie, '-h']
    result = subprocess.check_output(cmd)
    self.assertIn('print this usage information', result)

  def testVersion(self):
    cmd = [self.gie, '--version']
    result = subprocess.check_output(cmd)
    self.assertIn('gie: Rel', result)

  def testVerbose(self):
    test_file = os.path.join(self.test_data, 'unitconvert.gie')

    cmd = [self.gie, test_file]
    result = subprocess.check_output(cmd)
    self.assertNotIn('Succeeding roundtrips', result)

    cmd = [self.gie, '-v', test_file]
    result = subprocess.check_output(cmd)
    self.assertIn('Succeeding roundtrips', result)

  def testInputDoesNotExist(self):
    cmd = [self.gie, '/does/not/exist.gie']
    result = subprocess.check_output(cmd)
    self.assertIn('Cannot open spec', result)

  def testOutputToFile(self):
    test_file = os.path.join(self.test_data, 'unitconvert.gie')

    cmd = [self.gie, '-o', '/dev/null', test_file]
    result = subprocess.check_output(cmd)
    self.assertEqual('', result)

  def testOutputToFileFail(self):
    test_file = os.path.join(self.test_data, 'unitconvert.gie')
    cmd = [self.gie, '-o', '/does/not/exist', test_file]
    raised = False
    try:
      subprocess.check_output(cmd, stderr=subprocess.STDOUT)
    except subprocess.CalledProcessError as e:
      raised = True
      self.assertEqual(1, e.returncode)
      self.assertEqual('gie: Cannot open \'/does/not/exist\' for output\n',
                       e.output)
    self.assertTrue(raised)

  def testProjDebug(self):
    test_file = os.path.join(self.test_data, 'unitconvert.gie')

    with TempUnSetEnv('PROJ_DEBUG'):
      cmd = [self.gie, test_file]
      result = subprocess.check_output(cmd, stderr=subprocess.STDOUT)
      self.assertNotIn('xy_in unit', result)

    with TempSetEnv('PROJ_DEBUG', '2'):
      cmd = [self.gie, test_file]
      result = subprocess.check_output(cmd, stderr=subprocess.STDOUT)
      self.assertIn('xy_in unit', result)
      self.assertNotIn('searching cache for key', result)

    with TempSetEnv('PROJ_DEBUG', '3'):
      cmd = [self.gie, test_file]
      result = subprocess.check_output(cmd, stderr=subprocess.STDOUT)
      self.assertIn('searching cache for key', result)

  def testQuiet(self):
    test_file = os.path.join(self.test_data, 'unitconvert.gie')
    cmd = [self.gie, '-q', test_file]
    result = subprocess.check_output(cmd, stderr=subprocess.STDOUT)
    self.assertEqual('', result)

  def testListErrorCodes(self):
    cmd = [self.gie, '-l']
    result = subprocess.check_output(cmd, stderr=subprocess.STDOUT)
    self.assertIn('too_many_inits', result)

  def testBanner(self):
    filepath = os.path.join(FLAGS.test_srcdir,
                            'third_party/proj4/tests/testdata',
                            'banner.gie')
    cmd = [self.gie, filepath]
    result = subprocess.check_output(cmd, stderr=subprocess.STDOUT)
    self.assertIn('---\nhello', result)
    self.assertIn('---\nworld', result)

  def testEcho(self):
    filepath = os.path.join(FLAGS.test_srcdir,
                            'third_party/proj4/tests/testdata',
                            'echo.gie')
    cmd = [self.gie, filepath]
    result = subprocess.check_output(cmd, stderr=subprocess.STDOUT)
    self.assertIn('echo.gie\'\nfoo\nbar\n---', result)

  def testMissingEnd(self):
    filepath = os.path.join(FLAGS.test_srcdir,
                            'third_party/proj4/tests/testdata',
                            'missing-gie-end-tag.gie')
    cmd = [self.gie, filepath]
    result = subprocess.check_output(cmd, stderr=subprocess.STDOUT)
    self.assertIn('Missing \'</gie>\' cmnd', result)

  def testMissingStart(self):
    filepath = os.path.join(FLAGS.test_srcdir,
                            'third_party/proj4/tests/testdata',
                            'missing-gie-start-tag.gie')
    cmd = [self.gie, filepath]
    result = subprocess.check_output(cmd, stderr=subprocess.STDOUT)
    self.assertIn('Missing \'<gie>\' cmnd', result)

  def testMissingSkip(self):
    filepath = os.path.join(FLAGS.test_srcdir,
                            'third_party/proj4/tests/testdata',
                            'skip.gie')
    cmd = [self.gie, filepath]
    result = subprocess.check_output(cmd, stderr=subprocess.STDOUT)
    self.assertNotIn('Should not print', result)


if __name__ == '__main__':
  unittest.main()
