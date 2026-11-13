// Copyright 2024 Google Inc. All Rights Reserved.
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

// Test the NetCDF driver.

package org.gdal.frmts.netcdf;

import static com.google.common.truth.Truth.assertThat;

import com.google.testing.util.TestUtil;
import java.nio.file.Path;
import org.gdal.gdal.Dataset;
import org.gdal.gdal.Driver;
import org.gdal.gdal.gdal;
import org.gdal.gdalconst.gdalconst;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;

@RunWith(JUnit4.class)
public final class DriverTest {

  private static final String TESTDATA_DIR =
      TestUtil.getSrcDir()
          + "/google3/third_party/gdal/autotest2/javatests/org/gdal/frmts/netcdf/testdata/";

  @Before
  public void setupGdal() {
    gdal.AllRegister();
  }

  @Test
  public void basicNetcdfDriver() throws Exception {
    Driver driver = gdal.GetDriverByName("NetCDF");
    assertThat(driver).isNotNull();
  }

  @Test
  public void openSubdataset() throws Exception {
    Path testPath =
        Path.of(
            TESTDATA_DIR,
            "OR_ABI-L2-MCMIPM2-M6_G18_s20222141747552_e20222141748021_c20222141748096.nc");

    String spec = "NETCDF:\"" + testPath + "\":DQF_C16";
    Dataset src = gdal.Open(spec, gdalconst.GA_ReadOnly);
    assertThat(src).isNotNull();
  }

  @Test
  public void openGridmet() throws Exception {
    // Observed in b/383474279.
    Path testPath = Path.of(TESTDATA_DIR, "provisional_gridmet_20241026.nc");

    String spec = "NETCDF:\"" + testPath + "\":maximum_relative_humidity";
    Dataset src = gdal.Open(spec, gdalconst.GA_ReadOnly);
    assertThat(src).isNotNull();
  }
}
