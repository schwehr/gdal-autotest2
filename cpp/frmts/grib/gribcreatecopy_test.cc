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
// Tests for gribcreatecopy (gribcreatecopy.cpp).
// Note: google3 builds GDAL with WITHOUT_CPLDEBUG and without g2clib USE_PNG
// (DATA_ENCODING=PNG writes Template 5.41 via the PNG driver, while reading
// pixel data from Template 5.41 returns CE_Failure).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/grib/gribcreatecopy.cpp
//   https://gdal.org/drivers/raster/grib.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/grib.py

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>  // NOLINT
#include <limits>
#include <memory>
#include <span>  // NOLINT
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/matchers.h"
#include "third_party/gdal/frmts/grib/gribdataset.h"
#include "third_party/gdal/frmts/mem/memdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_geotransform.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::AllOf;
using ::testing::Contains;
using ::testing::HasSubstr;

constexpr char kVsiDir[] = "/vsimem/gribcreatecopy_test";

class ScopedVsiMemFile {
 public:
  explicit ScopedVsiMemFile(std::string_view filename)
      : path_(std::format("{}/{}", kVsiDir, filename)) {}
  ScopedVsiMemFile(const ScopedVsiMemFile&) = delete;
  ScopedVsiMemFile& operator=(const ScopedVsiMemFile&) = delete;
  ~ScopedVsiMemFile() {
    VSIUnlink(path_.c_str());
    VSIUnlink((path_ + ".aux.xml").c_str());
    VSIUnlink((path_ + ".idx").c_str());
  }

  const char* path() const { return path_.c_str(); }

 private:
  std::string path_;
};

// Collects all error/warning messages emitted across a call that produces
// multiple warnings before completing, resetting error state on exit.
class ScopedErrorCollector {
 public:
  ScopedErrorCollector() {
    CPLErrorReset();
    CPLPushErrorHandlerEx(&ScopedErrorCollector::Handle, this);
  }
  ~ScopedErrorCollector() {
    CPLPopErrorHandler();
    CPLErrorReset();
  }

  const std::vector<std::string>& messages() const { return messages_; }
  void Clear() {
    messages_.clear();
    CPLErrorReset();
  }

 private:
  static void CPL_STDCALL Handle(CPLErr /*eErrClass*/, CPLErrorNum /*err_no*/,
                                 const char* msg) {
    auto* self =
        static_cast<ScopedErrorCollector*>(CPLGetErrorHandlerUserData());
    if (self != nullptr && msg != nullptr) {
      self->messages_.emplace_back(msg);
    }
  }

  std::vector<std::string> messages_;
};

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

std::vector<float> ReadBandAsFloat(GDALDataset* ds, int band) {
  const int xsize = ds->GetRasterXSize();
  const int ysize = ds->GetRasterYSize();
  std::vector<float> out(static_cast<size_t>(xsize) * ysize,
                         std::numeric_limits<float>::quiet_NaN());
  EXPECT_EQ(CE_None, ds->GetRasterBand(band)->RasterIO(
                         GF_Read, 0, 0, xsize, ysize, out.data(), xsize, ysize,
                         GDT_Float32, 0, 0, nullptr));
  return out;
}

// Custom dataset/band for testing edge cases (dimension overflow, missing GT,
// IRasterIO failures, out-of-range integer values, huge scaled min values).
class CustomTestDataset;

class CustomTestBand final : public GDALRasterBand {
 public:
  CustomTestBand(CustomTestDataset* ds, int band, int xsize, int ysize,
                 GDALDataType dt);

  CPLErr IReadBlock(int nBlockXOff, int nBlockYOff, void* pData) override;
  CPLErr IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize,
                   int nYSize, void* pData, int nBufXSize, int nBufYSize,
                   GDALDataType eBufType, GSpacing nPixelSpace,
                   GSpacing nLineSpace,
                   GDALRasterIOExtraArg* psExtraArg) override;
  double GetNoDataValue(int* pbSuccess = nullptr) override;

  bool fail_nodata_scan = false;
  bool fail_first_read = false;
  bool fail_second_read = false;
  bool return_garbage_range = false;
  bool has_nodata = false;
  double nodata_value = -9999.0;
  double fill_value = 42.0;
  int read_calls = 0;
};

class CustomTestDataset final : public GDALDataset {
 public:
  CustomTestDataset(int xsize, int ysize, GDALDataType dt) {
    nRasterXSize = xsize;
    nRasterYSize = ysize;
    srs_.SetWellKnownGeogCS("WGS84");
    SetBand(1, new CustomTestBand(this, 1, xsize, ysize, dt));
  }

  CustomTestBand* custom_band() {
    return static_cast<CustomTestBand*>(GetRasterBand(1));
  }

  CPLErr GetGeoTransform(GDALGeoTransform& gt_out) const override {
    if (!has_gt) {
      return CE_Failure;
    }
    gt_out = gt;
    return CE_None;
  }

  const OGRSpatialReference* GetSpatialRef() const override {
    return has_srs ? &srs_ : nullptr;
  }

  bool has_gt = true;
  bool has_srs = true;
  GDALGeoTransform gt{0.0, 1.0, 0.0, 10.0, 0.0, -1.0};

 private:
  OGRSpatialReference srs_;
};

CustomTestBand::CustomTestBand(CustomTestDataset* ds, int band, int xsize,
                               int ysize, GDALDataType dt) {
  poDS = ds;
  nBand = band;
  nRasterXSize = xsize;
  nRasterYSize = ysize;
  eDataType = dt;
  nBlockXSize = xsize;
  nBlockYSize = 1;
}

CPLErr CustomTestBand::IReadBlock(int /*nBlockXOff*/, int /*nBlockYOff*/,
                                  void* /*pData*/) {
  return CE_Failure;
}

CPLErr CustomTestBand::IRasterIO(GDALRWFlag /*eRWFlag*/, int nXOff,
                                 int /*nYOff*/, int /*nXSize*/, int nYSize,
                                 void* pData, int nBufXSize, int nBufYSize,
                                 GDALDataType eBufType, GSpacing nPixelSpace,
                                 GSpacing nLineSpace,
                                 GDALRasterIOExtraArg* /*psExtraArg*/) {
  ++read_calls;
  if (eBufType != GDT_Float32 && eBufType != GDT_Float64) {
    CPLError(CE_Failure, CPLE_AppDefined, "Unsupported test buffer type");
    return CE_Failure;
  }
  if (fail_nodata_scan && eBufType == GDT_Float64 && nYSize == 1) {
    CPLError(CE_Failure, CPLE_AppDefined, "Simulated nodata scan failure");
    return CE_Failure;
  }
  if (fail_first_read) {
    CPLError(CE_Failure, CPLE_AppDefined, "Simulated first read failure");
    return CE_Failure;
  }
  if (fail_second_read && nXOff == 0) {
    CPLError(CE_Failure, CPLE_AppDefined, "Simulated second read failure");
    return CE_Failure;
  }

  const GSpacing elem_size = GDALGetDataTypeSizeBytes(eBufType);
  const GSpacing pix_stride = (nPixelSpace == 0) ? elem_size : nPixelSpace;
  const GSpacing line_stride =
      (nLineSpace == 0) ? pix_stride * nBufXSize : nLineSpace;

  for (int y = 0; y < nBufYSize; ++y) {
    GByte* row = static_cast<GByte*>(pData) + y * line_stride;
    for (int x = 0; x < nBufXSize; ++x) {
      double val = fill_value;
      if (return_garbage_range) {
        val = (x == 0 && y == 0) ? 0.0 : 1000.0;
      }
      GByte* cell = row + x * pix_stride;
      if (eBufType == GDT_Float32) {
        const float f = static_cast<float>(val);
        std::memcpy(cell, &f, sizeof(float));
      } else {
        std::memcpy(cell, &val, sizeof(double));
      }
    }
  }
  return CE_None;
}

double CustomTestBand::GetNoDataValue(int* pbSuccess) {
  if (pbSuccess != nullptr) {
    *pbSuccess = has_nodata ? 1 : 0;
  }
  return nodata_value;
}

