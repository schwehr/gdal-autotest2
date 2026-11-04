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
// Tests Kakadu JPEG 2000 raster driver (jp2kakdataset.cpp).
//
// Build and upstream caveats:
//   - USE_JPIP and JP2KAK_THREADS (>0) are compiled out in google3 builds.
//   - Requires C++ exceptions (-fexceptions) because Kakadu core/format APIs
//     throw kdu_exception / JP2KAKException on error.
//   - Upstream GDAL catch(...) blocks at jp2kakdataset.cpp:721 (malformed
//     J2K_SUBFILE:) and jp2kakdataset.cpp:2534 (invalid Kakadu creation
//     parameter) return without freeing poRawInput / oCodeStream, so those
//     error-path checks are scoped with absl::LeakCheckDisabler.
//
// See also:
//   https://gdal.org/drivers/raster/jp2kak.html
//   https://github.com/OSGeo/gdal/blob/master/frmts/jp2kak/jp2kakdataset.cpp
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/jp2kak.py

#include "third_party/gdal/frmts/jp2kak/jp2kakdataset.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/base/config.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/debugging/leak_check.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/matchers.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/frmts/jp2kak/vsil_target.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

std::string BytesToString(std::span<const uint8_t> bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::string ReadVsiFileBytes(const char* path) {
  vsi_l_offset size = 0;
  const GByte* data =
      VSIGetMemFileBuffer(path, &size, /*bUnlinkAndSeize=*/FALSE);
  if (data == nullptr) {
    return "";
  }
  return std::string(reinterpret_cast<const char*>(data),
                     static_cast<size_t>(size));
}

template <typename SizFn, typename ConfigFn>
void WriteCustomCodestream(const char* path, bool is_jp2, int num_components,
                           SizFn&& setup_siz, ConfigFn&& configure_cs,
                           std::span<kdu_byte* const> plane_ptrs,
                           std::span<const int> stripe_heights,
                           std::span<const int> precisions) {
  siz_params siz;
  siz.set(Scomponents, 0, 0, num_components);
  for (int c = 0; c < num_components; ++c) {
    siz.set(Sprecision, c, 0, precisions[c]);
    siz.set(Ssigned, c, 0, false);
  }
  setup_siz(siz);
  kdu_params* siz_ref = &siz;
  siz_ref->finalize();

  vsil_target vsil_tgt;
  vsil_tgt.open(path, "w");
  jp2_family_tgt fam;
  jp2_target jp2_tgt;
  kdu_codestream cs;
  absl::Cleanup destroy_cs = [&] {
    if (cs.exists()) {
      cs.destroy();
    }
  };
  if (is_jp2) {
    fam.open(&vsil_tgt);
    jp2_tgt.open(&fam);
    cs.create(&siz, &jp2_tgt);
  } else {
    cs.create(&siz, &vsil_tgt);
  }

  cs.access_siz()->parse_string("Creversible=yes");
  configure_cs(siz, jp2_tgt, cs);
  cs.access_siz()->finalize_all();

  if (is_jp2) {
    jp2_tgt.write_header();
    jp2_tgt.open_codestream();
  }

  kdu_stripe_compressor comp;
  comp.start(cs);
  comp.push_stripe(const_cast<kdu_byte**>(plane_ptrs.data()),
                   stripe_heights.data(), nullptr, nullptr, precisions.data());
  comp.finish();

  kdu_long layer_bytes = 0;
  cs.flush(&layer_bytes, 1);
  std::move(destroy_cs).Invoke();
  if (is_jp2) {
    jp2_tgt.close();
    fam.close();
  }
  vsil_tgt.close();
}

template <typename ConfigFn>
void WriteUniformJp2(const char* path, int width, int height,
                     std::span<const std::vector<kdu_byte>> planes,
                     std::span<const int> precisions, ConfigFn&& configure_cs) {
  const int num_components = static_cast<int>(planes.size());
  std::vector<kdu_byte*> ptrs(num_components);
  for (int c = 0; c < num_components; ++c) {
    ptrs[c] = const_cast<kdu_byte*>(planes[c].data());
  }
  std::vector<int> heights(num_components, height);
  WriteCustomCodestream(
      path, /*is_jp2=*/true, num_components,
      [width, height](siz_params& siz) {
        siz.set(Sdims, 0, 0, height);
        siz.set(Sdims, 0, 1, width);
      },
      [&](siz_params& siz, jp2_target& jp2_tgt, kdu_codestream& /*cs*/) {
        jp2_tgt.access_dimensions().init(&siz);
        configure_cs(jp2_tgt);
      },
      ptrs, heights, precisions);
}

// Synthetic raster band whose first IRasterIO call fails with CE_Failure and
// subsequent calls succeed (pinning JP2KAKCreateCopy_WriteTile's error check
// without triggering upstream's infinite while(true) loop on persistent error).
class FailingReadBand final : public GDALRasterBand {
 public:
  FailingReadBand(int band_num, int x_size, int y_size,
                  GDALDataType data_type) {
    nBand = band_num;
    nRasterXSize = x_size;
    nRasterYSize = y_size;
    eDataType = data_type;
    nBlockXSize = x_size;
    nBlockYSize = 1;
  }

  CPLErr IReadBlock(int /*nBlockXOff*/, int /*nBlockYOff*/,
                    void* /*pImage*/) override {
    CPLError(CE_Failure, CPLE_FileIO, "Simulated source read error");
    return CE_Failure;
  }

 protected:
  CPLErr IRasterIO(GDALRWFlag /*eRWFlag*/, int /*nXOff*/, int /*nYOff*/,
                   int /*nXSize*/, int /*nYSize*/, void* /*pData*/,
                   int /*nBufXSize*/, int /*nBufYSize*/,
                   GDALDataType /*eBufType*/, GSpacing /*nPixelSpace*/,
                   GSpacing /*nLineSpace*/,
                   GDALRasterIOExtraArg* /*psExtraArg*/) override {
    if (fail_calls_++ == 0) {
      CPLError(CE_Failure, CPLE_FileIO, "Simulated source band RasterIO error");
      return CE_Failure;
    }
    return CE_None;
  }

 private:
  int fail_calls_ = 0;
};

class FailingReadDataset final : public GDALDataset {
 public:
  FailingReadDataset(int x_size, int y_size, int bands,
                     GDALDataType data_type) {
    nRasterXSize = x_size;
    nRasterYSize = y_size;
    for (int i = 1; i <= bands; ++i) {
      SetBand(i, new FailingReadBand(i, x_size, y_size, data_type));
    }
  }

 protected:
  CPLErr IRasterIO(GDALRWFlag /*eRWFlag*/, int /*nXOff*/, int /*nYOff*/,
                   int /*nXSize*/, int /*nYSize*/, void* /*pData*/,
                   int /*nBufXSize*/, int /*nBufYSize*/,
                   GDALDataType /*eBufType*/, int /*nBandCount*/,
                   BANDMAP_TYPE /*panBandMap*/, GSpacing /*nPixelSpace*/,
                   GSpacing /*nLineSpace*/, GSpacing /*nBandSpace*/,
                   GDALRasterIOExtraArg* /*psExtraArg*/) override {
    CPLError(CE_Failure, CPLE_FileIO,
             "Simulated source dataset RasterIO error");
    return CE_Failure;
  }
};

struct ProgressCancelContext {
  int call_count = 0;
  int cancel_at_call = -1;
};

int CPL_STDCALL ProgressCallbackWithCancel(double /*dfComplete*/,
                                           const char* /*pszMessage*/,
                                           void* pProgressArg) {
  auto* ctx = static_cast<ProgressCancelContext*>(pProgressArg);
  ctx->call_count++;
  if (ctx->cancel_at_call > 0 && ctx->call_count >= ctx->cancel_at_call) {
    return FALSE;
  }
  return TRUE;
}

class Jp2kakdatasetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_JP2KAK();
    GDALRegister_MEM();
    GDALRegister_GTiff();
    RegisterOGRKML();
    RegisterOGRGML();
    CPLErrorReset();
  }

  void TearDown() override { CPLErrorReset(); }

  GDALDatasetUniquePtr CreateMem(int x_size, int y_size, int bands,
                                 GDALDataType data_type) {
    GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
    if (mem_driver == nullptr) {
      return nullptr;
    }
    return GDALDatasetUniquePtr(
        mem_driver->Create("", x_size, y_size, bands, data_type, nullptr));
  }

  GDALDatasetUniquePtr CreateJp2Copy(const char* path, GDALDataset* src_ds,
                                     CSLConstList options = nullptr,
                                     bool strict = false,
                                     GDALProgressFunc progress = nullptr,
                                     void* progress_data = nullptr) {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("JP2KAK");
    if (drv == nullptr) {
      return nullptr;
    }
    return GDALDatasetUniquePtr(drv->CreateCopy(
        path, src_ds, strict ? TRUE : FALSE, options, progress, progress_data));
  }

  GDALDatasetUniquePtr OpenDirect(const char* path,
                                  unsigned int open_flags = GDAL_OF_READONLY |
                                                            GDAL_OF_RASTER,
                                  CSLConstList open_options = nullptr) {
    GDALOpenInfo open_info(path, open_flags);
    open_info.papszOpenOptions = const_cast<char**>(open_options);
    return GDALDatasetUniquePtr(JP2KAKDataset::Open(&open_info));
  }
};

TEST_F(Jp2kakdatasetTest, RegisterDriverIsIdempotent) {
  // Calling GDALRegister_JP2KAK again when already registered returns early.
  GDALRegister_JP2KAK();
  EXPECT_NE(nullptr, GetGDALDriverManager()->GetDriverByName("JP2KAK"));
}

