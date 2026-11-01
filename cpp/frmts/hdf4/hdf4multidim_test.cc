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

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/match.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/gdal/autotest2/cpp/frmts/hdf4/hdf4_test_util.h"
// clang-format off
#include "third_party/gdal/frmts/hdf4/hdf4dataset.h"
#include "third_party/gdal/frmts/hdf4/hdf-eos/HdfEosDef.h"
// clang-format on
#include "third_party/gdal/frmts/hdf4/hdf4drivercore.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_multidim.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

// Writes an empty 32000-byte StructMetadata.0 SD attribute so SWopen() and
// GDopen() in HDF4Group::GetGroupNames() / OpenGroup() succeed and close
// cleanly without leaking internal HDF-EOS handles.
void WriteEmptyStructMetadata(int32_t sd_id) {
  std::string meta =
      "GROUP=SwathStructure\n"
      "END_GROUP=SwathStructure\n"
      "GROUP=GridStructure\n"
      "END_GROUP=GridStructure\n"
      "GROUP=PointStructure\n"
      "END_GROUP=PointStructure\n"
      "END\n";
  meta.resize(32000, '\0');
  ASSERT_EQ(
      SDsetattr(sd_id, "StructMetadata.0", DFNT_CHAR8, 32000, meta.data()), 0);
}

// libhdf4's SDend() names the root CDF0.0 Vgroup after the full file path,
// while HDF-EOS SWattach()/GDattach() read Vgroup names into char name[80].
// Shortening CDF0.0's name prevents stack buffer overflows when TempDir() is
// long.
void ShortenCdfVgroupName(int32_t hdf_id) {
  int32_t cdf_ref = Vfindclass(hdf_id, "CDF0.0");
  if (cdf_ref != -1) {
    int32_t vg_cdf = Vattach(hdf_id, cdf_ref, "w");
    if (vg_cdf != -1) {
      Vsetname(vg_cdf, "cdf");
      Vdetach(vg_cdf);
    }
  }
}

// libhdf4's HXsetdir() (hextelt.c:1144) leaks strdup(newdir) whenever it is
// called consecutively with the same directory string. Resetting
// HXsetdir(nullptr) after each Read() avoids that upstream leak.
bool ReadArrayZeroLeak(const GDALMDArray* array, const GUInt64* array_start_idx,
                       const size_t* count, const GInt64* array_step,
                       const GPtrDiff_t* buffer_stride,
                       const GDALExtendedDataType& buffer_data_type,
                       void* dst_buffer) {
  bool ok = array->Read(array_start_idx, count, array_step, buffer_stride,
                        buffer_data_type, dst_buffer);
  HXsetdir(nullptr);
  return ok;
}

std::shared_ptr<GDALAttribute> FindAttr(
    const std::vector<std::shared_ptr<GDALAttribute>>& attrs,
    std::string_view name) {
  for (const auto& attr : attrs) {
    if (attr->GetName() == name) {
      return attr;
    }
  }
  return nullptr;
}

TEST(Hdf4MultiDimTest, GdalCreatedDatasets2DAnd3D) {
  GDALAllRegister();
  GDALDriver* driver =
      GetGDALDriverManager()->GetDriverByName(HDF4_IMAGE_DRIVER_NAME);
  ASSERT_NE(driver, nullptr);

  // 1. 3D GDAL-created HDF4 dataset (RANK=3, 2 bands, EPSG:4326 -> {1, 2} axis
  // mapping).
  TempFile tmp_3d("md3.hdf");
  {
    auto ds = std::unique_ptr<GDALDataset>(
        driver->Create(tmp_3d.path(), 6, 4, 2, GDT_Int16, nullptr));
    ASSERT_NE(ds, nullptr);
    GDALGeoTransform gt{-120.0, 0.5, 0.0, 45.0, 0.0, -0.25};
    ASSERT_EQ(ds->SetGeoTransform(gt), CE_None);
    OGRSpatialReference srs;
    ASSERT_EQ(srs.importFromEPSG(4326), OGRERR_NONE);
    ASSERT_EQ(ds->SetSpatialRef(&srs), CE_None);

    std::vector<int16_t> b1(24);
    std::vector<int16_t> b2(24);
    for (int i = 0; i < 24; ++i) {
      b1[i] = static_cast<int16_t>(10 + i);
      b2[i] = static_cast<int16_t>(100 + i);
    }
    ASSERT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 6, 4, b1.data(), 6,
                                             4, GDT_Int16, 0, 0),
              CE_None);
    ASSERT_EQ(ds->GetRasterBand(2)->RasterIO(GF_Write, 0, 0, 6, 4, b2.data(), 6,
                                             4, GDT_Int16, 0, 0),
              CE_None);
  }

  {
    auto ds = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        tmp_3d.path(), GDAL_OF_MULTIDIM_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    auto root = ds->GetRootGroup();
    ASSERT_NE(root, nullptr);

    // GDAL-created datasets expose arrays directly at root and have no
    // subgroups.
    EXPECT_TRUE(root->GetGroupNames().empty());
    EXPECT_EQ(root->OpenGroup("swaths"), nullptr);

    auto dims = root->GetDimensions();
    ASSERT_EQ(dims.size(), 3);
    EXPECT_EQ(dims[0]->GetName(), "Y");
    EXPECT_EQ(dims[0]->GetType(), GDAL_DIM_TYPE_HORIZONTAL_Y);
    EXPECT_EQ(dims[0]->GetSize(), 4);
    ASSERT_NE(dims[0]->GetIndexingVariable(), nullptr);
    EXPECT_EQ(dims[0]->GetIndexingVariable()->GetName(), "Y");

    EXPECT_EQ(dims[1]->GetName(), "X");
    EXPECT_EQ(dims[1]->GetType(), GDAL_DIM_TYPE_HORIZONTAL_X);
    EXPECT_EQ(dims[1]->GetSize(), 6);
    ASSERT_NE(dims[1]->GetIndexingVariable(), nullptr);
    EXPECT_EQ(dims[1]->GetIndexingVariable()->GetName(), "X");

    EXPECT_EQ(dims[2]->GetName(), "Band");
    EXPECT_EQ(dims[2]->GetSize(), 2);

    auto array_names = root->GetMDArrayNames();
    EXPECT_EQ(array_names, (std::vector<std::string>{
                               "3-dimensional Scientific Dataset", "X", "Y"}));

    EXPECT_NE(root->OpenMDArray("X"), nullptr);
    EXPECT_NE(root->OpenMDArray("Y"), nullptr);
    EXPECT_EQ(root->OpenMDArray("nonexistent"), nullptr);

    auto arr = root->OpenMDArray("3-dimensional Scientific Dataset");
    ASSERT_NE(arr, nullptr);
    EXPECT_FALSE(arr->IsWritable());
    EXPECT_EQ(arr->GetFilename(), tmp_3d.str());
    EXPECT_NE(arr->GetRootGroup(), nullptr);

    auto srs = arr->GetSpatialRef();
    ASSERT_NE(srs, nullptr);
    EXPECT_EQ(srs->GetDataAxisToSRSAxisMapping(), (std::vector<int>{1, 2}));

    GUInt64 start[3] = {0, 0, 0};
    size_t count[3] = {4, 6, 2};
    std::vector<int16_t> out(48, 0);
    ASSERT_TRUE(ReadArrayZeroLeak(arr.get(), start, count, nullptr, nullptr,
                                  GDALExtendedDataType::Create(GDT_Int16),
                                  out.data()));
    EXPECT_EQ(out[0], 10);
    EXPECT_EQ(out[1], 100);
    EXPECT_EQ(out[46], 33);
    EXPECT_EQ(out[47], 123);
  }

  // 2. 2D GDAL-created HDF4 dataset (RANK=2, 1 band, EPSG:32616 -> {2, 1} axis
  // mapping).
  TempFile tmp_2d("md2.hdf");
  {
    char* create_opts[] = {const_cast<char*>("RANK=2"), nullptr};
    auto ds = std::unique_ptr<GDALDataset>(
        driver->Create(tmp_2d.path(), 6, 4, 1, GDT_Float32, create_opts));
    ASSERT_NE(ds, nullptr);
    GDALGeoTransform gt{500000.0, 100.0, 0.0, 4000000.0, 0.0, -100.0};
    ASSERT_EQ(ds->SetGeoTransform(gt), CE_None);
    OGRSpatialReference srs;
    ASSERT_EQ(srs.importFromEPSG(32616), OGRERR_NONE);
    ASSERT_EQ(ds->SetSpatialRef(&srs), CE_None);

    std::vector<float> buf(24, 3.25f);
    buf[5] = 7.5f;
    ASSERT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 6, 4, buf.data(),
                                             6, 4, GDT_Float32, 0, 0),
              CE_None);
  }

  {
    auto ds = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        tmp_2d.path(), GDAL_OF_MULTIDIM_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    auto root = ds->GetRootGroup();
    ASSERT_NE(root, nullptr);

    auto dims = root->GetDimensions();
    ASSERT_EQ(dims.size(), 2);
    EXPECT_EQ(dims[0]->GetName(), "Y");
    EXPECT_EQ(dims[1]->GetName(), "X");

    auto arr = root->OpenMDArray("Band0");
    ASSERT_NE(arr, nullptr);
    auto srs = arr->GetSpatialRef();
    ASSERT_NE(srs, nullptr);
    EXPECT_EQ(srs->GetDataAxisToSRSAxisMapping(), (std::vector<int>{2, 1}));

    GUInt64 start[2] = {0, 0};
    size_t count[2] = {4, 6};
    std::vector<float> out(24, 0.0f);
    ASSERT_TRUE(ReadArrayZeroLeak(arr.get(), start, count, nullptr, nullptr,
                                  GDALExtendedDataType::Create(GDT_Float32),
                                  out.data()));
    EXPECT_FLOAT_EQ(out[0], 3.25f);
    EXPECT_FLOAT_EQ(out[5], 7.5f);
  }
}

