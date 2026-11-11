// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Unit tests for libtiff RGBA image readers and color conversion routines
// (libtiff/tif_getimage.c), including the Google LogL samples/pixel patch.
//
// See also:
//   https://gitlab.com/libtiff/libtiff/-/blob/master/libtiff/tif_getimage.c

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/match.h"
#include "third_party/absl/types/span.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/temp_file.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"
#include "third_party/tiff/libtiff/tiffiop.h"

namespace libtiff {
namespace {

constexpr uint32_t PackAbgr(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 0xff) {
  return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) |
         (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) << 24);
}

bool HasDiagnostic(const std::vector<DiagnosticRecord>& records,
                   std::string_view substr) {
  return std::any_of(records.begin(), records.end(),
                     [substr](const DiagnosticRecord& rec) {
                       return absl::StrContains(rec.message, substr);
                     });
}

UniqueTiff OpenWithCumulatedLimit(MemoryTiffStream& stream, const char* mode,
                                  tmsize_t max_cumulated) {
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  stream.diagnostics().Install(opts.get());
  if (max_cumulated > 0) {
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), max_cumulated);
  }
  return stream.Open(mode, opts.get());
}

// Writes an arbitrary striped TIFF via libtiff APIs so codecs and directory
// tags match normal producer output.
struct StripSpec {
  uint32_t width = 8;
  uint32_t height = 8;
  uint16_t bps = 8;
  uint16_t spp = 3;
  uint16_t photometric = PHOTOMETRIC_RGB;
  uint16_t planar = PLANARCONFIG_CONTIG;
  uint16_t compression = COMPRESSION_NONE;
  uint16_t orientation = ORIENTATION_TOPLEFT;
  uint32_t rows_per_strip = 4;
  uint16_t sample_format = SAMPLEFORMAT_UINT;
  std::vector<uint16_t> extra_samples;
  std::vector<uint16_t> cmap_r;
  std::vector<uint16_t> cmap_g;
  std::vector<uint16_t> cmap_b;
  uint16_t inkset = INKSET_CMYK;
  bool set_inkset = false;
  uint16_t subsampling_x = 2;
  uint16_t subsampling_y = 2;
  bool set_subsampling = false;
  bool jpeg_rgb_mode = false;
  std::array<float, 3> ycbcr_coeffs{};
  bool set_ycbcr_coeffs = false;
  std::array<float, 6> ref_black_white{};
  bool set_ref_black_white = false;
  std::array<float, 2> white_point{};
  bool set_white_point = false;
  const char* open_mode = "wl";
};

void WriteStripedTiff(MemoryTiffStream& stream, const StripSpec& spec,
                      absl::Span<const uint8_t> raw_bytes) {
  UniqueTiff tif = stream.Open(spec.open_mode);
  ASSERT_NE(tif, nullptr);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, spec.width), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, spec.height), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, spec.bps), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, spec.spp), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, spec.photometric), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, spec.planar), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, spec.compression), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ORIENTATION, spec.orientation), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, spec.rows_per_strip),
            1);
  if (spec.sample_format != SAMPLEFORMAT_UINT) {
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLEFORMAT, spec.sample_format),
              1);
  }
  if (!spec.extra_samples.empty()) {
    std::vector<uint16_t> mutable_extras = spec.extra_samples;
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_EXTRASAMPLES,
                           static_cast<uint16_t>(mutable_extras.size()),
                           mutable_extras.data()),
              1);
  }
  if (!spec.cmap_r.empty()) {
    std::vector<uint16_t> r = spec.cmap_r;
    std::vector<uint16_t> g = spec.cmap_g;
    std::vector<uint16_t> b = spec.cmap_b;
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_COLORMAP, r.data(), g.data(), b.data()),
        1);
  }
  if (spec.set_inkset) {
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_INKSET, spec.inkset), 1);
  }
  if (spec.set_subsampling) {
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_YCBCRSUBSAMPLING,
                           spec.subsampling_x, spec.subsampling_y),
              1);
  }
  if (spec.jpeg_rgb_mode) {
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB),
              1);
  }
  if (spec.set_ycbcr_coeffs) {
    std::array<float, 3> coeffs = spec.ycbcr_coeffs;
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_YCBCRCOEFFICIENTS, coeffs.data()),
              1);
  }
  if (spec.set_ref_black_white) {
    std::array<float, 6> rbw = spec.ref_black_white;
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_REFERENCEBLACKWHITE, rbw.data()),
              1);
  }
  if (spec.set_white_point) {
    std::array<float, 2> wp = spec.white_point;
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_WHITEPOINT, wp.data()), 1);
  }

  const uint16_t planes = (spec.planar == PLANARCONFIG_SEPARATE) ? spec.spp : 1;
  const uint32_t strips_per_plane =
      (spec.height + spec.rows_per_strip - 1) / spec.rows_per_strip;
  size_t offset = 0;
  for (uint16_t p = 0; p < planes; ++p) {
    for (uint32_t s = 0; s < strips_per_plane; ++s) {
      const uint32_t strip_idx =
          static_cast<uint32_t>(p) * strips_per_plane + s;
      const uint32_t rows =
          std::min(spec.rows_per_strip, spec.height - s * spec.rows_per_strip);
      const tmsize_t strip_bytes = TIFFVStripSize(tif.get(), rows);
      ASSERT_GT(strip_bytes, 0);
      ASSERT_LE(offset + static_cast<size_t>(strip_bytes), raw_bytes.size());
      std::vector<uint8_t> mutable_strip(
          raw_bytes.begin() + offset,
          raw_bytes.begin() + offset + static_cast<size_t>(strip_bytes));
      ASSERT_EQ(TIFFWriteEncodedStrip(tif.get(), strip_idx,
                                      mutable_strip.data(), strip_bytes),
                strip_bytes);
      offset += static_cast<size_t>(strip_bytes);
    }
  }
  ASSERT_EQ(offset, raw_bytes.size());
  ASSERT_EQ(TIFFWriteDirectory(tif.get()), 1);
}

struct TileSpec {
  uint32_t width = 32;
  uint32_t height = 32;
  uint32_t tw = 16;
  uint32_t th = 16;
  uint16_t bps = 8;
  uint16_t spp = 3;
  uint16_t photometric = PHOTOMETRIC_RGB;
  uint16_t planar = PLANARCONFIG_CONTIG;
  uint16_t compression = COMPRESSION_NONE;
  uint16_t orientation = ORIENTATION_TOPLEFT;
  std::vector<uint16_t> extra_samples;
  std::vector<uint16_t> cmap_r;
  std::vector<uint16_t> cmap_g;
  std::vector<uint16_t> cmap_b;
  uint16_t subsampling_x = 2;
  uint16_t subsampling_y = 2;
  bool set_subsampling = false;
  bool jpeg_rgb_mode = false;
  const char* open_mode = "wl";
};

// Calls generator(tx, ty, plane, tile_buffer) for each tile in libtiff order.
template <typename TileGen>
void WriteTiledTiff(MemoryTiffStream& stream, const TileSpec& spec,
                    TileGen&& fill_tile) {
  // Uncompressed tiles smaller than 1 KiB round raw strip reads differently
  // unless memory-mapped I/O is enabled on subsequent reads.
  struct MmapOnExit {
    MemoryTiffStream& stream_ref;
    bool enable;
    ~MmapOnExit() {
      if (enable) stream_ref.set_enable_mmap(true);
    }
  } mmap_on_exit{stream, spec.compression == COMPRESSION_NONE};
  UniqueTiff tif = stream.Open(spec.open_mode);
  ASSERT_NE(tif, nullptr);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEWIDTH, spec.width), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGELENGTH, spec.height), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILEWIDTH, spec.tw), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILELENGTH, spec.th), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, spec.bps), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, spec.spp), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, spec.photometric), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, spec.planar), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, spec.compression), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ORIENTATION, spec.orientation), 1);
  if (!spec.extra_samples.empty()) {
    std::vector<uint16_t> mutable_extras = spec.extra_samples;
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_EXTRASAMPLES,
                           static_cast<uint16_t>(mutable_extras.size()),
                           mutable_extras.data()),
              1);
  }
  if (!spec.cmap_r.empty()) {
    std::vector<uint16_t> r = spec.cmap_r;
    std::vector<uint16_t> g = spec.cmap_g;
    std::vector<uint16_t> b = spec.cmap_b;
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_COLORMAP, r.data(), g.data(), b.data()),
        1);
  }
  if (spec.set_subsampling) {
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_YCBCRSUBSAMPLING,
                           spec.subsampling_x, spec.subsampling_y),
              1);
  }
  if (spec.jpeg_rgb_mode) {
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB),
              1);
  }

  const tmsize_t tile_bytes = TIFFTileSize(tif.get());
  ASSERT_GT(tile_bytes, 0);
  const uint16_t planes = (spec.planar == PLANARCONFIG_SEPARATE) ? spec.spp : 1;
  std::vector<uint8_t> tile_buf(static_cast<size_t>(tile_bytes), 0);
  for (uint16_t p = 0; p < planes; ++p) {
    for (uint32_t y = 0; y < spec.height; y += spec.th) {
      for (uint32_t x = 0; x < spec.width; x += spec.tw) {
        std::fill(tile_buf.begin(), tile_buf.end(), 0);
        fill_tile(x, y, p, tile_buf);
        const uint32_t tile_idx = TIFFComputeTile(tif.get(), x, y, 0, p);
        ASSERT_EQ(TIFFWriteEncodedTile(tif.get(), tile_idx, tile_buf.data(),
                                       tile_bytes),
                  tile_bytes);
      }
    }
  }
  ASSERT_EQ(TIFFWriteDirectory(tif.get()), 1);
}

// Builds a single-strip raw ClassicTIFF IFD in memory so invalid/unusual tag
// combinations that TIFFWriteCheck/TIFFSetField block can be tested on read.
std::vector<uint8_t> BuildRawStripIfd(
    uint32_t width, uint32_t height, uint16_t bps, uint16_t spp,
    uint16_t compression, std::optional<uint16_t> photometric, uint16_t planar,
    std::vector<RawIfdEntry> extra_entries = {},
    std::vector<uint8_t> payload = {}) {
  RawTiffBuilder builder(/*big_endian=*/false, /*big_tiff=*/false);
  if (payload.empty()) {
    const size_t row_bytes = ((static_cast<size_t>(width) * bps *
                               (planar == PLANARCONFIG_CONTIG ? spp : 1)) +
                              7U) /
                             8U;
    const size_t nbytes = std::max<size_t>(
        4, row_bytes * height * (planar == PLANARCONFIG_SEPARATE ? spp : 1));
    payload.assign(nbytes, 0x5a);
  }
  const uint64_t data_off = builder.AppendBlob(payload);
  std::vector<RawIfdEntry> entries;
  entries.push_back(builder.MakeLong(TIFFTAG_IMAGEWIDTH, width));
  entries.push_back(builder.MakeLong(TIFFTAG_IMAGELENGTH, height));
  std::vector<uint16_t> bps_arr(spp, bps);
  entries.push_back(builder.MakeShortArray(TIFFTAG_BITSPERSAMPLE, bps_arr));
  entries.push_back(builder.MakeShort(TIFFTAG_COMPRESSION, compression));
  if (photometric.has_value()) {
    entries.push_back(builder.MakeShort(TIFFTAG_PHOTOMETRIC, *photometric));
  }
  entries.push_back(
      builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(data_off)));
  entries.push_back(builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, spp));
  entries.push_back(builder.MakeLong(TIFFTAG_ROWSPERSTRIP, height));
  entries.push_back(builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                                     static_cast<uint32_t>(payload.size())));
  entries.push_back(builder.MakeShort(TIFFTAG_PLANARCONFIG, planar));
  entries.insert(entries.end(), extra_entries.begin(), extra_entries.end());
  builder.AddIfd(std::move(entries));
  return builder.Take();
}

std::vector<uint16_t> MakeRampColormap16(size_t count) {
  std::vector<uint16_t> cmap(count);
  for (size_t i = 0; i < count; ++i) {
    const uint8_t v =
        count > 1 ? static_cast<uint8_t>((i * 255U) / (count - 1U)) : 0;
    cmap[i] = static_cast<uint16_t>((static_cast<uint16_t>(v) << 8) | v);
  }
  return cmap;
}

std::vector<uint8_t> Uint16ToNativeBytes(absl::Span<const uint16_t> vals) {
  std::vector<uint8_t> out(vals.size() * sizeof(uint16_t));
  if (!vals.empty()) {
    std::memcpy(out.data(), vals.data(), out.size());
  }
  return out;
}

