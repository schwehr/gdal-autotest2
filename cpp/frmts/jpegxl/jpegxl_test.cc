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
// Tests for the JPEG-XL raster driver (jpegxl.cpp).
//
// Gated features in google3 static build: HAVE_JXL_BOX_API,
// HAVE_JxlEncoderInitExtraChannelInfo, HAVE_JxlEncoderSetExtraChannelDistance,
// and HAVE_JxlEncoderSetCodestreamLevel are enabled; HAVE_JXL_THREADS,
// PLUGIN_FILENAME, and PAM_ENABLED are disabled.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/jpegxl/jpegxl.cpp
//   https://gdal.org/drivers/raster/jpegxl.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/jpegxl.py

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>  // NOLINT
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/matchers.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::NotNull;

constexpr char kVsiDir[] = "/vsimem/jpegxl_test";

constexpr auto kJxlContainerSig = std::to_array<uint8_t>({
    0x00, 0x00, 0x00, 0x0C, 'J', 'X', 'L', ' ', 0x0D, 0x0A, 0x87, 0x0A,
});

// Valid base64-encoded sRGB ICC profile (592 bytes).
constexpr char kSRGBIccBase64[] =
    "AAACUGxjbXMEMAAAbW50clJHQiBYWVogB94ABQAVABEAFAATYWNzcE1TRlQAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAPbWAAEAAAAA0y1sY21zAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAALZGVzYwAAAQgAAAA4Y3BydAAAAUAAAABOd3RwdAAA"
    "AZAAAAAUY2hhZAAAAaQAAAAsclhZWgAAAdAAAAAUYlhZWgAAAeQAAAAUZ1hZWgAAAfgAAAAU"
    "clRSQwAAAgwAAAAgZ1RSQwAAAgwAAAAgYlRSQwAAAgwAAAAgY2hybQAAAiwAAAAkbWx1YwAA"
    "AAAAAAABAAAADGVuVVMAAAAcAAAAHABzAFIARwBCACAAYgB1AGkAbAB0AC0AaQBuAABtbHVj"
    "AAAAAAAAAAEAAAAMZW5VUwAAADIAAAAcAE4AbwAgAGMAbwBwAHkAcgBpAGcAaAB0ACwAIAB1"
    "AHMAZQAgAGYAcgBlAGUAbAB5AAAAAFhZWiAAAAAAAAD21gABAAAAANMtc2YzMgAAAAAAAQxK"
    "AAAF4///8yoAAAebAAD9h///+6L///2jAAAD2AAAwJRYWVogAAAAAAAAb5QAADjuAAADkFhZ"
    "WiAAAAAAAAAknQAAD4MAALa+WFlaIAAAAAAAAGKlAAC3kAAAGN5wYXJhAAAAAAADAAAAAmZm"
    "AADypwAADVkAABPQAAAKW2Nocm0AAAAAAAMAAAAAo9cAAFR7AABMzQAAmZoAACZmAAAPXA==";

// BitWriter to craft synthetic JXL codestreams.
class BitWriter {
 public:
  void WriteBits(uint32_t val, int count) {
    for (int i = 0; i < count; ++i) {
      current_byte_ |= static_cast<uint8_t>(((val >> i) & 1) << bit_pos_);
      bit_pos_++;
      if (bit_pos_ == 8) {
        bytes_.push_back(current_byte_);
        current_byte_ = 0;
        bit_pos_ = 0;
      }
    }
  }

  void Flush() {
    if (bit_pos_ > 0) {
      bytes_.push_back(current_byte_);
      current_byte_ = 0;
      bit_pos_ = 0;
    }
  }

  const std::vector<uint8_t>& Bytes() const { return bytes_; }

 private:
  std::vector<uint8_t> bytes_;
  uint8_t current_byte_ = 0;
  int bit_pos_ = 0;
};

// Builds an ISOBMFF box with 4-byte size and 4-byte type.
std::vector<uint8_t> MakeIsobmffBox(std::string_view type,
                                    const void* payload,
                                    size_t payload_size) {
  std::vector<uint8_t> box;
  const uint32_t total_size = static_cast<uint32_t>(8 + payload_size);
  box.push_back(static_cast<uint8_t>((total_size >> 24) & 0xFF));
  box.push_back(static_cast<uint8_t>((total_size >> 16) & 0xFF));
  box.push_back(static_cast<uint8_t>((total_size >> 8) & 0xFF));
  box.push_back(static_cast<uint8_t>(total_size & 0xFF));
  box.push_back(static_cast<uint8_t>(type[0]));
  box.push_back(static_cast<uint8_t>(type[1]));
  box.push_back(static_cast<uint8_t>(type[2]));
  box.push_back(static_cast<uint8_t>(type[3]));
  if (payload_size > 0 && payload != nullptr) {
    const auto* p = static_cast<const uint8_t*>(payload);
    box.insert(box.end(), p, p + payload_size);
  }
  return box;
}

// Builds a big-endian TIFF Exif box payload.
// JXL Exif box payload starts with 4 zero bytes (exif_offset = 0) followed by
// TIFF header.
// Note: Pins upstream GDAL quirk in jpegxl.cpp:358-370. When bSwab == true for
// big-endian TIFF ("MM\0*"), upstream calls CPL_LSBPTR32 on the IFD offset
// instead of CPL_SWAP32P. On little-endian hosts, CPL_LSBPTR32 is a no-op, so
// the IFD offset must be written in little-endian byte order
// ({0x08, 0x00, 0x00, 0x00}) inside an MM header for upstream to parse it.
std::vector<uint8_t> BuildTiffExifBoxPayload(uint16_t orientation) {
  std::vector<uint8_t> payload;
  payload.insert(payload.end(), 4, 0x00);

  // MM\0*
  payload.push_back('M');
  payload.push_back('M');
  payload.push_back(0x00);
  payload.push_back(0x2A);
  // Deliberately little-endian IFD0 offset = 8 to match upstream CPL_LSBPTR32 quirk.
  payload.push_back(0x08);
  payload.push_back(0x00);
  payload.push_back(0x00);
  payload.push_back(0x00);

  // IFD0: 4 tags (Orientation, ExifOffset, GPSInfoOffset, InteropOffset)
  // Tag count = 4
  payload.push_back(0x00);
  payload.push_back(0x04);

  // Tag 1: Orientation (0x0112, SHORT, count 1, value)
  payload.push_back(0x01);
  payload.push_back(0x12);
  payload.push_back(0x00);
  payload.push_back(0x03);
  payload.push_back(0x00);
  payload.push_back(0x00);
  payload.push_back(0x00);
  payload.push_back(0x01);
  payload.push_back(static_cast<uint8_t>((orientation >> 8) & 0xFF));
  payload.push_back(static_cast<uint8_t>(orientation & 0xFF));
  payload.push_back(0x00);
  payload.push_back(0x00);

  // Offset of sub-IFD 1 (after IFD0 header + 4 tags + next IFD offset =
  // 8 + 2 + 48 + 4 = 62)
  const uint32_t exif_subifd_offset = 62;
  // Tag 2: ExifOffset (0x8769, LONG, count 1, offset)
  payload.push_back(0x87);
  payload.push_back(0x69);
  payload.push_back(0x00);
  payload.push_back(0x04);
  payload.push_back(0x00);
  payload.push_back(0x00);
  payload.push_back(0x00);
  payload.push_back(0x01);
  payload.push_back(static_cast<uint8_t>((exif_subifd_offset >> 24) & 0xFF));
  payload.push_back(static_cast<uint8_t>((exif_subifd_offset >> 16) & 0xFF));
  payload.push_back(static_cast<uint8_t>((exif_subifd_offset >> 8) & 0xFF));
  payload.push_back(static_cast<uint8_t>(exif_subifd_offset & 0xFF));

  // Tag 3: GPSInfo (0x8825, LONG, count 1, offset)
  const uint32_t gps_subifd_offset = exif_subifd_offset + 6;
  payload.push_back(0x88);
  payload.push_back(0x25);
  payload.push_back(0x00);
  payload.push_back(0x04);
  payload.push_back(0x00);
  payload.push_back(0x00);
  payload.push_back(0x00);
  payload.push_back(0x01);
  payload.push_back(static_cast<uint8_t>((gps_subifd_offset >> 24) & 0xFF));
  payload.push_back(static_cast<uint8_t>((gps_subifd_offset >> 16) & 0xFF));
  payload.push_back(static_cast<uint8_t>((gps_subifd_offset >> 8) & 0xFF));
  payload.push_back(static_cast<uint8_t>(gps_subifd_offset & 0xFF));

  // Tag 4: InteroperabilityOffset (0xA005, LONG, count 1, offset)
  const uint32_t interop_subifd_offset = gps_subifd_offset + 6;
  payload.push_back(0xA0);
  payload.push_back(0x05);
  payload.push_back(0x00);
  payload.push_back(0x04);
  payload.push_back(0x00);
  payload.push_back(0x00);
  payload.push_back(0x00);
  payload.push_back(0x01);
  payload.push_back(
      static_cast<uint8_t>((interop_subifd_offset >> 24) & 0xFF));
  payload.push_back(
      static_cast<uint8_t>((interop_subifd_offset >> 16) & 0xFF));
  payload.push_back(static_cast<uint8_t>((interop_subifd_offset >> 8) & 0xFF));
  payload.push_back(static_cast<uint8_t>(interop_subifd_offset & 0xFF));

  // Next IFD offset = 0
  payload.push_back(0x00);
  payload.push_back(0x00);
  payload.push_back(0x00);
  payload.push_back(0x00);

  // Empty SubIFDs (tag count = 0, next IFD = 0)
  for (int i = 0; i < 3; ++i) {
    payload.push_back(0x00);
    payload.push_back(0x00);
    payload.push_back(0x00);
    payload.push_back(0x00);
    payload.push_back(0x00);
    payload.push_back(0x00);
  }

  return payload;
}

// Builds a synthetic JXL codestream header.
std::vector<uint8_t> BuildSyntheticJxlCodestream(
    bool small,
    uint32_t ysize,
    uint32_t ratio,
    bool floating_point,
    uint32_t bits_per_sample,
    uint32_t exponent_bits,
    const std::vector<uint32_t>& extra_channel_types) {
  BitWriter bw;
  // JXL codestream signature: 0xFF, 0x0A
  bw.WriteBits(0xFF, 8);
  bw.WriteBits(0x0A, 8);

  // SizeHeader
  if (small) {
    bw.WriteBits(1, 1);                    // small = true
    bw.WriteBits((ysize / 8) - 1, 5);     // ysize_div8_minus_1
    bw.WriteBits(ratio, 3);                // ratio
  } else {
    bw.WriteBits(0, 1);                    // small = false
    // U32 distribution 3: BitsOffset(30, 1) -> selector 3 (2 bits) + 30 bits
    bw.WriteBits(3, 2);                    // selector 3
    bw.WriteBits(ysize - 1, 30);           // 30 bits
    bw.WriteBits(ratio, 3);                // ratio (if 7: 2:1 ratio)
  }

  // ImageMetadata
  bw.WriteBits(0, 1);  // all_default = false
  bw.WriteBits(0, 1);  // extra_fields = false

  // BitDepth
  if (!floating_point) {
    bw.WriteBits(0, 1);  // floating_point_sample = false
    if (bits_per_sample == 8) {
      bw.WriteBits(0, 2);  // selector 0 = 8 bits
    } else if (bits_per_sample == 16) {
      bw.WriteBits(3, 2);  // selector 3
      bw.WriteBits(16 - 1, 6);
    } else {
      // Custom bits_per_sample (e.g. 24)
      bw.WriteBits(3, 2);  // selector 3 = BitsOffset(6, 1)
      bw.WriteBits(bits_per_sample - 1, 6);
    }
  } else {
    bw.WriteBits(1, 1);  // floating_point_sample = true
    if (bits_per_sample == 16) {
      bw.WriteBits(1, 2);  // selector 1 = 16 bits
    } else {
      bw.WriteBits(0, 2);  // selector 0 = 32 bits
    }
    bw.WriteBits(exponent_bits - 1, 4);
  }

  // modular_16_bit_buffer_sufficient
  bw.WriteBits(1, 1);

  // num_extra_channels
  if (extra_channel_types.empty()) {
    bw.WriteBits(0, 2);  // selector 0 = 0 channels
  } else if (extra_channel_types.size() == 1) {
    bw.WriteBits(1, 2);  // selector 1 = 1 channel
  } else {
    bw.WriteBits(2, 2);  // selector 2 = BitsOffset(4, 2)
    bw.WriteBits(static_cast<uint32_t>(extra_channel_types.size() - 2), 4);
  }

  // For each extra channel
  for (uint32_t ec_type : extra_channel_types) {
    bw.WriteBits(0, 1);  // all_default = false
    // type: U32(Val(0), Val(1), BitsOffset(4, 2), BitsOffset(6, 18))
    if (ec_type == 0) {
      bw.WriteBits(0, 2);  // Alpha
    } else if (ec_type == 1) {
      bw.WriteBits(1, 2);  // Depth
    } else {
      bw.WriteBits(2, 2);  // BitsOffset(4, 2)
      bw.WriteBits(ec_type - 2, 4);
    }
    // bit_depth: floating_point=0, 8-bit (selector 0)
    bw.WriteBits(0, 1);
    bw.WriteBits(0, 2);
    // dim_shift: selector 0
    bw.WriteBits(0, 2);
    // name_len: selector 0 (empty name)
    bw.WriteBits(0, 2);
    // Conditionals by type
    if (ec_type == 0) {
      bw.WriteBits(0, 1);  // alpha_associated = false
    } else if (ec_type == 2) {
      // SpotColor: 4x F16
      for (int i = 0; i < 4; ++i) {
        bw.WriteBits(0, 16);
      }
    } else if (ec_type == 5) {
      // CFA: cfa_channel = 0
      bw.WriteBits(0, 2);
    }
  }

  bw.WriteBits(0, 1);  // xyb_encoded = false
  bw.WriteBits(1, 1);  // colour_encoding: all_default = true
  bw.WriteBits(0, 2);  // extensions = 0
  bw.WriteBits(1, 1);  // default_m = true

  bw.Flush();
  // Pad with 32 zero bytes so decoder can process basic info
  std::vector<uint8_t> result = bw.Bytes();
  result.insert(result.end(), 32, 0);
  return result;
}