TEST_F(Jp2kakdatasetTest, OpenRejectsInvalidAndUnsupportedInputs) {
  // 1. Non-existent file fails Identify() inside Open().
  EXPECT_EQ(nullptr, OpenDirect("/vsimem/jp2kakdataset_test/missing.jp2"));

  // 2. JPIP / HTTP URLs fail Identify() inside Open() because USE_JPIP is off.
  for (const char* url : {"http://localhost/e.jp2", "https://localhost/e.jp2",
                          "jpip://localhost/e.jp2"}) {
    EXPECT_EQ(nullptr, OpenDirect(url)) << url;
  }

  // 3. 12-byte file matches jp2_header in Identify() (< 16 bytes) and hits
  //    Open()'s < 16-byte rejection branch.
  constexpr std::array<uint8_t, 12> kOnlySignature = {
      0x00, 0x00, 0x00, 0x0c, 0x6a, 0x50, 0x20, 0x20, 0x0d, 0x0a, 0x87, 0x0a};
  constexpr char kTooShortJp2[] = "/vsimem/jp2kakdataset_test/too_short.jp2";
  VsiMemTempWrapper short_wrapper(kTooShortJp2, BytesToString(kOnlySignature));
  EXPECT_EQ(nullptr, OpenDirect(kTooShortJp2));

  // 4. >= 16-byte corrupt JP2 fails jp2_src->open() / read_header().
  constexpr std::array<uint8_t, 24> kCorruptJp2 = {
      0x00, 0x00, 0x00, 0x0c, 0x6a, 0x50, 0x20, 0x20, 0x0d, 0x0a, 0x87, 0x0a,
      0x00, 0x00, 0x00, 0x0c, 0x66, 0x74, 0x79, 0x70, 0x00, 0x00, 0x00, 0x00};
  constexpr char kCorruptJp2Path[] = "/vsimem/jp2kakdataset_test/corrupt.jp2";
  VsiMemTempWrapper corrupt_jp2_wrapper(kCorruptJp2Path,
                                        BytesToString(kCorruptJp2));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenDirect(kCorruptJp2Path));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 5. >= 16-byte corrupt J2K codestream throws inside oCodeStream.create().
  constexpr std::array<uint8_t, 20> kCorruptJ2k = {
      0xff, 0x4f, 0xff, 0x51, 0x00, 0x04, 0xff, 0xff, 0xff, 0xff,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  constexpr char kCorruptJ2kPath[] = "/vsimem/jp2kakdataset_test/corrupt.j2k";
  VsiMemTempWrapper corrupt_j2k_wrapper(kCorruptJ2kPath,
                                        BytesToString(kCorruptJ2k));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenDirect(kCorruptJ2kPath));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 6. Malformed J2K_SUBFILE: specifications fail in subfile_source::open().
  for (const char* bad_subfile :
       {"J2K_SUBFILE:only_one_token", "J2K_SUBFILE:0,16",
        "J2K_SUBFILE:0,16,/vsimem/jp2kakdataset_test/no.bin"}) {
    WithQuietHandler quiet;
    CPLErrorReset();
    absl::LeakCheckDisabler leak_disabler;
    EXPECT_EQ(nullptr, OpenDirect(bad_subfile)) << bad_subfile;
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 7. Opening a valid JP2 with GA_Update is rejected with CPLE_NotSupported.
  auto mem_ds = CreateMem(8, 8, 1, GDT_Byte);
  ASSERT_NE(nullptr, mem_ds);
  constexpr char kValidPath[] = "/vsimem/jp2kakdataset_test/update_reject.jp2";
  absl::Cleanup cleanup_valid = [&] { VSIUnlink(kValidPath); };
  ASSERT_NE(nullptr, CreateJp2Copy(kValidPath, mem_ds.get()));

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenDirect(kValidPath, GDAL_OF_UPDATE | GDAL_OF_RASTER));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("does not support update access"));
  }
}

TEST_F(Jp2kakdatasetTest, DataTypesAndNbitsRoundTrip) {
  constexpr int kWidth = 16;
  constexpr int kHeight = 16;

  // 1. GDT_Byte lossless (8-bit).
  {
    auto src = CreateMem(kWidth, kHeight, 1, GDT_Byte);
    ASSERT_NE(nullptr, src);
    std::vector<uint8_t> pixels(kWidth * kHeight);
    for (int i = 0; i < kWidth * kHeight; ++i) {
      pixels[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFF);
    }
    ASSERT_EQ(CE_None, src->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, kWidth, kHeight, pixels.data(),
                           kWidth, kHeight, GDT_Byte, 0, 0));

    constexpr char kPath[] = "/vsimem/jp2kakdataset_test/byte_lossless.jp2";
    absl::Cleanup cleanup = [&] { VSIUnlink(kPath); };
    CPLStringList opts;
    opts.SetNameValue("QUALITY", "100");
    auto created = CreateJp2Copy(kPath, src.get(), opts.List());
    ASSERT_NE(nullptr, created);
    created.reset();

    auto ds = OpenDirect(kPath);
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    EXPECT_EQ(GDT_Byte, band->GetRasterDataType());
    EXPECT_EQ(nullptr, band->GetMetadataItem("NBITS", "IMAGE_STRUCTURE"));
    EXPECT_STREQ("JP2000",
                 band->GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"));

    std::vector<uint8_t> actual(kWidth * kHeight, 0xAA);
    ASSERT_EQ(CE_None,
              band->RasterIO(GF_Read, 0, 0, kWidth, kHeight, actual.data(),
                             kWidth, kHeight, GDT_Byte, 0, 0));
    EXPECT_EQ(pixels, actual);
  }

  // 2. GDT_Byte with NBITS=4 lossless.
  {
    auto src = CreateMem(kWidth, kHeight, 1, GDT_Byte);
    ASSERT_NE(nullptr, src);
    std::vector<uint8_t> pixels(kWidth * kHeight);
    for (int i = 0; i < kWidth * kHeight; ++i) {
      pixels[i] = static_cast<uint8_t>(i & 0x0F);
    }
    ASSERT_EQ(CE_None, src->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, kWidth, kHeight, pixels.data(),
                           kWidth, kHeight, GDT_Byte, 0, 0));

    constexpr char kPath[] = "/vsimem/jp2kakdataset_test/byte_nbits4.jp2";
    absl::Cleanup cleanup = [&] { VSIUnlink(kPath); };
    CPLStringList opts;
    opts.SetNameValue("QUALITY", "100");
    opts.SetNameValue("NBITS", "4");
    auto created = CreateJp2Copy(kPath, src.get(), opts.List());
    ASSERT_NE(nullptr, created);
    created.reset();

    auto ds = OpenDirect(kPath);
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    EXPECT_EQ(GDT_Byte, band->GetRasterDataType());
    EXPECT_STREQ("4", band->GetMetadataItem("NBITS", "IMAGE_STRUCTURE"));
    std::vector<uint8_t> actual(kWidth * kHeight, 0xAA);
    ASSERT_EQ(CE_None,
              band->RasterIO(GF_Read, 0, 0, kWidth, kHeight, actual.data(),
                             kWidth, kHeight, GDT_Byte, 0, 0));
    EXPECT_EQ(pixels, actual);
  }

  // 3. GDT_Int16 (full 16-bit).
  {
    auto src = CreateMem(kWidth, kHeight, 1, GDT_Int16);
    ASSERT_NE(nullptr, src);
    std::vector<int16_t> pixels(kWidth * kHeight);
    for (int i = 0; i < kWidth * kHeight; ++i) {
      pixels[i] = static_cast<int16_t>((i - 128) * 100);
    }
    ASSERT_EQ(CE_None, src->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, kWidth, kHeight, pixels.data(),
                           kWidth, kHeight, GDT_Int16, 0, 0));

    constexpr char kPath[] = "/vsimem/jp2kakdataset_test/int16.jp2";
    absl::Cleanup cleanup = [&] { VSIUnlink(kPath); };
    CPLStringList opts;
    opts.SetNameValue("QUALITY", "100");
    auto created = CreateJp2Copy(kPath, src.get(), opts.List());
    ASSERT_NE(nullptr, created);
    created.reset();

    auto ds = OpenDirect(kPath);
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(GDT_Int16, ds->GetRasterBand(1)->GetRasterDataType());
    std::vector<int16_t> actual(kWidth * kHeight, -999);
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, kWidth, kHeight, actual.data(),
                           kWidth, kHeight, GDT_Int16, 0, 0));
    EXPECT_EQ(pixels, actual);
  }

  // 4. GDT_UInt16 (full 16-bit).
  {
    auto src = CreateMem(kWidth, kHeight, 1, GDT_UInt16);
    ASSERT_NE(nullptr, src);
    std::vector<uint16_t> pixels(kWidth * kHeight);
    for (int i = 0; i < kWidth * kHeight; ++i) {
      pixels[i] = static_cast<uint16_t>(i * 250);
    }
    ASSERT_EQ(CE_None, src->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, kWidth, kHeight, pixels.data(),
                           kWidth, kHeight, GDT_UInt16, 0, 0));

    constexpr char kPath[] = "/vsimem/jp2kakdataset_test/uint16.jp2";
    absl::Cleanup cleanup = [&] { VSIUnlink(kPath); };
    CPLStringList opts;
    opts.SetNameValue("QUALITY", "100");
    auto created = CreateJp2Copy(kPath, src.get(), opts.List());
    ASSERT_NE(nullptr, created);
    created.reset();

    auto ds = OpenDirect(kPath);
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(GDT_UInt16, ds->GetRasterBand(1)->GetRasterDataType());
    std::vector<uint16_t> actual(kWidth * kHeight, 0xABCD);
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, kWidth, kHeight, actual.data(),
                           kWidth, kHeight, GDT_UInt16, 0, 0));
    EXPECT_EQ(pixels, actual);
  }

  // 5. GDT_Int32 (reversible -> NBITS=27).
  {
    auto src = CreateMem(kWidth, kHeight, 1, GDT_Int32);
    ASSERT_NE(nullptr, src);
    std::vector<int32_t> pixels(kWidth * kHeight);
    for (int i = 0; i < kWidth * kHeight; ++i) {
      pixels[i] = (i - 128) * 10000;
    }
    ASSERT_EQ(CE_None, src->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, kWidth, kHeight, pixels.data(),
                           kWidth, kHeight, GDT_Int32, 0, 0));

    constexpr char kPath[] = "/vsimem/jp2kakdataset_test/int32.jp2";
    absl::Cleanup cleanup = [&] { VSIUnlink(kPath); };
    CPLStringList opts;
    opts.SetNameValue("QUALITY", "100");
    auto created = CreateJp2Copy(kPath, src.get(), opts.List());
    ASSERT_NE(nullptr, created);
    created.reset();

    auto ds = OpenDirect(kPath);
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(GDT_Int32, ds->GetRasterBand(1)->GetRasterDataType());
    EXPECT_STREQ("27", ds->GetRasterBand(1)->GetMetadataItem(
                           "NBITS", "IMAGE_STRUCTURE"));
    // transfer_dwords() in kdu_stripe_decompressor.cpp omits the reversible
    // dst_bits == original_bits (27 == 27) branch, leaving the destination
    // buffer untouched; pin the sentinel explicitly so an upstream fix prompts
    // updating this check to EXPECT_EQ(pixels, actual).
    std::vector<int32_t> actual(kWidth * kHeight, -777777);
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, kWidth, kHeight, actual.data(),
                           kWidth, kHeight, GDT_Int32, 0, 0));
    EXPECT_EQ(std::vector<int32_t>(kWidth * kHeight, -777777), actual);
  }

  // 6. GDT_UInt32 (reversible -> NBITS=27, plus lossy 32-bit round-trip).
  {
    auto src = CreateMem(kWidth, kHeight, 1, GDT_UInt32);
    ASSERT_NE(nullptr, src);
    std::vector<uint32_t> pixels(kWidth * kHeight);
    for (int i = 0; i < kWidth * kHeight; ++i) {
      pixels[i] = static_cast<uint32_t>(i) * 50000U;
    }
    ASSERT_EQ(CE_None, src->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, kWidth, kHeight, pixels.data(),
                           kWidth, kHeight, GDT_UInt32, 0, 0));

    constexpr char kPath[] = "/vsimem/jp2kakdataset_test/uint32.jp2";
    absl::Cleanup cleanup = [&] { VSIUnlink(kPath); };
    CPLStringList opts;
    opts.SetNameValue("QUALITY", "100");
    auto created = CreateJp2Copy(kPath, src.get(), opts.List());
    ASSERT_NE(nullptr, created);
    created.reset();

    auto ds = OpenDirect(kPath);
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(GDT_UInt32, ds->GetRasterBand(1)->GetRasterDataType());
    EXPECT_STREQ("27", ds->GetRasterBand(1)->GetMetadataItem(
                           "NBITS", "IMAGE_STRUCTURE"));
    std::vector<uint32_t> actual(kWidth * kHeight, 777777U);
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, kWidth, kHeight, actual.data(),
                           kWidth, kHeight, GDT_UInt32, 0, 0));
    EXPECT_EQ(std::vector<uint32_t>(kWidth * kHeight, 777777U), actual);

    constexpr char kLossyU32Path[] =
        "/vsimem/jp2kakdataset_test/uint32_lossy.jp2";
    absl::Cleanup cleanup_lossy = [&] { VSIUnlink(kLossyU32Path); };
    CPLStringList lossy_opts;
    lossy_opts.SetNameValue("QUALITY", "50");
    lossy_opts.SetNameValue("NBITS", "28");
    ASSERT_NE(nullptr,
              CreateJp2Copy(kLossyU32Path, src.get(), lossy_opts.List()));
    auto lossy_ds = OpenDirect(kLossyU32Path);
    ASSERT_NE(nullptr, lossy_ds);
    EXPECT_STREQ("28", lossy_ds->GetRasterBand(1)->GetMetadataItem(
                           "NBITS", "IMAGE_STRUCTURE"));
    std::vector<uint32_t> lossy_actual(kWidth * kHeight, 0xDEADBEEFU);
    ASSERT_EQ(CE_None, lossy_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, kWidth, kHeight, lossy_actual.data(),
                           kWidth, kHeight, GDT_UInt32, 0, 0));
    EXPECT_NE(0xDEADBEEFU, lossy_actual.front());
    EXPECT_NEAR(12750000.0,
                static_cast<double>(lossy_actual.back()) -
                    static_cast<double>(lossy_actual.front()),
                2000000.0);
  }

  // 7. Unsupported GDT_Float64 fails in strict mode, warns in non-strict mode.
  {
    auto src = CreateMem(kWidth, kHeight, 1, GDT_Float64);
    ASSERT_NE(nullptr, src);
    constexpr char kPath[] = "/vsimem/jp2kakdataset_test/float64.jp2";
    absl::Cleanup cleanup = [&] { VSIUnlink(kPath); };

    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(nullptr,
                CreateJp2Copy(kPath, src.get(), nullptr, /*strict=*/true));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(),
                  HasSubstr("does not support data type Float64"));
    }
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      auto ds = CreateJp2Copy(kPath, src.get(), nullptr, /*strict=*/false);
      EXPECT_NE(nullptr, ds);
      EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("forcing to Float32"));
    }
  }

  // 8. Invalid NBITS relative to data type size fails.
  constexpr char kBadNbitsPath[] = "/vsimem/jp2kakdataset_test/bad_nbits.jp2";
  absl::Cleanup cleanup_bad = [&] { VSIUnlink(kBadNbitsPath); };
  struct InvalidNbitsCase {
    GDALDataType type;
    const char* nbits;
  };
  constexpr InvalidNbitsCase kInvalidNbits[] = {
      {.type = GDT_Byte, .nbits = "9"},   {.type = GDT_Int16, .nbits = "8"},
      {.type = GDT_Int16, .nbits = "17"}, {.type = GDT_Int32, .nbits = "16"},
      {.type = GDT_Int32, .nbits = "33"},
  };
  for (const InvalidNbitsCase& tc : kInvalidNbits) {
    auto src = CreateMem(8, 8, 1, tc.type);
    ASSERT_NE(nullptr, src);
    CPLStringList opts;
    opts.SetNameValue("NBITS", tc.nbits);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, CreateJp2Copy(kBadNbitsPath, src.get(), opts.List()))
        << tc.nbits;
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Wrong value for NBITS compared to data type"));
  }
}

