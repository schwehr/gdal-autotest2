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

// Test creating Cloud Optimized GeoTiffs (cogeo).
//
// See also:
//   https://www.cogeo.org/
//   https://github.com/OSGeo/gdal/blob/master/swig/python/gdal-utils/osgeo_utils/samples/validate_cloud_optimized_geotiff.py

package org.gdal.frmts.gtiff;

import static com.google.common.truth.Truth.assertThat;

import com.google.common.labs.command.Command;
import com.google.devtools.build.runtime.RunfilesPaths;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.Arrays;
import java.util.Vector;
import org.gdal.gdal.Dataset;
import org.gdal.gdal.Driver;
import org.gdal.gdal.gdal;
import org.gdal.gdalconst.gdalconst;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;

@RunWith(JUnit4.class)
public final class CogeoTest {

  private static final Path TESTDATA_DIR =
      RunfilesPaths.resolve(
          "third_party/gdal/autotest2/javatests/org/gdal/frmts/gtiff/testdata/cogeo");

  @Rule public TemporaryFolder testFolder = new TemporaryFolder();

  private static final String GEOTIFF_GDAL_FORMAT = "GTiff";
  private Driver driver;

  private static final Path VALIDATE_CMD =
      RunfilesPaths.resolve("third_party/gdal/validate_cloud_optimized_geotiff");

  @Before
  public void setupGdal() {
    gdal.AllRegister();
    driver = gdal.GetDriverByName(GEOTIFF_GDAL_FORMAT);
  }

  @Test
  public void createCogeo() throws Exception {
    Path original = TESTDATA_DIR.resolve("tile.tif");

    String testPath = testFolder.getRoot().getAbsolutePath();
    String tmpPath = testPath + "/tile-tmp.tif";
    String dstPath = testPath + "/tile-cogeo.tif";

    Files.copy(original, Paths.get(tmpPath));
    // Required for testing environments where the original is read only.
    Files.setPosixFilePermissions(Paths.get(tmpPath), PosixFilePermissions.fromString("rw-------"));
    assertThat(Files.isWritable(Paths.get(tmpPath))).isTrue();

    Dataset src = gdal.Open(tmpPath, gdalconst.GA_Update);
    assertThat(src).isNotNull();

    int[] overviewList = {2, 4, 8, 16, 32};
    assertThat(src.BuildOverviews("average", overviewList)).isEqualTo(gdalconst.CE_None);

    @SuppressWarnings("JdkObsolete") // The GDAL API requires a Vector<String>
    Vector<String> options =
        new Vector<>(Arrays.asList("TILED=YES", "COMPRESS=LZW", "COPY_SRC_OVERVIEWS=YES"));
    Dataset dst = driver.CreateCopy(dstPath, src, options);
    assertThat(dst).isNotNull();
    // Be aggressive about shoving this to disk.  Without the delete, you get a file, but not a
    // valid Cloud Optimized Geotiff.  You will likely get a validation error relating to
    // BLOCK_OFFSET_0_0 if you skip the delete call.
    dst.delete();

    Command.command(VALIDATE_CMD.toString(), dstPath).execute();
  }
}