TEST(TifGetImageTest, RgbaImageOkAndBeginRejectionsAndDefaults) {
  // Unconfigured compression leaves tif_decodestatus == 0.
  {
    MemoryTiffStream stream(BuildRawStripIfd(4, 4, 8, 1, /*compression=*/65000,
                                             PHOTOMETRIC_MINISBLACK,
                                             PLANARCONFIG_CONTIG));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    tif->tif_decodestatus = 0;
    char emsg[1024] = "";
    EXPECT_EQ(TIFFRGBAImageOK(tif.get(), emsg), 0);
    EXPECT_THAT(emsg, testing::HasSubstr("not configured"));
  }

  // Unsupported bitspersample (12-bit, 32-bit).
  for (uint16_t bad_bps : {uint16_t{12}, uint16_t{32}}) {
    SCOPED_TRACE(testing::Message() << "bad_bps=" << bad_bps);
    MemoryTiffStream stream(BuildRawStripIfd(4, 4, bad_bps, 1, COMPRESSION_NONE,
                                             PHOTOMETRIC_MINISBLACK,
                                             PLANARCONFIG_CONTIG));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    char emsg[1024] = "";
    EXPECT_EQ(TIFFRGBAImageOK(tif.get(), emsg), 0);
    EXPECT_THAT(emsg, testing::HasSubstr("-bit samples"));
  }

  // IEEE floating point samples rejected even for 16-bit.
  {
    RawTiffBuilder b;
    std::vector<uint16_t> fmt = {SAMPLEFORMAT_IEEEFP};
    MemoryTiffStream stream(BuildRawStripIfd(
        4, 4, 16, 1, COMPRESSION_NONE, PHOTOMETRIC_MINISBLACK,
        PLANARCONFIG_CONTIG, {b.MakeShortArray(TIFFTAG_SAMPLEFORMAT, fmt)}));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    char emsg[1024] = "";
    EXPECT_EQ(TIFFRGBAImageOK(tif.get(), emsg), 0);
    EXPECT_THAT(emsg, testing::HasSubstr("IEEE floating-point"));
  }

  // Missing PhotometricInterpretation: 1 color channel defaults to MINISBLACK,
  // or MINISWHITE under CCITT compression; 3 channels defaults to RGB; 2 fails.
  {
    MemoryTiffStream stream(BuildRawStripIfd(
        4, 2, 8, 1, COMPRESSION_NONE, std::nullopt, PLANARCONFIG_CONTIG));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    ScopedRgbaImage img;
    char emsg[1024] = "";
    ASSERT_TRUE(img.Begin(tif.get(), 1, emsg)) << emsg;
    EXPECT_EQ(img.get()->photometric, PHOTOMETRIC_MINISBLACK);
  }
  {
    // CCITT Group 4 1-bit strip written via PackBits/G4: use RawTiffBuilder
    // with COMPRESSION_CCITTFAX4 (all-white 1D/2D minimal payload isn't needed
    // for Begin).
    MemoryTiffStream stream(BuildRawStripIfd(
        8, 2, 1, 1, COMPRESSION_CCITTFAX4, std::nullopt, PLANARCONFIG_CONTIG));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    ScopedRgbaImage img;
    char emsg[1024] = "";
    ASSERT_TRUE(img.Begin(tif.get(), 1, emsg)) << emsg;
    EXPECT_EQ(img.get()->photometric, PHOTOMETRIC_MINISWHITE);
  }
  {
    MemoryTiffStream stream(BuildRawStripIfd(
        4, 2, 8, 3, COMPRESSION_NONE, std::nullopt, PLANARCONFIG_CONTIG));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    TIFFClrFieldBit(tif.get(), FIELD_PHOTOMETRIC);
    tif->tif_dir.td_extrasamples = 0;
    ScopedRgbaImage img;
    char emsg[1024] = "";
    ASSERT_TRUE(img.Begin(tif.get(), 1, emsg)) << emsg;
    EXPECT_EQ(img.get()->photometric, PHOTOMETRIC_RGB);
  }
  {
    MemoryTiffStream stream(BuildRawStripIfd(
        4, 2, 8, 2, COMPRESSION_NONE, std::nullopt, PLANARCONFIG_CONTIG));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    TIFFClrFieldBit(tif.get(), FIELD_PHOTOMETRIC);
    tif->tif_dir.td_extrasamples = 0;
    char emsg[1024] = "";
    EXPECT_EQ(TIFFRGBAImageOK(tif.get(), emsg), 0);
    EXPECT_THAT(emsg, testing::HasSubstr("Missing needed Photometric"));
  }

  // Sub-byte contig with multiple samples/pixel rejected for grayscale/palette.
  {
    MemoryTiffStream stream(BuildRawStripIfd(4, 2, 4, 2, COMPRESSION_NONE,
                                             PHOTOMETRIC_MINISBLACK,
                                             PLANARCONFIG_CONTIG));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    char emsg[1024] = "";
    EXPECT_EQ(TIFFRGBAImageOK(tif.get(), emsg), 0);
    EXPECT_THAT(emsg, testing::HasSubstr("Bits/Sample=4"));
  }

  // RGB with fewer than 3 color channels.
  {
    RawTiffBuilder b;
    std::vector<uint16_t> extras = {EXTRASAMPLE_ASSOCALPHA};
    MemoryTiffStream stream(BuildRawStripIfd(
        4, 2, 8, 3, COMPRESSION_NONE, PHOTOMETRIC_RGB, PLANARCONFIG_CONTIG,
        {b.MakeShortArray(TIFFTAG_EXTRASAMPLES, extras)}));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    char emsg[1024] = "";
    EXPECT_EQ(TIFFRGBAImageOK(tif.get(), emsg), 0);
    EXPECT_THAT(emsg, testing::HasSubstr("Color channels=2"));
  }

  // SEPARATED bad InkSet or insufficient samples.
  {
    RawTiffBuilder b;
    MemoryTiffStream stream(BuildRawStripIfd(
        4, 2, 8, 4, COMPRESSION_NONE, PHOTOMETRIC_SEPARATED,
        PLANARCONFIG_CONTIG, {b.MakeShort(TIFFTAG_INKSET, /*InkSet=*/2)}));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    char emsg[1024] = "";
    EXPECT_EQ(TIFFRGBAImageOK(tif.get(), emsg), 0);
    EXPECT_THAT(emsg, testing::HasSubstr("InkSet=2"));
  }
  {
    MemoryTiffStream stream(BuildRawStripIfd(4, 2, 8, 3, COMPRESSION_NONE,
                                             PHOTOMETRIC_SEPARATED,
                                             PLANARCONFIG_CONTIG));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    char emsg[1024] = "";
    EXPECT_EQ(TIFFRGBAImageOK(tif.get(), emsg), 0);
    EXPECT_THAT(emsg, testing::HasSubstr("Samples/pixel=3"));
  }

  // LOGL wrong compression and Google patch: COMPRESSION_SGILOG with spp != 1.
  {
    MemoryTiffStream stream(BuildRawStripIfd(
        4, 2, 16, 1, COMPRESSION_NONE, PHOTOMETRIC_LOGL, PLANARCONFIG_CONTIG));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    char emsg[1024] = "";
    EXPECT_EQ(TIFFRGBAImageOK(tif.get(), emsg), 0);
    EXPECT_THAT(emsg, testing::HasSubstr("LogL data must have Compression="));
  }
  {
    MemoryTiffStream stream(BuildRawStripIfd(4, 2, 16, 2, COMPRESSION_SGILOG,
                                             PHOTOMETRIC_LOGL,
                                             PLANARCONFIG_CONTIG));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    char emsg[1024] = "";
    EXPECT_EQ(TIFFRGBAImageOK(tif.get(), emsg), 0);
    EXPECT_EQ(std::string_view(emsg),
              "Sorry, can not handle LogL image with Samples/pixel=2");
  }

  // LOGLUV rejections: wrong compression, planar separate, bad
  // spp/colorchannels.
  {
    MemoryTiffStream stream(BuildRawStripIfd(4, 2, 16, 3, COMPRESSION_NONE,
                                             PHOTOMETRIC_LOGLUV,
                                             PLANARCONFIG_CONTIG));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    char emsg[1024] = "";
    EXPECT_EQ(TIFFRGBAImageOK(tif.get(), emsg), 0);
    EXPECT_THAT(emsg, testing::HasSubstr("LogLuv data must have Compression="));
  }
  {
    MemoryTiffStream stream(BuildRawStripIfd(4, 2, 16, 3, COMPRESSION_SGILOG,
                                             PHOTOMETRIC_LOGLUV,
                                             PLANARCONFIG_SEPARATE));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    char emsg[1024] = "";
    EXPECT_EQ(TIFFRGBAImageOK(tif.get(), emsg), 0);
    EXPECT_THAT(emsg, testing::HasSubstr("Planarconfiguration=2"));
  }
  {
    RawTiffBuilder b;
    std::vector<uint16_t> extras = {EXTRASAMPLE_UNASSALPHA};
    MemoryTiffStream stream(BuildRawStripIfd(
        4, 2, 16, 4, COMPRESSION_SGILOG, PHOTOMETRIC_LOGLUV,
        PLANARCONFIG_CONTIG, {b.MakeShortArray(TIFFTAG_EXTRASAMPLES, extras)}));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    char emsg[1024] = "";
    EXPECT_EQ(TIFFRGBAImageOK(tif.get(), emsg), 0);
    EXPECT_THAT(emsg, testing::HasSubstr("Samples/pixel=4, colorchannels=3"));
  }

  // CIELAB rejections: bps != 8/16 and spp != 3 / colorchannels != 3.
  {
    MemoryTiffStream stream(BuildRawStripIfd(
        4, 2, 4, 3, COMPRESSION_NONE, PHOTOMETRIC_CIELAB, PLANARCONFIG_CONTIG));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    char emsg[1024] = "";
    EXPECT_EQ(TIFFRGBAImageOK(tif.get(), emsg), 0);
    EXPECT_THAT(emsg, testing::HasSubstr("Bits/sample=4"));
  }
  {
    RawTiffBuilder b;
    std::vector<uint16_t> extras = {EXTRASAMPLE_ASSOCALPHA};
    MemoryTiffStream stream(BuildRawStripIfd(
        4, 2, 8, 4, COMPRESSION_NONE, PHOTOMETRIC_CIELAB, PLANARCONFIG_CONTIG,
        {b.MakeShortArray(TIFFTAG_EXTRASAMPLES, extras)}));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    char emsg[1024] = "";
    EXPECT_EQ(TIFFRGBAImageOK(tif.get(), emsg), 0);
    EXPECT_THAT(emsg, testing::HasSubstr("Samples/pixel=4"));
  }

  // Unknown photometric interpretation.
  {
    MemoryTiffStream stream(BuildRawStripIfd(
        4, 2, 8, 1, COMPRESSION_NONE, PHOTOMETRIC_MASK, PLANARCONFIG_CONTIG));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    char emsg[1024] = "";
    EXPECT_EQ(TIFFRGBAImageOK(tif.get(), emsg), 0);
    EXPECT_THAT(emsg, testing::HasSubstr("PhotometricInterpretation=4"));
  }

  // Palette missing Colormap passes OK but fails Begin.
  {
    MemoryTiffStream stream(BuildRawStripIfd(4, 2, 8, 1, COMPRESSION_NONE,
                                             PHOTOMETRIC_MINISBLACK,
                                             PLANARCONFIG_CONTIG));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC,
                           uint16_t{PHOTOMETRIC_PALETTE}),
              1);
    char emsg[1024] = "";
    EXPECT_EQ(TIFFRGBAImageOK(tif.get(), emsg), 1);
    ScopedRgbaImage img;
    EXPECT_FALSE(img.Begin(tif.get(), 1, emsg));
    EXPECT_THAT(emsg, testing::HasSubstr("Missing required \"Colormap\" tag"));
  }

  // PickContigCase / PickSeparateCase failures passing OK.
  struct PickFailCase {
    uint32_t w;
    uint32_t h;
    uint16_t bps;
    uint16_t spp;
    uint16_t photometric;
    uint16_t planar;
    bool set_subsampling = false;
    uint16_t sub_x = 1;
    uint16_t sub_y = 1;
    bool zero_white_y = false;
    std::string_view expected_diag;
  };
  const std::vector<PickFailCase> pick_fails = {
      {4, 2, 4, 3, PHOTOMETRIC_RGB, PLANARCONFIG_CONTIG},
      {4, 2, 16, 4, PHOTOMETRIC_SEPARATED, PLANARCONFIG_CONTIG},
      {4, 2, 16, 3, PHOTOMETRIC_YCBCR, PLANARCONFIG_CONTIG},
      {4, 2, 8, 4, PHOTOMETRIC_YCBCR, PLANARCONFIG_CONTIG},
      {2, 2, 8, 3, PHOTOMETRIC_YCBCR, PLANARCONFIG_CONTIG, true, 4, 4, false,
       "incompatible with image size"},
      {4, 4, 8, 3, PHOTOMETRIC_YCBCR, PLANARCONFIG_CONTIG, true, 3, 3},
      {4, 2, 8, 3, PHOTOMETRIC_CIELAB, PLANARCONFIG_CONTIG, false, 1, 1, true,
       "Invalid value for WhitePoint"},
      {4, 2, 8, 2, PHOTOMETRIC_PALETTE, PLANARCONFIG_SEPARATE},
      {4, 2, 4, 2, PHOTOMETRIC_MINISBLACK, PLANARCONFIG_SEPARATE},
      {4, 2, 4, 3, PHOTOMETRIC_RGB, PLANARCONFIG_SEPARATE},
      {4, 2, 8, 5, PHOTOMETRIC_SEPARATED, PLANARCONFIG_SEPARATE},
      {4, 2, 8, 3, PHOTOMETRIC_YCBCR, PLANARCONFIG_SEPARATE, true, 2, 2},
      {4, 2, 16, 3, PHOTOMETRIC_YCBCR, PLANARCONFIG_SEPARATE, true, 1, 1},
  };
  for (size_t i = 0; i < pick_fails.size(); ++i) {
    const auto& tc = pick_fails[i];
    SCOPED_TRACE(testing::Message() << "pick_fail index=" << i);
    RawTiffBuilder b;
    std::vector<RawIfdEntry> extras;
    if (tc.photometric == PHOTOMETRIC_PALETTE) {
      std::vector<uint16_t> pal(256 * 3, 0);
      extras.push_back(b.MakeShortArray(TIFFTAG_COLORMAP, pal));
    }
    if (tc.set_subsampling && tc.sub_x != 3 && tc.sub_y != 3) {
      std::vector<uint16_t> sub = {tc.sub_x, tc.sub_y};
      extras.push_back(b.MakeShortArray(TIFFTAG_YCBCRSUBSAMPLING, sub));
    }
    if (tc.zero_white_y) {
      // Encode WhitePoint as two rationals (3127/10000, 0/1).
      std::vector<uint8_t> wp(16, 0);
      uint32_t vals[4] = {3127, 10000, 0, 1};
      for (int k = 0; k < 4; ++k) {
        wp[k * 4 + 0] = static_cast<uint8_t>(vals[k] & 0xffU);
        wp[k * 4 + 1] = static_cast<uint8_t>((vals[k] >> 8) & 0xffU);
        wp[k * 4 + 2] = static_cast<uint8_t>((vals[k] >> 16) & 0xffU);
        wp[k * 4 + 3] = static_cast<uint8_t>((vals[k] >> 24) & 0xffU);
      }
      extras.push_back(
          b.MakeOutOfLine(TIFFTAG_WHITEPOINT, TIFF_RATIONAL, 2, std::move(wp)));
    }
    const uint16_t open_spp =
        (tc.photometric == PHOTOMETRIC_YCBCR && tc.spp == 4) ? 3 : tc.spp;
    MemoryTiffStream stream(BuildRawStripIfd(tc.w, tc.h, tc.bps, open_spp,
                                             COMPRESSION_NONE, tc.photometric,
                                             tc.planar, std::move(extras)));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    struct RestoreSpp {
      TIFF* t;
      uint16_t s;
      ~RestoreSpp() { t->tif_dir.td_samplesperpixel = s; }
    } restore_spp{tif.get(), open_spp};
    tif->tif_dir.td_samplesperpixel = tc.spp;
    tif->tif_dir.td_photometric = tc.photometric;
    if (tc.set_subsampling) {
      tif->tif_dir.td_ycbcrsubsampling[0] = tc.sub_x;
      tif->tif_dir.td_ycbcrsubsampling[1] = tc.sub_y;
    }
    char emsg[1024] = "";
    ScopedRgbaImage img;
    EXPECT_FALSE(img.Begin(tif.get(), 1, emsg));
    EXPECT_THAT(emsg, testing::HasSubstr("Sorry, can not handle image"));
    if (!tc.expected_diag.empty()) {
      EXPECT_TRUE(
          HasDiagnostic(stream.diagnostics().errors(), tc.expected_diag));
    }
  }
}

