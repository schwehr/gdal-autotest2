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
# Copyright (c) 2004, Andrey Kiselev <dron@ak4719.spb.edu>
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

"""Test PCI translations.

Rewrite of

http://trac.osgeo.org/gdal/browser/trunk/autotest/osr/osr_pci.py
"""

import unittest


from osgeo import osr
import unittest

# pylint:disable=bad-whitespace
PRJ_PARMS_EMPTY = (0, ) * 17


class OsrPci(unittest.TestCase):

  def testPci01ImportFromPci(self):
    prj_parms = (0, 0, 45., 54.5, 47., 62., 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    srs = osr.SpatialReference()
    srs.ImportFromPCI('EC          E015', 'METRE', prj_parms)

    self.assertEqual(srs.GetProjParm(osr.SRS_PP_STANDARD_PARALLEL_1), 47.0)
    self.assertEqual(srs.GetProjParm(osr.SRS_PP_STANDARD_PARALLEL_2), 62.0)
    self.assertEqual(srs.GetProjParm(osr.SRS_PP_LATITUDE_OF_CENTER), 54.5)
    self.assertEqual(srs.GetProjParm(osr.SRS_PP_LONGITUDE_OF_CENTER), 45.0)
    self.assertEqual(srs.GetProjParm(osr.SRS_PP_FALSE_EASTING), 0.0)
    self.assertEqual(srs.GetProjParm(osr.SRS_PP_FALSE_NORTHING), 0.0)

    expected_srs = osr.SpatialReference(
        'PROJCS["unnamed",'
        '    GEOGCS["Unknown - PCI E015",'
        '        DATUM["Unknown - PCI E015",'
        '            SPHEROID["Krassowsky 1940",6378245,298.3,'
        '                AUTHORITY["EPSG","7024"]]],'
        '        PRIMEM["Greenwich",0],'
        '        UNIT["degree",0.0174532925199433]],'
        '    PROJECTION["Equidistant_Conic"],'
        '    PARAMETER["standard_parallel_1",47],'
        '    PARAMETER["standard_parallel_2",62],'
        '    PARAMETER["latitude_of_center",54.5],'
        '    PARAMETER["longitude_of_center",45],'
        '    PARAMETER["false_easting",0],'
        '    PARAMETER["false_northing",0],'
        '    UNIT["Meter",1]]'
    )
    self.assertTrue(srs.IsSame(expected_srs))

    pci_parms = srs.ExportToPCI()
    self.assertEqual(pci_parms[0], 'EC          E015')
    self.assertEqual(pci_parms[1], 'METRE')
    self.assertEqual(pci_parms[2], prj_parms)

  def testPci02ExportToPci(self):
    srs = osr.SpatialReference(
        'PROJCS["unnamed",'
        '    GEOGCS["NAD27",'
        '        DATUM["North_American_Datum_1927",'
        '            SPHEROID["Clarke 1866",6378206.4,294.9786982139006,'
        '                AUTHORITY["EPSG","7008"]],'
        '            AUTHORITY["EPSG","6267"]],'
        '        PRIMEM["Greenwich",0],'
        '        UNIT["degree",0.0174532925199433],'
        '        AUTHORITY["EPSG","4267"]],'
        '    PROJECTION["Lambert_Conformal_Conic_2SP"],'
        '    PARAMETER["standard_parallel_1",33.90363402777778],'
        '    PARAMETER["standard_parallel_2",33.62529002777778],'
        '    PARAMETER["latitude_of_origin",33.76446202777777],'
        '    PARAMETER["central_meridian",-117.4745428888889],'
        '    PARAMETER["false_easting",0],'
        '    PARAMETER["false_northing",0],'
        '    UNIT["metre",1,'
        '        AUTHORITY["EPSG","9001"]]]'
    )
    proj, units, parms = srs.ExportToPCI()

    self.assertEqual(proj, 'LCC         D-01')
    self.assertEqual(units, 'METRE')
    self.assertAlmostEqual(parms[2], -117.4745428888889)
    self.assertAlmostEqual(parms[3], 33.76446203)
    self.assertAlmostEqual(parms[4], 33.90363403)
    self.assertAlmostEqual(parms[5], 33.62529003)

  def testPci03Mgrs(self):
    # http://trac.osgeo.org/gdal/ticket/3379
    srs = osr.SpatialReference()
    srs.ImportFromPCI('UTM    13   D000', 'METRE', PRJ_PARMS_EMPTY)
    self.assertIn('13, Northern Hemi', srs.ExportToWkt())

    srs = osr.SpatialReference()
    srs.ImportFromPCI('UTM    13 G D000', 'METRE', PRJ_PARMS_EMPTY)

    self.assertIn('13, Southern Hemi', srs.ExportToWkt())

    srs = osr.SpatialReference()
    srs.ImportFromPCI('UTM    13 X D000', 'METRE',  PRJ_PARMS_EMPTY)

    self.assertIn('13, Northern Hemi', srs.ExportToWkt())

  def testPci04DatumFromPciDatumTxt(self):
    srs = osr.SpatialReference()
    srs.ImportFromPCI('LONG/LAT    D506', 'DEGREE', PRJ_PARMS_EMPTY)

    expected_srs = osr.SpatialReference(
        'GEOGCS["Rijksdriehoeks Datum",'
        '    DATUM["Rijksdriehoeks Datum",'
        '        SPHEROID["Bessel 1841",6377397.155,299.1528128,'
        '            AUTHORITY["EPSG","7004"]],'
        '        TOWGS84[565.04,49.91,465.84,0.4094,-0.3597,1.8685,'
        '4.077200000063286]],'
        '    PRIMEM["Greenwich",0],'
        '    UNIT["degree",0.0174532925199433]]'
    )
    self.assertTrue(srs.IsSame(expected_srs))

    pci_parms = srs.ExportToPCI()
    self.assertEqual(pci_parms[0], 'LONG/LAT    D506')
    self.assertEqual(pci_parms[1], 'DEGREE')
    self.assertEqual(pci_parms[2], PRJ_PARMS_EMPTY)

  def testPci05DatumFromPciEllipsTxt(self):
    srs = osr.SpatialReference()
    srs.ImportFromPCI('LONG/LAT    E224', 'DEGREE', PRJ_PARMS_EMPTY)

    expected_srs = osr.SpatialReference(
        'GEOGCS["Unknown - PCI E224",'
        '    DATUM["Unknown - PCI E224",'
        '        SPHEROID["Xian 1980",6378140,298.2569978029123]],'
        '    PRIMEM["Greenwich",0],'
        '    UNIT["degree",0.0174532925199433]]'
    )
    self.assertTrue(srs.IsSame(expected_srs))

    pci_parms = srs.ExportToPCI()
    self.assertEqual(pci_parms[0], 'LONG/LAT    E224')
    self.assertEqual(pci_parms[1], 'DEGREE')
    self.assertEqual(pci_parms[2], PRJ_PARMS_EMPTY)

  def testPci06DatumFromPciDatumTxt(self):
    srs = osr.SpatialReference()
    srs.ImportFromPCI('LONG/LAT    D030', 'DEGREE', PRJ_PARMS_EMPTY)

    expected_srs = osr.SpatialReference(
        'GEOGCS["AGD84",'
        '    DATUM["Australian_Geodetic_Datum_1984",'
        '        SPHEROID["Australian National Spheroid",6378160,298.25,'
        '            AUTHORITY["EPSG","7003"]],'
        '        TOWGS84[-134,-48,149,0,0,0,0],'
        '        AUTHORITY["EPSG","6203"]],'
        '    PRIMEM["Greenwich",0,'
        '        AUTHORITY["EPSG","8901"]],'
        '    UNIT["degree",0.0174532925199433,'
        '        AUTHORITY["EPSG","9122"]],'
        '    AUTHORITY["EPSG","4203"]]'
    )
    self.assertTrue(srs.IsSame(expected_srs))

    pci_parms = srs.ExportToPCI()
    self.assertEqual(pci_parms[0], 'LONG/LAT    D030')
    self.assertEqual(pci_parms[1], 'DEGREE')
    self.assertEqual(pci_parms[2], PRJ_PARMS_EMPTY)

  def testPci07OnlyToWgs84(self):
    srs = osr.SpatialReference(
        'GEOGCS["My GCS",'
        '    DATUM["My Datum",'
        '        SPHEROID["Bessel 1841",6377397.155,299.1528128,'
        '            AUTHORITY["EPSG","7004"]],'
        '        TOWGS84[565.04,49.91,465.84,0.4094,-0.3597,1.8685,'
        '4.077200000063286]],'
        '    PRIMEM["Greenwich",0],'
        '    UNIT["degree",0.0174532925199433]]'
    )

    pci_parms = srs.ExportToPCI()
    self.assertEqual(pci_parms[0], 'LONG/LAT    D506')
    self.assertEqual(pci_parms[1], 'DEGREE')
    self.assertEqual(pci_parms[2], PRJ_PARMS_EMPTY)


if __name__ == '__main__':
  unittest.main()