// Helper to create an in-memory MEM dataset with test pattern.
std::unique_ptr<GDALDataset> CreateMemDataset(int width,
                                              int height,
                                              int bands,
                                              GDALDataType type) {
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  if (mem_driver == nullptr) return nullptr;

  auto ds = std::unique_ptr<GDALDataset>(
      mem_driver->Create("", width, height, bands, type, nullptr));
  if (ds == nullptr) return nullptr;

  for (int b = 1; b <= bands; ++b) {
    GDALRasterBand* band = ds->GetRasterBand(b);
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const double val = static_cast<double>((b * 10 + x + y * width) % 250 + 1);
        CPLErr err = band->RasterIO(GF_Write, x, y, 1, 1, const_cast<double*>(&val), 1, 1,
                                    GDT_Float64, 0, 0);
        if (err != CE_None) return nullptr;
      }
    }
  }
  return ds;
}

class JpegxlTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_JPEGXL();
    GDALRegister_MEM();
    GDALRegister_JPEG();
    // Needed by GDALJP2Metadata::CreateJP2GeoTIFF() when writing GeoJP2 jumb
    // boxes (jpegxl.cpp:1815) and for creating .msk files.
    GDALRegister_GTiff();
  }
};

TEST_F(JpegxlTest, DriverRegistrationAndIdentify) {
  // Test duplicate registration early return
  GDALRegister_JPEGXL();

  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };

  GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("JPEGXL");
  ASSERT_THAT(driver, NotNull());

  // Non-existent file
  GDALOpenInfo missing("/does_not_exist", GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(FALSE, driver->pfnIdentify(&missing));
  EXPECT_EQ(nullptr, driver->pfnOpen(&missing));

  // .jxl extension
  const std::string jxl_path = std::format("{}/file.jxl", kVsiDir);
  VsiMemTempWrapper jxl_file(jxl_path, "test");
  GDALOpenInfo jxl_info(jxl_path.c_str(), GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(TRUE, driver->pfnIdentify(&jxl_info));

  // Container signature on .bin file
  const std::string container_path = std::format("{}/container.bin", kVsiDir);
  VsiMemTempWrapper container_file(
      container_path,
      std::string(reinterpret_cast<const char*>(kJxlContainerSig.data()),
                  kJxlContainerSig.size()));
  GDALOpenInfo container_info(container_path.c_str(), GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(TRUE, driver->pfnIdentify(&container_info));

  // Valid raw codestream on .bin file
  std::vector<uint8_t> valid_cs =
      BuildSyntheticJxlCodestream(true, 8, 1, false, 8, 0, {});
  const std::string raw_path = std::format("{}/raw.bin", kVsiDir);
  VsiMemTempWrapper raw_file(raw_path,
                             std::string(reinterpret_cast<const char*>(
                                             valid_cs.data()),
                                         valid_cs.size()));
  GDALOpenInfo raw_info(raw_path.c_str(), GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(TRUE, driver->pfnIdentify(&raw_info));

  // Truncated codestream on .bin file (\xff\x0a 2 bytes only)
  const std::string bad_cs_path = std::format("{}/bad_raw.bin", kVsiDir);
  VsiMemTempWrapper bad_cs_file(bad_cs_path, std::string("\xff\x0a", 2));
  GDALOpenInfo bad_cs_info(bad_cs_path.c_str(), GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(FALSE, driver->pfnIdentify(&bad_cs_info));

  // Random non-matching .bin file
  const std::string unrelated_path = std::format("{}/other.bin", kVsiDir);
  VsiMemTempWrapper unrelated_file(unrelated_path, "not a jxl file");
  GDALOpenInfo unrelated_info(unrelated_path.c_str(), GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(FALSE, driver->pfnIdentify(&unrelated_info));
}

TEST_F(JpegxlTest, OpenErrorPathsAndSynthetics) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };

  // Empty .jxl file -> bGotInfo == false -> "Did not get basic info"
  {
    const std::string empty_path = std::format("{}/empty.jxl", kVsiDir);
    VsiMemTempWrapper empty_file(empty_path, "");
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(empty_path.c_str(), GDAL_OF_READONLY));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Did not get basic info"));
    CPLErrorReset();
    EXPECT_EQ(nullptr, ds);
  }

  // 12-byte container only
  {
    const std::string cont_path = std::format("{}/cont_only.jxl", kVsiDir);
    VsiMemTempWrapper cont_file(
        cont_path,
        std::string(reinterpret_cast<const char*>(kJxlContainerSig.data()),
                    kJxlContainerSig.size()));
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(cont_path.c_str(), GDAL_OF_READONLY));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Did not get basic info"));
    CPLErrorReset();
    EXPECT_EQ(nullptr, ds);
  }

  // Corrupt container payload
  {
    constexpr auto kCorruptContainer = std::to_array<uint8_t>({
        0x00, 0x00, 0x00, 0x0C, 'J', 'X', 'L', ' ', 0x0D, 0x0A, 0x87, 0x0A,
        0x00, 0x00, 0x00, 0x10, 'j', 'x', 'l', 'c', 0xFF, 0xFF, 0xFF, 0xFF,
    });
    static_assert(kCorruptContainer.size() == 24);
    const std::string corrupt_path = std::format("{}/corrupt.jxl", kVsiDir);
    VsiMemTempWrapper corrupt_file(
        corrupt_path,
        std::string(reinterpret_cast<const char*>(kCorruptContainer.data()),
                    kCorruptContainer.size()));
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(corrupt_path.c_str(), GDAL_OF_READONLY));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Did not get basic info"));
    CPLErrorReset();
    EXPECT_EQ(nullptr, ds);
  }

  // Synthetic codestream where xsize * ysize * nChannels * sizeof(float) > SIZE_MAX
  // (ysize = 2^30, ratio 4 = 3:2 -> xsize = 1610612736, 3 RGB channels, Float32).
  // Open succeeds; calling dataset RasterIO with nBandCount=1 executes the
  // size_t overflow check in GetDecodedImage() without allocating memory.
  {
    std::vector<uint8_t> huge_f32 =
        BuildSyntheticJxlCodestream(false, 1073741824, 4, true, 32, 8, {});
    const std::string huge_f32_path = std::format("{}/huge_f32.jxl", kVsiDir);
    VsiMemTempWrapper huge_f32_file(
        huge_f32_path,
        std::string(reinterpret_cast<const char*>(huge_f32.data()),
                    huge_f32.size()));
    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(huge_f32_path.c_str(), GDAL_OF_READONLY));
    ASSERT_THAT(ds, NotNull());
    EXPECT_EQ(1610612736, ds->GetRasterXSize());
    EXPECT_EQ(1073741824, ds->GetRasterYSize());
    float pixel = 0.0f;
    int band_map[1] = {1};
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              ds->RasterIO(GF_Read, 0, 0, ds->GetRasterXSize(),
                           ds->GetRasterYSize(), &pixel, 1, 1, GDT_Float32, 1,
                           band_map, 0, 0, 0, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Image too big for architecture"));
    CPLErrorReset();
  }

  // Synthetic codestream with xsize > INT_MAX -> "Too big raster"
  {
    // ysize = 2^30, ratio = 7 (2:1) -> xsize = 2^31 > INT_MAX
    std::vector<uint8_t> too_big =
        BuildSyntheticJxlCodestream(false, 1073741824, 7, false, 8, 0, {});
    const std::string too_big_path = std::format("{}/too_big.jxl", kVsiDir);
    VsiMemTempWrapper too_big_file(
        too_big_path, std::string(reinterpret_cast<const char*>(too_big.data()),
                                  too_big.size()));
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(too_big_path.c_str(), GDAL_OF_READONLY));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Too big raster"));
    CPLErrorReset();
    EXPECT_EQ(nullptr, ds);
  }

  // Synthetic codestream with 24-bit integer -> "Unhandled data type"
  {
    std::vector<uint8_t> int24 =
        BuildSyntheticJxlCodestream(true, 8, 1, false, 24, 0, {});
    const std::string int24_path = std::format("{}/int24.jxl", kVsiDir);
    VsiMemTempWrapper int24_file(
        int24_path, std::string(reinterpret_cast<const char*>(int24.data()),
                                int24.size()));
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(int24_path.c_str(), GDAL_OF_READONLY));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Unhandled data type"));
    CPLErrorReset();
    EXPECT_EQ(nullptr, ds);
  }

  // Synthetic codestream with Float16 -> GDT_Float32
  {
    std::vector<uint8_t> f16 =
        BuildSyntheticJxlCodestream(true, 8, 1, true, 16, 5, {});
    const std::string f16_path = std::format("{}/f16.jxl", kVsiDir);
    VsiMemTempWrapper f16_file(
        f16_path, std::string(reinterpret_cast<const char*>(f16.data()),
                              f16.size()));
    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(f16_path.c_str(), GDAL_OF_READONLY));
    ASSERT_THAT(ds, NotNull());
    EXPECT_EQ(8, ds->GetRasterXSize());
    EXPECT_EQ(8, ds->GetRasterYSize());
    EXPECT_EQ(GDT_Float32, ds->GetRasterBand(1)->GetRasterDataType());

    // Calling ReadBlock on header-only file triggers GetDecodedImage decode error
    std::vector<float> buf(8);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_NE(CE_None, ds->GetRasterBand(1)->ReadBlock(0, 0, buf.data()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Decoding error"));
    CPLErrorReset();
    // Second call hits m_bDecodingFailed early return
    EXPECT_NE(CE_None, ds->GetRasterBand(1)->ReadBlock(0, 0, buf.data()));
    CPLErrorReset();
  }

  // Synthetic codestream with all extra channel types
  {
    // Types: 1=Depth, 2=SpotColor, 3=SelectionMask, 4=Black, 5=CFA, 6=Thermal, 16=Optional
    std::vector<uint32_t> extra_types = {1, 2, 3, 4, 5, 6, 16};
    std::vector<uint8_t> ec =
        BuildSyntheticJxlCodestream(true, 8, 1, false, 8, 0, extra_types);
    const std::string ec_path = std::format("{}/extra_channels.jxl", kVsiDir);
    VsiMemTempWrapper ec_file(
        ec_path, std::string(reinterpret_cast<const char*>(ec.data()),
                             ec.size()));
    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(ec_path.c_str(), GDAL_OF_READONLY));
    ASSERT_THAT(ds, NotNull());
    EXPECT_EQ(3 + static_cast<int>(extra_types.size()), ds->GetRasterCount());
    EXPECT_STREQ("Depth channel", ds->GetRasterBand(4)->GetDescription());
    EXPECT_STREQ("Spot color channel", ds->GetRasterBand(5)->GetDescription());
    EXPECT_STREQ("Selection mask channel", ds->GetRasterBand(6)->GetDescription());
    EXPECT_STREQ("Black channel", ds->GetRasterBand(7)->GetDescription());
    EXPECT_STREQ("CFA channel", ds->GetRasterBand(8)->GetDescription());
    EXPECT_STREQ("Thermal channel", ds->GetRasterBand(9)->GetDescription());
  }
}

TEST_F(JpegxlTest, DataTypesAndBandCombinations) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALDriver* jxl_driver = GetGDALDriverManager()->GetDriverByName("JPEGXL");
  ASSERT_THAT(jxl_driver, NotNull());

  // Test various band configurations:
  // 1-band Gray, 2-band Gray+Alpha, 2-band Gray+Undefined, 3-band RGB,
  // 3-band non-RGB, 4-band RGBA, 5-band RGB+Alpha+Undefined (with description),
  // 7-band RGBA + 3 extra channels
  const struct BandConfig {
    int bands;
    std::vector<GDALColorInterp> interps;
    std::string desc;
  } kConfigs[] = {
      {1, {GCI_GrayIndex}, ""},
      {2, {GCI_GrayIndex, GCI_AlphaBand}, ""},
      {2, {GCI_GrayIndex, GCI_Undefined}, ""},
      {3, {GCI_RedBand, GCI_GreenBand, GCI_BlueBand}, ""},
      {3, {GCI_GrayIndex, GCI_Undefined, GCI_Undefined}, ""},
      {4, {GCI_RedBand, GCI_GreenBand, GCI_BlueBand, GCI_AlphaBand}, ""},
      {5,
       {GCI_RedBand, GCI_GreenBand, GCI_BlueBand, GCI_AlphaBand,
        GCI_Undefined},
       "custom_extra_channel"},
      {7,
       {GCI_RedBand, GCI_GreenBand, GCI_BlueBand, GCI_AlphaBand,
        GCI_Undefined, GCI_Undefined, GCI_Undefined},
       ""},
  };

  for (size_t idx = 0; idx < std::size(kConfigs); ++idx) {
    const auto& cfg = kConfigs[idx];
    SCOPED_TRACE(std::format("config_{}_bands_{}", idx, cfg.bands));
    auto src_ds = CreateMemDataset(8, 8, cfg.bands, GDT_Byte);
    ASSERT_THAT(src_ds, NotNull());

    for (size_t b = 0; b < cfg.interps.size(); ++b) {
      src_ds->GetRasterBand(static_cast<int>(b + 1))->SetColorInterpretation(
          cfg.interps[b]);
    }
    if (!cfg.desc.empty()) {
      src_ds->GetRasterBand(cfg.bands)->SetDescription(cfg.desc.c_str());
    }

    const std::string out_path =
        std::format("{}/bands_{}.jxl", kVsiDir, cfg.bands);
    auto jxl_ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(out_path.c_str(), src_ds.get(), FALSE, nullptr,
                               nullptr, nullptr));
    ASSERT_THAT(jxl_ds, NotNull());
    jxl_ds.reset();

    auto read_ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(out_path.c_str(), GDAL_OF_READONLY));
    ASSERT_THAT(read_ds, NotNull());
    EXPECT_EQ(cfg.bands, read_ds->GetRasterCount());
    if (!cfg.desc.empty()) {
      EXPECT_STREQ(cfg.desc.c_str(),
                   read_ds->GetRasterBand(cfg.bands)->GetDescription());
    }
  }

  // UInt16 lossless copy (exercises codestream level 10 >12 bits)
  {
    auto src_ds = CreateMemDataset(8, 8, 3, GDT_UInt16);
    const std::string uint16_path = std::format("{}/uint16.jxl", kVsiDir);
    auto jxl_ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(uint16_path.c_str(), src_ds.get(), FALSE,
                               nullptr, nullptr, nullptr));
    ASSERT_THAT(jxl_ds, NotNull());
    jxl_ds.reset();

    auto read_ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(uint16_path.c_str(), GDAL_OF_READONLY));
    ASSERT_THAT(read_ds, NotNull());
    EXPECT_EQ(GDT_UInt16, read_ds->GetRasterBand(1)->GetRasterDataType());
  }

  // Float32 lossless copy
  {
    auto src_ds = CreateMemDataset(8, 8, 1, GDT_Float32);
    const std::string float_path = std::format("{}/float32.jxl", kVsiDir);
    auto jxl_ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(float_path.c_str(), src_ds.get(), FALSE,
                               nullptr, nullptr, nullptr));
    ASSERT_THAT(jxl_ds, NotNull());
    jxl_ds.reset();

    auto read_ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(float_path.c_str(), GDAL_OF_READONLY));
    ASSERT_THAT(read_ds, NotNull());
    EXPECT_EQ(GDT_Float32, read_ds->GetRasterBand(1)->GetRasterDataType());
  }
}

