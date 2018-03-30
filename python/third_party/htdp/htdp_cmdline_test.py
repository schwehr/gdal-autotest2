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
"""Tests the htdp line application.

Test input comes from:

https://www.ngs.noaa.gov/TOOLS/Htdp/HTDP-user-guide.pdf
"""

import os
import subprocess
import unittest

import flags
import resources

FLAGS = flags.FLAGS


class HtdpCmdlineTest(unittest.TestCase):

  def setUp(self):
    self.htdp = os.path.join(resources.GetARootDirWithAllResources(),
                             'third_party/htdp/htdp')

  def testExit(self):
    cmd = [self.htdp]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stdin=subprocess.PIPE)
    result = proc.communicate(input='\n0\n')[0]
    self.assertIn('0... Exit software', result)

  def testExercise1Part1(self):
    # Request one point
    stdin_text = (
        '\n'
        '2\n'
        '/dev/null\n'
        '1\n'
        '1\n'
        'alpha\n'
        '1\n'
        '1\n'
        '38,6,12.96\n'
        '122,56,7.80\n'
        '0\n'
        '0\n'
        '0\n')
    cmd = [self.htdp]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stdin=subprocess.PIPE)
    result = proc.communicate(input=stdin_text)[0]

    self.assertIn('Northward velocity =  42.05 mm/yr', result)
    self.assertIn('Eastward velocity  = -63.95 mm/yr', result)
    self.assertIn('Upward velocity    =  -0.44 mm/yr', result)

    self.assertIn('X-dim. velocity    = -52.78 mm/yr', result)
    self.assertIn('Y-dim. velocity    =  36.15 mm/yr', result)
    self.assertIn('Z-dim. velocity    =  42.02 mm/yr', result)


if __name__ == '__main__':
  unittest.main()
