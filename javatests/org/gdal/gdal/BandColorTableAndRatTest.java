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

import java.awt.Color;
import java.awt.image.IndexColorModel;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.List;
import java.util.Vector;
import org.gdal.GoogleGdalLoader;
import org.gdal.gdalconst.gdalconst;
import org.gdal.osr.SpatialReference;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;

public final class BandColorTableAndRatTest {

  private static final String VSIMEM_DIR = "/vsimem/BandColorTableAndRatTest";

  @Before
  public void setUp() {
    GoogleGdalLoader.init();
    gdal.UseExceptions();
  }

  @After
  public void tearDown() {
    gdal.UseExceptions();
    gdal.ErrorReset();
    gdal.RmdirRecursive(VSIMEM_DIR);
  }

  @Test
  @SuppressWarnings({"unchecked", "JdkObsolete"})
  public void bandMetadataStatsHistogramsOverviewsAndInterpolation_workCorrectly() {
    gdal.MkdirRecursive(VSIMEM_DIR, 0755);
    String path = VSIMEM_DIR + "/band.tif";
    Dataset ds = gdal.GetDriverByName("GTiff").Create(path, 16, 16, 1, gdalconst.GDT_Float32);
    SpatialReference wgs84 = new SpatialReference();
    wgs84.ImportFromEPSG(4326);
    ds.SetSpatialRef(wgs84);
    ds.SetGeoTransform(new double[] {0.0, 1.0, 0.0, 16.0, 0.0, -1.0});

    Band band = ds.GetRasterBand(1);
    assertThat(band.GetBand()).isEqualTo(1);
    assertThat(band.GetXSize()).isEqualTo(16);
    assertThat(band.GetYSize()).isEqualTo(16);
    assertThat(band.getXSize()).isEqualTo(16);
    assertThat(band.getYSize()).isEqualTo(16);
    assertThat(band.GetRasterDataType()).isEqualTo(gdalconst.GDT_Float32);
    assertThat(band.getDataType()).isEqualTo(gdalconst.GDT_Float32);
    assertThat(band.GetBlockXSize()).isGreaterThan(0);
    assertThat(band.GetBlockYSize()).isGreaterThan(0);
    int[] bx = new int[1];
    int[] by = new int[1];
    band.GetBlockSize(bx, by);
    assertThat(bx[0]).isEqualTo(band.GetBlockXSize());
    assertThat(band.GetDataset().GetRasterXSize()).isEqualTo(16);
    assertThat(band.GetDatasetInternal().GetRasterXSize()).isEqualTo(16);

    assertThat(band.Fill(5.0)).isEqualTo(gdalconst.CE_None);
    assertThat(band.Fill(5.0, 0.0)).isEqualTo(gdalconst.CE_None);
    float[] gradient = new float[256];
    for (int i = 0; i < 256; i++) {
      gradient[i] = i;
    }
    assertThat(band.WriteRaster(0, 0, 16, 16, gradient)).isEqualTo(gdalconst.CE_None);
    band.FlushCache();

    // NoData, Offset, Scale, Unit, ColorInterpretation, CategoryNames.
    Double[] unsetVal = new Double[1];
    band.GetNoDataValue(unsetVal);
    assertThat(unsetVal[0]).isNull();
    assertThat(band.SetNoDataValue(-9999.0)).isEqualTo(gdalconst.CE_None);
    Double[] nodataOut = new Double[1];
    band.GetNoDataValue(nodataOut);
    assertThat(nodataOut[0]).isEqualTo(-9999.0);
    assertThat(band.DeleteNoDataValue()).isEqualTo(gdalconst.CE_None);

    assertThat(band.SetOffset(1.5)).isEqualTo(gdalconst.CE_None);
    assertThat(band.SetScale(2.0)).isEqualTo(gdalconst.CE_None);
    Double[] offOut = new Double[1];
    Double[] sclOut = new Double[1];
    band.GetOffset(offOut);
    band.GetScale(sclOut);
    assertThat(offOut[0]).isEqualTo(1.5);
    assertThat(sclOut[0]).isEqualTo(2.0);

    assertThat(band.SetUnitType("m")).isEqualTo(gdalconst.CE_None);
    assertThat(band.GetUnitType()).isEqualTo("m");
    assertThat(band.SetColorInterpretation(gdalconst.GCI_GrayIndex)).isEqualTo(gdalconst.CE_None);
    assertThat(band.GetColorInterpretation()).isEqualTo(gdalconst.GCI_GrayIndex);
    assertThat(band.SetRasterColorInterpretation(gdalconst.GCI_GrayIndex))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.GetRasterColorInterpretation()).isEqualTo(gdalconst.GCI_GrayIndex);

