# Copyright 2014 Google Inc. All Rights Reserved.
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

"""Tests the gdalinfo command line application."""

import os
import subprocess
import unittest

from autotest2.gcore import gcore_util
from autotest2.gdrivers import gdrivers_util


class GdalinfoTest(unittest.TestCase):

  @gdrivers_util.SkipIfDriverMissing(gdrivers_util.GTIFF_DRIVER)
  def testTiff(self):
    filepath = gcore_util.GetTestFilePath('utmsmall.tif')
    gdalinfo = os.path.join(
        'TODO(schwehr): Where?',
        'gdalinfo')
    cmd = [gdalinfo, filepath]
    result = subprocess.check_output(cmd)
    self.assertIn('GTiff/GeoTIFF', result)
    self.assertIn('NAD27', result)
    self.assertIn('26711', result)
    self.assertIn('440720', result)
    self.assertIn('60', result)
    self.assertIn('Area', result)
    self.assertIn('BAND', result)
    self.assertIn('117d38', result)
    self.assertIn('Band 1', result)
    self.assertIn('Byte', result)
    self.assertIn('Gray', result)

    cmd = [gdalinfo, '-mm', filepath]
    result = subprocess.check_output(cmd)
    self.assertIn('Computed', result)
    computed = [line for line in result.split('\n') if 'Computed' in line][0]
    computed_min, computed_max = [
        float(s) for s in computed.split('=')[1].split(',')]
    self.assertAlmostEqual(computed_min, 0.0)
    self.assertAlmostEqual(computed_max, 255.0)

    cmd = [gdalinfo, '-stats', filepath]
    result = subprocess.check_output(cmd)
    self.assertIn('STATISTICS_MAXIMUM=255', result)
    self.assertIn('STATISTICS_MINIMUM=0', result)
    self.assertIn('STATISTICS_MEAN', result)
    self.assertIn('STATISTICS_STDDEV', result)

    mean_line = [line for line in result.split('\n') if 'MEAN' in line][0]
    mean = [float(s) for s in mean_line.split('=')[1].split(',')][0]
    self.assertAlmostEqual(mean, 154.6212)

    stddev_line = [line for line in result.split('\n') if 'STDDEV' in line][0]
    stddev = [float(s) for s in stddev_line.split('=')[1].split(',')][0]
    self.assertAlmostEqual(stddev, 54.250980733624)


if __name__ == '__main__':
  unittest.main()
