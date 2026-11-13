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

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Vector;
import org.gdal.GoogleGdalLoader;
import org.gdal.gdalconst.gdalconst;
import org.gdal.ogr.DataSource;
import org.gdal.ogr.Feature;
import org.gdal.ogr.FieldDefn;
import org.gdal.ogr.Layer;
import org.gdal.ogr.ogr;
import org.gdal.ogr.ogrConstants;
import org.gdal.osr.SpatialReference;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;

public final class OperationsAndUtilitiesTest {

  private static final String VSIMEM_DIR = "/vsimem/OperationsAndUtilitiesTest";

  private static final class RecordingCallback extends ProgressCallback {
    int calls;

    @Override
    public int run(double dfComplete, String pszMessage) {
      calls++;
      return 1;
    }
  }

  @Before
  public void setUp() {
    GoogleGdalLoader.init();
    gdal.UseExceptions();
    ogr.UseExceptions();
  }

  @After
  public void tearDown() {
    gdal.SetConfigOption("CPL_TMPDIR", null);
    gdal.UseExceptions();
    ogr.UseExceptions();
    gdal.ErrorReset();
    gdal.RmdirRecursive(VSIMEM_DIR);
  }

  @Test
  @SuppressWarnings({"unchecked", "JdkObsolete"})
  public void rasterAndVectorAlgorithms_runSuccessfullyInVsimem() {
    gdal.MkdirRecursive(VSIMEM_DIR, 0755);
    gdal.SetConfigOption("CPL_TMPDIR", VSIMEM_DIR);
    Driver mem = gdal.GetDriverByName("MEM");
    Driver gtiff = gdal.GetDriverByName("GTiff");
    RecordingCallback cb = new RecordingCallback();
    ProgressCallback baseCb = new ProgressCallback();
    assertThat(baseCb.run(0.5, "base")).isEqualTo(1);
    TermProgressCallback termCb = new TermProgressCallback();
    assertThat(termCb.run(1.0, "term")).isEqualTo(1);

    SpatialReference wgs84 = new SpatialReference();
    wgs84.ImportFromEPSG(4326);
    String wkt = wgs84.ExportToWkt();
    SpatialReference merc = new SpatialReference();
    merc.ImportFromEPSG(3857);
    String mercWkt = merc.ExportToWkt();

    // ComputeMedianCutPCT & DitherRGB2PCT.
    Dataset rgb = mem.Create("", 8, 8, 3, gdalconst.GDT_Byte);
    rgb.GetRasterBand(1).Fill(100);
    rgb.GetRasterBand(2).Fill(150);
    rgb.GetRasterBand(3).Fill(200);
    Dataset pct = mem.Create("", 8, 8, 1, gdalconst.GDT_Byte);
    ColorTable ct = new ColorTable();
    assertThat(
            gdal.ComputeMedianCutPCT(
                rgb.GetRasterBand(1), rgb.GetRasterBand(2), rgb.GetRasterBand(3), 16, ct))
        .isEqualTo(gdalconst.CE_None);
    assertThat(
            gdal.ComputeMedianCutPCT(
                rgb.GetRasterBand(1), rgb.GetRasterBand(2), rgb.GetRasterBand(3), 16, ct, cb))
        .isEqualTo(gdalconst.CE_None);
    assertThat(
            gdal.DitherRGB2PCT(
                rgb.GetRasterBand(1),
                rgb.GetRasterBand(2),
                rgb.GetRasterBand(3),
                pct.GetRasterBand(1),
                ct))
        .isEqualTo(gdalconst.CE_None);
    assertThat(
            gdal.DitherRGB2PCT(
                rgb.GetRasterBand(1),
                rgb.GetRasterBand(2),
                rgb.GetRasterBand(3),
                pct.GetRasterBand(1),
                ct,
                cb))
        .isEqualTo(gdalconst.CE_None);
    ct.delete();
    pct.Close();
    rgb.Close();

    // DEM source + GeoTransform.
    Dataset src = gtiff.Create(VSIMEM_DIR + "/src.tif", 8, 8, 1, gdalconst.GDT_Float32);
    src.SetProjection(wkt);
    src.SetGeoTransform(new double[] {0.0, 0.01, 0.0, 1.0, 0.0, -0.01});
    float[] vals = new float[64];
    for (int i = 0; i < 64; i++) {
      vals[i] = 100.0f + (i / 8) * 10.0f + (i % 8);
    }
    src.GetRasterBand(1).WriteRaster(0, 0, 8, 8, vals);

    // AutoCreateWarpedVRT (all 5 overloads) + ReprojectImage (all 8 overloads) + Transformer + SWO.
    assertThat(gdal.GetTranformerOptionList()).contains("SRC_SRS");
    assertThat(gdal.WarpGetOptionList()).contains("INIT_DEST");

    Dataset warped1 = gdal.AutoCreateWarpedVRT(src);
    Dataset warped2 = gdal.AutoCreateWarpedVRT(src, wkt);
    Dataset warped3 = gdal.AutoCreateWarpedVRT(src, wkt, mercWkt);
    Dataset warped4 = gdal.AutoCreateWarpedVRT(src, wkt, mercWkt, gdalconst.GRA_Bilinear);
    Dataset warped5 = gdal.AutoCreateWarpedVRT(src, wkt, mercWkt, gdalconst.GRA_Bilinear, 0.125);
    assertThat(warped5.GetRasterXSize()).isGreaterThan(0);

    Vector<String> txOpts = new Vector<>();
    txOpts.add("SRC_SRS=" + wkt);
    Transformer transformer = new Transformer(src, warped5, txOpts);
    double[] ptInout = new double[] {2.0, 2.0, 0.0};
    assertThat(transformer.TransformPoint(0, ptInout)).isEqualTo(1);
    double[] ptOut = new double[3];
    assertThat(transformer.TransformPoint(ptOut, 0, 2.0, 2.0)).isEqualTo(1);
    assertThat(transformer.TransformPoint(ptOut, 0, 2.0, 2.0, 0.0)).isEqualTo(1);
    int[] panSuccess = new int[2];
    double[][] tPts = new double[][] {{1.0, 1.0, 0.0}, {2.0, 2.0, 0.0}};
    assertThat(transformer.TransformPoints(0, tPts, panSuccess)).isEqualTo(1);
    assertThat(transformer.equals(transformer)).isTrue();
    assertThat(transformer.equals(new Object())).isFalse();
    assertThat(transformer.hashCode()).isNotEqualTo(0);

    Dataset geoBands = mem.Create("", 4, 4, 3, gdalconst.GDT_Float64);
    geoBands.GetRasterBand(1).Fill(0.02);
    geoBands.GetRasterBand(2).Fill(0.98);
    geoBands.GetRasterBand(3).Fill(0.0);
    Vector<String> geoTxOpts = new Vector<>();
    geoTxOpts.add("NUM_THREADS=1");
    assertThat(
            transformer.TransformGeolocations(
                geoBands.GetRasterBand(1), geoBands.GetRasterBand(2), geoBands.GetRasterBand(3)))
        .isEqualTo(gdalconst.CE_None);
    assertThat(
            transformer.TransformGeolocations(
                geoBands.GetRasterBand(1),
                geoBands.GetRasterBand(2),
                geoBands.GetRasterBand(3),
                cb))
        .isEqualTo(gdalconst.CE_None);
    assertThat(
            transformer.TransformGeolocations(
                geoBands.GetRasterBand(1),
                geoBands.GetRasterBand(2),
                geoBands.GetRasterBand(3),
                cb,
                geoTxOpts))
        .isEqualTo(gdalconst.CE_None);
    geoBands.Close();

    SuggestedWarpOutputRes swo1 = gdal.SuggestedWarpOutput(src, transformer);
    assertThat(swo1.getWidth()).isGreaterThan(0);
    assertThat(swo1.getHeight()).isGreaterThan(0);
    assertThat(swo1.getXmax()).isGreaterThan(swo1.getXmin());
    assertThat(swo1.getYmax()).isGreaterThan(swo1.getYmin());
    double[] swoGt = new double[6];
    swo1.GetGeotransform(swoGt);
    assertThat(swoGt[1]).isGreaterThan(0.0);
    assertThat(swo1.equals(swo1)).isTrue();
    assertThat(swo1.equals(new Object())).isFalse();
    assertThat(swo1.hashCode()).isNotEqualTo(0);

    Vector<String> swoOpts = new Vector<>();
    swoOpts.add("DST_SRS=" + mercWkt);
    SuggestedWarpOutputRes swo2 = gdal.SuggestedWarpOutput(src, swoOpts);
    assertThat(swo2.getWidth()).isGreaterThan(0);
    swo2.delete();
    swo1.delete();
    transformer.delete();

    Dataset dstReproj = gtiff.Create(VSIMEM_DIR + "/reproj.tif", 8, 8, 1, gdalconst.GDT_Float32);
    dstReproj.SetProjection(mercWkt);
    dstReproj.SetGeoTransform(swoGt);
    assertThat(gdal.ReprojectImage(src, dstReproj)).isEqualTo(gdalconst.CE_None);
    assertThat(gdal.ReprojectImage(src, dstReproj, wkt)).isEqualTo(gdalconst.CE_None);
    assertThat(gdal.ReprojectImage(src, dstReproj, wkt, mercWkt)).isEqualTo(gdalconst.CE_None);
    assertThat(gdal.ReprojectImage(src, dstReproj, wkt, mercWkt, gdalconst.GRA_Bilinear))
        .isEqualTo(gdalconst.CE_None);
    assertThat(gdal.ReprojectImage(src, dstReproj, wkt, mercWkt, gdalconst.GRA_Bilinear, 0.0))
        .isEqualTo(gdalconst.CE_None);
    assertThat(
            gdal.ReprojectImage(src, dstReproj, wkt, mercWkt, gdalconst.GRA_Bilinear, 0.0, 0.125))
        .isEqualTo(gdalconst.CE_None);
    assertThat(
            gdal.ReprojectImage(
                src, dstReproj, wkt, mercWkt, gdalconst.GRA_Bilinear, 0.0, 0.125, cb))
        .isEqualTo(gdalconst.CE_None);
    Vector<String> reprojOpts = new Vector<>();
    reprojOpts.add("NUM_THREADS=1");
    assertThat(
            gdal.ReprojectImage(
                src, dstReproj, wkt, mercWkt, gdalconst.GRA_Bilinear, 0.0, 0.125, cb, reprojOpts))
        .isEqualTo(gdalconst.CE_None);
    dstReproj.Close();

    // ComputeProximity, FillNodata, SieveFilter, RegenerateOverview/Overviews.
    Dataset proxSrc = mem.Create("", 8, 8, 1, gdalconst.GDT_Byte);
    proxSrc.GetRasterBand(1).WriteRaster(4, 4, 1, 1, new byte[] {1});
    Dataset proxDst = mem.Create("", 8, 8, 1, gdalconst.GDT_Float32);
    Vector<String> proxOpts = new Vector<>();
    proxOpts.add("VALUES=1");
    assertThat(gdal.ComputeProximity(proxSrc.GetRasterBand(1), proxDst.GetRasterBand(1)))
        .isEqualTo(gdalconst.CE_None);
    assertThat(gdal.ComputeProximity(proxSrc.GetRasterBand(1), proxDst.GetRasterBand(1), proxOpts))
        .isEqualTo(gdalconst.CE_None);
    assertThat(
            gdal.ComputeProximity(proxSrc.GetRasterBand(1), proxDst.GetRasterBand(1), proxOpts, cb))
        .isEqualTo(gdalconst.CE_None);

    Dataset maskDs = mem.Create("", 8, 8, 1, gdalconst.GDT_Byte);
    maskDs.GetRasterBand(1).Fill(1);
    maskDs.GetRasterBand(1).WriteRaster(4, 4, 1, 1, new byte[] {0});
    Vector<String> fillOpts = new Vector<>();
    fillOpts.add("INTERPOLATION=NEAREST");
    assertThat(gdal.FillNodata(proxDst.GetRasterBand(1), maskDs.GetRasterBand(1), 5.0, 0))
        .isEqualTo(gdalconst.CE_None);
    assertThat(gdal.FillNodata(proxDst.GetRasterBand(1), maskDs.GetRasterBand(1), 5.0, 0, fillOpts))
        .isEqualTo(gdalconst.CE_None);
    assertThat(
            gdal.FillNodata(
                proxDst.GetRasterBand(1), maskDs.GetRasterBand(1), 5.0, 0, fillOpts, cb))
        .isEqualTo(gdalconst.CE_None);

    Dataset sieveDst = mem.Create("", 8, 8, 1, gdalconst.GDT_Byte);
    Vector<String> dummyOpts = new Vector<>();
    dummyOpts.add("DUMMY=1");
    assertThat(gdal.SieveFilter(proxSrc.GetRasterBand(1), null, sieveDst.GetRasterBand(1), 2))
        .isEqualTo(gdalconst.CE_None);
    assertThat(gdal.SieveFilter(proxSrc.GetRasterBand(1), null, sieveDst.GetRasterBand(1), 2, 4))
        .isEqualTo(gdalconst.CE_None);
    assertThat(
            gdal.SieveFilter(
                proxSrc.GetRasterBand(1), null, sieveDst.GetRasterBand(1), 2, 4, dummyOpts))
        .isEqualTo(gdalconst.CE_None);
    assertThat(
            gdal.SieveFilter(
                proxSrc.GetRasterBand(1), null, sieveDst.GetRasterBand(1), 2, 4, dummyOpts, cb))
        .isEqualTo(gdalconst.CE_None);

    Dataset ovrTarget = mem.Create("", 4, 4, 1, gdalconst.GDT_Byte);
    assertThat(gdal.RegenerateOverview(proxSrc.GetRasterBand(1), ovrTarget.GetRasterBand(1)))
        .isEqualTo(gdalconst.CE_None);
    assertThat(
            gdal.RegenerateOverview(
                proxSrc.GetRasterBand(1), ovrTarget.GetRasterBand(1), "NEAREST"))
        .isEqualTo(gdalconst.CE_None);
    assertThat(
            gdal.RegenerateOverview(
                proxSrc.GetRasterBand(1), ovrTarget.GetRasterBand(1), "NEAREST", cb))
        .isEqualTo(gdalconst.CE_None);
    Band[] ovrArr = new Band[] {ovrTarget.GetRasterBand(1)};
    assertThat(gdal.RegenerateOverviews(proxSrc.GetRasterBand(1), ovrArr))
        .isEqualTo(gdalconst.CE_None);
    assertThat(gdal.RegenerateOverviews(proxSrc.GetRasterBand(1), ovrArr, "NEAREST"))
        .isEqualTo(gdalconst.CE_None);
    assertThat(gdal.RegenerateOverviews(proxSrc.GetRasterBand(1), ovrArr, "NEAREST", cb))
        .isEqualTo(gdalconst.CE_None);

    // Polygonize, FPolygonize, ContourGenerate, ContourGenerateEx, RasterizeLayer.
    DataSource vecDs = ogr.GetDriverByName("Memory").CreateDataSource("mem_alg");
    Layer polyOut = vecDs.CreateLayer("poly_out", wgs84, ogrConstants.wkbPolygon);
    polyOut.CreateField(new FieldDefn("DN", ogrConstants.OFTInteger));
    Vector<String> polyOpts = new Vector<>();
    polyOpts.add("8CONNECTED=8");
    assertThat(gdal.Polygonize(proxSrc.GetRasterBand(1), null, polyOut, 0))
        .isEqualTo(gdalconst.CE_None);
    assertThat(gdal.Polygonize(proxSrc.GetRasterBand(1), null, polyOut, 0, polyOpts))
        .isEqualTo(gdalconst.CE_None);
    assertThat(gdal.Polygonize(proxSrc.GetRasterBand(1), null, polyOut, 0, polyOpts, cb))
        .isEqualTo(gdalconst.CE_None);
    assertThat(gdal.FPolygonize(proxSrc.GetRasterBand(1), null, polyOut, 0))
        .isEqualTo(gdalconst.CE_None);
    assertThat(gdal.FPolygonize(proxSrc.GetRasterBand(1), null, polyOut, 0, polyOpts))
        .isEqualTo(gdalconst.CE_None);
    assertThat(gdal.FPolygonize(proxSrc.GetRasterBand(1), null, polyOut, 0, polyOpts, cb))
        .isEqualTo(gdalconst.CE_None);

    Layer contLayer = vecDs.CreateLayer("contours", wgs84, ogrConstants.wkbLineString);
    contLayer.CreateField(new FieldDefn("ID", ogrConstants.OFTInteger));
    contLayer.CreateField(new FieldDefn("ELEV", ogrConstants.OFTReal));
    assertThat(
            gdal.ContourGenerate(
                src.GetRasterBand(1), 20.0, 0.0, new double[0], 0, 0.0, contLayer, 0, 1))
        .isEqualTo(gdalconst.CE_None);
    assertThat(
            gdal.ContourGenerate(
                src.GetRasterBand(1), 20.0, 0.0, new double[0], 0, 0.0, contLayer, 0, 1, cb))
        .isEqualTo(gdalconst.CE_None);
    assertThat(gdal.ContourGenerateEx(src.GetRasterBand(1), contLayer))
        .isEqualTo(gdalconst.CE_None);
    Vector<String> contExOpts = new Vector<>();
    contExOpts.add("LEVEL_INTERVAL=25");
    assertThat(gdal.ContourGenerateEx(src.GetRasterBand(1), contLayer, contExOpts))
        .isEqualTo(gdalconst.CE_None);
    assertThat(gdal.ContourGenerateEx(src.GetRasterBand(1), contLayer, contExOpts, cb))
        .isEqualTo(gdalconst.CE_None);

    Layer polySrcLayer = vecDs.CreateLayer("poly_src", wgs84, ogrConstants.wkbPolygon);
    Feature pf = new Feature(polySrcLayer.GetLayerDefn());
    pf.SetGeometryDirectly(
        ogr.CreateGeometryFromWkt("POLYGON ((0 0.92, 0 1, 0.08 1, 0.08 0.92, 0 0.92))"));
    polySrcLayer.CreateFeature(pf);
    pf.delete();
    Dataset rastTarget = mem.Create("", 8, 8, 1, gdalconst.GDT_Byte);
    rastTarget.SetProjection(wkt);
    rastTarget.SetGeoTransform(new double[] {0.0, 0.01, 0.0, 1.0, 0.0, -0.01});
    Vector<String> rastLayerOpts = new Vector<>();
    rastLayerOpts.add("ALL_TOUCHED=TRUE");
    assertThat(gdal.RasterizeLayer(rastTarget, new int[] {1}, polySrcLayer))
        .isEqualTo(gdalconst.CE_None);
    assertThat(gdal.RasterizeLayer(rastTarget, new int[] {1}, polySrcLayer, new double[] {255.0}))
        .isEqualTo(gdalconst.CE_None);
    assertThat(
            gdal.RasterizeLayer(
                rastTarget, new int[] {1}, polySrcLayer, new double[] {255.0}, rastLayerOpts))
        .isEqualTo(gdalconst.CE_None);
    assertThat(
            gdal.RasterizeLayer(
                rastTarget, new int[] {1}, polySrcLayer, new double[] {255.0}, rastLayerOpts, cb))
        .isEqualTo(gdalconst.CE_None);
    rastTarget.Close();
    vecDs.delete();

    // GridCreate (double[N][3] + direct ByteBuffer).
    double[][] gridPts =
        new double[][] {{0.0, 0.0, 1.0}, {10.0, 0.0, 2.0}, {0.0, 10.0, 3.0}, {10.0, 10.0, 4.0}};
    ByteBuffer gridBuf = ByteBuffer.allocateDirect(4 * 4 * 4).order(ByteOrder.nativeOrder());
    assertThat(
            gdal.GridCreate(
                "nearest:radius1=20:radius2=20",
                gridPts,
                0.0,
                10.0,
                0.0,
                10.0,
                4,
                4,
                gdalconst.GDT_Float32,
                gridBuf))
        .isEqualTo(gdalconst.CE_None);
    assertThat(
            gdal.GridCreate(
                "nearest:radius1=20:radius2=20",
                gridPts,
                0.0,
                10.0,
                0.0,
                10.0,
                4,
                4,
                gdalconst.GDT_Float32,
                gridBuf,
                cb))
        .isEqualTo(gdalconst.CE_None);

    // ViewshedGenerate runs progress callbacks on a C++ worker thread unattached to the JVM,
    // so pass null for ProgressCallback to avoid calling JavaProgressProxy off-thread.
    Dataset vs1 =
        gdal.ViewshedGenerate(
            src.GetRasterBand(1),
            "MEM",
            "",
            new Vector<String>(),
            0.03,
            0.97,
            10.0,
            0.0,
            255.0,
            0.0,
            0.0,
            0.0,
            0.85,
            ViewshedMode.GVM_Edge,
            1000.0);
    Dataset vs2 =
        gdal.ViewshedGenerate(
            src.GetRasterBand(1),
            "MEM",
            "",
            new Vector<String>(),
            0.03,
            0.97,
            10.0,
            0.0,
            255.0,
            0.0,
            0.0,
            0.0,
            0.85,
            ViewshedMode.GVM_Max,
            1000.0,
            null);
    Dataset vs3 =
        gdal.ViewshedGenerate(
            src.GetRasterBand(1),
            "MEM",
            "",
            new Vector<String>(),
            0.03,
            0.97,
            10.0,
            0.0,
            255.0,
            0.0,
            0.0,
            0.0,
            0.85,
            ViewshedMode.GVM_Min,
            1000.0,
            null,
            ViewshedOutputType.GVOT_NORMAL);
    Dataset vs4 =
        gdal.ViewshedGenerate(
            src.GetRasterBand(1),
            "MEM",
            "",
            new Vector<String>(),
            0.03,
            0.97,
            10.0,
            0.0,
            255.0,
            0.0,
            0.0,
            0.0,
            0.85,
            ViewshedMode.GVM_Diagonal,
            1000.0,
            null,
            ViewshedOutputType.GVOT_MIN_TARGET_HEIGHT_FROM_DEM,
            new Vector<String>());
    assertThat(vs4.GetRasterXSize()).isEqualTo(8);
    assertThat(ViewshedMode.GVM_Edge.swigValue()).isGreaterThan(-1);
    assertThat(ViewshedMode.GVM_Edge.toString()).isEqualTo("GVM_Edge");
    assertThat(ViewshedMode.swigToEnum(ViewshedMode.GVM_Max.swigValue()))
        .isEqualTo(ViewshedMode.GVM_Max);
    assertThat(ViewshedOutputType.GVOT_NORMAL.toString()).isEqualTo("GVOT_NORMAL");
    assertThat(ViewshedOutputType.swigToEnum(ViewshedOutputType.GVOT_NORMAL.swigValue()))
        .isEqualTo(ViewshedOutputType.GVOT_NORMAL);
    assertThat(gdal.IsLineOfSightVisible(src.GetRasterBand(1), 0, 0, 500.0, 5, 5, 500.0)).isTrue();
    assertThat(
            gdal.IsLineOfSightVisible(
                src.GetRasterBand(1), 0, 0, 500.0, 5, 5, 500.0, new Vector<String>()))
        .isTrue();
    vs4.Close();
    vs3.Close();
    vs2.Close();
    vs1.Close();

    // CreatePansharpenedVRT.
    String panXml =
        "<VRTDataset subClass=\"VRTPansharpenedDataset\">"
            + "<PansharpeningOptions><SpectralBand dstBand=\"1\"/></PansharpeningOptions>"
            + "</VRTDataset>";
    Dataset pan =
        gdal.CreatePansharpenedVRT(panXml, src.GetRasterBand(1), new Band[] {src.GetRasterBand(1)});
    assertThat(pan.GetRasterCount()).isEqualTo(1);
    pan.Close();

    // ApplyVerticalShiftGrid (5 overloads).
    Dataset gridShift = mem.Create("", 8, 8, 1, gdalconst.GDT_Float32);
    gridShift.SetProjection(wkt);
    gridShift.SetGeoTransform(new double[] {0.0, 0.01, 0.0, 1.0, 0.0, -0.01});
    gridShift.GetRasterBand(1).Fill(1.0);
    Dataset s1 = gdal.ApplyVerticalShiftGrid(src, gridShift);
    Dataset s2 = gdal.ApplyVerticalShiftGrid(src, gridShift, false);
    Dataset s3 = gdal.ApplyVerticalShiftGrid(src, gridShift, false, 1.0);
    Dataset s4 = gdal.ApplyVerticalShiftGrid(src, gridShift, false, 1.0, 1.0);
    Dataset s5 = gdal.ApplyVerticalShiftGrid(src, gridShift, false, 1.0, 1.0, new Vector<String>());
    assertThat(s5.GetRasterXSize()).isEqualTo(8);
    s5.Close();
    s4.Close();
    s3.Close();
    s2.Close();
    s1.Close();
    gridShift.Close();

    warped5.Close();
    warped4.Close();
    warped3.Close();
    warped2.Close();
    warped1.Close();
    ovrTarget.Close();
    sieveDst.Close();
    maskDs.Close();
    proxDst.Close();
    proxSrc.Close();
    src.Close();
    merc.delete();
    wgs84.delete();
    termCb.delete();
    baseCb.delete();
    cb.delete();
  }