TEST_F(Jp2kakdatasetTest, TileWriterMultiAnalysisAllDataTypesAndErrors) {
  // Force kdu_multi_analysis (tile writer) instead of kdu_stripe_compressor.
  CPLConfigOptionSetter use_stripe("JP2KAK_USE_STRIPE_COMPRESSOR", "NO", false);

  constexpr int kWidth = 20;
  constexpr int kHeight = 18;
  constexpr char kOutPath[] = "/vsimem/jp2kakdataset_test/tile_writer.jp2";
  absl::Cleanup cleanup = [&] { VSIUnlink(kOutPath); };

  // 1. GDT_Byte with 16x16 tiles (exercises partial right/bottom tiles),
  //    both non-precise and JP2KAK_PRECISE=YES, lossy and reversible.
  {
    auto src = CreateMem(kWidth, kHeight, 1, GDT_Byte);
    ASSERT_NE(nullptr, src);
    std::vector<uint8_t> pixels(kWidth * kHeight, 42);
    ASSERT_EQ(CE_None, src->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, kWidth, kHeight, pixels.data(),
                           kWidth, kHeight, GDT_Byte, 0, 0));

    for (const char* precise : {"NO", "YES"}) {
      CPLConfigOptionSetter precise_opt("JP2KAK_PRECISE", precise, false);
      for (const char* quality : {"50", "100"}) {
        CPLStringList opts;
        opts.SetNameValue("BLOCKXSIZE", "16");
        opts.SetNameValue("BLOCKYSIZE", "16");
        opts.SetNameValue("QUALITY", quality);
        auto ds = CreateJp2Copy(kOutPath, src.get(), opts.List());
        ASSERT_NE(nullptr, ds) << precise << " " << quality;
        if (std::string_view(quality) == "100") {
          std::vector<uint8_t> actual(kWidth * kHeight, 0);
          ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                                 GF_Read, 0, 0, kWidth, kHeight, actual.data(),
                                 kWidth, kHeight, GDT_Byte, 0, 0));
          EXPECT_EQ(pixels, actual);
        }
      }
    }
  }

  // 2. GDT_Int16 and GDT_UInt16: NBITS=10 (16-bit line buffer) and NBITS=16
  //    (32-bit line buffer), both lossy and reversible.
  struct Tile16Case {
    GDALDataType type;
    double sample_val;
  };
  constexpr Tile16Case kTile16Cases[] = {
      {.type = GDT_Int16, .sample_val = -250.0},
      {.type = GDT_UInt16, .sample_val = 500.0},
  };
  for (const Tile16Case& tc : kTile16Cases) {
    for (const char* nbits : {"10", "16"}) {
      auto src = CreateMem(16, 16, 1, tc.type);
      ASSERT_NE(nullptr, src);
      const std::vector<double> pixels(16 * 16, tc.sample_val);
      ASSERT_EQ(CE_None,
                src->GetRasterBand(1)->RasterIO(
                    GF_Write, 0, 0, 16, 16, const_cast<double*>(pixels.data()),
                    16, 16, GDT_Float64, 0, 0));
      for (const char* quality : {"50", "100"}) {
        CPLStringList opts;
        opts.SetNameValue("NBITS", nbits);
        opts.SetNameValue("QUALITY", quality);
        auto ds = CreateJp2Copy(kOutPath, src.get(), opts.List());
        ASSERT_NE(nullptr, ds)
            << GDALGetDataTypeName(tc.type) << " " << nbits << " " << quality;
        if (std::string_view(quality) == "100") {
          std::vector<double> actual(16 * 16, -9999.0);
          ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                                 GF_Read, 0, 0, 16, 16, actual.data(), 16, 16,
                                 GDT_Float64, 0, 0));
          EXPECT_DOUBLE_EQ(tc.sample_val, actual.front());
          EXPECT_DOUBLE_EQ(tc.sample_val, actual.back());
        }
      }
    }
  }

  // 3. GDT_Int32 and GDT_UInt32 in tile writer (lossy 32-bit & lossless
  // 27-bit).
  {
    auto src_i32 = CreateMem(16, 16, 1, GDT_Int32);
    ASSERT_NE(nullptr, src_i32);
    auto src_u32 = CreateMem(16, 16, 1, GDT_UInt32);
    ASSERT_NE(nullptr, src_u32);
    for (const char* quality : {"50", "100"}) {
      CPLStringList opts;
      opts.SetNameValue("QUALITY", quality);
      EXPECT_NE(nullptr, CreateJp2Copy(kOutPath, src_i32.get(), opts.List()));
      EXPECT_NE(nullptr, CreateJp2Copy(kOutPath, src_u32.get(), opts.List()));
    }
  }

  // 4. GDT_Float32 path in JP2KAKCreateCopy_WriteTile (via non-strict Float64).
  {
    auto src_f64 = CreateMem(16, 16, 1, GDT_Float64);
    ASSERT_NE(nullptr, src_f64);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_NE(nullptr, CreateJp2Copy(kOutPath, src_f64.get(), nullptr,
                                     /*strict=*/false));
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  }

  // 5. Out-of-domain NBITS pixel value failures across all types in WriteTile.
  struct OutOfDomainCase {
    GDALDataType type;
    double value;
    const char* nbits;
    const char* precise;
  };
  constexpr OutOfDomainCase kOutOfDomainCases[] = {
      {.type = GDT_Byte, .value = 200.0, .nbits = "4", .precise = "NO"},
      {.type = GDT_Byte, .value = 200.0, .nbits = "4", .precise = "YES"},
      {.type = GDT_Int16, .value = 4096.0, .nbits = "10", .precise = "NO"},
      {.type = GDT_Int16, .value = 4096.0, .nbits = "12", .precise = "YES"},
      {.type = GDT_UInt16, .value = 5000.0, .nbits = "10", .precise = "NO"},
      {.type = GDT_UInt16, .value = 5000.0, .nbits = "12", .precise = "YES"},
      {.type = GDT_Int32, .value = 1000000.0, .nbits = "20", .precise = "NO"},
      {.type = GDT_UInt32, .value = 2000000.0, .nbits = "20", .precise = "NO"},
  };
  for (const OutOfDomainCase& tc : kOutOfDomainCases) {
    auto ood_ds = CreateMem(8, 8, 1, tc.type);
    ASSERT_NE(nullptr, ood_ds);
    double val = tc.value;
    ASSERT_EQ(CE_None,
              ood_ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 1, 1, &val, 1,
                                                 1, GDT_Float64, 0, 0));
    CPLConfigOptionSetter precise_opt("JP2KAK_PRECISE", tc.precise, false);
    CPLStringList opts;
    opts.SetNameValue("NBITS", tc.nbits);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, CreateJp2Copy(kOutPath, ood_ds.get(), opts.List()))
        << GDALGetDataTypeName(tc.type) << " NBITS=" << tc.nbits
        << " PRECISE=" << tc.precise;
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Value outside of domain allowed by NBITS value"));
  }
}

