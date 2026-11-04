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
// Tests JPEG JFIF raster driver (jpgdataset.cpp).
//
// google3 build caveats:
//   - Built in 8-bit single-mode (neither JPEG_DUAL_MODE_8_12 nor JPEG_LIB_MK1
//     is defined), so 12-bit decompression/compression delegation paths are
//     not compiled; invalid precision and 16-bit source rejection/coercion are
//     pinned instead.
//   - libjpeg-turbo in //third_party/libjpeg_turbo defines D_LOSSLESS_SUPPORTED
//     and C_ARITH_CODING_SUPPORTED, enabling SOF3 lossless JPEG reading and
//     ARITHMETIC=YES creation, while GDALJPEGIsArithmeticCodingAvailable is
//     compiled out.
//   - CPLDebug and CPLDebugOnly are compiled out (-DWITHOUT_CPLDEBUG).
//
// See also:
//   https://gdal.org/drivers/raster/jpeg.html
//   https://github.com/OSGeo/gdal/blob/master/frmts/jpeg/jpgdataset.cpp
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/jpeg.py

#include "third_party/gdal/frmts/jpeg/jpgdataset.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>  // NOLINT
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/matchers.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/frmts/jpeg/jpegdrivercore.h"
#include "third_party/gdal/frmts/mem/memdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdalexif.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

constexpr char kVsiDir[] = "/vsimem/jpgdataset_test";

using CslDeleter = decltype([](char** p) { CSLDestroy(p); });
using StringListPtr = std::unique_ptr<char*, CslDeleter>;

constexpr auto kIdentifyMinimum = std::to_array<char>(
    {'\xff', '\xd8', '\xff', '\xee', '\xff', 0x0e, 0x00, 0x64, 0x41, '\x00'});
static_assert(kIdentifyMinimum.size() == 10);

// 1x1 black pixel grayscale JPEG with explicit DQT and DHT markers.
constexpr std::string_view kOneJpeg(
    "\xff\xd8\xff\xe0\x00\x10\x4a\x46\x49\x46\x00\x01\x01\x01\x00\x48\x00\x48"
    "\x00\x00\xff\xdb\x00\x43\x00\x08\x06\x06\x07\x06\x05\x08\x07\x07\x07\x09"
    "\x09\x08\x0a\x0c\x14\x0d\x0c\x0b\x0b\x0c\x19\x12\x13\x0f\x14\x1d\x1a\x1f"
    "\x1e\x1d\x1a\x1c\x1c\x20\x24\x2e\x27\x20\x22\x2c\x23\x1c\x1c\x28\x37\x29"
    "\x2c\x30\x31\x34\x34\x34\x1f\x27\x39\x3d\x38\x32\x3c\x2e\x33\x34\x32\xff"
    "\xc0\x00\x0b\x08\x00\x01\x00\x01\x01\x01\x11\x00\xff\xc4\x00\x14\x00\x01"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x08\xff\xc4"
    "\x00\x14\x10\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\xff\xda\x00\x08\x01\x01\x00\x00\x3f\x00\x3f\xbf\xff\xd9",
    160);

constexpr std::string_view kDatastreamContainsNoImage(
    "\xff\xd8\xff\x00\x00\x00\x00\x00\x00\x00\x0d\x64\x3d", 13);

std::vector<GByte> ReadVsiBytes(const char* path) {
  vsi_l_offset sz = 0;
  const GByte* raw = VSIGetMemFileBuffer(path, &sz, /*bUnlinkAndSeize=*/FALSE);
  if (!raw || sz == 0) return {};
  return std::vector<GByte>(raw, raw + sz);
}

void WriteVsiBytes(const char* path, std::span<const GByte> bytes) {
  auto* owned =
      static_cast<GByte*>(CPLMalloc(std::max<size_t>(1, bytes.size())));
  if (!bytes.empty()) {
    std::memcpy(owned, bytes.data(), bytes.size());
  }
  VSIFCloseL(VSIFileFromMemBuffer(path, owned, bytes.size(), TRUE));
}

template <typename T>
void AppendUInt(std::vector<GByte>& out, T v, bool little_endian) {
  for (size_t i = 0; i < sizeof(T); ++i) {
    const size_t shift = 8 * (little_endian ? i : (sizeof(T) - 1 - i));
    out.push_back(static_cast<GByte>((v >> shift) & 0xFF));
  }
}

void AppendBe16(std::vector<GByte>& out, uint16_t v) {
  AppendUInt(out, v, /*little_endian=*/false);
}

void AppendLe16(std::vector<GByte>& out, uint16_t v) {
  AppendUInt(out, v, /*little_endian=*/true);
}

void AppendBe32(std::vector<GByte>& out, uint32_t v) {
  AppendUInt(out, v, /*little_endian=*/false);
}

void AppendLe32(std::vector<GByte>& out, uint32_t v) {
  AppendUInt(out, v, /*little_endian=*/true);
}

void AppendU16(std::vector<GByte>& out, uint16_t v, bool little_endian) {
  AppendUInt(out, v, little_endian);
}

void AppendU32(std::vector<GByte>& out, uint32_t v, bool little_endian) {
  AppendUInt(out, v, little_endian);
}

void AppendF32(std::vector<GByte>& out, float f, bool little_endian) {
  AppendUInt(out, std::bit_cast<uint32_t>(f), little_endian);
}

void AppendF64(std::vector<GByte>& out, double d, bool little_endian) {
  AppendUInt(out, std::bit_cast<uint64_t>(d), little_endian);
}

std::vector<GByte> MakeMarkerSegment(GByte marker,
                                     std::span<const GByte> payload) {
  std::vector<GByte> seg;
  seg.reserve(4 + payload.size());
  seg.push_back(0xFF);
  seg.push_back(marker);
  AppendBe16(seg, static_cast<uint16_t>(payload.size() + 2));
  seg.insert(seg.end(), payload.begin(), payload.end());
  return seg;
}

std::vector<GByte> InjectAfterSoi(std::span<const GByte> jpeg,
                                  std::span<const GByte> extra) {
  std::vector<GByte> out;
  out.reserve(jpeg.size() + extra.size());
  out.insert(out.end(), jpeg.begin(), jpeg.begin() + 2);
  out.insert(out.end(), extra.begin(), extra.end());
  out.insert(out.end(), jpeg.begin() + 2, jpeg.end());
  return out;
}

std::vector<GByte> OneJpegBytes() {
  return std::vector<GByte>(
      reinterpret_cast<const GByte*>(kOneJpeg.data()),
      reinterpret_cast<const GByte*>(kOneJpeg.data()) + kOneJpeg.size());
}

// Creates a JPEG in /vsimem/ via CreateCopy from a MEMDataset and closes it.
std::vector<GByte> CreateJpegBytes(int width, int height, int bands,
                                   std::span<const GByte> pixels,
                                   CSLConstList options = nullptr,
                                   CSLConstList src_md = nullptr) {
  GDALRegister_JPEG();
  GDALDriver* jpg_drv = GetGDALDriverManager()->GetDriverByName("JPEG");
  if (!jpg_drv) return {};

  auto mem_ds = std::unique_ptr<GDALDataset>(
      MEMDataset::Create("", width, height, bands, GDT_Byte, nullptr));
  if (!mem_ds) return {};

  if (src_md) {
    mem_ds->SetMetadata(const_cast<char**>(src_md));
  }
  for (int b = 0; b < bands; ++b) {
    auto* band = mem_ds->GetRasterBand(b + 1);
    if (bands == 4) {
      band->SetColorInterpretation(
          static_cast<GDALColorInterp>(GCI_CyanBand + b));
    }
    if (band->RasterIO(
            GF_Write, 0, 0, width, height,
            const_cast<GByte*>(pixels.data() +
                               static_cast<size_t>(b) * width * height),
            width, height, GDT_Byte, 1, width, nullptr) != CE_None) {
      return {};
    }
  }

  const std::string tmp_path = VSIMemGenerateHiddenFilename("test_gen.jpg");
  absl::Cleanup cleanup = [&tmp_path] { VSIUnlink(tmp_path.c_str()); };
  GDALDatasetUniquePtr created(
      jpg_drv->CreateCopy(tmp_path.c_str(), mem_ds.get(), FALSE,
                          const_cast<char**>(options), nullptr, nullptr));
  created.reset();
  return ReadVsiBytes(tmp_path.c_str());
}

// Creates a 4-component JCS_YCCK JPEG (YCbCrK with Adobe APP14 transform=2).
std::vector<GByte> CreateYcckJpegBytes(int width, int height) {
  GDALRegister_JPEG();
  std::vector<GByte> pixels(width * height * 4);
  for (size_t i = 0; i < pixels.size(); ++i) {
    pixels[i] = static_cast<GByte>((i * 29 + 17) & 0xFF);
  }
  // Create 4-band CMYK JPEG first, then inject Adobe APP14 transform=2
  // so libjpeg treats the 4 components as YCCK.
  std::vector<GByte> cmyk = CreateJpegBytes(width, height, 4, pixels);
  constexpr GByte kAdobePayload[] = {'A',  'd',  'o',  'b',  'e',  0x00,
                                     0x64, 0x00, 0x00, 0x00, 0x00, 0x02};
  const std::vector<GByte> app14 = MakeMarkerSegment(0xEE, kAdobePayload);
  return InjectAfterSoi(cmyk, app14);
}

// Constructs a minimal valid 8-bit or 16-bit lossless (SOF3 0xC3) 1x1 JPEG.
std::vector<GByte> MakeLosslessSof3Jpeg(GByte precision) {
  std::vector<GByte> jpeg;
  // SOI
  jpeg.push_back(0xFF);
  jpeg.push_back(0xD8);
  // SOF3 (Lossless Huffman): length = 11, precision, height=1, width=1,
  // 1 component (id=1, samp=0x11, qtable=0)
  const GByte sof3[] = {precision, 0x00, 0x01, 0x00, 0x01,
                        0x01,      0x01, 0x11, 0x00};
  const auto sof3_seg = MakeMarkerSegment(0xC3, sof3);
  jpeg.insert(jpeg.end(), sof3_seg.begin(), sof3_seg.end());
  // DC Huffman table 0 with 1 symbol of bit-length 1 (symbol 0 -> value 0)
  const GByte dht[] = {0x00, 0x01, 0, 0, 0, 0, 0, 0, 0,
                       0,    0x00, 0, 0, 0, 0, 0, 0, 0x00};
  const auto dht_seg = MakeMarkerSegment(0xC4, dht);
  jpeg.insert(jpeg.end(), dht_seg.begin(), dht_seg.end());
  // SOS: 1 component (id=1, DC=0), Ss=1 (predictor 1), Se=0, Al=0
  const GByte sos[] = {0x01, 0x01, 0x00, 0x01, 0x00, 0x00};
  const auto sos_seg = MakeMarkerSegment(0xDA, sos);
  jpeg.insert(jpeg.end(), sos_seg.begin(), sos_seg.end());
  // Entropy-coded data (1 bit '0' padded with 1s -> 0x7F) + EOI
  jpeg.push_back(0x7F);
  jpeg.push_back(0xFF);
  jpeg.push_back(0xD9);
  return jpeg;
}

// Constructs a minimal 2-component SOF0 JPEG without JFIF/Adobe markers so
// libjpeg sets jpeg_color_space = JCS_UNKNOWN.
std::vector<GByte> MakeUnknownColorspaceTwoComponentJpeg() {
  std::vector<GByte> jpeg;
  jpeg.push_back(0xFF);
  jpeg.push_back(0xD8);
  // Quantization table 0 (all 1s)
  std::vector<GByte> dqt(65, 1);
  dqt[0] = 0x00;
  const auto dqt_seg = MakeMarkerSegment(0xDB, dqt);
  jpeg.insert(jpeg.end(), dqt_seg.begin(), dqt_seg.end());
  // SOF0 with 2 components (ids 1 and 2)
  const GByte sof0[] = {0x08, 0x00, 0x01, 0x00, 0x01, 0x02,
                        0x01, 0x11, 0x00, 0x02, 0x11, 0x00};
  const auto sof0_seg = MakeMarkerSegment(0xC0, sof0);
  jpeg.insert(jpeg.end(), sof0_seg.begin(), sof0_seg.end());
  // SOS with 2 components
  const GByte sos[] = {0x02, 0x01, 0x00, 0x02, 0x00, 0x00, 0x3F, 0x00};
  const auto sos_seg = MakeMarkerSegment(0xDA, sos);
  jpeg.insert(jpeg.end(), sos_seg.begin(), sos_seg.end());
  jpeg.push_back(0xFF);
  jpeg.push_back(0xD9);
  return jpeg;
}

// Appends a zlib-compressed mask and 4-byte LE original JPEG size trailer.
std::vector<GByte> AppendCompressedMaskTrailer(
    std::span<const GByte> jpeg, std::span<const GByte> raw_mask_bits) {
  std::vector<GByte> compressed(
      std::max<size_t>(64, raw_mask_bits.size() + 64));
  size_t comp_size = 0;
  void* res = CPLZLibDeflate(raw_mask_bits.data(), raw_mask_bits.size(), -1,
                             compressed.data(), compressed.size(), &comp_size);
  if (!res) return {};
  compressed.resize(comp_size);

  std::vector<GByte> out(jpeg.begin(), jpeg.end());
  out.insert(out.end(), compressed.begin(), compressed.end());
  AppendLe32(out, static_cast<uint32_t>(jpeg.size()));
  return out;
}

TEST(JpegIdentifyTest, IdentifyDoesNotExist) {
  auto open_info = std::make_unique<GDALOpenInfo>("/does_not_exist",
                                                  GDAL_OF_READONLY, nullptr);
  ASSERT_NE(nullptr, open_info);
  EXPECT_FALSE(JPEGDriverIdentify(open_info.get()));
}

TEST(JpegIdentifyTest, Subfile) {
  auto open_info = std::make_unique<GDALOpenInfo>(
      "JPEG_SUBFILE:/does_not_exist", GDAL_OF_READONLY, nullptr);
  ASSERT_NE(nullptr, open_info);
  EXPECT_TRUE(JPEGDriverIdentify(open_info.get()));
}

TEST(JpegIdentifyTest, MagicOnly) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  const std::string filename = std::format("{}/header-only.jpg", kVsiDir);
  VsiMemTempWrapper wrapper(
      filename, std::string(kIdentifyMinimum.data(), kIdentifyMinimum.size()));
  auto open_info = std::make_unique<GDALOpenInfo>(filename.c_str(),
                                                  GDAL_OF_READONLY, nullptr);
  ASSERT_NE(nullptr, open_info);
  EXPECT_TRUE(JPEGDriverIdentify(open_info.get()));
}

TEST(JpegDatasetCommonTest, One) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  const std::string filename = std::format("{}/one.jpg", kVsiDir);
  VsiMemTempWrapper wrapper(filename, std::string(kOneJpeg));
  auto open_info = std::make_unique<GDALOpenInfo>(filename.c_str(),
                                                  GDAL_OF_READONLY, nullptr);
  ASSERT_NE(nullptr, open_info);
  auto src =
      std::unique_ptr<GDALDataset>(JPGDatasetCommon::Open(open_info.get()));
  ASSERT_NE(nullptr, src);
  GDALGeoTransform gt;
  EXPECT_EQ(CE_Failure, src->GetGeoTransform(gt));
  EXPECT_THAT(std::span<const double>(gt.data(), 6),
              ElementsAre(0, 1, 0, 0, 0, 1));
  EXPECT_EQ(0, src->GetGCPCount());
  EXPECT_EQ(nullptr, src->GetGCPSpatialRef());
  EXPECT_EQ(nullptr, src->GetGCPs());
  EXPECT_EQ(1, src->GetRasterXSize());
  EXPECT_EQ(1, src->GetRasterYSize());
  EXPECT_EQ(1, src->GetRasterCount());

  // No file level metadata.
  EXPECT_EQ(nullptr, src->GetMetadata());

  GDALRasterBand* band = src->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  int block_xsize = 0;
  int block_ysize = 0;
  band->GetBlockSize(&block_xsize, &block_ysize);
  EXPECT_EQ(1, block_xsize);
  EXPECT_EQ(1, block_ysize);
  EXPECT_EQ(GDT_Byte, band->GetRasterDataType());
  EXPECT_EQ(GCI_GrayIndex, band->GetColorInterpretation());
  EXPECT_EQ(GSBAP_TOP_TO_BOTTOM, band->GetSuggestedBlockAccessPattern());
  EXPECT_EQ(GMF_ALL_VALID, band->GetMaskFlags());

  double minmax[2] = {0.0, 0.0};
  ASSERT_EQ(CE_None, band->ComputeRasterMinMax(false, minmax));
  EXPECT_THAT(minmax, ElementsAre(0, 0));

  // band owns metadata.
  CSLConstList metadata_list = band->GetMetadata();
  EXPECT_EQ(nullptr, metadata_list);
}

