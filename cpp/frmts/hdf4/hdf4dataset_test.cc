// Copyright 2017 Google LLC
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
// Tests HDF4 raster driver.
//
// See also:
//   https://gdal.org/drivers/raster/hdf4.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gcore/hdf4_read.py
//   https://github.com/OSGeo/gdal/blob/master/autotest/gcore/hdf4_write.py
//   https://github.com/OSGeo/gdal/blob/master/autotest/gcore/hdf4multidim.py
//   hdf4 is used as a part of netcdf:
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/netcdf.py

#include "third_party/gdal/frmts/hdf4/hdf4dataset.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/match.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/gdal/autotest2/cpp/frmts/hdf4/hdf4_test_util.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/hdf4/hdf4drivercore.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

void Write2DSds(int32_t sd_id, const char* name, int32_t num_type, int32_t rows,
                int32_t cols) {
  int32_t dims[2] = {rows, cols};
  int32_t sds_id = SDcreate(sd_id, name, num_type, 2, dims);
  ASSERT_NE(sds_id, -1);
  int32_t start[2] = {0, 0};
  std::vector<double> buf(rows * cols, 1.0);
  if (num_type == DFNT_INT16) {
    std::vector<int16_t> ibuf(rows * cols, 42);
    ASSERT_EQ(SDwritedata(sds_id, start, nullptr, dims, ibuf.data()), 0);
  } else if (num_type == DFNT_UINT8) {
    std::vector<uint8_t> ubuf(rows * cols, 7);
    ASSERT_EQ(SDwritedata(sds_id, start, nullptr, dims, ubuf.data()), 0);
  } else {
    ASSERT_EQ(SDwritedata(sds_id, start, nullptr, dims, buf.data()), 0);
  }
  ASSERT_EQ(SDendaccess(sds_id), 0);
}

void Write1DSds(int32_t sd_id, const char* name, int32_t len) {
  int32_t dims[1] = {len};
  int32_t sds_id = SDcreate(sd_id, name, DFNT_FLOAT32, 1, dims);
  ASSERT_NE(sds_id, -1);
  int32_t start[1] = {0};
  std::vector<float> buf(len, 2.5f);
  ASSERT_EQ(SDwritedata(sds_id, start, nullptr, dims, buf.data()), 0);
  ASSERT_EQ(SDendaccess(sds_id), 0);
}