  @Test
  @SuppressWarnings({"unchecked", "JdkObsolete"})
  public void librarifiedUtilitiesAndOptionsClasses_coverAllOverloads() {
    gdal.MkdirRecursive(VSIMEM_DIR, 0755);
    RecordingCallback cb = new RecordingCallback();

    Driver gtiff = gdal.GetDriverByName("GTiff");
    String srcTifPath = VSIMEM_DIR + "/util_src.tif";
    Dataset src = gtiff.Create(srcTifPath, 8, 8, 1, gdalconst.GDT_Float32);
    SpatialReference wgs84 = new SpatialReference();
    wgs84.ImportFromEPSG(4326);
    src.SetSpatialRef(wgs84);
    src.SetGeoTransform(new double[] {0.0, 0.01, 0.0, 1.0, 0.0, -0.01});
    float[] vals = new float[64];
    for (int i = 0; i < 64; i++) {
      vals[i] = (i == 0) ? 0.0f : (10.0f + i);
    }
    src.GetRasterBand(1).WriteRaster(0, 0, 8, 8, vals);
    src.FlushCache();

    // InfoOptions & GDALInfo.
    Vector<String> infoVec = new Vector<>();
    infoVec.add("-json");
    InfoOptions infoOpts = new InfoOptions(infoVec);
    assertThat(gdal.GDALInfo(src, infoOpts)).contains("\"size\"");
    assertThat(infoOpts.equals(infoOpts)).isTrue();
    assertThat(infoOpts.equals(new Object())).isFalse();
    assertThat(infoOpts.hashCode()).isNotEqualTo(0);
    infoOpts.delete();

    // TranslateOptions & Translate (both overloads).
    TranslateOptions transOpts = new TranslateOptions(new Vector<String>());
    Dataset t1 = gdal.Translate(VSIMEM_DIR + "/t1.tif", src, transOpts);
    Dataset t2 = gdal.Translate(VSIMEM_DIR + "/t2.tif", src, transOpts, cb);
    assertThat(t1.GetRasterXSize()).isEqualTo(8);
    assertThat(t2.GetRasterXSize()).isEqualTo(8);
    t2.Close();
    t1.Close();
    transOpts.delete();

    // WarpOptions & Warp (all 4 overloads; do not reuse *Options after a ProgressCallback call
    // because C++ SetProgress stores a stack pointer).
    WarpOptions warpOpts = new WarpOptions(new Vector<String>());
    Dataset wOutPath1 = gdal.Warp(VSIMEM_DIR + "/w1.tif", new Dataset[] {src}, warpOpts);
    assertThat(gdal.Warp(wOutPath1, new Dataset[] {src}, warpOpts)).isEqualTo(1);
    warpOpts.delete();
    WarpOptions warpOptsCb = new WarpOptions(new Vector<String>());
    Dataset wOutPath2 = gdal.Warp(VSIMEM_DIR + "/w2.tif", new Dataset[] {src}, warpOptsCb, cb);
    assertThat(gdal.Warp(wOutPath2, new Dataset[] {src}, warpOptsCb, cb)).isEqualTo(1);
    wOutPath2.Close();
    wOutPath1.Close();
    warpOptsCb.delete();

    // DEMProcessingOptions & DEMProcessing.
    Vector<String> demVec = new Vector<>();
    demVec.add("-of");
    demVec.add("GTiff");
    DEMProcessingOptions demOpts = new DEMProcessingOptions(demVec);
    Dataset dem1 = gdal.DEMProcessing(VSIMEM_DIR + "/dem1.tif", src, "slope", null, demOpts);
    Dataset dem2 = gdal.DEMProcessing(VSIMEM_DIR + "/dem2.tif", src, "slope", null, demOpts, cb);
    assertThat(dem1.GetRasterXSize()).isEqualTo(8);
    assertThat(dem2.GetRasterXSize()).isEqualTo(8);
    dem2.Close();
    dem1.Close();
    demOpts.delete();

    // NearblackOptions & Nearblack (all 4 overloads on Byte raster).
    Dataset byteSrc = gtiff.Create(VSIMEM_DIR + "/nb_src.tif", 8, 8, 1, gdalconst.GDT_Byte);
    byteSrc.GetRasterBand(1).Fill(200);
    byteSrc.FlushCache();
    Vector<String> nbVec = new Vector<>();
    nbVec.add("-near");
    nbVec.add("0");
    NearblackOptions nbOpts = new NearblackOptions(nbVec);
    Dataset nb1 = gdal.Nearblack(VSIMEM_DIR + "/nb1.tif", byteSrc, nbOpts);
    assertThat(gdal.Nearblack(nb1, byteSrc, nbOpts)).isEqualTo(1);
    nbOpts.delete();
    NearblackOptions nbOptsCb = new NearblackOptions(nbVec);
    Dataset nb2 = gdal.Nearblack(VSIMEM_DIR + "/nb2.tif", byteSrc, nbOptsCb, cb);
    assertThat(gdal.Nearblack(nb2, byteSrc, nbOptsCb, cb)).isEqualTo(1);
    nb2.Close();
    nb1.Close();
    byteSrc.Close();
    nbOptsCb.delete();

    // ContourOptions & Contour (all 4 overloads; -f is CLI-only in ContourOptions).
    Vector<String> contVec = new Vector<>();
    contVec.add("-i");
    contVec.add("20");
    ContourOptions contOpts = new ContourOptions(contVec);
    Dataset c1 = gdal.Contour(VSIMEM_DIR + "/c1.gpkg", src, contOpts);
    assertThat(c1.GetLayerCount()).isEqualTo(1);
    c1.Close();
    Dataset cDst1 = gdal.GetDriverByName("Memory").CreateVector("cont_dst1");
    cDst1.CreateLayer("contour", wgs84, ogrConstants.wkbLineString);
    assertThat(gdal.Contour(cDst1, src, contOpts)).isEqualTo(1);
    cDst1.Close();
    contOpts.delete();
    ContourOptions contOptsCb = new ContourOptions(contVec);
    Dataset c2 = gdal.Contour(VSIMEM_DIR + "/c2.gpkg", src, contOptsCb, cb);
    assertThat(c2.GetLayerCount()).isEqualTo(1);
    c2.Close();
    Dataset cDst2 = gdal.GetDriverByName("Memory").CreateVector("cont_dst2");
    cDst2.CreateLayer("contour", wgs84, ogrConstants.wkbLineString);
    assertThat(gdal.Contour(cDst2, src, contOptsCb, cb)).isEqualTo(1);
    cDst2.Close();
    contOptsCb.delete();

    // FootprintOptions & Footprint (all 4 overloads; omit -f when writing to an existing Dataset).
    Vector<String> fpVec = new Vector<>();
    fpVec.add("-f");
    fpVec.add("GeoJSON");
    FootprintOptions fpOpts = new FootprintOptions(fpVec);
    Dataset fp1 = gdal.Footprint(VSIMEM_DIR + "/fp1.geojson", src, fpOpts);
    fp1.Close();
    fpOpts.delete();
    FootprintOptions fpOptsCb = new FootprintOptions(fpVec);
    Dataset fp2 = gdal.Footprint(VSIMEM_DIR + "/fp2.geojson", src, fpOptsCb, cb);
    fp2.Close();
    fpOptsCb.delete();
    FootprintOptions fpDsOpts = new FootprintOptions(new Vector<String>());
    Dataset fpMem1 = gdal.GetDriverByName("Memory").CreateVector("fp_m1");
    assertThat(gdal.Footprint(fpMem1, src, fpDsOpts)).isEqualTo(1);
    fpMem1.Close();
    fpDsOpts.delete();
    FootprintOptions fpDsOptsCb = new FootprintOptions(new Vector<String>());
    Dataset fpMem2 = gdal.GetDriverByName("Memory").CreateVector("fp_m2");
    assertThat(gdal.Footprint(fpMem2, src, fpDsOptsCb, cb)).isEqualTo(1);
    fpMem2.Close();
    fpDsOptsCb.delete();

    // BuildVRTOptions & BuildVRT (all 4 overloads) + TileIndexOptions & TileIndex.
    BuildVRTOptions vrtOpts = new BuildVRTOptions(new Vector<String>());
    Vector<String> srcNames = new Vector<>();
    srcNames.add(srcTifPath);
    Dataset v1 = gdal.BuildVRT(VSIMEM_DIR + "/v1.vrt", new Dataset[] {src}, vrtOpts);
    Dataset v3 = gdal.BuildVRT(VSIMEM_DIR + "/v3.vrt", srcNames, vrtOpts);
    v3.Close();
    v1.Close();
    vrtOpts.delete();
    BuildVRTOptions vrtOptsCb = new BuildVRTOptions(new Vector<String>());
    Dataset v2 = gdal.BuildVRT(VSIMEM_DIR + "/v2.vrt", new Dataset[] {src}, vrtOptsCb, cb);
    Dataset v4 = gdal.BuildVRT(VSIMEM_DIR + "/v4.vrt", srcNames, vrtOptsCb, cb);
    assertThat(v4.GetRasterXSize()).isEqualTo(8);
    v4.Close();
    v2.Close();
    vrtOptsCb.delete();

    Vector<String> tiVec = new Vector<>();
    tiVec.add("-f");
    tiVec.add("GeoJSON");
    tiVec.add("-lyr_name");
    tiVec.add("tileindex");
    TileIndexOptions tiOpts = new TileIndexOptions(tiVec);
    Dataset ti1 = gdal.TileIndex(VSIMEM_DIR + "/ti1.geojson", srcNames, tiOpts);
    assertThat(ti1).isNotNull();
    ti1.Close();
    tiOpts.delete();
    TileIndexOptions tiOptsCb = new TileIndexOptions(tiVec);
    Dataset ti2 = gdal.TileIndex(VSIMEM_DIR + "/ti2.geojson", srcNames, tiOptsCb, cb);
    assertThat(ti2).isNotNull();
    ti2.Close();
    tiOptsCb.delete();

    // VectorInfoOptions + GDALVectorInfo, VectorTranslate, Grid, Rasterize.
    Dataset vecSrc = gdal.GetDriverByName("GPKG").CreateVector(VSIMEM_DIR + "/vsrc.gpkg");
    Layer ptLayer = vecSrc.CreateLayer("pts", wgs84, ogrConstants.wkbPoint);
    ptLayer.CreateField(new FieldDefn("z", ogrConstants.OFTReal));
    for (int i = 0; i < 4; i++) {
      Feature f = new Feature(ptLayer.GetLayerDefn());
      f.SetField("z", 10.0 * (i + 1));
      f.SetGeometryDirectly(
          ogr.CreateGeometryFromWkt(
              "POINT (" + (0.01 * (i % 2)) + " " + (0.99 - 0.01 * (i / 2)) + ")"));
      ptLayer.CreateFeature(f);
      f.delete();
    }
    vecSrc.FlushCache();

    Vector<String> viVec = new Vector<>();
    viVec.add("-al");
    viVec.add("-so");
    VectorInfoOptions viOpts = new VectorInfoOptions(viVec);
    assertThat(gdal.GDALVectorInfo(vecSrc, viOpts)).contains("Layer name: pts");
    viOpts.delete();

    Vector<String> vtVec = new Vector<>();
    vtVec.add("-f");
    vtVec.add("GPKG");
    VectorTranslateOptions vtOpts = new VectorTranslateOptions(vtVec);
    Dataset vt1 = gdal.VectorTranslate(VSIMEM_DIR + "/vt1.gpkg", vecSrc, vtOpts);
    vt1.Close();
    Dataset vtMem1 = gdal.GetDriverByName("Memory").CreateVector("vt_m1");
    assertThat(gdal.VectorTranslate(vtMem1, vecSrc, vtOpts)).isEqualTo(1);
    vtMem1.Close();
    vtOpts.delete();
    VectorTranslateOptions vtOptsCb = new VectorTranslateOptions(vtVec);
    Dataset vt2 = gdal.VectorTranslate(VSIMEM_DIR + "/vt2.gpkg", vecSrc, vtOptsCb, cb);
    vt2.Close();
    Dataset vtMem2 = gdal.GetDriverByName("Memory").CreateVector("vt_m2");
    assertThat(gdal.VectorTranslate(vtMem2, vecSrc, vtOptsCb, cb)).isEqualTo(1);
    vtMem2.Close();
    vtOptsCb.delete();

    Vector<String> gridVec = new Vector<>();
    gridVec.add("-zfield");
    gridVec.add("z");
    gridVec.add("-outsize");
    gridVec.add("4");
    gridVec.add("4");
    gridVec.add("-a");
    gridVec.add("nearest:radius1=1:radius2=1");
    GridOptions gridOpts = new GridOptions(gridVec);
    Dataset g1 = gdal.Grid(VSIMEM_DIR + "/g1.tif", vecSrc, gridOpts);
    Dataset g2 = gdal.Grid(VSIMEM_DIR + "/g2.tif", vecSrc, gridOpts, cb);
    g2.Close();
    g1.Close();
    gridOpts.delete();

    Vector<String> rastVec = new Vector<>();
    rastVec.add("-burn");
    rastVec.add("1");
    rastVec.add("-ts");
    rastVec.add("8");
    rastVec.add("8");
    RasterizeOptions rastOpts = new RasterizeOptions(rastVec);
    Dataset r1 = gdal.Rasterize(VSIMEM_DIR + "/r1.tif", vecSrc, rastOpts);
    Dataset r2 = gdal.Rasterize(VSIMEM_DIR + "/r2.tif", vecSrc, rastOpts, cb);
    Vector<String> rastInPlaceVec = new Vector<>();
    rastInPlaceVec.add("-burn");
    rastInPlaceVec.add("2");
    RasterizeOptions rastInPlaceOpts = new RasterizeOptions(rastInPlaceVec);
    assertThat(gdal.Rasterize(r1, vecSrc, rastInPlaceOpts)).isEqualTo(1);
    assertThat(gdal.Rasterize(r2, vecSrc, rastInPlaceOpts, cb)).isEqualTo(1);
    r2.Close();
    r1.Close();
    rastInPlaceOpts.delete();
    rastOpts.delete();
    vecSrc.Close();

    // MultiDimInfoOptions + GDALMultiDimInfo + MultiDimTranslateOptions + MultiDimTranslate.
    Driver memMd = gdal.GetDriverByName("MEM");
    Dataset mdSrc = memMd.CreateMultiDimensional("md_util");
    Group root = mdSrc.GetRootGroup();
    Dimension dim = root.CreateDimension("d", "", "", 4L);
    MDArray arr =
        root.CreateMDArray(
            "arr", new Dimension[] {dim}, ExtendedDataType.Create(gdalconst.GDT_Float32));
    arr.Write(new long[] {0L}, new long[] {4L}, new float[] {1.0f, 2.0f, 3.0f, 4.0f});
    MultiDimInfoOptions mdiOpts = new MultiDimInfoOptions(new Vector<String>());
    assertThat(gdal.GDALMultiDimInfo(mdSrc, mdiOpts)).contains("arr");
    mdiOpts.delete();

    Vector<String> mdtVec = new Vector<>();
    mdtVec.add("-of");
    mdtVec.add("Zarr");
    MultiDimTranslateOptions mdtOpts = new MultiDimTranslateOptions(mdtVec);
    Dataset mdt1 =
        gdal.MultiDimTranslate(VSIMEM_DIR + "/mdt1.zarr", new Dataset[] {mdSrc}, mdtOpts);
    Dataset mdt2 =
        gdal.MultiDimTranslate(VSIMEM_DIR + "/mdt2.zarr", new Dataset[] {mdSrc}, mdtOpts, cb);
    assertThat(mdt2.GetRootGroup().GetMDArrayNames()).isNotEmpty();
    mdt2.Close();
    mdt1.Close();
    mdtOpts.delete();

    arr.delete();
    dim.delete();
    root.delete();
    mdSrc.Close();
    src.Close();
    wgs84.delete();
    cb.delete();
  }
}