TEST(TifGetImageTest, GrayscaleBitDepthsEndiannessBigTiffAndAlpha) {
  struct GrayCase {
    uint16_t bps;
    uint16_t photometric;
    uint32_t width;
    uint32_t height;
    const char* write_mode;
    bool tiled;
  };
  const std::vector<GrayCase> cases = {
      {1, PHOTOMETRIC_MINISBLACK, 17, 4, "wl", false},
      {1, PHOTOMETRIC_MINISWHITE, 15, 3, "wb", false},
      {2, PHOTOMETRIC_MINISBLACK, 13, 3, "wl", false},
      {2, PHOTOMETRIC_MINISWHITE, 14, 3, "wl8", false},
      {4, PHOTOMETRIC_MINISBLACK, 15, 3, "wl", false},
      {4, PHOTOMETRIC_MINISWHITE, 14, 3, "wb", false},
      {8, PHOTOMETRIC_MINISBLACK, 15, 5, "wl", false},
      {8, PHOTOMETRIC_MINISWHITE, 19, 3, "w8l", true},
      {16, PHOTOMETRIC_MINISBLACK, 11, 4, "wl", false},
      {16, PHOTOMETRIC_MINISWHITE, 11, 4, "wb", false},
      {16, PHOTOMETRIC_MINISBLACK, 20, 18, "wl", true},
  };

  for (size_t c = 0; c < cases.size(); ++c) {
    const auto& tc = cases[c];
    SCOPED_TRACE(testing::Message()
                 << "bps=" << tc.bps << " photo=" << tc.photometric
                 << " mode=" << tc.write_mode << " tiled=" << tc.tiled);
    const uint32_t max_sample = (1U << tc.bps) - 1U;
    auto sample_at = [max_sample](uint32_t x, uint32_t y) -> uint32_t {
      return (x * 7U + y * 3U) & max_sample;
    };
    auto expected_gray = [max_sample, photometric = tc.photometric](
                             uint32_t x, uint32_t y, uint16_t bps,
                             auto& sample_fn) -> uint8_t {
      const uint32_t s = sample_fn(x, y);
      if (bps == 16) {
        const uint8_t msb = static_cast<uint8_t>(s >> 8);
        return photometric == PHOTOMETRIC_MINISWHITE ? (255U - msb) : msb;
      }
      const uint32_t raw_byte = (s * 255U) / max_sample;
      return static_cast<uint8_t>(
          photometric == PHOTOMETRIC_MINISWHITE ? (255U - raw_byte) : raw_byte);
    };

    MemoryTiffStream stream;
    if (!tc.tiled) {
      std::vector<uint8_t> raw;
      if (tc.bps == 16) {
        std::vector<uint16_t> words(static_cast<size_t>(tc.width) * tc.height);
        for (uint32_t y = 0; y < tc.height; ++y) {
          for (uint32_t x = 0; x < tc.width; ++x) {
            words[static_cast<size_t>(y) * tc.width + x] =
                static_cast<uint16_t>((sample_at(x, y) * 257U) & 0xffffU);
          }
        }
        raw = Uint16ToNativeBytes(words);
      } else {
        const uint32_t row_bytes = (tc.width * tc.bps + 7U) / 8U;
        raw.assign(static_cast<size_t>(row_bytes) * tc.height, 0);
        for (uint32_t y = 0; y < tc.height; ++y) {
          for (uint32_t x = 0; x < tc.width; ++x) {
            const uint32_t val = sample_at(x, y);
            const uint32_t bit_pos = x * tc.bps;
            const uint32_t byte_idx = y * row_bytes + (bit_pos / 8U);
            const uint32_t shift = 8U - tc.bps - (bit_pos % 8U);
            raw[byte_idx] |= static_cast<uint8_t>(val << shift);
          }
        }
      }
      WriteStripedTiff(stream,
                       StripSpec{
                           .width = tc.width,
                           .height = tc.height,
                           .bps = tc.bps,
                           .spp = 1,
                           .photometric = tc.photometric,
                           .planar = PLANARCONFIG_CONTIG,
                           .compression = COMPRESSION_NONE,
                           .orientation = ORIENTATION_TOPLEFT,
                           .rows_per_strip = 2,
                           .open_mode = tc.write_mode,
                       },
                       raw);
    } else {
      WriteTiledTiff(
          stream,
          TileSpec{
              .width = tc.width,
              .height = tc.height,
              .tw = 16,
              .th = 16,
              .bps = tc.bps,
              .spp = 1,
              .photometric = tc.photometric,
              .planar = PLANARCONFIG_CONTIG,
              .open_mode = tc.write_mode,
          },
          [&](uint32_t tx, uint32_t ty, uint16_t, std::vector<uint8_t>& tile) {
            for (uint32_t ry = 0; ry < 16 && ty + ry < tc.height; ++ry) {
              for (uint32_t rx = 0; rx < 16 && tx + rx < tc.width; ++rx) {
                const uint32_t s = sample_at(tx + rx, ty + ry);
                if (tc.bps == 16) {
                  const uint16_t w =
                      static_cast<uint16_t>((s * 257U) & 0xffffU);
                  std::memcpy(tile.data() + (ry * 16 + rx) * 2, &w, 2);
                } else {
                  tile[ry * 16 + rx] = static_cast<uint8_t>(s);
                }
              }
            }
          });
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint32_t> raster(static_cast<size_t>(tc.width) * tc.height, 0);
    ASSERT_EQ(TIFFReadRGBAImageOriented(reader.get(), tc.width, tc.height,
                                        raster.data(), ORIENTATION_TOPLEFT, 1),
              1);
    for (uint32_t y = 0; y < tc.height; ++y) {
      for (uint32_t x = 0; x < tc.width; ++x) {
        auto s_effective = [&](uint32_t px, uint32_t py) -> uint32_t {
          return tc.bps == 16 ? ((sample_at(px, py) * 257U) & 0xffffU)
                              : sample_at(px, py);
        };
        const uint8_t g = expected_gray(x, y, tc.bps, s_effective);
        EXPECT_EQ(raster[static_cast<size_t>(y) * tc.width + x],
                  PackAbgr(g, g, g, 0xff))
            << "at (" << x << "," << y << ")";
      }
    }
  }

  // Contig 8-bit grayscale + ExtraSamples (putagreytile vs putgreytile).
  {
    // spp=2 + ASSOCALPHA hits putagreytile.
    MemoryTiffStream s_alpha;
    std::vector<uint8_t> pix = {100, 128, 200, 64, 50, 255, 10, 0};
    WriteStripedTiff(s_alpha,
                     StripSpec{
                         .width = 2,
                         .height = 2,
                         .bps = 8,
                         .spp = 2,
                         .photometric = PHOTOMETRIC_MINISBLACK,
                         .extra_samples = {EXTRASAMPLE_ASSOCALPHA},
                     },
                     pix);
    UniqueTiff r_alpha = s_alpha.Open("r");
    ASSERT_NE(r_alpha, nullptr);
    std::array<uint32_t, 4> out{};
    ASSERT_EQ(TIFFReadRGBAImageOriented(r_alpha.get(), 2, 2, out.data(),
                                        ORIENTATION_TOPLEFT, 1),
              1);
    EXPECT_EQ(out[0], PackAbgr(100, 100, 100, 128));
    EXPECT_EQ(out[3], PackAbgr(10, 10, 10, 0));

    // spp=2 + UNSPECIFIED keeps alpha=0 and uses putgreytile.
    MemoryTiffStream s_unspec;
    WriteStripedTiff(s_unspec,
                     StripSpec{
                         .width = 2,
                         .height = 2,
                         .bps = 8,
                         .spp = 2,
                         .photometric = PHOTOMETRIC_MINISBLACK,
                         .extra_samples = {EXTRASAMPLE_UNSPECIFIED},
                     },
                     pix);
    UniqueTiff r_unspec = s_unspec.Open("r");
    ASSERT_NE(r_unspec, nullptr);
    ASSERT_EQ(TIFFReadRGBAImageOriented(r_unspec.get(), 2, 2, out.data(),
                                        ORIENTATION_TOPLEFT, 1),
              1);
    EXPECT_EQ(out[0], PackAbgr(100, 100, 100, 0xff));

    // spp=2 + unknown ExtraSamples value (3) hits default:.
    RawTiffBuilder b;
    std::vector<uint16_t> unspec_extra = {EXTRASAMPLE_UNSPECIFIED};
    MemoryTiffStream s_unk(BuildRawStripIfd(
        2, 2, 8, 2, COMPRESSION_NONE, PHOTOMETRIC_MINISBLACK,
        PLANARCONFIG_CONTIG,
        {b.MakeShortArray(TIFFTAG_EXTRASAMPLES, unspec_extra)}, pix));
    UniqueTiff r_unk = s_unk.Open("r");
    ASSERT_NE(r_unk, nullptr);
    ASSERT_EQ(r_unk->tif_dir.td_extrasamples, 1);
    ASSERT_NE(r_unk->tif_dir.td_sampleinfo, nullptr);
    r_unk->tif_dir.td_sampleinfo[0] = 3;
    ASSERT_EQ(TIFFReadRGBAImageOriented(r_unk.get(), 2, 2, out.data(),
                                        ORIENTATION_TOPLEFT, 1),
              1);
    EXPECT_EQ(out[0], PackAbgr(100, 100, 100, 0xff));
  }

  // Separate grayscale (colorchannels == 1) with and without alpha, strip+tile.
  for (bool tiled : {false, true}) {
    for (uint16_t extra : {EXTRASAMPLE_ASSOCALPHA, EXTRASAMPLE_UNASSALPHA,
                           EXTRASAMPLE_UNSPECIFIED}) {
      SCOPED_TRACE(testing::Message()
                   << "tiled=" << tiled << " extra=" << extra);
      MemoryTiffStream stream;
      if (!tiled) {
        std::vector<uint8_t> pix = {100, 200, 100, 200,  // gray plane (2x2)
                                    128, 255, 64,  0};   // alpha plane (2x2)
        WriteStripedTiff(stream,
                         StripSpec{
                             .width = 2,
                             .height = 2,
                             .bps = 8,
                             .spp = 2,
                             .photometric = PHOTOMETRIC_MINISBLACK,
                             .planar = PLANARCONFIG_SEPARATE,
                             .extra_samples = {extra},
                         },
                         pix);
      } else {
        WriteTiledTiff(
            stream,
            TileSpec{
                .width = 2,
                .height = 2,
                .tw = 16,
                .th = 16,
                .bps = 8,
                .spp = 2,
                .photometric = PHOTOMETRIC_MINISBLACK,
                .planar = PLANARCONFIG_SEPARATE,
                .extra_samples = {extra},
            },
            [](uint32_t, uint32_t, uint16_t plane, std::vector<uint8_t>& tile) {
              tile[0] = (plane == 0) ? 100 : 128;
              tile[1] = (plane == 0) ? 200 : 255;
            });
      }
      UniqueTiff reader = stream.Open("r");
      ASSERT_NE(reader, nullptr);
      std::array<uint32_t, 4> out{};
      ASSERT_EQ(TIFFReadRGBAImageOriented(reader.get(), 2, 2, out.data(),
                                          ORIENTATION_TOPLEFT, 1),
                1);
      if (extra == EXTRASAMPLE_ASSOCALPHA) {
        EXPECT_EQ(out[0], PackAbgr(100, 100, 100, 128));
      } else if (extra == EXTRASAMPLE_UNASSALPHA) {
        EXPECT_EQ(out[0], PackAbgr(50, 50, 50, 128));
      } else {
        EXPECT_EQ(out[0], PackAbgr(100, 100, 100, 0xff));
      }
    }
  }
}

TEST(TifGetImageTest, PaletteContigSubByteEightBitLegacyAndFromSkew) {
  for (uint16_t bps : {uint16_t{1}, uint16_t{2}, uint16_t{4}, uint16_t{8}}) {
    for (bool tiled : {false, true}) {
      SCOPED_TRACE(testing::Message() << "bps=" << bps << " tiled=" << tiled);
      const size_t ncolors = 1ULL << bps;
      std::vector<uint16_t> cr(ncolors), cg(ncolors), cb(ncolors);
      for (size_t i = 0; i < ncolors; ++i) {
        const uint8_t r8 = static_cast<uint8_t>((i * 255U) / (ncolors - 1U));
        const uint8_t g8 = static_cast<uint8_t>(255U - r8);
        const uint8_t b8 = static_cast<uint8_t>((i * 53U) & 0xffU);
        cr[i] = static_cast<uint16_t>((static_cast<uint16_t>(r8) << 8) | r8);
        cg[i] = static_cast<uint16_t>((static_cast<uint16_t>(g8) << 8) | g8);
        cb[i] = static_cast<uint16_t>((static_cast<uint16_t>(b8) << 8) | b8);
      }

      const uint32_t w = tiled ? 18 : 17;
      const uint32_t h = tiled ? 4 : 3;
      auto idx_at = [&](uint32_t x, uint32_t y) -> uint8_t {
        return static_cast<uint8_t>((x + y * 3U) % ncolors);
      };

      MemoryTiffStream stream;
      if (!tiled) {
        const uint32_t row_bytes = (w * bps + 7U) / 8U;
        std::vector<uint8_t> raw(static_cast<size_t>(row_bytes) * h, 0);
        for (uint32_t y = 0; y < h; ++y) {
          for (uint32_t x = 0; x < w; ++x) {
            const uint32_t bit_pos = x * bps;
            const uint32_t byte_idx = y * row_bytes + (bit_pos / 8U);
            const uint32_t shift = 8U - bps - (bit_pos % 8U);
            raw[byte_idx] |= static_cast<uint8_t>(idx_at(x, y) << shift);
          }
        }
        WriteStripedTiff(stream,
                         StripSpec{
                             .width = w,
                             .height = h,
                             .bps = bps,
                             .spp = 1,
                             .photometric = PHOTOMETRIC_PALETTE,
                             .rows_per_strip = 2,
                             .cmap_r = cr,
                             .cmap_g = cg,
                             .cmap_b = cb,
                         },
                         raw);
      } else {
        const uint32_t trow_bytes = (16U * bps + 7U) / 8U;
        WriteTiledTiff(
            stream,
            TileSpec{
                .width = w,
                .height = h,
                .tw = 16,
                .th = 16,
                .bps = bps,
                .spp = 1,
                .photometric = PHOTOMETRIC_PALETTE,
                .cmap_r = cr,
                .cmap_g = cg,
                .cmap_b = cb,
            },
            [&](uint32_t tx, uint32_t ty, uint16_t,
                std::vector<uint8_t>& tile) {
              for (uint32_t ry = 0; ry < 16 && ty + ry < h; ++ry) {
                for (uint32_t rx = 0; rx < 16 && tx + rx < w; ++rx) {
                  const uint32_t bit_pos = rx * bps;
                  const uint32_t byte_idx = ry * trow_bytes + (bit_pos / 8U);
                  const uint32_t shift = 8U - bps - (bit_pos % 8U);
                  tile[byte_idx] |=
                      static_cast<uint8_t>(idx_at(tx + rx, ty + ry) << shift);
                }
              }
            });
      }

      UniqueTiff reader = stream.Open("r");
      ASSERT_NE(reader, nullptr);
      // Read a partial width (fromskew > 0) to cover put*cmaptile fromskew
      // paths.
      const uint32_t sub_w = w - 8U;
      std::vector<uint32_t> sub_raster(static_cast<size_t>(sub_w) * h, 0);
      ASSERT_EQ(
          TIFFReadRGBAImageOriented(reader.get(), sub_w, h, sub_raster.data(),
                                    ORIENTATION_TOPLEFT, 1),
          1);
      for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < sub_w; ++x) {
          const uint8_t idx = idx_at(x, y);
          EXPECT_EQ(sub_raster[static_cast<size_t>(y) * sub_w + x],
                    PackAbgr(static_cast<uint8_t>(cr[idx] >> 8),
                             static_cast<uint8_t>(cg[idx] >> 8),
                             static_cast<uint8_t>(cb[idx] >> 8)));
        }
      }
    }
  }

  // Legacy 8-bit colormap (< 256 in every entry) emits warning and decodes as
  // 8-bit.
  {
    std::vector<uint16_t> cr(256, 0), cg(256, 0), cb(256, 0);
    cr[1] = 200;
    cg[1] = 100;
    cb[1] = 50;
    MemoryTiffStream stream;
    WriteStripedTiff(stream,
                     StripSpec{
                         .width = 2,
                         .height = 1,
                         .bps = 8,
                         .spp = 2,
                         .photometric = PHOTOMETRIC_PALETTE,
                         .extra_samples = {EXTRASAMPLE_UNSPECIFIED},
                         .cmap_r = cr,
                         .cmap_g = cg,
                         .cmap_b = cb,
                     },
                     std::array<uint8_t, 4>{1, 99, 0, 99});
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::array<uint32_t, 2> out{};
    ASSERT_EQ(TIFFReadRGBAImageOriented(reader.get(), 2, 1, out.data(),
                                        ORIENTATION_TOPLEFT, 1),
              1);
    EXPECT_TRUE(HasDiagnostic(stream.diagnostics().warnings(),
                              "Assuming 8-bit colormap"));
    EXPECT_EQ(out[0], PackAbgr(200, 100, 50));
    EXPECT_EQ(out[1], PackAbgr(0, 0, 0));
  }
}

