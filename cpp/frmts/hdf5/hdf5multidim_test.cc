// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests the HDF5 multidimensional raster API implementation (hdf5multidim.cpp).
// Note: In Google3 builds, SpecialCaseS100 2D grid delegation is disabled via
// '#if 0 // GOOGLE MODIFICATION' in hdf5multidim.cpp; Group_F nodata parsing is
// exercised on 0D datasets right before that block.
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/hdf5/hdf5multidim.cpp
//   https://gdal.org/drivers/raster/hdf5.html

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>  // NOLINT
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/frmts/hdf5/hdf5_test_util.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/hdf5/hdf5_api.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_multidim.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/hdf5/src/H5Apublic.h"
#include "third_party/hdf5/src/H5Ipublic.h"
#include "third_party/hdf5/src/H5Rpublic.h"
#include "third_party/hdf5/src/H5Spublic.h"
#include "third_party/hdf5/src/H5Tpublic.h"
#include "third_party/hdf5/src/H5version.h"

namespace autotest2 {
namespace {

using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::UnorderedElementsAre;

GDALDatasetUniquePtr OpenMultiDimDataset(const std::string& path_or_dsn) {
  return GDALDatasetUniquePtr(GDALDataset::Open(
      path_or_dsn.c_str(), GDAL_OF_MULTIDIM_RASTER | GDAL_OF_READONLY, nullptr,
      nullptr, nullptr));
}

void WritePaddedStringListAttr(hid_t obj_id, const char* name, size_t str_len,
                               std::initializer_list<const char*> vals) {
  std::vector<char> buf(vals.size() * str_len, '\0');
  size_t idx = 0;
  for (const char* s : vals) {
    const size_t n = std::min(str_len, std::strlen(s));
    std::memcpy(buf.data() + idx * str_len, s, n);
    ++idx;
  }
  ASSERT_TRUE(Write1DFixedStringAttr(obj_id, name, str_len, buf));
}

struct SimpleCompStruct {
  int32_t id;
  double val;
};

struct NestedSub {
  int32_t x;
  double y;
};

struct PaddedVlenOuter {
  uint8_t tag;
  NestedSub sub;
  const char* label;
};

TEST(Hdf5MultiDimTest, OpenHierarchyCyclesDimensionsAttributesAndLayout) {
  ScopedHdf5DriverManagerCleanup cleanup_dm;
  GDALRegister_HDF5();

  // 1. Opening non-existent path returns nullptr; invalid HDFEOS INFORMATION
  //    falls back cleanly without failing root group open.
  {
    ScopedQuietHdf5Errors quiet;
    EXPECT_EQ(nullptr, OpenMultiDimDataset("/does_not_exist/multidim.h5"));
  }

  ScopedVsimemFile invalid_eos("invalid_eos.h5");
  {
    ScopedH5File fid(CreateVsimemHdf5File(invalid_eos.path()));
    ASSERT_GE(fid.get(), 0);
    ScopedH5Group info_g(H5Gcreate(fid, "HDFEOS INFORMATION", 0));
    ASSERT_GE(info_g.get(), 0);
  }
  {
    ScopedQuietHdf5Errors quiet;
    auto ds = OpenMultiDimDataset(std::format("HDF5:{}", invalid_eos.path()));
    ASSERT_NE(nullptr, ds);
    auto root = ds->GetRootGroup();
    ASSERT_NE(nullptr, root);
    EXPECT_EQ("/", root->GetFullName());
    EXPECT_EQ(nullptr, root->OpenGroup("NoSuchGroup"));
    EXPECT_EQ(nullptr, root->OpenMDArray("NoSuchArray"));
    EXPECT_EQ(nullptr, root->GetAttribute("NoSuchAttr"));
  }

  // 2. Full hierarchy: cycle link, committed compound type, DIMENSION_SCALE
  //    (both indexing variable and netCDF non-variable), DIMENSION_LIST
  //    (in-group, root-level, and sibling-group refs), DIMENSION_LABELS,
  //    hidden/shown attributes, out-of-range _FillValue warning, units,
  //    0D/contiguous/chunked+deflate+shuffle+fletcher32 structural info.
  ScopedVsimemFile h5file("hierarchy.h5");
  {
    ScopedH5File fid(CreateVsimemHdf5File(h5file.path()));
    ASSERT_GE(fid.get(), 0);

    // Root attributes (_Netcdf4Dimid and _NCProperties filtered by default).
    ASSERT_TRUE(
        WriteScalarAttr<float>(fid, "_Netcdf4Dimid", H5T_NATIVE_FLOAT, 0.0f));
    ASSERT_TRUE(WriteScalarStringAttr(fid, "_NCProperties", "version=2",
                                      H5T_STR_NULLPAD));
    ASSERT_TRUE(
        WriteScalarStringAttr(fid, "title", "root_title", H5T_STR_NULLPAD));

    // Unsupported object-reference attribute & dataset (ignored/nullptr).
    {
      hobj_ref_t self_ref{};
      ASSERT_GE(H5Rcreate(&self_ref, fid, "/", H5R_OBJECT, H5I_INVALID_HID), 0);
      ScopedH5Dataspace sp(H5Screate(H5S_SCALAR));
      ASSERT_GE(sp.get(), 0);
      ScopedH5Attribute attr(
          H5Acreate(fid, "obj_ref_attr", H5T_STD_REF_OBJ, sp, H5P_DEFAULT));
      ASSERT_GE(attr.get(), 0);
      ASSERT_GE(H5Awrite(attr, H5T_STD_REF_OBJ, &self_ref), 0);
      ScopedH5Dataset ref_ds(
          H5Dcreate(fid, "obj_ref_ds", H5T_STD_REF_OBJ, sp, H5P_DEFAULT));
      ASSERT_GE(ref_ds.get(), 0);
    }

    // Root-level dimension dataset & sibling group dimension dataset.
    const hsize_t dim4 = 4;
    ScopedH5Dataspace sp4(H5Screate_simple(1, &dim4, nullptr));
    ASSERT_GE(sp4.get(), 0);
    const double root_scale_vals[4] = {10.0, 20.0, 30.0, 40.0};
    {
      ScopedH5Dataset root_scale(
          H5Dcreate(fid, "root_scale", H5T_NATIVE_DOUBLE, sp4, H5P_DEFAULT));
      ASSERT_GE(root_scale.get(), 0);
      ASSERT_GE(H5Dwrite(root_scale, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL,
                         H5P_DEFAULT, root_scale_vals),
                0);
    }

    {
      ScopedH5Group other_grp(H5Gcreate(fid, "other_grp", 0));
      ASSERT_GE(other_grp.get(), 0);
      const hsize_t dim2 = 2;
      ScopedH5Dataspace sp2(H5Screate_simple(1, &dim2, nullptr));
      ASSERT_GE(sp2.get(), 0);
      ScopedH5Dataset sub_scale(
          H5Dcreate(other_grp, "sub_scale", H5T_NATIVE_INT, sp2, H5P_DEFAULT));
      ASSERT_GE(sub_scale.get(), 0);
    }

    // Main test subgroup /grp with a cycle link back to "/" and a child group.
    ScopedH5Group grp(H5Gcreate(fid, "grp", 0));
    ASSERT_GE(grp.get(), 0);
    {
      ScopedH5Group child(H5Gcreate(grp, "child_grp", 0));
      ASSERT_GE(child.get(), 0);
    }
    ASSERT_GE(H5Lcreate_hard(fid, "/", grp, "cycle_to_root", H5P_DEFAULT,
                             H5P_DEFAULT),
              0);

    // Committed datatype "MyStruct" in /grp.
    {
      ScopedH5Datatype comp_t(
          H5Tcreate(H5T_COMPOUND, sizeof(SimpleCompStruct)));
      ASSERT_GE(comp_t.get(), 0);
      ASSERT_GE(H5Tinsert(comp_t, "id", offsetof(SimpleCompStruct, id),
                          H5T_NATIVE_INT32),
                0);
      ASSERT_GE(H5Tinsert(comp_t, "val", offsetof(SimpleCompStruct, val),
                          H5T_NATIVE_DOUBLE),
                0);
      ASSERT_GE(H5Tcommit2(grp, "MyStruct", comp_t, H5P_DEFAULT, H5P_DEFAULT,
                           H5P_DEFAULT),
                0);
      ScopedH5Dataspace sc(H5Screate(H5S_SCALAR));
      ASSERT_GE(sc.get(), 0);
      ScopedH5Dataset typed_ds(
          H5Dcreate(grp, "typed_ds", comp_t, sc, H5P_DEFAULT));
      ASSERT_GE(typed_ds.get(), 0);
      SimpleCompStruct item{.id = 7, .val = 3.25};
      ASSERT_GE(
          H5Dwrite(typed_ds, comp_t, H5S_ALL, H5S_ALL, H5P_DEFAULT, &item), 0);
      ScopedH5Attribute typed_attr(
          H5Acreate(typed_ds, "typed_attr", comp_t, sc, H5P_DEFAULT));
      ASSERT_GE(typed_attr.get(), 0);
      ASSERT_GE(H5Awrite(typed_attr, comp_t, &item), 0);
    }

    // XDim: DIMENSION_SCALE without netCDF non-variable NAME (indexing var).
    {
      ScopedH5Dataset xdim(
          H5Dcreate(grp, "XDim", H5T_NATIVE_DOUBLE, sp4, H5P_DEFAULT));
      ASSERT_GE(xdim.get(), 0);
      ASSERT_TRUE(WriteScalarStringAttr(xdim, "CLASS", "DIMENSION_SCALE",
                                        H5T_STR_NULLPAD));
      ASSERT_TRUE(WriteScalarStringAttr(xdim, "NAME", "XDim", H5T_STR_NULLPAD));
      ASSERT_GE(H5Dwrite(xdim, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                         root_scale_vals),
                0);
    }

    // YDim: DIMENSION_SCALE with netCDF "not a netCDF variable" NAME attr.
    const hsize_t dim3 = 3;
    ScopedH5Dataspace sp3(H5Screate_simple(1, &dim3, nullptr));
    ASSERT_GE(sp3.get(), 0);
    {
      ScopedH5Dataset ydim(
          H5Dcreate(grp, "YDim", H5T_NATIVE_DOUBLE, sp3, H5P_DEFAULT));
      ASSERT_GE(ydim.get(), 0);
      ASSERT_TRUE(WriteScalarStringAttr(ydim, "CLASS", "DIMENSION_SCALE",
                                        H5T_STR_NULLPAD));
      ASSERT_TRUE(WriteScalarStringAttr(
          ydim, "NAME",
          "This is a netCDF dimension but not a netCDF variable 123",
          H5T_STR_NULLPAD));
    }

    // 1D non-dimension-scale dataset (CLASS != "DIMENSION_SCALE").
    {
      ScopedH5Dataset non_dim(
          H5Dcreate(grp, "non_dim", H5T_NATIVE_INT, sp3, H5P_DEFAULT));
      ASSERT_GE(non_dim.get(), 0);
      ASSERT_TRUE(WriteScalarStringAttr(non_dim, "CLASS", "NOT_A_DIMENSION",
                                        H5T_STR_NULLPAD));
    }

    // 3D dataset using DIMENSION_LIST referencing YDim, /root_scale,
    // and /other_grp/sub_scale.
    {
      const hsize_t dims3d[3] = {3, 4, 2};
      ScopedH5Dataspace sp3d(H5Screate_simple(3, dims3d, nullptr));
      ASSERT_GE(sp3d.get(), 0);
      ScopedH5Dataset d_list(
          H5Dcreate(grp, "data_dim_list", H5T_NATIVE_INT32, sp3d, H5P_DEFAULT));
      ASSERT_GE(d_list.get(), 0);
      hobj_ref_t r0{};
      hobj_ref_t r1{};
      hobj_ref_t r2{};
      ASSERT_GE(H5Rcreate(&r0, fid, "/grp/YDim", H5R_OBJECT, H5I_INVALID_HID),
                0);
      ASSERT_GE(H5Rcreate(&r1, fid, "/root_scale", H5R_OBJECT, H5I_INVALID_HID),
                0);
      ASSERT_GE(H5Rcreate(&r2, fid, "/other_grp/sub_scale", H5R_OBJECT,
                          H5I_INVALID_HID),
                0);
      hvl_t vlen[3] = {
          {.len = 1, .p = &r0},
          {.len = 1, .p = &r1},
          {.len = 1, .p = &r2},
      };
      ScopedH5Datatype vlen_t(H5Tvlen_create(H5T_STD_REF_OBJ));
      ASSERT_GE(vlen_t.get(), 0);
      ScopedH5Dataspace sp_attr(H5Screate_simple(1, &dim3, nullptr));
      ASSERT_GE(sp_attr.get(), 0);
      ScopedH5Attribute attr(
          H5Acreate(d_list, "DIMENSION_LIST", vlen_t, sp_attr, H5P_DEFAULT));
      ASSERT_GE(attr.get(), 0);
      ASSERT_GE(H5Awrite(attr, vlen_t, vlen), 0);
    }

    // 3D dataset using DIMENSION_LABELS + hidden attrs + units + valid
    // _FillValue + chunking/deflate/shuffle/fletcher32.
    {
      const hsize_t dims3d[3] = {2, 3, 4};
      const hsize_t chunk3d[3] = {1, 3, 2};
      ScopedH5Dataspace sp3d(H5Screate_simple(3, dims3d, nullptr));
      ASSERT_GE(sp3d.get(), 0);
      ScopedH5PropertyList dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_GE(dcpl.get(), 0);
      ASSERT_GE(H5Pset_chunk(dcpl, 3, chunk3d), 0);
      ASSERT_GE(H5Pset_shuffle(dcpl), 0);
      ASSERT_GE(H5Pset_deflate(dcpl, 1), 0);
      ASSERT_GE(H5Pset_fletcher32(dcpl), 0);
      ScopedH5Dataset d_lbl(
          H5Dcreate(grp, "data_labels", H5T_NATIVE_FLOAT, sp3d, dcpl));
      ASSERT_GE(d_lbl.get(), 0);
      WritePaddedStringListAttr(d_lbl, "DIMENSION_LABELS", 6,
                                {"lbl_a", "", "lbl_c"});
      ASSERT_TRUE(WriteScalarAttr<float>(d_lbl, "_Netcdf4Dimid",
                                         H5T_NATIVE_FLOAT, 0.0f));
      ASSERT_TRUE(WriteScalarAttr<float>(d_lbl, "_Netcdf4Coordinates",
                                         H5T_NATIVE_FLOAT, 0.0f));
      ASSERT_TRUE(
          WriteScalarStringAttr(d_lbl, "CLASS", "DATA", H5T_STR_NULLPAD));
      ASSERT_TRUE(
          WriteScalarStringAttr(d_lbl, "NAME", "data_labels", H5T_STR_NULLPAD));
      ASSERT_TRUE(
          WriteScalarStringAttr(d_lbl, "units", "kelvin", H5T_STR_NULLPAD));
      ASSERT_TRUE(WriteScalarAttr<float>(d_lbl, "_FillValue", H5T_NATIVE_FLOAT,
                                         -999.0f));
    }

    // 0D scalar dataset with out-of-range Float64 _FillValue on UInt8.
    {
      ScopedH5Dataspace sc(H5Screate(H5S_SCALAR));
      ASSERT_GE(sc.get(), 0);
      ScopedH5Dataset bad_fill(
          H5Dcreate(grp, "bad_fill_0d", H5T_NATIVE_UCHAR, sc, H5P_DEFAULT));
      ASSERT_GE(bad_fill.get(), 0);
      const uint8_t v = 42;
      ASSERT_GE(H5Dwrite(bad_fill, H5T_NATIVE_UCHAR, H5S_ALL, H5S_ALL,
                         H5P_DEFAULT, &v),
                0);
      ASSERT_TRUE(WriteScalarAttr<double>(bad_fill, "_FillValue",
                                          H5T_NATIVE_DOUBLE, 999.5));
    }
  }

  // 3. Open hierarchy and verify dimensions, attributes, links, and layout.
  {
    auto ds = OpenMultiDimDataset(h5file.str());
    ASSERT_NE(nullptr, ds);
    auto root = ds->GetRootGroup();
    ASSERT_NE(nullptr, root);

    EXPECT_EQ(1, root->GetAttributes().size());
    const char* show_all[] = {"SHOW_ALL=YES", nullptr};
    EXPECT_EQ(3, root->GetAttributes(show_all).size());
    EXPECT_THAT(root->GetGroupNames(),
                UnorderedElementsAre("grp", "other_grp"));

    auto grp = root->OpenGroup("grp");
    ASSERT_NE(nullptr, grp);
    EXPECT_THAT(grp->GetGroupNames(), ElementsAre("child_grp"));
    EXPECT_THAT(grp->GetMDArrayNames(), Not(Contains("YDim")));

    const auto dims = grp->GetDimensions();
    EXPECT_EQ(dims.size(), grp->GetDimensions().size());
    ASSERT_EQ(2, dims.size());
    EXPECT_EQ("XDim", dims[0]->GetName());
    EXPECT_EQ(GDAL_DIM_TYPE_HORIZONTAL_X, dims[0]->GetType());
    ASSERT_NE(nullptr, dims[0]->GetIndexingVariable());
    EXPECT_EQ("YDim", dims[1]->GetName());
    EXPECT_EQ(GDAL_DIM_TYPE_HORIZONTAL_Y, dims[1]->GetType());
    EXPECT_EQ(nullptr, dims[1]->GetIndexingVariable());

    auto typed_ds = grp->OpenMDArray("typed_ds");
    ASSERT_NE(nullptr, typed_ds);
    EXPECT_EQ("MyStruct", typed_ds->GetDataType().GetName());
    auto typed_attr = typed_ds->GetAttribute("typed_attr");
    ASSERT_NE(nullptr, typed_attr);
    EXPECT_EQ("MyStruct", typed_attr->GetDataType().GetName());

    auto d_list = grp->OpenMDArray("data_dim_list");
    ASSERT_NE(nullptr, d_list);
    ASSERT_EQ(3, d_list->GetDimensionCount());
    EXPECT_EQ("/grp/YDim", d_list->GetDimensions()[0]->GetFullName());
    EXPECT_EQ("/root_scale", d_list->GetDimensions()[1]->GetFullName());
    EXPECT_NE(nullptr, d_list->GetDimensions()[1]->GetIndexingVariable());
    EXPECT_EQ("/other_grp/sub_scale",
              d_list->GetDimensions()[2]->GetFullName());
    EXPECT_NE(nullptr, d_list->GetDimensions()[2]->GetIndexingVariable());
    EXPECT_THAT(d_list->GetBlockSize(), ElementsAre(0, 0, 0));
    EXPECT_EQ(0, CSLCount(d_list->GetStructuralInfo()));

    auto d_lbl = grp->OpenMDArray("data_labels");
    ASSERT_NE(nullptr, d_lbl);
    EXPECT_EQ(0, d_lbl->GetAttributes().size());
    EXPECT_EQ("kelvin", d_lbl->GetUnit());
    bool has_nodata = false;
    EXPECT_DOUBLE_EQ(-999.0, d_lbl->GetNoDataValueAsDouble(&has_nodata));
    EXPECT_TRUE(has_nodata);
    ASSERT_NE(nullptr, d_lbl->GetAttribute("CLASS"));
    ASSERT_EQ(3, d_lbl->GetDimensionCount());
    EXPECT_EQ("lbl_a", d_lbl->GetDimensions()[0]->GetName());
    EXPECT_EQ("dim1", d_lbl->GetDimensions()[1]->GetName());
    EXPECT_EQ("lbl_c", d_lbl->GetDimensions()[2]->GetName());
    EXPECT_THAT(d_lbl->GetBlockSize(), ElementsAre(1, 3, 2));
    EXPECT_STREQ("DEFLATE",
                 CSLFetchNameValue(d_lbl->GetStructuralInfo(), "COMPRESSION"));
    EXPECT_STREQ("SHUFFLE,FLETCHER32",
                 CSLFetchNameValue(d_lbl->GetStructuralInfo(), "FILTER"));

    {
      WithQuietHandler quiet;
      CPLErrorReset();
      auto bad_fill = grp->OpenMDArray("bad_fill_0d");
      EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("not in the range"));
      CPLErrorReset();
      ASSERT_NE(nullptr, bad_fill);
      EXPECT_EQ(nullptr, bad_fill->GetRawNoDataValue());
      EXPECT_THAT(bad_fill->GetBlockSize(), IsEmpty());
      EXPECT_EQ(h5file.str(), bad_fill->GetFilename());
      EXPECT_FALSE(bad_fill->IsWritable());
      EXPECT_NE(nullptr, bad_fill->GetRootGroup());
    }
  }
}

TEST(Hdf5MultiDimTest, HdfEos5GridAndSwathDimensionsAndCoordinates) {
  ScopedHdf5DriverManagerCleanup cleanup_dm;
  GDALRegister_HDF5();

  constexpr std::string_view kGridMeta = R"(
GROUP=GridStructure
  GROUP=GRID_1
    GridName="GeoGrid"
    XDim=4
    YDim=3
    UpperLeftPointMtrs=(-10000000.0,45000000.0)
    LowerRightMtrs=(10000000.0,35000000.0)
    Projection="HE5_GCTP_GEO"
    SphereCode=12
    ProjParams=(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0)
    GROUP=Dimension
      OBJECT=Dimension_1
        DimensionName="XDim"
        Size=4
      END_OBJECT=Dimension_1
      OBJECT=Dimension_2
        DimensionName="YDim"
        Size=3
      END_OBJECT=Dimension_2
      OBJECT=Dimension_3
        DimensionName="GridOnlyDim"
        Size=2
      END_OBJECT=Dimension_3
    END_GROUP=Dimension
    GROUP=DataField
      OBJECT=DataField_1
        DataFieldName="Temp"
        DataType=DFNT_FLOAT32
        DimList=("YDim","XDim")
      END_OBJECT=DataField_1
    END_GROUP=DataField
  END_GROUP=GRID_1
  GROUP=GRID_2
    GridName="UtmGrid"
    XDim=4
    YDim=3
    UpperLeftPointMtrs=(400000.0,5000000.0)
    LowerRightMtrs=(404000.0,4996000.0)
    Projection="HE5_GCTP_UTM"
    ZoneCode=32
    SphereCode=12
    ProjParams=(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0)
    GROUP=Dimension
    END_GROUP=Dimension
    GROUP=DataField
      OBJECT=DataField_1
        DataFieldName="Band"
        DataType=DFNT_FLOAT32
        DimList=("YDim","XDim")
      END_OBJECT=DataField_1
    END_GROUP=DataField
  END_GROUP=GRID_2
END_GROUP=GridStructure
END
)";

