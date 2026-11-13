// Copyright 2018 Google Inc. All Rights Reserved.
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

// Test Overview capabilities of GeoTiffs.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/autotest/gcore/tiff_ovr.py
//   https://github.com/OSGeo/gdal/blob/master/autotest/gcore/tiff_write.py
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/vrtovr.py
//   https://github.com/OSGeo/gdal/blob/master/autotest/utilities/test_gdaladdo.py

package org.gdal.frmts.gtiff;

import static com.google.common.truth.Truth.assertThat;

import com.google.testing.util.TestUtil;
import java.nio.file.Paths;
import java.util.List;
import java.util.stream.Collectors;
import org.gdal.gdal.Band;
import org.gdal.gdal.Dataset;
import org.gdal.gdal.Driver;
import org.gdal.gdal.gdal;
import org.gdal.gdalconst.gdalconst;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;

@RunWith(JUnit4.class)
public final class OverviewsTest {

  private static final String TESTDATA_DIR =
      TestUtil.getSrcDir()
          + "/google3/third_party/gdal/autotest2/javatests/org/gdal/frmts/gtiff/testdata/";

  private static final String GEOTIFF_GDAL_FORMAT = "GTiff";
  private Driver driver;

  @Before
  public void setupGdal() {
    gdal.AllRegister();
    driver = gdal.GetDriverByName(GEOTIFF_GDAL_FORMAT);
  }

  @Test
  public void embeddedOverviews() throws Exception {
    String path = TESTDATA_DIR + "with-overviews.tif";
    Dataset dataset = gdal.Open(path);
    assertThat(dataset).isNotNull();
    assertThat(dataset.GetRasterCount()).isEqualTo(1);

    Band band = dataset.GetRasterBand(1);
    assertThat(band.GetOverviewCount()).isEqualTo(3);

    // Note that bands are indexed starting at 1, but overviews start at 0.
    Band overview0 = band.GetOverview(0);
    assertThat(overview0).isNotNull();
    assertThat(overview0.GetXSize()).isEqualTo(250);
    assertThat(overview0.GetYSize()).isEqualTo(125);
    assertThat(overview0.Checksum()).isEqualTo(22064);
    Band overview2 = band.GetOverview(2);

    assertThat(overview2).isNotNull();
    assertThat(overview2.GetXSize()).isEqualTo(63);
    assertThat(overview2.GetYSize()).isEqualTo(32);
    assertThat(overview2.Checksum()).isEqualTo(5624);

    assertThat(band.GetOverview(3)).isNull();
  }

  @Test
  public void externalOverviews() throws Exception {
    String path = TESTDATA_DIR + "with-overview-file.tif";
    Dataset dataset = gdal.Open(path);
    assertThat(dataset).isNotNull();
    assertThat(dataset.GetRasterCount()).isEqualTo(1);

    @SuppressWarnings("unchecked")
    // Convert from Vector.
    List<String> filepaths = dataset.GetFileList();
    List<String> filenames =
        filepaths
            .stream()
            .map(p -> Paths.get(p).getFileName().toString())
            .collect(Collectors.toList());
    assertThat(filenames).containsExactly("with-overview-file.tif", "with-overview-file.tif.ovr");

    Band band = dataset.GetRasterBand(1);
    assertThat(band.GetOverviewCount()).isEqualTo(1);

    Band overview = band.GetOverview(0);
    assertThat(overview).isNotNull();
    assertThat(overview.GetXSize()).isEqualTo(63);
    assertThat(overview.GetYSize()).isEqualTo(32);
    assertThat(overview.Checksum()).isEqualTo(5682);

    assertThat(band.GetOverview(1)).isNull();
  }

  // The values used to fill the 4 quadrants of the generated test input grids.
  private static final double UL_VALUE = 1.0; // Northwest quadrant: 0, 0
  private static final double UR_VALUE = 2.0; // Northeast quadrant: width, 0
  private static final double LL_VALUE = 3.0; // Southwest quadrant: 0, height
  private static final double LR_VALUE = 4.0; // Southeast quadrant: width, height

  private static double[] createData(int width, int height) {
    // Fill data that is checked via the checksum.
    final int size = width * height;
    double[] data = new double[size];

    for (int y = 0; y < height; y++) {
      for (int x = 0; x < width; x++) {
        if (y < height / 2) {
          data[y * height + x] = x < width / 2 ? UL_VALUE : UR_VALUE;
        } else {
          data[y * height + x] = x < width / 2 ? LL_VALUE : LR_VALUE;
        }
      }
    }

    return data;
  }

  private Dataset createFile(Driver driver, String path, int width, int height, double[] data) {
    int bandCount = 1;
    int dataType = gdalconst.GDT_Float64;
    Dataset dataset = driver.Create(path, width, height, bandCount, dataType);
    assertThat(dataset).isNotNull();

    int xOff = 0;
    int yOff = 0;
    Band band = dataset.GetRasterBand(1);
    int result = band.WriteRaster(xOff, yOff, width, height, data);
    assertThat(result).isEqualTo(gdalconst.CE_None);
    return dataset;
  }

  @Test
  public void buildOverviewDoubleLevel1() throws Exception {
    // Create a single overview that is the exact copy of the original.
    // Checks the size and checksum of the overview.
    String path = "/vsimem/one-nearest.tif";
    int width = 20;
    int height = 10;

    double[] data = createData(width, height);
    Dataset dataset = createFile(driver, path, width, height, data);

    assertThat(dataset).isNotNull();
    int[] overviewList = {1};
    dataset.BuildOverviews("nearest", overviewList);
    assertThat(gdal.GetLastErrorNo()).isEqualTo(gdalconst.CE_None);

    Band band = dataset.GetRasterBand(1);
    assertThat(band).isNotNull();
    // The overview band should exactly match the band data.  The checksum verifies that the
    // band data is directly copied to the overview band.
    int checksum = 240;
    assertThat(band.Checksum()).isEqualTo(checksum);
    assertThat(band.GetOverviewCount()).isEqualTo(1);

    Band overview = band.GetOverview(0);
    assertThat(overview.GetXSize()).isEqualTo(width);
    assertThat(overview.GetYSize()).isEqualTo(height);

    assertThat(overview.Checksum()).isEqualTo(checksum);
  }

  @Test
  public void buildOverviewDouble() throws Exception {
    // Create two overviews at half and quarter resolution using the average algorithm.
    // Checks the size and checksum of each overview.
    String path = "/vsimem/two-four-average.tif";
    int width = 200;
    int height = 100;

    double[] data = createData(width, height);
    Dataset dataset = createFile(driver, path, width, height, data);

    assertThat(dataset).isNotNull();
    int[] overviewList = {2, 4};
    dataset.BuildOverviews("average", overviewList);
    assertThat(gdal.GetLastErrorNo()).isEqualTo(gdalconst.CE_None);

    Band band = dataset.GetRasterBand(1);
    assertThat(band).isNotNull();
    int checksum = 20400;
    assertThat(band.Checksum()).isEqualTo(checksum);
    assertThat(band.GetOverviewCount()).isEqualTo(2);

    Band overview0 = band.GetOverview(0);
    assertThat(overview0.GetXSize()).isEqualTo(width / 2);
    assertThat(overview0.GetYSize()).isEqualTo(height / 2);
    assertThat(overview0.Checksum()).isEqualTo(5100);

    Band overview1 = band.GetOverview(1);
    assertThat(overview1.GetXSize()).isEqualTo(width / 4);
    assertThat(overview1.GetYSize()).isEqualTo(height / 4);
    assertThat(overview1.Checksum()).isEqualTo(1325);
  }
}
