// Unit tests for
// third_party/kakadu/vendor/apps/support/neon_region_compositor.cpp.
//
// Exercises NEON region compositor acceleration on ARM (`--cpu=arm`) and the
// scalar/SIMD equivalents on x86_64 across all 12 region operations:
// `erase_region`, `copy_region`, `rcopy_region`, `blend_region`,
// `premult_blend_region`, and `scaled_blend_region` (positive and negative
// alpha scaling) in both 32-bit packed pixel and floating-point modes.

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

class NeonRegionCompositorTestEnv final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto *const handler = new ThrowingErrorHandler();
    ::kdu_core::kdu_customize_errors(handler);
  }
};

const ::testing::Environment *const kTestEnv =
    ::testing::AddGlobalTestEnvironment(new NeonRegionCompositorTestEnv());

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

class OwnedCompositorBuf final : public kdu_compositor_buf {
 public:
  OwnedCompositorBuf(int width, int height, bool float_mode) {
    read_access_allowed = true;
    if (float_mode) {
      float_storage_.resize(static_cast<size_t>(width) * height * 4, 0.0f);
      init_float(float_storage_.data(), width * 4);
    } else {
      u32_storage_.resize(static_cast<size_t>(width) * height, 0);
      init(u32_storage_.data(), width);
    }
  }

  ~OwnedCompositorBuf() override {
    // Base destructor deletes raw `buf`/`float_buf`, so detach owned vectors.
    buf = nullptr;
    float_buf = nullptr;
  }

  const std::vector<float> &float_storage() const { return float_storage_; }

 private:
  std::vector<kdu_uint32> u32_storage_;
  std::vector<float> float_storage_;
};

class ConfigurableRegionCompositor final : public kdu_region_compositor {
 public:
  ConfigurableRegionCompositor(bool float_mode, bool allow_grow)
      : float_mode_(float_mode), allow_grow_(allow_grow) {}
  ~ConfigurableRegionCompositor() override { pre_destroy(); }

  kdu_compositor_buf *allocate_buffer(kdu_coords min_size,
                                      kdu_coords &actual_size,
                                      bool read_access_required) override {
    (void)read_access_required;
    if (allow_grow_) {
      actual_size = min_size;
    } else {
      actual_size =
          kdu_coords(std::max(min_size.x, 72), std::max(min_size.y, 64));
    }
    return new OwnedCompositorBuf(actual_size.x, actual_size.y, float_mode_);
  }

  void delete_buffer(kdu_compositor_buf *cbuf) override { delete cbuf; }

 private:
  bool float_mode_;
  bool allow_grow_;
};

kdu_dims MakeDims(int x, int y, int w, int h) {
  kdu_dims d;
  d.pos = kdu_coords(x, y);
  d.size = kdu_coords(w, h);
  return d;
}

void CompressCodestream(kdu_compressed_target *tgt, int width, int height,
                        int num_comps, const std::vector<kdu_byte> &pixels) {
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
  CompressCodestream(o0, width, height, 3, bg_pixels);
  o0->close();

  std::vector<kdu_byte> fg_pixels(static_cast<size_t>(width) * height * 4);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const size_t idx = (static_cast<size_t>(y) * width + x) * 4;
      const kdu_byte a = (x % 3 == 0) ? 0 : ((x % 3 == 1) ? 128 : 255);
      const kdu_byte r = premultiplied ? (a == 0 ? 0 : (a == 128 ? 100 : 200))
                                       : 200;
      const kdu_byte g = premultiplied ? (a == 0 ? 0 : (a == 128 ? 80 : 160))
                                       : 160;
      const kdu_byte b = premultiplied ? (a == 0 ? 0 : (a == 128 ? 20 : 40))
                                       : 40;
      fg_pixels[idx + 0] = r;
      fg_pixels[idx + 1] = g;
      fg_pixels[idx + 2] = b;
      fg_pixels[idx + 3] = a;
    }
  }
  auto *o1 = ct1.open_stream();
  CompressCodestream(o1, width, height, 4, fg_pixels);
  o1->close();

  if (add_roi_overlay) {
    while (jout.write_metadata() != nullptr) {
    }
  }
  jout.close();
  ftgt.close();
  return out;
}

