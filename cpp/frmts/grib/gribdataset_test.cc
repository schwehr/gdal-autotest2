// Copyright 2017 Google Inc. All Rights Reserved.
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
// Tests GRIB raster driver (gribdataset.cpp).
// Note: google3 builds GDAL with WITHOUT_CPLDEBUG and without g2clib USE_PNG
// (Template 5.41 metadata loads, while decoding pixel data returns an error).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/grib/gribdataset.cpp
//   https://gdal.org/drivers/raster/grib.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/grib.py

#include "third_party/gdal/frmts/grib/gribdataset.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>  // NOLINT
#include <memory>
#include <span>  // NOLINT
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/grib/gribdrivercore.h"
#include "third_party/gdal/frmts/mem/memdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_geotransform.h"
#include "third_party/gdal/gcore/gdal_multidim.h"
#include "third_party/gdal/gcore/gdal_openinfo.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::Contains;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;

constexpr char kVsiDir[] = "/vsimem/gribdataset_test";

class ScopedVsiMemFile {
 public:
  explicit ScopedVsiMemFile(std::string_view filename)
      : path_(std::format("{}/{}", kVsiDir, filename)) {}
  ScopedVsiMemFile(ScopedVsiMemFile&& other) noexcept
      : path_(std::exchange(other.path_, {})) {}
  ScopedVsiMemFile& operator=(ScopedVsiMemFile&& other) noexcept {
    if (this != &other) {
      Cleanup();
      path_ = std::exchange(other.path_, {});
    }
    return *this;
  }
  ScopedVsiMemFile(const ScopedVsiMemFile&) = delete;
  ScopedVsiMemFile& operator=(const ScopedVsiMemFile&) = delete;
  ~ScopedVsiMemFile() { Cleanup(); }

  const char* path() const { return path_.c_str(); }
  std::string idx_path() const { return path_ + ".idx"; }
  std::string aux_path() const { return path_ + ".aux.xml"; }

 private:
  void Cleanup() const {
    if (path_.empty()) return;
    VSIUnlink(path_.c_str());
    VSIUnlink((path_ + ".aux.xml").c_str());
    VSIUnlink((path_ + ".idx").c_str());
    VSIUnlink((path_ + ".ovr").c_str());
    VSIUnlink((path_ + ".ovr.aux.xml").c_str());
  }

  std::string path_;
};

void WriteVsiBytes(const char* path, std::span<const GByte> data) {
  VSILFILE* fp = VSIFOpenL(path, "wb");
  ASSERT_NE(nullptr, fp) << path;
  absl::Cleanup close_fp = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
  if (!data.empty()) {
    ASSERT_EQ(data.size(), VSIFWriteL(data.data(), 1, data.size(), fp));
  }
}

void WriteVsiText(const char* path, std::string_view text) {
  WriteVsiBytes(
      path, std::span<const GByte>(reinterpret_cast<const GByte*>(text.data()),
                                   text.size()));
}

std::vector<GByte> ReadVsiBytes(const char* path) {
  vsi_l_offset len = 0;
  GByte* ptr = VSIGetMemFileBuffer(path, &len, FALSE);
  if (ptr != nullptr && len > 0) {
    return std::vector<GByte>(ptr, ptr + len);
  }
  VSILFILE* fp = VSIFOpenL(path, "rb");
  if (fp == nullptr) return {};
  absl::Cleanup close_fp = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
  VSIFSeekL(fp, 0, SEEK_END);
  const vsi_l_offset sz = VSIFTellL(fp);
  VSIFSeekL(fp, 0, SEEK_SET);
  std::vector<GByte> buf(static_cast<size_t>(sz));
  if (sz > 0) {
    EXPECT_EQ(buf.size(), VSIFReadL(buf.data(), 1, buf.size(), fp));
  }
  return buf;
}

constexpr uint32_t ReadU32BE(const GByte* src) {
  return (static_cast<uint32_t>(src[0]) << 24) |
         (static_cast<uint32_t>(src[1]) << 16) |
         (static_cast<uint32_t>(src[2]) << 8) | static_cast<uint32_t>(src[3]);
}

constexpr void WriteU16BE(GByte* dst, uint16_t v) {
  dst[0] = static_cast<GByte>((v >> 8) & 0xFF);
  dst[1] = static_cast<GByte>(v & 0xFF);
}

constexpr void WriteU32BE(GByte* dst, uint32_t v) {
  dst[0] = static_cast<GByte>((v >> 24) & 0xFF);
  dst[1] = static_cast<GByte>((v >> 16) & 0xFF);
  dst[2] = static_cast<GByte>((v >> 8) & 0xFF);
  dst[3] = static_cast<GByte>(v & 0xFF);
}

constexpr void WriteS32BE(GByte* dst, int32_t v) {
  WriteU32BE(dst, static_cast<uint32_t>(v));
}

constexpr void WriteU64BE(GByte* dst, uint64_t v) {
  WriteU32BE(dst, static_cast<uint32_t>(v >> 32));
  WriteU32BE(dst + 4, static_cast<uint32_t>(v & 0xFFFFFFFFULL));
}

constexpr void WriteS24BE(GByte* dst, int32_t v) {
  const uint32_t mag =
      v < 0 ? static_cast<uint32_t>(-v) | 0x800000U : static_cast<uint32_t>(v);
  dst[0] = static_cast<GByte>((mag >> 16) & 0xFF);
  dst[1] = static_cast<GByte>((mag >> 8) & 0xFF);
  dst[2] = static_cast<GByte>(mag & 0xFF);
}

size_t FindGrib2SectionOffset(std::span<const GByte> msg, int target_section,
                              int subgrid_index = 0) {
  size_t off = 16;
  int cur_subgrid = 0;
  while (off + 5 <= msg.size()) {
    if (std::memcmp(msg.data() + off, "7777", 4) == 0) {
      break;
    }
    const uint32_t sect_len = ReadU32BE(&msg[off]);
    const int sect_num = msg[off + 4];
    if (sect_len < 5 || off + sect_len > msg.size()) {
      break;
    }
    if (cur_subgrid == subgrid_index && sect_num == target_section) {
      return off;
    }
    if (sect_num == 7) {
      ++cur_subgrid;
      if (cur_subgrid > subgrid_index) {
        break;
      }
    }
    off += sect_len;
  }
  return std::string::npos;
}

void UpdateGrib2MessageLength(std::vector<GByte>& msg) {
  ASSERT_GE(msg.size(), 16U);
  WriteU64BE(&msg[8], static_cast<uint64_t>(msg.size()));
}

OGRSpatialReference MakeWGS84() {
  OGRSpatialReference srs;
  srs.SetWellKnownGeogCS("WGS84");
  return srs;
}

std::unique_ptr<MEMDataset> MakeMemDataset(
    int xsize, int ysize, int nbands, GDALDataType dt,
    const OGRSpatialReference* srs,
    const GDALGeoTransform& gt = GDALGeoTransform(0.0, 1.0, 0.0, 10.0, 0.0,
                                                  -1.0)) {
  std::unique_ptr<MEMDataset> ds(
      MEMDataset::Create("", xsize, ysize, nbands, dt, nullptr));
  if (srs != nullptr) {
    ds->SetSpatialRef(srs);
  }
  ds->SetGeoTransform(gt);
  return ds;
}

template <typename T>
void FillBand(GDALDataset* ds, int band, std::span<const T> values,
              GDALDataType dt) {
  ASSERT_EQ(CE_None,
            ds->GetRasterBand(band)->RasterIO(
                GF_Write, 0, 0, ds->GetRasterXSize(), ds->GetRasterYSize(),
                const_cast<T*>(values.data()), ds->GetRasterXSize(),
                ds->GetRasterYSize(), dt, 0, 0, nullptr));
}

std::vector<GByte> CreateGrib2Bytes(GDALDataset* src_ds,
                                    std::span<const std::string> options = {}) {
  CPLStringList csl_opts;
  for (const auto& opt : options) {
    csl_opts.AddString(opt.c_str());
  }
  ScopedVsiMemFile tmp("gribdataset_tmp_create.grb2");
  GDALDatasetUniquePtr out(GRIBDataset::CreateCopy(
      tmp.path(), src_ds, FALSE, csl_opts.List(), nullptr, nullptr));
  if (out == nullptr) return {};
  out.reset();
  return ReadVsiBytes(tmp.path());
}

std::vector<GByte> MakeSimpleGrib2Message(
    int nx, int ny, double min_x, double dx, double max_y, double dy,
    std::span<const std::string> extra_opts = {},
    const OGRSpatialReference* custom_srs = nullptr,
    GDALDataType dt = GDT_Float32, double nodata = 0.0,
    bool has_nodata = false) {
  OGRSpatialReference wgs84 = MakeWGS84();
  const OGRSpatialReference* srs = custom_srs != nullptr ? custom_srs : &wgs84;
  auto mem_ds = MakeMemDataset(
      nx, ny, 1, dt, srs, GDALGeoTransform(min_x, dx, 0.0, max_y, 0.0, -dy));
  if (has_nodata) {
    mem_ds->GetRasterBand(1)->SetNoDataValue(nodata);
  }
  if (dt == GDT_Int32) {
    std::vector<int32_t> vals(static_cast<size_t>(nx) * ny);
    for (size_t i = 0; i < vals.size(); ++i) {
      vals[i] = static_cast<int32_t>((i * i * i) % 500 + i + 270);
    }
    if (has_nodata && !vals.empty()) {
      vals[0] = static_cast<int32_t>(nodata);
    }
    FillBand<int32_t>(mem_ds.get(), 1, vals, GDT_Int32);
  } else {
    std::vector<float> vals(static_cast<size_t>(nx) * ny);
    for (size_t i = 0; i < vals.size(); ++i) {
      vals[i] = static_cast<float>((i * i * i) % 500 + i) * 0.25f + 273.15f;
    }
    if (has_nodata && !vals.empty()) {
      vals[0] = static_cast<float>(nodata);
    }
    FillBand<float>(mem_ds.get(), 1, vals, GDT_Float32);
  }
  return CreateGrib2Bytes(mem_ds.get(), extra_opts);
}

