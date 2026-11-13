// Copyright 2026 Google LLC
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

package org.gdal.gdalconst;

import static com.google.common.truth.Truth.assertThat;

import org.gdal.GoogleGdalLoader;
import org.junit.Before;
import org.junit.Test;

public final class GdalConstTest {

  @Before
  public void initGdal() {
    GoogleGdalLoader.init();
  }

  @Test
  public void dataTypeConstants_haveExpectedOrderingAndValues() {
    assertThat(gdalconst.GDT_Unknown).isEqualTo(0);
    assertThat(gdalconst.GDT_Byte).isEqualTo(1);
    assertThat(gdalconst.GDT_UInt16).isEqualTo(2);
    assertThat(gdalconst.GDT_Int16).isEqualTo(3);
    assertThat(gdalconst.GDT_UInt32).isEqualTo(4);
    assertThat(gdalconst.GDT_Int32).isEqualTo(5);
    assertThat(gdalconst.GDT_Float32).isEqualTo(6);
    assertThat(gdalconst.GDT_Float64).isEqualTo(7);
    assertThat(gdalconst.GDT_CInt16).isEqualTo(8);
    assertThat(gdalconst.GDT_CInt32).isEqualTo(9);
    assertThat(gdalconst.GDT_CFloat32).isEqualTo(10);
    assertThat(gdalconst.GDT_CFloat64).isEqualTo(11);
    assertThat(gdalconst.GDT_UInt64).isEqualTo(12);
    assertThat(gdalconst.GDT_Int64).isEqualTo(13);
    assertThat(gdalconst.GDT_Int8).isEqualTo(14);
    assertThat(gdalconst.GDT_Float16).isEqualTo(15);
    assertThat(gdalconst.GDT_CFloat16).isEqualTo(16);
    assertThat(gdalconst.GDT_TypeCount).isGreaterThan(14);
  }

  @Test
  public void accessIoResamplingAndErrorConstants_areConsistent() {
    assertThat(gdalconstConstants.GA_ReadOnly).isEqualTo(0);
    assertThat(gdalconstConstants.GA_Update).isEqualTo(1);
    assertThat(gdalconstConstants.GF_Read).isEqualTo(0);
    assertThat(gdalconstConstants.GF_Write).isEqualTo(1);

    assertThat(gdalconst.GRIORA_NearestNeighbour).isEqualTo(0);
    assertThat(gdalconst.GRIORA_Bilinear).isEqualTo(1);
    assertThat(gdalconst.GRA_NearestNeighbour).isEqualTo(0);
    assertThat(gdalconst.GRA_Sum).isGreaterThan(gdalconst.GRA_Q3);

    assertThat(gdalconst.CE_None).isEqualTo(0);
    assertThat(gdalconst.CE_Debug).isEqualTo(1);
    assertThat(gdalconst.CE_Warning).isEqualTo(2);
    assertThat(gdalconst.CE_Failure).isEqualTo(3);
    assertThat(gdalconst.CE_Fatal).isEqualTo(4);

    assertThat(gdalconst.CPLE_None).isEqualTo(0);
    assertThat(gdalconst.CPLE_AppDefined).isEqualTo(1);
    assertThat(gdalconst.CPLE_IllegalArg).isEqualTo(5);
    assertThat(gdalconst.CPLE_NotSupported).isEqualTo(6);
  }

  @Test
  public void openFlagsColorXmlAndAlgorithmConstants_arePopulated() {
    assertThat(gdalconst.OF_READONLY).isEqualTo(0);
    assertThat(gdalconst.OF_UPDATE).isEqualTo(1);
    assertThat(gdalconst.OF_RASTER).isGreaterThan(0);
    assertThat(gdalconst.OF_VECTOR).isGreaterThan(0);
    assertThat(gdalconst.OF_MULTIDIM_RASTER).isGreaterThan(0);

    assertThat(gdalconst.GCI_Undefined).isEqualTo(0);
    assertThat(gdalconst.GCI_GrayIndex).isEqualTo(1);
    assertThat(gdalconst.GCI_RedBand).isEqualTo(3);
    assertThat(gdalconst.GPI_Gray).isEqualTo(0);
    assertThat(gdalconst.GPI_RGB).isEqualTo(1);
    assertThat(gdalconst.CXT_Element).isEqualTo(0);
    assertThat(gdalconst.CXT_Text).isEqualTo(1);

    assertThat(gdalconst.DMD_LONGNAME).isEqualTo(gdalconst.GDAL_DMD_LONGNAME);
    assertThat(gdalconst.DCAP_CREATE).isEqualTo(gdalconst.GDAL_DCAP_CREATE);
    assertThat(gdalconst.DCAP_RASTER).isEqualTo(gdalconst.GDAL_DCAP_RASTER);
    assertThat(gdalconst.DIM_TYPE_HORIZONTAL_X).isEqualTo(gdalconst.GDAL_DIM_TYPE_HORIZONTAL_X);
    assertThat(gdalconst.GDsCAddRelationship).isNotEmpty();

    assertThat(gdalconst.CPLES_BackslashQuotable).isEqualTo(0);
    assertThat(gdalconst.GFT_Integer).isEqualTo(0);
    assertThat(gdalconst.GFU_Generic).isEqualTo(0);
    assertThat(gdalconst.GRTT_THEMATIC).isEqualTo(0);
    assertThat(gdalconst.GMF_ALL_VALID).isGreaterThan(0);
    assertThat(gdalconst.GARIO_COMPLETE).isGreaterThan(0);
    assertThat(gdalconst.GTO_TIP).isEqualTo(0);
    assertThat(gdalconst.GRC_ONE_TO_MANY).isEqualTo(1);
    assertThat(gdalconst.GRT_ASSOCIATION).isEqualTo(1);
    assertThat(gdalconst.GAAT_BOOLEAN).isEqualTo(0);
    assertThat(gdalconst.GADV_NAME).isEqualTo(1);
    assertThat(gdalconst.GADV_OBJECT).isEqualTo(2);
    // In Forge statically-linked JNI builds, System.loadLibrary fails gracefully while JNI symbols
    // remain directly linked; ensure isAvailable() completes cleanly and returns boolean.
    assertThat(gdalconstJNI.isAvailable()).isAnyOf(true, false);
  }
}
