// Copyright 2017 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gdal/ogr/ogrspatialreference.cpp
//   https://github.com/OSGeo/gdal/blob/master/autotest/osr/osr_basic.py
//   https://github.com/schwehr/gdal-autotest2/blob/master/cpp/ogr/ogrspatialreference_test.cc

package org.gdal.ogr;

import static com.google.common.truth.Truth.assertThat;
import static org.junit.Assert.assertThrows;

import org.gdal.osr.SpatialReference;
import org.gdal.osr.osrJNI;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;

@RunWith(JUnit4.class)
public final class OgrSpatialReferenceTest {

  @Test
  public void testEmpty() throws Exception {
    SpatialReference srs = new SpatialReference();
    assertThat(srs.IsGeographic()).isEqualTo(0); // False
    assertThat(srs.IsProjected()).isEqualTo(0); // False
    assertThat(srs.IsGeocentric()).isEqualTo(0); // False
    assertThat(srs.IsLocal()).isEqualTo(0); // False
    assertThat(srs.IsVertical()).isEqualTo(0); // False
    assertThat(srs.IsCompound()).isEqualTo(0); // False
  }

  @Test
  public void testWgs84() throws Exception {
    SpatialReference srs = new SpatialReference();

    assertThat(srs.ImportFromEPSG(4326)).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(srs.IsGeographic()).isEqualTo(1); // True
    assertThat(srs.IsProjected()).isEqualTo(0); // False
    assertThat(srs.IsGeocentric()).isEqualTo(0);
    assertThat(srs.IsLocal()).isEqualTo(0);
    assertThat(srs.IsVertical()).isEqualTo(0);
    assertThat(srs.IsCompound()).isEqualTo(0);
  }

  @Test
  public void testUtm60S() throws Exception {
    SpatialReference srs = new SpatialReference();
    int result =
        srs.ImportFromWkt(
            "PROJCS[\"WGS 84 / UTM zone 60S\","
                + "  GEOGCS[\"WGS 84\","
                + "      DATUM[\"WGS_1984\","
                + "          SPHEROID[\"WGS 84\",6378137,298.257223563,"
                + "              AUTHORITY[\"EPSG\",\"7030\"]],"
                + "          AUTHORITY[\"EPSG\",\"6326\"]],"
                + "      PRIMEM[\"Greenwich\",0,"
                + "          AUTHORITY[\"EPSG\",\"8901\"]],"
                + "      UNIT[\"degree\",0.0174532925199433,"
                + "          AUTHORITY[\"EPSG\",\"9122\"]],"
                + "      AUTHORITY[\"EPSG\",\"4326\"]],"
                + "  PROJECTION[\"Transverse_Mercator\"],"
                + "  PARAMETER[\"latitude_of_origin\",0],"
                + "  PARAMETER[\"central_meridian\",177],"
                + "  PARAMETER[\"scale_factor\",0.9996],"
                + "  PARAMETER[\"false_easting\",500000],"
                + "  PARAMETER[\"false_northing\",10000000],"
                + "  UNIT[\"metre\",1,"
                + "      AUTHORITY[\"EPSG\",\"9001\"]],"
                + "  AXIS[\"Easting\",EAST],"
                + "  AXIS[\"Northing\",NORTH],"
                + "  AUTHORITY[\"EPSG\",\"32760\"]]");
    assertThat(result).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(srs.IsGeographic()).isEqualTo(0); // False
    assertThat(srs.IsProjected()).isEqualTo(1); // True
    assertThat(srs.IsGeocentric()).isEqualTo(0);
    assertThat(srs.IsLocal()).isEqualTo(0);
    assertThat(srs.IsVertical()).isEqualTo(0);
    assertThat(srs.IsCompound()).isEqualTo(0);
  }

  @Test
  public void testImportFail() throws Exception {
    osrJNI.DontUseExceptions();
    SpatialReference srs = new SpatialReference();
    assertThat(srs.ImportFromWkt("BOGUS")).isEqualTo(ogrConstants.OGRERR_CORRUPT_DATA);
  }

  @Test
  public void testImportFailException() throws Exception {
    osrJNI.UseExceptions();
    SpatialReference srs = new SpatialReference();
    assertThrows(RuntimeException.class, () -> srs.ImportFromWkt("BOGUS"));
  }
}