// Builds a valid 87-byte GRIB1 message for a 2x2 Lat/Lon grid of Temperature.
std::vector<GByte> MakeGrib1Message(uint8_t p1_hours, bool oblate) {
  std::vector<GByte> msg;
  msg.reserve(87);
  // Section 0 (8 bytes): "GRIB", length = 87, edition = 1
  msg.insert(msg.end(), {'G', 'R', 'I', 'B', 0x00, 0x00, 0x57, 0x01});

  // Section 1 (PDS, 28 bytes)
  std::array<GByte, 28> pds{};
  pds[2] = 28;              // sectLen = 28
  pds[3] = 2;               // table version 2
  pds[4] = 7;               // NCEP
  pds[5] = 81;              // generating process
  pds[6] = 255;             // grid ID
  pds[7] = 0x80;            // GDS present, no BMS
  pds[8] = 11;              // parameter 11: Temperature [K]
  pds[9] = 105;             // level type 105: fixed height above ground
  WriteU16BE(&pds[10], 2);  // 2 m
  pds[12] = 25;             // year of century (2025 with century=21)
  pds[13] = 1;              // month
  pds[14] = 1;              // day
  pds[15] = 0;              // hour
  pds[16] = 0;              // minute
  pds[17] = 1;              // time unit: hour
  pds[18] = p1_hours;       // P1
  pds[19] = 0;              // P2
  pds[20] = 0;              // time range indicator: refTime + P1
  pds[24] = 21;             // century
  pds[25] = 0;              // subcenter
  msg.insert(msg.end(), pds.begin(), pds.end());

  // Section 2 (GDS, 32 bytes: Lat/Lon 2x2)
  std::array<GByte, 32> gds{};
  gds[2] = 32;                     // sectLen = 32
  gds[3] = 0;                      // NV = 0
  gds[4] = 255;                    // PV = 255
  gds[5] = 0;                      // Lat/Lon grid
  WriteU16BE(&gds[6], 2);          // Nx = 2
  WriteU16BE(&gds[8], 2);          // Ny = 2
  WriteS24BE(&gds[10], 10000);     // lat1 = 10.0 deg
  WriteS24BE(&gds[13], 0);         // lon1 = 0.0 deg
  gds[16] = oblate ? 0xC0 : 0x80;  // increments given, oblate vs sphere
  WriteS24BE(&gds[17], 11000);     // lat2 = 11.0 deg
  WriteS24BE(&gds[20], 1000);      // lon2 = 1.0 deg
  WriteU16BE(&gds[23], 1000);      // Dx = 1.0 deg
  WriteU16BE(&gds[25], 1000);      // Dy = 1.0 deg
  gds[27] = 0x40;                  // scan mode: bottom-to-top, left-to-right
  msg.insert(msg.end(), gds.begin(), gds.end());

  // Section 4 (BDS, 15 bytes: 11-byte header + 4 8-bit values)
  std::array<GByte, 15> bds{};
  bds[2] = 15;    // sectLen = 15
  bds[3] = 0;     // simple packing, grid-point, float, 0 unused bits
  bds[10] = 8;    // numBits = 8
  bds[11] = 200;  // 200 K -> -73.15 C
  bds[12] = 210;
  bds[13] = 220;
  bds[14] = 230;
  msg.insert(msg.end(), bds.begin(), bds.end());

  // Section 5 (4 bytes): "7777"
  msg.insert(msg.end(), {'7', '7', '7', '7'});
  return msg;
}

std::string MakePds0Values(int cat, int num, int fcst_hours,
                           int surf_type = 103, int surf_val = 2) {
  return std::format("{} {} 2 0 81 0 0 1 {} {} 0 {} 255 0 0", cat, num,
                     fcst_hours, surf_type, surf_val);
}

class GribDatasetTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    // GDALGRIBDriver::InitializeMetadata() latches m_bHasFullInitMetadata =
    // true once per process on the first metadata query, so JP2KAK (and PNG)
    // must be registered before DriverMetadataAndRegistration queries GRIB
    // metadata. PNG also prevents an upstream leak in
    // GRIB2Section567Writer::GetFloatData() when testing DATA_ENCODING=PNG, and
    // GTiff is required by GDALDataset::BuildOverviews() in
    // OpenErrorsAndEdgeCases.
    GDALRegister_JP2KAK();
    GDALRegister_GRIB();
    GDALRegister_GTiff();
    GDALRegister_PNG();
  }

  void SetUp() override {
    CPLErrorReset();
    VSIMkdir(kVsiDir, 0755);
  }

  void TearDown() override {
    CPLErrorReset();
    VSIRmdirRecursive(kVsiDir);
  }
};