class GribCreateCopyTest : public ::testing::Test {
 protected:
  // GRIBDataset::CreateCopy delegates PNG and JPEG2000 encoding to the PNG and
  // JP2KAK drivers via GDALGetDriverByName(), and GDALGRIBDriver latches its
  // available JPEG2000 driver list once per process in InitializeMetadata().
  static void SetUpTestSuite() {
    GDALRegister_JP2KAK();
    GDALRegister_PNG();
    GDALRegister_GRIB();
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

TEST_F(GribCreateCopyTest, ValidationAndErrorPaths) {
  WithQuietHandler quiet;
  absl::Cleanup reset_err = [] { CPLErrorReset(); };
  ScopedVsiMemFile out("grib_cc_validation.grb2");
  const OGRSpatialReference wgs84 = MakeWGS84();

  // 1. Zero height / >2 billion pixels check.
  {
    CustomTestDataset zero_y(4, 0, GDT_Byte);
    CPLErrorReset();
    EXPECT_EQ(nullptr, GRIBDataset::CreateCopy(out.path(), &zero_y, FALSE,
                                               nullptr, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("more than 2 billion pixels"));

    // Pins upstream gribcreatecopy.cpp quirk: checks XSize > INT_MAX / XSize
    // (XSize squared) rather than YSize when guarding the 2B pixel limit.
    CustomTestDataset huge_x(50000, 1, GDT_Byte);
    CPLErrorReset();
    EXPECT_EQ(nullptr, GRIBDataset::CreateCopy(out.path(), &huge_x, FALSE,
                                               nullptr, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("more than 2 billion pixels"));
  }

  // 2. Missing geotransform.
  {
    CustomTestDataset no_gt(2, 2, GDT_Byte);
    no_gt.has_gt = false;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GRIBDataset::CreateCopy(out.path(), &no_gt, FALSE,
                                               nullptr, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Source dataset must have a geotransform"));
  }

  // 3. Rotated geotransform.
  {
    auto ds = MakeMemDataset(2, 2, 1, GDT_Byte, &wgs84,
                             GDALGeoTransform(0.0, 1.0, 0.1, 10.0, 0.0, -1.0));
    CPLErrorReset();
    EXPECT_EQ(nullptr, GRIBDataset::CreateCopy(out.path(), ds.get(), FALSE,
                                               nullptr, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Geotransform with rotation terms not supported"));
  }

  // 4. Missing SRS and unsupported projections.
  {
    auto no_srs = MakeMemDataset(2, 2, 1, GDT_Byte, nullptr);
    CPLErrorReset();
    EXPECT_EQ(nullptr, GRIBDataset::CreateCopy(out.path(), no_srs.get(), FALSE,
                                               nullptr, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Unsupported or missing spatial reference system"));

    OGRSpatialReference sinus;
    sinus.SetWellKnownGeogCS("WGS84");
    sinus.SetSinusoidal(0.0, 0.0, 0.0);
    auto sinus_ds = MakeMemDataset(2, 2, 1, GDT_Byte, &sinus);
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GRIBDataset::CreateCopy(out.path(), sinus_ds.get(), FALSE,
                                      nullptr, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Unsupported projection: Sinusoidal"));
  }

  // 5. Cannot open output file / APPEND_SUBDATASET on missing file.
  {
    auto ds = MakeMemDataset(2, 2, 1, GDT_Byte, &wgs84);
    CPLStringList opts;
    opts.SetNameValue("APPEND_SUBDATASET", "YES");
    const std::string missing_path =
        std::format("{}/does_not_exist_append.grb2", kVsiDir);
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GRIBDataset::CreateCopy(missing_path.c_str(), ds.get(), FALSE,
                                      opts.List(), nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot create"));
  }

  // 6. Unsupported data type (CFloat32).
  {
    auto ds = MakeMemDataset(2, 2, 1, GDT_CFloat32, &wgs84);
    CPLErrorReset();
    EXPECT_EQ(nullptr, GRIBDataset::CreateCopy(out.path(), ds.get(), FALSE,
                                               nullptr, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Unsupported data type: CFloat32"));
  }

  // 7. Incompatible JPEG2000_DRIVER + SPATIAL_DIFFERENCING_ORDER.
  {
    auto ds = MakeMemDataset(2, 2, 1, GDT_Byte, &wgs84);
    CPLStringList opts;
    opts.SetNameValue("JPEG2000_DRIVER", "JP2KAK");
    opts.SetNameValue("SPATIAL_DIFFERENCING_ORDER", "1");
    CPLErrorReset();
    EXPECT_EQ(nullptr, GRIBDataset::CreateCopy(out.path(), ds.get(), FALSE,
                                               opts.List(), nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("JPEG2000_DRIVER and SPATIAL_DIFFERENCING_ORDER are "
                          "not compatible"));
  }

  // 8. Unsupported DATA_ENCODING and SPATIAL_DIFFERENCING_ORDER.
  {
    auto ds = MakeMemDataset(2, 2, 1, GDT_Byte, &wgs84);
    CPLStringList bad_enc;
    bad_enc.SetNameValue("DATA_ENCODING", "BOGUS");
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GRIBDataset::CreateCopy(out.path(), ds.get(), FALSE,
                                      bad_enc.List(), nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Unsupported DATA_ENCODING=BOGUS"));

    CPLStringList bad_sdo;
    bad_sdo.SetNameValue("DATA_ENCODING", "COMPLEX_PACKING");
    bad_sdo.SetNameValue("SPATIAL_DIFFERENCING_ORDER", "3");
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GRIBDataset::CreateCopy(out.path(), ds.get(), FALSE,
                                      bad_sdo.List(), nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Unsupported value for SPATIAL_DIFFERENCING_ORDER"));
  }

  // 9. Non-finite values (NaN / Inf) fail in non-IEEE encodings, succeed in
  // IEEE.
  {
    auto ds = MakeMemDataset(2, 2, 1, GDT_Float32, &wgs84);
    const std::array<float, 4> vals = {
        1.0f, std::numeric_limits<float>::quiet_NaN(), 2.0f,
        std::numeric_limits<float>::infinity()};
    FillBand<float>(ds.get(), 1, vals, GDT_Float32);

    CPLStringList simple_opts;
    simple_opts.SetNameValue("DATA_ENCODING", "SIMPLE_PACKING");
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GRIBDataset::CreateCopy(out.path(), ds.get(), FALSE,
                                      simple_opts.List(), nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Non-finite values not supported"));

    CPLErrorReset();
    GDALDatasetUniquePtr ieee_ds(GRIBDataset::CreateCopy(
        out.path(), ds.get(), FALSE, nullptr, nullptr, nullptr));
    EXPECT_EQ(CE_None, CPLGetLastErrorType());
    ASSERT_NE(nullptr, ieee_ds);
  }

  // 10. Progress callback cancellation in WriteIEEE and at end of CreateCopy.
  {
    auto ds = MakeMemDataset(2, 2, 1, GDT_Float32, &wgs84);
    constexpr auto kVals = std::to_array<float>({1.0f, 2.0f, 3.0f, 4.0f});
    FillBand<float>(ds.get(), 1, kVals, GDT_Float32);

    auto cancel_immediately =
        [](double /*dfComplete*/, const char* /*pszMessage*/,
           void* /*pProgressArg*/) -> int { return FALSE; };
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GRIBDataset::CreateCopy(out.path(), ds.get(), FALSE, nullptr,
                                      cancel_immediately, nullptr));
    EXPECT_STREQ("", CPLGetLastErrorMsg());
    EXPECT_EQ(CE_None, CPLGetLastErrorType());

    auto cancel_at_end = [](double dfComplete, const char* /*pszMessage*/,
                            void* /*pProgressArg*/) -> int {
      return dfComplete < 0.99 ? TRUE : FALSE;
    };
    CPLStringList simple_opts;
    simple_opts.SetNameValue("DATA_ENCODING", "SIMPLE_PACKING");
    CPLErrorReset();
    EXPECT_EQ(nullptr, GRIBDataset::CreateCopy(out.path(), ds.get(), FALSE,
                                               simple_opts.List(),
                                               cancel_at_end, nullptr));
    EXPECT_STREQ("", CPLGetLastErrorMsg());
    EXPECT_EQ(CE_None, CPLGetLastErrorType());
  }
}

TEST_F(GribCreateCopyTest, CustomDatasetEdgeCasesAndIOFailures) {
  WithQuietHandler quiet;
  absl::Cleanup reset_err = [] { CPLErrorReset(); };
  ScopedVsiMemFile out("grib_cc_custom.grb2");

  // 1. Garbage range on integer dataset.
  {
    CustomTestDataset ds(2, 2, GDT_Byte);
    ds.custom_band()->return_garbage_range = true;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GRIBDataset::CreateCopy(out.path(), &ds, FALSE, nullptr,
                                               nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        HasSubstr("Garbage values found when requesting input dataset"));
  }

  // 2. Scaled min value overflowing float32.
  {
    CustomTestDataset ds(2, 2, GDT_Float64);
    ds.custom_band()->fill_value = -1e38;
    CPLStringList opts;
    opts.SetNameValue("DATA_ENCODING", "SIMPLE_PACKING");
    opts.SetNameValue("DECIMAL_SCALE_FACTOR", "10");
    CPLErrorReset();
    EXPECT_EQ(nullptr, GRIBDataset::CreateCopy(out.path(), &ds, FALSE,
                                               opts.List(), nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Scaled min value not representable on "
                          "IEEE754 single precision float"));
  }

  // 3. IRasterIO failures in GetFloatData (first & second read, both
  // split-and-swap and non-split), nodata scan, and WriteIEEE (first & second
  // read).
  struct IoFailCase {
    std::string_view label;
    int width;
    int height;
    GDALDataType dt;
    bool split_global;
    bool fail_first;
    bool fail_second;
    bool fail_nodata;
  };
  constexpr IoFailCase kIoFailCases[] = {
      {.label = "byte_nonsplit_first",
       .width = 4,
       .height = 2,
       .dt = GDT_Byte,
       .split_global = false,
       .fail_first = true,
       .fail_second = false,
       .fail_nodata = false},
      {.label = "byte_split_first",
       .width = 36,
       .height = 2,
       .dt = GDT_Byte,
       .split_global = true,
       .fail_first = true,
       .fail_second = false,
       .fail_nodata = false},
      {.label = "byte_split_second",
       .width = 36,
       .height = 2,
       .dt = GDT_Byte,
       .split_global = true,
       .fail_first = false,
       .fail_second = true,
       .fail_nodata = false},
      {.label = "nodata_scan",
       .width = 2,
       .height = 2,
       .dt = GDT_Byte,
       .split_global = false,
       .fail_first = false,
       .fail_second = false,
       .fail_nodata = true},
      {.label = "ieee_split_first",
       .width = 36,
       .height = 2,
       .dt = GDT_Float32,
       .split_global = true,
       .fail_first = true,
       .fail_second = false,
       .fail_nodata = false},
      {.label = "ieee_split_second",
       .width = 36,
       .height = 2,
       .dt = GDT_Float32,
       .split_global = true,
       .fail_first = false,
       .fail_second = true,
       .fail_nodata = false},
  };
  for (const auto& tc : kIoFailCases) {
    SCOPED_TRACE(tc.label);
    CustomTestDataset ds(tc.width, tc.height, tc.dt);
    if (tc.split_global) {
      ds.gt = GDALGeoTransform(-180.0, 10.0, 0.0, 10.0, 0.0, -10.0);
    }
    if (tc.fail_first) {
      ds.custom_band()->fail_first_read = true;
    }
    if (tc.fail_second) {
      ds.custom_band()->fail_second_read = true;
    }
    if (tc.fail_nodata) {
      ds.custom_band()->has_nodata = true;
      ds.custom_band()->fail_nodata_scan = true;
    }
    CPLErrorReset();
    EXPECT_EQ(nullptr, GRIBDataset::CreateCopy(out.path(), &ds, FALSE, nullptr,
                                               nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 4. VSIFWriteL failures on non-growable VSIFileFromMemBuffer across all 5
  // data encodings (use 8x8 non-uniform grid so complex packing reaches write).
  const OGRSpatialReference wgs84 = MakeWGS84();
  auto src = MakeMemDataset(8, 8, 1, GDT_Byte, &wgs84);
  std::vector<GByte> vals(64);
  for (int i = 0; i < 64; ++i) {
    vals[i] = static_cast<GByte>((i * i * i) % 250 + 1);
  }
  FillBand<GByte>(src.get(), 1, vals, GDT_Byte);

  constexpr auto kEncodings = std::to_array<std::string_view>(
      {"SIMPLE_PACKING", "COMPLEX_PACKING", "IEEE_FLOATING_POINT", "PNG",
       "JPEG2000"});
  for (std::string_view encoding : kEncodings) {
    ScopedVsiMemFile fixed_file("grib_cc_fixed_zero.grb2");
    VSILFILE* fp = VSIFileFromMemBuffer(fixed_file.path(), nullptr, 0, FALSE);
    ASSERT_NE(nullptr, fp);
    ASSERT_EQ(0, VSIFCloseL(fp));

    CPLStringList opts;
    opts.SetNameValue("APPEND_SUBDATASET", "YES");
    opts.SetNameValue("DATA_ENCODING", std::string(encoding).c_str());
    CPLErrorReset();
    GDALDatasetUniquePtr fail_ds(GRIBDataset::CreateCopy(
        fixed_file.path(), src.get(), FALSE, opts.List(), nullptr, nullptr));
    EXPECT_EQ(nullptr, fail_ds)
        << "Expected write failure for encoding " << encoding;
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(GribCreateCopyTest, GeographicAndEllipsoidGrids) {
  constexpr auto kVals = std::to_array<GByte>({1, 2, 3, 4, 5, 6, 7, 8});
  const std::vector<float> expected_vals = {1.f, 2.f, 3.f, 4.f,
                                            5.f, 6.f, 7.f, 8.f};

  // 1. WGS84 (code 5), GRS80 (code 4), Sphere (code 1), Oblate spheroid (code
  // 7).
  {
    OGRSpatialReference grs80;
    grs80.SetWellKnownGeogCS("NAD83");
    auto ds = MakeMemDataset(4, 2, 1, GDT_Byte, &grs80);
    FillBand<GByte>(ds.get(), 1, kVals, GDT_Byte);
    ScopedVsiMemFile out("grib_cc_grs80.grb2");
    GDALDatasetUniquePtr grib_ds(GRIBDataset::CreateCopy(
        out.path(), ds.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, grib_ds);
    ASSERT_NE(nullptr, grib_ds->GetSpatialRef());
    EXPECT_NEAR(298.257222101, grib_ds->GetSpatialRef()->GetInvFlattening(),
                1e-6);
    EXPECT_EQ(expected_vals, ReadBandAsFloat(grib_ds.get(), 1));
  }
  {
    OGRSpatialReference sphere;
    sphere.importFromProj4("+proj=longlat +a=6371229 +b=6371229 +no_defs");
    auto ds = MakeMemDataset(4, 2, 1, GDT_Byte, &sphere);
    FillBand<GByte>(ds.get(), 1, kVals, GDT_Byte);
    ScopedVsiMemFile out("grib_cc_sphere.grb2");
    GDALDatasetUniquePtr grib_ds(GRIBDataset::CreateCopy(
        out.path(), ds.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, grib_ds);
    ASSERT_NE(nullptr, grib_ds->GetSpatialRef());
    EXPECT_NEAR(6371229.0, grib_ds->GetSpatialRef()->GetSemiMajor(), 1.0);
    EXPECT_EQ(expected_vals, ReadBandAsFloat(grib_ds.get(), 1));
  }
  {
    OGRSpatialReference oblate;
    oblate.importFromProj4("+proj=longlat +a=6378000 +rf=300 +no_defs");
    auto ds = MakeMemDataset(4, 2, 1, GDT_Byte, &oblate);
    FillBand<GByte>(ds.get(), 1, kVals, GDT_Byte);
    ScopedVsiMemFile out("grib_cc_oblate.grb2");
    GDALDatasetUniquePtr grib_ds(GRIBDataset::CreateCopy(
        out.path(), ds.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, grib_ds);
    ASSERT_NE(nullptr, grib_ds->GetSpatialRef());
    EXPECT_NEAR(6378000.0, grib_ds->GetSpatialRef()->GetSemiMajor(), 1.0);
    EXPECT_EQ(expected_vals, ReadBandAsFloat(grib_ds.get(), 1));
  }

  const OGRSpatialReference wgs84 = MakeWGS84();

  // 2. Bottom-up geotransform (gt[5] > 0) for both simple packing and IEEE.
  {
    auto ds = MakeMemDataset(4, 2, 1, GDT_Float32, &wgs84,
                             GDALGeoTransform(0.0, 1.0, 0.0, 10.0, 0.0, 1.0));
    constexpr auto kFloatVals =
        std::to_array<float>({1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f});
    FillBand<float>(ds.get(), 1, kFloatVals, GDT_Float32);

    ScopedVsiMemFile out_ieee("grib_cc_bottomup_ieee.grb2");
    GDALDatasetUniquePtr ieee_ds(GRIBDataset::CreateCopy(
        out_ieee.path(), ds.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ieee_ds);
    // Source was bottom-up (row 0 = {1,2,3,4} south, row 1 = {5,6,7,8} north);
    // reopened GRIB2 is normalized to top-down.
    EXPECT_EQ(
        std::vector<float>({5.0f, 6.0f, 7.0f, 8.0f, 1.0f, 2.0f, 3.0f, 4.0f}),
        ReadBandAsFloat(ieee_ds.get(), 1));

    ScopedVsiMemFile out_simple("grib_cc_bottomup_simple.grb2");
    CPLStringList opts;
    opts.SetNameValue("DATA_ENCODING", "SIMPLE_PACKING");
    GDALDatasetUniquePtr simple_ds(GRIBDataset::CreateCopy(
        out_simple.path(), ds.get(), FALSE, opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, simple_ds);
    EXPECT_EQ(
        std::vector<float>({5.0f, 6.0f, 7.0f, 8.0f, 1.0f, 2.0f, 3.0f, 4.0f}),
        ReadBandAsFloat(simple_ds.get(), 1));
  }

  // 3. Global -180..180 longitude wrapping (nSplitAndSwapColumn > 0) for both
  // IEEE Float64 and Simple Packing.
  {
    auto ds =
        MakeMemDataset(36, 2, 1, GDT_Float64, &wgs84,
                       GDALGeoTransform(-180.0, 10.0, 0.0, 10.0, 0.0, -10.0));
    std::vector<double> dvals(72);
    for (int i = 0; i < 72; ++i) {
      dvals[i] = static_cast<double>(i);
    }
    FillBand<double>(ds.get(), 1, dvals, GDT_Float64);

    ScopedVsiMemFile out_ieee64("grib_cc_global_ieee64.grb2");
    GDALDatasetUniquePtr ieee64_ds(GRIBDataset::CreateCopy(
        out_ieee64.path(), ds.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ieee64_ds);
    const std::vector<float> read_back = ReadBandAsFloat(ieee64_ds.get(), 1);
    EXPECT_FLOAT_EQ(0.0f, read_back[0]);
    EXPECT_FLOAT_EQ(18.0f, read_back[18]);

    ScopedVsiMemFile out_simple("grib_cc_global_simple.grb2");
    CPLStringList opts;
    opts.SetNameValue("DATA_ENCODING", "SIMPLE_PACKING");
    GDALDatasetUniquePtr simple_ds(GRIBDataset::CreateCopy(
        out_simple.path(), ds.get(), FALSE, opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, simple_ds);
    const std::vector<float> simple_back = ReadBandAsFloat(simple_ds.get(), 1);
    EXPECT_NEAR(0.0f, simple_back[0], 1e-2f);
    EXPECT_NEAR(18.0f, simple_back[18], 1e-2f);
  }

  // 4. Regional crossing prime meridian (-10..+10), regional purely negative
  // (-120..-100), Lon180to360(180) == 180, and GRIB_ADJUST_LONGITUDE_RANGE=NO.
  {
    auto cross_pm =
        MakeMemDataset(4, 2, 1, GDT_Byte, &wgs84,
                       GDALGeoTransform(-10.0, 5.0, 0.0, 10.0, 0.0, -5.0));
    FillBand<GByte>(cross_pm.get(), 1, kVals, GDT_Byte);
    ScopedVsiMemFile out_pm("grib_cc_cross_pm.grb2");
    GDALDatasetUniquePtr pm_ds(GRIBDataset::CreateCopy(
        out_pm.path(), cross_pm.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, pm_ds);
    EXPECT_EQ(expected_vals, ReadBandAsFloat(pm_ds.get(), 1));

    auto neg_only =
        MakeMemDataset(4, 2, 1, GDT_Byte, &wgs84,
                       GDALGeoTransform(-120.0, 5.0, 0.0, 40.0, 0.0, -5.0));
    FillBand<GByte>(neg_only.get(), 1, kVals, GDT_Byte);
    ScopedVsiMemFile out_neg("grib_cc_neg_only.grb2");
    GDALDatasetUniquePtr neg_ds(GRIBDataset::CreateCopy(
        out_neg.path(), neg_only.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, neg_ds);
    GDALGeoTransform neg_gt;
    ASSERT_EQ(CE_None, neg_ds->GetGeoTransform(neg_gt));
    // GRIB2 stores first cell-center longitude Lo1 = 242.5 E (west edge 240 E),
    // which GRIBDataset normalizes back to -120 W on open.
    EXPECT_NEAR(-120.0, neg_gt[0], 1e-6);
    EXPECT_EQ(expected_vals, ReadBandAsFloat(neg_ds.get(), 1));

    // dfLLX = -90 < 0, dfURX = -135 + 45 + 3*90 = 180 -> hits Lon180to360(180);
    // writer splits at column 1 while reader splits [135..495] at column 2.
    auto hit_180 =
        MakeMemDataset(4, 2, 1, GDT_Byte, &wgs84,
                       GDALGeoTransform(-135.0, 90.0, 0.0, 45.0, 0.0, -45.0));
    FillBand<GByte>(hit_180.get(), 1, kVals, GDT_Byte);
    ScopedVsiMemFile out_180("grib_cc_hit180.grb2");
    GDALDatasetUniquePtr ds_180(GRIBDataset::CreateCopy(
        out_180.path(), hit_180.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds_180);
    EXPECT_EQ(std::vector<float>({4, 1, 2, 3, 8, 5, 6, 7}),
              ReadBandAsFloat(ds_180.get(), 1));

    CPLConfigOptionSetter no_adjust("GRIB_ADJUST_LONGITUDE_RANGE", "NO", false);
    ScopedVsiMemFile out_noadj("grib_cc_noadj.grb2");
    GDALDatasetUniquePtr noadj_ds(GRIBDataset::CreateCopy(
        out_noadj.path(), cross_pm.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, noadj_ds);
    GDALGeoTransform noadj_gt;
    ASSERT_EQ(CE_None, noadj_ds->GetGeoTransform(noadj_gt));
    EXPECT_NEAR(-10.0, noadj_gt[0], 1e-6);
  }
}

TEST_F(GribCreateCopyTest, DerivedGeographicRotatedPoleGrids) {
  WithQuietHandler quiet;
  absl::Cleanup reset_err = [] { CPLErrorReset(); };
  constexpr auto kVals = std::to_array<GByte>({10, 20, 30, 40, 50, 60, 70, 80});
  const std::vector<float> expected_vals = {10.f, 20.f, 30.f, 40.f,
                                            50.f, 60.f, 70.f, 80.f};

  // 1. GRIB convention (positive, global -180..180 wrap, and crossing PM).
  {
    OGRSpatialReference grib_rot;
    grib_rot.SetWellKnownGeogCS("WGS84");
    ASSERT_EQ(OGRERR_NONE,
              grib_rot.SetDerivedGeogCRSWithPoleRotationGRIBConvention(
                  "Rotated GRIB", -30.0, -15.0, 0.0));

    auto pos_ds =
        MakeMemDataset(4, 2, 1, GDT_Byte, &grib_rot,
                       GDALGeoTransform(2.0, 1.0, 0.0, 49.0, 0.0, -1.0));
    FillBand<GByte>(pos_ds.get(), 1, kVals, GDT_Byte);
    ScopedVsiMemFile out1("grib_cc_rot_grib_pos.grb2");
    GDALDatasetUniquePtr ds1(GRIBDataset::CreateCopy(
        out1.path(), pos_ds.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds1);
    ASSERT_NE(nullptr, ds1->GetSpatialRef());
    EXPECT_TRUE(ds1->GetSpatialRef()->IsDerivedGeographic());
    EXPECT_EQ(expected_vals, ReadBandAsFloat(ds1.get(), 1));

    auto cross_ds =
        MakeMemDataset(4, 2, 1, GDT_Byte, &grib_rot,
                       GDALGeoTransform(-10.0, 5.0, 0.0, 10.0, 0.0, -5.0));
    FillBand<GByte>(cross_ds.get(), 1, kVals, GDT_Byte);
    ScopedVsiMemFile out2("grib_cc_rot_grib_cross.grb2");
    GDALDatasetUniquePtr ds2(GRIBDataset::CreateCopy(
        out2.path(), cross_ds.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds2);
    ASSERT_NE(nullptr, ds2->GetSpatialRef());
    EXPECT_EQ(expected_vals, ReadBandAsFloat(ds2.get(), 1));

    auto global_ds =
        MakeMemDataset(36, 2, 1, GDT_Byte, &grib_rot,
                       GDALGeoTransform(-180.0, 10.0, 0.0, 10.0, 0.0, -10.0));
    std::vector<GByte> gvals(72, 5);
    FillBand<GByte>(global_ds.get(), 1, gvals, GDT_Byte);
    ScopedVsiMemFile out3("grib_cc_rot_grib_global.grb2");
    GDALDatasetUniquePtr ds3(GRIBDataset::CreateCopy(
        out3.path(), global_ds.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds3);
    ASSERT_NE(nullptr, ds3->GetSpatialRef());
    EXPECT_FLOAT_EQ(5.0f, ReadBandAsFloat(ds3.get(), 1)[0]);
  }

  // 2. netCDF CF convention.
  {
    OGRSpatialReference cf_rot;
    cf_rot.SetWellKnownGeogCS("WGS84");
    ASSERT_EQ(OGRERR_NONE,
              cf_rot.SetDerivedGeogCRSWithPoleRotationNetCDFCFConvention(
                  "Rotated CF", 30.0, -195.0, 0.0));
    auto ds = MakeMemDataset(4, 2, 1, GDT_Byte, &cf_rot);
    FillBand<GByte>(ds.get(), 1, kVals, GDT_Byte);
    ScopedVsiMemFile out("grib_cc_rot_cf.grb2");
    GDALDatasetUniquePtr out_ds(GRIBDataset::CreateCopy(
        out.path(), ds.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, out_ds);
    ASSERT_NE(nullptr, out_ds->GetSpatialRef());
    EXPECT_TRUE(out_ds->GetSpatialRef()->IsDerivedGeographic());
    EXPECT_EQ(expected_vals, ReadBandAsFloat(out_ds.get(), 1));
  }

  // 3. PROJ ob_tran o_proj=longlat WKT2 convention.
  {
    OGRSpatialReference proj_rot;
    ASSERT_EQ(
        OGRERR_NONE,
        proj_rot.importFromWkt(
            "GEOGCRS[\"unnamed\",BASEGEOGCRS[\"unknown\",DATUM[\"unknown\","
            "ELLIPSOID[\"unknown\",6367470,0,LENGTHUNIT[\"metre\",1,"
            "ID[\"EPSG\",9001]]]],PRIMEM[\"Greenwich\",0,ANGLEUNIT[\"degree\","
            "0.0174532925199433],ID[\"EPSG\",8901]]],DERIVINGCONVERSION["
            "\"unknown\",METHOD[\"PROJ ob_tran o_proj=longlat\"],"
            "PARAMETER[\"lon_0\",-15,ANGLEUNIT[\"degree\",0.0174532925199433,"
            "ID[\"EPSG\",9122]]],PARAMETER[\"o_lon_p\",0,ANGLEUNIT[\"degree\","
            "0.0174532925199433,ID[\"EPSG\",9122]]],PARAMETER[\"o_lat_p\",30,"
            "ANGLEUNIT[\"degree\",0.0174532925199433,ID[\"EPSG\",9122]]]],"
            "CS[ellipsoidal,2],AXIS[\"longitude\",east,ORDER[1],"
            "ANGLEUNIT[\"degree\",0.0174532925199433,ID[\"EPSG\",9122]]],"
            "AXIS[\"latitude\",north,ORDER[2],ANGLEUNIT[\"degree\","
            "0.0174532925199433,ID[\"EPSG\",9122]]]]"));
    auto ds = MakeMemDataset(4, 2, 1, GDT_Byte, &proj_rot);
    FillBand<GByte>(ds.get(), 1, kVals, GDT_Byte);
    ScopedVsiMemFile out("grib_cc_rot_proj.grb2");
    GDALDatasetUniquePtr out_ds(GRIBDataset::CreateCopy(
        out.path(), ds.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, out_ds);
    ASSERT_NE(nullptr, out_ds->GetSpatialRef());
    EXPECT_TRUE(out_ds->GetSpatialRef()->IsDerivedGeographic());
    EXPECT_EQ(expected_vals, ReadBandAsFloat(out_ds.get(), 1));
  }

  // 4. Unsupported DerivedGeographicCRS method.
  {
    OGRSpatialReference bad_rot;
    ASSERT_EQ(
        OGRERR_NONE,
        bad_rot.importFromWkt(
            "GEOGCRS[\"unnamed\",BASEGEOGCRS[\"unknown\",DATUM[\"unknown\","
            "ELLIPSOID[\"unknown\",6367470,0,LENGTHUNIT[\"metre\",1,"
            "ID[\"EPSG\",9001]]]],PRIMEM[\"Greenwich\",0,ANGLEUNIT[\"degree\","
            "0.0174532925199433],ID[\"EPSG\",8901]]],DERIVINGCONVERSION["
            "\"unknown\",METHOD[\"Unsupported_Method\"],"
            "PARAMETER[\"lon_0\",-15,ANGLEUNIT[\"degree\",0.0174532925199433,"
            "ID[\"EPSG\",9122]]]],CS[ellipsoidal,2],AXIS[\"longitude\",east,"
            "ORDER[1],ANGLEUNIT[\"degree\",0.0174532925199433,ID[\"EPSG\",9122]"
            "]],"
            "AXIS[\"latitude\",north,ORDER[2],ANGLEUNIT[\"degree\","
            "0.0174532925199433,ID[\"EPSG\",9122]]]]"));
    auto ds = MakeMemDataset(4, 2, 1, GDT_Byte, &bad_rot);
    ScopedVsiMemFile out("grib_cc_rot_bad.grb2");
    CPLErrorReset();
    EXPECT_EQ(nullptr, GRIBDataset::CreateCopy(out.path(), ds.get(), FALSE,
                                               nullptr, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Unsupported method for DerivedGeographicCRS: "
                          "Unsupported_Method"));
  }
}

TEST_F(GribCreateCopyTest, ProjectedGrids) {
  WithQuietHandler quiet;
  absl::Cleanup reset_err = [] { CPLErrorReset(); };
  constexpr auto kVals = std::to_array<GByte>({1, 2, 3, 4});
  const std::vector<float> expected_vals = {1.f, 2.f, 3.f, 4.f};
  const GDALGeoTransform proj_gt(100000.0, 1000.0, 0.0, 200000.0, 0.0, -1000.0);

  auto verify_proj = [&](std::string_view tag, const OGRSpatialReference& srs,
                         const GDALGeoTransform& gt = GDALGeoTransform(
                             100000.0, 1000.0, 0.0, 200000.0, 0.0, -1000.0),
                         const OGRSpatialReference* expected_srs_override =
                             nullptr) {
    auto ds = MakeMemDataset(2, 2, 1, GDT_Byte, &srs, gt);
    FillBand<GByte>(ds.get(), 1, kVals, GDT_Byte);
    ScopedVsiMemFile out(std::format("grib_cc_proj_{}.grb2", tag));
    CPLErrorReset();
    GDALDatasetUniquePtr grib_ds(GRIBDataset::CreateCopy(
        out.path(), ds.get(), FALSE, nullptr, nullptr, nullptr));
    EXPECT_EQ(CE_None, CPLGetLastErrorType()) << "Unexpected error for " << tag;
    ASSERT_NE(nullptr, grib_ds) << "Failed for projection " << tag;
    ASSERT_NE(nullptr, grib_ds->GetSpatialRef()) << tag;
    const OGRSpatialReference& expected_srs =
        expected_srs_override != nullptr ? *expected_srs_override : srs;
    EXPECT_THAT(*grib_ds->GetSpatialRef(), IsSameAs(expected_srs)) << tag;
    GDALGeoTransform out_gt;
    ASSERT_EQ(CE_None, grib_ds->GetGeoTransform(out_gt)) << tag;
    EXPECT_NEAR(gt[1], out_gt[1], 1.0) << tag;
    EXPECT_NEAR(gt[5], out_gt[5], 1.0) << tag;
    EXPECT_EQ(expected_vals, ReadBandAsFloat(grib_ds.get(), 1)) << tag;
  };

  // 1. Mercator_1SP valid & invalid cases.
  {
    OGRSpatialReference merc1;
    merc1.SetWellKnownGeogCS("WGS84");
    merc1.SetMercator(0.0, 0.0, 1.0, 0.0, 0.0);
    verify_proj("merc1", merc1);

    OGRSpatialReference merc1_bad_cm;
    merc1_bad_cm.SetWellKnownGeogCS("WGS84");
    merc1_bad_cm.SetMercator(0.0, 10.0, 1.0, 0.0, 0.0);
    auto ds_cm = MakeMemDataset(2, 2, 1, GDT_Byte, &merc1_bad_cm, proj_gt);
    ScopedVsiMemFile out_cm("grib_cc_merc1_bad_cm.grb2");
    CPLErrorReset();
    GDALDatasetUniquePtr cm_ds(GRIBDataset::CreateCopy(
        out_cm.path(), ds_cm.get(), FALSE, nullptr, nullptr, nullptr));
    EXPECT_EQ(nullptr, cm_ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        HasSubstr("Mercator_1SP with central_meridian != 0 not supported"));

    OGRSpatialReference merc1_bad_lat0;
    merc1_bad_lat0.SetWellKnownGeogCS("WGS84");
    // Use scale != 1.0 so SetMercator stays Mercator_1SP instead of converting
    // to Mercator_2SP; PROJ rejects exporting lat_0 != 0 for Mercator_1SP.
    merc1_bad_lat0.SetMercator(10.0, 0.0, 0.99, 0.0, 0.0);
    auto ds_lat0 = MakeMemDataset(2, 2, 1, GDT_Byte, &merc1_bad_lat0, proj_gt);
    ScopedVsiMemFile out_lat0("grib_cc_merc1_bad_lat0.grb2");
    CPLErrorReset();
    GDALDatasetUniquePtr lat0_ds(GRIBDataset::CreateCopy(
        out_lat0.path(), ds_lat0.get(), FALSE, nullptr, nullptr, nullptr));
    EXPECT_EQ(nullptr, lat0_ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Unsupported or missing spatial reference system"));
  }

  // 2. Mercator_2SP valid & invalid cases, plus TransformToGeo failure on LAEA.
  {
    OGRSpatialReference merc2;
    merc2.SetWellKnownGeogCS("WGS84");
    merc2.SetMercator2SP(30.0, 0.0, 0.0, 0.0, 0.0);
    verify_proj("merc2", merc2);

    // Out-of-domain coordinates in LAEA to trigger TransformToGeo failure.
    OGRSpatialReference laea_oob;
    laea_oob.SetWellKnownGeogCS("WGS84");
    laea_oob.SetLAEA(45.0, -100.0, 0.0, 0.0);
    auto bad_coords =
        MakeMemDataset(2, 2, 1, GDT_Byte, &laea_oob,
                       GDALGeoTransform(1e10, 1000.0, 0.0, 1e10, 0.0, -1000.0));
    ScopedVsiMemFile out_bad("grib_cc_laea_bad_coords.grb2");
    CPLErrorReset();
    GDALDatasetUniquePtr bad_ds(GRIBDataset::CreateCopy(
        out_bad.path(), bad_coords.get(), FALSE, nullptr, nullptr, nullptr));
    EXPECT_EQ(nullptr, bad_ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    OGRSpatialReference merc2_bad_cm;
    merc2_bad_cm.SetWellKnownGeogCS("WGS84");
    merc2_bad_cm.SetMercator2SP(30.0, 0.0, 10.0, 0.0, 0.0);
    auto ds_cm = MakeMemDataset(2, 2, 1, GDT_Byte, &merc2_bad_cm, proj_gt);
    ScopedVsiMemFile out_cm("grib_cc_merc2_bad_cm.grb2");
    CPLErrorReset();
    GDALDatasetUniquePtr cm_ds(GRIBDataset::CreateCopy(
        out_cm.path(), ds_cm.get(), FALSE, nullptr, nullptr, nullptr));
    EXPECT_EQ(nullptr, cm_ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        HasSubstr("Mercator_2SP with central_meridian != 0 not supported"));

    OGRSpatialReference merc2_bad_lat0;
    merc2_bad_lat0.SetWellKnownGeogCS("WGS84");
    merc2_bad_lat0.SetMercator2SP(30.0, 10.0, 0.0, 0.0, 0.0);
    auto ds_lat0 = MakeMemDataset(2, 2, 1, GDT_Byte, &merc2_bad_lat0, proj_gt);
    ScopedVsiMemFile out_lat0("grib_cc_merc2_bad_lat0.grb2");
    CPLErrorReset();
    GDALDatasetUniquePtr lat0_ds(GRIBDataset::CreateCopy(
        out_lat0.path(), ds_lat0.get(), FALSE, nullptr, nullptr, nullptr));
    EXPECT_EQ(nullptr, lat0_ds);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("No inverse operation"));
  }

  // 3. Transverse Mercator.
  {
    OGRSpatialReference tm;
    tm.SetWellKnownGeogCS("WGS84");
    tm.SetTM(0.0, -75.0, 0.9996, 500000.0, 0.0);
    verify_proj("tm", tm);
  }

  // 4. Polar Stereographic (North & South Pole).
  // WritePolarStereographic() wraps negative central_meridian (-90) to
  // [0, 360] (270 E), and gribdataset.cpp imports GS3_POLAR orientLon
  // directly without Lon360to180().
  {
    OGRSpatialReference ps_north;
    ps_north.SetWellKnownGeogCS("WGS84");
    ps_north.SetPS(60.0, -90.0, 1.0, 0.0, 0.0);
    OGRSpatialReference expected_ps_north;
    expected_ps_north.SetWellKnownGeogCS("WGS84");
    expected_ps_north.SetPS(60.0, 270.0, 1.0, 0.0, 0.0);
    verify_proj("ps_north", ps_north, proj_gt, &expected_ps_north);

    OGRSpatialReference ps_south;
    ps_south.SetWellKnownGeogCS("WGS84");
    ps_south.SetPS(-71.0, 0.0, 1.0, 0.0, 0.0);
    verify_proj("ps_south", ps_south);
  }

  // 5. Lambert Conformal Conic 1SP & 2SP.
  {
    OGRSpatialReference lcc1;
    lcc1.SetWellKnownGeogCS("WGS84");
    lcc1.SetLCC1SP(45.0, -90.0, 1.0, 0.0, 0.0);
    verify_proj("lcc1", lcc1);

    OGRSpatialReference lcc2;
    lcc2.SetWellKnownGeogCS("WGS84");
    lcc2.SetLCC(33.0, 45.0, 39.0, -96.0, 0.0, 0.0);
    verify_proj("lcc2", lcc2);
  }

  // 6. Albers Conic Equal Area.
  {
    OGRSpatialReference aea;
    aea.SetWellKnownGeogCS("WGS84");
    aea.SetACEA(29.5, 45.5, 23.0, -96.0, 0.0, 0.0);
    verify_proj("aea", aea);
  }

  // 7. Lambert Azimuthal Equal Area (with and without longitude adjustment).
  {
    OGRSpatialReference laea;
    laea.SetWellKnownGeogCS("WGS84");
    laea.SetLAEA(45.0, -100.0, 0.0, 0.0);
    verify_proj("laea", laea);

    CPLConfigOptionSetter no_adj("GRIB_ADJUST_LONGITUDE_RANGE", "NO", false);
    verify_proj("laea_noadj", laea);
  }
}

TEST_F(GribCreateCopyTest, Section1AndSection4OptionsAndTemplates) {
  const OGRSpatialReference wgs84 = MakeWGS84();
  auto ds = MakeMemDataset(2, 2, 1, GDT_Float32, &wgs84);
  constexpr auto kVals = std::to_array<float>({0.0f, 10.0f, 20.0f, 30.0f});
  FillBand<float>(ds.get(), 1, kVals, GDT_Float32);

  // 1. Section 1 options via IDS string, IDS_* options, and DISCIPLINE.
  {
    CPLStringList opts;
    opts.SetNameValue("DISCIPLINE", "2");
    opts.SetNameValue("IDS",
                      "CENTER=7 SUBCENTER=1 MASTER_TABLE=2 SIGNF_REF_TIME=1 "
                      "REF_TIME=2025-06-15T12:30:45Z PROD_STATUS=0 TYPE=1");
    opts.SetNameValue("IDS_SUBCENTER", "4");
    ScopedVsiMemFile out("grib_cc_sec1.grb2");
    GDALDatasetUniquePtr grib_ds(GRIBDataset::CreateCopy(
        out.path(), ds.get(), FALSE, opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, grib_ds);
    const char* ids_meta =
        grib_ds->GetRasterBand(1)->GetMetadataItem("GRIB_IDS");
    ASSERT_NE(nullptr, ids_meta);
    EXPECT_THAT(ids_meta, HasSubstr("CENTER=7"));
    EXPECT_THAT(ids_meta, HasSubstr("SUBCENTER=4"));
    EXPECT_THAT(ids_meta, HasSubstr("2025-06-15T12:30:45Z"));
  }

  // 2. Section 4 validation errors (non-zero PDTN without template; both
  // PDS_TEMPLATE_NUMBERS and PDS_TEMPLATE_ASSEMBLED_VALUES).
  {
    WithQuietHandler quiet;
    absl::Cleanup reset_err = [] { CPLErrorReset(); };
    CPLStringList missing_tmpl;
    missing_tmpl.SetNameValue("PDS_PDTN", "8");
    ScopedVsiMemFile out("grib_cc_sec4_err1.grb2");
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GRIBDataset::CreateCopy(out.path(), ds.get(), FALSE,
                                      missing_tmpl.List(), nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("PDS_PDTN != 0 specified"));

    CPLStringList both_tmpl;
    both_tmpl.SetNameValue("PDS_TEMPLATE_NUMBERS", "0 0");
    both_tmpl.SetNameValue("PDS_TEMPLATE_ASSEMBLED_VALUES", "0 0");
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GRIBDataset::CreateCopy(out.path(), ds.get(), FALSE,
                                      both_tmpl.List(), nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("PDS_TEMPLATE_NUMBERS and "
                          "PDS_TEMPLATE_ASSEMBLED_VALUES are exclusive"));
  }

  // 3. PDS_TEMPLATE_NUMBERS: valid, byte out-of-range warning, too few bytes,
  // too many bytes warning, unknown PDTN warning, and Celsius->Kelvin offset.
  {
    // PDTN 0 requires 25 bytes in PDS_TEMPLATE_NUMBERS.
    CPLStringList valid_nums;
    valid_nums.SetNameValue("PDS_PDTN", "0");
    valid_nums.SetNameValue(
        "PDS_TEMPLATE_NUMBERS",
        "0 0 2 0 96 0 0 0 1 0 0 0 14 100 0 0 0 0 2 255 0 0 0 0 0");
    ScopedVsiMemFile out_valid("grib_cc_pds_nums_valid.grb2");
    GDALDatasetUniquePtr grib_ds(
        GRIBDataset::CreateCopy(out_valid.path(), ds.get(), FALSE,
                                valid_nums.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, grib_ds);
    // Temperature (0, 0) without INPUT_UNIT converts Celsius -> Kelvin
    // (+273.15) on write, and GRIBDataset converts Kelvin -> Celsius (-273.15)
    // on read.
    const std::vector<float> kelvin = ReadBandAsFloat(grib_ds.get(), 1);
    EXPECT_NEAR(0.0f, kelvin[0], 1e-2f);

    // Explicit INPUT_UNIT=K disables the +273.15 offset on write (so 0 K is
    // read back as -273.15 C).
    valid_nums.SetNameValue("INPUT_UNIT", "K");
    ScopedVsiMemFile out_kelvin("grib_cc_pds_nums_kelvin.grb2");
    GDALDatasetUniquePtr kelvin_ds(
        GRIBDataset::CreateCopy(out_kelvin.path(), ds.get(), FALSE,
                                valid_nums.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, kelvin_ds);
    EXPECT_NEAR(-273.15f, ReadBandAsFloat(kelvin_ds.get(), 1)[0], 1e-2f);

    // Band metadata GRIB_UNIT=[C] triggers Celsius -> Kelvin offset on write.
    ds->GetRasterBand(1)->SetMetadataItem("GRIB_UNIT", "[C]");
    CPLStringList band_unit_opts;
    band_unit_opts.SetNameValue("PDS_PDTN", "0");
    band_unit_opts.SetNameValue(
        "PDS_TEMPLATE_NUMBERS",
        "0 21 2 0 96 0 0 0 1 0 0 0 14 100 0 0 0 0 2 255 0 0 0 0 0");
    ScopedVsiMemFile out_c("grib_cc_pds_nums_c.grb2");
    GDALDatasetUniquePtr c_ds(
        GRIBDataset::CreateCopy(out_c.path(), ds.get(), FALSE,
                                band_unit_opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, c_ds);
    EXPECT_NEAR(0.0f, ReadBandAsFloat(c_ds.get(), 1)[0], 1e-2f);
    ds->GetRasterBand(1)->SetMetadataItem("GRIB_UNIT", nullptr);

    ScopedErrorCollector errors;

    // Too few bytes -> failure.
    CPLStringList too_few;
    too_few.SetNameValue("PDS_PDTN", "0");
    too_few.SetNameValue("PDS_TEMPLATE_NUMBERS", "0 0 2 0 96");
    ScopedVsiMemFile out_few("grib_cc_pds_nums_few.grb2");
    GDALDatasetUniquePtr few_ds(GRIBDataset::CreateCopy(
        out_few.path(), ds.get(), FALSE, too_few.List(), nullptr, nullptr));
    EXPECT_EQ(nullptr, few_ds);
    EXPECT_THAT(
        errors.messages(),
        Contains(HasSubstr("requires 25 bytes in PDS_TEMPLATE_NUMBERS")));
    errors.Clear();

    // Too many bytes + out-of-range byte value -> multiple warnings, succeeds.
    CPLStringList too_many;
    too_many.SetNameValue("PDS_PDTN", "0");
    too_many.SetNameValue(
        "PDS_TEMPLATE_NUMBERS",
        "0 8 2 0 96 0 0 0 1 0 0 0 14 100 0 0 0 0 2 255 0 0 0 0 0 300 -1");
    ScopedVsiMemFile out_many("grib_cc_pds_nums_many.grb2");
    GDALDatasetUniquePtr many_ds(GRIBDataset::CreateCopy(
        out_many.path(), ds.get(), FALSE, too_many.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, many_ds);
    EXPECT_THAT(errors.messages(),
                AllOf(Contains(HasSubstr("should be in [0,255] range")),
                      Contains(HasSubstr("Extra bytes will be ignored"))));
    errors.Clear();

    // Unknown PDTN with PDS_TEMPLATE_NUMBERS -> warning.
    CPLStringList unknown_nums;
    unknown_nums.SetNameValue("PDS_PDTN", "9999");
    unknown_nums.SetNameValue("PDS_TEMPLATE_NUMBERS", "1 2 3 4");
    ScopedVsiMemFile out_unk("grib_cc_pds_nums_unk.grb2");
    // Open() may fail to read unknown PDTN, which still exercises the warning.
    GDALDatasetUniquePtr unk_ds(
        GRIBDataset::CreateCopy(out_unk.path(), ds.get(), FALSE,
                                unknown_nums.List(), nullptr, nullptr));
    EXPECT_THAT(errors.messages(),
                Contains(HasSubstr("PDS_PDTN = 9999 is unknown")));
  }

  // 4. PDS_TEMPLATE_ASSEMBLED_VALUES: unknown PDTN error, too few elements,
  // PDTN 0 signed extremes & out-of-range warnings, PDTN 44 (-2 size), and
  // PDTN 8 extended template (too few, too many, exact).
  {
    ScopedErrorCollector errors;
    ScopedVsiMemFile out("grib_cc_pds_asm_err.grb2");

    CPLStringList unk_asm;
    unk_asm.SetNameValue("PDS_PDTN", "9999");
    unk_asm.SetNameValue("PDS_TEMPLATE_ASSEMBLED_VALUES", "0 0 2");
    EXPECT_EQ(nullptr,
              GRIBDataset::CreateCopy(out.path(), ds.get(), FALSE,
                                      unk_asm.List(), nullptr, nullptr));
    EXPECT_THAT(errors.messages(),
                Contains(HasSubstr("PDS_PDTN = 9999 is unknown")));
    errors.Clear();

    CPLStringList few_asm;
    few_asm.SetNameValue("PDS_PDTN", "0");
    few_asm.SetNameValue("PDS_TEMPLATE_ASSEMBLED_VALUES", "0 0 2");
    EXPECT_EQ(nullptr,
              GRIBDataset::CreateCopy(out.path(), ds.get(), FALSE,
                                      few_asm.List(), nullptr, nullptr));
    EXPECT_THAT(
        errors.messages(),
        Contains(HasSubstr(
            "requires at least 15 elements in PDS_TEMPLATE_ASSEMBLED_VALUES")));
    errors.Clear();

    // PDTN 0 with signed min values (-128 for -1, -2147483648 for -4) and
    // out-of-range warnings for 1, 2, -1, -4.
    CPLStringList pdtn0_warn;
    pdtn0_warn.SetNameValue("PDS_PDTN", "0");
    pdtn0_warn.SetNameValue(
        "PDS_TEMPLATE_ASSEMBLED_VALUES",
        "300 0 2 0 96 70000 0 1 3000000000 100 200 -2147483648 255 -128 -5");
    ScopedVsiMemFile out_p0("grib_cc_pds_asm_p0.grb2");
    GDALDatasetUniquePtr p0_ds(GRIBDataset::CreateCopy(
        out_p0.path(), ds.get(), FALSE, pdtn0_warn.List(), nullptr, nullptr));
    EXPECT_THAT(errors.messages(),
                AllOf(Contains(HasSubstr("should be in [0,255] range")),
                      Contains(HasSubstr("should be in [0,65535] range")),
                      Contains(HasSubstr("should be in [-128,127] range"))));
    errors.Clear();

    // PDTN 44 (21 elements, index 14 has size -2): test -32768 and
    // out-of-range.
    CPLStringList pdtn44;
    pdtn44.SetNameValue("PDS_PDTN", "44");
    pdtn44.SetNameValue(
        "PDS_TEMPLATE_ASSEMBLED_VALUES",
        "0 20 1000 0 -10 -100 10 100 2 0 96 10 0 1 40000 100 0 0 255 -32768 0");
    ScopedVsiMemFile out_p44("grib_cc_pds_asm_p44.grb2");
    GDALDatasetUniquePtr p44_ds(GRIBDataset::CreateCopy(
        out_p44.path(), ds.get(), FALSE, pdtn44.List(), nullptr, nullptr));
    EXPECT_THAT(errors.messages(),
                Contains(HasSubstr("should be in [-32768,32767] range")));
    errors.Clear();

    // PDTN 8 (29 base elements, needext=1; list[21]=2 adds 6 extended elements
    // -> 35 total elements; index 22 has size 4).
    CPLStringList pdtn8_few;
    pdtn8_few.SetNameValue("PDS_PDTN", "8");
    pdtn8_few.SetNameValue("PDS_TEMPLATE_ASSEMBLED_VALUES",
                           "0 5 2 0 0 255 255 1 19 1 0 0 255 -1 -2147483647 "
                           "2008 2 22 12 0 0 2 0 3 255 1 12 1 0");
    EXPECT_EQ(nullptr,
              GRIBDataset::CreateCopy(out.path(), ds.get(), FALSE,
                                      pdtn8_few.List(), nullptr, nullptr));
    EXPECT_THAT(
        errors.messages(),
        Contains(HasSubstr("requires 35 elements in "
                           "PDS_TEMPLATE_ASSEMBLED_VALUES. Only 29 provided")));
    errors.Clear();

    // 36 elements (1 extra) + out-of-range uint32 (5000000000 at index 22).
    CPLStringList pdtn8_many;
    pdtn8_many.SetNameValue("PDS_PDTN", "8");
    pdtn8_many.SetNameValue("PDS_TEMPLATE_ASSEMBLED_VALUES",
                            "0 5 2 0 0 255 255 1 19 1 0 0 255 -1 -2147483647 "
                            "2008 2 22 12 0 0 2 5000000000 3 255 1 12 1 0 "
                            "3 255 1 12 1 0 99");
    ScopedVsiMemFile out_p8("grib_cc_pds_asm_p8.grb2");
    GDALDatasetUniquePtr p8_ds(GRIBDataset::CreateCopy(
        out_p8.path(), ds.get(), FALSE, pdtn8_many.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, p8_ds);
    EXPECT_THAT(errors.messages(),
                Contains(HasSubstr("Extra elements will be ignored")));
  }
}

TEST_F(GribCreateCopyTest, DataTypesAndEncodings) {
  const OGRSpatialReference wgs84 = MakeWGS84();

  // 1. All 7 supported data types with AUTO encoding.
  constexpr auto kDataTypes =
      std::to_array<GDALDataType>({GDT_Byte, GDT_UInt16, GDT_Int16, GDT_UInt32,
                                   GDT_Int32, GDT_Float32, GDT_Float64});
  constexpr auto kDblVals =
      std::to_array<double>({1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0});
  for (GDALDataType dt : kDataTypes) {
    auto ds = MakeMemDataset(4, 2, 1, dt, &wgs84);
    FillBand<double>(ds.get(), 1, kDblVals, GDT_Float64);
    ScopedVsiMemFile out(
        std::format("grib_cc_dt_{}.grb2", GDALGetDataTypeName(dt)));
    GDALDatasetUniquePtr grib_ds(GRIBDataset::CreateCopy(
        out.path(), ds.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, grib_ds) << GDALGetDataTypeName(dt);
    EXPECT_EQ(std::vector<float>({1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f}),
              ReadBandAsFloat(grib_ds.get(), 1));
  }

  // 2. Simple packing edge cases: Float32 with diff <= 256 (auto 8-bit) and
  // diff > 256, constant integer (zero bits), NBITS > 31 clamped,
  // DECIMAL_SCALE_FACTOR > 0 warning on integer, band metadata DRS_NBITS /
  // DRS_DECIMAL_SCALE_FACTOR, and nodata warning on non-complex packing.
  {
    auto fds = MakeMemDataset(4, 2, 1, GDT_Float32, &wgs84);
    constexpr auto kSmallDiff = std::to_array<float>(
        {10.0f, 20.0f, 30.0f, 40.0f, 50.0f, 60.0f, 70.0f, 80.0f});
    FillBand<float>(fds.get(), 1, kSmallDiff, GDT_Float32);
    CPLStringList simple_opts;
    simple_opts.SetNameValue("DATA_ENCODING", "SIMPLE_PACKING");
    ScopedVsiMemFile out1("grib_cc_simple_f1.grb2");
    GDALDatasetUniquePtr ds1(GRIBDataset::CreateCopy(
        out1.path(), fds.get(), FALSE, simple_opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, ds1);
    const std::vector<float> f1_pixels = ReadBandAsFloat(ds1.get(), 1);
    for (size_t i = 0; i < kSmallDiff.size(); ++i) {
      EXPECT_NEAR(kSmallDiff[i], f1_pixels[i], 0.5f);
    }

    constexpr auto kLargeDiff = std::to_array<float>(
        {0.0f, 100.0f, 200.0f, 300.0f, 400.0f, 500.0f, 600.0f, 1000.0f});
    FillBand<float>(fds.get(), 1, kLargeDiff, GDT_Float32);
    fds->GetRasterBand(1)->SetMetadataItem("DRS_NBITS", "12", "GRIB");
    fds->GetRasterBand(1)->SetMetadataItem("DRS_DECIMAL_SCALE_FACTOR", "1",
                                           "GRIB");
    ScopedVsiMemFile out2("grib_cc_simple_f2.grb2");
    GDALDatasetUniquePtr ds2(GRIBDataset::CreateCopy(
        out2.path(), fds.get(), FALSE, simple_opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, ds2);
    EXPECT_STREQ("12",
                 ds2->GetRasterBand(1)->GetMetadataItem("DRS_NBITS", "GRIB"));
    EXPECT_NEAR(1000.0f, ReadBandAsFloat(ds2.get(), 1)[7], 0.5f);

    ScopedErrorCollector errors;
    auto ids = MakeMemDataset(4, 2, 1, GDT_Int16, &wgs84);
    constexpr auto kConstVals = std::to_array<GInt16>({5, 5, 5, 5, 5, 5, 5, 5});
    FillBand<GInt16>(ids.get(), 1, kConstVals, GDT_Int16);
    CPLStringList warn_opts;
    warn_opts.SetNameValue("DATA_ENCODING", "SIMPLE_PACKING");
    warn_opts.SetNameValue("NBITS", "40");
    warn_opts.SetNameValue("DECIMAL_SCALE_FACTOR", "2");
    ScopedVsiMemFile out3("grib_cc_simple_warn.grb2");
    GDALDatasetUniquePtr ds3(GRIBDataset::CreateCopy(
        out3.path(), ids.get(), FALSE, warn_opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, ds3);
    EXPECT_THAT(
        errors.messages(),
        AllOf(Contains(HasSubstr("NBITS clamped to 31")),
              Contains(HasSubstr("DECIMAL_SCALE_FACTOR > 0 makes no sense for "
                                 "integer data types"))));
    EXPECT_EQ(std::vector<float>(8, 5.0f), ReadBandAsFloat(ds3.get(), 1));
    errors.Clear();

    // Non-complex packing with actual nodata pixel triggers warning.
    constexpr auto kNdVals = std::to_array<GInt16>({-999, 5, 5, 5, 5, 5, 5, 5});
    FillBand<GInt16>(ids.get(), 1, kNdVals, GDT_Int16);
    ids->GetRasterBand(1)->SetNoDataValue(-999);
    ScopedVsiMemFile out4("grib_cc_simple_nd_warn.grb2");
    GDALDatasetUniquePtr ds4(GRIBDataset::CreateCopy(
        out4.path(), ids.get(), FALSE, simple_opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, ds4);
    EXPECT_THAT(
        errors.messages(),
        Contains(HasSubstr(
            "non-COMPLEX_PACKING DATA_ENCODING cannot preserve nodata")));
    int has_nd = 1;
    ds4->GetRasterBand(1)->GetNoDataValue(&has_nd);
    EXPECT_EQ(0, has_nd);
    EXPECT_FLOAT_EQ(-999.0f, ReadBandAsFloat(ds4.get(), 1)[0]);
  }

  // 3. Complex packing: spatial differencing orders 0, 1, 2; partial nodata;
  // all nodata; constant values with & without nodata; declared nodata not
  // found in pixels; out-of-int-range nodata on UInt32.
  {
    auto fds = MakeMemDataset(8, 8, 1, GDT_Float32, &wgs84);
    std::vector<float> vals(64);
    for (int i = 0; i < 64; ++i) {
      vals[i] =
          (i % 7 == 0) ? -9999.0f : static_cast<float>((i * i * i) % 10000 + i);
    }
    FillBand<float>(fds.get(), 1, vals, GDT_Float32);
    fds->GetRasterBand(1)->SetNoDataValue(-9999.0);

    constexpr auto kSdoOrders =
        std::to_array<std::string_view>({"0", "1", "2"});
    for (std::string_view sdo : kSdoOrders) {
      CPLStringList opts;
      opts.SetNameValue("DATA_ENCODING", "COMPLEX_PACKING");
      opts.SetNameValue("SPATIAL_DIFFERENCING_ORDER", std::string(sdo).c_str());
      ScopedVsiMemFile out(std::format("grib_cc_cmplx_sdo_{}.grb2", sdo));
      GDALDatasetUniquePtr grib_ds(GRIBDataset::CreateCopy(
          out.path(), fds.get(), FALSE, opts.List(), nullptr, nullptr));
      ASSERT_NE(nullptr, grib_ds);
      int has_nd = 0;
      EXPECT_FLOAT_EQ(-9999.0f,
                      static_cast<float>(
                          grib_ds->GetRasterBand(1)->GetNoDataValue(&has_nd)));
      EXPECT_EQ(1, has_nd);
      const std::vector<float> read_px = ReadBandAsFloat(grib_ds.get(), 1);
      EXPECT_FLOAT_EQ(-9999.0f, read_px[0]);
      EXPECT_NEAR(vals[1], read_px[1], 1e-1f);
    }

    // All-nodata band -> m_fMin > m_fMax -> zero bits.
    std::vector<float> all_nd(64, -9999.0f);
    FillBand<float>(fds.get(), 1, all_nd, GDT_Float32);
    ScopedVsiMemFile out_allnd("grib_cc_cmplx_allnd.grb2");
    GDALDatasetUniquePtr allnd_ds(GRIBDataset::CreateCopy(
        out_allnd.path(), fds.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, allnd_ds);
    int has_nd = 0;
    EXPECT_FLOAT_EQ(-9999.0f,
                    static_cast<float>(
                        allnd_ds->GetRasterBand(1)->GetNoDataValue(&has_nd)));
    EXPECT_EQ(1, has_nd);
    EXPECT_EQ(all_nd, ReadBandAsFloat(allnd_ds.get(), 1));

    // Declared nodata not present in pixels -> AUTO falls back to non-complex.
    std::vector<float> no_nd_present(64, 12.5f);
    FillBand<float>(fds.get(), 1, no_nd_present, GDT_Float32);
    ScopedVsiMemFile out_nond("grib_cc_auto_nond.grb2");
    GDALDatasetUniquePtr nond_ds(GRIBDataset::CreateCopy(
        out_nond.path(), fds.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, nond_ds);
    EXPECT_EQ(no_nd_present, ReadBandAsFloat(nond_ds.get(), 1));

    // Int32 with in-range nodata vs UInt32 with out-of-int-range nodata.
    auto i32_ds = MakeMemDataset(8, 8, 1, GDT_Int32, &wgs84);
    std::vector<GInt32> i32_vals(64);
    for (int i = 0; i < 64; ++i) {
      i32_vals[i] = (i % 7 == 0) ? -10 : ((i * i * i) % 10000 + i);
    }
    FillBand<GInt32>(i32_ds.get(), 1, i32_vals, GDT_Int32);
    i32_ds->GetRasterBand(1)->SetNoDataValue(-10);
    ScopedVsiMemFile out_i32("grib_cc_cmplx_i32.grb2");
    GDALDatasetUniquePtr grib_i32(GRIBDataset::CreateCopy(
        out_i32.path(), i32_ds.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, grib_i32);
    has_nd = FALSE;
    // GRIB2 DRS encodes -10 in sign-magnitude 32-bit integer (0x8000000A =
    // -2147483638), which rounds to -2^31 (INT32_MIN) when converted through
    // float32 in GRIB2ComplexPacking::UnpackData().
    EXPECT_DOUBLE_EQ(-2147483638.0,
                     grib_i32->GetRasterBand(1)->GetNoDataValue(&has_nd));
    EXPECT_EQ(TRUE, has_nd);
    std::vector<int32_t> i32_read(64, 0);
    ASSERT_EQ(CE_None, grib_i32->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 8, 8, i32_read.data(), 8, 8,
                           GDT_Int32, 0, 0, nullptr));
    EXPECT_EQ(std::numeric_limits<int32_t>::min(), i32_read[0]);
    EXPECT_EQ(i32_vals[1], i32_read[1]);

    auto u32_ds = MakeMemDataset(8, 8, 1, GDT_UInt32, &wgs84);
    std::vector<GUInt32> u32_vals(64);
    for (int i = 0; i < 64; ++i) {
      u32_vals[i] = static_cast<GUInt32>((i * i * i) % 10000 + i);
    }
    FillBand<GUInt32>(u32_ds.get(), 1, u32_vals, GDT_UInt32);
    u32_ds->GetRasterBand(1)->SetNoDataValue(1e20);
    CPLStringList cmplx_opts;
    cmplx_opts.SetNameValue("DATA_ENCODING", "COMPLEX_PACKING");
    ScopedVsiMemFile out_u32("grib_cc_cmplx_u32.grb2");
    GDALDatasetUniquePtr grib_u32(
        GRIBDataset::CreateCopy(out_u32.path(), u32_ds.get(), FALSE,
                                cmplx_opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, grib_u32);
    EXPECT_FLOAT_EQ(static_cast<float>(u32_vals[1]),
                    ReadBandAsFloat(grib_u32.get(), 1)[1]);
  }

  // 4. IEEE floating point warnings (NBITS, DECIMAL_SCALE_FACTOR,
  // JPEG2000_DRIVER, SPATIAL_DIFFERENCING_ORDER) and Float64 with ValOffset.
  {
    ScopedErrorCollector errors;
    auto f64_ds = MakeMemDataset(2, 2, 1, GDT_Float64, &wgs84);
    constexpr auto kDvals4 = std::to_array<double>({0.0, 10.0, 20.0, 30.0});
    FillBand<double>(f64_ds.get(), 1, kDvals4, GDT_Float64);
    CPLStringList opts;
    opts.SetNameValue("DATA_ENCODING", "IEEE_FLOATING_POINT");
    opts.SetNameValue("NBITS", "16");
    opts.SetNameValue("DECIMAL_SCALE_FACTOR", "2");
    opts.SetNameValue("JPEG2000_DRIVER", "JP2KAK");
    opts.SetNameValue("PDS_PDTN", "0");
    opts.SetNameValue(
        "PDS_TEMPLATE_NUMBERS",
        "0 0 2 0 96 0 0 0 1 0 0 0 14 100 0 0 0 0 2 255 0 0 0 0 0");
    ScopedVsiMemFile out("grib_cc_ieee_warns.grb2");
    GDALDatasetUniquePtr grib_ds(GRIBDataset::CreateCopy(
        out.path(), f64_ds.get(), FALSE, opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, grib_ds);
    EXPECT_THAT(
        errors.messages(),
        AllOf(Contains(HasSubstr(
                  "NBITS ignored for DATA_ENCODING = IEEE_FLOATING_POINT")),
              Contains(HasSubstr("DECIMAL_SCALE_FACTOR ignored for "
                                 "DATA_ENCODING = IEEE_FLOATING_POINT")),
              Contains(
                  HasSubstr("JPEG2000_DRIVER option ignored for non-JPEG2000 "
                            "DATA_ENCODING"))));
    errors.Clear();

    CPLStringList sdo_warn_opts;
    sdo_warn_opts.SetNameValue("DATA_ENCODING", "IEEE_FLOATING_POINT");
    sdo_warn_opts.SetNameValue("SPATIAL_DIFFERENCING_ORDER", "1");
    ScopedVsiMemFile out_sdo("grib_cc_ieee_sdo_warn.grb2");
    GDALDatasetUniquePtr sdo_ds(
        GRIBDataset::CreateCopy(out_sdo.path(), f64_ds.get(), FALSE,
                                sdo_warn_opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, sdo_ds);
    EXPECT_THAT(errors.messages(),
                Contains(HasSubstr("SPATIAL_DIFFERENCING_ORDER option ignored "
                                   "for non-COMPLEX_PACKING DATA_ENCODING")));
  }

  // 5. PNG encoding: constant (zero bits), varying bit depths (3-bit -> 4,
  // 5-bit -> 8, 10-bit -> 16, >16-bit auto warning, NBITS=20 explicit warning).
  {
    ScopedErrorCollector errors;
    auto ds = MakeMemDataset(4, 2, 1, GDT_UInt32, &wgs84);
    constexpr auto kConstVals =
        std::to_array<GUInt32>({7, 7, 7, 7, 7, 7, 7, 7});
    FillBand<GUInt32>(ds.get(), 1, kConstVals, GDT_UInt32);
    CPLStringList png_opts;
    png_opts.SetNameValue("DATA_ENCODING", "PNG");
    ScopedVsiMemFile out_const("grib_cc_png_const.grb2");
    GDALDatasetUniquePtr ds_const(GRIBDataset::CreateCopy(
        out_const.path(), ds.get(), FALSE, png_opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, ds_const);
    EXPECT_STREQ(
        "41", ds_const->GetRasterBand(1)->GetMetadataItem("DRS_DRTN", "GRIB"));
    EXPECT_STREQ(
        "0", ds_const->GetRasterBand(1)->GetMetadataItem("DRS_NBITS", "GRIB"));

    constexpr auto kMaxVals = std::to_array<GUInt32>({6u, 20u, 600u, 1000000u});
    for (GUInt32 max_val : kMaxVals) {
      const std::array<GUInt32, 8> v = {0, 1, 2, 3, 4, 5, 6, max_val};
      FillBand<GUInt32>(ds.get(), 1, v, GDT_UInt32);
      ScopedVsiMemFile out(std::format("grib_cc_png_{}.grb2", max_val));
      GDALDatasetUniquePtr grib_ds(GRIBDataset::CreateCopy(
          out.path(), ds.get(), FALSE, png_opts.List(), nullptr, nullptr));
      ASSERT_NE(nullptr, grib_ds);
      EXPECT_STREQ(
          "41", grib_ds->GetRasterBand(1)->GetMetadataItem("DRS_DRTN", "GRIB"));
      if (max_val > 600u) {
        EXPECT_STREQ("16", grib_ds->GetRasterBand(1)->GetMetadataItem(
                               "DRS_NBITS", "GRIB"));
      }
      // g2clib is compiled without USE_PNG in google3; pin the Template 5.41
      // decode error path while PNG creation via PNG driver succeeds.
      WithQuietHandler quiet_png;
      CPLErrorReset();
      std::array<float, 8> px{};
      EXPECT_EQ(CE_Failure, grib_ds->GetRasterBand(1)->RasterIO(
                                GF_Read, 0, 0, 4, 2, px.data(), 4, 2,
                                GDT_Float32, 0, 0, nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }
    EXPECT_THAT(
        errors.messages(),
        Contains(HasSubstr(
            "More than 16 bits of integer precision would be required")));
    errors.Clear();

    png_opts.SetNameValue("NBITS", "20");
    ScopedVsiMemFile out_nbits("grib_cc_png_nbits20.grb2");
    GDALDatasetUniquePtr ds_nbits(GRIBDataset::CreateCopy(
        out_nbits.path(), ds.get(), FALSE, png_opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, ds_nbits);
    EXPECT_STREQ(
        "16", ds_nbits->GetRasterBand(1)->GetMetadataItem("DRS_NBITS", "GRIB"));
    EXPECT_THAT(
        errors.messages(),
        Contains(HasSubstr("Maximum bit depth supported is 16. Using that")));
  }

  // 6. JPEG2000 encoding: constant (zero bits), AUTO with JPEG2000_DRIVER,
  // lossless (<10000 pixels with COMPRESSION_RATIO=10), lossy (100x100 = 10000
  // pixels with COMPRESSION_RATIO=10), and unknown driver error.
  {
    auto ds = MakeMemDataset(4, 2, 1, GDT_UInt16, &wgs84);
    constexpr auto kConstVals =
        std::to_array<GUInt16>({9, 9, 9, 9, 9, 9, 9, 9});
    FillBand<GUInt16>(ds.get(), 1, kConstVals, GDT_UInt16);
    CPLStringList j2k_opts;
    j2k_opts.SetNameValue("DATA_ENCODING", "JPEG2000");
    ScopedVsiMemFile out_const("grib_cc_j2k_const.grb2");
    GDALDatasetUniquePtr ds_const(GRIBDataset::CreateCopy(
        out_const.path(), ds.get(), FALSE, j2k_opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, ds_const);
    EXPECT_EQ(std::vector<float>(8, 9.0f), ReadBandAsFloat(ds_const.get(), 1));

    constexpr auto kVarVals =
        std::to_array<GUInt16>({10, 20, 30, 40, 50, 60, 70, 80});
    FillBand<GUInt16>(ds.get(), 1, kVarVals, GDT_UInt16);
    CPLStringList auto_j2k;
    auto_j2k.SetNameValue("JPEG2000_DRIVER", "JP2KAK");
    auto_j2k.SetNameValue("COMPRESSION_RATIO", "10");
    ScopedVsiMemFile out_small("grib_cc_j2k_small.grb2");
    GDALDatasetUniquePtr ds_small(GRIBDataset::CreateCopy(
        out_small.path(), ds.get(), FALSE, auto_j2k.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, ds_small);
    EXPECT_STREQ(
        "40", ds_small->GetRasterBand(1)->GetMetadataItem("DRS_DRTN", "GRIB"));
    EXPECT_EQ(
        std::vector<float>({10.f, 20.f, 30.f, 40.f, 50.f, 60.f, 70.f, 80.f}),
        ReadBandAsFloat(ds_small.get(), 1));

    // 100x100 = 10000 pixels triggers lossy JPEG2000 compression.
    auto big_ds = MakeMemDataset(100, 100, 1, GDT_UInt16, &wgs84);
    std::vector<GUInt16> big_vals(10000);
    for (int i = 0; i < 10000; ++i) {
      big_vals[i] = static_cast<GUInt16>((i * 7) % 1000);
    }
    FillBand<GUInt16>(big_ds.get(), 1, big_vals, GDT_UInt16);
    ScopedVsiMemFile out_big("grib_cc_j2k_lossy.grb2");
    GDALDatasetUniquePtr ds_big(
        GRIBDataset::CreateCopy(out_big.path(), big_ds.get(), FALSE,
                                auto_j2k.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, ds_big);
    EXPECT_EQ(100, ds_big->GetRasterXSize());
    EXPECT_EQ(100, ds_big->GetRasterYSize());
    EXPECT_EQ(10000U, ReadBandAsFloat(ds_big.get(), 1).size());

    WithQuietHandler quiet;
    absl::Cleanup reset_err = [] { CPLErrorReset(); };
    CPLStringList bad_j2k;
    bad_j2k.SetNameValue("DATA_ENCODING", "JPEG2000");
    bad_j2k.SetNameValue("JPEG2000_DRIVER", "NO_SUCH_J2K_DRIVER");
    ScopedVsiMemFile out_bad("grib_cc_j2k_bad.grb2");
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GRIBDataset::CreateCopy(out_bad.path(), ds.get(), FALSE,
                                      bad_j2k.List(), nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot find JPEG2000 driver"));
  }
}

TEST_F(GribCreateCopyTest, MultiBandAppendAndWriteSubgrids) {
  const OGRSpatialReference wgs84 = MakeWGS84();

  // 1. Multi-band with per-band options + WRITE_SUBGRIDS=YES.
  {
    auto src = MakeMemDataset(8, 8, 2, GDT_Float32, &wgs84);
    std::vector<float> b1(64);
    std::vector<float> b2(64);
    for (int i = 0; i < 64; ++i) {
      b1[i] = static_cast<float>(i + 1);
      b2[i] = static_cast<float>((i * i * i) % 10000 + i);
    }
    FillBand<float>(src.get(), 1, b1, GDT_Float32);
    FillBand<float>(src.get(), 2, b2, GDT_Float32);

    CPLStringList opts;
    opts.SetNameValue("WRITE_SUBGRIDS", "YES");
    opts.SetNameValue("BAND_1_DATA_ENCODING", "SIMPLE_PACKING");
    opts.SetNameValue("BAND_2_DATA_ENCODING", "COMPLEX_PACKING");
    ScopedVsiMemFile out("grib_cc_subgrids.grb2");
    GDALDatasetUniquePtr grib_ds(GRIBDataset::CreateCopy(
        out.path(), src.get(), FALSE, opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, grib_ds);
    ASSERT_EQ(2, grib_ds->GetRasterCount());
    EXPECT_EQ(b1, ReadBandAsFloat(grib_ds.get(), 1));
    EXPECT_EQ(b2, ReadBandAsFloat(grib_ds.get(), 2));
  }

  // 2. APPEND_SUBDATASET=YES appends band 2 onto an existing GRIB2 file.
  {
    auto src1 = MakeMemDataset(4, 2, 1, GDT_Byte, &wgs84);
    constexpr auto b1 = std::to_array<GByte>({1, 2, 3, 4, 5, 6, 7, 8});
    FillBand<GByte>(src1.get(), 1, b1, GDT_Byte);

    auto src2 = MakeMemDataset(4, 2, 1, GDT_Byte, &wgs84);
    constexpr auto b2 = std::to_array<GByte>({11, 12, 13, 14, 15, 16, 17, 18});
    FillBand<GByte>(src2.get(), 1, b2, GDT_Byte);

    ScopedVsiMemFile out("grib_cc_append.grb2");
    GDALDatasetUniquePtr ds1(GRIBDataset::CreateCopy(
        out.path(), src1.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, ds1);
    ds1.reset();

    CPLStringList append_opts;
    append_opts.SetNameValue("APPEND_SUBDATASET", "YES");
    GDALDatasetUniquePtr ds2(GRIBDataset::CreateCopy(
        out.path(), src2.get(), FALSE, append_opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, ds2);
    ASSERT_EQ(2, ds2->GetRasterCount());
    EXPECT_EQ(std::vector<float>({1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f}),
              ReadBandAsFloat(ds2.get(), 1));
    EXPECT_EQ(
        std::vector<float>({11.f, 12.f, 13.f, 14.f, 15.f, 16.f, 17.f, 18.f}),
        ReadBandAsFloat(ds2.get(), 2));
  }
}

// TODO: b/335317901 - Cover remaining branches in gribcreatecopy.cpp
// (pszProjection == nullptr in ValidateSRS, WriteInt16 min clamping, Mercator
// lat_0 != 0 post-WKT check, WriteLAEA dfLLX > 360, simple/complex packing
// integer overflow, projection/PDS-ext write failures, and >2 GB file check).

}  // namespace
}  // namespace autotest2
