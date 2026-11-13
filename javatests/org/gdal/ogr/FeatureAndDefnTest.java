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

import java.nio.charset.StandardCharsets;
import java.util.Hashtable;
import java.util.List;
import java.util.Vector;
import org.gdal.GoogleGdalLoader;
import org.gdal.gdal.gdal;
import org.gdal.osr.SpatialReference;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;

public final class FeatureAndDefnTest {

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
  public void fieldDefnGeomPrecisionGeomFieldDefnAndFeatureDefn_roundTripAllProperties() {
    FieldDefn emptyField = new FieldDefn();
    FieldDefn namedField = new FieldDefn("label");
    FieldDefn intField = new FieldDefn("count", ogrConstants.OFTInteger);

    emptyField.SetName("alt");
    emptyField.SetType(ogrConstants.OFTString);
    assertThat(emptyField.GetName()).isEqualTo("alt");
    assertThat(emptyField.GetNameRef()).isEqualTo("alt");
    assertThat(new String(emptyField.GetNameAsByteArray(), StandardCharsets.UTF_8))
        .isEqualTo("alt");

    intField.SetAlternativeName("Count Alias");
    assertThat(intField.GetAlternativeName()).isEqualTo("Count Alias");
    assertThat(intField.GetAlternativeNameRef()).isEqualTo("Count Alias");
    assertThat(new String(intField.GetAlternativeNameAsByteArray(), StandardCharsets.UTF_8))
        .isEqualTo("Count Alias");
    assertThat(intField.GetType()).isEqualTo(ogrConstants.OFTInteger);
    assertThat(intField.GetFieldType()).isEqualTo(ogrConstants.OFTInteger);
    assertThat(intField.GetTypeName()).isEqualTo("Integer");
    assertThat(intField.GetFieldTypeName(ogrConstants.OFTReal)).isEqualTo("Real");

    intField.SetSubType(ogrConstants.OFSTInt16);
    assertThat(intField.GetSubType()).isEqualTo(ogrConstants.OFSTInt16);
    intField.SetJustify(ogrConstants.OJRight);
    assertThat(intField.GetJustify()).isEqualTo(ogrConstants.OJRight);
    intField.SetWidth(12);
    assertThat(intField.GetWidth()).isEqualTo(12);
    intField.SetPrecision(3);
    assertThat(intField.GetPrecision()).isEqualTo(3);
    intField.SetTZFlag(ogrConstants.TZFLAG_UTC);
    assertThat(intField.GetTZFlag()).isEqualTo(ogrConstants.TZFLAG_UTC);
    intField.SetIgnored(1);
    assertThat(intField.IsIgnored()).isEqualTo(1);
    intField.SetIgnored(0);
    intField.SetNullable(0);
    assertThat(intField.IsNullable()).isEqualTo(0);
    intField.SetUnique(1);
    assertThat(intField.IsUnique()).isEqualTo(1);
    intField.SetGenerated(1);
    assertThat(intField.IsGenerated()).isEqualTo(1);
    intField.SetGenerated(0);
    intField.SetDefault("42");
    assertThat(intField.GetDefault()).isEqualTo("42");
    assertThat(new String(intField.GetDefaultAsByteArray(), StandardCharsets.UTF_8))
        .isEqualTo("42");
    assertThat(intField.IsDefaultDriverSpecific()).isEqualTo(0);
    intField.SetDomainName("range_domain");
    assertThat(intField.GetDomainName()).isEqualTo("range_domain");
    assertThat(new String(intField.GetDomainNameAsByteArray(), StandardCharsets.UTF_8))
        .isEqualTo("range_domain");
    intField.SetComment("sample comment");
    assertThat(intField.GetComment()).isEqualTo("sample comment");
    assertThat(new String(intField.GetCommentAsByteArray(), StandardCharsets.UTF_8))
        .isEqualTo("sample comment");
    assertThat(intField.equals(intField)).isTrue();
    assertThat(intField.equals(new Object())).isFalse();
    assertThat(intField.hashCode()).isNotEqualTo(0);

    SpatialReference wgs84 = new SpatialReference();
    wgs84.ImportFromEPSG(4326);
    GeomCoordinatePrecision prec = ogr.CreateGeomCoordinatePrecision();
    prec.Set(1e-7, 1e-3, 1e-2);
    assertThat(prec.GetXYResolution()).isWithin(1e-9).of(1e-7);
    assertThat(prec.GetZResolution()).isWithin(1e-9).of(1e-3);
    assertThat(prec.GetMResolution()).isWithin(1e-9).of(1e-2);
    prec.SetFromMeter(wgs84, 1.0, 0.5, 0.1);
    Hashtable<String, String> gpkgOpts = new Hashtable<>();
    gpkgOpts.put("TEST", "1");
    prec.SetFormatSpecificOptions("GPKG", gpkgOpts);
    assertThat(prec.GetFormatSpecificOptions("GPKG")).isNotNull();
    List<String> formats = prec.GetFormats();
    assertThat(formats).contains("GPKG");
    assertThat(prec.equals(prec)).isTrue();
    assertThat(prec.equals(new Object())).isFalse();
    assertThat(prec.hashCode()).isNotEqualTo(0);

    GeomFieldDefn gfEmpty = new GeomFieldDefn();
    GeomFieldDefn gfNamed = new GeomFieldDefn("geom");
    GeomFieldDefn gf = new GeomFieldDefn("geom_main", ogrConstants.wkbPoint);
    gfEmpty.SetName("g0");
    assertThat(gfEmpty.GetName()).isEqualTo("g0");
    assertThat(gfEmpty.GetNameRef()).isEqualTo("g0");
    assertThat(new String(gfEmpty.GetNameAsByteArray(), StandardCharsets.UTF_8)).isEqualTo("g0");
    gf.SetType(ogrConstants.wkbPolygon);
    assertThat(gf.GetType()).isEqualTo(ogrConstants.wkbPolygon);
    gf.SetSpatialRef(wgs84);
    assertThat(gf.GetSpatialRef().IsGeographic()).isEqualTo(1);
    gf.SetIgnored(1);
    assertThat(gf.IsIgnored()).isEqualTo(1);
    gf.SetIgnored(0);
    gf.SetNullable(0);
    assertThat(gf.IsNullable()).isEqualTo(0);
    gf.SetCoordinatePrecision(prec);
    assertThat(gf.GetCoordinatePrecision()).isNotNull();
    assertThat(gf.equals(gf)).isTrue();
    assertThat(gf.equals(new Object())).isFalse();
    assertThat(gf.hashCode()).isNotEqualTo(0);

    FeatureDefn defn0 = new FeatureDefn();
    assertThat(defn0).isNotNull();
    FeatureDefn defn = new FeatureDefn("roads");
    assertThat(defn.GetName()).isEqualTo("roads");
    assertThat(new String(defn.GetNameAsByteArray(), StandardCharsets.UTF_8)).isEqualTo("roads");
    defn.AddFieldDefn(intField);
    defn.AddFieldDefn(namedField);
    assertThat(defn.GetFieldCount()).isEqualTo(2);
    assertThat(defn.GetFieldDefn(0).GetName()).isEqualTo("count");
    assertThat(defn.GetFieldIndex("label")).isEqualTo(1);

    defn.SetGeomType(ogrConstants.wkbLineString);
    assertThat(defn.GetGeomType()).isEqualTo(ogrConstants.wkbLineString);
    assertThat(defn.GetGeomFieldCount()).isEqualTo(1);
    defn.AddGeomFieldDefn(gf);
    assertThat(defn.GetGeomFieldCount()).isEqualTo(2);
    assertThat(defn.GetGeomFieldDefn(1).GetName()).isEqualTo("geom_main");
    assertThat(defn.GetGeomFieldIndex("geom_main")).isEqualTo(1);
    assertThat(defn.DeleteGeomFieldDefn(1)).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(defn.GetGeomFieldCount()).isEqualTo(1);
    defn.SetGeometryIgnored(1);
    assertThat(defn.IsGeometryIgnored()).isEqualTo(1);
    defn.SetGeometryIgnored(0);
    defn.SetStyleIgnored(1);
    assertThat(defn.IsStyleIgnored()).isEqualTo(1);
    defn.SetStyleIgnored(0);
    assertThat(defn.GetReferenceCount()).isAtLeast(0);
    assertThat(defn.IsSame(defn)).isEqualTo(1);
    assertThat(defn.equals(defn)).isTrue();
    assertThat(defn.equals(new Object())).isFalse();
    assertThat(defn.hashCode()).isNotEqualTo(0);

    defn.delete();
    defn0.delete();
    gf.delete();
    gfNamed.delete();
    gfEmpty.delete();
    prec.delete();
    wgs84.delete();
    intField.delete();
    namedField.delete();
    emptyField.delete();
  }

