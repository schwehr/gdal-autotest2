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

package org.gdal.osr;

import static com.google.common.truth.Truth.assertThat;
import static org.junit.Assert.assertThrows;

import com.google.testing.junit.testparameterinjector.TestParameter;
import com.google.testing.junit.testparameterinjector.TestParameterInjector;
import java.util.Vector;
import org.gdal.GoogleGdalLoader;
import org.gdal.gdal.gdal;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(TestParameterInjector.class)
public final class SpatialReferenceProjectionsAndIoTest {

  private enum ProjectionCase {
    ACEA("Albers_Conic_Equal_Area", srs -> srs.SetACEA(20.0, 50.0, 35.0, -95.0, 0.0, 0.0)),
    AE("Azimuthal_Equidistant", srs -> srs.SetAE(0.0, 0.0, 0.0, 0.0)),
    BONNE("Bonne", srs -> srs.SetBonne(45.0, 10.0, 0.0, 0.0)),
    CEA("Cylindrical_Equal_Area", srs -> srs.SetCEA(30.0, 0.0, 0.0, 0.0)),
    CS("Cassini_Soldner", srs -> srs.SetCS(10.0, 20.0, 0.0, 0.0)),
    EC("Equidistant_Conic", srs -> srs.SetEC(20.0, 50.0, 35.0, -95.0, 0.0, 0.0)),
    ECKERT_IV("Eckert_IV", srs -> srs.SetEckertIV(0.0, 0.0, 0.0)),
    ECKERT_VI("Eckert_VI", srs -> srs.SetEckertVI(0.0, 0.0, 0.0)),
    EQUIRECTANGULAR("Equirectangular", srs -> srs.SetEquirectangular(0.0, 0.0, 0.0, 0.0)),
    EQUIRECTANGULAR2("Equirectangular", srs -> srs.SetEquirectangular2(0.0, 0.0, 30.0, 0.0, 0.0)),
    GAUSS_SCHREIBER(
        "Gauss_Schreiber_Transverse_Mercator",
        srs -> srs.SetGaussSchreiberTMercator(0.0, 0.0, 1.0, 0.0, 0.0)),
    GS("Gall_Stereographic", srs -> srs.SetGS(0.0, 0.0, 0.0)),
    GH("Goode_Homolosine", srs -> srs.SetGH(0.0, 0.0, 0.0)),
    IGH("Interrupted_Goode_Homolosine", SpatialReference::SetIGH),
    GEOS("Geostationary_Satellite", srs -> srs.SetGEOS(0.0, 35785831.0, 0.0, 0.0)),
    GNOMONIC("Gnomonic", srs -> srs.SetGnomonic(0.0, 0.0, 0.0, 0.0)),
    HOM("Hotine_Oblique_Mercator", srs -> srs.SetHOM(45.0, 10.0, 30.0, 30.0, 0.9996, 0.0, 0.0)),
    HOM2PNO(
        "Hotine_Oblique_Mercator_Two_Point_Natural_Origin",
        srs -> srs.SetHOM2PNO(45.0, 40.0, 5.0, 50.0, 15.0, 0.9996, 0.0, 0.0)),
    KROVAK("Krovak", srs -> srs.SetKrovak(49.5, 24.833333, 30.288139, 78.5, 0.9999, 0.0, 0.0)),
    LAEA("Lambert_Azimuthal_Equal_Area", srs -> srs.SetLAEA(45.0, -100.0, 0.0, 0.0)),
    LCC("Lambert_Conformal_Conic_2SP", srs -> srs.SetLCC(33.0, 45.0, 39.0, -96.0, 0.0, 0.0)),
    LCC1SP("Lambert_Conformal_Conic_1SP", srs -> srs.SetLCC1SP(45.0, 0.0, 1.0, 0.0, 0.0)),
    LCCB(
        "Lambert_Conformal_Conic_2SP_Belgium",
        srs -> srs.SetLCCB(49.8333, 51.1667, 90.0, 4.3569, 150000.0, 5400088.0)),
    MC("Miller_Cylindrical", srs -> srs.SetMC(0.0, 0.0, 0.0, 0.0)),
    MERCATOR("Mercator_1SP", srs -> srs.SetMercator(0.0, 0.0, 1.0, 0.0, 0.0)),
    MERCATOR2SP("Mercator_2SP", srs -> srs.SetMercator2SP(30.0, 0.0, 0.0, 0.0, 0.0)),
    MOLLWEIDE("Mollweide", srs -> srs.SetMollweide(0.0, 0.0, 0.0)),
    NZMG("New_Zealand_Map_Grid", srs -> srs.SetNZMG(-41.0, 173.0, 2510000.0, 6023150.0)),
    OS("Oblique_Stereographic", srs -> srs.SetOS(52.156, 5.387, 0.9999079, 155000.0, 463000.0)),
    ORTHOGRAPHIC("Orthographic", srs -> srs.SetOrthographic(0.0, 0.0, 0.0, 0.0)),
    POLYCONIC("Polyconic", srs -> srs.SetPolyconic(0.0, 0.0, 0.0, 0.0)),
    PS("Polar_Stereographic", srs -> srs.SetPS(-71.0, 0.0, 1.0, 0.0, 0.0)),
    ROBINSON("Robinson", srs -> srs.SetRobinson(0.0, 0.0, 0.0)),
    SINUSOIDAL("Sinusoidal", srs -> srs.SetSinusoidal(0.0, 0.0, 0.0)),
    STEREOGRAPHIC("Stereographic", srs -> srs.SetStereographic(0.0, 0.0, 1.0, 0.0, 0.0)),
    SOC(
        "Hotine_Oblique_Mercator_Azimuth_Center",
        srs -> srs.SetSOC(46.9524, 7.43958, 600000.0, 200000.0)),
    TM("Transverse_Mercator", srs -> srs.SetTM(0.0, 9.0, 0.9996, 500000.0, 0.0)),
    TM_VARIANT(
        "Transverse_Mercator_South_Orientated",
        srs ->
            srs.SetTMVariant(
                osrConstants.SRS_PT_TRANSVERSE_MERCATOR_SOUTH_ORIENTED, 0.0, 19.0, 1.0, 0.0, 0.0)),
    TMG("Tunisia_Mining_Grid", srs -> srs.SetTMG(36.0, 9.0, 500000.0, 300000.0)),
    TMSO("Transverse_Mercator_South_Orientated", srs -> srs.SetTMSO(0.0, 19.0, 1.0, 0.0, 0.0)),
    VDG("VanDerGrinten", srs -> srs.SetVDG(0.0, 0.0, 0.0)),
    VERTICAL_PERSPECTIVE(
        "Vertical Perspective",
        srs -> srs.SetVerticalPerspective(35.0, -100.0, 0.0, 1000000.0, 0.0, 0.0));

