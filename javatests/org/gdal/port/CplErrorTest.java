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

// Test functions from C++ cpl_error.
//
// See:
//   https://trac.osgeo.org/gdal/browser/trunk/gdal/port/cpl_error.h
//   https://trac.osgeo.org/gdal/browser/trunk/gdal/port/cpl_error.cpp

package org.gdal.port;

import static com.google.common.truth.Truth.assertThat;

import org.gdal.gdal.gdal;
import org.gdal.gdalconst.gdalconst;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;

@RunWith(JUnit4.class)
public final class CplErrorTest {
  static final String DEFAULT_HANDLER = "CPLDefaultErrorHandler";
  static final String LOGGING_HANDLER = "CPLLoggingErrorHandler";
  static final String QUIET_HANDLER = "CPLQuietErrorHandler";

  @Before
  public void setUp() {
    gdal.ErrorReset();
  }

  private static class QuietHandler implements AutoCloseable {
    public QuietHandler() {
      gdal.PushErrorHandler(QUIET_HANDLER);
    }

    @Override
    public void close() throws Exception {
      gdal.PopErrorHandler();
    }
  }

  @Test
  public void errorHandlerStack_quietHandler() throws Exception {
    gdal.PushErrorHandler(QUIET_HANDLER);
    gdal.PopErrorHandler();
    // Okay to call if nothing on the error handler stack.
    gdal.PopErrorHandler();
  }

  @Test
  public void errorHandlerStack_noSuchHandler() throws Exception {
    gdal.PushErrorHandler("No such handler");
    // There is no way to find out that it failed.
    assertThat(gdal.GetLastErrorType()).isEqualTo(gdalconst.CE_None);
    gdal.PopErrorHandler();
    // Okay to call if nothing on the error handler stack.
    gdal.PopErrorHandler();
  }

  @Test
  public void errorContext_noErrors() throws Exception {
    assertThat(gdal.GetErrorCounter()).isEqualTo(0);
    assertThat(gdal.GetLastErrorType()).isEqualTo(gdalconst.CE_None);
    assertThat(gdal.GetLastErrorNo()).isEqualTo(gdalconst.CPLE_None);
    assertThat(gdal.GetLastErrorMsg()).isEmpty();

    // See: cpl_vsi_error.{cpp,h}
    assertThat(gdal.VSIGetLastErrorNo()).isEqualTo(0); // VSIE_None
    assertThat(gdal.VSIGetLastErrorMsg()).isEmpty();
  }

  @Test
  public void errorContext_oneError() throws Exception {
    String errorMessage = "A fake error";
    int errorClass = gdalconst.CE_Warning; // Actual value: 2
    int errorNum = gdalconst.CPLE_IllegalArg; // Actual value: 5
    try (QuietHandler handler = new QuietHandler()) {
      gdal.Error(errorClass, errorNum, errorMessage);
    }

    assertThat(gdal.GetErrorCounter()).isEqualTo(1);
    assertThat(gdal.GetLastErrorType()).isEqualTo(errorClass);
    assertThat(gdal.GetLastErrorNo()).isEqualTo(errorNum);
    assertThat(gdal.GetLastErrorMsg()).isEqualTo(errorMessage);

    // Prove that the VSI error space is not contaminated.
    assertThat(gdal.VSIGetLastErrorNo()).isEqualTo(0); // VSIE_None
    assertThat(gdal.VSIGetLastErrorMsg()).isEmpty();
  }

  @Test
  public void errorContext_twoErrors() throws Exception {
    String errorMessage = "First fake error";
    int errorClass = gdalconst.CE_Warning; // Actual value: 2
    int errorNum = gdalconst.CPLE_IllegalArg; // Actual value: 5

    String errorMessage2 = "Second fake error";
    int errorClass2 = gdalconst.CE_Failure; // Actual value: 2
    int errorNum2 = gdalconst.CPLE_UserInterrupt; // Actual value: 9

    try (QuietHandler handler = new QuietHandler()) {
      gdal.Error(errorClass, errorNum, errorMessage);
      gdal.Error(errorClass2, errorNum2, errorMessage2);
    }

    // Expect to see the values from the most recent error.
    assertThat(gdal.GetErrorCounter()).isEqualTo(2);
    assertThat(gdal.GetLastErrorType()).isEqualTo(errorClass2);
    assertThat(gdal.GetLastErrorNo()).isEqualTo(errorNum2);
    assertThat(gdal.GetLastErrorMsg()).isEqualTo(errorMessage2);
  }
}