TEST_F(Jp2kakdatasetTest, StripeCompressorAllDataTypesAndMaxBufferFallback) {
  constexpr char kOutPath[] = "/vsimem/jp2kakdataset_test/stripe.jp2";
  absl::Cleanup cleanup = [&] { VSIUnlink(kOutPath); };

  // 1. GDT_Int16 and GDT_UInt16 (2-byte stripe push) and GDT_Int32 (4-byte)
  //    with FLUSH=NO and COMSEG=NO.
  for (GDALDataType dt : {GDT_Int16, GDT_UInt16, GDT_Int32}) {
    auto src = CreateMem(16, 16, 2, dt);
    ASSERT_NE(nullptr, src);
    CPLStringList opts;
    opts.SetNameValue("FLUSH", "NO");
    opts.SetNameValue("COMSEG", "NO");
    EXPECT_NE(nullptr, CreateJp2Copy(kOutPath, src.get(), opts.List()));
  }

  // 2. JP2KAK_MAX_BUFFER_SIZE=1 with tiling triggers automatic fallback from
  //    stripe compressor to kdu_multi_analysis.
  {
    CPLConfigOptionSetter max_buf("JP2KAK_MAX_BUFFER_SIZE", "1", false);
    auto src = CreateMem(32, 32, 1, GDT_Byte);
    ASSERT_NE(nullptr, src);
    CPLStringList opts;
    opts.SetNameValue("BLOCKXSIZE", "16");
    opts.SetNameValue("BLOCKYSIZE", "16");
    EXPECT_NE(nullptr, CreateJp2Copy(kOutPath, src.get(), opts.List()));
  }
}

TEST_F(Jp2kakdatasetTest, ColorInterpretationsPaletteAndAlphaPromotion) {
  // 1. 3-band RGB JP2.
  {
    auto src = CreateMem(16, 16, 3, GDT_Byte);
    ASSERT_NE(nullptr, src);
    constexpr char kPath[] = "/vsimem/jp2kakdataset_test/rgb.jp2";
    absl::Cleanup cleanup = [&] { VSIUnlink(kPath); };
    ASSERT_NE(nullptr, CreateJp2Copy(kPath, src.get()));

    auto ds = OpenDirect(kPath);
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(GCI_RedBand, ds->GetRasterBand(1)->GetColorInterpretation());
    EXPECT_EQ(GCI_GreenBand, ds->GetRasterBand(2)->GetColorInterpretation());
    EXPECT_EQ(GCI_BlueBand, ds->GetRasterBand(3)->GetColorInterpretation());
  }

  // 2. 4-band RGBA JP2, premultiplied alpha mapping, and JP2_sYCC_SPACE.
  {
    auto src = CreateMem(16, 16, 4, GDT_Byte);
    ASSERT_NE(nullptr, src);
    src->GetRasterBand(4)->SetColorInterpretation(GCI_AlphaBand);
    constexpr char kPath[] = "/vsimem/jp2kakdataset_test/rgba.jp2";
    absl::Cleanup cleanup = [&] { VSIUnlink(kPath); };
    ASSERT_NE(nullptr, CreateJp2Copy(kPath, src.get()));

    auto ds = OpenDirect(kPath);
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(GCI_RedBand, ds->GetRasterBand(1)->GetColorInterpretation());
    EXPECT_EQ(GCI_GreenBand, ds->GetRasterBand(2)->GetColorInterpretation());
    EXPECT_EQ(GCI_BlueBand, ds->GetRasterBand(3)->GetColorInterpretation());
    EXPECT_EQ(GCI_AlphaBand, ds->GetRasterBand(4)->GetColorInterpretation());

    constexpr char kPremultPath[] = "/vsimem/jp2kakdataset_test/premult.jp2";
    absl::Cleanup cleanup_premult = [&] { VSIUnlink(kPremultPath); };
    const std::vector<std::vector<kdu_byte>> planes4(
        4, std::vector<kdu_byte>(64, 64));
    constexpr std::array<int, 4> kPrec8 = {8, 8, 8, 8};
    WriteUniformJp2(kPremultPath, 8, 8, planes4, kPrec8, [](jp2_target& jp2) {
      jp2.access_colour().init(JP2_sRGB_SPACE);
      jp2_channels chans = jp2.access_channels();
      chans.init(3);
      chans.set_colour_mapping(0, 0);
      chans.set_colour_mapping(1, 1);
      chans.set_colour_mapping(2, 2);
      chans.set_premult_mapping(0, 3);
      chans.set_premult_mapping(1, 3);
      chans.set_premult_mapping(2, 3);
    });

    auto premult_ds = OpenDirect(kPremultPath);
    ASSERT_NE(nullptr, premult_ds);
    ASSERT_EQ(4, premult_ds->GetRasterCount());
    EXPECT_EQ(GCI_AlphaBand,
              premult_ds->GetRasterBand(4)->GetColorInterpretation());

    constexpr char kSyccPath[] = "/vsimem/jp2kakdataset_test/sycc.jp2";
    absl::Cleanup cleanup_sycc = [&] { VSIUnlink(kSyccPath); };
    const std::vector<std::vector<kdu_byte>> planes3(
        3, std::vector<kdu_byte>(64, 64));
    WriteUniformJp2(
        kSyccPath, 8, 8, planes3, std::span<const int>(kPrec8.data(), 3),
        [](jp2_target& jp2) { jp2.access_colour().init(JP2_sYCC_SPACE); });

    auto sycc_ds = OpenDirect(kSyccPath);
    ASSERT_NE(nullptr, sycc_ds);
    EXPECT_EQ(3, sycc_ds->GetRasterCount());
  }

  // 3. 2-band Gray+Alpha JP2 and 4-band with undefined 4th band.
  {
    auto src_ga = CreateMem(16, 16, 2, GDT_Byte);
    ASSERT_NE(nullptr, src_ga);
    src_ga->GetRasterBand(2)->SetColorInterpretation(GCI_AlphaBand);
    constexpr char kGaPath[] = "/vsimem/jp2kakdataset_test/ga.jp2";
    absl::Cleanup cleanup_ga = [&] { VSIUnlink(kGaPath); };
    ASSERT_NE(nullptr, CreateJp2Copy(kGaPath, src_ga.get()));

    auto ga_ds = OpenDirect(kGaPath);
    ASSERT_NE(nullptr, ga_ds);
    EXPECT_EQ(GCI_GrayIndex, ga_ds->GetRasterBand(1)->GetColorInterpretation());
    EXPECT_EQ(GCI_AlphaBand, ga_ds->GetRasterBand(2)->GetColorInterpretation());

    auto src_4u = CreateMem(16, 16, 4, GDT_Byte);
    ASSERT_NE(nullptr, src_4u);
    constexpr char k4uPath[] = "/vsimem/jp2kakdataset_test/four_undef.jp2";
    absl::Cleanup cleanup_4u = [&] { VSIUnlink(k4uPath); };
    ASSERT_NE(nullptr, CreateJp2Copy(k4uPath, src_4u.get()));

    auto ds_4u = OpenDirect(k4uPath);
    ASSERT_NE(nullptr, ds_4u);
    EXPECT_EQ(GCI_Undefined, ds_4u->GetRasterBand(4)->GetColorInterpretation());
  }

  // 4. 4-band raw J2K codestream (!oJP2Channels.exists() -> R, G, B, Gray).
  {
    auto src = CreateMem(16, 16, 4, GDT_Byte);
    ASSERT_NE(nullptr, src);
    constexpr char kJ2kPath[] = "/vsimem/jp2kakdataset_test/four_bands.j2k";
    absl::Cleanup cleanup = [&] { VSIUnlink(kJ2kPath); };
    ASSERT_NE(nullptr, CreateJp2Copy(kJ2kPath, src.get()));

    auto ds = OpenDirect(kJ2kPath);
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(GCI_RedBand, ds->GetRasterBand(1)->GetColorInterpretation());
    EXPECT_EQ(GCI_GreenBand, ds->GetRasterBand(2)->GetColorInterpretation());
    EXPECT_EQ(GCI_BlueBand, ds->GetRasterBand(3)->GetColorInterpretation());
    EXPECT_EQ(GCI_GrayIndex, ds->GetRasterBand(4)->GetColorInterpretation());
  }

  // 5. 1-band Palette JP2 (3-LUT, 4-LUT RGBA, and 1-LUT early return).
  {
    auto src = CreateMem(16, 16, 1, GDT_Byte);
    ASSERT_NE(nullptr, src);
    GDALColorTable ct;
    const GDALColorEntry c0 = {10, 20, 30, 255};
    const GDALColorEntry c1 = {100, 90, 80, 255};
    ct.SetColorEntry(0, &c0);
    ct.SetColorEntry(1, &c1);
    src->GetRasterBand(1)->SetColorTable(&ct);

    constexpr char kPalPath[] = "/vsimem/jp2kakdataset_test/palette.jp2";
    absl::Cleanup cleanup = [&] { VSIUnlink(kPalPath); };
    ASSERT_NE(nullptr, CreateJp2Copy(kPalPath, src.get()));

    auto ds = OpenDirect(kPalPath);
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    EXPECT_EQ(GCI_PaletteIndex, band->GetColorInterpretation());
    GDALColorTable* out_ct = band->GetColorTable();
    ASSERT_NE(nullptr, out_ct);
    ASSERT_EQ(2, out_ct->GetColorEntryCount());
    EXPECT_EQ(10, out_ct->GetColorEntry(0)->c1);
    EXPECT_EQ(20, out_ct->GetColorEntry(0)->c2);
    EXPECT_EQ(30, out_ct->GetColorEntry(0)->c3);

    const std::vector<std::vector<kdu_byte>> idx_planes(
        1, std::vector<kdu_byte>(64, 0));
    constexpr std::array<int, 1> kOnePrec8 = {8};

    constexpr char kPal4Path[] = "/vsimem/jp2kakdataset_test/palette4.jp2";
    absl::Cleanup cleanup_pal4 = [&] { VSIUnlink(kPal4Path); };
    WriteUniformJp2(kPal4Path, 8, 8, idx_planes, kOnePrec8,
                    [](jp2_target& jp2) {
                      jp2.access_colour().init(JP2_sRGB_SPACE);
                      jp2_palette pal = jp2.access_palette();
                      pal.init(4, 2);
                      std::array<kdu_int32, 2> r_lut = {10, 100};
                      std::array<kdu_int32, 2> g_lut = {20, 90};
                      std::array<kdu_int32, 2> b_lut = {30, 80};
                      std::array<kdu_int32, 2> a_lut = {120, 64};
                      pal.set_lut(0, r_lut.data(), 8, false);
                      pal.set_lut(1, g_lut.data(), 8, false);
                      pal.set_lut(2, b_lut.data(), 8, false);
                      pal.set_lut(3, a_lut.data(), 8, false);
                      jp2_channels chans = jp2.access_channels();
                      chans.init(3);
                      chans.set_colour_mapping(0, 0, 0);
                      chans.set_colour_mapping(1, 0, 1);
                      chans.set_colour_mapping(2, 0, 2);
                      chans.set_opacity_mapping(0, 0, 3);
                    });

    auto pal4_ds = OpenDirect(kPal4Path);
    ASSERT_NE(nullptr, pal4_ds);
    GDALColorTable* pal4_ct = pal4_ds->GetRasterBand(1)->GetColorTable();
    ASSERT_NE(nullptr, pal4_ct);
    ASSERT_EQ(2, pal4_ct->GetColorEntryCount());
    EXPECT_EQ(120, pal4_ct->GetColorEntry(0)->c4);
    EXPECT_EQ(64, pal4_ct->GetColorEntry(1)->c4);

    constexpr char kPal1Path[] = "/vsimem/jp2kakdataset_test/palette1.jp2";
    absl::Cleanup cleanup_pal1 = [&] { VSIUnlink(kPal1Path); };
    WriteUniformJp2(kPal1Path, 8, 8, idx_planes, kOnePrec8,
                    [](jp2_target& jp2) {
                      jp2.access_colour().init(JP2_sLUM_SPACE);
                      jp2_palette pal = jp2.access_palette();
                      pal.init(1, 2);
                      std::array<kdu_int32, 2> l_lut = {10, 100};
                      pal.set_lut(0, l_lut.data(), 8, false);
                      jp2_channels chans = jp2.access_channels();
                      chans.init(1);
                      chans.set_colour_mapping(0, 0, 0);
                    });

    auto pal1_ds = OpenDirect(kPal1Path);
    ASSERT_NE(nullptr, pal1_ds);
    EXPECT_EQ(nullptr, pal1_ds->GetRasterBand(1)->GetColorTable());
  }

  // 6. 8,8,8,1-bit RGBA JP2 exercises bPromoteTo8Bit in Open() and 1-bit alpha
  //    promotion in DirectRasterIO().
  {
    constexpr char kAlpha1Path[] = "/vsimem/jp2kakdataset_test/alpha1bit.jp2";
    absl::Cleanup cleanup = [&] { VSIUnlink(kAlpha1Path); };

    std::vector<std::vector<kdu_byte>> planes(4);
    for (int c = 0; c < 4; ++c) {
      planes[c].assign(16 * 16, c == 3 ? 1 : 100);
      if (c == 3) {
        planes[c][0] = 0;
      }
    }
    constexpr std::array<int, 4> kPrecs = {8, 8, 8, 1};
    WriteUniformJp2(kAlpha1Path, 16, 16, planes, kPrecs, [](jp2_target& jp2) {
      jp2.access_colour().init(JP2_sRGB_SPACE);
      jp2_channels chans = jp2.access_channels();
      chans.init(3);
      chans.set_colour_mapping(0, 0);
      chans.set_colour_mapping(1, 1);
      chans.set_colour_mapping(2, 2);
      chans.set_opacity_mapping(0, 3);
      chans.set_opacity_mapping(1, 3);
      chans.set_opacity_mapping(2, 3);
    });

    auto promoted_ds = OpenDirect(kAlpha1Path);
    ASSERT_NE(nullptr, promoted_ds);
    EXPECT_EQ(nullptr, promoted_ds->GetRasterBand(4)->GetMetadataItem(
                           "NBITS", "IMAGE_STRUCTURE"));
    std::vector<uint8_t> rgba_buf(16 * 16 * 4, 99);
    std::array<int, 4> band_map = {1, 2, 3, 4};
    ASSERT_EQ(CE_None,
              promoted_ds->RasterIO(GF_Read, 0, 0, 16, 16, rgba_buf.data(), 16,
                                    16, GDT_Byte, 4, band_map.data(), 0, 0, 0));
    EXPECT_EQ(0, rgba_buf[3 * 256 + 0]);
    EXPECT_EQ(255, rgba_buf[3 * 256 + 1]);

    CPLStringList no_promo;
    no_promo.SetNameValue("1BIT_ALPHA_PROMOTION", "NO");
    auto raw_ds = OpenDirect(kAlpha1Path, GDAL_OF_READONLY | GDAL_OF_RASTER,
                             no_promo.List());
    ASSERT_NE(nullptr, raw_ds);
    EXPECT_STREQ("1", raw_ds->GetRasterBand(4)->GetMetadataItem(
                          "NBITS", "IMAGE_STRUCTURE"));
    std::vector<uint8_t> raw_rgba(16 * 16 * 4, 99);
    ASSERT_EQ(CE_None,
              raw_ds->RasterIO(GF_Read, 0, 0, 16, 16, raw_rgba.data(), 16, 16,
                               GDT_Byte, 4, band_map.data(), 0, 0, 0));
    EXPECT_EQ(0, raw_rgba[3 * 256 + 0]);
    EXPECT_EQ(1, raw_rgba[3 * 256 + 1]);
  }
}