  constexpr std::string_view kSwathMeta = R"(
GROUP=SwathStructure
  GROUP=SWATH_1
    SwathName="TestSwath"
    GROUP=Dimension
      OBJECT=Dimension_1
        DimensionName="Track"
        Size=2
      END_OBJECT=Dimension_1
      OBJECT=Dimension_2
        DimensionName="CrossTrack"
        Size=3
      END_OBJECT=Dimension_2
    END_GROUP=Dimension
    GROUP=DimensionMap
    END_GROUP=DimensionMap
    GROUP=GeoField
      OBJECT=GeoField_1
        GeoFieldName="Latitude"
        DataType=DFNT_FLOAT32
        DimList=("Track","CrossTrack")
      END_OBJECT=GeoField_1
      OBJECT=GeoField_2
        GeoFieldName="Longitude"
        DataType=DFNT_FLOAT32
        DimList=("Track","CrossTrack")
      END_OBJECT=GeoField_2
    END_GROUP=GeoField
    GROUP=DataField
      OBJECT=DataField_1
        DataFieldName="Radiance"
        DataType=DFNT_FLOAT32
        DimList=("Track","CrossTrack")
      END_OBJECT=DataField_1
    END_GROUP=DataField
  END_GROUP=SWATH_1
END_GROUP=SwathStructure
END
)";

  // 1. HDF-EOS5 Grid file.
  ScopedVsimemFile eos_grid_file("hdfeos5_grid.he5");
  {
    ScopedH5File fid(CreateVsimemHdf5File(eos_grid_file.path()));
    ASSERT_GE(fid.get(), 0);
    ASSERT_TRUE(WriteStructMetadata(fid, kGridMeta));

    ScopedH5Group eos_g(H5Gcreate(fid, "HDFEOS", 0));
    ASSERT_GE(eos_g.get(), 0);
    ScopedH5Group grids_g(H5Gcreate(eos_g, "GRIDS", 0));
    ASSERT_GE(grids_g.get(), 0);
    const hsize_t grid_dims[2] = {3, 4};
    ScopedH5Dataspace sp_grid(H5Screate_simple(2, grid_dims, nullptr));
    ASSERT_GE(sp_grid.get(), 0);

    {
      ScopedH5Group g1(H5Gcreate(grids_g, "GeoGrid", 0));
      ASSERT_GE(g1.get(), 0);
      const hsize_t d4 = 4;
      const hsize_t d3 = 3;
      ScopedH5Dataspace sp4(H5Screate_simple(1, &d4, nullptr));
      ASSERT_GE(sp4.get(), 0);
      ScopedH5Dataspace sp3(H5Screate_simple(1, &d3, nullptr));
      ASSERT_GE(sp3.get(), 0);
      const double xvals[4] = {-7.5, -2.5, 2.5, 7.5};
      const double yvals[3] = {43.3, 40.0, 36.7};

      ScopedH5Dataset xd(
          H5Dcreate(g1, "XDim", H5T_NATIVE_DOUBLE, sp4, H5P_DEFAULT));
      ASSERT_GE(xd.get(), 0);
      ASSERT_TRUE(WriteScalarStringAttr(xd, "CLASS", "DIMENSION_SCALE",
                                        H5T_STR_NULLPAD));
      ASSERT_GE(
          H5Dwrite(xd, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, xvals),
          0);

      ScopedH5Dataset yd(
          H5Dcreate(g1, "YDim", H5T_NATIVE_DOUBLE, sp3, H5P_DEFAULT));
      ASSERT_GE(yd.get(), 0);
      ASSERT_TRUE(WriteScalarStringAttr(yd, "CLASS", "DIMENSION_SCALE",
                                        H5T_STR_NULLPAD));
      ASSERT_GE(
          H5Dwrite(yd, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, yvals),
          0);

      ScopedH5Group df_g(H5Gcreate(g1, "Data Fields", 0));
      ASSERT_GE(df_g.get(), 0);
      ScopedH5Dataset temp_ds(
          H5Dcreate(df_g, "Temp", H5T_NATIVE_FLOAT, sp_grid, H5P_DEFAULT));
      ASSERT_GE(temp_ds.get(), 0);
    }

    {
      ScopedH5Group g2(H5Gcreate(grids_g, "UtmGrid", 0));
      ASSERT_GE(g2.get(), 0);
      ScopedH5Group df_g(H5Gcreate(g2, "Data Fields", 0));
      ASSERT_GE(df_g.get(), 0);
      ScopedH5Dataset band_ds(
          H5Dcreate(df_g, "Band", H5T_NATIVE_FLOAT, sp_grid, H5P_DEFAULT));
      ASSERT_GE(band_ds.get(), 0);
    }
  }

  // 2. HDF-EOS5 Swath file.
  ScopedVsimemFile eos_swath_file("hdfeos5_swath.he5");
  {
    ScopedH5File fid(CreateVsimemHdf5File(eos_swath_file.path()));
    ASSERT_GE(fid.get(), 0);
    ASSERT_TRUE(WriteStructMetadata(fid, kSwathMeta));

    ScopedH5Group eos_g(H5Gcreate(fid, "HDFEOS", 0));
    ASSERT_GE(eos_g.get(), 0);
    ScopedH5Group swaths_g(H5Gcreate(eos_g, "SWATHS", 0));
    ASSERT_GE(swaths_g.get(), 0);
    ScopedH5Group sw(H5Gcreate(swaths_g, "TestSwath", 0));
    ASSERT_GE(sw.get(), 0);
    const hsize_t d3 = 3;
    ScopedH5Dataspace sp3(H5Screate_simple(1, &d3, nullptr));
    ASSERT_GE(sp3.get(), 0);

    ScopedH5Dataset tnv(
        H5Dcreate(sw, "CrossTrack", H5T_NATIVE_INT, sp3, H5P_DEFAULT));
    ASSERT_GE(tnv.get(), 0);
    ASSERT_TRUE(WriteScalarStringAttr(tnv, "CLASS", "DIMENSION_SCALE",
                                      H5T_STR_NULLPAD));
    ASSERT_TRUE(WriteScalarStringAttr(
        tnv, "NAME", "This is a netCDF dimension but not a netCDF variable 0",
        H5T_STR_NULLPAD));

    const hsize_t sw_dims[2] = {2, 3};
    ScopedH5Dataspace sp_sw(H5Screate_simple(2, sw_dims, nullptr));
    ASSERT_GE(sp_sw.get(), 0);
    ScopedH5Group geo_g(H5Gcreate(sw, "Geolocation Fields", 0));
    ASSERT_GE(geo_g.get(), 0);
    ScopedH5Dataset d_lat(
        H5Dcreate(geo_g, "Latitude", H5T_NATIVE_FLOAT, sp_sw, H5P_DEFAULT));
    ASSERT_GE(d_lat.get(), 0);
    ScopedH5Dataset d_lon(
        H5Dcreate(geo_g, "Longitude", H5T_NATIVE_FLOAT, sp_sw, H5P_DEFAULT));
    ASSERT_GE(d_lon.get(), 0);

    ScopedH5Group df_g(H5Gcreate(sw, "Data Fields", 0));
    ASSERT_GE(df_g.get(), 0);
    ScopedH5Dataset d_rad(
        H5Dcreate(df_g, "Radiance", H5T_NATIVE_FLOAT, sp_sw, H5P_DEFAULT));
    ASSERT_GE(d_rad.get(), 0);
  }

  // 3. Verify Grid dimensions, coordinate indexing variables, and SRS mapping.
  {
    auto ds = OpenMultiDimDataset(eos_grid_file.str());
    ASSERT_NE(nullptr, ds);
    auto root = ds->GetRootGroup();
    ASSERT_NE(nullptr, root);

    auto geo_grid = root->OpenGroupFromFullname("/HDFEOS/GRIDS/GeoGrid");
    ASSERT_NE(nullptr, geo_grid);
    const auto g_dims = geo_grid->GetDimensions();
    ASSERT_EQ(3, g_dims.size());
    EXPECT_THAT(geo_grid->GetMDArrayNames(), Contains("XDim"));
    EXPECT_THAT(geo_grid->GetMDArrayNames(), Contains("YDim"));
    ASSERT_NE(nullptr, geo_grid->OpenMDArray("XDim"));
    ASSERT_NE(nullptr, geo_grid->OpenMDArray("YDim"));

    auto temp =
        root->OpenMDArrayFromFullname("/HDFEOS/GRIDS/GeoGrid/Data Fields/Temp");
    ASSERT_NE(nullptr, temp);
    ASSERT_EQ(2, temp->GetDimensionCount());
    EXPECT_EQ("YDim", temp->GetDimensions()[0]->GetName());
    EXPECT_EQ("XDim", temp->GetDimensions()[1]->GetName());
    ASSERT_NE(nullptr, temp->GetSpatialRef());
    EXPECT_THAT(temp->GetSpatialRef()->GetDataAxisToSRSAxisMapping(),
                ElementsAre(1, 2));

    auto band =
        root->OpenMDArrayFromFullname("/HDFEOS/GRIDS/UtmGrid/Data Fields/Band");
    ASSERT_NE(nullptr, band);
    ASSERT_NE(nullptr, band->GetSpatialRef());
    EXPECT_EQ(TRUE, band->GetSpatialRef()->IsProjected());
    EXPECT_THAT(band->GetSpatialRef()->GetDataAxisToSRSAxisMapping(),
                ElementsAre(2, 1));
  }

  // 4. Verify Swath dimensions and coordinate variables.
  {
    auto ds = OpenMultiDimDataset(eos_swath_file.str());
    ASSERT_NE(nullptr, ds);
    auto root = ds->GetRootGroup();
    ASSERT_NE(nullptr, root);

    auto lat = root->OpenMDArrayFromFullname(
        "/HDFEOS/SWATHS/TestSwath/Geolocation Fields/Latitude");
    ASSERT_NE(nullptr, lat);
    ASSERT_EQ(2, lat->GetDimensionCount());
    EXPECT_EQ("Track", lat->GetDimensions()[0]->GetName());
    EXPECT_EQ(nullptr, lat->GetDimensions()[0]->GetIndexingVariable());
    EXPECT_EQ("CrossTrack", lat->GetDimensions()[1]->GetName());
    EXPECT_EQ(nullptr, lat->GetDimensions()[1]->GetIndexingVariable());

    auto radiance = root->OpenMDArrayFromFullname(
        "/HDFEOS/SWATHS/TestSwath/Data Fields/Radiance");
    ASSERT_NE(nullptr, radiance);
    EXPECT_EQ(2, radiance->GetDimensionCount());
    const auto coords = radiance->GetCoordinateVariables();
    ASSERT_EQ(2, coords.size());
    EXPECT_EQ("Longitude", coords[0]->GetName());
    EXPECT_EQ("Latitude", coords[1]->GetName());
  }
}

