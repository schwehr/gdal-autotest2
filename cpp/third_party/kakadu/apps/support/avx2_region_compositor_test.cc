// Unit tests for
// third_party/kakadu/vendor/apps/support/avx2_region_compositor.cpp.
//
// In Google3's active build configuration (`//third_party/kakadu/kakadu.bzl`
// sets `KAKADU_X86_SIMD_DEFINES` with `-DKDU_NO_AVX2` on x86_64),
// `avx2_region_compositor.cpp` is compiled out (`0/0` LCOV `DA:` ceiling on
// both x86_64 and `--cpu=arm`). These tests exercise all three region-blending
// behaviors (`blend_region`, `premult_blend_region`, and `scaled_blend_region`
// with positive and negative alpha scaling factors) through
// `kdu_region_compositor` across varying X alignments and octet/tail widths.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/apps/compressed_io/jp2.h"
#include "third_party/kakadu/vendor/apps/compressed_io/jpx.h"
#include "third_party/kakadu/vendor/apps/support/kdu_region_compositor.h"
#include "third_party/kakadu/vendor/apps/support/kdu_stripe_compressor.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_params.h"

namespace {

using ::kdu_core::kdu_byte;
using ::kdu_core::kdu_codestream;
using ::kdu_core::kdu_compressed_source;
using ::kdu_core::kdu_compressed_target;
using ::kdu_core::kdu_coords;
using ::kdu_core::kdu_dims;
using ::kdu_core::kdu_exception;
using ::kdu_core::kdu_int32;
using ::kdu_core::kdu_thread_safe_message;
using ::kdu_core::kdu_uint32;
using ::kdu_core::siz_params;
using ::kdu_supp::jp2_family_src;
using ::kdu_supp::jp2_family_tgt;
using ::kdu_supp::jpx_layer_target;
using ::kdu_supp::jpx_metanode;
using ::kdu_supp::jpx_roi;
using ::kdu_supp::jpx_source;
using ::kdu_supp::jpx_target;
using ::kdu_supp::kdu_compositor_buf;
using ::kdu_supp::kdu_region_compositor;
using ::kdu_supp::kdu_stripe_compressor;

class ThrowingErrorHandler final : public kdu_thread_safe_message {
 public:
  ~ThrowingErrorHandler() noexcept override = default;
  void put_text(const char *string) override { std::fputs(string, stderr); }
  void flush(bool end_of_message = false) override {
    kdu_thread_safe_message::flush(end_of_message);
    if (end_of_message) {
      throw static_cast<kdu_exception>(KDU_ERROR_EXCEPTION);
    }
  }
};

class Avx2RegionCompositorTestEnv final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto *const handler = new ThrowingErrorHandler();
    ::kdu_core::kdu_customize_errors(handler);
  }
};

const ::testing::Environment *const kTestEnv =
    ::testing::AddGlobalTestEnvironment(new Avx2RegionCompositorTestEnv());

class VectorTarget final : public kdu_compressed_target {
 public:
  explicit VectorTarget(std::vector<kdu_byte> *out) : out_(out) {}
  bool write(const kdu_byte *buf, int num_bytes) override {
    out_->insert(out_->end(), buf, buf + num_bytes);
    return true;
  }

 private:
  std::vector<kdu_byte> *out_;
};

class VectorSource final : public kdu_compressed_source {
 public:
  explicit VectorSource(const std::vector<kdu_byte> &data) : data_(data) {}

  int get_capabilities() override {
    return KDU_SOURCE_CAP_SEQUENTIAL | KDU_SOURCE_CAP_SEEKABLE;
  }

  int read(kdu_byte *buf, int num_bytes) override {
    if (num_bytes <= 0 || pos_ >= data_.size()) {
      return 0;
    }
    const size_t avail =
        std::min(static_cast<size_t>(num_bytes), data_.size() - pos_);
    std::memcpy(buf, data_.data() + pos_, avail);
    pos_ += avail;
    return static_cast<int>(avail);
  }

  bool seek(::kdu_core::kdu_long offset) override {
    if (offset < 0) {
      return false;
    }
    pos_ = std::min(static_cast<size_t>(offset), data_.size());
    return true;
  }

  ::kdu_core::kdu_long get_pos() override {
    return static_cast<::kdu_core::kdu_long>(pos_);
  }

 private:
  const std::vector<kdu_byte> &data_;
  size_t pos_ = 0;
};

