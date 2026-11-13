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
import java.util.List;
import java.util.Vector;
import org.gdal.GoogleGdalLoader;
import org.gdal.gdal.gdal;
import org.gdal.osr.SpatialReference;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;

public final class OgrModuleTest {

  private static final String VSIMEM_DIR = "/vsimem/OgrModuleTest";

  private static final class RecordingProgressCallback extends ProgressCallback {
    int calls;
    double lastComplete = -1.0;

    @Override
    public int run(double dfComplete, String pszMessage) {
      calls++;
      lastComplete = dfComplete;
      return 1;
    }
  }

  @Before
  public void setUp() {
    GoogleGdalLoader.init();
    ogr.RegisterAll();
    ogr.UseExceptions();
  }

  @After
  public void tearDown() {
    ogr.UseExceptions();
    ogr.SetNonLinearGeometriesEnabledFlag(1);
    ogr.SetGenerate_DB2_V72_BYTE_ORDER(0);
    gdal.ErrorReset();
    gdal.RmdirRecursive(VSIMEM_DIR);
  }

  @Test
  public void exceptionModeAndRegistry_exposeDriversAndOpenOverloads() {
    assertThat(ogrJNI.isAvailable()).isAnyOf(true, false);
    assertThat(ogr.GetUseExceptions()).isTrue();
    ogr.DontUseExceptions();
    assertThat(ogr.GetUseExceptions()).isFalse();
    ogr.UseExceptions();
    assertThat(ogr.GetUseExceptions()).isTrue();

    assertThat(ogr.GetDriverCount()).isGreaterThan(0);
    Driver memDriver = ogr.GetDriverByName("Memory");
    assertThat(memDriver).isNotNull();
    assertThat(ogr.GetDriver(0)).isNotNull();

    String jsonPath = VSIMEM_DIR + "/sample.geojson";
    String geoJson =
        "{\"type\":\"FeatureCollection\",\"features\":[{\"type\":\"Feature\","
            + "\"geometry\":{\"type\":\"Point\",\"coordinates\":[1,2]},\"properties\":{}}]}";
    gdal.FileFromMemBuffer(jsonPath, geoJson.getBytes(StandardCharsets.UTF_8));

    DataSource ds1 = ogr.Open(jsonPath);
    assertThat(ds1).isNotNull();
    assertThat(ds1.GetLayerCount()).isEqualTo(1);
    DataSource ds2 = ogr.Open(jsonPath, 0);
    assertThat(ds2).isNotNull();
    DataSource ds3 = ogr.Open(jsonPath, false);
    assertThat(ds3).isNotNull();
    DataSource dsShared1 = ogr.OpenShared(jsonPath);
    assertThat(dsShared1).isNotNull();
    DataSource dsShared2 = ogr.OpenShared(jsonPath, 0);
    assertThat(dsShared2).isNotNull();

    assertThat(ogr.GetOpenDSCount()).isAtLeast(0);
    assertThat(ogr.GetOpenDS(0)).isNull();

    dsShared2.delete();
    dsShared1.delete();
    ds3.delete();
    ds2.delete();
    ds1.delete();

    ogr.DontUseExceptions();
    assertThat(ogr.Open(VSIMEM_DIR + "/nonexistent.geojson")).isNull();
    ogr.UseExceptions();
    assertThat(ogr.Open(VSIMEM_DIR + "/nonexistent.geojson")).isNull();
  }

