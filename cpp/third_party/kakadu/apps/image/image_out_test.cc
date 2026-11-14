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

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT(build/c++17)
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/apps/compressed_io/jp2.h"
#include "third_party/kakadu/vendor/apps/compressed_io/jpx.h"
#include "third_party/kakadu/vendor/apps/compressed_io/kdu_file_io.h"
#include "third_party/kakadu/vendor/apps/image/kdu_image.h"
#include "third_party/kakadu/vendor/apps/image/kdu_tiff.h"
#include "third_party/kakadu/vendor/apps/support/kdu_stripe_compressor.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_elementary.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_params.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_sample_processing.h"

namespace kdu_supp {
namespace {

using ::kdu_core::kdu_byte;
using ::kdu_core::kdu_codestream;
using ::kdu_core::kdu_compressed_source;
using ::kdu_core::kdu_compressed_target;
using ::kdu_core::kdu_coords;
using ::kdu_core::kdu_dims;
using ::kdu_core::kdu_exception;
using ::kdu_core::kdu_int16;
using ::kdu_core::kdu_int32;
using ::kdu_core::kdu_int64;
using ::kdu_core::kdu_line_buf;
using ::kdu_core::kdu_long;
using ::kdu_core::kdu_sample16;
using ::kdu_core::kdu_sample32;
using ::kdu_core::kdu_sample64;
using ::kdu_core::kdu_sample_allocator;
using ::kdu_core::kdu_thread_safe_message;
using ::kdu_core::kdu_uint16;
using ::kdu_core::kdu_uint32;
using ::kdu_core::siz_params;

class CapturingMessageHandler final : public kdu_thread_safe_message {
 public:
  explicit CapturingMessageHandler(bool throw_on_flush)
      : throw_on_flush_(throw_on_flush) {}

  void put_text(const char* text) override { last_message_ += text; }

  void flush(bool end_of_message = false) override {
    if (end_of_message) {
      ++flush_count_;
      kdu_thread_safe_message::flush(end_of_message);
      if (throw_on_flush_) {
        last_message_.clear();
        throw KDU_ERROR_EXCEPTION;
      }
      last_message_.clear();
      return;
    }
    kdu_thread_safe_message::flush(end_of_message);
  }

  int flush_count() const { return flush_count_; }

 private:
  bool throw_on_flush_;
  std::string last_message_;
  int flush_count_ = 0;
};

CapturingMessageHandler*& WarningHandlerPtr() {
  static CapturingMessageHandler* ptr = nullptr;
  return ptr;
}

class ImageOutTestEnv final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto* const err_handler = new CapturingMessageHandler(true);
    static auto* const warn_handler = new CapturingMessageHandler(false);
    WarningHandlerPtr() = warn_handler;
    ::kdu_core::kdu_customize_errors(err_handler);
    ::kdu_core::kdu_customize_warnings(warn_handler);
  }
};

const ::testing::Environment* const kTestEnv =
    ::testing::AddGlobalTestEnvironment(new ImageOutTestEnv());

class TempDir {
 public:
  explicit TempDir(const std::string& name)
      : path_(std::filesystem::path(::testing::TempDir()) / name) {
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  std::string File(const std::string& filename) const {
    return (path_ / filename).string();
  }

 private:
  std::filesystem::path path_;
};

class ImageOutGuard {
 public:
  explicit ImageOutGuard(kdu_image_out& out) : out_(out) {}
  ~ImageOutGuard() {
    if (out_.exists()) {
      out_.destroy();
    }
  }

 private:
  kdu_image_out& out_;
};

enum class SampleMode {
  kFixpoint16,
  kAbs16,
  kFloat32,
  kAbs32,
  kAbs64,
};

struct OwnedLineBuf {
  kdu_sample_allocator alloc;
  kdu_line_buf line;

  OwnedLineBuf(int width, SampleMode mode) {
    kdu_byte flags = 0;
    switch (mode) {
      case SampleMode::kFixpoint16:
        flags = KDU_LINE_BUF_SHORTS;
        break;
      case SampleMode::kAbs16:
        flags = KDU_LINE_BUF_SHORTS | KDU_LINE_BUF_ABSOLUTE;
        break;
      case SampleMode::kFloat32:
        flags = 0;
        break;
      case SampleMode::kAbs32:
        flags = KDU_LINE_BUF_ABSOLUTE;
        break;
      case SampleMode::kAbs64:
        flags = KDU_LINE_BUF_VPRECISE | KDU_LINE_BUF_ABSOLUTE;
        break;
    }
    if (width <= 0) return;
    line.pre_create(&alloc, width, flags, 0, 0);
    alloc.finalize(kdu_codestream());
    line.create();
  }

  ~OwnedLineBuf() {
    if (line.get_width() > 0) line.destroy();
  }
};

void FillSampleLine(kdu_line_buf& line, SampleMode mode, int width,
                    int row_offset, int bit_depth, bool is_signed,
                    int forced_exponent = 0) {
  if (forced_exponent > 0) {
    // Emit encoded float-as-int values spanning sub-normal, normal, underflow
    // and overflow boundaries.
    int prec = bit_depth > 32 ? 32 : bit_depth;
    int mantissa_bits = prec - 1 - forced_exponent;
    int exp_bias = (1 << (forced_exponent - 1)) - 1;
    auto encode_sample = [&](int idx) -> kdu_int64 {
      if (idx == 0) {
        return is_signed ? -1 : -(1LL << (prec - 1));
      }
      int exp_field = exp_bias + ((idx % 3) - 1);
      if (exp_field < 0) exp_field = 0;
      kdu_int64 mag = (static_cast<kdu_int64>(exp_field) << mantissa_bits) |
                      ((1LL << std::max(0, mantissa_bits - 1)) >> 0);
      if (idx == width - 1) {
        // Large magnitude triggering upper exponent/value clamp.
        mag = ((1LL << forced_exponent) - 1) << mantissa_bits;
      }
      if (!is_signed) {
        return mag - (1LL << (prec - 1));
      }
      kdu_int64 sign_bit = (idx % 2 == 1) ? -(1LL << (prec - 1)) : 0;
      return sign_bit | (mag & ((1LL << (prec - 1)) - 1));
    };
    if (mode == SampleMode::kAbs64) {
      kdu_sample64* dp = line.get_buf64();
      for (int c = 0; c < width; ++c)
        dp[c].ival = encode_sample(c + row_offset);
    } else if (mode == SampleMode::kAbs32) {
      kdu_sample32* dp = line.get_buf32();
      for (int c = 0; c < width; ++c)
        dp[c].ival = static_cast<kdu_int32>(encode_sample(c + row_offset));
    } else {
      kdu_sample16* dp = line.get_buf16();
      for (int c = 0; c < width; ++c)
        dp[c].ival = static_cast<kdu_int16>(encode_sample(c + row_offset));
    }
    return;
  }

  switch (mode) {
    case SampleMode::kFixpoint16: {
      kdu_sample16* dp = line.get_buf16();
      int half = 1 << (KDU_FIX_POINT - 1);
      for (int c = 0; c < width; ++c) {
        int val = -half - 20 + ((c + row_offset) * (half / 2 + 200));
        if (c == width - 1) val = half + 500;
        dp[c].ival = static_cast<kdu_int16>(std::clamp(val, -32768, 32767));
      }
      break;
    }
    case SampleMode::kAbs16: {
      kdu_sample16* dp = line.get_buf16();
      int limit = (bit_depth >= 16) ? 32767 : (1 << (bit_depth - 1));
      for (int c = 0; c < width; ++c) {
        int val = (c == 0) ? -limit - 10 : -limit + c * 73 + row_offset * 17;
        if (c == width - 1) val = limit + 10;
        dp[c].ival = static_cast<kdu_int16>(std::clamp(val, -32768, 32767));
      }
      break;
    }
    case SampleMode::kFloat32: {
      kdu_sample32* dp = line.get_buf32();
      for (int c = 0; c < width; ++c) {
        float val = -0.75f + 0.35f * static_cast<float>(c + row_offset);
        if (c == width - 1) val = 0.85f;
        dp[c].fval = val;
      }
      break;
    }
    case SampleMode::kAbs32: {
      kdu_sample32* dp = line.get_buf32();
      kdu_int64 half = 1LL << std::min(30, bit_depth - 1);
      for (int c = 0; c < width; ++c) {
        kdu_int64 val = (c == 0)
                            ? -half - 4
                            : -half + static_cast<kdu_int64>(c + row_offset) *
                                          std::max<kdu_int64>(1, half / 3);
        if (c == width - 1) val = half + 4;
        dp[c].ival = static_cast<kdu_int32>(
            std::clamp<kdu_int64>(val, INT32_MIN, INT32_MAX));
      }
      break;
    }
    case SampleMode::kAbs64: {
      kdu_sample64* dp = line.get_buf64();
      kdu_int64 half = 1LL << std::min(40, bit_depth - 1);
      for (int c = 0; c < width; ++c) {
        kdu_int64 val = (c == 0)
                            ? -half - 4
                            : -half + static_cast<kdu_int64>(c + row_offset) *
                                          std::max<kdu_int64>(1, half / 3);
        if (c == width - 1) val = half + 4;
        dp[c].ival = val;
      }
      break;
    }
  }
}

std::vector<kdu_byte> ReadFileBytes(const std::string& path) {
  std::ifstream ifs(path, std::ios::binary);
  return std::vector<kdu_byte>(std::istreambuf_iterator<char>(ifs),
                               std::istreambuf_iterator<char>());
}

class MemoryTarget final : public kdu_compressed_target {
 public:
  bool write(const kdu_byte* buf, int num_bytes) override {
    data_.insert(data_.end(), buf, buf + num_bytes);
    return true;
  }
  const std::vector<kdu_byte>& data() const { return data_; }

