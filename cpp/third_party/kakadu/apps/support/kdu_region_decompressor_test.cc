// Unit tests for kdu_region_decompressor.cpp.

#include "third_party/kakadu/vendor/apps/support/kdu_region_decompressor.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/kakadu/vendor/apps/compressed_io/jp2.h"
#include "third_party/kakadu/vendor/apps/compressed_io/jpx.h"
#include "third_party/kakadu/vendor/apps/support/kdu_stripe_compressor.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_params.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_sample_processing.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_threads.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_utils.h"

namespace kdu_supp {
namespace {

class MemoryTarget : public kdu_core::kdu_compressed_target {
 public:
  MemoryTarget() = default;
  int get_capabilities() override { return KDU_TARGET_CAP_SEQUENTIAL; }
  bool write(const kdu_core::kdu_byte* buf, int num_bytes) override {
    if (num_bytes > 0) {
      data_.insert(data_.end(), buf, buf + num_bytes);
    }
    return true;
  }
  const std::vector<kdu_core::kdu_byte>& data() const { return data_; }

 private:
  std::vector<kdu_core::kdu_byte> data_;
};

class MemorySource : public kdu_core::kdu_compressed_source {
 public:
  explicit MemorySource(const std::vector<kdu_core::kdu_byte>& data)
      : data_(data), pos_(0) {}

  int get_capabilities() override {
    return KDU_SOURCE_CAP_SEEKABLE | KDU_SOURCE_CAP_SEQUENTIAL;
  }

  int read(kdu_core::kdu_byte* buf, int num_bytes) override {
    if (pos_ >= data_.size()) return 0;
    size_t avail = data_.size() - pos_;
    size_t to_read =
        std::min(avail, static_cast<size_t>(std::max(0, num_bytes)));
    if (to_read > 0) {
      std::memcpy(buf, data_.data() + pos_, to_read);
      pos_ += to_read;
    }
    return static_cast<int>(to_read);
  }

  bool seek(kdu_core::kdu_long offset) override {
    if (offset < 0 || static_cast<size_t>(offset) > data_.size()) {
      return false;
    }
    pos_ = static_cast<size_t>(offset);
    return true;
  }

  kdu_core::kdu_long get_pos() override {
    return static_cast<kdu_core::kdu_long>(pos_);
  }

 private:
  const std::vector<kdu_core::kdu_byte>& data_;
  size_t pos_;
};

class ThrowingMessage : public kdu_core::kdu_thread_safe_message {
 public:
  ~ThrowingMessage() noexcept override = default;
  void put_text(const char* string) override { last_ += string; }
  void flush(bool end_of_message = false) override {
    kdu_core::kdu_thread_safe_message::flush(end_of_message);
    if (end_of_message) {
      std::cerr << "KDU_ERROR: " << last_ << "\n";
      throw KDU_ERROR_EXCEPTION;
    }
  }
  const std::string& last() const { return last_; }
  void clear() { last_.clear(); }

 private:
  std::string last_;
};

class SilentMessage : public kdu_core::kdu_thread_safe_message {
 public:
  ~SilentMessage() noexcept override = default;
  void put_text(const char*) override {}
  void flush(bool = false) override {}
};

class ScopedErrorHooks {
 public:
  ScopedErrorHooks() {
    kdu_core::kdu_customize_errors(&err_);
    kdu_core::kdu_customize_warnings(&warn_);
  }
  ~ScopedErrorHooks() noexcept {
    kdu_core::kdu_customize_errors(nullptr);
    kdu_core::kdu_customize_warnings(nullptr);
  }
  const std::string& last_error() const { return err_.last(); }
  void clear() { err_.clear(); }

 private:
  ThrowingMessage err_;
  SilentMessage warn_;
};

class KduRegionDecompressorTest : public ::testing::Test {
 protected:
  void TearDown() override {
    if (!hooks_.last_error().empty()) {
      ADD_FAILURE() << "Uncaught Kakadu error: " << hooks_.last_error();
    }
  }
  ScopedErrorHooks hooks_;
};

class TrackingMembroker : public kdu_core::kdu_membroker {
 public:
  TrackingMembroker() : kdu_membroker(-1) {}
  kdu_core::kdu_int64 request(kdu_core::kdu_int64 min_bytes,
                              kdu_core::kdu_int64 pref_bytes,
                              kdu_core::kdu_int64 generator_limit) override {
    granted_ += pref_bytes;
    return pref_bytes;
  }
  void release(kdu_core::kdu_int64 returned_bytes) override {
    released_ += returned_bytes;
  }
  kdu_core::kdu_int64 granted() const { return granted_; }
  kdu_core::kdu_int64 released() const { return released_; }

 private:
  kdu_core::kdu_int64 granted_ = 0;
  kdu_core::kdu_int64 released_ = 0;
};

struct CodestreamSpec {
  int width = 32;
  int height = 32;
  int num_comps = 3;
  int bit_depth = 8;
  bool is_signed = false;
  bool reversible = true;
  bool use_ycc = false;
  std::string extra_siz;
  std::string extra_cod;
  std::string extra_crg;
  std::vector<kdu_core::kdu_int32> custom_samples;
};

void ParseMultiParams(kdu_core::kdu_params* params, std::string_view text) {
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() &&
           (text[i] == ' ' || text[i] == '\t' || text[i] == '\n')) {
      ++i;
    }
    if (i >= text.size()) break;
    size_t j = i;
    while (j < text.size() && text[j] != ' ' && text[j] != '\t' &&
           text[j] != '\n') {
      ++j;
    }
    const std::string token(text.substr(i, j - i));
    ASSERT_TRUE(params->parse_string(token.c_str())) << "token=" << token;
    i = j;
  }
}

void ConfigureSizParams(kdu_core::siz_params* siz, const CodestreamSpec& spec) {
  const std::string dims_key =
      (spec.extra_siz.find("Ssampling") != std::string::npos) ? "Ssize"
                                                              : "Sdims";
  siz->parse_string(("Scomponents=" + std::to_string(spec.num_comps)).c_str());
  siz->parse_string((dims_key + "={" + std::to_string(spec.height) + "," +
                     std::to_string(spec.width) + "}")
                        .c_str());
  siz->parse_string(("Sprecision=" + std::to_string(spec.bit_depth)).c_str());
  siz->parse_string(spec.is_signed ? "Ssigned=yes" : "Ssigned=no");
  if (!spec.extra_siz.empty()) {
    ParseMultiParams(siz, spec.extra_siz);
  }
  static_cast<kdu_core::kdu_params&>(*siz).finalize();
}

void ConfigureCodParams(kdu_core::kdu_codestream& cs,
                        const CodestreamSpec& spec) {
  kdu_core::kdu_params* cod = cs.access_siz()->access_cluster(COD_params);
  ASSERT_NE(cod, nullptr);
  cod->parse_string(spec.reversible ? "Creversible=yes" : "Creversible=no");
  if (spec.num_comps < 3 || !spec.use_ycc) {
    cod->parse_string("Cycc=no");
  } else {
    cod->parse_string("Cycc=yes");
  }
  if (!spec.extra_cod.empty()) {
    ParseMultiParams(cod, spec.extra_cod);
  }
  if (!spec.extra_crg.empty()) {
    kdu_core::kdu_params* crg = cs.access_siz()->access_cluster(CRG_params);
    ASSERT_NE(crg, nullptr);
    ParseMultiParams(crg, spec.extra_crg);
  }
  cs.access_siz()->finalize_all();
}

std::vector<kdu_core::kdu_byte> EncodeCodestream(const CodestreamSpec& spec) {
  MemoryTarget target;
  kdu_core::siz_params siz;
  ConfigureSizParams(&siz, spec);

  kdu_core::kdu_codestream cs;
  cs.create(&siz, &target);
  auto cs_cleanup = absl::MakeCleanup([&]() { cs.destroy(); });

  ConfigureCodParams(cs, spec);

  kdu_stripe_compressor comp;
  comp.start(cs);
  std::vector<int> heights(spec.num_comps);
  std::vector<int> sample_gaps(spec.num_comps, 1);
  std::vector<int> row_gaps(spec.num_comps);
  std::vector<int> precisions(spec.num_comps);
  auto is_signed_vec = std::make_unique<bool[]>(spec.num_comps);
  for (int c = 0; c < spec.num_comps; ++c) {
    kdu_core::kdu_dims dims;
    cs.get_dims(c, dims);
    heights[c] = dims.size.y;
    row_gaps[c] = dims.size.x;
    precisions[c] = cs.get_bit_depth(c);
    is_signed_vec[c] = cs.get_signed(c);
  }

  std::vector<std::vector<kdu_core::kdu_int32>> bufs(spec.num_comps);
  std::vector<kdu_core::kdu_int32*> ptrs(spec.num_comps);
  for (int c = 0; c < spec.num_comps; ++c) {
    kdu_core::kdu_dims dims;
    cs.get_dims(c, dims);
    size_t n = static_cast<size_t>(dims.size.x) * dims.size.y;
    bufs[c].resize(n);
    for (size_t i = 0; i < n; ++i) {
      if (!spec.custom_samples.empty()) {
        bufs[c][i] = spec.custom_samples[i % spec.custom_samples.size()];
      } else {
        kdu_core::kdu_int32 v = static_cast<kdu_core::kdu_int32>(
            ((i + c * 17) * 5) & ((1 << std::min(spec.bit_depth, 15)) - 1));
        if (spec.is_signed) {
          v -= (1 << std::min(spec.bit_depth - 1, 14));
        }
        bufs[c][i] = v;
      }
    }
    ptrs[c] = bufs[c].data();
  }
  comp.push_stripe(ptrs.data(), heights.data(), sample_gaps.data(),
                   row_gaps.data(), precisions.data(), is_signed_vec.get());
  comp.finish();
  return target.data();
}

struct Jp2Spec {
  CodestreamSpec cs_spec;
  jp2_colour_space space = JP2_sRGB_SPACE;
  bool use_palette = false;
  int palette_entries = 5;  // Intentionally non-power-of-two.
  int lut_bit_depth = 8;
  bool palette_has_alpha = false;
  bool add_opacity_alpha = false;
  bool add_premult_alpha = false;
  bool premult_consistent = true;
};

std::vector<kdu_core::kdu_byte> EncodeJp2(const Jp2Spec& spec) {
  MemoryTarget raw_tgt;
  jp2_family_tgt fam_tgt;
  jp2_target jp2_out;
  kdu_core::kdu_codestream cs;
  auto cleanup = absl::MakeCleanup([&]() {
    if (cs.exists()) cs.destroy();
    jp2_out.close();
    fam_tgt.close();
  });

  fam_tgt.open(&raw_tgt);
  jp2_out.open(&fam_tgt);

  kdu_core::siz_params siz;
  ConfigureSizParams(&siz, spec.cs_spec);

  cs.create(&siz, &jp2_out);
  ConfigureCodParams(cs, spec.cs_spec);

  jp2_dimensions dims = jp2_out.access_dimensions();
  dims.init(cs.access_siz(), false);
  dims.finalize_compatibility(cs.access_siz());

  jp2_colour colr = jp2_out.access_colour();
  colr.init(spec.space);

  int num_colours = 3;
  if (spec.space == JP2_sLUM_SPACE || spec.space == JP2_bilevel1_SPACE) {
    num_colours = 1;
  } else if (spec.space == JP2_CMYK_SPACE || spec.space == JP2_YCCK_SPACE) {
    num_colours = 4;
  }
  jp2_channels chnl = jp2_out.access_channels();
  if (spec.use_palette) {
    jp2_palette plt = jp2_out.access_palette();
    int num_luts = spec.palette_has_alpha ? 4 : 3;
    plt.init(num_luts, spec.palette_entries);
    std::vector<kdu_core::kdu_int32> lut(spec.palette_entries);
    for (int l = 0; l < num_luts; ++l) {
      for (int i = 0; i < spec.palette_entries; ++i) {
        kdu_core::kdu_int32 base = (i * 51 + l * 31) & 0xFF;
        lut[i] = (spec.lut_bit_depth > 8) ? (base << (spec.lut_bit_depth - 8))
                                          : base;
      }
      plt.set_lut(l, lut.data(), spec.lut_bit_depth, false);
    }
    chnl.init(3);
    for (int c = 0; c < 3; ++c) {
      chnl.set_colour_mapping(c, 0, c);
    }
    if (spec.palette_has_alpha) {
      chnl.set_opacity_mapping(-1, 0, 3);
    }
  } else {
    chnl.init(num_colours);
    for (int c = 0; c < num_colours; ++c) {
      chnl.set_colour_mapping(c, c);
    }
    if (spec.add_opacity_alpha && spec.cs_spec.num_comps > num_colours) {
      chnl.set_opacity_mapping(-1, num_colours);
    } else if (spec.add_premult_alpha && spec.cs_spec.num_comps > num_colours) {
      if (spec.premult_consistent) {
        chnl.set_premult_mapping(-1, num_colours);
      } else {
        chnl.set_premult_mapping(0, num_colours);
      }
    }
  }

  jp2_out.write_header();
  jp2_out.open_codestream();

  kdu_stripe_compressor comp;
  comp.start(cs);
  std::vector<int> heights(spec.cs_spec.num_comps);
  std::vector<int> sample_gaps(spec.cs_spec.num_comps, 1);
  std::vector<int> row_gaps(spec.cs_spec.num_comps);
  std::vector<int> precisions(spec.cs_spec.num_comps);
  auto is_signed_vec = std::make_unique<bool[]>(spec.cs_spec.num_comps);
  std::vector<std::vector<kdu_core::kdu_int32>> bufs(spec.cs_spec.num_comps);
  std::vector<kdu_core::kdu_int32*> ptrs(spec.cs_spec.num_comps);
  for (int c = 0; c < spec.cs_spec.num_comps; ++c) {
    kdu_core::kdu_dims cdims;
    cs.get_dims(c, cdims);
    heights[c] = cdims.size.y;
    row_gaps[c] = cdims.size.x;
    precisions[c] = cs.get_bit_depth(c);
    is_signed_vec[c] = cs.get_signed(c);
    size_t n = static_cast<size_t>(cdims.size.x) * cdims.size.y;
    bufs[c].resize(n);
    for (size_t i = 0; i < n; ++i) {
      if (spec.use_palette) {
        bufs[c][i] = static_cast<kdu_core::kdu_int32>(i % spec.palette_entries);
      } else {
        bufs[c][i] = static_cast<kdu_core::kdu_int32>(
            ((i + c * 29) * 7) &
            ((1 << std::min(spec.cs_spec.bit_depth, 15)) - 1));
      }
    }
    ptrs[c] = bufs[c].data();
  }
  comp.push_stripe(ptrs.data(), heights.data(), sample_gaps.data(),
                   row_gaps.data(), precisions.data(), is_signed_vec.get());
  comp.finish();
  cs.destroy();
  jp2_out.close();
  fam_tgt.close();
  return raw_tgt.data();
}

TEST_F(KduRegionDecompressorTest, GeometryHelpersAndSafeExpansionFactors) {
  kdu_core::kdu_dims cs_dims;
  cs_dims.pos = kdu_core::kdu_coords(4, 6);
  cs_dims.size = kdu_core::kdu_coords(32, 24);
  // Pathological zero/negative subsampling gets clamped to 1.
  kdu_core::kdu_dims r_clamped = kdu_region_decompressor::find_render_dims(
      cs_dims, kdu_core::kdu_coords(0, -1), kdu_core::kdu_coords(1, 1),
      kdu_core::kdu_coords(1, 1));
  EXPECT_EQ(r_clamped.pos.x, 4);
  EXPECT_EQ(r_clamped.pos.y, 6);
  EXPECT_EQ(r_clamped.size.x, 32);
  EXPECT_EQ(r_clamped.size.y, 24);

  kdu_core::kdu_coords p0 = kdu_region_decompressor::find_codestream_point(
      kdu_core::kdu_coords(10, 12), kdu_core::kdu_coords(0, 0),
      kdu_core::kdu_coords(3, 5), kdu_core::kdu_coords(2, 2), true);
  EXPECT_GT(p0.x, 0);
  kdu_core::kdu_coords p1 = kdu_region_decompressor::find_codestream_point(
      kdu_core::kdu_coords(10, 12), kdu_core::kdu_coords(2, 2),
      kdu_core::kdu_coords(3, 4), kdu_core::kdu_coords(2, 2), true);
  EXPECT_GT(p1.x, 0);
  kdu_core::kdu_coords p1_exact =
      kdu_region_decompressor::find_codestream_point(
          kdu_core::kdu_coords(10, 12), kdu_core::kdu_coords(2, 2),
          kdu_core::kdu_coords(3, 4), kdu_core::kdu_coords(2, 2), false);
  EXPECT_EQ(p1_exact.x % 2, 0);

  kdu_core::kdu_coords rp0 = kdu_region_decompressor::find_render_point(
      kdu_core::kdu_coords(10, 12), kdu_core::kdu_coords(0, 0),
      kdu_core::kdu_coords(3, 5), kdu_core::kdu_coords(2, 2), true);
  kdu_core::kdu_coords rp1 = kdu_region_decompressor::find_render_point(
      kdu_core::kdu_coords(10, 12), kdu_core::kdu_coords(2, 2),
      kdu_core::kdu_coords(3, 4), kdu_core::kdu_coords(2, 2), true);
  EXPECT_GT(rp0.x, 0);
  EXPECT_GT(rp1.x, 0);

  kdu_core::kdu_dims rc = kdu_region_decompressor::find_render_cover_dims(
      cs_dims, kdu_core::kdu_coords(2, 2), kdu_core::kdu_coords(3, 3),
      kdu_core::kdu_coords(2, 2), false);
  EXPECT_FALSE(rc.is_empty());

  kdu_core::kdu_dims cc0 = kdu_region_decompressor::find_codestream_cover_dims(
      r_clamped, kdu_core::kdu_coords(0, 0), kdu_core::kdu_coords(3, 5),
      kdu_core::kdu_coords(2, 2), false);
  kdu_core::kdu_dims cc1 = kdu_region_decompressor::find_codestream_cover_dims(
      r_clamped, kdu_core::kdu_coords(2, 2), kdu_core::kdu_coords(3, 4),
      kdu_core::kdu_coords(2, 2), true);
  EXPECT_FALSE(cc0.is_empty());
  EXPECT_FALSE(cc1.is_empty());

  // Exercise 64-bit shift loop inside allow_fractional_mapping.
  kdu_core::kdu_coords large_num(1 << 30, 1 << 30);
  kdu_core::kdu_coords large_den(1 << 29, 1 << 29);
  kdu_core::kdu_coords p_big = kdu_region_decompressor::find_codestream_point(
      kdu_core::kdu_coords(8, 8), kdu_core::kdu_coords(8, 8), large_num,
      large_den, true);
  kdu_core::kdu_coords r_big = kdu_region_decompressor::find_render_point(
      kdu_core::kdu_coords(8, 8), kdu_core::kdu_coords(8, 8), large_num,
      large_den, true);
  kdu_core::kdu_dims c_big =
      kdu_region_decompressor::find_codestream_cover_dims(
          cs_dims, kdu_core::kdu_coords(8, 8), large_num, large_den, true);
  EXPECT_GE(p_big.x, 0);
  EXPECT_GE(r_big.x, 0);
  EXPECT_FALSE(c_big.is_empty());

  // Exercise get_safe_expansion_factors and get_rendered_image_dims.
  CodestreamSpec spec;
  spec.num_comps = 3;
  spec.extra_siz = "Ssampling={1,1},{1,2},{1,2}";
  auto bytes = EncodeCodestream(spec);
  MemorySource src(bytes);
  kdu_core::kdu_codestream cs;
  cs.create(&src);
  auto cs_cleanup = absl::MakeCleanup([&]() { cs.destroy(); });

  double min_prod = 0.0, max_x = 0.0, max_y = 0.0;
  kdu_region_decompressor::get_safe_expansion_factors(cs, nullptr, 0, 0,
                                                      min_prod, max_x, max_y);
  EXPECT_GT(min_prod, 0.0);
  EXPECT_GT(max_x, 1.0);
  EXPECT_GT(max_y, 1.0);

  kdu_channel_mapping map;
  map.set_num_channels(3);
  map.source_components[0] = 0;
  map.source_components[1] = 1;
  map.source_components[2] = 2;
  kdu_region_decompressor::get_safe_expansion_factors(cs, &map, 0, 0, min_prod,
                                                      max_x, max_y);
  EXPECT_GT(min_prod, 0.0);

  kdu_channel_mapping empty_map;
  kdu_region_decompressor::get_safe_expansion_factors(cs, &empty_map, 0, 0,
                                                      min_prod, max_x, max_y);
  EXPECT_DOUBLE_EQ(min_prod, 1.0);

  kdu_region_decompressor decomp;
  EXPECT_TRUE(decomp
                  .get_rendered_image_dims(cs, &empty_map, 0, 0,
                                           kdu_core::kdu_coords(1, 1))
                  .is_empty());
  kdu_core::kdu_dims rdims = decomp.get_rendered_image_dims(
      cs, nullptr, 0, 0, kdu_core::kdu_coords(0, 0),
      kdu_core::kdu_coords(0, 0));
  EXPECT_EQ(rdims.size.x, 32);
  EXPECT_EQ(rdims.size.y, 32);
  EXPECT_TRUE(decomp.get_rendered_image_dims().is_empty());
}

TEST_F(KduRegionDecompressorTest, ChannelInterpAndMappingApi) {
  kdu_channel_interp interp;
  EXPECT_TRUE(interp.init(0, false, -0.5f));
  EXPECT_EQ(interp.orig_prec, 1);
  EXPECT_FLOAT_EQ(interp.zeta, 0.0f);

  EXPECT_TRUE(interp.init(8, false, 1.5f));
  EXPECT_FLOAT_EQ(interp.zeta, 0.75f);

  int fix_params[1] = {2};
  EXPECT_TRUE(
      interp.init(16, true, 0.25f, JP2_CHANNEL_FORMAT_FIXPOINT, fix_params));
  EXPECT_EQ(interp.fixpoint_int_bits, 2);

  int flt_params[1] = {5};
  EXPECT_TRUE(
      interp.init(16, true, 0.0f, JP2_CHANNEL_FORMAT_FLOAT, flt_params));
  EXPECT_EQ(interp.float_exp_bits, 5);

  int bad_flt[1] = {16};
  EXPECT_FALSE(interp.init(16, true, 0.0f, JP2_CHANNEL_FORMAT_FLOAT, bad_flt));
  int zero_flt[1] = {0};
  EXPECT_FALSE(interp.init(16, true, 0.0f, JP2_CHANNEL_FORMAT_FLOAT, zero_flt));
  EXPECT_FALSE(interp.init(8, false, 0.0f, 999, nullptr));

  // kdu_channel_mapping simple and codestream configuration.
  kdu_channel_mapping mapping;
  EXPECT_TRUE(mapping.configure(3, 10, true));
  EXPECT_EQ(mapping.get_num_channels(), 3);
  EXPECT_EQ(mapping.get_num_colour_channels(), 3);
  EXPECT_EQ(mapping.get_source_component(1), 0);
  EXPECT_EQ(mapping.get_default_rendering_precision(2), 10);
  EXPECT_TRUE(mapping.get_default_rendering_signed(0));
  EXPECT_NE(mapping.get_colour_converter(), nullptr);

  // Grow and shrink num_channels, plus excess error.
  mapping.set_num_channels(5);
  EXPECT_EQ(mapping.get_num_channels(), 5);
  mapping.set_num_channels(2);
  EXPECT_EQ(mapping.get_num_channels(), 2);
  EXPECT_THROW(mapping.set_num_channels(8193), kdu_core::kdu_exception);
  hooks_.clear();

  // Subsampling mismatch falls back to 1 channel in configure(codestream).
  CodestreamSpec subs_spec;
  subs_spec.num_comps = 3;
  subs_spec.extra_siz = "Ssampling={1,1},{1,2},{1,2}";
  auto subs_bytes = EncodeCodestream(subs_spec);
  MemorySource subs_src(subs_bytes);
  kdu_core::kdu_codestream subs_cs;
  subs_cs.create(&subs_src);
  auto subs_cleanup = absl::MakeCleanup([&]() { subs_cs.destroy(); });
  EXPECT_TRUE(mapping.configure(subs_cs));
  EXPECT_EQ(mapping.get_num_channels(), 1);
}