  @Test
  @SuppressWarnings({"unchecked", "JdkObsolete"})
  public void versionsCmdLineTypesAndFlags_workAsExpected() {
    assertThat(ogr.GetGEOSVersionMajor()).isAtLeast(3);
    assertThat(ogr.GetGEOSVersionMinor()).isAtLeast(0);
    assertThat(ogr.GetGEOSVersionMicro()).isAtLeast(0);

    Vector<String> argvVec = new Vector<>();
    argvVec.add("--config");
    argvVec.add("OGR_TEST_OPTION");
    argvVec.add("YES");
    argvVec.add("input.shp");
    List<String> processedVec1 = ogr.GeneralCmdLineProcessor(argvVec);
    assertThat(processedVec1).contains("input.shp");
    List<String> processedVec2 = ogr.GeneralCmdLineProcessor(argvVec, 0);
    assertThat(processedVec2).contains("input.shp");

    String[] processedArr1 = ogr.GeneralCmdLineProcessor(new String[] {"a.shp", "b.shp"});
    assertThat(processedArr1).asList().containsExactly("a.shp", "b.shp").inOrder();
    String[] processedArr2 = ogr.GeneralCmdLineProcessor(new String[] {"c.shp"}, 0);
    assertThat(processedArr2).asList().containsExactly("c.shp");

    assertThat(ogr.GetFieldTypeByName("Integer")).isEqualTo(ogrConstants.OFTInteger);
    assertThat(ogr.GetFieldTypeName(ogrConstants.OFTString)).isEqualTo("String");
    assertThat(ogr.GetFieldSubtypeByName("Boolean")).isEqualTo(ogrConstants.OFSTBoolean);
    assertThat(ogr.GetFieldSubTypeName(ogrConstants.OFSTInt16)).isEqualTo("Int16");
    assertThat(ogr.GeometryTypeToName(ogrConstants.wkbPoint)).isEqualTo("Point");

    assertThat(ogr.GT_SetZ(ogrConstants.wkbPoint)).isEqualTo(ogrConstants.wkbPoint25D);
    assertThat(ogr.GT_SetM(ogrConstants.wkbPoint)).isEqualTo(ogrConstants.wkbPointM);
    assertThat(ogr.GT_SetModifier(ogrConstants.wkbPoint, 1, 1)).isEqualTo(ogrConstants.wkbPointZM);
    assertThat(ogr.GT_SetModifier(ogrConstants.wkbPoint, 1)).isEqualTo(ogrConstants.wkbPoint25D);
    assertThat(ogr.GT_Flatten(ogrConstants.wkbPointZM)).isEqualTo(ogrConstants.wkbPoint);
    assertThat(ogr.GT_HasZ(ogrConstants.wkbPointZM)).isEqualTo(1);
    assertThat(ogr.GT_HasM(ogrConstants.wkbPointZM)).isEqualTo(1);
    assertThat(ogr.GT_IsSubClassOf(ogrConstants.wkbPoint, ogrConstants.wkbUnknown)).isEqualTo(1);
    assertThat(ogr.GT_IsCurve(ogrConstants.wkbCircularString)).isEqualTo(1);
    assertThat(ogr.GT_IsSurface(ogrConstants.wkbPolygon)).isEqualTo(1);
    assertThat(ogr.GT_IsNonLinear(ogrConstants.wkbCircularString)).isEqualTo(1);
    assertThat(ogr.GT_GetCollection(ogrConstants.wkbPoint)).isEqualTo(ogrConstants.wkbMultiPoint);
    assertThat(ogr.GT_GetSingle(ogrConstants.wkbMultiPolygon)).isEqualTo(ogrConstants.wkbPolygon);
    assertThat(ogr.GT_GetCurve(ogrConstants.wkbPolygon)).isEqualTo(ogrConstants.wkbCurvePolygon);
    assertThat(ogr.GT_GetLinear(ogrConstants.wkbCurvePolygon)).isEqualTo(ogrConstants.wkbPolygon);

    ogr.SetNonLinearGeometriesEnabledFlag(0);
    assertThat(ogr.GetNonLinearGeometriesEnabledFlag()).isEqualTo(0);
    ogr.SetNonLinearGeometriesEnabledFlag(1);
    assertThat(ogr.GetNonLinearGeometriesEnabledFlag()).isEqualTo(1);
    assertThat(ogr.SetGenerate_DB2_V72_BYTE_ORDER(1)).isEqualTo(0);
    assertThat(ogr.SetGenerate_DB2_V72_BYTE_ORDER(0)).isEqualTo(0);
  }

