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

package org.gdal.ogr;

import static com.google.common.truth.Truth.assertThat;

import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import org.gdal.GoogleGdalLoader;
import org.gdal.gdal.gdal;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;

public final class FieldDomainAndStyleTableTest {

  private static final String VSIMEM_DIR = "/vsimem/FieldDomainAndStyleTableTest";

  @Before
  public void setUp() {
    GoogleGdalLoader.init();
    ogr.RegisterAll();
    ogr.UseExceptions();
  }

  @After
  public void tearDown() {
    ogr.UseExceptions();
    gdal.ErrorReset();
    gdal.RmdirRecursive(VSIMEM_DIR);
  }

  @Test
  @SuppressWarnings("JdkObsolete") // HashMap signature dictated by SWIG FieldDomain API.
  public void fieldDomains_codedRangeDateTimeAndGlobExposeMetadata() {
    HashMap<String, String> codes = new HashMap<>();
    codes.put("1", "Residential");
    codes.put("2", "Commercial");
    FieldDomain coded =
        ogr.CreateCodedFieldDomain(
            "zoning", "Zoning codes", ogrConstants.OFTInteger, ogrConstants.OFSTNone, codes);
    assertThat(coded.GetName()).isEqualTo("zoning");
    assertThat(new String(coded.GetNameAsByteArray(), StandardCharsets.UTF_8)).isEqualTo("zoning");
    assertThat(coded.GetDescription()).isEqualTo("Zoning codes");
    assertThat(new String(coded.GetDescriptionAsByteArray(), StandardCharsets.UTF_8))
        .isEqualTo("Zoning codes");
    assertThat(coded.GetFieldType()).isEqualTo(ogrConstants.OFTInteger);
    assertThat(coded.GetFieldSubType()).isEqualTo(ogrConstants.OFSTNone);
    assertThat(coded.GetDomainType()).isEqualTo(ogrConstants.OFDT_CODED);
    coded.SetSplitPolicy(ogrConstants.OFDSP_DUPLICATE);
    assertThat(coded.GetSplitPolicy()).isEqualTo(ogrConstants.OFDSP_DUPLICATE);
    coded.SetMergePolicy(ogrConstants.OFDMP_SUM);
    assertThat(coded.GetMergePolicy()).isEqualTo(ogrConstants.OFDMP_SUM);
    assertThat(coded.GetEnumeration()).containsEntry("1", "Residential");
    assertThat(coded.equals(coded)).isTrue();
    assertThat(coded.equals(new Object())).isFalse();
    assertThat(coded.hashCode()).isNotEqualTo(0);

    FieldDomain rangeNum =
        ogr.CreateRangeFieldDomain(
            "elevation",
            "Elevation bounds",
            ogrConstants.OFTReal,
            ogrConstants.OFSTNone,
            -100.0,
            true,
            8850.0,
            false);
    assertThat(rangeNum.GetDomainType()).isEqualTo(ogrConstants.OFDT_RANGE);
    assertThat(rangeNum.GetMinAsDouble()).isWithin(1e-9).of(-100.0);
    assertThat(rangeNum.IsMinInclusive()).isTrue();
    assertThat(rangeNum.GetMaxAsDouble()).isWithin(1e-9).of(8850.0);
    assertThat(rangeNum.IsMaxInclusive()).isFalse();

    FieldDomain rangeDt =
        ogr.CreateRangeFieldDomainDateTime(
            "dates", "Date bounds", "2020-01-01T00:00:00Z", true, "2030-12-31T23:59:59Z", 1.0);
    assertThat(rangeDt.GetMinAsString()).isEqualTo("2020-01-01T00:00:00");
    assertThat(rangeDt.GetMaxAsString()).isEqualTo("2030-12-31T23:59:59");

    FieldDomain glob =
        ogr.CreateGlobFieldDomain(
            "iso_code", "Country prefix", ogrConstants.OFTString, ogrConstants.OFSTNone, "US-*");
    assertThat(glob.GetDomainType()).isEqualTo(ogrConstants.OFDT_GLOB);
    assertThat(glob.GetGlob()).isEqualTo("US-*");
    assertThat(new String(glob.GetGlobAsByteArray(), StandardCharsets.UTF_8)).isEqualTo("US-*");

    glob.delete();
    rangeDt.delete();
    rangeNum.delete();
    coded.delete();
  }

  @Test
  public void styleTable_addFindIterateSaveAndLoadInVsimem() {
    gdal.MkdirRecursive(VSIMEM_DIR, 0755);
    String ofsPath = VSIMEM_DIR + "/styles.ofs";

    StyleTable table = new StyleTable();
    assertThat(table.AddStyle("road", "PEN(c:#112233,w:2px)")).isEqualTo(1);
    assertThat(table.AddStyle("park", "BRUSH(fc:#00FF00)")).isEqualTo(1);
    assertThat(table.Find("road")).isEqualTo("PEN(c:#112233,w:2px)");

    table.ResetStyleStringReading();
    String first = table.GetNextStyle();
    assertThat(first).isNotNull();
    assertThat(table.GetLastStyleName()).isAnyOf("road", "park");
    assertThat(table.SaveStyleTable(ofsPath)).isEqualTo(1);

    StyleTable loaded = new StyleTable();
    assertThat(loaded.LoadStyleTable(ofsPath)).isEqualTo(1);
    assertThat(loaded.Find("park")).isEqualTo("BRUSH(fc:#00FF00)");
    assertThat(table.equals(table)).isTrue();
    assertThat(table.equals(new Object())).isFalse();
    assertThat(table.hashCode()).isNotEqualTo(0);

    loaded.delete();
    table.delete();
  }
}