TEST_F(JpegxlTest, NBitsRescaling) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALDriver* jxl_driver = GetGDALDriverManager()->GetDriverByName("JPEGXL");
  ASSERT_THAT(jxl_driver, NotNull());

  // 2-band GDT_Byte (base Gray + extra Undefined) with NBITS=7
  {
    auto src_ds = CreateMemDataset(8, 8, 2, GDT_Byte);
    src_ds->GetRasterBand(1)->SetColorInterpretation(GCI_GrayIndex);
    src_ds->GetRasterBand(2)->SetColorInterpretation(GCI_Undefined);
    // Fill with values within 7-bit range [0..127]
    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 8; ++x) {
        uint8_t val1 = static_cast<uint8_t>((x + y * 8) % 120);
        uint8_t val2 = static_cast<uint8_t>((x * 2 + y) % 120);
        EXPECT_EQ(CE_None, src_ds->GetRasterBand(1)->RasterIO(
                               GF_Write, x, y, 1, 1, &val1, 1, 1, GDT_Byte, 0,
                               0));
        EXPECT_EQ(CE_None, src_ds->GetRasterBand(2)->RasterIO(
                               GF_Write, x, y, 1, 1, &val2, 1, 1, GDT_Byte, 0,
                               0));
      }
    }

    CPLStringList options;
    options.AddNameValue("NBITS", "7");
    const std::string out_path = std::format("{}/nbits_7.jxl", kVsiDir);
    auto jxl_ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(out_path.c_str(), src_ds.get(), FALSE,
                               options.List(), nullptr, nullptr));
    ASSERT_THAT(jxl_ds, NotNull());
    jxl_ds.reset();

    auto read_ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(out_path.c_str(), GDAL_OF_READONLY));
    ASSERT_THAT(read_ds, NotNull());
    EXPECT_STREQ("7", read_ds->GetRasterBand(1)->GetMetadataItem(
                          "NBITS", "IMAGE_STRUCTURE"));
    EXPECT_STREQ("7", read_ds->GetRasterBand(2)->GetMetadataItem(
                          "NBITS", "IMAGE_STRUCTURE"));

    // Verify ReadBlock on both bands with sentinel pre-fill
    std::vector<uint8_t> block1(8, 0xFE);
    std::vector<uint8_t> block2(8, 0xFE);
    EXPECT_EQ(CE_None,
              read_ds->GetRasterBand(1)->ReadBlock(0, 0, block1.data()));
    EXPECT_EQ(CE_None,
              read_ds->GetRasterBand(2)->ReadBlock(0, 0, block2.data()));
    EXPECT_EQ(0, block1[0]);
    EXPECT_EQ(1, block1[1]);
    EXPECT_EQ(7, block1[7]);
    EXPECT_EQ(0, block2[0]);
    EXPECT_EQ(2, block2[1]);
    EXPECT_EQ(14, block2[7]);
  }

  // 2-band GDT_UInt16 with NBITS=12
  {
    auto src_ds = CreateMemDataset(8, 8, 2, GDT_UInt16);
    src_ds->GetRasterBand(1)->SetColorInterpretation(GCI_GrayIndex);
    src_ds->GetRasterBand(2)->SetColorInterpretation(GCI_Undefined);

    CPLStringList options;
    options.AddNameValue("NBITS", "12");
    const std::string out_path = std::format("{}/nbits_12.jxl", kVsiDir);
    auto jxl_ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(out_path.c_str(), src_ds.get(), FALSE,
                               options.List(), nullptr, nullptr));
    ASSERT_THAT(jxl_ds, NotNull());
    jxl_ds.reset();

    auto read_ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(out_path.c_str(), GDAL_OF_READONLY));
    ASSERT_THAT(read_ds, NotNull());
    EXPECT_STREQ("12", read_ds->GetRasterBand(1)->GetMetadataItem(
                           "NBITS", "IMAGE_STRUCTURE"));

    std::vector<uint16_t> block1(8, 0xDEAD);
    std::vector<uint16_t> block2(8, 0xDEAD);
    EXPECT_EQ(CE_None,
              read_ds->GetRasterBand(1)->ReadBlock(0, 0, block1.data()));
    EXPECT_EQ(CE_None,
              read_ds->GetRasterBand(2)->ReadBlock(0, 0, block2.data()));
    EXPECT_EQ(11, block1[0]);
    EXPECT_EQ(12, block1[1]);
    EXPECT_EQ(18, block1[7]);
    EXPECT_EQ(21, block2[0]);
    EXPECT_EQ(22, block2[1]);
    EXPECT_EQ(28, block2[7]);
  }
}

TEST_F(JpegxlTest, IReadBlockAndIRasterIO) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALDriver* jxl_driver = GetGDALDriverManager()->GetDriverByName("JPEGXL");
  ASSERT_THAT(jxl_driver, NotNull());

  // 1-band dataset full-window band RasterIO delegates to dataset IRasterIO
  {
    auto src_ds = CreateMemDataset(8, 8, 1, GDT_Byte);
    const std::string out_path = std::format("{}/band_rasterio_1b.jxl", kVsiDir);
    auto jxl_ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(out_path.c_str(), src_ds.get(), FALSE, nullptr,
                               nullptr, nullptr));
    ASSERT_THAT(jxl_ds, NotNull());
    jxl_ds.reset();

    auto read_ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(out_path.c_str(), GDAL_OF_READONLY));
    ASSERT_THAT(read_ds, NotNull());

    // Full window band RasterIO
    std::vector<uint8_t> full_buf(64, 0xFF);
    EXPECT_EQ(CE_None, read_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 8, 8, full_buf.data(), 8, 8,
                           GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(11, full_buf[0]);
    EXPECT_EQ(12, full_buf[1]);
    EXPECT_EQ(74, full_buf[63]);

    // Partial window band RasterIO
    std::vector<uint8_t> partial_buf(16, 0xFF);
    EXPECT_EQ(CE_None, read_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 1, 1, 4, 4, partial_buf.data(), 4, 4,
                           GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(20, partial_buf[0]);
  }

  // 5-band dataset (3 RGB + 1 Alpha + 1 Undefined) to exercise all dataset IRasterIO paths
  {
    auto src_ds = CreateMemDataset(8, 8, 5, GDT_Byte);
    src_ds->GetRasterBand(1)->SetColorInterpretation(GCI_RedBand);
    src_ds->GetRasterBand(2)->SetColorInterpretation(GCI_GreenBand);
    src_ds->GetRasterBand(3)->SetColorInterpretation(GCI_BlueBand);
    src_ds->GetRasterBand(4)->SetColorInterpretation(GCI_AlphaBand);
    src_ds->GetRasterBand(5)->SetColorInterpretation(GCI_Undefined);

    const std::string out_path = std::format("{}/rasterio_5b.jxl", kVsiDir);
    auto jxl_ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(out_path.c_str(), src_ds.get(), FALSE, nullptr,
                               nullptr, nullptr));
    ASSERT_THAT(jxl_ds, NotNull());
    jxl_ds.reset();

    auto read_ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(out_path.c_str(), GDAL_OF_READONLY));
    ASSERT_THAT(read_ds, NotNull());

    // 1. Direct decode / copy words into pixel-interleaved buffer across first 3 bands
    std::vector<uint8_t> rgb_interleaved(8 * 8 * 3, 0xFF);
    int band_map_3[3] = {1, 2, 3};
    EXPECT_EQ(CE_None, read_ds->RasterIO(GF_Read, 0, 0, 8, 8,
                                         rgb_interleaved.data(), 8, 8, GDT_Byte,
                                         3, band_map_3, 3, 3 * 8, 1, nullptr));
    EXPECT_EQ(11, rgb_interleaved[0]);
    EXPECT_EQ(21, rgb_interleaved[1]);
    EXPECT_EQ(31, rgb_interleaved[2]);

    // 2. GDALCopyWords64 path: reading UInt16 into pixel interleaved buffer
    std::vector<uint16_t> rgb_uint16(8 * 8 * 3, 0xFFFF);
    EXPECT_EQ(CE_None,
              read_ds->RasterIO(GF_Read, 0, 0, 8, 8, rgb_uint16.data(), 8, 8,
                                GDT_UInt16, 3, band_map_3, 3 * sizeof(uint16_t),
                                3 * 8 * sizeof(uint16_t), sizeof(uint16_t),
                                nullptr));
    EXPECT_EQ(11, rgb_uint16[0]);
    EXPECT_EQ(21, rgb_uint16[1]);
    EXPECT_EQ(31, rgb_uint16[2]);

    // 3. General per-band/per-line loop with non-sequential band map spanning base and extra bands
    int band_map_custom[4] = {5, 4, 1, 2};
    std::vector<uint8_t> band_seq_buf(8 * 8 * 4, 0xFF);
    EXPECT_EQ(CE_None,
              read_ds->RasterIO(GF_Read, 0, 0, 8, 8, band_seq_buf.data(), 8, 8,
                                GDT_Byte, 4, band_map_custom, 1, 8, 64, nullptr));
    EXPECT_EQ(51, band_seq_buf[0]);
    EXPECT_EQ(41, band_seq_buf[64]);
    EXPECT_EQ(11, band_seq_buf[128]);
    EXPECT_EQ(21, band_seq_buf[192]);

    // 4. Subsampled read (nXSize != nBufXSize) -> falls back to GDALPamDataset::IRasterIO
    std::vector<uint8_t> subsampled_buf(4 * 4 * 5, 0xFF);
    int band_map_5[5] = {1, 2, 3, 4, 5};
    EXPECT_EQ(CE_None,
              read_ds->RasterIO(GF_Read, 0, 0, 8, 8, subsampled_buf.data(), 4,
                                4, GDT_Byte, 5, band_map_5, 1, 4, 16, nullptr));
    EXPECT_GT(subsampled_buf[0], 0);

    // 5. GF_Write on read-only dataset fails gracefully
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_NE(CE_None,
              read_ds->RasterIO(GF_Write, 0, 0, 8, 8, band_seq_buf.data(), 8, 8,
                                GDT_Byte, 4, band_map_custom, 1, 8, 64, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("read-only mode"));
    CPLErrorReset();
  }
}