  @Test
  @SuppressWarnings({"unchecked", "JdkObsolete"})
  public void feature_allFieldTypesGeometriesValidationAndCloning() {
    FeatureDefn defn = new FeatureDefn("items");
    defn.AddFieldDefn(new FieldDefn("i32", ogrConstants.OFTInteger));
    defn.AddFieldDefn(new FieldDefn("i64", ogrConstants.OFTInteger64));
    defn.AddFieldDefn(new FieldDefn("dbl", ogrConstants.OFTReal));
    defn.AddFieldDefn(new FieldDefn("str", ogrConstants.OFTString));
    defn.AddFieldDefn(new FieldDefn("dt", ogrConstants.OFTDateTime));
    defn.AddFieldDefn(new FieldDefn("ilist", ogrConstants.OFTIntegerList));
    defn.AddFieldDefn(new FieldDefn("dlist", ogrConstants.OFTRealList));
    defn.AddFieldDefn(new FieldDefn("slist", ogrConstants.OFTStringList));
    defn.AddFieldDefn(new FieldDefn("bin", ogrConstants.OFTBinary));
    FieldDefn withDefault = new FieldDefn("def", ogrConstants.OFTString);
    withDefault.SetDefault("'fallback'");
    defn.AddFieldDefn(withDefault);
    defn.AddGeomFieldDefn(new GeomFieldDefn("g2", ogrConstants.wkbPoint));

    Feature feat = new Feature(defn);
    assertThat(feat.GetDefnRef().GetName()).isEqualTo("items");
    assertThat(feat.GetFieldCount()).isEqualTo(10);
    assertThat(feat.GetGeomFieldCount()).isEqualTo(2);
    assertThat(feat.GetFieldDefnRef(0).GetName()).isEqualTo("i32");
    assertThat(feat.GetFieldDefnRef("str").GetType()).isEqualTo(ogrConstants.OFTString);
    assertThat(feat.GetGeomFieldDefnRef(1).GetName()).isEqualTo("g2");
    assertThat(feat.GetGeomFieldDefnRef("g2").GetType()).isEqualTo(ogrConstants.wkbPoint);
    assertThat(feat.GetFieldIndex("dt")).isEqualTo(4);
    assertThat(feat.GetGeomFieldIndex("g2")).isEqualTo(1);
    assertThat(feat.GetFieldType(0)).isEqualTo(ogrConstants.OFTInteger);
    assertThat(feat.GetFieldType("str")).isEqualTo(ogrConstants.OFTString);

    feat.SetFID(101L);
    assertThat(feat.GetFID()).isEqualTo(101L);

    // By index & by name setters/getters.
    feat.SetField(0, 7);
    assertThat(feat.GetFieldAsInteger(0)).isEqualTo(7);
    feat.SetField("i32", 9);
    assertThat(feat.GetFieldAsInteger("i32")).isEqualTo(9);

    feat.SetFieldInteger64(1, 5_000_000_000L);
    assertThat(feat.GetFieldAsInteger64(1)).isEqualTo(5_000_000_000L);
    assertThat(feat.GetFieldAsInteger64("i64")).isEqualTo(5_000_000_000L);

    feat.SetField(2, 3.25);
    assertThat(feat.GetFieldAsDouble(2)).isWithin(1e-9).of(3.25);
    feat.SetField("dbl", 6.5);
    assertThat(feat.GetFieldAsDouble("dbl")).isWithin(1e-9).of(6.5);

    feat.SetField(3, "alpha");
    assertThat(feat.GetFieldAsString(3)).isEqualTo("alpha");
    feat.SetField("str", "beta");
    assertThat(feat.GetFieldAsString("str")).isEqualTo("beta");
    assertThat(new String(feat.GetFieldAsStringAsByteArray(3), StandardCharsets.UTF_8))
        .isEqualTo("beta");
    assertThat(new String(feat.GetFieldAsStringAsByteArray("str"), StandardCharsets.UTF_8))
        .isEqualTo("beta");

    feat.SetField(4, 2026, 5, 6, 12, 30, 15.5f, ogrConstants.TZFLAG_UTC);
    feat.SetField("dt", 2026, 5, 6, 12, 30, 15.5f, ogrConstants.TZFLAG_UTC);
    int[] y = new int[1];
    int[] mo = new int[1];
    int[] d = new int[1];
    int[] h = new int[1];
    int[] mi = new int[1];
    float[] s = new float[1];
    int[] tz = new int[1];
    feat.GetFieldAsDateTime(4, y, mo, d, h, mi, s, tz);
    assertThat(y[0]).isEqualTo(2026);
    feat.GetFieldAsDateTime("dt", y, mo, d, h, mi, s, tz);
    assertThat(mo[0]).isEqualTo(5);
    assertThrows(
        NullPointerException.class, () -> feat.GetFieldAsDateTime(4, null, mo, d, h, mi, s, tz));
    assertThrows(
        IndexOutOfBoundsException.class,
        () -> feat.GetFieldAsDateTime("dt", new int[0], mo, d, h, mi, s, tz));
    Vector<String> dtOpts = new Vector<>();
    dtOpts.add("DUMMY=1");
    assertThat(feat.GetFieldAsISO8601DateTime(4)).startsWith("2026-05-06");
    assertThat(feat.GetFieldAsISO8601DateTime("dt")).startsWith("2026-05-06");
    assertThat(feat.GetFieldAsISO8601DateTime(4, dtOpts)).startsWith("2026-05-06");
    assertThat(feat.GetFieldAsISO8601DateTime("dt", dtOpts)).startsWith("2026-05-06");

    feat.SetFieldIntegerList(5, new int[] {10, 20});
    assertThat(feat.GetFieldAsIntegerList(5)).asList().containsExactly(10, 20).inOrder();
    feat.SetFieldDoubleList(6, new double[] {1.5, 2.5});
    assertThat(feat.GetFieldAsDoubleList(6)).usingExactEquality().containsExactly(1.5, 2.5);
    Vector<String> slist = new Vector<>();
    slist.add("x");
    slist.add("y");
    feat.SetFieldStringList(7, slist);
    assertThat(feat.GetFieldAsStringList(7)).asList().containsExactly("x", "y").inOrder();

    feat.SetFieldBinaryFromHexString(8, "DEADBEEF");
    feat.SetFieldBinaryFromHexString("bin", "CAFEBABE");
    assertThat(feat.GetFieldAsBinary(8)).hasLength(4);
    assertThat(feat.GetFieldAsBinary("bin")).hasLength(4);

    // Null & unset checks.
    assertThat(feat.IsFieldSet(0)).isTrue();
    assertThat(feat.IsFieldSet("i32")).isTrue();
    assertThat(feat.IsFieldSetAndNotNull(0)).isTrue();
    assertThat(feat.IsFieldSetAndNotNull("i32")).isTrue();
    feat.SetFieldNull(0);
    assertThat(feat.IsFieldNull(0)).isTrue();
    feat.SetFieldNull("i64");
    assertThat(feat.IsFieldNull("i64")).isTrue();
    feat.UnsetField(0);
    assertThat(feat.IsFieldSet(0)).isFalse();
    feat.UnsetField("i64");
    feat.FillUnsetWithDefault();
    feat.FillUnsetWithDefault(0);
    feat.FillUnsetWithDefault(0, dtOpts);
    assertThat(feat.GetFieldAsString("def")).isEqualTo("fallback");

    // Geometry setters/getters.
    Geometry pt1 = ogr.CreateGeometryFromWkt("POINT (1 2)");
    Geometry pt2 = ogr.CreateGeometryFromWkt("POINT (3 4)");
    assertThat(feat.SetGeometry(pt1)).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(feat.GetGeometryRef().GetX()).isWithin(1e-9).of(1.0);
    assertThat(feat.SetGeometryDirectly(pt2.Clone())).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(feat.SetGeomField(1, pt1)).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(feat.SetGeomField("g2", pt1)).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(feat.SetGeomFieldDirectly(1, pt2.Clone())).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(feat.SetGeomFieldDirectly("g2", pt2.Clone())).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(feat.GetGeomFieldRef(1).GetY()).isWithin(1e-9).of(4.0);
    assertThat(feat.GetGeomFieldRef("g2").GetY()).isWithin(1e-9).of(4.0);

    // Style & native data.
    feat.SetStyleString("PEN(c:#FF0000)");
    assertThat(feat.GetStyleString()).isEqualTo("PEN(c:#FF0000)");
    assertThat(new String(feat.GetStyleStringAsByteArray(), StandardCharsets.UTF_8))
        .isEqualTo("PEN(c:#FF0000)");
    feat.SetNativeData("{\"raw\":1}");
    feat.SetNativeMediaType("application/json");
    assertThat(feat.GetNativeData()).isEqualTo("{\"raw\":1}");
    assertThat(new String(feat.GetNativeDataAsByteArray(), StandardCharsets.UTF_8))
        .isEqualTo("{\"raw\":1}");
    assertThat(feat.GetNativeMediaType()).isEqualTo("application/json");

    // Validation & DumpReadable.
    assertThat(feat.Validate()).isEqualTo(1);
    assertThat(feat.Validate(ogrConstants.F_VAL_ALL)).isEqualTo(1);
    assertThat(feat.Validate(ogrConstants.F_VAL_ALL, 0)).isEqualTo(1);
    feat.DumpReadable();
    assertThat(feat.DumpReadableAsString()).contains("OGRFeature(items):101");
    assertThat(feat.DumpReadableAsString(dtOpts)).contains("OGRFeature(items)");

    // Cloning & SetFrom.
    Feature cloned = feat.Clone();
    Object javaCloned = feat.clone();
    assertThat(cloned.Equal(feat)).isTrue();
    assertThat(((Feature) javaCloned).Equal(feat)).isTrue();
    assertThat(feat.equals(feat)).isTrue();
    // Feature.equals delegates to OGR_F_Equal rather than pointer identity.
    assertThat(feat.equals(cloned)).isTrue();
    assertThat(feat.equals(new Object())).isFalse();
    assertThat(feat.hashCode()).isNotEqualTo(0);

    Feature target = new Feature(defn);
    assertThat(target.SetFrom(feat)).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(target.SetFrom(feat, 1)).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(target.SetFromWithMap(feat, 1, new int[] {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}))
        .isEqualTo(ogrConstants.OGRERR_NONE);

    target.delete();
    ((Feature) javaCloned).delete();
    cloned.delete();
    pt2.delete();
    pt1.delete();
    feat.delete();
    defn.delete();
  }
}