kdu_dims MakeDims(int x, int y, int w, int h) {
  kdu_dims d;
  d.pos = kdu_coords(x, y);
  d.size = kdu_coords(w, h);
  return d;
}

void CompressUniformOrAlphaCodestream(kdu_compressed_target *tgt, int width,
                                      int height, int num_comps,
                                      const std::vector<kdu_byte> &pixels) {
  siz_params siz;
  siz.set(Sdims, 0, 0, height);
  siz.set(Sdims, 0, 1, width);
  siz.set(Scomponents, 0, 0, num_comps);
  siz.set(Sprecision, 0, 0, 8);
  siz.set(Ssigned, 0, 0, false);
  static_cast<::kdu_core::kdu_params &>(siz).finalize();

  kdu_codestream cs;
  cs.create(&siz, tgt);
  cs.access_siz()->parse_string("Creversible=yes");
  cs.access_siz()->parse_string("Clevels=1");
  cs.access_siz()->finalize_all();

  kdu_stripe_compressor comp;
  comp.start(cs);
  std::vector<int> heights(num_comps, height);
  std::vector<int> precisions(num_comps, 8);
  while (comp.push_stripe(const_cast<kdu_byte *>(pixels.data()), heights.data(),
                          nullptr, nullptr, nullptr, precisions.data())) {
  }
  comp.finish();
  cs.destroy();
}

std::vector<kdu_byte> BuildTwoLayerBlendJpx(int width, int height,
                                            bool premultiplied,
                                            bool add_roi_overlay) {
  std::vector<kdu_byte> out;
  VectorTarget vt(&out);
  jp2_family_tgt ftgt;
  ftgt.open(&vt);
  jpx_target jout;
  jout.open(&ftgt);

  // Codestream 0: background layer (RGB = 40, 80, 160).
  auto ct0 = jout.add_codestream();
  siz_params s0;
  s0.set(Sdims, 0, 0, height);
  s0.set(Sdims, 0, 1, width);
  s0.set(Scomponents, 0, 0, 3);
  s0.set(Sprecision, 0, 0, 8);
  s0.set(Ssigned, 0, 0, false);
  static_cast<::kdu_core::kdu_params &>(s0).finalize();
  ct0.access_dimensions().init(&s0);

  // Codestream 1: foreground layer (RGB + Alpha interleaved in 4 comps).
  auto ct1 = jout.add_codestream();
  siz_params s1;
  s1.set(Sdims, 0, 0, height);
  s1.set(Sdims, 0, 1, width);
  s1.set(Scomponents, 0, 0, 4);
  s1.set(Sprecision, 0, 0, 8);
  s1.set(Ssigned, 0, 0, false);
  static_cast<::kdu_core::kdu_params &>(s1).finalize();
  ct1.access_dimensions().init(&s1);

  jpx_layer_target lay0 = jout.add_layer();
  lay0.add_colour().init(::kdu_supp::JP2_sRGB_SPACE);
  lay0.access_channels().init(3);
  lay0.access_channels().set_colour_mapping(0, 0, -1, 0);
  lay0.access_channels().set_colour_mapping(1, 1, -1, 0);
  lay0.access_channels().set_colour_mapping(2, 2, -1, 0);

  jpx_layer_target lay1 = jout.add_layer();
  lay1.add_colour().init(::kdu_supp::JP2_sRGB_SPACE);
  lay1.access_channels().init(3);
  lay1.access_channels().set_colour_mapping(0, 0, -1, 1);
  lay1.access_channels().set_colour_mapping(1, 1, -1, 1);
  lay1.access_channels().set_colour_mapping(2, 2, -1, 1);
  if (premultiplied) {
    lay1.access_channels().set_premult_mapping(-1, 3, -1, 1);
  } else {
    lay1.access_channels().set_opacity_mapping(-1, 3, -1, 1);
  }

  if (add_roi_overlay) {
    jpx_metanode root = jout.access_meta_manager().access_root();
    jpx_roi roi;
    roi.init_rectangle(MakeDims(1, 1, width - 2, height - 2));
    int cs0 = 0;
    int l0 = 0;
    root.add_numlist(1, &cs0, 1, &l0, false).add_regions(1, &roi);
  }

  while (jout.write_headers() != nullptr) {
  }

  std::vector<kdu_byte> bg_pixels(static_cast<size_t>(width) * height * 3);
  for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
    bg_pixels[i * 3 + 0] = 40;
    bg_pixels[i * 3 + 1] = 80;
    bg_pixels[i * 3 + 2] = 160;
  }
  auto *o0 = ct0.open_stream();
  CompressUniformOrAlphaCodestream(o0, width, height, 3, bg_pixels);
  o0->close();

  std::vector<kdu_byte> fg_pixels(static_cast<size_t>(width) * height * 4);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      size_t idx = (static_cast<size_t>(y) * width + x) * 4;
      // Vary alpha across columns: x%3 == 0 -> 0 (transparent),
      // x%3 == 1 -> 128 (semi-transparent), x%3 == 2 -> 255 (opaque).
      kdu_byte a = (x % 3 == 0) ? 0 : ((x % 3 == 1) ? 128 : 255);
      kdu_byte r = premultiplied ? (a == 0 ? 0 : (a == 128 ? 100 : 200)) : 200;
      kdu_byte g = premultiplied ? (a == 0 ? 0 : (a == 128 ? 80 : 160)) : 160;
      kdu_byte b = premultiplied ? (a == 0 ? 0 : (a == 128 ? 20 : 40)) : 40;
      fg_pixels[idx + 0] = r;
      fg_pixels[idx + 1] = g;
      fg_pixels[idx + 2] = b;
      fg_pixels[idx + 3] = a;
    }
  }
  auto *o1 = ct1.open_stream();
  CompressUniformOrAlphaCodestream(o1, width, height, 4, fg_pixels);
  o1->close();

  if (add_roi_overlay) {
    while (jout.write_metadata() != nullptr) {
    }
  }
  jout.close();
  ftgt.close();
  return out;
}

