#!/usr/bin/env python

# Copyright 2014 Google Inc. All Rights Reserved.
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

# This is a complete rewrite of a file licensed as follows:

# Copyright (c) 2007, Frank Warmerdam <warmerdam@pobox.com>
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

"""Test NetCDF reading.

Format is described here:

http://www.gdal.org/frmt_netcdf.html

Inspired by netcdf.py:

http://trac.osgeo.org/gdal/browser/trunk/autotest/gdrivers/netcdf.py
"""


import unittest
from autotest2.gdrivers import gdrivers_util

@gdrivers_util.SkipIfDriverMissing(gdrivers_util.HDF5IMAGE_DRIVER)
class NetcdfHdf5Test(gdrivers_util.DriverTestCase):
  """NetCDF-4 format using HDF5."""

  def setUp(self):
    super(NetcdfHdf5Test, self).setUp(gdrivers_util.HDF5IMAGE_DRIVER, 'nc')

  def testNetcdfPamless(self):
    self.CheckOpen(
        gdrivers_util.GetTestFilePath('trmm-nc4.nc'), check_driver=True)
    self.assertEquals('CF-1.4', self.src.GetMetadata_Dict()['Conventions'])
    self.CheckProjection('')
    self.CheckGeoTransform((0.0, 1.0, 0.0, 0.0, 0.0, 1.0))
    self.assertEquals(1, self.src.RasterCount)
    band = self.src.GetRasterBand(1)
    self.assertEquals(14, band.Checksum())
    self.assertEquals('-9999.9004 ', band.GetMetadata_Dict()['pcp__FillValue'])


@gdrivers_util.SkipIfDriverMissing(gdrivers_util.NETCDF_DRIVER)
class NetcdfTest(gdrivers_util.DriverTestCase):
  """NetCDF Classic format."""

  def setUp(self):
    super(NetcdfTest, self).setUp(gdrivers_util.NETCDF_DRIVER, 'nc')

  def testNetcdfProjection(self):
    self.CheckOpen(gdrivers_util.GetTestFilePath('orog_CRCM1.nc'))
    self.CheckProjection(
        'PROJCS["unnamed",'
        '    GEOGCS["WGS 84",'
        '        DATUM["WGS_1984",'
        '            SPHEROID["WGS 84",6378137,298.257223563,'
        '                AUTHORITY["EPSG","7030"]],'
        '            TOWGS84[0,0,0,0,0,0,0],'
        '                AUTHORITY["EPSG","6326"]],'
        '        PRIMEM["Greenwich",0,'
        '            AUTHORITY["EPSG","8901"]],'
        '        UNIT["degree",0.0174532925199433,'
        '            AUTHORITY["EPSG","9108"]],'
        '        AUTHORITY["EPSG","4326"]],'
        '    PROJECTION["Polar_Stereographic"],'
        '    PARAMETER["latitude_of_origin",60],'
        '    PARAMETER["central_meridian",263],'
        '    PARAMETER["scale_factor",1],'
        '    PARAMETER["false_easting",3450000],'
        '    PARAMETER["false_northing",7450000],'
        '    UNIT["metre",1,'
        '        AUTHORITY["EPSG","9001"]]]')


if __name__ == '__main__':
  unittest.main()