TEST_F(JpegxlTest, BoxesAndMetadata) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALDriver* jxl_driver = GetGDALDriverManager()->GetDriverByName("JPEGXL");
  ASSERT_THAT(jxl_driver, NotNull());

  // XMP metadata (uncompressed and compressed boxes)
  {
    auto src_ds = CreateMemDataset(8, 8, 1, GDT_Byte);
    const char* kXmp =
        "<?xpacket begin='' id='W5M0MpCehiHzreSzNTczkc9d'?>\n"
        "<x:xmpmeta xmlns:x='adobe:ns:meta/'>\n"
        " <rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>\n"
        "  <rdf:Description rdf:about='' xmlns:dc='http://purl.org/dc/elements/1.1/'>\n"
        "   <dc:title>Test XMP Title</dc:title>\n"
        "  </rdf:Description>\n"
        " </rdf:RDF>\n"
        "</x:xmpmeta>\n"
        "<?xpacket end='w'?>";
    char* xmp_list[] = {const_cast<char*>(kXmp), nullptr};
    src_ds->SetMetadata(xmp_list, "xml:XMP");

    for (const char* compress : {"NO", "YES"}) {
      SCOPED_TRACE(compress);
      CPLStringList options;
      options.AddNameValue("COMPRESS_BOXES", compress);
      const std::string out_path =
          std::format("{}/xmp_comp_{}.jxl", kVsiDir, compress);
      auto jxl_ds = std::unique_ptr<GDALDataset>(
          jxl_driver->CreateCopy(out_path.c_str(), src_ds.get(), FALSE,
                                 options.List(), nullptr, nullptr));
      ASSERT_THAT(jxl_ds, NotNull());
      jxl_ds.reset();

      auto read_ds = std::unique_ptr<GDALDataset>(
          GDALDataset::Open(out_path.c_str(), GDAL_OF_READONLY));
      ASSERT_THAT(read_ds, NotNull());
      StringListPtr domains(read_ds->GetMetadataDomainList(), CSLDestroy);
      EXPECT_THAT(CSLFindString(domains.get(), "xml:XMP"), ::testing::Ge(0));
      char** xmp_meta = read_ds->GetMetadata("xml:XMP");
      ASSERT_THAT(xmp_meta, NotNull());
      EXPECT_THAT(xmp_meta[0], HasSubstr("Test XMP Title"));
      EXPECT_EQ(nullptr, read_ds->GetMetadata("NON_EXISTENT_DOMAIN"));
    }
  }

  // EXIF metadata and Orientation
  {
    auto src_ds = CreateMemDataset(4, 6, 1, GDT_Byte);
    src_ds->SetMetadataItem("EXIF_Make", "Google", "EXIF");
    src_ds->SetMetadataItem("EXIF_Orientation", "6", "EXIF");

    const std::string out_path = std::format("{}/exif.jxl", kVsiDir);
    auto jxl_ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(out_path.c_str(), src_ds.get(), FALSE, nullptr,
                               nullptr, nullptr));
    ASSERT_THAT(jxl_ds, NotNull());
    jxl_ds.reset();

    // Re-open with APPLY_ORIENTATION=NO
    CPLStringList open_options_no;
    open_options_no.AddNameValue("APPLY_ORIENTATION", "NO");
    auto ds_no = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(out_path.c_str(), GDAL_OF_READONLY, nullptr,
                          open_options_no.List(), nullptr));
    ASSERT_THAT(ds_no, NotNull());
    EXPECT_EQ(4, ds_no->GetRasterXSize());
    EXPECT_EQ(6, ds_no->GetRasterYSize());
    EXPECT_STREQ("Google", ds_no->GetMetadataItem("EXIF_Make", "EXIF"));
    EXPECT_STREQ("6", ds_no->GetMetadataItem("EXIF_Orientation", "EXIF"));

    // Re-open with APPLY_ORIENTATION=YES (orientation 6 transposes 4x6 -> 6x4)
    CPLStringList open_options_yes;
    open_options_yes.AddNameValue("APPLY_ORIENTATION", "YES");
    auto ds_yes = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(out_path.c_str(), GDAL_OF_READONLY, nullptr,
                          open_options_yes.List(), nullptr));
    ASSERT_THAT(ds_yes, NotNull());
    EXPECT_EQ(6, ds_yes->GetRasterXSize());
    EXPECT_EQ(4, ds_yes->GetRasterYSize());
    EXPECT_STREQ("6", ds_yes->GetMetadataItem("original_EXIF_Orientation", "EXIF"));
    EXPECT_EQ(nullptr, ds_yes->GetMetadataItem("NONEXISTENT", "EXIF"));
    char** exif_list = ds_yes->GetMetadata("EXIF");
    ASSERT_THAT(exif_list, NotNull());
    EXPECT_THAT(CSLFindString(exif_list, "original_EXIF_Orientation=6"),
                ::testing::Ge(0));
  }

  // EXIF metadata from default main domain (EXIF_* keys)
  {
    auto src_ds = CreateMemDataset(4, 4, 1, GDT_Byte);
    src_ds->SetMetadataItem("EXIF_Model", "PixelTest");
    const std::string out_path = std::format("{}/exif_main.jxl", kVsiDir);
    auto jxl_ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(out_path.c_str(), src_ds.get(), FALSE, nullptr,
                               nullptr, nullptr));
    ASSERT_THAT(jxl_ds, NotNull());
    jxl_ds.reset();

    auto read_ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(out_path.c_str(), GDAL_OF_READONLY));
    ASSERT_THAT(read_ds, NotNull());
    EXPECT_STREQ("PixelTest", read_ds->GetMetadataItem("EXIF_Model", "EXIF"));
  }

  // Big-endian TIFF EXIF box injected into JXL container (exercises bSwab)
  {
    // First create a minimal container JXL
    auto src_ds = CreateMemDataset(4, 4, 1, GDT_Byte);
    const std::string base_path = std::format("{}/base_cont.jxl", kVsiDir);
    auto jxl_ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(base_path.c_str(), src_ds.get(), FALSE, nullptr,
                               nullptr, nullptr));
    ASSERT_THAT(jxl_ds, NotNull());
    jxl_ds.reset();

    // Read file bytes
    vsi_l_offset nSize = 0;
    GByte* pabyData = VSIGetMemFileBuffer(base_path.c_str(), &nSize, FALSE);
    ASSERT_THAT(pabyData, NotNull());

    std::vector<uint8_t> cont_bytes(std::begin(kJxlContainerSig),
                                    std::end(kJxlContainerSig));
    const char kFtypPayload[12] = {'j', 'x', 'l', ' ', 0, 0, 0, 0,
                                   'j', 'x', 'l', ' '};
    std::vector<uint8_t> ftyp_box = MakeIsobmffBox("ftyp", kFtypPayload, 12);
    cont_bytes.insert(cont_bytes.end(), ftyp_box.begin(), ftyp_box.end());

    std::vector<uint8_t> exif_payload = BuildTiffExifBoxPayload(6);
    std::vector<uint8_t> exif_box =
        MakeIsobmffBox("Exif", exif_payload.data(), exif_payload.size());
    cont_bytes.insert(cont_bytes.end(), exif_box.begin(), exif_box.end());

    std::vector<uint8_t> jxlc_box = MakeIsobmffBox("jxlc", pabyData, nSize);
    cont_bytes.insert(cont_bytes.end(), jxlc_box.begin(), jxlc_box.end());

    const std::string swab_path = std::format("{}/swab_exif.jxl", kVsiDir);
    VsiMemTempWrapper swab_file(
        swab_path, std::string(reinterpret_cast<const char*>(cont_bytes.data()),
                               cont_bytes.size()));

    auto read_ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(swab_path.c_str(), GDAL_OF_READONLY));
    ASSERT_THAT(read_ds, NotNull());
    EXPECT_STREQ("6", read_ds->GetMetadataItem("EXIF_Orientation", "EXIF"));
  }

  // GeoJP2 (jumb box) with SRS, GeoTransform, AREA_OR_POINT, GCPs, and RPC
  {
    auto src_ds = CreateMemDataset(8, 8, 1, GDT_Byte);
    OGRSpatialReference srs;
    ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    ASSERT_EQ(CE_None, src_ds->SetSpatialRef(&srs));

    GDALGeoTransform gt{10.0, 1.0, 0.0, 20.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, src_ds->SetGeoTransform(gt));
    ASSERT_EQ(CE_None,
              src_ds->SetMetadataItem(GDALMD_AREA_OR_POINT, GDALMD_AOP_POINT));

    const std::string geo_path = std::format("{}/georef.jxl", kVsiDir);
    auto jxl_ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(geo_path.c_str(), src_ds.get(), FALSE, nullptr,
                               nullptr, nullptr));
    ASSERT_THAT(jxl_ds, NotNull());
    jxl_ds.reset();

    auto read_ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(geo_path.c_str(), GDAL_OF_READONLY));
    ASSERT_THAT(read_ds, NotNull());
    const OGRSpatialReference* read_srs = read_ds->GetSpatialRef();
    ASSERT_THAT(read_srs, NotNull());
    EXPECT_THAT(*read_srs, IsSameAs(srs));

    GDALGeoTransform read_gt;
    EXPECT_EQ(CE_None, read_ds->GetGeoTransform(read_gt));
    for (int i = 0; i < 6; ++i) {
      EXPECT_DOUBLE_EQ(gt[i], read_gt[i]);
    }
  }

  // GCPs and RPC metadata in GeoJP2
  {
    auto src_ds = CreateMemDataset(8, 8, 1, GDT_Byte);
    GDAL_GCP gcp;
    GDALInitGCPs(1, &gcp);
    absl::Cleanup gcp_cleanup = [&gcp] { GDALDeinitGCPs(1, &gcp); };
    gcp.dfGCPPixel = 0;
    gcp.dfGCPLine = 0;
    gcp.dfGCPX = 100;
    gcp.dfGCPY = 200;
    gcp.dfGCPZ = 0;
    OGRSpatialReference srs;
    ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
    ASSERT_EQ(CE_None, src_ds->SetGCPs(1, &gcp, &srs));

    ASSERT_EQ(CE_None, src_ds->SetMetadataItem("LINE_OFF", "100", "RPC"));

    const std::string rpc_path = std::format("{}/gcps_rpc.jxl", kVsiDir);
    auto jxl_ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(rpc_path.c_str(), src_ds.get(), FALSE, nullptr,
                               nullptr, nullptr));
    ASSERT_THAT(jxl_ds, NotNull());
    jxl_ds.reset();

    auto read_ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(rpc_path.c_str(), GDAL_OF_READONLY));
    ASSERT_THAT(read_ds, NotNull());
    EXPECT_EQ(1, read_ds->GetGCPCount());
  }

  // Worldfile fallback when no jumb box is present
  {
    auto src_ds = CreateMemDataset(8, 8, 1, GDT_Byte);
    const std::string no_geo_path = std::format("{}/no_geo.jxl", kVsiDir);
    auto jxl_ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(no_geo_path.c_str(), src_ds.get(), FALSE,
                               nullptr, nullptr, nullptr));
    ASSERT_THAT(jxl_ds, NotNull());
    jxl_ds.reset();

    // Create a .wld sidecar file in /vsimem/
    const std::string wld_path = std::format("{}/no_geo.wld", kVsiDir);
    VsiMemTempWrapper wld_file(wld_path, "1.0\n0.0\n0.0\n-1.0\n10.5\n20.5\n");

    auto read_ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(no_geo_path.c_str(), GDAL_OF_READONLY));
    ASSERT_THAT(read_ds, NotNull());
    GDALGeoTransform wld_gt;
    EXPECT_EQ(CE_None, read_ds->GetGeoTransform(wld_gt));
    EXPECT_DOUBLE_EQ(10.0, wld_gt[0]);
    EXPECT_DOUBLE_EQ(21.0, wld_gt[3]);
  }

  // GDAL_JPEGXL_MAX_BOX_BUFFER_SIZE tests
  {
    auto src_ds = CreateMemDataset(8, 8, 1, GDT_Byte);
    const char* kShortXmp =
        "<?xpacket begin='' id='W5M0MpCehiHzreSzNTczkc9d'?>\n"
        "<test/>\n"
        "<?xpacket end='w'?>";
    char* short_xmp_list[] = {const_cast<char*>(kShortXmp), nullptr};
    src_ds->SetMetadata(short_xmp_list, "xml:XMP");
    const std::string box_path = std::format("{}/box_limit.jxl", kVsiDir);
    auto jxl_ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(box_path.c_str(), src_ds.get(), FALSE, nullptr,
                               nullptr, nullptr));
    ASSERT_THAT(jxl_ds, NotNull());
    jxl_ds.reset();

    // Set limit very small (10 bytes) to skip uncompressed box
    CPLSetConfigOption("GDAL_JPEGXL_MAX_BOX_BUFFER_SIZE", "10");
    absl::Cleanup reset_opt = [] {
      CPLSetConfigOption("GDAL_JPEGXL_MAX_BOX_BUFFER_SIZE", nullptr);
    };

    WithQuietHandler quiet;
    CPLErrorReset();
    auto read_ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(box_path.c_str(), GDAL_OF_READONLY));
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("GDAL_JPEGXL_MAX_BOX_BUFFER_SIZE"));
    CPLErrorReset();
    ASSERT_THAT(read_ds, NotNull());
    // Box was skipped so XMP domain should not be present
    EXPECT_EQ(nullptr, read_ds->GetMetadata("xml:XMP"));
  }

  // Large compressed box (>1.1 MB uncompressed) growing buffer via JXL_DEC_BOX_NEED_MORE_OUTPUT
  {
    auto src_ds = CreateMemDataset(8, 8, 1, GDT_Byte);
    // Create ~1.1MB uncompressed XMP
    std::string huge_xmp = "<?xpacket begin='' id='W5M0MpCehiHzreSzNTczkc9d'?>\n";
    huge_xmp.append(1100 * 1024, ' ');
    huge_xmp.append("<?xpacket end='w'?>");
    char* huge_xmp_list[] = {huge_xmp.data(), nullptr};
    src_ds->SetMetadata(huge_xmp_list, "xml:XMP");

    CPLStringList comp_opts;
    comp_opts.AddNameValue("COMPRESS_BOXES", "YES");
    const std::string huge_path = std::format("{}/huge_xmp.jxl", kVsiDir);
    auto jxl_ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(huge_path.c_str(), src_ds.get(), FALSE,
                               comp_opts.List(), nullptr, nullptr));
    ASSERT_THAT(jxl_ds, NotNull());
    jxl_ds.reset();

    // Read back normally: exercises JXL_DEC_BOX_NEED_MORE_OUTPUT buffer growth
    auto read_ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(huge_path.c_str(), GDAL_OF_READONLY));
    ASSERT_THAT(read_ds, NotNull());
    char** meta = read_ds->GetMetadata("xml:XMP");
    ASSERT_THAT(meta, NotNull());
    EXPECT_THAT(meta[0], HasSubstr("<?xpacket"));

    // Now test with GDAL_JPEGXL_MAX_BOX_BUFFER_SIZE = 500000 to trigger compressed box skip
    CPLSetConfigOption("GDAL_JPEGXL_MAX_BOX_BUFFER_SIZE", "500000");
    absl::Cleanup reset_opt = [] {
      CPLSetConfigOption("GDAL_JPEGXL_MAX_BOX_BUFFER_SIZE", nullptr);
    };
    WithQuietHandler quiet;
    CPLErrorReset();
    auto read_ds_skip = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(huge_path.c_str(), GDAL_OF_READONLY));
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("GDAL_JPEGXL_MAX_BOX_BUFFER_SIZE"));
    CPLErrorReset();
    ASSERT_THAT(read_ds_skip, NotNull());
    EXPECT_EQ(nullptr, read_ds_skip->GetMetadata("xml:XMP"));
  }
}

TEST_F(JpegxlTest, ColorEncodingAndICCProfile) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALDriver* jxl_driver = GetGDALDriverManager()->GetDriverByName("JPEGXL");
  ASSERT_THAT(jxl_driver, NotNull());

  // Default sRGB -> no SOURCE_ICC_PROFILE
  {
    auto src_ds = CreateMemDataset(8, 8, 3, GDT_Byte);
    const std::string default_path = std::format("{}/default_srgb.jxl", kVsiDir);
    auto jxl_ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(default_path.c_str(), src_ds.get(), FALSE,
                               nullptr, nullptr, nullptr));
    ASSERT_THAT(jxl_ds, NotNull());
    jxl_ds.reset();

    auto read_ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(default_path.c_str(), GDAL_OF_READONLY));
    ASSERT_THAT(read_ds, NotNull());
    EXPECT_EQ(nullptr, read_ds->GetMetadataItem("SOURCE_ICC_PROFILE",
                                                "COLOR_PROFILE"));
  }

  // Valid ICC profile via SOURCE_ICC_PROFILE creation option
  {
    auto src_ds = CreateMemDataset(8, 8, 3, GDT_Byte);
    src_ds->GetRasterBand(1)->SetColorInterpretation(GCI_RedBand);
    src_ds->GetRasterBand(2)->SetColorInterpretation(GCI_GreenBand);
    src_ds->GetRasterBand(3)->SetColorInterpretation(GCI_BlueBand);

    CPLStringList options;
    options.AddNameValue("SOURCE_ICC_PROFILE", kSRGBIccBase64);
    const std::string icc_path = std::format("{}/icc.jxl", kVsiDir);
    auto jxl_ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(icc_path.c_str(), src_ds.get(), FALSE,
                               options.List(), nullptr, nullptr));
    ASSERT_THAT(jxl_ds, NotNull());
    jxl_ds.reset();

    auto read_ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(icc_path.c_str(), GDAL_OF_READONLY));
    ASSERT_THAT(read_ds, NotNull());
    EXPECT_STREQ(kSRGBIccBase64, read_ds->GetMetadataItem(
                                     "SOURCE_ICC_PROFILE", "COLOR_PROFILE"));
  }

  // Valid ICC profile via COLOR_PROFILE domain metadata
  {
    auto src_ds = CreateMemDataset(8, 8, 3, GDT_Byte);
    src_ds->GetRasterBand(1)->SetColorInterpretation(GCI_RedBand);
    src_ds->GetRasterBand(2)->SetColorInterpretation(GCI_GreenBand);
    src_ds->GetRasterBand(3)->SetColorInterpretation(GCI_BlueBand);
    src_ds->SetMetadataItem("SOURCE_ICC_PROFILE", kSRGBIccBase64,
                            "COLOR_PROFILE");
    const std::string icc_path = std::format("{}/icc_meta.jxl", kVsiDir);
    auto jxl_ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(icc_path.c_str(), src_ds.get(), FALSE, nullptr,
                               nullptr, nullptr));
    ASSERT_THAT(jxl_ds, NotNull());
    jxl_ds.reset();

    auto read_ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(icc_path.c_str(), GDAL_OF_READONLY));
    ASSERT_THAT(read_ds, NotNull());
    EXPECT_STREQ(kSRGBIccBase64, read_ds->GetMetadataItem(
                                     "SOURCE_ICC_PROFILE", "COLOR_PROFILE"));
  }

  // Invalid ICC profile fails cleanly
  {
    auto src_ds = CreateMemDataset(8, 8, 3, GDT_Byte);
    CPLStringList options;
    options.AddNameValue("SOURCE_ICC_PROFILE", "INVALID_BASE64_PROFILE");
    const std::string bad_icc_path = std::format("{}/bad_icc.jxl", kVsiDir);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto jxl_ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(bad_icc_path.c_str(), src_ds.get(), FALSE,
                               options.List(), nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("JxlEncoderSetICCProfile() failed"));
    CPLErrorReset();
    EXPECT_EQ(nullptr, jxl_ds);
  }
}