TEST_F(KduRegionDecompressorTest, Jp2PaletteAlphaAndColourSpaces) {
  // 1. Palette with alpha via JP2.
  {
    Jp2Spec pspec;
    pspec.cs_spec.num_comps = 1;
    pspec.cs_spec.width = 24;
    pspec.cs_spec.height = 24;
    pspec.space = JP2_sRGB_SPACE;
    pspec.use_palette = true;
    pspec.palette_entries = 5;
    pspec.palette_has_alpha = true;
    auto jp2_bytes = EncodeJp2(pspec);

    jp2_family_src fam_src;
    jp2_source jp2_in;
    kdu_core::kdu_codestream cs;
    auto cleanup = absl::MakeCleanup([&]() {
      if (cs.exists()) cs.destroy();
      jp2_in.close();
      fam_src.close();
    });

    MemorySource mem_src(jp2_bytes);
    fam_src.open(&mem_src);
    ASSERT_TRUE(jp2_in.open(&fam_src));
    ASSERT_GT(jp2_in.read_header(), 0);
    cs.create(&jp2_in);
    cs.set_persistent();

    kdu_channel_mapping mapping;
    ASSERT_TRUE(mapping.configure(&jp2_in, false));
    EXPECT_EQ(mapping.get_num_colour_channels(), 3);
    EXPECT_EQ(mapping.get_num_channels(), 4);

    // Also test codestream_idx mismatch returns false.
    EXPECT_FALSE(
        mapping.configure(jp2_in.access_colour(), jp2_in.access_channels(), 1,
                          jp2_in.access_palette(), jp2_in.access_dimensions()));
    ASSERT_TRUE(
        mapping.configure(jp2_in.access_colour(), jp2_in.access_channels(), 0,
                          jp2_in.access_palette(), jp2_in.access_dimensions()));
    ASSERT_TRUE(mapping.add_alpha_to_configuration(
        jp2_in.access_channels(), 0, jp2_in.access_palette(),
        jp2_in.access_dimensions(), true));

    // Decompress palette RGBA through precise=false (fix16) and precise=true.
    for (bool precise : {false, true}) {
      kdu_region_decompressor decomp;
      kdu_core::kdu_dims region = decomp.get_rendered_image_dims(
          cs, &mapping, 0, 0, kdu_core::kdu_coords(1, 1));
      ASSERT_TRUE(decomp.start(cs, &mapping, 0, 0, 1, region,
                               kdu_core::kdu_coords(1, 1),
                               kdu_core::kdu_coords(1, 1), precise));
      std::vector<kdu_core::kdu_int32> packed(region.area(), 0);
      kdu_core::kdu_dims inc = region, nr;
      while (!inc.is_empty()) {
        ASSERT_TRUE(decomp.process(packed.data(), region.pos, region.size.x,
                                   256, 256, inc, nr));
      }
      EXPECT_TRUE(decomp.finish());
      EXPECT_NE(packed[0], 0);
    }

    // Palette with boxcar integration (> 3x reduction).
    kdu_region_decompressor dec_box;
    kdu_core::kdu_dims b_region = dec_box.get_rendered_image_dims(
        cs, &mapping, 0, 0, kdu_core::kdu_coords(1, 1),
        kdu_core::kdu_coords(4, 4));
    ASSERT_TRUE(dec_box.start(cs, &mapping, 0, 0, 1, b_region,
                              kdu_core::kdu_coords(1, 1),
                              kdu_core::kdu_coords(4, 4)));
    std::vector<kdu_core::kdu_byte> c0(b_region.area()), c1(b_region.area()),
        c2(b_region.area()), c3(b_region.area());
    kdu_core::kdu_byte* ptrs[4] = {c0.data(), c1.data(), c2.data(), c3.data()};
    kdu_core::kdu_dims inc = b_region, nr;
    while (!inc.is_empty()) {
      ASSERT_TRUE(dec_box.process(ptrs, false, 1, b_region.pos, b_region.size.x,
                                  64, 64, inc, nr));
    }
    EXPECT_TRUE(dec_box.finish());
  }

  // 2. YCbCr1 colour conversion + opacity alpha + premultiplied alpha.
  for (bool premult : {false, true}) {
    SCOPED_TRACE(premult ? "premult=true" : "premult=false");
    Jp2Spec spec;
    spec.cs_spec.num_comps = 4;
    spec.cs_spec.width = 24;
    spec.cs_spec.height = 24;
    spec.space = JP2_sYCC_SPACE;
    spec.add_opacity_alpha = !premult;
    spec.add_premult_alpha = premult;
    spec.premult_consistent = true;
    auto jp2_bytes = EncodeJp2(spec);

    jp2_family_src fam_src;
    jp2_source jp2_in;
    kdu_core::kdu_codestream cs;
    auto cleanup = absl::MakeCleanup([&]() {
      if (cs.exists()) cs.destroy();
      jp2_in.close();
      fam_src.close();
    });

    MemorySource mem_src(jp2_bytes);
    fam_src.open(&mem_src);
    ASSERT_TRUE(jp2_in.open(&fam_src));
    ASSERT_GT(jp2_in.read_header(), 0);
    cs.create(&jp2_in);
    cs.set_persistent();

    kdu_channel_mapping mapping;
    ASSERT_TRUE(mapping.configure(&jp2_in, false));
    if (premult) {
      EXPECT_EQ(mapping.get_num_channels(), 3);
      EXPECT_FALSE(mapping.add_alpha_to_configuration(
          jp2_in.access_channels(), 0, jp2_in.access_palette(),
          jp2_in.access_dimensions(), true));
      EXPECT_TRUE(mapping.add_alpha_to_configuration(
          jp2_in.access_channels(), 0, jp2_in.access_palette(),
          jp2_in.access_dimensions(), false));
    }
    EXPECT_EQ(mapping.get_num_channels(), 4);

    kdu_region_decompressor decomp;
    decomp.set_white_stretch(8);
    decomp.set_true_scaling(true, true);
    kdu_core::kdu_dims region = decomp.get_rendered_image_dims(
        cs, &mapping, 0, 0, kdu_core::kdu_coords(1, 1));
    ASSERT_TRUE(decomp.start(cs, &mapping, 0, 0, 1, region,
                             kdu_core::kdu_coords(1, 1)));
    std::vector<kdu_core::kdu_byte> ilv(region.area() * 4);
    int offsets[4] = {0, 1, 2, 3};
    kdu_core::kdu_dims inc = region, nr;
    while (!inc.is_empty()) {
      ASSERT_TRUE(decomp.process(ilv.data(), offsets, 4, region.pos, 0, 128,
                                 region.area(), inc, nr));
    }
    EXPECT_TRUE(decomp.finish());
  }

  // 3. Inconsistent premultiplied alpha returns false from add_alpha.
  {
    Jp2Spec spec;
    spec.cs_spec.num_comps = 4;
    spec.space = JP2_sRGB_SPACE;
    spec.add_premult_alpha = true;
    spec.premult_consistent = false;
    auto jp2_bytes = EncodeJp2(spec);
    jp2_family_src fam_src;
    jp2_source jp2_in;
    auto cleanup = absl::MakeCleanup([&]() {
      jp2_in.close();
      fam_src.close();
    });
    MemorySource mem_src(jp2_bytes);
    fam_src.open(&mem_src);
    ASSERT_TRUE(jp2_in.open(&fam_src));
    ASSERT_GT(jp2_in.read_header(), 0);
    kdu_channel_mapping mapping;
    ASSERT_TRUE(mapping.configure(&jp2_in, true));
    EXPECT_FALSE(mapping.add_alpha_to_configuration(
        jp2_in.access_channels(), 0, jp2_in.access_palette(),
        jp2_in.access_dimensions(), false));
  }
}

TEST_F(KduRegionDecompressorTest, AllProcessOverloadsAndOptions) {
  // 1-component codestream (signed and unsigned variants).
  for (bool is_signed : {false, true}) {
    CodestreamSpec spec;
    spec.num_comps = 1;
    spec.width = 20;
    spec.height = 16;
    spec.bit_depth = 10;
    spec.is_signed = is_signed;
    spec.reversible = false;
    auto bytes = EncodeCodestream(spec);
    MemorySource src(bytes);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    cs.set_persistent();
    auto cleanup = absl::MakeCleanup([&]() { cs.destroy(); });

    kdu_channel_mapping map;
    ASSERT_TRUE(map.configure(cs));

    // byte** with expand_monochrome=true, null channel pointer, row_gap=0.
    {
      kdu_region_decompressor decomp;
      decomp.set_true_scaling(true, is_signed);
      kdu_core::kdu_dims region = decomp.get_rendered_image_dims(
          cs, &map, 0, 0, kdu_core::kdu_coords(1, 1));
      ASSERT_TRUE(
          decomp.start(cs, &map, 0, 0, 1, region, kdu_core::kdu_coords(1, 1)));
      std::vector<kdu_core::kdu_byte> r(region.area()), b(region.area());
      kdu_core::kdu_byte* bufs[3] = {r.data(), nullptr, b.data()};
      kdu_core::kdu_dims inc = region, nr;
      while (!inc.is_empty()) {
        ASSERT_TRUE(decomp.process(bufs, true, 1, region.pos, 0, 0,
                                   region.area(), inc, nr, 0, false));
      }
      EXPECT_TRUE(decomp.finish());
    }

    // uint16** with expand_monochrome=true, null pointer, precision_bits <= 0.
    {
      kdu_region_decompressor decomp;
      decomp.set_true_scaling(false, true);
      kdu_core::kdu_dims region = decomp.get_rendered_image_dims(
          cs, &map, 0, 0, kdu_core::kdu_coords(1, 1));
      ASSERT_TRUE(
          decomp.start(cs, &map, 0, 0, 1, region, kdu_core::kdu_coords(1, 1)));
      std::vector<kdu_core::kdu_uint16> r(region.area()), g(region.area());
      kdu_core::kdu_uint16* bufs[3] = {r.data(), g.data(), nullptr};
      kdu_core::kdu_dims inc = region, nr;
      while (!inc.is_empty()) {
        ASSERT_TRUE(decomp.process(bufs, true, 1, region.pos, region.size.x,
                                   100, 100, inc, nr, 0, true));
      }
      EXPECT_TRUE(decomp.finish());
    }

    // float** with expand_monochrome, normalize={true,false}, always_clip.
    for (bool norm : {true, false}) {
      kdu_region_decompressor decomp;
      decomp.set_true_scaling(true, true);
      kdu_core::kdu_dims region = decomp.get_rendered_image_dims(
          cs, &map, 0, 0, kdu_core::kdu_coords(1, 1));
      ASSERT_TRUE(decomp.start(cs, &map, 0, 0, 1, region,
                               kdu_core::kdu_coords(1, 1),
                               kdu_core::kdu_coords(1, 1), true));
      std::vector<float> r(region.area()), b(region.area());
      float* bufs[3] = {r.data(), nullptr, b.data()};
      kdu_core::kdu_dims inc = region, nr;
      while (!inc.is_empty()) {
        ASSERT_TRUE(decomp.process(bufs, true, 1, region.pos, region.size.x,
                                   100, 100, inc, nr, norm, true, false));
      }
      EXPECT_TRUE(decomp.finish());
    }

    // Packed int32 with monochrome replication + alpha fill.
    {
      kdu_region_decompressor decomp;
      kdu_core::kdu_dims region = decomp.get_rendered_image_dims(
          cs, &map, 0, 0, kdu_core::kdu_coords(1, 1));
      ASSERT_TRUE(
          decomp.start(cs, &map, 0, 0, 1, region, kdu_core::kdu_coords(1, 1)));
      std::vector<kdu_core::kdu_int32> packed(region.area());
      kdu_core::kdu_dims inc = region, nr;
      while (!inc.is_empty()) {
        ASSERT_TRUE(decomp.process(packed.data(), region.pos, region.size.x,
                                   200, 200, inc, nr));
      }
      EXPECT_TRUE(decomp.finish());
    }

    // Interleaved byte*, uint16*, float* with expand_mono + fill_alpha.
    {
      kdu_region_decompressor decomp;
      kdu_core::kdu_dims region = decomp.get_rendered_image_dims(
          cs, &map, 0, 0, kdu_core::kdu_coords(1, 1));
      ASSERT_TRUE(
          decomp.start(cs, &map, 0, 0, 1, region, kdu_core::kdu_coords(1, 1)));
      std::vector<kdu_core::kdu_byte> ib(region.area() * 4);
      int off[4] = {0, 1, 2, 3};
      kdu_core::kdu_dims inc = region, nr;
      while (!inc.is_empty()) {
        ASSERT_TRUE(decomp.process(ib.data(), off, 4, region.pos, region.size.x,
                                   128, 128, inc, nr, 6, true, 3, 1, 0));
      }
      EXPECT_TRUE(decomp.finish());

      ASSERT_TRUE(
          decomp.start(cs, &map, 0, 0, 1, region, kdu_core::kdu_coords(1, 1)));
      std::vector<kdu_core::kdu_uint16> iw(region.area() * 4);
      inc = region;
      while (!inc.is_empty()) {
        ASSERT_TRUE(decomp.process(iw.data(), off, 4, region.pos, region.size.x,
                                   128, 128, inc, nr, 12, true, 3, 1, 0));
      }
      EXPECT_TRUE(decomp.finish());

      ASSERT_TRUE(decomp.start(cs, &map, 0, 0, 1, region,
                               kdu_core::kdu_coords(1, 1),
                               kdu_core::kdu_coords(1, 1), true));
      std::vector<float> iflt(region.area() * 4);
      inc = region;
      while (!inc.is_empty()) {
        ASSERT_TRUE(decomp.process(iflt.data(), off, 4, region.pos,
                                   region.size.x, 128, 128, inc, nr, false,
                                   true, 3, 1, 0, false));
      }
      EXPECT_TRUE(decomp.finish());
    }
  }

  // 3-component max_colour_channels < 3 for interleaved overloads.
  {
    CodestreamSpec spec;
    spec.num_comps = 3;
    spec.width = 24;
    spec.height = 16;
    spec.bit_depth = 12;
    spec.reversible = false;
    auto bytes = EncodeCodestream(spec);
    MemorySource src(bytes);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    cs.set_persistent();
    auto cleanup = absl::MakeCleanup([&]() { cs.destroy(); });
    kdu_channel_mapping map;
    ASSERT_TRUE(map.configure(cs));
    kdu_region_decompressor decomp;
    kdu_core::kdu_dims region = decomp.get_rendered_image_dims(
        cs, &map, 0, 0, kdu_core::kdu_coords(1, 1));
    int off[3] = {0, 1, 2};
    kdu_core::kdu_dims inc, nr;

    ASSERT_TRUE(
        decomp.start(cs, &map, 0, 0, 1, region, kdu_core::kdu_coords(1, 1)));
    std::vector<kdu_core::kdu_byte> ib(region.area() * 2);
    inc = region;
    while (!inc.is_empty()) {
      ASSERT_TRUE(decomp.process(ib.data(), off, 2, region.pos, region.size.x,
                                 200, 200, inc, nr, 0, true, 1, 0, 2));
    }
    EXPECT_TRUE(decomp.finish());

    ASSERT_TRUE(
        decomp.start(cs, &map, 0, 0, 1, region, kdu_core::kdu_coords(1, 1)));
    std::vector<kdu_core::kdu_uint16> iw(region.area() * 2);
    inc = region;
    while (!inc.is_empty()) {
      ASSERT_TRUE(decomp.process(iw.data(), off, 2, region.pos, region.size.x,
                                 200, 200, inc, nr, 0, true, 1, 0, 2));
    }
    EXPECT_TRUE(decomp.finish());

    ASSERT_TRUE(
        decomp.start(cs, &map, 0, 0, 1, region, kdu_core::kdu_coords(1, 1)));
    std::vector<float> iflt(region.area() * 2);
    inc = region;
    while (!inc.is_empty()) {
      ASSERT_TRUE(decomp.process(iflt.data(), off, 2, region.pos, region.size.x,
                                 200, 200, inc, nr, true, true, 1, 0, 2, true));
    }
    EXPECT_TRUE(decomp.finish());
  }
}

TEST_F(KduRegionDecompressorTest,
       ResamplingBoxcarSubsamplingMultiTileAndThreads) {
  // Multi-tile 4:2:0 codestream, tiled into small tiles so > 8 tiles fit on
  // a row to exercise tile-line array growth, skipped tiles, and backing lines.
  CodestreamSpec spec;
  spec.num_comps = 3;
  spec.width = 80;
  spec.height = 48;
  spec.bit_depth = 8;
  spec.reversible = false;
  spec.extra_siz = "Stiles={8,8}\nSsampling={1,1},{2,2},{2,2}";
  auto bytes = EncodeCodestream(spec);
  MemorySource src(bytes);
  kdu_core::kdu_codestream cs;
  cs.create(&src);
  cs.set_persistent();
  auto cleanup = absl::MakeCleanup([&]() { cs.destroy(); });

  kdu_channel_mapping map;
  map.set_num_channels(3);
  for (int c = 0; c < 3; ++c) {
    map.source_components[c] = c;
    map.channel_interp[c].init(8, false, 0.0f);
  }

  // Exercise rational upsampling (both 2-tap and 6-tap) and >3x boxcar
  // decimation, in both fast/fix16 and precise/float modes.
  struct ResampleCase {
    kdu_core::kdu_coords num;
    kdu_core::kdu_coords den;
    bool precise;
    float max_overshoot;
    int zero_thld;
  };
  const ResampleCase cases[] = {
      {{3, 3}, {2, 2}, false, 0.25f, 3},  // 1.5x (< zero_thld -> 6-tap clamped)
      {{5, 5}, {2, 2}, true, 0.25f, 2},   // 2.5x (>= zero_thld -> 2-tap float)
      {{1, 1}, {4, 4}, false, 0.0f, 2},   // 0.25x boxcar fix16
      {{1, 1}, {4, 4}, true, 0.4f, 2},    // 0.25x boxcar float
      {{2, 3}, {5, 7}, false, 0.0f, 2},   // non-power-of-two reduction
  };

  TrackingMembroker broker;
  kdu_core::kdu_quality_limiter limiter(0.01f, false);
  kdu_region_decompressor decomp;
  decomp.mem_configure(&broker, 0);
  decomp.set_quality_limiting(&limiter, 150.0f, 150.0f);

  for (const auto& tc : cases) {
    decomp.set_interpolation_behaviour(tc.max_overshoot, tc.zero_thld);
    kdu_core::kdu_dims full =
        decomp.get_rendered_image_dims(cs, &map, 0, 0, tc.num, tc.den);
    // Use an interior sub-region to skip boundary tiles on left/top.
    kdu_core::kdu_dims sub = full;
    if (sub.size.x > 16) {
      sub.pos.x += 4;
      sub.size.x -= 8;
    }
    if (sub.size.y > 16) {
      sub.pos.y += 4;
      sub.size.y -= 8;
    }
    ASSERT_TRUE(decomp.start(cs, &map, 0, 0, 1, sub, tc.num, tc.den, tc.precise,
                             KDU_WANT_OUTPUT_COMPONENTS, !tc.precise));
    EXPECT_EQ(decomp.get_rendered_image_dims(), full);
    std::vector<kdu_core::kdu_byte> c0(sub.area()), c1(sub.area()),
        c2(sub.area());
    kdu_core::kdu_byte* bufs[3] = {c0.data(), c1.data(), c2.data()};
    kdu_core::kdu_dims inc = sub, nr;
    while (!inc.is_empty()) {
      ASSERT_TRUE(decomp.process(bufs, false, 1, sub.pos, sub.size.x, 128,
                                 sub.area(), inc, nr));
    }
    EXPECT_TRUE(decomp.finish());
    cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
  }

  // Multi-threaded run with >= 64px height to activate background_bank and
  // MT_DWT.
  CodestreamSpec tall_spec;
  tall_spec.num_comps = 3;
  tall_spec.width = 64;
  tall_spec.height = 96;
  tall_spec.extra_siz = "Stiles={32,32}";
  auto tall_bytes = EncodeCodestream(tall_spec);
  MemorySource tall_src(tall_bytes);
  kdu_core::kdu_codestream tall_cs;
  tall_cs.create(&tall_src);
  auto tall_cleanup = absl::MakeCleanup([&]() { tall_cs.destroy(); });

  kdu_core::kdu_thread_env env;
  env.create();
  env.add_thread();
  auto env_cleanup = absl::MakeCleanup([&]() { env.destroy(); });

  decomp.set_quality_limiting(nullptr, -1.0f, -1.0f);
  kdu_core::kdu_dims tall_reg = decomp.get_rendered_image_dims(
      tall_cs, &map, 0, 0, kdu_core::kdu_coords(1, 1));
  ASSERT_TRUE(decomp.start(tall_cs, &map, 0, 0, 1, tall_reg,
                           kdu_core::kdu_coords(1, 1),
                           kdu_core::kdu_coords(1, 1), false,
                           KDU_WANT_OUTPUT_COMPONENTS, false, &env, nullptr));
  std::vector<kdu_core::kdu_byte> tb0(tall_reg.area()), tb1(tall_reg.area()),
      tb2(tall_reg.area());
  kdu_core::kdu_byte* tbufs[3] = {tb0.data(), tb1.data(), tb2.data()};
  kdu_core::kdu_dims tinc = tall_reg, tnr;
  while (!tinc.is_empty()) {
    ASSERT_TRUE(decomp.process(tbufs, false, 1, tall_reg.pos, tall_reg.size.x,
                               256, tall_reg.area(), tinc, tnr));
  }
  kdu_core::kdu_exception exc = 0;
  EXPECT_TRUE(decomp.finish(&exc, true));
  EXPECT_EQ(exc, 0);
  decomp.reset();
  decomp.mem_configure(nullptr, 0);
  EXPECT_GT(broker.granted(), 0);
  EXPECT_EQ(broker.granted(), broker.released());
}

