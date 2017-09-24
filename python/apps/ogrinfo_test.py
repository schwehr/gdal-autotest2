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

"""Tests the ogrinfo command line application."""

import os
import subprocess
import unittest

from autotest2.ogr import ogr_util


class OgrinfoTest(unittest.TestCase):

  @ogr_util.SkipIfDriverMissing(ogr_util.GEOJSON_DRIVER)
  def testGeojsonFromStdin(self):
    # Replaces the need for geojson_test.py test #12.
    ogrinfo = os.path.join(
        'TODO(schwehr): Where?',
        'ogrinfo')
    cmd = [
        ogrinfo, '-ro', '-al', '{"type": "Point","coordinates": [100.0, 0.0]}']
    result = subprocess.check_output(cmd)
    self.assertIn('GeoJSON', result)
    self.assertIn('Geometry: Point', result)
    self.assertIn('Feature Count: 1', result)
    self.assertIn('WGS 84', result)
    self.assertIn('4326', result)
    self.assertIn('POINT (100 0)', result)

if __name__ == '__main__':
  unittest.main()