TEST_F(JpegxlTest, CreateCopyOptionsAndValidation) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALDriver* jxl_driver = GetGDALDriverManager()->GetDriverByName("JPEGXL");
  ASSERT_THAT(jxl_driver, NotNull());

  // 0-band source dataset
  {
    auto src_ds = CreateMemDataset(1, 1, 0, GDT_Byte);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = std::unique_ptr<GDALDataset>(jxl_driver->CreateCopy(
        std::format("{}/zero_band.jxl", kVsiDir).c_str(), src_ds.get(), FALSE,
        nullptr, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid source dataset"));
    CPLErrorReset();
    EXPECT_EQ(nullptr, ds);
  }

  // Unsupported data type (GDT_Int16)
  {
    auto src_ds = CreateMemDataset(1, 1, 1, GDT_Int16);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = std::unique_ptr<GDALDataset>(jxl_driver->CreateCopy(
        std::format("{}/int16.jxl", kVsiDir).c_str(), src_ds.get(), FALSE,
        nullptr, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Unsupported data type"));
    CPLErrorReset();
    EXPECT_EQ(nullptr, ds);
  }

  // Unwritable path
  {
    auto src_ds = CreateMemDataset(1, 1, 1, GDT_Byte);
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = std::unique_ptr<GDALDataset>(jxl_driver->CreateCopy(
        "/does_not_exist_dir/out.jxl", src_ds.get(), FALSE, nullptr, nullptr,
        nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot create"));
    CPLErrorReset();
    EXPECT_EQ(nullptr, ds);
  }

  // Mutually exclusive and invalid options
  struct InvalidOptionCase {
    std::vector<const char*> options;
    const char* expected_err;
  };
  const std::vector<InvalidOptionCase> kInvalidOptionSets = {
      {{"LOSSLESS=YES", "DISTANCE=1"}, "mutually exclusive"},
      {{"LOSSLESS=YES", "ALPHA_DISTANCE=1"}, "mutually exclusive"},
      {{"LOSSLESS=YES", "QUALITY=90"}, "mutually exclusive"},
      {{"DISTANCE=1", "QUALITY=90"}, "mutually exclusive"},
      {{"DISTANCE=-1"}, "JxlEncoderSetFrameDistance() failed"},
      {{"EFFORT=-1"}, "JxlEncoderFrameSettingsSetOption() failed"},
      {{"EFFORT=999"}, "JxlEncoderFrameSettingsSetOption() failed"},
      {{"LOSSLESS=NO", "LOSSLESS_COPY=YES"},
       "LOSSLESS_COPY=YES requested but not possible"},
  };

  auto valid_src = CreateMemDataset(2, 2, 1, GDT_Byte);
  for (const auto& opt_case : kInvalidOptionSets) {
    SCOPED_TRACE(opt_case.options[0]);
    CPLStringList options;
    for (const char* opt : opt_case.options) {
      options.AddString(opt);
    }
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = std::unique_ptr<GDALDataset>(jxl_driver->CreateCopy(
        std::format("{}/bad_opts.jxl", kVsiDir).c_str(), valid_src.get(),
        FALSE, options.List(), nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr(opt_case.expected_err));
    CPLErrorReset();
    EXPECT_EQ(nullptr, ds);
  }

  // Lossy quality thresholds: QUALITY=100 (fDistance clamped to 0.01), QUALITY=80, QUALITY=15
  for (const char* q : {"100", "80", "15"}) {
    SCOPED_TRACE(q);
    CPLStringList options;
    options.AddNameValue("QUALITY", q);
    const std::string q_path = std::format("{}/q_{}.jxl", kVsiDir, q);
    auto ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(q_path.c_str(), valid_src.get(), FALSE,
                               options.List(), nullptr, nullptr));
    ASSERT_THAT(ds, NotNull());
  }

  // ALPHA_DISTANCE options on 4-band RGBA: 0, 0.001, 1.5
  auto rgba_src = CreateMemDataset(4, 4, 4, GDT_Byte);
  rgba_src->GetRasterBand(4)->SetColorInterpretation(GCI_AlphaBand);
  for (const char* ad : {"0", "0.001", "1.5"}) {
    SCOPED_TRACE(ad);
    CPLStringList options;
    options.AddNameValue("LOSSLESS", "NO");
    options.AddNameValue("ALPHA_DISTANCE", ad);
    const std::string ad_path = std::format("{}/ad_{}.jxl", kVsiDir, ad);
    auto ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(ad_path.c_str(), rgba_src.get(), FALSE,
                               options.List(), nullptr, nullptr));
    ASSERT_THAT(ds, NotNull());
  }

  // NBITS options: NBITS=9 on Byte succeeds (promotes to UInt16), NBITS=20 on UInt16 fails in libjxl
  {
    CPLStringList opt_9;
    opt_9.AddNameValue("NBITS", "9");
    auto ds_9 = std::unique_ptr<GDALDataset>(jxl_driver->CreateCopy(
        std::format("{}/nbits_9.jxl", kVsiDir).c_str(), valid_src.get(), FALSE,
        opt_9.List(), nullptr, nullptr));
    ASSERT_THAT(ds_9, NotNull());
    ds_9.reset();

    auto read_9 = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        std::format("{}/nbits_9.jxl", kVsiDir).c_str(), GDAL_OF_READONLY));
    ASSERT_THAT(read_9, NotNull());
    EXPECT_STREQ("9", read_9->GetRasterBand(1)->GetMetadataItem(
                          "NBITS", "IMAGE_STRUCTURE"));
    EXPECT_EQ(GDT_UInt16, read_9->GetRasterBand(1)->GetRasterDataType());

    auto u16_src = CreateMemDataset(2, 2, 1, GDT_UInt16);
    CPLStringList opt_20;
    opt_20.AddNameValue("NBITS", "20");
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds_20 = std::unique_ptr<GDALDataset>(jxl_driver->CreateCopy(
        std::format("{}/nbits_20.jxl", kVsiDir).c_str(), u16_src.get(), FALSE,
        opt_20.List(), nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Unhandled data type"));
    CPLErrorReset();
    EXPECT_EQ(nullptr, ds_20);
  }
}

TEST_F(JpegxlTest, LosslessCopyJxl) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALDriver* jxl_driver = GetGDALDriverManager()->GetDriverByName("JPEGXL");
  ASSERT_THAT(jxl_driver, NotNull());

  // 1. Raw codestream JXL source (no metadata boxes)
  auto mem_ds = CreateMemDataset(8, 8, 1, GDT_Byte);
  const std::string raw_jxl = std::format("{}/src_raw.jxl", kVsiDir);
  auto ds_raw = std::unique_ptr<GDALDataset>(
      jxl_driver->CreateCopy(raw_jxl.c_str(), mem_ds.get(), FALSE, nullptr,
                             nullptr, nullptr));
  ASSERT_THAT(ds_raw, NotNull());
  ds_raw.reset();

  // Re-open raw codestream JXL and attach XMP + EXIF metadata
  auto open_raw = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(raw_jxl.c_str(), GDAL_OF_READONLY));
  ASSERT_THAT(open_raw, NotNull());
  const char* kCopiedXmp = "<?xpacket begin=''?>copied<?xpacket end='w'?>";
  char* copied_xmp[] = {const_cast<char*>(kCopiedXmp), nullptr};
  open_raw->SetMetadata(copied_xmp, "xml:XMP");
  open_raw->SetMetadataItem("EXIF_Make", "RawJxlSource", "EXIF");

  // CreateCopy with LOSSLESS_COPY=YES on raw codestream JXL wraps it into container and writes boxes
  CPLStringList lc_opts;
  lc_opts.AddNameValue("LOSSLESS_COPY", "YES");
  const std::string cont_out = std::format("{}/cont_wrapped.jxl", kVsiDir);
  auto wrapped_ds = std::unique_ptr<GDALDataset>(
      jxl_driver->CreateCopy(cont_out.c_str(), open_raw.get(), FALSE,
                             lc_opts.List(), nullptr, nullptr));
  ASSERT_THAT(wrapped_ds, NotNull());
  wrapped_ds.reset();

  // Verify wrapped container has XMP and EXIF
  auto check_ds = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(cont_out.c_str(), GDAL_OF_READONLY));
  ASSERT_THAT(check_ds, NotNull());
  EXPECT_STREQ("RawJxlSource", check_ds->GetMetadataItem("EXIF_Make", "EXIF"));

  // 2. Container JXL source with boxes -> CreateCopy with updated metadata runs box splicing loop
  check_ds->SetMetadataItem("EXIF_Model", "SplicedModel", "EXIF");
  const std::string spliced_out = std::format("{}/spliced.jxl", kVsiDir);
  auto spliced_ds = std::unique_ptr<GDALDataset>(
      jxl_driver->CreateCopy(spliced_out.c_str(), check_ds.get(), FALSE,
                             lc_opts.List(), nullptr, nullptr));
  ASSERT_THAT(spliced_ds, NotNull());
  spliced_ds.reset();

  // 3. Re-open and verify spliced container
  auto check_spliced = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(spliced_out.c_str(), GDAL_OF_READONLY));
  ASSERT_THAT(check_spliced, NotNull());
  EXPECT_STREQ("RawJxlSource",
               check_spliced->GetMetadataItem("EXIF_Make", "EXIF"));
}

TEST_F(JpegxlTest, LosslessCopyJpeg) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALDriver* jxl_driver = GetGDALDriverManager()->GetDriverByName("JPEGXL");
  ASSERT_THAT(jxl_driver, NotNull());
  GDALDriver* jpeg_driver = GetGDALDriverManager()->GetDriverByName("JPEG");
  ASSERT_THAT(jpeg_driver, NotNull());

  // 1. Create a baseline 3-band JPEG file
  auto mem_ds = CreateMemDataset(16, 16, 3, GDT_Byte);
  const std::string jpg_path = std::format("{}/test.jpg", kVsiDir);
  CPLStringList jpg_opts;
  jpg_opts.AddNameValue("COMMENT", "Lossless JPEG transcode test");
  auto jpeg_src = std::unique_ptr<GDALDataset>(
      jpeg_driver->CreateCopy(jpg_path.c_str(), mem_ds.get(), FALSE,
                              jpg_opts.List(), nullptr, nullptr));
  ASSERT_THAT(jpeg_src, NotNull());
  jpeg_src.reset();

  auto open_jpg = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(jpg_path.c_str(), GDAL_OF_READONLY));
  ASSERT_THAT(open_jpg, NotNull());

  // Copy JPEG -> JXL with LOSSLESS_COPY=AUTO
  const std::string jxl_from_jpg = std::format("{}/from_jpg.jxl", kVsiDir);
  auto jxl_ds = std::unique_ptr<GDALDataset>(
      jxl_driver->CreateCopy(jxl_from_jpg.c_str(), open_jpg.get(), FALSE,
                             nullptr, nullptr, nullptr));
  ASSERT_THAT(jxl_ds, NotNull());
  jxl_ds.reset();

  auto read_jxl = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(jxl_from_jpg.c_str(), GDAL_OF_READONLY));
  ASSERT_THAT(read_jxl, NotNull());
  EXPECT_STREQ("JPEG", read_jxl->GetMetadataItem("ORIGINAL_COMPRESSION",
                                                 "IMAGE_STRUCTURE"));

  // 2. Copy JPEG with LOSSLESS_COPY=NO
  CPLStringList no_lc;
  no_lc.AddNameValue("LOSSLESS_COPY", "NO");
  const std::string no_lc_path = std::format("{}/no_lc_jpg.jxl", kVsiDir);
  auto ds_no_lc = std::unique_ptr<GDALDataset>(
      jxl_driver->CreateCopy(no_lc_path.c_str(), open_jpg.get(), FALSE,
                             no_lc.List(), nullptr, nullptr));
  ASSERT_THAT(ds_no_lc, NotNull());
  ds_no_lc.reset();

  auto read_no_lc = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(no_lc_path.c_str(), GDAL_OF_READONLY));
  ASSERT_THAT(read_no_lc, NotNull());
  EXPECT_EQ(nullptr, read_no_lc->GetMetadataItem("ORIGINAL_COMPRESSION",
                                                 "IMAGE_STRUCTURE"));

  // 3. JPEG with per-dataset mask band
  const std::string masked_jpg_path = std::format("{}/masked.jpg", kVsiDir);
  auto masked_jpg = std::unique_ptr<GDALDataset>(
      jpeg_driver->CreateCopy(masked_jpg_path.c_str(), mem_ds.get(), FALSE,
                              nullptr, nullptr, nullptr));
  ASSERT_THAT(masked_jpg, NotNull());
  EXPECT_EQ(CE_None, masked_jpg->CreateMaskBand(GMF_PER_DATASET));
  GDALRasterBand* mask_band = masked_jpg->GetRasterBand(1)->GetMaskBand();
  ASSERT_THAT(mask_band, NotNull());
  std::vector<uint8_t> mask_vals(16 * 16, 255);
  mask_vals[0] = 0;
  EXPECT_EQ(CE_None, mask_band->RasterIO(GF_Write, 0, 0, 16, 16,
                                         mask_vals.data(), 16, 16, GDT_Byte, 0,
                                         0, nullptr));
  masked_jpg.reset();

  auto open_masked = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(masked_jpg_path.c_str(), GDAL_OF_READONLY));
  ASSERT_THAT(open_masked, NotNull());

  const std::string masked_jxl_path = std::format("{}/masked.jxl", kVsiDir);
  auto masked_jxl = std::unique_ptr<GDALDataset>(
      jxl_driver->CreateCopy(masked_jxl_path.c_str(), open_masked.get(), FALSE,
                             nullptr, nullptr, nullptr));
  ASSERT_THAT(masked_jxl, NotNull());
  masked_jxl.reset();

  auto read_masked = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(masked_jxl_path.c_str(), GDAL_OF_READONLY));
  ASSERT_THAT(read_masked, NotNull());
  EXPECT_EQ(4, read_masked->GetRasterCount());
  EXPECT_STREQ("JPEG", read_masked->GetMetadataItem("ORIGINAL_COMPRESSION",
                                                   "IMAGE_STRUCTURE"));
}