TEST(TifGetImageTest, Rgb8And16BitContigAndSeparateWithAlphaVariants) {
  struct RgbCase {
    uint16_t bps;
    uint16_t spp;
    uint16_t planar;
    bool set_extra;
    uint16_t extra_type;
    bool tiled;
  };
  const std::vector<RgbCase> cases = {
      {8, 3, PLANARCONFIG_CONTIG, false, 0, false},
      {8, 3, PLANARCONFIG_SEPARATE, false, 0, false},
      {8, 3, PLANARCONFIG_CONTIG, false, 0, true},
      {8, 3, PLANARCONFIG_SEPARATE, false, 0, true},
      {8, 4, PLANARCONFIG_CONTIG, false, 0,
       false},  // DEFAULT_EXTRASAMPLE_AS_ALPHA
      {8, 4, PLANARCONFIG_CONTIG, true, EXTRASAMPLE_UNSPECIFIED, false},
      {8, 4, PLANARCONFIG_CONTIG, true, EXTRASAMPLE_ASSOCALPHA, false},
      {8, 4, PLANARCONFIG_CONTIG, true, EXTRASAMPLE_UNASSALPHA, false},
      {8, 5, PLANARCONFIG_CONTIG, true, EXTRASAMPLE_ASSOCALPHA, true},
      {8, 4, PLANARCONFIG_SEPARATE, true, EXTRASAMPLE_ASSOCALPHA, false},
      {8, 4, PLANARCONFIG_SEPARATE, true, EXTRASAMPLE_UNASSALPHA, true},
      {16, 3, PLANARCONFIG_CONTIG, false, 0, false},
      {16, 4, PLANARCONFIG_CONTIG, true, EXTRASAMPLE_ASSOCALPHA, false},
      {16, 4, PLANARCONFIG_CONTIG, true, EXTRASAMPLE_UNASSALPHA, false},
      {16, 3, PLANARCONFIG_SEPARATE, false, 0, true},
      {16, 4, PLANARCONFIG_SEPARATE, true, EXTRASAMPLE_ASSOCALPHA, false},
      {16, 4, PLANARCONFIG_SEPARATE, true, EXTRASAMPLE_UNASSALPHA, true},
  };

  for (size_t idx = 0; idx < cases.size(); ++idx) {
    const auto& tc = cases[idx];
    SCOPED_TRACE(testing::Message()
                 << "case=" << idx << " bps=" << tc.bps << " spp=" << tc.spp
                 << " planar=" << tc.planar << " extra=" << tc.extra_type);
    // Use width >= 15 (7 remainder in UNROLL8) to exercise fast + remainder
    // loops.
    const uint32_t w = tc.tiled ? 18 : 15;
    const uint32_t h = 4;

    auto chan_val = [&](uint32_t x, uint32_t y, uint16_t ch) -> uint8_t {
      return static_cast<uint8_t>((x * 13U + y * 29U + ch * 51U) & 0xffU);
    };

    std::vector<uint16_t> extras;
    if (tc.set_extra) {
      for (uint16_t s = 3; s < tc.spp; ++s) extras.push_back(tc.extra_type);
    }

    MemoryTiffStream stream;
    if (!tc.tiled) {
      std::vector<uint8_t> raw;
      if (tc.planar == PLANARCONFIG_CONTIG) {
        if (tc.bps == 8) {
          raw.resize(static_cast<size_t>(w) * h * tc.spp);
          for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
              for (uint16_t s = 0; s < tc.spp; ++s) {
                raw[(static_cast<size_t>(y) * w + x) * tc.spp + s] =
                    chan_val(x, y, s);
              }
            }
          }
        } else {
          std::vector<uint16_t> words(static_cast<size_t>(w) * h * tc.spp);
          for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
              for (uint16_t s = 0; s < tc.spp; ++s) {
                words[(static_cast<size_t>(y) * w + x) * tc.spp + s] =
                    static_cast<uint16_t>(chan_val(x, y, s) * 257U);
              }
            }
          }
          raw = Uint16ToNativeBytes(words);
        }
      } else {
        if (tc.bps == 8) {
          raw.resize(static_cast<size_t>(w) * h * tc.spp);
          for (uint16_t s = 0; s < tc.spp; ++s) {
            for (uint32_t y = 0; y < h; ++y) {
              for (uint32_t x = 0; x < w; ++x) {
                raw[(static_cast<size_t>(s) * h + y) * w + x] =
                    chan_val(x, y, s);
              }
            }
          }
        } else {
          std::vector<uint16_t> words(static_cast<size_t>(w) * h * tc.spp);
          for (uint16_t s = 0; s < tc.spp; ++s) {
            for (uint32_t y = 0; y < h; ++y) {
              for (uint32_t x = 0; x < w; ++x) {
                words[(static_cast<size_t>(s) * h + y) * w + x] =
                    static_cast<uint16_t>(chan_val(x, y, s) * 257U);
              }
            }
          }
          raw = Uint16ToNativeBytes(words);
        }
      }
      WriteStripedTiff(stream,
                       StripSpec{
                           .width = w,
                           .height = h,
                           .bps = tc.bps,
                           .spp = tc.spp,
                           .photometric = PHOTOMETRIC_RGB,
                           .planar = tc.planar,
                           .rows_per_strip = h,
                           .extra_samples = extras,
                       },
                       raw);
    } else {
      WriteTiledTiff(
          stream,
          TileSpec{
              .width = w,
              .height = h,
              .tw = 16,
              .th = 16,
              .bps = tc.bps,
              .spp = tc.spp,
              .photometric = PHOTOMETRIC_RGB,
              .planar = tc.planar,
              .extra_samples = extras,
          },
          [&](uint32_t tx, uint32_t ty, uint16_t plane,
              std::vector<uint8_t>& tile) {
            const uint32_t bytes_per_sample = tc.bps / 8U;
            for (uint32_t ry = 0; ry < 16 && ty + ry < h; ++ry) {
              for (uint32_t rx = 0; rx < 16 && tx + rx < w; ++rx) {
                if (tc.planar == PLANARCONFIG_CONTIG) {
                  for (uint16_t s = 0; s < tc.spp; ++s) {
                    const uint8_t v = chan_val(tx + rx, ty + ry, s);
                    const size_t pos =
                        ((static_cast<size_t>(ry) * 16 + rx) * tc.spp + s) *
                        bytes_per_sample;
                    if (tc.bps == 8) {
                      tile[pos] = v;
                    } else {
                      const uint16_t w16 = static_cast<uint16_t>(v * 257U);
                      std::memcpy(tile.data() + pos, &w16, 2);
                    }
                  }
                } else {
                  const uint8_t v = chan_val(tx + rx, ty + ry, plane);
                  const size_t pos =
                      (static_cast<size_t>(ry) * 16 + rx) * bytes_per_sample;
                  if (tc.bps == 8) {
                    tile[pos] = v;
                  } else {
                    const uint16_t w16 = static_cast<uint16_t>(v * 257U);
                    std::memcpy(tile.data() + pos, &w16, 2);
                  }
                }
              }
            }
          });
    }

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint32_t> raster(static_cast<size_t>(w) * h, 0);
    ASSERT_EQ(TIFFReadRGBAImageOriented(reader.get(), w, h, raster.data(),
                                        ORIENTATION_TOPLEFT, 1),
              1);
    const bool has_alpha = tc.spp >= 4;
    const bool is_unass =
        tc.set_extra && tc.extra_type == EXTRASAMPLE_UNASSALPHA;
    for (uint32_t y = 0; y < h; ++y) {
      for (uint32_t x = 0; x < w; ++x) {
        uint8_t r = chan_val(x, y, 0);
        uint8_t g = chan_val(x, y, 1);
        uint8_t b = chan_val(x, y, 2);
        uint8_t a = has_alpha ? chan_val(x, y, 3) : 0xff;
        if (is_unass) {
          r = static_cast<uint8_t>((static_cast<uint32_t>(r) * a + 127U) /
                                   255U);
          g = static_cast<uint8_t>((static_cast<uint32_t>(g) * a + 127U) /
                                   255U);
          b = static_cast<uint8_t>((static_cast<uint32_t>(b) * a + 127U) /
                                   255U);
        }
        EXPECT_EQ(raster[static_cast<size_t>(y) * w + x], PackAbgr(r, g, b, a))
            << "at (" << x << "," << y << ")";
      }
    }
  }
}