struct ComplexInt16 {
  int16_t r;
  int16_t i;
};

struct ComplexFloat16 {
  uint16_t r;
  uint16_t i;
};

TEST(Hdf5MultiDimTest, DataTypesStringsEnumsCompoundsAndStridedReads) {
  ScopedHdf5DriverManagerCleanup cleanup_dm;
  GDALRegister_HDF5();

  ScopedVsimemFile dt_file("datatypes.h5");
  {
    ScopedH5File fid(CreateVsimemHdf5File(dt_file.path()));
    ASSERT_GE(fid.get(), 0);

    ScopedH5Dataspace sc(H5Screate(H5S_SCALAR));
    ASSERT_GE(sc.get(), 0);

    // 1. Float16 & CFloat16 scalar datasets (#ifdef HDF5_HAVE_FLOAT16 paths).
#ifdef HDF5_HAVE_FLOAT16
    {
      const uint16_t hval = 0x4000;  // IEEE-754 half for +2.0
      ScopedH5Dataset df16(
          H5Dcreate(fid, "f16", H5T_NATIVE_FLOAT16, sc, H5P_DEFAULT));
      ASSERT_GE(df16.get(), 0);
      ASSERT_GE(H5Dwrite(df16, H5T_NATIVE_FLOAT16, H5S_ALL, H5S_ALL,
                         H5P_DEFAULT, &hval),
                0);

      ScopedH5Datatype cf16_t(CreateComplexH5Type(H5T_NATIVE_FLOAT16));
      const ComplexFloat16 cfval{.r = 0x3C00, .i = 0xC000};  // (1.0, -2.0)
      ScopedH5Dataset dcf16(H5Dcreate(fid, "cf16", cf16_t, sc, H5P_DEFAULT));
      ASSERT_GE(dcf16.get(), 0);
      ASSERT_GE(H5Dwrite(dcf16, cf16_t, H5S_ALL, H5S_ALL, H5P_DEFAULT, &cfval),
                0);
    }
#endif

    // Complex Int16 scalar dataset for CInt16 read & mismatched complex test.
    {
      ScopedH5Datatype ci16_t(CreateComplexH5Type(H5T_NATIVE_SHORT));
      const ComplexInt16 cival{.r = 5, .i = -7};
      ScopedH5Dataset dci16(H5Dcreate(fid, "ci16", ci16_t, sc, H5P_DEFAULT));
      ASSERT_GE(dci16.get(), 0);
      ASSERT_GE(H5Dwrite(dci16, ci16_t, H5S_ALL, H5S_ALL, H5P_DEFAULT, &cival),
                0);
    }

    // 2. Variable-length 2D string array & fixed-length space-padded 1D string
    //    array, plus 0D/1D/2D string attributes.
    {
      ScopedH5Datatype vstr_t(H5Tcopy(H5T_C_S1));
      ASSERT_GE(H5Tset_size(vstr_t, H5T_VARIABLE), 0);
      const hsize_t dims2x2[2] = {2, 2};
      ScopedH5Dataspace sp2x2(H5Screate_simple(2, dims2x2, nullptr));
      ASSERT_GE(sp2x2.get(), 0);
      ScopedH5Dataset d_vstr(
          H5Dcreate(fid, "vstr_2d", vstr_t, sp2x2, H5P_DEFAULT));
      ASSERT_GE(d_vstr.get(), 0);
      const char* sdata[4] = {"hello", nullptr, "world", "gdal"};
      ASSERT_GE(H5Dwrite(d_vstr, vstr_t, H5S_ALL, H5S_ALL, H5P_DEFAULT, sdata),
                0);

      ASSERT_TRUE(WriteScalarStringAttr(d_vstr, "attr_0d_str", "scalar_ok",
                                        H5T_STR_NULLPAD));
      ScopedH5Attribute a_vstr_2d(
          H5Acreate(d_vstr, "attr_2d_vstr", vstr_t, sp2x2, H5P_DEFAULT));
      ASSERT_GE(a_vstr_2d.get(), 0);
      ASSERT_GE(H5Awrite(a_vstr_2d, vstr_t, sdata), 0);

      ScopedH5Datatype fstr_t(H5Tcopy(H5T_C_S1));
      ASSERT_GE(H5Tset_size(fstr_t, 6), 0);
      ASSERT_GE(H5Tset_strpad(fstr_t, H5T_STR_SPACEPAD), 0);
      const hsize_t dim2 = 2;
      ScopedH5Dataspace sp2(H5Screate_simple(1, &dim2, nullptr));
      ASSERT_GE(sp2.get(), 0);
      ScopedH5Dataset d_fstr(
          H5Dcreate(fid, "fstr_spacepad", fstr_t, sp2, H5P_DEFAULT));
      ASSERT_GE(d_fstr.get(), 0);
      const char fbuf[12] = {'a', 'b', 'c', ' ', ' ', ' ',
                             'x', 'y', ' ', ' ', ' ', ' '};
      ASSERT_GE(H5Dwrite(d_fstr, fstr_t, H5S_ALL, H5S_ALL, H5P_DEFAULT, fbuf),
                0);
      WritePaddedStringListAttr(d_fstr, "attr_1d_fstr", 4, {"one ", "two "});
    }

    // 3. Enum datasets & attributes covering all 8 integer parent types +
    //    custom 24-bit int enum fallback.
    {
      const hid_t parent_types[] = {
          H5T_NATIVE_UCHAR,  H5T_NATIVE_SCHAR, H5T_NATIVE_USHORT,
          H5T_NATIVE_SHORT,  H5T_NATIVE_UINT,  H5T_NATIVE_INT,
          H5T_NATIVE_UINT64, H5T_NATIVE_INT64,
      };
      for (size_t idx = 0; idx < std::size(parent_types); ++idx) {
        ScopedH5Datatype enum_t(H5Tenum_create(parent_types[idx]));
        ASSERT_GE(enum_t.get(), 0);
        uint64_t zero = 0;
        uint64_t three = 3;
        H5Tenum_insert(enum_t, "ZERO", &zero);
        H5Tenum_insert(enum_t, "THREE", &three);
        const std::string name = std::format("enum_{}", idx);
        ScopedH5Dataset ds(
            H5Dcreate(fid, name.c_str(), enum_t, sc, H5P_DEFAULT));
        ASSERT_GE(ds.get(), 0);
        ScopedH5Attribute attr(
            H5Acreate(ds, "enum_attr", enum_t, sc, H5P_DEFAULT));
        ASSERT_GE(attr.get(), 0);
        const uint64_t val = 3;
        ASSERT_GE(H5Dwrite(ds, enum_t, H5S_ALL, H5S_ALL, H5P_DEFAULT, &val), 0);
        ASSERT_GE(H5Awrite(attr, enum_t, &val), 0);
      }

      ScopedH5Datatype int24_t(H5Tcopy(H5T_NATIVE_INT));
      ASSERT_GE(H5Tset_precision(int24_t, 24), 0);
      ScopedH5Datatype enum24_t(H5Tenum_create(int24_t));
      ASSERT_GE(enum24_t.get(), 0);
      const int val24 = 5;
      H5Tenum_insert(enum24_t, "FIVE", &val24);
      ScopedH5Dataset ds24(
          H5Dcreate(fid, "enum_24bit", enum24_t, sc, H5P_DEFAULT));
      ASSERT_GE(ds24.get(), 0);
      ScopedH5Attribute attr24(
          H5Acreate(ds24, "enum_attr", enum24_t, sc, H5P_DEFAULT));
      ASSERT_GE(attr24.get(), 0);
      ASSERT_GE(H5Dwrite(ds24, enum24_t, H5S_ALL, H5S_ALL, H5P_DEFAULT, &val24),
                0);
      ASSERT_GE(H5Awrite(attr24, enum24_t, &val24), 0);
    }

    // 4. Compound datasets:
    //    (a) Simple {int32 a; float64 b;} for single-component view fast path.
    //    (b) Aligned padded nested struct with vlen string member (dataset &
    //        scalar attribute) for CreateMapTargetComponentsToSrc + CopyValue.
    //    (c) Compound with unsupported member type -> OpenMDArray returns
    //        nullptr.
    {
      ScopedH5Datatype simple_t(
          H5Tcreate(H5T_COMPOUND, sizeof(SimpleCompStruct)));
      ASSERT_GE(simple_t.get(), 0);
      ASSERT_GE(H5Tinsert(simple_t, "a", offsetof(SimpleCompStruct, id),
                          H5T_NATIVE_INT32),
                0);
      ASSERT_GE(H5Tinsert(simple_t, "b", offsetof(SimpleCompStruct, val),
                          H5T_NATIVE_DOUBLE),
                0);
      const hsize_t d2x2[2] = {2, 2};
      ScopedH5Dataspace sp2x2(H5Screate_simple(2, d2x2, nullptr));
      ASSERT_GE(sp2x2.get(), 0);
      ScopedH5Dataset d_simple(
          H5Dcreate(fid, "comp_simple", simple_t, sp2x2, H5P_DEFAULT));
      ASSERT_GE(d_simple.get(), 0);
      const SimpleCompStruct s_rows[4] = {
          {.id = 1, .val = 10.5},
          {.id = 2, .val = 20.5},
          {.id = 3, .val = 30.5},
          {.id = 4, .val = 40.5},
      };
      ASSERT_GE(
          H5Dwrite(d_simple, simple_t, H5S_ALL, H5S_ALL, H5P_DEFAULT, s_rows),
          0);

      // Padded nested struct with naturally aligned vlen string pointer.
      ScopedH5Datatype sub_t(H5Tcreate(H5T_COMPOUND, sizeof(NestedSub)));
      ASSERT_GE(sub_t.get(), 0);
      ASSERT_GE(H5Tinsert(sub_t, "x", offsetof(NestedSub, x), H5T_NATIVE_INT32),
                0);
      ASSERT_GE(
          H5Tinsert(sub_t, "y", offsetof(NestedSub, y), H5T_NATIVE_DOUBLE), 0);
      ScopedH5Datatype vstr_t(H5Tcopy(H5T_C_S1));
      ASSERT_GE(H5Tset_size(vstr_t, H5T_VARIABLE), 0);
      // Add 4 extra trailing bytes to exercise bNonNativeDataType padding.
      ScopedH5Datatype outer_t(
          H5Tcreate(H5T_COMPOUND, sizeof(PaddedVlenOuter) + 4));
      ASSERT_GE(outer_t.get(), 0);
      ASSERT_GE(H5Tinsert(outer_t, "tag", offsetof(PaddedVlenOuter, tag),
                          H5T_NATIVE_UCHAR),
                0);
      ASSERT_GE(
          H5Tinsert(outer_t, "sub", offsetof(PaddedVlenOuter, sub), sub_t), 0);
      ASSERT_GE(
          H5Tinsert(outer_t, "label", offsetof(PaddedVlenOuter, label), vstr_t),
          0);
      ScopedH5Datatype outer_mem_t(
          H5Tcreate(H5T_COMPOUND, sizeof(PaddedVlenOuter)));
      ASSERT_GE(outer_mem_t.get(), 0);
      ASSERT_GE(H5Tinsert(outer_mem_t, "tag", offsetof(PaddedVlenOuter, tag),
                          H5T_NATIVE_UCHAR),
                0);
      ASSERT_GE(
          H5Tinsert(outer_mem_t, "sub", offsetof(PaddedVlenOuter, sub), sub_t),
          0);
      ASSERT_GE(H5Tinsert(outer_mem_t, "label",
                          offsetof(PaddedVlenOuter, label), vstr_t),
                0);

      ScopedH5Dataset d_outer(
          H5Dcreate(fid, "comp_nested_vlen", outer_t, sp2x2, H5P_DEFAULT));
      ASSERT_GE(d_outer.get(), 0);
      const PaddedVlenOuter o_rows[4] = {
          {.tag = 1, .sub = {.x = 11, .y = 1.5}, .label = "s0"},
          {.tag = 2, .sub = {.x = 22, .y = 2.5}, .label = "s1"},
          {.tag = 3, .sub = {.x = 33, .y = 3.5}, .label = "s2"},
          {.tag = 4, .sub = {.x = 44, .y = 4.5}, .label = "s3"},
      };
      ASSERT_GE(
          H5Dwrite(d_outer, outer_mem_t, H5S_ALL, H5S_ALL, H5P_DEFAULT, o_rows),
          0);
      ScopedH5Attribute a_outer(
          H5Acreate(d_outer, "comp_attr", outer_t, sc, H5P_DEFAULT));
      ASSERT_GE(a_outer.get(), 0);
      ASSERT_GE(H5Awrite(a_outer, outer_mem_t, &o_rows[2]), 0);

      ScopedH5Datatype bad_comp_t(H5Tcreate(H5T_COMPOUND, sizeof(hobj_ref_t)));
      ASSERT_GE(bad_comp_t.get(), 0);
      ASSERT_GE(H5Tinsert(bad_comp_t, "bad_ref", 0, H5T_STD_REF_OBJ), 0);
      ScopedH5Dataset d_bad(
          H5Dcreate(fid, "comp_unsupported", bad_comp_t, sc, H5P_DEFAULT));
      ASSERT_GE(d_bad.get(), 0);
    }

    // 5. 2D 4x6 Int32 dataset for strided, transposed, ReadSlow & recursive
    //    split threshold testing.
    {
      const hsize_t dims4x6[2] = {4, 6};
      ScopedH5Dataspace sp4x6(H5Screate_simple(2, dims4x6, nullptr));
      ASSERT_GE(sp4x6.get(), 0);
      ScopedH5Dataset d_grid(
          H5Dcreate(fid, "grid_i32", H5T_NATIVE_INT32, sp4x6, H5P_DEFAULT));
      ASSERT_GE(d_grid.get(), 0);
      int32_t grid[24];
      for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 6; ++c) {
          grid[r * 6 + c] = r * 10 + c;
        }
      }
      ASSERT_GE(H5Dwrite(d_grid, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                         H5P_DEFAULT, grid),
                0);
    }
  }

  // 6. Verify datatypes, strings, enums, compounds, and strided ReadSlow paths.
  {
    auto ds = OpenMultiDimDataset(dt_file.str());
    ASSERT_NE(nullptr, ds);
    auto root = ds->GetRootGroup();
    ASSERT_NE(nullptr, root);

#ifdef HDF5_HAVE_FLOAT16
    {
      auto f16 = root->OpenMDArray("f16");
      ASSERT_NE(nullptr, f16);
      EXPECT_EQ(GDT_Float32, f16->GetDataType().GetNumericDataType());
      double dval = 0.0;
      ASSERT_TRUE(f16->Read(nullptr, nullptr, nullptr, nullptr,
                            GDALExtendedDataType::Create(GDT_Float64), &dval));
      EXPECT_DOUBLE_EQ(2.0, dval);

      auto cf16 = root->OpenMDArray("cf16");
      ASSERT_NE(nullptr, cf16);
      float cf32_out[2] = {};
      ASSERT_TRUE(cf16->Read(nullptr, nullptr, nullptr, nullptr,
                             GDALExtendedDataType::Create(GDT_CFloat32),
                             cf32_out));
      EXPECT_FLOAT_EQ(1.0f, cf32_out[0]);
      EXPECT_FLOAT_EQ(-2.0f, cf32_out[1]);
      double cf64_out[2] = {};
      ASSERT_TRUE(cf16->Read(nullptr, nullptr, nullptr, nullptr,
                             GDALExtendedDataType::Create(GDT_CFloat64),
                             cf64_out));
      EXPECT_DOUBLE_EQ(1.0, cf64_out[0]);
      EXPECT_DOUBLE_EQ(-2.0, cf64_out[1]);
    }
#endif

    {
      auto ci16 = root->OpenMDArray("ci16");
      ASSERT_NE(nullptr, ci16);
      ComplexInt16 c_out{};
      ASSERT_TRUE(ci16->Read(nullptr, nullptr, nullptr, nullptr,
                             GDALExtendedDataType::Create(GDT_CInt16), &c_out));
      EXPECT_EQ(5, c_out.r);
      EXPECT_EQ(-7, c_out.i);

      auto g_i32 = root->OpenMDArray("grid_i32");
      ASSERT_NE(nullptr, g_i32);
      const GUInt64 st0[2] = {0, 0};
      const size_t cnt1[2] = {1, 1};
      const GInt64 step1[2] = {1, 1};
      const GPtrDiff_t stride1[2] = {1, 1};
      const GDALDataType target_types[] = {
          GDT_Byte,    GDT_Int8,    GDT_UInt16, GDT_Int16,
          GDT_UInt32,  GDT_Int32,   GDT_UInt64, GDT_Int64,
#ifdef HDF5_HAVE_FLOAT16
          GDT_Float16,
#endif
          GDT_Float32, GDT_Float64,
      };
      for (GDALDataType t : target_types) {
        uint8_t buf[8] = {};
        EXPECT_TRUE(g_i32->Read(st0, cnt1, step1, stride1,
                                GDALExtendedDataType::Create(t), buf));
      }
      uint8_t dummy = 0;
      EXPECT_FALSE(g_i32->Read(st0, cnt1, step1, stride1,
                               GDALExtendedDataType::Create(GDT_Unknown),
                               &dummy));
    }

    {
      auto vstr = root->OpenMDArray("vstr_2d");
      ASSERT_NE(nullptr, vstr);
      const GUInt64 st[2] = {0, 0};
      const size_t cnt[2] = {2, 2};
      const GInt64 step[2] = {1, 1};
      const GPtrDiff_t stride[2] = {2, 1};
      int32_t bad_out[4] = {};
      EXPECT_FALSE(vstr->Read(st, cnt, step, stride,
                              GDALExtendedDataType::Create(GDT_Int32),
                              bad_out));

      char* out_strs[4] = {};
      const auto str_dt = GDALExtendedDataType::CreateString();
      ASSERT_TRUE(vstr->Read(st, cnt, step, stride, str_dt, out_strs));
      EXPECT_STREQ("hello", out_strs[0]);
      EXPECT_EQ(nullptr, out_strs[1]);
      EXPECT_STREQ("world", out_strs[2]);
      EXPECT_STREQ("gdal", out_strs[3]);
      for (char* p : out_strs) {
        str_dt.FreeDynamicMemory(&p);
      }

      auto attr0 = vstr->GetAttribute("attr_0d_str");
      ASSERT_NE(nullptr, attr0);
      EXPECT_STREQ("scalar_ok", attr0->ReadAsString());
      EXPECT_EQ(std::numeric_limits<int>::min(), attr0->ReadAsInt());

      auto attr2 = vstr->GetAttribute("attr_2d_vstr");
      ASSERT_NE(nullptr, attr2);
      char* a2_out[4] = {};
      ASSERT_TRUE(attr2->Read(st, cnt, step, stride, str_dt, a2_out));
      EXPECT_STREQ("hello", a2_out[0]);
      EXPECT_EQ(nullptr, a2_out[1]);
      for (char* p : a2_out) {
        str_dt.FreeDynamicMemory(&p);
      }

      auto fstr = root->OpenMDArray("fstr_spacepad");
      ASSERT_NE(nullptr, fstr);
      const GUInt64 st1[1] = {0};
      const size_t cnt1[1] = {2};
      const GInt64 step1[1] = {1};
      const GPtrDiff_t stride1[1] = {1};
      char* f_out[2] = {};
      ASSERT_TRUE(fstr->Read(st1, cnt1, step1, stride1, str_dt, f_out));
      EXPECT_STREQ("abc", f_out[0]);
      EXPECT_STREQ("xy", f_out[1]);
      for (char* p : f_out) {
        str_dt.FreeDynamicMemory(&p);
      }

      auto attr1 = fstr->GetAttribute("attr_1d_fstr");
      ASSERT_NE(nullptr, attr1);
      const GUInt64 a1_st[1] = {1};
      const size_t a1_cnt[1] = {1};
      char* a1_out = nullptr;
      ASSERT_TRUE(attr1->Read(a1_st, a1_cnt, step1, stride1, str_dt, &a1_out));
      EXPECT_STREQ("two ", a1_out);
      str_dt.FreeDynamicMemory(&a1_out);
    }

    for (int idx = 0; idx <= 8; ++idx) {
      const std::string name =
          (idx < 8) ? std::format("enum_{}", idx) : "enum_24bit";
      auto e_ds = root->OpenMDArray(name);
      ASSERT_NE(nullptr, e_ds);
      double val = 0.0;
      ASSERT_TRUE(e_ds->Read(nullptr, nullptr, nullptr, nullptr,
                             GDALExtendedDataType::Create(GDT_Float64), &val));
      EXPECT_DOUBLE_EQ((idx < 8) ? 3.0 : 5.0, val);
      auto e_attr = e_ds->GetAttribute("enum_attr");
      ASSERT_NE(nullptr, e_attr);
      EXPECT_EQ((idx < 8) ? 3 : 5, e_attr->ReadAsInt());
    }

    {
      auto comp_s = root->OpenMDArray("comp_simple");
      ASSERT_NE(nullptr, comp_s);
      auto view_b = comp_s->GetView("[\"b\"]");
      ASSERT_NE(nullptr, view_b);
      const GUInt64 st[2] = {0, 0};
      const size_t cnt[2] = {2, 2};
      const GInt64 step[2] = {1, 1};
      const GPtrDiff_t stride[2] = {2, 1};
      double b_vals[4] = {};
      ASSERT_TRUE(
          view_b->Read(st, cnt, step, stride, view_b->GetDataType(), b_vals));
      EXPECT_THAT(b_vals, ElementsAre(10.5, 20.5, 30.5, 40.5));
    }

    {
      EXPECT_EQ(nullptr, root->OpenMDArray("comp_unsupported"));
      auto comp_n = root->OpenMDArray("comp_nested_vlen");
      ASSERT_NE(nullptr, comp_n);
      auto lbl_view = comp_n->GetView("[\"label\"]");
      ASSERT_NE(nullptr, lbl_view);
      const auto str_dt = GDALExtendedDataType::CreateString();
      char* lbl0 = nullptr;
      ASSERT_TRUE(lbl_view->at(0, 0)->Read(nullptr, nullptr, nullptr, nullptr,
                                           str_dt, &lbl0));
      EXPECT_STREQ("s0", lbl0);
      str_dt.FreeDynamicMemory(&lbl0);

      std::vector<std::unique_ptr<GDALEDTComponent>> sub_comps;
      sub_comps.emplace_back(std::make_unique<GDALEDTComponent>(
          "y", 0, GDALExtendedDataType::Create(GDT_Float64)));
      const auto sub_dt = GDALExtendedDataType::Create("SubY", sizeof(double),
                                                       std::move(sub_comps));

      struct CustomRec {
        char* label;
        double y;
        uint8_t tag;
      };
      std::vector<std::unique_ptr<GDALEDTComponent>> comps;
      comps.emplace_back(std::make_unique<GDALEDTComponent>(
          "label", offsetof(CustomRec, label),
          GDALExtendedDataType::CreateString()));
      comps.emplace_back(std::make_unique<GDALEDTComponent>(
          "sub", offsetof(CustomRec, y), sub_dt));
      comps.emplace_back(std::make_unique<GDALEDTComponent>(
          "tag", offsetof(CustomRec, tag),
          GDALExtendedDataType::Create(GDT_Byte)));
      const auto dst_dt = GDALExtendedDataType::Create(
          "CustomRec", sizeof(CustomRec), std::move(comps));

      const GUInt64 st[2] = {0, 0};
      const size_t cnt[2] = {2, 2};
      const GInt64 step[2] = {1, 1};
      const GPtrDiff_t stride[2] = {2, 1};
      CustomRec out_recs[4] = {};
      ASSERT_TRUE(comp_n->Read(st, cnt, step, stride, dst_dt, out_recs));
      EXPECT_STREQ("s0", out_recs[0].label);
      EXPECT_DOUBLE_EQ(1.5, out_recs[0].y);
      EXPECT_EQ(1, out_recs[0].tag);
      EXPECT_STREQ("s3", out_recs[3].label);
      EXPECT_DOUBLE_EQ(4.5, out_recs[3].y);
      EXPECT_EQ(4, out_recs[3].tag);
      for (CustomRec& r : out_recs) {
        dst_dt.FreeDynamicMemory(&r);
      }

      auto comp_attr = comp_n->GetAttribute("comp_attr");
      ASSERT_NE(nullptr, comp_attr);
      CustomRec attr_rec{};
      ASSERT_TRUE(comp_attr->Read(nullptr, nullptr, nullptr, nullptr, dst_dt,
                                  &attr_rec));
      EXPECT_STREQ("s2", attr_rec.label);
      EXPECT_DOUBLE_EQ(3.5, attr_rec.y);
      EXPECT_EQ(3, attr_rec.tag);
      dst_dt.FreeDynamicMemory(&attr_rec);
    }

    {
      auto g = root->OpenMDArray("grid_i32");
      ASSERT_NE(nullptr, g);
      const auto dt = GDALExtendedDataType::Create(GDT_Int32);

      {
        constexpr GUInt64 st[2] = {0, 0};
        constexpr size_t cnt[2] = {2, 3};
        constexpr GInt64 step[2] = {1, 2};
        constexpr GPtrDiff_t stride[2] = {6, 2};
        int32_t out[12] = {};
        ASSERT_TRUE(g->Read(st, cnt, step, stride, dt, out));
        EXPECT_EQ(0, out[0]);
        EXPECT_EQ(2, out[2]);
        EXPECT_EQ(4, out[4]);
        EXPECT_EQ(10, out[6]);
      }

      {
        CPLConfigOptionSetter low_thresh("GDAL_HDF5_TEMP_ARRAY_ALLOC_SIZE", "8",
                                         false);
        constexpr GUInt64 st[2] = {3, 5};
        constexpr size_t cnt[2] = {4, 6};
        constexpr GInt64 step[2] = {-1, -1};
        constexpr GPtrDiff_t stride[2] = {6, 1};
        int32_t rev[24] = {};
        ASSERT_TRUE(g->Read(st, cnt, step, stride, dt, rev));
        EXPECT_EQ(35, rev[0]);
        EXPECT_EQ(30, rev[5]);
        EXPECT_EQ(5, rev[18]);
        EXPECT_EQ(0, rev[23]);

        // Mixed-sign arrayStep = {1, -1} exercises ReadSlow's positive-step
        // recursive split and CopyBuffer positive/negative step branches
        // without negative bufferStride.
        constexpr GUInt64 st_mixed[2] = {0, 2};
        constexpr size_t cnt_mixed[2] = {2, 3};
        constexpr GInt64 step_mixed[2] = {1, -1};
        constexpr GPtrDiff_t stride_mixed[2] = {3, 1};
        int32_t mixed[6] = {};
        ASSERT_TRUE(
            g->Read(st_mixed, cnt_mixed, step_mixed, stride_mixed, dt, mixed));
        EXPECT_THAT(mixed, ElementsAre(2, 1, 0, 12, 11, 10));
      }

      {
        // TODO: b/582752651 - Restore a count > 1 negative-bufferStride read
        // once upstream computes bufferStride offsets in signed arithmetic.
        // Currently, the hypercube-corner loop in
        // GDALAbstractMDArray::CheckReadWriteParams, CopyBuffer(), and
        // HDF5Array::ReadSlow()'s pOtherHalfDstBuffer multiply signed
        // negative `bufferStride[i]` (GPtrDiff_t) by unsigned `size_t`,
        // promoting the offset to unsigned `size_t` and triggering UBSan
        // pointer-overflow under --config=asan-chlor whenever
        // bufferStride[i] < 0 and count[i] > 1. Using count = 1 on the
        // negative-stride axis (cnt = {1, 3}, stride = {-3, 1}) still sets
        // bufferStride_all_positive = false and exercises the hypercube corner
        // bounds-check loop without unsigned pointer wrap-around.
        constexpr GUInt64 st[2] = {0, 0};
        constexpr size_t cnt[2] = {1, 3};
        constexpr GInt64 step[2] = {1, 1};
        constexpr GPtrDiff_t stride[2] = {-3, 1};
        int32_t storage[3] = {};
        ASSERT_TRUE(g->Read(st, cnt, step, stride, dt, &storage[0], storage,
                            sizeof(storage)));
        EXPECT_THAT(storage, ElementsAre(0, 1, 2));
        CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
        EXPECT_FALSE(g->Read(st, cnt, step, stride, dt, &storage[0], storage,
                             sizeof(storage) - 1));
      }

      {
        constexpr GUInt64 st[2] = {0, 0};
        constexpr size_t cnt[2] = {2, 3};
        constexpr GInt64 step[2] = {1, 1};
        constexpr GPtrDiff_t stride[2] = {1, 2};
        int32_t transposed[6] = {};
        ASSERT_TRUE(g->Read(st, cnt, step, stride, dt, transposed));
        EXPECT_THAT(transposed, ElementsAre(0, 10, 1, 11, 2, 12));
      }
    }
  }
}

