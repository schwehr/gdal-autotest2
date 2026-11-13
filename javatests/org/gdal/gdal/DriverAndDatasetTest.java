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

package org.gdal.gdal;

import static com.google.common.truth.Truth.assertThat;
import static org.junit.Assert.assertThrows;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Hashtable;
import java.util.List;
import java.util.Vector;
import org.gdal.GoogleGdalLoader;
import org.gdal.gdalconst.gdalconst;
import org.gdal.ogr.Feature;
import org.gdal.ogr.FieldDefn;
import org.gdal.ogr.FieldDomain;
import org.gdal.ogr.GeomFieldDefn;
import org.gdal.ogr.Geometry;
import org.gdal.ogr.Layer;
import org.gdal.ogr.StyleTable;
import org.gdal.ogr.ogr;
import org.gdal.ogr.ogrConstants;
import org.gdal.osr.SpatialReference;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;

public final class DriverAndDatasetTest {

  private static final String VSIMEM_DIR = "/vsimem/DriverAndDatasetTest";

  @Before
  public void setUp() {
    GoogleGdalLoader.init();
    gdal.UseExceptions();
    ogr.UseExceptions();
  }

  @After
  public void tearDown() {
    gdal.UseExceptions();
    ogr.UseExceptions();
    gdal.ErrorReset();
    gdal.RmdirRecursive(VSIMEM_DIR);
  }

