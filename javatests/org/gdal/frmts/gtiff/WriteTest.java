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

// Test writing GeoTiffs from Java.

package org.gdal.frmts.gtiff;

import static com.google.common.truth.Truth.assertThat;

import com.google.testing.util.TestUtil;
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
public final class WriteTest {
  private static final String GEOTIFF_GDAL_FORMAT = "GTiff";
  private static final String TESTDATA_DIR =
      TestUtil.getSrcDir()
          + "/google3/third_party/gdal/autotest2/javatests/org/gdal/frmts/gtiff/testdata/";

  private Driver driver;

  private static class QuietHandler implements AutoCloseable {
    public QuietHandler() {
      gdal.PushErrorHandler("CPLQuietErrorHandler");
    }

    @Override
    public void close() throws Exception {
      gdal.PopErrorHandler();
    }
  }

  @Before
  public void setupGdal() {
    gdal.AllRegister();
    driver = gdal.GetDriverByName(GEOTIFF_GDAL_FORMAT);
  }

  @Test
  public void createCopy_minimum() throws Exception {
    String filename = "minimum.tif";
    String srcPath = TESTDATA_DIR + filename;
    Dataset src = gdal.Open(srcPath);
    assertThat(src).isNotNull();

    String dstPath = "/vsimem/" + filename;
    {
      Dataset dst = driver.CreateCopy(dstPath, src);
      assertThat(dst).isNotNull();

      Band band = dst.GetRasterBand(1);
      assertThat(band).isNotNull();
      assertThat(band.GetXSize()).isEqualTo(1);
      assertThat(band.GetYSize()).isEqualTo(1);
      assertThat(band.Checksum()).isEqualTo(0);
    }

    // Reopen the new file and make sure it all got there.
    Dataset dataset = gdal.Open(dstPath);
    assertThat(dataset).isNotNull();

    Band band = dataset.GetRasterBand(1);
    assertThat(band).isNotNull();
    assertThat(band.GetXSize()).isEqualTo(1);
    assertThat(band.GetYSize()).isEqualTo(1);
    assertThat(band.Checksum()).isEqualTo(0);
  }

  @Test
  public void createToNonexistantPath_fails() throws Exception {
    String path = "/does/not/exist";
    try (QuietHandler handler = new QuietHandler()) {
      Dataset dataset = driver.Create(path, 1, 1);
      assertThat(dataset).isNull();
      assertThat(gdal.GetLastErrorNo()).isEqualTo(gdalconst.CPLE_OpenFailed);
      assertThat(gdal.GetLastErrorType()).isEqualTo(gdalconst.CPLE_FileIO);
      assertThat(gdal.GetLastErrorMsg()).contains("No such file or directory");
    }
  }

  @Test
  public void createByteFile() throws Exception {
    String path = "/vsimem/byte.tif";
    int width = 2;
    int height = 1;
    int bands = 1;
    int eType = gdalconst.GDT_Byte;
    byte[] data = {1, 2};
    int[] bandList = {1};
    int xOff = 0;
    int yOff = 0;

    // Destination
    Dataset dataset = driver.Create(path, width, height, bands, eType);
    assertThat(dataset).isNotNull();

    int result =
        dataset.WriteRaster(xOff, yOff, width, height, width, height, eType, data, bandList);
    assertThat(result).isEqualTo(gdalconst.CE_None);
    dataset.FlushCache();

    // Reopen the new file and make sure it all got there.
    dataset = gdal.Open(path);
    assertThat(dataset).isNotNull();

    Band band = dataset.GetRasterBand(1);
    assertThat(band).isNotNull();
    assertThat(band.GetXSize()).isEqualTo(width);
    assertThat(band.GetYSize()).isEqualTo(height);
    assertThat(band.Checksum()).isEqualTo(3);
  }
}