TEST_F(KduRegionDecompressorTest, HighBitDepthFloatFixpointAndErrorPaths) {
  // High bit depth (> KDU_FIX_POINT -> INT32 internal line type) +
  // reinterpret-as-float & fixpoint formats via kdu_channel_mapping.
  CodestreamSpec spec32;
  spec32.num_comps = 1;
  spec32.width = 24;
  spec32.height = 16;
  spec32.bit_depth = 20;
  spec32.is_signed = true;
  spec32.reversible = true;
  auto bytes32 = EncodeCodestream(spec32);
  MemorySource src32(bytes32);
  kdu_core::kdu_codestream cs32;
  cs32.create(&src32);
  cs32.set_persistent();
  auto c32_cleanup = absl::MakeCleanup([&]() { cs32.destroy(); });

  // INT32 direct path + resampled float fallback from INT32.
  {
    kdu_region_decompressor decomp;
    kdu_core::kdu_dims r1 = decomp.get_rendered_image_dims(
        cs32, nullptr, 0, 0, kdu_core::kdu_coords(1, 1));
    ASSERT_TRUE(
        decomp.start(cs32, nullptr, 0, 0, 1, r1, kdu_core::kdu_coords(1, 1)));
    std::vector<float> fbuf(r1.area());
    float* fptrs[1] = {fbuf.data()};
    kdu_core::kdu_dims inc = r1, nr;
    while (!inc.is_empty()) {
      ASSERT_TRUE(decomp.process(fptrs, false, 1, r1.pos, r1.size.x, 256,
                                 r1.area(), inc, nr, true, true, true));
    }
    EXPECT_TRUE(decomp.finish());

    // Boxcar reduction on high-bit-depth component.
    kdu_core::kdu_dims rb = decomp.get_rendered_image_dims(
        cs32, nullptr, 0, 0, kdu_core::kdu_coords(1, 1),
        kdu_core::kdu_coords(4, 4));
    ASSERT_TRUE(decomp.start(cs32, nullptr, 0, 0, 1, rb,
                             kdu_core::kdu_coords(1, 1),
                             kdu_core::kdu_coords(4, 4)));
    std::vector<kdu_core::kdu_uint16> wbuf(rb.area());
    kdu_core::kdu_uint16* wptrs[1] = {wbuf.data()};
    inc = rb;
    while (!inc.is_empty()) {
      ASSERT_TRUE(decomp.process(wptrs, false, 1, rb.pos, rb.size.x, 64,
                                 rb.area(), inc, nr));
    }
    EXPECT_TRUE(decomp.finish());
  }

  // Float & fixpoint kdu_channel_interp paths (20-bit float exp=6).
  {
    kdu_channel_mapping fmap;
    fmap.configure(1, 20, true);
    int flt_p[1] = {6};
    ASSERT_TRUE(fmap.channel_interp[0].init(20, true, 0.0f,
                                            JP2_CHANNEL_FORMAT_FLOAT, flt_p));
    kdu_region_decompressor decomp;
    kdu_core::kdu_dims r1 = decomp.get_rendered_image_dims(
        cs32, &fmap, 0, 0, kdu_core::kdu_coords(1, 1));
    ASSERT_TRUE(
        decomp.start(cs32, &fmap, 0, 0, 1, r1, kdu_core::kdu_coords(1, 1)));
    std::vector<float> out(r1.area());
    float* ptrs[1] = {out.data()};
    kdu_core::kdu_dims inc = r1, nr;
    while (!inc.is_empty()) {
      ASSERT_TRUE(decomp.process(ptrs, false, 1, r1.pos, r1.size.x, 256,
                                 r1.area(), inc, nr, true, true, false));
    }
    EXPECT_TRUE(decomp.finish());

    int fix_p[1] = {2};
    ASSERT_TRUE(fmap.channel_interp[0].init(
        20, true, 0.0f, JP2_CHANNEL_FORMAT_FIXPOINT, fix_p));
    ASSERT_TRUE(
        decomp.start(cs32, &fmap, 0, 0, 1, r1, kdu_core::kdu_coords(1, 1)));
    inc = r1;
    while (!inc.is_empty()) {
      ASSERT_TRUE(decomp.process(ptrs, false, 1, r1.pos, r1.size.x, 256,
                                 r1.area(), inc, nr, true, true, false));
    }
    EXPECT_TRUE(decomp.finish());
  }

  // Error checks in start/reset/mem_configure/process(packed).
  {
    kdu_region_decompressor decomp;
    kdu_channel_mapping empty_map;
    EXPECT_FALSE(decomp.start(cs32, &empty_map, 0, 0, 1, kdu_core::kdu_dims(),
                              kdu_core::kdu_coords(1, 1)));
    hooks_.clear();

    kdu_core::kdu_dims r1 = decomp.get_rendered_image_dims(
        cs32, nullptr, 0, 0, kdu_core::kdu_coords(1, 1));
    // Region outside full image returns false via catch.
    kdu_core::kdu_dims oob;
    oob.pos = kdu_core::kdu_coords(100, 100);
    oob.size = kdu_core::kdu_coords(4, 4);
    EXPECT_FALSE(
        decomp.start(cs32, nullptr, 0, 0, 1, oob, kdu_core::kdu_coords(1, 1)));
    hooks_.clear();

    // Valid start, then trigger dev error on mem_configure, reset,
    // get_rendered_image_dims, and double start (after a process call populates
    // tile_banks).
    ASSERT_TRUE(
        decomp.start(cs32, nullptr, 0, 0, 1, r1, kdu_core::kdu_coords(1, 1)));
    kdu_core::kdu_dims inc_pre = r1, nr_pre;
    std::vector<kdu_core::kdu_byte> pre_buf(r1.area());
    kdu_core::kdu_byte* pre_ptrs[1] = {pre_buf.data()};
    ASSERT_TRUE(decomp.process(pre_ptrs, false, 1, r1.pos, r1.size.x, 1,
                               r1.size.x, inc_pre, nr_pre));
    EXPECT_THROW(decomp.mem_configure(nullptr, 0), kdu_core::kdu_exception);
    hooks_.clear();
    EXPECT_THROW(decomp.reset(), kdu_core::kdu_exception);
    hooks_.clear();
    EXPECT_THROW(decomp.get_rendered_image_dims(cs32, nullptr, 0, 0,
                                                kdu_core::kdu_coords(1, 1)),
                 kdu_core::kdu_exception);
    hooks_.clear();
    EXPECT_THROW(
        decomp.start(cs32, nullptr, 0, 0, 1, r1, kdu_core::kdu_coords(1, 1)),
        kdu_core::kdu_exception);
    hooks_.clear();
    // Finish cleanly first, then use a separate codestream for failure path.
    EXPECT_TRUE(decomp.finish());
    cs32.apply_input_restrictions(0, 0, 0, 0, nullptr);

    MemorySource err_src(bytes32);
    kdu_core::kdu_codestream err_cs;
    err_cs.create(&err_src);
    auto err_cleanup = absl::MakeCleanup([&]() { err_cs.destroy(); });
    ASSERT_TRUE(
        decomp.start(err_cs, nullptr, 0, 0, 1, r1, kdu_core::kdu_coords(1, 1)));
    // max_region_pixels too small (< 1 line).
    kdu_core::kdu_dims inc = r1, nr;
    std::vector<kdu_core::kdu_byte> tiny(4);
    kdu_core::kdu_byte* ptrs[1] = {tiny.data()};
    EXPECT_FALSE(decomp.process(ptrs, false, 1, r1.pos, 0, 0, 1, inc, nr));
    hooks_.clear();
    EXPECT_FALSE(decomp.finish());
  }

  // Packed int32 with 2 colour channels throws.
  {
    kdu_channel_mapping two_map;
    two_map.configure(2, 8, false);
    kdu_region_decompressor decomp;
    kdu_core::kdu_dims r1 = decomp.get_rendered_image_dims(
        cs32, &two_map, 0, 0, kdu_core::kdu_coords(1, 1));
    ASSERT_TRUE(
        decomp.start(cs32, &two_map, 0, 0, 1, r1, kdu_core::kdu_coords(1, 1)));
    std::vector<kdu_core::kdu_int32> packed(r1.area());
    kdu_core::kdu_dims inc = r1, nr;
    EXPECT_THROW(
        decomp.process(packed.data(), r1.pos, r1.size.x, 64, 64, inc, nr),
        kdu_core::kdu_exception);
    hooks_.clear();
    EXPECT_TRUE(decomp.finish());
  }
}

TEST_F(KduRegionDecompressorTest, ConversionReinterpretAndBoxcarCoverage) {
  auto run_interp_case = [&](int bit_depth, bool is_signed, bool reversible,
                             int format, int fmt_param,
                             kdu_core::kdu_coords num, kdu_core::kdu_coords den,
                             bool precise, bool sub_crop,
                             bool expect_process_error = false) {
    CodestreamSpec spec;
    spec.num_comps = 1;
    spec.width = 40;
    spec.height = 40;
    spec.bit_depth = bit_depth;
    spec.is_signed = is_signed;
    spec.reversible = reversible;
    spec.extra_siz = "Stiles={12,12}\nSorigin={4,4}";
    if (bit_depth >= 24 && reversible) {
      spec.extra_cod = "Clevels=0";
    }
    // Fill in-range bit patterns (0, subnormal, normal, +inf/NaN, negative) to
    // hit underflow/overflow/clamp branches without overflowing push_stripe.
    if (format == JP2_CHANNEL_FORMAT_FLOAT) {
      if (bit_depth == 16) {
        spec.custom_samples =
            is_signed
                ? std::vector<kdu_core::kdu_int32>{0,       1,       -1,
                                                   0x3C00,  -0x3C00, 0x7C00,
                                                   -0x7C00, -32768}
                : std::vector<kdu_core::kdu_int32>{
                      0, 1, 0x3C00, 0x7BFF, 0x7C00, 0x7FFF, 0xFFFF};
      } else if (bit_depth == 20) {
        spec.custom_samples =
            is_signed
                ? std::vector<kdu_core::kdu_int32>{0,       1,        -1,
                                                   0x3FFFF, -0x3FFFF, 0x7FFFF,
                                                   -0x80000}
                : std::vector<kdu_core::kdu_int32>{0, 1, 0x3FFFF, 0x7FFFF,
                                                   0xFFFFF};
      } else if (bit_depth == 30) {
        spec.custom_samples =
            is_signed
                ? std::vector<kdu_core::kdu_int32>{0,           1,
                                                   -1,          0x1F800000,
                                                   -0x1F800000, 0x1FFFFFFF,
                                                   -0x20000000}
                : std::vector<kdu_core::kdu_int32>{0, 1, 0x1F800000, 0x3F000000,
                                                   0x3FFFFFFF};
      } else if (bit_depth >= 32) {
        spec.custom_samples =
            is_signed
                ? std::vector<
                      kdu_core::kdu_int32>{0,
                                           1,
                                           -1,
                                           0x3F800000,
                                           static_cast<kdu_core::kdu_int32>(
                                               0xBF800000u),
                                           0x7F800000,
                                           static_cast<kdu_core::kdu_int32>(
                                               0xFF800000u)}
                : std::vector<kdu_core::kdu_int32>{
                      0,
                      1,
                      0x3F800000,
                      0x7F800000,
                      static_cast<kdu_core::kdu_int32>(0x80000000u),
                      static_cast<kdu_core::kdu_int32>(0xFFFFFFFFu)};
      }
    }
    auto bytes = EncodeCodestream(spec);
    MemorySource src(bytes);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    cs.set_persistent();
    auto cleanup = absl::MakeCleanup([&]() { cs.destroy(); });

    kdu_channel_mapping map;
    map.configure(1, bit_depth, is_signed);
    if (format != JP2_CHANNEL_FORMAT_DEFAULT) {
      int params[1] = {fmt_param};
      ASSERT_TRUE(map.channel_interp[0].init(bit_depth, is_signed, 0.0f, format,
                                             params));
    }

    kdu_region_decompressor decomp;
    kdu_core::kdu_dims full =
        decomp.get_rendered_image_dims(cs, &map, 0, 0, num, den);
    kdu_core::kdu_dims reg = full;
    if (sub_crop && reg.size.x > 10 && reg.size.y > 10) {
      reg.pos.x += 5;
      reg.pos.y += 4;
      reg.size.x -= 8;
      reg.size.y -= 7;
    }

    ASSERT_TRUE(decomp.start(cs, &map, 0, 0, 1, reg, num, den, precise));
    std::vector<float> fbuf(reg.area());
    float* ptrs[1] = {fbuf.data()};
    kdu_core::kdu_dims inc = reg, nr;
    if (expect_process_error) {
      EXPECT_FALSE(decomp.process(ptrs, false, 1, reg.pos, reg.size.x, 128,
                                  reg.area(), inc, nr, true, true, false));
      hooks_.clear();
      decomp.finish();
      return;
    }

    while (!inc.is_empty()) {
      ASSERT_TRUE(decomp.process(ptrs, false, 1, reg.pos, reg.size.x, 128,
                                 reg.area(), inc, nr, true, true, false));
    }
    EXPECT_TRUE(decomp.finish());
  };

  // Error: JP2_CHANNEL_FORMAT_FLOAT with non-absolute source (bit_depth=8).
  run_interp_case(8, false, false, JP2_CHANNEL_FORMAT_FLOAT, 5, {1, 1}, {1, 1},
                  true, false, /*expect_start_error=*/true);

  // Unsigned & signed JP2_CHANNEL_FORMAT_FLOAT across exp<=8 (SIMD), exp=9
  // (scalar), prec=30/exp=5 (mantissa downshift), prec=32/exp=10 (int64 +
  // exp_off>127), at 1:1 sub-crop, 3:2 resample, and 1:4 boxcar integration.
  for (bool is_signed : {false, true}) {
    struct FloatCfg {
      int bit_depth;
      int exp_bits;
    };
    const FloatCfg cfgs[] = {
        {16, 5},   // mantissa_upshift >= 0, exp_adjust < 0
        {20, 9},   // exp_bits > 8 -> scalar local_reinterpret_*_floats
        {30, 5},   // mantissa_bits = 24 > 23 -> mantissa_downshift > 0
        {32, 10},  // int64 source buffer + exp_adjust > 0
    };
    for (const auto& cfg : cfgs) {
      run_interp_case(cfg.bit_depth, is_signed, true, JP2_CHANNEL_FORMAT_FLOAT,
                      cfg.exp_bits, {1, 1}, {1, 1}, true, true);
      run_interp_case(cfg.bit_depth, is_signed, true, JP2_CHANNEL_FORMAT_FLOAT,
                      cfg.exp_bits, {3, 3}, {2, 2}, true, false);
      run_interp_case(cfg.bit_depth, is_signed, true, JP2_CHANNEL_FORMAT_FLOAT,
                      cfg.exp_bits, {1, 1}, {4, 4}, true, true);
      run_interp_case(cfg.bit_depth, is_signed, true, JP2_CHANNEL_FORMAT_FLOAT,
                      cfg.exp_bits, {1, 1}, {4, 4}, true, false);
    }
  }

  // Standard & fixpoint conversions: to_fix16 (int32/float/int64), to_int32
  // (int16/float/int32/int64), to_float (int16/float/int32/int64), add &
  // add_float.
  for (bool reversible : {true, false}) {
    const int depths[] = {8, 16, 28, 32};
    for (int depth : depths) {
      if (!reversible && depth > 16) continue;
      for (bool precise : {false, true}) {
        int fmt = (depth == 8 && precise) ? JP2_CHANNEL_FORMAT_FIXPOINT
                                          : JP2_CHANNEL_FORMAT_DEFAULT;
        int param = 1;
        run_interp_case(depth, true, reversible, fmt, param, {1, 1}, {1, 1},
                        precise, true);
        run_interp_case(depth, true, reversible, fmt, param, {3, 3}, {2, 2},
                        precise, false);
        run_interp_case(depth, true, reversible, fmt, param, {1, 1}, {4, 4},
                        precise, true);
      }
    }
  }
}

TEST_F(KduRegionDecompressorTest,
       TransferFunctionsPaletteHighPrecAndResamplingCoverage) {
  // 1. Transfer functions with gap=2 (bypasses SIMD) across byte, word, and
  // float outputs, leave_signed={false,true}, precisions (<8, 8, >8, <16, 16,
  // >16), src_scale != 1 (white_stretch + zeta != 0), across fix16, int32,
  // and float internal line types.
  struct SrcCfg {
    int src_bits;
    bool reversible;
    bool precise;
    bool stretch_and_offset;
  };
  const SrcCfg src_cfgs[] = {
      {8, true, false, false},  // KDRD_FIX16_TYPE, scale~1, off~0
      {7, true, false, true},   // KDRD_FIX16_TYPE, non-trivial scale & offset
      {16, true, true, false},  // KDRD_INT32_TYPE, scale~1
      {16, true, true, true},   // KDRD_INT32_TYPE, non-trivial scale & offset
      {8, false, true, false},  // KDRD_FLOAT_TYPE, scale~1
      {12, false, true, true},  // KDRD_FLOAT_TYPE, non-trivial scale & offset
  };
  for (const auto& sc : src_cfgs) {
    CodestreamSpec spec;
    spec.num_comps = 1;
    spec.width = 24;
    spec.height = 16;
    spec.bit_depth = sc.src_bits;
    spec.is_signed = false;
    spec.reversible = sc.reversible;
    auto bytes = EncodeCodestream(spec);
    MemorySource src(bytes);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    cs.set_persistent();
    auto cleanup = absl::MakeCleanup([&]() { cs.destroy(); });

    kdu_channel_mapping map;
    map.configure(1, sc.src_bits, false);
    if (sc.stretch_and_offset) {
      map.channel_interp[0].init(sc.src_bits, false, 1.5f);
    }

    kdu_region_decompressor decomp;
    if (sc.stretch_and_offset) {
      decomp.set_white_stretch(12);
      decomp.set_true_scaling(true, true);
    }
    kdu_core::kdu_dims region = decomp.get_rendered_image_dims(
        cs, &map, 0, 0, kdu_core::kdu_coords(1, 1));

    // Bytes: dst_prec in {6, 8, 10}, leave_signed in {false, true}, gap=2.
    for (int dprec : {6, 8, 10}) {
      for (bool lsig : {false, true}) {
        kdu_channel_mapping m;
        m.configure(1, dprec, lsig);
        if (sc.stretch_and_offset) {
          m.channel_interp[0].init(dprec, lsig, 1.5f);
        }
        ASSERT_TRUE(decomp.start(cs, &m, 0, 0, 1, region,
                                 kdu_core::kdu_coords(1, 1),
                                 kdu_core::kdu_coords(1, 1), sc.precise));
        std::vector<kdu_core::kdu_byte> buf(region.area() * 2);
        kdu_core::kdu_byte* ptrs[1] = {buf.data()};
        kdu_core::kdu_dims inc = region, nr;
        while (!inc.is_empty()) {
          ASSERT_TRUE(decomp.process(ptrs, false, 2, region.pos, region.size.x,
                                     128, region.area(), inc, nr,
                                     lsig ? 0 : dprec));
        }
        EXPECT_TRUE(decomp.finish());
      }
    }

    // Words: dst_prec in {12, 16, 20}, leave_signed in {false, true}, gap=2.
    for (int dprec : {12, 16, 20}) {
      for (bool lsig : {false, true}) {
        kdu_channel_mapping m;
        m.configure(1, dprec, lsig);
        if (sc.stretch_and_offset) {
          m.channel_interp[0].init(dprec, lsig, 1.5f);
        }
        ASSERT_TRUE(decomp.start(cs, &m, 0, 0, 1, region,
                                 kdu_core::kdu_coords(1, 1),
                                 kdu_core::kdu_coords(1, 1), sc.precise));
        std::vector<kdu_core::kdu_uint16> buf(region.area() * 2);
        kdu_core::kdu_uint16* ptrs[1] = {buf.data()};
        kdu_core::kdu_dims inc = region, nr;
        while (!inc.is_empty()) {
          ASSERT_TRUE(decomp.process(ptrs, false, 2, region.pos, region.size.x,
                                     128, region.area(), inc, nr,
                                     lsig ? 0 : dprec));
        }
        EXPECT_TRUE(decomp.finish());
      }
    }

    // Floats: leave_signed in {false, true} (via normalize), clip_outputs in
    // {false, true}.
    for (bool lsig : {false, true}) {
      kdu_channel_mapping m;
      m.configure(1, sc.src_bits, lsig);
      if (sc.stretch_and_offset) {
        m.channel_interp[0].init(sc.src_bits, lsig, 1.5f);
      }
      for (bool clip : {false, true}) {
        ASSERT_TRUE(decomp.start(cs, &m, 0, 0, 1, region,
                                 kdu_core::kdu_coords(1, 1),
                                 kdu_core::kdu_coords(1, 1), sc.precise));
        std::vector<float> buf(region.area() * 2);
        float* ptrs[1] = {buf.data()};
        kdu_core::kdu_dims inc = region, nr;
        while (!inc.is_empty()) {
          ASSERT_TRUE(decomp.process(ptrs, false, 2, region.pos, region.size.x,
                                     128, region.area(), inc, nr, !lsig, true,
                                     clip));
        }
        EXPECT_TRUE(decomp.finish());
      }
    }
  }

  // 2. High-precision (18-bit LUT) & lossy & 32-bit index JP2 palette paths.
  {
    Jp2Spec lspec;
    lspec.cs_spec.num_comps = 1;
    lspec.cs_spec.width = 24;
    lspec.cs_spec.height = 24;
    lspec.cs_spec.bit_depth = 8;
    lspec.cs_spec.reversible = false;
    lspec.space = JP2_sRGB_SPACE;
    lspec.use_palette = true;
    lspec.palette_entries = 6;
    lspec.lut_bit_depth = 18;
    auto jp2_bytes = EncodeJp2(lspec);

    jp2_family_src fam_src;
    jp2_source jp2_in;
    kdu_core::kdu_codestream cs;
    auto cleanup = absl::MakeCleanup([&]() {
      if (cs.exists()) cs.destroy();
      jp2_in.close();
      fam_src.close();
    });
    MemorySource mem_src(jp2_bytes);
    fam_src.open(&mem_src);
    ASSERT_TRUE(jp2_in.open(&fam_src));
    ASSERT_GT(jp2_in.read_header(), 0);
    cs.create(&jp2_in);
    cs.set_persistent();
    kdu_channel_mapping mapping;
    ASSERT_TRUE(mapping.configure(&jp2_in, true));

    // 18-bit LUT + precise=false -> KDRD_FIX16_TYPE lossy palette map +
    // convert_samples_to_palette_indices(buf16).
    {
      kdu_region_decompressor decomp;
      kdu_core::kdu_dims reg =
          decomp.get_rendered_image_dims(cs, &mapping, 0, 0, {1, 1}, {1, 1});
      ASSERT_TRUE(decomp.start(cs, &mapping, 0, 0, 1, reg, {1, 1}, {1, 1},
                               /*precise=*/false));
      std::vector<kdu_core::kdu_byte> c0(reg.area()), c1(reg.area()),
          c2(reg.area());
      kdu_core::kdu_byte* ptrs[3] = {c0.data(), c1.data(), c2.data()};
      kdu_core::kdu_dims inc = reg, nr;
      while (!inc.is_empty()) {
        ASSERT_TRUE(decomp.process(ptrs, false, 1, reg.pos, reg.size.x, 64,
                                   reg.area(), inc, nr));
      }
      EXPECT_TRUE(decomp.finish());
    }

    // precise=true + 18-bit LUT -> KDRD_FLOAT_TYPE palette map & boxcar float.
    for (int scale_den : {1, 4}) {
      cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
      kdu_region_decompressor decomp;
      kdu_core::kdu_dims reg = decomp.get_rendered_image_dims(
          cs, &mapping, 0, 0, {1, 1}, {scale_den, scale_den});
      ASSERT_TRUE(decomp.start(cs, &mapping, 0, 0, 1, reg, {1, 1},
                               {scale_den, scale_den}, /*precise=*/true));
      std::vector<float> c0(reg.area()), c1(reg.area()), c2(reg.area());
      float* ptrs[3] = {c0.data(), c1.data(), c2.data()};
      kdu_core::kdu_dims inc = reg, nr;
      while (!inc.is_empty()) {
        ASSERT_TRUE(decomp.process(ptrs, false, 1, reg.pos, reg.size.x, 64,
                                   reg.area(), inc, nr));
      }
      EXPECT_TRUE(decomp.finish());
    }
  }

  // 32-bit reversible palette component -> KDU_LINE_BUF_VPRECISE (int64).
  {
    Jp2Spec p32;
    p32.cs_spec.num_comps = 1;
    p32.cs_spec.width = 16;
    p32.cs_spec.height = 16;
    p32.cs_spec.bit_depth = 32;
    p32.cs_spec.reversible = true;
    p32.cs_spec.custom_samples = {0, 1, 2, 3, 4};
    p32.space = JP2_sRGB_SPACE;
    p32.use_palette = true;
    p32.palette_entries = 5;
    auto jp2_bytes = EncodeJp2(p32);

    jp2_family_src fam_src;
    jp2_source jp2_in;
    kdu_core::kdu_codestream cs;
    auto cleanup = absl::MakeCleanup([&]() {
      if (cs.exists()) cs.destroy();
      jp2_in.close();
      fam_src.close();
    });
    MemorySource mem_src(jp2_bytes);
    fam_src.open(&mem_src);
    ASSERT_TRUE(jp2_in.open(&fam_src));
    ASSERT_GT(jp2_in.read_header(), 0);
    cs.create(&jp2_in);
    kdu_channel_mapping mapping;
    ASSERT_TRUE(mapping.configure(&jp2_in, true));
    kdu_region_decompressor decomp;
    kdu_core::kdu_dims reg =
        decomp.get_rendered_image_dims(cs, &mapping, 0, 0, {1, 1});
    ASSERT_TRUE(decomp.start(cs, &mapping, 0, 0, 1, reg, {1, 1}, {1, 1}));
    std::vector<kdu_core::kdu_int32> packed(reg.area());
    kdu_core::kdu_dims inc = reg, nr;
    while (!inc.is_empty()) {
      ASSERT_TRUE(
          decomp.process(packed.data(), reg.pos, reg.size.x, 64, 64, inc, nr));
    }
    EXPECT_TRUE(decomp.finish());
  }

  // 3. CRG offsets (need_backing_lines) + 2/5 decimation & 2x/4x upsampling.
  {
    CodestreamSpec cspec;
    cspec.num_comps = 3;
    cspec.width = 40;
    cspec.height = 40;
    cspec.bit_depth = 8;
    cspec.reversible = true;
    cspec.extra_siz = "Stiles={16,16}";
    cspec.extra_crg = "CRGoffset={0.0,0.5},{0.0,0.0},{0.0,0.0}";
    auto bytes = EncodeCodestream(cspec);
    MemorySource src(bytes);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    cs.set_persistent();
    auto cleanup = absl::MakeCleanup([&]() { cs.destroy(); });

    kdu_channel_mapping map;
    ASSERT_TRUE(map.configure(cs));
    kdu_region_decompressor decomp;
    const std::pair<kdu_core::kdu_coords, kdu_core::kdu_coords> ratios[] = {
        {{2, 2}, {5, 5}},  // 0.4x -> SSSE3 non-shuffle 6-tap
        {{2, 2}, {1, 1}},  // 2x aligned
        {{4, 4}, {1, 1}},  // 4x aligned
    };
    for (const auto& r : ratios) {
      for (bool precise : {false, true}) {
        cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
        kdu_core::kdu_dims reg =
            decomp.get_rendered_image_dims(cs, &map, 0, 0, r.first, r.second);
        ASSERT_TRUE(
            decomp.start(cs, &map, 0, 0, 1, reg, r.first, r.second, precise));
        std::vector<kdu_core::kdu_byte> c0(reg.area()), c1(reg.area()),
            c2(reg.area());
        kdu_core::kdu_byte* ptrs[3] = {c0.data(), c1.data(), c2.data()};
        kdu_core::kdu_dims inc = reg, nr;
        while (!inc.is_empty()) {
          ASSERT_TRUE(decomp.process(ptrs, false, 1, reg.pos, reg.size.x, 128,
                                     reg.area(), inc, nr));
        }
        EXPECT_TRUE(decomp.finish());
      }
    }
  }
}