TEST(NeonRegionCompositorTest, AlphaBlendUint32AndFloatAcrossAlignments) {
  constexpr int kSize = 64;
  std::vector<kdu_byte> jpx =
      BuildTwoLayerBlendJpx(kSize, kSize, /*premultiplied=*/false, false);

  for (bool float_mode : {false, true}) {
    VectorSource src(jpx);
    jp2_family_src fsrc;
    fsrc.open(&src);
    jpx_source jpx_src;
    ASSERT_GE(jpx_src.open(&fsrc, true), 0);

    ConfigurableRegionCompositor comp(float_mode, /*allow_grow=*/false);
    comp.create(&jpx_src, 65536);
    comp.add_ilayer(0, kdu_dims(), MakeDims(0, 0, kSize, kSize));
    comp.add_ilayer(1, kdu_dims(), MakeDims(0, 0, kSize, kSize));
    comp.set_scale(false, false, false, 1.0f);

    for (int x_off = 0; x_off < 8; ++x_off) {
      for (int width : {1, 5, 8, 9, 17, 33}) {
        comp.set_buffer_surface(MakeDims(x_off, 2, width, 4), 0x20103040u);
        kdu_dims new_reg;
        int steps = 0;
        while (comp.process(65536, new_reg) && steps++ < 20) {
        }
        ASSERT_TRUE(comp.is_processing_complete());

        kdu_dims reg_dims;
        kdu_compositor_buf *cbuf = comp.get_composition_buffer(reg_dims, true);
        ASSERT_NE(cbuf, nullptr);
        ASSERT_GE(reg_dims.size.x, width);
        if (!float_mode) {
          std::vector<kdu_int32> pixels(
              static_cast<size_t>(reg_dims.size.x) * reg_dims.size.y, 0);
          ASSERT_TRUE(cbuf->get_region(
              MakeDims(0, 0, reg_dims.size.x, reg_dims.size.y), pixels.data(),
              0, 0));
          for (int x = 0; x < width; ++x) {
            const kdu_uint32 px = static_cast<kdu_uint32>(pixels[x]);
            const int col = x_off + x;
            const int r = static_cast<int>((px >> 16) & 0xFF);
            if (col % 3 == 0) {
              EXPECT_NEAR(r, 40, 2);
            } else if (col % 3 == 2) {
              EXPECT_NEAR(r, 200, 2);
            } else {
              EXPECT_NEAR(r, 120, 4);
            }
          }
        } else {
          auto *owned = static_cast<OwnedCompositorBuf *>(cbuf);
          const auto &fp = owned->float_storage();
          for (int x = 0; x < width; ++x) {
            const float r = fp[static_cast<size_t>(x) * 4 + 1] * 255.0f;
            const int col = x_off + x;
            if (col % 3 == 0) {
              EXPECT_NEAR(r, 40.0f, 3.0f);
            } else if (col % 3 == 2) {
              EXPECT_NEAR(r, 200.0f, 3.0f);
            } else {
              EXPECT_NEAR(r, 120.0f, 5.0f);
            }
          }
        }
      }
    }

    comp.pre_destroy();
    jpx_src.close();
    fsrc.close();
  }
}

TEST(NeonRegionCompositorTest, PremultBlendUint32AndFloatAcrossAlignments) {
  constexpr int kSize = 64;
  std::vector<kdu_byte> jpx =
      BuildTwoLayerBlendJpx(kSize, kSize, /*premultiplied=*/true, false);

  for (bool float_mode : {false, true}) {
    VectorSource src(jpx);
    jp2_family_src fsrc;
    fsrc.open(&src);
    jpx_source jpx_src;
    ASSERT_GE(jpx_src.open(&fsrc, true), 0);

    ConfigurableRegionCompositor comp(float_mode, /*allow_grow=*/false);
    comp.create(&jpx_src, 65536);
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
        if (!float_mode) {
          std::vector<kdu_int32> pixels(
              static_cast<size_t>(reg_dims.size.x) * reg_dims.size.y, 0);
          ASSERT_TRUE(cbuf->get_region(
              MakeDims(0, 0, reg_dims.size.x, reg_dims.size.y), pixels.data(),
              0, 0));
          for (int x = 0; x < width; ++x) {
            const kdu_uint32 px = static_cast<kdu_uint32>(pixels[x]);
            const int col = x_off + x;
            const int r = static_cast<int>((px >> 16) & 0xFF);
            if (col % 3 == 0) {
              EXPECT_NEAR(r, 40, 2);
            } else if (col % 3 == 2) {
              EXPECT_NEAR(r, 200, 2);
            } else {
              EXPECT_NEAR(r, 120, 4);
            }
          }
        } else {
          auto *owned = static_cast<OwnedCompositorBuf *>(cbuf);
          const auto &fp = owned->float_storage();
          for (int x = 0; x < width; ++x) {
            const float r = fp[static_cast<size_t>(x) * 4 + 1] * 255.0f;
            const int col = x_off + x;
            if (col % 3 == 0) {
              EXPECT_NEAR(r, 40.0f, 3.0f);
            } else if (col % 3 == 2) {
              EXPECT_NEAR(r, 200.0f, 3.0f);
            } else {
              EXPECT_NEAR(r, 120.0f, 5.0f);
            }
          }
        }
      }
    }

    comp.pre_destroy();
    jpx_src.close();
    fsrc.close();
  }
}

