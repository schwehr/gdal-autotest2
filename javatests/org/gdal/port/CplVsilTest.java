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

// Test functions from cpl_vsil.cpp.
//
// See:
//   https://github.com/schwehr/gdal-autotest2/blob/master/cpp/port/cpl_vsil_test.cc

package org.gdal.port;

import static com.google.common.truth.Truth.assertThat;

import java.util.List;
import org.gdal.gdal.gdal;
import org.gdal.gdalconst.gdalconst;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;

@RunWith(JUnit4.class)
public final class CplVsilTest {

  @Rule public TemporaryFolder testFolder = new TemporaryFolder();

  private static final int SUCCESS = 0;
  private static final int FAILURE = -1;

  private static void checkNoContextFailure(int condition) {
    assertThat(condition).isEqualTo(FAILURE);
    assertThat(gdal.GetLastErrorType()).isEqualTo(gdalconst.CE_None);
  }

  private static void checkSuccess(int condition) {
    assertThat(condition).isEqualTo(SUCCESS);
  }

  @Test
  public void pathTest_doesNotExist() throws Exception {
    // These calls do not set the error context.
    assertThat(gdal.ReadDir("/does/not/exist0")).isEmpty();
    assertThat(gdal.GetLastErrorType()).isEqualTo(gdalconst.CE_None);

    checkNoContextFailure(gdal.Mkdir("/does/not/exist1", 0755));
    checkNoContextFailure(gdal.Unlink("/does/not/exist2"));
    checkNoContextFailure(gdal.Rmdir("/does/not/exist3"));

    // Recursive
    checkNoContextFailure(gdal.MkdirRecursive("/does/not/exist4", 0755));
    assertThat(gdal.ReadDirRecursive("/does/not/exist5")).isEmpty();
    assertThat(gdal.GetLastErrorType()).isEqualTo(gdalconst.CE_None);
    checkNoContextFailure(gdal.RmdirRecursive("/does/not/exist6"));
  }

  @Test
  public void localTempDir() throws Exception {
    String testPath = testFolder.getRoot().getAbsolutePath();

    String srcDir = testPath + "/foo";
    String dstDir = testPath + "/bar";
    checkSuccess(gdal.Mkdir(srcDir, 0755));
    checkSuccess(gdal.Rename(srcDir, dstDir));
    // Error on overwriting.
    checkNoContextFailure(gdal.Mkdir(dstDir, 0755));

    String subDir = dstDir + "/baz";
    checkSuccess(gdal.Mkdir(subDir, 0755));

    assertThat(gdal.ReadDir(subDir)).containsExactly(".", "..");
    assertThat(gdal.ReadDir(dstDir)).containsExactly(".", "..", "baz");

    // Fail to rmdir with contents.
    checkNoContextFailure(gdal.Rmdir(dstDir));

    // Unlink will not remove a directory even if it is empty.
    checkNoContextFailure(gdal.Unlink(subDir));
    checkSuccess(gdal.Rmdir(subDir));
  }

  @Test
  public void localTempDirRecursive() throws Exception {
    String testPath = testFolder.getRoot().getAbsolutePath();
    String dir = testPath + "/a/b/c";
    checkSuccess(gdal.MkdirRecursive(dir, 0755));
    assertThat(gdal.ReadDirRecursive(testPath)).containsExactly("a/", "a/b/", "a/b/c/");
    checkSuccess(gdal.RmdirRecursive(dir));
  }

  @Test
  public void vsimemRecursive() throws Exception {
    String testPath = "/vsimem/";
    String dir = testPath + "a/b/c";
    checkSuccess(gdal.MkdirRecursive(dir, 0755));
    assertThat(gdal.ReadDirRecursive(testPath)).containsExactly("a/", "a/b/", "a/b/c/");
    checkSuccess(gdal.RmdirRecursive(dir));
  }

  @Test
  public void fileSystemsPrefixes() throws Exception {
    @SuppressWarnings("unchecked")
    // Convert from Vector.
    List<String> prefixes = gdal.GetFileSystemsPrefixes();
    assertThat(prefixes)
        .containsAtLeast(
            "/vsicrypt/",
            "/vsigzip/",
            "/vsimem/",
            "/vsisparse/",
            "/vsistdin/",
            "/vsistdout/",
            "/vsistdout_redirect/",
            "/vsisubfile/",
            "/vsitar/",
            "/vsizip/");
  }

  private static void assertNullOption(String filesystemOption) {
    assertThat(gdal.GetFileSystemOptions(filesystemOption)).isNull();
  }

  @Test
  public void fileSystemOptions() throws Exception {
    // Only the web interfaces have options.
    assertNullOption("/vsicrypt/");
    // "/vsigzip/" has the GDAL_NUM_THREADS option.
    assertNullOption("/vsisparse/");
    assertNullOption("/vsistdin/");
    assertNullOption("/vsistdout/");
    assertNullOption("/vsisubfile/");
    assertNullOption("/vsitar/");
    // "/vsizip/" has the GDAL_NUM_THREADS option.
  }
}