TEST(JpegDatasetCommonTest, NoImage) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  CPLErrorReset();
  const std::string filename = std::format("{}/no-image.jpg", kVsiDir);
  VsiMemTempWrapper wrapper(filename, std::string(kDatastreamContainsNoImage));
  auto open_info = std::make_unique<GDALOpenInfo>(filename.c_str(),
                                                  GDAL_OF_READONLY, nullptr);
  ASSERT_NE(nullptr, open_info);

  WithQuietHandler error_handler;

  auto src =
      std::unique_ptr<GDALDataset>(JPGDatasetCommon::Open(open_info.get()));
  ASSERT_EQ(nullptr, src);
  // Expect these two issues:
  // libjpeg: Premature end of JPEG file
  // libjpeg: JPEG datastream contains no image
  // Only JERR_NO_IMAGE from libjpeg will remain.
  EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(2, CPLGetErrorCounter());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("contains no image"));
}

TEST(JpegDatasetCommonTest, b64482898_CorruptICCProfile) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  CPLErrorReset();
  // poc-9c3b06aec99bca6a6c945a7b8ee15e81a9472d243009684ef84a6bfddaf61393
  constexpr GByte kData[] =
      "\xff\xd8\xff\xc1\x00\x14\x08\x6a\x70\x67\x04\x04\x04\x34\x00\x00\x14\x00"
      "\x00\x21\x00\xc1\x14\x00\xff\xda\x00\x0e\x04\x04\x28\x00\x00\x00\x00\xc1"
      "\xda\xff\xff\xff\xff\xe2\x00\x0f\x49\x43\x43\x5f\x50\x52\x4f\x46\x49\x4c"
      "\x45\x00\x01\x01\x2f\x00";
  const std::string filename = std::format("{}/corrupt_icc.jpg", kVsiDir);
  VsiMemTempWrapper wrapper(
      filename,
      std::string(reinterpret_cast<const char*>(kData), sizeof(kData)));

  auto open_info = std::make_unique<GDALOpenInfo>(filename.c_str(),
                                                  GDAL_OF_READONLY, nullptr);
  ASSERT_NE(nullptr, open_info);

  auto src =
      std::unique_ptr<GDALDataset>(JPGDatasetCommon::Open(open_info.get()));
  ASSERT_NE(nullptr, src);

  WithQuietHandler error_handler;
  // This hits an issue with the corrupt ICCProfile.
  CSLDestroy(src->GetMetadataDomainList());

  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_LT(0, CPLGetErrorCounter());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("nICCChunkLength"));
}

TEST(JpegDatasetCommonTest, DriverRegistrationAndMetadataInitialization) {
  GDALRegister_JPEG();
  // Calling GDALRegister_JPEG a second time hits early return.
  GDALRegister_JPEG();

  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("JPEG");
  ASSERT_NE(nullptr, drv);

  // Querying a metadata item other than GDAL_DMD_CREATIONOPTIONLIST first keeps
  // m_bMetadataInitialized false, then querying GDAL_DMD_CREATIONOPTIONLIST
  // initializes it, and GetMetadata returns the cached list.
  EXPECT_STREQ("JPEG JFIF", drv->GetMetadataItem(GDAL_DMD_LONGNAME));
  const char* creation_opts = drv->GetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST);
  ASSERT_NE(nullptr, creation_opts);
  EXPECT_THAT(creation_opts, HasSubstr("PROGRESSIVE"));
  EXPECT_THAT(creation_opts, HasSubstr("LOSSLESS_COPY"));
  EXPECT_THAT(creation_opts, HasSubstr("ARITHMETIC"));
  EXPECT_THAT(creation_opts, HasSubstr("EXIF_THUMBNAIL"));

  // Second call hits m_bMetadataInitialized == true early return.
  EXPECT_NE(nullptr, drv->GetMetadata());
  EXPECT_STREQ(creation_opts,
               drv->GetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST));
}

TEST(JpegDatasetCommonTest, OpenRejectsInvalidAndUpdateRequests) {
  GDALRegister_JPEG();

  // 1. Non-JPEG input rejected via JPEGDriverIdentify.
  {
    GDALOpenInfo info("/vsimem/nonexistent_file.txt", GDAL_OF_READONLY,
                      nullptr);
    EXPECT_EQ(nullptr,
              std::unique_ptr<GDALDataset>(JPGDatasetCommon::Open(&info)));
  }

  // 2. GA_Update rejected with ReportUpdateNotSupportedByDriver.
  {
    CPLErrorReset();
    WithQuietHandler quiet;
    GDALOpenInfo info("JPEG_SUBFILE:0,10,/vsimem/dummy.jpg", GDAL_OF_UPDATE,
                      nullptr);
    EXPECT_EQ(nullptr,
              std::unique_ptr<GDALDataset>(JPGDatasetCommon::Open(&info)));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("update"));
  }

  // 3. Invalid JPEG: subdataset token count != 3 and unknown subdataset name.
  {
    GDALOpenInfo bad_tokens("JPEG:/vsimem/dummy.jpg", GDAL_OF_READONLY,
                            nullptr);
    EXPECT_EQ(nullptr, std::unique_ptr<GDALDataset>(
                           JPGDatasetCommon::Open(&bad_tokens)));

    absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
    const std::string path = std::format("{}/valid.jpg", kVsiDir);
    VsiMemTempWrapper file(path, std::string(kOneJpeg));

    const std::string unknown_sub =
        std::format("JPEG:\"{}\":UNKNOWN_SUBDATASET", path);
    GDALOpenInfo unknown_info(unknown_sub.c_str(), GDAL_OF_READONLY, nullptr);
    EXPECT_EQ(nullptr, std::unique_ptr<GDALDataset>(
                           JPGDatasetCommon::Open(&unknown_info)));
  }
}

TEST(JpegDatasetCommonTest, SubfilesAndAbbreviatedDefaultQuantTables) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALRegister_JPEG();

  // Build an abbreviated 1x1 JPEG without DQT or DHT markers (relies on
  // LoadDefaultTables(0) for Q1..Q5).
  std::vector<GByte> abbrev;
  abbrev.push_back(0xFF);
  abbrev.push_back(0xD8);
  const GByte sof0[] = {0x08, 0x00, 0x01, 0x00, 0x01, 0x01, 0x01, 0x11, 0x00};
  const auto sof0_seg = MakeMarkerSegment(0xC0, sof0);
  abbrev.insert(abbrev.end(), sof0_seg.begin(), sof0_seg.end());
  const GByte sos[] = {0x01, 0x01, 0x00, 0x00, 0x3F, 0x00};
  const auto sos_seg = MakeMarkerSegment(0xDA, sos);
  abbrev.insert(abbrev.end(), sos_seg.begin(), sos_seg.end());
  // Entropy-coded data using LoadDefaultTables Huffman codes:
  // DC diff = 0 ("00") + AC EOB ("1010") + padding ("11") -> 0x2B, followed by
  // EOI.
  abbrev.push_back(0x2B);
  abbrev.push_back(0xFF);
  abbrev.push_back(0xD9);

  // Prefix with 64 padding bytes to verify non-zero subfile offset seeking.
  std::vector<GByte> container(64, 0xAA);
  container.insert(container.end(), abbrev.begin(), abbrev.end());
  const std::string subfile_path =
      std::format("{}/abbreviated_nitf.bin", kVsiDir);
  WriteVsiBytes(subfile_path.c_str(), container);

  // 1. Test Q1 through Q5 subfiles: Open, read scanline 0, flush (triggers
  // Restart() -> LoadDefaultTables), and re-read scanline 0.
  for (int q = 1; q <= 5; ++q) {
    SCOPED_TRACE(q);
    const std::string conn = std::format("JPEG_SUBFILE:Q{},64,{},{}", q,
                                         abbrev.size(), subfile_path);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(conn.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(1, ds->GetRasterXSize());
    EXPECT_EQ(1, ds->GetRasterYSize());

    GByte pixel = 42;
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &pixel, 1, 1,
                                             GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(128, pixel);
    // GDALPamDataset::GetFileList() returns nullptr for JPEG_SUBFILE paths.
    StringListPtr files(ds->GetFileList());
    EXPECT_EQ(nullptr, files.get());
    EXPECT_EQ(CE_None, ds->FlushCache(false));
    pixel = 99;
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &pixel, 1, 1,
                                             GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(128, pixel);
  }

  // 2. Test Q0 and Q9 (LoadDefaultTables early return -> Huffman table missing
  // when decoding).
  for (int bad_q : {0, 9}) {
    WithQuietHandler quiet;
    CPLErrorReset();
    const std::string conn = std::format("JPEG_SUBFILE:Q{},64,{},{}", bad_q,
                                         abbrev.size(), subfile_path);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(conn.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    GByte pixel = 0;
    EXPECT_EQ(CE_Failure,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &pixel, 1, 1,
                                             GDT_Byte, 0, 0, nullptr));
  }

  // 3. Test non-Q subfile at non-zero offset with full standalone JPEG.
  std::vector<GByte> standalone_container(32, 0x55);
  standalone_container.insert(standalone_container.end(), kOneJpeg.begin(),
                              kOneJpeg.end());
  const std::string full_sub_path = std::format("{}/full_sub.bin", kVsiDir);
  WriteVsiBytes(full_sub_path.c_str(), standalone_container);
  {
    const std::string conn =
        std::format("JPEG_SUBFILE:32,{},{}", kOneJpeg.size(), full_sub_path);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(conn.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    GByte pixel = 255;
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &pixel, 1, 1,
                                             GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(0, pixel);
  }

  // 4. Corrupt subfile syntax errors.
  for (const char* bad_conn : {
           "JPEG_SUBFILE:Q1,only_one",
           "JPEG_SUBFILE:no_comma",
           "JPEG_SUBFILE:0,10",
           "JPEG_SUBFILE:0,10,/vsimem/missing_backing_file.bin",
       }) {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALOpenInfo info(bad_conn, GDAL_OF_READONLY, nullptr);
    EXPECT_EQ(nullptr,
              std::unique_ptr<GDALDataset>(JPGDatasetCommon::Open(&info)))
        << bad_conn;
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType()) << bad_conn;
  }
}

TEST(JpegDatasetCommonTest, ColorSpacesLosslessAndPrecisionChecks) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALRegister_JPEG();

  constexpr int kWidth = 16;
  constexpr int kHeight = 8;

  // 1. 3-band YCbCr (default) opened with GDAL_JPEG_TO_RGB=YES and =NO.
  std::vector<GByte> rgb_pixels(kWidth * kHeight * 3);
  for (size_t i = 0; i < rgb_pixels.size(); ++i) {
    rgb_pixels[i] = static_cast<GByte>((i * 19 + 5) & 0xFF);
  }
  const std::vector<GByte> ycbcr_jpg =
      CreateJpegBytes(kWidth, kHeight, 3, rgb_pixels);
  const std::string ycbcr_path = std::format("{}/ycbcr.jpg", kVsiDir);
  WriteVsiBytes(ycbcr_path.c_str(), ycbcr_jpg);

  {
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(ycbcr_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(3, ds->GetRasterCount());
    EXPECT_STREQ("YCbCr",
                 ds->GetMetadataItem("SOURCE_COLOR_SPACE", "IMAGE_STRUCTURE"));
    EXPECT_EQ(GCI_RedBand, ds->GetRasterBand(1)->GetColorInterpretation());
    EXPECT_EQ(GCI_GreenBand, ds->GetRasterBand(2)->GetColorInterpretation());
    EXPECT_EQ(GCI_BlueBand, ds->GetRasterBand(3)->GetColorInterpretation());
  }

  {
    CPLConfigOptionSetter raw_ycbcr("GDAL_JPEG_TO_RGB", "NO", false);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(ycbcr_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(3, ds->GetRasterCount());
    EXPECT_EQ(GCI_YCbCr_YBand, ds->GetRasterBand(1)->GetColorInterpretation());
    EXPECT_EQ(GCI_YCbCr_CbBand, ds->GetRasterBand(2)->GetColorInterpretation());
    EXPECT_EQ(GCI_YCbCr_CrBand, ds->GetRasterBand(3)->GetColorInterpretation());
  }

  // 2. 3-band JCS_RGB (created with JPEG_WRITE_RGB=YES).
  {
    CPLConfigOptionSetter write_rgb("JPEG_WRITE_RGB", "YES", false);
    const std::vector<GByte> rgb_jpg =
        CreateJpegBytes(kWidth, kHeight, 3, rgb_pixels);
    const std::string rgb_path = std::format("{}/rgb.jpg", kVsiDir);
    WriteVsiBytes(rgb_path.c_str(), rgb_jpg);

    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(rgb_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(GCI_RedBand, ds->GetRasterBand(1)->GetColorInterpretation());
  }

  // 3. 4-band JCS_CMYK opened with GDAL_JPEG_TO_RGB=YES (CMYK->RGB conversion
  // across bands 1..3 + IRasterIO CMYK guard) and =NO (raw 4-band CMYK).
  std::vector<GByte> cmyk_pixels(kWidth * kHeight * 4);
  for (size_t i = 0; i < cmyk_pixels.size(); ++i) {
    cmyk_pixels[i] = static_cast<GByte>(128 + ((i * 7) & 0x3F));
  }
  const std::vector<GByte> cmyk_jpg =
      CreateJpegBytes(kWidth, kHeight, 4, cmyk_pixels);
  const std::string cmyk_path = std::format("{}/cmyk.jpg", kVsiDir);
  WriteVsiBytes(cmyk_path.c_str(), cmyk_jpg);

  {
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(cmyk_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(3, ds->GetRasterCount());
    EXPECT_STREQ("CMYK",
                 ds->GetMetadataItem("SOURCE_COLOR_SPACE", "IMAGE_STRUCTURE"));
    std::vector<GByte> buf(kWidth * kHeight * 3);
    EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, kWidth, kHeight, buf.data(),
                                    kWidth, kHeight, GDT_Byte, 3, nullptr, 3,
                                    kWidth * 3, 1, nullptr));
  }

  {
    CPLConfigOptionSetter raw_cmyk("GDAL_JPEG_TO_RGB", "NO", false);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(cmyk_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(4, ds->GetRasterCount());
    EXPECT_EQ(GCI_CyanBand, ds->GetRasterBand(1)->GetColorInterpretation());
    EXPECT_EQ(GCI_MagentaBand, ds->GetRasterBand(2)->GetColorInterpretation());
    EXPECT_EQ(GCI_YellowBand, ds->GetRasterBand(3)->GetColorInterpretation());
    EXPECT_EQ(GCI_BlackBand, ds->GetRasterBand(4)->GetColorInterpretation());
    std::vector<GByte> buf(kWidth * kHeight * 4);
    EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, kWidth, kHeight, buf.data(),
                                    kWidth, kHeight, GDT_Byte, 4, nullptr, 4,
                                    kWidth * 4, 1, nullptr));
  }

  // 4. 4-band JCS_YCCK opened with GDAL_JPEG_TO_RGB=YES (YCbCrK->RGB) and =NO.
  const std::vector<GByte> ycck_jpg = CreateYcckJpegBytes(kWidth, kHeight);
  const std::string ycck_path = std::format("{}/ycck.jpg", kVsiDir);
  WriteVsiBytes(ycck_path.c_str(), ycck_jpg);

  {
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(ycck_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(3, ds->GetRasterCount());
    EXPECT_STREQ("YCbCrK",
                 ds->GetMetadataItem("SOURCE_COLOR_SPACE", "IMAGE_STRUCTURE"));
    std::vector<GByte> buf(kWidth * kHeight * 3);
    EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, kWidth, kHeight, buf.data(),
                                    kWidth, kHeight, GDT_Byte, 3, nullptr, 3,
                                    kWidth * 3, 1, nullptr));
  }

  {
    CPLConfigOptionSetter raw_ycck("GDAL_JPEG_TO_RGB", "NO", false);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(ycck_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(4, ds->GetRasterCount());
    EXPECT_EQ(GCI_YCbCr_YBand, ds->GetRasterBand(1)->GetColorInterpretation());
    EXPECT_EQ(GCI_BlackBand, ds->GetRasterBand(4)->GetColorInterpretation());
  }

  // 5. JCS_UNKNOWN (2-component JPEG) rejected cleanly.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    const std::vector<GByte> unk_jpg = MakeUnknownColorspaceTwoComponentJpeg();
    const std::string unk_path = std::format("{}/unknown_cs.jpg", kVsiDir);
    WriteVsiBytes(unk_path.c_str(), unk_jpg);

    GDALOpenInfo info(unk_path.c_str(), GDAL_OF_READONLY, nullptr);
    EXPECT_EQ(nullptr,
              std::unique_ptr<GDALDataset>(JPGDatasetCommon::Open(&info)));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Unrecognized jpeg_color_space"));
  }

  // 6. 8-bit SOF3 lossless JPEG -> COMPRESSION_REVERSIBILITY=LOSSLESS.
  {
    const std::vector<GByte> ls_jpg = MakeLosslessSof3Jpeg(8);
    const std::string ls_path = std::format("{}/lossless8.jpg", kVsiDir);
    WriteVsiBytes(ls_path.c_str(), ls_jpg);

    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(ls_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_STREQ("LOSSLESS", ds->GetMetadataItem("COMPRESSION_REVERSIBILITY",
                                                 "IMAGE_STRUCTURE"));
  }

  // 7. 16-bit SOF3 lossless JPEG -> precision != 8 && != 12 rejected.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    const std::vector<GByte> ls16_jpg = MakeLosslessSof3Jpeg(16);
    const std::string ls16_path = std::format("{}/lossless16.jpg", kVsiDir);
    WriteVsiBytes(ls16_path.c_str(), ls16_jpg);

    GDALOpenInfo info(ls16_path.c_str(), GDAL_OF_READONLY, nullptr);
    EXPECT_EQ(nullptr,
              std::unique_ptr<GDALDataset>(JPGDatasetCommon::Open(&info)));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("precision of other than 8 or 12 bits"));
  }
}