TEST(NeonRegionCompositorTest, ScaledBlendPositiveAndNegativeUint32AndFloat) {
  constexpr int kSize = 64;
  std::vector<kdu_byte> jpx =
      BuildTwoLayerBlendJpx(kSize, kSize, /*premultiplied=*/false, true);

  for (bool float_mode : {false, true}) {
    VectorSource src(jpx);
    jp2_family_src fsrc;
    fsrc.open(&src);
    jpx_source jpx_src;
    ASSERT_GE(jpx_src.open(&fsrc, true), 0);

    ConfigurableRegionCompositor comp(float_mode, /*allow_grow=*/false);
    comp.create(&jpx_src, 65536);
    comp.add_ilayer(0, kdu_dims(), MakeDims(0, 0, kSize, kSize));
    comp.load_metadata_matches();
    const kdu_uint32 aux[2] = {1, 0x80FF00FFu};
    comp.set_scale(false, false, false, 1.0f);

    for (float blend : {0.5f, -0.5f, 1.0f}) {
      comp.configure_overlays(true, 0, blend, 4, jpx_metanode(), 0, aux, 2);
      for (int x_off : {0, 1, 2, 3}) {
        for (int width : {3, 8, 17, 35}) {
          comp.set_buffer_surface(MakeDims(x_off, 1, width, 4), 0u);
          kdu_dims new_reg;
          int steps = 0;
          while (comp.process(65536, new_reg) && steps++ < 30) {
          }
          EXPECT_TRUE(comp.is_processing_complete());
          kdu_dims reg_dims;
          kdu_compositor_buf *cbuf =
              comp.get_composition_buffer(reg_dims, true);
          EXPECT_NE(cbuf, nullptr);
        }
      }
    }

    comp.pre_destroy();
    jpx_src.close();
    fsrc.close();
  }
}

TEST(NeonRegionCompositorTest, EraseCopyAndReverseCopyUint32AndFloat) {
  constexpr int kSize = 64;
  std::vector<kdu_byte> jpx =
      BuildTwoLayerBlendJpx(kSize, kSize, /*premultiplied=*/false, false);

  for (bool float_mode : {false, true}) {
    for (bool allow_grow : {false, true}) {
      VectorSource src(jpx);
      jp2_family_src fsrc;
      fsrc.open(&src);
      jpx_source jpx_src;
      ASSERT_GE(jpx_src.open(&fsrc, true), 0);

      ConfigurableRegionCompositor comp(float_mode, allow_grow);
      comp.create(&jpx_src, 65536);
      comp.add_ilayer(0, kdu_dims(), MakeDims(0, 0, kSize, kSize));
      comp.set_scale(false, false, false, 1.0f);

      const std::vector<kdu_dims> sequence = {
          MakeDims(4, 4, 24, 10),
          // Overlapping shift toward top-left -> backward rcopy in-place.
          MakeDims(1, 2, 24, 10),
          // Overlapping shift toward bottom-right -> forward copy in-place.
          MakeDims(6, 5, 25, 10),
          // Growth step across alignment offsets (copies to new buffer).
          MakeDims(3, 3, 33, 12),
          // Small width with unaligned start -> left/right edge loops.
          MakeDims(5, 4, 3, 6),
          MakeDims(7, 5, 19, 8),
      };
      for (const kdu_dims &surf : sequence) {
        comp.set_buffer_surface(surf, 0xFF123456u);
        kdu_dims new_reg;
        int steps = 0;
        while (comp.process(65536, new_reg) && steps++ < 25) {
        }
        EXPECT_TRUE(comp.is_processing_complete());
      }

      comp.pre_destroy();
      jpx_src.close();
      fsrc.close();
    }
  }
}

}  // namespace
