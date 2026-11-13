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
import static org.junit.Assert.assertThrows;

import java.util.Vector;
import org.gdal.GoogleGdalLoader;
import org.gdal.gdal.gdal;
import org.gdal.osr.CoordinateTransformation;
import org.gdal.osr.SpatialReference;
import org.gdal.osr.osrConstants;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;

public final class GeometryTest {

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
  }

  @Test
  @SuppressWarnings({"unchecked", "JdkObsolete"})
  public void constructorsFactoriesAndExports_roundTripAllFormats() {
    Geometry fromType = new Geometry(ogrConstants.wkbPoint);
    assertThat(fromType.GetGeometryType()).isEqualTo(ogrConstants.wkbPoint);
    assertThat(fromType.GetGeometryName()).isEqualTo("POINT");

    Geometry fromWkt = new Geometry(ogrConstants.wkbUnknown, "POINT (10 20)", null, null);
    assertThat(fromWkt.ExportToWkt()).isEqualTo("POINT (10 20)");

    byte[] wkb = fromWkt.ExportToWkb();
    Geometry fromWkbCtor = new Geometry(ogrConstants.wkbUnknown, null, wkb, null);
    assertThat(fromWkbCtor.GetX()).isWithin(1e-9).of(10.0);

    String gml = fromWkt.ExportToGML();
    Geometry fromGmlCtor = new Geometry(ogrConstants.wkbUnknown, null, null, gml);
    assertThat(fromGmlCtor.GetY()).isWithin(1e-9).of(20.0);

    RuntimeException invalidWktThrown =
        assertThrows(
            RuntimeException.class,
            () -> new Geometry(ogrConstants.wkbUnknown, "NOT VALID WKT", null, null));
    assertThat(invalidWktThrown).isNotNull();

    Geometry sWkt = Geometry.CreateFromWkt("LINESTRING (0 0, 3 4)");
    Geometry sWkb = Geometry.CreateFromWkb(sWkt.ExportToWkb(ogrConstants.wkbNDR));
    Geometry sGml = Geometry.CreateFromGML(gml);
    Geometry sJson = Geometry.CreateFromJson(fromWkt.ExportToJson());

    assertThat(sWkb.Length()).isWithin(1e-9).of(5.0);
    assertThat(sGml.Equal(fromWkt)).isTrue();
    assertThat(sJson.Equals(fromWkt)).isTrue();

    String[] wktOut = new String[1];
    assertThat(fromWkt.ExportToWkt(wktOut)).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(wktOut[0]).isEqualTo("POINT (10 20)");
    String[] isoWktOut = new String[1];
    assertThat(fromWkt.ExportToIsoWkt(isoWktOut)).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(isoWktOut[0]).isEqualTo("POINT (10 20)");

    byte[] isoWkb0 = fromWkt.ExportToIsoWkb();
    byte[] isoWkb1 = fromWkt.ExportToIsoWkb(ogrConstants.wkbXDR);
    assertThat(isoWkb0.length).isEqualTo(fromWkt.WkbSize());
    assertThat(isoWkb1.length).isEqualTo(fromWkt.WkbSize());

    byte[] exactBuf = new byte[(int) fromWkt.WkbSize()];
    assertThat(fromWkt.ExportToWkb(exactBuf, ogrConstants.wkbNDR))
        .isEqualTo(ogrConstants.OGRERR_NONE);
    NullPointerException wkbNullThrown =
        assertThrows(
            NullPointerException.class, () -> fromWkt.ExportToWkb(null, ogrConstants.wkbNDR));
    assertThat(wkbNullThrown).isNotNull();
    byte[] tooSmall = new byte[1];
    RuntimeException smallThrown =
        assertThrows(
            RuntimeException.class, () -> fromWkt.ExportToWkb(tooSmall, ogrConstants.wkbNDR));
    assertThat(smallThrown).hasMessageThat().contains("Array too small");

    Vector<String> gmlOpts = new Vector<>();
    gmlOpts.add("FORMAT=GML3");
    assertThat(fromWkt.ExportToGML(gmlOpts)).contains("gml:Point");
    assertThat(fromWkt.ExportToKML()).contains("<Point>");
    Geometry pt3dKml = Geometry.CreateFromWkt("POINT Z (10 20 5)");
    assertThat(pt3dKml.ExportToKML("clampToGround")).contains("clampToGround");
    pt3dKml.delete();
    Vector<String> jsonOpts = new Vector<>();
    jsonOpts.add("COORDINATE_PRECISION=3");
    assertThat(fromWkt.ExportToJson(jsonOpts)).contains("Point");

    sJson.delete();
    sGml.delete();
    sWkb.delete();
    sWkt.delete();
    fromGmlCtor.delete();
    fromWkbCtor.delete();
    fromWkt.delete();
    fromType.delete();
  }

  @Test
  public void verticesSubGeometriesDimensionsAndCloning_workCorrectly() {
    Geometry line = new Geometry(ogrConstants.wkbLineStringZM);
    line.AddPoint(0.0, 0.0, 1.0);
    line.AddPoint(2.0, 3.0);
    line.AddPoint_2D(4.0, 5.0);
    line.AddPointM(6.0, 7.0, 8.0);
    line.AddPointZM(9.0, 10.0, 11.0, 12.0);
    assertThat(line.GetPointCount()).isEqualTo(5);

    line.SetPoint(0, 1.0, 2.0, 3.0);
    line.SetPoint(1, 2.5, 3.5);
    line.SetPoint_2D(2, 4.5, 5.5);
    line.SetPointM(3, 6.5, 7.5, 8.5);
    line.SetPointZM(4, 9.5, 10.5, 11.5, 12.5);

    assertThat(line.GetX()).isWithin(1e-9).of(1.0);
    assertThat(line.GetY()).isWithin(1e-9).of(2.0);
    assertThat(line.GetZ()).isWithin(1e-9).of(3.0);
    assertThat(line.GetM()).isWithin(1e-9).of(0.0);
    assertThat(line.GetX(4)).isWithin(1e-9).of(9.5);
    assertThat(line.GetY(4)).isWithin(1e-9).of(10.5);
    assertThat(line.GetZ(4)).isWithin(1e-9).of(11.5);
    assertThat(line.GetM(4)).isWithin(1e-9).of(12.5);

    assertThat(line.GetPoint_2D(0)).usingExactEquality().containsExactly(1.0, 2.0);
    assertThat(line.GetPoint(0)).usingExactEquality().containsExactly(1.0, 2.0, 3.0);
    double[] pt2dOut = new double[2];
    line.GetPoint_2D(1, pt2dOut);
    assertThat(pt2dOut).usingExactEquality().containsExactly(2.5, 3.5);
    double[] pt3dOut = new double[3];
    line.GetPoint(0, pt3dOut);
    assertThat(pt3dOut).usingExactEquality().containsExactly(1.0, 2.0, 3.0);
    double[] ptZmOut = new double[4];
    line.GetPointZM(4, ptZmOut);
    assertThat(ptZmOut).usingExactEquality().containsExactly(9.5, 10.5, 11.5, 12.5);

    double[][] ptsDefault = line.GetPoints();
    assertThat(ptsDefault).hasLength(5);
    double[][] pts2d = line.GetPoints(2);
    assertThat(pts2d[0]).hasLength(2);

    assertThat(line.GetDimension()).isEqualTo(1);
    assertThat(line.CoordinateDimension()).isEqualTo(4);
    // GetCoordinateDimension() only reflects 2D vs 3D; CoordinateDimension() includes M.
    assertThat(line.GetCoordinateDimension()).isEqualTo(3);
    assertThat(line.Is3D()).isEqualTo(1);
    assertThat(line.IsMeasured()).isEqualTo(1);
    line.SwapXY();
    assertThat(line.GetX(0)).isWithin(1e-9).of(2.0);

    double[] env2d = new double[4];
    line.GetEnvelope(env2d);
    assertThat(env2d[1]).isAtLeast(env2d[0]);
    double[] env3d = new double[6];
    line.GetEnvelope3D(env3d);
    assertThat(env3d[5]).isAtLeast(env3d[4]);

    Geometry cloned = line.Clone();
    Object javaCloned = line.clone();
    assertThat(line.equals(line)).isTrue();
    // Geometry.equals delegates to Equal(), so cloned geometries are equal.
    assertThat(line.equals(cloned)).isTrue();
    assertThat(line.equals(new Object())).isFalse();
    assertThat(line.hashCode()).isNotEqualTo(0);

    cloned.Set3D(0);
    assertThat(cloned.Is3D()).isEqualTo(0);
    cloned.SetMeasured(0);
    assertThat(cloned.IsMeasured()).isEqualTo(0);
    cloned.SetCoordinateDimension(3);
    assertThat(cloned.GetCoordinateDimension()).isEqualTo(3);
    cloned.FlattenTo2D();
    assertThat(cloned.GetCoordinateDimension()).isEqualTo(2);

    cloned.Empty();
    assertThat(cloned.IsEmpty()).isTrue();

    // MultiGeometry add/remove.
    Geometry collection = new Geometry(ogrConstants.wkbGeometryCollection);
    assertThat(collection.AddGeometry(Geometry.CreateFromWkt("POINT (1 1)")))
        .isEqualTo(ogrConstants.OGRERR_NONE);
    Geometry ownedPt = Geometry.CreateFromWkt("POINT (2 2)");
    assertThat(collection.AddGeometryDirectly(ownedPt)).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(collection.GetGeometryCount()).isEqualTo(2);
    assertThat(collection.GetGeometryRef(0).GetX()).isWithin(1e-9).of(1.0);
    assertThat(collection.RemoveGeometry(0)).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(collection.GetGeometryCount()).isEqualTo(1);

    // Sub-geometry filtering and ring closing.
    Geometry unclosedRing = new Geometry(ogrConstants.wkbLinearRing);
    unclosedRing.AddPoint_2D(0.0, 0.0);
    unclosedRing.AddPoint_2D(0.0, 2.0);
    unclosedRing.AddPoint_2D(2.0, 0.0);
    assertThat(unclosedRing.IsRing()).isFalse();
    Geometry polyWithUnclosed = new Geometry(ogrConstants.wkbPolygon);
    polyWithUnclosed.AddGeometryDirectly(unclosedRing);
    polyWithUnclosed.CloseRings();
    assertThat(polyWithUnclosed.GetGeometryRef(0).IsClockwise()).isTrue();

    Geometry lowerRemoved = collection.RemoveLowerDimensionSubGeoms();
    assertThat(lowerRemoved).isNotNull();

    lowerRemoved.delete();
    polyWithUnclosed.delete();
    collection.delete();
    ((Geometry) javaCloned).delete();
    cloned.delete();
    line.delete();
  }

  @Test
  @SuppressWarnings({"unchecked", "JdkObsolete"})
  public void predicatesGeosTopologyCurvesAndTransforms_workCorrectly() {
    Geometry square = Geometry.CreateFromWkt("POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))");
    Geometry shifted = Geometry.CreateFromWkt("POLYGON ((5 5, 5 15, 15 15, 15 5, 5 5))");
    Geometry insidePt = Geometry.CreateFromWkt("POINT (5 5)");
    Geometry farPt = Geometry.CreateFromWkt("POINT (30 5)");
    Geometry touchingLine = Geometry.CreateFromWkt("LINESTRING (10 0, 15 0)");
    Geometry crossLine = Geometry.CreateFromWkt("LINESTRING (-2 5, 12 5)");

    assertThat(square.Area()).isWithin(1e-6).of(100.0);
    assertThat(square.GetArea()).isWithin(1e-6).of(100.0);
    assertThat(square.IsValid()).isTrue();
    assertThat(square.IsSimple()).isTrue();

    assertThat(square.Intersects(shifted)).isTrue();
    assertThat(square.Intersect(shifted)).isTrue();
    assertThat(square.Disjoint(farPt)).isTrue();
    assertThat(square.Touches(touchingLine)).isTrue();
    assertThat(crossLine.Crosses(square)).isTrue();
    assertThat(insidePt.Within(square)).isTrue();
    assertThat(square.Contains(insidePt)).isTrue();
    assertThat(square.Overlaps(shifted)).isTrue();
    assertThat(insidePt.Distance(farPt)).isWithin(1e-6).of(25.0);

    Geometry pt3d1 = Geometry.CreateFromWkt("POINT Z (0 0 0)");
    Geometry pt3d2 = Geometry.CreateFromWkt("POINT Z (0 3 4)");
    // Distance3D requires SFCGAL (not enabled in Google3 builds), so it returns -1.
    ogr.DontUseExceptions();
    assertThat(pt3d1.Distance3D(pt3d2)).isWithin(1e-6).of(-1.0);
    ogr.UseExceptions();

    Vector<String> bufOpts = new Vector<>();
    bufOpts.add("ENDCAP_STYLE=ROUND");
    Geometry buf1 = insidePt.Buffer(2.0);
    Geometry buf2 = insidePt.Buffer(2.0, 8);
    Geometry buf3 = insidePt.Buffer(2.0, bufOpts);
    assertThat(buf1.GetArea()).isGreaterThan(10.0);
    assertThat(buf2.GetArea()).isGreaterThan(10.0);
    assertThat(buf3.GetArea()).isGreaterThan(10.0);

    Geometry inter = square.Intersection(shifted);
    assertThat(inter.GetArea()).isWithin(1e-6).of(25.0);
    Geometry union = square.Union(shifted);
    assertThat(union.GetArea()).isWithin(1e-6).of(175.0);
    Geometry diff = square.Difference(shifted);
    assertThat(diff.GetArea()).isWithin(1e-6).of(75.0);
    Geometry symDiff = square.SymDifference(shifted);
    Geometry symDiff2 = square.SymmetricDifference(shifted);
    assertThat(symDiff.GetArea()).isWithin(1e-6).of(150.0);
    assertThat(symDiff2.GetArea()).isWithin(1e-6).of(150.0);

    Geometry multiPoly =
        Geometry.CreateFromWkt(
            "MULTIPOLYGON (((0 0, 0 2, 2 2, 2 0, 0 0)), ((1 0, 1 2, 3 2, 3 0, 1 0)))");
    assertThat(multiPoly.UnionCascaded().GetArea()).isWithin(1e-6).of(6.0);
    assertThat(multiPoly.UnaryUnion().GetArea()).isWithin(1e-6).of(6.0);

    Geometry centroid = square.Centroid();
    assertThat(centroid.GetX()).isWithin(1e-6).of(5.0);
    Geometry onSurface = square.PointOnSurface();
    assertThat(square.Contains(onSurface)).isTrue();
    Geometry bnd1 = square.Boundary();
    Geometry bnd2 = square.GetBoundary();
    assertThat(bnd1.Length()).isWithin(1e-6).of(40.0);
    assertThat(bnd2.Length()).isWithin(1e-6).of(40.0);

    Geometry hull = multiPoly.ConvexHull();
    assertThat(hull.GetArea()).isWithin(1e-6).of(6.0);
    Geometry concave = multiPoly.ConcaveHull(0.8, false);
    assertThat(concave.IsEmpty()).isFalse();

    Geometry denseLine = Geometry.CreateFromWkt("LINESTRING (0 0, 5 0.01, 10 0)");
    assertThat(denseLine.Simplify(0.1).GetPointCount()).isEqualTo(2);
    assertThat(denseLine.SimplifyPreserveTopology(0.1).GetPointCount()).isEqualTo(2);
    denseLine.Segmentize(2.5);
    assertThat(denseLine.GetPointCount()).isGreaterThan(3);

    Geometry pts = Geometry.CreateFromWkt("MULTIPOINT ((0 0), (10 0), (10 10), (0 10))");
    assertThat(pts.DelaunayTriangulation().GetGeometryCount()).isEqualTo(2);
    assertThat(pts.DelaunayTriangulation(0.0).GetGeometryCount()).isEqualTo(2);
    assertThat(pts.DelaunayTriangulation(0.0, 1).GetGeometryCount()).isGreaterThan(0);

    Geometry polyLines =
        Geometry.CreateFromWkt(
            "MULTILINESTRING ((0 0, 0 10), (0 10, 10 10), (10 10, 10 0), (10 0, 0 0))");
    assertThat(polyLines.Polygonize().GetGeometryCount()).isEqualTo(1);
    Geometry closedRingLine = Geometry.CreateFromWkt("LINESTRING (0 0, 0 10, 10 10, 10 0, 0 0)");
    // BuildArea() is disabled via GOOGLE MODIFICATION in ogrgeometry.cpp (returns null).
    ogr.DontUseExceptions();
    assertThat(closedRingLine.BuildArea()).isNull();
    ogr.UseExceptions();
    closedRingLine.delete();
    assertThat(polyLines.ConstrainedDelaunayTriangulation()).isNotNull();

    Geometry bowtie = Geometry.CreateFromWkt("POLYGON ((0 0, 10 10, 10 0, 0 10, 0 0))");
    Vector<String> validOpts = new Vector<>();
    validOpts.add("METHOD=STRUCTURE");
    assertThat(bowtie.MakeValid().IsValid()).isTrue();
    assertThat(bowtie.MakeValid(validOpts).IsValid()).isTrue();

    Geometry unrounded = Geometry.CreateFromWkt("POINT (1.234 5.678)");
    assertThat(unrounded.SetPrecision(0.1).GetX()).isWithin(1e-6).of(1.2);
    assertThat(unrounded.SetPrecision(0.1, ogrConstants.GEOS_PREC_NO_TOPO).GetY())
        .isWithin(1e-6)
        .of(5.7);
    Geometry norm = square.Normalize();
    assertThat(norm.IsValid()).isTrue();

    // PreparedGeometry.
    PreparedGeometry prepared = square.CreatePreparedGeometry();
    assertThat(prepared).isNotNull();
    assertThat(prepared.Intersects(insidePt)).isTrue();
    assertThat(prepared.Contains(insidePt)).isTrue();
    assertThat(prepared.Contains(farPt)).isFalse();
    assertThat(prepared.equals(prepared)).isTrue();
    assertThat(prepared.equals(new Object())).isFalse();
    assertThat(prepared.hashCode()).isNotEqualTo(0);

    // Curves & Value.
    Geometry circ = Geometry.CreateFromWkt("CIRCULARSTRING (0 0, 1 1, 2 0)");
    assertThat(circ.HasCurveGeometry()).isEqualTo(1);
    assertThat(circ.HasCurveGeometry(1)).isEqualTo(1);
    Geometry linearized = circ.GetLinearGeometry();
    assertThat(linearized.HasCurveGeometry()).isEqualTo(0);
    Vector<String> curveOpts = new Vector<>();
    curveOpts.add("ADD_INTERMEDIATE_POINT=NO");
    circ.GetLinearGeometry(5.0).delete();
    circ.GetLinearGeometry(5.0, curveOpts).delete();
    Geometry curvedBack = square.GetCurveGeometry();
    assertThat(curvedBack).isNotNull();
    square.GetCurveGeometry(curveOpts).delete();
    Geometry midPt = touchingLine.Value(2.5);
    assertThat(midPt.GetX()).isWithin(1e-6).of(12.5);

    // Geodesic & CRS transforms + GeomTransformer.
    SpatialReference wgs84 = new SpatialReference();
    wgs84.ImportFromEPSG(4326);
    wgs84.SetAxisMappingStrategy(osrConstants.OAMS_TRADITIONAL_GIS_ORDER);
    SpatialReference merc = new SpatialReference();
    merc.ImportFromEPSG(3857);
    merc.SetAxisMappingStrategy(osrConstants.OAMS_TRADITIONAL_GIS_ORDER);

    Geometry geoBox = Geometry.CreateFromWkt("POLYGON ((0 0, 0 1, 1 1, 1 0, 0 0))");
    geoBox.AssignSpatialReference(wgs84);
    assertThat(geoBox.GetSpatialReference()).isNotNull();
    assertThat(geoBox.GeodesicArea()).isGreaterThan(1.0e10);
    Geometry geoLine = Geometry.CreateFromWkt("LINESTRING (0 0, 1 0)");
    geoLine.AssignSpatialReference(wgs84);
    assertThat(geoLine.GeodesicLength()).isGreaterThan(100000.0);

    CoordinateTransformation ct = new CoordinateTransformation(wgs84, merc);
    Geometry boxForCt = geoBox.Clone();
    assertThat(boxForCt.Transform(ct)).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(boxForCt.TransformTo(wgs84)).isEqualTo(ogrConstants.OGRERR_NONE);

    GeomTransformer gt1 = new GeomTransformer(ct);
    GeomTransformer gt2 = new GeomTransformer(ct, new Vector<String>());
    Geometry transformedViaGt = geoBox.Transform(gt1);
    assertThat(transformedViaGt).isNotNull();
    Geometry directGt = gt2.Transform(geoBox);
    assertThat(directGt).isNotNull();
    assertThat(gt1.equals(gt1)).isTrue();
    assertThat(gt1.equals(new Object())).isFalse();
    assertThat(gt1.hashCode()).isNotEqualTo(0);

    directGt.delete();
    transformedViaGt.delete();
    gt2.delete();
    gt1.delete();
    boxForCt.delete();
    ct.delete();
    geoLine.delete();
    geoBox.delete();
    merc.delete();
    wgs84.delete();
    midPt.delete();
    curvedBack.delete();
    linearized.delete();
    circ.delete();
    prepared.delete();
    norm.delete();
    unrounded.delete();
    bowtie.delete();
    polyLines.delete();
    pts.delete();
    concave.delete();
    hull.delete();
    bnd2.delete();
    bnd1.delete();
    onSurface.delete();
    centroid.delete();
    multiPoly.delete();
    symDiff2.delete();
    symDiff.delete();
    diff.delete();
    union.delete();
    inter.delete();
    buf3.delete();
    buf2.delete();
    buf1.delete();
    pt3d2.delete();
    pt3d1.delete();
    crossLine.delete();
    touchingLine.delete();
    farPt.delete();
    insidePt.delete();
    shifted.delete();
    square.delete();
  }
}
