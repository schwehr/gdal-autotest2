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
"""Tests the Proj via the gie command line.

Geospatial integrity of geoscience software - GIGS

See also:
- http://www.iogp.org/bookstore/product/geospatial-integrity-of-geoscience-software-part-1-gigs-guidelines/
- https://github.com/OSGeo/proj.4/tree/master/test/gigs
"""

import glob
import os
import subprocess

from pyglib import flags
from pyglib import resources
import unittest

FLAGS = flags.FLAGS


class GigsTest(googletest.TestCase):

  def setUp(self):
    self.gie = os.path.join(resources.GetARootDirWithAllResources(),
                            'third_party/proj4/gie')

  def testAll(self):
    glob_path = os.path.join(
        FLAGS.test_srcdir, 'third_party/proj4/proj/test/gigs', '*.gie')
    gie_files = glob.glob(glob_path)
    self.assertTrue(gie_files)
    for filepath in gie_files:
      cmd = [self.gie, filepath]
      result = subprocess.check_output(cmd, stderr=subprocess.STDOUT)
      self.assertIn('0 tests failed', result)


if __name__ == '__main__':
  googletest.main()