struct GroupFRow {
  const char* code;
  const char* name;
  const char* uom_name;
  const char* fill_value;
  const char* datatype;
  const char* lower;
  const char* upper;
  const char* closure;
};

void WriteGroupFDataset(hid_t group_f, const char* name,
                        std::initializer_list<const char*> fill_vals) {
  ScopedH5Datatype vstr_t(H5Tcopy(H5T_C_S1));
  ASSERT_GE(H5Tset_size(vstr_t, H5T_VARIABLE), 0);
  ScopedH5Datatype row_t(H5Tcreate(H5T_COMPOUND, sizeof(GroupFRow)));
  ASSERT_GE(H5Tinsert(row_t, "code", offsetof(GroupFRow, code), vstr_t), 0);
  ASSERT_GE(H5Tinsert(row_t, "name", offsetof(GroupFRow, name), vstr_t), 0);
  ASSERT_GE(H5Tinsert(row_t, "uom.name", offsetof(GroupFRow, uom_name), vstr_t),
            0);
  ASSERT_GE(
      H5Tinsert(row_t, "fillValue", offsetof(GroupFRow, fill_value), vstr_t),
      0);
  ASSERT_GE(H5Tinsert(row_t, "datatype", offsetof(GroupFRow, datatype), vstr_t),
            0);
  ASSERT_GE(H5Tinsert(row_t, "lower", offsetof(GroupFRow, lower), vstr_t), 0);
  ASSERT_GE(H5Tinsert(row_t, "upper", offsetof(GroupFRow, upper), vstr_t), 0);
  ASSERT_GE(H5Tinsert(row_t, "closure", offsetof(GroupFRow, closure), vstr_t),
            0);

  std::vector<GroupFRow> rows;
  rows.reserve(fill_vals.size());
  for (const char* fv : fill_vals) {
    rows.push_back(GroupFRow{
        .code = "C",
        .name = "N",
        .uom_name = "U",
        .fill_value = fv,
        .datatype = "H5T_FLOAT",
        .lower = "0",
        .upper = "1",
        .closure = "closed",
    });
  }
  const hsize_t count = rows.size();
  ScopedH5Dataspace sp(H5Screate_simple(1, &count, nullptr));
  ASSERT_GE(sp.get(), 0);
  ScopedH5Dataset ds(H5Dcreate(group_f, name, row_t, sp, H5P_DEFAULT));
  ASSERT_GE(ds.get(), 0);
  ASSERT_GE(H5Dwrite(ds, row_t, H5S_ALL, H5S_ALL, H5P_DEFAULT, rows.data()), 0);
}

