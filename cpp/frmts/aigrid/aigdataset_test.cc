// Tests for aigdataset:
//   https://github.com/OSGeo/gdal/blob/master/frmts/aigrid/aigdataset.cpp
//
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

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/debugging/leak_check.h"
#include "third_party/absl/memory/memory.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/gdal/frmts/aigrid/aigrid.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

void WriteBE16(std::vector<GByte>& buf, size_t offset, uint16_t val) {
  buf[offset + 0] = static_cast<GByte>((val >> 8) & 0xFF);
  buf[offset + 1] = static_cast<GByte>(val & 0xFF);
}

void WriteBE32(std::vector<GByte>& buf, size_t offset, uint32_t val) {
  buf[offset + 0] = static_cast<GByte>((val >> 24) & 0xFF);
  buf[offset + 1] = static_cast<GByte>((val >> 16) & 0xFF);
  buf[offset + 2] = static_cast<GByte>((val >> 8) & 0xFF);
  buf[offset + 3] = static_cast<GByte>(val & 0xFF);
}

void WriteBEDouble(std::vector<GByte>& buf, size_t offset, double val) {
  uint64_t bits = 0;
  std::memcpy(&bits, &val, sizeof(bits));
  buf[offset + 0] = static_cast<GByte>((bits >> 56) & 0xFF);
  buf[offset + 1] = static_cast<GByte>((bits >> 48) & 0xFF);
  buf[offset + 2] = static_cast<GByte>((bits >> 40) & 0xFF);
  buf[offset + 3] = static_cast<GByte>((bits >> 32) & 0xFF);
  buf[offset + 4] = static_cast<GByte>((bits >> 24) & 0xFF);
  buf[offset + 5] = static_cast<GByte>((bits >> 16) & 0xFF);
  buf[offset + 6] = static_cast<GByte>((bits >> 8) & 0xFF);
  buf[offset + 7] = static_cast<GByte>(bits & 0xFF);
}

void WriteMemFile(const std::string& path, const std::vector<GByte>& data) {
  const size_t alloc_size = data.empty() ? 1 : data.size();
  GByte* buf = static_cast<GByte*>(CPLMalloc(alloc_size));
  if (!data.empty()) {
    std::memcpy(buf, data.data(), data.size());
  }
  VSILFILE* fp = VSIFileFromMemBuffer(path.c_str(), buf, data.size(), TRUE);
  ASSERT_NE(nullptr, fp) << path;
  ASSERT_EQ(0, VSIFCloseL(fp));
}

void WriteMemText(const std::string& path, const std::string& text) {
  std::vector<GByte> bytes(text.begin(), text.end());
  WriteMemFile(path, bytes);
}