TEST_F(Jp2kakdatasetTest, BlockSizesPartialBlocksAndYccBlockCache) {
  // 1. 2100x4 single-tile JP2 tests 2048 block-size cap and USE_TILE_AS_BLOCK.
  {
    auto src = CreateMem(2100, 4, 1, GDT_Byte);
    ASSERT_NE(nullptr, src);
    constexpr char kWidePath[] = "/vsimem/jp2kakdataset_test/wide.jp2";
    absl::Cleanup cleanup = [&] { VSIUnlink(kWidePath); };
    CPLStringList opts;
    opts.SetNameValue("BLOCKXSIZE", "2100");
    opts.SetNameValue("BLOCKYSIZE", "4");
    ASSERT_NE(nullptr, CreateJp2Copy(kWidePath, src.get(), opts.List()));

    auto default_ds = OpenDirect(kWidePath);
    ASSERT_NE(nullptr, default_ds);
    int bx = 0;
    int by = 0;
    default_ds->GetRasterBand(1)->GetBlockSize(&bx, &by);
    EXPECT_EQ(2048, bx);
    EXPECT_EQ(4, by);

    CPLConfigOptionSetter use_tile("USE_TILE_AS_BLOCK", "YES", false);
    auto tile_ds = OpenDirect(kWidePath);
    ASSERT_NE(nullptr, tile_ds);
    tile_ds->GetRasterBand(1)->GetBlockSize(&bx, &by);
    EXPECT_EQ(2100, bx);
    EXPECT_EQ(4, by);
  }

  // 2. 20x18 3-band RGB JP2 with 16x16 tiles tests YCC multi-band block
  //    caching and partial right/bottom/corner blocks.
  {
    auto src = CreateMem(20, 18, 3, GDT_Byte);
    ASSERT_NE(nullptr, src);
    for (int b = 1; b <= 3; ++b) {
      std::vector<uint8_t> data(20 * 18, static_cast<uint8_t>(b * 50));
      ASSERT_EQ(CE_None, src->GetRasterBand(b)->RasterIO(GF_Write, 0, 0, 20, 18,
                                                         data.data(), 20, 18,
                                                         GDT_Byte, 0, 0));
    }

    constexpr char kYccPath[] = "/vsimem/jp2kakdataset_test/ycc_blocks.jp2";
    absl::Cleanup cleanup = [&] { VSIUnlink(kYccPath); };
    CPLStringList opts;
    opts.SetNameValue("BLOCKXSIZE", "16");
    opts.SetNameValue("BLOCKYSIZE", "16");
    opts.SetNameValue("QUALITY", "100");
    ASSERT_NE(nullptr, CreateJp2Copy(kYccPath, src.get(), opts.List()));

    auto ds = OpenDirect(kYccPath);
    ASSERT_NE(nullptr, ds);
    std::vector<uint8_t> block(16 * 16, 0xFF);
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->ReadBlock(0, 0, block.data()));
    EXPECT_EQ(50, block[0]);
    std::fill(block.begin(), block.end(), 0xFF);
    ASSERT_EQ(CE_None, ds->GetRasterBand(2)->ReadBlock(0, 0, block.data()));
    EXPECT_EQ(100, block[0]);
    for (const auto [bx, by] :
         {std::pair{1, 0}, std::pair{0, 1}, std::pair{1, 1}}) {
      std::fill(block.begin(), block.end(), 0xFF);
      ASSERT_EQ(CE_None, ds->GetRasterBand(1)->ReadBlock(bx, by, block.data()));
      EXPECT_EQ(50, block[0]);
    }
  }
}

TEST_F(Jp2kakdatasetTest, OverviewsInternalAndExternal) {
  // 1. 256x256 image creates 2 internal overviews (128x128 and 64x64) and
  //    skips smaller resolution levels.
  auto src = CreateMem(256, 256, 1, GDT_Byte);
  ASSERT_NE(nullptr, src);
  std::vector<uint8_t> pixels(256 * 256);
  for (int y = 0; y < 256; ++y) {
    for (int x = 0; x < 256; ++x) {
      pixels[y * 256 + x] = static_cast<uint8_t>((x + y) & 0xFF);
    }
  }
  ASSERT_EQ(CE_None, src->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 256, 256,
                                                     pixels.data(), 256, 256,
                                                     GDT_Byte, 0, 0));

  constexpr char kDir[] = "/vsimem/jp2kakdataset_test/overviews";
  constexpr char kPath[] = "/vsimem/jp2kakdataset_test/overviews/ovr.jp2";
  absl::Cleanup cleanup_dir = [&] { VSIRmdirRecursive(kDir); };

  CPLStringList opts;
  opts.SetNameValue("QUALITY", "100");
  ASSERT_NE(nullptr, CreateJp2Copy(kPath, src.get(), opts.List()));

  {
    auto ds = OpenDirect(kPath);
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    EXPECT_EQ(2, band->GetOverviewCount());
    EXPECT_EQ(nullptr, band->GetOverview(-1));
    EXPECT_EQ(nullptr, band->GetOverview(2));

    GDALRasterBand* ovr0 = band->GetOverview(0);
    ASSERT_NE(nullptr, ovr0);
    EXPECT_EQ(128, ovr0->GetXSize());
    EXPECT_EQ(128, ovr0->GetYSize());

    std::vector<uint8_t> ds_ovr_buf(128 * 128, 0xAA);
    int band_map = 1;
    ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 256, 256, ds_ovr_buf.data(),
                                    128, 128, GDT_Byte, 1, &band_map, 0, 0, 0));
    EXPECT_NE(std::vector<uint8_t>(128 * 128, 0xAA), ds_ovr_buf);

    std::vector<uint8_t> band_ovr_buf(128 * 128, 0x55);
    ASSERT_EQ(CE_None,
              band->RasterIO(GF_Read, 0, 0, 256, 256, band_ovr_buf.data(), 128,
                             128, GDT_Byte, 0, 0));
    EXPECT_EQ(ds_ovr_buf, band_ovr_buf);

    // Non-power-of-two downsample (256->100) uses overview + intermediate
    // buffer.
    std::vector<uint8_t> resampled_nn(100 * 100, 0xBB);
    ASSERT_EQ(CE_None,
              band->RasterIO(GF_Read, 0, 0, 256, 256, resampled_nn.data(), 100,
                             100, GDT_Byte, 0, 0));
    EXPECT_NE(std::vector<uint8_t>(100 * 100, 0xBB), resampled_nn);

    // Non-nearest resampling (Bilinear) delegates to RasterIOResampled().
    GDALRasterIOExtraArg extra_arg;
    INIT_RASTERIO_EXTRA_ARG(extra_arg);
    extra_arg.eResampleAlg = GRIORA_Bilinear;
    std::vector<uint8_t> resampled_bilinear(100 * 100, 0xCC);
    ASSERT_EQ(CE_None,
              band->RasterIO(GF_Read, 0, 0, 256, 256, resampled_bilinear.data(),
                             100, 100, GDT_Byte, 0, 0, &extra_arg));
    EXPECT_NE(std::vector<uint8_t>(100 * 100, 0xCC), resampled_bilinear);
  }

  // 2. Building external overviews clears internal overviews and creates .ovr.
  {
    auto ds = OpenDirect(kPath);
    ASSERT_NE(nullptr, ds);
    const int ovr_list[] = {2};
    ASSERT_EQ(CE_None, ds->BuildOverviews("NEAREST", 1, ovr_list, 0, nullptr,
                                          nullptr, nullptr));
    GDALRasterBand* band = ds->GetRasterBand(1);
    EXPECT_EQ(1, band->GetOverviewCount());
    ASSERT_NE(nullptr, band->GetOverview(0));
    EXPECT_EQ(128, band->GetOverview(0)->GetXSize());
  }

  // 3. Re-opening detects existing external .ovr and skips internal overviews.
  {
    auto ds = OpenDirect(kPath);
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(1, ds->GetRasterBand(1)->GetOverviewCount());
  }
}

