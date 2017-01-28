# Copyright 2015 Google Inc. All Rights Reserved.
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

"""Tests the gdal2tiles commandline application."""

import os
import subprocess
import tempfile
import unittest

import gflags as flags
from autotest2.gcore import gcore_util
from autotest2.gdrivers import gdrivers_util

FLAGS = flags.FLAGS


@gdrivers_util.SkipIfDriverMissing(gdrivers_util.PNG_DRIVER)
@gdrivers_util.SkipIfDriverMissing(gdrivers_util.MEM_DRIVER)
class Gdal2tilesTest(unittest.TestCase):

  @gdrivers_util.SkipIfDriverMissing(gdrivers_util.GTIFF_DRIVER)
  def testWithSingleTiff(self):
    inputpath = gcore_util.GetTestFilePath('byte.tif')
    outputdir = tempfile.mkdtemp(dir=FLAGS.test_tmpdir)
    binary = os.path.join('TODO(schwehr): Where?',
                          'gdal2tiles')
    cmd = [binary, '-z 11', inputpath, outputdir]
    subprocess.check_call(cmd)

    # Checks the existence of the tile tree.
    self.assertTrue(os.path.exists(
        os.path.join(outputdir, '11', '354', '1229.png')))


if __name__ == '__main__':
  unittest.main()