TEST(JpegDatasetCommonTest, RasterIoStridesScanlineOrderAndErrors) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALRegister_JPEG();

  constexpr int kWidth = 24;
  constexpr int kHeight = 12;
  std::vector<GByte> pixels(kWidth * kHeight * 3);
  for (int b = 0; b < 3; ++b) {
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        pixels[b * kWidth * kHeight + y * kWidth + x] =
            static_cast<GByte>((x * 9 + y * 5 + b * 40) & 0xFF);
      }
    }
  }

  const std::vector<GByte> jpg = CreateJpegBytes(kWidth, kHeight, 3, pixels);
  const std::string path = std::format("{}/stride.jpg", kVsiDir);
  WriteVsiBytes(path.c_str(), jpg);

  GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
      GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                 nullptr, nullptr)));
  ASSERT_NE(nullptr, ds);

  // 1. Direct IRasterIO with nullptr panBandMap returns CE_Failure.
  {
    std::vector<GByte> dummy(3);
    EXPECT_EQ(CE_Failure,
              cpl::down_cast<JPGDatasetCommon*>(ds.get())->IRasterIO(
                  GF_Read, 0, 0, 1, 1, dummy.data(), 1, 1, GDT_Byte, 1, nullptr,
                  1, 1, 1, nullptr));
  }

  // 2. Full-extent 3-band pixel-interleaved (nBandSpace == 1, nPixelSpace ==
  // 3).
  std::vector<GByte> bip_3(kWidth * kHeight * 3, 0);
  int band_map[3] = {1, 2, 3};
  ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, kWidth, kHeight, bip_3.data(),
                                  kWidth, kHeight, GDT_Byte, 3, band_map, 3,
                                  kWidth * 3, 1, nullptr));

  // 3. Full-extent 3-band with nBandSpace == 1 and nPixelSpace == 4.
  std::vector<GByte> bip_4(kWidth * kHeight * 4, 0xEE);
  ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, kWidth, kHeight, bip_4.data(),
                                  kWidth, kHeight, GDT_Byte, 3, band_map, 4,
                                  kWidth * 4, 1, nullptr));
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      EXPECT_EQ(bip_3[(y * kWidth + x) * 3 + 0],
                bip_4[(y * kWidth + x) * 4 + 0]);
      EXPECT_EQ(bip_3[(y * kWidth + x) * 3 + 1],
                bip_4[(y * kWidth + x) * 4 + 1]);
      EXPECT_EQ(bip_3[(y * kWidth + x) * 3 + 2],
                bip_4[(y * kWidth + x) * 4 + 2]);
      EXPECT_EQ(0xEE, bip_4[(y * kWidth + x) * 4 + 3]);
    }
  }

  // 4. Full-extent 3-band BSQ (nBandSpace == kWidth * kHeight > 1).
  std::vector<GByte> bsq(kWidth * kHeight * 3, 0);
  ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, kWidth, kHeight, bsq.data(),
                                  kWidth, kHeight, GDT_Byte, 3, band_map, 1,
                                  kWidth, kWidth * kHeight, nullptr));
  for (int b = 0; b < 3; ++b) {
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        EXPECT_EQ(bip_3[(y * kWidth + x) * 3 + b],
                  bsq[b * kWidth * kHeight + y * kWidth + x]);
      }
    }
  }

  // 5. Partial subwindow read, cached scanline re-read, and reverse scanline
  // read (triggers Restart() in LoadScanline), then GF_Write failure.
  std::vector<GByte> sub(8 * 4 * 3, 0);
  EXPECT_EQ(CE_None, ds->RasterIO(GF_Read, 4, 2, 8, 4, sub.data(), 8, 4,
                                  GDT_Byte, 3, band_map, 3, 8 * 3, 1, nullptr));
  std::vector<GByte> row_buf(kWidth);
  EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 5, kWidth, 1,
                                                    row_buf.data(), kWidth, 1,
                                                    GDT_Byte, 0, 0, nullptr));
  // Reverse read (row 2 < nLoadedScanline) triggers Restart().
  EXPECT_EQ(CE_None, ds->GetRasterBand(2)->RasterIO(GF_Read, 0, 2, kWidth, 1,
                                                    row_buf.data(), kWidth, 1,
                                                    GDT_Byte, 0, 0, nullptr));

  // GF_Write on GA_ReadOnly dataset delegates to GDALPamDataset::IRasterIO.
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, ds->RasterIO(GF_Write, 0, 0, 1, 1, row_buf.data(), 1, 1,
                                     GDT_Byte, 1, band_map, 1, 1, 1, nullptr));
}

TEST(JpegDatasetCommonTest, ProgressiveMemoryLimitOverviewSwitchAndMaxScans) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALRegister_JPEG();

  // 1. Huge progressive SOF2 JPEG (24576x24576, 1 band -> ~1.21 GB required >
  // 500 MB default max_memory_to_use when JPEGMEM is unset) fails with the
  // GDAL_ALLOW_LARGE_LIBJPEG_MEM_ALLOC hint in StartDecompress().
  {
    CPLConfigOptionSetter no_jpegmem("JPEGMEM", nullptr, false);
    CPLStringList create_opts;
    create_opts.SetNameValue("PROGRESSIVE", "YES");
    std::vector<GByte> huge_prog = CreateJpegBytes(
        32, 32, 1, std::vector<GByte>(32 * 32, 64), create_opts.List());
    // Patch SOF2 (0xFF 0xC2) dimensions to 24576 x 24576 (0x6000 x 0x6000).
    for (size_t i = 0; i + 8 < huge_prog.size(); ++i) {
      if (huge_prog[i] == 0xFF && huge_prog[i + 1] == 0xC2) {
        huge_prog[i + 5] = 0x60;
        huge_prog[i + 6] = 0x00;
        huge_prog[i + 7] = 0x60;
        huge_prog[i + 8] = 0x00;
        break;
      }
    }
    const std::string huge_path = std::format("{}/huge_prog.jpg", kVsiDir);
    WriteVsiBytes(huge_path.c_str(), huge_prog);

    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(huge_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    std::vector<GByte> row(24576);
    EXPECT_EQ(CE_Failure, ds->GetRasterBand(1)->RasterIO(
                              GF_Read, 0, 0, 24576, 1, row.data(), 24576, 1,
                              GDT_Byte, 0, 0, nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("GDAL_ALLOW_LARGE_LIBJPEG_MEM_ALLOC"));
  }

  // 2. Real 3072x3072 3-band progressive JPEG (~57 MB > 10 MB overview-stop
  // threshold on main DS, ~15 MB > 10 MB on overview 0): alternating reads
  // exercise *ppoActiveDS->StopDecompress() in StartDecompress() and
  // Restart(), !bHasDoneJpegCreateDecompress, and JPEGMEM="1M" override.
  constexpr int kDim = 3072;
  std::vector<GByte> pixels(kDim * kDim * 3, 128);
  CPLStringList create_opts;
  create_opts.SetNameValue("PROGRESSIVE", "YES");
  const std::vector<GByte> prog_jpg =
      CreateJpegBytes(kDim, kDim, 3, pixels, create_opts.List());
  const std::string path = std::format("{}/progressive_3k.jpg", kVsiDir);
  WriteVsiBytes(path.c_str(), prog_jpg);

  // JPEGMEM="1M" skips SetMaxMemoryToUse 500MB floor (max_memory_to_use == 0).
  {
    CPLConfigOptionSetter mem_limit("JPEGMEM", "1M", false);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    std::vector<GByte> row(kDim);
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, kDim, 1, row.data(),
                                             kDim, 1, GDT_Byte, 0, 0, nullptr));
  }

  // Default memory limit: alternating reads between main dataset (3072x3072)
  // and overview 0 (1536x1536, > 10 MB) exercises
  // *ppoActiveDS->StopDecompress() in StartDecompress() and Restart(), plus
  // !bHasDoneJpegCreateDecompress.
  {
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    ASSERT_GT(ds->GetRasterBand(1)->GetOverviewCount(), 0);
    GDALRasterBand* ovr0 = ds->GetRasterBand(1)->GetOverview(0);
    ASSERT_NE(nullptr, ovr0);

    std::vector<GByte> main_row(kDim);
    std::vector<GByte> ovr_row(kDim / 2);
    // Read scanline 0 on main DS -> sets *ppoActiveDS = main DS.
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, kDim, 1,
                                                      main_row.data(), kDim, 1,
                                                      GDT_Byte, 0, 0, nullptr));
    // Read scanline 0 on overview 0 -> stops main DS
    // (*ppoActiveDS->StopDecompress()).
    ASSERT_EQ(CE_None,
              ovr0->RasterIO(GF_Read, 0, 0, kDim / 2, 1, ovr_row.data(),
                             kDim / 2, 1, GDT_Byte, 0, 0, nullptr));
    // Read scanline 1 on main DS -> !bHasDoneJpegCreateDecompress -> Restart()
    // -> stops overview 0!
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 1, kDim, 1,
                                                      main_row.data(), kDim, 1,
                                                      GDT_Byte, 0, 0, nullptr));
  }

  // 3. GDAL_JPEG_MAX_ALLOWED_SCAN_NUMBER=1 aborts in ProgressMonitor().
  {
    CPLConfigOptionSetter max_scans("GDAL_JPEG_MAX_ALLOWED_SCAN_NUMBER", "1",
                                    false);
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    std::vector<GByte> row(kDim);
    EXPECT_EQ(CE_Failure,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, kDim, 1, row.data(),
                                             kDim, 1, GDT_Byte, 0, 0, nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("exceeds maximum scans"));
  }
}

TEST(JpegDatasetCommonTest, EmitMessageAndNonFatalWarningConfigModes) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALRegister_JPEG();

  constexpr int kWidth = 32;
  constexpr int kHeight = 32;
  std::vector<GByte> pixels(kWidth * kHeight, 128);
  for (size_t i = 0; i < pixels.size(); ++i) {
    pixels[i] = static_cast<GByte>((i * 41) & 0xFF);
  }
  const std::vector<GByte> full_jpg =
      CreateJpegBytes(kWidth, kHeight, 1, pixels);

  // 1. Truncated JPEG ("Premature end of JPEG file" -> JWRN_JPEG_EOF at EOF
  // without EOI marker) across all three modes of
  // GDAL_ERROR_ON_LIBJPEG_WARNING: unset (failure with hint), "TRUE" (failure
  // without hint), and "FALSE" (warning, read succeeds).
  std::vector<GByte> trunc_jpg(full_jpg.begin(), full_jpg.end() - 10);
  const std::string trunc_path = std::format("{}/truncated.jpg", kVsiDir);
  WriteVsiBytes(trunc_path.c_str(), trunc_jpg);

  // 1a. Unset: CE_Failure + hint message.
  {
    CPLConfigOptionSetter unset_opt("GDAL_ERROR_ON_LIBJPEG_WARNING", nullptr,
                                    false);
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(trunc_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    std::vector<GByte> out(kWidth * kHeight);
    EXPECT_EQ(CE_Failure, ds->GetRasterBand(1)->RasterIO(
                              GF_Read, 0, 0, kWidth, kHeight, out.data(),
                              kWidth, kHeight, GDT_Byte, 0, 0, nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("GDAL_ERROR_ON_LIBJPEG_WARNING to FALSE"));
  }

  // 1b. "TRUE": CE_Failure without hint.
  {
    CPLConfigOptionSetter err_true("GDAL_ERROR_ON_LIBJPEG_WARNING", "TRUE",
                                   false);
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(trunc_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    std::vector<GByte> out(kWidth * kHeight);
    EXPECT_EQ(CE_Failure, ds->GetRasterBand(1)->RasterIO(
                              GF_Read, 0, 0, kWidth, kHeight, out.data(),
                              kWidth, kHeight, GDT_Byte, 0, 0, nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("libjpeg:"));
  }

  // 1c. "FALSE": CE_Warning, RasterIO succeeds.
  {
    CPLConfigOptionSetter err_false("GDAL_ERROR_ON_LIBJPEG_WARNING", "FALSE",
                                    false);
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(trunc_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    std::vector<GByte> out(kWidth * kHeight);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, kWidth, kHeight, out.data(), kWidth,
                           kHeight, GDT_Byte, 0, 0, nullptr));
  }

  // 2. Non-EOF warning (JFIF version 2.01 -> JWRN_JFIF_MAJOR) across all
  // three modes of GDAL_ERROR_ON_LIBJPEG_WARNING: unset (CE_Warning + hint),
  // "FALSE" (CE_Warning without hint), and "TRUE" (CE_Failure -> Open fails).
  constexpr GByte kJfifV2Payload[] = {'J',  'F',  'I',  'F',  0x00, 0x02, 0x01,
                                      0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00};
  const std::vector<GByte> jfif_v2_jpg =
      InjectAfterSoi(OneJpegBytes(), MakeMarkerSegment(0xE0, kJfifV2Payload));
  const std::string jfif_v2_path = std::format("{}/jfif_v2.jpg", kVsiDir);
  WriteVsiBytes(jfif_v2_path.c_str(), jfif_v2_jpg);

  // 2a. Unset: opens with warning containing hint.
  {
    CPLConfigOptionSetter unset_opt("GDAL_ERROR_ON_LIBJPEG_WARNING", nullptr,
                                    false);
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(jfif_v2_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr)));
    EXPECT_NE(nullptr, ds);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("GDAL_ERROR_ON_LIBJPEG_WARNING to TRUE"));
  }

  // 2b. "FALSE": opens with plain warning.
  {
    CPLConfigOptionSetter err_false("GDAL_ERROR_ON_LIBJPEG_WARNING", "FALSE",
                                    false);
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(jfif_v2_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr)));
    EXPECT_NE(nullptr, ds);
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  }

  // 2c. "TRUE": emits CE_Failure on Open and fails in LoadScanline via
  // ErrorOutOnNonFatalError().
  {
    CPLConfigOptionSetter err_true("GDAL_ERROR_ON_LIBJPEG_WARNING", "TRUE",
                                   false);
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALOpenInfo info(jfif_v2_path.c_str(), GDAL_OF_READONLY, nullptr);
    auto ds = std::unique_ptr<GDALDataset>(JPGDatasetCommon::Open(&info));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    ASSERT_NE(nullptr, ds);
    GByte px = 0;
    EXPECT_EQ(CE_Failure,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &px, 1, 1,
                                             GDT_Byte, 0, 0, nullptr));
  }
}