TEST_F(Jp2kakdatasetTest, DirectRasterIOAndBlockIOSelection) {
  constexpr int kWidth = 20;
  constexpr int kHeight = 20;

  // Test exact-resolution and resampled DirectRasterIO across all supported
  // integer data types, plus TestUseBlockIO selection criteria.
  for (GDALDataType dt :
       {GDT_Byte, GDT_Int16, GDT_UInt16, GDT_Int32, GDT_UInt32}) {
    auto src = CreateMem(kWidth, kHeight, 1, dt);
    ASSERT_NE(nullptr, src);
    std::vector<uint8_t> init_pixels(kWidth * kHeight);
    for (int i = 0; i < kWidth * kHeight; ++i) {
      init_pixels[i] = static_cast<uint8_t>((i + 10) & 0x7F);
    }
    ASSERT_EQ(CE_None, src->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, kWidth, kHeight, init_pixels.data(),
                           kWidth, kHeight, GDT_Byte, 0, 0));
    constexpr char kPath[] = "/vsimem/jp2kakdataset_test/direct_io.jp2";
    absl::Cleanup cleanup = [&] { VSIUnlink(kPath); };
    CPLStringList create_opts;
    if (dt == GDT_UInt32) {
      // Avoid 32-bit unsigned offset (-1 << 31) signed overflow in Kakadu's
      // kdu_stripe_compressor under ASan/UBSan while keeping GDT_UInt32.
      create_opts.SetNameValue("NBITS", "28");
    } else {
      create_opts.SetNameValue("QUALITY", "100");
    }
    ASSERT_NE(nullptr, CreateJp2Copy(kPath, src.get(), create_opts.List()));

    auto ds = OpenDirect(kPath);
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(dt, ds->GetRasterBand(1)->GetRasterDataType());
    const int dt_size = GDALGetDataTypeSizeBytes(dt);

    // 1. Exact resolution DirectRasterIO (fast path, no intermediate buffer).
    std::vector<uint8_t> exact_buf(kWidth * kHeight * dt_size, 0xAA);
    int band_map = 1;
    ASSERT_EQ(CE_None,
              ds->RasterIO(GF_Read, 0, 0, kWidth, kHeight, exact_buf.data(),
                           kWidth, kHeight, dt, 1, &band_map, 0, 0, 0));
    if (dt == GDT_Byte) {
      EXPECT_EQ(init_pixels, exact_buf);
    }

    // 2. Upsampled DirectRasterIO with GDAL_ONE_BIG_READ=YES exercises
    //    intermediate buffer + nearest-neighbor resampling for each data type.
    //    Upstream DirectRasterIO (jp2kakdataset.cpp:1423-1426) leaves
    //    vector_safe_bool is_signed[i] default-initialized to false for
    //    Int32/UInt32 (value-initialized via C++20 default constructor), and
    //    under ASan/UBSan Kakadu's 32-bit float-to-int stripe transfer
    //    overflows signed kdu_int32 when subtracting neg_off = (-1 << 31).
#if defined(UNDEFINED_BEHAVIOR_SANITIZER) || \
    defined(ABSL_HAVE_ADDRESS_SANITIZER)
    if (dt != GDT_Int32 && dt != GDT_UInt32)
#endif
    {
      CPLConfigOptionSetter one_big("GDAL_ONE_BIG_READ", "YES", false);
      std::vector<uint8_t> up_buf(28 * 24 * dt_size, 0x55);
      ASSERT_EQ(CE_None,
                ds->RasterIO(GF_Read, 0, 0, kWidth, kHeight, up_buf.data(), 28,
                             24, dt, 1, &band_map, 0, 0, 0));
      // Reversible 32-bit decompression leaves pabyIntermediate uninitialized
      // (kdu_stripe_decompressor::transfer_dwords() omits dst_bits ==
      // original_bits), so avoid reading copied uninitialized bytes under MSan.
      if (dt != GDT_Int32 && dt != GDT_UInt32) {
        EXPECT_NE(0x55, up_buf[0]);
      }
    }
  }

  // 3. TestUseBlockIO branches: type mismatch, duplicate bands, small window,
  //    and GDAL_ONE_BIG_READ=NO.
  {
    auto src = CreateMem(20, 20, 2, GDT_Byte);
    ASSERT_NE(nullptr, src);
    for (int b = 1; b <= 2; ++b) {
      std::vector<uint8_t> band_data(20 * 20, static_cast<uint8_t>(b * 50));
      ASSERT_EQ(CE_None, src->GetRasterBand(b)->RasterIO(GF_Write, 0, 0, 20, 20,
                                                         band_data.data(), 20,
                                                         20, GDT_Byte, 0, 0));
    }
    constexpr char kPath[] = "/vsimem/jp2kakdataset_test/block_io_rules.jp2";
    absl::Cleanup cleanup = [&] { VSIUnlink(kPath); };
    CPLStringList opts;
    opts.SetNameValue("QUALITY", "100");
    ASSERT_NE(nullptr, CreateJp2Copy(kPath, src.get(), opts.List()));

    auto ds = OpenDirect(kPath);
    ASSERT_NE(nullptr, ds);

    std::vector<double> f64_buf(20 * 20, -1.0);
    int band1 = 1;
    ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 20, 20, f64_buf.data(), 20,
                                    20, GDT_Float64, 1, &band1, 0, 0, 0));
    EXPECT_DOUBLE_EQ(50.0, f64_buf[0]);

    std::vector<uint8_t> dup_buf(20 * 20 * 2, 0xAA);
    std::array<int, 2> dup_bands = {1, 1};
    ASSERT_EQ(CE_None,
              ds->RasterIO(GF_Read, 0, 0, 20, 20, dup_buf.data(), 20, 20,
                           GDT_Byte, 2, dup_bands.data(), 0, 0, 0));
    EXPECT_EQ(50, dup_buf[0]);
    EXPECT_EQ(50, dup_buf[20 * 20]);

    std::vector<uint8_t> small_buf(25, 0xBB);
    ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 20, 1, small_buf.data(), 20,
                                    1, GDT_Byte, 1, &band1, 0, 0, 0));
    EXPECT_EQ(50, small_buf[0]);
    std::fill(small_buf.begin(), small_buf.end(), 0xBB);
    ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 5, 5, small_buf.data(), 5, 5,
                                    GDT_Byte, 1, &band1, 0, 0, 0));
    EXPECT_EQ(50, small_buf[0]);

    CPLConfigOptionSetter no_big("GDAL_ONE_BIG_READ", "NO", false);
    std::vector<uint8_t> full_buf(20 * 20, 0xCC);
    ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 20, 20, full_buf.data(), 20,
                                    20, GDT_Byte, 1, &band1, 0, 0, 0));
    EXPECT_EQ(50, full_buf[0]);
  }
}

TEST_F(Jp2kakdatasetTest, NonPersistentBufferedResilientAndSubfileModes) {
  auto src = CreateMem(20, 20, 1, GDT_Byte);
  ASSERT_NE(nullptr, src);
  std::vector<uint8_t> pixels(20 * 20, 77);
  ASSERT_EQ(CE_None,
            src->GetRasterBand(1)->RasterIO(
                GF_Write, 0, 0, 20, 20, pixels.data(), 20, 20, GDT_Byte, 0, 0));

  constexpr char kJp2Path[] = "/vsimem/jp2kakdataset_test/mode.jp2";
  constexpr char kJ2kPath[] = "/vsimem/jp2kakdataset_test/mode.j2k";
  absl::Cleanup cleanup_jp2 = [&] { VSIUnlink(kJp2Path); };
  absl::Cleanup cleanup_j2k = [&] { VSIUnlink(kJ2kPath); };

  CPLStringList lossless_opts;
  lossless_opts.SetNameValue("QUALITY", "100");
  ASSERT_NE(nullptr, CreateJp2Copy(kJp2Path, src.get(), lossless_opts.List()));
  ASSERT_NE(nullptr, CreateJp2Copy(kJ2kPath, src.get(), lossless_opts.List()));

  // 1. Non-persistent reads (JP2KAK_PERSIST=NO) + FUSSY + RESILIENT + BUFFERED
  //    on both JP2 and J2K files.
  for (const char* path : {kJp2Path, kJ2kPath}) {
    CPLConfigOptionSetter persist("JP2KAK_PERSIST", "NO", false);
    CPLConfigOptionSetter fussy("JP2KAK_FUSSY", "YES", false);
    CPLConfigOptionSetter resilient("JP2KAK_RESILIENT", "YES", false);
    CPLConfigOptionSetter buffered("JP2KAK_BUFFERED", "YES", false);

    auto ds = OpenDirect(path);
    ASSERT_NE(nullptr, ds) << path;
    std::vector<uint8_t> actual(20 * 20, 0);
    int band1 = 1;
    ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 20, 20, actual.data(), 20,
                                    20, GDT_Byte, 1, &band1, 0, 0, 0));
    EXPECT_EQ(pixels, actual);
  }

  // 2. Non-persistent read failure when backing file is unlinked mid-session.
  {
    constexpr char kEphemeral[] = "/vsimem/jp2kakdataset_test/ephemeral.jp2";
    absl::Cleanup cleanup_ephemeral = [&] { VSIUnlink(kEphemeral); };
    ASSERT_NE(nullptr,
              CreateJp2Copy(kEphemeral, src.get(), lossless_opts.List()));
    CPLConfigOptionSetter persist("JP2KAK_PERSIST", "NO", false);
    auto ds = OpenDirect(kEphemeral);
    ASSERT_NE(nullptr, ds);
    VSIUnlink(kEphemeral);

    std::vector<uint8_t> buf(20 * 20, 0xFF);
    int band1 = 1;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, ds->RasterIO(GF_Read, 0, 0, 20, 20, buf.data(), 20,
                                       20, GDT_Byte, 1, &band1, 0, 0, 0));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 3. J2K_SUBFILE: and /vsisubfile/ embedded codestream with leading padding.
  {
    const std::string j2k_bytes = ReadVsiFileBytes(kJ2kPath);
    ASSERT_FALSE(j2k_bytes.empty());
    constexpr size_t kOffset = 32;
    std::string container(kOffset, '\xAA');
    container.append(j2k_bytes);

    constexpr char kContainerPath[] =
        "/vsimem/jp2kakdataset_test/container.bin";
    VsiMemTempWrapper container_file(kContainerPath, container);

    const std::string j2k_subfile = std::format(
        "J2K_SUBFILE:{},{},{}", kOffset, j2k_bytes.size(), kContainerPath);
    auto sub_ds = OpenDirect(j2k_subfile.c_str());
    ASSERT_NE(nullptr, sub_ds);
    EXPECT_EQ(20, sub_ds->GetRasterXSize());
    EXPECT_EQ(20, sub_ds->GetRasterYSize());

    const std::string vsi_subfile = std::format(
        "/vsisubfile/{}_{},{}", kOffset, j2k_bytes.size(), kContainerPath);
    auto vsi_ds = OpenDirect(vsi_subfile.c_str());
    ASSERT_NE(nullptr, vsi_ds);
    EXPECT_EQ(20, vsi_ds->GetRasterXSize());
  }
}