class VsiDirGuard {
 public:
  explicit VsiDirGuard(std::string path) : path_(std::move(path)) {
    VSIMkdir(path_.c_str(), 0777);
  }
  ~VsiDirGuard() { CPLUnlinkTree(path_.c_str()); }
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

struct GridSpec {
  int32_t cell_type = AIG_CELLTYPE_INT;
  int32_t compressed_hdr_flag = 1;  // 1 -> uncompressed, 0 -> compressed
  double cell_size_x = 1.0;
  double cell_size_y = 1.0;
  double llx = 0.0;
  double lly = 0.0;
  double urx = 2.0;
  double ury = 2.0;
  int32_t blocks_per_row = 1;
  int32_t blocks_per_col = 1;
  int32_t block_x_size = 2;
  int32_t block_y_size = 2;
  double min_val = 0.0;
  double max_val = 10.0;
  double mean_val = 5.0;
  double stddev_val = 2.0;
  bool uppercase_files = false;
  std::string tile_basename = "w001001";
};

void WriteGridMetadata(const std::string& dir, const GridSpec& spec) {
  std::vector<GByte> hdr(308, 0);
  WriteBE32(hdr, 16, static_cast<uint32_t>(spec.cell_type));
  WriteBE32(hdr, 20, static_cast<uint32_t>(spec.compressed_hdr_flag));
  WriteBEDouble(hdr, 256, spec.cell_size_x);
  WriteBEDouble(hdr, 264, spec.cell_size_y);
  WriteBE32(hdr, 288, static_cast<uint32_t>(spec.blocks_per_row));
  WriteBE32(hdr, 292, static_cast<uint32_t>(spec.blocks_per_col));
  WriteBE32(hdr, 296, static_cast<uint32_t>(spec.block_x_size));
  WriteBE32(hdr, 304, static_cast<uint32_t>(spec.block_y_size));

  std::vector<GByte> bnd(32, 0);
  WriteBEDouble(bnd, 0, spec.llx);
  WriteBEDouble(bnd, 8, spec.lly);
  WriteBEDouble(bnd, 16, spec.urx);
  WriteBEDouble(bnd, 24, spec.ury);

  std::vector<GByte> sta(32, 0);
  WriteBEDouble(sta, 0, spec.min_val);
  WriteBEDouble(sta, 8, spec.max_val);
  WriteBEDouble(sta, 16, spec.mean_val);
  WriteBEDouble(sta, 24, spec.stddev_val);

  if (spec.uppercase_files) {
    WriteMemFile(dir + "/HDR.ADF", hdr);
    WriteMemFile(dir + "/DBLBND.ADF", bnd);
    WriteMemFile(dir + "/STA.ADF", sta);
  } else {
    WriteMemFile(dir + "/hdr.adf", hdr);
    WriteMemFile(dir + "/dblbnd.adf", bnd);
    WriteMemFile(dir + "/sta.adf", sta);
  }
}

void WriteUncompressedIntBlockTile(const std::string& dir,
                                   const std::string& basename,
                                   const std::vector<GInt32>& values) {
  const uint32_t payload_bytes = static_cast<uint32_t>(values.size() * 4);
  std::vector<GByte> tile(100 + 2 + payload_bytes, 0);
  WriteBE16(tile, 100, static_cast<uint16_t>(payload_bytes / 2));
  for (size_t i = 0; i < values.size(); ++i) {
    WriteBE32(tile, 102 + i * 4, static_cast<uint32_t>(values[i]));
  }
  WriteMemFile(absl::StrFormat("%s/%s.adf", dir, basename), tile);

  std::vector<GByte> idx(108, 0);
  idx[2] = 0x27;
  idx[3] = 0x0A;
  idx[4] = 0xFF;
  idx[5] = 0xFF;
  WriteBE32(idx, 24, 108 / 2);
  WriteBE32(idx, 100, 100 / 2);
  WriteBE32(idx, 104, payload_bytes / 2);
  WriteMemFile(absl::StrFormat("%s/%sx.adf", dir, basename), idx);
}

void WriteFloatBlockTile(const std::string& dir, const std::string& basename,
                         const std::vector<float>& values) {
  const uint32_t payload_bytes = static_cast<uint32_t>(values.size() * 4);
  std::vector<GByte> tile(100 + 2 + payload_bytes, 0);
  WriteBE16(tile, 100, static_cast<uint16_t>(payload_bytes / 2));
  for (size_t i = 0; i < values.size(); ++i) {
    uint32_t bits = 0;
    std::memcpy(&bits, &values[i], sizeof(bits));
    WriteBE32(tile, 102 + i * 4, bits);
  }
  WriteMemFile(absl::StrFormat("%s/%s.adf", dir, basename), tile);

  std::vector<GByte> idx(108, 0);
  idx[2] = 0x27;
  idx[3] = 0x0A;
  idx[4] = 0xFF;
  idx[5] = 0xFF;
  WriteBE32(idx, 24, 108 / 2);
  WriteBE32(idx, 100, 100 / 2);
  WriteBE32(idx, 104, payload_bytes / 2);
  WriteMemFile(absl::StrFormat("%s/%sx.adf", dir, basename), idx);
}

class AigdatasetTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_AIGrid(); }
};