TEST_F(KduRegionDecompressorTest, NaturalEdgeCasesAndScalingCoverage) {
  // 1. Transfer functions with non-trivial src_scale in (1, 7] and > 7 and
  // src_off >= 1 across fix16, int32 (<16 and >=16), and float line types, plus
  // 14-bit reversible (local_convert_and_copy_to_int32 SHORT & int32_to_words
  // < 16).
  struct ScaleCfg {
    int src_bits;
    bool reversible;
    bool precise;
    float norm_max;
    float norm_nat_zero;
    float zeta;
  };
  const ScaleCfg scale_cfgs[] = {
      {8, true, false, 0.0f, -0.25f, 0.25f},  // fix16, 1 < src_scale <= 7
      {8, true, false, -0.45f, -0.5f, 0.5f},  // fix16, src_scale > 7
      {14, true, false, 0.48f, -0.5f, 0.0f},  // int32 from SHORT, src_bits<16
      {16, true, true, 0.0f, -0.25f, 0.25f},  // int32, 1 < src_scale <= 7
      {16, true, true, -0.45f, -0.5f, 0.5f},  // int32, src_scale > 7
      {8, false, true, -0.45f, -0.5f, 0.5f},  // float, src_scale > 7
  };
  for (const auto& sc : scale_cfgs) {
    CodestreamSpec spec;
    spec.num_comps = 1;
    spec.width = 24;
    spec.height = 16;
    spec.bit_depth = sc.src_bits;
    spec.reversible = sc.reversible;
    auto bytes = EncodeCodestream(spec);
    MemorySource src(bytes);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    cs.set_persistent();
    auto cleanup = absl::MakeCleanup([&]() { cs.destroy(); });

    kdu_region_decompressor decomp;
    // Note: configure_transfer_function treats (true_max && !true_zero) and
    // (!true_max && true_zero) as separate branches; pass (false, true)
    // here to unlock normalized_max scaling (signature is true_zero, true_max).
    decomp.set_true_scaling(false, true);

    for (bool lsig : {false, true}) {
      for (int dprec : {6, 8, 10, 20}) {
        cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
        kdu_channel_mapping m;
        m.configure(1, dprec, lsig);
        m.channel_interp[0].normalized_max = sc.norm_max;
        m.channel_interp[0].normalized_natural_zero = sc.norm_nat_zero;
        m.channel_interp[0].zeta = sc.zeta;
        kdu_core::kdu_dims region =
            decomp.get_rendered_image_dims(cs, &m, 0, 0, {1, 1});
        ASSERT_TRUE(
            decomp.start(cs, &m, 0, 0, 1, region, {1, 1}, {1, 1}, sc.precise));
        std::vector<kdu_core::kdu_byte> buf(region.area() * 2);
        kdu_core::kdu_byte* ptrs[1] = {buf.data()};
        kdu_core::kdu_dims inc = region, nr;
        while (!inc.is_empty()) {
          ASSERT_TRUE(decomp.process(ptrs, false, 2, region.pos, region.size.x,
                                     128, region.area(), inc, nr,
                                     lsig ? 0 : dprec));
        }
        EXPECT_TRUE(decomp.finish());
      }
      for (int dprec : {12, 16, 20}) {
        cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
        kdu_channel_mapping m;
        m.configure(1, dprec, lsig);
        m.channel_interp[0].normalized_max = sc.norm_max;
        m.channel_interp[0].normalized_natural_zero = sc.norm_nat_zero;
        m.channel_interp[0].zeta = sc.zeta;
        kdu_core::kdu_dims region =
            decomp.get_rendered_image_dims(cs, &m, 0, 0, {1, 1});
        ASSERT_TRUE(
            decomp.start(cs, &m, 0, 0, 1, region, {1, 1}, {1, 1}, sc.precise));
        std::vector<kdu_core::kdu_uint16> buf(region.area() * 2);
        kdu_core::kdu_uint16* ptrs[1] = {buf.data()};
        kdu_core::kdu_dims inc = region, nr;
        while (!inc.is_empty()) {
          ASSERT_TRUE(decomp.process(ptrs, false, 2, region.pos, region.size.x,
                                     128, region.area(), inc, nr,
                                     lsig ? 0 : dprec));
        }
        EXPECT_TRUE(decomp.finish());
      }
    }
  }

  // 2. White stretch (without set_true_scaling canceling it) on 4-bit
  // reversible image, hitting stretch_residual > 0, out_line == tile_lines[0],
  // and local_white_stretch.
  {
    CodestreamSpec ws_spec;
    ws_spec.num_comps = 1;
    ws_spec.width = 24;
    ws_spec.height = 16;
    ws_spec.bit_depth = 4;
    ws_spec.reversible = true;
    auto bytes = EncodeCodestream(ws_spec);
    MemorySource src(bytes);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    cs.set_persistent();
    auto cleanup = absl::MakeCleanup([&]() { cs.destroy(); });

    kdu_channel_mapping map;
    map.configure(1, 4, false);
    kdu_region_decompressor decomp;
    decomp.set_white_stretch(12);
    for (int num : {1, 3}) {
      int den = (num == 1) ? 1 : 2;
      cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
      kdu_core::kdu_dims reg = decomp.get_rendered_image_dims(
          cs, &map, 0, 0, {num, num}, {den, den});
      ASSERT_TRUE(
          decomp.start(cs, &map, 0, 0, 1, reg, {num, num}, {den, den}, false));
      std::vector<kdu_core::kdu_byte> buf(reg.area());
      kdu_core::kdu_byte* ptrs[1] = {buf.data()};
      kdu_core::kdu_dims inc = reg, nr;
      while (!inc.is_empty()) {
        ASSERT_TRUE(decomp.process(ptrs, false, 1, reg.pos, reg.size.x, 128,
                                   reg.area(), inc, nr));
      }
      EXPECT_TRUE(decomp.finish());
    }
  }

  // 3. local_convert_and_add (FIX16 boxcar via JP2_sYCC_SPACE /
  // colour_converter and precise=false, FLOAT boxcar via precise=true) plus
  // 1:1 colour_converter conversions across FLOAT, INT32, INT64, and SHORT
  // with subsampled chroma (missing_src_samples != 0).
  {
    struct BoxcarConvCfg {
      int bit_depth;
      bool reversible;
      bool precise;
      int den;
      jp2_colour_space space;
    };
    const BoxcarConvCfg cfgs[] = {
        {8, false, false, 4, JP2_sYCC_SPACE},  // SHORT rel -> FIX16 boxcar
        {8, false, true, 4, JP2_sYCC_SPACE},   // FLOAT -> FIX16 boxcar
        {15, true, false, 4, JP2_sRGB_SPACE},  // SHORT abs -> FIX16 boxcar
        {16, true, false, 4, JP2_sRGB_SPACE},  // INT32 -> FIX16 boxcar
        {32, true, false, 4, JP2_sRGB_SPACE},  // INT64 -> FIX16 boxcar
        {8, false, true, 4, JP2_sRGB_SPACE},   // FLOAT -> FLOAT boxcar
        {15, true, true, 4, JP2_sRGB_SPACE},   // SHORT abs -> FLOAT boxcar
        {16, true, true, 4, JP2_sRGB_SPACE},   // INT32 -> FLOAT boxcar
        {32, true, true, 4, JP2_sRGB_SPACE},   // INT64 -> FLOAT boxcar
        {8, false, true, 1, JP2_sYCC_SPACE},   // FLOAT -> FIX16 copy
        {16, true, true, 1, JP2_sYCC_SPACE},   // INT32 -> FIX16 copy
        {32, true, true, 1, JP2_sYCC_SPACE},   // INT64 -> FIX16 copy
    };
    for (const auto& c : cfgs) {
      Jp2Spec jspec;
      jspec.cs_spec.num_comps = 3;
      jspec.cs_spec.width = 32;
      jspec.cs_spec.height = 32;
      jspec.cs_spec.bit_depth = c.bit_depth;
      jspec.cs_spec.reversible = c.reversible;
      jspec.cs_spec.use_ycc = false;
      jspec.cs_spec.extra_siz =
          "Sorigin={3,0}\nSsampling={1,1},{2,2},{2,2}\nStiles={4,4}";
      if (c.bit_depth >= 24 && c.reversible) {
        jspec.cs_spec.extra_cod = "Clevels=0";
      }
      jspec.space = c.space;
      auto jp2_bytes = EncodeJp2(jspec);

      jp2_family_src fam_src;
      jp2_source jp2_in;
      kdu_core::kdu_codestream cs;
      auto cleanup = absl::MakeCleanup([&]() {
        if (cs.exists()) cs.destroy();
        jp2_in.close();
        fam_src.close();
      });
      MemorySource mem_src(jp2_bytes);
      fam_src.open(&mem_src);
      ASSERT_TRUE(jp2_in.open(&fam_src));
      ASSERT_GT(jp2_in.read_header(), 0);
      cs.create(&jp2_in);
      cs.set_persistent();
      kdu_channel_mapping mapping;
      ASSERT_TRUE(mapping.configure(&jp2_in, true));
      kdu_region_decompressor decomp;
      kdu_core::kdu_dims full = decomp.get_rendered_image_dims(
          cs, &mapping, 0, 0, {1, 1}, {c.den, c.den});
      kdu_core::kdu_dims reg = full;
      if (reg.size.x > 4) {
        reg.pos.x += 1;
        reg.size.x -= 2;
      }
      ASSERT_TRUE(decomp.start(cs, &mapping, 0, 0, 1, reg, {1, 1},
                               {c.den, c.den}, c.precise));
      std::vector<kdu_core::kdu_byte> b0(reg.area()), b1(reg.area()),
          b2(reg.area());
      kdu_core::kdu_byte* ptrs[3] = {b0.data(), b1.data(), b2.data()};
      kdu_core::kdu_dims inc = reg, nr;
      while (!inc.is_empty()) {
        ASSERT_TRUE(decomp.process(ptrs, false, 1, reg.pos, reg.size.x, 128,
                                   reg.area(), inc, nr));
      }
      EXPECT_TRUE(decomp.finish());
    }
  }

  // 4. CRG Y-offset -> need_backing_lines (6298-6315, 7539-7551) with CYcc=no,
  // plus > 8 tiles per row (6213-6231), and additional resampling ratios.
  {
    CodestreamSpec bspec;
    bspec.num_comps = 3;
    bspec.width = 40;
    bspec.height = 16;
    bspec.bit_depth = 8;
    bspec.reversible = true;
    bspec.use_ycc = false;
    bspec.extra_siz = "Stiles={4,4}";
    bspec.extra_crg = "CRGoffset={0.5,0.0},{0.0,0.0},{0.0,0.0}";
    auto bytes = EncodeCodestream(bspec);
    MemorySource src(bytes);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    cs.set_persistent();
    auto cleanup = absl::MakeCleanup([&]() { cs.destroy(); });

    kdu_channel_mapping map;
    ASSERT_TRUE(map.configure(cs));
    kdu_region_decompressor decomp;
    const std::pair<kdu_core::kdu_coords, kdu_core::kdu_coords> ratios[] = {
        {{1, 1}, {1, 1}},
        {{1, 1}, {3, 3}},  // 0.333x -> boxcar/decimate
                           // (exceeds kernel stride)
        {{2, 2}, {3, 3}},
        {{3, 3}, {4, 4}},
        {{5, 5}, {4, 4}},
        {{8, 8}, {3, 3}},
    };
    for (const auto& r : ratios) {
      for (bool precise : {false, true}) {
        cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
        kdu_core::kdu_dims reg =
            decomp.get_rendered_image_dims(cs, &map, 0, 0, r.first, r.second);
        ASSERT_TRUE(
            decomp.start(cs, &map, 0, 0, 1, reg, r.first, r.second, precise));
        std::vector<kdu_core::kdu_byte> c0(reg.area()), c1(reg.area()),
            c2(reg.area());
        kdu_core::kdu_byte* ptrs[3] = {c0.data(), c1.data(), c2.data()};
        kdu_core::kdu_dims inc = reg, nr;
        while (!inc.is_empty()) {
          ASSERT_TRUE(decomp.process(ptrs, false, 1, reg.pos, reg.size.x, 128,
                                     reg.area(), inc, nr));
        }
        EXPECT_TRUE(decomp.finish());
      }
    }
  }

  // 5. Empty subsampled component line in first tile row (reset_line_buf +
  // initial_empty_tile_lines + dims.size.y == 0) + interleaved
  // max_channel_bufs growth (fill_alpha = 6).
  {
    CodestreamSpec espec;
    espec.num_comps = 3;
    espec.width = 24;
    espec.height = 24;
    espec.bit_depth = 14;
    espec.reversible = true;
    espec.use_ycc = false;
    espec.extra_siz =
        "Sorigin={1,1}\nStiles={2,2}\nSsampling={1,1},{4,4},{4,4}";
    auto bytes = EncodeCodestream(espec);
    MemorySource src(bytes);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    cs.set_persistent();
    auto cleanup = absl::MakeCleanup([&]() { cs.destroy(); });

    kdu_channel_mapping map;
    ASSERT_TRUE(map.configure(cs));
    kdu_region_decompressor decomp;
    kdu_core::kdu_dims reg =
        decomp.get_rendered_image_dims(cs, &map, 0, 0, {1, 1});
    // Start with 1 channel to shrink/grow max_channel_bufs.
    {
      kdu_channel_mapping m1;
      m1.configure(1, 14, false);
      ASSERT_TRUE(decomp.start(cs, &m1, 0, 0, 1, reg, {1, 1}, {1, 1}, false));
      std::vector<kdu_core::kdu_uint16> wbuf(reg.area());
      int offs[1] = {0};
      kdu_core::kdu_dims inc = reg, nr;
      while (!inc.is_empty()) {
        ASSERT_TRUE(decomp.process(wbuf.data(), offs, 1, reg.pos, reg.size.x,
                                   64, reg.area(), inc, nr, 16, true, 1, 0));
      }
      EXPECT_TRUE(decomp.finish());
    }
    cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
    ASSERT_TRUE(decomp.start(cs, &map, 0, 0, 1, reg, {1, 1}, {1, 1}, false));
    std::vector<kdu_core::kdu_uint16> wbuf(reg.area() * 8);
    int woffs[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    kdu_core::kdu_dims inc = reg, nr;
    while (!inc.is_empty()) {
      ASSERT_TRUE(decomp.process(wbuf.data(), woffs, 8, reg.pos, reg.size.x, 64,
                                 reg.area(), inc, nr, 16, true, 1, 6));
    }
    EXPECT_TRUE(decomp.finish());
  }

  // 6. Bank > 8 tiles (6215-6233), insufficient DWT levels (6084-6087),
  // CYcc+CRG dependency error (7573-7589), 4-ch JP2_CMYK_SPACE convert_rgb4
  // (7971-7975), float* max_channel_bufs growth (7228-7232), and
  // quality_limiter + true_zero-only scaling (5925-5937, 2837-2846).
  {
    // > 8 horizontal tiles in one bank via narrow 1px tiles & 4x4 chroma.
    CodestreamSpec tspec;
    tspec.num_comps = 3;
    tspec.width = 24;
    tspec.height = 16;
    tspec.bit_depth = 8;
    tspec.reversible = true;
    tspec.use_ycc = false;
    tspec.extra_siz = "Stiles={1,16}\nSsampling={1,1},{4,4},{4,4}";
    auto tbytes = EncodeCodestream(tspec);
    MemorySource tsrc(tbytes);
    kdu_core::kdu_codestream tcs;
    tcs.create(&tsrc);
    tcs.set_persistent();
    auto tcleanup = absl::MakeCleanup([&]() { tcs.destroy(); });
    kdu_channel_mapping tmap;
    ASSERT_TRUE(tmap.configure(tcs));
    kdu_region_decompressor tdecomp;
    kdu_core::kdu_dims treg =
        tdecomp.get_rendered_image_dims(tcs, &tmap, 0, 0, {1, 1});
    ASSERT_TRUE(
        tdecomp.start(tcs, &tmap, 0, 0, 1, treg, {1, 1}, {1, 1}, false));
    std::vector<kdu_core::kdu_byte> c0(treg.area()), c1(treg.area()),
        c2(treg.area());
    kdu_core::kdu_byte* ptrs[3] = {c0.data(), c1.data(), c2.data()};
    kdu_core::kdu_dims tinc = treg, tnr;
    while (!tinc.is_empty()) {
      ASSERT_TRUE(tdecomp.process(ptrs, false, 1, treg.pos, treg.size.x, 128,
                                  treg.area(), tinc, tnr));
    }
    EXPECT_TRUE(tdecomp.finish());

    // process(float*, ...) max_channel_bufs growth (1 ch -> 1 ch +
    // fill_alpha=4) and set_true_scaling(false, true) + set_quality_limiting.
    CodestreamSpec fspec;
    fspec.num_comps = 1;
    fspec.width = 16;
    fspec.height = 16;
    fspec.bit_depth = 16;
    fspec.reversible = true;
    auto fbytes = EncodeCodestream(fspec);
    MemorySource fsrc(fbytes);
    kdu_core::kdu_codestream fcs;
    fcs.create(&fsrc);
    fcs.set_persistent();
    auto fcleanup = absl::MakeCleanup([&]() { fcs.destroy(); });

    kdu_core::kdu_quality_limiter qlim(2.0f, true);
    kdu_region_decompressor fdecomp;
    fdecomp.set_true_scaling(true, false);
    fdecomp.set_quality_limiting(&qlim, 72.0f, 72.0f);
    kdu_channel_mapping fm1;
    fm1.configure(1, 16, true);
    fm1.channel_interp[0].orig_signed = false;
    fm1.channel_interp[0].normalized_natural_zero = -0.25f;
    fm1.channel_interp[0].zeta = 0.25f;
    fm1.channel_interp[0].float_exp_bits = 5;
    kdu_core::kdu_dims freg =
        fdecomp.get_rendered_image_dims(fcs, &fm1, 0, 0, {1, 1});
    ASSERT_TRUE(fdecomp.start(fcs, &fm1, 0, 0, 1, freg, {1, 1}, {1, 1}, true));
    std::vector<float> fbuf1(freg.area());
    int foff1[1] = {0};
    kdu_core::kdu_dims finc = freg, fnr;
    while (!finc.is_empty()) {
      ASSERT_TRUE(fdecomp.process(fbuf1.data(), foff1, 1, freg.pos, freg.size.x,
                                  128, freg.area(), finc, fnr, true, 1, 0));
    }
    EXPECT_TRUE(fdecomp.finish());

    fcs.apply_input_restrictions(0, 0, 0, 0, nullptr);
    kdu_channel_mapping fm2;
    fm2.configure(1, 16, false);
    fm2.channel_interp[0].orig_signed = true;
    fm2.channel_interp[0].zeta = 0.25f;
    ASSERT_TRUE(fdecomp.start(fcs, &fm2, 0, 0, 1, freg, {1, 1}, {1, 1}, true));
    std::vector<float> fbuf5(freg.area() * 5);
    int foff5[5] = {0, 1, 2, 3, 4};
    finc = freg;
    while (!finc.is_empty()) {
      ASSERT_TRUE(fdecomp.process(fbuf5.data(), foff5, 5, freg.pos, freg.size.x,
                                  128, freg.area(), finc, fnr, true, 1, 4));
    }
    EXPECT_TRUE(fdecomp.finish());

    // 4-channel JP2_CMYK_SPACE via JPX triggering
    // colour_converter->convert_rgb4.
    {
      CodestreamSpec cspec;
      cspec.num_comps = 4;
      cspec.width = 16;
      cspec.height = 16;
      cspec.bit_depth = 8;
      cspec.reversible = false;
      cspec.use_ycc = false;
      MemoryTarget raw_jpx;
      {
        jp2_family_tgt ftgt;
        jpx_target jout;
        kdu_core::kdu_codestream ocs;
        auto oclean = absl::MakeCleanup([&]() {
          if (ocs.exists()) ocs.destroy();
          jout.close();
          ftgt.close();
        });
        ftgt.open(&raw_jpx);
        jout.open(&ftgt);
        jpx_codestream_target jst = jout.add_codestream();
        jpx_layer_target jlt = jout.add_layer();
        kdu_core::siz_params siz;
        ConfigureSizParams(&siz, cspec);
        ocs.create(&siz, jst.access_stream());
        ConfigureCodParams(ocs, cspec);
        jp2_dimensions jdim = jst.access_dimensions();
        jdim.init(ocs.access_siz(), false);
        jdim.finalize_compatibility(ocs.access_siz());
        jp2_colour jcol = jlt.add_colour();
        jcol.init(JP2_CMYK_SPACE);
        jp2_channels jchn = jlt.access_channels();
        jchn.init(4);
        for (int c = 0; c < 4; ++c) jchn.set_colour_mapping(c, c);
        jout.write_headers();
        jst.open_stream()->set_rubber_length();
        kdu_stripe_compressor comp;
        comp.start(ocs);
        std::vector<kdu_core::kdu_byte> src_cmyk(16 * 16 * 4, 128);
        kdu_core::kdu_byte* sptrs[4] = {src_cmyk.data(), src_cmyk.data() + 256,
                                        src_cmyk.data() + 512,
                                        src_cmyk.data() + 768};
        int h[4] = {16, 16, 16, 16};
        int sg[4] = {1, 1, 1, 1};
        int rg[4] = {16, 16, 16, 16};
        int pr[4] = {8, 8, 8, 8};
        comp.push_stripe(sptrs, h, sg, rg, pr);
        comp.finish();
        ocs.destroy();
        jst.access_stream()->close();
      }
      for (bool prec : {false, true}) {
        jp2_family_src jfam;
        jpx_source jsrc;
        kdu_core::kdu_codestream jcs;
        auto jclean = absl::MakeCleanup([&]() {
          if (jcs.exists()) jcs.destroy();
          jsrc.close();
          jfam.close();
        });
        MemorySource msrc(raw_jpx.data());
        jfam.open(&msrc);
        ASSERT_EQ(jsrc.open(&jfam, true), 1);
        jpx_layer_source jls = jsrc.access_layer(0, true);
        jpx_codestream_source jcss = jsrc.access_codestream(0, true);
        jcs.create(jcss.open_stream());
        jcs.set_persistent();
        kdu_channel_mapping jmap;
        ASSERT_TRUE(jmap.configure(jls.access_colour(0), jls.access_channels(),
                                   0, jcss.access_palette(),
                                   jcss.access_dimensions()));
        kdu_region_decompressor jdecomp;
        kdu_core::kdu_dims jreg =
            jdecomp.get_rendered_image_dims(jcs, &jmap, 0, 0, {1, 1});
        ASSERT_TRUE(
            jdecomp.start(jcs, &jmap, 0, 0, 1, jreg, {1, 1}, {1, 1}, prec));
        std::vector<kdu_core::kdu_byte> jb(jreg.area() * 4);
        kdu_core::kdu_byte* jptrs[4] = {jb.data(), jb.data() + jreg.area(),
                                        jb.data() + 2 * jreg.area(),
                                        jb.data() + 3 * jreg.area()};
        kdu_core::kdu_dims jinc = jreg, jnr;
        while (!jinc.is_empty()) {
          ASSERT_TRUE(jdecomp.process(jptrs, false, 1, jreg.pos, jreg.size.x,
                                      128, jreg.area(), jinc, jnr));
        }
        EXPECT_TRUE(jdecomp.finish());
      }
    }

    // CYcc=yes + CRG Y-offset -> engine->get_line MCT blocking check.
    for (const char* crg : {"CRGoffset={0.5,0.0},{0.0,0.0},{0.0,0.0}",
                            "CRGoffset={0.0,0.0},{0.5,0.0},{0.0,0.0}"}) {
      CodestreamSpec yspec;
      yspec.num_comps = 3;
      yspec.width = 16;
      yspec.height = 16;
      yspec.bit_depth = 8;
      yspec.reversible = true;
      yspec.use_ycc = true;
      yspec.extra_crg = crg;
      auto ybytes = EncodeCodestream(yspec);
      MemorySource ysrc(ybytes);
      kdu_core::kdu_codestream ycs;
      ycs.create(&ysrc);
      auto yclean = absl::MakeCleanup([&]() { ycs.destroy(); });
      kdu_channel_mapping ymap;
      ASSERT_TRUE(ymap.configure(ycs));
      kdu_region_decompressor ydecomp;
      kdu_core::kdu_dims yreg =
          ydecomp.get_rendered_image_dims(ycs, &ymap, 0, 0, {1, 1});
      if (ydecomp.start(ycs, &ymap, 0, 0, 1, yreg, {1, 1}, {1, 1}, false)) {
        std::vector<kdu_core::kdu_byte> yc0(yreg.area()), yc1(yreg.area()),
            yc2(yreg.area());
        kdu_core::kdu_byte* yptrs[3] = {yc0.data(), yc1.data(), yc2.data()};
        kdu_core::kdu_dims yinc = yreg, ynr;
        while (!yinc.is_empty() &&
               ydecomp.process(yptrs, false, 1, yreg.pos, yreg.size.x, 128,
                               yreg.area(), yinc, ynr)) {
        }
        ydecomp.finish();
      }
      hooks_.clear();
    }
  }
}

TEST_F(KduRegionDecompressorTest, ScalarAndSse2FallbackPaths) {
  // Single-threaded temporarily lowered kdu_mmx_level to cover non-SIMD and
  // SSE2-only kernel/conversion/resampling fallbacks cleanly without threads.
  const int saved_mmx = kdu_core::kdu_mmx_level;
  auto restore_mmx =
      absl::MakeCleanup([&]() { kdu_core::kdu_mmx_level = saved_mmx; });

  struct FallbackSpec {
    int bit_depth;
    bool reversible;
    bool precise;
    const char* extra_cod;
  };
  const FallbackSpec specs[] = {
      {8, false, false, ""},          {8, false, true, ""},
      {14, true, false, ""},          {16, true, false, ""},
      {32, true, false, "Clevels=0"}, {32, true, true, "Clevels=0"},
  };
  for (const auto& fs : specs) {
    CodestreamSpec spec;
    spec.num_comps = 1;
    spec.width = 24;
    spec.height = 24;
    spec.bit_depth = fs.bit_depth;
    spec.reversible = fs.reversible;
    spec.extra_siz = "Stiles={12,12}";
    spec.extra_cod = fs.extra_cod;
    kdu_core::kdu_mmx_level = saved_mmx;
    auto bytes = EncodeCodestream(spec);
    MemorySource src(bytes);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    cs.set_persistent();
    auto cleanup = absl::MakeCleanup([&]() { cs.destroy(); });

    kdu_channel_mapping map;
    map.configure(1, fs.bit_depth, false);
    const std::pair<kdu_core::kdu_coords, kdu_core::kdu_coords> ratios[] = {
        {{1, 1}, {1, 1}}, {{3, 3}, {2, 2}}, {{2, 2}, {5, 5}},
        {{2, 2}, {1, 1}}, {{4, 4}, {1, 1}}, {{1, 1}, {4, 4}},
    };

    for (int mmx_lvl : {2, 0}) {
      kdu_core::kdu_mmx_level = mmx_lvl;
      kdu_region_decompressor decomp;
      if (mmx_lvl == 0 && fs.bit_depth == 8 && fs.reversible == false &&
          !fs.precise) {
        decomp.set_white_stretch(12);
      }

      for (const auto& r : ratios) {
        cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
        kdu_core::kdu_dims full =
            decomp.get_rendered_image_dims(cs, &map, 0, 0, r.first, r.second);
        kdu_core::kdu_dims reg = full;
        if (r.first.x == 1 && r.second.x == 1 && reg.size.x > 8) {
          reg.pos.x += 2;
          reg.size.x -= 4;
        }
        ASSERT_TRUE(decomp.start(cs, &map, 0, 0, 1, reg, r.first, r.second,
                                 fs.precise));
        std::vector<kdu_core::kdu_byte> buf(reg.area());
        kdu_core::kdu_byte* ptrs[1] = {buf.data()};
        kdu_core::kdu_dims inc = reg, nr;
        while (!inc.is_empty()) {
          ASSERT_TRUE(decomp.process(ptrs, false, 1, reg.pos, reg.size.x, 128,
                                     reg.area(), inc, nr));
        }
        EXPECT_TRUE(decomp.finish());
      }
    }
  }

  // Scalar (mmx_lvl = 0) JP2_sYCC_SPACE + 1:4 boxcar & 1:1 to cover
  // non-SIMD local_convert_and_* branches (FLOAT, INT32, INT64, SHORT).
  for (const auto& fs : specs) {
    Jp2Spec jspec;
    jspec.cs_spec.num_comps = 3;
    jspec.cs_spec.width = 24;
    jspec.cs_spec.height = 24;
    jspec.cs_spec.bit_depth = fs.bit_depth;
    jspec.cs_spec.reversible = fs.reversible;
    jspec.cs_spec.use_ycc = false;
    jspec.cs_spec.extra_siz =
        "Sorigin={3,0}\nSsampling={1,1},{2,2},{2,2}\nStiles={4,4}";
    jspec.cs_spec.extra_cod = fs.extra_cod;
    jspec.space = JP2_sYCC_SPACE;
    kdu_core::kdu_mmx_level = saved_mmx;
    auto jbytes = EncodeJp2(jspec);
    kdu_core::kdu_mmx_level = 0;

    jp2_family_src jfam;
    jp2_source jsrc;
    kdu_core::kdu_codestream jcs;
    auto jclean = absl::MakeCleanup([&]() {
      if (jcs.exists()) jcs.destroy();
      jsrc.close();
      jfam.close();
    });
    MemorySource msrc(jbytes);
    jfam.open(&msrc);
    ASSERT_TRUE(jsrc.open(&jfam));
    ASSERT_GT(jsrc.read_header(), 0);
    jcs.create(&jsrc);
    jcs.set_persistent();
    kdu_channel_mapping jmap;
    ASSERT_TRUE(jmap.configure(&jsrc, true));
    kdu_region_decompressor jdecomp;
    for (int den : {1, 4}) {
      jcs.apply_input_restrictions(0, 0, 0, 0, nullptr);
      kdu_core::kdu_dims jreg =
          jdecomp.get_rendered_image_dims(jcs, &jmap, 0, 0, {1, 1}, {den, den});
      ASSERT_TRUE(jdecomp.start(jcs, &jmap, 0, 0, 1, jreg, {1, 1}, {den, den},
                                fs.precise));
      std::vector<kdu_core::kdu_byte> jb(jreg.area() * 3);
      kdu_core::kdu_byte* jptrs[3] = {jb.data(), jb.data() + jreg.area(),
                                      jb.data() + 2 * jreg.area()};
      kdu_core::kdu_dims jinc = jreg, jnr;
      while (!jinc.is_empty()) {
        ASSERT_TRUE(jdecomp.process(jptrs, false, 1, jreg.pos, jreg.size.x, 128,
                                    jreg.area(), jinc, jnr));
      }
      EXPECT_TRUE(jdecomp.finish());
    }
  }
}

TEST_F(KduRegionDecompressorTest, Int32PaletteResamplingAndQueryEdgeCases) {
  // 1. 4-bit reversible with precise=true and default normalized_max (0.5f,
  // src_scale == 1.0f) to cover local_transfer_int32_to_bytes (downshift < 0,
  // dst_prec <= 8 and > 8, signed & unsigned), local_transfer_int32_to_floats,
  // missing_src_samples in local_convert_and_copy_to_int32, and row_gap == 0.
  {
    CodestreamSpec spec;
    spec.num_comps = 1;
    spec.width = 24;
    spec.height = 16;
    spec.bit_depth = 4;
    spec.reversible = true;
    spec.extra_siz = "Stiles={8,8}";
    auto bytes = EncodeCodestream(spec);
    MemorySource src(bytes);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    cs.set_persistent();
    auto cleanup = absl::MakeCleanup([&]() { cs.destroy(); });

    TrackingMembroker broker;
    kdu_region_decompressor decomp;
    decomp.mem_configure(&broker, 10);

    for (bool lsig : {false, true}) {
      for (int dprec : {8, 12}) {
        cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
        kdu_channel_mapping m;
        m.configure(1, dprec, lsig);
        kdu_core::kdu_dims reg = decomp.get_rendered_image_dims(
            cs, &m, 0, 0, kdu_core::kdu_coords(1, 1));
        ASSERT_TRUE(decomp.start(cs, &m, 0, 0, 1, reg,
                                 kdu_core::kdu_coords(1, 1),
                                 kdu_core::kdu_coords(1, 1), true));
        std::vector<kdu_core::kdu_byte> buf(reg.area() * 2);
        kdu_core::kdu_byte* ptrs[1] = {buf.data()};
        kdu_core::kdu_dims inc = reg, nr;
        while (!inc.is_empty()) {
          ASSERT_TRUE(decomp.process(ptrs, false, 1, reg.pos, 0, 0, reg.area(),
                                     inc, nr, lsig ? 0 : dprec));
        }
        EXPECT_TRUE(decomp.finish());
      }
      for (int wprec : {16, 20}) {
        cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
        kdu_channel_mapping wm;
        wm.configure(1, wprec, lsig);
        kdu_core::kdu_dims reg = decomp.get_rendered_image_dims(
            cs, &wm, 0, 0, kdu_core::kdu_coords(1, 1));
        ASSERT_TRUE(decomp.start(cs, &wm, 0, 0, 1, reg,
                                 kdu_core::kdu_coords(1, 1),
                                 kdu_core::kdu_coords(1, 1), true));
        std::vector<kdu_core::kdu_uint16> wbuf(reg.area() * 2);
        kdu_core::kdu_uint16* wptrs[1] = {wbuf.data()};
        kdu_core::kdu_dims inc = reg, nr;
        while (!inc.is_empty()) {
          ASSERT_TRUE(decomp.process(wptrs, false, 1, reg.pos, reg.size.x, 128,
                                     reg.area(), inc, nr, lsig ? 0 : wprec));
        }
        EXPECT_TRUE(decomp.finish());
      }
    }

    // Reconfigure mem_configure once tile_banks exist, and confirm active
    // mem_configure rejects calls between start() and finish().
    decomp.mem_configure(&broker, 8);
    cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
    kdu_channel_mapping fm;
    fm.configure(1, 16, false);
    kdu_core::kdu_dims freg = decomp.get_rendered_image_dims(
        cs, &fm, 0, 0, kdu_core::kdu_coords(1, 1));
    ASSERT_TRUE(decomp.start(cs, &fm, 0, 0, 1, freg, kdu_core::kdu_coords(1, 1),
                             kdu_core::kdu_coords(1, 1), true));
    EXPECT_THROW(decomp.mem_configure(&broker, 8), kdu_core::kdu_exception);
    EXPECT_FALSE(hooks_.last_error().empty());
    hooks_.clear();
    std::vector<float> fbuf(freg.area());
    int foff[1] = {0};
    for (bool normalize : {false, true}) {
      for (bool clip : {false, true}) {
        kdu_core::kdu_dims finc = freg, fnr;
        while (!finc.is_empty()) {
          ASSERT_TRUE(decomp.process(fbuf.data(), foff, 1, freg.pos,
                                     freg.size.x, 128, freg.area(), finc, fnr,
                                     normalize, true, 1, 0, -1, clip));
        }
        EXPECT_TRUE(decomp.finish());
        cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
        ASSERT_TRUE(decomp.start(cs, &fm, 0, 0, 1, freg,
                                 kdu_core::kdu_coords(1, 1),
                                 kdu_core::kdu_coords(1, 1), true));
      }
    }
    EXPECT_TRUE(decomp.finish());

    // Single-component query overloads (mapping == nullptr) + static point/dim
    // helpers with fractional mapping and clamped ref_comp_subs.
    double min_prod = 0.0, max_x = 0.0, max_y = 0.0;
    decomp.get_safe_expansion_factors(cs, nullptr, 0, 0, min_prod, max_x,
                                      max_y);
    EXPECT_GT(min_prod, 0.0);
    kdu_core::kdu_dims single_dims = decomp.get_rendered_image_dims(
        cs, nullptr, 0, 0, kdu_core::kdu_coords(1, 1));
    EXPECT_FALSE(single_dims.is_empty());
    (void)kdu_region_decompressor::find_codestream_point(
        kdu_core::kdu_coords(5, 7), kdu_core::kdu_coords(0, 0),
        kdu_core::kdu_coords(3, 3), kdu_core::kdu_coords(2, 2), true);
    (void)kdu_region_decompressor::find_codestream_point(
        kdu_core::kdu_coords(5, 7), kdu_core::kdu_coords(2, 2),
        kdu_core::kdu_coords(3, 3), kdu_core::kdu_coords(2, 2), true);
    (void)kdu_region_decompressor::find_render_point(
        kdu_core::kdu_coords(5, 7), kdu_core::kdu_coords(0, 0),
        kdu_core::kdu_coords(3, 3), kdu_core::kdu_coords(2, 2), true);
    (void)kdu_region_decompressor::find_render_point(
        kdu_core::kdu_coords(5, 7), kdu_core::kdu_coords(2, 2),
        kdu_core::kdu_coords(3, 3), kdu_core::kdu_coords(2, 2), true);
    (void)kdu_region_decompressor::find_codestream_cover_dims(
        freg, kdu_core::kdu_coords(0, 0), kdu_core::kdu_coords(3, 3),
        kdu_core::kdu_coords(2, 2), true);
    (void)kdu_region_decompressor::find_codestream_cover_dims(
        freg, kdu_core::kdu_coords(2, 2), kdu_core::kdu_coords(3, 3),
        kdu_core::kdu_coords(2, 2), true);
  }

  // 2. Tiled, subsampled 16-bit reversible with non-zero origin and cropped
  // subregions + max_colour_channels < num_colour_channels.
  {
    CodestreamSpec spec;
    spec.num_comps = 3;
    spec.width = 24;
    spec.height = 16;
    spec.bit_depth = 16;
    spec.reversible = true;
    spec.use_ycc = false;
    spec.extra_siz = "Sorigin={3,2}\nSsampling={1,1},{2,2},{2,2}\nStiles={6,6}";
    auto bytes = EncodeCodestream(spec);
    MemorySource src(bytes);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    cs.set_persistent();
    auto cleanup = absl::MakeCleanup([&]() { cs.destroy(); });

    kdu_channel_mapping m;
    m.configure(cs);
    kdu_region_decompressor decomp;
    const kdu_core::kdu_coords exp_nums[] = {kdu_core::kdu_coords(1, 1),
                                             kdu_core::kdu_coords(3, 1),
                                             kdu_core::kdu_coords(2, 2)};
    const kdu_core::kdu_coords exp_dens[] = {kdu_core::kdu_coords(1, 1),
                                             kdu_core::kdu_coords(1, 1),
                                             kdu_core::kdu_coords(3, 3)};
    for (int k = 0; k < 3; ++k) {
      cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
      kdu_core::kdu_dims full = decomp.get_rendered_image_dims(
          cs, &m, 0, 0, exp_nums[k], exp_dens[k]);
      kdu_core::kdu_dims crop = full;
      crop.pos.x += std::min(5, crop.size.x / 3);
      crop.pos.y += std::min(1, crop.size.y / 3);
      crop.size.x = std::max(1, crop.size.x - std::min(7, crop.size.x - 1));
      crop.size.y = std::max(1, crop.size.y - std::min(2, crop.size.y - 1));

      for (const auto& reg : {full, crop}) {
        // Interleaved byte, word, and float paths with max_colour_channels=2.
        cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
        ASSERT_TRUE(
            decomp.start(cs, &m, 0, 0, 1, reg, exp_nums[k], exp_dens[k], true));
        std::vector<kdu_core::kdu_byte> bbuf(reg.area() * 2);
        int off[2] = {0, 1};
        kdu_core::kdu_dims inc = reg, nr;
        while (!inc.is_empty()) {
          ASSERT_TRUE(decomp.process(bbuf.data(), off, 2, reg.pos, reg.size.x,
                                     128, reg.area(), inc, nr, 8, false, 1, 0,
                                     2));
        }
        EXPECT_TRUE(decomp.finish());

        cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
        ASSERT_TRUE(
            decomp.start(cs, &m, 0, 0, 1, reg, exp_nums[k], exp_dens[k], true));
        std::vector<kdu_core::kdu_uint16> wbuf(reg.area() * 2);
        inc = reg;
        while (!inc.is_empty()) {
          ASSERT_TRUE(decomp.process(wbuf.data(), off, 2, reg.pos, reg.size.x,
                                     128, reg.area(), inc, nr, 12, false, 1, 0,
                                     2));
        }
        EXPECT_TRUE(decomp.finish());

        cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
        ASSERT_TRUE(
            decomp.start(cs, &m, 0, 0, 1, reg, exp_nums[k], exp_dens[k], true));
        std::vector<float> fbuf(reg.area() * 2);
        inc = reg;
        while (!inc.is_empty()) {
          ASSERT_TRUE(decomp.process(fbuf.data(), off, 2, reg.pos, reg.size.x,
                                     128, reg.area(), inc, nr, false, false, 1,
                                     0, 2, false));
        }
        EXPECT_TRUE(decomp.finish());
      }
    }
  }

  // 3. Heavy-lossy and tiled JP2 palette: clamping in
  // convert_samples_to_palette_indices, perform_palette_map (fix16 & float)
  // missing/crop padding, and map_and_integrate (fix16 & float).
  for (bool reversible : {false, true}) {
    Jp2Spec pspec;
    pspec.cs_spec.num_comps = 1;
    pspec.cs_spec.width = 24;
    pspec.cs_spec.height = 16;
    pspec.cs_spec.bit_depth = 8;
    pspec.cs_spec.reversible = reversible;
    pspec.cs_spec.extra_siz = "Sorigin={3,0}\nStiles={6,6}";
    if (!reversible) {
      pspec.cs_spec.extra_cod = "Clevels=1";
    }
    pspec.space = JP2_sRGB_SPACE;
    pspec.use_palette = true;
    pspec.palette_entries = 5;
    pspec.palette_has_alpha = true;
    auto jp2_bytes = EncodeJp2(pspec);

    jp2_family_src fam_src;
    jp2_source jp2_in;
    kdu_core::kdu_codestream cs;
    auto cleanup = absl::MakeCleanup([&]() {
      if (cs.exists()) cs.destroy();
      jp2_in.close();
      fam_src.close();
    });
    MemorySource mem_src(jp2_bytes);
    fam_src.open(&mem_src);
    ASSERT_TRUE(jp2_in.open(&fam_src));
    ASSERT_GT(jp2_in.read_header(), 0);
    cs.create(&jp2_in);
    cs.set_persistent();

    kdu_channel_mapping mapping;
    ASSERT_TRUE(mapping.configure(&jp2_in, false));
    kdu_region_decompressor decomp;

    for (bool precise : {false, true}) {
      // Upsample 3:1 (missing_source_samples > 0 + right edge padding) and
      // 1:4 boxcar (map_and_integrate fix16 and float).
      for (int mode : {0, 1, 2}) {
        kdu_core::kdu_coords en(mode == 0 ? 3 : 1, 1);
        kdu_core::kdu_coords ed(mode == 2 ? 4 : 1, mode == 2 ? 4 : 1);
        cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
        kdu_core::kdu_dims reg =
            decomp.get_rendered_image_dims(cs, &mapping, 0, 0, en, ed);
        if (mode == 1) {
          reg.pos.x += 4;
          reg.size.x -= 8;
        }
        ASSERT_TRUE(decomp.start(cs, &mapping, 0, 0, 1, reg, en, ed, precise));
        std::vector<kdu_core::kdu_byte> c0(reg.area()), c1(reg.area()),
            c2(reg.area()), c3(reg.area());
        kdu_core::kdu_byte* ptrs[4] = {c0.data(), c1.data(), c2.data(),
                                       c3.data()};
        kdu_core::kdu_dims inc = reg, nr;
        while (!inc.is_empty()) {
          ASSERT_TRUE(decomp.process(ptrs, false, 1, reg.pos, reg.size.x, 64,
                                     reg.area(), inc, nr));
        }
        EXPECT_TRUE(decomp.finish());
      }
    }
  }

  // 4. Shared component mapped to multiple channels (need_backing_lines /
  // tile_backing_lines) and fractional downsampling (2:3) + upsampling (5:2)
  // across AVX2/SSE4/SSE2/scalar kernels.
  {
    const int saved_mmx = kdu_core::kdu_mmx_level;
    auto restore_mmx =
        absl::MakeCleanup([&]() { kdu_core::kdu_mmx_level = saved_mmx; });

    CodestreamSpec spec;
    spec.num_comps = 3;
    spec.width = 32;
    spec.height = 24;
    spec.bit_depth = 8;
    spec.reversible = false;
    spec.use_ycc = false;
    spec.extra_siz = "Stiles={16,12}";
    auto bytes = EncodeCodestream(spec);
    MemorySource src(bytes);
    kdu_core::kdu_codestream cs;
    cs.create(&src);
    cs.set_persistent();
    auto cleanup = absl::MakeCleanup([&]() { cs.destroy(); });

    kdu_channel_mapping shared_map;
    shared_map.configure(3, 8, false);
    shared_map.source_components[0] = 0;
    shared_map.source_components[1] = 0;
    shared_map.source_components[2] = 0;

    kdu_region_decompressor decomp;
    // Empty/non-intersecting region returns false from start(); huge coprime
    // ratio triggers reduce_ratio_to_ints overflow reduction; small
    // suggested_increment across many tiles exercises start_tile_bank.
    kdu_core::kdu_dims empty_region;
    empty_region.pos = kdu_core::kdu_coords(1000, 1000);
    empty_region.size = kdu_core::kdu_coords(8, 8);
    EXPECT_FALSE(decomp.start(cs, &shared_map, 0, 0, 1, empty_region,
                              kdu_core::kdu_coords(1, 1)));
    EXPECT_FALSE(hooks_.last_error().empty());
    hooks_.clear();
    EXPECT_TRUE(decomp.finish());

    cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
    kdu_core::kdu_coords big_en(600000001, 600000001);
    kdu_core::kdu_coords big_ed(1200000007, 1200000007);
    kdu_core::kdu_dims big_reg =
        decomp.get_rendered_image_dims(cs, &shared_map, 0, 0, big_en, big_ed);
    ASSERT_TRUE(
        decomp.start(cs, &shared_map, 0, 0, 1, big_reg, big_en, big_ed, false));
    std::vector<kdu_core::kdu_byte> bb0(big_reg.area()), bb1(big_reg.area()),
        bb2(big_reg.area());
    kdu_core::kdu_byte* bptrs[3] = {bb0.data(), bb1.data(), bb2.data()};
    kdu_core::kdu_dims binc = big_reg, bnr;
    while (!binc.is_empty()) {
      ASSERT_TRUE(decomp.process(bptrs, false, 1, big_reg.pos, big_reg.size.x,
                                 16, big_reg.area(), binc, bnr));
    }
    EXPECT_TRUE(decomp.finish());

    // Scalar den >= (INT_MAX >> 2) path in do_horz_resampling_{fix16,float}.
    for (bool h_prec : {false, true}) {
      kdu_core::kdu_mmx_level = 0;
      cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
      kdu_core::kdu_coords hugen(400000001, 400000001);
      kdu_core::kdu_coords huged(600000002, 600000002);
      kdu_core::kdu_dims hreg =
          decomp.get_rendered_image_dims(cs, &shared_map, 0, 0, hugen, huged);
      ASSERT_TRUE(
          decomp.start(cs, &shared_map, 0, 0, 1, hreg, hugen, huged, h_prec));
      std::vector<kdu_core::kdu_byte> hb0(hreg.area()), hb1(hreg.area()),
          hb2(hreg.area());
      kdu_core::kdu_byte* hptrs[3] = {hb0.data(), hb1.data(), hb2.data()};
      kdu_core::kdu_dims hinc = hreg, hnr;
      while (!hinc.is_empty()) {
        ASSERT_TRUE(decomp.process(hptrs, false, 1, hreg.pos, hreg.size.x, 128,
                                   hreg.area(), hinc, hnr));
      }
      EXPECT_TRUE(decomp.finish());
    }

    for (int mmx_lvl : {saved_mmx, 4, 2, 0}) {
      kdu_core::kdu_mmx_level = mmx_lvl;
      for (bool precise : {false, true}) {
        const kdu_core::kdu_coords en[] = {kdu_core::kdu_coords(1, 1),
                                           kdu_core::kdu_coords(2, 3),
                                           kdu_core::kdu_coords(5, 2)};
        const kdu_core::kdu_coords ed[] = {kdu_core::kdu_coords(1, 1),
                                           kdu_core::kdu_coords(3, 3),
                                           kdu_core::kdu_coords(2, 2)};
        for (int idx = 0; idx < 3; ++idx) {
          cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
          kdu_core::kdu_dims reg = decomp.get_rendered_image_dims(
              cs, &shared_map, 0, 0, en[idx], ed[idx]);
          ASSERT_TRUE(decomp.start(cs, &shared_map, 0, 0, 1, reg, en[idx],
                                   ed[idx], precise));
          std::vector<kdu_core::kdu_byte> c0(reg.area()), c1(reg.area()),
              c2(reg.area());
          kdu_core::kdu_byte* ptrs[3] = {c0.data(), c1.data(), c2.data()};
          kdu_core::kdu_dims inc = reg, nr;
          while (!inc.is_empty()) {
            ASSERT_TRUE(decomp.process(ptrs, false, 1, reg.pos, reg.size.x, 128,
                                       reg.area(), inc, nr));
          }
          EXPECT_TRUE(decomp.finish());
        }
      }
    }
  }

  // 5. Small tiles (Stiles={2,16}) + 4x4 subsampling on comps 1..2 with
  // cropped region starting at x=5 so comp 0 has missing_src_samples <= -len
  // across tile boundaries in convert_and_copy and convert_and_add, plus
  // scalar non-multiple-of-4 horizontal 2:3 downsampling tail loops.
  {
    const int saved_mmx = kdu_core::kdu_mmx_level;
    auto restore_mmx =
        absl::MakeCleanup([&]() { kdu_core::kdu_mmx_level = saved_mmx; });
    for (int bit_depth : {8, 16}) {
      for (bool reversible : {false, true}) {
        CodestreamSpec spec;
        spec.num_comps = 3;
        spec.width = 25;
        spec.height = 16;
        spec.bit_depth = bit_depth;
        spec.reversible = reversible;
        spec.use_ycc = false;
        spec.extra_siz = "Stiles={2,16}\nSsampling={1,1},{4,4},{4,4}";
        kdu_core::kdu_mmx_level = saved_mmx;
        auto bytes = EncodeCodestream(spec);
        MemorySource src(bytes);
        kdu_core::kdu_codestream cs;
        cs.create(&src);
        cs.set_persistent();
        auto cleanup = absl::MakeCleanup([&]() { cs.destroy(); });

        kdu_channel_mapping m;
        m.configure(3, bit_depth, false);
        m.source_components[0] = 0;
        m.source_components[1] = 1;
        m.source_components[2] = 2;
        kdu_region_decompressor decomp;
        for (int mmx_lvl : {saved_mmx, 0}) {
          kdu_core::kdu_mmx_level = mmx_lvl;
          for (bool precise : {false, true}) {
            const kdu_core::kdu_coords en[] = {kdu_core::kdu_coords(1, 1),
                                               kdu_core::kdu_coords(1, 1),
                                               kdu_core::kdu_coords(2, 2)};
            const kdu_core::kdu_coords ed[] = {kdu_core::kdu_coords(1, 1),
                                               kdu_core::kdu_coords(2, 2),
                                               kdu_core::kdu_coords(3, 3)};
            for (int mode = 0; mode < 3; ++mode) {
              cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
              kdu_core::kdu_dims reg = decomp.get_rendered_image_dims(
                  cs, &m, 0, 0, en[mode], ed[mode]);
              if (mode < 2 && reg.size.x > 6) {
                reg.pos.x += 5;
                reg.size.x -= 5;
              }
              ASSERT_TRUE(decomp.start(cs, &m, 0, 0, 1, reg, en[mode], ed[mode],
                                       precise));
              std::vector<kdu_core::kdu_byte> b0(reg.area()), b1(reg.area()),
                  b2(reg.area());
              kdu_core::kdu_byte* ptrs[3] = {b0.data(), b1.data(), b2.data()};
              kdu_core::kdu_dims inc = reg, nr;
              while (!inc.is_empty()) {
                ASSERT_TRUE(decomp.process(ptrs, false, 1, reg.pos, reg.size.x,
                                           8, reg.area(), inc, nr));
              }
              EXPECT_TRUE(decomp.finish());
            }
          }
        }
      }
    }

    // Same 3-comp Stiles={2,16} + Ssampling={1,1},{4,4},{4,4} with
    // JP2_CHANNEL_FORMAT_FLOAT to hit missing_src_samples <= -src_len in all 4
    // local_reinterpret_and_{copy,add}_{unsigned,signed}_floats functions.
    for (bool is_signed : {false, true}) {
      CodestreamSpec fspec;
      fspec.num_comps = 3;
      fspec.width = 24;
      fspec.height = 16;
      fspec.bit_depth = 20;
      fspec.is_signed = is_signed;
      fspec.reversible = true;
      fspec.use_ycc = false;
      fspec.extra_siz = "Stiles={4,16}\nSsampling={1,1},{4,4},{4,4}";
      kdu_core::kdu_mmx_level = saved_mmx;
      auto bytes = EncodeCodestream(fspec);
      MemorySource src(bytes);
      kdu_core::kdu_codestream cs;
      cs.create(&src);
      cs.set_persistent();
      auto cleanup = absl::MakeCleanup([&]() { cs.destroy(); });

      kdu_channel_mapping fm;
      fm.configure(3, 20, is_signed);
      int exp_p[1] = {6};
      for (int c = 0; c < 3; ++c) {
        fm.source_components[c] = c;
      }
      ASSERT_TRUE(fm.channel_interp[0].init(20, is_signed, 0.0f,
                                            JP2_CHANNEL_FORMAT_FLOAT, exp_p));
      kdu_region_decompressor decomp;
      for (int ed_val : {1, 2}) {
        kdu_core::kdu_coords en(1, 1);
        kdu_core::kdu_coords ed(ed_val, ed_val);
        cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
        kdu_core::kdu_dims reg =
            decomp.get_rendered_image_dims(cs, &fm, 0, 0, en, ed);
        reg.pos.x += 6;
        reg.size.x -= 6;
        ASSERT_TRUE(decomp.start(cs, &fm, 0, 0, 1, reg, en, ed, true));
        std::vector<float> f0(reg.area()), f1(reg.area()), f2(reg.area());
        float* fptrs[3] = {f0.data(), f1.data(), f2.data()};
        kdu_core::kdu_dims inc = reg, nr;
        while (!inc.is_empty()) {
          ASSERT_TRUE(decomp.process(fptrs, false, 1, reg.pos, reg.size.x,
                                     10000, 10000, inc, nr, true));
        }
        EXPECT_TRUE(decomp.finish());
      }
    }
  }
}

TEST_F(KduRegionDecompressorTest, WhiteStretchPaletteAndJpxSection6To9) {
  // 6. Irreversible white_stretch (out_line == tile_lines[0]),
  // kdu_channel_mapping edge cases, planar & interleaved process overloads
  // (null channel pointers, max_channel_bufs growth, fill_alpha,
  // max_colour_channels on RGBA, empty incomplete_region), and JPX CMYK with
  // precise=true.
  {
    // kdu_channel_interp normalized_natural_zero > normalized_max - one_lsb.
    kdu_channel_interp ci;
    EXPECT_TRUE(ci.init(2, false, 0.75f));

    // Irreversible white_stretch at 1:1 -> can_use_component_samples_directly
    // forces out_line == source->tile_lines[0] -> temporary line allocation.
    {
      CodestreamSpec wspec;
      wspec.num_comps = 1;
      wspec.width = 16;
      wspec.height = 16;
      wspec.bit_depth = 4;
      wspec.reversible = false;
      auto bytes = EncodeCodestream(wspec);
      MemorySource src(bytes);
      kdu_core::kdu_codestream cs;
      cs.create(&src);
      cs.set_persistent();
      auto cleanup = absl::MakeCleanup([&]() { cs.destroy(); });

      kdu_channel_mapping wm;
      wm.configure(1, 4, false);
      kdu_region_decompressor wdec;
      wdec.set_white_stretch(12);
      kdu_core::kdu_dims reg = wdec.get_rendered_image_dims(
          cs, &wm, 0, 0, kdu_core::kdu_coords(1, 1));
      ASSERT_TRUE(
          wdec.start(cs, &wm, 0, 0, 1, reg, kdu_core::kdu_coords(1, 1)));
      std::vector<kdu_core::kdu_byte> buf(reg.area());
      kdu_core::kdu_byte* ptrs[1] = {buf.data()};
      kdu_core::kdu_dims inc = reg, nr;
      while (!inc.is_empty()) {
        ASSERT_TRUE(wdec.process(ptrs, false, 1, reg.pos, reg.size.x, 128,
                                 reg.area(), inc, nr));
      }
      // Calling process with empty incomplete_region returns false.
      kdu_core::kdu_dims empty_inc;
      empty_inc.pos = reg.pos;
      empty_inc.size = kdu_core::kdu_coords(0, 0);
      EXPECT_FALSE(wdec.process(ptrs, false, 1, reg.pos, reg.size.x, 128,
                                reg.area(), empty_inc, nr));
      EXPECT_TRUE(wdec.finish());
    }

    // Fresh decompressors to test max_channel_bufs growth, have_null_bufs,
    // default_rendering_precision <= 0, fill_alpha precision clamping across
    // planar and interleaved byte/word/float overloads.
    {
      CodestreamSpec mspec;
      mspec.num_comps = 1;
      mspec.width = 16;
      mspec.height = 16;
      mspec.bit_depth = 8;
      mspec.reversible = false;
      auto bytes = EncodeCodestream(mspec);
      MemorySource src(bytes);
      kdu_core::kdu_codestream cs;
      cs.create(&src);
      cs.set_persistent();
      auto cleanup = absl::MakeCleanup([&]() { cs.destroy(); });

      kdu_channel_mapping m;
      m.configure(1, 0, false);

      // Planar byte, word, float with null middle buffer + max_channel_bufs
      // reallocation.
      for (int overload = 0; overload < 6; ++overload) {
        kdu_region_decompressor dec;
        for (bool grow : {false, true}) {
          cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
          kdu_core::kdu_dims reg = dec.get_rendered_image_dims(
              cs, &m, 0, 0, kdu_core::kdu_coords(1, 1));
          ASSERT_TRUE(
              dec.start(cs, &m, 0, 0, 1, reg, kdu_core::kdu_coords(1, 1)));
          kdu_core::kdu_dims inc = reg, nr;
          if (overload == 0) {
            std::vector<kdu_core::kdu_byte> c0(reg.area()), c2(reg.area());
            kdu_core::kdu_byte* p1[1] = {c0.data()};
            kdu_core::kdu_byte* p3[3] = {c0.data(), nullptr, c2.data()};
            while (!inc.is_empty()) {
              ASSERT_TRUE(dec.process(grow ? p3 : p1, grow, 1, reg.pos,
                                      reg.size.x, 128, reg.area(), inc, nr, 0));
            }
          } else if (overload == 1) {
            std::vector<kdu_core::kdu_uint16> w0(reg.area()), w2(reg.area());
            kdu_core::kdu_uint16* p1[1] = {w0.data()};
            kdu_core::kdu_uint16* p3[3] = {w0.data(), nullptr, w2.data()};
            while (!inc.is_empty()) {
              ASSERT_TRUE(dec.process(grow ? p3 : p1, grow, 1, reg.pos,
                                      reg.size.x, 128, reg.area(), inc, nr, 0));
            }
          } else if (overload == 2) {
            std::vector<float> f0(reg.area()), f2(reg.area());
            float* p1[1] = {f0.data()};
            float* p3[3] = {f0.data(), nullptr, f2.data()};
            while (!inc.is_empty()) {
              ASSERT_TRUE(dec.process(grow ? p3 : p1, grow, 1, reg.pos,
                                      reg.size.x, 128, reg.area(), inc, nr,
                                      false));
            }
          } else if (overload == 3) {
            std::vector<kdu_core::kdu_byte> ib(reg.area() * 4);
            int off4[4] = {0, 1, 2, 3};
            while (!inc.is_empty()) {
              ASSERT_TRUE(dec.process(ib.data(), off4, grow ? 4 : 1, reg.pos,
                                      reg.size.x, 128, reg.area(), inc, nr,
                                      grow ? 12 : 0, false, 1, grow ? 3 : 0));
            }
          } else if (overload == 4) {
            std::vector<kdu_core::kdu_uint16> iw(reg.area() * 4);
            int off4[4] = {0, 1, 2, 3};
            while (!inc.is_empty()) {
              ASSERT_TRUE(dec.process(iw.data(), off4, grow ? 4 : 1, reg.pos,
                                      reg.size.x, 128, reg.area(), inc, nr,
                                      grow ? 20 : 0, false, 1, grow ? 3 : 0));
            }
          } else {
            std::vector<float> fl(reg.area() * 4);
            int off4[4] = {0, 1, 2, 3};
            while (!inc.is_empty()) {
              ASSERT_TRUE(dec.process(fl.data(), off4, grow ? 4 : 1, reg.pos,
                                      reg.size.x, 128, reg.area(), inc, nr,
                                      false, false, 1, grow ? 3 : 0));
            }
          }
          EXPECT_TRUE(dec.finish());
        }
      }
    }

    // 4-channel RGBA JP2 (paletted) with fill_alpha=0 and max_colour_channels=2
    // on uint16 and float interleaved overloads + shrinking paletted mapping
    // via set_num_channels(1).
    {
      Jp2Spec pspec;
      pspec.cs_spec.num_comps = 1;
      pspec.cs_spec.width = 16;
      pspec.cs_spec.height = 16;
      pspec.space = JP2_sRGB_SPACE;
      pspec.use_palette = true;
      pspec.palette_entries = 5;
      pspec.palette_has_alpha = true;
      auto jp2_bytes = EncodeJp2(pspec);
      jp2_family_src fam_src;
      jp2_source jp2_in;
      kdu_core::kdu_codestream cs;
      auto cleanup = absl::MakeCleanup([&]() {
        if (cs.exists()) cs.destroy();
        jp2_in.close();
        fam_src.close();
      });
      MemorySource mem_src(jp2_bytes);
      fam_src.open(&mem_src);
      ASSERT_TRUE(jp2_in.open(&fam_src));
      ASSERT_GT(jp2_in.read_header(), 0);
      cs.create(&jp2_in);
      cs.set_persistent();

      kdu_channel_mapping mapping;
      ASSERT_TRUE(mapping.configure(&jp2_in, false));
      kdu_region_decompressor dec;
      kdu_core::kdu_dims reg = dec.get_rendered_image_dims(
          cs, &mapping, 0, 0, kdu_core::kdu_coords(1, 1));
      int off3[3] = {0, 1, 2};
      {
        std::vector<kdu_core::kdu_uint16> wbuf(reg.area() * 3);
        ASSERT_TRUE(
            dec.start(cs, &mapping, 0, 0, 1, reg, kdu_core::kdu_coords(1, 1)));
        kdu_core::kdu_dims inc = reg, nr;
        while (!inc.is_empty()) {
          ASSERT_TRUE(dec.process(wbuf.data(), off3, 3, reg.pos, reg.size.x,
                                  128, reg.area(), inc, nr, 12, false, 1, 0,
                                  2));
        }
        EXPECT_TRUE(dec.finish());
      }
      {
        cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
        std::vector<float> fbuf(reg.area() * 3);
        ASSERT_TRUE(
            dec.start(cs, &mapping, 0, 0, 1, reg, kdu_core::kdu_coords(1, 1)));
        kdu_core::kdu_dims inc = reg, nr;
        while (!inc.is_empty()) {
          ASSERT_TRUE(dec.process(fbuf.data(), off3, 3, reg.pos, reg.size.x,
                                  128, reg.area(), inc, nr, true, false, 1, 0,
                                  2));
        }
        EXPECT_TRUE(dec.finish());
      }
      mapping.set_num_channels(1);
      EXPECT_EQ(mapping.num_channels, 1);
    }

    // 7. Ssampling={3,3} with large odd expand ratio (900000003 / 1500000003)
    // so (den * subs) > 0x7FFFFFFF triggers reduce_ratio_to_ints divisor scan.
    {
      CodestreamSpec rspec;
      rspec.num_comps = 1;
      rspec.width = 9;
      rspec.height = 9;
      rspec.bit_depth = 8;
      rspec.reversible = false;
      rspec.extra_siz = "Ssampling={3,3}";
      auto rbytes = EncodeCodestream(rspec);
      MemorySource rsrc(rbytes);
      kdu_core::kdu_codestream rcs;
      rcs.create(&rsrc);
      rcs.set_persistent();
      auto rclean = absl::MakeCleanup([&]() { rcs.destroy(); });

      kdu_channel_mapping rm;
      rm.configure(rcs);
      kdu_region_decompressor rdec;
      kdu_core::kdu_coords ren(900000003, 900000003);
      kdu_core::kdu_coords red(1500000003, 1500000003);
      kdu_core::kdu_dims rreg =
          rdec.get_rendered_image_dims(rcs, &rm, 0, 0, ren, red);
      ASSERT_TRUE(rdec.start(rcs, &rm, 0, 0, 1, rreg, ren, red, false));
      std::vector<kdu_core::kdu_byte> rb(rreg.area());
      kdu_core::kdu_byte* rptrs[1] = {rb.data()};
      kdu_core::kdu_dims rinc = rreg, rnr;
      while (!rinc.is_empty()) {
        ASSERT_TRUE(rdec.process(rptrs, false, 1, rreg.pos, rreg.size.x, 128,
                                 rreg.area(), rinc, rnr));
      }
      EXPECT_TRUE(rdec.finish());
    }

    // 8. Additional high-yield coverage paths:
    //  - set_true_scaling(true, true) (2844-2869)
    //  - fix16_to_floats with normalize=true, always_clip_outputs=false
    //  - int32_to_words with bit_depth=24 & 32 and wprec=18 (dst_prec > 16)
    //  - 32-bit reversible INT64 paths (default + JP2_CHANNEL_FORMAT_FLOAT)
    //  - scalar 10-bit reversible 1:4 boxcar (local_convert_and_add upshift<0)
    {
      CodestreamSpec sspec;
      sspec.num_comps = 1;
      sspec.width = 16;
      sspec.height = 16;
      sspec.bit_depth = 8;
      sspec.reversible = false;
      auto sbytes = EncodeCodestream(sspec);
      MemorySource ssrc(sbytes);
      kdu_core::kdu_codestream scs;
      scs.create(&ssrc);
      scs.set_persistent();
      auto sclean = absl::MakeCleanup([&]() { scs.destroy(); });

      kdu_region_decompressor sdec;
      sdec.set_true_scaling(true, true);
      for (bool osig : {false, true}) {
        for (float nmax : {0.25f, -0.4f}) {
          scs.apply_input_restrictions(0, 0, 0, 0, nullptr);
          kdu_channel_mapping sm;
          sm.configure(1, 8, osig);
          sm.channel_interp[0].normalized_natural_zero = -0.25f;
          sm.channel_interp[0].normalized_max = nmax;
          kdu_core::kdu_dims reg = sdec.get_rendered_image_dims(
              scs, &sm, 0, 0, kdu_core::kdu_coords(1, 1));
          ASSERT_TRUE(sdec.start(scs, &sm, 0, 0, 1, reg,
                                 kdu_core::kdu_coords(1, 1),
                                 kdu_core::kdu_coords(1, 1), false));
          std::vector<kdu_core::kdu_byte> sb(reg.area());
          kdu_core::kdu_byte* sptrs[1] = {sb.data()};
          kdu_core::kdu_dims inc = reg, nr;
          while (!inc.is_empty()) {
            ASSERT_TRUE(sdec.process(sptrs, false, 1, reg.pos, reg.size.x, 128,
                                     reg.area(), inc, nr));
          }
          EXPECT_TRUE(sdec.finish());

          scs.apply_input_restrictions(0, 0, 0, 0, nullptr);
          ASSERT_TRUE(sdec.start(scs, &sm, 0, 0, 1, reg,
                                 kdu_core::kdu_coords(1, 1),
                                 kdu_core::kdu_coords(1, 1), false));
          std::vector<float> fb(reg.area());
          float* fptrs[1] = {fb.data()};
          inc = reg;
          while (!inc.is_empty()) {
            ASSERT_TRUE(sdec.process(fptrs, false, 1, reg.pos, reg.size.x, 128,
                                     reg.area(), inc, nr, true, false, false));
          }
          EXPECT_TRUE(sdec.finish());
        }
      }
    }

    for (int bd : {24, 32}) {
      CodestreamSpec hspec;
      hspec.num_comps = 1;
      hspec.width = 16;
      hspec.height = 16;
      hspec.bit_depth = bd;
      hspec.reversible = true;
      hspec.extra_cod = "Clevels=0";
      auto hbytes = EncodeCodestream(hspec);
      MemorySource hsrc(hbytes);
      kdu_core::kdu_codestream hcs;
      hcs.create(&hsrc);
      hcs.set_persistent();
      auto hclean = absl::MakeCleanup([&]() { hcs.destroy(); });

      kdu_region_decompressor hdec;
      kdu_channel_mapping hm;
      hm.configure(1, 18, true);
      kdu_core::kdu_dims reg = hdec.get_rendered_image_dims(
          hcs, &hm, 0, 0, kdu_core::kdu_coords(1, 1));
      ASSERT_TRUE(hdec.start(hcs, &hm, 0, 0, 1, reg, kdu_core::kdu_coords(1, 1),
                             kdu_core::kdu_coords(1, 1), true));
      std::vector<kdu_core::kdu_uint16> wb(reg.area());
      kdu_core::kdu_uint16* wptrs[1] = {wb.data()};
      kdu_core::kdu_dims inc = reg, nr;
      while (!inc.is_empty()) {
        ASSERT_TRUE(hdec.process(wptrs, false, 1, reg.pos, reg.size.x, 128,
                                 reg.area(), inc, nr, 0));
      }
      EXPECT_TRUE(hdec.finish());

      if (bd == 32) {
        // Default format + 1:2 boxcar on 32-bit reversible -> KDRD_INT64_TYPE.
        for (int ed : {1, 2}) {
          hcs.apply_input_restrictions(0, 0, 0, 0, nullptr);
          kdu_core::kdu_dims breg = hdec.get_rendered_image_dims(
              hcs, &hm, 0, 0, kdu_core::kdu_coords(1, 1),
              kdu_core::kdu_coords(ed, ed));
          ASSERT_TRUE(hdec.start(hcs, &hm, 0, 0, 1, breg,
                                 kdu_core::kdu_coords(1, 1),
                                 kdu_core::kdu_coords(ed, ed), true));
          std::vector<float> fb(breg.area());
          float* fptrs[1] = {fb.data()};
          inc = breg;
          while (!inc.is_empty()) {
            ASSERT_TRUE(hdec.process(fptrs, false, 1, breg.pos, breg.size.x,
                                     128, breg.area(), inc, nr, true));
          }
          EXPECT_TRUE(hdec.finish());
        }
        // JP2_CHANNEL_FORMAT_FLOAT on 32-bit reversible -> precision == 32.
        for (bool is_signed : {false, true}) {
          CodestreamSpec pfspec;
          pfspec.num_comps = 1;
          pfspec.width = 16;
          pfspec.height = 16;
          pfspec.bit_depth = 32;
          pfspec.is_signed = is_signed;
          pfspec.reversible = true;
          pfspec.extra_cod = "Clevels=0";
          pfspec.custom_samples =
              is_signed
                  ? std::vector<kdu_core::kdu_int32>{0, 0x3F800000, -0x3F800000}
                  : std::vector<kdu_core::kdu_int32>{0, 0x3F800000, 0x7F7FFFFF};
          auto pfbytes = EncodeCodestream(pfspec);
          MemorySource pfsrc(pfbytes);
          kdu_core::kdu_codestream pfcs;
          pfcs.create(&pfsrc);
          pfcs.set_persistent();
          auto pfclean = absl::MakeCleanup([&]() { pfcs.destroy(); });

          kdu_channel_mapping pfm;
          pfm.configure(1, 32, is_signed);
          int ep[1] = {8};
          ASSERT_TRUE(pfm.channel_interp[0].init(32, is_signed, 0.0f,
                                                 JP2_CHANNEL_FORMAT_FLOAT, ep));
          for (int ed : {1, 2}) {
            pfcs.apply_input_restrictions(0, 0, 0, 0, nullptr);
            kdu_core::kdu_dims preg = hdec.get_rendered_image_dims(
                pfcs, &pfm, 0, 0, kdu_core::kdu_coords(1, 1),
                kdu_core::kdu_coords(ed, ed));
            ASSERT_TRUE(hdec.start(pfcs, &pfm, 0, 0, 1, preg,
                                   kdu_core::kdu_coords(1, 1),
                                   kdu_core::kdu_coords(ed, ed), true));
            std::vector<float> fb(preg.area());
            float* fptrs[1] = {fb.data()};
            inc = preg;
            while (!inc.is_empty()) {
              ASSERT_TRUE(hdec.process(fptrs, false, 1, preg.pos, preg.size.x,
                                       128, preg.area(), inc, nr, true));
            }
            EXPECT_TRUE(hdec.finish());
          }
        }
      }
    }

    // Scalar 10-bit reversible 1:4 boxcar -> local_convert_and_add upshift < 0.
    {
      const int saved_mmx = kdu_core::kdu_mmx_level;
      auto restore_mmx =
          absl::MakeCleanup([&]() { kdu_core::kdu_mmx_level = saved_mmx; });
      CodestreamSpec aspec;
      aspec.num_comps = 1;
      aspec.width = 24;
      aspec.height = 24;
      aspec.bit_depth = 10;
      aspec.reversible = true;
      auto abytes = EncodeCodestream(aspec);
      MemorySource asrc(abytes);
      kdu_core::kdu_codestream acs;
      acs.create(&asrc);
      acs.set_persistent();
      auto aclean = absl::MakeCleanup([&]() { acs.destroy(); });

      kdu_core::kdu_mmx_level = 0;
      kdu_channel_mapping am;
      am.configure(1, 10, false);
      kdu_region_decompressor adec;
      kdu_core::kdu_dims reg = adec.get_rendered_image_dims(
          acs, &am, 0, 0, kdu_core::kdu_coords(1, 1),
          kdu_core::kdu_coords(4, 4));
      ASSERT_TRUE(adec.start(acs, &am, 0, 0, 1, reg, kdu_core::kdu_coords(1, 1),
                             kdu_core::kdu_coords(4, 4), false));
      std::vector<kdu_core::kdu_uint16> wb(reg.area());
      kdu_core::kdu_uint16* wptrs[1] = {wb.data()};
      kdu_core::kdu_dims inc = reg, nr;
      while (!inc.is_empty()) {
        ASSERT_TRUE(adec.process(wptrs, false, 1, reg.pos, reg.size.x, 128,
                                 reg.area(), inc, nr));
      }
      EXPECT_TRUE(adec.finish());
    }

    // 9. Palette 1:8 boxcar (shift != 0) + horizontal sub-crop (missing < 0)
    // in fix16 & float, cropped multi-tile 32-bit float reinterpret with
    // exponent_bits=5, cropped multi-tile 24-bit int32 copies, and scalar
    // cropped 1:4 irreversible boxcar.
    {
      // 9a. Paletted 32x32 multi-tile 1:8 boxcar + horizontal crop.
      CodestreamSpec pspec;
      pspec.num_comps = 1;
      pspec.width = 32;
      pspec.height = 32;
      pspec.bit_depth = 8;
      pspec.reversible = true;
      pspec.extra_siz = "Stiles={16,16}";
      auto pbytes = EncodeCodestream(pspec);
      MemorySource psrc(pbytes);
      kdu_core::kdu_codestream pcs;
      pcs.create(&psrc);
      pcs.set_persistent();
      auto pclean = absl::MakeCleanup([&]() { pcs.destroy(); });

      for (bool use_float : {false, true}) {
        kdu_channel_mapping pm;
        pm.configure(1, 8, false);
        pm.source_components[0] = 0;
        pm.palette_bits = 8;
        if (use_float) {
          pm.float_palette[0] = new float[256];
          for (int i = 0; i < 256; ++i)
            pm.float_palette[0][i] = (static_cast<float>(i) / 255.0f) - 0.5f;
        } else {
          pm.fix16_palette[0] = new kdu_core::kdu_sample16[256];
          for (int i = 0; i < 256; ++i)
            pm.fix16_palette[0][i].ival =
                static_cast<kdu_core::kdu_int16>((i - 128) << 5);
        }

        pcs.apply_input_restrictions(0, 0, 0, 0, nullptr);
        kdu_region_decompressor pdec;
        kdu_core::kdu_dims full = pdec.get_rendered_image_dims(
            pcs, &pm, 0, 0, kdu_core::kdu_coords(1, 1),
            kdu_core::kdu_coords(8, 8));
        ASSERT_GE(full.size.x, 4);
        kdu_core::kdu_dims crop = full;
        crop.pos.x += 1;
        crop.size.x -= 1;
        ASSERT_TRUE(pdec.start(pcs, &pm, 0, 0, 1, crop,
                               kdu_core::kdu_coords(1, 1),
                               kdu_core::kdu_coords(8, 8), true));
        std::vector<kdu_core::kdu_byte> pb(crop.area());
        kdu_core::kdu_byte* pptrs[1] = {pb.data()};
        kdu_core::kdu_dims inc = crop, nr;
        while (!inc.is_empty()) {
          ASSERT_TRUE(pdec.process(pptrs, false, 1, crop.pos, crop.size.x, 64,
                                   crop.area(), inc, nr));
        }
        EXPECT_TRUE(pdec.finish());
      }

      // 9b. 32-bit unsigned JP2_CHANNEL_FORMAT_FLOAT with exponent_bits=5
      // (mantissa_downshift > 0) and multi-tile horizontal sub-crop across
      // multiple narrow tiles (missing_src_samples < 0 multi-line loop).
      CodestreamSpec f32;
      f32.num_comps = 1;
      f32.width = 24;
      f32.height = 16;
      f32.bit_depth = 32;
      f32.is_signed = false;
      f32.reversible = true;
      f32.extra_siz = "Stiles={16,4}";
      f32.extra_cod = "Clevels=0";
      f32.custom_samples = {0, 0x3F800000, 0x7F7FFFFF};
      auto f32bytes = EncodeCodestream(f32);
      MemorySource f32src(f32bytes);
      kdu_core::kdu_codestream f32cs;
      f32cs.create(&f32src);
      f32cs.set_persistent();
      auto f32clean = absl::MakeCleanup([&]() { f32cs.destroy(); });

      kdu_channel_mapping f32m;
      f32m.configure(1, 32, false);
      int ep5[1] = {5};
      ASSERT_TRUE(f32m.channel_interp[0].init(32, false, 0.0f,
                                              JP2_CHANNEL_FORMAT_FLOAT, ep5));
      f32cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
      kdu_region_decompressor f32dec;
      kdu_core::kdu_dims f32crop;
      f32crop.pos = kdu_core::kdu_coords(9, 2);
      f32crop.size = kdu_core::kdu_coords(12, 10);
      ASSERT_TRUE(f32dec.start(f32cs, &f32m, 0, 0, 1, f32crop,
                               kdu_core::kdu_coords(1, 1),
                               kdu_core::kdu_coords(1, 1), true));
      std::vector<float> f32buf(f32crop.area());
      float* f32ptrs[1] = {f32buf.data()};
      kdu_core::kdu_dims f32inc = f32crop, f32nr;
      while (!f32inc.is_empty()) {
        ASSERT_TRUE(f32dec.process(f32ptrs, false, 1, f32crop.pos,
                                   f32crop.size.x, 64, f32crop.area(), f32inc,
                                   f32nr, true));
      }
      EXPECT_TRUE(f32dec.finish());

      // 9c. 24-bit narrow-tiled reversible & irreversible with horizontal
      // sub-crop across tile boundaries (local_convert_and_copy_to_int32).
      for (bool rev : {true, false}) {
        CodestreamSpec i24;
        i24.num_comps = 1;
        i24.width = 24;
        i24.height = 16;
        i24.bit_depth = 24;
        i24.is_signed = true;
        i24.reversible = rev;
        i24.extra_siz = "Stiles={16,4}";
        i24.extra_cod = "Clevels=0";
        auto i24bytes = EncodeCodestream(i24);
        MemorySource i24src(i24bytes);
        kdu_core::kdu_codestream i24cs;
        i24cs.create(&i24src);
        i24cs.set_persistent();
        auto i24clean = absl::MakeCleanup([&]() { i24cs.destroy(); });

        kdu_channel_mapping i24m;
        i24m.configure(1, 24, true);
        i24cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
        kdu_region_decompressor i24dec;
        kdu_core::kdu_dims i24crop;
        i24crop.pos = kdu_core::kdu_coords(9, 2);
        i24crop.size = kdu_core::kdu_coords(12, 10);
        ASSERT_TRUE(i24dec.start(i24cs, &i24m, 0, 0, 1, i24crop,
                                 kdu_core::kdu_coords(1, 1),
                                 kdu_core::kdu_coords(1, 1), true));
        std::vector<kdu_core::kdu_uint16> i24w(i24crop.area());
        kdu_core::kdu_uint16* i24ptrs[1] = {i24w.data()};
        kdu_core::kdu_dims i24inc = i24crop, i24nr;
        while (!i24inc.is_empty()) {
          ASSERT_TRUE(i24dec.process(i24ptrs, false, 1, i24crop.pos,
                                     i24crop.size.x, 64, i24crop.area(), i24inc,
                                     i24nr, 16, true));
        }
        EXPECT_TRUE(i24dec.finish());
      }

      // 9d. Scalar 8-bit irreversible 1:4 boxcar with horizontal sub-crop
      // across narrow tiles (local_convert_and_add_float missing < 0).
      const int saved_mmx = kdu_core::kdu_mmx_level;
      auto restore_mmx =
          absl::MakeCleanup([&]() { kdu_core::kdu_mmx_level = saved_mmx; });
      CodestreamSpec fbox;
      fbox.num_comps = 1;
      fbox.width = 48;
      fbox.height = 32;
      fbox.bit_depth = 8;
      fbox.reversible = false;
      fbox.extra_siz = "Stiles={16,12}";
      auto fbbytes = EncodeCodestream(fbox);
      MemorySource fbsrc(fbbytes);
      kdu_core::kdu_codestream fbcs;
      fbcs.create(&fbsrc);
      fbcs.set_persistent();
      auto fbclean = absl::MakeCleanup([&]() { fbcs.destroy(); });

      kdu_core::kdu_mmx_level = 0;
      kdu_channel_mapping fbm;
      fbm.configure(1, 8, false);
      fbcs.apply_input_restrictions(0, 0, 0, 0, nullptr);
      kdu_region_decompressor fbdec;
      kdu_core::kdu_dims fbcrop = fbdec.get_rendered_image_dims(
          fbcs, &fbm, 0, 0, kdu_core::kdu_coords(1, 1),
          kdu_core::kdu_coords(4, 4));
      fbcrop.pos.x += 5;
      fbcrop.size.x -= 5;
      ASSERT_TRUE(fbdec.start(fbcs, &fbm, 0, 0, 1, fbcrop,
                              kdu_core::kdu_coords(1, 1),
                              kdu_core::kdu_coords(4, 4), true));
      std::vector<float> fbbuf(fbcrop.area());
      float* fbptrs[1] = {fbbuf.data()};
      kdu_core::kdu_dims fbinc = fbcrop, fbnr;
      while (!fbinc.is_empty()) {
        ASSERT_TRUE(fbdec.process(fbptrs, false, 1, fbcrop.pos, fbcrop.size.x,
                                  64, fbcrop.area(), fbinc, fbnr, true));
      }
      EXPECT_TRUE(fbdec.finish());
    }
  }
}

TEST_F(KduRegionDecompressorTest,
       ScalingHighPrecAndHorizontalCropSection10To13) {
  // 10. Cover local_convert_and_copy_to_int32 (FIX16, INT32, FLOAT, INT64
  // sources via precision_bits > 16), set_true_scaling(false, true), and
  // INT32 float reinterpret with mantissa_downshift > 0.
  {
    // 10a. local_convert_and_copy_to_int32 with precision_bits = 20 / 28.
    struct Int32SrcCase {
      int bit_depth;
      bool reversible;
      bool precise;
      int out_prec;
    };
    const Int32SrcCase kCases[] = {
        {8, true, false, 20},  // KDRD_FIX16_TYPE -> KDRD_INT32_TYPE
        {8, false, true, 20},  // KDRD_FLOAT_TYPE -> KDRD_INT32_TYPE
        {24, true, true, 18},  // KDRD_INT32_TYPE downshift
        {24, true, true, 28},  // KDRD_INT32_TYPE upshift
        {32, true, true, 24},  // KDRD_INT64_TYPE -> KDRD_INT32_TYPE
    };
    for (const auto& c : kCases) {
      CodestreamSpec ispec;
      ispec.num_comps = 1;
      ispec.width = 16;
      ispec.height = 16;
      ispec.bit_depth = c.bit_depth;
      ispec.is_signed = true;
      ispec.reversible = c.reversible;
      ispec.extra_cod = "Clevels=0";
      auto ibytes = EncodeCodestream(ispec);
      MemorySource isrc(ibytes);
      kdu_core::kdu_codestream ics;
      ics.create(&isrc);
      ics.set_persistent();
      auto iclean = absl::MakeCleanup([&]() { ics.destroy(); });

      kdu_channel_mapping im;
      im.configure(1, c.bit_depth, true);
      kdu_region_decompressor idec;
      for (int num : {1, 2}) {
        ics.apply_input_restrictions(0, 0, 0, 0, nullptr);
        kdu_core::kdu_coords en(num, num);
        kdu_core::kdu_coords ed(1, 1);
        kdu_core::kdu_dims ireg =
            idec.get_rendered_image_dims(ics, &im, 0, 0, en, ed);
        ASSERT_TRUE(idec.start(ics, &im, 0, 0, 1, ireg, en, ed, c.precise));
        std::vector<kdu_core::kdu_uint16> iw(ireg.area());
        kdu_core::kdu_uint16* iwptrs[1] = {iw.data()};
        kdu_core::kdu_dims iinc = ireg, inr;
        while (!iinc.is_empty()) {
          ASSERT_TRUE(idec.process(iwptrs, false, 1, ireg.pos, ireg.size.x, 128,
                                   ireg.area(), iinc, inr, c.out_prec));
        }
        EXPECT_TRUE(idec.finish());
      }
    }

    // 10b. set_true_scaling(false, true) across byte/word/int32 and
    // fix16/float source types.
    CodestreamSpec tspec;
    tspec.num_comps = 1;
    tspec.width = 16;
    tspec.height = 16;
    tspec.bit_depth = 8;
    tspec.reversible = false;
    auto tbytes = EncodeCodestream(tspec);
    MemorySource tsrc(tbytes);
    kdu_core::kdu_codestream tcs;
    tcs.create(&tsrc);
    tcs.set_persistent();
    auto tclean = absl::MakeCleanup([&]() { tcs.destroy(); });

    kdu_region_decompressor tdec;
    tdec.set_true_scaling(false, true);
    for (bool osig : {false, true}) {
      for (float nmax : {0.25f, -0.4f}) {
        for (bool precise : {false, true}) {
          tcs.apply_input_restrictions(0, 0, 0, 0, nullptr);
          kdu_channel_mapping tm;
          tm.configure(1, 8, osig);
          tm.channel_interp[0].normalized_natural_zero = -0.25f;
          tm.channel_interp[0].normalized_max = nmax;
          kdu_core::kdu_dims reg = tdec.get_rendered_image_dims(
              tcs, &tm, 0, 0, kdu_core::kdu_coords(1, 1));
          ASSERT_TRUE(tdec.start(tcs, &tm, 0, 0, 1, reg,
                                 kdu_core::kdu_coords(1, 1),
                                 kdu_core::kdu_coords(1, 1), precise));
          std::vector<kdu_core::kdu_byte> tb(reg.area());
          kdu_core::kdu_byte* tptrs[1] = {tb.data()};
          kdu_core::kdu_dims inc = reg, nr;
          while (!inc.is_empty()) {
            ASSERT_TRUE(tdec.process(tptrs, false, 1, reg.pos, reg.size.x, 128,
                                     reg.area(), inc, nr));
          }
          EXPECT_TRUE(tdec.finish());

          tcs.apply_input_restrictions(0, 0, 0, 0, nullptr);
          ASSERT_TRUE(tdec.start(tcs, &tm, 0, 0, 1, reg,
                                 kdu_core::kdu_coords(1, 1),
                                 kdu_core::kdu_coords(1, 1), precise));
          std::vector<kdu_core::kdu_uint16> tw(reg.area());
          kdu_core::kdu_uint16* twptrs[1] = {tw.data()};
          inc = reg;
          while (!inc.is_empty()) {
            ASSERT_TRUE(tdec.process(twptrs, false, 1, reg.pos, reg.size.x, 128,
                                     reg.area(), inc, nr, 20));
          }
          EXPECT_TRUE(tdec.finish());
        }
      }
    }

    // 10c. 28-bit INT32 float reinterpret with exponent_bits = 3 ->
    // mantissa_bits = 24 > 23 -> mantissa_downshift = 1.
    for (bool is_signed : {false, true}) {
      CodestreamSpec rspec;
      rspec.num_comps = 1;
      rspec.width = 16;
      rspec.height = 16;
      rspec.bit_depth = 28;
      rspec.is_signed = is_signed;
      rspec.reversible = true;
      rspec.extra_cod = "Clevels=0";
      auto rbytes = EncodeCodestream(rspec);
      MemorySource rsrc(rbytes);
      kdu_core::kdu_codestream rcs;
      rcs.create(&rsrc);
      rcs.set_persistent();
      auto rclean = absl::MakeCleanup([&]() { rcs.destroy(); });

      kdu_channel_mapping rm;
      rm.configure(1, 28, is_signed);
      int ep3[1] = {3};
      ASSERT_TRUE(rm.channel_interp[0].init(28, is_signed, 0.0f,
                                            JP2_CHANNEL_FORMAT_FLOAT, ep3));
      kdu_region_decompressor rdec;
      kdu_core::kdu_dims reg = rdec.get_rendered_image_dims(
          rcs, &rm, 0, 0, kdu_core::kdu_coords(1, 1));
      ASSERT_TRUE(rdec.start(rcs, &rm, 0, 0, 1, reg, kdu_core::kdu_coords(1, 1),
                             kdu_core::kdu_coords(1, 1), true));
      std::vector<float> rf(reg.area());
      float* rfptrs[1] = {rf.data()};
      kdu_core::kdu_dims inc = reg, nr;
      while (!inc.is_empty()) {
        ASSERT_TRUE(rdec.process(rfptrs, false, 1, reg.pos, reg.size.x, 128,
                                 reg.area(), inc, nr, true));
      }
      EXPECT_TRUE(rdec.finish());
    }
  }

  // 11. Cover local_convert_and_copy_to_int32 FLOAT/FIX16/INT32 upshift via
  // default_rendering_precision > 16, JP2_CMYK_SPACE with precise=true
  // across byte/uint16/float outputs, true_zero with zeta != 0, and 26-bit
  // INT32 float reinterpret with exponent_bits = 1.
  {
    // 11a. default_rendering_precision > 16 -> dst_type == KDRD_INT32_TYPE.
    struct Int32DefCase {
      int src_bits;
      bool reversible;
      bool precise;
      int def_prec;
    };
    const Int32DefCase kDefCases[] = {
        {8, true, false, 20},   // KDRD_FIX16_TYPE -> KDRD_INT32_TYPE
        {24, false, true, 24},  // KDRD_FLOAT_TYPE -> KDRD_INT32_TYPE
        {20, true, true, 24},   // KDRD_INT32_TYPE upshift > 0
    };
    for (const auto& c : kDefCases) {
      CodestreamSpec ispec;
      ispec.num_comps = 1;
      ispec.width = 16;
      ispec.height = 16;
      ispec.bit_depth = c.src_bits;
      ispec.is_signed = true;
      ispec.reversible = c.reversible;
      ispec.extra_cod = "Clevels=0";
      auto ibytes = EncodeCodestream(ispec);
      MemorySource isrc(ibytes);
      kdu_core::kdu_codestream ics;
      ics.create(&isrc);
      ics.set_persistent();
      auto iclean = absl::MakeCleanup([&]() { ics.destroy(); });

      kdu_channel_mapping im;
      im.configure(1, c.def_prec, true);
      kdu_region_decompressor idec;
      for (int num : {1, 2}) {
        ics.apply_input_restrictions(0, 0, 0, 0, nullptr);
        kdu_core::kdu_coords en(num, num);
        kdu_core::kdu_coords ed(1, 1);
        kdu_core::kdu_dims ireg =
            idec.get_rendered_image_dims(ics, &im, 0, 0, en, ed);
        ASSERT_TRUE(idec.start(ics, &im, 0, 0, 1, ireg, en, ed, c.precise));
        std::vector<kdu_core::kdu_uint16> iw(ireg.area());
        kdu_core::kdu_uint16* iwptrs[1] = {iw.data()};
        kdu_core::kdu_dims iinc = ireg, inr;
        while (!iinc.is_empty()) {
          ASSERT_TRUE(idec.process(iwptrs, false, 1, ireg.pos, ireg.size.x, 128,
                                   ireg.area(), iinc, inr, 0));
        }
        EXPECT_TRUE(idec.finish());
      }
    }

    // 11b. 4-channel JP2_CMYK_SPACE irreversible with precise=true.
    for (jp2_colour_space sp : {JP2_CMYK_SPACE}) {
      CodestreamSpec cspec;
      cspec.num_comps = 4;
      cspec.width = 16;
      cspec.height = 16;
      cspec.bit_depth = 8;
      cspec.reversible = false;
      kdu_core::siz_params siz;
      ConfigureSizParams(&siz, cspec);
      MemoryTarget raw_jpx;
      jp2_family_tgt ftgt;
      ftgt.open(&raw_jpx);
      jpx_target jout;
      jout.open(&ftgt);
      jpx_codestream_target jst = jout.add_codestream();
      jpx_layer_target jlt = jout.add_layer();
      kdu_core::kdu_codestream ocs;
      ocs.create(&siz, jst.access_stream());
      ConfigureCodParams(ocs, cspec);
      jp2_dimensions jdim = jst.access_dimensions();
      jdim.init(ocs.access_siz(), false);
      jdim.finalize_compatibility(ocs.access_siz());
      jp2_colour jcol = jlt.add_colour();
      jcol.init(sp);
      jp2_channels jchn = jlt.access_channels();
      jchn.init(4);
      for (int c = 0; c < 4; ++c) jchn.set_colour_mapping(c, c);
      jout.write_headers();
      jst.open_stream()->set_rubber_length();
      kdu_stripe_compressor comp;
      comp.start(ocs);
      std::vector<kdu_core::kdu_byte> src_buf(16 * 16 * 4, 100);
      int heights[4] = {16, 16, 16, 16};
      comp.push_stripe(src_buf.data(), heights);
      comp.finish();
      ocs.destroy();
      jst.access_stream()->close();
      jout.close();
      ftgt.close();

      MemorySource mem_src(raw_jpx.data());
      jp2_family_src jfam;
      jpx_source jsrc;
      kdu_core::kdu_codestream cs;
      auto cleanup = absl::MakeCleanup([&]() {
        if (cs.exists()) cs.destroy();
        jsrc.close();
        jfam.close();
      });
      jfam.open(&mem_src);
      ASSERT_EQ(jsrc.open(&jfam, true), 1);
      jpx_layer_source jls = jsrc.access_layer(0, true);
      jpx_codestream_source jcss = jsrc.access_codestream(0, true);
      cs.create(jcss.open_stream());
      cs.set_persistent();

      kdu_channel_mapping mapping;
      ASSERT_TRUE(mapping.configure(jls.access_colour(0), jls.access_channels(),
                                    0, jcss.access_palette(),
                                    jcss.access_dimensions()));
      kdu_region_decompressor dec;
      kdu_core::kdu_dims reg = dec.get_rendered_image_dims(
          cs, &mapping, 0, 0, kdu_core::kdu_coords(1, 1));

      // Byte, word, and float outputs with precise=true.
      for (bool norm : {true, false}) {
        cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
        ASSERT_TRUE(dec.start(cs, &mapping, 0, 0, 1, reg,
                              kdu_core::kdu_coords(1, 1),
                              kdu_core::kdu_coords(1, 1), true));
        std::vector<std::vector<float>> fplanes(mapping.num_channels,
                                                std::vector<float>(reg.area()));
        std::vector<float*> fptrs(mapping.num_channels);
        for (int c = 0; c < mapping.num_channels; ++c)
          fptrs[c] = fplanes[c].data();
        kdu_core::kdu_dims inc = reg, nr;
        while (!inc.is_empty()) {
          ASSERT_TRUE(dec.process(fptrs.data(), false, 1, reg.pos, reg.size.x,
                                  128, reg.area(), inc, nr, norm, true, norm));
        }
        EXPECT_TRUE(dec.finish());
      }
      {
        cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
        ASSERT_TRUE(dec.start(cs, &mapping, 0, 0, 1, reg,
                              kdu_core::kdu_coords(1, 1),
                              kdu_core::kdu_coords(1, 1), true));
        std::vector<std::vector<kdu_core::kdu_uint16>> wplanes(
            mapping.num_channels,
            std::vector<kdu_core::kdu_uint16>(reg.area()));
        std::vector<kdu_core::kdu_uint16*> wptrs(mapping.num_channels);
        for (int c = 0; c < mapping.num_channels; ++c)
          wptrs[c] = wplanes[c].data();
        kdu_core::kdu_dims inc = reg, nr;
        while (!inc.is_empty()) {
          ASSERT_TRUE(dec.process(wptrs.data(), false, 1, reg.pos, reg.size.x,
                                  128, reg.area(), inc, nr, 12));
        }
        EXPECT_TRUE(dec.finish());
      }
    }

    // 11c. True-zero scaling initialized with zeta=0.25f via channel_interp
    // init (for byte/word/float and fix16/float source).
    {
      CodestreamSpec zspec;
      zspec.num_comps = 1;
      zspec.width = 16;
      zspec.height = 16;
      zspec.bit_depth = 8;
      zspec.reversible = false;
      auto zbytes = EncodeCodestream(zspec);
      MemorySource zsrc(zbytes);
      kdu_core::kdu_codestream zcs;
      zcs.create(&zsrc);
      zcs.set_persistent();
      auto zclean = absl::MakeCleanup([&]() { zcs.destroy(); });

      for (bool t_max : {false, true}) {
        kdu_region_decompressor zdec;
        zdec.set_true_scaling(t_max, true);
        for (bool precise : {false, true}) {
          zcs.apply_input_restrictions(0, 0, 0, 0, nullptr);
          kdu_channel_mapping zm;
          zm.configure(1, 8, false);
          ASSERT_TRUE(zm.channel_interp[0].init(8, false, 0.25f));
          kdu_core::kdu_dims reg = zdec.get_rendered_image_dims(
              zcs, &zm, 0, 0, kdu_core::kdu_coords(1, 1));
          ASSERT_TRUE(zdec.start(zcs, &zm, 0, 0, 1, reg,
                                 kdu_core::kdu_coords(1, 1),
                                 kdu_core::kdu_coords(1, 1), precise));
          std::vector<kdu_core::kdu_byte> zb(reg.area());
          kdu_core::kdu_byte* zptrs[1] = {zb.data()};
          kdu_core::kdu_dims inc = reg, nr;
          while (!inc.is_empty()) {
            ASSERT_TRUE(zdec.process(zptrs, false, 1, reg.pos, reg.size.x, 128,
                                     reg.area(), inc, nr));
          }
          EXPECT_TRUE(zdec.finish());
        }
      }
    }

    // 11d. 26-bit JP2_CHANNEL_FORMAT_FLOAT with exponent_bits = 1 ->
    // mantissa_bits = 24 > 23 (INT32 mantissa_downshift).
    for (bool is_signed : {false, true}) {
      CodestreamSpec qspec;
      qspec.num_comps = 1;
      qspec.width = 16;
      qspec.height = 16;
      qspec.bit_depth = 26;
      qspec.is_signed = is_signed;
      qspec.reversible = true;
      qspec.extra_cod = "Clevels=0";
      auto qbytes = EncodeCodestream(qspec);
      MemorySource qsrc(qbytes);
      kdu_core::kdu_codestream qcs;
      qcs.create(&qsrc);
      qcs.set_persistent();
      auto qclean = absl::MakeCleanup([&]() { qcs.destroy(); });

      kdu_channel_mapping qm;
      qm.configure(1, 26, is_signed);
      int ep1[1] = {1};
      ASSERT_TRUE(qm.channel_interp[0].init(26, is_signed, 0.0f,
                                            JP2_CHANNEL_FORMAT_FLOAT, ep1));
      kdu_region_decompressor qdec;
      kdu_core::kdu_dims reg = qdec.get_rendered_image_dims(
          qcs, &qm, 0, 0, kdu_core::kdu_coords(1, 1));
      ASSERT_TRUE(qdec.start(qcs, &qm, 0, 0, 1, reg, kdu_core::kdu_coords(1, 1),
                             kdu_core::kdu_coords(1, 1), true));
      std::vector<float> qf(reg.area());
      float* qfptrs[1] = {qf.data()};
      kdu_core::kdu_dims inc = reg, nr;
      while (!inc.is_empty()) {
        ASSERT_TRUE(qdec.process(qfptrs, false, 1, reg.pos, reg.size.x, 128,
                                 reg.area(), inc, nr, true));
      }
      EXPECT_TRUE(qdec.finish());
    }
  }

  // 12. Horizontal skip_cols > 0 (incomplete_region.pos.x > region.pos.x
  // with narrow multi-tile Stiles={16,4}) -> missing_src_samples < 0 across
  // fix16/int32/float, scalar boxcar, float reinterpret, and palette paths.
  {
    const int saved_mmx = kdu_core::kdu_mmx_level;
    auto restore_mmx =
        absl::MakeCleanup([&]() { kdu_core::kdu_mmx_level = saved_mmx; });
    kdu_core::kdu_mmx_level = 0;

    struct SkipCase {
      int bit_depth;
      bool is_signed;
      bool reversible;
      bool precise;
      bool use_float_format;
      bool use_palette;
      bool palette_float;
      int out_prec;
    };
    const SkipCase kSkipCases[] = {
        {8, false, true, false, false, false, false, 8},  // fix16
        {8, false, false, true, false, false, false, 8},  // float
        {24, true, true, true, false, false, false, 24},  // int32
        {16, false, true, true, true, false, false, 0},   // ufloat
        {16, true, true, true, true, false, false, 0},    // sfloat
        {8, false, true, false, false, true, false, 8},   // pal fix16
        {8, false, false, true, false, true, true, 8},    // pal float
    };
    for (const auto& sc : kSkipCases) {
      CodestreamSpec spec;
      spec.num_comps = 1;
      spec.width = 32;
      spec.height = 16;
      spec.bit_depth = sc.bit_depth;
      spec.is_signed = sc.is_signed;
      spec.reversible = sc.reversible;
      spec.extra_siz = "Stiles={16,4}";
      spec.extra_cod = "Clevels=0";
      auto bytes = EncodeCodestream(spec);
      MemorySource src(bytes);
      kdu_core::kdu_codestream cs;
      cs.create(&src);
      cs.set_persistent();
      auto cclean = absl::MakeCleanup([&]() { cs.destroy(); });

      kdu_channel_mapping m;
      m.configure(1, sc.bit_depth, sc.is_signed);
      if (sc.use_float_format) {
        int ep[1] = {5};
        ASSERT_TRUE(m.channel_interp[0].init(sc.bit_depth, sc.is_signed, 0.0f,
                                             JP2_CHANNEL_FORMAT_FLOAT, ep));
      }
      if (sc.use_palette) {
        m.source_components[0] = 0;
        m.palette_bits = 8;
        if (sc.palette_float) {
          m.float_palette[0] = new float[256];
          for (int i = 0; i < 256; ++i)
            m.float_palette[0][i] = (static_cast<float>(i) / 255.0f) - 0.5f;
        } else {
          m.fix16_palette[0] = new kdu_core::kdu_sample16[256];
          for (int i = 0; i < 256; ++i)
            m.fix16_palette[0][i].ival =
                static_cast<kdu_core::kdu_int16>((i - 128) << 5);
        }
      }

      kdu_region_decompressor dec;
      for (int ed : {1, 2}) {
        cs.apply_input_restrictions(0, 0, 0, 0, nullptr);
        kdu_core::kdu_coords en(1, 1);
        kdu_core::kdu_coords edc(ed, ed);
        kdu_core::kdu_dims full =
            dec.get_rendered_image_dims(cs, &m, 0, 0, en, edc);
        ASSERT_TRUE(dec.start(cs, &m, 0, 0, 1, full, en, edc, sc.precise));
        kdu_core::kdu_dims step1 = full, nr;
        kdu_core::kdu_dims sub;
        sub.pos = kdu_core::kdu_coords(full.pos.x + (10 / ed), full.pos.y + 2);
        sub.size =
            kdu_core::kdu_coords(full.size.x - (10 / ed), full.size.y - 2);
        const int row1_px = full.size.x * 2;
        if (sc.use_float_format ||
            (sc.precise && !sc.use_palette && sc.out_prec == 8)) {
          std::vector<float> fb(full.area());
          float* fptrs[1] = {fb.data()};
          ASSERT_TRUE(dec.process(fptrs, false, 1, full.pos, full.size.x, 2,
                                  row1_px, step1, nr, true));
          kdu_core::kdu_dims inc = sub;
          while (!inc.is_empty()) {
            ASSERT_TRUE(dec.process(fptrs, false, 1, full.pos, full.size.x, 128,
                                    full.area(), inc, nr, true));
          }
        } else if (sc.out_prec > 8) {
          std::vector<kdu_core::kdu_uint16> wb(full.area());
          kdu_core::kdu_uint16* wptrs[1] = {wb.data()};
          ASSERT_TRUE(dec.process(wptrs, false, 1, full.pos, full.size.x, 2,
                                  row1_px, step1, nr, 0));
          kdu_core::kdu_dims inc = sub;
          while (!inc.is_empty()) {
            ASSERT_TRUE(dec.process(wptrs, false, 1, full.pos, full.size.x, 128,
                                    full.area(), inc, nr, 0));
          }
        } else {
          std::vector<kdu_core::kdu_byte> bb(full.area());
          kdu_core::kdu_byte* bptrs[1] = {bb.data()};
          ASSERT_TRUE(dec.process(bptrs, false, 1, full.pos, full.size.x, 2,
                                  row1_px, step1, nr));
          kdu_core::kdu_dims inc = sub;
          while (!inc.is_empty()) {
            ASSERT_TRUE(dec.process(bptrs, false, 1, full.pos, full.size.x, 128,
                                    full.area(), inc, nr));
          }
        }
        EXPECT_TRUE(dec.finish());
      }
    }
  }

  // 13. JPX kdu_channel_mapping::configure edge cases (codestream_idx
  // mismatch, fixpoint format, empty channels).
  {
    CodestreamSpec cspec;
    cspec.num_comps = 3;
    cspec.width = 16;
    cspec.height = 16;
    cspec.bit_depth = 8;
    cspec.reversible = true;
    kdu_core::siz_params siz;
    ConfigureSizParams(&siz, cspec);
    MemoryTarget raw_jpx;
    jp2_family_tgt ftgt;
    ftgt.open(&raw_jpx);
    jpx_target jout;
    jout.open(&ftgt);
    jpx_codestream_target jst = jout.add_codestream();
    jpx_layer_target jlt = jout.add_layer();
    kdu_core::kdu_codestream ocs;
    ocs.create(&siz, jst.access_stream());
    ConfigureCodParams(ocs, cspec);
    jp2_dimensions jdim = jst.access_dimensions();
    jdim.init(ocs.access_siz(), false);
    jdim.finalize_compatibility(ocs.access_siz());
    jp2_colour jcol = jlt.add_colour();
    jcol.init(JP2_sRGB_SPACE);
    jp2_channels jchn = jlt.access_channels();
    jchn.init(3);
    int fp[1] = {2};
    for (int c = 0; c < 3; ++c) {
      jchn.set_colour_mapping(c, c, -1, 0, JP2_CHANNEL_FORMAT_FIXPOINT, fp);
    }
    jout.write_headers();
    jst.open_stream()->set_rubber_length();
    kdu_stripe_compressor comp;
    comp.start(ocs);
    std::vector<kdu_core::kdu_byte> src_buf(16 * 16 * 3, 90);
    int heights[3] = {16, 16, 16};
    comp.push_stripe(src_buf.data(), heights);
    comp.finish();
    ocs.destroy();
    jst.access_stream()->close();
    jout.close();
    ftgt.close();

    MemorySource mem_src(raw_jpx.data());
    jp2_family_src jfam;
    jpx_source jsrc;
    kdu_core::kdu_codestream cs;
    auto cleanup = absl::MakeCleanup([&]() {
      if (cs.exists()) cs.destroy();
      jsrc.close();
      jfam.close();
    });
    jfam.open(&mem_src);
    ASSERT_EQ(jsrc.open(&jfam, true), 1);
    jpx_layer_source jls = jsrc.access_layer(0, true);
    jpx_codestream_source jcss = jsrc.access_codestream(0, true);
    cs.create(jcss.open_stream());
    cs.set_persistent();

    kdu_channel_mapping mapping;
    // Non-matching codestream_idx falls back via last_valid_c / fails safely.
    mapping.configure(jls.access_colour(0), jls.access_channels(), 1,
                      jcss.access_palette(), jcss.access_dimensions());
    ASSERT_TRUE(mapping.configure(jls.access_colour(0), jls.access_channels(),
                                  0, jcss.access_palette(),
                                  jcss.access_dimensions()));
    kdu_region_decompressor dec;
    kdu_core::kdu_dims reg = dec.get_rendered_image_dims(
        cs, &mapping, 0, 0, kdu_core::kdu_coords(1, 1));
    ASSERT_TRUE(dec.start(cs, &mapping, 0, 0, 1, reg,
                          kdu_core::kdu_coords(1, 1),
                          kdu_core::kdu_coords(1, 1), false));
    std::vector<kdu_core::kdu_byte> bbuf(reg.area() * 3);
    int off3[3] = {0, 1, 2};
    kdu_core::kdu_dims inc = reg, nr;
    while (!inc.is_empty()) {
      ASSERT_TRUE(dec.process(bbuf.data(), off3, 3, reg.pos, reg.size.x, 128,
                              reg.area(), inc, nr));
    }
    EXPECT_TRUE(dec.finish());
  }
}

TEST_F(KduRegionDecompressorTest, ValidationAndErrorPathsSection14) {
  CodestreamSpec spec;
  spec.width = 16;
  spec.height = 16;
  spec.num_comps = 3;
  spec.bit_depth = 8;
  spec.use_ycc = false;
  auto bytes = EncodeCodestream(spec);
  MemorySource src(bytes);
  kdu_core::kdu_codestream cs;
  cs.create(&src);
  auto cs_cleanup = absl::MakeCleanup([&]() {
    if (cs.exists()) cs.destroy();
  });

  // 1. kdu_channel_mapping::configure(jp2_colour, jp2_channels, ...) with
  //    0 channels / missing map.
  {
    kdu_channel_mapping bad_map;
    MemoryTarget mtgt;
    jp2_family_tgt ftgt;
    jpx_target jout;
    ftgt.open(&mtgt);
    jout.open(&ftgt);
    jpx_layer_target jlt = jout.add_layer();
    jpx_codestream_target jst = jout.add_codestream();
    kdu_core::siz_params osiz;
    CodestreamSpec jspec;
    jspec.width = 16;
    jspec.height = 16;
    jspec.num_comps = 3;
    jspec.bit_depth = 8;
    jspec.use_ycc = false;
    ConfigureSizParams(&osiz, jspec);
    kdu_core::kdu_codestream ocs;
    ocs.create(&osiz, jst.access_stream());
    ConfigureCodParams(ocs, jspec);
    jp2_dimensions jdim = jst.access_dimensions();
    jdim.init(ocs.access_siz(), false);
    jdim.finalize_compatibility(ocs.access_siz());
    jp2_colour jcol = jlt.add_colour();
    jcol.init(JP2_sRGB_SPACE);
    jp2_channels jchn = jlt.access_channels();
    EXPECT_THROW(bad_map.configure(jcol, jchn, 0, jst.access_palette(), jdim),
                 kdu_core::kdu_exception);
    hooks_.clear();

    // Missing colour mapping (only opacity mapping set for channel 0).
    jchn.init(3);
    jchn.set_opacity_mapping(0, 0, -1, 0);
    EXPECT_THROW(bad_map.configure(jcol, jchn, 0, jst.access_palette(), jdim),
                 kdu_core::kdu_exception);
    hooks_.clear();

    ocs.destroy();
    jst.access_stream()->close();
    jout.close();
    ftgt.close();
  }

  // 2. Geometry helpers invalid single_component.
  {
    double min_prod = 0.0, max_x = 0.0, max_y = 0.0;
    EXPECT_THROW(kdu_region_decompressor::get_safe_expansion_factors(
                     cs, nullptr, 99, 0, min_prod, max_x, max_y),
                 kdu_core::kdu_exception);
    hooks_.clear();
  }

  // 3. start() validation errors (caught internally by start(), returns false).
  {
    kdu_core::kdu_dims reg;
    reg.pos = kdu_core::kdu_coords(0, 0);
    reg.size = kdu_core::kdu_coords(16, 16);
    {
      MemorySource s3(bytes);
      kdu_core::kdu_codestream cs3;
      cs3.create(&s3);
      auto c3 = absl::MakeCleanup([&]() { cs3.destroy(); });
      kdu_region_decompressor dec;
      EXPECT_FALSE(
          dec.start(cs3, nullptr, -1, 0, 1, reg, kdu_core::kdu_coords(1, 1)));
      hooks_.clear();
      EXPECT_FALSE(
          dec.start(cs3, nullptr, 99, 0, 1, reg, kdu_core::kdu_coords(1, 1)));
      hooks_.clear();

      kdu_channel_mapping bad_comp_map;
      bad_comp_map.configure(3, 8, false);
      bad_comp_map.source_components[0] = 99;
      EXPECT_FALSE(dec.start(cs3, &bad_comp_map, 0, 0, 1, reg,
                             kdu_core::kdu_coords(1, 1)));
      hooks_.clear();
    }
  }

  // 4. process() & process_generic() validation and failure reuse paths.
  {
    MemorySource s4(bytes);
    kdu_core::kdu_codestream cs4;
    cs4.create(&s4);
    auto c4 = absl::MakeCleanup([&]() { cs4.destroy(); });
    kdu_channel_mapping map3;
    map3.configure(cs4);
    kdu_region_decompressor dec;
    kdu_core::kdu_dims reg = dec.get_rendered_image_dims(
        cs4, &map3, 0, 0, kdu_core::kdu_coords(1, 1));
    std::vector<kdu_core::kdu_int32> pbuf(reg.area() * 2, 0);
    kdu_core::kdu_dims inc = reg, nr;

    // Out-of-bounds incomplete_region completely outside reg returns false /
    // triggers KDU_ERROR caught cleanly by process_generic.
    {
      kdu_region_decompressor err_dec;
      ASSERT_TRUE(
          err_dec.start(cs4, &map3, 0, 0, 1, reg, kdu_core::kdu_coords(1, 1)));
      kdu_core::kdu_dims bad_inc;
      bad_inc.pos = kdu_core::kdu_coords(50, 50);
      bad_inc.size = kdu_core::kdu_coords(4, 4);
      EXPECT_FALSE(err_dec.process(pbuf.data(), reg.pos, reg.size.x, 128,
                                   reg.area(), bad_inc, nr));
      hooks_.clear();
      EXPECT_TRUE(err_dec.finish());
    }

    // Valid initialized empty incomplete_region & packed int32 process.
    MemorySource s4b(bytes);
    kdu_core::kdu_codestream cs4b;
    cs4b.create(&s4b);
    auto c4b = absl::MakeCleanup([&]() { cs4b.destroy(); });
    ASSERT_TRUE(
        dec.start(cs4b, &map3, 0, 0, 1, reg, kdu_core::kdu_coords(1, 1)));
    kdu_core::kdu_dims empty_inc;
    empty_inc.pos = kdu_core::kdu_coords(0, 0);
    empty_inc.size = kdu_core::kdu_coords(0, 0);
    EXPECT_FALSE(dec.process(pbuf.data(), reg.pos, reg.size.x, 128, reg.area(),
                             empty_inc, nr));
    inc = reg;
    while (!inc.is_empty()) {
      ASSERT_TRUE(dec.process(pbuf.data(), reg.pos, reg.size.x, 128, reg.area(),
                              inc, nr));
    }
    EXPECT_TRUE(dec.finish());
  }

  // Packed int32 with 1 channel and byte-based row_gap on planar kdu_byte**.
  {
    kdu_channel_mapping map1;
    map1.configure(1, 8, false);
    kdu_region_decompressor dec;
    kdu_core::kdu_dims reg = dec.get_rendered_image_dims(
        cs, &map1, 0, 0, kdu_core::kdu_coords(1, 1));
    ASSERT_TRUE(dec.start(cs, &map1, 0, 0, 1, reg, kdu_core::kdu_coords(1, 1)));
    std::vector<kdu_core::kdu_int32> pbuf(reg.area(), 0);
    kdu_core::kdu_dims inc = reg, nr;
    while (!inc.is_empty()) {
      ASSERT_TRUE(dec.process(pbuf.data(), reg.pos, reg.size.x, 128, reg.area(),
                              inc, nr));
    }
    EXPECT_TRUE(dec.finish());
  }

  // Calling process() again after failure sets codestream_failure = true.
  {
    MemorySource src2(bytes);
    kdu_core::kdu_codestream cs2;
    cs2.create(&src2);
    auto cs2_cleanup = absl::MakeCleanup([&]() { cs2.destroy(); });
    kdu_region_decompressor dec;
    kdu_core::kdu_dims reg = dec.get_rendered_image_dims(
        cs2, nullptr, 0, 0, kdu_core::kdu_coords(1, 1));
    ASSERT_TRUE(
        dec.start(cs2, nullptr, 0, 0, 1, reg, kdu_core::kdu_coords(1, 1)));
    std::vector<kdu_core::kdu_byte> tiny(4);
    kdu_core::kdu_byte* ptrs[1] = {tiny.data()};
    kdu_core::kdu_dims inc = reg, nr;
    EXPECT_FALSE(dec.process(ptrs, false, 1, reg.pos, 0, 0, 1, inc, nr));
    hooks_.clear();
    EXPECT_FALSE(dec.process(ptrs, false, 1, reg.pos, 0, 0, 1, inc, nr));
    EXPECT_FALSE(dec.finish());
  }
}

}  // namespace
}  // namespace kdu_supp