 private:
  std::vector<kdu_byte> data_;
};

class MemorySource final : public kdu_compressed_source {
 public:
  explicit MemorySource(const std::vector<kdu_byte>& data) : data_(data) {}

  int get_capabilities() override {
    return KDU_SOURCE_CAP_SEQUENTIAL | KDU_SOURCE_CAP_SEEKABLE;
  }
  int read(kdu_byte* buf, int num_bytes) override {
    int avail = static_cast<int>(data_.size()) - pos_;
    int n = std::min(avail, num_bytes);
    if (n > 0) {
      std::memcpy(buf, data_.data() + pos_, n);
      pos_ += n;
    }
    return n;
  }
  bool seek(kdu_long offset) override {
    if (offset < 0 || offset > static_cast<kdu_long>(data_.size())) {
      return false;
    }
    pos_ = static_cast<int>(offset);
    return true;
  }
  kdu_long get_pos() override { return pos_; }

 private:
  const std::vector<kdu_byte>& data_;
  int pos_ = 0;
};

std::vector<kdu_byte> BuildMinimalJpxWithUuidBoxes(
    const TempDir& dir, const std::vector<kdu_byte>& xmp_payload,
    const std::vector<kdu_byte>& iptc_payload,
    const std::vector<kdu_byte>& geo_payload) {
  MemoryTarget jpx_tgt;
  jp2_family_tgt family;
  family.open(&jpx_tgt);
  jpx_target jout;
  jout.open(&family);

  jpx_codestream_target cs0 = jout.add_codestream();
  siz_params siz;
  siz.set(Scomponents, 0, 0, 1);
  siz.set(Sdims, 0, 0, 4);
  siz.set(Sdims, 0, 1, 4);
  siz.set(Sprecision, 0, 0, 8);
  siz.set(Ssigned, 0, 0, false);
  static_cast<::kdu_core::kdu_params&>(siz).finalize();
  cs0.access_dimensions().init(&siz);

  jpx_layer_target lay0 = jout.add_layer();
  lay0.add_colour().init(JP2_sLUM_SPACE);
  lay0.access_channels().init(1);
  lay0.access_channels().set_colour_mapping(0, 0, -1, 0);

  jpx_metanode root = jout.access_meta_manager().access_root();
  if (!xmp_payload.empty()) root.add_delayed(jp2_uuid_4cc, 1);
  if (!iptc_payload.empty()) root.add_delayed(jp2_uuid_4cc, 2);
  if (!geo_payload.empty()) root.add_delayed(jp2_uuid_4cc, 3);

  while (jout.write_headers() != nullptr) {
  }
  auto* stream = cs0.open_stream();
  kdu_codestream cs;
  cs.create(&siz, stream);
  cs.access_siz()->finalize_all();
  kdu_stripe_compressor compressor;
  compressor.start(cs);
  std::vector<kdu_byte> pixels(16, 128);
  const int stripe_heights[1] = {4};
  compressor.push_stripe(pixels.data(), stripe_heights);
  compressor.finish();
  cs.destroy();
  stream->close();
  const kdu_byte xmp_uuid[16] = {0xBE, 0x7A, 0xCF, 0xCB, 0x97, 0xA9,
                                 0x42, 0xE8, 0x9C, 0x71, 0x99, 0x94,
                                 0x91, 0xE3, 0xAF, 0xAC};
  const kdu_byte iptc_uuid[16] = {0x33, 0xC7, 0xA4, 0xD2, 0xB8, 0x1D,
                                  0x47, 0x23, 0xA0, 0xBA, 0xF1, 0xA3,
                                  0xE0, 0x97, 0xAD, 0x38};
  const kdu_byte geo_uuid[16] = {0xB1, 0x4B, 0xF8, 0xBD, 0x08, 0x3D,
                                 0x4B, 0x43, 0xA5, 0xAE, 0x8C, 0xD7,
                                 0xD5, 0xA6, 0xCE, 0x03};
  int tag_id = 0;
  jp2_output_box* mbox = nullptr;
  while ((mbox = jout.write_metadata(&tag_id)) != nullptr) {
    if (tag_id == 1) {
      mbox->write(xmp_uuid, 16);
      mbox->write(xmp_payload.data(), static_cast<int>(xmp_payload.size()));
    } else if (tag_id == 2) {
      mbox->write(iptc_uuid, 16);
      mbox->write(iptc_payload.data(), static_cast<int>(iptc_payload.size()));
    } else if (tag_id == 3) {
      mbox->write(geo_uuid, 16);
      mbox->write(geo_payload.data(), static_cast<int>(geo_payload.size()));
    }
  }
  jout.close();
  family.close();
  return jpx_tgt.data();
}

TEST(ImageOutTest, DispatchAndConstructorErrorPaths) {
  TempDir dir("dispatch");
  kdu_image_out empty_out;
  EXPECT_FALSE(empty_out.exists());
  EXPECT_TRUE(!empty_out);

  {
    kdu_image_dims dims;
    dims.add_component(2, 4, 8, false);
    int next = 0;
    bool vflip = false;
    EXPECT_THROW(
        kdu_image_out(dir.File("no_suffix").c_str(), dims, next, vflip, true),
        kdu_exception);
    next = 0;
    EXPECT_THROW(
        kdu_image_out(dir.File("bad.xyz").c_str(), dims, next, vflip, true),
        kdu_exception);
    next = 0;
    EXPECT_THROW(kdu_image_out(dir.File("missing_dir/a.pgm").c_str(), dims,
                               next, vflip, true),
                 kdu_exception);
    next = 0;
    EXPECT_THROW(kdu_image_out(dir.File("missing_dir/a.pgx").c_str(), dims,
                               next, vflip, true),
                 kdu_exception);
    next = 0;
    EXPECT_THROW(kdu_image_out(dir.File("missing_dir/a.raw").c_str(), dims,
                               next, vflip, true),
                 kdu_exception);
    next = 0;
    EXPECT_THROW(kdu_image_out(dir.File("missing_dir/a.bmp").c_str(), dims,
                               next, vflip, true),
                 kdu_exception);
    next = 0;
    EXPECT_THROW(kdu_image_out(dir.File("missing_dir/a.tif").c_str(), dims,
                               next, vflip, true),
                 kdu_exception);
    // Single-component error paths for formats needing 3 components.
    next = 0;
    EXPECT_THROW(
        kdu_image_out(dir.File("too_few.ppm").c_str(), dims, next, vflip, true),
        kdu_exception);
    // Exhausted component index across single-component writers.
    next = 1;
    EXPECT_THROW(
        kdu_image_out(dir.File("no_comp.pgm").c_str(), dims, next, vflip, true),
        kdu_exception);
    next = 1;
    EXPECT_THROW(
        kdu_image_out(dir.File("no_comp.pgx").c_str(), dims, next, vflip, true),
        kdu_exception);
    next = 1;
    EXPECT_THROW(
        kdu_image_out(dir.File("no_comp.raw").c_str(), dims, next, vflip, true),
        kdu_exception);
    next = 1;
    EXPECT_THROW(
        kdu_image_out(dir.File("no_comp.bmp").c_str(), dims, next, vflip, true),
        kdu_exception);
    next = 1;
    EXPECT_THROW(
        kdu_image_out(dir.File("no_comp.tif").c_str(), dims, next, vflip, true),
        kdu_exception);
  }

  // Test uppercase suffixes and /dev/null dispatch.
  const char* single_exts[] = {
      ".pgm", ".PGM", ".pgx", ".PGX", ".raw", ".RAW", ".rawl", ".RAWL",
      ".pfm", ".PFM", ".bmp", ".BMP", ".tif", ".TIF", ".tiff", ".TIFF"};
  for (int i = 0; i < 16; ++i) {
    kdu_image_dims dims;
    dims.add_component(1, 2, 8, false);
    int next = 0;
    bool vflip = false;
    kdu_image_out out(
        dir.File("u_" + std::to_string(i) + single_exts[i]).c_str(), dims, next,
        vflip, true);
    ImageOutGuard guard(out);
    EXPECT_TRUE(out.exists());
    EXPECT_FALSE(!out);
    EXPECT_EQ(next, 1);
    EXPECT_EQ(vflip, std::string(single_exts[i]) == ".bmp" ||
                         std::string(single_exts[i]) == ".BMP");
  }

  {
    kdu_image_dims dims;
    dims.add_component(1, 2, 8, false);
    int next = 0;
    bool vflip = false;
    // "/dev/null" has no '.', so strrchr(fname, '.') fails before the inner
    // "/dev/null" check in kdu_image_out::kdu_image_out.
    EXPECT_THROW(kdu_image_out("/dev/null", dims, next, vflip, true),
                 kdu_exception);
  }
}

TEST(ImageOutTest, WriterSpecificConstructorErrorsAndWarnings) {
  TempDir dir("ctor_errors");
  // PPM errors: missing dir, mismatched dims/sign, mixed >8bit precisions,
  // image too wide (> INT_MAX/3), signed warning.
  {
    kdu_image_dims dims;
    dims.add_component(2, 4, 8, false);
    dims.add_component(2, 4, 8, false);
    dims.add_component(2, 4, 8, false);
    int next = 0;
    bool vflip = false;
    EXPECT_THROW(kdu_image_out(dir.File("missing_dir/a.ppm").c_str(), dims,
                               next, vflip, true),
                 kdu_exception);
  }
  {
    kdu_image_dims dims;
    dims.add_component(2, 4, 8, false);
    dims.add_component(3, 4, 8, false);
    dims.add_component(2, 4, 8, false);
    int next = 0;
    bool vflip = false;
    EXPECT_THROW(kdu_image_out(dir.File("mismatch.ppm").c_str(), dims, next,
                               vflip, true),
                 kdu_exception);
  }
  {
    kdu_image_dims dims;
    dims.add_component(2, 4, 10, false);
    dims.add_component(2, 4, 12, false);
    dims.add_component(2, 4, 12, false);
    int next = 0;
    bool vflip = false;
    EXPECT_THROW(kdu_image_out(dir.File("mixed_prec.ppm").c_str(), dims, next,
                               vflip, true),
                 kdu_exception);
  }
  {
    kdu_image_dims dims;
    dims.add_component(2, (INT_MAX / 3) + 1, 8, false);
    dims.add_component(2, (INT_MAX / 3) + 1, 8, false);
    dims.add_component(2, (INT_MAX / 3) + 1, 8, false);
    int next = 0;
    bool vflip = false;
    EXPECT_THROW(kdu_image_out(dir.File("too_wide.ppm").c_str(), dims, next,
                               vflip, true),
                 kdu_exception);
  }
  // PFM errors: missing dir, > INT_MAX/4 width, forced_prec != 32 warning.
  {
    kdu_image_dims dims;
    dims.add_component(2, 4, 8, false);
    int next = 0;
    bool vflip = false;
    EXPECT_THROW(kdu_image_out(dir.File("missing_dir/a.pfm").c_str(), dims,
                               next, vflip, true),
                 kdu_exception);
  }
  {
    kdu_image_dims dims;
    dims.add_component(2, (INT_MAX / 4) + 1, 8, false);
    int next = 0;
    bool vflip = false;
    EXPECT_THROW(kdu_image_out(dir.File("too_wide.pfm").c_str(), dims, next,
                               vflip, true),
                 kdu_exception);
  }
  // PGX and RAW >32 bit error.
  {
    kdu_image_dims dims;
    dims.add_component(2, 4, 33, false);
    int next = 0;
    bool vflip = false;
    EXPECT_THROW(kdu_image_out(dir.File("too_deep.pgx").c_str(), dims, next,
                               vflip, true),
                 kdu_exception);
    next = 0;
    EXPECT_THROW(kdu_image_out(dir.File("too_deep.raw").c_str(), dims, next,
                               vflip, true),
                 kdu_exception);
  }
  // BMP > INT_MAX/3 width error and >4GB file_bytes error.
  {
    kdu_image_dims dims;
    for (int c = 0; c < 3; ++c) {
      dims.add_component(2, (INT_MAX / 3) + 1, 8, false);
    }
    int next = 0;
    bool vflip = false;
    EXPECT_THROW(kdu_image_out(dir.File("too_wide.bmp").c_str(), dims, next,
                               vflip, true),
                 kdu_exception);
  }
  {
    kdu_image_dims dims;
    dims.add_component(200000, 10000, 8, false);
    dims.add_component(200000, 10000, 8, false);
    dims.add_component(200000, 10000, 8, false);
    int next = 0;
    bool vflip = false;
    EXPECT_THROW(
        kdu_image_out(dir.File("over4gb.bmp").c_str(), dims, next, vflip, true),
        kdu_exception);
  }
}

TEST(ImageOutTest, PgmOutAllBufferModesPrecisionForcingAndMultiTile) {
  TempDir dir("pgm");
  const SampleMode modes[] = {SampleMode::kFixpoint16, SampleMode::kAbs16,
                              SampleMode::kFloat32, SampleMode::kAbs32,
                              SampleMode::kAbs64};
  for (int m = 0; m < 5; ++m) {
    for (int bit_depth : {6, 8, 12, 18}) {
      for (bool is_signed : {false, true}) {
        kdu_image_dims dims;
        dims.add_component(3, 6, bit_depth, is_signed);
        if (bit_depth == 18) {
          dims.set_forced_precision(0, 14, true);
        } else if (bit_depth == 6) {
          dims.set_forced_precision(0, 8, false);
        }
        int next = 0;
        bool vflip = false;
        std::string fname = dir.File("p_" + std::to_string(m) + "_" +
                                     std::to_string(bit_depth) + "_" +
                                     std::to_string(is_signed) + ".pgm");
        kdu_image_out out(fname.c_str(), dims, next, vflip, true);
        ImageOutGuard guard(out);
        for (int r = 0; r < 3; ++r) {
          OwnedLineBuf t0(4, modes[m]);
          OwnedLineBuf t1(2, modes[m]);
          OwnedLineBuf t2(0, modes[m]);
          FillSampleLine(t0.line, modes[m], 4, r, bit_depth, is_signed);
          FillSampleLine(t1.line, modes[m], 2, r + 4, bit_depth, is_signed);
          if (r == 0) {
            OwnedLineBuf r1t0(4, modes[m]);
            OwnedLineBuf r1t1(2, modes[m]);
            FillSampleLine(r1t0.line, modes[m], 4, 1, bit_depth, is_signed);
            FillSampleLine(r1t1.line, modes[m], 2, 5, bit_depth, is_signed);
            out.put(0, t0.line, 0);
            out.put(0, r1t0.line, 0);
            out.put(0, t1.line, 1);
            out.put(0, t2.line, 2);
            out.put(0, r1t1.line, 1);
            out.put(0, t2.line, 2);
            r = 1;
          } else {
            out.put(0, t0.line, 0);
            out.put(0, t1.line, 1);
            out.put(0, t2.line, 2);
          }
        }
        // Writing an extra row hits num_unwritten_rows == 0.
        OwnedLineBuf t0(4, modes[m]);
        OwnedLineBuf t1(2, modes[m]);
        FillSampleLine(t0.line, modes[m], 4, 3, bit_depth, is_signed);
        FillSampleLine(t1.line, modes[m], 2, 7, bit_depth, is_signed);
        out.put(0, t0.line, 0);
        EXPECT_THROW(out.put(0, t1.line, 1), kdu_exception);
      }
    }
  }

  // Float forcing (`forced_exponent > 0`) on PGM (both 8-bit forced & default
  // 16-bit precision) and error on non-absolute / invalid exponent.
  for (int forced_p : {-1, 8}) {
    for (SampleMode mode :
         {SampleMode::kAbs16, SampleMode::kAbs32, SampleMode::kAbs64}) {
      for (bool is_signed : {false, true}) {
        int bit_depth = (mode == SampleMode::kAbs16) ? 16 : 16;
        kdu_image_dims dims;
        dims.add_component(2, 4, bit_depth, is_signed);
        dims.set_forced_precision(0, forced_p, false, 5);
        int next = 0;
        bool vflip = false;
        kdu_image_out out(dir.File("pgm_f_" + std::to_string(forced_p) + "_" +
                                   std::to_string(static_cast<int>(mode)) +
                                   "_" + std::to_string(is_signed) + ".pgm")
                              .c_str(),
                          dims, next, vflip, true);
        ImageOutGuard guard(out);
        for (int r = 0; r < 2; ++r) {
          OwnedLineBuf lb(4, mode);
          FillSampleLine(lb.line, mode, 4, r, bit_depth, is_signed, 5);
          out.put(0, lb.line, 0);
        }
      }
    }
  }
}

TEST(ImageOutTest, PpmOutAllBufferModesPrecisionForcingAndMultiTile) {
  TempDir dir("ppm");
  const SampleMode modes[] = {SampleMode::kFixpoint16, SampleMode::kAbs16,
                              SampleMode::kFloat32, SampleMode::kAbs32,
                              SampleMode::kAbs64};
  for (int m = 0; m < 5; ++m) {
    for (int bit_depth : {8, 12, 18}) {
      kdu_image_dims dims;
      for (int c = 0; c < 3; ++c) {
        dims.add_component(3, 5, bit_depth, c == 0 && bit_depth == 8);
        // Same signedness required for PPM.
      }
      if (bit_depth == 8) {
        dims.add_component(3, 5, 8, false, 0);
      }
      if (bit_depth == 18) {
        for (int c = 0; c < 3; ++c) dims.set_forced_precision(c, 16, true);
      }
      int next = 0;
      bool vflip = false;
      kdu_image_out out(dir.File("ppm_" + std::to_string(m) + "_" +
                                 std::to_string(bit_depth) + ".ppm")
                            .c_str(),
                        dims, next, vflip, true);
      ImageOutGuard guard(out);
      EXPECT_EQ(next, 3);
      for (int r = 0; r < 3; ++r) {
        OwnedLineBuf t0(3, modes[m]);
        OwnedLineBuf t1(2, modes[m]);
        OwnedLineBuf t2(0, modes[m]);
        for (int c = 0; c < 3; ++c) {
          FillSampleLine(t0.line, modes[m], 3, r + c, bit_depth, false);
          out.put(c, t0.line, 0);
        }
        for (int c = 0; c < 3; ++c) {
          FillSampleLine(t1.line, modes[m], 2, r + c + 3, bit_depth, false);
          out.put(c, t1.line, 1);
        }
        for (int c = 0; c < 3; ++c) out.put(c, t2.line, 2);
      }
      // Extra row error.
      OwnedLineBuf t0(3, modes[m]);
      OwnedLineBuf t1(2, modes[m]);
      for (int c = 0; c < 3; ++c) {
        FillSampleLine(t0.line, modes[m], 3, c, bit_depth, false);
        out.put(c, t0.line, 0);
      }
      FillSampleLine(t1.line, modes[m], 2, 0, bit_depth, false);
      out.put(0, t1.line, 1);
      out.put(1, t1.line, 1);
      EXPECT_THROW(out.put(2, t1.line, 1), kdu_exception);
    }
  }

  // Float forcing & signed PPM warning.
  for (int forced_p : {-1, 8}) {
    for (SampleMode mode :
         {SampleMode::kAbs16, SampleMode::kAbs32, SampleMode::kAbs64}) {
      kdu_image_dims dims;
      for (int c = 0; c < 3; ++c) {
        dims.add_component(2, 3, 16, true);
        dims.set_forced_precision(c, forced_p, false, 5);
      }
      int next = 0;
      bool vflip = false;
      kdu_image_out out(
          dir.File("ppm_f_" + std::to_string(forced_p) + "_" +
                   std::to_string(static_cast<int>(mode)) + ".ppm")
              .c_str(),
          dims, next, vflip, true);
      ImageOutGuard guard(out);
      for (int r = 0; r < 2; ++r) {
        OwnedLineBuf lb(3, mode);
        FillSampleLine(lb.line, mode, 3, r, 16, true, 5);
        for (int c = 0; c < 3; ++c) out.put(c, lb.line, 0);
      }
    }
  }
}

TEST(ImageOutTest, PfmOutSingleAndTripleComponentsAllModes) {
  TempDir dir("pfm");
  const SampleMode modes[] = {SampleMode::kFixpoint16, SampleMode::kAbs16,
                              SampleMode::kFloat32, SampleMode::kAbs32,
                              SampleMode::kAbs64};
  for (int comps : {1, 3}) {
    for (int m = 0; m < 5; ++m) {
      kdu_image_dims dims;
      for (int c = 0; c < comps; ++c) {
        dims.add_component(3, 4, 12, comps == 1);
        if (m == 0) dims.set_forced_precision(c, 16, false);
      }
      int next = 0;
      bool vflip = false;
      kdu_image_out out(dir.File("pfm_" + std::to_string(comps) + "_" +
                                 std::to_string(m) + ".pfm")
                            .c_str(),
                        dims, next, vflip, true);
      ImageOutGuard guard(out);
      EXPECT_EQ(next, comps);
      for (int r = 0; r < 3; ++r) {
        OwnedLineBuf t0(2, modes[m]);
        OwnedLineBuf t1(2, modes[m]);
        OwnedLineBuf t2(0, modes[m]);
        for (int c = 0; c < comps; ++c) {
          FillSampleLine(t0.line, modes[m], 2, r + c, 12, comps == 1);
          out.put(c, t0.line, 0);
        }
        for (int c = 0; c < comps; ++c) {
          FillSampleLine(t1.line, modes[m], 2, r + c + 2, 12, comps == 1);
          out.put(c, t1.line, 1);
        }
        for (int c = 0; c < comps; ++c) out.put(c, t2.line, 2);
      }
      OwnedLineBuf t0(2, modes[m]);
      OwnedLineBuf t1(2, modes[m]);
      for (int c = 0; c < comps; ++c) {
        FillSampleLine(t0.line, modes[m], 2, c, 12, comps == 1);
        out.put(c, t0.line, 0);
      }
      FillSampleLine(t1.line, modes[m], 2, 0, 12, comps == 1);
      for (int c = 0; c < comps - 1; ++c) out.put(c, t1.line, 1);
      EXPECT_THROW(out.put(comps - 1, t1.line, 1), kdu_exception);
    }
  }

  // Float forcing with PFM (`forced_exponent > 0`).
  for (SampleMode mode :
       {SampleMode::kAbs16, SampleMode::kAbs32, SampleMode::kAbs64}) {
    kdu_image_dims dims;
    dims.add_component(2, 4, 16, false);
    dims.set_forced_precision(0, 32, false, 5);
    int next = 0;
    bool vflip = false;
    kdu_image_out out(
        dir.File("pfm_f_" + std::to_string(static_cast<int>(mode)) + ".pfm")
            .c_str(),
        dims, next, vflip, true);
    ImageOutGuard guard(out);
    OwnedLineBuf lb(4, mode);
    FillSampleLine(lb.line, mode, 4, 0, 16, false, 5);
    out.put(0, lb.line, 0);
    // Destroy after writing 1 of 2 rows to also cover ~pfm_out partial warning.
  }
}

TEST(ImageOutTest, PgxAndRawOutAllByteWidthsAndFloatClipping) {
  TempDir dir("pgx_raw");
  const SampleMode modes[] = {SampleMode::kFixpoint16, SampleMode::kAbs16,
                              SampleMode::kFloat32, SampleMode::kAbs32,
                              SampleMode::kAbs64};
  // Cover sample_bytes == 1, 2, 3 (raw only), and 4, with big & little endian.
  for (const char* ext : {".pgx", ".raw", ".rawl"}) {
    for (int bit_depth : {8, 14, 24, 30}) {
      for (bool is_signed : {false, true}) {
        for (int m = 0; m < 5; ++m) {
          kdu_image_dims dims;
          dims.add_component(2, 4, bit_depth, is_signed);
          if (bit_depth == 14) {
            dims.set_forced_precision(0, 12, is_signed);
          }
          int next = 0;
          bool vflip = false;
          std::string fname = dir.File(
              std::string("pr_") + (ext + 1) + "_" + std::to_string(bit_depth) +
              "_" + std::to_string(is_signed) + "_" + std::to_string(m) + ext);
          kdu_image_out out(fname.c_str(), dims, next, vflip, true);
          ImageOutGuard guard(out);
          for (int r = 0; r < 2; ++r) {
            OwnedLineBuf t0(2, modes[m]);
            OwnedLineBuf t1(2, modes[m]);
            OwnedLineBuf t2(0, modes[m]);
            FillSampleLine(t0.line, modes[m], 2, r, bit_depth, is_signed);
            FillSampleLine(t1.line, modes[m], 2, r + 2, bit_depth, is_signed);
            out.put(0, t0.line, 0);
            out.put(0, t1.line, 1);
            out.put(0, t2.line, 2);
          }
          OwnedLineBuf t0(2, modes[m]);
          OwnedLineBuf t1(2, modes[m]);
          FillSampleLine(t0.line, modes[m], 2, 0, bit_depth, is_signed);
          FillSampleLine(t1.line, modes[m], 2, 2, bit_depth, is_signed);
          out.put(0, t0.line, 0);
          EXPECT_THROW(out.put(0, t1.line, 1), kdu_exception);
        }
      }
    }
  }

  // Forced float clipping (`clip_ints_interpreted_as_floats`) on PGX & RAWL.
  for (const char* ext : {".pgx", ".rawl"}) {
    for (SampleMode mode :
         {SampleMode::kAbs16, SampleMode::kAbs32, SampleMode::kAbs64}) {
      for (bool is_signed : {false, true}) {
        kdu_image_dims dims;
        dims.add_component(1, 4, 16, is_signed);
        dims.set_forced_precision(0, 16, false, 5);
        int next = 0;
        bool vflip = false;
        kdu_image_out out(dir.File(std::string("clip_") + (ext + 1) + "_" +
                                   std::to_string(static_cast<int>(mode)) +
                                   "_" + std::to_string(is_signed) + ext)
                              .c_str(),
                          dims, next, vflip, true);
        ImageOutGuard guard(out);
        OwnedLineBuf lb(4, mode);
        FillSampleLine(lb.line, mode, 4, 0, 16, is_signed, 5);
        out.put(0, lb.line, 0);
      }
    }
  }

  // Cover check_absolute_ints_for_float_forcing errors (non-absolute line &
  // orig_precision < exponent_bits + 1) + PGX incomplete row destructor
  // warning.
  {
    kdu_image_dims dims;
    dims.add_component(2, 4, 16, false);
    dims.set_forced_precision(0, 16, false, 5);
    int next = 0;
    bool vflip = false;
    kdu_image_out out(dir.File("non_abs.pgx").c_str(), dims, next, vflip, true);
    ImageOutGuard guard(out);
    OwnedLineBuf flt(4, SampleMode::kFloat32);
    FillSampleLine(flt.line, SampleMode::kFloat32, 4, 0, 16, false);
    EXPECT_THROW(out.put(0, flt.line, 0), kdu_exception);
  }
  {
    kdu_image_dims dims;
    dims.add_component(2, 4, 4, false);
    dims.set_forced_precision(0, 8, false, 5);
    int next = 0;
    bool vflip = false;
    kdu_image_out out(dir.File("bad_exp.raw").c_str(), dims, next, vflip, true);
    ImageOutGuard guard(out);
    OwnedLineBuf lb(4, SampleMode::kAbs32);
    FillSampleLine(lb.line, SampleMode::kAbs32, 4, 0, 4, false);
    EXPECT_THROW(out.put(0, lb.line, 0), kdu_exception);
  }
}

TEST(ImageOutTest, BmpOutGrayscaleAndRgbResolutionAndModes) {
  TempDir dir("bmp");
  const SampleMode modes[] = {SampleMode::kFixpoint16, SampleMode::kAbs16,
                              SampleMode::kFloat32, SampleMode::kAbs32,
                              SampleMode::kAbs64};
  // Test widths 1, 2, 3, 4 (exercising alignment_bytes 3, 2, 1, 0) & 1/3 comps.
  for (int comps : {1, 3}) {
    for (int width : {1, 2, 3, 4}) {
      kdu_image_dims dims;
      for (int c = 0; c < comps; ++c) dims.add_component(2, width, 8, true);
      if (width == 1) {
        dims.set_resolution(width, 2, true, 3000.0, 3000.0);
      } else if (width == 2) {
        dims.set_resolution(width, 2, false, 100.0, 200.0);
      } else if (width == 3) {
        dims.set_resolution(width, 2, true, 3000.0, 3000.0, true);
      }
      int next = 0;
      bool vflip = false;
      kdu_image_out out(dir.File("b_" + std::to_string(comps) + "_" +
                                 std::to_string(width) + ".bmp")
                            .c_str(),
                        dims, next, vflip, true);
      ImageOutGuard guard(out);
      EXPECT_TRUE(vflip);
      SampleMode mode = modes[(comps + width) % 5];
      for (int r = 0; r < 2; ++r) {
        OwnedLineBuf lb(width, mode);
        for (int c = 0; c < comps; ++c) {
          FillSampleLine(lb.line, mode, width, r + c, 8, true);
          out.put(c, lb.line, 0);
        }
      }
    }
  }

  // All 5 SampleModes + multi-tile + forced_exponent + component mismatch
  // falling back to 1-comp BMP + extra row error.
  for (int m = 0; m < 5; ++m) {
    kdu_image_dims dims;
    for (int c = 0; c < 3; ++c) dims.add_component(2, 5, 16, false);
    if (m == 0) dims.set_forced_precision(0, 8, true);
    int next = 0;
    bool vflip = false;
    kdu_image_out out(dir.File("bm_" + std::to_string(m) + ".bmp").c_str(),
                      dims, next, vflip, true);
    ImageOutGuard guard(out);
    for (int r = 0; r < 2; ++r) {
      OwnedLineBuf t0(3, modes[m]);
      OwnedLineBuf t1(2, modes[m]);
      OwnedLineBuf t2(0, modes[m]);
      for (int c = 0; c < 3; ++c) {
        FillSampleLine(t0.line, modes[m], 3, r + c, 16, false);
        out.put(c, t0.line, 0);
      }
      for (int c = 0; c < 3; ++c) {
        FillSampleLine(t1.line, modes[m], 2, r + c + 3, 16, false);
        out.put(c, t1.line, 1);
      }
      for (int c = 0; c < 3; ++c) out.put(c, t2.line, 2);
    }
    OwnedLineBuf t0(3, modes[m]);
    OwnedLineBuf t1(2, modes[m]);
    FillSampleLine(t0.line, modes[m], 3, 0, 16, false);
    for (int c = 0; c < 3; ++c) out.put(c, t0.line, 0);
    FillSampleLine(t1.line, modes[m], 2, 0, 16, false);
    out.put(0, t1.line, 1);
    out.put(1, t1.line, 1);
    EXPECT_THROW(out.put(2, t1.line, 1), kdu_exception);
  }

  // BMP forced_exponent > 0 across 16/32/64-bit line buffers.
  for (SampleMode mode :
       {SampleMode::kAbs16, SampleMode::kAbs32, SampleMode::kAbs64}) {
    kdu_image_dims dims;
    dims.add_component(2, 4, 16, true);
    dims.set_forced_precision(0, -1, false, 5);
    int next = 0;
    bool vflip = false;
    kdu_image_out out(
        dir.File("bf_" + std::to_string(static_cast<int>(mode)) + ".bmp")
            .c_str(),
        dims, next, vflip, true);
    ImageOutGuard guard(out);
    OwnedLineBuf lb(4, mode);
    FillSampleLine(lb.line, mode, 4, 0, 16, true, 5);
    out.put(0, lb.line, 0);
  }
}

TEST(ImageOutTest, TifOutColorSpacesSubBytePackingFloatAndMetadata) {
  TempDir dir("tif");
  // Test colour space & alpha combinations.
  struct ColorCase {
    int comps;
    int num_colours;
    bool premul;
    bool unassoc;
    int conf;
    jp2_colour_space space;
  };
  const ColorCase cases[] = {
      {1, 1, false, false, 2, JP2_sLUM_SPACE},
      {3, 3, false, false, 2, JP2_sRGB_SPACE},
      {4, 4, false, false, 2, JP2_CMYK_SPACE},
      {1, 1, false, false, 2, JP2_bilevel1_SPACE},
      {1, 1, false, false, 2, JP2_bilevel2_SPACE},
      {3, 3, false, false, 2, JP2_sYCC_SPACE},
      {4, 4, false, false, 2, JP2_YCCK_SPACE},
      {1, 1, false, false, 2, JP2_CIELab_SPACE},
      {3, 0, false, false, 0, JP2_sRGB_SPACE},
      {2, 0, false, false, 0, JP2_sLUM_SPACE},
      {3, 3, false, false, 0, JP2_sRGB_SPACE},
      {2, 2, false, false, 2, JP2_sLUM_SPACE},
      {3, 4, false, false, 2, JP2_CMYK_SPACE},
      {4, 3, true, false, 2, JP2_sRGB_SPACE},
      {4, 3, false, true, 2, JP2_sRGB_SPACE},
      {2, 3, false, false, 0, JP2_sLUM_SPACE},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    kdu_image_dims dims;
    for (int c = 0; c < cases[i].comps; ++c) {
      dims.add_component(2, 4, (i == 3 || i == 4) ? 1 : 8, false);
    }
    const kdu_byte icc[16] = {0, 0, 0, 16, 'I', 'C', 'C', ' ',
                              0, 0, 0, 0,  0,   0,   0,   0};
    dims.set_colour_info(cases[i].num_colours, cases[i].premul,
                         cases[i].unassoc, cases[i].conf, cases[i].space,
                         (i == 0) ? icc : nullptr, (i == 0) ? 16 : 0);
    if (i == 0) {
      dims.set_resolution(4.0, 2.0, true, 2835.0, 2835.0);
    } else if (i == 1) {
      dims.set_resolution(4.0, 2.0, false, 100.0, 100.0);
    } else if (i == 2) {
      dims.set_resolution(4.0, 2.0, true, 2835.0, 2835.0, true);
    }
    int next = 0;
    bool vflip = false;
    kdu_image_out out(dir.File("cs_" + std::to_string(i) + ".tif").c_str(),
                      dims, next, vflip, true);
    ImageOutGuard guard(out);
    OwnedLineBuf lb(4, SampleMode::kFixpoint16);
    FillSampleLine(lb.line, SampleMode::kFixpoint16, 4, 0, 8, false);
    for (int r = 0; r < 2; ++r) {
      for (int c = 0; c < next; ++c) out.put(c, lb.line, 0);
    }
  }

  // Test mismatched component dimensions forcing 1-comp TIFF + second comp file
  // (`next_comp_idx > 0`).
  {
    kdu_image_dims dims;
    dims.add_component(2, 4, 8, false);
    dims.add_component(3, 4, 8, false);
    dims.add_component(2, 4, 8, false);
    int next = 0;
    bool vflip = false;
    {
      kdu_image_out out0(dir.File("mismatch0.tif").c_str(), dims, next, vflip,
                         true);
      ImageOutGuard g0(out0);
      EXPECT_EQ(next, 1);
      OwnedLineBuf lb(4, SampleMode::kFixpoint16);
      FillSampleLine(lb.line, SampleMode::kFixpoint16, 4, 0, 8, false);
      for (int r = 0; r < 2; ++r) out0.put(0, lb.line, 0);
    }
    kdu_image_out out1(dir.File("mismatch1.tif").c_str(), dims, next, vflip,
                       true);
    ImageOutGuard g1(out1);
    EXPECT_EQ(next, 2);
    OwnedLineBuf lb1(4, SampleMode::kAbs16);
    FillSampleLine(lb1.line, SampleMode::kAbs16, 4, 0, 8, false);
    for (int r = 0; r < 3; ++r) out1.put(1, lb1.line, 0);
  }

  // Test sub-byte, sub-word, and sub-dword TIFF packing (`precision` = 1, 4, 5,
  // 12, 20, 24, 32) across SampleModes & multi-tile & error on extra row.
  const SampleMode modes[] = {SampleMode::kFixpoint16, SampleMode::kAbs16,
                              SampleMode::kFloat32, SampleMode::kAbs32,
                              SampleMode::kAbs64};
  for (int prec : {1, 4, 5, 12, 20, 24, 32}) {
    for (int m = 0; m < 5; ++m) {
      kdu_image_dims dims;
      dims.add_component(2, 8, prec, prec >= 24);
      int next = 0;
      bool vflip = false;
      kdu_image_out out(dir.File("pack_" + std::to_string(prec) + "_" +
                                 std::to_string(m) + ".tif")
                            .c_str(),
                        dims, next, vflip, true);
      ImageOutGuard guard(out);
      for (int r = 0; r < 2; ++r) {
        OwnedLineBuf t0(5, modes[m]);
        OwnedLineBuf t1(3, modes[m]);
        OwnedLineBuf t2(0, modes[m]);
        FillSampleLine(t0.line, modes[m], 5, r, prec, prec >= 24);
        FillSampleLine(t1.line, modes[m], 3, r + 5, prec, prec >= 24);
        out.put(0, t0.line, 0);
        out.put(0, t1.line, 1);
        out.put(0, t2.line, 2);
      }
      OwnedLineBuf t0(5, modes[m]);
      OwnedLineBuf t1(3, modes[m]);
      FillSampleLine(t0.line, modes[m], 5, 0, prec, prec >= 24);
      FillSampleLine(t1.line, modes[m], 3, 5, prec, prec >= 24);
      out.put(0, t0.line, 0);
      EXPECT_THROW(out.put(0, t1.line, 1), kdu_exception);
    }
  }

  // True 32-bit float TIFF output (`forced_exponent > 0`) for abs16/32/64.
  for (SampleMode mode :
       {SampleMode::kAbs16, SampleMode::kAbs32, SampleMode::kAbs64}) {
    for (bool is_signed : {false, true}) {
      kdu_image_dims dims;
      dims.add_component(2, 4, 16, is_signed);
      dims.set_forced_precision(0, -1, false, 5);
      int next = 0;
      bool vflip = false;
      kdu_image_out out(
          dir.File("tf_" + std::to_string(static_cast<int>(mode)) + "_" +
                   std::to_string(is_signed) + ".tif")
              .c_str(),
          dims, next, vflip, true);
      ImageOutGuard guard(out);
      OwnedLineBuf lb(4, mode);
      FillSampleLine(lb.line, mode, 4, 0, 16, is_signed, 5);
      out.put(0, lb.line, 0);
      out.put(0, lb.line, 0);
    }
  }

  // XMP + swapped IPTC + GeoTIFF metadata propagation via jpx_meta_manager.
  std::string geo_tmp = dir.File("geo_dir.tif");
  {
    kdu_tiffdir gd;
    gd.init(false);
    gd.write_tag(KDU_TIFFTAG_ImageWidth16, static_cast<kdu_uint16>(4));
    gd.write_tag(KDU_TIFFTAG_ImageHeight16, static_cast<kdu_uint16>(4));
    double scale[3] = {10.0, 20.0, 0.0};
    gd.write_tag((static_cast<kdu_uint32>(33550) << 16) | 12, 3, scale);
    double tie[6] = {0.0, 0.0, 0.0, 100.0, 200.0, 0.0};
    gd.write_tag((static_cast<kdu_uint32>(33922) << 16) | 12, 6, tie);
    double mat[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    gd.write_tag((static_cast<kdu_uint32>(34264) << 16) | 12, 16, mat);
    kdu_uint16 keydir[4] = {1, 1, 0, 0};
    gd.write_tag((static_cast<kdu_uint32>(34735) << 16) | 3, 4, keydir);
    double geo_d = 6378137.0;
    gd.write_tag((static_cast<kdu_uint32>(34736) << 16) | 12, 1, &geo_d);
    const char geo_ascii[] = "WGS 84|";
    gd.write_tag((static_cast<kdu_uint32>(34737) << 16) | 2, 8,
                 reinterpret_cast<const kdu_byte*>(geo_ascii));
    kdu_simple_file_target ftgt;
    ASSERT_TRUE(ftgt.open(geo_tmp.c_str(), false, true));
    gd.write_header(&ftgt, 8);
    ASSERT_TRUE(gd.writedir(&ftgt, 8));
    ftgt.close();
  }
  std::vector<kdu_byte> geo_bytes = ReadFileBytes(geo_tmp);
  std::vector<kdu_byte> xmp = {'<', 'x', '/', '>'};
  // Byte-swapped IPTC marker (`0x1C` at index 3) + unaligned length 5.
  std::vector<kdu_byte> iptc = {0x00, 0x01, 0x02, 0x1C, 0xAA};
  std::vector<kdu_byte> jpx_bytes =
      BuildMinimalJpxWithUuidBoxes(dir, xmp, iptc, geo_bytes);

  MemorySource msrc(jpx_bytes);
  jp2_family_src fsrc;
  fsrc.open(&msrc);
  jpx_source jpx_src;
  ASSERT_GT(jpx_src.open(&fsrc, false), 0);
  jpx_src.access_meta_manager().load_matches(-1, nullptr, -1, nullptr);

  kdu_image_dims dims;
  dims.add_component(2, 4, 8, false);
  dims.set_resolution(8.0, 4.0, true, 100.0, 100.0);
  dims.set_meta_manager(jpx_src.access_meta_manager());
  int next = 0;
  bool vflip = false;
  std::string out_tif = dir.File("with_meta.tif");
  {
    testing::internal::CaptureStdout();
    kdu_image_out out(out_tif.c_str(), dims, next, vflip, /*quiet=*/false);
    ImageOutGuard guard(out);
    OwnedLineBuf lb(4, SampleMode::kFixpoint16);
    FillSampleLine(lb.line, SampleMode::kFixpoint16, 4, 0, 8, false);
    out.put(0, lb.line, 0);
    out.put(0, lb.line, 0);
    std::string captured = testing::internal::GetCapturedStdout();
    EXPECT_NE(captured.find("XMP"), std::string::npos);
    EXPECT_NE(captured.find("IPTC"), std::string::npos);
    EXPECT_NE(captured.find("Geo"), std::string::npos);
  }
  // Verify written GeoTIFF tags roundtrip via kdu_tiffdir.
  {
    kdu_simple_file_source fcheck;
    ASSERT_TRUE(fcheck.open(out_tif.c_str()));
    kdu_tiffdir td;
    ASSERT_TRUE(td.opendir(&fcheck));
    double scale[3] = {};
    EXPECT_TRUE(
        td.read_tag((static_cast<kdu_uint32>(33550) << 16) | 12, 3, scale));
    EXPECT_NEAR(scale[0], 20.0, 1e-6);
    EXPECT_NEAR(scale[1], 40.0, 1e-6);
    td.close();
  }
  jpx_src.close();
  fsrc.close();
}

TEST(ImageOutTest, FloatAndWordConversionCornerCases) {
  TempDir dir("conv");
  // 1) Precision < 8 forced on PGX and PPM (`precision = 4` vs `orig = 8` and
  //    `orig = 4` vs `precision = 12`), both align_lsbs=false and
  //    align_lsbs=true to hit diff_precision > 0, < 0, and downshift < 0.
  const SampleMode modes[] = {SampleMode::kFixpoint16, SampleMode::kAbs16,
                              SampleMode::kFloat32, SampleMode::kAbs32,
                              SampleMode::kAbs64};
  struct PrecCase {
    int orig;
    int forced;
    bool align_lsbs;
    bool is_signed;
  };
  const PrecCase pcases[] = {
      {12, 6, true, false},  {12, 6, true, true},   {12, 6, false, true},
      {6, 12, true, false},  {6, 12, true, true},   {6, 12, false, false},
      {20, 10, true, false}, {20, 10, false, true}, {10, 20, true, false},
      {24, 32, true, true},  {8, 4, true, false},   {4, 8, true, true},
  };
  for (size_t i = 0; i < sizeof(pcases) / sizeof(pcases[0]); ++i) {
    for (int m = 0; m < 5; ++m) {
      kdu_image_dims dims;
      dims.add_component(1, 4, pcases[i].orig, pcases[i].is_signed);
      dims.set_forced_precision(0, pcases[i].forced, pcases[i].align_lsbs);
      int next = 0;
      bool vflip = false;
      kdu_image_out out(dir.File("pc_" + std::to_string(i) + "_" +
                                 std::to_string(m) + ".rawl")
                            .c_str(),
                        dims, next, vflip, true);
      ImageOutGuard guard(out);
      OwnedLineBuf lb(4, modes[m]);
      FillSampleLine(lb.line, modes[m], 4, 0, pcases[i].orig,
                     pcases[i].is_signed);
      out.put(0, lb.line, 0);
    }
    // Also exercise convert_*_to_bytes (via PGM with forced <= 8).
    if (pcases[i].forced <= 8) {
      for (int m = 0; m < 5; ++m) {
        kdu_image_dims dims;
        dims.add_component(1, 4, pcases[i].orig, pcases[i].is_signed);
        dims.set_forced_precision(0, pcases[i].forced, pcases[i].align_lsbs);
        int next = 0;
        bool vflip = false;
        kdu_image_out out(dir.File("pcb_" + std::to_string(i) + "_" +
                                   std::to_string(m) + ".pgm")
                              .c_str(),
                          dims, next, vflip, true);
        ImageOutGuard guard(out);
        OwnedLineBuf lb(4, modes[m]);
        FillSampleLine(lb.line, modes[m], 4, 0, pcases[i].orig,
                       pcases[i].is_signed);
        out.put(0, lb.line, 0);
      }
    }
  }

  // 2) Forced exponent (`convert_*_as_floats_to_*`) with:
  //    - `orig_precision > 24` (`mantissa_downshift > 0`), `orig_precision >
  //    32`
  //    - `exponent_bits == 9` (`exp_off == 255 > 127` -> `exp_adjust > 0`)
  //    - `is_signed = true, forced_prec = 1` error
  //    - output precisions 6 (<8), 12 (word), 20 (>16)
  for (SampleMode mode :
       {SampleMode::kAbs16, SampleMode::kAbs32, SampleMode::kAbs64}) {
    int src_bits = (mode == SampleMode::kAbs16) ? 16 : 30;
    for (int out_prec : {6, 12, 20}) {
      for (int exp_bits : {5, 9}) {
        if (exp_bits >= src_bits - 1) continue;
        for (bool is_signed : {false, true}) {
          kdu_image_dims dims;
          dims.add_component(1, 4, src_bits, is_signed);
          dims.set_forced_precision(0, out_prec, false, exp_bits);
          int next = 0;
          bool vflip = false;
          kdu_image_out out(
              dir.File("fexp_" + std::to_string(static_cast<int>(mode)) + "_" +
                       std::to_string(out_prec) + "_" +
                       std::to_string(exp_bits) + "_" +
                       std::to_string(is_signed) + ".pgm")
                  .c_str(),
              dims, next, vflip, true);
          ImageOutGuard guard(out);
          OwnedLineBuf lb(4, mode);
          FillSampleLine(lb.line, mode, 4, 0, src_bits, is_signed, exp_bits);
          out.put(0, lb.line, 0);
        }
      }
    }
    // Also TIFF output (`as_floats_to_floats`) with exp_bits=9 & src_bits=36.
    int tif_bits = (mode == SampleMode::kAbs16) ? 16 : 36;
    for (bool is_signed : {false, true}) {
      kdu_image_dims dims;
      dims.add_component(1, 4, tif_bits, is_signed);
      dims.set_forced_precision(0, -1, false, 9);
      int next = 0;
      bool vflip = false;
      kdu_image_out out(
          dir.File("tf_big_" + std::to_string(static_cast<int>(mode)) + "_" +
                   std::to_string(is_signed) + ".tif")
              .c_str(),
          dims, next, vflip, true);
      ImageOutGuard guard(out);
      OwnedLineBuf lb(4, mode);
      FillSampleLine(lb.line, mode, 4, 0, tif_bits, is_signed, 9);
      out.put(0, lb.line, 0);
    }
  }

  // Signed float-forcing into precision < 2 generates kdu_error.
  for (int out_prec : {1}) {
    for (SampleMode mode :
         {SampleMode::kAbs16, SampleMode::kAbs32, SampleMode::kAbs64}) {
      kdu_image_dims dims;
      dims.add_component(1, 4, 16, true);
      dims.set_forced_precision(0, out_prec, false, 5);
      int next = 0;
      bool vflip = false;
      kdu_image_out out(
          dir.File("err1_" + std::to_string(static_cast<int>(mode)) + ".bmp")
              .c_str(),
          dims, next, vflip, true);
      ImageOutGuard guard(out);
      OwnedLineBuf lb(4, mode);
      FillSampleLine(lb.line, mode, 4, 0, 16, true, 5);
      EXPECT_THROW(out.put(0, lb.line, 0), kdu_exception);
    }
  }
}

}  // namespace
}  // namespace kdu_supp
