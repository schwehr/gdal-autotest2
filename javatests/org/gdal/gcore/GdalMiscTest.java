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

// See also:
//   https://trac.osgeo.org/gdal/browser/trunk/gdal/gcore/gdal_misc.cpp
//   https://github.com/schwehr/gdal-autotest2/blob/master/cpp/gcore/gdal_misc_test.cc

package org.gdal.gcore;

import static com.google.common.truth.Truth.assertThat;

import org.gdal.gdal.gdal;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;

@RunWith(JUnit4.class)
public final class GdalMiscTest {

  @Test
  public void testVersion() throws Exception {
    assertThat(Integer.parseInt(gdal.VersionInfo())).isAtLeast(2030000);
    assertThat(Integer.parseInt(gdal.VersionInfo("VERSION_NUM"))).isAtLeast(2030000);
    assertThat(gdal.VersionInfo("RELEASE_NAME")).contains("."); // e.g. 2.3.0dev
    assertThat(gdal.VersionInfo("RELEASE_DATE")).startsWith("20"); // e.g. 20179999
    assertThat(gdal.VersionInfo("--version")).contains("GDAL"); // e.g. GDAL 2.3.0
    assertThat(gdal.VersionInfo("")).contains("GDAL"); // Fall-through --version
    assertThat(gdal.VersionInfo("garbage")).contains("GDAL"); // Fall-through --version
    assertThat(gdal.VersionInfo("LICENSE")).contains("MIT");
    assertThat(gdal.VersionInfo("BUILD_INFO")).contains("GEOS_ENABLED=YES");
    assertThat(gdal.VersionInfo("BUILD_INFO")).doesNotContain("PAM_ENABLED=YES");
  }
}
