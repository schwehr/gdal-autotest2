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
"""Tests the Proj cs2cs command line application."""

import os
import subprocess
import unittest

from pyglib import flags
from pyglib import resources

FLAGS = flags.FLAGS


class Cs2CsTest(unittest.TestCase):

  def setUp(self):
    self.cs2cs = os.path.join(resources.GetARootDirWithAllResources(),
                              'third_party/proj4/cs2cs')

  def testHelp(self):
    cmd = [self.cs2cs]
    result = subprocess.check_output(cmd, stderr=subprocess.STDOUT)
    self.assertIn('usage:', result)

  def testList(self):
    cmd = [self.cs2cs, '-l']
    result = subprocess.check_output(cmd)
    self.assertIn('wintri : Winkel Tripel', result)

  def testListLowerP(self):
    cmd = [self.cs2cs, '-lp']
    result = subprocess.check_output(cmd)
    self.assertIn('wink2 : Winkel II', result)

  def testListP(self):
    # Detailed list
    cmd = [self.cs2cs, '-lP']
    result = subprocess.check_output(cmd)
    self.assertIn('PCyl', result)

  def testListEqual(self):
    # Detailed list
    cmd = [self.cs2cs, '-l=ups']
    result = subprocess.check_output(cmd)
    self.assertIn('Universal Polar Stereographic', result)
    self.assertIn('Azi', result)
    self.assertNotIn('PCyl', result)
    self.assertNotIn('wintri', result)

  def testListEllipsoidIdentifiers(self):
    cmd = [self.cs2cs, '-le']
    result = subprocess.check_output(cmd)
    self.assertIn('evrst30', result)
    self.assertIn('a=6377276.345', result)
    self.assertIn('rf=300.8017', result)
    self.assertIn('Everest 1830', result)

  def testListUnits(self):
    cmd = [self.cs2cs, '-lu']
    result = subprocess.check_output(cmd)
    self.assertIn('ch', result)
    self.assertIn('20.1168', result)
    self.assertIn('International Chain', result)

  def testListDatums(self):
    cmd = [self.cs2cs, '-ld']
    result = subprocess.check_output(cmd)
    self.assertIn('NAD27', result)
    self.assertIn('clrk66', result)
    self.assertIn('conus', result)

  def testTransform(self):
    cmd = [self.cs2cs, '+proj=latlong', '+datum=NAD83',
           '+to', '+proj=utm', '+zone=10', '+datum=NAD27', '-r']
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    # Pass in latitude longitude to transform to UTM.
    stdout, _ = proc.communicate('45d15\'33.1"   111.5W\n')
    result = [float(val) for val in stdout.replace('\t', ' ').split(' ')]
    self.assertEqual(len(result), 3)
    self.assertAlmostEqual(result[0], 1402285.98, delta=0.001)
    self.assertAlmostEqual(result[1], 5076292.42)
    self.assertAlmostEqual(result[2], 0.0)

# TODO(schwehr): Add more tests

if __name__ == '__main__':
  unittest.main()