TEST_F(GribDatasetTest, IdentifyDoesNotExist) {
  GDALOpenInfo open_info("/does_not_exist", GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(FALSE, GRIBDriverIdentify(&open_info));
}

TEST_F(GribDatasetTest, UnpackFails) {
  WithQuietHandler quiet;
  absl::Cleanup reset_err = [] { CPLErrorReset(); };

  // 1. Corrupt Section 7 bitstream in a complex-packed GRIB2 message (use
  // 8x8 non-uniform grid so complex packing emits group descriptors):
  // GRIBDriverIdentify and Open() metadata inspection succeed, while RasterIO()
  // fails in LoadData() when degrib unpacks Section 7.
  const auto cmplx_opts =
      std::to_array<std::string>({"DATA_ENCODING=COMPLEX_PACKING"});
  std::vector<GByte> msg =
      MakeSimpleGrib2Message(8, 8, 0.0, 1.0, 10.0, 1.0, cmplx_opts);
  ASSERT_FALSE(msg.empty());
  const size_t s7 = FindGrib2SectionOffset(msg, 7);
  ASSERT_NE(std::string::npos, s7);
  constexpr auto kCorruptS7 =
      std::to_array<GByte>({0, 0, 0, 5, 7, '7', '7', '7', '7'});
  msg.erase(msg.begin() + s7, msg.end());
  msg.insert(msg.end(), kCorruptS7.begin(), kCorruptS7.end());
  UpdateGrib2MessageLength(msg);

  ScopedVsiMemFile bad_s7("grib_unpack_corrupt_s7.grb2");
  WriteVsiBytes(bad_s7.path(), msg);
  GDALOpenInfo open_info(bad_s7.path(), GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(TRUE, GRIBDriverIdentify(&open_info));
  std::unique_ptr<GDALDataset> ds(GRIBDataset::Open(&open_info));
  ASSERT_NE(nullptr, ds);

  CPLErrorReset();
  std::array<double, 64> buf{};
  EXPECT_EQ(CE_Failure,
            ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 8, 8, buf.data(), 8,
                                           8, GDT_Float64, 0, 0, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST_F(GribDatasetTest, DriverMetadataAndRegistration) {
  // Calling GDALRegister_GRIB again hits the early-return guard.
  GDALRegister_GRIB();

  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GRIB");
  ASSERT_NE(nullptr, drv);
  EXPECT_STREQ("YES", drv->GetMetadataItem("HAVE_AEC", ""));

  const char* long_name = drv->GetMetadataItem(GDAL_DMD_LONGNAME, "");
  ASSERT_NE(nullptr, long_name);
  EXPECT_THAT(long_name, HasSubstr("GRIdded Binary"));

  const char* creation_opts =
      drv->GetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST, "");
  ASSERT_NE(nullptr, creation_opts);
  EXPECT_THAT(creation_opts, HasSubstr("SIMPLE_PACKING"));
  EXPECT_THAT(creation_opts, HasSubstr("COMPLEX_PACKING"));
  EXPECT_THAT(creation_opts, HasSubstr("PNG"));
  EXPECT_THAT(creation_opts, HasSubstr("PDS_TEMPLATE_ASSEMBLED_VALUES"));
  EXPECT_THAT(creation_opts, HasSubstr("JPEG2000_DRIVER"));
  EXPECT_THAT(creation_opts, HasSubstr("JP2KAK"));

  char** md = drv->GetMetadata("");
  EXPECT_NE(nullptr, CSLFetchNameValue(md, GDAL_DMD_CREATIONOPTIONLIST));
}

TEST_F(GribDatasetTest, OpenErrorsAndEdgeCases) {
  WithQuietHandler quiet;
  absl::Cleanup reset_err = [] { CPLErrorReset(); };

  // 1. fpL == nullptr (subdataset / non-file syntax).
  {
    const std::string sub_path =
        std::format("GRIB:{}/no_such_file.grb2", kVsiDir);
    GDALOpenInfo open_info(sub_path.c_str(), GDAL_OF_READONLY);
    EXPECT_EQ(nullptr, GRIBDataset::Open(&open_info));
  }

  // 2. Truncated Section 0 (< 16 bytes) -> ReadSECT0 fails.
  {
    ScopedVsiMemFile trunc("grib_trunc_sect0.grb2");
    constexpr auto kData =
        std::to_array<GByte>({'G', 'R', 'I', 'B', 0, 0, 0, 2, 0, 0});
    WriteVsiBytes(trunc.path(), kData);
    GDALOpenInfo open_info(trunc.path(), GDAL_OF_READONLY);
    EXPECT_EQ(nullptr, GRIBDataset::Open(&open_info));
  }

  // 3. Valid GRIB2 message opened with GA_Update -> ReportUpdateNotSupported.
  std::vector<GByte> valid_msg =
      MakeSimpleGrib2Message(4, 2, 0.0, 1.0, 10.0, 1.0);
  ASSERT_FALSE(valid_msg.empty());
  {
    ScopedVsiMemFile upd("grib_update_test.grb2");
    WriteVsiBytes(upd.path(), valid_msg);
    CPLErrorReset();
    GDALOpenInfo open_info(upd.path(), GA_Update);
    EXPECT_EQ(nullptr, GRIBDataset::Open(&open_info));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("does not support update"));
  }

  // 4. Valid 16-byte Section 0 + invalid Section 1 -> Inventory() <= 0.
  {
    ScopedVsiMemFile bad_inv("grib_bad_inv.grb2");
    constexpr auto kBuf = std::to_array<GByte>(
        {'G', 'R', 'I', 'B', 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 32,
         0,   0,   0,   0,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    WriteVsiBytes(bad_inv.path(), kBuf);
    CPLErrorReset();
    GDALOpenInfo open_info(bad_inv.path(), GDAL_OF_READONLY);
    EXPECT_EQ(nullptr, GRIBDataset::Open(&open_info));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("no raster dataset was successfully"));

    // Same file opened in MULTIDIM_RASTER mode -> OpenMultiDim inventory <= 0.
    CPLErrorReset();
    GDALOpenInfo md_info(bad_inv.path(), GDAL_OF_MULTIDIM_RASTER);
    EXPECT_EQ(nullptr, GRIBDataset::Open(&md_info));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("no raster dataset was successfully"));
  }

  // 5. Sidecar .idx succeeds on a 32-byte truncated GRIB2 file, then
  // ReadGribData on band 1 returns metaData == nullptr.
  {
    ScopedVsiMemFile bad_meta("grib_sidecar_bad_meta.grb2");
    constexpr auto kBuf = std::to_array<GByte>(
        {'G', 'R', 'I', 'B', 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 32,
         0,   0,   0,   0,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    WriteVsiBytes(bad_meta.path(), kBuf);
    WriteVsiText(bad_meta.idx_path().c_str(), "1:0:d=2025010100:TMP:2 m:anl\n");
    CPLErrorReset();
    GDALOpenInfo open_info(bad_meta.path(), GDAL_OF_READONLY);
    EXPECT_EQ(nullptr, GRIBDataset::Open(&open_info));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("no raster dataset was successfully"));
  }

  // 6. GRIB2 message with Nx = 0, Ny = 0 in Section 3 -> Inventory succeeds,
  // ReadGribData returns metaData == nullptr in both Open() and OpenMultiDim().
  {
    std::vector<GByte> zero_dim = valid_msg;
    const size_t s3 = FindGrib2SectionOffset(zero_dim, 3);
    ASSERT_NE(std::string::npos, s3);
    WriteU32BE(&zero_dim[s3 + 6], 0);   // numPts = 0
    WriteU32BE(&zero_dim[s3 + 30], 0);  // Nx = 0
    WriteU32BE(&zero_dim[s3 + 34], 0);  // Ny = 0
    ScopedVsiMemFile zero_file("grib_zero_dim.grb2");
    WriteVsiBytes(zero_file.path(), zero_dim);

    CPLErrorReset();
    GDALOpenInfo open_info(zero_file.path(), GDAL_OF_READONLY);
    EXPECT_EQ(nullptr, GRIBDataset::Open(&open_info));

    CPLErrorReset();
    GDALOpenInfo md_info(zero_file.path(), GDAL_OF_MULTIDIM_RASTER);
    EXPECT_EQ(nullptr, GRIBDataset::Open(&md_info));
  }

  // 7. External PAM .aux.xml and external overview .ovr loading.
  {
    CPLConfigOptionSetter pam_enabled("GDAL_PAM_ENABLED", "YES", false);
    ScopedVsiMemFile pam_file("grib_pam_ovr.grb2");
    WriteVsiBytes(pam_file.path(), valid_msg);
    WriteVsiText(
        pam_file.aux_path().c_str(),
        "<PAMDataset><Metadata><MDI key=\"CUSTOM_PAM\">hello</MDI></Metadata>"
        "</PAMDataset>");
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(pam_file.path(), GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_STREQ("hello", ds->GetMetadataItem("CUSTOM_PAM"));
    int overview_level = 2;
    ASSERT_EQ(CE_None, ds->BuildOverviews("NEAREST", 1, &overview_level, 0,
                                          nullptr, nullptr, nullptr));
    ds.reset();

    // Reopen to exercise TryLoadXML and oOvManager.Initialize with .ovr
    // present.
    ds.reset(GDALDataset::Open(pam_file.path(), GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(1, ds->GetRasterBand(1)->GetOverviewCount());
  }
}

TEST_F(GribDatasetTest, SidecarIdxAndVsiSubfile) {
  // Create a 2-subgrid GRIB2 message (WRITE_SUBGRIDS=YES).
  OGRSpatialReference wgs84 = MakeWGS84();
  auto mem_ds = MakeMemDataset(4, 2, 2, GDT_Float32, &wgs84);
  const std::vector<float> b1 = {280.0f, 281.0f, 282.0f, 283.0f,
                                 284.0f, 285.0f, 286.0f, 287.0f};
  const std::vector<float> b2 = {50.0f, 55.0f, 60.0f, 65.0f,
                                 70.0f, 75.0f, 80.0f, 85.0f};
  FillBand<float>(mem_ds.get(), 1, b1, GDT_Float32);
  FillBand<float>(mem_ds.get(), 2, b2, GDT_Float32);
  const std::vector<GByte> subgrid_msg = CreateGrib2Bytes(
      mem_ds.get(),
      std::to_array<std::string>(
          {"WRITE_SUBGRIDS=YES", "BAND_1_PDS_PDTN=0",
           "BAND_1_PDS_TEMPLATE_ASSEMBLED_VALUES=" + MakePds0Values(0, 0, 0),
           "BAND_2_PDS_PDTN=0",
           "BAND_2_PDS_TEMPLATE_ASSEMBLED_VALUES=" + MakePds0Values(1, 1, 6)}));
  ASSERT_FALSE(subgrid_msg.empty());

  // 1. Valid sidecar .idx with 1-part and 2-part msgNum.subgNum.
  {
    ScopedVsiMemFile f("grib_subgrid_sidecar.grb2");
    WriteVsiBytes(f.path(), subgrid_msg);
    WriteVsiText(f.idx_path().c_str(),
                 "1:0:d=2025010100:TMP:2 m above ground:anl\n"
                 "1.2:0:d=2025010100:RH:2 m above ground:6 hour fcst\n");
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(f.path(), GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(2, ds->GetRasterCount());

    // Before FindMetaData, GetDescription returns the sidecar longFstLevel.
    EXPECT_STREQ("TMP:2 m above ground:anl",
                 ds->GetRasterBand(1)->GetDescription());
    EXPECT_STREQ("RH:2 m above ground:6 hour fcst",
                 ds->GetRasterBand(2)->GetDescription());

    // Band 1 already has m_Grib_MetaData set, while bLoadedMetadata is false.
    char** md1 = ds->GetRasterBand(1)->GetMetadata();
    EXPECT_STREQ("TMP", CSLFetchNameValue(md1, "GRIB_ELEMENT"));
    EXPECT_STREQ("[C]", CSLFetchNameValue(md1, "GRIB_UNIT"));

    // Band 2 has m_Grib_MetaData == nullptr and subgNum == 1 -> FindMetaData()
    // calls ReadGribData(..., subgNum=1) and FindPDSTemplateGRIB2() skips
    // subgrid 0's sections.
    char** md2 = ds->GetRasterBand(2)->GetMetadata();
    EXPECT_STREQ("RH", CSLFetchNameValue(md2, "GRIB_ELEMENT"));
    EXPECT_STREQ("[%]", CSLFetchNameValue(md2, "GRIB_UNIT"));
    EXPECT_STREQ("21600", CSLFetchNameValue(md2, "GRIB_FORECAST_SECONDS"));

    // Reopen with USE_IDX=NO open option -> bypasses sidecar .idx.
    const char* open_opts[] = {"USE_IDX=NO", nullptr};
    std::unique_ptr<GDALDataset> ds_noidx(GDALDataset::Open(
        f.path(), GDAL_OF_READONLY, nullptr, open_opts, nullptr));
    ASSERT_NE(nullptr, ds_noidx);
    EXPECT_EQ(2, ds_noidx->GetRasterCount());
  }

  // 2. /vsisubfile/<offset>_<size>,<container> with .idx filtering entries
  // before nStartOffset, inside [nStartOffset, nStartOffset + nSize), and
  // >= nStartOffset + nSize.
  {
    ScopedVsiMemFile container("grib_subfile_container.bin");
    constexpr size_t kPrefixLen = 128;
    constexpr size_t kSuffixLen = 64;
    std::vector<GByte> blob(kPrefixLen, 0xAA);
    blob.insert(blob.end(), subgrid_msg.begin(), subgrid_msg.end());
    blob.insert(blob.end(), kSuffixLen, 0xBB);
    WriteVsiBytes(container.path(), blob);

    const std::string idx_text = std::format(
        "1:0:d=2025010100:SKIP_BEFORE:surface:anl\n"
        "2:{}:d=2025010100:TMP:2 m above ground:anl\n"
        "2.2:{}:d=2025010100:RH:2 m above ground:6 hour fcst\n"
        "3:{}:d=2025010100:SKIP_AFTER:surface:anl\n",
        kPrefixLen, kPrefixLen, kPrefixLen + subgrid_msg.size());
    WriteVsiText(container.idx_path().c_str(), idx_text);

    const std::string subfile_path =
        std::format("/vsisubfile/{}_{},{}", kPrefixLen, subgrid_msg.size(),
                    container.path());
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(subfile_path.c_str(), GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(2, ds->GetRasterCount());
    EXPECT_STREQ("TMP:2 m above ground:anl",
                 ds->GetRasterBand(1)->GetDescription());
    EXPECT_STREQ("RH:2 m above ground:6 hour fcst",
                 ds->GetRasterBand(2)->GetDescription());
  }

  // 3. Malformed sidecar .idx variants falling back to InventoryWrapperGrib.
  const std::vector<GByte> valid_msg =
      MakeSimpleGrib2Message(4, 2, 0.0, 1.0, 10.0, 1.0);
  ASSERT_FALSE(valid_msg.empty());
  constexpr auto kBadIdxContents = std::to_array<std::string_view>({
      "1:0:d=2025010100:TMP:2m\n",                  // < 6 tokens
      "abc:0:d=2025010100:TMP:2m:anl\n",            // non-numeric msgNum
      "1.abc:0:d=2025010100:TMP:2m:anl\n",          // non-numeric subgNum
      "1.0:0:d=2025010100:TMP:2m:anl\n",            // subgNum <= 0
      "1.70000:0:d=2025010100:TMP:2m:anl\n",        // subgNum > 65536
      "1:not_an_offset:d=2025010100:TMP:2m:anl\n",  // non-numeric start
  });
  for (size_t i = 0; i < kBadIdxContents.size(); ++i) {
    ScopedVsiMemFile f(std::format("grib_bad_sidecar_{}.grb2", i));
    WriteVsiBytes(f.path(), valid_msg);
    WriteVsiText(f.idx_path().c_str(), kBadIdxContents[i]);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(f.path(), GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds) << "Failed fallback on case " << i;
    EXPECT_EQ(1, ds->GetRasterCount());
  }
  {
    ScopedVsiMemFile f("grib_oversized_sidecar.grb2");
    WriteVsiBytes(f.path(), valid_msg);
    WriteVsiText(f.idx_path().c_str(), std::string(4 * 1024 * 1024 + 16, ' '));
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(f.path(), GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(1, ds->GetRasterCount());
  }
}

TEST_F(GribDatasetTest, BandMetadataPdsTemplatesAndGrib1) {
  WithQuietHandler quiet;
  absl::Cleanup reset_err = [] { CPLErrorReset(); };

  // 1. Discipline codes, IDS fields (known/unknown/missing center & subcenter,
  // signf_ref_time, prod_status, type), and PDTN 8 (4-byte unsigned values).
  // Band 1: known discipline/center/subcenter/PDT8.
  std::vector<GByte> msg_known_ids = MakeSimpleGrib2Message(
      4, 2, 0.0, 1.0, 10.0, 1.0,
      std::to_array<std::string>(
          {"DISCIPLINE=0", "IDS_CENTER=7", "IDS_SUBCENTER=1",
           "IDS_SIGNF_REF_TIME=1", "IDS_REF_TIME=2025-01-01T00:00:00Z",
           "IDS_PROD_STATUS=0", "IDS_TYPE=1", "PDS_PDTN=8",
           "PDS_TEMPLATE_ASSEMBLED_VALUES=0 0 2 0 81 0 0 1 0 103 0 2 255 0 0 "
           "2025 1 1 6 0 0 1 0 1 2 1 6 255 0"}));
  ASSERT_FALSE(msg_known_ids.empty());

  // Band 2: unknown discipline (255), missing center (65535), missing subcenter
  // (65535), out-of-table signf_ref_time / prod_status / type, and
  // unrecognized PDTN 999.
  std::vector<GByte> msg_unknown_pdt =
      MakeSimpleGrib2Message(4, 2, 0.0, 1.0, 10.0, 1.0);
  ASSERT_FALSE(msg_unknown_pdt.empty());
  msg_unknown_pdt[6] = 255;  // discipline 255 (>= table00 size)
  {
    const size_t s1 = FindGrib2SectionOffset(msg_unknown_pdt, 1);
    ASSERT_NE(std::string::npos, s1);
    WriteU16BE(&msg_unknown_pdt[s1 + 5], 65535);  // center missing
    WriteU16BE(&msg_unknown_pdt[s1 + 7], 65535);  // subcenter missing
    msg_unknown_pdt[s1 + 11] = 255;               // signf_ref_time >= 4
    msg_unknown_pdt[s1 + 19] = 255;               // prod_status >= 10
    msg_unknown_pdt[s1 + 20] = 255;               // type >= 9
    const size_t s4 = FindGrib2SectionOffset(msg_unknown_pdt, 4);
    ASSERT_NE(std::string::npos, s4);
    WriteU16BE(&msg_unknown_pdt[s4 + 7], 999);  // PDTN 999 (unrecognized)
  }

  // Band 3: unknown center (250) and subcenter (99), plus Section 4 padded by
  // 2 extra bytes so nTemplateByteCount != nTemplateFoundByteCount.
  std::vector<GByte> msg_mismatch_pdt = MakeSimpleGrib2Message(
      4, 2, 0.0, 1.0, 10.0, 1.0,
      std::to_array<std::string>(
          {"DISCIPLINE=10", "IDS_CENTER=250", "IDS_SUBCENTER=99"}));
  ASSERT_FALSE(msg_mismatch_pdt.empty());
  {
    const size_t s4 = FindGrib2SectionOffset(msg_mismatch_pdt, 4);
    ASSERT_NE(std::string::npos, s4);
    const uint32_t old_len = ReadU32BE(&msg_mismatch_pdt[s4]);
    WriteU32BE(&msg_mismatch_pdt[s4], old_len + 2);
    msg_mismatch_pdt.insert(msg_mismatch_pdt.begin() + s4 + old_len, {0, 0});
    UpdateGrib2MessageLength(msg_mismatch_pdt);
  }

  {
    ScopedVsiMemFile f("grib_meta_pdt.grb2");
    std::vector<GByte> combined = msg_known_ids;
    combined.insert(combined.end(), msg_unknown_pdt.begin(),
                    msg_unknown_pdt.end());
    combined.insert(combined.end(), msg_mismatch_pdt.begin(),
                    msg_mismatch_pdt.end());
    WriteVsiBytes(f.path(), combined);

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(f.path(), GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(3, ds->GetRasterCount());

    GDALRasterBand* b1 = ds->GetRasterBand(1);
    // Querying STATISTICS_MINIMUM skips FindMetaData/FindPDSTemplateGRIB2.
    EXPECT_EQ(nullptr, b1->GetMetadataItem("STATISTICS_MINIMUM"));
    const char* ids1 = b1->GetMetadataItem("GRIB_IDS");
    ASSERT_NE(nullptr, ids1);
    EXPECT_THAT(ids1, HasSubstr("CENTER=7(US-NCEP)"));
    EXPECT_NE(nullptr,
              b1->GetMetadataItem("GRIB_PDS_TEMPLATE_ASSEMBLED_VALUES"));

    CPLErrorReset();
    char** md2 = ds->GetRasterBand(2)->GetMetadata();
    EXPECT_STREQ("255", CSLFetchNameValue(md2, "GRIB_DISCIPLINE"));
    EXPECT_STREQ("999", CSLFetchNameValue(md2, "GRIB_PDS_PDTN"));
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Template 4.999 is not recognized"));

    char** md3 = ds->GetRasterBand(3)->GetMetadata();
    EXPECT_STREQ("10(Oceanographic_Products)",
                 CSLFetchNameValue(md3, "GRIB_DISCIPLINE"));
    EXPECT_EQ(nullptr,
              CSLFetchNameValue(md3, "GRIB_PDS_TEMPLATE_ASSEMBLED_VALUES"));
  }

  // 2. Leading garbage before "GRIB" (FindTrueStart) + GRIB_NORMALIZE_UNITS=NO
  // + GRIB_PDS_ALL_BANDS=NO.
  {
    std::vector<GByte> tmp_msg = MakeSimpleGrib2Message(
        4, 2, 0.0, 1.0, 10.0, 1.0,
        std::to_array<std::string>(
            {"PDS_PDTN=0",
             "PDS_TEMPLATE_ASSEMBLED_VALUES=" + MakePds0Values(0, 0, 0)}));
    ASSERT_FALSE(tmp_msg.empty());
    std::vector<GByte> with_garbage(40, 'X');
    with_garbage.insert(with_garbage.end(), tmp_msg.begin(), tmp_msg.end());
    with_garbage.insert(with_garbage.end(), tmp_msg.begin(), tmp_msg.end());

    ScopedVsiMemFile f("grib_garbage_units.grb2");
    WriteVsiBytes(f.path(), with_garbage);

    CPLConfigOptionSetter norm_units("GRIB_NORMALIZE_UNITS", "NO", false);
    CPLConfigOptionSetter pds_all("GRIB_PDS_ALL_BANDS", "NO", false);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(f.path(), GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(2, ds->GetRasterCount());
    // Band 2 with GRIB_PDS_ALL_BANDS=NO skips FindPDSTemplateGRIB2.
    char** md2 = ds->GetRasterBand(2)->GetMetadata();
    EXPECT_STREQ("[K]", CSLFetchNameValue(md2, "GRIB_UNIT"));
    EXPECT_EQ(nullptr, CSLFetchNameValue(md2, "GRIB_IDS"));
    double val = 0.0;
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &val, 1, 1,
                                             GDT_Float64, 0, 0, nullptr));
    EXPECT_GT(val, 200.0);  // Unconverted Kelvin (> 200 K)
  }

  // 3. GRIB1 multi-message file with sidecar .idx -> exercises m_nGribVersion
  // == 1 in FindMetaData() and oblate vs spherical earth.
  {
    std::vector<GByte> g1_a =
        MakeGrib1Message(/*p1_hours=*/0, /*oblate=*/false);
    std::vector<GByte> g1_b = MakeGrib1Message(/*p1_hours=*/6, /*oblate=*/true);
    std::vector<GByte> g1_all = g1_a;
    g1_all.insert(g1_all.end(), g1_b.begin(), g1_b.end());

    ScopedVsiMemFile f("grib1_multi.grb");
    WriteVsiBytes(f.path(), g1_all);
    WriteVsiText(f.idx_path().c_str(),
                 std::format("1:0:d=2025010100:TMP:2 m:anl\n"
                             "2:{}:d=2025010100:TMP:2 m:6 hour fcst\n",
                             g1_a.size()));
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(f.path(), GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(2, ds->GetRasterCount());
    char** md1 = ds->GetRasterBand(1)->GetMetadata();
    EXPECT_STREQ("TMP", CSLFetchNameValue(md1, "GRIB_ELEMENT"));
    EXPECT_STREQ("0", CSLFetchNameValue(md1, "GRIB_FORECAST_SECONDS"));
    char** md2 = ds->GetRasterBand(2)->GetMetadata();
    EXPECT_STREQ("TMP", CSLFetchNameValue(md2, "GRIB_ELEMENT"));
    EXPECT_STREQ("21600", CSLFetchNameValue(md2, "GRIB_FORECAST_SECONDS"));
    std::array<double, 4> px = {-9999.0, -9999.0, -9999.0, -9999.0};
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, px.data(), 2,
                                             2, GDT_Float64, 0, 0, nullptr));
    // Bottom-left pixel (200 K) converted to Celsius = -73.15 C.
    EXPECT_NEAR(200.0 - 273.15, px[2], 1e-4);
  }
}

TEST_F(GribDatasetTest, NoDataAndBitmaps) {
  // 1. Simple packing with negative decimal scale factor (no NoData), IEEE
  // floating point, and PNG encodings.
  {
    std::vector<GByte> simple_msg = MakeSimpleGrib2Message(
        4, 2, 0.0, 1.0, 10.0, 1.0,
        std::to_array<std::string>(
            {"DATA_ENCODING=SIMPLE_PACKING", "DECIMAL_SCALE_FACTOR=-1"}));
    std::vector<GByte> ieee_msg = MakeSimpleGrib2Message(
        4, 2, 0.0, 1.0, 10.0, 1.0,
        std::to_array<std::string>({"DATA_ENCODING=IEEE_FLOATING_POINT"}));
    std::vector<GByte> png_msg = MakeSimpleGrib2Message(
        4, 2, 0.0, 1.0, 10.0, 1.0,
        std::to_array<std::string>({"DATA_ENCODING=PNG"}));
    ASSERT_FALSE(simple_msg.empty());
    ASSERT_FALSE(ieee_msg.empty());
    ASSERT_FALSE(png_msg.empty());

    std::vector<GByte> all = simple_msg;
    all.insert(all.end(), ieee_msg.begin(), ieee_msg.end());
    all.insert(all.end(), png_msg.begin(), png_msg.end());

    ScopedVsiMemFile f("grib_encodings_drs.grb2");
    WriteVsiBytes(f.path(), all);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(f.path(), GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(3, ds->GetRasterCount());

    int ok = TRUE;
    EXPECT_EQ(0.0, ds->GetRasterBand(1)->GetNoDataValue(&ok));
    EXPECT_EQ(FALSE, ok);
    EXPECT_STREQ("0",
                 ds->GetRasterBand(1)->GetMetadataItem("DRS_DRTN", "GRIB"));
    EXPECT_STREQ("-1", ds->GetRasterBand(1)->GetMetadataItem(
                           "DRS_DECIMAL_SCALE_FACTOR", "GRIB"));
    EXPECT_STREQ("4",
                 ds->GetRasterBand(2)->GetMetadataItem("DRS_DRTN", "GRIB"));
    EXPECT_STREQ("41",
                 ds->GetRasterBand(3)->GetMetadataItem("DRS_DRTN", "GRIB"));
  }

  // 2. Complex packing (8x8 grid, SDO=2) with Float32 and Int32, testing both
  // nMiss=1 and patched nMiss=2 (primary + secondary missing values), in both
  // GetNoDataValue-first and GetMetadata-first orders, plus patched
  // original_field_type == 2.
  {
    constexpr double kNodataFlt = -9999.0;
    constexpr double kNodataInt = 9999.0;
    std::vector<GByte> cmplx_flt = MakeSimpleGrib2Message(
        8, 8, 0.0, 1.0, 10.0, 1.0,
        std::to_array<std::string>(
            {"DATA_ENCODING=COMPLEX_PACKING", "SPATIAL_DIFFERENCING_ORDER=2"}),
        nullptr, GDT_Float32, kNodataFlt, /*has_nodata=*/true);
    std::vector<GByte> cmplx_int = MakeSimpleGrib2Message(
        8, 8, 0.0, 1.0, 10.0, 1.0,
        std::to_array<std::string>(
            {"DATA_ENCODING=COMPLEX_PACKING", "SPATIAL_DIFFERENCING_ORDER=1"}),
        nullptr, GDT_Int32, kNodataInt, /*has_nodata=*/true);
    ASSERT_FALSE(cmplx_flt.empty());
    ASSERT_FALSE(cmplx_int.empty());

    // Patch copies to nMiss = 2 (primary + secondary missing values).
    std::vector<GByte> cmplx_flt_nmiss2 = cmplx_flt;
    {
      const size_t s5 = FindGrib2SectionOffset(cmplx_flt_nmiss2, 5);
      ASSERT_NE(std::string::npos, s5);
      cmplx_flt_nmiss2[s5 + 22] = 2;  // nMiss = 2
    }
    std::vector<GByte> cmplx_int_nmiss2 = cmplx_int;
    {
      const size_t s5 = FindGrib2SectionOffset(cmplx_int_nmiss2, 5);
      ASSERT_NE(std::string::npos, s5);
      cmplx_int_nmiss2[s5 + 22] = 2;  // nMiss = 2
    }
    // Patch a copy with original_field_type = 2 (unknown type in FindNoData).
    std::vector<GByte> cmplx_type2 = cmplx_flt;
    {
      const size_t s5 = FindGrib2SectionOffset(cmplx_type2, 5);
      ASSERT_NE(std::string::npos, s5);
      cmplx_type2[s5 + 20] = 2;  // original_field_type = 2
    }

    // Dataset A: GetNoDataValue() called BEFORE GetMetadata() on bands 2..4.
    {
      std::vector<GByte> all = cmplx_flt;
      all.insert(all.end(), cmplx_flt_nmiss2.begin(), cmplx_flt_nmiss2.end());
      all.insert(all.end(), cmplx_int_nmiss2.begin(), cmplx_int_nmiss2.end());
      all.insert(all.end(), cmplx_int.begin(), cmplx_int.end());
      ScopedVsiMemFile f("grib_nodata_first.grb2");
      WriteVsiBytes(f.path(), all);
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(f.path(), GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      ASSERT_EQ(4, ds->GetRasterCount());
      constexpr auto kExpected = std::to_array<double>(
          {kNodataFlt, kNodataFlt, kNodataInt, kNodataInt});
      for (int b = 1; b <= 4; ++b) {
        int ok = FALSE;
        EXPECT_NEAR(kExpected[b - 1], ds->GetRasterBand(b)->GetNoDataValue(&ok),
                    1.0);
        EXPECT_EQ(TRUE, ok);
        // Second call hits m_bHasLookedForNoData cache.
        EXPECT_NEAR(kExpected[b - 1], ds->GetRasterBand(b)->GetNoDataValue(&ok),
                    1.0);
      }
    }

    // Dataset B: GetMetadata() called BEFORE GetNoDataValue() on bands 2..4
    // (exercises FindNoDataGrib2).
    {
      std::vector<GByte> all = cmplx_flt;
      all.insert(all.end(), cmplx_flt_nmiss2.begin(), cmplx_flt_nmiss2.end());
      all.insert(all.end(), cmplx_int_nmiss2.begin(), cmplx_int_nmiss2.end());
      all.insert(all.end(), cmplx_type2.begin(), cmplx_type2.end());
      ScopedVsiMemFile f("grib_meta_first.grb2");
      WriteVsiBytes(f.path(), all);
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(f.path(), GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ds);
      ASSERT_EQ(4, ds->GetRasterCount());
      EXPECT_STREQ("2", ds->GetRasterBand(2)->GetMetadataItem(
                            "DRS_SPATIAL_DIFFERENCING_ORDER", "GRIB"));
      int ok = FALSE;
      EXPECT_NEAR(kNodataFlt, ds->GetRasterBand(2)->GetNoDataValue(&ok), 1.0);
      EXPECT_EQ(TRUE, ok);
      EXPECT_NEAR(kNodataInt, ds->GetRasterBand(3)->GetNoDataValue(&ok), 1.0);
      EXPECT_EQ(TRUE, ok);
      // Band 4 has original_field_type == 2 -> FindNoDataGrib2 logs debug and
      // does not set m_bHasNoData.
      EXPECT_NE(nullptr, ds->GetRasterBand(4)->GetMetadata());
      EXPECT_EQ(0.0, ds->GetRasterBand(4)->GetNoDataValue(&ok));
      EXPECT_EQ(FALSE, ok);
    }
  }

  // 3. Section 6 bitmap (abySection6[4] == 6 && abySection6[5] == 0) ->
  // m_dfNoData = 9999.0 in both FindNoDataGrib2 and GetNoDataValue.
  {
    std::vector<GByte> bmp_msg =
        MakeSimpleGrib2Message(4, 2, 0.0, 1.0, 10.0, 1.0);
    ASSERT_FALSE(bmp_msg.empty());
    const size_t s6 = FindGrib2SectionOffset(bmp_msg, 6);
    ASSERT_NE(std::string::npos, s6);
    // Replace 6-byte Section 6 ([0,0,0,6,6,255]) with 8-byte bitmap section.
    constexpr auto kNewS6 =
        std::to_array<GByte>({0, 0, 0, 8, 6, 0, 0xF0, 0x00});
    bmp_msg.erase(bmp_msg.begin() + s6, bmp_msg.begin() + s6 + 6);
    bmp_msg.insert(bmp_msg.begin() + s6, kNewS6.begin(), kNewS6.end());
    UpdateGrib2MessageLength(bmp_msg);

    std::vector<GByte> two_bands = bmp_msg;
    two_bands.insert(two_bands.end(), bmp_msg.begin(), bmp_msg.end());
    ScopedVsiMemFile f("grib_bitmap_s6.grb2");
    WriteVsiBytes(f.path(), two_bands);

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(f.path(), GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(2, ds->GetRasterCount());
    // Band 1 already ran FindNoDataGrib2 in Open().
    int ok1 = FALSE;
    EXPECT_EQ(9999.0, ds->GetRasterBand(1)->GetNoDataValue(&ok1));
    EXPECT_EQ(TRUE, ok1);
    // Band 2 runs GetNoDataValue() directly via ReadGribData().
    int ok2 = FALSE;
    EXPECT_EQ(9999.0, ds->GetRasterBand(2)->GetNoDataValue(&ok2));
    EXPECT_EQ(TRUE, ok2);
  }
}

TEST_F(GribDatasetTest, CachingMismatchedBandsAndAntimeridian) {
  WithQuietHandler quiet;
  absl::Cleanup reset_err = [] { CPLErrorReset(); };

  // 1. GRIB_CACHEMAX=0 forces nCachedBytes > nCachedBytesThreshold and
  // bCacheOnlyOneBand UncacheData() across multiple bands.
  {
    CPLConfigOptionSetter cache_max("GRIB_CACHEMAX", "0", false);
    std::vector<GByte> b_msg =
        MakeSimpleGrib2Message(4, 2, 0.0, 1.0, 10.0, 1.0);
    ASSERT_FALSE(b_msg.empty());
    std::vector<GByte> three_bands = b_msg;
    three_bands.insert(three_bands.end(), b_msg.begin(), b_msg.end());
    three_bands.insert(three_bands.end(), b_msg.begin(), b_msg.end());

    ScopedVsiMemFile f("grib_cachemax_zero.grb2");
    WriteVsiBytes(f.path(), three_bands);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(f.path(), GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(3, ds->GetRasterCount());

    std::array<double, 8> buf{};
    for (int band : {1, 2, 3, 1}) {
      EXPECT_EQ(CE_None, ds->GetRasterBand(band)->RasterIO(
                             GF_Read, 0, 0, 4, 2, buf.data(), 4, 2, GDT_Float64,
                             0, 0, nullptr));
    }
  }

  // 2. Mismatched band dimensions (band 1 is 4x2, band 2 is 2x1) -> warning in
  // LoadData and partial/off-image row handling in IReadBlock.
  {
    std::vector<GByte> b1 = MakeSimpleGrib2Message(4, 2, 0.0, 1.0, 10.0, 1.0);
    std::vector<GByte> b2 = MakeSimpleGrib2Message(2, 1, 0.0, 1.0, 10.0, 1.0);
    ASSERT_FALSE(b1.empty());
    ASSERT_FALSE(b2.empty());
    std::vector<GByte> combined = b1;
    combined.insert(combined.end(), b2.begin(), b2.end());

    ScopedVsiMemFile f("grib_mismatched_bands.grb2");
    WriteVsiBytes(f.path(), combined);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(f.path(), GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(2, ds->GetRasterCount());

    CPLErrorReset();
    std::array<double, 8> buf{};
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(2)->RasterIO(GF_Read, 0, 0, 4, 2, buf.data(), 4,
                                             2, GDT_Float64, 0, 0, nullptr));
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("while the first band and dataset is 4x2"));
  }

  // 3. Global 0..350 (36x2, dx=10) grid -> nSplitAndSwapColumn = 18
  // antimeridian rewrapping in SetGribMetaData and IReadBlock.
  {
    OGRSpatialReference wgs84 = MakeWGS84();
    auto mem_ds =
        MakeMemDataset(36, 2, 1, GDT_Float32, &wgs84,
                       GDALGeoTransform(-5.0, 10.0, 0.0, 15.0, 0.0, -10.0));
    std::vector<float> vals(72);
    for (int i = 0; i < 72; ++i) {
      vals[i] = static_cast<float>(i);
    }
    FillBand<float>(mem_ds.get(), 1, vals, GDT_Float32);
    std::vector<GByte> global_msg = CreateGrib2Bytes(mem_ds.get());
    ASSERT_FALSE(global_msg.empty());

    ScopedVsiMemFile f("grib_antimeridian_wrap.grb2");
    WriteVsiBytes(f.path(), global_msg);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(f.path(), GDAL_OF_READONLY));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt;
    ASSERT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_NEAR(-185.0, gt[0], 1e-6);

    std::array<double, 36> row0{};
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 36, 1, row0.data(), 36, 1,
                           GDT_Float64, 0, 0, nullptr));
    // Column 0 of unwrapped grid corresponds to original column 18 (180 E).
    EXPECT_NEAR(18.0, row0[0], 1e-4);
    EXPECT_NEAR(0.0, row0[18], 1e-4);
  }
}

TEST_F(GribDatasetTest, ProjectionsEllipsoidsAndGeotransforms) {
  WithQuietHandler quiet;
  absl::Cleanup reset_err = [] { CPLErrorReset(); };

  struct OpenedDataset {
    ScopedVsiMemFile file;
    std::unique_ptr<GDALDataset> ds;
  };

  auto open_and_check = [](std::span<const GByte> msg,
                           std::string_view tag) -> OpenedDataset {
    OpenedDataset out{
        .file = ScopedVsiMemFile(std::format("grib_proj_{}.grb2", tag)),
        .ds = nullptr};
    WriteVsiBytes(out.file.path(), msg);
    out.ds.reset(GDALDataset::Open(out.file.path(), GDAL_OF_READONLY));
    VSIStatBufL sb;
    EXPECT_EQ(0, VSIStatL(out.file.path(), &sb)) << tag;
    return out;
  };

  // 1. Geographic ellipsoids: WGS84, GRS80, Sphere, Custom Oblate (shape 2),
  // and Invalid Earth Model (majEarth = -1) with GRIB_USE_DEFAULT_SPHEROID
  // unset / NO / YES.
  {
    OGRSpatialReference grs80;
    grs80.SetWellKnownGeogCS("NAD83");
    auto grib_grs80 = open_and_check(
        MakeSimpleGrib2Message(4, 2, 0.0, 1.0, 10.0, 1.0, {}, &grs80), "grs80");
    ASSERT_NE(nullptr, grib_grs80.ds);
    ASSERT_NE(nullptr, grib_grs80.ds->GetSpatialRef());
    EXPECT_NEAR(298.257222101,
                grib_grs80.ds->GetSpatialRef()->GetInvFlattening(), 1e-6);

    OGRSpatialReference sphere;
    sphere.SetGeogCS("Sphere", nullptr, "Sphere", 6371229.0, 0.0);
    auto grib_sphere = open_and_check(
        MakeSimpleGrib2Message(4, 2, 0.0, 1.0, 10.0, 1.0, {}, &sphere),
        "sphere");
    ASSERT_NE(nullptr, grib_sphere.ds);
    ASSERT_NE(nullptr, grib_sphere.ds->GetSpatialRef());
    EXPECT_EQ(6371229.0, grib_sphere.ds->GetSpatialRef()->GetSemiMajor());

    // Shape of earth = 2 (IAU 1965 oblate spheroid: 6378.160 / 6356.775 km).
    std::vector<GByte> iau_msg =
        MakeSimpleGrib2Message(4, 2, 0.0, 1.0, 10.0, 1.0);
    const size_t s3_iau = FindGrib2SectionOffset(iau_msg, 3);
    ASSERT_NE(std::string::npos, s3_iau);
    iau_msg[s3_iau + 14] = 2;
    auto grib_iau = open_and_check(iau_msg, "iau1965");
    ASSERT_NE(nullptr, grib_iau.ds);
    ASSERT_NE(nullptr, grib_iau.ds->GetSpatialRef());
    EXPECT_NEAR(6378160.0, grib_iau.ds->GetSpatialRef()->GetSemiMajor(), 1.0);

    // Invalid earth model: shape = 1 (sphere) with radius = 100 m (< 6300 km)
    // -> metaparse sets majEarth = -1, minEarth = -1.
    std::vector<GByte> bad_earth =
        MakeSimpleGrib2Message(4, 2, 0.0, 1.0, 10.0, 1.0);
    const size_t s3_bad = FindGrib2SectionOffset(bad_earth, 3);
    ASSERT_NE(std::string::npos, s3_bad);
    bad_earth[s3_bad + 14] = 1;
    bad_earth[s3_bad + 15] = 0;
    WriteS32BE(&bad_earth[s3_bad + 16], 100);

    CPLErrorReset();
    auto grib_bad_unset = open_and_check(bad_earth, "bad_earth_unset");
    ASSERT_NE(nullptr, grib_bad_unset.ds);
    EXPECT_EQ(nullptr, grib_bad_unset.ds->GetSpatialRef());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("GRIB_USE_DEFAULT_SPHEROID"));

    {
      CPLConfigOptionSetter opt("GRIB_USE_DEFAULT_SPHEROID", "NO", false);
      auto grib_bad_no = open_and_check(bad_earth, "bad_earth_no");
      ASSERT_NE(nullptr, grib_bad_no.ds);
      EXPECT_EQ(nullptr, grib_bad_no.ds->GetSpatialRef());
    }
    {
      CPLConfigOptionSetter opt("GRIB_USE_DEFAULT_SPHEROID", "YES", false);
      CPLConfigOptionSetter maj("GRIB_DEFAULT_SEMI_MAJOR", "6378137.0", false);
      CPLConfigOptionSetter min("GRIB_DEFAULT_SEMI_MINOR", "6356752.314245",
                                false);
      auto grib_bad_yes = open_and_check(bad_earth, "bad_earth_yes");
      ASSERT_NE(nullptr, grib_bad_yes.ds);
      ASSERT_NE(nullptr, grib_bad_yes.ds->GetSpatialRef());
      EXPECT_NEAR(6378137.0, grib_bad_yes.ds->GetSpatialRef()->GetSemiMajor(),
                  1.0);
    }
  }

  // 2. Geographic geotransform edge cases: 1x1 grid, top-down scan (lat2 <
  // lat1), lon1 > lon2, Dx/Dy mismatch, overlap > 360, rMinX == 180, spanning
  // antimeridian, purely > 180, and GRIB_ADJUST_LONGITUDE_RANGE=NO.
  {
    auto grib_1x1 = open_and_check(
        MakeSimpleGrib2Message(1, 1, 10.0, 0.5, 20.0, 0.5), "1x1");
    ASSERT_NE(nullptr, grib_1x1.ds);
    EXPECT_EQ(1, grib_1x1.ds->GetRasterXSize());
    EXPECT_EQ(1, grib_1x1.ds->GetRasterYSize());

    // Top-down scan (scan = 0x00, lat1 = 20.0, lat2 = 19.0 -> lat2 < rMaxY).
    std::vector<GByte> topdown =
        MakeSimpleGrib2Message(4, 2, 0.0, 1.0, 20.0, 1.0);
    const size_t s3_td = FindGrib2SectionOffset(topdown, 3);
    ASSERT_NE(std::string::npos, s3_td);
    WriteS32BE(&topdown[s3_td + 46], 20000000);  // lat1 = 20.0
    WriteS32BE(&topdown[s3_td + 55], 19000000);  // lat2 = 19.0
    topdown[s3_td + 71] = 0x00;                  // top-down scan
    auto grib_td = open_and_check(topdown, "topdown");
    ASSERT_NE(nullptr, grib_td.ds);
    GDALGeoTransform gt_td;
    ASSERT_EQ(CE_None, grib_td.ds->GetGeoTransform(gt_td));
    EXPECT_NEAR(20.5, gt_td[3], 1e-6);

    // lon1 > lon2 (e.g. lon1 = 350, lon2 = 10, Nx = 3, Dx = 10) + Dx/Dy
    // mismatch (> 0.002).
    std::vector<GByte> wrap_lon =
        MakeSimpleGrib2Message(3, 2, 0.0, 10.0, 10.0, 1.0);
    const size_t s3_wl = FindGrib2SectionOffset(wrap_lon, 3);
    ASSERT_NE(std::string::npos, s3_wl);
    WriteS32BE(&wrap_lon[s3_wl + 50], 350000000);  // lon1 = 350
    WriteS32BE(&wrap_lon[s3_wl + 59], 10000000);   // lon2 = 10
    WriteS32BE(&wrap_lon[s3_wl + 63], 10050000);   // Dx = 10.05 (diff > 0.002)
    WriteS32BE(&wrap_lon[s3_wl + 67], 1050000);    // Dy = 1.05 (diff > 0.002)
    auto grib_wl = open_and_check(wrap_lon, "lon1_gt_lon2");
    ASSERT_NE(nullptr, grib_wl.ds);
    EXPECT_EQ(3, grib_wl.ds->GetRasterXSize());

    // Overlap > 360 (37x2, 0..360 step 10).
    auto grib_overlap = open_and_check(
        MakeSimpleGrib2Message(37, 2, -5.0, 10.0, 15.0, 10.0), "overlap360");
    ASSERT_NE(nullptr, grib_overlap.ds);
    EXPECT_EQ(37, grib_overlap.ds->GetRasterXSize());

    // rMinX == 180 (lon1 = 180, lon2 = 210, Nx = 4, dx = 10).
    auto grib_180 = open_and_check(
        MakeSimpleGrib2Message(4, 2, 175.0, 10.0, 15.0, 10.0), "minx180");
    ASSERT_NE(nullptr, grib_180.ds);
    GDALGeoTransform gt_180;
    ASSERT_EQ(CE_None, grib_180.ds->GetGeoTransform(gt_180));
    EXPECT_NEAR(-185.0, gt_180[0], 1e-6);

    // Spanning antimeridian (lon1 = 170, lon2 = 200, Nx = 4, dx = 10).
    auto grib_span = open_and_check(
        MakeSimpleGrib2Message(4, 2, 165.0, 10.0, 15.0, 10.0), "span_anti");
    ASSERT_NE(nullptr, grib_span.ds);
    GDALGeoTransform gt_span;
    ASSERT_EQ(CE_None, grib_span.ds->GetGeoTransform(gt_span));
    EXPECT_NEAR(165.0, gt_span[0], 1e-6);

    // Purely > 180 (lon1 = 200, lon2 = 230, Nx = 4, dx = 10).
    auto grib_gt180 = open_and_check(
        MakeSimpleGrib2Message(4, 2, 195.0, 10.0, 15.0, 10.0), "gt180");
    ASSERT_NE(nullptr, grib_gt180.ds);
    GDALGeoTransform gt_gt180;
    ASSERT_EQ(CE_None, grib_gt180.ds->GetGeoTransform(gt_gt180));
    EXPECT_NEAR(-165.0, gt_gt180[0], 1e-6);

    // GRIB_ADJUST_LONGITUDE_RANGE=NO preserves 200..230.
    {
      CPLConfigOptionSetter no_adj("GRIB_ADJUST_LONGITUDE_RANGE", "NO", false);
      auto grib_noadj = open_and_check(
          MakeSimpleGrib2Message(4, 2, 195.0, 10.0, 15.0, 10.0), "noadj");
      ASSERT_NE(nullptr, grib_noadj.ds);
      GDALGeoTransform gt_noadj;
      ASSERT_EQ(CE_None, grib_noadj.ds->GetGeoTransform(gt_noadj));
      EXPECT_NEAR(195.0, gt_noadj[0], 1e-6);
    }
  }

  // 3. Gaussian Lat/Lon (template 40) and Rotated Lat/Lon (template 1).
  {
    std::vector<GByte> gauss_msg =
        MakeSimpleGrib2Message(4, 2, 0.0, 1.0, 10.0, 2.0);
    const size_t s3_g = FindGrib2SectionOffset(gauss_msg, 3);
    ASSERT_NE(std::string::npos, s3_g);
    WriteU16BE(&gauss_msg[s3_g + 12], 40);       // GS3_GAUSSIAN_LATLON
    WriteS32BE(&gauss_msg[s3_g + 55], 8000000);  // positive lat2
    WriteS32BE(&gauss_msg[s3_g + 67], 45);       // np = 45 (Dy = 90/45 = 2.0)
    auto grib_gauss = open_and_check(gauss_msg, "gaussian");
    ASSERT_NE(nullptr, grib_gauss.ds);
    ASSERT_NE(nullptr, grib_gauss.ds->GetSpatialRef());
    EXPECT_TRUE(grib_gauss.ds->GetSpatialRef()->IsGeographic());

    OGRSpatialReference rot_srs = MakeWGS84();
    rot_srs.SetDerivedGeogCRSWithPoleRotationGRIBConvention("Rotated", -30.0,
                                                            10.0, 0.0);
    auto grib_rot = open_and_check(
        MakeSimpleGrib2Message(4, 2, 0.0, 1.0, 10.0, 1.0, {}, &rot_srs),
        "rotated");
    ASSERT_NE(nullptr, grib_rot.ds);
    ASSERT_NE(nullptr, grib_rot.ds->GetSpatialRef());
    EXPECT_TRUE(grib_rot.ds->GetSpatialRef()->IsDerivedGeographic());
  }

  // 4. Mercator 1SP, Mercator 2SP, Mercator orientLon != 0, Mercator
  // unprojectable corner (lat1 = 100 deg -> Transform fails), and Mercator
  // top-down scan.
  {
    OGRSpatialReference merc1;
    merc1.SetMercator(0.0, 0.0, 1.0, 0.0, 0.0);
    merc1.SetWellKnownGeogCS("WGS84");
    std::vector<GByte> merc_msg = MakeSimpleGrib2Message(
        4, 2, 0.0, 10000.0, 100000.0, 10000.0, {}, &merc1);
    ASSERT_FALSE(merc_msg.empty());
    auto grib_merc1 = open_and_check(merc_msg, "merc1sp");
    ASSERT_NE(nullptr, grib_merc1.ds);
    ASSERT_NE(nullptr, grib_merc1.ds->GetSpatialRef());
    EXPECT_TRUE(grib_merc1.ds->GetSpatialRef()->IsProjected());

    // Top-down scan on Mercator (scan = 0x00).
    std::vector<GByte> merc_td = merc_msg;
    const size_t s3_mtd = FindGrib2SectionOffset(merc_td, 3);
    ASSERT_NE(std::string::npos, s3_mtd);
    merc_td[s3_mtd + 59] = 0x00;
    auto grib_merc_td = open_and_check(merc_td, "merc_topdown");
    ASSERT_NE(nullptr, grib_merc_td.ds);
    ASSERT_NE(nullptr, grib_merc_td.ds->GetSpatialRef());
    EXPECT_TRUE(grib_merc_td.ds->GetSpatialRef()->IsProjected());

    // orientLon != 0 -> warning and early return in SetGribMetaData.
    std::vector<GByte> merc_orient = merc_msg;
    const size_t s3_mo = FindGrib2SectionOffset(merc_orient, 3);
    ASSERT_NE(std::string::npos, s3_mo);
    WriteS32BE(&merc_orient[s3_mo + 60], 15000000);  // orientLon = 15 deg
    CPLErrorReset();
    auto grib_merc_orient = open_and_check(merc_orient, "merc_orient");
    ASSERT_NE(nullptr, grib_merc_orient.ds);
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Orientation of the grid != 0"));

    // Unprojectable corner (lat1 = 100.0 deg -> Transform fails).
    std::vector<GByte> merc_pole = merc_msg;
    const size_t s3_mp = FindGrib2SectionOffset(merc_pole, 3);
    ASSERT_NE(std::string::npos, s3_mp);
    WriteS32BE(&merc_pole[s3_mp + 38], 100000000);  // lat1 = 100 deg (> 90)
    CPLErrorReset();
    auto grib_merc_pole = open_and_check(merc_pole, "merc_pole_fail");
    ASSERT_NE(nullptr, grib_merc_pole.ds);
    EXPECT_EQ(nullptr, grib_merc_pole.ds->GetSpatialRef());
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Defaulting to ungeoreferenced"));

    OGRSpatialReference merc2;
    merc2.SetMercator2SP(20.0, 0.0, 0.0, 0.0, 0.0);
    merc2.SetWellKnownGeogCS("WGS84");
    auto grib_merc2 =
        open_and_check(MakeSimpleGrib2Message(4, 2, 0.0, 10000.0, 100000.0,
                                              10000.0, {}, &merc2),
                       "merc2sp");
    ASSERT_NE(nullptr, grib_merc2.ds);
    ASSERT_NE(nullptr, grib_merc2.ds->GetSpatialRef());
    EXPECT_TRUE(grib_merc2.ds->GetSpatialRef()->IsProjected());
  }

  // 5. Transverse Mercator (scale = 0.9996 and custom scale = 0.9995).
  {
    OGRSpatialReference tm_utm;
    tm_utm.SetTM(0.0, -75.0, 0.9996, 500000.0, 0.0);
    tm_utm.SetWellKnownGeogCS("WGS84");
    auto grib_tm1 =
        open_and_check(MakeSimpleGrib2Message(4, 2, 400000.0, 1000.0, 4500000.0,
                                              1000.0, {}, &tm_utm),
                       "tm_09996");
    ASSERT_NE(nullptr, grib_tm1.ds);
    ASSERT_NE(nullptr, grib_tm1.ds->GetSpatialRef());
    EXPECT_TRUE(grib_tm1.ds->GetSpatialRef()->IsProjected());

    OGRSpatialReference tm_custom;
    tm_custom.SetTM(0.0, -75.0, 0.9995, 500000.0, 0.0);
    tm_custom.SetWellKnownGeogCS("WGS84");
    auto grib_tm2 =
        open_and_check(MakeSimpleGrib2Message(4, 2, 400000.0, 1000.0, 4500000.0,
                                              1000.0, {}, &tm_custom),
                       "tm_09995");
    ASSERT_NE(nullptr, grib_tm2.ds);
    ASSERT_NE(nullptr, grib_tm2.ds->GetSpatialRef());
    EXPECT_TRUE(grib_tm2.ds->GetSpatialRef()->IsProjected());
  }

  // 6. Polar Stereographic (GS3_POLAR), Lambert Conformal (GS3_LAMBERT +
  // MANAL heuristic + GRIB_LATITUDE_OF_FIRST_GRID_POINT_IS_SOUTHERN_MOST=NO),
  // Albers Equal Area (GS3_ALBERS_EQUAL_AREA), and Lambert Azimuthal
  // (GS3_LAMBERT_AZIMUTHAL).
  {
    OGRSpatialReference ps;
    ps.SetPS(60.0, -105.0, 1.0, 0.0, 0.0);
    ps.SetWellKnownGeogCS("WGS84");
    auto grib_ps =
        open_and_check(MakeSimpleGrib2Message(4, 2, -100000.0, 10000.0,
                                              -1000000.0, 10000.0, {}, &ps),
                       "polar");
    ASSERT_NE(nullptr, grib_ps.ds);
    ASSERT_NE(nullptr, grib_ps.ds->GetSpatialRef());
    EXPECT_TRUE(grib_ps.ds->GetSpatialRef()->IsProjected());

    OGRSpatialReference lcc;
    lcc.SetLCC(30.0, 60.0, 40.0, -95.0, 0.0, 0.0);
    lcc.SetWellKnownGeogCS("WGS84");
    std::vector<GByte> lcc_msg =
        MakeSimpleGrib2Message(4, 2, 0.0, 10000.0, 100000.0, 10000.0, {}, &lcc);
    auto grib_lcc = open_and_check(lcc_msg, "lcc");
    ASSERT_NE(nullptr, grib_lcc.ds);
    ASSERT_NE(nullptr, grib_lcc.ds->GetSpatialRef());
    EXPECT_TRUE(grib_lcc.ds->GetSpatialRef()->IsProjected());

    {
      CPLConfigOptionSetter southern_no(
          "GRIB_LATITUDE_OF_FIRST_GRID_POINT_IS_SOUTHERN_MOST", "NO", false);
      auto grib_lcc_no = open_and_check(lcc_msg, "lcc_southern_no");
      ASSERT_NE(nullptr, grib_lcc_no.ds);
      ASSERT_NE(nullptr, grib_lcc_no.ds->GetSpatialRef());
      EXPECT_TRUE(grib_lcc_no.ds->GetSpatialRef()->IsProjected());
    }

    // MANAL_2023030103.grb2 heuristic: LCC(60, 30, 30, 140) where (lon1, lat1)
    // is the northern-most point (lat1 = 45, lon1 = 135) with large grid.
    OGRSpatialReference lcc_manal;
    lcc_manal.SetLCC(60.0, 30.0, 30.0, 140.0, 0.0, 0.0);
    lcc_manal.SetWellKnownGeogCS("WGS84");
    std::vector<GByte> manal_msg = MakeSimpleGrib2Message(
        10, 10, -500000.0, 150000.0, 1500000.0, 150000.0, {}, &lcc_manal);
    const size_t s3_manal = FindGrib2SectionOffset(manal_msg, 3);
    ASSERT_NE(std::string::npos, s3_manal);
    WriteS32BE(&manal_msg[s3_manal + 38], 45000000);   // lat1 = 45.0 N
    WriteS32BE(&manal_msg[s3_manal + 42], 135000000);  // lon1 = 135.0 E
    CPLErrorReset();
    auto grib_manal = open_and_check(manal_msg, "lcc_manal");
    ASSERT_NE(nullptr, grib_manal.ds);
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Likely buggy grid registration"));

    OGRSpatialReference aea;
    aea.SetACEA(29.5, 45.5, 23.0, -96.0, 0.0, 0.0);
    aea.SetWellKnownGeogCS("WGS84");
    auto grib_aea = open_and_check(
        MakeSimpleGrib2Message(4, 2, 0.0, 10000.0, 100000.0, 10000.0, {}, &aea),
        "albers");
    ASSERT_NE(nullptr, grib_aea.ds);
    ASSERT_NE(nullptr, grib_aea.ds->GetSpatialRef());
    EXPECT_TRUE(grib_aea.ds->GetSpatialRef()->IsProjected());

    OGRSpatialReference laea;
    laea.SetLAEA(45.0, -100.0, 0.0, 0.0);
    laea.SetWellKnownGeogCS("WGS84");
    auto grib_laea =
        open_and_check(MakeSimpleGrib2Message(4, 2, 0.0, 10000.0, 100000.0,
                                              10000.0, {}, &laea),
                       "laea");
    ASSERT_NE(nullptr, grib_laea.ds);
    ASSERT_NE(nullptr, grib_laea.ds->GetSpatialRef());
    EXPECT_TRUE(grib_laea.ds->GetSpatialRef()->IsProjected());
  }

  // 7. Orthographic / GEOS (template 90, 80-byte Section 3), Equator Equidist
  // (template 110, 62-byte Section 3), and Azimuth-Range (template 120,
  // 39-byte Section 3).
  {
    std::vector<GByte> base_msg =
        MakeSimpleGrib2Message(4, 2, 0.0, 1.0, 10.0, 1.0);
    ASSERT_FALSE(base_msg.empty());
    const size_t s3 = FindGrib2SectionOffset(base_msg, 3);
    const size_t s4 = FindGrib2SectionOffset(base_msg, 4);
    ASSERT_NE(std::string::npos, s3);
    ASSERT_NE(std::string::npos, s4);

    auto replace_s3 = [&](const std::vector<GByte>& new_s3,
                          std::string_view tag) {
      std::vector<GByte> m = base_msg;
      m.erase(m.begin() + s3, m.begin() + s4);
      m.insert(m.begin() + s3, new_s3.begin(), new_s3.end());
      UpdateGrib2MessageLength(m);
      return open_and_check(m, tag);
    };

    // Template 90 (80 bytes):
    {
      std::vector<GByte> s3_90(80, 0);
      WriteU32BE(&s3_90[0], 80);
      s3_90[4] = 3;
      WriteU32BE(&s3_90[6], 8);    // numPts = 4 * 2
      WriteU16BE(&s3_90[12], 90);  // GS3_ORTHOGRAPHIC
      s3_90[14] = 6;               // sphere 6371.229 km
      WriteU32BE(&s3_90[30], 4);   // Nx = 4
      WriteU32BE(&s3_90[34], 2);   // Ny = 2
      auto grib_90 = replace_s3(s3_90, "ortho_90");
      ASSERT_NE(nullptr, grib_90.ds);
      GDALGeoTransform gt;
      ASSERT_EQ(CE_None, grib_90.ds->GetGeoTransform(gt));
      EXPECT_NEAR(-11137496.552 / 2.0 - 11137496.552 / 8.0, gt[0], 1.0);
    }

    // Template 110 (62 bytes):
    {
      std::vector<GByte> s3_110(62, 0);
      WriteU32BE(&s3_110[0], 62);
      s3_110[4] = 3;
      WriteU32BE(&s3_110[6], 8);
      WriteU16BE(&s3_110[12], 110);  // GS3_EQUATOR_EQUIDIST
      s3_110[14] = 6;
      WriteU32BE(&s3_110[30], 4);
      WriteU32BE(&s3_110[34], 2);
      auto grib_110 = replace_s3(s3_110, "eq_equidist_110");
      ASSERT_NE(nullptr, grib_110.ds);
      ASSERT_NE(nullptr, grib_110.ds->GetSpatialRef());
      EXPECT_FALSE(grib_110.ds->GetSpatialRef()->IsProjected());
    }

    // Template 120 (39 bytes, Nr = 0, is3[30] = Nx = 4, is3[34] = Ny = 2):
    {
      std::vector<GByte> s3_120(39, 0);
      WriteU32BE(&s3_120[0], 39);
      s3_120[4] = 3;
      WriteU32BE(&s3_120[6], 8);
      WriteU16BE(&s3_120[12], 120);  // GS3_AZIMUTH_RANGE
      WriteU32BE(&s3_120[18], 0);    // Nr = 0 -> extlen = 0
      WriteU32BE(&s3_120[30], 4);    // Nx = 4
      WriteU32BE(&s3_120[34], 2);    // Ny = 2
      auto grib_120 = replace_s3(s3_120, "azimuth_120");
      ASSERT_NE(nullptr, grib_120.ds);
      GDALGeoTransform gt_120;
      ASSERT_EQ(CE_None, grib_120.ds->GetGeoTransform(gt_120));
      EXPECT_DOUBLE_EQ(0.0, gt_120[1]);
    }
  }
}

TEST_F(GribDatasetTest, MultidimGroupsArraysAndRead) {
  // 1. OpenMultiDim on GRIB1 file (exercises GRIB1 multidim + missing GRIB2
  // optional metadata attributes).
  {
    std::vector<GByte> g1 = MakeGrib1Message(/*p1_hours=*/0, /*oblate=*/false);
    ScopedVsiMemFile f("grib_multidim_grib1.grb");
    WriteVsiBytes(f.path(), g1);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(f.path(), GDAL_OF_MULTIDIM_RASTER));
    ASSERT_NE(nullptr, ds);
    auto root = ds->GetRootGroup();
    ASSERT_NE(nullptr, root);
    EXPECT_THAT(root->GetMDArrayNames(nullptr), Contains("TMP_2-HTGL"));
  }

  // 2. Multi-band GRIB2 file with leading garbage, 3D arrays (both varying
  // foreSec and varying refTime), shared dimensions, X-origin mismatch,
  // Y-origin mismatch, different TIME dim (TIME2), duplicate array name (_2),
  // NoData array, and projected CRS array.
  std::vector<GByte> stream(24, ' ');  // Leading garbage for OpenMultiDim scan.
  auto append_msg = [&](const std::vector<GByte>& m) {
    ASSERT_FALSE(m.empty());
    stream.insert(stream.end(), m.begin(), m.end());
  };

  // TMP at 2m: INPUT_UNIT=K so MakeSimpleGrib2Message values (273.15 + ...)
  // round-trip as Kelvin and convert to Celsius (17 C south, 0 C north);
  // refTime 2025-01-01T00:00:00Z, foreSec = 0, 6h, 12h -> 3D array creating
  // Y(2), X(4), TIME(3).
  for (int fcst : {0, 6, 12}) {
    append_msg(MakeSimpleGrib2Message(
        4, 2, 0.0, 1.0, 10.0, 1.0,
        std::to_array<std::string>(
            {"INPUT_UNIT=K", "IDS_CENTER=7", "IDS_SUBCENTER=1",
             "IDS_SIGNF_REF_TIME=1", "IDS_REF_TIME=2025-01-01T00:00:00Z",
             "IDS_PROD_STATUS=0", "IDS_TYPE=1", "PDS_PDTN=0",
             "PDS_TEMPLATE_ASSEMBLED_VALUES=" + MakePds0Values(0, 0, fcst)})));
  }

  // RH at 2m: same grid, same refTime & foreSec -> reuses Y, X, TIME.
  for (int fcst : {0, 6, 12}) {
    append_msg(MakeSimpleGrib2Message(
        4, 2, 0.0, 1.0, 10.0, 1.0,
        std::to_array<std::string>(
            {"IDS_REF_TIME=2025-01-01T00:00:00Z", "PDS_PDTN=0",
             "PDS_TEMPLATE_ASSEMBLED_VALUES=" + MakePds0Values(1, 1, fcst)})));
  }

  // UGRD at 10m: same foreSec (0), different refTime (00Z vs 06Z) -> hits
  // `dfRefTime != psInv->refTime && dfForecastTime == psInv->foreSec` and
  // creates TIME2 (size 2).
  append_msg(MakeSimpleGrib2Message(
      4, 2, 0.0, 1.0, 10.0, 1.0,
      std::to_array<std::string>({"IDS_REF_TIME=2025-01-01T00:00:00Z",
                                  "PDS_PDTN=0",
                                  "PDS_TEMPLATE_ASSEMBLED_VALUES=" +
                                      MakePds0Values(2, 2, 0, 103, 10)})));
  append_msg(MakeSimpleGrib2Message(
      4, 2, 0.0, 1.0, 10.0, 1.0,
      std::to_array<std::string>({"IDS_REF_TIME=2025-01-01T06:00:00Z",
                                  "PDS_PDTN=0",
                                  "PDS_TEMPLATE_ASSEMBLED_VALUES=" +
                                      MakePds0Values(2, 2, 0, 103, 10)})));

  // VGRD at 10m ([m/s], no Kelvin conversion): same 4x2 size, same X origin
  // (0.0), DIFFERENT Y origin (20.0) -> hits bOK=true on X, bOK=false on Y ->
  // creates Y2, X2.
  append_msg(MakeSimpleGrib2Message(
      4, 2, 0.0, 1.0, 20.0, 1.0,
      std::to_array<std::string>({"IDS_REF_TIME=2025-01-01T00:00:00Z",
                                  "PDS_PDTN=0",
                                  "PDS_TEMPLATE_ASSEMBLED_VALUES=" +
                                      MakePds0Values(2, 3, 0, 103, 10)})));

  // PRES at surface: same 4x2 size, DIFFERENT X origin (5.0) -> hits bOK=false
  // on X -> creates Y3, X3.
  append_msg(MakeSimpleGrib2Message(
      4, 2, 5.0, 1.0, 10.0, 1.0,
      std::to_array<std::string>(
          {"IDS_REF_TIME=2025-01-01T00:00:00Z", "PDS_PDTN=0",
           "PDS_TEMPLATE_ASSEMBLED_VALUES=" + MakePds0Values(3, 0, 0, 1, 0)})));

  // Duplicate TMP at 2m with BOTH different refTime and different foreSec ->
  // creates TMP_2-HTGL_2 (2D array).
  append_msg(MakeSimpleGrib2Message(
      4, 2, 0.0, 1.0, 10.0, 1.0,
      std::to_array<std::string>(
          {"INPUT_UNIT=K", "IDS_REF_TIME=2025-01-02T00:00:00Z", "PDS_PDTN=0",
           "PDS_TEMPLATE_ASSEMBLED_VALUES=" + MakePds0Values(0, 0, 24)})));

  // Complex-packed variable with NoData (8x8) -> GetRawNoDataValue() != null.
  append_msg(MakeSimpleGrib2Message(
      8, 8, 0.0, 1.0, 10.0, 1.0,
      std::to_array<std::string>(
          {"DATA_ENCODING=COMPLEX_PACKING", "PDS_PDTN=0",
           "PDS_TEMPLATE_ASSEMBLED_VALUES=" + MakePds0Values(0, 2, 0)}),
      nullptr, GDT_Float32, -9999.0, /*has_nodata=*/true));

  // Projected LCC variable -> non-{2,1} axis mapping branch in Init/Finalize.
  OGRSpatialReference lcc;
  lcc.SetLCC(30.0, 60.0, 40.0, -95.0, 0.0, 0.0);
  lcc.SetWellKnownGeogCS("WGS84");
  for (int fcst : {0, 6}) {
    append_msg(MakeSimpleGrib2Message(
        4, 2, 0.0, 10000.0, 100000.0, 10000.0,
        std::to_array<std::string>(
            {"INPUT_UNIT=K", "IDS_REF_TIME=2025-01-01T00:00:00Z", "PDS_PDTN=0",
             "PDS_TEMPLATE_ASSEMBLED_VALUES=" + MakePds0Values(0, 6, fcst)}),
        &lcc));
  }

  ScopedVsiMemFile md_file("grib_multidim_full.grb2");
  WriteVsiBytes(md_file.path(), stream);

  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(md_file.path(), GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(nullptr, ds);
  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);

  const std::vector<std::string> names = root->GetMDArrayNames(nullptr);
  EXPECT_THAT(names, Contains("TMP_2-HTGL"));
  EXPECT_THAT(names, Contains("RH_2-HTGL"));
  EXPECT_THAT(names, Contains("UGRD_10-HTGL"));
  EXPECT_THAT(names, Contains("VGRD_10-HTGL"));
  EXPECT_THAT(names, Contains("PRES_0-SFC"));
  EXPECT_THAT(names, Contains("TMP_2-HTGL_2"));

  EXPECT_EQ(nullptr, root->OpenMDArray("DOES_NOT_EXIST", nullptr));
  EXPECT_GE(root->GetDimensions(nullptr).size(), 8U);

  // Verify 3D array metadata, unit stripping ("[C]" -> "C"), SRS, and Read()
  // (both direct Float64 and strided/converting Float32).
  auto tmp_arr = root->OpenMDArray("TMP_2-HTGL", nullptr);
  ASSERT_NE(nullptr, tmp_arr);
  EXPECT_FALSE(tmp_arr->IsWritable());
  EXPECT_EQ(md_file.path(), tmp_arr->GetFilename());
  EXPECT_EQ(3U, tmp_arr->GetDimensions().size());
  EXPECT_EQ(GDT_Float64, tmp_arr->GetDataType().GetNumericDataType());
  EXPECT_NE(nullptr, tmp_arr->GetSpatialRef());
  EXPECT_EQ("C", tmp_arr->GetUnit());
  EXPECT_EQ(nullptr, tmp_arr->GetRawNoDataValue());
  EXPECT_THAT(tmp_arr->GetAttributes(nullptr), Not(IsEmpty()));

  {
    constexpr auto kStart = std::to_array<GUInt64>({0, 0, 0});
    constexpr auto kCount = std::to_array<size_t>({2, 2, 4});
    constexpr auto kStep = std::to_array<GInt64>({1, 1, 1});
    constexpr auto kStride = std::to_array<GPtrDiff_t>({8, 4, 1});
    std::array<double, 16> buf{};
    ASSERT_TRUE(tmp_arr->Read(
        kStart.data(), kCount.data(), kStep.data(), kStride.data(),
        GDALExtendedDataType::Create(GDT_Float64), buf.data()));
    // Multidim LoadData strips brackets on GetUnit() ("C"), preserves
    // south-to-north row scan order (row 0 = south = 290.15 K -> 17.0 C,
    // row 1 = north = 273.15 K -> 0.0 C), and converts Kelvin to Celsius.
    EXPECT_NEAR(17.0, buf[0], 1e-2);
    EXPECT_NEAR(0.0, buf[4], 1e-2);
    EXPECT_NEAR(17.0, buf[8], 1e-2);

    // Strided + Float32 conversion path in 3D IRead.
    constexpr auto kCountStrided = std::to_array<size_t>({1, 2, 2});
    constexpr auto kStepStrided = std::to_array<GInt64>({1, 1, 2});
    constexpr auto kStrideStrided = std::to_array<GPtrDiff_t>({4, 2, 1});
    std::array<float, 4> fbuf{};
    ASSERT_TRUE(tmp_arr->Read(kStart.data(), kCountStrided.data(),
                              kStepStrided.data(), kStrideStrided.data(),
                              GDALExtendedDataType::Create(GDT_Float32),
                              fbuf.data()));
    EXPECT_NEAR(17.0f, fbuf[0], 1e-2f);
    EXPECT_NEAR(55.5f, fbuf[1], 1e-2f);
    EXPECT_NEAR(0.0f, fbuf[2], 1e-2f);
    EXPECT_NEAR(2.5f, fbuf[3], 1e-2f);
  }

  // Verify 2D array (VGRD) direct Float64 Read (twice for cache hit) and
  // strided/converting Float32 Read.
  auto vgrd_arr = root->OpenMDArray("VGRD_10-HTGL", nullptr);
  ASSERT_NE(nullptr, vgrd_arr);
  EXPECT_EQ(2U, vgrd_arr->GetDimensions().size());
  {
    constexpr auto kStart = std::to_array<GUInt64>({0, 0});
    constexpr auto kCount = std::to_array<size_t>({2, 4});
    constexpr auto kStep = std::to_array<GInt64>({1, 1});
    constexpr auto kStride = std::to_array<GPtrDiff_t>({4, 1});
    std::array<double, 8> buf{};
    ASSERT_TRUE(vgrd_arr->Read(
        kStart.data(), kCount.data(), kStep.data(), kStride.data(),
        GDALExtendedDataType::Create(GDT_Float64), buf.data()));
    EXPECT_NEAR(290.15, buf[0], 1e-2);
    EXPECT_NEAR(273.15, buf[4], 1e-2);

    // Second read at same offset hits GRIBSharedResource::LoadData cache.
    std::array<double, 8> cached_buf{};
    ASSERT_TRUE(vgrd_arr->Read(
        kStart.data(), kCount.data(), kStep.data(), kStride.data(),
        GDALExtendedDataType::Create(GDT_Float64), cached_buf.data()));
    EXPECT_EQ(buf, cached_buf);

    constexpr auto kCountStrided = std::to_array<size_t>({2, 2});
    constexpr auto kStepStrided = std::to_array<GInt64>({1, 2});
    constexpr auto kStrideStrided = std::to_array<GPtrDiff_t>({2, 1});
    std::array<float, 4> fbuf{};
    ASSERT_TRUE(vgrd_arr->Read(kStart.data(), kCountStrided.data(),
                               kStepStrided.data(), kStrideStrided.data(),
                               GDALExtendedDataType::Create(GDT_Float32),
                               fbuf.data()));
    EXPECT_NEAR(290.15f, fbuf[0], 1e-2f);
    EXPECT_NEAR(328.65f, fbuf[1], 1e-2f);
    EXPECT_NEAR(273.15f, fbuf[2], 1e-2f);
    EXPECT_NEAR(275.65f, fbuf[3], 1e-2f);
  }

  // Verify NoData array exposes non-null GetRawNoDataValue().
  auto nodata_arr = root->OpenMDArray("POT_2-HTGL", nullptr);
  ASSERT_NE(nullptr, nodata_arr);
  ASSERT_NE(nullptr, nodata_arr->GetRawNoDataValue());
  double raw_nd = 0.0;
  std::memcpy(&raw_nd, nodata_arr->GetRawNoDataValue(), sizeof(double));
  EXPECT_NEAR(-9999.0, raw_nd, 1.0);
}

// TODO: b/335317901 - Cover remaining branches in gribdataset.cpp
// (ConvertUnitInText nullptr input, Lon360to180(180), truncated section reads
// in FindPDSTemplateGRIB2 and FindNoDataGrib2, LoadData out-of-memory and
// zero-dimension band paths, GRIBSharedResource::LoadData error paths, and
// GDALDeregister_GRIB cleanup).

}  // namespace
}  // namespace autotest2