TEST(TifGetImageTest, SeparatedCmykContigAndSeparate) {
  for (uint16_t planar : {PLANARCONFIG_CONTIG, PLANARCONFIG_SEPARATE}) {
    for (uint16_t spp :
         (planar == PLANARCONFIG_CONTIG ? std::vector<uint16_t>{4, 5}
                                        : std::vector<uint16_t>{4})) {
      for (bool tiled : {false, true}) {
        SCOPED_TRACE(testing::Message() << "planar=" << planar << " spp=" << spp
                                        << " tiled=" << tiled);
        const uint32_t w = tiled ? 17 : 10;
        const uint32_t h = 2;
        auto cmyk_val = [](uint32_t x, uint32_t y, uint16_t s) -> uint8_t {
          return static_cast<uint8_t>((x * 23U + y * 47U + s * 61U) & 0xffU);
        };
        std::vector<uint16_t> extras;
        if (spp == 5) extras.push_back(EXTRASAMPLE_UNSPECIFIED);

        MemoryTiffStream stream;
        if (!tiled) {
          std::vector<uint8_t> raw(static_cast<size_t>(w) * h * spp);
          for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
              for (uint16_t s = 0; s < spp; ++s) {
                const size_t idx =
                    (planar == PLANARCONFIG_CONTIG)
                        ? ((static_cast<size_t>(y) * w + x) * spp + s)
                        : ((static_cast<size_t>(s) * h + y) * w + x);
                raw[idx] = cmyk_val(x, y, s);
              }
            }
          }
          WriteStripedTiff(stream,
                           StripSpec{
                               .width = w,
                               .height = h,
                               .bps = 8,
                               .spp = spp,
                               .photometric = PHOTOMETRIC_SEPARATED,
                               .planar = planar,
                               .rows_per_strip = 2,
                               .extra_samples = extras,
                               .inkset = INKSET_CMYK,
                               .set_inkset = true,
                           },
                           raw);
        } else {
          WriteTiledTiff(
              stream,
              TileSpec{
                  .width = w,
                  .height = h,
                  .tw = 16,
                  .th = 16,
                  .bps = 8,
                  .spp = spp,
                  .photometric = PHOTOMETRIC_SEPARATED,
                  .planar = planar,
                  .extra_samples = extras,
              },
              [&](uint32_t tx, uint32_t ty, uint16_t plane,
                  std::vector<uint8_t>& tile) {
                for (uint32_t ry = 0; ry < 16 && ty + ry < h; ++ry) {
                  for (uint32_t rx = 0; rx < 16 && tx + rx < w; ++rx) {
                    if (planar == PLANARCONFIG_CONTIG) {
                      for (uint16_t s = 0; s < spp; ++s) {
                        tile[(static_cast<size_t>(ry) * 16 + rx) * spp + s] =
                            cmyk_val(tx + rx, ty + ry, s);
                      }
                    } else {
                      tile[static_cast<size_t>(ry) * 16 + rx] =
                          cmyk_val(tx + rx, ty + ry, plane);
                    }
                  }
                }
              });
        }

        UniqueTiff reader = stream.Open("r");
        ASSERT_NE(reader, nullptr);
        std::vector<uint32_t> raster(static_cast<size_t>(w) * h, 0);
        ASSERT_EQ(TIFFReadRGBAImageOriented(reader.get(), w, h, raster.data(),
                                            ORIENTATION_TOPLEFT, 1),
                  1);
        for (uint32_t y = 0; y < h; ++y) {
          for (uint32_t x = 0; x < w; ++x) {
            const uint32_t k = 255U - cmyk_val(x, y, 3);
            const uint8_t r =
                static_cast<uint8_t>((k * (255U - cmyk_val(x, y, 0))) / 255U);
            const uint8_t g =
                static_cast<uint8_t>((k * (255U - cmyk_val(x, y, 1))) / 255U);
            const uint8_t b =
                static_cast<uint8_t>((k * (255U - cmyk_val(x, y, 2))) / 255U);
            EXPECT_EQ(raster[static_cast<size_t>(y) * w + x],
                      PackAbgr(r, g, b));
          }
        }
      }
    }
  }
}

TEST(TifGetImageTest, YCbCrAllSubsamplingsSeparateAndJpeg) {
  // Neutral gray YCbCr blocks: Y=180, Cb=128, Cr=128 decodes near R=G=B=180.
  auto make_ycbcr_blocks = [](uint32_t blocks_hor, uint32_t blocks_ver,
                              uint16_t sub_x, uint16_t sub_y, uint8_t y_base,
                              uint8_t cb, uint8_t cr) -> std::vector<uint8_t> {
    const size_t y_per_block = static_cast<size_t>(sub_x) * sub_y;
    const size_t block_bytes = y_per_block + 2;
    std::vector<uint8_t> out(static_cast<size_t>(blocks_hor) * blocks_ver *
                             block_bytes);
    for (uint32_t by = 0; by < blocks_ver; ++by) {
      for (uint32_t bx = 0; bx < blocks_hor; ++bx) {
        const size_t off =
            (static_cast<size_t>(by) * blocks_hor + bx) * block_bytes;
        for (size_t k = 0; k < y_per_block; ++k) {
          out[off + k] = static_cast<uint8_t>(y_base + ((bx + by + k) & 7U));
        }
        out[off + y_per_block] = cb;
        out[off + y_per_block + 1] = cr;
      }
    }
    return out;
  };

  struct YCbCrCase {
    uint16_t sub_x;
    uint16_t sub_y;
    uint32_t w;
    uint32_t h;
  };
  // Note: for (4,4) we use w >= 5 (2 horizontal blocks = 36 bytes/blockrow,
  // divisible by 4) so TIFFScanlineSize (samplingrow_size / 4 = 9) matches
  // temp * scanline without truncating trailing Cb/Cr bytes.
  const std::vector<YCbCrCase> ycbcr_cases = {
      {4, 4, 8, 8},  // fast path
      {4, 4, 7, 7},  // remainder x=3, h=3
      {4, 4, 6, 5},  // remainder x=2, h=1
      {4, 4, 5, 6},  // remainder x=1, h=2
      {4, 2, 8, 4},  // fast path
      {4, 2, 7, 3},  // remainder x=3, h=1
      {4, 2, 6, 2},  // remainder x=2
      {4, 2, 5, 3},  // remainder x=1
      {4, 1, 7, 2},  // w&3 == 3
      {4, 1, 6, 2},  // w&3 == 2
      {4, 1, 5, 1},  // w&3 == 1
      {2, 2, 5, 3},  // covers h>=2 + h==1 and x>=2 + x==1
      {2, 1, 5, 2},  // covers w>>1 and w&1
      {1, 2, 3, 3},  // covers h>=2 and h==1
      {1, 1, 4, 3},
  };

  for (size_t i = 0; i < ycbcr_cases.size(); ++i) {
    const auto& tc = ycbcr_cases[i];
    SCOPED_TRACE(testing::Message() << "sub=" << tc.sub_x << "x" << tc.sub_y
                                    << " size=" << tc.w << "x" << tc.h);
    const uint32_t bx = (tc.w + tc.sub_x - 1U) / tc.sub_x;
    const uint32_t by = (tc.h + tc.sub_y - 1U) / tc.sub_y;
    const std::vector<uint8_t> raw =
        make_ycbcr_blocks(bx, by, tc.sub_x, tc.sub_y, 180, 128, 128);
    MemoryTiffStream stream;
    WriteStripedTiff(stream,
                     StripSpec{
                         .width = tc.w,
                         .height = tc.h,
                         .bps = 8,
                         .spp = 3,
                         .photometric = PHOTOMETRIC_YCBCR,
                         .planar = PLANARCONFIG_CONTIG,
                         .rows_per_strip = tc.h,
                         .subsampling_x = tc.sub_x,
                         .subsampling_y = tc.sub_y,
                         .set_subsampling = true,
                     },
                     raw);
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint32_t> raster(static_cast<size_t>(tc.w) * tc.h, 0);
    ASSERT_EQ(TIFFReadRGBAImageOriented(reader.get(), tc.w, tc.h, raster.data(),
                                        ORIENTATION_TOPLEFT, 1),
              1);
    for (uint32_t px : raster) {
      EXPECT_GE(TIFFGetR(px), 175);
      EXPECT_LE(TIFFGetR(px), 190);
      EXPECT_EQ(TIFFGetR(px), TIFFGetG(px));
      EXPECT_EQ(TIFFGetG(px), TIFFGetB(px));
      EXPECT_EQ(TIFFGetA(px), 0xff);
    }

    // Sub-width crop (fromskew > 0) for (4,4) on 8x8.
    if (tc.sub_x == 4 && tc.sub_y == 4 && tc.w == 8) {
      std::vector<uint32_t> crop(4 * 8, 0);
      ASSERT_EQ(TIFFReadRGBAImageOriented(reader.get(), 4, 8, crop.data(),
                                          ORIENTATION_TOPLEFT, 1),
                1);
      EXPECT_EQ(TIFFGetA(crop[0]), 0xff);
    }
  }

  // Tiled contig YCbCr (1x1 and 2x2).
  for (uint16_t sub : {uint16_t{1}, uint16_t{2}}) {
    MemoryTiffStream stream;
    const uint32_t bx = 16U / sub;
    const std::vector<uint8_t> tile_data =
        make_ycbcr_blocks(bx, bx, sub, sub, 200, 128, 128);
    WriteTiledTiff(
        stream,
        TileSpec{
            .width = 16,
            .height = 16,
            .tw = 16,
            .th = 16,
            .bps = 8,
            .spp = 3,
            .photometric = PHOTOMETRIC_YCBCR,
            .planar = PLANARCONFIG_CONTIG,
            .subsampling_x = sub,
            .subsampling_y = sub,
            .set_subsampling = true,
        },
        [&](uint32_t, uint32_t, uint16_t, std::vector<uint8_t>& tile) {
          ASSERT_EQ(tile.size(), tile_data.size());
          std::copy(tile_data.begin(), tile_data.end(), tile.begin());
        });
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint32_t> raster(16 * 16, 0);
    ASSERT_EQ(TIFFReadRGBAImageOriented(reader.get(), 16, 16, raster.data(),
                                        ORIENTATION_TOPLEFT, 1),
              1);
    EXPECT_GE(TIFFGetR(raster[0]), 195);
    EXPECT_EQ(TIFFGetR(raster[0]), TIFFGetB(raster[0]));
  }

  // Separate 1x1 YCbCr (both striped and tiled).
  for (bool tiled : {false, true}) {
    MemoryTiffStream stream;
    if (!tiled) {
      std::vector<uint8_t> raw(4 * 4 * 3);
      std::fill_n(raw.begin(), 16, uint8_t{160});
      std::fill_n(raw.begin() + 16, 16, uint8_t{128});
      std::fill_n(raw.begin() + 32, 16, uint8_t{128});
      WriteStripedTiff(stream,
                       StripSpec{
                           .width = 4,
                           .height = 4,
                           .bps = 8,
                           .spp = 3,
                           .photometric = PHOTOMETRIC_YCBCR,
                           .planar = PLANARCONFIG_SEPARATE,
                           .rows_per_strip = 4,
                           .subsampling_x = 1,
                           .subsampling_y = 1,
                           .set_subsampling = true,
                       },
                       raw);
    } else {
      WriteTiledTiff(
          stream,
          TileSpec{
              .width = 4,
              .height = 4,
              .tw = 16,
              .th = 16,
              .bps = 8,
              .spp = 3,
              .photometric = PHOTOMETRIC_YCBCR,
              .planar = PLANARCONFIG_SEPARATE,
              .subsampling_x = 1,
              .subsampling_y = 1,
              .set_subsampling = true,
          },
          [](uint32_t, uint32_t, uint16_t plane, std::vector<uint8_t>& tile) {
            std::fill(tile.begin(), tile.end(), plane == 0 ? 160 : 128);
          });
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::array<uint32_t, 16> out{};
    ASSERT_EQ(TIFFReadRGBAImageOriented(reader.get(), 4, 4, out.data(),
                                        ORIENTATION_TOPLEFT, 1),
              1);
    EXPECT_EQ(out[0], PackAbgr(160, 160, 160));
  }

  // COMPRESSION_JPEG + PHOTOMETRIC_YCBCR (contig strip and tile).
  for (bool tiled : {false, true}) {
    MemoryTiffStream stream;
    if (!tiled) {
      std::vector<uint8_t> rgb(16 * 16 * 3, 210);
      WriteStripedTiff(stream,
                       StripSpec{
                           .width = 16,
                           .height = 16,
                           .bps = 8,
                           .spp = 3,
                           .photometric = PHOTOMETRIC_YCBCR,
                           .planar = PLANARCONFIG_CONTIG,
                           .compression = COMPRESSION_JPEG,
                           .rows_per_strip = 16,
                           .subsampling_x = 2,
                           .subsampling_y = 2,
                           .set_subsampling = true,
                           .jpeg_rgb_mode = true,
                       },
                       rgb);
    } else {
      WriteTiledTiff(
          stream,
          TileSpec{
              .width = 16,
              .height = 16,
              .tw = 16,
              .th = 16,
              .bps = 8,
              .spp = 3,
              .photometric = PHOTOMETRIC_YCBCR,
              .planar = PLANARCONFIG_CONTIG,
              .compression = COMPRESSION_JPEG,
              .subsampling_x = 2,
              .subsampling_y = 2,
              .set_subsampling = true,
              .jpeg_rgb_mode = true,
          },
          [](uint32_t, uint32_t, uint16_t, std::vector<uint8_t>& tile) {
            std::fill(tile.begin(), tile.end(), uint8_t{210});
          });
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint32_t> raster(16 * 16, 0);
    ASSERT_EQ(TIFFReadRGBAImageOriented(reader.get(), 16, 16, raster.data(),
                                        ORIENTATION_TOPLEFT, 1),
              1);
    EXPECT_NEAR(TIFFGetR(raster[0]), 210, 5);
    EXPECT_NEAR(TIFFGetG(raster[0]), 210, 5);
    EXPECT_NEAR(TIFFGetB(raster[0]), 210, 5);
  }

  // Invalid YCbCrCoefficients (luma[1] == 0.0f) and invalid
  // ReferenceBlackWhite.
  {
    MemoryTiffStream stream;
    WriteStripedTiff(stream,
                     StripSpec{
                         .width = 2,
                         .height = 2,
                         .bps = 8,
                         .spp = 3,
                         .photometric = PHOTOMETRIC_YCBCR,
                         .planar = PLANARCONFIG_CONTIG,
                         .rows_per_strip = 2,
                         .subsampling_x = 1,
                         .subsampling_y = 1,
                         .set_subsampling = true,
                         .ycbcr_coeffs = {0.299f, 0.0f, 0.114f},
                         .set_ycbcr_coeffs = true,
                     },
                     std::vector<uint8_t>(12, 128));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    char emsg[1024] = "";
    ScopedRgbaImage img;
    EXPECT_FALSE(img.Begin(reader.get(), 1, emsg));
    EXPECT_TRUE(HasDiagnostic(stream.diagnostics().errors(),
                              "Invalid values for YCbCrCoefficients"));
  }
  {
    MemoryTiffStream stream;
    WriteStripedTiff(
        stream,
        StripSpec{
            .width = 2,
            .height = 2,
            .bps = 8,
            .spp = 3,
            .photometric = PHOTOMETRIC_YCBCR,
            .planar = PLANARCONFIG_CONTIG,
            .rows_per_strip = 2,
            .subsampling_x = 1,
            .subsampling_y = 1,
            .set_subsampling = true,
            .ref_black_white = {0.0f, 255.0f, 128.0f, 255.0f, 128.0f, 1e12f},
            .set_ref_black_white = true,
        },
        std::vector<uint8_t>(12, 128));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    constexpr std::array<float, 6> kBadRbw = {0.0f,   255.0f, 128.0f,
                                              255.0f, 128.0f, 1e12f};
    ASSERT_EQ(
        TIFFSetField(reader.get(), TIFFTAG_REFERENCEBLACKWHITE, kBadRbw.data()),
        1);
    char emsg[1024] = "";
    ScopedRgbaImage img;
    EXPECT_FALSE(img.Begin(reader.get(), 1, emsg));
    EXPECT_TRUE(HasDiagnostic(stream.diagnostics().errors(),
                              "Invalid values for ReferenceBlackWhite"));
  }
}

TEST(TifGetImageTest, CieLab8And16BitAndLogLLogLuv) {
  // 8-bit and 16-bit CIE L*a*b* with neutral white and black pixels.
  {
    MemoryTiffStream s8;
    // Pixel 0: L=255, a=0, b=0 (white); Pixel 1: L=0, a=0, b=0 (black).
    WriteStripedTiff(s8,
                     StripSpec{
                         .width = 2,
                         .height = 1,
                         .bps = 8,
                         .spp = 3,
                         .photometric = PHOTOMETRIC_CIELAB,
                         .rows_per_strip = 1,
                         .white_point = {0.3127f, 0.3290f},
                         .set_white_point = true,
                     },
                     std::array<uint8_t, 6>{255, 0, 0, 0, 0, 0});
    UniqueTiff r8 = s8.Open("r");
    ASSERT_NE(r8, nullptr);
    std::array<uint32_t, 2> out8{};
    ASSERT_EQ(TIFFReadRGBAImageOriented(r8.get(), 2, 1, out8.data(),
                                        ORIENTATION_TOPLEFT, 1),
              1);
    EXPECT_GE(TIFFGetR(out8[0]), 250);
    EXPECT_GE(TIFFGetG(out8[0]), 250);
    EXPECT_GE(TIFFGetB(out8[0]), 250);
    EXPECT_LE(TIFFGetR(out8[1]), 5);
  }
  {
    MemoryTiffStream s16;
    std::vector<uint16_t> words = {65535, 0, 0, 0, 0, 0};
    WriteStripedTiff(s16,
                     StripSpec{
                         .width = 2,
                         .height = 1,
                         .bps = 16,
                         .spp = 3,
                         .photometric = PHOTOMETRIC_CIELAB,
                         .rows_per_strip = 1,
                     },
                     Uint16ToNativeBytes(words));
    UniqueTiff r16 = s16.Open("r");
    ASSERT_NE(r16, nullptr);
    std::array<uint32_t, 2> out16{};
    ASSERT_EQ(TIFFReadRGBAImageOriented(r16.get(), 2, 1, out16.data(),
                                        ORIENTATION_TOPLEFT, 1),
              1);
    EXPECT_GE(TIFFGetR(out16[0]), 250);
    EXPECT_LE(TIFFGetR(out16[1]), 5);
  }

  // PHOTOMETRIC_LOGL with COMPRESSION_SGILOG (strip and tile).
  for (bool tiled : {false, true}) {
    SCOPED_TRACE(testing::Message() << "LogL tiled=" << tiled);
    MemoryTiffStream stream;
    {
      UniqueTiff writer = stream.Open("wl");
      ASSERT_NE(writer, nullptr);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, 16), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, 16), 1);
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, 1), 1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
          1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, COMPRESSION_SGILOG),
          1);
      ASSERT_EQ(
          TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_LOGL), 1);
      if (tiled) {
        ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILEWIDTH, 16), 1);
        ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILELENGTH, 16), 1);
      } else {
        ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, 16), 1);
      }
      ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SGILOGDATAFMT,
                             SGILOGDATAFMT_FLOAT),
                1);
      if (tiled) {
        std::vector<float> tile(16 * 16, 0.25f);
        tile[0] = 1.0f;
        tile[1] = 0.02f;
        ASSERT_EQ(TIFFWriteEncodedTile(writer.get(), 0, tile.data(),
                                       tile.size() * sizeof(float)),
                  static_cast<tmsize_t>(tile.size() * sizeof(float)));
      } else {
        std::vector<float> row(16, 0.25f);
        for (uint32_t y = 0; y < 16; ++y) {
          row[0] = 1.0f;
          row[1] = 0.02f;
          ASSERT_EQ(TIFFWriteScanline(writer.get(), row.data(), y, 0), 1);
        }
      }
      ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
    }
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint32_t> raster(16 * 16, 0);
    ASSERT_EQ(TIFFReadRGBAImageOriented(reader.get(), 16, 16, raster.data(),
                                        ORIENTATION_TOPLEFT, 1),
              1);
    EXPECT_GT(TIFFGetR(raster[0]), TIFFGetR(raster[1]));
    EXPECT_EQ(TIFFGetR(raster[0]), TIFFGetG(raster[0]));
  }

  // PHOTOMETRIC_LOGLUV with COMPRESSION_SGILOG and COMPRESSION_SGILOG24.
  for (uint16_t comp :
       {uint16_t{COMPRESSION_SGILOG}, uint16_t{COMPRESSION_SGILOG24}}) {
    for (bool tiled : {false, true}) {
      SCOPED_TRACE(testing::Message()
                   << "LogLuv comp=" << comp << " tiled=" << tiled);
      MemoryTiffStream stream;
      {
        UniqueTiff writer = stream.Open("wl");
        ASSERT_NE(writer, nullptr);
        ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, 16), 1);
        ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, 16), 1);
        ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, 3), 1);
        ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG,
                               PLANARCONFIG_CONTIG),
                  1);
        ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_COMPRESSION, comp), 1);
        ASSERT_EQ(
            TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_LOGLUV),
            1);
        if (tiled) {
          ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILEWIDTH, 16), 1);
          ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_TILELENGTH, 16), 1);
        } else {
          ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, 16), 1);
        }
        ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SGILOGDATAFMT,
                               SGILOGDATAFMT_FLOAT),
                  1);
        auto fill_xyz = [](std::vector<float>& xyz) {
          for (size_t i = 0; i < xyz.size(); i += 3) {
            xyz[i + 0] = 0.5f;
            xyz[i + 1] = 0.5f;
            xyz[i + 2] = 0.5f;
          }
          // Pixel 0 bright white, Pixel 1 dim dark.
          xyz[0] = 1.0f;
          xyz[1] = 1.0f;
          xyz[2] = 1.0f;
          xyz[3] = 0.02f;
          xyz[4] = 0.02f;
          xyz[5] = 0.02f;
        };
        if (tiled) {
          std::vector<float> tile(16 * 16 * 3);
          fill_xyz(tile);
          ASSERT_EQ(TIFFWriteEncodedTile(writer.get(), 0, tile.data(),
                                         tile.size() * sizeof(float)),
                    static_cast<tmsize_t>(tile.size() * sizeof(float)));
        } else {
          for (uint32_t y = 0; y < 16; ++y) {
            std::vector<float> row(16 * 3);
            fill_xyz(row);
            ASSERT_EQ(TIFFWriteScanline(writer.get(), row.data(), y, 0), 1);
          }
        }
        ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
      }
      UniqueTiff reader = stream.Open("r");
      ASSERT_NE(reader, nullptr);
      std::vector<uint32_t> raster(16 * 16, 0);
      ASSERT_EQ(TIFFReadRGBAImageOriented(reader.get(), 16, 16, raster.data(),
                                          ORIENTATION_TOPLEFT, 1),
                1);
      EXPECT_GT(TIFFGetR(raster[0]), TIFFGetR(raster[1]));
      EXPECT_EQ(TIFFGetA(raster[0]), 0xff);
    }
  }
}