TEST(JpegDatasetCommonTest, MetadataImageStructureCommentExifAndXmp) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALRegister_JPEG();

  constexpr int kWidth = 8;
  constexpr int kHeight = 8;
  std::vector<GByte> rgb(kWidth * kHeight * 3, 64);

  // 1. IMAGE_STRUCTURE JPEG_QUALITY for 3-band YCbCr (QUALITY=85), 1-band
  // grayscale (QUALITY=75 in kOneJpeg), custom non-standard DQT (no
  // JPEG_QUALITY), and COM marker (separate file so COM precedes DQT cleanly).
  {
    CPLStringList opts;
    opts.SetNameValue("QUALITY", "85");
    std::vector<GByte> jpg =
        CreateJpegBytes(kWidth, kHeight, 3, rgb, opts.List());
    // Inject an empty COM marker (length=2 -> nChunkLength == 2 branch)
    // right before SOS (0xFF 0xDA), after DQT (0xDB).
    for (size_t i = 2; i + 1 < jpg.size(); ++i) {
      if (jpg[i] == 0xFF && jpg[i + 1] == 0xDA) {
        const auto empty_com = MakeMarkerSegment(0xFE, {});
        jpg.insert(jpg.begin() + i, empty_com.begin(), empty_com.end());
        break;
      }
    }
    const std::string path = std::format("{}/quality85.jpg", kVsiDir);
    WriteVsiBytes(path.c_str(), jpg);

    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    // Non-JPEG_QUALITY IMAGE_STRUCTURE query does not read DQT yet; then
    // JPEG_QUALITY triggers ReadImageStructureMetadata; second call hits cache.
    EXPECT_STREQ("PIXEL", ds->GetMetadataItem("INTERLEAVE", "IMAGE_STRUCTURE"));
    EXPECT_STREQ("85", ds->GetMetadataItem("JPEG_QUALITY", "IMAGE_STRUCTURE"));
    EXPECT_STREQ("85", ds->GetMetadataItem("JPEG_QUALITY", "IMAGE_STRUCTURE"));
    // Hit bHasReadImageStructureMetadata early return via GetMetadata().
    EXPECT_NE(nullptr, ds->GetMetadata("IMAGE_STRUCTURE"));
    // No EXIF or COM on quality85.jpg; second call hits bHasReadEXIFMetadata.
    EXPECT_EQ(nullptr, ds->GetMetadata());
    EXPECT_EQ(nullptr, ds->GetMetadata());

    // Separate file with non-empty COMMENT creation option.
    CPLStringList com_opts;
    com_opts.SetNameValue("COMMENT", "Test JPEG Comment");
    const std::vector<GByte> com_jpg = CreateJpegBytes(
        kWidth, kHeight, 1, std::vector<GByte>(kWidth * kHeight, 32),
        com_opts.List());
    const std::string com_path = std::format("{}/with_comment.jpg", kVsiDir);
    WriteVsiBytes(com_path.c_str(), com_jpg);
    GDALDatasetUniquePtr com_ds(GDALDataset::FromHandle(
        GDALOpenEx(com_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, com_ds);
    EXPECT_STREQ("Test JPEG Comment", com_ds->GetMetadataItem("COMMENT"));
    EXPECT_NE(nullptr, com_ds->GetMetadata());
  }

  {
    // Perturb DQT table values (note: all-1s is quality 100 in IJG tables!)
    // so MD5 does not match any standard quality 1..100.
    std::vector<GByte> custom_dqt = OneJpegBytes();
    for (size_t i = 2; i + 10 < custom_dqt.size(); ++i) {
      if (custom_dqt[i] == 0xFF && custom_dqt[i + 1] == 0xDB) {
        custom_dqt[i + 5] ^= 0x55;
        break;
      }
    }
    const std::string one_path =
        std::format("{}/custom_dqt_struct.jpg", kVsiDir);
    WriteVsiBytes(one_path.c_str(), custom_dqt);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(one_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    // Custom DQT does not match any md5JPEGQuantTable_generic_8bit[1..100] ->
    // JPEG_QUALITY is nullptr.
    EXPECT_EQ(nullptr, ds->GetMetadataItem("JPEG_QUALITY", "IMAGE_STRUCTURE"));
  }

  // 2. Build a synthetic Little-Endian EXIF APP1 block with:
  // - IFD0: Model (0x110), DNG_UniqueCameraModel (0xC614, same -> unset),
  //         DNG_CameraSerialNumber (0xC62F, diff -> kept), ExifIFD (0x8769),
  //         GPSIFD (0x8825), EXIF_XmlPacket (0x2BC -> xml:XMP)
  // - ExifIFD: BodySerialNumber (0xA431), InteropIFD (0xA005)
  // - InteropIFD: Index "R98" (0x0001)
  // - GPSIFD: Version 2.3.0.0 (0x0000), GPSLatitudeRef "N" (0x0001)
  // Plus a duplicate Exif APP1 to exercise the duplicate Exif warning/skip.
  auto BuildExifApp1 = [](bool little_endian) {
    std::vector<GByte> tiff;
    // Header: magic, version 42, ifd0 offset = 8
    tiff.push_back(little_endian ? 'I' : 'M');
    tiff.push_back(little_endian ? 'I' : 'M');
    AppendU16(tiff, 42, little_endian);
    AppendU32(tiff, 8, little_endian);

    const char* model = "PixCam\0";
    const char* serial = "SN-999\0";
    const char* body_sn = "SN-001\0";
    const char* xml_pkt =
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
        "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
        "<rdf:Description xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "dc:title=\"FromExif\"/></rdf:RDF></x:xmpmeta>\0";
    const uint32_t xml_len = static_cast<uint32_t>(std::strlen(xml_pkt) + 1);

    // Layout:
    // Offset 8: IFD0 (6 entries: 2 + 6*12 + 4 = 78 bytes -> ends at 86)
    const uint32_t exif_ifd_off = 86;    // 2 entries: 2 + 2*12 + 4 = 30 -> 116
    const uint32_t inter_ifd_off = 116;  // 1 entry: 2 + 1*12 + 4 = 18 -> 134
    const uint32_t gps_ifd_off = 134;    // 1 entry: 2 + 1*12 + 4 = 18 -> 152
    const uint32_t model_off = 152;      // 7 bytes -> 159 (pad to 160)
    const uint32_t serial_off = 160;     // 7 bytes -> 167 (pad to 168)
    const uint32_t body_sn_off = 168;    // 7 bytes -> 175 (pad to 176)
    const uint32_t xml_off = 176;

    auto add_entry = [&](uint16_t tag, uint16_t type, uint32_t count,
                         uint32_t val_or_off) {
      AppendU16(tiff, tag, little_endian);
      AppendU16(tiff, type, little_endian);
      AppendU32(tiff, count, little_endian);
      AppendU32(tiff, val_or_off, little_endian);
    };

    // IFD0
    AppendU16(tiff, 6, little_endian);
    add_entry(0x0110, TIFF_ASCII, 7, model_off);      // EXIF_Model
    add_entry(0x02BC, TIFF_ASCII, xml_len, xml_off);  // EXIF_XmlPacket
    add_entry(0x8769, TIFF_LONG, 1, exif_ifd_off);    // ExifIFD
    add_entry(0x8825, TIFF_LONG, 1, gps_ifd_off);     // GPSIFD
    add_entry(0xC614, TIFF_ASCII, 7,
              model_off);  // DNG_UniqueCameraModel (matches EXIF_Model)
    add_entry(
        0xC62F, TIFF_ASCII, 7,
        serial_off);  // DNG_CameraSerialNumber (differs from BodySerialNumber)
    AppendU32(tiff, 0, little_endian);

    // ExifIFD
    AppendU16(tiff, 2, little_endian);
    add_entry(0xA005, TIFF_LONG, 1, inter_ifd_off);  // InteropIFD
    add_entry(0xA431, TIFF_ASCII, 7, body_sn_off);   // EXIF_BodySerialNumber
    AppendU32(tiff, 0, little_endian);

    // InteropIFD: Index "R98" (4 bytes inline)
    AppendU16(tiff, 1, little_endian);
    AppendU16(tiff, 0x0001, little_endian);
    AppendU16(tiff, TIFF_ASCII, little_endian);
    AppendU32(tiff, 4, little_endian);
    tiff.insert(tiff.end(), {'R', '9', '8', '\0'});
    AppendU32(tiff, 0, little_endian);

    // GPSIFD: GPSLatitudeRef "N" (2 bytes inline + 2 zero pad)
    AppendU16(tiff, 1, little_endian);
    AppendU16(tiff, 0x0001, little_endian);
    AppendU16(tiff, TIFF_ASCII, little_endian);
    AppendU32(tiff, 2, little_endian);
    tiff.insert(tiff.end(), {'N', '\0', 0, 0});
    AppendU32(tiff, 0, little_endian);

    // Strings
    tiff.insert(tiff.end(), model, model + 7);
    tiff.push_back(0);
    tiff.insert(tiff.end(), serial, serial + 7);
    tiff.push_back(0);
    tiff.insert(tiff.end(), body_sn, body_sn + 7);
    tiff.push_back(0);
    tiff.insert(tiff.end(), xml_pkt, xml_pkt + xml_len);

    std::vector<GByte> payload = {'E', 'x', 'i', 'f', '\0', '\0'};
    payload.insert(payload.end(), tiff.begin(), tiff.end());
    return MakeMarkerSegment(0xE1, payload);
  };

  for (bool little_endian : {true, false}) {
    SCOPED_TRACE(little_endian);
    std::vector<GByte> jpg(kOneJpeg.begin(), kOneJpeg.end());
    const auto exif1 = BuildExifApp1(little_endian);
    // Inject duplicate Exif markers (second is ignored).
    jpg = InjectAfterSoi(jpg, exif1);
    jpg = InjectAfterSoi(jpg, exif1);
    const std::string path =
        std::format("{}/exif_{}.jpg", kVsiDir, little_endian ? "le" : "be");
    WriteVsiBytes(path.c_str(), jpg);

    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_STREQ("PixCam", ds->GetMetadataItem("EXIF_Model"));
    // DNG_UniqueCameraModel == EXIF_Model -> unset!
    EXPECT_EQ(nullptr, ds->GetMetadataItem("DNG_UniqueCameraModel"));
    // DNG_CameraSerialNumber != EXIF_BodySerialNumber -> kept!
    EXPECT_STREQ("SN-999", ds->GetMetadataItem("DNG_CameraSerialNumber"));
    EXPECT_STREQ("SN-001", ds->GetMetadataItem("EXIF_BodySerialNumber"));
    EXPECT_STREQ("R98", ds->GetMetadataItem("EXIF_Interoperability_Index"));
    EXPECT_STREQ("N", ds->GetMetadataItem("EXIF_GPSLatitudeRef"));
    // XMP extracted from EXIF_XmlPacket:
    CSLConstList xmp = ds->GetMetadata("xml:XMP");
    ASSERT_NE(nullptr, xmp);
    ASSERT_NE(nullptr, xmp[0]);
    EXPECT_THAT(xmp[0], HasSubstr("FromExif"));
    EXPECT_EQ(nullptr, ds->GetMetadataItem("EXIF_XmlPacket"));
  }

  // Also test Pix4D case where DNG_CameraSerialNumber == EXIF_BodySerialNumber
  // (unsets DNG_CameraSerialNumber).
  {
    std::vector<GByte> jpg = CreateJpegBytes(kWidth, kHeight, 3, rgb);
    // Build minimal LE EXIF with both DNG_CameraSerialNumber and ExifIFD
    // BodySerialNumber = "MATCH-SN"
    std::vector<GByte> tiff = {'I', 'I', 42, 0, 8, 0, 0, 0};
    AppendLe16(tiff, 2);  // 2 entries -> ends at 8 + 2 + 24 + 4 = 38
    AppendLe16(tiff, 0x8769);
    AppendLe16(tiff, TIFF_LONG);
    AppendLe32(tiff, 1);
    AppendLe32(tiff, 38);
    AppendLe16(tiff, 0xC62F);
    AppendLe16(tiff, TIFF_ASCII);
    AppendLe32(tiff, 9);
    AppendLe32(tiff, 56);
    AppendLe32(tiff, 0);
    // ExifIFD at 38 (1 entry -> ends at 38 + 2 + 12 + 4 = 56)
    AppendLe16(tiff, 1);
    AppendLe16(tiff, 0xA431);
    AppendLe16(tiff, TIFF_ASCII);
    AppendLe32(tiff, 9);
    AppendLe32(tiff, 56);
    AppendLe32(tiff, 0);
    const char* sn = "MATCH-SN\0";
    tiff.insert(tiff.end(), sn, sn + 9);
    std::vector<GByte> payload = {'E', 'x', 'i', 'f', '\0', '\0'};
    payload.insert(payload.end(), tiff.begin(), tiff.end());
    jpg = InjectAfterSoi(jpg, MakeMarkerSegment(0xE1, payload));
    const std::string path = std::format("{}/pix4d_sn.jpg", kVsiDir);
    WriteVsiBytes(path.c_str(), jpg);

    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_STREQ("MATCH-SN", ds->GetMetadataItem("EXIF_BodySerialNumber"));
    EXPECT_EQ(nullptr, ds->GetMetadataItem("DNG_CameraSerialNumber"));
  }

  // 3. EXIFInit TIFF header error cases: bad magic and bad version.
  struct BadExifCase {
    std::string_view name;
    std::string_view tiff_bytes;
    std::string_view err_substr;
  };
  constexpr BadExifCase kBadExifCases[] = {
      {.name = "bad_magic",
       .tiff_bytes = std::string_view("\x12\x34\x2a\x00\x08\x00\x00\x00", 8),
       .err_substr = "bad magic number"},
      {.name = "bad_version",
       .tiff_bytes = std::string_view("\x49\x49\x63\x00\x08\x00\x00\x00", 8),
       .err_substr = "bad version number"},
  };
  for (const auto& tc : kBadExifCases) {
    SCOPED_TRACE(tc.name);
    WithQuietHandler quiet;
    CPLErrorReset();
    std::vector<GByte> payload = {'E', 'x', 'i', 'f', '\0', '\0'};
    payload.insert(payload.end(),
                   reinterpret_cast<const GByte*>(tc.tiff_bytes.data()),
                   reinterpret_cast<const GByte*>(tc.tiff_bytes.data()) +
                       tc.tiff_bytes.size());
    std::vector<GByte> jpg = InjectAfterSoi(
        std::span<const GByte>(reinterpret_cast<const GByte*>(kOneJpeg.data()),
                               kOneJpeg.size()),
        MakeMarkerSegment(0xE1, payload));
    const std::string path =
        std::format("{}/bad_exif_{}.jpg", kVsiDir, tc.name);
    WriteVsiBytes(path.c_str(), jpg);

    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    ds->GetMetadata();
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr(tc.err_substr));
  }

  // 4. ReadXMPMetadata & GetSpatialRef (Pix4D Camera:HorizCS + Camera:VertCS:
  // 2D EPSG:4326, ellipsoidal 3D, vertical CRS compound CS, and invalid/empty).
  auto MakePix4dXmpJpeg = [](std::string_view extra_attrs) {
    std::string xml = std::format(
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
        "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
        "<rdf:Description xmlns:Camera=\"http://pix4d.com/camera/1.0/\" {}/>"
        "</rdf:RDF></x:xmpmeta>",
        extra_attrs);
    constexpr char kSig[] = "http://ns.adobe.com/xap/1.0/";
    std::vector<GByte> payload(kSig, kSig + sizeof(kSig));  // includes '\0'
    payload.insert(payload.end(), xml.begin(), xml.end());
    return InjectAfterSoi(
        std::span<const GByte>(reinterpret_cast<const GByte*>(kOneJpeg.data()),
                               kOneJpeg.size()),
        MakeMarkerSegment(0xE1, payload));
  };

  // 4a. 2D EPSG:4326
  {
    const auto jpg = MakePix4dXmpJpeg("Camera:HorizCS=\"EPSG:4326\"");
    const std::string path = std::format("{}/xmp_4326.jpg", kVsiDir);
    WriteVsiBytes(path.c_str(), jpg);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    const OGRSpatialReference* srs = ds->GetSpatialRef();
    ASSERT_NE(nullptr, srs);
    OGRSpatialReference expected;
    expected.importFromEPSG(4326);
    expected.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    EXPECT_THAT(*srs, IsSameAs(expected));
    // Second call returns cached m_oSRS.
    EXPECT_EQ(srs, ds->GetSpatialRef());
  }

  // 4b. Ellipsoidal 3D
  {
    const auto jpg = MakePix4dXmpJpeg(
        "Camera:HorizCS=\"EPSG:32632\" Camera:VertCS=\"ellipsoidal\"");
    const std::string path = std::format("{}/xmp_3d.jpg", kVsiDir);
    WriteVsiBytes(path.c_str(), jpg);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    const OGRSpatialReference* srs = ds->GetSpatialRef();
    ASSERT_NE(nullptr, srs);
    EXPECT_TRUE(srs->IsGeographic() || srs->IsProjected());
    EXPECT_EQ(3, srs->GetAxesCount());
  }

  // 4c. Compound CS with vertical datum EPSG:5773
  {
    const auto jpg = MakePix4dXmpJpeg(
        "Camera:HorizCS=\"EPSG:32632\" Camera:VertCS=\"EPSG:5773\"");
    const std::string path = std::format("{}/xmp_compound.jpg", kVsiDir);
    WriteVsiBytes(path.c_str(), jpg);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    const OGRSpatialReference* srs = ds->GetSpatialRef();
    ASSERT_NE(nullptr, srs);
    EXPECT_TRUE(srs->IsCompound());
  }

  // 4d. Empty XMP payload (nXMPLength == 0) + invalid HorizCS returns nullptr.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    constexpr char kSig[] = "http://ns.adobe.com/xap/1.0/";
    std::vector<GByte> empty_xmp_payload(kSig, kSig + sizeof(kSig));
    std::vector<GByte> jpg = InjectAfterSoi(
        std::span<const GByte>(reinterpret_cast<const GByte*>(kOneJpeg.data()),
                               kOneJpeg.size()),
        MakeMarkerSegment(0xE1, empty_xmp_payload));
    const std::string path = std::format("{}/xmp_empty.jpg", kVsiDir);
    WriteVsiBytes(path.c_str(), jpg);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(nullptr, ds->GetSpatialRef());
  }
}

