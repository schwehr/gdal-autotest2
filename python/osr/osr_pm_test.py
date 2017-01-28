#!/usr/bin/env python

# Copyright (c) 2003, Frank Warmerdam <warmerdam@pobox.com>
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
# OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.

"""Test of prime meridian issues with EPSG translation and evaluation by PROJ.4.

Rewrite of

http://trac.osgeo.org/gdal/browser/trunk/autotest/osr/osr_pm.py
"""
import unittest


from osgeo import osr
import unittest


class OsrPm(unittest.TestCase):

  def testPm01PrimeAndCentralMeridianEpsg27572(self):
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(27572)

    # Prime meridian in Paris, France.
    # http://spatialreference.org/ref/epsg/27572/
    self.assertAlmostEqual(float(srs.GetAttrValue('PRIMEM', 1)), 2.33722917)
    self.assertAlmostEqual(srs.GetProjParm(osr.SRS_PP_CENTRAL_MERIDIAN), 0)

  def testPm02Proj4Epsg27572(self):
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(27572)
    proj4_srs = srs.ExportToProj4()
    self.assertIn('+pm=paris', proj4_srs)
    self.assertIn('+lon_0=0', proj4_srs)

  def testPm03Proj4ToWkt(self):
    srs = osr.SpatialReference()
    srs.ImportFromProj4('+proj=utm +zone=30 +datum=WGS84 +pm=bogota')

    self.assertAlmostEqual(float(srs.GetAttrValue('PRIMEM', 1)),
                           -74.08091666678081)
    self.assertAlmostEqual(srs.GetProjParm(osr.SRS_PP_CENTRAL_MERIDIAN), -3.0)


if __name__ == '__main__':
  unittest.main()
