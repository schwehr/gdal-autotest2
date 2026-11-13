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
import java.util.Vector;
import org.gdal.GoogleGdalLoader;
import org.gdal.gdal.gdal;
import org.gdal.osr.SpatialReference;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;

public final class OgrDataSourceDriverLayerTest {

  private static final String VSIMEM_DIR = "/vsimem/OgrDataSourceDriverLayerTest";

  private static final class CountingProgressCallback extends ProgressCallback {
    int count;

    @Override
    public int run(double dfComplete, String pszMessage) {
      count++;
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
    gdal.ErrorReset();
    gdal.RmdirRecursive(VSIMEM_DIR);
  }

  @Test
  @SuppressWarnings({"unchecked", "JdkObsolete"})
  public void driverAndDataSource_createCopySqlTransactionsAndClose() {
    Driver gpkgDriver = ogr.GetDriverByName("GPKG");
    assertThat(gpkgDriver).isNotNull();
    assertThat(gpkgDriver.GetName()).isEqualTo("GPKG");
    assertThat(gpkgDriver.getName()).isEqualTo("GPKG");
    assertThat(gpkgDriver.TestCapability(ogrConstants.ODrCCreateDataSource)).isTrue();
    assertThat(gpkgDriver.equals(gpkgDriver)).isTrue();
    assertThat(gpkgDriver.equals(new Object())).isFalse();
    assertThat(gpkgDriver.hashCode()).isNotEqualTo(0);

    try {
      gpkgDriver.Deregister();
      assertThat(ogr.GetDriverByName("GPKG")).isNull();
    } finally {
      gpkgDriver.Register();
    }
    assertThat(ogr.GetDriverByName("GPKG")).isNotNull();

    String dsPath = VSIMEM_DIR + "/test.gpkg";
    String copyPath = VSIMEM_DIR + "/copy.gpkg";
    String memCopyPath = VSIMEM_DIR + "/copy2.gpkg";

    SpatialReference wgs84 = new SpatialReference();
    wgs84.ImportFromEPSG(4326);

    DataSource ds = gpkgDriver.CreateDataSource(dsPath, new Vector<String>());
    Vector<String> gpkgLayerOpts = new Vector<>();
    gpkgLayerOpts.add("SPATIAL_INDEX=YES");
    assertThat(ds.GetName()).isEqualTo(dsPath);
    assertThat(ds.getName()).isEqualTo(dsPath);
    assertThat(new String(ds.GetNameAsByteArray(), StandardCharsets.UTF_8)).isEqualTo(dsPath);
    assertThat(ds.GetDriver().GetName()).isEqualTo("GPKG");
    assertThat(ds.GetRefCount()).isAtLeast(1);
    assertThat(ds.GetSummaryRefCount()).isAtLeast(1);
    assertThat(ds.TestCapability(ogrConstants.ODsCCreateLayer)).isTrue();
    assertThat(ds.equals(ds)).isTrue();
    assertThat(ds.equals(new Object())).isFalse();
    assertThat(ds.hashCode()).isNotEqualTo(0);

    Layer l1 = ds.CreateLayer("l1");
    Layer l2 = ds.CreateLayer("l2", wgs84);
    Layer l3 = ds.CreateLayer("l3", wgs84, ogrConstants.wkbPoint);
    Layer l4 = ds.CreateLayer("l4", wgs84, ogrConstants.wkbPolygon, gpkgLayerOpts);
    assertThat(l1).isNotNull();
    assertThat(l2).isNotNull();
    assertThat(l3).isNotNull();
    assertThat(l4).isNotNull();

    Layer copied2 = ds.CopyLayer(l3, "l3_copy2");
    Layer copied3 = ds.CopyLayer(l3, "l3_copy3", gpkgLayerOpts);
    assertThat(copied2).isNotNull();
    assertThat(copied3).isNotNull();
    assertThat(ds.GetLayerCount()).isEqualTo(6);
    assertThat(ds.GetLayer(0).GetName()).isEqualTo("l1");
    assertThat(ds.GetLayer("l3").GetName()).isEqualTo("l3");
    assertThat(ds.GetLayerByIndex(1).GetName()).isEqualTo("l2");
    assertThat(ds.GetLayerByName("l4").GetName()).isEqualTo("l4");

    assertThat(ds.StartTransaction()).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(ds.CommitTransaction()).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(ds.StartTransaction(1)).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(ds.RollbackTransaction()).isEqualTo(ogrConstants.OGRERR_NONE);

    StyleTable styles = new StyleTable();
    styles.AddStyle("s1", "PEN(c:#000000)");
    ds.SetStyleTable(styles);
    assertThat(ds.GetStyleTable()).isNotNull();

    Layer sql1 = ds.ExecuteSQL("SELECT count(*) FROM l1");
    assertThat(sql1.GetNextFeature()).isNotNull();
    ds.ReleaseResultSet(sql1);
    Geometry filterRect = ogr.CreateGeometryFromEnvelope(-10, -10, 10, 10);
    Layer sql2 = ds.ExecuteSQL("SELECT * FROM l3", filterRect);
    ds.ReleaseResultSet(sql2);
    Layer sql3 = ds.ExecuteSQL("SELECT * FROM l3", filterRect, "SQLITE");
    ds.ReleaseResultSet(sql3);
    assertThat(ds.AbortSQL()).isEqualTo(ogrConstants.OGRERR_NONE);

    assertThat(ds.DeleteLayer(0)).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(ds.SyncToDisk()).isEqualTo(ogrConstants.OGRERR_NONE);
    ds.FlushCache();

    Vector<String> gpkgCopyOpts = new Vector<>();
    gpkgCopyOpts.add("ADD_GPKG_OGR_CONTENTS=YES");
    DataSource dsCopy2 = gpkgDriver.CopyDataSource(ds, copyPath);
    assertThat(dsCopy2).isNotNull();
    DataSource dsCopy3 = gpkgDriver.CopyDataSource(ds, memCopyPath, gpkgCopyOpts);
    assertThat(dsCopy3).isNotNull();
    dsCopy3.delete();
    dsCopy2.delete();
    // Note: ogr.DataSource.Close() frees the native handle without clearing swigCMemOwn;
    // ds.delete() is the safe Java close path that also prevents finalizer double-free.
    ds.delete();

    DataSource opened1 = gpkgDriver.Open(copyPath);
    assertThat(opened1.GetLayerCount()).isEqualTo(5);
    opened1.delete();
    DataSource opened2 = gpkgDriver.Open(copyPath, 1);
    assertThat(opened2.GetLayerCount()).isEqualTo(5);
    opened2.delete();

    assertThat(gpkgDriver.DeleteDataSource(copyPath)).isEqualTo(ogrConstants.OGRERR_NONE);
    filterRect.delete();
    styles.delete();
    wgs84.delete();
  }