TEST(TifGetImageTest, OrientationMatrixAllFourGetRoutines) {
  constexpr std::array<uint16_t, 8> kAllOrientations = {
      ORIENTATION_TOPLEFT,  ORIENTATION_TOPRIGHT, ORIENTATION_BOTRIGHT,
      ORIENTATION_BOTLEFT,  ORIENTATION_LEFTTOP,  ORIENTATION_RIGHTTOP,
      ORIENTATION_RIGHTBOT, ORIENTATION_LEFTBOT,
  };
  auto is_left = [](uint16_t o) {
    return o == ORIENTATION_TOPLEFT || o == ORIENTATION_LEFTTOP ||
           o == ORIENTATION_BOTLEFT || o == ORIENTATION_LEFTBOT;
  };
  auto is_top = [](uint16_t o) {
    return o == ORIENTATION_TOPLEFT || o == ORIENTATION_LEFTTOP ||
           o == ORIENTATION_TOPRIGHT || o == ORIENTATION_RIGHTTOP;
  };

  for (bool tiled : {false, true}) {
    for (uint16_t planar : {PLANARCONFIG_CONTIG, PLANARCONFIG_SEPARATE}) {
      for (uint16_t img_ori : kAllOrientations) {
        MemoryTiffStream stream;
        const uint32_t w = 4;
        const uint32_t h = 2;
        auto px_r = [](uint32_t x, uint32_t y) -> uint8_t {
          return static_cast<uint8_t>(10 + x * 40 + y * 100);
        };
        if (!tiled) {
          std::vector<uint8_t> raw(w * h * 3);
          for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
              if (planar == PLANARCONFIG_CONTIG) {
                raw[(y * w + x) * 3 + 0] = px_r(x, y);
                raw[(y * w + x) * 3 + 1] = static_cast<uint8_t>(x * 20);
                raw[(y * w + x) * 3 + 2] = static_cast<uint8_t>(y * 80);
              } else {
                raw[0 * w * h + y * w + x] = px_r(x, y);
                raw[1 * w * h + y * w + x] = static_cast<uint8_t>(x * 20);
                raw[2 * w * h + y * w + x] = static_cast<uint8_t>(y * 80);
              }
            }
          }
          WriteStripedTiff(stream,
                           StripSpec{
                               .width = w,
                               .height = h,
                               .bps = 8,
                               .spp = 3,
                               .photometric = PHOTOMETRIC_RGB,
                               .planar = planar,
                               .orientation = img_ori,
                               .rows_per_strip = 1,
                           },
                           raw);
        } else {
          WriteTiledTiff(
              stream,
              TileSpec{
                  .width = w,
                  .height = h,
                  .tw = 16,
                  .th = 16,
                  .bps = 8,
                  .spp = 3,
                  .photometric = PHOTOMETRIC_RGB,
                  .planar = planar,
                  .orientation = img_ori,
              },
              [&](uint32_t, uint32_t, uint16_t plane,
                  std::vector<uint8_t>& tile) {
                for (uint32_t y = 0; y < h; ++y) {
                  for (uint32_t x = 0; x < w; ++x) {
                    if (planar == PLANARCONFIG_CONTIG) {
                      tile[(y * 16 + x) * 3 + 0] = px_r(x, y);
                      tile[(y * 16 + x) * 3 + 1] = static_cast<uint8_t>(x * 20);
                      tile[(y * 16 + x) * 3 + 2] = static_cast<uint8_t>(y * 80);
                    } else if (plane == 0) {
                      tile[y * 16 + x] = px_r(x, y);
                    } else if (plane == 1) {
                      tile[y * 16 + x] = static_cast<uint8_t>(x * 20);
                    } else {
                      tile[y * 16 + x] = static_cast<uint8_t>(y * 80);
                    }
                  }
                }
              });
        }

        UniqueTiff reader = stream.Open("r");
        ASSERT_NE(reader, nullptr);
        for (uint16_t req_ori : kAllOrientations) {
          SCOPED_TRACE(testing::Message()
                       << "tiled=" << tiled << " planar=" << planar
                       << " img_ori=" << img_ori << " req_ori=" << req_ori);
          std::array<uint32_t, 8> out{};
          ASSERT_EQ(TIFFReadRGBAImageOriented(reader.get(), w, h, out.data(),
                                              req_ori, 1),
                    1);
          const bool flip_h = is_left(img_ori) != is_left(req_ori);
          const bool flip_v = is_top(img_ori) != is_top(req_ori);
          const uint32_t dst_x0 = flip_h ? (w - 1U) : 0U;
          const uint32_t dst_y0 = flip_v ? (h - 1U) : 0U;
          EXPECT_EQ(TIFFGetR(out[dst_y0 * w + dst_x0]), px_r(0, 0));
        }
      }
    }
  }
}