TEST_F(JpegxlTest, GetCompressionFormatsAndReadCompressedData) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALDriver* jxl_driver = GetGDALDriverManager()->GetDriverByName("JPEGXL");
  ASSERT_THAT(jxl_driver, NotNull());
  GDALDriver* jpeg_driver = GetGDALDriverManager()->GetDriverByName("JPEG");
  ASSERT_THAT(jpeg_driver, NotNull());

  // 1. Test on a container JXL dataset with boxes
  auto src_ds = CreateMemDataset(8, 8, 1, GDT_Byte);
  const char* kXmp =
      "<?xpacket begin='' id='W5M0MpCehiHzreSzNTczkc9d'?>\n"
      "<xmp>test</xmp>\n"
      "<?xpacket end='w'?>";
  char* xmp_list[] = {const_cast<char*>(kXmp), nullptr};
  ASSERT_EQ(CE_None, src_ds->SetMetadata(xmp_list, "xml:XMP"));
  const std::string jxl_path = std::format("{}/read_comp.jxl", kVsiDir);
  auto ds = std::unique_ptr<GDALDataset>(jxl_driver->CreateCopy(
      jxl_path.c_str(), src_ds.get(), FALSE, nullptr, nullptr, nullptr));
  ASSERT_THAT(ds, NotNull());
  ds.reset();

  auto open_ds = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(jxl_path.c_str(), GDAL_OF_READONLY));
  ASSERT_THAT(open_ds, NotNull());

  // Partial window -> empty compression formats / CE_Failure
  CPLStringList formats_partial =
      open_ds->GetCompressionFormats(0, 0, 4, 4, 1, nullptr);
  EXPECT_TRUE(formats_partial.empty());

  int band_list[1] = {1};
  CPLStringList formats_full =
      open_ds->GetCompressionFormats(0, 0, 8, 8, 1, band_list);
  EXPECT_THAT(formats_full.FindString("JXL"), ::testing::Ge(0));

  void* pBuffer = nullptr;
  size_t nBufferSize = 0;
  EXPECT_NE(CE_None, open_ds->ReadCompressedData("JXL", 0, 0, 4, 4, 1,
                                                 nullptr, &pBuffer,
                                                 &nBufferSize, nullptr));

  // Invalid formats: "UNKNOWN", "JXL;opt=1"
  EXPECT_NE(CE_None, open_ds->ReadCompressedData("UNKNOWN", 0, 0, 8, 8,
                                                 1, band_list, &pBuffer,
                                                 &nBufferSize, nullptr));
  EXPECT_NE(CE_None, open_ds->ReadCompressedData("JXL;opt=1", 0, 0, 8, 8,
                                                 1, band_list, &pBuffer,
                                                 &nBufferSize, nullptr));

  // Full window JXL format: query size
  char* pszDetailedRaw = nullptr;
  ASSERT_EQ(CE_None, open_ds->ReadCompressedData("JXL", 0, 0, 8, 8,
                                                 1, band_list, nullptr,
                                                 &nBufferSize, &pszDetailedRaw));
  std::unique_ptr<char, CplFreeDeleter> pszDetailed(pszDetailedRaw);
  ASSERT_GT(nBufferSize, 0u);
  EXPECT_STREQ("JXL", pszDetailed.get());

  // Buffer allocation error paths
  EXPECT_NE(CE_None, open_ds->ReadCompressedData("JXL", 0, 0, 8, 8,
                                                 1, band_list, &pBuffer,
                                                 nullptr, nullptr));

  // Caller buffer too small
  std::vector<uint8_t> small_buf(4);
  void* pSmall = small_buf.data();
  size_t nSmallSize = small_buf.size();
  EXPECT_NE(CE_None, open_ds->ReadCompressedData("JXL", 0, 0, 8, 8,
                                                 1, band_list, &pSmall,
                                                 &nSmallSize, nullptr));

  // Caller buffer sufficient
  std::vector<uint8_t> caller_buf(nBufferSize);
  void* pCaller = caller_buf.data();
  size_t nCallerSize = caller_buf.size();
  EXPECT_EQ(CE_None, open_ds->ReadCompressedData("JXL", 0, 0, 8, 8,
                                                 1, band_list, &pCaller,
                                                 &nCallerSize, nullptr));

  // Driver allocated buffer (strips Exif, xml, jumb boxes)
  void* pAllocatedRaw = nullptr;
  size_t nAllocSize = 0;
  EXPECT_EQ(CE_None, open_ds->ReadCompressedData("JXL", 0, 0, 8, 8,
                                                 1, band_list, &pAllocatedRaw,
                                                 &nAllocSize, nullptr));
  std::unique_ptr<void, decltype(&VSIFree)> pAllocated(pAllocatedRaw, VSIFree);
  ASSERT_THAT(pAllocated, NotNull());
  EXPECT_GT(nAllocSize, 0u);

  // 2. Test JPEG reconstruction from JXL created from JPEG
  auto rgb_ds = CreateMemDataset(16, 16, 3, GDT_Byte);
  const std::string jpg_file = std::format("{}/recon.jpg", kVsiDir);
  auto jpg_src = std::unique_ptr<GDALDataset>(
      jpeg_driver->CreateCopy(jpg_file.c_str(), rgb_ds.get(), FALSE, nullptr,
                              nullptr, nullptr));
  ASSERT_THAT(jpg_src, NotNull());
  jpg_src.reset();

  auto open_recon_jpg = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(jpg_file.c_str(), GDAL_OF_READONLY));
  ASSERT_THAT(open_recon_jpg, NotNull());

  const std::string recon_jxl = std::format("{}/recon.jxl", kVsiDir);
  auto jxl_recon = std::unique_ptr<GDALDataset>(
      jxl_driver->CreateCopy(recon_jxl.c_str(), open_recon_jpg.get(), FALSE,
                             nullptr, nullptr, nullptr));
  ASSERT_THAT(jxl_recon, NotNull());
  jxl_recon.reset();

  auto read_recon = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(recon_jxl.c_str(), GDAL_OF_READONLY));
  ASSERT_THAT(read_recon, NotNull());

  CPLStringList formats =
      read_recon->GetCompressionFormats(0, 0, 16, 16, 3, nullptr);
  EXPECT_THAT(formats.FindString("JPEG"), ::testing::Ge(0));

  // ReadCompressedData("JPEG"): query size
  size_t nJpgSize = 0;
  int band_list_3[3] = {1, 2, 3};
  char* pszDetailedJpgRaw = nullptr;
  ASSERT_EQ(CE_None, read_recon->ReadCompressedData(
                         "JPEG", 0, 0, 16, 16, 3, band_list_3,
                         nullptr, &nJpgSize, &pszDetailedJpgRaw));
  std::unique_ptr<char, CplFreeDeleter> pszDetailedJpg(pszDetailedJpgRaw);
  ASSERT_GT(nJpgSize, 0u);

  // Caller buffer too small vs sufficient
  std::vector<uint8_t> small_jpg(2);
  void* pSmallJpg = small_jpg.data();
  size_t nSmallJpg = small_jpg.size();
  EXPECT_NE(CE_None, read_recon->ReadCompressedData(
                         "JPEG", 0, 0, 16, 16, 3, band_list_3,
                         &pSmallJpg, &nSmallJpg, nullptr));

  std::vector<uint8_t> caller_jpg(nJpgSize);
  void* pCallerJpg = caller_jpg.data();
  size_t nCallerJpg = caller_jpg.size();
  EXPECT_EQ(CE_None, read_recon->ReadCompressedData(
                         "JPEG", 0, 0, 16, 16, 3, band_list_3,
                         &pCallerJpg, &nCallerJpg, nullptr));

  // Driver allocated buffer
  void* pJpgBufRaw = nullptr;
  size_t nAllocJpg = 0;
  EXPECT_EQ(CE_None, read_recon->ReadCompressedData(
                         "JPEG", 0, 0, 16, 16, 3, band_list_3,
                         &pJpgBufRaw, &nAllocJpg, nullptr));
  std::unique_ptr<void, decltype(&VSIFree)> pJpgBuf(pJpgBufRaw, VSIFree);
  ASSERT_THAT(pJpgBuf, NotNull());

  // ReadCompressedData("JPEG") on non-JPEG JXL fails
  void* pFailBuf = nullptr;
  size_t nFailSize = 0;
  EXPECT_NE(CE_None, open_ds->ReadCompressedData("JPEG", 0, 0, 8, 8,
                                                 1, band_list, &pFailBuf,
                                                 &nFailSize, nullptr));
}

TEST_F(JpegxlTest, CloseMethod) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALDriver* jxl_driver = GetGDALDriverManager()->GetDriverByName("JPEGXL");
  ASSERT_THAT(jxl_driver, NotNull());

  auto src_ds = CreateMemDataset(4, 4, 1, GDT_Byte);
  const std::string path = std::format("{}/close_test.jxl", kVsiDir);
  auto ds = std::unique_ptr<GDALDataset>(jxl_driver->CreateCopy(
      path.c_str(), src_ds.get(), FALSE, nullptr, nullptr, nullptr));
  ASSERT_THAT(ds, NotNull());
  ds.reset();

  auto open_ds = std::unique_ptr<GDALDataset>(
      GDALDataset::Open(path.c_str(), GDAL_OF_READONLY));
  ASSERT_THAT(open_ds, NotNull());
  EXPECT_EQ(CE_None, open_ds->Close());
}

class FakeSourceDataset;

class FakeRasterMaskBand : public GDALRasterBand {
 public:
  bool fail_rasterio_ = false;

  FakeRasterMaskBand(int xsize, int ysize) {
    nRasterXSize = xsize;
    nRasterYSize = ysize;
    nBlockXSize = xsize;
    nBlockYSize = 1;
    eDataType = GDT_Byte;
  }

  CPLErr IReadBlock(int, int, void* pImage) override {
    memset(pImage, 255, nBlockXSize);
    return CE_None;
  }

  CPLErr IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize,
                   int nYSize, void* pData, int nBufXSize, int nBufYSize,
                   GDALDataType eBufType, GSpacing nPixelSpace,
                   GSpacing nLineSpace,
                   GDALRasterIOExtraArg* psExtraArg) override {
    if (fail_rasterio_) {
      return CE_Failure;
    }
    return GDALRasterBand::IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize,
                                     pData, nBufXSize, nBufYSize, eBufType,
                                     nPixelSpace, nLineSpace, psExtraArg);
  }
};

class FakeRasterBand : public GDALRasterBand {
 public:
  FakeSourceDataset* parent_ = nullptr;
  bool fail_rasterio_ = false;
  GDALColorInterp color_interp_ = GCI_Undefined;
  std::unique_ptr<FakeRasterMaskBand> mask_band_;

  FakeRasterBand(FakeSourceDataset* parent, int band, int xsize, int ysize,
                 GDALDataType dt);

  GDALColorInterp GetColorInterpretation() override { return color_interp_; }
  CPLErr SetColorInterpretation(GDALColorInterp eColorInterp) override {
    color_interp_ = eColorInterp;
    return CE_None;
  }

  CPLErr IReadBlock(int, int, void* pImage) override {
    memset(pImage, 0, nBlockXSize * GDALGetDataTypeSizeBytes(eDataType));
    return CE_None;
  }

  CPLErr IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize,
                   int nYSize, void* pData, int nBufXSize, int nBufYSize,
                   GDALDataType eBufType, GSpacing nPixelSpace,
                   GSpacing nLineSpace,
                   GDALRasterIOExtraArg* psExtraArg) override {
    if (fail_rasterio_) {
      return CE_Failure;
    }
    return GDALRasterBand::IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize,
                                     pData, nBufXSize, nBufYSize, eBufType,
                                     nPixelSpace, nLineSpace, psExtraArg);
  }

  int GetMaskFlags() override;
  GDALRasterBand* GetMaskBand() override;
};

class FakeSourceDataset : public GDALDataset {
 public:
  bool fail_dataset_rasterio_ = false;
  bool has_per_dataset_mask_ = false;
  bool has_fake_jpeg_ = false;
  std::string fake_jpeg_details_;
  std::vector<uint8_t> fake_jpeg_bytes_;

  FakeSourceDataset(int xsize, int ysize, int nbands, GDALDataType dt) {
    nRasterXSize = xsize;
    nRasterYSize = ysize;
    for (int i = 1; i <= nbands; ++i) {
      SetBand(i, new FakeRasterBand(this, i, xsize, ysize, dt));
    }
  }

  CPLErr IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize,
                   int nYSize, void* pData, int nBufXSize, int nBufYSize,
                   GDALDataType eBufType, int nBandCount, BANDMAP_TYPE panBandMap,
                   GSpacing nPixelSpace, GSpacing nLineSpace,
                   GSpacing nBandSpace,
                   GDALRasterIOExtraArg* psExtraArg) override {
    if (fail_dataset_rasterio_) {
      return CE_Failure;
    }
    return GDALDataset::IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize, pData,
                                  nBufXSize, nBufYSize, eBufType, nBandCount,
                                  panBandMap, nPixelSpace, nLineSpace,
                                  nBandSpace, psExtraArg);
  }

  CPLErr ReadCompressedData(const char* pszFormat, int, int, int, int, int,
                            const int*, void** ppBuffer, size_t* pnBufferSize,
                            char** ppszDetailedFormat) override {
    if (EQUAL(pszFormat, "JPEG") && has_fake_jpeg_) {
      if (ppszDetailedFormat) {
        *ppszDetailedFormat = VSIStrdup(fake_jpeg_details_.c_str());
      }
      if (ppBuffer) {
        *ppBuffer = VSIMalloc(fake_jpeg_bytes_.size());
        memcpy(*ppBuffer, fake_jpeg_bytes_.data(), fake_jpeg_bytes_.size());
      }
      if (pnBufferSize) {
        *pnBufferSize = fake_jpeg_bytes_.size();
      }
      return CE_None;
    }
    return CE_Failure;
  }
};

inline FakeRasterBand::FakeRasterBand(FakeSourceDataset* parent, int band,
                                     int xsize, int ysize, GDALDataType dt)
    : parent_(parent) {
  nBand = band;
  nRasterXSize = xsize;
  nRasterYSize = ysize;
  eDataType = dt;
  nBlockXSize = xsize;
  nBlockYSize = 1;
  mask_band_ = std::make_unique<FakeRasterMaskBand>(xsize, ysize);
}