TEST(IdentifyTest, DoesNotExist) {
  auto open_info = std::make_unique<GDALOpenInfo>("/does_not_exist",
                                                  GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(FALSE, HDF4DatasetIdentify(open_info.get()));
}

TEST(Hdf4DatasetHelpersTest, SPrintArrayAllTypes) {
  {
    const GByte vals[2] = {10, 250};
    char* s = SPrintArray(GDT_Byte, vals, 2, ", ");
    EXPECT_STREQ(s, "10, 250");
    CPLFree(s);
  }
  {
    const GInt8 vals[2] = {-5, 120};
    char* s = SPrintArray(GDT_Int8, vals, 2, "x");
    EXPECT_STREQ(s, "-5x120");
    CPLFree(s);
  }
  {
    const GUInt16 vals[2] = {100, 65000};
    char* s = SPrintArray(GDT_UInt16, vals, 2, ", ");
    EXPECT_STREQ(s, "100, 65000");
    CPLFree(s);
  }
  {
    const GInt16 vals[2] = {-300, 400};
    char* s = SPrintArray(GDT_Int16, vals, 2, ", ");
    EXPECT_STREQ(s, "-300, 400");
    CPLFree(s);
  }
  {
    const GUInt32 vals[2] = {1, 4000000000U};
    char* s = SPrintArray(GDT_UInt32, vals, 2, ", ");
    EXPECT_STREQ(s, "1, 4000000000");
    CPLFree(s);
  }
  {
    const GInt32 vals[2] = {-100000, 200000};
    char* s = SPrintArray(GDT_Int32, vals, 2, ", ");
    EXPECT_STREQ(s, "-100000, 200000");
    CPLFree(s);
  }
  {
    const float vals[2] = {1.5f, -2.25f};
    char* s = SPrintArray(GDT_Float32, vals, 2, ", ");
    EXPECT_STREQ(s, "1.5, -2.25");
    CPLFree(s);
  }
  {
    const double vals[2] = {3.125, -4.5};
    char* s = SPrintArray(GDT_Float64, vals, 2, ", ");
    EXPECT_STREQ(s, "3.125, -4.5");
    CPLFree(s);
  }
}

TEST(Hdf4DatasetHelpersTest, DataTypeConversions) {
  EXPECT_EQ(HDF4Dataset::GetDataType(DFNT_CHAR8), GDT_Byte);
  EXPECT_EQ(HDF4Dataset::GetDataType(DFNT_UCHAR8), GDT_Byte);
  EXPECT_EQ(HDF4Dataset::GetDataType(DFNT_UINT8), GDT_Byte);
  EXPECT_EQ(HDF4Dataset::GetDataType(DFNT_INT8), GDT_Int8);
  EXPECT_EQ(HDF4Dataset::GetDataType(DFNT_INT16), GDT_Int16);
  EXPECT_EQ(HDF4Dataset::GetDataType(DFNT_UINT16), GDT_UInt16);
  EXPECT_EQ(HDF4Dataset::GetDataType(DFNT_INT32), GDT_Int32);
  EXPECT_EQ(HDF4Dataset::GetDataType(DFNT_UINT32), GDT_UInt32);
  EXPECT_EQ(HDF4Dataset::GetDataType(DFNT_INT64), GDT_Int64);
  EXPECT_EQ(HDF4Dataset::GetDataType(DFNT_UINT64), GDT_UInt64);
  EXPECT_EQ(HDF4Dataset::GetDataType(DFNT_FLOAT32), GDT_Float32);
  EXPECT_EQ(HDF4Dataset::GetDataType(DFNT_FLOAT64), GDT_Float64);
  EXPECT_EQ(HDF4Dataset::GetDataType(9999), GDT_Unknown);

  EXPECT_STREQ(HDF4Dataset::GetDataTypeName(DFNT_CHAR8), "8-bit character");
  EXPECT_STREQ(HDF4Dataset::GetDataTypeName(DFNT_UCHAR8),
               "8-bit unsigned character");
  EXPECT_STREQ(HDF4Dataset::GetDataTypeName(DFNT_INT8), "8-bit integer");
  EXPECT_STREQ(HDF4Dataset::GetDataTypeName(DFNT_UINT8),
               "8-bit unsigned integer");
  EXPECT_STREQ(HDF4Dataset::GetDataTypeName(DFNT_INT16), "16-bit integer");
  EXPECT_STREQ(HDF4Dataset::GetDataTypeName(DFNT_UINT16),
               "16-bit unsigned integer");
  EXPECT_STREQ(HDF4Dataset::GetDataTypeName(DFNT_INT32), "32-bit integer");
  EXPECT_STREQ(HDF4Dataset::GetDataTypeName(DFNT_UINT32),
               "32-bit unsigned integer");
  EXPECT_STREQ(HDF4Dataset::GetDataTypeName(DFNT_INT64), "64-bit integer");
  EXPECT_STREQ(HDF4Dataset::GetDataTypeName(DFNT_UINT64),
               "64-bit unsigned integer");
  EXPECT_STREQ(HDF4Dataset::GetDataTypeName(DFNT_FLOAT32),
               "32-bit floating-point");
  EXPECT_STREQ(HDF4Dataset::GetDataTypeName(DFNT_FLOAT64),
               "64-bit floating-point");

  EXPECT_EQ(HDF4Dataset::GetDataTypeSize(DFNT_CHAR8), 1);
  EXPECT_EQ(HDF4Dataset::GetDataTypeSize(DFNT_UCHAR8), 1);
  EXPECT_EQ(HDF4Dataset::GetDataTypeSize(DFNT_INT8), 1);
  EXPECT_EQ(HDF4Dataset::GetDataTypeSize(DFNT_UINT8), 1);
  EXPECT_EQ(HDF4Dataset::GetDataTypeSize(DFNT_INT16), 2);
  EXPECT_EQ(HDF4Dataset::GetDataTypeSize(DFNT_UINT16), 2);
  EXPECT_EQ(HDF4Dataset::GetDataTypeSize(DFNT_INT32), 4);
  EXPECT_EQ(HDF4Dataset::GetDataTypeSize(DFNT_UINT32), 4);
  EXPECT_EQ(HDF4Dataset::GetDataTypeSize(DFNT_FLOAT32), 4);
  EXPECT_EQ(HDF4Dataset::GetDataTypeSize(DFNT_INT64), 8);
  EXPECT_EQ(HDF4Dataset::GetDataTypeSize(DFNT_UINT64), 8);
  EXPECT_EQ(HDF4Dataset::GetDataTypeSize(DFNT_FLOAT64), 8);

  {
    WithQuietHandler quiet;
    EXPECT_STREQ(HDF4Dataset::GetDataTypeName(9999), "unknown type");
    EXPECT_EQ(HDF4Dataset::GetDataTypeSize(9999), 0);
    int dummy = 0;
    EXPECT_DOUBLE_EQ(HDF4Dataset::AnyTypeToDouble(9999, &dummy), 0.0);
  }

  signed char v_i8 = -7;
  GByte v_u8 = 200;
  GInt16 v_i16 = -1234;
  GUInt16 v_u16 = 54321;
  GInt32 v_i32 = -100000;
  GUInt32 v_u32 = 3000000000U;
  GInt64 v_i64 = -5000000000LL;
  GUInt64 v_u64 = 9000000000ULL;
  float v_f32 = 12.5f;
  double v_f64 = -99.75;

  EXPECT_DOUBLE_EQ(HDF4Dataset::AnyTypeToDouble(DFNT_INT8, &v_i8), -7.0);
  EXPECT_DOUBLE_EQ(HDF4Dataset::AnyTypeToDouble(DFNT_UINT8, &v_u8), 200.0);
  EXPECT_DOUBLE_EQ(HDF4Dataset::AnyTypeToDouble(DFNT_INT16, &v_i16), -1234.0);
  EXPECT_DOUBLE_EQ(HDF4Dataset::AnyTypeToDouble(DFNT_UINT16, &v_u16), 54321.0);
  EXPECT_DOUBLE_EQ(HDF4Dataset::AnyTypeToDouble(DFNT_INT32, &v_i32), -100000.0);
  EXPECT_DOUBLE_EQ(HDF4Dataset::AnyTypeToDouble(DFNT_UINT32, &v_u32),
                   3000000000.0);
  EXPECT_DOUBLE_EQ(HDF4Dataset::AnyTypeToDouble(DFNT_INT64, &v_i64),
                   -5000000000.0);
  EXPECT_DOUBLE_EQ(HDF4Dataset::AnyTypeToDouble(DFNT_UINT64, &v_u64),
                   9000000000.0);
  EXPECT_DOUBLE_EQ(HDF4Dataset::AnyTypeToDouble(DFNT_FLOAT32, &v_f32), 12.5);
  EXPECT_DOUBLE_EQ(HDF4Dataset::AnyTypeToDouble(DFNT_FLOAT64, &v_f64), -99.75);
}

TEST(Hdf4DatasetHelpersTest, TranslateAttributesAndEosOdlParsing) {
  TempFile tmp("hdf4dataset_attrs.hdf");
  const std::string odl_text =
      "GROUP = INVENTORYMETADATA\n"
      "  GROUPTYPE = MASTERGROUP\n"
      "  OBJECT = PARAMETERNAME\n"
      "    CLASS = \"9\"\n"
      "    NUM_VAL = 1\n"
      "    VALUE = \"Spectral IR Surf Bidirect Reflectivity\"\n"
      "  END_OBJECT = PARAMETERNAME\n"
      "  OBJECT = NOCLASSOBJECT_LONG_NAME_OVER_TEN_CHARS\n"
      "    NUM_VAL = 2\n"
      "    VALUE = (\n"
      "      \"first_item\",\n"
      "      \"second_item\"\n"
      "    )\n"
      "  END_OBJECT = NOCLASSOBJECT_LONG_NAME_OVER_TEN_CHARS\n"
      "  OBJECT = ADDITIONALATTRIBUTENAME\n"
      "    CLASS = \"1\"\n"
      "    VALUE = \"QAPERCENTGOODQUALITY\"\n"
      "  END_OBJECT = ADDITIONALATTRIBUTENAME\n"
      "  OBJECT = PARAMETERVALUE\n"
      "    CLASS = \"1\"\n"
      "    VALUE = \"98\"\n"
      "  END_OBJECT = PARAMETERVALUE\n"
      "END_GROUP = INVENTORYMETADATA\n"
      "END\n ";

  {
    int32_t sd_id = SDstart(tmp.path(), DFACC_CREATE);
    ASSERT_NE(sd_id, -1);
    SetCharAttr(sd_id, "coremetadata.0", odl_text);
    SetCharAttr(sd_id, "archivemetadata.0", std::string_view("\0", 1));
    SetCharAttr(sd_id, "productmetadata.0",
                "OBJECT = UNTERMINATED_OBJ\n  VALUE = \"x\"\n");
    SetCharAttr(sd_id, "badpixelinformation", "END\n");
    SetCharAttr(sd_id, "product_summary", "END\n");
    SetCharAttr(sd_id, "dem_specific", "END\n");
    SetCharAttr(sd_id, "bts_specific", "END\n");
    SetCharAttr(sd_id, "etse_specific", "END\n");
    SetCharAttr(sd_id, "dst_specific", "END\n");
    SetCharAttr(sd_id, "acv_specific", "END\n");
    SetCharAttr(sd_id, "act_specific", "END\n");
    SetCharAttr(sd_id, "etst_specific", "END\n");
    SetCharAttr(sd_id, "level_1_carryover", "END\n");
    SetCharAttr(sd_id, "StructMetadata.0", "GROUP=SwathStructure\nEND\n");
    SetCharAttr(sd_id, "RegularCharAttr", "hello");
    const uint8_t uchars[3] = {'a', 'b', 'c'};
    ASSERT_EQ(SDsetattr(sd_id, "RegularUCharAttr", DFNT_UCHAR8, 3, uchars), 0);
    const int32_t nums[2] = {10, 20};
    ASSERT_EQ(SDsetattr(sd_id, "RegularIntAttr", DFNT_INT32, 2, nums), 0);
    Write2DSds(sd_id, "BandA", DFNT_INT16, 4, 4);
    Write2DSds(sd_id, "BandB", DFNT_INT16, 4, 4);
    ASSERT_EQ(SDend(sd_id), 0);
  }

  GDALAllRegister();
  auto ds = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(tmp.path(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  ASSERT_NE(ds, nullptr);

  char** domains = ds->GetMetadataDomainList();
  EXPECT_NE(CSLFindString(domains, "SUBDATASETS"), -1);
  CSLDestroy(domains);

  char** global_md = ds->GetMetadata("");
  EXPECT_STREQ(CSLFetchNameValue(global_md, "PARAMETERNAME.9"),
               "Spectral IR Surf Bidirect Reflectivity");
  EXPECT_STREQ(
      CSLFetchNameValue(global_md, "NOCLASSOBJECT_LONG_NAME_OVER_TEN_CHARS"),
      "first_item,second_item");
  EXPECT_STREQ(CSLFetchNameValue(global_md, "QAPERCENTGOODQUALITY"), "98");
  EXPECT_STREQ(CSLFetchNameValue(global_md, "RegularCharAttr"), "hello");
  EXPECT_STREQ(CSLFetchNameValue(global_md, "RegularUCharAttr"), "a");
  EXPECT_STREQ(CSLFetchNameValue(global_md, "RegularIntAttr"), "10, 20");
  // StructMetadata.0 should be skipped by ReadGlobalAttributes.
  EXPECT_EQ(CSLFetchNameValue(global_md, "StructMetadata.0"), nullptr);

  char** subdatasets = ds->GetMetadata("SUBDATASETS");
  EXPECT_EQ(CSLCount(subdatasets), 4);
  EXPECT_TRUE(
      absl::StrContains(CSLFetchNameValue(subdatasets, "SUBDATASET_1_NAME"),
                        "HDF4_SDS:UNKNOWN:"));
}

TEST(Hdf4DatasetOpenTest, RejectsInvalidAndUpdateOnMultiSubdataset) {
  GDALAllRegister();

  // Non-HDF4 file.
  TempFile non_hdf("not_hdf4.dat");
  {
    FILE* fp = std::fopen(non_hdf.path(), "wb");
    ASSERT_NE(fp, nullptr);
    std::fwrite("NOT_HDF4", 1, 8, fp);
    std::fclose(fp);
  }
  GDALOpenInfo non_hdf_info(non_hdf.path(), GDAL_OF_READONLY);
  EXPECT_EQ(HDF4Dataset::Open(&non_hdf_info), nullptr);

  // Valid 4-byte HDF4 magic header in /vsimem/ (not on POSIX disk so Hopen
  // cleanly fails).
  const char* kVsiMagicPath = "/vsimem/hdf4dataset_vsi_only.hdf";
  auto* buf = static_cast<GByte*>(CPLMalloc(4));
  buf[0] = 0x0e;
  buf[1] = 0x03;
  buf[2] = 0x13;
  buf[3] = 0x01;
  VSIFCloseL(VSIFileFromMemBuffer(kVsiMagicPath, buf, 4, TRUE));
  absl::Cleanup cleanup_vsi = [&] { VSIUnlink(kVsiMagicPath); };
  {
    WithQuietHandler quiet;
    GDALOpenInfo vsi_info(kVsiMagicPath, GDAL_OF_READONLY);
    EXPECT_EQ(HDF4Dataset::Open(&vsi_info), nullptr);
  }

  // Multi-subdataset file opened with GA_Update is rejected.
  TempFile multi("multi_update.hdf");
  {
    int32_t sd_id = SDstart(multi.path(), DFACC_CREATE);
    ASSERT_NE(sd_id, -1);
    Write2DSds(sd_id, "Sds1", DFNT_INT16, 4, 4);
    Write2DSds(sd_id, "Sds2", DFNT_INT16, 4, 4);
    ASSERT_EQ(SDend(sd_id), 0);
  }
  {
    WithQuietHandler quiet;
    EXPECT_EQ(GDALDataset::Open(multi.path(), GDAL_OF_RASTER | GDAL_OF_UPDATE),
              nullptr);
  }
}

TEST(Hdf4DatasetOpenTest, ProductTypesAndSingleSubdatasetAutoOpen) {
  GDALAllRegister();

  // 1. GDAL signature with single SDS -> auto-opens the single subdataset.
  {
    TempFile tmp("gdal_sig.hdf");
    int32_t sd_id = SDstart(tmp.path(), DFACC_CREATE);
    ASSERT_NE(sd_id, -1);
    SetCharAttr(sd_id, "Signature",
                "Created with GDAL (http://www.remotesensing.org/gdal/)");
    Write1DSds(sd_id, "Ignored1D", 4);
    Write2DSds(sd_id, "OnlyBand", DFNT_INT16, 4, 5);
    ASSERT_EQ(SDend(sd_id), 0);

    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(tmp.path(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    EXPECT_STREQ(ds->GetDescription(), tmp.path());
    EXPECT_EQ(ds->GetRasterXSize(), 5);
    EXPECT_EQ(ds->GetRasterYSize(), 4);
  }

  // 2. SeaWiFS Level-1A: filters to l1a_data only -> single subdataset.
  {
    TempFile tmp("seawifs_l1a.hdf");
    int32_t sd_id = SDstart(tmp.path(), DFACC_CREATE);
    ASSERT_NE(sd_id, -1);
    SetCharAttr(sd_id, "Title", "SeaWiFS Level-1A Data");
    Write2DSds(sd_id, "other_field", DFNT_INT16, 4, 4);
    Write2DSds(sd_id, "l1a_data", DFNT_INT16, 4, 6);
    ASSERT_EQ(SDend(sd_id), 0);

    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(tmp.path(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    EXPECT_EQ(ds->GetRasterXSize(), 6);
    EXPECT_EQ(ds->GetRasterYSize(), 4);
  }

  // 3. SeaWiFS Level-2, SeaWiFS Level-3, and Hyperion L1.
  struct ProductCase {
    const char* attr_name;
    const char* attr_val;
    const char* expected_prefix;
  };
  const ProductCase kCases[] = {
      {"Title", "SeaWiFS Level-2 Data", "HDF4_SDS:SEAWIFS_L2:"},
      {"Title", "SeaWiFS Level-3 Standard Mapped Image",
       "HDF4_SDS:SEAWIFS_L3:"},
      {"L1 File Generated By", "HYP version 2.1", "HDF4_SDS:HYPERION_L1:"},
  };
  for (int i = 0; i < 3; ++i) {
    TempFile tmp(absl::StrCat("product_", i, ".hdf"));
    int32_t sd_id = SDstart(tmp.path(), DFACC_CREATE);
    ASSERT_NE(sd_id, -1);
    SetCharAttr(sd_id, kCases[i].attr_name, kCases[i].attr_val);
    Write2DSds(sd_id, "Band1", DFNT_INT16, 3, 3);
    Write2DSds(sd_id, "Band2", DFNT_INT16, 3, 3);
    ASSERT_EQ(SDend(sd_id), 0);

    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(tmp.path(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    const char* sds1 =
        CSLFetchNameValue(ds->GetMetadata("SUBDATASETS"), "SUBDATASET_1_NAME");
    ASSERT_NE(sds1, nullptr);
    EXPECT_TRUE(absl::StartsWith(sds1, kCases[i].expected_prefix)) << sds1;
  }
}

TEST(Hdf4DatasetOpenTest, ListSdsOpenOptionAndGeneralRasters) {
  GDALAllRegister();
  TempFile tmp("gr_and_sds.hdf");
  {
    int32_t sd_id = SDstart(tmp.path(), DFACC_CREATE);
    ASSERT_NE(sd_id, -1);
    Write2DSds(sd_id, "Sds1", DFNT_UINT8, 4, 4);
    Write2DSds(sd_id, "Sds2", DFNT_UINT8, 4, 4);
    ASSERT_EQ(SDend(sd_id), 0);

    int32_t hdf_id = Hopen(tmp.path(), DFACC_RDWR, 0);
    ASSERT_NE(hdf_id, -1);
    int32_t gr_id = GRstart(hdf_id);
    ASSERT_NE(gr_id, -1);
    int32_t dims[2] = {4, 3};
    int32_t ri_id = GRcreate(gr_id, "RasterImg1", 1, DFNT_UINT8,
                             MFGR_INTERLACE_PIXEL, dims);
    ASSERT_NE(ri_id, -1);
    int32_t start[2] = {0, 0};
    int32_t stride[2] = {1, 1};
    std::vector<uint8_t> pixels(12, 99);
    ASSERT_EQ(GRwriteimage(ri_id, start, stride, dims, pixels.data()), 0);
    ASSERT_EQ(GRendaccess(ri_id), 0);
    ASSERT_EQ(GRend(gr_id), 0);
    ASSERT_EQ(Hclose(hdf_id), 0);
  }

  // With LIST_SDS=NO, only the 1 GR raster image is reported -> auto-opens it!
  {
    const char* opts[] = {"LIST_SDS=NO", nullptr};
    auto ds = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        tmp.path(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, opts, nullptr));
    ASSERT_NE(ds, nullptr);
    EXPECT_EQ(ds->GetRasterXSize(), 4);
    EXPECT_EQ(ds->GetRasterYSize(), 3);
  }

  // Default LIST_SDS=AUTO lists both 2 SDSs and 1 GR image (3 subdatasets).
  {
    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(tmp.path(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    char** subdatasets = ds->GetMetadata("SUBDATASETS");
    EXPECT_EQ(CSLCount(subdatasets), 6);
    EXPECT_TRUE(
        absl::StartsWith(CSLFetchNameValue(subdatasets, "SUBDATASET_3_NAME"),
                         "HDF4_GR:UNKNOWN:"));
  }
}

TEST(Hdf4DatasetOpenTest, HdfEosSwathAndGridSubdatasetsAndMultiDim) {
  GDALAllRegister();
  TempFile tmp("eos_swath_grid.hdf");

  // Create SDSs and StructMetadata.0 for 1 Swath (with special characters in
  // names to exercise QuoteIfNeeded, plus a 1D field) and 1 Grid (with a 2D
  // field and a 1D field).
  const std::string struct_meta =
      "GROUP=SwathStructure\n"
      "\tGROUP=SWATH_1\n"
      "\t\tSwathName=\"Swath:With Spaces\"\n"
      "\t\tGROUP=Dimension\n"
      "\t\t\tOBJECT=Dimension_1\n"
      "\t\t\t\tDimensionName=\"YDim\"\n"
      "\t\t\t\tSize=4\n"
      "\t\t\tEND_OBJECT=Dimension_1\n"
      "\t\t\tOBJECT=Dimension_2\n"
      "\t\t\t\tDimensionName=\"XDim\"\n"
      "\t\t\t\tSize=5\n"
      "\t\t\tEND_OBJECT=Dimension_2\n"
      "\t\tEND_GROUP=Dimension\n"
      "\t\tGROUP=DimensionMap\n"
      "\t\tEND_GROUP=DimensionMap\n"
      "\t\tGROUP=IndexDimensionMap\n"
      "\t\tEND_GROUP=IndexDimensionMap\n"
      "\t\tGROUP=GeoField\n"
      "\t\tEND_GROUP=GeoField\n"
      "\t\tGROUP=DataField\n"
      "\t\t\tOBJECT=DataField_1\n"
      "\t\t\t\tDataFieldName=\"Field\\\\With:Special\"\n"
      "\t\t\t\tDataType=DFNT_INT16\n"
      "\t\t\t\tDimList=(\"YDim\",\"XDim\")\n"
      "\t\t\tEND_OBJECT=DataField_1\n"
      "\t\t\tOBJECT=DataField_2\n"
      "\t\t\t\tDataFieldName=\"Swath1D\"\n"
      "\t\t\t\tDataType=DFNT_FLOAT32\n"
      "\t\t\t\tDimList=(\"XDim\")\n"
      "\t\t\tEND_OBJECT=DataField_2\n"
      "\t\tEND_GROUP=DataField\n"
      "\t\tGROUP=MergedFields\n"
      "\t\tEND_GROUP=MergedFields\n"
      "\tEND_GROUP=SWATH_1\n"
      "END_GROUP=SwathStructure\n"
      "GROUP=GridStructure\n"
      "\tGROUP=GRID_1\n"
      "\t\tGridName=\"TestGrid\"\n"
      "\t\tXDim=5\n"
      "\t\tYDim=4\n"
      "\t\tUpperLeftPointMtrs=(0.000000,400.000000)\n"
      "\t\tLowerRightMtrs=(500.000000,0.000000)\n"
      "\t\tProjection=GCTP_GEO\n"
      "\t\tGROUP=Dimension\n"
      "\t\tEND_GROUP=Dimension\n"
      "\t\tGROUP=DataField\n"
      "\t\t\tOBJECT=DataField_1\n"
      "\t\t\t\tDataFieldName=\"GridField2D\"\n"
      "\t\t\t\tDataType=DFNT_INT16\n"
      "\t\t\t\tDimList=(\"YDim\",\"XDim\")\n"
      "\t\t\tEND_OBJECT=DataField_1\n"
      "\t\t\tOBJECT=DataField_2\n"
      "\t\t\t\tDataFieldName=\"GridField1D\"\n"
      "\t\t\t\tDataType=DFNT_FLOAT32\n"
      "\t\t\t\tDimList=(\"XDim\")\n"
      "\t\t\tEND_OBJECT=DataField_2\n"
      "\t\tEND_GROUP=DataField\n"
      "\t\tGROUP=MergedFields\n"
      "\t\tEND_GROUP=MergedFields\n"
      "\tEND_GROUP=GRID_1\n"
      "END_GROUP=GridStructure\n"
      "GROUP=PointStructure\n"
      "END_GROUP=PointStructure\n"
      "END\n";

  {
    int32_t sd_id = SDstart(tmp.path(), DFACC_CREATE);
    ASSERT_NE(sd_id, -1);
    SetCharAttr(sd_id, "HDFEOSVersion", "HDFEOS_V2.12");

    int32_t hdf_id = Hopen(tmp.path(), DFACC_RDWR, 0);
    ASSERT_NE(hdf_id, -1);
    ASSERT_EQ(Vstart(hdf_id), 0);

    std::string padded_meta = struct_meta;
    padded_meta.resize(32000, '\0');
    ASSERT_EQ(SDsetattr(sd_id, "StructMetadata.0", DFNT_CHAR8, 32000,
                        padded_meta.data()),
              0);

    auto make_2d = [&](const char* name) -> int32_t {
      int32_t dims[2] = {4, 5};
      int32_t sds = SDcreate(sd_id, name, DFNT_INT16, 2, dims);
      int32_t start[2] = {0, 0};
      std::vector<int16_t> buf(20, 5);
      SDwritedata(sds, start, nullptr, dims, buf.data());
      int32_t ref = SDidtoref(sds);
      SDendaccess(sds);
      return ref;
    };
    auto make_1d = [&](const char* name) -> int32_t {
      int32_t dims[1] = {5};
      int32_t sds = SDcreate(sd_id, name, DFNT_FLOAT32, 1, dims);
      int32_t start[1] = {0};
      std::vector<float> buf(5, 1.0f);
      SDwritedata(sds, start, nullptr, dims, buf.data());
      int32_t ref = SDidtoref(sds);
      SDendaccess(sds);
      return ref;
    };

    const int32_t sw_field_ref = make_2d("Field\\With:Special");
    const int32_t sw_1d_ref = make_1d("Swath1D");
    const int32_t gd_field_ref = make_2d("GridField2D");
    const int32_t gd_1d_ref = make_1d("GridField1D");

    // Build Swath Vgroups.
    int32_t vg_sw = Vattach(hdf_id, -1, "w");
    Vsetname(vg_sw, "Swath:With Spaces");
    Vsetclass(vg_sw, "SWATH");

    int32_t vg_sw_geo = Vattach(hdf_id, -1, "w");
    Vsetname(vg_sw_geo, "Geolocation Fields");
    Vsetclass(vg_sw_geo, "SWATH Vgroup");

    int32_t vg_sw_data = Vattach(hdf_id, -1, "w");
    Vsetname(vg_sw_data, "Data Fields");
    Vsetclass(vg_sw_data, "SWATH Vgroup");
    Vaddtagref(vg_sw_data, DFTAG_NDG, sw_field_ref);
    Vaddtagref(vg_sw_data, DFTAG_NDG, sw_1d_ref);

    int32_t vg_sw_attr = Vattach(hdf_id, -1, "w");
    Vsetname(vg_sw_attr, "Swath Attributes");
    Vsetclass(vg_sw_attr, "SWATH Vgroup");

    Vinsert(vg_sw, vg_sw_geo);
    Vinsert(vg_sw, vg_sw_data);
    Vinsert(vg_sw, vg_sw_attr);
    Vdetach(vg_sw_attr);
    Vdetach(vg_sw_data);
    Vdetach(vg_sw_geo);
    Vdetach(vg_sw);

    // Build Grid Vgroups.
    int32_t vg_gd = Vattach(hdf_id, -1, "w");
    Vsetname(vg_gd, "TestGrid");
    Vsetclass(vg_gd, "GRID");

    int32_t vg_gd_data = Vattach(hdf_id, -1, "w");
    Vsetname(vg_gd_data, "Data Fields");
    Vsetclass(vg_gd_data, "GRID Vgroup");
    Vaddtagref(vg_gd_data, DFTAG_NDG, gd_field_ref);
    Vaddtagref(vg_gd_data, DFTAG_NDG, gd_1d_ref);

    int32_t vg_gd_attr = Vattach(hdf_id, -1, "w");
    Vsetname(vg_gd_attr, "Grid Attributes");
    Vsetclass(vg_gd_attr, "GRID Vgroup");

    Vinsert(vg_gd, vg_gd_data);
    Vinsert(vg_gd, vg_gd_attr);
    Vdetach(vg_gd_attr);
    Vdetach(vg_gd_data);
    Vdetach(vg_gd);

    ASSERT_EQ(SDend(sd_id), 0);
    ASSERT_EQ(Vend(hdf_id), 0);
    ASSERT_EQ(Hclose(hdf_id), 0);
  }

  // Default open: enumerates 1 Swath 2D field + 1 Grid 2D field (skips 1D).
  {
    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(tmp.path(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    char** subdatasets = ds->GetMetadata("SUBDATASETS");
    ASSERT_EQ(CSLCount(subdatasets), 4);
    const char* sds1 = CSLFetchNameValue(subdatasets, "SUBDATASET_1_NAME");
    const char* sds2 = CSLFetchNameValue(subdatasets, "SUBDATASET_2_NAME");
    ASSERT_NE(sds1, nullptr);
    ASSERT_NE(sds2, nullptr);
    EXPECT_TRUE(absl::StartsWith(sds1, "HDF4_EOS:EOS_SWATH:"));
    EXPECT_TRUE(absl::StartsWith(sds2, "HDF4_EOS:EOS_GRID:"));
  }

  // With LIST_SDS=YES: also includes the 2D SDSs!
  {
    const char* opts[] = {"LIST_SDS=YES", nullptr};
    auto ds = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        tmp.path(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, opts, nullptr));
    ASSERT_NE(ds, nullptr);
    char** subdatasets = ds->GetMetadata("SUBDATASETS");
    EXPECT_EQ(CSLCount(subdatasets), 8);
  }

  // Open as Multidim Raster.
  {
    auto ds = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        tmp.path(), GDAL_OF_MULTIDIM_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    EXPECT_NE(ds->GetRootGroup(), nullptr);
  }
}

TEST(Hdf4DatasetOpenTest, MalformedGridCommaNameReturnsNull) {
  GDALAllRegister();
  WithQuietHandler quiet;

  TempFile tmp_gd("comma_grid.hdf");
  int32_t sd_id = SDstart(tmp_gd.path(), DFACC_CREATE);
  ASSERT_NE(sd_id, -1);
  SetCharAttr(sd_id, "HDFEOSVersion", "HDFEOS_V2.12");
  int32_t hdf_id = Hopen(tmp_gd.path(), DFACC_RDWR, 0);
  ASSERT_NE(hdf_id, -1);
  ASSERT_EQ(Vstart(hdf_id), 0);
  int32_t vg = Vattach(hdf_id, -1, "w");
  Vsetname(vg, ",");
  Vsetclass(vg, "GRID");
  Vdetach(vg);
  ASSERT_EQ(SDend(sd_id), 0);
  ASSERT_EQ(Vend(hdf_id), 0);
  ASSERT_EQ(Hclose(hdf_id), 0);

  EXPECT_EQ(GDALDataset::Open(tmp_gd.path(), GDAL_OF_RASTER | GDAL_OF_READONLY),
            nullptr);
}

TEST(Hdf4DatasetOpenTest, RegisterAndUnloadDriver) {
  GDALRegister_HDF4();
  GDALDriver* driver =
      GetGDALDriverManager()->GetDriverByName(HDF4_DRIVER_NAME);
  ASSERT_NE(driver, nullptr);
  EXPECT_STREQ(driver->GetMetadataItem("HDF4_HAS_MAXOPENFILES"), "YES");
  if (driver->pfnUnloadDriver != nullptr) {
    driver->pfnUnloadDriver(driver);
  }
}

}  // namespace
}  // namespace autotest2
