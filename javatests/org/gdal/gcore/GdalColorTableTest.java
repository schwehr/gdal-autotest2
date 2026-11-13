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

package org.gdal.gcore;

import static com.google.common.truth.Truth.assertThat;

import java.awt.Color;
import java.awt.image.IndexColorModel;
import org.gdal.gdal.ColorTable;
import org.gdal.gdalconst.gdalconst;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;

@RunWith(JUnit4.class)
public final class GdalColorTableTest {

  @Test
  public void defaultTest() {
    ColorTable colorTable = new ColorTable();
    assertThat(colorTable.GetCount()).isEqualTo(0);
    assertThat(colorTable.GetPaletteInterpretation()).isEqualTo(gdalconst.GPI_RGB);
  }

  @Test
  public void hlsTest() {
    ColorTable colorTable = new ColorTable(gdalconst.GPI_HLS);
    assertThat(colorTable.GetPaletteInterpretation()).isEqualTo(gdalconst.GPI_HLS);
  }

  @Test
  public void cloneTest() {
    ColorTable colorTable = new ColorTable(gdalconst.GPI_Gray);
    colorTable.SetColorEntry(1, Color.white);

    ColorTable clone = colorTable.Clone();
    assertThat(clone.GetPaletteInterpretation()).isEqualTo(gdalconst.GPI_Gray);
    assertThat(clone.GetCount()).isEqualTo(2);
    assertThat(clone.GetColorEntry(1)).isEqualTo(Color.white);
  }

  @Test
  public void rampTest() {
    ColorTable colorTable = new ColorTable(gdalconst.GPI_CMYK);
    colorTable.CreateColorRamp(10, Color.white, 20, Color.blue);
    assertThat(colorTable.GetCount()).isEqualTo(21);
    Color expected = new Color(127, 127, 255, 255);
    assertThat(colorTable.GetColorEntry(15)).isEqualTo(expected);
  }

  @Test
  public void indexColorModelTest() {
    ColorTable colorTable = new ColorTable(gdalconst.GPI_RGB);
    colorTable.SetColorEntry(0, Color.red);
    colorTable.SetColorEntry(1, Color.green);
    colorTable.SetColorEntry(2, Color.blue);

    int bits = 3;
    IndexColorModel model = colorTable.getIndexColorModel(bits);
    assertThat(model.getMapSize()).isEqualTo(3);

    // TODO(schwehr): Do some useful checks on the model.
  }
}