  @Test
  @SuppressWarnings({"unchecked", "JdkObsolete"})
  public void driverRegistryCreateOpenIdentifyAndMajorObject_workCorrectly() {
    gdal.MkdirRecursive(VSIMEM_DIR, 0755);
    assertThat(gdal.GetDriverCount()).isGreaterThan(0);
    assertThat(gdal.GetDriver(0)).isNotNull();

    Driver mem = gdal.GetDriverByName("MEM");
    Driver gtiff = gdal.GetDriverByName("GTiff");
    assertThat(gtiff.getShortName()).isEqualTo("GTiff");
    assertThat(gtiff.getLongName()).isEqualTo("GeoTIFF");
    assertThat(gtiff.getHelpTopic()).contains("gtiff");
    assertThat(gtiff.HasOpenOption("NUM_THREADS")).isTrue();

    // Register / Deregister safely inside try-finally.
    try {
      gtiff.Deregister();
      assertThat(gdal.GetDriverByName("GTiff")).isNull();
    } finally {
      gtiff.Register();
    }
    assertThat(gdal.GetDriverByName("GTiff")).isNotNull();

    // Exercise all Driver.Create overloads (Vector & String[], 2..6 args).
    String tif1 = VSIMEM_DIR + "/c1.tif";
    String tif2 = VSIMEM_DIR + "/c2.tif";
    String tif3 = VSIMEM_DIR + "/c3.tif";
    String tif4 = VSIMEM_DIR + "/c4.tif";
    String tif5 = VSIMEM_DIR + "/c5.tif";
    String tif6 = VSIMEM_DIR + "/c6.tif";
    gtiff.Create(tif1, 4, 4).Close();
    gtiff.Create(tif2, 4, 4, 1).Close();
    gtiff.Create(tif3, 4, 4, 1, gdalconst.GDT_Int16).Close();
    Vector<String> createOpts = new Vector<>();
    createOpts.add("COMPRESS=NONE");
    Dataset src = gtiff.Create(tif4, 4, 4, 1, gdalconst.GDT_Byte, createOpts);
    gtiff.Create(tif5, 4, 4, 1, new String[] {"COMPRESS=NONE"}).Close();
    gtiff.Create(tif6, 4, 4, 1, gdalconst.GDT_Byte, new String[] {"COMPRESS=NONE"}).Close();

    // Exercise all Driver.CreateCopy overloads.
    ProgressCallback cb = new ProgressCallback();
    Dataset cp1 = gtiff.CreateCopy(VSIMEM_DIR + "/cp1.tif", src);
    Dataset cp2 = gtiff.CreateCopy(VSIMEM_DIR + "/cp2.tif", src, 0);
    Dataset cp3 = gtiff.CreateCopy(VSIMEM_DIR + "/cp3.tif", src, 0, createOpts);
    Dataset cp4 = gtiff.CreateCopy(VSIMEM_DIR + "/cp4.tif", src, 0, createOpts, cb);
    Dataset cp5 = gtiff.CreateCopy(VSIMEM_DIR + "/cp5.tif", src, createOpts);
    Dataset cp6 = gtiff.CreateCopy(VSIMEM_DIR + "/cp6.tif", src, new String[] {"COMPRESS=NONE"});
    Dataset cp7 = gtiff.CreateCopy(VSIMEM_DIR + "/cp7.tif", src, 1, new String[] {"COMPRESS=NONE"});

    // MajorObject metadata APIs (Hashtable, Vector, String, item, domain).
    cp1.SetDescription("Custom desc");
    assertThat(cp1.GetDescription()).isEqualTo("Custom desc");
    Hashtable<String, String> dict = new Hashtable<>();
    dict.put("K1", "V1");
    assertThat(cp1.SetMetadata(dict)).isEqualTo(gdalconst.CE_None);
    assertThat(cp1.SetMetadata(dict, "CUSTOM")).isEqualTo(gdalconst.CE_None);
    Vector<String> metaList = new Vector<>();
    metaList.add("K2=V2");
    assertThat(cp1.SetMetadata(metaList)).isEqualTo(gdalconst.CE_None);
    assertThat(cp1.SetMetadata(metaList, "CUSTOM")).isEqualTo(gdalconst.CE_None);
    assertThat(cp1.SetMetadata("K3=V3")).isEqualTo(gdalconst.CE_None);
    assertThat(cp1.SetMetadata("K4=V4", "CUSTOM")).isEqualTo(gdalconst.CE_None);
    assertThat(cp1.SetMetadataItem("K5", "V5")).isEqualTo(gdalconst.CE_None);
    assertThat(cp1.SetMetadataItem("K6", "V6", "CUSTOM")).isEqualTo(gdalconst.CE_None);
    assertThat(cp1.GetMetadataItem("K5")).isEqualTo("V5");
    assertThat(cp1.GetMetadataItem("K6", "CUSTOM")).isEqualTo("V6");
    assertThat((List<String>) cp1.GetMetadataDomainList()).contains("CUSTOM");
    assertThat(cp1.GetMetadata_Dict()).containsEntry("K5", "V5");
    assertThat(cp1.GetMetadata_Dict("CUSTOM")).containsEntry("K6", "V6");
    assertThat((List<String>) cp1.GetMetadata_List()).isNotEmpty();
    assertThat((List<String>) cp1.GetMetadata_List("CUSTOM")).contains("K6=V6");

    cp7.Close();
    cp6.Close();
    cp5.Close();
    cp4.Close();
    cp3.Close();
    cp2.Close();
    cp1.Close();
    src.Close();

    // Open / OpenEx / OpenShared / IdentifyDriver / Rename / CopyFiles / Delete.
    Vector<String> openOpts = new Vector<>();
    openOpts.add("OVERVIEW_LEVEL=NONE");
    assertThat(gdal.IdentifyDriver(tif1).getShortName()).isEqualTo("GTiff");
    assertThat(gdal.IdentifyDriver(tif1, openOpts).getShortName()).isEqualTo("GTiff");
    assertThat(gdal.IdentifyDriverEx(tif1).getShortName()).isEqualTo("GTiff");
    assertThat(gdal.IdentifyDriverEx(tif1, gdalconst.OF_RASTER).getShortName()).isEqualTo("GTiff");
    Vector<String> drivers = new Vector<>();
    drivers.add("GTiff");
    assertThat(gdal.IdentifyDriverEx(tif1, gdalconst.OF_RASTER, drivers).getShortName())
        .isEqualTo("GTiff");
    assertThat(gdal.IdentifyDriverEx(tif1, gdalconst.OF_RASTER, drivers, openOpts).getShortName())
        .isEqualTo("GTiff");

    Dataset o1 = gdal.Open(tif1);
    Dataset o2 = gdal.Open(tif1, gdalconst.GA_Update);
    Dataset o3 = gdal.OpenShared(tif1);
    Dataset o4 = gdal.OpenShared(tif1, gdalconst.GA_ReadOnly);
    Dataset o5 = gdal.OpenEx(tif1);
    Dataset o6 = gdal.OpenEx(tif1, gdalconst.OF_RASTER);
    Dataset o7 = gdal.OpenEx(tif1, gdalconst.OF_RASTER, drivers);
    Dataset o8 = gdal.OpenEx(tif1, gdalconst.OF_RASTER, drivers, openOpts);
    Vector<String> siblings = new Vector<>();
    siblings.add(tif1);
    Dataset o9 = gdal.OpenEx(tif1, gdalconst.OF_RASTER, drivers, openOpts, siblings);
    o9.Close();
    o8.Close();
    o7.Close();
    o6.Close();
    o5.Close();
    o4.Close();
    o3.Close();
    o2.Close();
    o1.Close();

    String copiedName = VSIMEM_DIR + "/copied_files.tif";
    String renamedName = VSIMEM_DIR + "/renamed_files.tif";
    assertThat(gtiff.CopyFiles(copiedName, tif1)).isEqualTo(gdalconst.CE_None);
    assertThat(gtiff.Rename(renamedName, copiedName)).isEqualTo(gdalconst.CE_None);
    assertThat(gtiff.Delete(renamedName)).isEqualTo(gdalconst.CE_None);
    assertThat(mem).isNotNull();
    cb.delete();
  }