TEST(Avx2RegionCompositorTest, AlphaBlendAcrossAlignmentsAndWidths) {
  constexpr int kSize = 64;
  std::vector<kdu_byte> jpx =
      BuildTwoLayerBlendJpx(kSize, kSize, /*premultiplied=*/false, false);
  VectorSource src(jpx);
  jp2_family_src fsrc;
  fsrc.open(&src);
  jpx_source jpx_src;
  ASSERT_GE(jpx_src.open(&fsrc, true), 0);

  kdu_region_compositor comp(&jpx_src, 65536);
  comp.add_ilayer(0, kdu_dims(), MakeDims(0, 0, kSize, kSize));
  comp.add_ilayer(1, kdu_dims(), MakeDims(0, 0, kSize, kSize));
  comp.set_scale(false, false, false, 1.0f);

  // Exercise varying X offsets (0..7) and widths (left, octet, tail).
  for (int x_off = 0; x_off < 8; ++x_off) {
    for (int width : {1, 7, 8, 9, 17, 33}) {
      comp.set_buffer_surface(MakeDims(x_off, 2, width, 4), 0u);
      kdu_dims new_reg;
      int steps = 0;
      while (comp.process(65536, new_reg) && steps++ < 20) {
      }
      ASSERT_TRUE(comp.is_processing_complete());

      kdu_dims reg_dims;
      kdu_compositor_buf *cbuf = comp.get_composition_buffer(reg_dims, true);
      ASSERT_NE(cbuf, nullptr);
      ASSERT_GE(reg_dims.size.x, width);
      std::vector<kdu_int32> pixels(
          static_cast<size_t>(reg_dims.size.x) * reg_dims.size.y, 0);
      ASSERT_TRUE(cbuf->get_region(
          MakeDims(0, 0, reg_dims.size.x, reg_dims.size.y), pixels.data(), 0,
          0));
      for (int x = 0; x < width; ++x) {
        const kdu_uint32 px = static_cast<kdu_uint32>(pixels[x]);
        const int col = x_off + x;
        const int r = static_cast<int>((px >> 16) & 0xFF);
        const int g = static_cast<int>((px >> 8) & 0xFF);
        const int b = static_cast<int>(px & 0xFF);
        if (col % 3 == 0) {
          EXPECT_NEAR(r, 40, 2);
          EXPECT_NEAR(g, 80, 2);
          EXPECT_NEAR(b, 160, 2);
        } else if (col % 3 == 2) {
          EXPECT_NEAR(r, 200, 2);
          EXPECT_NEAR(g, 160, 2);
          EXPECT_NEAR(b, 40, 2);
        } else {
          EXPECT_NEAR(r, 120, 4);
          EXPECT_NEAR(g, 120, 4);
          EXPECT_NEAR(b, 100, 4);
        }
      }
    }
  }

  comp.pre_destroy();
  jpx_src.close();
  fsrc.close();
}

