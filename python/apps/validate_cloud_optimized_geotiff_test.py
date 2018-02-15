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
"""Tests the validate_cloud_optimized_geotiff command line application."""

import os
import subprocess

import google3
from google3.pyglib import flags
from google3.pyglib import resources
from google3.testing.pybase import googletest
from google3.third_party.gdal.autotest2.python.ogr import ogr_util

FLAGS = flags.FLAGS


class ValidateCloudOptimizedGeotiffTest(googletest.TestCase):

  def setUp(self):
    self.validate_cmd = os.path.join(
        resources.GetARootDirWithAllResources(),
        'gdal/validate_cloud_optimized_geotiff')

    # TODO(schwehr): Move this to an apps_util.py.
    self.test_data_path = os.path.join(
        FLAGS.test_srcdir,
        'autotest2/python/apps/testdata/cogeo')

  def testHelp(self):
    # Note that options other than -q and the filename always report a failure
    # by calling exit(1).  There is no real failure.  It's just how the user
    # gets help.
    cmd = [self.validate_cmd]
    raised = False
    try:
      subprocess.check_output(cmd)
    except subprocess.CalledProcessError as e:
      raised = True
      self.assertEqual(1, e.returncode)
      self.assertIn('Usage', e.output)
    self.assertTrue(raised)

    cmd = [self.validate_cmd, '-']
    raised = False
    try:
      subprocess.check_output(cmd)
    except subprocess.CalledProcessError as e:
      raised = True
      self.assertEqual(1, e.returncode)
      self.assertIn('Usage', e.output)
    self.assertTrue(raised)

  def testMissingFile(self):
    filepath = os.path.join(self.test_data_path, '/does/not/exist.tif')
    cmd = [self.validate_cmd, filepath]
    raised = False
    try:
      subprocess.check_output(cmd)
    except subprocess.CalledProcessError as e:
      raised = True
      self.assertEqual(1, e.returncode)
      self.assertIn('Invalid file', e.output)
      self.assertIn('No such file', e.output)
    self.assertTrue(raised)

  def testMissingTilesMissingOverviews(self):
    filepath = os.path.join(self.test_data_path, '1.tif')
    cmd = [self.validate_cmd, filepath]
    raised = False
    try:
      subprocess.check_output(cmd)
    except subprocess.CalledProcessError as e:
      raised = True
      self.assertEqual(1, e.returncode)
      self.assertIn('is not tiled', e.output)
      self.assertIn('has no overviews', e.output)
    self.assertTrue(raised)

  def testMissingOverviews(self):
    filepath = os.path.join(self.test_data_path, '2-tiled-lzw.tif')
    cmd = [self.validate_cmd, filepath]
    raised = False
    try:
      subprocess.check_output(cmd)
    except subprocess.CalledProcessError as e:
      raised = True
      self.assertEqual(1, e.returncode)
      self.assertNotIn('is not tiled', e.output)
      self.assertIn('has no overviews', e.output)
    self.assertTrue(raised)

  def testBadOverviewOrder(self):
    filepath = os.path.join(self.test_data_path, '3-overviews.tif')
    cmd = [self.validate_cmd, filepath]
    raised = False
    try:
      subprocess.check_output(cmd)
    except subprocess.CalledProcessError as e:
      raised = True
      self.assertEqual(1, e.returncode)
      self.assertNotIn('is not tiled', e.output)
      self.assertNotIn('has no overviews', e.output)
      self.assertIn('should be after', e.output)
    self.assertTrue(raised)

  def testMissingBlockOffset(self):
    filepath = os.path.join(self.test_data_path, 'block-offset-missing.tif')
    cmd = [self.validate_cmd, filepath]
    raised = False
    try:
      subprocess.check_output(cmd)
    except subprocess.CalledProcessError as e:
      raised = True
      self.assertEqual(1, e.returncode)
      self.assertIn('Missing BLOCK_OFFSET_0_0', e.output)
    self.assertTrue(raised)

  def testCogeoFile(self):
    filepath = os.path.join(self.test_data_path, '4-cogeo.tif')
    cmd = [self.validate_cmd, filepath]
    result = subprocess.check_output(cmd)
    self.assertIn('is a valid', result)

    # Quiet mode.
    cmd = [self.validate_cmd, '-q', filepath]
    result = subprocess.check_output(cmd)
    self.assertEqual('', result)


if __name__ == '__main__':
  googletest.main()
