#!/usr/bin/env python
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
#
# This is a complete rewrite of a file licensed as follows:
#
# Copyright (c) 2010, Andrey Kiselev <dron@ak4719.spb.edu>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Library General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Library General Public License for more details.
#
# You should have received a copy of the GNU Library General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

"""Test of ERMapper spatial reference implementation.

Rewrite of

http://trac.osgeo.org/gdal/browser/trunk/autotest/osr/osr_erm.py
"""

import unittest


from osgeo import osr
import unittest


class OsrErm(unittest.TestCase):

  # TODO(schwehr): Broken test.
  @unittest.skip('Fix this test')
  def testErm01SphericalInverseFlattening(self):
    # http://trac.osgeo.org/gdal/ticket/3787
    # Spherical datums should have inverse flattening parameter 0, not 1.
    for datum in ['SPHERE', 'SPHERE2', 'USSPHERE']:
      srs = osr.SpatialReference()
      srs.ImportFromERM('MRWORLD', datum, 'METRE')

      self.assertEqual(srs.GetInvFlattening(), 0)

  def testErm02UnsupportedSrsesTranslatedFromEpsgColonN(self):
    # http://trac.osgeo.org/gdal/ticket/3955
    srs = osr.SpatialReference()
    self.assertEqual(srs.ImportFromERM('EPSG:3395', 'EPSG:3395', 'METRE'), 0)

    srs2 = osr.SpatialReference()
    srs2.SetFromUserInput('EPSG:3395')

    self.assertTrue(srs2.IsSame(srs))


if __name__ == '__main__':
  unittest.main()
