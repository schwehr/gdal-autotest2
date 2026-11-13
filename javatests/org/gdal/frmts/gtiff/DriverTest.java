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

// Test GeoTiff driver properties.

package org.gdal.frmts.gtiff;

import static com.google.common.truth.Truth.assertThat;

import java.util.Arrays;
import java.util.List;
import org.gdal.gdal.Driver;
import org.gdal.gdal.gdal;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;

@RunWith(JUnit4.class)
public final class DriverTest {

  @Before
  public void setupGdal() {
    gdal.AllRegister();
  }

  @Test
  public void basicGeoTiffDriver() throws Exception {
    // This test covers how the GeoTiff driver is configured inside of Google.
    assertThat(gdal.GetDriverCount()).isGreaterThan(0);
    Driver driver = gdal.GetDriverByName("gtiff");
    assertThat(driver).isNotNull();
    Driver driver2 = gdal.GetDriverByName("GTIFF");
    assertThat(driver).isNotNull();

    assertThat(driver.getShortName()).isEqualTo("GTiff");
    // Sadly, the above may return 2 separate instances of Driver, so compare with short name.
    assertThat(driver2.getShortName()).isEqualTo(driver.getShortName());
    assertThat(driver.getLongName()).isEqualTo("GeoTIFF");

    // This file may not be on a local system.  Find it here:
    // http://www.gdal.org/frmt_gtiff.html
    assertThat(driver.getHelpTopic()).isEqualTo("drivers/raster/gtiff.html");

    assertThat(driver.GetDescription()).isEqualTo("GTiff");

    @SuppressWarnings("unchecked")
    // Convert from Vector.
    List<String> domains = driver.GetMetadataDomainList();
    assertThat(domains).containsExactly("");

    @SuppressWarnings("unchecked")
    // Convert from Vector.
    List<String> metadata = driver.GetMetadata_List();

    // These are capabilities that we always expect to be there.
    assertThat(metadata).contains("DCAP_CREATE=YES");
    assertThat(metadata).contains("DCAP_CREATECOPY=YES");
    assertThat(metadata).contains("DCAP_OPEN=YES");
    assertThat(metadata).contains("DCAP_RASTER=YES");
    assertThat(metadata).contains("DCAP_VIRTUALIO=YES");

    // Basic information about tifs that should never change.
    assertThat(metadata).contains("DMD_EXTENSION=tif");
    assertThat(metadata).contains("DMD_EXTENSIONS=tif tiff");
    assertThat(metadata).contains("DMD_MIMETYPE=image/tiff");

    // Make sure the driver supports creating all types of pixel data types.
    List<String> creationTypes =
        Arrays.asList(driver.GetMetadataItem("DMD_CREATIONDATATYPES").split(" "));
    assertThat(creationTypes)
        .containsExactly(
            "Byte",
            "Int8",
            "UInt16",
            "Int16",
            "UInt32",
            "Int32",
            "Float32",
            "Float64",
            "CInt16",
            "CInt32",
            "CFloat32",
            "CFloat64");

    // Don't try to parse this XML string.
    String openOptionList = driver.GetMetadataItem("DMD_OPENOPTIONLIST");
    assertThat(openOptionList).contains("NUM_THREADS");
    assertThat(openOptionList).contains("GEOTIFF_KEYS_FLAVOR");
    assertThat(openOptionList).contains("GEOREF_SOURCES");
    assertThat(openOptionList).contains("SPARSE_OK");
  }
}