    interface Setter {
      int apply(SpatialReference srs);
    }

    final String expectedWktProjection;
    final Setter setter;

    ProjectionCase(String expectedWktProjection, Setter setter) {
      this.expectedWktProjection = expectedWktProjection;
      this.setter = setter;
    }
  }

  @Before
  public void setUp() {
    GoogleGdalLoader.init();
    osr.UseExceptions();
  }

  @After
  public void tearDown() {
    osr.UseExceptions();
    gdal.ErrorReset();
  }

  @Test
  public void setProjectionMethods_produceValidProjectedCrs(
      @TestParameter ProjectionCase projectionCase) {
    SpatialReference srs = new SpatialReference();
    srs.SetWellKnownGeogCS("WGS84");
    assertThat(projectionCase.setter.apply(srs)).isEqualTo(0);
    assertThat(srs.IsProjected()).isEqualTo(1);
    assertThat(srs.ExportToWkt()).contains(projectionCase.expectedWktProjection);
    srs.delete();
  }

  @Test
  @SuppressWarnings({"unchecked", "JdkObsolete"})
  public void importAndExportFormats_roundTripSuccessfully() {
    SpatialReference srs = new SpatialReference();
    assertThat(srs.ImportFromEPSG(32610)).isEqualTo(0);
    assertThat(srs.ImportFromEPSGA(4326)).isEqualTo(0);

    assertThat(srs.ExportToWkt()).contains("WGS 84");
    String[] wktOut = new String[1];
    Vector<String> wktOpts = new Vector<>();
    wktOpts.add("FORMAT=WKT2_2019");
    assertThat(srs.ExportToWkt(wktOut, wktOpts)).isEqualTo(0);
    assertThat(wktOut[0]).contains("GEOGCRS");

    assertThat(srs.ExportToPrettyWkt()).contains("GEOGCS");
    assertThat(srs.ExportToPrettyWkt(1)).doesNotContain("AUTHORITY");
    assertThat(srs.ExportToProj4()).contains("+proj=longlat");

    String[] projJson0 = new String[1];
    assertThat(srs.ExportToPROJJSON(projJson0)).isEqualTo(0);
    assertThat(projJson0[0]).contains("\"GeographicCRS\"");
    String[] projJson1 = new String[1];
    Vector<String> jsonOpts = new Vector<>();
    jsonOpts.add("MULTILINE=NO");
    assertThat(srs.ExportToPROJJSON(projJson1, jsonOpts)).isEqualTo(0);
    assertThat(projJson1[0]).contains("WGS 84");

    // ImportFromWkt & ImportFromProj4.
    assertThat(srs.ImportFromProj4("+proj=utm +zone=10 +datum=WGS84 +units=m +no_defs"))
        .isEqualTo(0);
    assertThat(srs.GetUTMZone()).isEqualTo(10);
    assertThat(srs.ImportFromWkt(osrConstants.SRS_WKT_WGS84_LAT_LONG)).isEqualTo(0);

    // PCI import/export.
    String[] pciProj = new String[1];
    String[] pciUnits = new String[1];
    double[] pciParams = new double[17];
    SpatialReference utm = new SpatialReference();
    utm.ImportFromEPSG(32610);
    assertThat(utm.ExportToPCI(pciProj, pciUnits, pciParams)).isEqualTo(0);
    assertThat(pciProj[0]).contains("UTM");
    assertThat(srs.ImportFromPCI(pciProj[0], pciUnits[0], pciParams)).isEqualTo(0);
    assertThat(srs.ImportFromPCI("LONG/LAT    D000", "DEGREE")).isEqualTo(0);
    assertThat(srs.ImportFromPCI("LONG/LAT    D000")).isEqualTo(0);

    // USGS import/export.
    long[] usgsCode = new long[1];
    long[] usgsZone = new long[1];
    double[] usgsParams = new double[15];
    long[] usgsDatum = new long[1];
    assertThat(utm.ExportToUSGS(usgsCode, usgsZone, usgsParams, usgsDatum)).isEqualTo(0);
    assertThat(usgsCode[0]).isEqualTo(1L);
    assertThat(srs.ImportFromUSGS(usgsCode[0], usgsZone[0], usgsParams, usgsDatum[0])).isEqualTo(0);
    assertThat(srs.ImportFromUSGS(usgsCode[0], usgsZone[0], usgsParams)).isEqualTo(0);
    // The 1-arg and 2-arg ImportFromUSGS overloads pass null parameter arrays and report error.
    osr.DontUseExceptions();
    assertThat(srs.ImportFromUSGS(0L, 0L)).isNotEqualTo(0);
    assertThat(srs.ImportFromUSGS(0L)).isNotEqualTo(0);
    osr.UseExceptions();

    // ERM import/export.
    String[] ermProj = new String[1];
    String[] ermDatum = new String[1];
    String[] ermUnits = new String[1];
    assertThat(utm.ExportToERM(ermProj, ermDatum, ermUnits)).isEqualTo(0);
    assertThat(srs.ImportFromERM(ermProj[0], ermDatum[0], ermUnits[0])).isEqualTo(0);

    // GML/XML import/export.
    String xml = utm.ExportToXML();
    assertThat(xml).contains("gml:");
    assertThat(utm.ExportToXML("GML")).contains("gml:");
    assertThat(srs.ImportFromXML(xml)).isEqualTo(0);

    // MapInfo CoordSys import/export.
    String mi = utm.ExportToMICoordSys();
    assertThat(mi).contains("Earth Projection");
    assertThat(srs.ImportFromMICoordSys(mi)).isEqualTo(0);

    // ESRI PRJ import.
    Vector<String> esriLines = new Vector<>();
    esriLines.add(
        "GEOGCS[\"GCS_WGS_1984\",DATUM[\"D_WGS_1984\",SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
            + "PRIMEM[\"Greenwich\",0.0],UNIT[\"Degree\",0.0174532925199433]]");
    assertThat(srs.ImportFromESRI(esriLines)).isEqualTo(0);

    // OziExplorer import.
    Vector<String> oziLines = new Vector<>();
    oziLines.add("OziExplorer Map Data File Version 2.2");
    oziLines.add("TestMap");
    oziLines.add("test.png");
    oziLines.add("1 ,Map Code,");
    oziLines.add("WGS 84,WGS 84,   0.0000,   0.0000,WGS 84");
    oziLines.add("Map Projection,Latitude/Longitude,PolyCal,No,AutoCalOnly,No,BSBUseWPX,No");
    oziLines.add("Projection Setup,");
    assertThat(srs.ImportFromOzi(oziLines)).isEqualTo(0);

    // CF-1 import/export.
    // Note: ExportToCF1() crashes in tcmalloc when OSRExportToCF1 returns non-empty results due to
    // upstream SWIG GetCSLStringAsHashTable incrementing stringarray before CSLDestroy.
    // On an unsupported projection (Bonne), OSRExportToCF1 leaves ppapszKeyValues == NULL cleanly.
    Vector<String> cfOpts = new Vector<>();
    cfOpts.add("DUMMY=1");
    assertThat(utm.ExportToCF1Units()).isEqualTo("m");
    assertThat(utm.ExportToCF1Units(cfOpts)).isEqualTo("m");

    SpatialReference bonne = new SpatialReference();
    bonne.SetWellKnownGeogCS("WGS84");
    bonne.SetBonne(45.0, 0.0, 0.0, 0.0);
    osr.DontUseExceptions();
    assertThat(bonne.ExportToCF1()).isEmpty();
    assertThat(bonne.ExportToCF1(cfOpts)).isEmpty();
    osr.UseExceptions();
    bonne.delete();

    SpatialReference mercAlt =
        utm.ConvertToOtherProjection(osrConstants.SRS_PT_MERCATOR_1SP, cfOpts);
    if (mercAlt != null) {
      mercAlt.delete();
    }
    srs.SetFromUserInput("EPSG:4326", cfOpts);
    assertThat(srs.IsSame(srs, cfOpts)).isEqualTo(1);

    // Also exercise array/null validation paths on ExportToPCI / ExportToUSGS / ImportFrom*.
    assertThrows(
        IndexOutOfBoundsException.class, () -> utm.ExportToPCI(pciProj, pciUnits, new double[1]));
    assertThrows(
        IndexOutOfBoundsException.class,
        () -> utm.ExportToUSGS(usgsCode, usgsZone, new double[1], usgsDatum));
    osr.DontUseExceptions();
    assertThat(srs.ImportFromPCI("BAD")).isNotEqualTo(0);
    assertThat(srs.ImportFromUSGS(999L)).isNotEqualTo(0);
    assertThat(srs.ImportFromUSGS(999L, 0L)).isNotEqualTo(0);
    assertThat(srs.ImportFromESRI(new Vector<String>())).isNotEqualTo(0);
    assertThat(srs.ImportFromOzi(cfOpts)).isNotEqualTo(0);
    assertThrows(NullPointerException.class, () -> srs.ImportFromOzi(new Vector<String>()));
    assertThat(srs.ImportFromERM("UNKNOWN", "UNKNOWN", "UNKNOWN")).isNotEqualTo(0);
    assertThat(srs.ImportFromCF1(new Vector<String>())).isNotEqualTo(0);
    assertThat(srs.ImportFromCF1(new Vector<String>(), "")).isNotEqualTo(0);
    osr.UseExceptions();

    Vector<String> cfKv = new Vector<>();
    cfKv.add("grid_mapping_name=latitude_longitude");
    cfKv.add("semi_major_axis=6378137");
    cfKv.add("inverse_flattening=298.257223563");
    assertThat(srs.ImportFromCF1(cfKv, "degrees_east")).isEqualTo(0);
    assertThat(srs.ImportFromCF1(cfKv)).isEqualTo(0);

    // ImportFromUrl has HAVE_CURL disabled in Google3.
    osr.DontUseExceptions();
    assertThat(srs.ImportFromUrl("http://example.invalid/crs.wkt")).isNotEqualTo(0);
    osr.UseExceptions();
    RuntimeException thrownUrl =
        assertThrows(
            RuntimeException.class, () -> srs.ImportFromUrl("http://example.invalid/crs.wkt"));
    assertThat(thrownUrl).isNotNull();

    utm.delete();
    srs.delete();
  }
}