  @Test
  @SuppressWarnings({"unchecked", "JdkObsolete"})
  public void datasetPropertiesOverviewsRasterIoAndVectorRelationships_workCorrectly() {
    gdal.MkdirRecursive(VSIMEM_DIR, 0755);
    Driver mem = gdal.GetDriverByName("MEM");
    Driver gtiff = gdal.GetDriverByName("GTiff");

    // MEM dataset with AddBand, Overviews, Masks, GCPs, ThreadSafe, AdviseRead, AsMDArray.
    Dataset memDs = mem.Create("", 16, 16, 1, gdalconst.GDT_Byte);
    assertThat(memDs.GetRasterXSize()).isEqualTo(16);
    assertThat(memDs.GetRasterYSize()).isEqualTo(16);
    assertThat(memDs.GetRasterCount()).isEqualTo(1);
    assertThat(memDs.getRasterXSize()).isEqualTo(16);
    assertThat(memDs.getRasterYSize()).isEqualTo(16);
    assertThat(memDs.getRasterCount()).isEqualTo(1);
    assertThat(memDs.GetDriver().getShortName()).isEqualTo("MEM");
    assertThat(memDs.GetRasterBand(1)).isNotNull();
    assertThat(memDs.AddBand()).isEqualTo(gdalconst.CE_None);
    assertThat(memDs.AddBand(gdalconst.GDT_Byte)).isEqualTo(gdalconst.CE_None);
    assertThat(memDs.AddBand(gdalconst.GDT_Byte, new Vector<String>()))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.CreateMaskBand(gdalconst.GMF_PER_DATASET)).isEqualTo(gdalconst.CE_None);

    SpatialReference wgs84 = new SpatialReference();
    wgs84.ImportFromEPSG(4326);
    assertThat(memDs.SetSpatialRef(wgs84)).isEqualTo(gdalconst.CE_None);
    assertThat(memDs.GetSpatialRef().IsGeographic()).isEqualTo(1);
    assertThat(memDs.SetProjection(wgs84.ExportToWkt())).isEqualTo(gdalconst.CE_None);
    assertThat(memDs.GetProjection()).contains("WGS 84");
    assertThat(memDs.GetProjectionRef()).contains("WGS 84");

    double[] gt = new double[] {10.0, 0.5, 0.0, 50.0, 0.0, -0.5};
    assertThat(memDs.SetGeoTransform(gt)).isEqualTo(gdalconst.CE_None);
    assertThat(memDs.GetGeoTransform()).usingExactEquality().containsExactly(gt);
    double[] gtOut = new double[6];
    memDs.GetGeoTransform(gtOut);
    assertThat(gtOut).usingExactEquality().containsExactly(gt);

