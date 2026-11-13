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

import java.util.List;
import java.util.Vector;
import org.gdal.GoogleGdalLoader;
import org.gdal.gdalconst.gdalconst;
import org.gdal.osr.SpatialReference;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;

public final class MultiDimensionalTest {

  private static final String VSIMEM_DIR = "/vsimem/MultiDimensionalTest";

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
  public void extendedDataTypeAndComponent_exposeMetadataAndEnums() {
    ExtendedDataType floatType = ExtendedDataType.Create(gdalconst.GDT_Float32);
    assertThat(floatType.GetName()).isEmpty();
    assertThat(floatType.GetClass()).isEqualTo(ExtendedDataTypeClass.GEDTC_NUMERIC);
    assertThat(floatType.GetNumericDataType()).isEqualTo(gdalconst.GDT_Float32);
    assertThat(floatType.GetSize()).isEqualTo(4L);
    assertThat(floatType.GetSubType()).isEqualTo(ExtendedDataTypeSubType.GEDTST_NONE);
    assertThat(floatType.GetRAT()).isNull();
    assertThat(floatType.CanConvertTo(ExtendedDataType.Create(gdalconst.GDT_Float64))).isTrue();
    assertThat(floatType.Equals(floatType)).isTrue();
    assertThat(floatType.equals(floatType)).isTrue();
    assertThat(floatType.equals(new Object())).isFalse();
    assertThat(floatType.hashCode()).isNotEqualTo(0);

    ExtendedDataType str0 = ExtendedDataType.CreateString();
    ExtendedDataType str1 = ExtendedDataType.CreateString(16L);
    ExtendedDataType str2 = ExtendedDataType.CreateString(32L, ExtendedDataTypeSubType.GEDTST_JSON);
    assertThat(str0.GetClass()).isEqualTo(ExtendedDataTypeClass.GEDTC_STRING);
    assertThat(str1.GetMaxStringLength()).isEqualTo(16L);
    assertThat(str2.GetSubType()).isEqualTo(ExtendedDataTypeSubType.GEDTST_JSON);

    EDTComponent comp = EDTComponent.Create("field_a", 0L, floatType);
    assertThat(comp.GetName()).isEqualTo("field_a");
    assertThat(comp.GetOffset()).isEqualTo(0L);
    assertThat(comp.GetType().GetNumericDataType()).isEqualTo(gdalconst.GDT_Float32);
    assertThat(comp.equals(comp)).isTrue();
    assertThat(comp.equals(new Object())).isFalse();
    assertThat(comp.hashCode()).isNotEqualTo(0);

    assertThat(ExtendedDataTypeClass.GEDTC_NUMERIC.swigValue()).isEqualTo(0);
    assertThat(ExtendedDataTypeClass.GEDTC_COMPOUND.toString()).isEqualTo("GEDTC_COMPOUND");
    assertThat(ExtendedDataTypeClass.swigToEnum(ExtendedDataTypeClass.GEDTC_STRING.swigValue()))
        .isEqualTo(ExtendedDataTypeClass.GEDTC_STRING);
    assertThat(ExtendedDataTypeSubType.GEDTST_JSON.swigValue()).isGreaterThan(0);
    assertThat(ExtendedDataTypeSubType.GEDTST_NONE.toString()).isEqualTo("GEDTST_NONE");
    assertThat(ExtendedDataTypeSubType.swigToEnum(ExtendedDataTypeSubType.GEDTST_JSON.swigValue()))
        .isEqualTo(ExtendedDataTypeSubType.GEDTST_JSON);

    comp.delete();
    str2.delete();
    str1.delete();
    str0.delete();
    floatType.delete();
  }