TEST(Hdf4MultiDimTest, RootAttributesSdsAndGeneralRasters) {
  GDALAllRegister();
  TempFile tmp("sds.hdf");

  {
    int32_t sd_id = SDstart(tmp.path(), DFACC_CREATE);
    ASSERT_NE(sd_id, -1);
    WriteEmptyStructMetadata(sd_id);

    const std::string core_meta =
        "GROUP = INVENTORYMETADATA\n"
        "  OBJECT = SHORTNAME\n"
        "    VALUE = \"TESTMOD\"\n"
        "  END_OBJECT = SHORTNAME\n"
        "  OBJECT = identifier_product_doi\n"
        "    VALUE = \"10.5067/TEST\"\n"
        "  END_OBJECT = identifier_product_doi\n"
        "  OBJECT = conflict_attr\n"
        "    VALUE = \"odl_val\"\n"
        "  END_OBJECT = conflict_attr\n"
        "END_GROUP = INVENTORYMETADATA\n"
        "END\n";
    SetCharAttr(sd_id, "coremetadata.0", core_meta);

    const std::string archive_meta =
        "GROUP = ARCHIVEDMETADATA\n"
        "  OBJECT = ARCHIVED_FLAG\n"
        "    VALUE = \"YES\"\n"
        "  END_OBJECT = ARCHIVED_FLAG\n"
        "END_GROUP = ARCHIVEDMETADATA\n"
        "END\n";
    SetCharAttr(sd_id, "archivemetadata.0", archive_meta);

    // Duplicate DOI with identical value (deduplicated) + conflicting attr
    // with different value.
    SetCharAttr(sd_id, "identifier_product_doi", "10.5067/TEST");
    SetCharAttr(sd_id, "conflict_attr", "sd_val");

    int32_t scalar_int = 42;
    ASSERT_EQ(SDsetattr(sd_id, "scalar_int", DFNT_INT32, 1, &scalar_int), 0);
    float array_float[3] = {1.5f, 2.5f, 3.5f};
    ASSERT_EQ(SDsetattr(sd_id, "array_float", DFNT_FLOAT32, 3, array_float), 0);

    // 1. data_sds (4x6 INT16) with named coordinate dimensions "lat" and "lon",
    // numeric _FillValue, float32 scale_factor, float64 add_offset, and units.
    {
      int32_t dims[2] = {4, 6};
      int32_t sds = SDcreate(sd_id, "data_sds", DFNT_INT16, 2, dims);
      ASSERT_NE(sds, -1);

      double lat_vals[4] = {40.0, 39.0, 38.0, 37.0};
      int32_t dim0 = SDgetdimid(sds, 0);
      ASSERT_EQ(SDsetdimname(dim0, "lat"), 0);
      ASSERT_EQ(SDsetdimscale(dim0, 4, DFNT_FLOAT64, lat_vals), 0);

      double lon_vals[6] = {-105.0, -104.0, -103.0, -102.0, -101.0, -100.0};
      int32_t dim1 = SDgetdimid(sds, 1);
      ASSERT_EQ(SDsetdimname(dim1, "lon"), 0);
      ASSERT_EQ(SDsetdimscale(dim1, 6, DFNT_FLOAT64, lon_vals), 0);

      double fill_val = -999.0;
      ASSERT_EQ(SDsetattr(sds, "_FillValue", DFNT_FLOAT64, 1, &fill_val), 0);
      float scale_val = 0.5f;
      ASSERT_EQ(SDsetattr(sds, "scale_factor", DFNT_FLOAT32, 1, &scale_val), 0);
      double offset_val = 12.5;
      ASSERT_EQ(SDsetattr(sds, "add_offset", DFNT_FLOAT64, 1, &offset_val), 0);
      SetCharAttr(sds, "units", "Kelvin");

      std::vector<int16_t> buf(24);
      for (int i = 0; i < 24; ++i) {
        buf[i] = static_cast<int16_t>(100 + i);
      }
      int32_t start[2] = {0, 0};
      ASSERT_EQ(SDwritedata(sds, start, nullptr, dims, buf.data()), 0);
      ASSERT_EQ(SDendaccess(sds), 0);

      // Attach "axis" attributes to the coordinate variable SDSs created by
      // SDsetdimscale.
      int32_t lat_idx = SDnametoindex(sd_id, "lat");
      ASSERT_NE(lat_idx, -1);
      int32_t lat_sds = SDselect(sd_id, lat_idx);
      SetCharAttr(lat_sds, "axis", "Y");
      ASSERT_EQ(SDendaccess(lat_sds), 0);

      int32_t lon_idx = SDnametoindex(sd_id, "lon");
      ASSERT_NE(lon_idx, -1);
      int32_t lon_sds = SDselect(sd_id, lon_idx);
      SetCharAttr(lon_sds, "axis", "X");
      ASSERT_EQ(SDendaccess(lon_sds), 0);
    }

    // 2. fill_api_sds: fill value set via SDsetfillvalue (no _FillValue attr).
    {
      int32_t dims[2] = {4, 6};
      int32_t sds = SDcreate(sd_id, "fill_api_sds", DFNT_INT16, 2, dims);
      ASSERT_NE(sds, -1);
      int16_t fv = -32767;
      ASSERT_EQ(SDsetfillvalue(sds, &fv), 0);
      std::vector<int16_t> buf(24, 5);
      int32_t start[2] = {0, 0};
      ASSERT_EQ(SDwritedata(sds, start, nullptr, dims, buf.data()), 0);
      ASSERT_EQ(SDendaccess(sds), 0);
    }

    // 3. plain_sds: unnamed dimensions, no fill value, no scale/offset/units.
    {
      int32_t dims[2] = {4, 6};
      int32_t sds = SDcreate(sd_id, "plain_sds", DFNT_UINT8, 2, dims);
      ASSERT_NE(sds, -1);
      std::vector<uint8_t> buf(24, 7);
      int32_t start[2] = {0, 0};
      ASSERT_EQ(SDwritedata(sds, start, nullptr, dims, buf.data()), 0);
      ASSERT_EQ(SDendaccess(sds), 0);
    }

    ASSERT_EQ(SDend(sd_id), 0);

    // Add General Rasters (GR) to the same HDF4 file.
    int32_t hdf_id = Hopen(tmp.path(), DFACC_RDWR, 0);
    ASSERT_NE(hdf_id, -1);
    int32_t gr_id = GRstart(hdf_id);
    ASSERT_NE(gr_id, -1);

    const char gr_global[] = "GRGlobalValue";
    ASSERT_EQ(GRsetattr(gr_id, "gr_global_attr", DFNT_CHAR8,
                        static_cast<int32_t>(strlen(gr_global)), gr_global),
              0);

    // GR image 0: 3-band UINT16 with image-level attribute.
    {
      int32_t dims[2] = {6, 4};
      int32_t ri =
          GRcreate(gr_id, "rgb_gr", 3, DFNT_UINT16, MFGR_INTERLACE_PIXEL, dims);
      ASSERT_NE(ri, -1);
      int32_t img_attr[2] = {100, 200};
      ASSERT_EQ(GRsetattr(ri, "gr_img_attr", DFNT_INT32, 2, img_attr), 0);

      std::vector<uint16_t> rgb_data(6 * 4 * 3);
      for (int i = 0; i < 24; ++i) {
        rgb_data[3 * i + 0] = static_cast<uint16_t>(100 + i);
        rgb_data[3 * i + 1] = static_cast<uint16_t>(200 + i);
        rgb_data[3 * i + 2] = static_cast<uint16_t>(300 + i);
      }
      int32_t start[2] = {0, 0};
      ASSERT_EQ(GRwriteimage(ri, start, nullptr, dims, rgb_data.data()), 0);
      ASSERT_EQ(GRendaccess(ri), 0);
    }

    // GR image 1: 1-band UINT8 with 256-entry RGB palette.
    {
      int32_t dims[2] = {6, 4};
      int32_t ri =
          GRcreate(gr_id, "pal_gr", 1, DFNT_UINT8, MFGR_INTERLACE_PIXEL, dims);
      ASSERT_NE(ri, -1);
      std::vector<uint8_t> data(24, 3);
      int32_t start[2] = {0, 0};
      ASSERT_EQ(GRwriteimage(ri, start, nullptr, dims, data.data()), 0);

      int32_t lut_id = GRgetlutid(ri, 0);
      ASSERT_NE(lut_id, -1);
      uint8_t pal[256][3];
      for (int i = 0; i < 256; ++i) {
        pal[i][0] = static_cast<uint8_t>(i);
        pal[i][1] = static_cast<uint8_t>(255 - i);
        pal[i][2] = static_cast<uint8_t>(i * 2);
      }
      ASSERT_EQ(
          GRwritelut(lut_id, 3, DFNT_UINT8, MFGR_INTERLACE_PIXEL, 256, pal), 0);
      ASSERT_EQ(GRendaccess(ri), 0);
    }

    ASSERT_EQ(GRend(gr_id), 0);
    ASSERT_EQ(Hclose(hdf_id), 0);
  }

  // Test LIST_SDS=NO open option omits "scientific_datasets".
  {
    const char* opts[] = {"LIST_SDS=NO", nullptr};
    auto ds = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        tmp.path(), GDAL_OF_MULTIDIM_RASTER | GDAL_OF_READONLY, nullptr, opts,
        nullptr));
    ASSERT_NE(ds, nullptr);
    auto root = ds->GetRootGroup();
    ASSERT_NE(root, nullptr);
    auto groups = root->GetGroupNames();
    EXPECT_EQ(std::find(groups.begin(), groups.end(), "scientific_datasets"),
              groups.end());
    EXPECT_NE(std::find(groups.begin(), groups.end(), "general_rasters"),
              groups.end());
  }

  // Open with default options (LIST_SDS=AUTO).
  auto ds = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      tmp.path(), GDAL_OF_MULTIDIM_RASTER | GDAL_OF_READONLY));
  ASSERT_NE(ds, nullptr);
  auto root = ds->GetRootGroup();
  ASSERT_NE(root, nullptr);

  EXPECT_EQ(
      root->GetGroupNames(),
      (std::vector<std::string>{"scientific_datasets", "general_rasters"}));
  EXPECT_EQ(root->OpenGroup("unknown_group"), nullptr);
  EXPECT_TRUE(root->GetMDArrayNames().empty());
  EXPECT_EQ(root->OpenMDArray("any"), nullptr);
  EXPECT_TRUE(root->GetDimensions().empty());

  // Root attributes: ODL + SD + deduplication + conflict + scalar + 1D array.
  auto root_attrs = root->GetAttributes();
  auto attr_shortname = FindAttr(root_attrs, "SHORTNAME");
  ASSERT_NE(attr_shortname, nullptr);
  EXPECT_STREQ(attr_shortname->ReadAsString(), "TESTMOD");

  auto attr_archive = FindAttr(root_attrs, "ARCHIVED_FLAG");
  ASSERT_NE(attr_archive, nullptr);
  EXPECT_STREQ(attr_archive->ReadAsString(), "YES");

  auto attr_doi = FindAttr(root_attrs, "identifier_product_doi");
  ASSERT_NE(attr_doi, nullptr);
  EXPECT_STREQ(attr_doi->ReadAsString(), "10.5067/TEST");

  int conflict_count = 0;
  for (const auto& attr : root_attrs) {
    if (attr->GetName() == "conflict_attr") {
      ++conflict_count;
    }
  }
  EXPECT_EQ(conflict_count, 2);

  auto attr_scalar = FindAttr(root_attrs, "scalar_int");
  ASSERT_NE(attr_scalar, nullptr);
  EXPECT_TRUE(attr_scalar->GetDimensions().empty());
  EXPECT_EQ(attr_scalar->ReadAsInt(), 42);

  auto attr_arr = FindAttr(root_attrs, "array_float");
  ASSERT_NE(attr_arr, nullptr);
  ASSERT_EQ(attr_arr->GetDimensions().size(), 1);
  EXPECT_EQ(attr_arr->GetDimensions()[0]->GetSize(), 3);
  {
    GUInt64 start[1] = {2};
    size_t count[1] = {2};
    GInt64 step[1] = {-1};
    double vals[2] = {0.0, 0.0};
    ASSERT_TRUE(attr_arr->Read(start, count, step, nullptr,
                               GDALExtendedDataType::Create(GDT_Float64),
                               vals));
    EXPECT_DOUBLE_EQ(vals[0], 3.5);
    EXPECT_DOUBLE_EQ(vals[1], 2.5);
  }

  // Test scientific_datasets group and HDF4SDSArray.
  auto sds_group = root->OpenGroup("scientific_datasets");
  ASSERT_NE(sds_group, nullptr);
  auto sds_dims = sds_group->GetDimensions();
  // Second call hits cached m_dims (2 named dims on data_sds + 2 fakeDims on
  // fill_api_sds + 2 fakeDims on plain_sds = 6).
  EXPECT_EQ(sds_group->GetDimensions().size(), sds_dims.size());
  ASSERT_EQ(sds_dims.size(), 6);
  bool found_lat = false;
  bool found_lon = false;
  for (const auto& dim : sds_dims) {
    if (dim->GetName() == "lat") {
      found_lat = true;
      EXPECT_EQ(dim->GetType(), GDAL_DIM_TYPE_HORIZONTAL_Y);
      EXPECT_NE(dim->GetIndexingVariable(), nullptr);
    } else if (dim->GetName() == "lon") {
      found_lon = true;
      EXPECT_EQ(dim->GetType(), GDAL_DIM_TYPE_HORIZONTAL_X);
      EXPECT_NE(dim->GetIndexingVariable(), nullptr);
    }
  }
  EXPECT_TRUE(found_lat);
  EXPECT_TRUE(found_lon);

  EXPECT_EQ(sds_group->OpenMDArray("missing_sds"), nullptr);

  auto data_sds = sds_group->OpenMDArray("data_sds");
  ASSERT_NE(data_sds, nullptr);
  EXPECT_FALSE(data_sds->IsWritable());
  EXPECT_EQ(data_sds->GetFilename(), tmp.str());
  EXPECT_NE(data_sds->GetRootGroup(), nullptr);
  EXPECT_EQ(data_sds->GetSpatialRef(), nullptr);
  EXPECT_EQ(data_sds->GetAttributes().size(), 4);

  const void* raw_nd = data_sds->GetRawNoDataValue();
  ASSERT_NE(raw_nd, nullptr);
  // Second call hits cached m_abyNoData.
  EXPECT_EQ(data_sds->GetRawNoDataValue(), raw_nd);
  EXPECT_EQ(*static_cast<const int16_t*>(raw_nd), -999);

  bool has_scale = false;
  GDALDataType scale_dt = GDT_Unknown;
  EXPECT_DOUBLE_EQ(data_sds->GetScale(&has_scale, &scale_dt), 0.5);
  EXPECT_TRUE(has_scale);
  EXPECT_EQ(scale_dt, GDT_Float32);

  bool has_offset = false;
  GDALDataType offset_dt = GDT_Unknown;
  EXPECT_DOUBLE_EQ(data_sds->GetOffset(&has_offset, &offset_dt), 12.5);
  EXPECT_TRUE(has_offset);
  EXPECT_EQ(offset_dt, GDT_Float64);

  EXPECT_EQ(data_sds->GetUnit(), "Kelvin");

  // Fast-path contiguous read.
  {
    GUInt64 start[2] = {0, 0};
    size_t count[2] = {4, 6};
    std::vector<int16_t> out(24, 0);
    ASSERT_TRUE(
        ReadArrayZeroLeak(data_sds.get(), start, count, nullptr, nullptr,
                          GDALExtendedDataType::Create(GDT_Int16), out.data()));
    EXPECT_EQ(out[0], 100);
    EXPECT_EQ(out[23], 123);
  }

  // Slow-path read with negative step and type conversion to Float64.
  {
    GUInt64 start[2] = {3, 0};
    size_t count[2] = {4, 6};
    GInt64 step[2] = {-1, 1};
    std::vector<double> out(24, 0.0);
    ASSERT_TRUE(ReadArrayZeroLeak(data_sds.get(), start, count, step, nullptr,
                                  GDALExtendedDataType::Create(GDT_Float64),
                                  out.data()));
    // Row 0 of output is row 3 of source (100 + 3*6 = 118).
    EXPECT_DOUBLE_EQ(out[0], 118.0);
    EXPECT_DOUBLE_EQ(out[18], 100.0);
  }

  auto fill_api_sds = sds_group->OpenMDArray("fill_api_sds");
  ASSERT_NE(fill_api_sds, nullptr);
  const void* api_nd = fill_api_sds->GetRawNoDataValue();
  ASSERT_NE(api_nd, nullptr);
  EXPECT_EQ(*static_cast<const int16_t*>(api_nd), -32767);

  auto plain_sds = sds_group->OpenMDArray("plain_sds");
  ASSERT_NE(plain_sds, nullptr);
  ASSERT_EQ(plain_sds->GetDimensions().size(), 2);
  EXPECT_FALSE(plain_sds->GetDimensions()[0]->GetName().empty());
  EXPECT_FALSE(plain_sds->GetDimensions()[1]->GetName().empty());
  EXPECT_EQ(plain_sds->GetRawNoDataValue(), nullptr);
  EXPECT_DOUBLE_EQ(plain_sds->GetScale(&has_scale), 1.0);
  EXPECT_FALSE(has_scale);
  EXPECT_DOUBLE_EQ(plain_sds->GetOffset(&has_offset), 0.0);
  EXPECT_FALSE(has_offset);
  EXPECT_TRUE(plain_sds->GetUnit().empty());

  // Test general_rasters group, HDF4GRArray, HDF4GRAttribute, and
  // HDF4GRPalette.
  auto gr_group = root->OpenGroup("general_rasters");
  ASSERT_NE(gr_group, nullptr);
  auto gr_global_attrs = gr_group->GetAttributes();
  auto gr_gattr = FindAttr(gr_global_attrs, "gr_global_attr");
  ASSERT_NE(gr_gattr, nullptr);
  EXPECT_STREQ(gr_gattr->ReadAsString(), "GRGlobalValue");

  EXPECT_EQ(gr_group->GetMDArrayNames(),
            (std::vector<std::string>{"rgb_gr", "pal_gr"}));
  EXPECT_EQ(gr_group->OpenMDArray("missing_gr"), nullptr);

  auto rgb_gr = gr_group->OpenMDArray("rgb_gr");
  ASSERT_NE(rgb_gr, nullptr);
  EXPECT_FALSE(rgb_gr->IsWritable());
  EXPECT_EQ(rgb_gr->GetFilename(), tmp.str());
  EXPECT_NE(rgb_gr->GetRootGroup(), nullptr);

  const auto& rgb_dims = rgb_gr->GetDimensions();
  ASSERT_EQ(rgb_dims.size(), 3);
  EXPECT_EQ(rgb_dims[0]->GetName(), "y");
  EXPECT_EQ(rgb_dims[1]->GetName(), "x");
  EXPECT_EQ(rgb_dims[2]->GetName(), "bands");
  EXPECT_EQ(rgb_dims[2]->GetSize(), 3);

  auto rgb_attrs = rgb_gr->GetAttributes();
  auto gr_iattr = FindAttr(rgb_attrs, "gr_img_attr");
  ASSERT_NE(gr_iattr, nullptr);
  EXPECT_EQ(gr_iattr->ReadAsInt(), 100);

  const size_t d0 = static_cast<size_t>(rgb_dims[0]->GetSize());
  const size_t d1 = static_cast<size_t>(rgb_dims[1]->GetSize());
  const size_t d2 = static_cast<size_t>(rgb_dims[2]->GetSize());

  // Fast-path contiguous read of all 3 bands.
  {
    GUInt64 start[3] = {0, 0, 0};
    size_t count[3] = {d0, d1, d2};
    std::vector<uint16_t> out(d0 * d1 * d2, 0);
    ASSERT_TRUE(ReadArrayZeroLeak(rgb_gr.get(), start, count, nullptr, nullptr,
                                  GDALExtendedDataType::Create(GDT_UInt16),
                                  out.data()));
    EXPECT_EQ(out[0], 100);
    EXPECT_EQ(out[1], 200);
    EXPECT_EQ(out[2], 300);
  }

  // Slow-path read of band 1 only with negative step on dim 0 and Float64
  // conversion.
  {
    GUInt64 start[3] = {d0 - 1, 0, 1};
    size_t count[3] = {d0, d1, 1};
    GInt64 step[3] = {-1, 1, 1};
    std::vector<double> out(d0 * d1, 0.0);
    ASSERT_TRUE(ReadArrayZeroLeak(rgb_gr.get(), start, count, step, nullptr,
                                  GDALExtendedDataType::Create(GDT_Float64),
                                  out.data()));
    EXPECT_DOUBLE_EQ(out[0], 200.0 + static_cast<double>((d0 - 1) * d1));
    EXPECT_DOUBLE_EQ(out[(d0 - 1) * d1], 200.0);
  }

  auto pal_gr = gr_group->OpenMDArray("pal_gr");
  ASSERT_NE(pal_gr, nullptr);
  auto pal_attrs = pal_gr->GetAttributes();
  auto lut_attr = FindAttr(pal_attrs, "lut");
  ASSERT_NE(lut_attr, nullptr);
  ASSERT_EQ(lut_attr->GetDimensions().size(), 2);
  EXPECT_EQ(lut_attr->GetDimensions()[0]->GetName(), "index");
  EXPECT_EQ(lut_attr->GetDimensions()[0]->GetSize(), 256);
  EXPECT_EQ(lut_attr->GetDimensions()[1]->GetName(), "component");
  EXPECT_EQ(lut_attr->GetDimensions()[1]->GetSize(), 3);
  EXPECT_EQ(lut_attr->GetDataType().GetNumericDataType(), GDT_Byte);
  {
    GUInt64 start[2] = {10, 0};
    size_t count[2] = {2, 3};
    uint16_t lut_vals[2][3] = {};
    ASSERT_TRUE(lut_attr->Read(start, count, nullptr, nullptr,
                               GDALExtendedDataType::Create(GDT_UInt16),
                               lut_vals));
    EXPECT_EQ(lut_vals[0][0], 10);
    EXPECT_EQ(lut_vals[0][1], 245);
    EXPECT_EQ(lut_vals[0][2], 20);
    EXPECT_EQ(lut_vals[1][0], 11);
  }
}