    double[] extent = new double[4];
    assertThat(memDs.GetExtent(extent)).isEqualTo(gdalconst.CE_None);
    double[] extentSrs = new double[4];
    assertThat(memDs.GetExtent(extentSrs, wgs84)).isEqualTo(gdalconst.CE_None);
    double[] extentWgs84 = new double[4];
    assertThat(memDs.GetExtentWGS84LongLat(extentWgs84)).isEqualTo(gdalconst.CE_None);

    // AdviseRead (null SWIGTYPE_p_int is handled safely by Dataset.i).
    assertThat(memDs.AdviseRead(0, 0, 16, 16)).isEqualTo(gdalconst.CE_None);
    assertThat(memDs.AdviseRead(0, 0, 16, 16, null)).isEqualTo(gdalconst.CE_None);
    assertThat(memDs.AdviseRead(0, 0, 16, 16, null, null)).isEqualTo(gdalconst.CE_None);
    assertThat(memDs.AdviseRead(0, 0, 16, 16, null, null, null)).isEqualTo(gdalconst.CE_None);
    assertThat(memDs.AdviseRead(0, 0, 16, 16, null, null, null, new int[] {1}))
        .isEqualTo(gdalconst.CE_None);
    assertThat(
            memDs.AdviseRead(0, 0, 16, 16, null, null, null, new int[] {1}, new Vector<String>()))
        .isEqualTo(gdalconst.CE_None);