TEST_F(Jp2kakdatasetTest, GeoreferencingMetadataResolutionAndVectorLayers) {
  // 1. GeoTransform, EPSG:32611 SRS, AREA_OR_POINT=Point, Resolution (inches
  //    and cm with non-square aspect), and xml:BOX_0 round-trip.
  {
    auto src = CreateMem(20, 20, 1, GDT_Byte);
    ASSERT_NE(nullptr, src);
    constexpr double kGt[6] = {440720.0, 60.0, 0.0, 3751320.0, 0.0, -60.0};
    ASSERT_EQ(CE_None, src->SetGeoTransform(kGt));

    OGRSpatialReference srs;
    ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(32611));
    ASSERT_EQ(CE_None, src->SetSpatialRef(&srs));
    src->SetMetadataItem(GDALMD_AREA_OR_POINT, GDALMD_AOP_POINT);
    src->SetMetadataItem("TIFFTAG_XRESOLUTION", "300");
    src->SetMetadataItem("TIFFTAG_YRESOLUTION", "150");
    src->SetMetadataItem("TIFFTAG_RESOLUTIONUNIT", "2");
    CPLStringList xml_boxes;
    xml_boxes.AddString("<custom><tag>val</tag></custom>");
    src->SetMetadata(xml_boxes.List(), "xml:BOX_0");

    constexpr char kGeoPath[] = "/vsimem/jp2kakdataset_test/geo_meta.jp2";
    absl::Cleanup cleanup = [&] { VSIUnlink(kGeoPath); };
    ASSERT_NE(nullptr, CreateJp2Copy(kGeoPath, src.get()));

    auto ds = OpenDirect(kGeoPath);
    ASSERT_NE(nullptr, ds);
    double out_gt[6] = {};
    ASSERT_EQ(CE_None, ds->GetGeoTransform(out_gt));
    EXPECT_THAT(out_gt,
                ElementsAre(440720.0, 60.0, 0.0, 3751320.0, 0.0, -60.0));
    const OGRSpatialReference* out_srs = ds->GetSpatialRef();
    ASSERT_NE(nullptr, out_srs);
    EXPECT_THAT(*out_srs, IsSameAs(srs));
    CSLConstList out_xml = ds->GetMetadata("xml:BOX_0");
    ASSERT_NE(nullptr, out_xml);
    EXPECT_STREQ("<custom><tag>val</tag></custom>", out_xml[0]);
    EXPECT_STREQ("3 (pixels/cm)",
                 ds->GetMetadataItem("TIFFTAG_RESOLUTIONUNIT"));
  }

  // 2. Square resolution in pixels/cm (RESOLUTIONUNIT=3) and GMLJP2=OFF /
  //    GeoJP2=OFF.
  {
    auto src = CreateMem(16, 16, 1, GDT_Byte);
    ASSERT_NE(nullptr, src);
    constexpr double kGt[6] = {100.0, 1.0, 0.0, 200.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, src->SetGeoTransform(kGt));
    src->SetMetadataItem("TIFFTAG_XRESOLUTION", "200");
    src->SetMetadataItem("TIFFTAG_YRESOLUTION", "200");
    src->SetMetadataItem("TIFFTAG_RESOLUTIONUNIT", "3");

    constexpr char kResPath[] = "/vsimem/jp2kakdataset_test/res_cm.jp2";
    absl::Cleanup cleanup = [&] { VSIUnlink(kResPath); };
    CPLStringList opts;
    opts.SetNameValue("GMLJP2", "OFF");
    opts.SetNameValue("GeoJP2", "OFF");
    ASSERT_NE(nullptr, CreateJp2Copy(kResPath, src.get(), opts.List()));

    auto res_ds = OpenDirect(kResPath);
    ASSERT_NE(nullptr, res_ds);
    EXPECT_STREQ("200", res_ds->GetMetadataItem("TIFFTAG_XRESOLUTION"));
    EXPECT_STREQ("3 (pixels/cm)",
                 res_ds->GetMetadataItem("TIFFTAG_RESOLUTIONUNIT"));
  }

  // 3. Unsupported SRS triggers warning when GMLJP2=YES is explicit, and
  //    succeeds quietly when GMLJP2 is omitted.
  {
    auto src = CreateMem(16, 16, 1, GDT_Byte);
    ASSERT_NE(nullptr, src);
    constexpr double kGt[6] = {10.0, 1.0, 0.0, 20.0, 0.0, -1.0};
    ASSERT_EQ(CE_None, src->SetGeoTransform(kGt));
    OGRSpatialReference local_srs;
    ASSERT_EQ(OGRERR_NONE, local_srs.SetLocalCS("CustomLocalEngineering"));
    ASSERT_EQ(CE_None, src->SetSpatialRef(&local_srs));

    constexpr char kLocalPath[] = "/vsimem/jp2kakdataset_test/local_srs.jp2";
    absl::Cleanup cleanup = [&] { VSIUnlink(kLocalPath); };
    EXPECT_NE(nullptr, CreateJp2Copy(kLocalPath, src.get()));

    CPLStringList gml_yes;
    gml_yes.SetNameValue("GMLJP2", "YES");
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_NE(nullptr, CreateJp2Copy(kLocalPath, src.get(), gml_yes.List()));
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  }

  // 4. GCPs and RPC metadata round-trip.
  {
    auto src = CreateMem(16, 16, 1, GDT_Byte);
    ASSERT_NE(nullptr, src);
    OGRSpatialReference srs;
    ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
    std::array<GDAL_GCP, 4> gcps{};
    GDALInitGCPs(4, gcps.data());
    absl::Cleanup deinit_gcps = [&] { GDALDeinitGCPs(4, gcps.data()); };
    for (int i = 0; i < 4; ++i) {
      gcps[i].dfGCPPixel = (i % 2) * 16.0;
      gcps[i].dfGCPLine = (i / 2) * 16.0;
      gcps[i].dfGCPX = -120.0 + (i % 2) * 0.1;
      gcps[i].dfGCPY = 35.0 - (i / 2) * 0.1;
    }
    ASSERT_EQ(CE_None, src->SetGCPs(4, gcps.data(), &srs));

    CPLStringList rpc_md;
    rpc_md.SetNameValue("LINE_OFF", "8.0");
    rpc_md.SetNameValue("SAMP_OFF", "8.0");
    rpc_md.SetNameValue("LAT_OFF", "35.0");
    rpc_md.SetNameValue("LONG_OFF", "-120.0");
    rpc_md.SetNameValue("HEIGHT_OFF", "100.0");
    rpc_md.SetNameValue("LINE_SCALE", "8.0");
    rpc_md.SetNameValue("SAMP_SCALE", "8.0");
    rpc_md.SetNameValue("LAT_SCALE", "0.1");
    rpc_md.SetNameValue("LONG_SCALE", "0.1");
    rpc_md.SetNameValue("HEIGHT_SCALE", "50.0");
    rpc_md.SetNameValue("LINE_NUM_COEFF",
                        "0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0");
    rpc_md.SetNameValue("LINE_DEN_COEFF",
                        "1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0");
    rpc_md.SetNameValue("SAMP_NUM_COEFF",
                        "0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0");
    rpc_md.SetNameValue("SAMP_DEN_COEFF",
                        "1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0");
    src->SetMetadata(rpc_md.List(), "RPC");

    constexpr char kGcpPath[] = "/vsimem/jp2kakdataset_test/gcp_rpc.jp2";
    absl::Cleanup cleanup = [&] { VSIUnlink(kGcpPath); };
    ASSERT_NE(nullptr, CreateJp2Copy(kGcpPath, src.get()));

    auto ds = OpenDirect(kGcpPath);
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(4, ds->GetGCPCount());
    CSLConstList out_rpc = ds->GetMetadata("RPC");
    ASSERT_NE(nullptr, out_rpc);
    EXPECT_STREQ("35", CSLFetchNameValue(out_rpc, "LAT_OFF"));
  }

  // 5. GMLJP2V2_DEF with embedded KML annotation and vector-only open mode.
  {
    constexpr char kKmlPath[] = "/vsimem/jp2kakdataset_test/annot.kml";
    VsiMemTempWrapper kml_file(kKmlPath,
                               R"xml(<?xml version="1.0" encoding="UTF-8"?>
<kml xmlns="http://www.opengis.net/kml/2.2">
  <Placemark><name>Pt1</name><Point><coordinates>-120.0,35.0,0</coordinates></Point></Placemark>
</kml>)xml");

    auto src = CreateMem(16, 16, 1, GDT_Byte);
    ASSERT_NE(nullptr, src);
    constexpr double kGt[6] = {-120.5, 0.01, 0.0, 35.5, 0.0, -0.01};
    ASSERT_EQ(CE_None, src->SetGeoTransform(kGt));
    OGRSpatialReference srs;
    ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
    ASSERT_EQ(CE_None, src->SetSpatialRef(&srs));

    const std::string gmlv2_def = std::format(
        R"json({{"root_instance":{{"annotations":["{}"]}}}})json", kKmlPath);
    CPLStringList opts;
    opts.SetNameValue("GMLJP2V2_DEF", gmlv2_def.c_str());

    constexpr char kVecJp2Path[] = "/vsimem/jp2kakdataset_test/vec_annot.jp2";
    absl::Cleanup cleanup = [&] { VSIUnlink(kVecJp2Path); };
    ASSERT_NE(nullptr, CreateJp2Copy(kVecJp2Path, src.get(), opts.List()));

    CPLStringList open_opts;
    open_opts.SetNameValue("OPEN_REMOTE_GML", "YES");
    auto vec_ds = OpenDirect(kVecJp2Path, GDAL_OF_READONLY | GDAL_OF_VECTOR,
                             open_opts.List());
    ASSERT_NE(nullptr, vec_ds);
    EXPECT_EQ(1, vec_ds->GetLayerCount());

    auto plain_src = CreateMem(16, 16, 1, GDT_Byte);
    ASSERT_NE(nullptr, plain_src);
    constexpr char kNoVecPath[] = "/vsimem/jp2kakdataset_test/no_vec.jp2";
    absl::Cleanup cleanup_novec = [&] { VSIUnlink(kNoVecPath); };
    ASSERT_NE(nullptr, CreateJp2Copy(kNoVecPath, plain_src.get()));
    EXPECT_EQ(nullptr,
              OpenDirect(kNoVecPath, GDAL_OF_READONLY | GDAL_OF_VECTOR));
  }
}