inline int FakeRasterBand::GetMaskFlags() {
  if (parent_ && parent_->has_per_dataset_mask_) {
    return GMF_PER_DATASET;
  }
  return GDALRasterBand::GetMaskFlags();
}

inline GDALRasterBand* FakeRasterBand::GetMaskBand() {
  if (parent_ && parent_->has_per_dataset_mask_) {
    return mask_band_.get();
  }
  return GDALRasterBand::GetMaskBand();
}

TEST_F(JpegxlTest, AdditionalCoveragePaths) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALDriver* jxl_driver = GetGDALDriverManager()->GetDriverByName("JPEGXL");
  ASSERT_THAT(jxl_driver, NotNull());

  // 1. Uncompressed box > 1 MB buffer growth
  {
    auto src_ds = CreateMemDataset(8, 8, 1, GDT_Byte);
    std::string huge_xmp =
        "<?xpacket begin='' id='W5M0MpCehiHzreSzNTczkc9d'?>\n";
    huge_xmp.append(1100 * 1024, ' ');
    huge_xmp.append("<?xpacket end='w'?>");
    char* huge_xmp_list[] = {huge_xmp.data(), nullptr};
    src_ds->SetMetadata(huge_xmp_list, "xml:XMP");

    CPLStringList comp_opts;
    comp_opts.AddNameValue("COMPRESS_BOXES", "NO");
    const std::string huge_uncomp_path =
        std::format("{}/huge_uncomp_xmp.jxl", kVsiDir);
    auto jxl_ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(huge_uncomp_path.c_str(), src_ds.get(), FALSE,
                               comp_opts.List(), nullptr, nullptr));
    ASSERT_THAT(jxl_ds, NotNull());
    jxl_ds.reset();

    auto read_ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(huge_uncomp_path.c_str(), GDAL_OF_READONLY));
    ASSERT_THAT(read_ds, NotNull());
    char** meta = read_ds->GetMetadata("xml:XMP");
    ASSERT_THAT(meta, NotNull());
    EXPECT_THAT(meta[0], HasSubstr("<?xpacket"));
  }

  // 2. ReadCompressedData("JPEG") > 16 KB with APP1 EXIF/XMP stripping
  {
    auto src_ds = CreateMemDataset(256, 256, 3, GDT_Byte);
    for (int b = 1; b <= 3; ++b) {
      std::vector<uint8_t> data(256 * 256);
      for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 256; ++x) {
          data[y * 256 + x] =
              static_cast<uint8_t>((x * 131 + y * 97 + b * 53) ^ (x * y));
        }
      }
      EXPECT_EQ(CE_None, src_ds->GetRasterBand(b)->RasterIO(
                             GF_Write, 0, 0, 256, 256, data.data(), 256, 256,
                             GDT_Byte, 0, 0, nullptr));
    }
    GDALDriver* jpeg_driver = GetGDALDriverManager()->GetDriverByName("JPEG");
    ASSERT_THAT(jpeg_driver, NotNull());
    const std::string raw_jpg_path =
        std::format("{}/large_entropy.jpg", kVsiDir);
    CPLStringList jpg_opts;
    jpg_opts.AddNameValue("QUALITY", "99");
    auto jpg_ds = std::unique_ptr<GDALDataset>(
        jpeg_driver->CreateCopy(raw_jpg_path.c_str(), src_ds.get(), FALSE,
                                jpg_opts.List(), nullptr, nullptr));
    ASSERT_THAT(jpg_ds, NotNull());
    jpg_ds.reset();

    vsi_l_offset nJpgSize = 0;
    GByte* pabyJpg =
        VSIGetMemFileBuffer(raw_jpg_path.c_str(), &nJpgSize, FALSE);
    ASSERT_THAT(pabyJpg, NotNull());
    std::vector<uint8_t> jpg_bytes(pabyJpg, pabyJpg + nJpgSize);
    EXPECT_GT(jpg_bytes.size(), 16384u);

    // Find the first DQT marker (0xFF, 0xDB)
    size_t dqt_pos = 0;
    for (size_t i = 0; i + 1 < jpg_bytes.size(); ++i) {
      if (jpg_bytes[i] == 0xFF && jpg_bytes[i + 1] == 0xDB) {
        dqt_pos = i;
        break;
      }
    }
    ASSERT_GT(dqt_pos, 0u);
    size_t dqt_len = (jpg_bytes[dqt_pos + 2] << 8) | jpg_bytes[dqt_pos + 3];
    size_t insert_pos = dqt_pos + 2 + dqt_len;

    // Segment 1: APP1 Exif (18 bytes total, length field = 16)
    const uint8_t kExifSegment[] = {
        0xFF, 0xE1, 0x00, 0x10, 'E', 'x', 'i', 'f', 0x00, 0x00,
        'I',  'I',  0x2A, 0x00, 0x08, 0x00, 0x00, 0x00,
    };
    // Segment 2: APP1 XMP (36 bytes total, length field = 34)
    const uint8_t kXmpSegment[] = {
        0xFF, 0xE1, 0x00, 0x22, 'h', 't', 't', 'p', ':', '/', '/',
        'n',  's',  '.',  'a',  'd', 'o', 'b', 'e', '.', 'c', 'o',
        'm',  '/',  'x',  'a',  'p', '/', '1', '.', '0', '/', 0x00,
        'A',  'B',  'C',
    };
    jpg_bytes.insert(jpg_bytes.begin() + insert_pos, std::begin(kXmpSegment),
                     std::end(kXmpSegment));
    jpg_bytes.insert(jpg_bytes.begin() + insert_pos, std::begin(kExifSegment),
                     std::end(kExifSegment));

    FakeSourceDataset mod_jpg_ds(256, 256, 3, GDT_Byte);
    mod_jpg_ds.GetRasterBand(1)->SetColorInterpretation(GCI_RedBand);
    mod_jpg_ds.GetRasterBand(2)->SetColorInterpretation(GCI_GreenBand);
    mod_jpg_ds.GetRasterBand(3)->SetColorInterpretation(GCI_BlueBand);
    mod_jpg_ds.has_fake_jpeg_ = true;
    mod_jpg_ds.fake_jpeg_details_ =
        "bit_depth=8;colorspace=YCbCr;frame_type=SOF0_baseline";
    mod_jpg_ds.fake_jpeg_bytes_ = std::move(jpg_bytes);

    const std::string large_jxl_path =
        std::format("{}/large_recon.jxl", kVsiDir);
    CPLStringList jxl_opts;
    jxl_opts.AddNameValue("LOSSLESS_COPY", "YES");
    auto recon_ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(large_jxl_path.c_str(), &mod_jpg_ds, FALSE,
                               jxl_opts.List(), nullptr, nullptr));
    ASSERT_THAT(recon_ds, NotNull());
    recon_ds.reset();

    auto open_recon = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(large_jxl_path.c_str(), GDAL_OF_READONLY));
    ASSERT_THAT(open_recon, NotNull());

    void* pReconBufRaw = nullptr;
    size_t nReconSize = 0;
    const int band_list_3[3] = {1, 2, 3};
    EXPECT_EQ(CE_None, open_recon->ReadCompressedData(
                           "JPEG", 0, 0, 256, 256, 3, band_list_3,
                           &pReconBufRaw, &nReconSize, nullptr));
    std::unique_ptr<void, decltype(&VSIFree)> pReconBuf(pReconBufRaw, VSIFree);
    ASSERT_THAT(pReconBuf, NotNull());

    // Test with non-null pointer but null pnBufferSize (hits line 1325)
    void* dummy_ptr = &nReconSize;
    EXPECT_EQ(CE_Failure, open_recon->ReadCompressedData(
                              "JPEG", 0, 0, 256, 256, 3, band_list_3,
                              &dummy_ptr, nullptr, nullptr));

    // Test truncated JBRD codestream (hits line 1182)
    vsi_l_offset nReconBytes = 0;
    GByte* pabyRecon =
        VSIGetMemFileBuffer(large_jxl_path.c_str(), &nReconBytes, FALSE);
    ASSERT_THAT(pabyRecon, NotNull());
    ASSERT_GT(nReconBytes, 150u);
    const std::string trunc_jbrd_path =
        std::format("{}/trunc_jbrd.jxl", kVsiDir);
    VsiMemTempWrapper trunc_jbrd_file(
        trunc_jbrd_path,
        std::string(reinterpret_cast<const char*>(pabyRecon),
                    nReconBytes - 100));
    WithQuietHandler quiet;
    CPLErrorReset();
    auto trunc_recon_ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(trunc_jbrd_path.c_str(), GDAL_OF_READONLY));
    ASSERT_THAT(trunc_recon_ds, NotNull());
    void* pTruncBuf = nullptr;
    size_t nTruncSize = 0;
    EXPECT_NE(CE_None, trunc_recon_ds->ReadCompressedData(
                           "JPEG", 0, 0, 256, 256, 3, band_list_3,
                           &pTruncBuf, &nTruncSize, nullptr));
    CPLErrorReset();
  }

  // 3. Truncated codestream in GetDecodedImage and IRasterIO cached failure
  {
    auto src_ds = CreateMemDataset(64, 64, 3, GDT_Byte);
    const std::string valid_jxl_path =
        std::format("{}/valid_64.jxl", kVsiDir);
    auto jxl_ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(valid_jxl_path.c_str(), src_ds.get(), FALSE,
                               nullptr, nullptr, nullptr));
    ASSERT_THAT(jxl_ds, NotNull());
    jxl_ds.reset();

    vsi_l_offset nSize = 0;
    GByte* pabyData =
        VSIGetMemFileBuffer(valid_jxl_path.c_str(), &nSize, FALSE);
    ASSERT_THAT(pabyData, NotNull());

    const std::string trunc_path = std::format("{}/trunc_frame.jxl", kVsiDir);
    VsiMemTempWrapper trunc_file(
        trunc_path,
        std::string(reinterpret_cast<const char*>(pabyData),
                    std::min<size_t>(nSize, 32)));

    WithQuietHandler quiet;
    CPLErrorReset();
    auto read_ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(trunc_path.c_str(), GDAL_OF_READONLY));
    ASSERT_THAT(read_ds, NotNull());
    std::vector<uint8_t> buf(64 * 64 * 3);
    EXPECT_NE(CE_None, read_ds->RasterIO(GF_Read, 0, 0, 64, 64, buf.data(),
                                         64, 64, GDT_Byte, 3, nullptr, 3,
                                         64 * 3, 1, nullptr));
    CPLErrorReset();
    EXPECT_NE(CE_None, read_ds->RasterIO(GF_Read, 0, 0, 64, 64, buf.data(),
                                         64, 64, GDT_Byte, 3, nullptr, 3,
                                         64 * 3, 1, nullptr));
    CPLErrorReset();
  }

  // 4. Lossless JXL Container Copy with georef (JUMBF), AREA_OR_POINT, main EXIF, and write errors
  {
    auto src_ds = CreateMemDataset(8, 8, 1, GDT_Byte);
    const std::string base_path = std::format("{}/jxl_base_copy.jxl", kVsiDir);
    auto jxl_ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(base_path.c_str(), src_ds.get(), FALSE, nullptr,
                               nullptr, nullptr));
    ASSERT_THAT(jxl_ds, NotNull());
    jxl_ds.reset();

    auto open_src = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(base_path.c_str(), GDAL_OF_READONLY));
    ASSERT_THAT(open_src, NotNull());
    GDALGeoTransform gt{10.0, 1.0, 0.0, 20.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, open_src->SetGeoTransform(gt));
    ASSERT_EQ(CE_None,
              open_src->SetMetadataItem(GDALMD_AREA_OR_POINT, GDALMD_AOP_AREA));
    ASSERT_EQ(CE_None, open_src->SetMetadataItem("EXIF_Make", "GoogleJXLTest"));

    const std::string copy_out_path =
        std::format("{}/jxl_copy_out.jxl", kVsiDir);
    CPLStringList copy_opts;
    copy_opts.AddNameValue("LOSSLESS_COPY", "YES");
    auto copy_ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(copy_out_path.c_str(), open_src.get(), FALSE,
                               copy_opts.List(), nullptr, nullptr));
    ASSERT_THAT(copy_ds, NotNull());
    copy_ds.reset();

    auto verify_ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(copy_out_path.c_str(), GDAL_OF_READONLY));
    ASSERT_THAT(verify_ds, NotNull());
    EXPECT_STREQ("GoogleJXLTest",
                 verify_ds->GetMetadataItem("EXIF_Make", "EXIF"));
    GDALGeoTransform verify_gt;
    EXPECT_EQ(CE_None, verify_ds->GetGeoTransform(verify_gt));
    EXPECT_DOUBLE_EQ(10.0, verify_gt[0]);

    // Test write failure via ||maxlength=4||
    const std::string fail_write_path =
        std::format("{}/jxl_fail_write.jxl||maxlength=4||", kVsiDir);
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      auto fail_ds = std::unique_ptr<GDALDataset>(
          jxl_driver->CreateCopy(fail_write_path.c_str(), open_src.get(), FALSE,
                                 copy_opts.List(), nullptr, nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Failure writing data"));
      CPLErrorReset();
      EXPECT_EQ(nullptr, fail_ds);
    }

    // Test open failure via invalid filename
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      auto fail_ds = std::unique_ptr<GDALDataset>(
          jxl_driver->CreateCopy("", open_src.get(), FALSE,
                                 copy_opts.List(), nullptr, nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Unable to create"));
      CPLErrorReset();
      EXPECT_EQ(nullptr, fail_ds);
    }

    // Test JXL container where jxlc box size is 0
    vsi_l_offset nBaseSize = 0;
    GByte* pabyBase =
        VSIGetMemFileBuffer(base_path.c_str(), &nBaseSize, FALSE);
    ASSERT_THAT(pabyBase, NotNull());
    std::vector<uint8_t> cont_bytes(std::begin(kJxlContainerSig),
                                    std::end(kJxlContainerSig));
    const char kFtypPayload[12] = {'j', 'x', 'l', ' ', 0, 0, 0, 0,
                                   'j', 'x', 'l', ' '};
    std::vector<uint8_t> ftyp_box = MakeIsobmffBox("ftyp", kFtypPayload, 12);
    cont_bytes.insert(cont_bytes.end(), ftyp_box.begin(), ftyp_box.end());

    cont_bytes.push_back(0);
    cont_bytes.push_back(0);
    cont_bytes.push_back(0);
    cont_bytes.push_back(0);
    cont_bytes.push_back('j');
    cont_bytes.push_back('x');
    cont_bytes.push_back('l');
    cont_bytes.push_back('c');
    cont_bytes.insert(cont_bytes.end(), pabyBase, pabyBase + nBaseSize);

    const std::string size0_path = std::format("{}/size0.jxl", kVsiDir);
    VsiMemTempWrapper size0_file(
        size0_path,
        std::string(reinterpret_cast<const char*>(cont_bytes.data()),
                    cont_bytes.size()));
    auto size0_ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(size0_path.c_str(), GDAL_OF_READONLY));
    ASSERT_THAT(size0_ds, NotNull());
    ASSERT_EQ(CE_None, size0_ds->SetMetadataItem("EXIF_Make", "Test"));
    const char* kShortXmp =
        "<?xpacket begin='' id='W5M0MpCehiHzreSzNTczkc9d'?>\n<x/>\n<?xpacket "
        "end='w'?>";
    char* xmp_list[] = {const_cast<char*>(kShortXmp), nullptr};
    ASSERT_EQ(CE_None, size0_ds->SetMetadata(xmp_list, "xml:XMP"));
    ASSERT_EQ(CE_None, size0_ds->SetGeoTransform(gt));
    const std::string size0_out = std::format("{}/size0_out.jxl", kVsiDir);
    auto out_ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(size0_out.c_str(), size0_ds.get(), FALSE,
                               copy_opts.List(), nullptr, nullptr));
    ASSERT_THAT(out_ds, NotNull());
  }

  // 5. General CreateCopy with 6 bands (num_extra_channels >= 5), ALPHA_DISTANCE, and write error
  {
    auto src_ds = CreateMemDataset(8, 8, 6, GDT_Byte);
    ASSERT_EQ(CE_None,
              src_ds->SetMetadataItem(GDALMD_AREA_OR_POINT, GDALMD_AOP_AREA));
    const std::string six_band_path =
        std::format("{}/six_band.jxl", kVsiDir);
    auto six_ds = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(six_band_path.c_str(), src_ds.get(), FALSE,
                               nullptr, nullptr, nullptr));
    ASSERT_THAT(six_ds, NotNull());
    six_ds.reset();

    // 2-band with Alpha and ALPHA_DISTANCE
    auto alpha_ds = CreateMemDataset(8, 8, 2, GDT_Byte);
    alpha_ds->GetRasterBand(2)->SetColorInterpretation(GCI_AlphaBand);
    CPLStringList alpha_opts;
    alpha_opts.AddNameValue("ALPHA_DISTANCE", "3.0");
    alpha_opts.AddNameValue("DISTANCE", "1.0");
    const std::string alpha_path = std::format("{}/alpha_dist.jxl", kVsiDir);
    auto out_alpha = std::unique_ptr<GDALDataset>(
        jxl_driver->CreateCopy(alpha_path.c_str(), alpha_ds.get(), FALSE,
                               alpha_opts.List(), nullptr, nullptr));
    ASSERT_THAT(out_alpha, NotNull());

    // General CreateCopy write failure via ||maxlength=4||
    const std::string fail_gen_path =
        std::format("{}/gen_fail_write.jxl||maxlength=4||", kVsiDir);
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      auto fail_ds = std::unique_ptr<GDALDataset>(
          jxl_driver->CreateCopy(fail_gen_path.c_str(), src_ds.get(), FALSE,
                                 nullptr, nullptr, nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("VSIFWriteL() failed"));
      CPLErrorReset();
      EXPECT_EQ(nullptr, fail_ds);
    }
  }

  // 6. FakeSourceDataset covering edge-case branches in CreateCopy
  {
    // Fake JPEG: unsupported bit depth (hits lines 2406, 2410-2412)
    {
      FakeSourceDataset fake_ds(8, 8, 3, GDT_Byte);
      fake_ds.has_fake_jpeg_ = true;
      fake_ds.fake_jpeg_details_ =
          "bit_depth=12;colorspace=RGB;frame_type=SOF0_baseline";
      fake_ds.fake_jpeg_bytes_ = {0xFF, 0xD8, 0xFF, 0xDA};
      const std::string path = std::format("{}/fake_bits.jxl", kVsiDir);
      auto ds = std::unique_ptr<GDALDataset>(
          jxl_driver->CreateCopy(path.c_str(), &fake_ds, FALSE, nullptr,
                                 nullptr, nullptr));
      EXPECT_THAT(ds, NotNull());
    }

    // Fake JPEG: unsupported colorspace (hits lines 2418, 2422-2424)
    {
      FakeSourceDataset fake_ds(8, 8, 3, GDT_Byte);
      fake_ds.has_fake_jpeg_ = true;
      fake_ds.fake_jpeg_details_ =
          "bit_depth=8;colorspace=CMYK;frame_type=SOF0_baseline";
      fake_ds.fake_jpeg_bytes_ = {0xFF, 0xD8, 0xFF, 0xDA};
      const std::string path = std::format("{}/fake_cmyk.jxl", kVsiDir);
      auto ds = std::unique_ptr<GDALDataset>(
          jxl_driver->CreateCopy(path.c_str(), &fake_ds, FALSE, nullptr,
                                 nullptr, nullptr));
      EXPECT_THAT(ds, NotNull());
    }

    // Fake JPEG: unsupported frame_type (hits lines 2427-2428, 2430, 2434-2436)
    {
      FakeSourceDataset fake_ds(8, 8, 3, GDT_Byte);
      fake_ds.has_fake_jpeg_ = true;
      fake_ds.fake_jpeg_details_ =
          "bit_depth=8;colorspace=RGB;frame_type=SOF9_arithmetic";
      fake_ds.fake_jpeg_bytes_ = {0xFF, 0xD8, 0xFF, 0xDA};
      const std::string path = std::format("{}/fake_sof9.jxl", kVsiDir);
      auto ds = std::unique_ptr<GDALDataset>(
          jxl_driver->CreateCopy(path.c_str(), &fake_ds, FALSE, nullptr,
                                 nullptr, nullptr));
      EXPECT_THAT(ds, NotNull());
    }

    // Fake JPEG: invalid marker tag (hits lines 2463-2464)
    {
      FakeSourceDataset fake_ds(8, 8, 3, GDT_Byte);
      fake_ds.has_fake_jpeg_ = true;
      fake_ds.fake_jpeg_details_ =
          "bit_depth=8;colorspace=RGB;frame_type=SOF0_baseline";
      fake_ds.fake_jpeg_bytes_ = {0xFF, 0xD8, 0x00, 0x00};
      const std::string path = std::format("{}/fake_invalid_tag.jxl", kVsiDir);
      auto ds = std::unique_ptr<GDALDataset>(
          jxl_driver->CreateCopy(path.c_str(), &fake_ds, FALSE, nullptr,
                                 nullptr, nullptr));
      EXPECT_THAT(ds, NotNull());
    }

    // Fake JPEG: truncated marker length (hits lines 2485-2486)
    {
      FakeSourceDataset fake_ds(8, 8, 3, GDT_Byte);
      fake_ds.has_fake_jpeg_ = true;
      fake_ds.fake_jpeg_details_ =
          "bit_depth=8;colorspace=RGB;frame_type=SOF0_baseline";
      fake_ds.fake_jpeg_bytes_ = {0xFF, 0xD8, 0xFF, 0xE0};
      const std::string path = std::format("{}/fake_trunc_tag.jxl", kVsiDir);
      auto ds = std::unique_ptr<GDALDataset>(
          jxl_driver->CreateCopy(path.c_str(), &fake_ds, FALSE, nullptr,
                                 nullptr, nullptr));
      EXPECT_THAT(ds, NotNull());
    }

    // Fake JPEG: JxlEncoderAddJPEGFrame fails, fallback AUTO (hits lines 2747, 2750, 2754-2761).
    // TODO: b/335317901 - Upstream bug: jpegxl.cpp:2754 CPLStringList aosOptions(papszOptions)
    // takes ownership of caller's papszOptions. Pass nullptr for options so
    // LOSSLESS_COPY defaults to "AUTO" without passing caller ownership.
    {
      FakeSourceDataset fake_ds(8, 8, 3, GDT_Byte);
      fake_ds.has_fake_jpeg_ = true;
      fake_ds.fake_jpeg_details_ =
          "bit_depth=8;colorspace=RGB;frame_type=SOF0_baseline";
      fake_ds.fake_jpeg_bytes_ = {0xFF, 0xD8, 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00};
      const std::string path = std::format("{}/fake_auto_fallback.jxl", kVsiDir);
      auto ds = std::unique_ptr<GDALDataset>(
          jxl_driver->CreateCopy(path.c_str(), &fake_ds, FALSE, nullptr,
                                 nullptr, nullptr));
      EXPECT_THAT(ds, NotNull());
    }

    // Fake JPEG: JxlEncoderAddJPEGFrame fails, LOSSLESS_COPY=YES (hits lines 2764, 2766)
    {
      FakeSourceDataset fake_ds(8, 8, 3, GDT_Byte);
      fake_ds.has_fake_jpeg_ = true;
      fake_ds.fake_jpeg_details_ =
          "bit_depth=8;colorspace=RGB;frame_type=SOF0_baseline";
      fake_ds.fake_jpeg_bytes_ = {0xFF, 0xD8, 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00};
      CPLStringList opts;
      opts.AddNameValue("LOSSLESS_COPY", "YES");
      const std::string path = std::format("{}/fake_lossless_fail.jxl", kVsiDir);
      WithQuietHandler quiet;
      CPLErrorReset();
      auto ds = std::unique_ptr<GDALDataset>(
          jxl_driver->CreateCopy(path.c_str(), &fake_ds, FALSE, opts.List(),
                                 nullptr, nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(),
                  HasSubstr("JxlEncoderAddJPEGFrame() failed"));
      CPLErrorReset();
      EXPECT_EQ(nullptr, ds);
    }

    // Fake JPEG with per-dataset mask where mask RasterIO fails (hits line 2819)
    {
      GDALDriver* jpeg_driver = GetGDALDriverManager()->GetDriverByName("JPEG");
      ASSERT_THAT(jpeg_driver, NotNull());
      auto mem_ds = CreateMemDataset(8, 8, 3, GDT_Byte);
      const std::string valid_jpg_path = std::format("{}/valid_8x8.jpg", kVsiDir);
      auto valid_jpg = std::unique_ptr<GDALDataset>(
          jpeg_driver->CreateCopy(valid_jpg_path.c_str(), mem_ds.get(), FALSE,
                                  nullptr, nullptr, nullptr));
      ASSERT_THAT(valid_jpg, NotNull());
      valid_jpg.reset();

      vsi_l_offset nJpgSize = 0;
      GByte* pabyJpg =
          VSIGetMemFileBuffer(valid_jpg_path.c_str(), &nJpgSize, FALSE);
      ASSERT_THAT(pabyJpg, NotNull());

      FakeSourceDataset fake_ds(8, 8, 3, GDT_Byte);
      fake_ds.has_fake_jpeg_ = true;
      fake_ds.fake_jpeg_details_ =
          "bit_depth=8;colorspace=RGB;frame_type=SOF0_baseline";
      fake_ds.fake_jpeg_bytes_.assign(pabyJpg, pabyJpg + nJpgSize);
      fake_ds.GetRasterBand(1)->SetColorInterpretation(GCI_RedBand);
      fake_ds.GetRasterBand(2)->SetColorInterpretation(GCI_GreenBand);
      fake_ds.GetRasterBand(3)->SetColorInterpretation(GCI_BlueBand);
      fake_ds.has_per_dataset_mask_ = true;
      auto* band1 = static_cast<FakeRasterBand*>(fake_ds.GetRasterBand(1));
      band1->mask_band_->fail_rasterio_ = true;

      const std::string path = std::format("{}/fake_mask_fail.jxl", kVsiDir);
      WithQuietHandler quiet;
      CPLErrorReset();
      auto ds = std::unique_ptr<GDALDataset>(
          jxl_driver->CreateCopy(path.c_str(), &fake_ds, FALSE, nullptr,
                                 nullptr, nullptr));
      CPLErrorReset();
      EXPECT_EQ(nullptr, ds);
    }

    // Dataset RasterIO failure (hits line 2871)
    {
      FakeSourceDataset fake_ds(8, 8, 3, GDT_Byte);
      fake_ds.GetRasterBand(1)->SetColorInterpretation(GCI_RedBand);
      fake_ds.GetRasterBand(2)->SetColorInterpretation(GCI_GreenBand);
      fake_ds.GetRasterBand(3)->SetColorInterpretation(GCI_BlueBand);
      fake_ds.fail_dataset_rasterio_ = true;
      const std::string path = std::format("{}/fake_ds_rio_fail.jxl", kVsiDir);
      WithQuietHandler quiet;
      CPLErrorReset();
      auto ds = std::unique_ptr<GDALDataset>(
          jxl_driver->CreateCopy(path.c_str(), &fake_ds, FALSE, nullptr,
                                 nullptr, nullptr));
      CPLErrorReset();
      EXPECT_EQ(nullptr, ds);
    }

    // Band RasterIO failure on extra channel (hits line 2932)
    {
      FakeSourceDataset fake_ds(8, 8, 2, GDT_Byte);
      auto* band2 = static_cast<FakeRasterBand*>(fake_ds.GetRasterBand(2));
      band2->fail_rasterio_ = true;
      const std::string path = std::format("{}/fake_band_rio_fail.jxl", kVsiDir);
      WithQuietHandler quiet;
      CPLErrorReset();
      auto ds = std::unique_ptr<GDALDataset>(
          jxl_driver->CreateCopy(path.c_str(), &fake_ds, FALSE, nullptr,
                                 nullptr, nullptr));
      CPLErrorReset();
      EXPECT_EQ(nullptr, ds);
    }

    // Source dimensions exceed libjxl encoder limits (hits lines 2525-2530).
    // Note: on 64-bit platforms lines 2843, 2845 are defensive.
    {
      FakeSourceDataset huge_ds(2000000000, 2000000000, 3, GDT_Float32);
      const std::string path = std::format("{}/huge_dim.jxl", kVsiDir);
      WithQuietHandler quiet;
      CPLErrorReset();
      auto ds = std::unique_ptr<GDALDataset>(
          jxl_driver->CreateCopy(path.c_str(), &huge_ds, FALSE, nullptr,
                                 nullptr, nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(),
                  HasSubstr("JxlEncoderSetBasicInfo() failed"));
      CPLErrorReset();
      EXPECT_EQ(nullptr, ds);
    }
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Remaining uncovered lines in jpegxl.cpp are defensive OOM
// catch blocks, internal libjxl encoder failure branches, and #ifdef blocks for
// legacy libjxl versions.