TEST_F(AigdatasetTest, OpenRejectionPathsAndUpdateNotSupported) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };

  // 1. Non-directory file not ending in .adf -> rejected.
  WriteMemText("/vsimem/aigds_plain.txt", "hello");
  absl::Cleanup unlink_txt = [] { VSIUnlink("/vsimem/aigds_plain.txt"); };
  EXPECT_EQ(nullptr, GDALOpenEx("/vsimem/aigds_plain.txt", GDAL_OF_RASTER,
                                nullptr, nullptr, nullptr));

  // 2. Bare .adf filename without directory -> resolves to ".", rejected.
  EXPECT_EQ(nullptr, GDALOpenEx("nonexistent_aigds_probe.adf", GDAL_OF_RASTER,
                                nullptr, nullptr, nullptr));

  // 3. Empty directory without hdr.adf / HDR.ADF -> rejected.
  VsiDirGuard empty_dir("/vsimem/aigds_empty");
  EXPECT_EQ(nullptr, GDALOpenEx(empty_dir.path().c_str(), GDAL_OF_RASTER,
                                nullptr, nullptr, nullptr));

  // 4. Directory with hdr.adf but no tile file (w0010xx.adf / z0010xx.adf).
  VsiDirGuard no_tile_dir("/vsimem/aigds_notile");
  WriteGridMetadata(no_tile_dir.path(), {});
  // Add an unrelated 11-char .adf file to test filename filter branches.
  WriteMemFile(no_tile_dir.path() + "/x001001.adf", {0});
  WriteMemFile(no_tile_dir.path() + "/w999001.adf", {0});
  WriteMemFile(no_tile_dir.path() + "/w001001.txt", {0});
  EXPECT_EQ(nullptr, GDALOpenEx(no_tile_dir.path().c_str(), GDAL_OF_RASTER,
                                nullptr, nullptr, nullptr));

  // 5. Corrupt hdr.adf with tile present -> AIGOpen fails -> returns nullptr.
  VsiDirGuard bad_hdr_dir("/vsimem/aigds_badhdr");
  WriteMemFile(bad_hdr_dir.path() + "/hdr.adf", std::vector<GByte>(50, 0));
  WriteMemFile(bad_hdr_dir.path() + "/w001001.adf", {0});
  EXPECT_EQ(nullptr, GDALOpenEx(bad_hdr_dir.path().c_str(), GDAL_OF_RASTER,
                                nullptr, nullptr, nullptr));

  // 6. Valid coverage opened with GA_Update ->
  // ReportUpdateNotSupportedByDriver.
  VsiDirGuard valid_dir("/vsimem/aigds_ro");
  WriteGridMetadata(valid_dir.path(), {});
  WriteUncompressedIntBlockTile(valid_dir.path(), "w001001", {1, 2, 3, 4});
  CPLErrorReset();
  EXPECT_EQ(nullptr, GDALOpenEx(valid_dir.path().c_str(),
                                GDAL_OF_RASTER | GDAL_OF_UPDATE, nullptr,
                                nullptr, nullptr));
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("update"));
}