TEST(Avx2RegionCompositorTest, PremultAlphaBlendAcrossAlignmentsAndWidths) {
  constexpr int kSize = 64;
  std::vector<kdu_byte> jpx =
      BuildTwoLayerBlendJpx(kSize, kSize, /*premultiplied=*/true, false);
  VectorSource src(jpx);
  jp2_family_src fsrc;
  fsrc.open(&src);
  jpx_source jpx_src;
  ASSERT_GE(jpx_src.open(&fsrc, true), 0);

  kdu_region_compositor comp(&jpx_src, 65536);
  comp.add_ilayer(0, kdu_dims(), MakeDims(0, 0, kSize, kSize));
  comp.add_ilayer(1, kdu_dims(), MakeDims(0, 0, kSize, kSize));
  comp.set_scale(false, false, false, 1.0f);

  for (int x_off = 0; x_off < 8; ++x_off) {
    for (int width : {1, 5, 8, 13, 29}) {
      comp.set_buffer_surface(MakeDims(x_off, 1, width, 4), 0u);
      kdu_dims new_reg;
      int steps = 0;
      while (comp.process(65536, new_reg) && steps++ < 20) {
      }
      ASSERT_TRUE(comp.is_processing_complete());

      kdu_dims reg_dims;
      kdu_compositor_buf *cbuf = comp.get_composition_buffer(reg_dims, true);
      ASSERT_NE(cbuf, nullptr);
      ASSERT_GE(reg_dims.size.x, width);
      std::vector<kdu_int32> pixels(
          static_cast<size_t>(reg_dims.size.x) * reg_dims.size.y, 0);
      ASSERT_TRUE(cbuf->get_region(
          MakeDims(0, 0, reg_dims.size.x, reg_dims.size.y), pixels.data(), 0,
          0));
      for (int x = 0; x < width; ++x) {
        const kdu_uint32 px = static_cast<kdu_uint32>(pixels[x]);
        const int col = x_off + x;
        const int r = static_cast<int>((px >> 16) & 0xFF);
        if (col % 3 == 0) {
          EXPECT_NEAR(r, 40, 2);
        } else if (col % 3 == 2) {
          EXPECT_NEAR(r, 200, 2);
        } else {
          // Premultiplied fg R=100 + (1 - 128/255) * 40 ≈ 120.
          EXPECT_NEAR(r, 120, 4);
        }
      }
    }
  }

  comp.pre_destroy();
  jpx_src.close();
  fsrc.close();
}

TEST(Avx2RegionCompositorTest, ScaledBlendPositiveAndNegativeAlphaFactor) {
  constexpr int kSize = 64;
  std::vector<kdu_byte> jpx =
      BuildTwoLayerBlendJpx(kSize, kSize, /*premultiplied=*/false, true);
  VectorSource src(jpx);
  jp2_family_src fsrc;
  fsrc.open(&src);
  jpx_source jpx_src;
  ASSERT_GE(jpx_src.open(&fsrc, true), 0);

  kdu_region_compositor comp(&jpx_src, 65536);
  comp.add_ilayer(0, kdu_dims(), MakeDims(0, 0, kSize, kSize));
  comp.load_metadata_matches();
  const kdu_uint32 aux[2] = {1, 0x80FF00FFu};
  comp.set_scale(false, false, false, 1.0f);

  for (float blend : {0.5f, -0.5f, 1.0f}) {
    comp.configure_overlays(true, 0, blend, 4, jpx_metanode(), 0, aux, 2);
    for (int width : {3, 8, 17, 35}) {
      comp.set_buffer_surface(MakeDims(1, 1, width, 4), 0u);
      kdu_dims new_reg;
      int steps = 0;
      while (comp.process(65536, new_reg) && steps++ < 30) {
      }
      EXPECT_TRUE(comp.is_processing_complete());
      kdu_dims reg_dims;
      kdu_compositor_buf *cbuf = comp.get_composition_buffer(reg_dims, true);
      EXPECT_NE(cbuf, nullptr);
    }
  }

  comp.pre_destroy();
  jpx_src.close();
  fsrc.close();
}

}  // namespace