TEST(Hdf5MultiDimTest, S100SpecialCaseNodataFromGroupF) {
  ScopedHdf5DriverManagerCleanup cleanup_dm;
  GDALRegister_HDF5();

  ScopedVsimemFile s100_file("s100.h5");
  {
    ScopedH5File fid(CreateVsimemHdf5File(s100_file.path()));
    ASSERT_GE(fid.get(), 0);

    ScopedH5Group group_f(H5Gcreate(fid, "Group_F", 0));
    ASSERT_GE(group_f.get(), 0);
    WriteGroupFDataset(group_f, "BathymetryCoverage", {"1234.5", "678.25"});
    WriteGroupFDataset(group_f, "QualityOfSurvey", {"9999"});
    WriteGroupFDataset(group_f, "QualityOfBathymetryCoverage", {"8888"});
    WriteGroupFDataset(group_f, "WaterLevel", {"-4321.5", "0"});
    WriteGroupFDataset(group_f, "SurfaceCurrent", {"-777.0", "-666.0"});

    ScopedH5Dataspace sc(H5Screate(H5S_SCALAR));
    ASSERT_GE(sc.get(), 0);

    // 1. S-102 BathymetryCoverage (2 Float32 components).
    {
      ScopedH5Group g0(H5Gcreate(fid, "BathymetryCoverage", 0));
      ScopedH5Group g1(H5Gcreate(g0, "BathymetryCoverage.01", 0));
      ScopedH5Group g2(H5Gcreate(g1, "Group_001", 0));
      ScopedH5Datatype t(H5Tcreate(H5T_COMPOUND, 2 * sizeof(float)));
      ASSERT_GE(H5Tinsert(t, "depth", 0, H5T_NATIVE_FLOAT), 0);
      ASSERT_GE(H5Tinsert(t, "uncertainty", sizeof(float), H5T_NATIVE_FLOAT),
                0);
      ScopedH5Dataset d(H5Dcreate(g2, "values", t, sc, H5P_DEFAULT));
      ASSERT_GE(d.get(), 0);
    }

    // 2. S-102 QualityOfSurvey & QualityOfBathymetryCoverage (UInt32).
    {
      ScopedH5Group g0(H5Gcreate(fid, "QualityOfSurvey", 0));
      ScopedH5Group g1(H5Gcreate(g0, "QualityOfSurvey.01", 0));
      ScopedH5Group g2(H5Gcreate(g1, "Group_001", 0));
      ScopedH5Dataset d(
          H5Dcreate(g2, "values", H5T_NATIVE_UINT32, sc, H5P_DEFAULT));
      ASSERT_GE(d.get(), 0);
    }
    {
      ScopedH5Group g0(H5Gcreate(fid, "QualityOfBathymetryCoverage", 0));
      ScopedH5Group g1(H5Gcreate(g0, "QualityOfBathymetryCoverage.01", 0));
      ScopedH5Group g2(H5Gcreate(g1, "Group_001", 0));
      ScopedH5Datatype t(H5Tcreate(H5T_COMPOUND, sizeof(uint32_t)));
      ASSERT_GE(H5Tinsert(t, "q", 0, H5T_NATIVE_UINT32), 0);
      ScopedH5Dataset d(H5Dcreate(g2, "values", t, sc, H5P_DEFAULT));
      ASSERT_GE(d.get(), 0);
    }

    // 3. S-104 WaterLevel ({Float32, Int32}, size 8).
    {
      ScopedH5Group g0(H5Gcreate(fid, "WaterLevel", 0));
      ScopedH5Group g1(H5Gcreate(g0, "WaterLevel.01", 0));
      ScopedH5Group g2(H5Gcreate(g1, "Group_001", 0));
      ScopedH5Datatype t(
          H5Tcreate(H5T_COMPOUND, sizeof(float) + sizeof(int32_t)));
      ASSERT_GE(H5Tinsert(t, "height", 0, H5T_NATIVE_FLOAT), 0);
      ASSERT_GE(H5Tinsert(t, "trend", sizeof(float), H5T_NATIVE_INT32), 0);
      ScopedH5Dataset d(H5Dcreate(g2, "values", t, sc, H5P_DEFAULT));
      ASSERT_GE(d.get(), 0);
    }

    // 4. S-111 SurfaceCurrent ({Float32, Float32}).
    {
      ScopedH5Group g0(H5Gcreate(fid, "SurfaceCurrent", 0));
      ScopedH5Group g1(H5Gcreate(g0, "SurfaceCurrent.01", 0));
      ScopedH5Group g2(H5Gcreate(g1, "Group_001", 0));
      ScopedH5Datatype t(H5Tcreate(H5T_COMPOUND, 2 * sizeof(float)));
      ASSERT_GE(H5Tinsert(t, "speed", 0, H5T_NATIVE_FLOAT), 0);
      ASSERT_GE(H5Tinsert(t, "dir", sizeof(float), H5T_NATIVE_FLOAT), 0);
      ScopedH5Dataset d(H5Dcreate(g2, "values", t, sc, H5P_DEFAULT));
      ASSERT_GE(d.get(), 0);
    }
  }

  // 5. Read back Group_F nodata tables across S-102, S-104, and S-111.
  {
    auto ds = OpenMultiDimDataset(s100_file.str());
    ASSERT_NE(nullptr, ds);
    auto root = ds->GetRootGroup();
    ASSERT_NE(nullptr, root);

    auto bathy = root->OpenMDArrayFromFullname(
        "/BathymetryCoverage/BathymetryCoverage.01/Group_001/values");
    ASSERT_NE(nullptr, bathy);
    ASSERT_NE(nullptr, bathy->GetRawNoDataValue());
    const float* bf = static_cast<const float*>(bathy->GetRawNoDataValue());
    EXPECT_FLOAT_EQ(1234.5f, bf[0]);
    EXPECT_FLOAT_EQ(678.25f, bf[1]);

    auto qos = root->OpenMDArrayFromFullname(
        "/QualityOfSurvey/QualityOfSurvey.01/Group_001/values");
    ASSERT_NE(nullptr, qos);
    EXPECT_EQ(9999u, qos->GetNoDataValueAsUInt64());

    auto qob = root->OpenMDArrayFromFullname(
        "/QualityOfBathymetryCoverage/QualityOfBathymetryCoverage.01/"
        "Group_001/values");
    ASSERT_NE(nullptr, qob);
    ASSERT_NE(nullptr, qob->GetRawNoDataValue());
    EXPECT_EQ(8888u, *static_cast<const uint32_t*>(qob->GetRawNoDataValue()));

    auto wl = root->OpenMDArrayFromFullname(
        "/WaterLevel/WaterLevel.01/Group_001/values");
    ASSERT_NE(nullptr, wl);
    ASSERT_NE(nullptr, wl->GetRawNoDataValue());
    EXPECT_FLOAT_EQ(-4321.5f,
                    *static_cast<const float*>(wl->GetRawNoDataValue()));

    auto sc = root->OpenMDArrayFromFullname(
        "/SurfaceCurrent/SurfaceCurrent.01/Group_001/values");
    ASSERT_NE(nullptr, sc);
    ASSERT_NE(nullptr, sc->GetRawNoDataValue());
    const float* sc_nd = static_cast<const float*>(sc->GetRawNoDataValue());
    EXPECT_FLOAT_EQ(-777.0f, sc_nd[0]);
    EXPECT_FLOAT_EQ(-666.0f, sc_nd[1]);
  }
}

// TODO: b/335317901 - Cover HDF5-EOS Grid explicit XDim/YDim 1D coordinate
// array reading in HDF5EOS2Grid when HDF5SharedResources shared_ptr cycle is
// resolved upstream.

}  // namespace
}  // namespace autotest2