TEST(TifGetImageTest, ReadRgbaStripAndTilePartialAndErrors) {
  // Partial / boundary reads with TIFFReadRGBAStrip / TIFFReadRGBAStripExt.
  for (uint16_t planar : {PLANARCONFIG_CONTIG, PLANARCONFIG_SEPARATE}) {
    MemoryTiffStream stream;
    const uint32_t w = 6;
    const uint32_t h = 5;
    std::vector<uint8_t> raw(w * h * 3);
    for (uint32_t y = 0; y < h; ++y) {
      for (uint32_t x = 0; x < w; ++x) {
        if (planar == PLANARCONFIG_CONTIG) {
          raw[(y * w + x) * 3 + 0] = static_cast<uint8_t>(y * 40 + x);
          raw[(y * w + x) * 3 + 1] = 100;
          raw[(y * w + x) * 3 + 2] = 200;
        } else {
          raw[0 * w * h + y * w + x] = static_cast<uint8_t>(y * 40 + x);
          raw[1 * w * h + y * w + x] = 100;
          raw[2 * w * h + y * w + x] = 200;
        }
      }
    }
    WriteStripedTiff(stream,
                     StripSpec{
                         .width = w,
                         .height = h,
                         .bps = 8,
                         .spp = 3,
                         .photometric = PHOTOMETRIC_RGB,
                         .planar = planar,
                         .rows_per_strip = 2,
                     },
                     raw);
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);

    // Strip at row 2 reads rows 2..3 bottom-up (row 3 at raster row 0).
    std::vector<uint32_t> buf(w * 2, 0);
    ASSERT_EQ(TIFFReadRGBAStrip(reader.get(), 2, buf.data()), 1);
    EXPECT_EQ(TIFFGetR(buf[0]), 3 * 40 + 0);
    EXPECT_EQ(TIFFGetR(buf[w]), 2 * 40 + 0);

    // Last partial strip at row 4 reads only row 4 (1 row).
    std::fill(buf.begin(), buf.end(), 0x11223344U);
    ASSERT_EQ(TIFFReadRGBAStripExt(reader.get(), 4, buf.data(), 1), 1);
    EXPECT_EQ(TIFFGetR(buf[0]), 4 * 40 + 0);

    // Error paths for TIFFReadRGBAStripExt:
    EXPECT_EQ(TIFFReadRGBAStripExt(reader.get(), 1, buf.data(), 1),
              0);  // unaligned
    EXPECT_EQ(TIFFReadRGBAStripExt(reader.get(), 6, buf.data(), 1),
              0);  // >= height

    // TIFFReadRGBATile on striped file must fail.
    EXPECT_EQ(TIFFReadRGBATileExt(reader.get(), 0, 0, buf.data(), 1), 0);
  }

  // Multi-tile image (35x21, 16x16 tiles) testing TIFFReadRGBATile, corner
  // padding, and TIFFRGBAImageGet with left/mid/right tile + vertical tile
  // crossing.
  for (uint16_t planar : {PLANARCONFIG_CONTIG, PLANARCONFIG_SEPARATE}) {
    SCOPED_TRACE(testing::Message() << "planar=" << planar);
    const uint32_t w = 35;
    const uint32_t h = 21;
    MemoryTiffStream stream;
    WriteTiledTiff(
        stream,
        TileSpec{
            .width = w,
            .height = h,
            .tw = 16,
            .th = 16,
            .bps = 8,
            .spp = 3,
            .photometric = PHOTOMETRIC_RGB,
            .planar = planar,
        },
        [&](uint32_t tx, uint32_t ty, uint16_t plane,
            std::vector<uint8_t>& tile) {
          for (uint32_t ry = 0; ry < 16 && ty + ry < h; ++ry) {
            for (uint32_t rx = 0; rx < 16 && tx + rx < w; ++rx) {
              const uint32_t gx = tx + rx;
              const uint32_t gy = ty + ry;
              if (planar == PLANARCONFIG_CONTIG) {
                tile[(ry * 16 + rx) * 3 + 0] = static_cast<uint8_t>(gx & 0xffU);
                tile[(ry * 16 + rx) * 3 + 1] = static_cast<uint8_t>(gy & 0xffU);
                tile[(ry * 16 + rx) * 3 + 2] = 0xaa;
              } else if (plane == 0) {
                tile[ry * 16 + rx] = static_cast<uint8_t>(gx & 0xffU);
              } else if (plane == 1) {
                tile[ry * 16 + rx] = static_cast<uint8_t>(gy & 0xffU);
              } else {
                tile[ry * 16 + rx] = 0xaa;
              }
            }
          }
        });

    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    std::vector<uint32_t> tile_out(16 * 16, 0xdeadbeefU);

    // Full interior tile (0, 0).
    ASSERT_EQ(TIFFReadRGBATile(reader.get(), 0, 0, tile_out.data()), 1);
    EXPECT_EQ(TIFFGetR(tile_out[15 * 16 + 0]), 0);
    EXPECT_EQ(TIFFGetG(tile_out[15 * 16 + 0]), 0);

    // Corner edge tile (32, 16) -> 3x5 valid pixels bottom-up, rest
    // zero-padded.
    std::fill(tile_out.begin(), tile_out.end(), 0xdeadbeefU);
    ASSERT_EQ(TIFFReadRGBATileExt(reader.get(), 32, 16, tile_out.data(), 1), 1);
    // Top row of valid region in tile (y=16, which is 5th row from bottom = idx
    // 4 from row=0? Bottom-up of 16 rows: image y=20 is at tile row 16 - 5 =
    // 11, image y=16 is at tile row 15!).
    EXPECT_EQ(tile_out[15 * 16 + 0], PackAbgr(32, 16, 0xaa));
    EXPECT_EQ(tile_out[15 * 16 + 2], PackAbgr(34, 16, 0xaa));
    EXPECT_EQ(tile_out[15 * 16 + 3], 0U);  // Zero-padded past right edge.
    EXPECT_EQ(tile_out[0 * 16 + 0], 0U);   // Zero-padded past bottom edge.

    // Error paths for TIFFReadRGBATileExt:
    EXPECT_EQ(TIFFReadRGBATileExt(reader.get(), 1, 0, tile_out.data(), 1), 0);
    EXPECT_EQ(TIFFReadRGBATileExt(reader.get(), 48, 0, tile_out.data(), 1), 0);
    EXPECT_EQ(TIFFReadRGBAStripExt(reader.get(), 0, tile_out.data(), 1), 0);

    // Windowed TIFFRGBAImageGet crossing left (col 4..15), middle (16..31),
    // and right (32..34) tiles + vertical boundary at y=16.
    ScopedRgbaImage img;
    char emsg[1024] = "";
    ASSERT_TRUE(img.Begin(reader.get(), 1, emsg)) << emsg;
    img.get()->req_orientation = ORIENTATION_TOPLEFT;
    img.get()->row_offset = 3;
    img.get()->col_offset = 4;
    const uint32_t win_w = 34;  // wmin = min(34, 35 - 4) = 31 (cols 4..34)
    const uint32_t win_h = 18;  // rows 3..20
    std::vector<uint32_t> win(win_w * win_h, 0);
    ASSERT_EQ(img.Get(win.data(), win_w, win_h), 1);
    EXPECT_EQ(win[0 * win_w + 0], PackAbgr(4, 3, 0xaa));
    EXPECT_EQ(win[0 * win_w + 12], PackAbgr(16, 3, 0xaa));
    EXPECT_EQ(win[13 * win_w + 30], PackAbgr(34, 16, 0xaa));
    EXPECT_EQ(win[13 * win_w + 31], 0U);  // Untouched right margin.

    // Taller raster (h > hx): bottom-aligned in raster.
    img.get()->row_offset = 0;
    img.get()->col_offset = 0;
    std::vector<uint32_t> tall(w * (h + 3), 0);
    ASSERT_EQ(img.Get(tall.data(), w, h + 3), 1);
    EXPECT_EQ(tall[0], 0U);
    EXPECT_EQ(tall[3 * w + 0], PackAbgr(0, 0, 0xaa));

    // Invalid row_offset and col_offset errors.
    img.get()->row_offset = -1;
    EXPECT_EQ(img.Get(win.data(), 1, 1), 0);
    img.get()->row_offset = static_cast<int>(h);
    EXPECT_EQ(img.Get(win.data(), 1, 1), 0);
    img.get()->row_offset = 0;
    img.get()->col_offset = static_cast<int>(w);
    EXPECT_EQ(img.Get(win.data(), 1, 1), 0);
    img.get()->col_offset = -1;
    EXPECT_EQ(img.Get(win.data(), 1, 1), 0);
    img.get()->col_offset = 0;

    // Width / skew overflow checks without allocating huge raster.
    uint32_t dummy_px = 0;
    img.get()->req_orientation = ORIENTATION_BOTLEFT;  // FLIP_VERTICALLY
    EXPECT_EQ(img.Get(&dummy_px, 0x7ffffff8U, 1), 0);
    img.get()->req_orientation = ORIENTATION_TOPLEFT;  // !FLIP_VERTICALLY
    EXPECT_EQ(img.Get(&dummy_px, 0x80000100U, 1), 0);
    // Invalid skew overflow: tw=16, w = INT_MAX + 16 => toskew = INT_MAX,
    // leftmost_fromskew = 2 => skew_i64 = INT_MAX + 2 > INT_MAX.
    img.get()->col_offset = 2;
    EXPECT_EQ(img.Get(&dummy_px, 0x8000000fU, 1), 0);
    img.get()->col_offset = 0;

    // Null get / put pointers in TIFFRGBAImageGet.
    auto saved_put = img.get()->put;
    img.get()->put.any = nullptr;
    EXPECT_EQ(img.Get(&dummy_px, 1, 1), 0);
    img.get()->put = saved_put;
    img.get()->get = nullptr;
    EXPECT_EQ(img.Get(&dummy_px, 1, 1), 0);
  }
}

TEST(TifGetImageTest, StripAndTileZeroDimsYCbCrSubZeroAndWidthOverflow) {
  // rowsperstrip == 0 in TIFFReadRGBAStripExt, gtStripContig, gtStripSeparate.
  {
    RawTiffBuilder zero_rps;
    std::vector<uint8_t> payload(24, 0x42);
    const uint64_t off = zero_rps.AppendBlob(payload);
    zero_rps.AddIfd({
        zero_rps.MakeLong(TIFFTAG_IMAGEWIDTH, 4),
        zero_rps.MakeLong(TIFFTAG_IMAGELENGTH, 2),
        zero_rps.MakeShortArray(TIFFTAG_BITSPERSAMPLE,
                                std::array<uint16_t, 3>{8, 8, 8}),
        zero_rps.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        zero_rps.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB),
        zero_rps.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(off)),
        zero_rps.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 3),
        zero_rps.MakeLong(TIFFTAG_ROWSPERSTRIP, 2),
        zero_rps.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                          static_cast<uint32_t>(payload.size())),
        zero_rps.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
    });
    MemoryTiffStream stream(zero_rps.Take());
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    const uint32_t orig_rps = tif->tif_dir.td_rowsperstrip;
    struct RestoreRps {
      TIFF* t;
      uint32_t v;
      ~RestoreRps() { t->tif_dir.td_rowsperstrip = v; }
    } restore_rps{tif.get(), orig_rps};
    tif->tif_dir.td_rowsperstrip = 0;
    std::array<uint32_t, 8> out{};
    EXPECT_EQ(TIFFReadRGBAStripExt(tif.get(), 0, out.data(), 1), 0);
    EXPECT_EQ(TIFFReadRGBAImage(tif.get(), 4, 2, out.data(), 1), 0);
    EXPECT_TRUE(
        HasDiagnostic(stream.diagnostics().errors(), "rowsperstrip is zero"));
  }

  // subsamplingver == 0 in gtStripContig and width overflow in gtStripContig /
  // gtStripSeparate, plus col_offset out of range.
  for (uint16_t planar : {PLANARCONFIG_CONTIG, PLANARCONFIG_SEPARATE}) {
    RawTiffBuilder b;
    MemoryTiffStream stream(BuildRawStripIfd(4, 2, 8, 3, COMPRESSION_NONE,
                                             PHOTOMETRIC_RGB, planar));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    ScopedRgbaImage img;
    char emsg[1024] = "";
    ASSERT_TRUE(img.Begin(tif.get(), 1, emsg)) << emsg;
    uint32_t dummy = 0;
    img.get()->col_offset = 4;
    EXPECT_EQ(img.Get(&dummy, 1, 1), 0);
    img.get()->col_offset = 0;
    img.get()->req_orientation = ORIENTATION_BOTLEFT;  // FLIP_VERTICALLY
    EXPECT_EQ(img.Get(&dummy, 0x40000001U, 1), 0);
    EXPECT_TRUE(HasDiagnostic(stream.diagnostics().errors(), "Width overflow"));
  }
  {
    RawTiffBuilder b;
    std::vector<uint16_t> good_sub = {2, 2};
    MemoryTiffStream stream(BuildRawStripIfd(
        4, 4, 8, 3, COMPRESSION_NONE, PHOTOMETRIC_YCBCR, PLANARCONFIG_CONTIG,
        {b.MakeShortArray(TIFFTAG_YCBCRSUBSAMPLING, good_sub)}));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    ScopedRgbaImage img;
    char emsg[1024] = "";
    ASSERT_TRUE(img.Begin(tif.get(), 1, emsg)) << emsg;
    struct RestoreSub {
      TIFF* t;
      ~RestoreSub() { t->tif_dir.td_ycbcrsubsampling[1] = 2; }
    } restore_sub{tif.get()};
    tif->tif_dir.td_ycbcrsubsampling[1] = 0;
    std::array<uint32_t, 16> out{};
    EXPECT_EQ(img.Get(out.data(), 4, 4), 0);

    RawTiffBuilder zt;
    std::vector<uint8_t> payload(256, 0);
    const uint64_t off = zt.AppendBlob(payload);
    zt.AddIfd({
        zt.MakeLong(TIFFTAG_IMAGEWIDTH, 4),
        zt.MakeLong(TIFFTAG_IMAGELENGTH, 4),
        zt.MakeShort(TIFFTAG_BITSPERSAMPLE, 8),
        zt.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
        zt.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK),
        zt.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 1),
        zt.MakeShort(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        zt.MakeLong(TIFFTAG_TILEWIDTH, 16),
        zt.MakeLong(TIFFTAG_TILELENGTH, 16),
        zt.MakeLong(TIFFTAG_TILEOFFSETS, static_cast<uint32_t>(off)),
        zt.MakeLong(TIFFTAG_TILEBYTECOUNTS, 256),
    });
    MemoryTiffStream zstream(zt.Take());
    UniqueTiff ztif = zstream.Open("r");
    ASSERT_NE(ztif, nullptr);
    struct RestoreTw {
      TIFF* t;
      ~RestoreTw() { t->tif_dir.td_tilewidth = 16; }
    } restore_tw{ztif.get()};
    ztif->tif_dir.td_tilewidth = 0;
    std::array<uint32_t, 256> tile_out{};
    EXPECT_EQ(TIFFReadRGBATileExt(ztif.get(), 0, 0, tile_out.data(), 1), 0);
    EXPECT_EQ(TIFFReadRGBAImage(ztif.get(), 4, 4, tile_out.data(), 1), 0);
  }
}