  @Test
  @SuppressWarnings({"unchecked", "JdkObsolete"})
  public void groupDimensionAttributeAndMdArray_readWriteAndDerivedViews() {
    gdal.MkdirRecursive(VSIMEM_DIR, 0755);
    Driver memDriver = gdal.GetDriverByName("MEM");
    Dataset ds = memDriver.CreateMultiDimensional("md_mem");
    memDriver.CreateMultiDimensional("md_opts", new Vector<String>()).Close();
    memDriver
        .CreateMultiDimensional("md_opts2", new Vector<String>(), new Vector<String>())
        .Close();

    Group root = ds.GetRootGroup();
    assertThat(root.GetName()).isEqualTo("/");
    assertThat(root.GetFullName()).isEqualTo("/");
    assertThat(root.equals(root)).isTrue();
    assertThat(root.equals(new Object())).isFalse();
    assertThat(root.hashCode()).isNotEqualTo(0);

    Group sub1 = root.CreateGroup("sub");
    Group sub2 = root.CreateGroup("sub_opts", new Vector<String>());
    assertThat((List<String>) root.GetGroupNames()).containsAtLeast("sub", "sub_opts");
    assertThat((List<String>) root.GetGroupNames(new Vector<String>())).contains("sub");
    assertThat(root.OpenGroup("sub").GetFullName()).isEqualTo("/sub");
    assertThat(root.OpenGroup("sub", new Vector<String>()).GetName()).isEqualTo("sub");
    assertThat(root.OpenGroupFromFullname("/sub").GetName()).isEqualTo("sub");
    assertThat(root.OpenGroupFromFullname("/sub", new Vector<String>()).GetName()).isEqualTo("sub");
    assertThat(root.GetStructuralInfo()).isNotNull();
    assertThat(root.GetDataTypeCount()).isEqualTo(0L);

    Dimension dimY = root.CreateDimension("y", gdalconst.DIM_TYPE_HORIZONTAL_Y, "NORTH", 4L);
    Dimension dimX =
        root.CreateDimension(
            "x", gdalconst.DIM_TYPE_HORIZONTAL_X, "EAST", 4L, new Vector<String>());
    assertThat(dimY.GetName()).isEqualTo("y");
    assertThat(dimY.GetFullName()).isEqualTo("/y");
    assertThat(dimY.GetType()).isEqualTo(gdalconst.DIM_TYPE_HORIZONTAL_Y);
    assertThat(dimY.GetDirection()).isEqualTo("NORTH");
    assertThat(dimY.GetSize()).isEqualTo(4L);
    assertThat(dimY.equals(dimY)).isTrue();
    assertThat(dimY.equals(new Object())).isFalse();
    assertThat(dimY.hashCode()).isNotEqualTo(0);

    ExtendedDataType dtFloat64 = ExtendedDataType.Create(gdalconst.GDT_Float64);
    MDArray yCoord = root.CreateMDArray("y", new Dimension[] {dimY}, dtFloat64);
    yCoord.Write(new long[] {0L}, new long[] {4L}, new double[] {3.0, 2.0, 1.0, 0.0});
    assertThat(dimY.SetIndexingVariable(yCoord)).isTrue();
    assertThat(dimY.GetIndexingVariable().GetName()).isEqualTo("y");

    MDArray xCoord =
        root.CreateMDArray("x", new Dimension[] {dimX}, dtFloat64, new Vector<String>());
    xCoord.Write(new long[] {0L}, new long[] {4L}, new double[] {0.0, 1.0, 2.0, 3.0});
    assertThat(dimX.SetIndexingVariable(xCoord)).isTrue();

    MDArray arr = sub1.CreateMDArray("temp", new Dimension[] {dimY, dimX}, dtFloat64);
    assertThat(arr.GetName()).isEqualTo("temp");
    assertThat(arr.GetFullName()).isEqualTo("/sub/temp");
    assertThat(arr.GetTotalElementsCount()).isEqualTo(16L);
    assertThat(arr.GetDimensionCount()).isEqualTo(2L);
    assertThat(arr.GetDimension(0).GetName()).isEqualTo("y");
    assertThat(arr.GetDimensions()).hasLength(2);
    assertThat(arr.GetDataType().GetNumericDataType()).isEqualTo(gdalconst.GDT_Float64);
    assertThat(arr.GetStructuralInfo()).isNotNull();

    assertThat((List<String>) sub1.GetMDArrayNames()).contains("temp");
    assertThat((List<String>) sub1.GetMDArrayNames(new Vector<String>())).contains("temp");
    assertThat((List<String>) root.GetMDArrayFullNamesRecursive()).contains("/sub/temp");
    assertThat((List<String>) root.GetMDArrayFullNamesRecursive(new Vector<String>()))
        .contains("/sub/temp");
    assertThat(
            (List<String>)
                root.GetMDArrayFullNamesRecursive(new Vector<String>(), new Vector<String>()))
        .contains("/sub/temp");
    assertThat(sub1.OpenMDArray("temp").GetName()).isEqualTo("temp");
    assertThat(sub1.OpenMDArray("temp", new Vector<String>()).GetName()).isEqualTo("temp");
    assertThat(root.OpenMDArrayFromFullname("/sub/temp").GetName()).isEqualTo("temp");
    assertThat(root.OpenMDArrayFromFullname("/sub/temp", new Vector<String>()).GetName())
        .isEqualTo("temp");
    assertThat(root.ResolveMDArray("temp", "/sub").GetFullName()).isEqualTo("/sub/temp");
    assertThat(root.ResolveMDArray("temp", "/sub", new Vector<String>()).GetFullName())
        .isEqualTo("/sub/temp");

    // Attributes on Group & MDArray.
    ExtendedDataType dtStr = ExtendedDataType.CreateString();
    Attribute gAttr = root.CreateAttribute("title", new long[0], dtStr);
    assertThat(gAttr.WriteString("hello")).isEqualTo(gdalconst.CE_None);
    assertThat(gAttr.ReadAsString()).isEqualTo("hello");
    assertThat(root.GetAttribute("title").GetFullName()).contains("title");

    Vector<String> attrOpts = new Vector<>();
    attrOpts.add("DUMMY=1");
    Attribute aStrArr = sub1.CreateAttribute("tags", new long[] {2L}, dtStr, attrOpts);
    Vector<String> tags = new Vector<>();
    tags.add("t1");
    tags.add("t2");
    assertThat(aStrArr.WriteStringArray(tags)).isEqualTo(gdalconst.CE_None);
    assertThat((List<String>) aStrArr.ReadAsStringArray()).containsExactly("t1", "t2").inOrder();
    assertThat(aStrArr.GetDimensionCount()).isEqualTo(1L);
    assertThat(aStrArr.GetDimensionSize(0)).isEqualTo(2L);
    assertThat(aStrArr.GetDimensionSizes()).isEqualTo(new long[] {2L});
    assertThat(aStrArr.GetTotalElementsCount()).isEqualTo(2L);
    assertThat(aStrArr.GetDataType().GetClass()).isEqualTo(ExtendedDataTypeClass.GEDTC_STRING);

    Attribute aInt =
        arr.CreateAttribute("int_attr", new long[0], ExtendedDataType.Create(gdalconst.GDT_Int32));
    assertThat(aInt.WriteInt(42)).isEqualTo(gdalconst.CE_None);
    assertThat(aInt.ReadAsInt()).isEqualTo(42);
    Attribute aInt64 =
        arr.CreateAttribute(
            "int64_attr", new long[0], ExtendedDataType.Create(gdalconst.GDT_Int64), attrOpts);
    assertThat(aInt64.WriteInt64(9_000_000_000L)).isEqualTo(gdalconst.CE_None);
    assertThat(aInt64.ReadAsInt64()).isEqualTo(9_000_000_000L);
    Attribute aDbl = arr.CreateAttribute("dbl_attr", new long[0], dtFloat64);
    assertThat(aDbl.WriteDouble(3.5)).isEqualTo(gdalconst.CE_None);
    assertThat(aDbl.ReadAsDouble()).isWithin(1e-9).of(3.5);
    assertThat(arr.GetAttribute("dbl_attr")).isNotNull();

    // NoData, Offset, Scale, Unit, SRS, Stats, View, Transpose, Unscaled, Mask, AsClassicDataset.
    Double[] unsetNoData = new Double[1];
    arr.GetNoDataValueAsDouble(unsetNoData);
    assertThat(unsetNoData[0]).isNull();
    assertThat(arr.SetNoDataValueDouble(-999.0)).isEqualTo(gdalconst.CE_None);
    Double[] ndOut = new Double[1];
    arr.GetNoDataValueAsDouble(ndOut);
    assertThat(ndOut[0]).isEqualTo(-999.0);
    assertThat(arr.DeleteNoDataValue()).isEqualTo(gdalconst.CE_None);

    // SetNoDataValueString applies to String-typed arrays (returns CE_Failure on Float64).
    gdal.DontUseExceptions();
    assertThat(arr.SetNoDataValueString("-999")).isEqualTo(gdalconst.CE_Failure);
    gdal.UseExceptions();
    MDArray strArr = root.CreateMDArray("str_arr", new Dimension[] {dimX}, dtStr);
    assertThat(strArr.SetNoDataValueString("NA")).isEqualTo(gdalconst.CE_None);
    assertThat(strArr.GetNoDataValueAsString()).isEqualTo("NA");
    assertThat(strArr.DeleteNoDataValue()).isEqualTo(gdalconst.CE_None);
    strArr.delete();

    assertThat(arr.SetOffset(1.0)).isEqualTo(gdalconst.CE_None);
    assertThat(arr.SetOffset(1.0, gdalconst.GDT_Float64)).isEqualTo(gdalconst.CE_None);
    assertThat(arr.SetScale(2.0)).isEqualTo(gdalconst.CE_None);
    assertThat(arr.SetScale(2.0, gdalconst.GDT_Float64)).isEqualTo(gdalconst.CE_None);
    Double[] off = new Double[1];
    Double[] scl = new Double[1];
    arr.GetOffset(off);
    arr.GetScale(scl);
    assertThat(off[0]).isEqualTo(1.0);
    assertThat(scl[0]).isEqualTo(2.0);
    assertThat(arr.GetOffsetStorageType()).isEqualTo(gdalconst.GDT_Float64);
    assertThat(arr.GetScaleStorageType()).isEqualTo(gdalconst.GDT_Float64);
    assertThat(arr.SetUnit("K")).isEqualTo(gdalconst.CE_None);
    assertThat(arr.GetUnit()).isEqualTo("K");

    SpatialReference wgs84 = new SpatialReference();
    wgs84.ImportFromEPSG(4326);
    assertThat(arr.SetSpatialRef(wgs84)).isEqualTo(gdalconst.CE_None);
    assertThat(arr.GetSpatialRef().IsGeographic()).isEqualTo(1);

    // Exercise ALL 36 Write/Read overloads (byte, short, int, long, float, double x 3/4/5-arg).
    long[] st = new long[] {0L, 0L};
    long[] cnt = new long[] {2L, 2L};
    long[] step = new long[] {1L, 1L};
    long[] strd = new long[] {2L, 1L};

    byte[] mb = new byte[4];
    assertThat(arr.Write(st, cnt, mb)).isTrue();
    assertThat(arr.Write(st, cnt, step, mb)).isTrue();
    assertThat(arr.Write(st, cnt, step, strd, mb)).isTrue();
    assertThat(arr.Read(st, cnt, mb)).isTrue();
    assertThat(arr.Read(st, cnt, step, mb)).isTrue();
    assertThat(arr.Read(st, cnt, step, strd, mb)).isTrue();

    short[] ms = new short[4];
    assertThat(arr.Write(st, cnt, ms)).isTrue();
    assertThat(arr.Write(st, cnt, step, ms)).isTrue();
    assertThat(arr.Write(st, cnt, step, strd, ms)).isTrue();
    assertThat(arr.Read(st, cnt, ms)).isTrue();
    assertThat(arr.Read(st, cnt, step, ms)).isTrue();
    assertThat(arr.Read(st, cnt, step, strd, ms)).isTrue();

    int[] mi = new int[4];
    assertThat(arr.Write(st, cnt, mi)).isTrue();
    assertThat(arr.Write(st, cnt, step, mi)).isTrue();
    assertThat(arr.Write(st, cnt, step, strd, mi)).isTrue();
    assertThat(arr.Read(st, cnt, mi)).isTrue();
    assertThat(arr.Read(st, cnt, step, mi)).isTrue();
    assertThat(arr.Read(st, cnt, step, strd, mi)).isTrue();

    long[] ml = new long[4];
    assertThat(arr.Write(st, cnt, ml)).isTrue();
    assertThat(arr.Write(st, cnt, step, ml)).isTrue();
    assertThat(arr.Write(st, cnt, step, strd, ml)).isTrue();
    assertThat(arr.Read(st, cnt, ml)).isTrue();
    assertThat(arr.Read(st, cnt, step, ml)).isTrue();
    assertThat(arr.Read(st, cnt, step, strd, ml)).isTrue();

    float[] mf = new float[4];
    assertThat(arr.Write(st, cnt, mf)).isTrue();
    assertThat(arr.Write(st, cnt, step, mf)).isTrue();
    assertThat(arr.Write(st, cnt, step, strd, mf)).isTrue();
    assertThat(arr.Read(st, cnt, mf)).isTrue();
    assertThat(arr.Read(st, cnt, step, mf)).isTrue();
    assertThat(arr.Read(st, cnt, step, strd, mf)).isTrue();

    double[] mdIn = new double[] {1.0, 2.0, 3.0, 4.0};
    double[] mdOut = new double[4];
    assertThat(arr.Write(st, cnt, mdIn)).isTrue();
    assertThat(arr.Write(st, cnt, step, mdIn)).isTrue();
    assertThat(arr.Write(st, cnt, step, strd, mdIn)).isTrue();
    assertThat(arr.Read(st, cnt, mdOut)).isTrue();
    assertThat(mdOut).usingExactEquality().containsExactly(1.0, 2.0, 3.0, 4.0);
    assertThat(arr.Read(st, cnt, step, mdOut)).isTrue();
    assertThat(arr.Read(st, cnt, step, strd, mdOut)).isTrue();

    // Statistics & Statistics class.
    ProgressCallback cb = new ProgressCallback();
    Statistics st0 = arr.ComputeStatistics();
    assertThat(st0.getMax()).isWithin(1e-6).of(4.0);
    assertThat(st0.getMin()).isWithin(1e-6).of(0.0);
    assertThat(st0.getMean()).isAtLeast(0.0);
    assertThat(st0.getStd_dev()).isAtLeast(0.0);
    assertThat(st0.getValid_count()).isEqualTo(16L);
    assertThat(st0.equals(st0)).isTrue();
    assertThat(st0.equals(new Object())).isFalse();
    assertThat(st0.hashCode()).isNotEqualTo(0);
    Statistics stEmpty = new Statistics();
    stEmpty.delete();
    st0.delete();
    arr.ComputeStatistics(false).delete();
    arr.ComputeStatistics(false, cb).delete();
    arr.ComputeStatistics(false, cb, new Vector<String>()).delete();
    arr.GetStatistics().delete();
    arr.GetStatistics(false).delete();
    arr.GetStatistics(false, true).delete();
    arr.GetStatistics(false, true, cb).delete();

    // Views, Transpose, Unscaled, Mask, Gridded, AsClassicDataset.
    MDArray view = arr.GetView("[0:2, 0:2]");
    assertThat(view.GetDimension(0).GetSize()).isEqualTo(2L);
    MDArray transposed = arr.Transpose(new int[] {1, 0});
    assertThat(transposed.GetDimension(0).GetName()).isEqualTo("x");
    MDArray unscaled = arr.GetUnscaled();
    assertThat(unscaled).isNotNull();
    MDArray mask0 = arr.GetMask();
    MDArray mask1 = arr.GetMask(new Vector<String>());
    assertThat(mask0).isNotNull();
    assertThat(mask1).isNotNull();

    Dataset classic1 = arr.AsClassicDataset(1L, 0L);
    assertThat(classic1.GetRasterXSize()).isEqualTo(4);
    classic1.Close();
    Dataset classic2 = arr.AsClassicDataset(1L, 0L, root);
    classic2.Close();
    Dataset classic3 = arr.AsClassicDataset(1L, 0L, root, new Vector<String>());
    classic3.Close();

    // Cache, GetGridded, Subset, VectorLayer, Resize, Rename & Delete coverage.
    Vector<String> dummyOpts = new Vector<>();
    dummyOpts.add("DUMMY=1");
    assertThat(root.GetDataType(0L)).isNull();
    assertThat((List<String>) root.GetVectorLayerNames()).isNotNull();
    assertThat((List<String>) root.GetVectorLayerNames(dummyOpts)).isNotNull();

    gdal.DontUseExceptions();
    assertThat(root.OpenVectorLayer("missing_layer")).isNull();
    assertThat(root.OpenVectorLayer("missing_layer", dummyOpts)).isNull();
    Group subSel0 = root.SubsetDimensionFromSelection("x(0:2)");
    if (subSel0 != null) {
      subSel0.delete();
    }
    Group subSel1 = root.SubsetDimensionFromSelection("x(0:2)", dummyOpts);
    if (subSel1 != null) {
      subSel1.delete();
    }
    arr.Cache();
    arr.Cache(dummyOpts);
    gdal.UseExceptions();
    gdal.ErrorReset();

    // 4 overloads of MDArray.GetGridded on 1D coordinates & values.
    Dimension dimPts = root.CreateDimension("pts", "", "", 4L);
    MDArray gx = root.CreateMDArray("gx", new Dimension[] {dimPts}, dtFloat64);
    MDArray gy = root.CreateMDArray("gy", new Dimension[] {dimPts}, dtFloat64);
    MDArray gv = root.CreateMDArray("gv", new Dimension[] {dimPts}, dtFloat64);
    gx.Write(new long[] {0L}, new long[] {4L}, new double[] {0.0, 1.0, 0.0, 1.0});
    gy.Write(new long[] {0L}, new long[] {4L}, new double[] {0.0, 0.0, 1.0, 1.0});
    gv.Write(new long[] {0L}, new long[] {4L}, new double[] {10.0, 20.0, 30.0, 40.0});
    Vector<String> gridOpts = new Vector<>();
    gridOpts.add("RESAMPLING=NEAREST");
    MDArray gr4 = gv.GetGridded("nearest:radius1=5:radius2=5", gx, gy, gridOpts);
    if (gr4 != null) {
      gr4.delete();
    }
    MDArray gr3 = gv.GetGridded("nearest:radius1=5:radius2=5", gx, gy);
    if (gr3 != null) {
      gr3.delete();
    }
    gdal.DontUseExceptions();
    MDArray gr2 = gv.GetGridded("nearest:radius1=5:radius2=5", gx);
    if (gr2 != null) {
      gr2.delete();
    }
    MDArray gr1 = gv.GetGridded("nearest:radius1=5:radius2=5");
    if (gr1 != null) {
      gr1.delete();
    }
    gdal.UseExceptions();
    gdal.ErrorReset();
    gv.delete();
    gy.delete();
    gx.delete();
    dimPts.delete();

    // Cache, Resize, Rename & Delete on Zarr.
    String zarrPath = VSIMEM_DIR + "/test.zarr";
    Vector<String> zarrDsOpts = new Vector<>();
    zarrDsOpts.add("FORMAT=ZARR_V2");
    Dataset zarrDs = gdal.GetDriverByName("Zarr").CreateMultiDimensional(zarrPath, zarrDsOpts);
    Group zRoot = zarrDs.GetRootGroup();
    Dimension zDim = zRoot.CreateDimension("d", "", "", 2L);
    MDArray zArr = zRoot.CreateMDArray("v", new Dimension[] {zDim}, dtFloat64, dummyOpts);
    assertThat(zArr.Resize(new long[] {4L})).isEqualTo(gdalconst.CE_None);
    assertThat(zArr.Resize(new long[] {5L}, dummyOpts)).isEqualTo(gdalconst.CE_None);
    assertThat(zArr.Rename("v_renamed")).isEqualTo(gdalconst.CE_None);

    Group zg1 = zRoot.CreateGroup("zg1");
    Group zg2 = zRoot.CreateGroup("zg2");
    Attribute ga1 = zRoot.CreateAttribute("ga1", new long[0], dtStr);
    assertThat(ga1.GetName()).isEqualTo("ga1");
    Attribute ga2 = zRoot.CreateAttribute("ga2", new long[0], dtStr);
    Attribute aa1 = zArr.CreateAttribute("aa1", new long[0], dtStr);
    Attribute aa2 = zArr.CreateAttribute("aa2", new long[0], dtStr);
    MDArray zaTmp1 = zRoot.CreateMDArray("za1", new Dimension[] {zDim}, dtFloat64);
    MDArray zaTmp2 = zRoot.CreateMDArray("za2", new Dimension[] {zDim}, dtFloat64);
    gdal.DontUseExceptions();
    zg1.Rename("zg1_ren");
    ga1.Rename("ga1_ren");
    zDim.Rename("d_ren");
    zArr.Cache();
    zArr.Cache(dummyOpts);
    ga2.delete();
    ga1.delete();
    aa2.delete();
    aa1.delete();
    zg2.delete();
    zg1.delete();
    zaTmp2.delete();
    zaTmp1.delete();
    zRoot.DeleteAttribute("ga1_ren");
    zRoot.DeleteAttribute("ga2", dummyOpts);
    zArr.DeleteAttribute("aa1");
    zArr.DeleteAttribute("aa2", dummyOpts);
    zRoot.DeleteGroup("zg1_ren");
    zRoot.DeleteGroup("zg2", dummyOpts);
    zRoot.DeleteMDArray("za1");
    zRoot.DeleteMDArray("za2", dummyOpts);
    gdal.UseExceptions();
    gdal.ErrorReset();

    zArr.delete();
    zDim.delete();
    zRoot.delete();
    zarrDs.Close();

    mask1.delete();
    mask0.delete();
    unscaled.delete();
    transposed.delete();
    view.delete();
    wgs84.delete();
    cb.delete();
    aDbl.delete();
    aInt64.delete();
    aInt.delete();
    aStrArr.delete();
    gAttr.delete();
    arr.delete();
    xCoord.delete();
    yCoord.delete();
    dtStr.delete();
    dtFloat64.delete();
    dimX.delete();
    dimY.delete();
    sub2.delete();
    sub1.delete();
    root.delete();
    ds.Close();
  }
}