TEST(JpegDatasetCommonTest, WorldFilesTabFilesAndFileList) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALRegister_JPEG();

  // 1. Worldfile (.jpw) alongside .jpg.
  {
    const std::string jpg_path = std::format("{}/world_case.jpg", kVsiDir);
    const std::string jpw_path = std::format("{}/world_case.jpw", kVsiDir);
    VsiMemTempWrapper jpg(jpg_path, std::string(kOneJpeg));
    VsiMemTempWrapper jpw(jpw_path, "2.5\n0.0\n0.0\n-2.5\n100.0\n200.0\n");

    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(jpg_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt;
    EXPECT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(2.5, gt[1]);
    EXPECT_DOUBLE_EQ(-2.5, gt[5]);

    StringListPtr files(ds->GetFileList());
    EXPECT_NE(-1, CSLFindString(files.get(), jpg_path.c_str()));
    EXPECT_NE(-1, CSLFindString(files.get(), jpw_path.c_str()));
  }

  // 2. JPEG file ending in ".wld" (TIROS3 naming): must not treat itself as
  // its own worldfile.
  {
    const std::string wld_img_path = std::format("{}/tiros3.wld", kVsiDir);
    VsiMemTempWrapper wld_img(wld_img_path, std::string(kOneJpeg));
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(wld_img_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt;
    EXPECT_EQ(CE_Failure, ds->GetGeoTransform(gt));
  }

  // 3. MapInfo .tab file with non-affine GCPs -> GetGCPCount > 0,
  // GetGCPSpatialRef != nullptr.
  {
    const std::string jpg_path = std::format("{}/tab_gcp.jpg", kVsiDir);
    const std::string tab_path = std::format("{}/tab_gcp.tab", kVsiDir);
    VsiMemTempWrapper jpg(jpg_path, std::string(kOneJpeg));
    VsiMemTempWrapper tab(tab_path, R"tab(!table
!version 300
Definition Table
  File "tab_gcp.jpg"
  Type "RASTER"
  (10.0, 50.0) (0, 0) Label "P1",
  (20.0, 51.0) (100, 0) Label "P2",
  (20.0, 40.0) (100, 100) Label "P3",
  (10.0, 40.0) (0, 100) Label "P4"
  CoordSys Earth Projection 1, 104
  Units "degree"
)tab");

    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(jpg_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(4, ds->GetGCPCount());
    EXPECT_NE(nullptr, ds->GetGCPSpatialRef());
    EXPECT_NE(nullptr, ds->GetGCPs());
    GDALGeoTransform gt;
    EXPECT_EQ(CE_Failure, ds->GetGeoTransform(gt));
    StringListPtr files(ds->GetFileList());
    EXPECT_NE(-1, CSLFindString(files.get(), tab_path.c_str()));
  }

  // 4. MapInfo .tab file with exact affine rectangle -> nGCPCount == 0,
  // bGeoTransformValid = true.
  {
    const std::string jpg_path = std::format("{}/tab_affine.jpg", kVsiDir);
    const std::string tab_path = std::format("{}/tab_affine.tab", kVsiDir);
    VsiMemTempWrapper jpg(jpg_path, std::string(kOneJpeg));
    VsiMemTempWrapper tab(tab_path, R"tab(!table
!version 300
Definition Table
  File "tab_affine.jpg"
  Type "RASTER"
  (10.0, 50.0) (0, 0) Label "P1",
  (20.0, 50.0) (100, 0) Label "P2",
  (20.0, 40.0) (100, 100) Label "P3",
  (10.0, 40.0) (0, 100) Label "P4"
  CoordSys Earth Projection 1, 104
  Units "degree"
)tab");

    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(jpg_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(0, ds->GetGCPCount());
    GDALGeoTransform gt;
    EXPECT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(10.0, gt[0]);
  }

  // 5. In-memory PAM metadata overrides GetGeoTransform, GetSpatialRef,
  // GetGCPCount, GetGCPSpatialRef, and GetGCPs via GDALPamDataset base.
  {
    const std::string jpg_path = std::format("{}/pam_override.jpg", kVsiDir);
    VsiMemTempWrapper jpg(jpg_path, std::string(kOneJpeg));
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(jpg_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);

    GDALGeoTransform set_gt(1.0, 2.0, 0.0, 4.0, 0.0, -5.0);
    EXPECT_EQ(CE_None, ds->SetGeoTransform(set_gt));
    GDALGeoTransform gt;
    EXPECT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(2.0, gt[1]);

    OGRSpatialReference srs;
    srs.importFromEPSG(4326);
    EXPECT_EQ(CE_None, ds->SetSpatialRef(&srs));
    EXPECT_NE(nullptr, ds->GetSpatialRef());

    GDAL_GCP gcp = {const_cast<char*>("1"),
                    const_cast<char*>("P1"),
                    0.0,
                    0.0,
                    10.0,
                    20.0,
                    0.0};
    EXPECT_EQ(CE_None, ds->SetGCPs(1, &gcp, &srs));
    EXPECT_EQ(1, ds->GetGCPCount());
    EXPECT_NE(nullptr, ds->GetGCPSpatialRef());
    EXPECT_NE(nullptr, ds->GetGCPs());
  }
}

TEST(JpegDatasetCommonTest, ThermalMetadataAndRawThermalSubdatasets) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALRegister_JPEG();
  GDALRegister_PNG();

  // Helper to build a single-entry EXIF APP1 with EXIF_Make = "DJI".
  auto BuildDjiExif = []() {
    std::vector<GByte> tiff = {'I', 'I', 42, 0, 8, 0, 0, 0};
    AppendLe16(tiff, 1);       // 1 entry -> ends at 8 + 2 + 12 + 4 = 26
    AppendLe16(tiff, 0x010F);  // EXIF_Make
    AppendLe16(tiff, TIFF_ASCII);
    AppendLe32(tiff, 4);
    tiff.insert(tiff.end(), {'D', 'J', 'I', '\0'});
    AppendLe32(tiff, 0);
    std::vector<GByte> payload = {'E', 'x', 'i', 'f', '\0', '\0'};
    payload.insert(payload.end(), tiff.begin(), tiff.end());
    return MakeMarkerSegment(0xE1, payload);
  };

  // 1. DJI thermal multi-chunk 0xE3 on 16x8 image + OpenRawThermalImage.
  {
    std::vector<GByte> img =
        CreateJpegBytes(16, 8, 1, std::vector<GByte>(16 * 8, 100));
    img = InjectAfterSoi(img, BuildDjiExif());
    std::vector<GByte> raw_thermal(16 * 8 * 2);
    for (int i = 0; i < 16 * 8; ++i) {
      raw_thermal[2 * i + 0] = static_cast<GByte>((1000 + i) & 0xFF);
      raw_thermal[2 * i + 1] = static_cast<GByte>(((1000 + i) >> 8) & 0xFF);
    }
    const size_t half = raw_thermal.size() / 2;
    img = InjectAfterSoi(
        img, MakeMarkerSegment(0xE3, std::span<const GByte>(
                                         raw_thermal.data() + half, half)));
    img = InjectAfterSoi(
        img, MakeMarkerSegment(
                 0xE3, std::span<const GByte>(raw_thermal.data(), half)));
    const std::string path = std::format("{}/dji_thermal.jpg", kVsiDir);
    WriteVsiBytes(path.c_str(), img);

    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    CSLConstList subs = ds->GetMetadata("SUBDATASETS");
    ASSERT_NE(nullptr, subs);
    const std::string sub_conn =
        std::format("JPEG:\"{}\":DJI_RAW_THERMAL_IMAGE", path);
    EXPECT_STREQ(sub_conn.c_str(),
                 ds->GetMetadataItem("SUBDATASET_1_NAME", "SUBDATASETS"));
    EXPECT_STREQ("16", ds->GetMetadataItem("RawThermalImageWidth", "DJI"));
    EXPECT_STREQ("8", ds->GetMetadataItem("RawThermalImageHeight", "DJI"));

    GDALDatasetUniquePtr thermal_ds(GDALDataset::FromHandle(
        GDALOpenEx(sub_conn.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, thermal_ds);
    EXPECT_EQ(16, thermal_ds->GetRasterXSize());
    EXPECT_EQ(8, thermal_ds->GetRasterYSize());
    EXPECT_EQ(GDT_UInt16, thermal_ds->GetRasterBand(1)->GetRasterDataType());
    uint16_t val = 0;
    EXPECT_EQ(CE_None,
              thermal_ds->GetRasterBand(1)->RasterIO(
                  GF_Read, 3, 2, 1, 1, &val, 1, 1, GDT_UInt16, 0, 0, nullptr));
    EXPECT_EQ(1000 + 2 * 16 + 3, val);
  }

  // 2. DJI M4T (1280x1024 JPEG with 640x512*2 thermal bytes), plus DJI error
  // warnings: oversized dimensions (> 10000*10000*2), buffer bigger than
  // expected, and mismatched byte size.
  {
    // M4T: patch SOF0 height=1024 (0x0400), width=1280 (0x0500) on a tiny JPEG
    std::vector<GByte> m4t_img(kOneJpeg.begin(), kOneJpeg.end());
    for (size_t i = 0; i + 8 < m4t_img.size(); ++i) {
      if (m4t_img[i] == 0xFF && m4t_img[i + 1] == 0xC0) {
        m4t_img[i + 5] = 0x04;
        m4t_img[i + 6] = 0x00;  // height = 1024
        m4t_img[i + 7] = 0x05;
        m4t_img[i + 8] = 0x00;  // width = 1280
        break;
      }
    }
    m4t_img = InjectAfterSoi(m4t_img, BuildDjiExif());
    // 640 * 512 * 2 = 655360 bytes split into 20 chunks of 32768 bytes each
    // (inserted in reverse so file order is 0..19).
    const std::vector<GByte> chunk(32768, 0x22);
    const std::vector<GByte> seg = MakeMarkerSegment(0xE3, chunk);
    for (int i = 0; i < 20; ++i) {
      m4t_img = InjectAfterSoi(m4t_img, seg);
    }
    const std::string m4t_path = std::format("{}/dji_m4t.jpg", kVsiDir);
    WriteVsiBytes(m4t_path.c_str(), m4t_img);
    GDALDatasetUniquePtr m4t_ds(GDALDataset::FromHandle(
        GDALOpenEx(m4t_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, m4t_ds);
    EXPECT_STREQ("640", m4t_ds->GetMetadataItem("RawThermalImageWidth", "DJI"));
    EXPECT_STREQ("512",
                 m4t_ds->GetMetadataItem("RawThermalImageHeight", "DJI"));

    // Oversized dimensions (12000 x 12000 -> expected > 10000*10000*2)
    WithQuietHandler quiet;
    CPLErrorReset();
    std::vector<GByte> big_img(kOneJpeg.begin(), kOneJpeg.end());
    for (size_t i = 0; i + 8 < big_img.size(); ++i) {
      if (big_img[i] == 0xFF && big_img[i + 1] == 0xC0) {
        big_img[i + 5] = 0x2E;
        big_img[i + 6] = 0xE0;  // 12000
        big_img[i + 7] = 0x2E;
        big_img[i + 8] = 0xE0;  // 12000
        break;
      }
    }
    const std::vector<GByte> small_e3 = {0x01, 0x02, 0x03, 0x04};
    big_img = InjectAfterSoi(big_img, MakeMarkerSegment(0xE3, small_e3));
    big_img = InjectAfterSoi(big_img, BuildDjiExif());
    const std::string big_path = std::format("{}/dji_big.jpg", kVsiDir);
    WriteVsiBytes(big_path.c_str(), big_img);
    GDALDatasetUniquePtr big_ds(GDALDataset::FromHandle(
        GDALOpenEx(big_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, big_ds);
    big_ds->GetMetadata("DJI");
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("too big for thermal data"));

    // Buffer bigger than expected (1x1 -> expected 2 bytes, given 6 bytes)
    CPLErrorReset();
    std::vector<GByte> overbuf_img(kOneJpeg.begin(), kOneJpeg.end());
    const std::vector<GByte> six_bytes(6, 0x11);
    overbuf_img =
        InjectAfterSoi(overbuf_img, MakeMarkerSegment(0xE3, six_bytes));
    overbuf_img = InjectAfterSoi(overbuf_img, BuildDjiExif());
    const std::string overbuf_path = std::format("{}/dji_overbuf.jpg", kVsiDir);
    WriteVsiBytes(overbuf_path.c_str(), overbuf_img);
    GDALDatasetUniquePtr over_ds(GDALDataset::FromHandle(
        GDALOpenEx(overbuf_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, over_ds);
    over_ds->GetMetadata("DJI");
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("buffer bigger than expected"));

    // Sizes do not match (4x1 -> expected 8 bytes, given 4 bytes)
    CPLErrorReset();
    std::vector<GByte> mismatch_img =
        CreateJpegBytes(4, 1, 1, std::vector<GByte>(4, 50));
    mismatch_img =
        InjectAfterSoi(mismatch_img, MakeMarkerSegment(0xE3, small_e3));
    mismatch_img = InjectAfterSoi(mismatch_img, BuildDjiExif());
    const std::string mismatch_path =
        std::format("{}/dji_mismatch.jpg", kVsiDir);
    WriteVsiBytes(mismatch_path.c_str(), mismatch_img);
    GDALDatasetUniquePtr mm_ds(GDALDataset::FromHandle(
        GDALOpenEx(mismatch_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, mm_ds);
    mm_ds->GetMetadata("DJI");
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("sizes do not match"));
  }

  // 3. Comprehensive FLIR FFF metadata (multi-chunk APP1 "FLIR\0", LE & BE,
  // nByteOrder == 512 byte-swap, CameraInfo with humidity > 2 &
  // positive/negative timezone, PaletteInfo with RGB palette entries, GPSInfo
  // with 0x01000000 endian flip, RawData raw 16-bit and PNG 16-bit + error
  // paths).
  auto BuildFlirJpeg = [](bool little_endian, uint16_t rec_byte_order,
                          int16_t tz_minutes, float humidity,
                          std::span<const GByte> raw_payload_override = {}) {
    // 7 directory entries -> 7 * 32 = 224 bytes at offset 64 (ends at 288):
    // 0: FLIR_REC_FREE (0, len=0)
    // 1: Invalid OOB record
    // 2: Unknown record type 99
    // 3: FLIR_REC_CAMERA_INFO (32, len=1126) at 288
    // 4: FLIR_REC_PALETTE_INFO (34, len=118) at 1424
    // 5: FLIR_REC_GPS_INFO (43, len=104) at 1552
    // 6: FLIR_REC_RAWDATA (1) at 1664
    std::vector<GByte> flir(1664, 0);
    std::memcpy(flir.data(), "FFF\0TestCreator\0", 16);

    auto write_u32 = [&](size_t off, uint32_t v) {
      std::vector<GByte> tmp;
      AppendU32(tmp, v, little_endian);
      std::memcpy(flir.data() + off, tmp.data(), 4);
    };
    auto write_u16 = [&](size_t off, uint16_t v) {
      std::vector<GByte> tmp;
      AppendU16(tmp, v, little_endian);
      std::memcpy(flir.data() + off, tmp.data(), 2);
    };
    auto write_f32 = [&](size_t off, float v, bool le) {
      std::vector<GByte> tmp;
      AppendF32(tmp, v, le);
      std::memcpy(flir.data() + off, tmp.data(), 4);
    };
    auto write_f64 = [&](size_t off, double v, bool le) {
      std::vector<GByte> tmp;
      AppendF64(tmp, v, le);
      std::memcpy(flir.data() + off, tmp.data(), 8);
    };

    write_u32(20, 100);  // file format version 100
    write_u32(24, 64);   // record directory offset
    write_u32(28, 7);    // 7 entries

    auto set_dir = [&](int idx, uint16_t type, uint32_t off, uint32_t len) {
      const size_t base = 64 + static_cast<size_t>(idx) * 32;
      write_u16(base + 0, type);
      write_u32(base + 12, off);
      write_u32(base + 16, len);
    };
    set_dir(0, 0, 0, 0);           // FLIR_REC_FREE empty
    set_dir(1, 32, 9999999, 100);  // out-of-bounds record
    set_dir(2, 99, 64, 4);         // unknown record type

    const bool rec_le =
        (rec_byte_order == 512) ? !little_endian : little_endian;
    auto write_rec_u16 = [&](size_t off, uint16_t v) {
      std::vector<GByte> tmp;
      AppendU16(tmp, v, rec_le);
      std::memcpy(flir.data() + off, tmp.data(), 2);
    };

    // CameraInfo at 288, len 1126
    const size_t cam = 288;
    set_dir(3, 32, cam, 1126);
    write_u16(cam, rec_byte_order);
    write_f32(cam + 32, 0.95f, rec_le);    // Emissivity
    write_f32(cam + 36, 2.0f, rec_le);     // ObjectDistance
    write_f32(cam + 40, 293.15f, rec_le);  // ReflectedApparentTemperature
    write_f32(cam + 60, humidity, rec_le);
    std::memcpy(flir.data() + cam + 212, "FLIR_A65\0", 9);
    write_rec_u16(cam + 908, static_cast<uint16_t>(tz_minutes));

    // PaletteInfo at 1424, len 118 (2 palette colors * 3 = 6 + 112)
    const size_t pal = 1424;
    set_dir(4, 34, pal, 118);
    flir[pal] = 2;  // 2 colors
    std::memcpy(flir.data() + pal + 80, "Iron\0", 5);
    flir[pal + 112] = 10;
    flir[pal + 113] = 20;
    flir[pal + 114] = 30;
    flir[pal + 115] = 40;
    flir[pal + 116] = 50;
    flir[pal + 117] = 60;

    // GPSInfo at 1552, len 104
    const size_t gps = 1552;
    set_dir(5, 43, gps, 104);
    write_u32(gps, 0x01000000u);  // triggers bLittleEndian = !bLittleEndian
    std::memcpy(flir.data() + gps + 4, "0220", 4);
    flir[gps + 8] = 'N';
    flir[gps + 10] = 'W';
    write_f64(gps + 16, 37.42, !little_endian);
    write_f64(gps + 24, -122.08, !little_endian);

    // RawData at 1664: 32-byte header + pixels or PNG payload
    const size_t raw = 1664;
    if (raw_payload_override.empty()) {
      const size_t raw_rec_len = 32 + 4 * 2 * 2;
      flir.resize(raw + raw_rec_len, 0);
      set_dir(6, 1, raw, static_cast<uint32_t>(raw_rec_len));
      write_u16(raw, rec_byte_order);
      write_rec_u16(raw + 2, 4);  // width
      write_rec_u16(raw + 4, 2);  // height
      for (int i = 0; i < 8; ++i) {
        write_rec_u16(raw + 32 + i * 2, static_cast<uint16_t>(500 + i));
      }
    } else {
      const size_t raw_rec_len = 32 + raw_payload_override.size();
      flir.resize(raw + raw_rec_len, 0);
      set_dir(6, 1, raw, static_cast<uint32_t>(raw_rec_len));
      write_u16(raw, rec_byte_order);
      write_rec_u16(raw + 2, 2);
      write_rec_u16(raw + 4, 2);
      std::memcpy(flir.data() + raw + 32, raw_payload_override.data(),
                  raw_payload_override.size());
    }

    // Split flir buffer across two APP1 "FLIR\0" segments
    const size_t split = flir.size() / 2;
    auto make_flir_seg = [](int idx, std::span<const GByte> part) {
      std::vector<GByte> p = {
          'F', 'L', 'I', 'R', '\0', 0x01, static_cast<GByte>(idx), 0x02};
      p.insert(p.end(), part.begin(), part.end());
      return MakeMarkerSegment(0xE1, p);
    };

    std::vector<GByte> jpg = InjectAfterSoi(
        std::span<const GByte>(reinterpret_cast<const GByte*>(kOneJpeg.data()),
                               kOneJpeg.size()),
        make_flir_seg(1, std::span<const GByte>(flir.data() + split,
                                                flir.size() - split)));
    jpg = InjectAfterSoi(
        jpg, make_flir_seg(0, std::span<const GByte>(flir.data(), split)));
    return jpg;
  };

  // 3a. Big-endian FLIR with nByteOrder=512 (flipped to LE), humidity=65.0
  // (>2), tz=-60, and DJI EXIF (verifying DJI skips when FLIR already loaded).
  {
    std::vector<GByte> flir_be = BuildFlirJpeg(false, 512, -60, 65.0f);
    flir_be = InjectAfterSoi(flir_be, BuildDjiExif());
    const std::string path = std::format("{}/flir_be.jpg", kVsiDir);
    WriteVsiBytes(path.c_str(), flir_be);

    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    CSLConstList subs = ds->GetMetadata("SUBDATASETS");
    ASSERT_NE(nullptr, subs);
    const std::string sub_conn =
        std::format("JPEG:\"{}\":FLIR_RAW_THERMAL_IMAGE", path);
    EXPECT_STREQ(sub_conn.c_str(),
                 ds->GetMetadataItem("SUBDATASET_1_NAME", "SUBDATASETS"));
    EXPECT_STREQ("FLIR_A65", ds->GetMetadataItem("CameraModel", "FLIR"));
    EXPECT_STREQ("Iron", ds->GetMetadataItem("PaletteName", "FLIR"));
    EXPECT_THAT(ds->GetMetadataItem("Palette", "FLIR"),
                HasSubstr("(10 20 30), (40 50 60)"));
    EXPECT_STREQ("N", ds->GetMetadataItem("GPSLatitudeRef", "FLIR"));

    // Open raw 16-bit FLIR subdataset and verify pixel values.
    GDALDatasetUniquePtr raw_ds(GDALDataset::FromHandle(
        GDALOpenEx(sub_conn.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, raw_ds);
    uint16_t px = 0;
    EXPECT_EQ(CE_None,
              raw_ds->GetRasterBand(1)->RasterIO(GF_Read, 1, 0, 1, 1, &px, 1, 1,
                                                 GDT_UInt16, 0, 0, nullptr));
    EXPECT_EQ(501, px);
  }

  // 3b. Little-endian FLIR with positive timezone tz=+120, humidity=0.5 (<=2),
  // and 16-bit PNG raw thermal image!
  {
    GDALDriver* png_drv = GetGDALDriverManager()->GetDriverByName("PNG");
    ASSERT_NE(nullptr, png_drv);
    auto png_mem = std::unique_ptr<GDALDataset>(
        MEMDataset::Create("", 2, 2, 1, GDT_UInt16, nullptr));
    uint16_t png_vals[4] = {1000, 2000, 3000, 4000};
    ASSERT_EQ(CE_None, png_mem->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 2, 2, png_vals, 2, 2, GDT_UInt16, 0,
                           0, nullptr));
    const std::string png_tmp = std::format("{}/flir_inner.png", kVsiDir);
    GDALDatasetUniquePtr created_png(png_drv->CreateCopy(
        png_tmp.c_str(), png_mem.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, created_png);
    created_png.reset();
    const std::vector<GByte> png_bytes = ReadVsiBytes(png_tmp.c_str());

    const std::vector<GByte> flir_png =
        BuildFlirJpeg(true, 2, 120, 0.5f, png_bytes);
    const std::string path = std::format("{}/flir_png.jpg", kVsiDir);
    WriteVsiBytes(path.c_str(), flir_png);

    const std::string sub_conn =
        std::format("JPEG:\"{}\":FLIR_RAW_THERMAL_IMAGE", path);
    GDALDatasetUniquePtr raw_ds(GDALDataset::FromHandle(
        GDALOpenEx(sub_conn.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, raw_ds);
    EXPECT_EQ(2, raw_ds->GetRasterXSize());
    EXPECT_EQ(2, raw_ds->GetRasterYSize());
  }

  // 3c. Corrupt PNG raw thermal image and unrecognized raw thermal format, plus
  // missing raw thermal image error and corrupt FLIR headers.
  {
    WithQuietHandler quiet;
    CPLErrorReset();

    const std::string one_path = std::format("{}/no_thermal.jpg", kVsiDir);
    VsiMemTempWrapper one_file(one_path, std::string(kOneJpeg));
    const std::string no_therm_conn =
        std::format("JPEG:\"{}\":FLIR_RAW_THERMAL_IMAGE", one_path);
    EXPECT_EQ(nullptr, GDALOpenEx(no_therm_conn.c_str(),
                                  GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                                  nullptr, nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot find raw thermal"));

    // Corrupt PNG signature in FLIR RawData
    const GByte bad_png[] = {0x89, 'P', 'N', 'G', 0, 1, 2, 3, 4};
    const std::string bad_png_path =
        std::format("{}/flir_bad_png.jpg", kVsiDir);
    WriteVsiBytes(bad_png_path.c_str(),
                  BuildFlirJpeg(false, 2, 0, 0.5f, bad_png));
    EXPECT_EQ(
        nullptr,
        GDALOpenEx(
            std::format("JPEG:\"{}\":FLIR_RAW_THERMAL_IMAGE", bad_png_path)
                .c_str(),
            GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr, nullptr, nullptr));

    // Unrecognized raw thermal payload
    const GByte unk_raw[] = {'N', 'O', 'T', '_', 'R', 'A', 'W'};
    const std::string unk_raw_path =
        std::format("{}/flir_unk_raw.jpg", kVsiDir);
    WriteVsiBytes(unk_raw_path.c_str(),
                  BuildFlirJpeg(false, 2, 0, 0.5f, unk_raw));
    EXPECT_EQ(
        nullptr,
        GDALOpenEx(
            std::format("JPEG:\"{}\":FLIR_RAW_THERMAL_IMAGE", unk_raw_path)
                .c_str(),
            GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr, nullptr, nullptr));

    // Too-short FLIR segment (nMarkerLength < 8), bad version, bad dir offset
    const GByte short_flir[] = {'F', 'L', 'I', 'R', '\0', 0x01};
    std::vector<GByte> short_jpg = InjectAfterSoi(
        std::span<const GByte>(reinterpret_cast<const GByte*>(kOneJpeg.data()),
                               kOneJpeg.size()),
        MakeMarkerSegment(0xE1, short_flir));
    const std::string short_path = std::format("{}/flir_short.jpg", kVsiDir);
    WriteVsiBytes(short_path.c_str(), short_jpg);
    GDALDatasetUniquePtr ds_short(GDALDataset::FromHandle(
        GDALOpenEx(short_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds_short);
    EXPECT_EQ(nullptr, ds_short->GetMetadata("FLIR"));
  }
}

TEST(JpegDatasetCommonTest, IccProfileMultiChunkRestartAndErrors) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALRegister_JPEG();

  // 1. Multi-chunk ICC profile (> 65519 bytes raw) round-tripped via
  // SOURCE_ICC_PROFILE option and dataset metadata domain COLOR_PROFILE.
  std::vector<GByte> icc_raw(70000);
  for (size_t i = 0; i < icc_raw.size(); ++i) {
    icc_raw[i] = static_cast<GByte>((i * 13 + 7) & 0xFF);
  }
  std::unique_ptr<char, CplFreeDeleter> b64(
      CPLBase64Encode(static_cast<int>(icc_raw.size()), icc_raw.data()));
  ASSERT_NE(nullptr, b64);

  {
    CPLStringList opts;
    opts.SetNameValue("SOURCE_ICC_PROFILE", b64.get());
    std::vector<GByte> jpg = CreateJpegBytes(
        8, 8, 3, std::vector<GByte>(8 * 8 * 3, 200), opts.List());
    // Also inject a standalone RST0 marker (\xFF\xD0) after SOI to exercise the
    // 2-byte standalone marker skip in ReadICCProfile.
    std::vector<GByte> with_rst0;
    with_rst0.insert(with_rst0.end(), jpg.begin(), jpg.begin() + 2);
    with_rst0.push_back(0xFF);
    with_rst0.push_back(0xD0);
    with_rst0.insert(with_rst0.end(), jpg.begin() + 2, jpg.end());

    const std::string path = std::format("{}/icc_multichunk.jpg", kVsiDir);
    WriteVsiBytes(path.c_str(), with_rst0);

    WithQuietHandler quiet;
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_STREQ(b64.get(),
                 ds->GetMetadataItem("SOURCE_ICC_PROFILE", "COLOR_PROFILE"));
    // Second call hits bHasReadICCMetadata cache in ReadICCProfile().
    EXPECT_STREQ(b64.get(),
                 ds->GetMetadataItem("SOURCE_ICC_PROFILE", "COLOR_PROFILE"));
    EXPECT_NE(nullptr, ds->GetMetadata("COLOR_PROFILE"));
  }

  // 2. ICC profile corruption edge cases: mismatched max ID, chunk ID 0,
  // chunk ID > max ID, duplicate chunk ID, missing chunk.
  auto MakeIccPayload = [](GByte chunk_id, GByte max_id,
                           std::span<const GByte> body) {
    std::vector<GByte> p = {'I', 'C', 'C', '_', 'P',  'R',      'O',
                            'F', 'I', 'L', 'E', '\0', chunk_id, max_id};
    p.insert(p.end(), body.begin(), body.end());
    return MakeMarkerSegment(0xE2, p);
  };

  const GByte small_icc[] = {0x01, 0x02, 0x03, 0x04};
  struct BadIccCase {
    std::string_view name;
    std::vector<GByte> markers;
  };
  std::vector<BadIccCase> cases;

  // Mismatched max chunk ID (1/2 followed by 2/3)
  {
    auto m1 = MakeIccPayload(1, 2, small_icc);
    auto m2 = MakeIccPayload(2, 3, small_icc);
    m1.insert(m1.end(), m2.begin(), m2.end());
    cases.push_back({.name = "mismatched_max", .markers = std::move(m1)});
  }
  // Zero chunk ID (0/1)
  cases.push_back(
      {.name = "zero_id", .markers = MakeIccPayload(0, 1, small_icc)});
  // Chunk ID > max ID (2/1)
  cases.push_back(
      {.name = "id_gt_max", .markers = MakeIccPayload(2, 1, small_icc)});
  // Duplicate chunk ID (1/2 and 1/2)
  {
    auto m1 = MakeIccPayload(1, 2, small_icc);
    auto m2 = MakeIccPayload(1, 2, small_icc);
    m1.insert(m1.end(), m2.begin(), m2.end());
    cases.push_back({.name = "duplicate_id", .markers = std::move(m1)});
  }
  // Missing second chunk (only 1/2)
  cases.push_back(
      {.name = "missing_chunk", .markers = MakeIccPayload(1, 2, small_icc)});

  for (const auto& tc : cases) {
    SCOPED_TRACE(tc.name);
    WithQuietHandler quiet;
    CPLErrorReset();
    const std::vector<GByte> jpg = InjectAfterSoi(
        std::span<const GByte>(reinterpret_cast<const GByte*>(kOneJpeg.data()),
                               kOneJpeg.size()),
        tc.markers);
    const std::string path = std::format("{}/icc_{}.jpg", kVsiDir, tc.name);
    WriteVsiBytes(path.c_str(), jpg);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(nullptr, ds->GetMetadata("COLOR_PROFILE"));
  }
}

TEST(JpegDatasetCommonTest, ValidityMaskLsbMsbHeuristicsAndErrors) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALRegister_JPEG();
  GDALDriver* jpg_drv = GetGDALDriverManager()->GetDriverByName("JPEG");
  ASSERT_NE(nullptr, jpg_drv);

  constexpr int kWidth = 16;
  constexpr int kHeight = 2;

  // Create a 1-band MEMDataset with a per-dataset mask band where row 0 and
  // row 1 both transition 1 -> 0 at iX = 3 (mid-byte, non-byte-boundary) to
  // trigger DecompressMask() MSB auto-detection when written with MSB order.
  auto make_masked_src = [&](bool same_rows, int split_x) {
    auto mem = std::unique_ptr<GDALDataset>(
        MEMDataset::Create("", kWidth, kHeight, 1, GDT_Byte, nullptr));
    std::vector<GByte> gray(kWidth * kHeight, 100);
    EXPECT_EQ(CE_None, mem->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, kWidth, kHeight, gray.data(), kWidth,
                           kHeight, GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_None, mem->CreateMaskBand(GMF_PER_DATASET));
    std::vector<GByte> alpha(kWidth * kHeight, 0);
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        if (split_x < 0) {
          // Alternating 255, 0, 255 -> multiple transitions
          alpha[y * kWidth + x] = (x % 2 == 0) ? 255 : 0;
        } else if (y == 0) {
          alpha[x] = (x < split_x) ? 255 : 0;
        } else {
          alpha[kWidth + x] = same_rows ? ((x < split_x) ? 255 : 0)
                                        : ((x < split_x + 2) ? 255 : 0);
        }
      }
    }
    EXPECT_EQ(CE_None, mem->GetRasterBand(1)->GetMaskBand()->RasterIO(
                           GF_Write, 0, 0, kWidth, kHeight, alpha.data(),
                           kWidth, kHeight, GDT_Byte, 0, 0, nullptr));
    return mem;
  };

  // 1. Default LSB mask round-trip via CreateCopy + overview mask check.
  {
    auto mem_src = make_masked_src(true, 3);
    const std::string path = std::format("{}/mask_lsb.jpg", kVsiDir);
    GDALDatasetUniquePtr created(jpg_drv->CreateCopy(
        path.c_str(), mem_src.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, created);
    created.reset();

    // Test JPEG_READ_MASK=NO first.
    {
      CPLConfigOptionSetter no_mask("JPEG_READ_MASK", "NO", false);
      GDALDatasetUniquePtr ds_no(GDALDataset::FromHandle(
          GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                     nullptr, nullptr)));
      ASSERT_NE(nullptr, ds_no);
      EXPECT_EQ(GMF_ALL_VALID, ds_no->GetRasterBand(1)->GetMaskFlags());
    }

    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(GMF_PER_DATASET, ds->GetRasterBand(1)->GetMaskFlags());
    std::vector<GByte> got_mask(kWidth * kHeight);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->GetMaskBand()->RasterIO(
                           GF_Read, 0, 0, kWidth, kHeight, got_mask.data(),
                           kWidth, kHeight, GDT_Byte, 0, 0, nullptr));
    for (int x = 0; x < kWidth; ++x) {
      EXPECT_EQ(x < 3 ? 255 : 0, got_mask[x]);
    }
  }

  // 2. MSB mask auto-detection (JPEG_WRITE_MASK_BIT_ORDER=MSB, split at iX=3,
  // identical rows) + explicit JPEG_MASK_BIT_ORDER=LSB override.
  {
    CPLConfigOptionSetter write_msb("JPEG_WRITE_MASK_BIT_ORDER", "MSB", false);
    auto mem_src = make_masked_src(true, 3);
    const std::string path = std::format("{}/mask_msb.jpg", kVsiDir);
    GDALDatasetUniquePtr created(jpg_drv->CreateCopy(
        path.c_str(), mem_src.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, created);
    created.reset();

    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    std::vector<GByte> got_mask(kWidth * kHeight);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->GetMaskBand()->RasterIO(
                           GF_Read, 0, 0, kWidth, kHeight, got_mask.data(),
                           kWidth, kHeight, GDT_Byte, 0, 0, nullptr));
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        EXPECT_EQ(x < 3 ? 255 : 0, got_mask[y * kWidth + x]);
      }
    }

    // Explicit JPEG_MASK_BIT_ORDER=LSB and =MSB branches.
    for (const char* order : {"LSB", "MSB"}) {
      CPLConfigOptionSetter explicit_order("JPEG_MASK_BIT_ORDER", order, false);
      GDALDatasetUniquePtr ds_ord(GDALDataset::FromHandle(
          GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                     nullptr, nullptr)));
      ASSERT_NE(nullptr, ds_ord);
      EXPECT_EQ(CE_None, ds_ord->GetRasterBand(1)->GetMaskBand()->RasterIO(
                             GF_Read, 0, 0, kWidth, 1, got_mask.data(), kWidth,
                             1, GDT_Byte, 0, 0, nullptr));
    }
  }

  // 3. Heuristic negative exits in DecompressMask():
  //    a) Byte-boundary transition (iX = 8 on width = 16)
  //    b) Multiple transitions on row 0
  //    c) Row 0 != Row 1 transition position
  struct MaskHeuristicCase {
    std::string_view name;
    bool same_rows;
    int split_x;
  };
  constexpr MaskHeuristicCase kHeurCases[] = {
      {.name = "byte_bound", .same_rows = true, .split_x = 8},
      {.name = "multi_trans", .same_rows = true, .split_x = -1},
      {.name = "row_mismatch", .same_rows = false, .split_x = 3},
  };
  for (const auto& tc : kHeurCases) {
    auto mem_src = make_masked_src(tc.same_rows, tc.split_x);
    const std::string path =
        std::format("{}/mask_heur_{}.jpg", kVsiDir, tc.name);
    GDALDatasetUniquePtr created(jpg_drv->CreateCopy(
        path.c_str(), mem_src.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, created);
    created.reset();

    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    std::vector<GByte> row(kWidth);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->GetMaskBand()->RasterIO(
                           GF_Read, 0, 0, kWidth, 1, row.data(), kWidth, 1,
                           GDT_Byte, 0, 0, nullptr));
  }

  // 4. Corrupt zlib compressed mask trailer -> DecompressMask fails cleanly.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    std::vector<GByte> bad_mask(kOneJpeg.begin(), kOneJpeg.end());
    const GByte garbage_zlib[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
    bad_mask.insert(bad_mask.end(), std::begin(garbage_zlib),
                    std::end(garbage_zlib));
    AppendLe32(bad_mask, static_cast<uint32_t>(kOneJpeg.size()));
    const std::string path = std::format("{}/corrupt_mask.jpg", kVsiDir);
    WriteVsiBytes(path.c_str(), bad_mask);

    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* mask_band = ds->GetRasterBand(1)->GetMaskBand();
    ASSERT_NE(nullptr, mask_band);
    GByte px = 0;
    EXPECT_EQ(CE_Failure, mask_band->RasterIO(GF_Read, 0, 0, 1, 1, &px, 1, 1,
                                              GDT_Byte, 0, 0, nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Failure decoding JPEG validity bitmask"));
  }

  // 5. JPGAppendMask progress cancellation and missing file error.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto mem_src = make_masked_src(true, 3);
    auto abort_progress = [](double, const char*, void*) -> int {
      return FALSE;
    };
    const std::string path = std::format("{}/mask_cancel.jpg", kVsiDir);
    WriteVsiBytes(
        path.c_str(),
        std::span<const GByte>(reinterpret_cast<const GByte*>(kOneJpeg.data()),
                               kOneJpeg.size()));
    EXPECT_EQ(
        CE_Failure,
        JPGAppendMask(path.c_str(), mem_src->GetRasterBand(1)->GetMaskBand(),
                      abort_progress, nullptr));
    EXPECT_EQ(CE_Failure,
              JPGAppendMask("/nonexistent_dir/missing.jpg",
                            mem_src->GetRasterBand(1)->GetMaskBand(), nullptr,
                            nullptr));
  }
}

TEST(JpegDatasetCommonTest, OverviewsExifThumbnailExternalAndErrors) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALRegister_JPEG();
  GDALRegister_GTiff();
  GDALDriver* jpg_drv = GetGDALDriverManager()->GetDriverByName("JPEG");
  ASSERT_NE(nullptr, jpg_drv);

  // 1. Large 1024x1024 with EXIF_THUMBNAIL=YES -> implicit overviews (512, 256)
  // + EXIF overview (128x128), plus GetOverview out-of-bounds and
  // SetEnableOverviews(false).
  {
    auto mem_src = std::unique_ptr<GDALDataset>(
        MEMDataset::Create("", 1024, 1024, 3, GDT_Byte, nullptr));
    CPLStringList opts;
    opts.SetNameValue("EXIF_THUMBNAIL", "YES");
    const std::string path = std::format("{}/ovr_1024_exif.jpg", kVsiDir);
    GDALDatasetUniquePtr created(jpg_drv->CreateCopy(
        path.c_str(), mem_src.get(), FALSE, opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, created);
    created.reset();

    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    auto* b1 = ds->GetRasterBand(1);
    const int ovr_count = b1->GetOverviewCount();
    EXPECT_EQ(3, ovr_count);
    EXPECT_EQ(nullptr, b1->GetOverview(-1));
    EXPECT_EQ(nullptr, b1->GetOverview(ovr_count));

    GDALRasterBand* ovr0 = b1->GetOverview(0);
    GDALRasterBand* ovr2 = b1->GetOverview(2);
    ASSERT_NE(nullptr, ovr0);
    ASSERT_NE(nullptr, ovr2);
    EXPECT_EQ(512, ovr0->GetXSize());
    EXPECT_EQ(128, ovr2->GetXSize());
    // Implicit overview has default mask band / mask flags.
    EXPECT_EQ(GMF_ALL_VALID, ovr0->GetMaskFlags());
    EXPECT_NE(nullptr, ovr0->GetMaskBand());

    // Disabling overviews causes GetOverviewCount() to return 0.
    ds->SetEnableOverviews(false);
    EXPECT_EQ(0, b1->GetOverviewCount());
    ds->SetEnableOverviews(true);
    EXPECT_EQ(3, b1->GetOverviewCount());

    // Calling GetMetadata() first sets nTiffDirStart > 0 (with EXIF) and
    // nTiffDirStart == 0 (without EXIF) so subsequent GetOverviewCount()
    // -> InitEXIFOverview() -> EXIFInit() hits both early return branches.
    {
      GDALDatasetUniquePtr ds_exif_first(GDALDataset::FromHandle(
          GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                     nullptr, nullptr)));
      ASSERT_NE(nullptr, ds_exif_first);
      (void)ds_exif_first->GetMetadata();
      EXPECT_EQ(3, ds_exif_first->GetRasterBand(1)->GetOverviewCount());

      const std::string plain1024 = std::format("{}/plain_1024.jpg", kVsiDir);
      GDALDatasetUniquePtr c_plain(jpg_drv->CreateCopy(
          plain1024.c_str(), mem_src.get(), FALSE, nullptr, nullptr, nullptr));
      ASSERT_NE(nullptr, c_plain);
      c_plain.reset();
      GDALDatasetUniquePtr ds_no_exif_first(GDALDataset::FromHandle(
          GDALOpenEx(plain1024.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                     nullptr, nullptr, nullptr)));
      ASSERT_NE(nullptr, ds_no_exif_first);
      (void)ds_no_exif_first->GetMetadata();
      EXPECT_EQ(3, ds_no_exif_first->GetRasterBand(1)->GetOverviewCount());
    }
  }

  // 2. Portrait thumbnail (nYSize > nXSize), clamped THUMBNAIL_WIDTH / HEIGHT
  // (< 32 and > 1024), single-dimension THUMBNAIL_WIDTH / THUMBNAIL_HEIGHT.
  {
    auto mem_port = std::unique_ptr<GDALDataset>(
        MEMDataset::Create("", 600, 800, 1, GDT_Byte, nullptr));
    CPLStringList opts1;
    opts1.SetNameValue("EXIF_THUMBNAIL", "YES");
    const std::string p1 = std::format("{}/ovr_portrait.jpg", kVsiDir);
    GDALDatasetUniquePtr c1(jpg_drv->CreateCopy(
        p1.c_str(), mem_port.get(), FALSE, opts1.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, c1);
    c1.reset();
    GDALDatasetUniquePtr ds1(GDALDataset::FromHandle(
        GDALOpenEx(p1.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    // 600x800 (nYSize >= 512): 2 implicit overviews (300x400, 150x200) +
    // 1 EXIF thumbnail overview (96x128) = 3 overviews.
    ASSERT_NE(nullptr, ds1);
    EXPECT_EQ(3, ds1->GetRasterBand(1)->GetOverviewCount());

    // Explicit THUMBNAIL_WIDTH=400 (>= 600 >> 1 = 300) stops implicit loop on
    // i=0 so only 1 EXIF overview is exposed; also test THUMBNAIL_WIDTH=10
    // (< 32 clamped) and THUMBNAIL_WIDTH=2000 (> 1024 clamped) warnings.
    auto mem_clamped = std::unique_ptr<GDALDataset>(
        MEMDataset::Create("", 600, 600, 1, GDT_Byte, nullptr));
    CPLStringList opts2;
    opts2.SetNameValue("EXIF_THUMBNAIL", "YES");
    opts2.SetNameValue("THUMBNAIL_WIDTH", "400");
    opts2.SetNameValue("THUMBNAIL_HEIGHT", "350");
    const std::string p2 = std::format("{}/ovr_clamped.jpg", kVsiDir);
    GDALDatasetUniquePtr c2(jpg_drv->CreateCopy(
        p2.c_str(), mem_clamped.get(), FALSE, opts2.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, c2);
    c2.reset();
    GDALDatasetUniquePtr ds2(GDALDataset::FromHandle(
        GDALOpenEx(p2.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds2);
    EXPECT_EQ(1, ds2->GetRasterBand(1)->GetOverviewCount());
    EXPECT_EQ(350, ds2->GetRasterBand(1)->GetOverview(0)->GetYSize());

    // Verify out-of-bounds THUMBNAIL_WIDTH (< 32 and > 1024) clamp cleanly.
    {
      WithQuietHandler quiet;
      for (const char* w_val : {"10", "2000"}) {
        CPLStringList clamp_opts;
        clamp_opts.SetNameValue("EXIF_THUMBNAIL", "YES");
        clamp_opts.SetNameValue("THUMBNAIL_WIDTH", w_val);
        const std::string pc =
            std::format("{}/ovr_clamp_{}.jpg", kVsiDir, w_val);
        GDALDatasetUniquePtr cc(
            jpg_drv->CreateCopy(pc.c_str(), mem_clamped.get(), FALSE,
                                clamp_opts.List(), nullptr, nullptr));
        EXPECT_NE(nullptr, cc);
      }
    }
  }

  // 3. USE_INTERNAL_OVERVIEWS=NO open option,
  // JPEG_FORCE_INTERNAL_OVERVIEWS=YES, and _gdal_http_ eager overview
  // initialization.
  {
    auto mem = std::unique_ptr<GDALDataset>(
        MEMDataset::Create("", 600, 600, 1, GDT_Byte, nullptr));
    const std::string p = std::format("{}/ovr_opts.jpg", kVsiDir);
    GDALDatasetUniquePtr c(jpg_drv->CreateCopy(p.c_str(), mem.get(), FALSE,
                                               nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, c);
    c.reset();

    CPLStringList open_no_ovr;
    open_no_ovr.SetNameValue("USE_INTERNAL_OVERVIEWS", "NO");
    GDALDatasetUniquePtr ds_no(GDALDataset::FromHandle(
        GDALOpenEx(p.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   open_no_ovr.List(), nullptr)));
    ASSERT_NE(nullptr, ds_no);
    EXPECT_EQ(0, ds_no->GetRasterBand(1)->GetOverviewCount());
  }

  {
    CPLConfigOptionSetter force_ovr("JPEG_FORCE_INTERNAL_OVERVIEWS", "YES",
                                    false);
    const std::string http_path =
        std::format("{}/_gdal_http_eager.jpg", kVsiDir);
    VsiMemTempWrapper f(http_path, std::string(kOneJpeg));
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(http_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(3, ds->GetRasterBand(1)->GetOverviewCount());
  }

  // 4. External overviews via BuildOverviews() -> delegates to GDALPamDataset.
  {
    const std::string path = std::format("{}/ext_ovr.jpg", kVsiDir);
    WriteVsiBytes(path.c_str(),
                  CreateJpegBytes(32, 32, 1, std::vector<GByte>(32 * 32, 90)));
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    int level = 2;
    ASSERT_EQ(CE_None, ds->BuildOverviews("NEAREST", 1, &level, 0, nullptr,
                                          nullptr, nullptr));
    EXPECT_GE(ds->GetRasterBand(1)->GetOverviewCount(), 1);
    EXPECT_NE(nullptr, ds->GetRasterBand(1)->GetOverview(0));
  }

  // 5. InitEXIFOverview corruption cases on a 600x600 image:
  //    a) IFD0 offset past EOF
  //    b) IFD0 entry count > 125
  //    c) IFD1 offset past EOF
  //    d) IFD1 entry count > 125
  //    e) IFD1 truncated entry
  //    f) IFD1 mismatched band count (1-band thumbnail inside 3-band image)
  const std::vector<GByte> base_600_rgb =
      CreateJpegBytes(600, 600, 3, std::vector<GByte>(600 * 600 * 3, 77));

  auto run_bad_exif_ovr = [&](std::string_view name,
                              const std::vector<GByte>& tiff,
                              bool expect_null_ovr) {
    SCOPED_TRACE(name);
    WithQuietHandler quiet;
    CPLErrorReset();
    std::vector<GByte> payload = {'E', 'x', 'i', 'f', '\0', '\0'};
    payload.insert(payload.end(), tiff.begin(), tiff.end());
    std::vector<GByte> jpg =
        InjectAfterSoi(base_600_rgb, MakeMarkerSegment(0xE1, payload));
    const std::string path =
        std::format("{}/bad_exif_ovr_{}.jpg", kVsiDir, name);
    WriteVsiBytes(path.c_str(), jpg);
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    if (expect_null_ovr) {
      // 600x600 has 2 implicit overviews (300x300, 150x150) and 0 EXIF
      // overviews.
      EXPECT_EQ(2, ds->GetRasterBand(1)->GetOverviewCount());
    }
  };

  // a) IFD0 offset past EOF
  {
    std::vector<GByte> tiff = {'I', 'I', 42, 0};
    AppendLe32(tiff, 9999);
    run_bad_exif_ovr("ifd0_eof", tiff, true);
  }
  // b) IFD0 entry count = 200 (> 125)
  {
    std::vector<GByte> tiff = {'I', 'I', 42, 0, 8, 0, 0, 0};
    AppendLe16(tiff, 200);
    run_bad_exif_ovr("ifd0_count_200", tiff, true);
  }
  // c) IFD1 offset past EOF
  {
    std::vector<GByte> tiff = {'I', 'I', 42, 0, 8, 0, 0, 0};
    AppendLe16(tiff, 0);
    AppendLe32(tiff, 9999);
    run_bad_exif_ovr("ifd1_eof", tiff, true);
  }
  // d) IFD1 entry count = 200 (> 125)
  {
    std::vector<GByte> tiff = {'I', 'I', 42, 0, 8, 0, 0, 0};
    AppendLe16(tiff, 0);
    AppendLe32(tiff, 14);
    AppendLe16(tiff, 200);
    run_bad_exif_ovr("ifd1_count_200", tiff, true);
  }
  // e) IFD1 entry count = 2, truncated after 1 entry
  {
    std::vector<GByte> tiff = {'I', 'I', 42, 0, 8, 0, 0, 0};
    AppendLe16(tiff, 0);
    AppendLe32(tiff, 14);
    AppendLe16(tiff, 2);
    tiff.insert(tiff.end(), 12, 0);
    run_bad_exif_ovr("ifd1_trunc_entry", tiff, true);
  }
  // f) IFD1 valid 1-band grayscale thumbnail inside 3-band 600x600 image ->
  // band count mismatch closes EXIF overview!
  {
    std::vector<GByte> tiff = {'I', 'I', 42, 0, 8, 0, 0, 0};
    AppendLe16(tiff, 0);
    AppendLe32(tiff, 14);
    const uint32_t thumb_off = 14 + 2 + 5 * 12 + 4;  // = 80
    AppendLe16(tiff, 5);
    auto add_short = [&](uint16_t tag, uint32_t val) {
      AppendLe16(tiff, tag);
      AppendLe16(tiff, TIFF_LONG);
      AppendLe32(tiff, 1);
      AppendLe32(tiff, val);
    };
    add_short(0x100, 1);
    add_short(0x101, 1);
    add_short(0x103, 6);
    add_short(0x201, thumb_off);
    add_short(0x202, static_cast<uint32_t>(kOneJpeg.size()));
    AppendLe32(tiff, 0);
    tiff.insert(tiff.end(), kOneJpeg.begin(), kOneJpeg.end());
    run_bad_exif_ovr("ifd1_band_mismatch", tiff, true);
  }
}

TEST(JpegDatasetCommonTest, CompressionFormatsReadCompressedDataAndCopy) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALRegister_JPEG();
  GDALDriver* jpg_drv = GetGDALDriverManager()->GetDriverByName("JPEG");
  ASSERT_NE(nullptr, jpg_drv);

  const std::string src_path = std::format("{}/comp_src.jpg", kVsiDir);

  // Build rich JPEG with valid EXIF tag (EXIF_Make = "Cam"), XMP APP1, and
  // compressed mask trailer so lossless CreateCopy writes EXIF and XMP.
  const std::vector<GByte> exif_payload = {
      'E',  'x',  'i', 'f', 0x00, 0x00, 'I', 'I', 42,
      0,    8,    0,   0,   0,    1,    0,  // 1 IFD0 entry at TIFF offset 8
      0x0F, 0x01, 2,   0,   4,    0,    0,   0,   'C',
      'a',  'm',  0,   0,   0,    0,    0};
  const char xmp_str[] = "http://ns.adobe.com/xap/1.0/\0<x:xmpmeta/>";
  const std::vector<GByte> xmp_payload(xmp_str, xmp_str + sizeof(xmp_str));
  // Re-append mask after injecting APP1s so mask trailer offset remains exact.
  const std::vector<GByte> mask_bits(16 * 2, 0xFF);
  std::vector<GByte> bare_jpg =
      CreateJpegBytes(16, 16, 3, std::vector<GByte>(16 * 16 * 3, 120));
  bare_jpg = InjectAfterSoi(bare_jpg, MakeMarkerSegment(0xE1, exif_payload));
  bare_jpg = InjectAfterSoi(bare_jpg, MakeMarkerSegment(0xE1, xmp_payload));
  const std::vector<GByte> rich_jpg =
      AppendCompressedMaskTrailer(bare_jpg, mask_bits);
  WriteVsiBytes(src_path.c_str(), rich_jpg);

  GDALDatasetUniquePtr src_ds(GDALDataset::FromHandle(
      GDALOpenEx(src_path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                 nullptr, nullptr)));
  ASSERT_NE(nullptr, src_ds);

  // 1. GetCompressionFormats & ReadCompressedData validation.
  const int bands[3] = {1, 2, 3};
  EXPECT_TRUE(src_ds->GetCompressionFormats(0, 0, 8, 8, 3, bands).empty());
  const CPLStringList fmts =
      src_ds->GetCompressionFormats(0, 0, 16, 16, 3, bands);
  ASSERT_EQ(1, fmts.size());
  EXPECT_THAT(fmts[0], HasSubstr("JPEG"));

  // Error branches in ReadCompressedData:
  size_t sz = 0;
  void* null_buf = nullptr;
  EXPECT_EQ(CE_Failure, src_ds->ReadCompressedData("JPEG", 0, 0, 8, 8, 3, bands,
                                                   nullptr, &sz, nullptr));
  EXPECT_EQ(CE_Failure,
            src_ds->ReadCompressedData("JPEG;extra", 0, 0, 16, 16, 3, bands,
                                       nullptr, &sz, nullptr));
  EXPECT_EQ(CE_Failure,
            src_ds->ReadCompressedData("PNG", 0, 0, 16, 16, 3, bands, nullptr,
                                       &sz, nullptr));
  EXPECT_EQ(CE_Failure,
            src_ds->ReadCompressedData("JPEG", 0, 0, 16, 16, 3, bands,
                                       &null_buf, nullptr, nullptr));

  // Query size only + detailed format:
  char* detailed_raw = nullptr;
  ASSERT_EQ(CE_None, src_ds->ReadCompressedData("JPEG", 0, 0, 16, 16, 3, bands,
                                                nullptr, &sz, &detailed_raw));
  std::unique_ptr<char, decltype(&VSIFree)> detailed(detailed_raw, VSIFree);
  EXPECT_EQ(bare_jpg.size(), sz);
  ASSERT_NE(nullptr, detailed);
  EXPECT_THAT(detailed.get(), HasSubstr("JPEG"));

  // Too-small caller buffer fails; sufficiently sized caller buffer succeeds.
  std::vector<GByte> caller_buf(sz);
  void* caller_ptr = caller_buf.data();
  size_t small_sz = 4;
  EXPECT_EQ(CE_Failure,
            src_ds->ReadCompressedData("JPEG", 0, 0, 16, 16, 3, bands,
                                       &caller_ptr, &small_sz, nullptr));
  size_t caller_sz = caller_buf.size();
  ASSERT_EQ(CE_None,
            src_ds->ReadCompressedData("JPEG", 0, 0, 16, 16, 3, bands,
                                       &caller_ptr, &caller_sz, nullptr));
  EXPECT_LT(caller_sz, sz);  // Both EXIF and XMP APP1 markers were stripped!

  // 2. Lossless CreateCopy with EXIF, XMP, @CREATE_ONLY_VISIBLE_AT_CLOSE_TIME,
  // WORLDFILE=YES, GDAL_OPEN_AFTER_COPY=NO, and 3-band VRTDataset fallback
  // (poSrcDS->GetMetadata("EXIF") == nullptr -> GetMetadata()).
  {
    CPLStringList lossless_opts;
    lossless_opts.SetNameValue("LOSSLESS_COPY", "YES");
    lossless_opts.SetNameValue("@CREATE_ONLY_VISIBLE_AT_CLOSE_TIME", "YES");
    lossless_opts.SetNameValue("WORLDFILE", "YES");
    const std::string copy_path = std::format("{}/lossless_out.jpg", kVsiDir);
    GDALDatasetUniquePtr copied(
        jpg_drv->CreateCopy(copy_path.c_str(), src_ds.get(), FALSE,
                            lossless_opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, copied);
    EXPECT_STREQ("Cam", copied->GetMetadataItem("EXIF_Make"));
    EXPECT_NE(nullptr, copied->GetMetadata("xml:XMP"));
    copied.reset();

    {
      GDALRegister_VRT();
      GDALDriver* vrt_drv = GetGDALDriverManager()->GetDriverByName("VRT");
      ASSERT_NE(nullptr, vrt_drv);
      GDALDatasetUniquePtr vrt_ds(vrt_drv->CreateCopy(
          "", src_ds.get(), FALSE, nullptr, nullptr, nullptr));
      ASSERT_NE(nullptr, vrt_ds);
      vrt_ds->SetMetadata(nullptr, "EXIF");

      CPLConfigOptionSetter no_reopen("GDAL_OPEN_AFTER_COPY", "NO", false);
      GDALDatasetUniquePtr stub(
          jpg_drv->CreateCopy(copy_path.c_str(), vrt_ds.get(), FALSE,
                              lossless_opts.List(), nullptr, nullptr));
      ASSERT_NE(nullptr, stub);
      EXPECT_EQ(16, stub->GetRasterXSize());
    }
    // TODO: b/335317901 - Add lossless CreateCopy progress callback abort
    // check once upstream frees pJPEGContent before early return at
    // jpgdataset.cpp:4330-4331.
  }

  // 3. CreateCopy error and option branches:
  //    a) LOSSLESS_COPY=YES on non-JPEG MEMDataset
  //    b) Unsupported 2-band and 5-band counts
  //    c) 1-band with ColorTable (strict=TRUE fails, strict=FALSE warns)
  //    d) 4-band without GCI_CyanBand warns
  //    e) UInt16 data type (strict=TRUE fails, strict=FALSE warns)
  //    f) Out-of-range QUALITY (0 and 101)
  //    g) Invalid output path
  //    h) ARITHMETIC=YES, WORLDFILE=YES, custom non-EXIF metadata,
  //       GDAL_OPEN_AFTER_COPY=NO, and mid-loop progress abort.
  {
    WithQuietHandler quiet;

    // 3a. LOSSLESS_COPY=YES impossible
    auto mem1 = std::unique_ptr<GDALDataset>(
        MEMDataset::Create("", 8, 8, 1, GDT_Byte, nullptr));
    CPLStringList force_ll;
    force_ll.SetNameValue("LOSSLESS_COPY", "YES");
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              jpg_drv->CreateCopy(std::format("{}/fail.jpg", kVsiDir).c_str(),
                                  mem1.get(), FALSE, force_ll.List(), nullptr,
                                  nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("LOSSLESS_COPY=YES requested"));

    // 3b. 2-band rejected
    auto mem2 = std::unique_ptr<GDALDataset>(
        MEMDataset::Create("", 8, 8, 2, GDT_Byte, nullptr));
    CPLErrorReset();
    EXPECT_EQ(nullptr, jpg_drv->CreateCopy(
                           std::format("{}/fail.jpg", kVsiDir).c_str(),
                           mem2.get(), FALSE, nullptr, nullptr, nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("doesn't support 2 bands"));

    // 3c. ColorTable strict vs non-strict
    GDALColorTable ct;
    GDALColorEntry c0 = {10, 20, 30, 255};
    ct.SetColorEntry(0, &c0);
    mem1->GetRasterBand(1)->SetColorTable(&ct);
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              jpg_drv->CreateCopy(std::format("{}/fail.jpg", kVsiDir).c_str(),
                                  mem1.get(), TRUE, nullptr, nullptr, nullptr));
    GDALDatasetUniquePtr ct_ok(
        jpg_drv->CreateCopy(std::format("{}/ct_ok.jpg", kVsiDir).c_str(),
                            mem1.get(), FALSE, nullptr, nullptr, nullptr));
    EXPECT_NE(nullptr, ct_ok);
    ct_ok.reset();

    // 3d. 4-band without CyanBand warns
    auto mem4 = std::unique_ptr<GDALDataset>(
        MEMDataset::Create("", 8, 8, 4, GDT_Byte, nullptr));
    GDALDatasetUniquePtr b4_ok(
        jpg_drv->CreateCopy(std::format("{}/b4_ok.jpg", kVsiDir).c_str(),
                            mem4.get(), FALSE, nullptr, nullptr, nullptr));
    EXPECT_NE(nullptr, b4_ok);
    b4_ok.reset();

    // 3e. UInt16 strict=TRUE vs strict=FALSE (including value > 4095 clipping).
    auto mem16 = std::unique_ptr<GDALDataset>(
        MEMDataset::Create("", 8, 8, 1, GDT_UInt16, nullptr));
    std::vector<uint16_t> u16_pixels(64, 5000);
    EXPECT_EQ(CE_None, mem16->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 8, 8, u16_pixels.data(), 8, 8,
                           GDT_UInt16, 0, 0, nullptr));
    EXPECT_EQ(nullptr, jpg_drv->CreateCopy(
                           std::format("{}/fail.jpg", kVsiDir).c_str(),
                           mem16.get(), TRUE, nullptr, nullptr, nullptr));
    GDALDatasetUniquePtr u16_ok(
        jpg_drv->CreateCopy(std::format("{}/u16_ok.jpg", kVsiDir).c_str(),
                            mem16.get(), FALSE, nullptr, nullptr, nullptr));
    EXPECT_NE(nullptr, u16_ok);
    u16_ok.reset();

    // 3f. Out-of-range QUALITY and invalid output path
    CPLStringList bad_q0;
    bad_q0.SetNameValue("QUALITY", "0");
    EXPECT_EQ(nullptr,
              jpg_drv->CreateCopy(std::format("{}/fail.jpg", kVsiDir).c_str(),
                                  mem16.get(), FALSE, bad_q0.List(), nullptr,
                                  nullptr));
    CPLStringList bad_q101;
    bad_q101.SetNameValue("QUALITY", "101");
    EXPECT_EQ(nullptr,
              jpg_drv->CreateCopy(std::format("{}/fail.jpg", kVsiDir).c_str(),
                                  mem16.get(), FALSE, bad_q101.List(), nullptr,
                                  nullptr));
    EXPECT_EQ(nullptr,
              jpg_drv->CreateCopy("/nonexistent_dir_xyz/out.jpg", mem4.get(),
                                  FALSE, nullptr, nullptr, nullptr));

    // 3g. ARITHMETIC=YES, WORLDFILE=YES, custom non-EXIF metadata,
    // GDAL_OPEN_AFTER_COPY=NO, and progress callbacks.
    auto mem3 = std::unique_ptr<GDALDataset>(
        MEMDataset::Create("", 8, 8, 3, GDT_Byte, nullptr));
    mem3->SetMetadataItem("CUSTOM_TAG", "VALUE_123");
    CPLStringList arith_opts;
    arith_opts.SetNameValue("ARITHMETIC", "YES");
    arith_opts.SetNameValue("WORLDFILE", "YES");
    const std::string arith_path = std::format("{}/arith.jpg", kVsiDir);
    GDALDatasetUniquePtr arith_ds(
        jpg_drv->CreateCopy(arith_path.c_str(), mem3.get(), FALSE,
                            arith_opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, arith_ds);
    arith_ds.reset();

    {
      CPLConfigOptionSetter no_open("GDAL_OPEN_AFTER_COPY", "NO", false);
      GDALDatasetUniquePtr stub_ds(
          jpg_drv->CreateCopy(arith_path.c_str(), mem3.get(), FALSE,
                              arith_opts.List(), nullptr, nullptr));
      ASSERT_NE(nullptr, stub_ds);
    }

    // Abort at 0.0 and mid-loop (> 0.0) in CreateCopyStage2.
    auto abort_0 = [](double, const char*, void*) -> int { return FALSE; };
    EXPECT_EQ(nullptr, jpg_drv->CreateCopy(arith_path.c_str(), mem3.get(),
                                           FALSE, nullptr, abort_0, nullptr));
    auto abort_mid = [](double dfComplete, const char*, void*) -> int {
      return dfComplete < 0.2 ? TRUE : FALSE;
    };
    EXPECT_EQ(nullptr, jpg_drv->CreateCopy(arith_path.c_str(), mem3.get(),
                                           FALSE, nullptr, abort_mid, nullptr));
  }
}

TEST(JpegDatasetCommonTest, ApplyOrientationAndSubfileOpen) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kVsiDir); };
  GDALRegister_JPEG();
  GDALDriver* jpg_drv = GetGDALDriverManager()->GetDriverByName("JPEG");
  ASSERT_NE(nullptr, jpg_drv);

  // 16x8 image with EXIF_Orientation = "6" (90 deg CW -> 8x16 when
  // APPLY_ORIENTATION=YES)
  auto mem = std::unique_ptr<GDALDataset>(
      MEMDataset::Create("", 16, 8, 1, GDT_Byte, nullptr));
  mem->SetMetadataItem("EXIF_Orientation", "6");
  const std::string path = std::format("{}/oriented.jpg", kVsiDir);
  GDALDatasetUniquePtr created(jpg_drv->CreateCopy(
      path.c_str(), mem.get(), FALSE, nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, created);
  created.reset();

  // Default (APPLY_ORIENTATION=NO): 16x8
  {
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(16, ds->GetRasterXSize());
    EXPECT_EQ(8, ds->GetRasterYSize());
  }

  // APPLY_ORIENTATION=YES: rotated to 8x16
  {
    CPLStringList open_opts;
    open_opts.SetNameValue("APPLY_ORIENTATION", "YES");
    GDALDatasetUniquePtr ds(GDALDataset::FromHandle(
        GDALOpenEx(path.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                   open_opts.List(), nullptr)));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(8, ds->GetRasterXSize());
    EXPECT_EQ(16, ds->GetRasterYSize());
  }
}

// TODO: b/335317901 - Cover 12-bit dual-mode paths if LIBJPEG_DUAL_MODE_8_12 is
// ever enabled in //third_party/gdal/BUILD.

}  // namespace
}  // namespace autotest2