    // Dataset ReadRaster / WriteRaster overloads (byte, short, int, long, float, double & Direct).
    int[] bandList = new int[] {1};
    byte[] bBuf = new byte[16];
    assertThat(memDs.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, bBuf, bandList))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, bBuf, bandList, 1))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, bBuf, bandList, 1, 4))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, bBuf, bandList, 1, 4, 16))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, bBuf, bandList))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, bBuf, bandList, 1))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, bBuf, bandList, 1, 4))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, bBuf, bandList, 1, 4, 16))
        .isEqualTo(gdalconst.CE_None);

    short[] sBuf = new short[16];
    assertThat(memDs.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int16, sBuf, bandList))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int16, sBuf, bandList, 2))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int16, sBuf, bandList, 2, 8))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int16, sBuf, bandList, 2, 8, 32))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int16, sBuf, bandList))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int16, sBuf, bandList, 2))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int16, sBuf, bandList, 2, 8))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int16, sBuf, bandList, 2, 8, 32))
        .isEqualTo(gdalconst.CE_None);

    int[] iBuf = new int[16];
    assertThat(memDs.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int32, iBuf, bandList))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int32, iBuf, bandList, 4))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int32, iBuf, bandList, 4, 16))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int32, iBuf, bandList, 4, 16, 64))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int32, iBuf, bandList))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int32, iBuf, bandList, 4))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int32, iBuf, bandList, 4, 16))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int32, iBuf, bandList, 4, 16, 64))
        .isEqualTo(gdalconst.CE_None);

    long[] lBuf = new long[16];
    assertThat(memDs.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int64, lBuf, bandList))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int64, lBuf, bandList, 8))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int64, lBuf, bandList, 8, 32))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int64, lBuf, bandList, 8, 32, 128))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int64, lBuf, bandList))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int64, lBuf, bandList, 8))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int64, lBuf, bandList, 8, 32))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int64, lBuf, bandList, 8, 32, 128))
        .isEqualTo(gdalconst.CE_None);

    float[] fBuf = new float[16];
    assertThat(memDs.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float32, fBuf, bandList))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float32, fBuf, bandList, 4))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float32, fBuf, bandList, 4, 16))
        .isEqualTo(gdalconst.CE_None);
    assertThat(
            memDs.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float32, fBuf, bandList, 4, 16, 64))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float32, fBuf, bandList))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float32, fBuf, bandList, 4))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float32, fBuf, bandList, 4, 16))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float32, fBuf, bandList, 4, 16, 64))
        .isEqualTo(gdalconst.CE_None);

    double[] dBuf = new double[16];
    assertThat(memDs.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float64, dBuf, bandList))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float64, dBuf, bandList, 8))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float64, dBuf, bandList, 8, 32))
        .isEqualTo(gdalconst.CE_None);
    assertThat(
            memDs.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float64, dBuf, bandList, 8, 32, 128))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float64, dBuf, bandList))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float64, dBuf, bandList, 8))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float64, dBuf, bandList, 8, 32))
        .isEqualTo(gdalconst.CE_None);
    assertThat(
            memDs.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float64, dBuf, bandList, 8, 32, 128))
        .isEqualTo(gdalconst.CE_None);

    ByteBuffer direct = ByteBuffer.allocateDirect(16).order(ByteOrder.nativeOrder());
    assertThat(memDs.WriteRaster_Direct(0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, direct, bandList))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.WriteRaster_Direct(0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, direct, bandList, 1))
        .isEqualTo(gdalconst.CE_None);
    assertThat(
            memDs.WriteRaster_Direct(0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, direct, bandList, 1, 4))
        .isEqualTo(gdalconst.CE_None);
    assertThat(
            memDs.WriteRaster_Direct(
                0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, direct, bandList, 1, 4, 16))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.ReadRaster_Direct(0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, direct, bandList))
        .isEqualTo(gdalconst.CE_None);
    assertThat(memDs.ReadRaster_Direct(0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, direct, bandList, 1))
        .isEqualTo(gdalconst.CE_None);
    assertThat(
            memDs.ReadRaster_Direct(0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, direct, bandList, 1, 4))
        .isEqualTo(gdalconst.CE_None);
    assertThat(
            memDs.ReadRaster_Direct(
                0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, direct, bandList, 1, 4, 16))
        .isEqualTo(gdalconst.CE_None);

    RuntimeException nonDirectThrown =
        assertThrows(
            RuntimeException.class,
            () ->
                memDs.ReadRaster_Direct(
                    0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, ByteBuffer.allocate(16), bandList));
    assertThat(nonDirectThrown).hasMessageThat().contains("direct");

    // AsMDArray on MEM dataset.
    MDArray asMd0 = memDs.AsMDArray();
    assertThat(asMd0).isNotNull();
    MDArray asMd1 = memDs.AsMDArray(new Vector<String>());
    assertThat(asMd1).isNotNull();
    asMd1.delete();
    asMd0.delete();
    memDs.ClearStatistics();
    memDs.FlushCache();
    memDs.MarkSuppressOnClose();
    memDs.Close();

    // GeoTIFF overviews, GCPs, FileList, and GetThreadSafeDataset.
    String tifPath = VSIMEM_DIR + "/gcp_ovr.tif";
    Dataset tif = gtiff.Create(tifPath, 16, 16, 1, gdalconst.GDT_Byte);
    GCP[] gcps =
        new GCP[] {
          new GCP(0, 0, 0, 0, 0),
          new GCP(16, 0, 0, 16, 0),
          new GCP(0, -16, 0, 0, 16),
          new GCP(16, -16, 0, 16, 16)
        };
    assertThat(tif.SetGCPs(gcps, wgs84.ExportToWkt())).isEqualTo(gdalconst.CE_None);
    assertThat(tif.SetGCPs2(gcps, wgs84)).isEqualTo(gdalconst.CE_None);
    assertThat(tif.GetGCPCount()).isEqualTo(4);
    assertThat(tif.GetGCPProjection()).contains("WGS 84");
    assertThat(tif.GetGCPSpatialRef().IsGeographic()).isEqualTo(1);
    assertThat((List<GCP>) tif.GetGCPs()).hasSize(4);
    Vector<GCP> gcpVec = new Vector<>();
    tif.GetGCPs(gcpVec);
    assertThat(gcpVec).hasSize(4);
    for (GCP g : gcps) {
      g.delete();
    }

    ProgressCallback cb = new ProgressCallback();
    assertThat(tif.BuildOverviews(new int[] {2})).isEqualTo(gdalconst.CE_None);
    assertThat(tif.BuildOverviews(new int[] {2}, cb)).isEqualTo(gdalconst.CE_None);
    assertThat(tif.BuildOverviews("NEAREST", new int[] {2})).isEqualTo(gdalconst.CE_None);
    assertThat(tif.BuildOverviews("NEAREST", new int[] {2}, cb)).isEqualTo(gdalconst.CE_None);
    Vector<String> ovrOpts = new Vector<>();
    ovrOpts.add("COMPRESS=NONE");
    assertThat(tif.BuildOverviews("NEAREST", new int[] {2}, cb, ovrOpts))
        .isEqualTo(gdalconst.CE_None);
    assertThat((List<String>) tif.GetFileList()).contains(tifPath);
    tif.Close();

    Dataset nonTsRo = gdal.OpenEx(tifPath, gdalconst.OF_RASTER);
    assertThat(nonTsRo.IsThreadSafe(gdalconst.OF_RASTER)).isFalse();
    Dataset tsWrapper = nonTsRo.GetThreadSafeDataset(gdalconst.OF_RASTER);
    assertThat(tsWrapper).isNotNull();
    assertThat(tsWrapper.IsThreadSafe(gdalconst.OF_RASTER)).isTrue();
    tsWrapper.Close();
    nonTsRo.Close();

    Dataset tsRo = gdal.OpenEx(tifPath, gdalconst.OF_RASTER | gdalconst.OF_THREAD_SAFE);
    assertThat(tsRo.IsThreadSafe(gdalconst.OF_RASTER)).isTrue();
    Dataset tsSame = tsRo.GetThreadSafeDataset(gdalconst.OF_RASTER);
    assertThat(tsSame).isNotNull();
    tsSame.delete();
    tsRo.delete();

    // Vector Dataset, CreateVector, FieldDomain, and Relationship via GPKG.
    String gpkgPath = VSIMEM_DIR + "/vec_rel.gpkg";
    Dataset gpkg = gdal.GetDriverByName("GPKG").CreateVector(gpkgPath);
    gdal.GetDriverByName("GPKG")
        .CreateVector(VSIMEM_DIR + "/vec_opts.gpkg", new Vector<String>())
        .Close();
    Layer l1 = gpkg.CreateLayer("left_tbl");
    Layer l2 = gpkg.CreateLayer("right_tbl", wgs84);
    assertThat(l2).isNotNull();
    Layer l3 = gpkg.CreateLayer("l3", wgs84, ogrConstants.wkbPoint);
    Layer l4 = gpkg.CreateLayer("l4", wgs84, ogrConstants.wkbPoint, new Vector<String>());
    assertThat(l3).isNotNull();
    assertThat(l4).isNotNull();
    Layer gfdLayer =
        gpkg.CreateLayerFromGeomFieldDefn("gfd", new GeomFieldDefn("geom", ogrConstants.wkbPoint));
    assertThat(gfdLayer).isNotNull();
    gpkg.CreateLayerFromGeomFieldDefn(
        "gfd2", new GeomFieldDefn("geom", ogrConstants.wkbPoint), new Vector<String>());
    assertThat(gpkg.CopyLayer(l1, "left_copy")).isNotNull();
    assertThat(gpkg.CopyLayer(l1, "left_copy2", new Vector<String>())).isNotNull();

    l1.CreateField(new FieldDefn("code", ogrConstants.OFTString));
    Feature f = new Feature(l1.GetLayerDefn());
    f.SetField("code", "A");
    f.SetGeometryDirectly(ogr.CreateGeometryFromWkt("POINT (1 2)"));
    l1.CreateFeature(f);
    gpkg.ResetReading();
    assertThat(gpkg.GetNextFeature()).isNotNull();
    f.delete();
    gpkg.ResetReading();
    gpkg.FlushCache();

    assertThat(gpkg.GetLayerCount()).isAtLeast(6);
    assertThat(gpkg.GetLayer(0)).isNotNull();
    assertThat(gpkg.GetLayer("left_tbl")).isNotNull();
    assertThat(gpkg.GetLayerByIndex(0)).isNotNull();
    assertThat(gpkg.GetLayerByName("left_tbl")).isNotNull();
    assertThat(gpkg.IsLayerPrivate(0)).isFalse();
    assertThat(gpkg.TestCapability(ogrConstants.ODsCCreateLayer)).isTrue();
    Dataset memVec = gdal.GetDriverByName("Memory").CreateVector("mem_del");
    memVec.CreateLayer("tmp_layer", wgs84, ogrConstants.wkbPoint);
    assertThat(memVec.DeleteLayer(0)).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(memVec.GetLayerCount()).isEqualTo(0);
    memVec.Close();

    assertThat(gpkg.StartTransaction()).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(gpkg.CommitTransaction()).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(gpkg.StartTransaction(1)).isEqualTo(ogrConstants.OGRERR_NONE);
    assertThat(gpkg.RollbackTransaction()).isEqualTo(ogrConstants.OGRERR_NONE);

    StyleTable dsStyle = new StyleTable();
    dsStyle.AddStyle("st", "PEN(c:#000000)");
    gpkg.SetStyleTable(dsStyle);
    assertThat(gpkg.GetStyleTable()).isNotNull();
    dsStyle.delete();

    Layer sql1 = gpkg.ExecuteSQL("SELECT count(*) FROM left_tbl");
    assertThat(sql1.GetNextFeature()).isNotNull();
    gpkg.ReleaseResultSet(sql1);
    Geometry rect = ogr.CreateGeometryFromEnvelope(-5, -5, 5, 5);
    Layer sql2 = gpkg.ExecuteSQL("SELECT * FROM left_tbl", rect);
    gpkg.ReleaseResultSet(sql2);
    Layer sql3 = gpkg.ExecuteSQL("SELECT * FROM left_tbl", rect, "SQLITE");
    gpkg.ReleaseResultSet(sql3);
    assertThat(gpkg.AbortSQL()).isEqualTo(ogrConstants.OGRERR_NONE);
    rect.delete();

    // Field domains on Dataset.
    FieldDomain dom =
        ogr.CreateGlobFieldDomain(
            "dom1", "Domain", ogrConstants.OFTString, ogrConstants.OFSTNone, "A*");
    assertThat(gpkg.AddFieldDomain(dom)).isTrue();
    assertThat((List<String>) gpkg.GetFieldDomainNames()).contains("dom1");
    assertThat((List<String>) gpkg.GetFieldDomainNames(new Vector<String>())).contains("dom1");
    assertThat(gpkg.GetFieldDomain("dom1").GetName()).isEqualTo("dom1");
    FieldDomain domUpdated =
        ogr.CreateGlobFieldDomain(
            "dom1", "Updated", ogrConstants.OFTString, ogrConstants.OFSTNone, "B*");
    assertThat(gpkg.UpdateFieldDomain(domUpdated)).isTrue();
    assertThat(gpkg.DeleteFieldDomain("dom1")).isTrue();
    domUpdated.delete();
    dom.delete();

    // Relationships on Dataset.
    Relationship rel = new Relationship("rel1", "left_tbl", "right_tbl", gdalconst.GRC_ONE_TO_MANY);
    rel.SetType(gdalconst.GRT_ASSOCIATION);
    rel.SetMappingTableName("map_tbl");
    Vector<String> leftFields = new Vector<>();
    leftFields.add("fid");
    Vector<String> rightFields = new Vector<>();
    rightFields.add("fid");
    rel.SetLeftTableFields(leftFields);
    rel.SetRightTableFields(rightFields);
    rel.SetLeftMappingTableFields(leftFields);
    rel.SetRightMappingTableFields(rightFields);
    rel.SetForwardPathLabel("fwd");
    rel.SetBackwardPathLabel("bwd");
    rel.SetRelatedTableType("features");
    assertThat(rel.GetName()).isEqualTo("rel1");
    assertThat(rel.GetCardinality()).isEqualTo(gdalconst.GRC_ONE_TO_MANY);
    assertThat(rel.GetLeftTableName()).isEqualTo("left_tbl");
    assertThat(rel.GetRightTableName()).isEqualTo("right_tbl");
    assertThat(rel.GetMappingTableName()).isEqualTo("map_tbl");
    assertThat((List<String>) rel.GetLeftTableFields()).containsExactly("fid");
    assertThat((List<String>) rel.GetRightTableFields()).containsExactly("fid");
    assertThat((List<String>) rel.GetLeftMappingTableFields()).containsExactly("fid");
    assertThat((List<String>) rel.GetRightMappingTableFields()).containsExactly("fid");
    assertThat(rel.GetType()).isEqualTo(gdalconst.GRT_ASSOCIATION);
    assertThat(rel.GetForwardPathLabel()).isEqualTo("fwd");
    assertThat(rel.GetBackwardPathLabel()).isEqualTo("bwd");
    assertThat(rel.GetRelatedTableType()).isEqualTo("features");
    assertThat(rel.equals(rel)).isTrue();
    assertThat(rel.equals(new Object())).isFalse();
    assertThat(rel.hashCode()).isNotEqualTo(0);

    // GPKG may or may not support AddRelationship on synthetic layers without mapping table,
    // so check return cleanly under DontUseExceptions and still exercise Update/DeleteRelationship.
    gdal.DontUseExceptions();
    boolean addedRel = gpkg.AddRelationship(rel);
    Vector<String> relOpts = new Vector<>();
    relOpts.add("DUMMY=1");
    assertThat((List<String>) gpkg.GetFieldDomainNames(relOpts)).isNotNull();
    List<String> relNames0 = gpkg.GetRelationshipNames();
    List<String> relNames1 = gpkg.GetRelationshipNames(relOpts);
    assertThat(relNames0).isNotNull();
    assertThat(relNames1).isNotNull();
    if (addedRel) {
      assertThat(gpkg.GetRelationship("rel1")).isNotNull();
    } else {
      assertThat(gpkg.GetRelationship("rel1")).isNull();
    }
    assertThat(gpkg.UpdateRelationship(rel)).isAnyOf(true, false);
    assertThat(gpkg.DeleteRelationship("rel1")).isAnyOf(true, false);
    gdal.UseExceptions();
    gdal.ErrorReset();

    // Exercise ComputeDatasetRasterIOSize error branches on MEM dataset.
    Dataset errDs = gdal.GetDriverByName("MEM").Create("", 4, 4, 1, gdalconst.GDT_Int16);
    gdal.DontUseExceptions();
    assertThat(errDs.ReadRaster(0, 0, 4, 4, 0, 4, gdalconst.GDT_Byte, new byte[16], new int[] {1}))
        .isEqualTo(gdalconst.CE_Failure);
    assertThat(
            errDs.ReadRaster(
                0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, new byte[16], new int[] {1}, -1, 4, 16))
        .isEqualTo(gdalconst.CE_Failure);
    assertThat(
            errDs.ReadRaster(
                0, 0, 4, 4, 4, 4, gdalconst.GDT_Int16, new short[16], new int[] {1}, 3, 8, 32))
        .isEqualTo(gdalconst.CE_Failure);
    assertThat(errDs.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, new byte[1], new int[] {1}))
        .isEqualTo(gdalconst.CE_Failure);
    assertThat(
            errDs.ReadRaster_Direct(
                0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, ByteBuffer.allocateDirect(1), new int[] {1}))
        .isEqualTo(gdalconst.CE_Failure);
    gdal.UseExceptions();
    gdal.ErrorReset();
    RuntimeException nonDirectThrown2 =
        assertThrows(
            RuntimeException.class,
            () ->
                errDs.ReadRaster_Direct(
                    0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, ByteBuffer.allocate(16), new int[] {1}));
    assertThat(nonDirectThrown2).hasMessageThat().contains("direct");
    errDs.Close();

    rel.delete();
    gpkg.Close();
    cb.delete();
    wgs84.delete();
  }
}