    // Statistics, Min/Max, Checksum, Histograms, Interpolation.
    double[] min = new double[1];
    double[] max = new double[1];
    double[] mean = new double[1];
    double[] stddev = new double[1];
    ProgressCallback cb = new ProgressCallback();
    assertThat(band.ComputeStatistics(false, min, max, mean, stddev)).isEqualTo(gdalconst.CE_None);
    assertThat(band.ComputeStatistics(false, min, max, mean, stddev, cb))
        .isEqualTo(gdalconst.CE_None);
    assertThat(min[0]).isWithin(1e-6).of(0.0);
    assertThat(max[0]).isWithin(1e-6).of(255.0);
    assertThat(band.SetStatistics(0.0, 255.0, 127.5, 73.9)).isEqualTo(gdalconst.CE_None);
    assertThat(band.GetStatistics(false, true, min, max, mean, stddev))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.GetStatistics(0, 1, min, max, mean, stddev)).isEqualTo(gdalconst.CE_None);

    Double[] minWrap = new Double[1];
    Double[] maxWrap = new Double[1];
    band.GetMinimum(minWrap);
    band.GetMaximum(maxWrap);
    assertThat(minWrap[0]).isNotNull();
    assertThat(maxWrap[0]).isNotNull();

    double[] minMax = new double[2];
    band.ComputeRasterMinMax(minMax);
    band.ComputeRasterMinMax(minMax, 0);
    double[] bandStats = new double[2];
    band.ComputeBandStats(bandStats);
    band.ComputeBandStats(bandStats, 1);

    double[] locMin = new double[1];
    double[] locMax = new double[1];
    int[] minX = new int[1];
    int[] minY = new int[1];
    int[] maxX = new int[1];
    int[] maxY = new int[1];
    assertThat(band.ComputeMinMaxLocation(locMin, locMax, minX, minY, maxX, maxY))
        .isEqualTo(gdalconst.CE_None);
    assertThat(minX[0]).isEqualTo(0);
    assertThat(maxX[0]).isEqualTo(15);
    NullPointerException cmlNull =
        assertThrows(
            NullPointerException.class,
            () -> band.ComputeMinMaxLocation(null, locMax, minX, minY, maxX, maxY));
    assertThat(cmlNull).isNotNull();
    assertThrows(
        NullPointerException.class,
        () -> band.ComputeMinMaxLocation(locMin, null, minX, minY, maxX, maxY));
    assertThrows(
        NullPointerException.class,
        () -> band.ComputeMinMaxLocation(locMin, locMax, null, minY, maxX, maxY));
    assertThrows(
        NullPointerException.class,
        () -> band.ComputeMinMaxLocation(locMin, locMax, minX, null, maxX, maxY));
    assertThrows(
        NullPointerException.class,
        () -> band.ComputeMinMaxLocation(locMin, locMax, minX, minY, null, maxY));
    assertThrows(
        NullPointerException.class,
        () -> band.ComputeMinMaxLocation(locMin, locMax, minX, minY, maxX, null));
    IndexOutOfBoundsException cmlEmpty =
        assertThrows(
            IndexOutOfBoundsException.class,
            () -> band.ComputeMinMaxLocation(new double[0], locMax, minX, minY, maxX, maxY));
    assertThat(cmlEmpty).isNotNull();

    gdal.DontUseExceptions();
    assertThat(band.ReadRaster(0, 0, 4, 4, 0, 4, gdalconst.GDT_Byte, new byte[16]))
        .isEqualTo(gdalconst.CE_Failure);
    assertThat(band.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, new byte[16], -1, 4))
        .isEqualTo(gdalconst.CE_Failure);
    assertThat(band.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, new byte[1]))
        .isEqualTo(gdalconst.CE_Failure);
    assertThat(
            band.ReadRaster_Direct(
                0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, ByteBuffer.allocateDirect(1)))
        .isEqualTo(gdalconst.CE_Failure);
    gdal.UseExceptions();
    gdal.ErrorReset();
    RuntimeException bandNonDirect =
        assertThrows(
            RuntimeException.class,
            () ->
                band.ReadRaster_Direct(
                    0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, ByteBuffer.allocate(16)));
    assertThat(bandNonDirect).hasMessageThat().contains("direct");

    assertThat(band.Checksum()).isEqualTo(band.Checksum(0, 0, 16, 16));

    int[] hist = new int[16];
    assertThat(band.GetHistogram(hist)).isEqualTo(gdalconst.CE_None);
    assertThat(band.GetHistogram(0.0, 256.0, hist)).isEqualTo(gdalconst.CE_None);
    assertThat(band.GetHistogram(0.0, 256.0, hist, false, false)).isEqualTo(gdalconst.CE_None);
    assertThat(band.GetHistogram(0.0, 256.0, hist, false, false, cb)).isEqualTo(gdalconst.CE_None);

    assertThat(band.SetDefaultHistogram(0.0, 256.0, hist)).isEqualTo(gdalconst.CE_None);
    double[] hMin = new double[1];
    double[] hMax = new double[1];
    int[][] hBuckets = new int[1][];
    assertThat(band.GetDefaultHistogram(hMin, hMax, hBuckets)).isEqualTo(gdalconst.CE_None);
    assertThat(band.GetDefaultHistogram(hMin, hMax, hBuckets, true)).isEqualTo(gdalconst.CE_None);
    assertThat(band.GetDefaultHistogram(hMin, hMax, hBuckets, true, cb))
        .isEqualTo(gdalconst.CE_None);

    // Interpolation & AdviseRead.
    double[] realVal = new double[1];
    double[] imagVal = new double[1];
    assertThat(
            band.InterpolateAtPoint(2.0, 1.0, gdalconst.GRIORA_NearestNeighbour, realVal, imagVal))
        .isEqualTo(gdalconst.CE_None);
    assertThat(realVal[0]).isWithin(1e-6).of(18.0);
    assertThat(
            band.InterpolateAtGeolocation(
                2.0, 15.0, wgs84, gdalconst.GRIORA_NearestNeighbour, realVal, imagVal))
        .isEqualTo(gdalconst.CE_None);
    Vector<String> interpOpts = new Vector<>();
    interpOpts.add("DUMMY=1");
    assertThat(
            band.InterpolateAtGeolocation(
                2.0, 15.0, wgs84, gdalconst.GRIORA_NearestNeighbour, realVal, imagVal, interpOpts))
        .isEqualTo(gdalconst.CE_None);

    assertThat(band.AdviseRead(0, 0, 16, 16)).isEqualTo(gdalconst.CE_None);
    assertThat(band.AdviseRead(0, 0, 16, 16, null)).isEqualTo(gdalconst.CE_None);
    assertThat(band.AdviseRead(0, 0, 16, 16, null, null)).isEqualTo(gdalconst.CE_None);
    assertThat(band.AdviseRead(0, 0, 16, 16, null, null, null)).isEqualTo(gdalconst.CE_None);
    assertThat(band.AdviseRead(0, 0, 16, 16, null, null, null, interpOpts))
        .isEqualTo(gdalconst.CE_None);

    // Mask & Overviews.
    assertThat(ds.BuildOverviews(new int[] {2})).isEqualTo(gdalconst.CE_None);
    assertThat(band.GetOverviewCount()).isEqualTo(1);
    assertThat(band.GetOverview(0).GetXSize()).isEqualTo(8);
    assertThat(band.GetSampleOverview(64)).isNotNull();
    assertThat(band.HasArbitraryOverviews()).isFalse();
    assertThat(band.IsMaskBand()).isFalse();
    assertThat(band.GetMaskFlags()).isGreaterThan(0);
    assertThat(band.CreateMaskBand(gdalconst.GMF_PER_DATASET)).isEqualTo(gdalconst.CE_None);
    Band maskBand = band.GetMaskBand();
    assertThat(maskBand.IsMaskBand()).isTrue();

    // AsMDArray & category names.
    MDArray md = band.AsMDArray();
    assertThat(md).isNotNull();
    md.delete();
    band._EnablePixelTypeSignedByteWarning(true);
    ds.Close();

    // CategoryNames & ColorTable work cleanly on MEM Byte band.
    Dataset memDs = gdal.GetDriverByName("MEM").Create("", 4, 4, 1, gdalconst.GDT_Byte);
    Band memBand = memDs.GetRasterBand(1);
    Vector<String> cats = new Vector<>();
    cats.add("Water");
    cats.add("Land");
    assertThat(memBand.SetCategoryNames(cats)).isEqualTo(gdalconst.CE_None);
    assertThat((List<String>) memBand.GetCategoryNames()).containsExactly("Water", "Land");
    assertThat(memBand.SetRasterCategoryNames(cats)).isEqualTo(gdalconst.CE_None);
    assertThat((List<String>) memBand.GetRasterCategoryNames()).containsExactly("Water", "Land");

    ColorTable ct = new ColorTable();
    ct.SetColorEntry(0, new Color(10, 20, 30));
    memBand.SetColorTable(ct);
    assertThat(memBand.GetColorTable().GetCount()).isEqualTo(1);
    memBand.SetRasterColorTable(ct);
    assertThat(memBand.GetRasterColorTable().GetCount()).isEqualTo(1);

    ct.delete();
    memDs.Close();
    cb.delete();
    wgs84.delete();
  }

  @Test
  public void bandRasterAndBlockIoOverloads_coverAllPrimitiveAndDirectVariants() {
    Dataset ds = gdal.GetDriverByName("MEM").Create("", 4, 4, 1, gdalconst.GDT_Float64);
    Band band = ds.GetRasterBand(1);

    // byte[]
    byte[] b = new byte[16];
    assertThat(band.WriteRaster(0, 0, 4, 4, b)).isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster(0, 0, 4, 4, gdalconst.GDT_Byte, b)).isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, b))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, b, 1))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, b, 1, 4))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, b)).isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, gdalconst.GDT_Byte, b)).isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, b))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, b, 1))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Byte, b, 1, 4))
        .isEqualTo(gdalconst.CE_None);

    // short[]
    short[] s = new short[16];
    assertThat(band.WriteRaster(0, 0, 4, 4, s)).isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster(0, 0, 4, 4, gdalconst.GDT_Int16, s)).isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int16, s))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int16, s, 2))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int16, s, 2, 8))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, s)).isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, gdalconst.GDT_Int16, s)).isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int16, s))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int16, s, 2))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int16, s, 2, 8))
        .isEqualTo(gdalconst.CE_None);

    // int[]
    int[] iv = new int[16];
    assertThat(band.WriteRaster(0, 0, 4, 4, iv)).isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster(0, 0, 4, 4, gdalconst.GDT_Int32, iv)).isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int32, iv))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int32, iv, 4))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int32, iv, 4, 16))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, iv)).isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, gdalconst.GDT_Int32, iv)).isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int32, iv))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int32, iv, 4))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int32, iv, 4, 16))
        .isEqualTo(gdalconst.CE_None);

    // long[]
    long[] lv = new long[16];
    assertThat(band.WriteRaster(0, 0, 4, 4, lv)).isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster(0, 0, 4, 4, gdalconst.GDT_Int64, lv)).isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int64, lv))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int64, lv, 8))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int64, lv, 8, 32))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, lv)).isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, gdalconst.GDT_Int64, lv)).isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int64, lv))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int64, lv, 8))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Int64, lv, 8, 32))
        .isEqualTo(gdalconst.CE_None);

    // float[]
    float[] fv = new float[16];
    assertThat(band.WriteRaster(0, 0, 4, 4, fv)).isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster(0, 0, 4, 4, gdalconst.GDT_Float32, fv))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float32, fv))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float32, fv, 4))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float32, fv, 4, 16))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, fv)).isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, gdalconst.GDT_Float32, fv)).isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float32, fv))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float32, fv, 4))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float32, fv, 4, 16))
        .isEqualTo(gdalconst.CE_None);

    // double[]
    double[] dv = new double[16];
    dv[0] = 9.5;
    assertThat(band.WriteRaster(0, 0, 4, 4, dv)).isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster(0, 0, 4, 4, gdalconst.GDT_Float64, dv))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float64, dv))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float64, dv, 8))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float64, dv, 8, 32))
        .isEqualTo(gdalconst.CE_None);
    double[] dvOut = new double[16];
    assertThat(band.ReadRaster(0, 0, 4, 4, dvOut)).isEqualTo(gdalconst.CE_None);
    assertThat(dvOut[0]).isWithin(1e-9).of(9.5);
    assertThat(band.ReadRaster(0, 0, 4, 4, gdalconst.GDT_Float64, dvOut))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float64, dvOut))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float64, dvOut, 8))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float64, dvOut, 8, 32))
        .isEqualTo(gdalconst.CE_None);

    // Direct ByteBuffer ReadRaster_Direct / WriteRaster_Direct / Block I/O.
    ByteBuffer direct = ByteBuffer.allocateDirect(16 * 8).order(ByteOrder.nativeOrder());
    assertThat(band.WriteRaster_Direct(0, 0, 4, 4, direct)).isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster_Direct(0, 0, 4, 4, gdalconst.GDT_Float64, direct))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster_Direct(0, 0, 4, 4, 4, 4, direct)).isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster_Direct(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float64, direct))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster_Direct(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float64, direct, 8))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteRaster_Direct(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float64, direct, 8, 32))
        .isEqualTo(gdalconst.CE_None);

    assertThat(band.ReadRaster_Direct(0, 0, 4, 4)).isNotNull();
    assertThat(band.ReadRaster_Direct(0, 0, 4, 4, gdalconst.GDT_Float32)).isNotNull();
    assertThat(band.ReadRaster_Direct(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float32)).isNotNull();
    assertThat(band.ReadRaster_Direct(0, 0, 4, 4, direct)).isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster_Direct(0, 0, 4, 4, 4, 4, direct)).isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster_Direct(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float64, direct))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster_Direct(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float64, direct, 8))
        .isEqualTo(gdalconst.CE_None);
    assertThat(band.ReadRaster_Direct(0, 0, 4, 4, 4, 4, gdalconst.GDT_Float64, direct, 8, 32))
        .isEqualTo(gdalconst.CE_None);

    int blockBytes = band.GetBlockXSize() * band.GetBlockYSize() * 8;
    ByteBuffer blockBuf = ByteBuffer.allocateDirect(blockBytes).order(ByteOrder.nativeOrder());
    assertThat(band.ReadBlock_Direct(0, 0, blockBuf)).isEqualTo(gdalconst.CE_None);
    assertThat(band.WriteBlock_Direct(0, 0, blockBuf)).isEqualTo(gdalconst.CE_None);

    ds.Close();
  }

  @Test
  public void computedBandRasterAlgebraColorTableAlphaAndRat_workCorrectly() {
    Dataset ds = gdal.GetDriverByName("MEM").Create("", 2, 2, 2, gdalconst.GDT_Float32);
    Band b1 = ds.GetRasterBand(1);
    Band b2 = ds.GetRasterBand(2);
    b1.Fill(-4.0);
    b2.Fill(10.0);

    ComputedBand absB1 = b1.UnaryOp(RasterAlgebraUnaryOperation.GRAUO_ABS);
    ComputedBand sqrtPos = absB1.UnaryOp(RasterAlgebraUnaryOperation.GRAUO_SQRT);
    ComputedBand added = sqrtPos.BinaryOpBand(RasterAlgebraBinaryOperation.GRABO_ADD, b2);
    ComputedBand mul2 = added.BinaryOpDouble(RasterAlgebraBinaryOperation.GRABO_MUL, 2.0);
    double[] out = new double[4];
    assertThat(mul2.ReadRaster(0, 0, 2, 2, out)).isEqualTo(gdalconst.CE_None);
    assertThat(out[0]).isWithin(1e-6).of(24.0); // (|-4|^0.5 + 10) * 2 = 24

    ComputedBand rev = Band.BinaryOpDoubleToBand(100.0, RasterAlgebraBinaryOperation.GRABO_SUB, b2);
    assertThat(rev.ReadRaster(0, 0, 2, 2, out)).isEqualTo(gdalconst.CE_None);
    assertThat(out[0]).isWithin(1e-6).of(90.0);

    // Comparison ops and IfThenElse require muparser (disabled in google3 build).
    gdal.DontUseExceptions();
    assertThat(b2.BinaryOpDouble(RasterAlgebraBinaryOperation.GRABO_GT, 5.0)).isNull();
    assertThat(Band.BinaryOpDoubleToBand(5.0, RasterAlgebraBinaryOperation.GRABO_GT, b2)).isNull();
    assertThat(Band.IfThenElse(b2, b2, b1)).isNull();
    gdal.UseExceptions();
    gdal.ErrorReset();

    ComputedBand asInt = added.AsType(gdalconst.GDT_Int32);
    ComputedBand maxN = Band.MaximumOfNBands(new Band[] {b1, b2});
    ComputedBand maxC = b1.MaxConstant(3.0);
    ComputedBand minN = Band.MinimumOfNBands(new Band[] {b1, b2});
    ComputedBand minC = b2.MinConstant(3.0);
    ComputedBand meanN = Band.MeanOfNBands(new Band[] {b1, b2});
    assertThat(meanN.ReadRaster(0, 0, 2, 2, out)).isEqualTo(gdalconst.CE_None);
    assertThat(out[0]).isWithin(1e-6).of(3.0);

    // Exercise enum helpers.
    assertThat(RasterAlgebraUnaryOperation.GRAUO_LOG.swigValue()).isGreaterThan(-1);
    assertThat(RasterAlgebraUnaryOperation.GRAUO_LOG10.toString()).isEqualTo("GRAUO_LOG10");
    assertThat(
            RasterAlgebraUnaryOperation.swigToEnum(
                RasterAlgebraUnaryOperation.GRAUO_LOG.swigValue()))
        .isEqualTo(RasterAlgebraUnaryOperation.GRAUO_LOG);
    assertThat(RasterAlgebraBinaryOperation.GRABO_DIV.toString()).isEqualTo("GRABO_DIV");
    assertThat(
            RasterAlgebraBinaryOperation.swigToEnum(
                RasterAlgebraBinaryOperation.GRABO_POW.swigValue()))
        .isEqualTo(RasterAlgebraBinaryOperation.GRABO_POW);
    assertThat(added.equals(added)).isTrue();
    assertThat(added.equals(new Object())).isFalse();
    assertThat(added.hashCode()).isNotEqualTo(0);

    meanN.delete();
    minC.delete();
    minN.delete();
    maxC.delete();
    maxN.delete();
    asInt.delete();
    rev.delete();
    mul2.delete();
    added.delete();
    sqrtPos.delete();
    absB1.delete();
    ds.Close();

    // ColorTable across opaque, single-transparent, and translucent alpha branches + null check.
    ColorTable ctOpaque = new ColorTable();
    ctOpaque.SetColorEntry(0, new Color(255, 0, 0, 255));
    ctOpaque.SetColorEntry(1, new Color(0, 255, 0, 255));
    IndexColorModel cmOpaque = ctOpaque.getIndexColorModel(8);
    assertThat(cmOpaque.hasAlpha()).isFalse();

    ColorTable ctSingleTrans = new ColorTable();
    ctSingleTrans.SetColorEntry(0, new Color(0, 0, 0, 0));
    ctSingleTrans.SetColorEntry(1, new Color(255, 255, 255, 255));
    IndexColorModel cmSingleTrans = ctSingleTrans.getIndexColorModel(8);
    assertThat(cmSingleTrans.getTransparentPixel()).isEqualTo(0);

    ColorTable ctSemiAlpha = new ColorTable();
    ctSemiAlpha.SetColorEntry(0, new Color(10, 20, 30, 128));
    ctSemiAlpha.SetColorEntry(1, new Color(40, 50, 60, 255));
    IndexColorModel cmSemiAlpha = ctSemiAlpha.getIndexColorModel(8);
    assertThat(cmSemiAlpha.hasAlpha()).isTrue();
    assertThat(ctSemiAlpha.clone()).isNotNull();

    IllegalArgumentException ceNull =
        assertThrows(IllegalArgumentException.class, () -> ctOpaque.SetColorEntry(0, null));
    assertThat(ceNull).isNotNull();

    ctSemiAlpha.delete();
    ctSingleTrans.delete();
    ctOpaque.delete();

    // RasterAttributeTable & RATDateTime.
    RATDateTime dt = new RATDateTime();
    dt.setNYear(2026);
    dt.setNMonth(11);
    dt.setNDay(12);
    dt.setNHour(10);
    dt.setNMinute(20);
    dt.setFSecond(30.5f);
    dt.setNTimeZoneHour(0);
    dt.setNTimeZoneMinute(0);
    dt.setBPositiveTimeZone(true);
    dt.setBIsValid(true);
    assertThat(dt.getNYear()).isEqualTo(2026);
    assertThat(dt.getNMonth()).isEqualTo(11);
    assertThat(dt.getNDay()).isEqualTo(12);
    assertThat(dt.getNHour()).isEqualTo(10);
    assertThat(dt.getNMinute()).isEqualTo(20);
    assertThat(dt.getFSecond()).isWithin(1e-5f).of(30.5f);
    assertThat(dt.getNTimeZoneHour()).isEqualTo(0);
    assertThat(dt.getNTimeZoneMinute()).isEqualTo(0);
    assertThat(dt.getBPositiveTimeZone()).isTrue();
    assertThat(dt.getBIsValid()).isTrue();
    assertThat(dt.equals(dt)).isTrue();
    assertThat(dt.equals(new Object())).isFalse();
    assertThat(dt.hashCode()).isNotEqualTo(0);

    RasterAttributeTable rat = new RasterAttributeTable();
    assertThat(rat.CreateColumn("val", gdalconst.GFT_Integer, gdalconst.GFU_Min))
        .isEqualTo(gdalconst.CE_None);
    assertThat(rat.CreateColumn("name", gdalconst.GFT_String, gdalconst.GFU_Name))
        .isEqualTo(gdalconst.CE_None);
    assertThat(rat.CreateColumn("area", gdalconst.GFT_Real, gdalconst.GFU_Generic))
        .isEqualTo(gdalconst.CE_None);
    assertThat(rat.CreateColumn("flag", gdalconst.GFT_Boolean, gdalconst.GFU_Generic))
        .isEqualTo(gdalconst.CE_None);
    assertThat(rat.CreateColumn("ts", gdalconst.GFT_DateTime, gdalconst.GFU_Generic))
        .isEqualTo(gdalconst.CE_None);

    rat.SetRowCount(2);
    assertThat(rat.GetRowCount()).isEqualTo(2);
    assertThat(rat.GetColumnCount()).isEqualTo(5);
    assertThat(rat.GetNameOfCol(1)).isEqualTo("name");
    assertThat(rat.GetUsageOfCol(0)).isEqualTo(gdalconst.GFU_Min);
    assertThat(rat.GetTypeOfCol(2)).isEqualTo(gdalconst.GFT_Real);
    assertThat(rat.GetColOfUsage(gdalconst.GFU_Name)).isEqualTo(1);

    rat.SetValueAsInt(0, 0, 10);
    rat.SetValueAsString(0, 1, "forest");
    rat.SetValueAsDouble(0, 2, 12.5);
    rat.SetValueAsBoolean(0, 3, true);
    rat.SetValueAsDateTime(0, 4, dt);
    assertThat(rat.GetValueAsInt(0, 0)).isEqualTo(10);
    assertThat(rat.GetValueAsString(0, 1)).isEqualTo("forest");
    assertThat(rat.GetValueAsDouble(0, 2)).isWithin(1e-9).of(12.5);
    assertThat(rat.GetValueAsBoolean(0, 3)).isTrue();
    RATDateTime dtRead = rat.GetValueAsDateTime(0, 4);
    assertThat(dtRead.getNYear()).isEqualTo(2026);
    dtRead.delete();

    assertThat(rat.SetLinearBinning(0.0, 5.0)).isEqualTo(gdalconst.CE_None);
    double[] row0Min = new double[1];
    double[] binSize = new double[1];
    assertThat(rat.GetLinearBinning(row0Min, binSize)).isTrue();
    assertThat(row0Min[0]).isEqualTo(0.0);
    assertThat(binSize[0]).isEqualTo(5.0);
    assertThat(rat.GetRowOfValue(7.0)).isEqualTo(1);

    rat.SetTableType(gdalconst.GRTT_THEMATIC);
    assertThat(rat.GetTableType()).isEqualTo(gdalconst.GRTT_THEMATIC);
    assertThat(rat.ChangesAreWrittenToFile()).isAnyOf(0, 1);
    rat.DumpReadable();
    rat.RemoveStatistics();

    RasterAttributeTable ratClone = rat.Clone();
    Object ratJavaClone = rat.clone();
    assertThat(ratClone.GetColumnCount()).isAtLeast(4);
    assertThat(((RasterAttributeTable) ratJavaClone).GetRowCount()).isEqualTo(2);

    Dataset ratDs = gdal.GetDriverByName("MEM").Create("", 2, 2, 1, gdalconst.GDT_Byte);
    assertThat(ratDs.GetRasterBand(1).SetDefaultRAT(rat)).isEqualTo(gdalconst.CE_None);
    assertThat(ratDs.GetRasterBand(1).GetDefaultRAT().GetColumnCount()).isAtLeast(4);
    ratDs.Close();

    ((RasterAttributeTable) ratJavaClone).delete();
    ratClone.delete();
    rat.delete();
    dt.delete();
  }
}