  @Test
  @SuppressWarnings({"unchecked", "JdkObsolete"})
  public void geometryFactoriesAndForceTo_createExpectedGeometries() {
    SpatialReference wgs84 = new SpatialReference();
    wgs84.ImportFromEPSG(4326);

    Geometry pt1 = ogr.CreateGeometryFromWkt("POINT (1 2)");
    assertThat(pt1.GetGeometryType()).isEqualTo(ogrConstants.wkbPoint);
    Geometry pt2 = ogr.CreateGeometryFromWkt("POINT (3 4)", wgs84);
    assertThat(pt2.GetSpatialReference()).isNotNull();

    byte[] wkb = pt1.ExportToWkb();
    Geometry fromWkb1 = ogr.CreateGeometryFromWkb(wkb);
    assertThat(fromWkb1.GetX()).isWithin(1e-9).of(1.0);
    Geometry fromWkb2 = ogr.CreateGeometryFromWkb(wkb, wgs84);
    assertThat(fromWkb2.GetSpatialReference()).isNotNull();

    Geometry fromGml =
        ogr.CreateGeometryFromGML("<gml:Point><gml:coordinates>5,6</gml:coordinates></gml:Point>");
    assertThat(fromGml.GetX()).isWithin(1e-9).of(5.0);

    Geometry fromJson = ogr.CreateGeometryFromJson("{\"type\":\"Point\",\"coordinates\":[7,8]}");
    assertThat(fromJson.GetY()).isWithin(1e-9).of(8.0);

    Geometry fromEsri = ogr.CreateGeometryFromEsriJson("{\"x\":9,\"y\":10}");
    assertThat(fromEsri.GetX()).isWithin(1e-9).of(9.0);

    Geometry env1 = ogr.CreateGeometryFromEnvelope(0.0, 0.0, 10.0, 20.0);
    assertThat(env1.GetArea()).isWithin(1e-6).of(200.0);
    Geometry env2 = ogr.CreateGeometryFromEnvelope(0.0, 0.0, 1.0, 2.0, wgs84);
    assertThat(env2.GetArea()).isWithin(1e-6).of(2.0);

    Geometry lines = ogr.CreateGeometryFromWkt("MULTILINESTRING ((0 0, 0 1, 1 1, 1 0, 0 0))");
    Geometry poly1 = ogr.BuildPolygonFromEdges(lines);
    Geometry poly2 = ogr.BuildPolygonFromEdges(lines, 1);
    Geometry poly3 = ogr.BuildPolygonFromEdges(lines, 1, 1);
    Geometry poly4 = ogr.BuildPolygonFromEdges(lines, 1, 1, 0.01);
    assertThat(poly1.GetArea()).isWithin(1e-6).of(1.0);
    assertThat(poly2.GetArea()).isWithin(1e-6).of(1.0);
    assertThat(poly3.GetArea()).isWithin(1e-6).of(1.0);
    assertThat(poly4.GetArea()).isWithin(1e-6).of(1.0);

    Geometry arc = ogr.ApproximateArcAngles(0.0, 0.0, 0.0, 10.0, 10.0, 0.0, 0.0, 90.0, 15.0);
    assertThat(arc.GetPointCount()).isGreaterThan(2);

    Geometry closedRing = ogr.CreateGeometryFromWkt("LINESTRING (0 0, 0 2, 2 2, 2 0, 0 0)");
    Geometry forcedPoly = ogr.ForceToPolygon(closedRing.Clone());
    assertThat(forcedPoly.GetGeometryType()).isEqualTo(ogrConstants.wkbPolygon);
    closedRing.delete();
    Geometry forcedLine = ogr.ForceToLineString(forcedPoly.Clone());
    assertThat(forcedLine.GetGeometryType()).isEqualTo(ogrConstants.wkbLineString);
    Geometry forcedMultiPoly = ogr.ForceToMultiPolygon(forcedPoly.Clone());
    assertThat(forcedMultiPoly.GetGeometryType()).isEqualTo(ogrConstants.wkbMultiPolygon);
    Geometry forcedMultiPt = ogr.ForceToMultiPoint(pt1.Clone());
    assertThat(forcedMultiPt.GetGeometryType()).isEqualTo(ogrConstants.wkbMultiPoint);
    Geometry forcedMultiLine = ogr.ForceToMultiLineString(forcedLine.Clone());
    assertThat(forcedMultiLine.GetGeometryType()).isEqualTo(ogrConstants.wkbMultiLineString);

    Vector<String> forceOpts = new Vector<>();
    forceOpts.add("DUMMY=1");
    Geometry forced2 = ogr.ForceTo(pt1.Clone(), ogrConstants.wkbPoint25D);
    assertThat(forced2.GetGeometryType()).isEqualTo(ogrConstants.wkbPoint25D);
    Geometry forced3 = ogr.ForceTo(pt1.Clone(), ogrConstants.wkbMultiPoint, forceOpts);
    assertThat(forced3.GetGeometryType()).isEqualTo(ogrConstants.wkbMultiPoint);

    RecordingProgressCallback progress = new RecordingProgressCallback();
    assertThat(progress.run(0.5, "halfway")).isEqualTo(1);
    assertThat(progress.calls).isEqualTo(1);
    assertThat(progress.lastComplete).isEqualTo(0.5);
    ProgressCallback baseProgress = new ProgressCallback();
    assertThat(baseProgress.run(1.0, "done")).isEqualTo(1);
    TermProgressCallback termProgress = new TermProgressCallback();
    assertThat(termProgress.run(1.0, "done")).isEqualTo(1);

    termProgress.delete();
    baseProgress.delete();
    progress.delete();
    forced3.delete();
    forced2.delete();
    forcedMultiLine.delete();
    forcedMultiPt.delete();
    forcedMultiPoly.delete();
    forcedLine.delete();
    forcedPoly.delete();
    arc.delete();
    poly4.delete();
    poly3.delete();
    poly2.delete();
    poly1.delete();
    lines.delete();
    env2.delete();
    env1.delete();
    fromEsri.delete();
    fromJson.delete();
    fromGml.delete();
    fromWkb2.delete();
    fromWkb1.delete();
    pt2.delete();
    pt1.delete();
    wgs84.delete();
  }
}