  @Test
  @SuppressWarnings({"unchecked", "JdkObsolete"})
  public void layer_crudFilteringAlterSchemaAndAllOverlayOperations() {
    Driver memDriver = ogr.GetDriverByName("Memory");
    DataSource ds = memDriver.CreateDataSource("mem_ds");
    SpatialReference wgs84 = new SpatialReference();
    wgs84.ImportFromEPSG(4326);

    Layer layer = ds.CreateLayer("polys", wgs84, ogrConstants.wkbPolygon);
    assertThat(layer.GetDataset().GetDescription()).isEqualTo("mem_ds");
    assertThat(layer.GetName()).isEqualTo("polys");
    assertThat(new String(layer.GetNameAsByteArray(), StandardCharsets.UTF_8)).isEqualTo("polys");
    assertThat(layer.GetGeomType()).isEqualTo(ogrConstants.wkbPolygon);
    assertThat(layer.GetRefCount()).isAtLeast(0);
    assertThat(layer.GetSpatialRef().IsGeographic()).isEqualTo(1);
    assertThat(layer.GetFIDColumn()).isEmpty();
    assertThat(layer.GetFIDColumnAsByteArray()).isEmpty();
    assertThat(layer.GetGeometryColumn()).isEmpty();
    assertThat(layer.GetGeometryColumnAsByteArray()).isEmpty();
    assertThat(layer.equals(layer)).isTrue();
    assertThat(layer.equals(new Object())).isFalse();
    assertThat(layer.hashCode()).isNotEqualTo(0);

    // Schema creation & reordering.
    assertThat(layer.CreateField(new FieldDefn("a", ogrConstants.OFTInteger)))
        .isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(layer.CreateField(new FieldDefn("b", ogrConstants.OFTString), 1))
        .isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(layer.FindFieldIndex("b", 1)).isEqualTo(1);
    FieldDefn bNew = new FieldDefn("b_renamed", ogrConstants.OFTString);
    assertThat(layer.AlterFieldDefn(1, bNew, ogrConstants.ALTER_NAME_FLAG))
        .isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(layer.ReorderField(0, 1)).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(layer.ReorderFields(new int[] {1, 0})).isEqualTo(ogrConstants.OGRERR_NONE);

    assertThat(layer.CreateGeomField(new GeomFieldDefn("g_extra", ogrConstants.wkbPoint)))
        .isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(layer.CreateGeomField(new GeomFieldDefn("g_extra2", ogrConstants.wkbPoint), 1))
        .isEqualTo(ogrConstants.OGRERR_NONE);
    GeomFieldDefn gfAlt = new GeomFieldDefn("g_extra_renamed", ogrConstants.wkbPoint);
    assertThat(layer.AlterGeomFieldDefn(1, gfAlt, ogrConstants.ALTER_GEOM_FIELD_DEFN_NAME_FLAG))
        .isEqualTo(ogrConstants.OGRERR_NONE);
    // SetActiveSRS is only supported on select drivers; verify error handling on Memory layer.
    ogr.DontUseExceptions();
    assertThat(layer.SetActiveSRS(1, wgs84)).isEqualTo(ogrConstants.OGRERR_FAILURE);
    ogr.UseExceptions();

    // Create, Update, Upsert, Set, Get, Delete features.
    FeatureDefn defn = layer.GetLayerDefn();
    Feature f1 = new Feature(defn);
    f1.SetField("a", 10);
    f1.SetField("b_renamed", "first");
    f1.SetGeometryDirectly(ogr.CreateGeometryFromWkt("POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0))"));
    assertThat(layer.CreateFeature(f1)).isEqualTo(ogrConstants.OGRERR_NONE);

    Feature f2 = new Feature(defn);
    f2.SetField("a", 20);
    f2.SetField("b_renamed", "second");
    f2.SetGeometryDirectly(ogr.CreateGeometryFromWkt("POLYGON ((20 0, 20 10, 30 10, 30 0, 20 0))"));
    assertThat(layer.CreateFeature(f2)).isEqualTo(ogrConstants.OGRERR_NONE);

    f1.SetField("a", 11);
    assertThat(layer.SetFeature(f1)).isEqualTo(ogrConstants.OGRERR_NONE);
    f1.SetField("a", 12);
    assertThat(layer.UpsertFeature(f1)).isEqualTo(ogrConstants.OGRERR_NONE);
    f1.SetField("a", 13);
    assertThat(layer.UpdateFeature(f1, new int[] {0}, null, false))
        .isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(layer.GetFeature(f1.GetFID()).GetFieldAsInteger("a")).isEqualTo(13);

    // Filtering, reading & extent.
    assertThat(layer.GetFeatureCount()).isEqualTo(2L);
    assertThat(layer.GetFeatureCount(1)).isEqualTo(2L);
    assertThat(layer.GetExtent())
        .usingTolerance(1e-6)
        .containsExactly(0.0, 30.0, 0.0, 10.0)
        .inOrder();
    assertThat(layer.GetExtent(true)).hasLength(4);
    double[] extArg = new double[4];
    assertThat(layer.GetExtent(extArg, 1)).isEqualTo(ogrConstants.OGRERR_NONE);

    Vector<String> ignored = new Vector<>();
    ignored.add("b_renamed");
    assertThat(layer.SetIgnoredFields(ignored)).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(layer.SetAttributeFilter("a >= 13")).isEqualTo(ogrConstants.OGRERR_NONE);
    layer.SetSpatialFilterRect(0.0, 0.0, 12.0, 12.0);
    layer.SetSpatialFilterRect(0, 0.0, 0.0, 12.0, 12.0);
    layer.ResetReading();
    assertThat(layer.GetNextFeature().GetFieldAsInteger("a")).isEqualTo(13);
    assertThat(layer.GetFeaturesRead()).isAtLeast(1L);
    assertThat(layer.SetNextByIndex(0L)).isEqualTo(ogrConstants.OGRERR_NONE);

    Geometry rect = ogr.CreateGeometryFromEnvelope(0, 0, 12, 12);
    layer.SetSpatialFilter(rect);
    layer.SetSpatialFilter(0, rect);
    assertThat(layer.GetSpatialFilter()).isNotNull();
    layer.SetSpatialFilter(null);
    layer.SetAttributeFilter(null);
    layer.SetIgnoredFields(new Vector<String>());

    assertThat(layer.TestCapability(ogrConstants.OLCRandomRead)).isTrue();
    StyleTable lStyles = new StyleTable();
    layer.SetStyleTable(lStyles);
    assertThat(layer.GetStyleTable()).isNotNull();

    // Transactions & schema cleanup (Memory layer does not support layer rollback/rename).
    assertThat(layer.StartTransaction()).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(layer.CommitTransaction()).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(layer.StartTransaction()).isEqualTo(ogrConstants.OGRERR_NONE);
    ogr.DontUseExceptions();
    assertThat(layer.RollbackTransaction()).isEqualTo(ogrConstants.OGRERR_UNSUPPORTED_OPERATION);
    assertThat(layer.Rename("polys_renamed")).isEqualTo(ogrConstants.OGRERR_UNSUPPORTED_OPERATION);
    ogr.UseExceptions();

    // Verify layer-level transaction commit/rollback and rename on GPKG.
    DataSource gpkgTxnDs =
        ogr.GetDriverByName("GPKG").CreateDataSource(VSIMEM_DIR + "/layer_txn.gpkg");
    Layer gpkgTxnLayer = gpkgTxnDs.CreateLayer("t", wgs84, ogrConstants.wkbPoint);
    gpkgTxnLayer.CreateField(new FieldDefn("k", ogrConstants.OFTInteger));
    gpkgTxnLayer.SyncToDisk();
    assertThat(gpkgTxnLayer.StartTransaction()).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(gpkgTxnLayer.RollbackTransaction()).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(gpkgTxnLayer.Rename("t_renamed")).isEqualTo(ogrConstants.OGRERR_NONE);
    gpkgTxnDs.delete();

    assertThat(layer.SyncToDisk()).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(layer.DeleteField(1)).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(layer.DeleteFeature(f2.GetFID())).isEqualTo(ogrConstants.OGRERR_NONE);

    // Overlay operations (2, 3, and 4 arg overloads).
    Layer methodLayer = ds.CreateLayer("method", wgs84, ogrConstants.wkbPolygon);
    Feature mf = new Feature(methodLayer.GetLayerDefn());
    mf.SetGeometryDirectly(ogr.CreateGeometryFromWkt("POLYGON ((5 5, 5 15, 15 15, 15 5, 5 5))"));
    methodLayer.CreateFeature(mf);

    CountingProgressCallback cb = new CountingProgressCallback();
    Vector<String> opts = new Vector<>();
    opts.add("SKIP_FAILURES=YES");

    Layer outInter = ds.CreateLayer("out_inter", wgs84, ogrConstants.wkbPolygon);
    assertThat(layer.Intersection(methodLayer, outInter)).isEqualTo(ogrConstants.OGRERR_NONE);
    Layer outInter3 = ds.CreateLayer("out_inter3", wgs84, ogrConstants.wkbPolygon);
    assertThat(layer.Intersection(methodLayer, outInter3, opts))
        .isEqualTo(ogrConstants.OGRERR_NONE);
    Layer outInter4 = ds.CreateLayer("out_inter4", wgs84, ogrConstants.wkbPolygon);
    assertThat(layer.Intersection(methodLayer, outInter4, opts, cb))
        .isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(outInter4.GetFeatureCount()).isEqualTo(1L);

    Layer outUnion = ds.CreateLayer("out_union", wgs84, ogrConstants.wkbPolygon);
    assertThat(layer.Union(methodLayer, outUnion)).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(layer.Union(methodLayer, ds.CreateLayer("u3", wgs84, ogrConstants.wkbPolygon), opts))
        .isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(
            layer.Union(
                methodLayer, ds.CreateLayer("u4", wgs84, ogrConstants.wkbPolygon), opts, cb))
        .isEqualTo(ogrConstants.OGRERR_NONE);

    assertThat(
            layer.SymDifference(methodLayer, ds.CreateLayer("sd2", wgs84, ogrConstants.wkbPolygon)))
        .isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(
            layer.SymDifference(
                methodLayer, ds.CreateLayer("sd3", wgs84, ogrConstants.wkbPolygon), opts))
        .isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(
            layer.SymDifference(
                methodLayer, ds.CreateLayer("sd4", wgs84, ogrConstants.wkbPolygon), opts, cb))
        .isEqualTo(ogrConstants.OGRERR_NONE);

    assertThat(layer.Identity(methodLayer, ds.CreateLayer("id2", wgs84, ogrConstants.wkbPolygon)))
        .isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(
            layer.Identity(
                methodLayer, ds.CreateLayer("id3", wgs84, ogrConstants.wkbPolygon), opts))
        .isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(
            layer.Identity(
                methodLayer, ds.CreateLayer("id4", wgs84, ogrConstants.wkbPolygon), opts, cb))
        .isEqualTo(ogrConstants.OGRERR_NONE);

    assertThat(layer.Update(methodLayer, ds.CreateLayer("up2", wgs84, ogrConstants.wkbPolygon)))
        .isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(
            layer.Update(methodLayer, ds.CreateLayer("up3", wgs84, ogrConstants.wkbPolygon), opts))
        .isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(
            layer.Update(
                methodLayer, ds.CreateLayer("up4", wgs84, ogrConstants.wkbPolygon), opts, cb))
        .isEqualTo(ogrConstants.OGRERR_NONE);

    assertThat(layer.Clip(methodLayer, ds.CreateLayer("cl2", wgs84, ogrConstants.wkbPolygon)))
        .isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(layer.Clip(methodLayer, ds.CreateLayer("cl3", wgs84, ogrConstants.wkbPolygon), opts))
        .isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(
            layer.Clip(
                methodLayer, ds.CreateLayer("cl4", wgs84, ogrConstants.wkbPolygon), opts, cb))
        .isEqualTo(ogrConstants.OGRERR_NONE);

    assertThat(layer.Erase(methodLayer, ds.CreateLayer("er2", wgs84, ogrConstants.wkbPolygon)))
        .isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(
            layer.Erase(methodLayer, ds.CreateLayer("er3", wgs84, ogrConstants.wkbPolygon), opts))
        .isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(
            layer.Erase(
                methodLayer, ds.CreateLayer("er4", wgs84, ogrConstants.wkbPolygon), opts, cb))
        .isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(cb.count).isGreaterThan(0);

    cb.delete();
    mf.delete();
    lStyles.delete();
    rect.delete();
    f2.delete();
    f1.delete();
    gfAlt.delete();
    bNew.delete();
    wgs84.delete();
    ds.delete();
  }
}