TEST(TifGetImageTest, StopOnErrorVsContinueAndIoFailures) {
  // Corrupt compressed strip/tile 1 (or separate planes) after strip/tile 0
  // succeeds so buf != NULL: stop=1 returns 0 while stop=0 continues and
  // returns 1.
  auto corrupt_strile = [](MemoryTiffStream& stream, TIFF* tif, uint32_t strile,
                           bool is_tiled) {
    uint64_t* offsets = nullptr;
    uint64_t* counts = nullptr;
    if (is_tiled) {
      ASSERT_EQ(TIFFGetField(tif, TIFFTAG_TILEOFFSETS, &offsets), 1);
      ASSERT_EQ(TIFFGetField(tif, TIFFTAG_TILEBYTECOUNTS, &counts), 1);
    } else {
      ASSERT_EQ(TIFFGetField(tif, TIFFTAG_STRIPOFFSETS, &offsets), 1);
      ASSERT_EQ(TIFFGetField(tif, TIFFTAG_STRIPBYTECOUNTS, &counts), 1);
    }
    ASSERT_GT(counts[strile], 0U);
    auto& bytes = stream.mutable_data();
    ASSERT_LE(offsets[strile] + counts[strile], bytes.size());
    std::fill_n(bytes.begin() + offsets[strile], counts[strile], uint8_t{0xff});
  };

  for (uint16_t planar : {PLANARCONFIG_CONTIG, PLANARCONFIG_SEPARATE}) {
    // 2 strips of 2 rows each with COMPRESSION_ADOBE_DEFLATE.
    const uint32_t spp = 4;
    const uint32_t w = 4;
    const uint32_t h = 4;
    std::vector<uint8_t> raw(w * h * spp, 0x80);
    MemoryTiffStream base_stream;
    WriteStripedTiff(base_stream,
                     StripSpec{
                         .width = w,
                         .height = h,
                         .bps = 8,
                         .spp = spp,
                         .photometric = PHOTOMETRIC_RGB,
                         .planar = planar,
                         .compression = COMPRESSION_ADOBE_DEFLATE,
                         .rows_per_strip = 2,
                         .extra_samples = {EXTRASAMPLE_ASSOCALPHA},
                     },
                     raw);

    // Strip layout: for separate, 4 planes x 2 strips = 8 strips.
    // Corrupt strip index in second half to exercise each separate plane
    // branch!
    const std::vector<uint32_t> targets =
        (planar == PLANARCONFIG_CONTIG) ? std::vector<uint32_t>{1}
                                        : std::vector<uint32_t>{1, 3, 5, 7};
    for (uint32_t corrupt_idx : targets) {
      SCOPED_TRACE(testing::Message()
                   << "strip planar=" << planar << " idx=" << corrupt_idx);
      MemoryTiffStream s(base_stream.data());
      {
        UniqueTiff probe = s.Open("r");
        ASSERT_NE(probe, nullptr);
        corrupt_strile(s, probe.get(), corrupt_idx, false);
      }
      {
        UniqueTiff reader = s.Open("r");
        ASSERT_NE(reader, nullptr);
        std::vector<uint32_t> out(w * h, 0);
        EXPECT_EQ(TIFFReadRGBAImage(reader.get(), w, h, out.data(), /*stop=*/1),
                  0);
      }
      {
        UniqueTiff reader = s.Open("r");
        ASSERT_NE(reader, nullptr);
        std::vector<uint32_t> out(w * h, 0);
        EXPECT_EQ(TIFFReadRGBAImage(reader.get(), w, h, out.data(), /*stop=*/0),
                  1);
      }
    }
  }

  for (uint16_t planar : {PLANARCONFIG_CONTIG, PLANARCONFIG_SEPARATE}) {
    const uint32_t w = 32;
    const uint32_t h = 16;
    const uint16_t spp = 4;
    MemoryTiffStream base_stream;
    WriteTiledTiff(
        base_stream,
        TileSpec{
            .width = w,
            .height = h,
            .tw = 16,
            .th = 16,
            .bps = 8,
            .spp = spp,
            .photometric = PHOTOMETRIC_RGB,
            .planar = planar,
            .compression = COMPRESSION_ADOBE_DEFLATE,
            .extra_samples = {EXTRASAMPLE_ASSOCALPHA},
        },
        [](uint32_t, uint32_t, uint16_t, std::vector<uint8_t>& tile) {
          std::fill(tile.begin(), tile.end(), uint8_t{0x90});
        });

    // Tiles per plane = 2: corrupt tile 1 in plane 0..3.
    const std::vector<uint32_t> targets =
        (planar == PLANARCONFIG_CONTIG) ? std::vector<uint32_t>{1}
                                        : std::vector<uint32_t>{1, 3, 5, 7};
    for (uint32_t corrupt_idx : targets) {
      SCOPED_TRACE(testing::Message()
                   << "tile planar=" << planar << " idx=" << corrupt_idx);
      MemoryTiffStream s(base_stream.data());
      {
        UniqueTiff probe = s.Open("r");
        ASSERT_NE(probe, nullptr);
        corrupt_strile(s, probe.get(), corrupt_idx, true);
      }
      {
        UniqueTiff reader = s.Open("r");
        ASSERT_NE(reader, nullptr);
        std::vector<uint32_t> out(w * h, 0);
        EXPECT_EQ(TIFFReadRGBAImage(reader.get(), w, h, out.data(), /*stop=*/1),
                  0);
      }
      {
        UniqueTiff reader = s.Open("r");
        ASSERT_NE(reader, nullptr);
        std::vector<uint32_t> out(w * h, 0);
        EXPECT_EQ(TIFFReadRGBAImage(reader.get(), w, h, out.data(), /*stop=*/0),
                  1);
      }
    }
  }

  // First strile I/O failure (buf == NULL) fails even with stop == 0.
  {
    MemoryTiffStream stream;
    WriteStripedTiff(stream,
                     StripSpec{
                         .width = 4,
                         .height = 4,
                         .bps = 8,
                         .spp = 3,
                         .photometric = PHOTOMETRIC_RGB,
                         .rows_per_strip = 4,
                     },
                     std::vector<uint8_t>(48, 0x55));
    UniqueTiff reader = stream.Open("r");
    ASSERT_NE(reader, nullptr);
    stream.FailReadsAfter(0);
    std::vector<uint32_t> out(16, 0);
    EXPECT_EQ(TIFFReadRGBAImage(reader.get(), 4, 4, out.data(), /*stop=*/0), 0);
  }
}

TEST(TifGetImageTest, AllocationLimitOomPaths) {
  constexpr tmsize_t kInitialBudget = 1 << 24;

  // Palette: redcmap / greencmap / bluecmap OOM and makecmap OOM.
  {
    MemoryTiffStream stream;
    std::vector<uint16_t> ramp = MakeRampColormap16(256);
    WriteStripedTiff(stream,
                     StripSpec{
                         .width = 4,
                         .height = 4,
                         .bps = 8,
                         .spp = 1,
                         .photometric = PHOTOMETRIC_PALETTE,
                         .rows_per_strip = 4,
                         .cmap_r = ramp,
                         .cmap_g = ramp,
                         .cmap_b = ramp,
                     },
                     std::vector<uint8_t>(16, 1));
    // Fail redcmap (0), greencmap (512), bluecmap (1024), makecmap (1536).
    for (tmsize_t budget :
         {tmsize_t{0}, tmsize_t{512}, tmsize_t{1024}, tmsize_t{1536}}) {
      SCOPED_TRACE(testing::Message() << "palette budget=" << budget);
      UniqueTiff tif = OpenWithCumulatedLimit(stream, "r", kInitialBudget);
      ASSERT_NE(tif, nullptr);
      ScopedCumulatedMemAllocRemaining guard(tif.get(), budget);
      ScopedRgbaImage img;
      char emsg[1024] = "";
      EXPECT_FALSE(img.Begin(tif.get(), 1, emsg));
    }
  }

  // MINISBLACK 8-bit: setupMap OOM (0) and makebwmap OOM after Map alloc (256).
  {
    MemoryTiffStream stream;
    WriteStripedTiff(stream,
                     StripSpec{
                         .width = 4,
                         .height = 4,
                         .bps = 8,
                         .spp = 1,
                         .photometric = PHOTOMETRIC_MINISBLACK,
                         .rows_per_strip = 4,
                     },
                     std::vector<uint8_t>(16, 100));
    for (tmsize_t budget : {tmsize_t{0}, tmsize_t{256}}) {
      SCOPED_TRACE(testing::Message() << "minisblack budget=" << budget);
      UniqueTiff tif = OpenWithCumulatedLimit(stream, "r", kInitialBudget);
      ASSERT_NE(tif, nullptr);
      ScopedCumulatedMemAllocRemaining guard(tif.get(), budget);
      ScopedRgbaImage img;
      char emsg[1024] = "";
      EXPECT_FALSE(img.Begin(tif.get(), 1, emsg));
    }
  }

  // BuildMapUaToAa (65536 bytes) and BuildMapBitdepth16To8 (65536 bytes) OOMs.
  {
    MemoryTiffStream stream;
    WriteStripedTiff(stream,
                     StripSpec{
                         .width = 2,
                         .height = 2,
                         .bps = 16,
                         .spp = 4,
                         .photometric = PHOTOMETRIC_RGB,
                         .rows_per_strip = 2,
                         .extra_samples = {EXTRASAMPLE_UNASSALPHA},
                     },
                     std::vector<uint8_t>(2 * 2 * 4 * 2, 0));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    ScopedSingleMemAllocLimit single_guard(tif.get(), 1024);
    ScopedRgbaImage img;
    char emsg[1024] = "";
    EXPECT_FALSE(img.Begin(tif.get(), 1, emsg));
    EXPECT_TRUE(HasDiagnostic(stream.diagnostics().errors(), "Out of memory"));
  }
  {
    MemoryTiffStream stream;
    WriteStripedTiff(stream,
                     StripSpec{
                         .width = 2,
                         .height = 2,
                         .bps = 8,
                         .spp = 4,
                         .photometric = PHOTOMETRIC_RGB,
                         .rows_per_strip = 2,
                         .extra_samples = {EXTRASAMPLE_UNASSALPHA},
                     },
                     std::vector<uint8_t>(2 * 2 * 4, 0));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    ScopedSingleMemAllocLimit single_guard(tif.get(), 1024);
    ScopedRgbaImage img;
    char emsg[1024] = "";
    EXPECT_FALSE(img.Begin(tif.get(), 1, emsg));
    EXPECT_TRUE(HasDiagnostic(stream.diagnostics().errors(), "Out of memory"));
  }

  // initYCbCrConversion OOM and initCIELabConversion OOM.
  {
    MemoryTiffStream stream;
    WriteStripedTiff(stream,
                     StripSpec{
                         .width = 2,
                         .height = 2,
                         .bps = 8,
                         .spp = 3,
                         .photometric = PHOTOMETRIC_YCBCR,
                         .rows_per_strip = 2,
                         .subsampling_x = 1,
                         .subsampling_y = 1,
                         .set_subsampling = true,
                     },
                     std::vector<uint8_t>(12, 128));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    ScopedSingleMemAllocLimit single_guard(tif.get(), 512);
    ScopedRgbaImage img;
    char emsg[1024] = "";
    EXPECT_FALSE(img.Begin(tif.get(), 1, emsg));
    EXPECT_TRUE(HasDiagnostic(stream.diagnostics().errors(),
                              "No space for YCbCr->RGB"));
  }
  {
    MemoryTiffStream stream;
    WriteStripedTiff(stream,
                     StripSpec{
                         .width = 2,
                         .height = 2,
                         .bps = 8,
                         .spp = 3,
                         .photometric = PHOTOMETRIC_CIELAB,
                         .rows_per_strip = 2,
                     },
                     std::vector<uint8_t>(12, 128));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    ScopedSingleMemAllocLimit single_guard(tif.get(), 512);
    ScopedRgbaImage img;
    char emsg[1024] = "";
    EXPECT_FALSE(img.Begin(tif.get(), 1, emsg));
    EXPECT_TRUE(HasDiagnostic(stream.diagnostics().errors(),
                              "No space for CIE L*a*b*->RGB"));
  }

  // OOM during gtStripContig / gtStripSeparate / gtTileContig / gtTileSeparate
  // strip/tile buffer allocation.
  for (bool tiled : {false, true}) {
    for (uint16_t planar : {PLANARCONFIG_CONTIG, PLANARCONFIG_SEPARATE}) {
      MemoryTiffStream stream;
      if (!tiled) {
        WriteStripedTiff(stream,
                         StripSpec{
                             .width = 8,
                             .height = 8,
                             .bps = 8,
                             .spp = 3,
                             .photometric = PHOTOMETRIC_RGB,
                             .planar = planar,
                             .rows_per_strip = 8,
                         },
                         std::vector<uint8_t>(8 * 8 * 3, 10));
      } else {
        WriteTiledTiff(
            stream,
            TileSpec{
                .width = 16,
                .height = 16,
                .tw = 16,
                .th = 16,
                .bps = 8,
                .spp = 3,
                .photometric = PHOTOMETRIC_RGB,
                .planar = planar,
            },
            [](uint32_t, uint32_t, uint16_t, std::vector<uint8_t>&) {});
      }
      UniqueTiff tif = OpenWithCumulatedLimit(stream, "r", kInitialBudget);
      ASSERT_NE(tif, nullptr);
      ScopedRgbaImage img;
      char emsg[1024] = "";
      ASSERT_TRUE(img.Begin(tif.get(), 1, emsg)) << emsg;
      ScopedCumulatedMemAllocRemaining guard(tif.get(), 0);
      std::vector<uint32_t> out(16 * 16, 0);
      EXPECT_EQ(img.Get(out.data(), img.get()->width, img.get()->height), 0);
    }
  }
}

TEST(TifGetImageTest, MmapFileReadRgbaRoundTrip) {
  TempFile file("getimage_mmap.tif");
  DiagnosticRecorder diag;
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  diag.Install(opts.get());

  {
    UniqueTiff writer(TIFFOpenExt(file.c_str(), "wl", opts.get()));
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGEWIDTH, 4), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_IMAGELENGTH, 4), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_BITSPERSAMPLE, 8), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_SAMPLESPERPIXEL, 3), 1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB),
              1);
    ASSERT_EQ(
        TIFFSetField(writer.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG),
        1);
    ASSERT_EQ(TIFFSetField(writer.get(), TIFFTAG_ROWSPERSTRIP, 2), 1);
    std::vector<uint8_t> row(4 * 3, 77);
    for (uint32_t y = 0; y < 4; ++y) {
      row[0] = static_cast<uint8_t>(10 + y);
      ASSERT_EQ(TIFFWriteScanline(writer.get(), row.data(), y, 0), 1);
    }
    ASSERT_EQ(TIFFWriteDirectory(writer.get()), 1);
  }

  UniqueTiff reader(TIFFOpenExt(file.c_str(), "rm", opts.get()));
  ASSERT_NE(reader, nullptr);
  std::array<uint32_t, 16> out{};
  ASSERT_EQ(TIFFReadRGBAImageOriented(reader.get(), 4, 4, out.data(),
                                      ORIENTATION_TOPLEFT, 1),
            1);
  EXPECT_EQ(out[0], PackAbgr(10, 77, 77));
  EXPECT_EQ(out[3 * 4], PackAbgr(13, 77, 77));
  EXPECT_EQ(diag.error_count(), 0U);
}

}  // namespace
}  // namespace libtiff