TEST_F(AigdatasetTest, DataTypesByteInt16Int32Float32AndNodata) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };

  // 1. GDT_Byte (min=0, max=254) opened via uppercase HDR.ADF and z001013.adf.
  {
    VsiDirGuard dir("/vsimem/aigds_byte");
    GridSpec spec;
    spec.min_val = 0.0;
    spec.max_val = 254.0;
    spec.uppercase_files = true;
    WriteGridMetadata(dir.path(), spec);
    // Include z001013.adf so Open() detects a z0010xx.adf tile file.
    WriteMemFile(dir.path() + "/z001013.adf", {0});
    WriteUncompressedIntBlockTile(dir.path(), "w001001",
                                  {0, 127, 254, ESRI_GRID_NO_DATA});

    auto ds = absl::WrapUnique(static_cast<GDALDataset*>(GDALOpenEx(
        (dir.path() + "/HDR.ADF").c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
        nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(nullptr, ds->GetSpatialRef());

    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    EXPECT_EQ(GDT_Byte, band->GetRasterDataType());
    EXPECT_EQ(GCI_Undefined, band->GetColorInterpretation());
    EXPECT_EQ(nullptr, band->GetColorTable());

    int ok = 0;
    EXPECT_DOUBLE_EQ(0.0, band->GetMinimum(&ok));
    EXPECT_TRUE(ok);
    EXPECT_DOUBLE_EQ(254.0, band->GetMaximum(&ok));
    EXPECT_TRUE(ok);
    EXPECT_DOUBLE_EQ(255.0, band->GetNoDataValue(&ok));
    EXPECT_TRUE(ok);

    GByte buf[4] = {};
    ASSERT_EQ(CE_None,
              band->RasterIO(GF_Read, 0, 0, 2, 2, buf, 2, 2, GDT_Byte, 0, 0));
    EXPECT_THAT(buf, ElementsAre(0, 127, 254, 255));

    char** file_list = ds->GetFileList();
    EXPECT_GE(CSLCount(file_list), 5);
    CSLDestroy(file_list);
  }

  // 2. GDT_Int16 (min=-100, max=1000).
  {
    VsiDirGuard dir("/vsimem/aigds_int16");
    GridSpec spec;
    spec.min_val = -100.0;
    spec.max_val = 1000.0;
    WriteGridMetadata(dir.path(), spec);
    WriteUncompressedIntBlockTile(dir.path(), "w001001",
                                  {-100, 0, 1000, ESRI_GRID_NO_DATA});

    auto ds = absl::WrapUnique(static_cast<GDALDataset*>(
        GDALOpenEx(dir.path().c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    EXPECT_EQ(GDT_Int16, band->GetRasterDataType());
    int ok = 0;
    EXPECT_DOUBLE_EQ(-32768.0, band->GetNoDataValue(&ok));
    EXPECT_TRUE(ok);

    GInt16 buf[4] = {};
    ASSERT_EQ(CE_None,
              band->RasterIO(GF_Read, 0, 0, 2, 2, buf, 2, 2, GDT_Int16, 0, 0));
    EXPECT_THAT(buf, ElementsAre(-100, 0, 1000, -32768));
  }

  // 3. GDT_Int32 (min=-100000, max=100000) + IReadBlock failure path.
  {
    VsiDirGuard dir("/vsimem/aigds_int32");
    GridSpec spec;
    spec.min_val = -100000.0;
    spec.max_val = 100000.0;
    WriteGridMetadata(dir.path(), spec);
    WriteUncompressedIntBlockTile(dir.path(), "w001001",
                                  {-100000, 42, 100000, ESRI_GRID_NO_DATA});

    auto ds = absl::WrapUnique(static_cast<GDALDataset*>(
        GDALOpenEx(dir.path().c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    EXPECT_EQ(GDT_Int32, band->GetRasterDataType());
    int ok = 0;
    EXPECT_DOUBLE_EQ(static_cast<double>(ESRI_GRID_NO_DATA),
                     band->GetNoDataValue(&ok));

    GInt32 buf[4] = {};
    ASSERT_EQ(CE_None,
              band->RasterIO(GF_Read, 0, 0, 2, 2, buf, 2, 2, GDT_Int32, 0, 0));
    EXPECT_THAT(buf, ElementsAre(-100000, 42, 100000, ESRI_GRID_NO_DATA));

    // Delete w001001x.adf on a fresh open to exercise IReadBlock failure.
    VSIUnlink((dir.path() + "/w001001x.adf").c_str());
    auto ds2 = absl::WrapUnique(static_cast<GDALDataset*>(
        GDALOpenEx(dir.path().c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds2);
    EXPECT_EQ(CE_Failure, ds2->GetRasterBand(1)->ReadBlock(0, 0, buf));
  }

  // 4. GDT_Float32.
  {
    VsiDirGuard dir("/vsimem/aigds_flt32");
    GridSpec spec;
    spec.cell_type = AIG_CELLTYPE_FLOAT;
    spec.min_val = -2.5;
    spec.max_val = 7.5;
    WriteGridMetadata(dir.path(), spec);
    WriteFloatBlockTile(dir.path(), "w001001", {-2.5f, 0.0f, 3.25f, 7.5f});

    auto ds = absl::WrapUnique(static_cast<GDALDataset*>(
        GDALOpenEx(dir.path().c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    EXPECT_EQ(GDT_Float32, band->GetRasterDataType());
    int ok = 0;
    EXPECT_DOUBLE_EQ(static_cast<double>(ESRI_GRID_FLOAT_NO_DATA),
                     band->GetNoDataValue(&ok));

    float buf[4] = {};
    ASSERT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 2, 2, buf, 2, 2,
                                      GDT_Float32, 0, 0));
    EXPECT_FLOAT_EQ(-2.5f, buf[0]);
    EXPECT_FLOAT_EQ(0.0f, buf[1]);
    EXPECT_FLOAT_EQ(3.25f, buf[2]);
    EXPECT_FLOAT_EQ(7.5f, buf[3]);
  }
}

TEST_F(AigdatasetTest, ColorTableInDirAndParentDirAndCorruption) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };

  // 1. Color table inside coverage directory with comment + corrupt trailing
  // line.
  {
    VsiDirGuard dir("/vsimem/aigds_clr_in");
    WriteGridMetadata(dir.path(), {});
    WriteUncompressedIntBlockTile(dir.path(), "w001001", {0, 1, 2, 0});
    WriteMemText(dir.path() + "/custom.clr",
                 "# comment line\n"
                 "0 255 0 0\n"
                 "1 0 128 255\n"
                 "2 300 0 0\n"
                 "3 10 20 30\n");

    CPLErrorReset();
    auto ds = absl::WrapUnique(static_cast<GDALDataset*>(
        GDALOpenEx(dir.path().c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Color table entry"));
    GDALRasterBand* band = ds->GetRasterBand(1);
    EXPECT_EQ(GCI_PaletteIndex, band->GetColorInterpretation());
    GDALColorTable* ct = band->GetColorTable();
    ASSERT_NE(nullptr, ct);
    EXPECT_EQ(2, ct->GetColorEntryCount());
    EXPECT_EQ(255, ct->GetColorEntry(0)->c1);
    EXPECT_EQ(128, ct->GetColorEntry(1)->c2);
  }

  // 2. Lowercase <cover>.clr in parent directory.
  // Note: /vsimem/ NormalizePath does not collapse "..", so populate the
  // literal "%s/../%s.clr" key probed by AIGDataset::Open.
  {
    VsiDirGuard dir("/vsimem/aigds_pclr_lo");
    WriteGridMetadata(dir.path(), {});
    WriteUncompressedIntBlockTile(dir.path(), "w001001", {0, 1, 0, 1});
    WriteMemText(dir.path() + "/../aigds_pclr_lo.clr", "0 10 20 30\n");
    absl::Cleanup unlink_clr = [&] {
      CPLUnlinkTree((dir.path() + "/..").c_str());
    };

    auto ds = absl::WrapUnique(static_cast<GDALDataset*>(
        GDALOpenEx(dir.path().c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(GCI_PaletteIndex, ds->GetRasterBand(1)->GetColorInterpretation());
    ASSERT_NE(nullptr, ds->GetRasterBand(1)->GetColorTable());
    EXPECT_EQ(10, ds->GetRasterBand(1)->GetColorTable()->GetColorEntry(0)->c1);

    char** fl = ds->GetFileList();
    EXPECT_GE(CSLCount(fl), 4);
    CSLDestroy(fl);
  }

  // 3. Uppercase <cover>.CLR in parent directory.
  {
    VsiDirGuard dir("/vsimem/aigds_pclr_hi");
    WriteGridMetadata(dir.path(), {});
    WriteUncompressedIntBlockTile(dir.path(), "w001001", {0, 1, 0, 1});
    WriteMemText(dir.path() + "/../aigds_pclr_hi.CLR", "0 40 50 60\n");
    absl::Cleanup unlink_clr = [&] {
      CPLUnlinkTree((dir.path() + "/..").c_str());
    };

    auto ds = absl::WrapUnique(static_cast<GDALDataset*>(
        GDALOpenEx(dir.path().c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(GCI_PaletteIndex, ds->GetRasterBand(1)->GetColorInterpretation());
    ASSERT_NE(nullptr, ds->GetRasterBand(1)->GetColorTable());
    EXPECT_EQ(40, ds->GetRasterBand(1)->GetColorTable()->GetColorEntry(0)->c1);
  }
}

TEST_F(AigdatasetTest, ProjectionProjectedAndGeographicArcSeconds) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };

  // 1. Projected UTM Zone 11 WGS84 in METERS.
  {
    VsiDirGuard dir("/vsimem/aigds_prj_utm");
    GridSpec spec;
    spec.llx = 500000.0;
    spec.lly = 4000000.0;
    spec.urx = 500020.0;
    spec.ury = 4000020.0;
    spec.cell_size_x = 10.0;
    spec.cell_size_y = 10.0;
    WriteGridMetadata(dir.path(), spec);
    WriteUncompressedIntBlockTile(dir.path(), "w001001", {1, 2, 3, 4});
    WriteMemText(dir.path() + "/prj.adf",
                 "Projection    UTM\n"
                 "Zone          11\n"
                 "Datum         WGS84\n"
                 "Units         METERS\n");

    auto ds = absl::WrapUnique(static_cast<GDALDataset*>(
        GDALOpenEx(dir.path().c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    const OGRSpatialReference* srs = ds->GetSpatialRef();
    ASSERT_NE(nullptr, srs);
    EXPECT_TRUE(srs->IsProjected());

    GDALGeoTransform gt;
    ASSERT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(500000.0, gt[0]);
    EXPECT_DOUBLE_EQ(10.0, gt[1]);
    EXPECT_DOUBLE_EQ(4000020.0, gt[3]);
    EXPECT_DOUBLE_EQ(-10.0, gt[5]);
  }

  // 2. Geographic in arc-seconds (Units DS) -> coordinates divided by 3600.0.
  {
    VsiDirGuard dir("/vsimem/aigds_prj_ds");
    GridSpec spec;
    spec.llx = 36000.0;  // 10 degrees in arc-seconds
    spec.lly = 72000.0;
    spec.urx = 43200.0;
    spec.ury = 79200.0;  // 22 degrees in arc-seconds
    spec.cell_size_x = 3600.0;
    spec.cell_size_y = 3600.0;
    WriteGridMetadata(dir.path(), spec);
    WriteUncompressedIntBlockTile(dir.path(), "w001001", {1, 2, 3, 4});
    WriteMemText(dir.path() + "/prj.adf",
                 "Projection    GEOGRAPHIC\n"
                 "Datum         WGS84\n"
                 "Units         DS\n");

    auto ds = absl::WrapUnique(static_cast<GDALDataset*>(
        GDALOpenEx(dir.path().c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    const OGRSpatialReference* srs = ds->GetSpatialRef();
    ASSERT_NE(nullptr, srs);
    EXPECT_TRUE(srs->IsGeographic());

    GDALGeoTransform gt;
    ASSERT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(10.0, gt[0]);
    EXPECT_DOUBLE_EQ(1.0, gt[1]);
    EXPECT_DOUBLE_EQ(22.0, gt[3]);
    EXPECT_DOUBLE_EQ(-1.0, gt[5]);
  }

  // 3. Geographic with single-token Units line (covers OSR_GDS fallback).
  {
    VsiDirGuard dir("/vsimem/aigds_prj_notok");
    WriteGridMetadata(dir.path(), {});
    WriteUncompressedIntBlockTile(dir.path(), "w001001", {1, 2, 3, 4});
    WriteMemText(dir.path() + "/prj.adf",
                 "Projection    GEOGRAPHIC\n"
                 "Datum         WGS84\n"
                 "Units\n");

    auto ds = absl::WrapUnique(static_cast<GDALDataset*>(
        GDALOpenEx(dir.path().c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    ASSERT_NE(nullptr, ds->GetSpatialRef());
  }

  // 4. Geographic without Units line (covers OSR_GDS missing-field return).
  {
    VsiDirGuard dir("/vsimem/aigds_prj_nounits");
    WriteGridMetadata(dir.path(), {});
    WriteUncompressedIntBlockTile(dir.path(), "w001001", {1, 2, 3, 4});
    WriteMemText(dir.path() + "/prj.adf",
                 "Projection    GEOGRAPHIC\n"
                 "Datum         WGS84\n");

    auto ds = absl::WrapUnique(static_cast<GDALDataset*>(
        GDALOpenEx(dir.path().c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    ASSERT_NE(nullptr, ds->GetSpatialRef());
  }
}

void WriteNitField(std::vector<GByte>& nit, size_t offset,
                   const std::string& name, int16_t size, int16_t field_offset,
                   int16_t type1, int16_t index) {
  std::memset(nit.data() + offset, ' ', 16);
  std::memcpy(nit.data() + offset, name.data(),
              std::min<size_t>(16, name.size()));
  WriteBE16(nit, offset + 16, static_cast<uint16_t>(size));
  WriteBE16(nit, offset + 18, 0xFFFF);
  WriteBE16(nit, offset + 20, static_cast<uint16_t>(field_offset));
  WriteBE16(nit, offset + 22, 4);
  WriteBE16(nit, offset + 24, 0xFFFF);
  WriteBE16(nit, offset + 26, static_cast<uint16_t>(size));
  WriteBE16(nit, offset + 28, 0);
  WriteBE16(nit, offset + 30, static_cast<uint16_t>(type1));
  WriteBE16(nit, offset + 32, 0);
  for (int i = 0; i < 4; ++i) {
    WriteBE16(nit, offset + 34 + i * 2, 0xFFFF);
  }
  std::memset(nit.data() + offset + 42, ' ', 16);
  WriteBE16(nit, offset + 114, static_cast<uint16_t>(index));
}

TEST_F(AigdatasetTest, RasterAttributeTableVatAndErrorHandling) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };

  // 1. Coverage without ../info -> GetDefaultRAT returns nullptr (twice).
  {
    VsiDirGuard dir("/vsimem/aigds_norat");
    WriteGridMetadata(dir.path(), {});
    WriteUncompressedIntBlockTile(dir.path(), "w001001", {1, 2, 3, 4});
    auto ds = absl::WrapUnique(static_cast<GDALDataset*>(
        GDALOpenEx(dir.path().c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(nullptr, ds->GetRasterBand(1)->GetDefaultRAT());
    EXPECT_EQ(nullptr, ds->GetRasterBand(1)->GetDefaultRAT());
  }

  // 2. Coverage with ../info and full VAT table covering BININT (4 & 2),
  // BINFLOAT (4 & 8), CHAR, VALUE (GFU_MinMax), COUNT (GFU_PixelCount).
  // Note: /vsimem/ NormalizePath keeps ".." literally, so create the
  // literal "%s/../info" directory key probed by AIGDataset::ReadRAT.
  {
    VsiDirGuard root("/vsimem/aigds_rat_root");
    VsiDirGuard cover_dir("/vsimem/aigds_rat_root/mycov");
    VsiDirGuard dotdot_dir("/vsimem/aigds_rat_root/mycov/..");
    VsiDirGuard info_dir("/vsimem/aigds_rat_root/mycov/../info");

    WriteGridMetadata(cover_dir.path(), {});
    WriteUncompressedIntBlockTile(cover_dir.path(), "w001001", {1, 2, 1, 2});

    // arc.dir: 1 entry (380 bytes) for MYCOV.VAT -> ARC0000, 6 fields,
    // rec_size = 4 + 4 + 2 + 4 + 8 + 8 = 30, 2 records.
    std::vector<GByte> arc_dir(380, 0);
    std::memset(arc_dir.data(), ' ', 40);
    const std::string tbl_name = "MYCOV.VAT";
    std::memcpy(arc_dir.data(), tbl_name.data(), tbl_name.size());
    const std::string info_file = "ARC0000";
    std::memcpy(arc_dir.data() + 32, info_file.data(), info_file.size());
    WriteBE16(arc_dir, 40, 6);
    WriteBE16(arc_dir, 42, 30);
    WriteBE16(arc_dir, 62, 0);
    WriteBE32(arc_dir, 64, 2);
    arc_dir[78] = ' ';
    arc_dir[79] = ' ';
    WriteMemFile(info_dir.path() + "/arc.dir", arc_dir);

    // arc0000.nit: 6 field definitions * 144 bytes = 864 bytes.
    std::vector<GByte> nit(6 * 144, 0);
    WriteNitField(nit, 0 * 144, "VALUE", 4, 1, 5, 1);     // BININT 4
    WriteNitField(nit, 1 * 144, "COUNT", 4, 5, 5, 2);     // BININT 4
    WriteNitField(nit, 2 * 144, "SHORT_ID", 2, 9, 5, 3);  // BININT 2
    WriteNitField(nit, 3 * 144, "FLT_VAL", 4, 11, 6, 4);  // BINFLOAT 4
    WriteNitField(nit, 4 * 144, "DBL_VAL", 8, 15, 6, 5);  // BINFLOAT 8
    WriteNitField(nit, 5 * 144, "LABEL", 8, 23, 2, 6);    // CHAR 8
    WriteMemFile(info_dir.path() + "/arc0000.nit", nit);

    // arc0000.dat: 2 records * 30 bytes = 60 bytes.
    std::vector<GByte> dat(60, 0);
    // Record 0: VALUE=1, COUNT=2, SHORT_ID=11, FLT_VAL=1.5f, DBL_VAL=10.25,
    // LABEL="Forest  "
    WriteBE32(dat, 0, 1);
    WriteBE32(dat, 4, 2);
    WriteBE16(dat, 8, 11);
    float f0 = 1.5f;
    uint32_t fb0 = 0;
    std::memcpy(&fb0, &f0, 4);
    WriteBE32(dat, 10, fb0);
    WriteBEDouble(dat, 14, 10.25);
    std::memcpy(dat.data() + 22, "Forest  ", 8);

    // Record 1: VALUE=2, COUNT=2, SHORT_ID=22, FLT_VAL=2.5f, DBL_VAL=20.75,
    // LABEL="Water   "
    WriteBE32(dat, 30, 2);
    WriteBE32(dat, 34, 2);
    WriteBE16(dat, 38, 22);
    float f1 = 2.5f;
    uint32_t fb1 = 0;
    std::memcpy(&fb1, &f1, 4);
    WriteBE32(dat, 40, fb1);
    WriteBEDouble(dat, 44, 20.75);
    std::memcpy(dat.data() + 52, "Water   ", 8);
    WriteMemFile(info_dir.path() + "/arc0000.dat", dat);

    auto ds = absl::WrapUnique(static_cast<GDALDataset*>(
        GDALOpenEx(cover_dir.path().c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    GDALRasterAttributeTable* rat = ds->GetRasterBand(1)->GetDefaultRAT();
    ASSERT_NE(nullptr, rat);
    ASSERT_EQ(6, rat->GetColumnCount());
    ASSERT_EQ(2, rat->GetRowCount());

    EXPECT_EQ(GFU_MinMax, rat->GetUsageOfCol(0));
    EXPECT_EQ(GFT_Integer, rat->GetTypeOfCol(0));
    EXPECT_EQ(GFU_PixelCount, rat->GetUsageOfCol(1));
    EXPECT_EQ(GFT_Real, rat->GetTypeOfCol(3));
    EXPECT_EQ(GFT_Real, rat->GetTypeOfCol(4));
    EXPECT_EQ(GFT_String, rat->GetTypeOfCol(5));

    EXPECT_EQ(1, rat->GetValueAsInt(0, 0));
    EXPECT_EQ(2, rat->GetValueAsInt(0, 1));
    EXPECT_EQ(11, rat->GetValueAsInt(0, 2));
    EXPECT_DOUBLE_EQ(1.5, rat->GetValueAsDouble(0, 3));
    EXPECT_DOUBLE_EQ(10.25, rat->GetValueAsDouble(0, 4));
    EXPECT_STREQ("Forest", rat->GetValueAsString(0, 5));

    EXPECT_EQ(2, rat->GetValueAsInt(1, 0));
    EXPECT_EQ(22, rat->GetValueAsInt(1, 2));
    EXPECT_DOUBLE_EQ(2.5, rat->GetValueAsDouble(1, 3));
    EXPECT_DOUBLE_EQ(20.75, rat->GetValueAsDouble(1, 4));
    EXPECT_STREQ("Water", rat->GetValueAsString(1, 5));

    // Also test corrupt .nit file in same ../info to trigger non-suppressed
    // error re-emission in AIGErrorHandlerVATOpen.
    WriteMemFile(info_dir.path() + "/arc0000.nit", std::vector<GByte>(10, 0));
    auto ds_bad_nit = absl::WrapUnique(static_cast<GDALDataset*>(
        GDALOpenEx(cover_dir.path().c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds_bad_nit);
    EXPECT_EQ(nullptr, ds_bad_nit->GetRasterBand(1)->GetDefaultRAT());

    // Test arc.dir without matching MYCOV.VAT -> suppresses "Failed to open
    // table".
    std::memcpy(arc_dir.data(), "OTHER.VAT", 9);
    WriteMemFile(info_dir.path() + "/arc.dir", arc_dir);
    auto ds_no_tbl = absl::WrapUnique(static_cast<GDALDataset*>(
        GDALOpenEx(cover_dir.path().c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds_no_tbl);
    EXPECT_EQ(nullptr, ds_no_tbl->GetRasterBand(1)->GetDefaultRAT());

    // Test truncated arc.dir -> suppresses "EOF encountered in
    // .../info/arc.dir".
    WriteMemFile(info_dir.path() + "/arc.dir", std::vector<GByte>(10, 0));
    auto ds_eof_dir = absl::WrapUnique(static_cast<GDALDataset*>(
        GDALOpenEx(cover_dir.path().c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds_eof_dir);
    EXPECT_EQ(nullptr, ds_eof_dir->GetRasterBand(1)->GetDefaultRAT());
  }
}

TEST_F(AigdatasetTest, RenameAndDeleteDataset) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };

  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("AIG");
  ASSERT_NE(nullptr, driver);

  // Rename and Delete non-existent dataset -> CE_Failure.
  EXPECT_EQ(CE_Failure, driver->Rename("/vsimem/aigds_nonexist_dst",
                                       "/vsimem/aigds_nonexist_src"));
  EXPECT_EQ(CE_Failure, driver->Delete("/vsimem/aigds_nonexist_src"));

  // Create valid coverage and rename it via directory path, then via .adf
  // path, and finally delete it.
  {
    VsiDirGuard src_dir("/vsimem/aigds_ren_src");
    WriteGridMetadata(src_dir.path(), {});
    WriteUncompressedIntBlockTile(src_dir.path(), "w001001", {5, 6, 7, 8});

    ASSERT_EQ(CE_None,
              driver->Rename("/vsimem/aigds_ren_dst", "/vsimem/aigds_ren_src"));
    absl::Cleanup cleanup_dst = [] {
      CPLUnlinkTree("/vsimem/aigds_ren_dst");
      CPLUnlinkTree("/vsimem/aigds_ren_dst2");
    };

    // Rename via .adf path.
    ASSERT_EQ(CE_None, driver->Rename("/vsimem/aigds_ren_dst2/hdr.adf",
                                      "/vsimem/aigds_ren_dst/hdr.adf"));

    // Verify renamed dataset opens and reads properly.
    auto ds = absl::WrapUnique(static_cast<GDALDataset*>(
        GDALOpenEx("/vsimem/aigds_ren_dst2", GDAL_OF_RASTER | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    GInt32 buf[4] = {};
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, buf,
                                                      2, 2, GDT_Int32, 0, 0));
    EXPECT_THAT(buf, ElementsAre(5, 6, 7, 8));
    ds.reset();

    // Delete via driver. Note: AIGDelete in aigdataset.cpp omits
    // CSLDestroy(papszFileList), so disable leak checking for this call.
    {
      absl::LeakCheckDisabler disabler;
      ASSERT_EQ(CE_None, driver->Delete("/vsimem/aigds_ren_dst2"));
    }
    EXPECT_EQ(nullptr, GDALOpenEx("/vsimem/aigds_ren_dst2", GDAL_OF_RASTER,
                                  nullptr, nullptr, nullptr));
  }
}

}  // namespace
}  // namespace autotest2