TEST_F(Jp2kakdatasetTest, CreationOptionsRateRoiProgressionOrdersAndErrors) {
  auto src = CreateMem(16, 16, 1, GDT_Byte);
  ASSERT_NE(nullptr, src);
  constexpr char kOutPath[] = "/vsimem/jp2kakdataset_test/options.jp2";
  absl::Cleanup cleanup = [&] { VSIUnlink(kOutPath); };

  // 1. Zero-band dataset is rejected.
  {
    auto zero_ds = CreateMem(16, 16, 0, GDT_Byte);
    ASSERT_NE(nullptr, zero_ds);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, CreateJp2Copy(kOutPath, zero_ds.get()));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Creating zero band files not supported"));
  }

  // 2. Invalid QUALITY values (< 0.01 and > 100.0).
  for (const char* bad_q : {"0.0", "100.1"}) {
    CPLStringList opts;
    opts.SetNameValue("QUALITY", bad_q);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, CreateJp2Copy(kOutPath, src.get(), opts.List()))
        << bad_q;
    EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
  }

  // 3. Invalid BLOCKXSIZE / BLOCKYSIZE (<= 0) and oversized block clamping.
  {
    CPLStringList bad_blk;
    bad_blk.SetNameValue("BLOCKXSIZE", "0");
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(nullptr, CreateJp2Copy(kOutPath, src.get(), bad_blk.List()));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(),
                  HasSubstr("Wrong value for BLOCKXSIZE/BLOCKYSIZE"));
    }

    CPLStringList big_blk;
    big_blk.SetNameValue("BLOCKXSIZE", "128");
    big_blk.SetNameValue("BLOCKYSIZE", "128");
    EXPECT_NE(nullptr, CreateJp2Copy(kOutPath, src.get(), big_blk.List()));
  }

  // 4. RATE option validation errors and valid multi-layer rate configurations.
  struct RateErrorCase {
    const char* rate;
    const char* layers_key;
    const char* layers_val;
    const char* expected_msg;
  };
  constexpr RateErrorCase kRateErrors[] = {
      {.rate = "",
       .layers_key = nullptr,
       .layers_val = nullptr,
       .expected_msg = "RATE argument must be followed"},
      {.rate = "0.5,1.0",
       .layers_key = "LAYERS",
       .layers_val = "1",
       .expected_msg = "relationship between the number of bit-rates"},
      {.rate = "0.5",
       .layers_key = "LAYERS",
       .layers_val = "0",
       .expected_msg = "relationship between the number of bit-rates"},
      {.rate = "0.2,0.5,1.0,2.0",
       .layers_key = "Clayers",
       .layers_val = "3",
       .expected_msg = "relationship between the number of bit-rates"},
      {.rate = "-1.0",
       .layers_key = nullptr,
       .layers_val = nullptr,
       .expected_msg = "not a legal value in the range 0.0-100"},
      {.rate = "105.0",
       .layers_key = nullptr,
       .layers_val = nullptr,
       .expected_msg = "not a legal value in the range 0.0-100"},
  };
  for (const RateErrorCase& tc : kRateErrors) {
    CPLStringList opts;
    opts.SetNameValue("RATE", tc.rate);
    if (tc.layers_key != nullptr) {
      opts.SetNameValue(tc.layers_key, tc.layers_val);
    }
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, CreateJp2Copy(kOutPath, src.get(), opts.List()))
        << tc.rate;
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr(tc.expected_msg));
  }

  for (const char* rate : {"0.5", "0.5,1.5", "0.5,1.0,2.0,3.0", "-,0.5"}) {
    CPLStringList opts;
    opts.SetNameValue("RATE", rate);
    opts.SetNameValue("Clayers", "4");
    EXPECT_NE(nullptr, CreateJp2Copy(kOutPath, src.get(), opts.List())) << rate;
  }

  // 5. Explicit Creversible=YES/NO and all Corder progression orders.
  struct OrderCase {
    const char* opt_val;
    const char* expected_md;
  };
  constexpr OrderCase kOrders[] = {
      {.opt_val = "LRCP", .expected_md = "LRCP"},
      {.opt_val = "RLCP", .expected_md = nullptr},
      {.opt_val = "RPCL", .expected_md = "RPCL"},
      {.opt_val = "PCRL", .expected_md = "PCRL"},
      {.opt_val = "CPRL", .expected_md = "CPRL"},
  };
  for (const OrderCase& oc : kOrders) {
    CPLStringList opts;
    opts.SetNameValue("Corder", oc.opt_val);
    opts.SetNameValue("Creversible", "YES");
    opts.SetNameValue("QUALITY", "50");
    ASSERT_NE(nullptr, CreateJp2Copy(kOutPath, src.get(), opts.List()))
        << oc.opt_val;
    auto ds = OpenDirect(kOutPath);
    ASSERT_NE(nullptr, ds);
    const char* actual = ds->GetMetadataItem("Corder", "IMAGE_STRUCTURE");
    if (oc.expected_md == nullptr) {
      EXPECT_EQ(nullptr, actual);
    } else {
      EXPECT_STREQ(oc.expected_md, actual);
    }
  }

  // 6. ROI option (corrupt warning + valid region), CODEC overrides, and
  //    invalid Kakadu parameter exception cleanup for JP2 and J2K.
  {
    CPLStringList bad_roi;
    bad_roi.SetNameValue("ROI", "1,2,3");
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_NE(nullptr, CreateJp2Copy(kOutPath, src.get(), bad_roi.List()));
      EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Skipping corrupt ROI def"));
    }

    CPLStringList good_roi;
    good_roi.SetNameValue("ROI", "0,0,8,8");
    CPLErrorReset();
    EXPECT_NE(nullptr, CreateJp2Copy(kOutPath, src.get(), good_roi.List()));
    EXPECT_EQ(CE_None, CPLGetLastErrorType());

    CPLStringList bad_kdu_jp2;
    bad_kdu_jp2.SetNameValue("Cblk", "{invalid}");
    CPLStringList bad_kdu_j2k;
    bad_kdu_j2k.SetNameValue("Cblk", "{invalid}");
    bad_kdu_j2k.SetNameValue("CODEC", "J2K");
    {
      WithQuietHandler quiet;
      absl::LeakCheckDisabler leak_disabler;
      CPLErrorReset();
      EXPECT_EQ(nullptr,
                CreateJp2Copy(kOutPath, src.get(), bad_kdu_jp2.List()));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      CPLErrorReset();
      EXPECT_EQ(nullptr,
                CreateJp2Copy(kOutPath, src.get(), bad_kdu_j2k.List()));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }

    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(nullptr, CreateJp2Copy("", src.get()));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }
  }
}

TEST_F(Jp2kakdatasetTest, ProgressCancellationAndSourceReadFailures) {
  auto src = CreateMem(16, 16, 1, GDT_Byte);
  ASSERT_NE(nullptr, src);
  constexpr char kOutPath[] = "/vsimem/jp2kakdataset_test/cancel.jp2";
  absl::Cleanup cleanup = [&] { VSIUnlink(kOutPath); };

  // 1. Progress callback cancels immediately at 0.0.
  {
    ProgressCancelContext ctx{.call_count = 0, .cancel_at_call = 1};
    EXPECT_EQ(nullptr, CreateJp2Copy(kOutPath, src.get(), nullptr, false,
                                     ProgressCallbackWithCancel, &ctx));
  }

  // 2. Progress callback cancels mid-stripe in kdu_stripe_compressor.
  {
    ProgressCancelContext ctx{.call_count = 0, .cancel_at_call = 2};
    EXPECT_EQ(nullptr, CreateJp2Copy(kOutPath, src.get(), nullptr, false,
                                     ProgressCallbackWithCancel, &ctx));
  }

  // 3. Progress callback cancels mid-tile in JP2KAKCreateCopy_WriteTile.
  {
    CPLConfigOptionSetter no_stripe("JP2KAK_USE_STRIPE_COMPRESSOR", "NO",
                                    false);
    ProgressCancelContext ctx{.call_count = 0, .cancel_at_call = 3};
    EXPECT_EQ(nullptr, CreateJp2Copy(kOutPath, src.get(), nullptr, false,
                                     ProgressCallbackWithCancel, &ctx));
  }

  // 4. Source dataset RasterIO failure in both stripe compressor and tile
  //    writer aborts CreateCopy and cleans up the output file.
  {
    FailingReadDataset failing_ds(16, 16, 1, GDT_Byte);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, CreateJp2Copy(kOutPath, &failing_ds));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    CPLConfigOptionSetter no_stripe("JP2KAK_USE_STRIPE_COMPRESSOR", "NO",
                                    false);
    CPLErrorReset();
    EXPECT_EQ(nullptr, CreateJp2Copy(kOutPath, &failing_ds));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(Jp2kakdatasetTest, MismatchedComponentDimensionsIgnoresSecondaryBands) {
  constexpr char kPath[] = "/vsimem/jp2kakdataset_test/subsampled.j2k";
  absl::Cleanup cleanup = [&] { VSIUnlink(kPath); };

  std::vector<kdu_byte> comp0(16 * 16, 128);
  std::vector<kdu_byte> comp1(8 * 8, 64);
  const std::array<kdu_byte*, 2> ptrs = {comp0.data(), comp1.data()};
  constexpr std::array<int, 2> kHeights = {16, 8};
  constexpr std::array<int, 2> kPrecs = {8, 8};
  WriteCustomCodestream(
      kPath, /*is_jp2=*/false, 2,
      [](siz_params& siz) {
        siz.set(Ssize, 0, 0, 16);
        siz.set(Ssize, 0, 1, 16);
        siz.set(Ssampling, 0, 0, 1);
        siz.set(Ssampling, 0, 1, 1);
        siz.set(Ssampling, 1, 0, 2);
        siz.set(Ssampling, 1, 1, 2);
      },
      [](siz_params& /*siz*/, jp2_target& /*jp2_tgt*/, kdu_codestream& cs) {
        cs.access_siz()->parse_string("Cycc=no");
      },
      ptrs, kHeights, kPrecs);

  WithQuietHandler quiet;
  CPLErrorReset();
  auto ds = OpenDirect(kPath);
  ASSERT_NE(nullptr, ds);
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Some components have mismatched dimensions"));
  EXPECT_EQ(1, ds->GetRasterCount());
}

// TODO: b/335317901 - Cover remaining branches in jp2kakdataset.cpp:
//   - JPIP streaming open/read/cleanup (USE_JPIP is disabled in google3).
//   - kdu_thread_env multi-threaded paths (JP2KAK_THREADS is pinned to 0 in
//     google3).
//   - Mixed per-band GDALDataType block-cache skip (IReadBlock line 342) and
//     kdu_simple_file_source direct POSIX file open (line 870).
//   - >2GB 32-bit kdu_long layer size clamp (line 2251), huge tile size clamp
//     (>25000px / >64K tiles, lines 2272-2304), and Kakadu exception catch
//     blocks in write_header/flush/oTile.close.

}  // namespace
}  // namespace autotest2
