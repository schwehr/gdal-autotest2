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

"""Tests the gdalbuildvrt commandline application."""

import os
import subprocess
import unittest

from autotest2.gcore import gcore_util
from autotest2.gdrivers import gdrivers_util


@gdrivers_util.SkipIfDriverMissing(gdrivers_util.VRT_DRIVER)
class GdalbuildvrtTest(unittest.TestCase):

  @gdrivers_util.SkipIfDriverMissing(gdrivers_util.GTIFF_DRIVER)
  def testWithSingleTiff(self):
    inputpath = gcore_util.GetTestFilePath('byte.tif')
    binary = os.path.join('TODO(schwehr): Where?',
                          'gdalbuildvrt')
    cmd = [binary, '-q', '/dev/stdout', inputpath]
    result = subprocess.check_output(cmd)

    # Checks the existence of some mandatory fields
    self.assertIn('<VRTDataset', result)
    self.assertIn('</VRTDataset>', result)
    self.assertIn('<VRTRasterBand', result)
    self.assertIn('</VRTRasterBand>', result)
    self.assertIn('<SourceFilename', result)
    self.assertIn('</SourceFilename>', result)


if __name__ == '__main__':
  unittest.main()
