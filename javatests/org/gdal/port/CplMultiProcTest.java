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
//   https://trac.osgeo.org/gdal/browser/trunk/gdal/port/cpl_multiproc.cpp
//   https://github.com/schwehr/gdal-autotest2/blob/master/cpp/port/cpl_multiproc_test.cc

package org.gdal.port;

import static com.google.common.truth.Truth.assertThat;

import org.gdal.gdal.gdal;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;

@RunWith(JUnit4.class)
public final class CplMultiProcTest {
  static final int TRUE = 1;

  @Test
  public void testCplMultiProc() throws Exception {
    // GDAL Java wrapper must always have GDAL built with threading support.
    assertThat(gdal.HasThreadSupport()).isEqualTo(TRUE);
  }
}