TEST(Hdf4MultiDimTest, SwathsAndEosGridsMultidim) {
  GDALAllRegister();
  TempFile tmp("sg.hdf");

  const std::string struct_meta =
      "GROUP=SwathStructure\n"
      "\tGROUP=SWATH_1\n"
      "\t\tSwathName=\"TestSwath\"\n"
      "\t\tGROUP=Dimension\n"
      "\t\t\tOBJECT=Dimension_1\n"
      "\t\t\t\tDimensionName=\"YDim\"\n"
      "\t\t\t\tSize=4\n"
      "\t\t\tEND_OBJECT=Dimension_1\n"
      "\t\t\tOBJECT=Dimension_2\n"
      "\t\t\t\tDimensionName=\"XDim\"\n"
      "\t\t\t\tSize=6\n"
      "\t\t\tEND_OBJECT=Dimension_2\n"
      "\t\t\tOBJECT=Dimension_3\n"
      "\t\t\t\tDimensionName=\"OtherDim\"\n"
      "\t\t\t\tSize=2\n"
      "\t\t\tEND_OBJECT=Dimension_3\n"
      "\t\tEND_GROUP=Dimension\n"
      "\t\tGROUP=DimensionMap\n"
      "\t\tEND_GROUP=DimensionMap\n"
      "\t\tGROUP=IndexDimensionMap\n"
      "\t\tEND_GROUP=IndexDimensionMap\n"
      "\t\tGROUP=GeoField\n"
      "\t\t\tOBJECT=GeoField_1\n"
      "\t\t\t\tGeoFieldName=\"Latitude\"\n"
      "\t\t\t\tDataType=DFNT_FLOAT32\n"
      "\t\t\t\tDimList=(\"YDim\",\"XDim\")\n"
      "\t\t\tEND_OBJECT=GeoField_1\n"
      "\t\t\tOBJECT=GeoField_2\n"
      "\t\t\t\tGeoFieldName=\"Longitude\"\n"
      "\t\t\t\tDataType=DFNT_FLOAT32\n"
      "\t\t\t\tDimList=(\"YDim\",\"XDim\")\n"
      "\t\t\tEND_OBJECT=GeoField_2\n"
      "\t\tEND_GROUP=GeoField\n"
      "\t\tGROUP=DataField\n"
      "\t\t\tOBJECT=DataField_1\n"
      "\t\t\t\tDataFieldName=\"Band1\"\n"
      "\t\t\t\tDataType=DFNT_INT16\n"
      "\t\t\t\tDimList=(\"YDim\",\"XDim\")\n"
      "\t\t\tEND_OBJECT=DataField_1\n"
      "\t\t\tOBJECT=DataField_2\n"
      "\t\t\t\tDataFieldName=\"FillViaFV\"\n"
      "\t\t\t\tDataType=DFNT_INT16\n"
      "\t\t\t\tDimList=(\"YDim\",\"XDim\")\n"
      "\t\t\tEND_OBJECT=DataField_2\n"
      "\t\t\tOBJECT=DataField_3\n"
      "\t\t\t\tDataFieldName=\"PlainSwath\"\n"
      "\t\t\t\tDataType=DFNT_INT16\n"
      "\t\t\t\tDimList=(\"YDim\",\"XDim\")\n"
      "\t\t\tEND_OBJECT=DataField_3\n"
      "\t\t\tOBJECT=DataField_4\n"
      "\t\t\t\tDataFieldName=\"Cube3D\"\n"
      "\t\t\t\tDataType=DFNT_INT16\n"
      "\t\t\t\tDimList=(\"YDim\",\"XDim\",\"OtherDim\")\n"
      "\t\t\tEND_OBJECT=DataField_4\n"
      "\t\tEND_GROUP=DataField\n"
      "\t\tGROUP=MergedFields\n"
      "\t\tEND_GROUP=MergedFields\n"
      "\tEND_GROUP=SWATH_1\n"
      "END_GROUP=SwathStructure\n"
      "GROUP=GridStructure\n"
      "\tGROUP=GRID_1\n"
      "\t\tGridName=\"UTMGrid\"\n"
      "\t\tXDim=6\n"
      "\t\tYDim=4\n"
      "\t\tUpperLeftPointMtrs=(500000.000000,4000000.000000)\n"
      "\t\tLowerRightMtrs=(506000.000000,3996000.000000)\n"
      "\t\tProjection=GCTP_UTM\n"
      "\t\tZoneCode=16\n"
      "\t\tSphereCode=8\n"
      "\t\tGROUP=Dimension\n"
      "\t\t\tOBJECT=Dimension_1\n"
      "\t\t\t\tDimensionName=\"BandDim\"\n"
      "\t\t\t\tSize=2\n"
      "\t\t\tEND_OBJECT=Dimension_1\n"
      "\t\t\tOBJECT=Dimension_2\n"
      "\t\t\t\tDimensionName=\"YDim\"\n"
      "\t\t\t\tSize=4\n"
      "\t\t\tEND_OBJECT=Dimension_2\n"
      "\t\t\tOBJECT=Dimension_3\n"
      "\t\t\t\tDimensionName=\"XDim\"\n"
      "\t\t\t\tSize=6\n"
      "\t\t\tEND_OBJECT=Dimension_3\n"
      "\t\tEND_GROUP=Dimension\n"
      "\t\tGROUP=DataField\n"
      "\t\t\tOBJECT=DataField_1\n"
      "\t\t\t\tDataFieldName=\"Temp\"\n"
      "\t\t\t\tDataType=DFNT_INT16\n"
      "\t\t\t\tDimList=(\"YDim\",\"XDim\")\n"
      "\t\t\tEND_OBJECT=DataField_1\n"
      "\t\t\tOBJECT=DataField_2\n"
      "\t\t\t\tDataFieldName=\"FillViaFV\"\n"
      "\t\t\t\tDataType=DFNT_INT16\n"
      "\t\t\t\tDimList=(\"YDim\",\"XDim\")\n"
      "\t\t\tEND_OBJECT=DataField_2\n"
      "\t\t\tOBJECT=DataField_3\n"
      "\t\t\t\tDataFieldName=\"NoFillGrid\"\n"
      "\t\t\t\tDataType=DFNT_INT16\n"
      "\t\t\t\tDimList=(\"YDim\",\"XDim\")\n"
      "\t\t\tEND_OBJECT=DataField_3\n"
      "\t\t\tOBJECT=DataField_4\n"
      "\t\t\t\tDataFieldName=\"Cube\"\n"
      "\t\t\t\tDataType=DFNT_FLOAT32\n"
      "\t\t\t\tDimList=(\"BandDim\",\"YDim\",\"XDim\")\n"
      "\t\t\tEND_OBJECT=DataField_4\n"
      "\t\tEND_GROUP=DataField\n"
      "\t\tGROUP=MergedFields\n"
      "\t\tEND_GROUP=MergedFields\n"
      "\tEND_GROUP=GRID_1\n"
      "\tGROUP=GRID_2\n"
      "\t\tGridName=\"GeoGrid\"\n"
      "\t\tXDim=6\n"
      "\t\tYDim=4\n"
      "\t\tUpperLeftPointMtrs=(-100000000.000000,45000000.000000)\n"
      "\t\tLowerRightMtrs=(-90000000.000000,35000000.000000)\n"
      "\t\tProjection=GCTP_GEO\n"
      "\t\tGROUP=Dimension\n"
      "\t\tEND_GROUP=Dimension\n"
      "\t\tGROUP=DataField\n"
      "\t\t\tOBJECT=DataField_1\n"
      "\t\t\t\tDataFieldName=\"Geo2D\"\n"
      "\t\t\t\tDataType=DFNT_INT16\n"
      "\t\t\t\tDimList=(\"YDim\",\"XDim\")\n"
      "\t\t\tEND_OBJECT=DataField_1\n"
      "\t\tEND_GROUP=DataField\n"
      "\t\tGROUP=MergedFields\n"
      "\t\tEND_GROUP=MergedFields\n"
      "\tEND_GROUP=GRID_2\n"
      "END_GROUP=GridStructure\n"
      "GROUP=PointStructure\n"
      "END_GROUP=PointStructure\n"
      "END\n";

  {
    int32_t sd_id = SDstart(tmp.path(), DFACC_CREATE);
    ASSERT_NE(sd_id, -1);
    SetCharAttr(sd_id, "HDFEOSVersion", "HDFEOS_V2.12");

    std::string padded = struct_meta;
    padded.resize(32000, '\0');
    ASSERT_EQ(
        SDsetattr(sd_id, "StructMetadata.0", DFNT_CHAR8, 32000, padded.data()),
        0);

    int32_t dims2[2] = {4, 6};
    int32_t start2[2] = {0, 0};

    // Swath Geolocation SDSs.
    int32_t lat_ref = 0;
    int32_t lon_ref = 0;
    {
      int32_t sds_lat = SDcreate(sd_id, "Latitude", DFNT_FLOAT32, 2, dims2);
      std::vector<float> lat_buf(24, 35.0f);
      SDwritedata(sds_lat, start2, nullptr, dims2, lat_buf.data());
      lat_ref = SDidtoref(sds_lat);
      SDendaccess(sds_lat);

      int32_t sds_lon = SDcreate(sd_id, "Longitude", DFNT_FLOAT32, 2, dims2);
      std::vector<float> lon_buf(24, -115.0f);
      SDwritedata(sds_lon, start2, nullptr, dims2, lon_buf.data());
      lon_ref = SDidtoref(sds_lon);
      SDendaccess(sds_lon);
    }

    // Swath Data SDSs.
    int32_t sw_b1_ref = 0;
    int32_t sw_fv_ref = 0;
    int32_t sw_plain_ref = 0;
    int32_t sw_cube_ref = 0;
    {
      int32_t sds = SDcreate(sd_id, "Band1", DFNT_INT16, 2, dims2);
      double fv = -999.0;
      ASSERT_EQ(SDsetattr(sds, "_FillValue", DFNT_FLOAT64, 1, &fv), 0);
      SetCharAttr(sds, "units", "W/m2");
      std::vector<int16_t> buf(24);
      for (int i = 0; i < 24; ++i) {
        buf[i] = static_cast<int16_t>(50 + i);
      }
      SDwritedata(sds, start2, nullptr, dims2, buf.data());
      sw_b1_ref = SDidtoref(sds);
      SDendaccess(sds);

      int32_t sds_fv = SDcreate(sd_id, "FillViaFV", DFNT_INT16, 2, dims2);
      SDwritedata(sds_fv, start2, nullptr, dims2, buf.data());
      sw_fv_ref = SDidtoref(sds_fv);
      SDendaccess(sds_fv);

      int32_t sds_pl = SDcreate(sd_id, "PlainSwath", DFNT_INT16, 2, dims2);
      SDwritedata(sds_pl, start2, nullptr, dims2, buf.data());
      sw_plain_ref = SDidtoref(sds_pl);
      SDendaccess(sds_pl);

      int32_t dims3[3] = {4, 6, 2};
      int32_t start3[3] = {0, 0, 0};
      int32_t sds_c3 = SDcreate(sd_id, "Cube3D", DFNT_INT16, 3, dims3);
      std::vector<int16_t> buf3(48, 9);
      SDwritedata(sds_c3, start3, nullptr, dims3, buf3.data());
      sw_cube_ref = SDidtoref(sds_c3);
      SDendaccess(sds_c3);
    }

    // Grid SDSs.
    int32_t gd_temp_ref = 0;
    int32_t gd_fv_ref = 0;
    int32_t gd_nofill_ref = 0;
    int32_t gd_cube_ref = 0;
    int32_t gd_geo_ref = 0;
    {
      int32_t sds_temp = SDcreate(sd_id, "Temp", DFNT_INT16, 2, dims2);
      double fv = -999.0;
      ASSERT_EQ(SDsetattr(sds_temp, "_FillValue", DFNT_FLOAT64, 1, &fv), 0);
      float scale_val = 0.25f;
      ASSERT_EQ(
          SDsetattr(sds_temp, "scale_factor", DFNT_FLOAT32, 1, &scale_val), 0);
      double offset_val = 10.0;
      ASSERT_EQ(SDsetattr(sds_temp, "add_offset", DFNT_FLOAT64, 1, &offset_val),
                0);
      SetCharAttr(sds_temp, "units", "Celsius");
      std::vector<int16_t> buf(24);
      for (int i = 0; i < 24; ++i) {
        buf[i] = static_cast<int16_t>(200 + i);
      }
      SDwritedata(sds_temp, start2, nullptr, dims2, buf.data());
      gd_temp_ref = SDidtoref(sds_temp);
      SDendaccess(sds_temp);

      int32_t sds_fv = SDcreate(sd_id, "FillViaFV", DFNT_INT16, 2, dims2);
      SDwritedata(sds_fv, start2, nullptr, dims2, buf.data());
      gd_fv_ref = SDidtoref(sds_fv);
      SDendaccess(sds_fv);

      int32_t sds_nf = SDcreate(sd_id, "NoFillGrid", DFNT_INT16, 2, dims2);
      SDwritedata(sds_nf, start2, nullptr, dims2, buf.data());
      gd_nofill_ref = SDidtoref(sds_nf);
      SDendaccess(sds_nf);

      int32_t dims3[3] = {2, 4, 6};
      int32_t start3[3] = {0, 0, 0};
      int32_t sds_cube = SDcreate(sd_id, "Cube", DFNT_FLOAT32, 3, dims3);
      std::vector<float> fbuf(48, 2.5f);
      SDwritedata(sds_cube, start3, nullptr, dims3, fbuf.data());
      gd_cube_ref = SDidtoref(sds_cube);
      SDendaccess(sds_cube);

      int32_t sds_geo = SDcreate(sd_id, "Geo2D", DFNT_INT16, 2, dims2);
      SDwritedata(sds_geo, start2, nullptr, dims2, buf.data());
      gd_geo_ref = SDidtoref(sds_geo);
      SDendaccess(sds_geo);
    }

    // Close SD interface before opening Vgroup interface so DD blocks do not
    // collide across two concurrent write handles.
    ASSERT_EQ(SDend(sd_id), 0);

    int32_t hdf_id = Hopen(tmp.path(), DFACC_RDWR, 0);
    ASSERT_NE(hdf_id, -1);
    ASSERT_EQ(Vstart(hdf_id), 0);
    ShortenCdfVgroupName(hdf_id);

    // Build Swath Vgroups.
    int32_t vg_sw = Vattach(hdf_id, -1, "w");
    Vsetname(vg_sw, "TestSwath");
    Vsetclass(vg_sw, "SWATH");

    int32_t vg_sw_geo = Vattach(hdf_id, -1, "w");
    Vsetname(vg_sw_geo, "Geolocation Fields");
    Vsetclass(vg_sw_geo, "SWATH Vgroup");
    Vaddtagref(vg_sw_geo, DFTAG_NDG, lat_ref);
    Vaddtagref(vg_sw_geo, DFTAG_NDG, lon_ref);

    int32_t vg_sw_data = Vattach(hdf_id, -1, "w");
    Vsetname(vg_sw_data, "Data Fields");
    Vsetclass(vg_sw_data, "SWATH Vgroup");
    Vaddtagref(vg_sw_data, DFTAG_NDG, sw_b1_ref);
    Vaddtagref(vg_sw_data, DFTAG_NDG, sw_fv_ref);
    Vaddtagref(vg_sw_data, DFTAG_NDG, sw_plain_ref);
    Vaddtagref(vg_sw_data, DFTAG_NDG, sw_cube_ref);

    int32_t vg_sw_attr = Vattach(hdf_id, -1, "w");
    Vsetname(vg_sw_attr, "Swath Attributes");
    Vsetclass(vg_sw_attr, "SWATH Vgroup");
    char sw_char_attr[] = "SwathAttrText";
    ASSERT_EQ(
        EHattr(hdf_id, vg_sw_attr, "SwathCharAttr", DFNT_CHAR8,
               static_cast<int32_t>(strlen(sw_char_attr)), "w", sw_char_attr),
        0);
    float sw_float_attr[2] = {1.5f, 2.5f};
    ASSERT_EQ(EHattr(hdf_id, vg_sw_attr, "SwathFloatAttr", DFNT_FLOAT32, 2, "w",
                     sw_float_attr),
              0);
    int16_t fv_sw = -777;
    ASSERT_EQ(
        EHattr(hdf_id, vg_sw_attr, "_FV_FillViaFV", DFNT_INT16, 1, "w", &fv_sw),
        0);

    Vinsert(vg_sw, vg_sw_geo);
    Vinsert(vg_sw, vg_sw_data);
    Vinsert(vg_sw, vg_sw_attr);
    Vdetach(vg_sw_attr);
    Vdetach(vg_sw_data);
    Vdetach(vg_sw_geo);
    Vdetach(vg_sw);

    // Build UTMGrid Vgroups.
    int32_t vg_gd1 = Vattach(hdf_id, -1, "w");
    Vsetname(vg_gd1, "UTMGrid");
    Vsetclass(vg_gd1, "GRID");

    int32_t vg_gd1_data = Vattach(hdf_id, -1, "w");
    Vsetname(vg_gd1_data, "Data Fields");
    Vsetclass(vg_gd1_data, "GRID Vgroup");
    Vaddtagref(vg_gd1_data, DFTAG_NDG, gd_temp_ref);
    Vaddtagref(vg_gd1_data, DFTAG_NDG, gd_fv_ref);
    Vaddtagref(vg_gd1_data, DFTAG_NDG, gd_nofill_ref);
    Vaddtagref(vg_gd1_data, DFTAG_NDG, gd_cube_ref);

    int32_t vg_gd1_attr = Vattach(hdf_id, -1, "w");
    Vsetname(vg_gd1_attr, "Grid Attributes");
    Vsetclass(vg_gd1_attr, "GRID Vgroup");
    char gd_char_attr[] = "GridAttrText";
    ASSERT_EQ(
        EHattr(hdf_id, vg_gd1_attr, "GridCharAttr", DFNT_CHAR8,
               static_cast<int32_t>(strlen(gd_char_attr)), "w", gd_char_attr),
        0);
    int32_t gd_int_attr[2] = {10, 20};
    ASSERT_EQ(EHattr(hdf_id, vg_gd1_attr, "GridIntAttr", DFNT_INT32, 2, "w",
                     gd_int_attr),
              0);
    int16_t fv_gd = -888;
    ASSERT_EQ(EHattr(hdf_id, vg_gd1_attr, "_FV_FillViaFV", DFNT_INT16, 1, "w",
                     &fv_gd),
              0);

    Vinsert(vg_gd1, vg_gd1_data);
    Vinsert(vg_gd1, vg_gd1_attr);
    Vdetach(vg_gd1_attr);
    Vdetach(vg_gd1_data);
    Vdetach(vg_gd1);

    // Build GeoGrid Vgroups.
    int32_t vg_gd2 = Vattach(hdf_id, -1, "w");
    Vsetname(vg_gd2, "GeoGrid");
    Vsetclass(vg_gd2, "GRID");

    int32_t vg_gd2_data = Vattach(hdf_id, -1, "w");
    Vsetname(vg_gd2_data, "Data Fields");
    Vsetclass(vg_gd2_data, "GRID Vgroup");
    Vaddtagref(vg_gd2_data, DFTAG_NDG, gd_geo_ref);

    int32_t vg_gd2_attr = Vattach(hdf_id, -1, "w");
    Vsetname(vg_gd2_attr, "Grid Attributes");
    Vsetclass(vg_gd2_attr, "GRID Vgroup");

    Vinsert(vg_gd2, vg_gd2_data);
    Vinsert(vg_gd2, vg_gd2_attr);
    Vdetach(vg_gd2_attr);
    Vdetach(vg_gd2_data);
    Vdetach(vg_gd2);

    ASSERT_EQ(Vend(hdf_id), 0);
    ASSERT_EQ(Hclose(hdf_id), 0);
  }

  // Test LIST_SDS=YES includes "scientific_datasets" alongside "swaths" and
  // "eos_grids".
  {
    const char* opts[] = {"LIST_SDS=YES", nullptr};
    auto ds_yes = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        tmp.path(), GDAL_OF_MULTIDIM_RASTER | GDAL_OF_READONLY, nullptr, opts,
        nullptr));
    ASSERT_NE(ds_yes, nullptr);
    auto root_yes = ds_yes->GetRootGroup();
    ASSERT_NE(root_yes, nullptr);
    EXPECT_EQ(root_yes->GetGroupNames(),
              (std::vector<std::string>{"swaths", "eos_grids",
                                        "scientific_datasets"}));
  }

  // Open with default options (LIST_SDS=AUTO).
  auto ds = std::unique_ptr<GDALDataset>(GDALDataset::Open(
      tmp.path(), GDAL_OF_MULTIDIM_RASTER | GDAL_OF_READONLY));
  ASSERT_NE(ds, nullptr);
  auto root = ds->GetRootGroup();
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(root->GetGroupNames(),
            (std::vector<std::string>{"swaths", "eos_grids"}));

  // 1. Test Swaths hierarchy.
  auto swaths_group = root->OpenGroup("swaths");
  ASSERT_NE(swaths_group, nullptr);
  EXPECT_EQ(swaths_group->GetGroupNames(),
            (std::vector<std::string>{"TestSwath"}));
  EXPECT_EQ(swaths_group->OpenGroup("missing_swath"), nullptr);

  auto swath = swaths_group->OpenGroup("TestSwath");
  ASSERT_NE(swath, nullptr);
  auto sw_dims = swath->GetDimensions();
  // Second call hits cached m_dims.
  EXPECT_EQ(swath->GetDimensions().size(), sw_dims.size());
  EXPECT_EQ(sw_dims.size(), 3);

  auto sw_attrs = swath->GetAttributes();
  auto sw_cattr = FindAttr(sw_attrs, "SwathCharAttr");
  ASSERT_NE(sw_cattr, nullptr);
  EXPECT_STREQ(sw_cattr->ReadAsString(), "SwathAttrText");
  auto sw_fattr = FindAttr(sw_attrs, "SwathFloatAttr");
  ASSERT_NE(sw_fattr, nullptr);
  EXPECT_DOUBLE_EQ(sw_fattr->ReadAsDouble(), 1.5);

  EXPECT_EQ(swath->GetGroupNames(),
            (std::vector<std::string>{"Data Fields", "Geolocation Fields"}));
  EXPECT_EQ(swath->OpenGroup("missing_sub"), nullptr);

  auto geo_fields = swath->OpenGroup("Geolocation Fields");
  ASSERT_NE(geo_fields, nullptr);
  EXPECT_EQ(geo_fields->GetMDArrayNames(),
            (std::vector<std::string>{"Latitude", "Longitude"}));
  EXPECT_NE(geo_fields->OpenMDArray("Latitude"), nullptr);

  auto data_fields = swath->OpenGroup("Data Fields");
  ASSERT_NE(data_fields, nullptr);
  EXPECT_EQ(
      data_fields->GetMDArrayNames(),
      (std::vector<std::string>{"Band1", "FillViaFV", "PlainSwath", "Cube3D"}));
  EXPECT_EQ(data_fields->OpenMDArray("missing_field"), nullptr);

  auto sw_b1 = data_fields->OpenMDArray("Band1");
  ASSERT_NE(sw_b1, nullptr);
  EXPECT_FALSE(sw_b1->IsWritable());
  EXPECT_EQ(sw_b1->GetFilename(), tmp.str());
  EXPECT_EQ(sw_b1->GetDimensions().size(), 2);
  EXPECT_EQ(sw_b1->GetAttributes().size(), 2);
  const void* sw_nd = sw_b1->GetRawNoDataValue();
  ASSERT_NE(sw_nd, nullptr);
  // Second call hits cached m_abyNoData.
  EXPECT_EQ(sw_b1->GetRawNoDataValue(), sw_nd);
  EXPECT_EQ(*static_cast<const int16_t*>(sw_nd), -999);

  // Fast-path + slow-path reads on HDF4SwathArray.
  {
    GUInt64 start[2] = {0, 0};
    size_t count[2] = {4, 6};
    std::vector<int16_t> out(24, 0);
    ASSERT_TRUE(ReadArrayZeroLeak(sw_b1.get(), start, count, nullptr, nullptr,
                                  GDALExtendedDataType::Create(GDT_Int16),
                                  out.data()));
    EXPECT_EQ(out[0], 50);
    EXPECT_EQ(out[23], 73);
  }
  {
    GUInt64 start[2] = {3, 0};
    size_t count[2] = {4, 6};
    GInt64 step[2] = {-1, 1};
    std::vector<double> out(24, 0.0);
    ASSERT_TRUE(ReadArrayZeroLeak(sw_b1.get(), start, count, step, nullptr,
                                  GDALExtendedDataType::Create(GDT_Float64),
                                  out.data()));
    EXPECT_DOUBLE_EQ(out[0], 68.0);
    EXPECT_DOUBLE_EQ(out[18], 50.0);
  }

  auto sw_fv = data_fields->OpenMDArray("FillViaFV");
  ASSERT_NE(sw_fv, nullptr);
  const void* sw_fv_nd = sw_fv->GetRawNoDataValue();
  ASSERT_NE(sw_fv_nd, nullptr);
  EXPECT_EQ(*static_cast<const int16_t*>(sw_fv_nd), -777);

  auto sw_plain = data_fields->OpenMDArray("PlainSwath");
  ASSERT_NE(sw_plain, nullptr);
  EXPECT_EQ(sw_plain->GetRawNoDataValue(), nullptr);

  auto sw_cube = data_fields->OpenMDArray("Cube3D");
  ASSERT_NE(sw_cube, nullptr);
  EXPECT_EQ(sw_cube->GetDimensions().size(), 3);

  // 2. Test EOS Grids hierarchy.
  auto grids_group = root->OpenGroup("eos_grids");
  ASSERT_NE(grids_group, nullptr);
  EXPECT_EQ(grids_group->GetGroupNames(),
            (std::vector<std::string>{"UTMGrid", "GeoGrid"}));
  EXPECT_EQ(grids_group->OpenGroup("missing_grid"), nullptr);

  auto utm_grid = grids_group->OpenGroup("UTMGrid");
  ASSERT_NE(utm_grid, nullptr);
  auto utm_dims = utm_grid->GetDimensions();
  // Second call hits cached m_dims.
  EXPECT_EQ(utm_grid->GetDimensions().size(), utm_dims.size());
  ASSERT_EQ(utm_dims.size(), 2);
  EXPECT_EQ(utm_dims[0]->GetName(), "YDim");
  EXPECT_EQ(utm_dims[1]->GetName(), "XDim");

  EXPECT_EQ(utm_grid->GetMDArrayNames(),
            (std::vector<std::string>{"YDim", "XDim"}));
  EXPECT_NE(utm_grid->OpenMDArray("YDim"), nullptr);
  EXPECT_NE(utm_grid->OpenMDArray("XDim"), nullptr);
  EXPECT_EQ(utm_grid->OpenMDArray("missing_var"), nullptr);

  auto gd_attrs = utm_grid->GetAttributes();
  auto gd_cattr = FindAttr(gd_attrs, "GridCharAttr");
  ASSERT_NE(gd_cattr, nullptr);
  EXPECT_STREQ(gd_cattr->ReadAsString(), "GridAttrText");
  auto gd_iattr = FindAttr(gd_attrs, "GridIntAttr");
  ASSERT_NE(gd_iattr, nullptr);
  EXPECT_EQ(gd_iattr->ReadAsInt(), 10);

  EXPECT_EQ(utm_grid->GetGroupNames(),
            (std::vector<std::string>{"Data Fields"}));
  EXPECT_EQ(utm_grid->OpenGroup("missing_sub"), nullptr);

  auto gd_data = utm_grid->OpenGroup("Data Fields");
  ASSERT_NE(gd_data, nullptr);
  EXPECT_EQ(
      gd_data->GetMDArrayNames(),
      (std::vector<std::string>{"Temp", "FillViaFV", "NoFillGrid", "Cube"}));
  EXPECT_EQ(gd_data->OpenMDArray("missing_field"), nullptr);

  auto temp_arr = gd_data->OpenMDArray("Temp");
  ASSERT_NE(temp_arr, nullptr);
  EXPECT_FALSE(temp_arr->IsWritable());
  EXPECT_EQ(temp_arr->GetFilename(), tmp.str());
  EXPECT_NE(temp_arr->GetRootGroup(), nullptr);
  EXPECT_EQ(temp_arr->GetAttributes().size(), 4);

  const void* gd_nd = temp_arr->GetRawNoDataValue();
  ASSERT_NE(gd_nd, nullptr);
  // Second call hits cached m_abyNoData.
  EXPECT_EQ(temp_arr->GetRawNoDataValue(), gd_nd);
  EXPECT_EQ(*static_cast<const int16_t*>(gd_nd), -999);

  bool has_scale = false;
  GDALDataType scale_dt = GDT_Unknown;
  EXPECT_DOUBLE_EQ(temp_arr->GetScale(&has_scale, &scale_dt), 0.25);
  EXPECT_TRUE(has_scale);
  EXPECT_EQ(scale_dt, GDT_Float32);

  bool has_offset = false;
  GDALDataType offset_dt = GDT_Unknown;
  EXPECT_DOUBLE_EQ(temp_arr->GetOffset(&has_offset, &offset_dt), 10.0);
  EXPECT_TRUE(has_offset);
  EXPECT_EQ(offset_dt, GDT_Float64);

  EXPECT_EQ(temp_arr->GetUnit(), "Celsius");

  auto utm_srs = temp_arr->GetSpatialRef();
  ASSERT_NE(utm_srs, nullptr);
  EXPECT_EQ(utm_srs->GetDataAxisToSRSAxisMapping(), (std::vector<int>{2, 1}));

  // Fast-path + slow-path reads on HDF4EOSGridArray.
  {
    GUInt64 start[2] = {0, 0};
    size_t count[2] = {4, 6};
    std::vector<int16_t> out(24, 0);
    ASSERT_TRUE(
        ReadArrayZeroLeak(temp_arr.get(), start, count, nullptr, nullptr,
                          GDALExtendedDataType::Create(GDT_Int16), out.data()));
    EXPECT_EQ(out[0], 200);
    EXPECT_EQ(out[23], 223);
  }
  {
    GUInt64 start[2] = {3, 0};
    size_t count[2] = {4, 6};
    GInt64 step[2] = {-1, 1};
    std::vector<double> out(24, 0.0);
    ASSERT_TRUE(ReadArrayZeroLeak(temp_arr.get(), start, count, step, nullptr,
                                  GDALExtendedDataType::Create(GDT_Float64),
                                  out.data()));
    EXPECT_DOUBLE_EQ(out[0], 218.0);
    EXPECT_DOUBLE_EQ(out[18], 200.0);
  }

  auto gd_fv = gd_data->OpenMDArray("FillViaFV");
  ASSERT_NE(gd_fv, nullptr);
  const void* gd_fv_nd = gd_fv->GetRawNoDataValue();
  ASSERT_NE(gd_fv_nd, nullptr);
  EXPECT_EQ(*static_cast<const int16_t*>(gd_fv_nd), -888);

  auto gd_nofill = gd_data->OpenMDArray("NoFillGrid");
  ASSERT_NE(gd_nofill, nullptr);
  EXPECT_EQ(gd_nofill->GetRawNoDataValue(), nullptr);
  EXPECT_DOUBLE_EQ(gd_nofill->GetScale(&has_scale), 1.0);
  EXPECT_FALSE(has_scale);
  EXPECT_DOUBLE_EQ(gd_nofill->GetOffset(&has_offset), 0.0);
  EXPECT_FALSE(has_offset);
  EXPECT_TRUE(gd_nofill->GetUnit().empty());

  auto gd_cube = gd_data->OpenMDArray("Cube");
  ASSERT_NE(gd_cube, nullptr);
  ASSERT_EQ(gd_cube->GetDimensions().size(), 3);
  EXPECT_EQ(gd_cube->GetDimensions()[0]->GetName(), "BandDim");

  // Geographic Grid (GCTP_GEO -> iProjCode == 0 -> CPLPackedDMSToDec + {2, 1}
  // axis mapping).
  auto geo_grid = grids_group->OpenGroup("GeoGrid");
  ASSERT_NE(geo_grid, nullptr);
  auto geo_dims = geo_grid->GetDimensions();
  ASSERT_EQ(geo_dims.size(), 2);
  auto geo_x_var = geo_dims[1]->GetIndexingVariable();
  ASSERT_NE(geo_x_var, nullptr);
  {
    GUInt64 start[1] = {0};
    size_t count[1] = {1};
    double x0 = 0.0;
    ASSERT_TRUE(geo_x_var->Read(start, count, nullptr, nullptr,
                                GDALExtendedDataType::Create(GDT_Float64),
                                &x0));
    // First pixel center: -100.0 + 0.5 * (10.0 / 6.0).
    EXPECT_NEAR(x0, -100.0 + 5.0 / 6.0, 1e-6);
  }

  auto geo_data = geo_grid->OpenGroup("Data Fields");
  ASSERT_NE(geo_data, nullptr);
  auto geo_arr = geo_data->OpenMDArray("Geo2D");
  ASSERT_NE(geo_arr, nullptr);
  auto geo_srs = geo_arr->GetSpatialRef();
  ASSERT_NE(geo_srs, nullptr);
  EXPECT_EQ(geo_srs->GetDataAxisToSRSAxisMapping(), (std::vector<int>{1, 2}));
}

}  // namespace
}  // namespace autotest2
