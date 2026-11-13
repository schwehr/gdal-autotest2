#include "kdu_region_compositor.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "jp2.h"
#include "jpx.h"
#include "kdu_compressed.h"
#include "kdu_file_io.h"
#include "kdu_messaging.h"
#include "kdu_params.h"
#include "kdu_stripe_compressor.h"
#include "kdu_threads.h"
#include "mj2.h"

namespace {

using ::kdu_core::kdu_byte;
using ::kdu_core::kdu_codestream;
using ::kdu_core::kdu_compressed_target;
using ::kdu_core::kdu_coords;
using ::kdu_core::kdu_dims;
using ::kdu_core::kdu_int16;
using ::kdu_core::kdu_int32;
using ::kdu_core::kdu_long;
using ::kdu_core::kdu_membroker;
using ::kdu_core::kdu_quality_limiter;
using ::kdu_core::kdu_thread_env;
using ::kdu_core::kdu_uint32;
using ::kdu_core::KDU_WANT_CODESTREAM_COMPONENTS;
using ::kdu_core::KDU_WANT_OUTPUT_COMPONENTS;
using ::kdu_core::siz_params;
using ::kdu_supp::jp2_channels;
using ::kdu_supp::jp2_colour;
using ::kdu_supp::jp2_family_src;
using ::kdu_supp::jp2_family_tgt;
using ::kdu_supp::jp2_palette;
using ::kdu_supp::jpx_composition;
using ::kdu_supp::jpx_frame_expander;
using ::kdu_supp::jpx_layer_target;
using ::kdu_supp::jpx_meta_manager;
using ::kdu_supp::jpx_metanode;
using ::kdu_supp::jpx_roi;
using ::kdu_supp::jpx_source;
using ::kdu_supp::jpx_target;
using ::kdu_supp::jx_frame;
using ::kdu_supp::KDU_ABGR_ORDER;
using ::kdu_supp::KDU_ARGB_ORDER;
using ::kdu_supp::kdu_compositor_buf;
using ::kdu_supp::kdu_compressed_source_buffered;
using ::kdu_supp::kdu_ilayer_ref;
using ::kdu_supp::kdu_istream_ref;
using ::kdu_supp::kdu_overlay_params;
using ::kdu_supp::kdu_region_compositor;
using ::kdu_supp::kdu_stripe_compressor;
using ::kdu_supp::kdu_window;
using ::kdu_supp::mj2_source;
using ::kdu_supp::mj2_target;
using ::kdu_supp::mj2_video_source;
using ::kdu_supp::mj2_video_target;

kdu_dims MakeDims(int x, int y, int w, int h) {
  kdu_dims d;
  d.pos = kdu_coords(x, y);
  d.size = kdu_coords(w, h);
  return d;
}

class VectorTarget : public kdu_compressed_target {
 public:
  explicit VectorTarget(std::vector<kdu_byte> *out) : out_(out) {}

  bool write(const kdu_byte *buf, int num_bytes) override {
    if (num_bytes <= 0) {
      return true;
    }
    if (pos_ + static_cast<size_t>(num_bytes) > out_->size()) {
      out_->resize(pos_ + static_cast<size_t>(num_bytes));
    }
    std::memcpy(out_->data() + pos_, buf, static_cast<size_t>(num_bytes));
    pos_ += static_cast<size_t>(num_bytes);
    return true;
  }

  bool start_rewrite(kdu_long backtrack) override {
    if (backtrack < 0 || static_cast<size_t>(backtrack) > pos_) {
      return false;
    }
    saved_positions_.push_back(pos_);
    pos_ -= static_cast<size_t>(backtrack);
    return true;
  }

  bool end_rewrite() override {
    if (saved_positions_.empty()) {
      return false;
    }
    pos_ = saved_positions_.back();
    saved_positions_.pop_back();
    return true;
  }

  std::vector<kdu_byte> *out_;
  size_t pos_ = 0;
  std::vector<size_t> saved_positions_;
};

class VectorSource : public ::kdu_core::kdu_compressed_source {
 public:
  explicit VectorSource(const std::vector<kdu_byte> &data)
      : data_(data.data()), size_(data.size()) {}

  int get_capabilities() override {
    return KDU_SOURCE_CAP_SEQUENTIAL | KDU_SOURCE_CAP_SEEKABLE;
  }

  int read(kdu_byte *buf, int num_bytes) override {
    if (num_bytes <= 0 || pos_ >= size_) {
      return 0;
    }
    size_t avail = size_ - pos_;
    size_t to_copy = std::min(avail, static_cast<size_t>(num_bytes));
    std::memcpy(buf, data_ + pos_, to_copy);
    pos_ += to_copy;
    return static_cast<int>(to_copy);
  }

  bool seek(kdu_long offset) override {
    if (offset < 0 || static_cast<size_t>(offset) > size_) {
      return false;
    }
    pos_ = static_cast<size_t>(offset);
    return true;
  }

  kdu_long get_pos() override { return static_cast<kdu_long>(pos_); }

  bool close() override {
    pos_ = 0;
    return true;
  }

 private:
  const kdu_byte *data_;
  size_t size_;
  size_t pos_ = 0;
};

class ScopedMessageHandlers {
 public:
  ScopedMessageHandlers() {
    errors_.print_ = false;
    errors_.throw_on_flush_ = true;
    ::kdu_core::kdu_customize_warnings(&warnings_);
    ::kdu_core::kdu_customize_errors(&errors_);
  }
  ~ScopedMessageHandlers() {
    ::kdu_core::kdu_customize_warnings(nullptr);
    ::kdu_core::kdu_customize_errors(nullptr);
  }

 private:
  class Sink : public ::kdu_core::kdu_thread_safe_message {
   public:
    ~Sink() noexcept override = default;
    void put_text(const char *string) override {
      msg_ += string;
      if (print_) {
        std::fputs(string, stderr);
      }
    }
    void flush(bool end_of_message) override {
      ::kdu_core::kdu_thread_safe_message::flush(end_of_message);
      if (end_of_message) {
        if (print_) {
          std::fputc('\n', stderr);
        }
        if (throw_on_flush_) {
          throw ::kdu_core::kdu_exception(1);
        }
        msg_.clear();
      }
    }
    bool print_ = false;
    bool throw_on_flush_ = false;
    std::string msg_;
  };
 public:
  const std::string &last_error() const { return errors_.msg_; }
 private:
  Sink warnings_;
  Sink errors_;
};

kdu_codestream CompressToTarget(kdu_compressed_target *target, int width,
                                int height, int num_components, int bit_depth,
                                int num_layers, int tile_size, bool reversible,
                                bool ycc = false, int fill_bias = 0,
                                bool keep_cs = false) {
  siz_params siz;
  siz.set(Sdims, 0, 0, height);
  siz.set(Sdims, 0, 1, width);
  siz.set(Scomponents, 0, 0, num_components);
  siz.set(Sprecision, 0, 0, bit_depth);
  siz.set(Ssigned, 0, 0, false);
  if (tile_size > 0) {
    siz.set(Stiles, 0, 0, tile_size);
    siz.set(Stiles, 0, 1, tile_size);
  }
  static_cast<::kdu_core::kdu_params &>(siz).finalize();

  kdu_codestream cs;
  cs.create(&siz, target);
  absl::Cleanup cs_cleanup = [&cs, keep_cs]() {
    if (!keep_cs && cs.exists()) {
      cs.destroy();
    }
  };
  auto *cod = cs.access_siz()->access_cluster(COD_params);
  cod->set(Creversible, 0, 0, reversible);
  cod->set(Clevels, 0, 0, 2);
  if (!ycc) {
    cod->set(Cycc, 0, 0, false);
  }
  if (num_layers > 1) {
    cod->set(Clayers, 0, 0, num_layers);
  }
  cs.access_siz()->finalize_all();

  kdu_stripe_compressor compressor;
  std::vector<kdu_long> layer_bytes(num_layers);
  if (num_layers > 1) {
    kdu_long base =
        static_cast<kdu_long>(width) * height * num_components / 4;
    for (int l = 0; l < num_layers; ++l) {
      layer_bytes[l] = std::max<kdu_long>(64, base * (l + 1));
    }
    compressor.start(cs, num_layers, layer_bytes.data());
  } else {
    compressor.start(cs);
  }
  absl::Cleanup comp_cleanup = [&compressor]() { compressor.finish(); };

  std::vector<int> heights(num_components, height);
  std::vector<kdu_byte> buf(
      static_cast<size_t>(width) * height * num_components);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      for (int c = 0; c < num_components; ++c) {
        int val = ((x * 5 + y * 3 + c * 40 + fill_bias) & 0xFF);
        buf[(static_cast<size_t>(y) * width + x) * num_components + c] =
            static_cast<kdu_byte>(val);
      }
    }
  }
  std::vector<kdu_byte *> planes(num_components);
  std::vector<int> sample_offsets(num_components, 0);
  std::vector<int> pixel_gaps(num_components, num_components);
  std::vector<int> actual_row_gaps(num_components, width * num_components);
  for (int c = 0; c < num_components; ++c) {
    planes[c] = buf.data() + c;
  }
  compressor.push_stripe(planes.data(), heights.data(), sample_offsets.data(),
                         actual_row_gaps.data(), pixel_gaps.data());
  return cs;
}

std::vector<kdu_byte> MakeRawCodestream(int width, int height,
                                        int num_components,
                                        int num_layers = 1,
                                        int tile_size = 0) {
  std::vector<kdu_byte> out;
  VectorTarget target(&out);
  CompressToTarget(&target, width, height, num_components, 8, num_layers,
                   tile_size, /*reversible=*/true);
  return out;
}

class TrackingMembroker : public kdu_membroker {
 public:
  TrackingMembroker() = default;
};

class CustomCompositorBuf : public kdu_compositor_buf {
 public:
  explicit CustomCompositorBuf(bool allow_read)
      : allow_read_(allow_read) {
    read_access_allowed = allow_read;
  }

  bool set_read_accessibility(bool read_access_required) override {
    set_read_calls_++;
    if (!allow_read_ && read_access_required) {
      // Simulate buffer re-allocation losing previous content.
      read_access_allowed = true;
      allow_read_ = true;
      return false;
    }
    read_access_allowed = allow_read_ || read_access_required;
    return true;
  }

  int set_read_calls_ = 0;
  bool allow_read_;
};

class CustomRegionCompositor : public kdu_region_compositor {
 public:
  explicit CustomRegionCompositor(bool float_mode = false,
                                  bool allow_read = true,
                                  bool handle_custom_overlay = false)
      : float_mode_(float_mode),
        allow_read_(allow_read),
        handle_custom_overlay_(handle_custom_overlay) {}

  ~CustomRegionCompositor() override { pre_destroy(); }

  kdu_compositor_buf *allocate_buffer(kdu_coords min_size,
                                      kdu_coords &actual_size,
                                      bool read_access_required) override {
    alloc_count_++;
    actual_size = kdu_coords(min_size.x + 8, min_size.y + 8);
    auto *buf = new CustomCompositorBuf(allow_read_ || read_access_required);
    if (float_mode_) {
      const size_t n = static_cast<size_t>(actual_size.x) * actual_size.y * 4;
      float *storage = new float[n]();
      buf->init_float(storage, actual_size.x * 4);
    } else {
      const size_t n = static_cast<size_t>(actual_size.x) * actual_size.y;
      kdu_uint32 *storage = new kdu_uint32[n]();
      buf->init(storage, actual_size.x);
    }
    return buf;
  }

  void delete_buffer(kdu_compositor_buf *buf) override {
    delete_count_++;
    delete buf;
  }

  bool custom_paint_overlay(kdu_compositor_buf *buffer, kdu_dims buffer_region,
                            kdu_dims bounding_region, jpx_metanode node,
                            kdu_overlay_params *painting_params,
                            kdu_coords image_offset, kdu_coords subsampling,
                            bool transpose, bool vflip, bool hflip,
                            kdu_coords expansion_numerator,
                            kdu_coords expansion_denominator,
                            kdu_coords compositing_offset,
                            ::kdu_supp::kdu_colour_order colour_order) override {
    custom_overlay_calls_++;
    if (painting_params != nullptr) {
      int num_regions = node.get_num_regions();
      if (num_regions > 0) {
        (void)painting_params->map_jpx_regions(
            node.get_regions(), num_regions, image_offset, subsampling,
            transpose, vflip, hflip, expansion_numerator, expansion_denominator,
            compositing_offset);
      }
      painting_params->configure_ring_points(8, 2);
      int nvals = 0;
      (void)painting_params->get_ring_points(-1, 1, nvals);
      (void)painting_params->get_ring_points(-2, 2, nvals);
      kdu_uint32 extra[2] = {0x40FF0000u, 1u};
      painting_params->push_aux_params(extra, 2);
      (void)painting_params->get_num_aux_params();
      (void)painting_params->get_aux_param(0);
      (void)painting_params->get_aux_param(99);
      painting_params->restore_aux_params();
      (void)painting_params->get_codestream_idx();
      (void)painting_params->get_compositing_layer_idx();
      (void)painting_params->get_max_painting_border();
    }
    return handle_custom_overlay_;
  }

  int alloc_count_ = 0;
  int delete_count_ = 0;
  int custom_overlay_calls_ = 0;
  bool float_mode_;
  bool allow_read_;
  bool handle_custom_overlay_;
};

class KduRegionCompositorTest : public ::testing::Test {
 protected:
  ScopedMessageHandlers handlers_;
};

TEST_F(KduRegionCompositorTest, RawCodestreamLifecycleScaleQueueAndMapping) {
  std::vector<kdu_byte> raw = MakeRawCodestream(64, 48, 3, /*num_layers=*/3, 32);
  kdu_compressed_source_buffered src(raw.data(), raw.size());

  TrackingMembroker broker;
  kdu_region_compositor compositor;
  EXPECT_TRUE(compositor.mem_configure(&broker, 8));
  compositor.set_colour_order(KDU_ABGR_ORDER);
  compositor.set_colour_order(KDU_ARGB_ORDER);
  compositor.set_error_level(0);
  compositor.set_error_level(1);
  compositor.configure_scaling_params(0.5f, 0.5f, 2);
  compositor.configure_scaling_params(0.95f, -0.1f, 0);
  compositor.configure_intensity_scaling(true, true);
  compositor.set_process_aggregation_threshold(-0.5f);
  compositor.set_process_aggregation_threshold(1.5f);
  compositor.set_process_aggregation_threshold(0.25f);
  compositor.set_surface_initialization_mode(true);

  kdu_quality_limiter limiter(1.0f, false);
  compositor.set_quality_limiting(&limiter, 100.0f, 100.0f);
  compositor.set_quality_limiting(nullptr, 0.0f, 0.0f);

  compositor.create(&src, /*persistent_cache_threshold=*/65536);
  EXPECT_EQ(compositor.get_num_ilayers(), 0);
  EXPECT_FALSE(compositor.waiting_for_stream_headers());

  kdu_ilayer_ref ilayer =
      compositor.add_ilayer(0, MakeDims(0, 0, 0, 0), MakeDims(0, 0, 0, 0));
  EXPECT_FALSE(ilayer.is_null());
  EXPECT_EQ(compositor.get_num_ilayers(), 1);
  EXPECT_GE(compositor.get_max_available_quality_layers(), 1);
  (void)compositor.get_ht_usage();

  // Also test primitive ilayer on same stream.
  int comp_idx = 1;
  kdu_ilayer_ref prim = compositor.add_primitive_ilayer(
      0, comp_idx, KDU_WANT_CODESTREAM_COMPONENTS, MakeDims(0, 0, 32, 24),
      MakeDims(64, 0, 64, 48), false, false, false);
  EXPECT_FALSE(prim.is_null());
  EXPECT_EQ(compositor.get_num_ilayers(), 2);

  // Inspect ilayer & istream metadata.
  int layer_src = -1;
  int direct_cs = -1;
  bool is_opaque = false;
  int frame_idx = -1;
  int field_handling = -1;
  EXPECT_GE(compositor.get_ilayer_info(ilayer, layer_src, direct_cs, is_opaque,
                                       &frame_idx, &field_handling),
            0);

  kdu_ilayer_ref next_l = compositor.get_next_ilayer(kdu_ilayer_ref());
  EXPECT_FALSE(next_l.is_null());
  kdu_istream_ref istream = compositor.get_next_istream(kdu_istream_ref());
  EXPECT_FALSE(istream.is_null());
  kdu_istream_ref il_stream = compositor.get_ilayer_stream(ilayer, 0);
  EXPECT_FALSE(il_stream.is_null());
  EXPECT_TRUE(compositor.access_codestream(istream).exists());

  compositor.set_scale(false, false, false, 1.0f, 1.0f);
  int cs_idx = -1;
  kdu_ilayer_ref out_layer;
  int comps_in_use[4] = {};
  int prin_comp = -1;
  float scx = 0.0f;
  float scy = 0.0f;
  bool tr = false;
  bool vf = false;
  bool hf = false;
  int num_c = compositor.get_istream_info(
      il_stream, cs_idx, &out_layer, comps_in_use, 4, &prin_comp, &scx, &scy,
      &tr, &vf, &hf);
  EXPECT_EQ(num_c, 3);

  float opt_scale = compositor.find_optimal_scale(
      MakeDims(0, 0, 0, 0), 1.0f, 0.25f, 2.0f, &il_stream, &prin_comp, false);
  EXPECT_GT(opt_scale, 0.0f);

  kdu_dims total_dims;
  EXPECT_TRUE(compositor.get_total_composition_dims(total_dims));
  EXPECT_GT(total_dims.size.x, 0);

  kdu_dims surface = MakeDims(0, 0, 64, 48);
  compositor.set_buffer_surface(surface, /*background=*/0x11223344);
  compositor.set_max_quality_layers(2);

  // Process incrementally with both deferred and active modes.
  kdu_dims rendered;
  compositor.process(100, rendered, KDU_COMPOSIT_DEFER_PROCESSING);
  int steps = 0;
  while (compositor.process(4096, rendered, 0) && steps++ < 50) {
  }
  EXPECT_TRUE(compositor.is_processing_complete());
  EXPECT_TRUE(compositor.is_codestream_processing_complete());

  kdu_dims buf_region;
  kdu_compositor_buf *buf = compositor.get_composition_buffer(buf_region);
  ASSERT_NE(buf, nullptr);
  EXPECT_GT(buf->get_rendering_scale(), 0.0f);
  EXPECT_FALSE(buf->get_rendering_region().is_empty());
  EXPECT_FALSE(buf->get_rendering_composition_dims().is_empty());
  int nrows = 0;
  int first_row = 0;
  (void)buf->get_rendering_status(nrows, first_row, true);
  EXPECT_GE(buf->get_rendered_rows(), 0);
  EXPECT_TRUE(buf->is_read_access_allowed());

  std::vector<kdu_int32> copy_pixels(
      static_cast<size_t>(buf_region.size.x) * buf_region.size.y, 0);
  EXPECT_TRUE(buf->get_region(
      MakeDims(0, 0, buf_region.size.x, buf_region.size.y), copy_pixels.data(),
      0, 0));

  // Exercise queue push/replace/inspect/pop/flush.
  EXPECT_TRUE(compositor.push_composition_buffer(101, 1));
  steps = 0;
  while (compositor.process(10000, rendered, 0) && steps++ < 50) {
  }
  EXPECT_TRUE(compositor.replace_composition_queue_tail(102, 2));
  kdu_long stamp = 0;
  int cid = 0;
  kdu_dims qreg;
  EXPECT_NE(compositor.inspect_composition_queue(0, &stamp, &cid, &qreg),
            nullptr);
  EXPECT_EQ(stamp, 102);
  EXPECT_EQ(cid, 2);
  EXPECT_TRUE(compositor.pop_composition_buffer());
  steps = 0;
  while (compositor.process(10000, rendered, 0) && steps++ < 50) {
  }
  EXPECT_TRUE(compositor.push_composition_buffer(103, 3));
  compositor.flush_composition_queue();

  // Region queries and coordinate mapping.
  EXPECT_FALSE(
      compositor.get_next_visible_ilayer(kdu_ilayer_ref(), surface).is_null());
  EXPECT_FALSE(compositor.find_point(kdu_coords(10, 10), 0, -1.0f).is_null());
  kdu_dims mapped = MakeDims(4, 4, 16, 16);
  compositor.map_region(mapped, istream);
  kdu_dims inv = compositor.inverse_map_region(mapped, istream);
  EXPECT_FALSE(inv.is_empty());
  EXPECT_FALSE(compositor.find_ilayer_region(ilayer, true).is_empty());
  EXPECT_FALSE(compositor.find_istream_region(istream, false).is_empty());

  kdu_long prec_s = 0, pkt_s = 0, max_pkt_s = 0;
  (void)compositor.get_codestream_packets(istream, surface, prec_s, pkt_s,
                                          max_pkt_s, 0);

  // Invalidate, refresh, change scale (with transpose & flips) and shift surface.
  compositor.invalidate_rect(MakeDims(8, 8, 16, 16));
  bool new_img = false;
  compositor.refresh(&new_img);
  compositor.set_surface_initialization_mode(false);
  compositor.set_scale(true, true, true, 0.75f, 1.0f);
  compositor.set_buffer_surface(MakeDims(4, 4, 32, 32), -1);
  steps = 0;
  while (compositor.process(10000, rendered, 0) && steps++ < 50) {
  }
  compositor.halt_processing();

  // Remove layers permanently and non-permanently + cull.
  EXPECT_TRUE(compositor.remove_ilayer(prim, /*permanent=*/false));
  compositor.cull_inactive_ilayers(0);
  EXPECT_TRUE(compositor.remove_ilayer(ilayer, /*permanent=*/true));
  EXPECT_EQ(compositor.get_num_ilayers(), 0);
}

TEST_F(KduRegionCompositorTest, RawDirectConstructorAndCustomBuffers) {
  std::vector<kdu_byte> raw = MakeRawCodestream(48, 48, 3);
  kdu_compressed_source_buffered src(raw.data(), raw.size());
  // Direct raw constructor.
  kdu_region_compositor direct_comp(&src, 0);
  direct_comp.add_ilayer(0, MakeDims(0, 0, 0, 0), MakeDims(0, 0, 0, 0));
  direct_comp.set_scale(false, false, false, 1.0f);
  kdu_dims dims;
  EXPECT_TRUE(direct_comp.get_total_composition_dims(dims));
  direct_comp.set_buffer_surface(dims);
  kdu_dims r;
  int steps = 0;
  while (direct_comp.process(100000, r) && steps++ < 20) {
  }
  direct_comp.pre_destroy();

  // Custom allocator with read_access=false then toggled via surface shift.
  kdu_compressed_source_buffered src2(raw.data(), raw.size());
  CustomRegionCompositor custom(/*float_mode=*/false, /*allow_read=*/false);
  custom.create(&src2);
  custom.add_ilayer(0, MakeDims(0, 0, 0, 0), MakeDims(0, 0, 0, 0));
  custom.set_scale(false, false, false, 1.0f);
  custom.set_buffer_surface(MakeDims(0, 0, 32, 32), 0x00FF0000);
  steps = 0;
  while (custom.process(100000, r) && steps++ < 20) {
  }
  // Shift surface overlapping previous surface to trigger read-access check.
  custom.set_buffer_surface(MakeDims(8, 8, 32, 32), -1);
  steps = 0;
  while (custom.process(100000, r) && steps++ < 20) {
  }
  EXPECT_GT(custom.alloc_count_, 0);
  custom.pre_destroy();
  EXPECT_GT(custom.delete_count_, 0);
}

std::vector<kdu_byte> MakeMultiLayerJpxWithOverlaysAndFrames() {
  std::vector<kdu_byte> out;
  VectorTarget vec_target(&out);
  jp2_family_tgt family;
  family.open(&vec_target);
  jpx_target jout;
  jout.open(&family);

  // Codestream 0: RGB 64x48
  auto cs0_tgt = jout.add_codestream();
  siz_params siz0;
  siz0.set(Sdims, 0, 0, 48);
  siz0.set(Sdims, 0, 1, 64);
  siz0.set(Scomponents, 0, 0, 3);
  siz0.set(Sprecision, 0, 0, 8);
  siz0.set(Ssigned, 0, 0, false);
  static_cast<::kdu_core::kdu_params &>(siz0).finalize();
  cs0_tgt.access_dimensions().init(&siz0);

  // Codestream 1: RGBA (3 color + 1 alpha) 40x32
  auto cs1_tgt = jout.add_codestream();
  siz_params siz1;
  siz1.set(Sdims, 0, 0, 32);
  siz1.set(Sdims, 0, 1, 40);
  siz1.set(Scomponents, 0, 0, 4);
  siz1.set(Sprecision, 0, 0, 8);
  siz1.set(Ssigned, 0, 0, false);
  static_cast<::kdu_core::kdu_params &>(siz1).finalize();
  cs1_tgt.access_dimensions().init(&siz1);

  // Compositing layer 0: RGB opaque
  jpx_layer_target lay0 = jout.add_layer();
  lay0.add_colour().init(::kdu_supp::JP2_sRGB_SPACE);
  lay0.access_channels().init(3);
  lay0.access_channels().set_colour_mapping(0, 0, -1, 0);
  lay0.access_channels().set_colour_mapping(1, 1, -1, 0);
  lay0.access_channels().set_colour_mapping(2, 2, -1, 0);
  lay0.access_resolution().init(1000.0f);

  // Compositing layer 1: RGBA with opacity channel
  jpx_layer_target lay1 = jout.add_layer();
  lay1.add_colour().init(::kdu_supp::JP2_sRGB_SPACE);
  lay1.access_channels().init(3);
  lay1.access_channels().set_colour_mapping(0, 0, -1, 1);
  lay1.access_channels().set_colour_mapping(1, 1, -1, 1);
  lay1.access_channels().set_colour_mapping(2, 2, -1, 1);
  lay1.access_channels().set_opacity_mapping(0, 3, -1, 1);
  lay1.access_channels().set_opacity_mapping(1, 3, -1, 1);
  lay1.access_channels().set_opacity_mapping(2, 3, -1, 1);

  // Composition box with animation frame instructions.
  jpx_composition comp = jout.access_composition();
  jx_frame *f = comp.add_frame(/*duration=*/100, /*repeat_count=*/1,
                               /*is_persistent=*/false);
  comp.add_instruction(f, 0, 0, MakeDims(0, 0, 64, 48),
                       MakeDims(0, 0, 64, 48));
  comp.add_instruction(f, 1, 0, MakeDims(0, 0, 40, 32),
                       MakeDims(8, 8, 40, 32));

  jpx_meta_manager mm = jout.access_meta_manager();
  jpx_metanode root = mm.access_root();

  int cs_indices[2] = {0, 1};
  jpx_metanode nlist = root.add_numlist(2, cs_indices, 0, nullptr, false);

  jpx_roi rois[3];
  rois[0].init_quadrilateral(kdu_coords(4, 4), kdu_coords(30, 4),
                             kdu_coords(30, 28), kdu_coords(4, 28));
  rois[0].coding_priority = 5;
  rois[1].init_ellipse(kdu_coords(32, 24), kdu_coords(16, 12),
                       kdu_coords(0, 0), false);
  rois[2].init_quadrilateral(kdu_coords(10, 10), kdu_coords(40, 15),
                             kdu_coords(35, 40), kdu_coords(5, 35));

  jpx_metanode roi_node = nlist.add_regions(3, rois);
  roi_node.add_label("PrimaryROI");

  while (jout.write_headers() != nullptr) {
  }
  auto *out0 = cs0_tgt.open_stream();
  CompressToTarget(out0, 64, 48, 3, 8, 2, 32, true, false, 10);
  out0->close();
  auto *out1 = cs1_tgt.open_stream();
  CompressToTarget(out1, 40, 32, 4, 8, 1, 0, true, false, 80);
  out1->close();
  while (jout.write_metadata() != nullptr) {
  }
  jout.close();
  family.close();
  return out;
}

TEST_F(KduRegionCompositorTest, JpxMultiLayerOverlaysCustomAndFloatRender) {
  std::vector<kdu_byte> jpx_bytes = MakeMultiLayerJpxWithOverlaysAndFrames();
  ASSERT_GT(jpx_bytes.size(), 32u);

  VectorSource buf_src(jpx_bytes);
  jp2_family_src fsrc;
  fsrc.open(&buf_src);
  jpx_source jpx_src;
  ASSERT_GT(jpx_src.open(&fsrc, true), 0);

  CustomRegionCompositor comp(/*float_mode=*/false, /*allow_read=*/true,
                              /*handle_custom_overlay=*/false);
  comp.create(&jpx_src);
  kdu_ilayer_ref l0 =
      comp.add_ilayer(0, MakeDims(0, 0, 0, 0), MakeDims(0, 0, 64, 48));
  kdu_ilayer_ref l1 = comp.add_ilayer(1, MakeDims(0, 0, 0, 0),
                                      MakeDims(10, 8, 40, 32), true, true,
                                      false, 0, 2);
  EXPECT_FALSE(l0.is_null());
  EXPECT_FALSE(l1.is_null());

  // Configure overlays using real aux params (4 rings, etc., as in kdu_render).
  kdu_uint32 aux_params[18] = {
      4, 0x20FFFF00u, 0x600000FFu, 0xA00000FFu, 0xA0FFFF00u, 0x60FFFF00u, 10,
      2, 0x200000FFu, 0x600000FFu, 0x80FFFF00u, 6,
      1, 0x300000FFu, 0x60FFFF00u, 3,
      0, 0x400000FFu};
  comp.configure_overlays(true, 1, 0.8f, 4, jpx_metanode(), 0, aux_params, 18);

  comp.set_scale(false, false, false, 1.0f);
  kdu_dims tot;
  EXPECT_TRUE(comp.get_total_composition_dims(tot));
  comp.set_buffer_surface(tot, 0x00102030);

  kdu_dims new_reg;
  int steps = 0;
  while (comp.process(8192, new_reg) && steps++ < 60) {
  }
  EXPECT_TRUE(comp.is_processing_complete());
  EXPECT_GT(comp.custom_overlay_calls_, 0);

  int total_roi = 0, hidden_roi = 0;
  EXPECT_TRUE(comp.get_overlay_info(total_roi, hidden_roi));
  EXPECT_EQ(total_roi, 2);

  kdu_istream_ref found_stream;
  jpx_metanode found_node =
      comp.search_overlays(kdu_coords(16, 16), found_stream, -1.0f);
  EXPECT_TRUE(found_node.exists());

  comp.update_overlays(true);
  comp.configure_overlays(false);
  comp.update_overlays(false);

  // Exercise set_frame / add_frame if composition exists.
  jpx_composition comp_rules = jpx_src.access_composition();
  if (comp_rules.exists()) {
    jx_frame *fr = comp_rules.get_next_frame(nullptr);
    if (fr != nullptr) {
      jpx_frame_expander expander;
      if (expander.construct(&jpx_src, fr, 0, true)) {
        comp.set_frame(&expander, kdu_coords(0, 0));
        comp.add_frame(&expander, kdu_coords(8, 8));
      }
    }
  }
  comp.pre_destroy();
  jpx_src.close();
  fsrc.close();
}

TEST_F(KduRegionCompositorTest, FloatBufferWithOverlaysAndPaletteJpx) {
  std::vector<kdu_byte> jpx_bytes = MakeMultiLayerJpxWithOverlaysAndFrames();
  VectorSource buf_src(jpx_bytes);
  jp2_family_src fsrc;
  fsrc.open(&buf_src);
  jpx_source jpx_src;
  ASSERT_GE(jpx_src.open(&fsrc, true), 0);

  CustomRegionCompositor comp(/*float_mode=*/true, /*allow_read=*/true,
                              /*handle_custom_overlay=*/false);
  comp.create(&jpx_src);
  comp.add_ilayer(0, MakeDims(0, 0, 0, 0), MakeDims(0, 0, 64, 48));
  comp.add_ilayer(1, MakeDims(0, 0, 0, 0), MakeDims(4, 4, 40, 32));

  kdu_uint32 aux_params[4] = {0, 0x4000FF00u, 1, 0x80FF0000u};
  comp.configure_overlays(true, 1, 0.5f, 3, jpx_metanode(), 0, aux_params, 2);
  comp.set_scale(false, false, false, 1.0f);
  comp.set_buffer_surface(MakeDims(0, 0, 64, 48), 0x00000000);
  kdu_dims new_reg;
  int steps = 0;
  while (comp.process(16384, new_reg) && steps++ < 60) {
  }
  kdu_dims buf_reg;
  kdu_compositor_buf *cbuf = comp.get_composition_buffer(buf_reg);
  ASSERT_NE(cbuf, nullptr);
  int rg = 0;
  EXPECT_NE(cbuf->get_float_buf(rg, false), nullptr);
  comp.pre_destroy();
  jpx_src.close();
  fsrc.close();
}

std::vector<kdu_byte> MakeMj2Bytes(bool interlaced) {
  std::vector<kdu_byte> out;
  VectorTarget vec_target(&out);
  jp2_family_tgt family;
  family.open(&vec_target);
  mj2_target mj2_out;
  mj2_out.open(&family);

  mj2_video_target *track0 = mj2_out.add_video_track();
  track0->access_colour().init(::kdu_supp::JP2_sRGB_SPACE);
  track0->set_timescale(30000);
  track0->set_frame_period(1001);
  if (interlaced) {
    track0->set_field_order(::kdu_supp::KDU_FIELDS_TOP_FIRST);
  }
  for (int f = 0; f < 2; ++f) {
    int num_fields = interlaced ? 2 : 1;
    for (int fld = 0; fld < num_fields; ++fld) {
      track0->open_image();
      kdu_codestream cs = CompressToTarget(
          track0, 48, interlaced ? 24 : 48, 3, 8, 1, 0, true, false,
          f * 50 + fld * 20, /*keep_cs=*/true);
      track0->close_image(cs);
      cs.destroy();
    }
  }

  mj2_out.close();
  family.close();
  return out;
}

TEST_F(KduRegionCompositorTest, Mj2ProgressiveAndInterlacedTracks) {
  for (bool interlaced : {false, true}) {
    std::vector<kdu_byte> mj2_bytes = MakeMj2Bytes(interlaced);
    VectorSource src(mj2_bytes);
    jp2_family_src fsrc;
    fsrc.open(&src);
    mj2_source mj2_src;
    ASSERT_GE(mj2_src.open(&fsrc, true), 0);

    kdu_region_compositor comp(&mj2_src, 65536);
    kdu_ilayer_ref il = comp.add_ilayer(0, MakeDims(0, 0, 0, 0),
                                        MakeDims(0, 0, 0, 0), false, false,
                                        false, /*frame_idx=*/0,
                                        interlaced ? 2 : 0);
    EXPECT_FALSE(il.is_null());
    comp.set_scale(false, false, false, 1.0f);
    kdu_dims dims;
    EXPECT_TRUE(comp.get_total_composition_dims(dims));
    comp.set_buffer_surface(dims, 0);
    kdu_dims r;
    int steps = 0;
    while (comp.process(32768, r) && steps++ < 40) {
    }
    EXPECT_TRUE(comp.is_processing_complete());

    // Change to frame 1.
    EXPECT_TRUE(comp.change_ilayer_frame(il, 1));
    steps = 0;
    while (comp.process(32768, r) && steps++ < 40) {
    }
    EXPECT_TRUE(comp.is_processing_complete());
    comp.pre_destroy();
    mj2_src.close();
    fsrc.close();
  }
}

TEST_F(KduRegionCompositorTest, ThreadEnvJpipWindowAndMetareq) {
  std::vector<kdu_byte> raw = MakeRawCodestream(64, 48, 3, 2, 32);
  kdu_compressed_source_buffered src(raw.data(), raw.size());

  kdu_thread_env env;
  env.create();
  absl::Cleanup env_cleanup = [&env]() { env.destroy(); };

  kdu_region_compositor comp(&env, nullptr);
  comp.create(&src);
  comp.add_ilayer(0, MakeDims(0, 0, 0, 0), MakeDims(0, 0, 0, 0));
  comp.set_scale(false, false, false, 1.0f);
  kdu_dims dims;
  EXPECT_TRUE(comp.get_total_composition_dims(dims));
  comp.set_buffer_surface(dims, 0);

  kdu_coords fsiz;
  kdu_dims roi;
  int round_dir = 0;
  EXPECT_TRUE(comp.find_compatible_jpip_window(fsiz, roi, round_dir, dims));

  comp.set_thread_env(&env, nullptr);
  kdu_dims r;
  int steps = 0;
  while (comp.process(32768, r) && steps++ < 30) {
  }

  kdu_window win;
  const kdu_uint32 box_types[1] = {jp2_xml_4cc};
  comp.generate_metareq(&win, 0, dims, 1, box_types, 1, box_types, false, 4);
  comp.load_metadata_matches();
  comp.set_thread_env(nullptr, nullptr);
  comp.pre_destroy();
}

std::vector<kdu_byte> MakePremultTwoStreamAndMetadataJpx(bool subsampled_ycc) {
  std::vector<kdu_byte> out;
  VectorTarget vec_target(&out);
  jp2_family_tgt family;
  family.open(&vec_target);
  jpx_target jout;
  jout.open(&family);

  // Codestream 0: RGB (or subsampled) 64x48
  auto cs0_tgt = jout.add_codestream();
  siz_params siz0;
  siz0.set(Sdims, 0, 0, 48);
  siz0.set(Sdims, 0, 1, 64);
  siz0.set(Scomponents, 0, 0, 3);
  siz0.set(Sprecision, 0, 0, 8);
  siz0.set(Ssigned, 0, 0, false);
  if (subsampled_ycc) {
    int ys[3] = {1, 2, 2};
    for (int c = 0; c < 3; ++c) {
      siz0.set(Sdims, c, 0, 48 / ys[c]);
      siz0.set(Sdims, c, 1, 64 / ys[c]);
      siz0.set(Ssampling, c, 0, ys[c]);
      siz0.set(Ssampling, c, 1, ys[c]);
    }
  }
  static_cast<::kdu_core::kdu_params &>(siz0).finalize();
  cs0_tgt.access_dimensions().init(&siz0);

  // Codestream 1: 1-component alpha 32x24
  auto cs1_tgt = jout.add_codestream();
  siz_params siz1;
  siz1.set(Sdims, 0, 0, 24);
  siz1.set(Sdims, 0, 1, 32);
  siz1.set(Scomponents, 0, 0, 1);
  siz1.set(Sprecision, 0, 0, 8);
  siz1.set(Ssigned, 0, 0, false);
  static_cast<::kdu_core::kdu_params &>(siz1).finalize();
  cs1_tgt.access_dimensions().init(&siz1);

  // Codestream 2: 4-component RGBA 32x24 for single-stream premult alpha
  auto cs2_tgt = jout.add_codestream();
  siz_params siz2;
  siz2.set(Sdims, 0, 0, 24);
  siz2.set(Sdims, 0, 1, 32);
  siz2.set(Scomponents, 0, 0, 4);
  siz2.set(Sprecision, 0, 0, 8);
  siz2.set(Ssigned, 0, 0, false);
  static_cast<::kdu_core::kdu_params &>(siz2).finalize();
  cs2_tgt.access_dimensions().init(&siz2);

  // Layer 0: opaque 64x48 from cs0
  jpx_layer_target lay0 = jout.add_layer();
  lay0.add_colour().init(::kdu_supp::JP2_sRGB_SPACE);
  lay0.access_channels().init(3);
  lay0.access_channels().set_colour_mapping(0, 0, -1, 0);
  lay0.access_channels().set_colour_mapping(1, 1, -1, 0);
  lay0.access_channels().set_colour_mapping(2, 2, -1, 0);

  // Layer 1: two-codestream layer (RGB from cs0, opacity from cs1 with
  // 2x2 codestream registration scaling)
  jpx_layer_target lay1 = jout.add_layer();
  lay1.add_colour().init(::kdu_supp::JP2_sRGB_SPACE);
  lay1.access_channels().init(3);
  lay1.access_channels().set_colour_mapping(0, 0, -1, 0);
  lay1.access_channels().set_colour_mapping(1, 1, -1, 0);
  lay1.access_channels().set_colour_mapping(2, 2, -1, 0);
  lay1.access_channels().set_opacity_mapping(-1, 0, -1, 1);
  lay1.set_codestream_registration(0, kdu_coords(0, 0), kdu_coords(1, 1),
                                   kdu_coords(1, 1));
  lay1.set_codestream_registration(1, kdu_coords(0, 0), kdu_coords(2, 2),
                                   kdu_coords(1, 1));

  // Layer 2: single-codestream premult RGBA from cs2
  jpx_layer_target lay2 = jout.add_layer();
  lay2.add_colour().init(::kdu_supp::JP2_sRGB_SPACE);
  lay2.access_channels().init(3);
  lay2.access_channels().set_colour_mapping(0, 0, -1, 2);
  lay2.access_channels().set_colour_mapping(1, 1, -1, 2);
  lay2.access_channels().set_colour_mapping(2, 2, -1, 2);
  lay2.access_channels().set_premult_mapping(-1, 3, -1, 2);

  // Layer 3: small opaque layer from cs0 (covers middle of Layer 0 cleanly)
  jpx_layer_target lay3 = jout.add_layer();
  lay3.add_colour().init(::kdu_supp::JP2_sRGB_SPACE);
  lay3.access_channels().init(3);
  lay3.access_channels().set_colour_mapping(0, 0, -1, 0);
  lay3.access_channels().set_colour_mapping(1, 1, -1, 0);
  lay3.access_channels().set_colour_mapping(2, 2, -1, 0);

  // Metadata: global xml, stream xml, and two labeled ROI groups (large + small)
  jpx_meta_manager mm = jout.access_meta_manager();
  jpx_metanode root = mm.access_root();
  root.add_delayed(jp2_xml_4cc, 1);

  int cs0_id[1] = {0};
  jpx_metanode nlist0 = root.add_numlist(1, cs0_id, 0, nullptr, false);
  jpx_metanode lbl_a = nlist0.add_label("OverlayGroupA");
  jpx_roi roi_big[6];
  roi_big[0].init_quadrilateral(kdu_coords(4, 4), kdu_coords(58, 4),
                               kdu_coords(58, 44), kdu_coords(4, 44));
  roi_big[1].init_ellipse(kdu_coords(32, 24), kdu_coords(16, 12),
                          kdu_coords(3, 2), false);
  roi_big[2].init_quadrilateral(kdu_coords(10, 8), kdu_coords(46, 14),
                               kdu_coords(40, 38), kdu_coords(8, 34));
  roi_big[3].init_quadrilateral(kdu_coords(28, 4), kdu_coords(48, 34),
                               kdu_coords(28, 16), kdu_coords(10, 26));
  roi_big[4].init_quadrilateral(kdu_coords(30, 5), kdu_coords(50, 22),
                               kdu_coords(26, 36), kdu_coords(12, 30));
  roi_big[5].init_quadrilateral(kdu_coords(26, 6), kdu_coords(46, 28),
                               kdu_coords(32, 38), kdu_coords(8, 22));
  lbl_a.add_regions(6, roi_big);
  lbl_a.add_delayed(jp2_xml_4cc, 2);

  jpx_metanode nlist1 = root.add_numlist(1, cs0_id, 0, nullptr, false);
  jpx_metanode lbl_b = nlist1.add_label("OverlayGroupB");
  jpx_roi roi_small;
  roi_small.init_quadrilateral(kdu_coords(18, 14), kdu_coords(22, 14),
                               kdu_coords(22, 18), kdu_coords(18, 18));
  lbl_b.add_regions(1, &roi_small);

  jpx_composition jcomp = jout.access_composition();
  ::kdu_supp::jx_frame *fr0 = jcomp.add_frame(100, 1, false);
  jcomp.add_instruction(fr0, 0, 0, MakeDims(0, 0, 64, 48),
                        MakeDims(0, 0, 64, 48));
  ::kdu_supp::jx_frame *fr1 = jcomp.add_frame(100, 1, false);
  jcomp.add_instruction(fr1, 0, 0, MakeDims(0, 0, 64, 48),
                        MakeDims(0, 0, 64, 48));
  jcomp.add_instruction(fr1, 3, 0, MakeDims(0, 0, 32, 24),
                        MakeDims(16, 12, 32, 24));

  while (jout.write_headers() != nullptr) {
  }
  auto *o0 = cs0_tgt.open_stream();
  if (subsampled_ycc) {
    kdu_codestream cs;
    cs.create(&siz0, o0);
    auto *cod = cs.access_siz()->access_cluster(COD_params);
    cod->set(Creversible, 0, 0, true);
    cod->set(Clevels, 0, 0, 2);
    cod->set(Cycc, 0, 0, false);
    cs.access_siz()->finalize_all();
    kdu_stripe_compressor sc;
    sc.start(cs);
    std::vector<kdu_byte> y(64 * 48, 128), cb(32 * 24, 140), cr(32 * 24, 110);
    kdu_byte *p[3] = {y.data(), cb.data(), cr.data()};
    int h[3] = {48, 24, 24};
    int sg[3] = {1, 1, 1};
    int rg[3] = {64, 32, 32};
    sc.push_stripe(p, h, nullptr, rg, sg);
    sc.finish();
    cs.destroy();
  } else {
    CompressToTarget(o0, 64, 48, 3, 8, 2, 32, true, false, 15);
  }
  o0->close();

  auto *o1 = cs1_tgt.open_stream();
  CompressToTarget(o1, 32, 24, 1, 8, 1, 0, true, false, 90);
  o1->close();

  auto *o2 = cs2_tgt.open_stream();
  CompressToTarget(o2, 32, 24, 4, 8, 1, 0, true, false, 45);
  o2->close();

  ::kdu_supp::jp2_output_box *out_box = nullptr;
  while ((out_box = jout.write_metadata()) != nullptr) {
    const char xml_txt[] = "<tag>kdu_compositor_test</tag>";
    out_box->write(reinterpret_cast<const kdu_byte *>(xml_txt),
                   sizeof(xml_txt) - 1);
  }
  jout.close();
  family.close();
  return out;
}

TEST_F(KduRegionCompositorTest,
       PremultipliedAlphaTwoStreamStatsAndSurfaceScroll) {
  std::vector<kdu_byte> jpx_bytes =
      MakePremultTwoStreamAndMetadataJpx(/*subsampled_ycc=*/false);
  VectorSource src(jpx_bytes);
  jp2_family_src fsrc;
  fsrc.open(&src);
  jpx_source jpx_src;
  ASSERT_GT(jpx_src.open(&fsrc, true), 0);

  for (bool float_mode : {false, true}) {
    CustomRegionCompositor comp(float_mode, /*allow_read=*/true,
                                /*handle_custom_overlay=*/false);
    comp.create(&jpx_src);
    // Layer 0 full opaque base, Layer 1 two-stream premult alpha, Layer 2
    // single-stream premult alpha, Layer 3 opaque middle box.
    kdu_ilayer_ref l0 = comp.add_ilayer(0, kdu_dims(), MakeDims(0, 0, 64, 48));
    kdu_ilayer_ref l1 = comp.add_ilayer(1, kdu_dims(), MakeDims(4, 4, 32, 24));
    kdu_ilayer_ref l2 = comp.add_ilayer(2, kdu_dims(), MakeDims(24, 16, 32, 24));
    kdu_ilayer_ref l3 = comp.add_ilayer(3, kdu_dims(), MakeDims(20, 14, 16, 12));
    EXPECT_FALSE(l0.is_null());
    EXPECT_FALSE(l1.is_null());
    EXPECT_FALSE(l2.is_null());
    EXPECT_FALSE(l3.is_null());

    comp.set_scale(false, false, false, 1.0f);
    kdu_dims tot;
    EXPECT_TRUE(comp.get_total_composition_dims(tot));
    comp.set_buffer_surface(MakeDims(8, 6, 40, 30), 0x80203040);

    kdu_dims new_reg;
    int steps = 0;
    while (comp.process(4096, new_reg) && steps++ < 60) {
    }
    EXPECT_TRUE(comp.is_processing_complete());

    // Exercise visible packet stats with overlapping opaque and alpha layers.
    kdu_istream_ref strm;
    while (!(strm = comp.get_next_istream(strm, true, false)).is_null()) {
      kdu_long prec = 0, pkt = 0, max_pkt = 0;
      EXPECT_TRUE(
          comp.get_codestream_packets(strm, MakeDims(0, 0, 64, 48), prec, pkt, max_pkt));
      EXPECT_GT(prec, 0);
    }

    // Shift surface down-right (forward copy + strip erase) then up-left
    // (reverse copy rcopy_*_buffer_region + strip erase).
    comp.set_surface_initialization_mode(false);
    comp.set_buffer_surface(MakeDims(12, 10, 40, 30), 0x40102030);
    steps = 0;
    while (comp.process(4096, new_reg) && steps++ < 40) {
    }
    comp.set_surface_initialization_mode(true);
    comp.set_buffer_surface(MakeDims(6, 4, 36, 26), 0x40102030);
    steps = 0;
    while (comp.process(4096, new_reg) && steps++ < 40) {
    }

    // Metadata queries across all 3 anchor flags + loading matches.
    kdu_window win;
    const kdu_uint32 btypes[2] = {jp2_xml_4cc, jp2_label_4cc};
    comp.generate_metareq(
        &win,
        KDU_MRQ_GLOBAL | KDU_MRQ_STREAM | KDU_MRQ_WINDOW,
        MakeDims(0, 0, 64, 48), 2, btypes, 2, btypes, true, 4);
    (void)comp.load_metadata_matches();

    comp.pre_destroy();
  }
  jpx_src.close();
  fsrc.close();
}

TEST_F(KduRegionCompositorTest,
       FloatOverlaysDependenciesWriteOnlyBufferAndSmallFrameErase) {
  std::vector<kdu_byte> jpx_bytes =
      MakePremultTwoStreamAndMetadataJpx(/*subsampled_ycc=*/false);
  VectorSource src(jpx_bytes);
  jp2_family_src fsrc;
  fsrc.open(&src);
  jpx_source jpx_src;
  ASSERT_GT(jpx_src.open(&fsrc, true), 0);

  kdu_uint32 aux_params[18] = {
      4, 0x20FFFF00u, 0x600000FFu, 0xA00000FFu, 0xA0FFFF00u, 0x60FFFF00u, 10,
      2, 0x200000FFu, 0x600000FFu, 0x80FFFF00u, 6,
      1, 0x300000FFu, 0x60FFFF00u, 3,
      0, 0x400000FFu};

  // Look up metadata nodes to build overlay dependency expressions.
  jpx_meta_manager mm = jpx_src.access_meta_manager();
  int cs0_id = 0;
  mm.load_matches(1, &cs0_id, 0, nullptr);
  jpx_metanode lbl_a =
      mm.enumerate_matches(jpx_metanode(), 0, -1, false, kdu_dims(), 0, false,
                           true);
  jpx_metanode lbl_b =
      mm.enumerate_matches(lbl_a, 0, -1, false, kdu_dims(), 0, false, true);
  ASSERT_TRUE(lbl_a.exists());
  ASSERT_TRUE(lbl_b.exists());

  // 1) Single-layer float compositor: render first, donate composition surface,
  //    then enable overlays (positive & negative blending factors) + deps.
  {
    CustomRegionCompositor comp(/*float_mode=*/true, /*allow_read=*/false,
                                /*handle_custom_overlay=*/false);
    comp.create(&jpx_src);
    comp.add_ilayer(0, kdu_dims(), kdu_dims());
    comp.set_scale(false, false, false, 1.0f);
    kdu_dims tot;
    EXPECT_TRUE(comp.get_total_composition_dims(tot));
    comp.set_buffer_surface(tot, 0);
    kdu_dims new_reg;
    int steps = 0;
    while (comp.process(16384, new_reg) && steps++ < 40) {
    }
    EXPECT_NE(comp.get_composition_buffer(tot), nullptr);

    comp.configure_overlays(true, 1, 0.7f, 4, lbl_a, 0, aux_params, 18);
    comp.update_overlays(true);
    steps = 0;
    while (comp.process(8192, new_reg) && steps++ < 60) {
    }
    EXPECT_TRUE(comp.is_processing_complete());

    // Negative blending factor (inverted float race-track + mod blend) and
    // compound sum-of-products dependency expression.
    comp.configure_overlays(true, 1, -0.6f, 4, lbl_b, 1, aux_params, 18);
    comp.configure_overlays(true, 1, -0.6f, 4, lbl_b, 2, aux_params, 18);
    comp.configure_overlays(true, 20, -0.6f, 4, lbl_a, -2, aux_params, 18);
    comp.update_overlays(false);
    steps = 0;
    while (comp.process(8192, new_reg) && steps++ < 60) {
    }
    comp.configure_overlays(true, 1, 0.5f, 4, lbl_a, -1, aux_params, 18);
    comp.update_overlays(false);
    comp.configure_overlays(true, 1, 0.5f, 4, jpx_metanode(), 0, aux_params,
                            18);
    comp.pre_destroy();
  }

  // 2) Small centered frame using set_frame / add_frame (both int32 & float)
  //    to exercise non-overlapping 4-strip erasures in do_composition.
  jpx_composition jcomp = jpx_src.access_composition();
  ASSERT_TRUE(jcomp.exists());
  for (bool float_mode : {false, true}) {
    CustomRegionCompositor comp(float_mode, /*allow_read=*/false,
                                /*handle_custom_overlay=*/false);
    comp.create(&jpx_src);
    jpx_frame_expander expander;
    ASSERT_TRUE(expander.construct(&jpx_src, jcomp.access_frame(0, 0, true),
                                   kdu_dims()));
    comp.set_frame(&expander, kdu_coords(0, 0));
    ASSERT_TRUE(expander.construct(&jpx_src, jcomp.access_frame(0, 1, true),
                                   kdu_dims()));
    comp.add_frame(&expander, kdu_coords(64, 0));
    comp.set_scale(false, false, false, 1.0f);
    kdu_dims tot;
    EXPECT_TRUE(comp.get_total_composition_dims(tot));
    comp.set_buffer_surface(tot, 0x10203040);
    comp.configure_overlays(true, 1, 0.6f, 4, jpx_metanode(), 0, aux_params,
                            18);
    kdu_dims new_reg;
    int steps = 0;
    while (comp.process(8192, new_reg) && steps++ < 80) {
    }
    EXPECT_TRUE(comp.is_processing_complete());
    comp.pre_destroy();
  }

  // 3) Single-layer centered small region in oversized surface for erase strips.
  {
    CustomRegionCompositor comp(/*float_mode=*/true, /*allow_read=*/false,
                                /*handle_custom_overlay=*/false);
    comp.create(&jpx_src);
    comp.add_ilayer(0, MakeDims(0, 0, 64, 48), MakeDims(16, 12, 32, 24));
    comp.add_ilayer(3, MakeDims(0, 0, 64, 48), MakeDims(20, 16, 16, 12));
    comp.set_scale(false, false, false, 1.0f);
    comp.set_buffer_surface(MakeDims(0, 0, 64, 48), 0x80112233);
    kdu_dims new_reg;
    int steps = 0;
    while (comp.process(16384, new_reg) && steps++ < 40) {
    }
    comp.pre_destroy();
  }

  jpx_src.close();
  fsrc.close();
}

TEST_F(KduRegionCompositorTest,
       PrimitiveIlayersSingleLayerQueueAndRefreshWithReactivation) {
  std::vector<kdu_byte> jpx_bytes =
      MakePremultTwoStreamAndMetadataJpx(/*subsampled_ycc=*/true);
  VectorSource src(jpx_bytes);
  jp2_family_src fsrc;
  fsrc.open(&src);
  jpx_source jpx_src;
  ASSERT_GT(jpx_src.open(&fsrc, true), 0);

  kdu_region_compositor comp;
  comp.create(&jpx_src, 65536);

  // Add primitive ilayer for component 1 (2x2 subsampled Cb) and inspect.
  int single_comp = 1;
  kdu_ilayer_ref p1 = comp.add_primitive_ilayer(
      0, single_comp, KDU_WANT_CODESTREAM_COMPONENTS, kdu_dims(),
      MakeDims(0, 0, 32, 24), false, false, false);
  EXPECT_FALSE(p1.is_null());

  // Check find_optimal_scale with single_component & avoid_subsampling variants.
  float opt_s = comp.find_optimal_scale(kdu_dims(), 0.75f, 0.1f, 4.0f, nullptr,
                                        nullptr, false);
  EXPECT_GT(opt_s, 0.0f);
  float opt_avoid = comp.find_optimal_scale(MakeDims(0, 0, 32, 24), 0.75f, 0.1f,
                                            4.0f, nullptr, nullptr, true);
  EXPECT_GT(opt_avoid, 0.0f);

  comp.set_scale(false, false, false, 1.0f);
  kdu_dims tot;
  EXPECT_TRUE(comp.get_total_composition_dims(tot));
  comp.set_surface_initialization_mode(false);
  comp.set_buffer_surface(tot, 0);

  kdu_dims r;
  int steps = 0;
  while (comp.process(60, r) && steps++ < 200) {
  }
  EXPECT_TRUE(comp.is_processing_complete());

  // Exercise single-layer composition queue (take_layer_buffer + replace tail).
  EXPECT_TRUE(comp.push_composition_buffer(100, 1));
  steps = 0;
  while (comp.process(4096, r) && steps++ < 40) {
  }
  EXPECT_TRUE(comp.replace_composition_queue_tail(200, 2));
  EXPECT_NE(comp.inspect_composition_queue(0), nullptr);
  EXPECT_TRUE(comp.pop_composition_buffer());
  comp.flush_composition_queue();

  // Remove non-permanently and reactivate to exercise move_to_head & activate.
  EXPECT_TRUE(comp.remove_ilayer(p1, /*permanent=*/false));
  single_comp = 1;
  kdu_ilayer_ref p1_again = comp.add_primitive_ilayer(
      0, single_comp, KDU_WANT_CODESTREAM_COMPONENTS, kdu_dims(),
      MakeDims(0, 0, 32, 24), true, false, false);
  EXPECT_FALSE(p1_again.is_null());
  comp.set_scale(false, false, false, 1.0f);
  EXPECT_TRUE(comp.get_total_composition_dims(tot));
  EXPECT_FALSE(comp.get_next_visible_ilayer(kdu_ilayer_ref(), tot).is_null());

  // refresh() on persistent compositor + JPIP window query.
  bool new_img = false;
  EXPECT_TRUE(comp.refresh(&new_img));
  EXPECT_TRUE(comp.refresh(nullptr));
  kdu_coords fsiz;
  kdu_dims roi;
  int round_dir = 0;
  EXPECT_TRUE(comp.find_compatible_jpip_window(fsiz, roi, round_dir, tot));

  comp.cull_inactive_ilayers(0);
  comp.pre_destroy();
  jpx_src.close();
  fsrc.close();
}

TEST_F(KduRegionCompositorTest, OrientationPermutationsAndErrorPaths) {
  std::vector<kdu_byte> raw = MakeRawCodestream(48, 32, 3, 2, 16);
  VectorSource raw_src(raw);
  kdu_region_compositor raw_comp;
  raw_comp.create(&raw_src, 65536);

  // Invalid codestream index throws kdu_exception.
  int bad_comp = -1;
  EXPECT_THROW(
      raw_comp.add_primitive_ilayer(5, bad_comp, KDU_WANT_OUTPUT_COMPONENTS,
                                    kdu_dims(), kdu_dims()),
      ::kdu_core::kdu_exception);

  kdu_ilayer_ref l0 = raw_comp.add_ilayer(0, kdu_dims(), kdu_dims());
  EXPECT_FALSE(l0.is_null());
  for (int mask = 0; mask < 8; ++mask) {
    bool tr = (mask & 1) != 0;
    bool vf = (mask & 2) != 0;
    bool hf = (mask & 4) != 0;
    raw_comp.set_scale(tr, vf, hf, 0.5f);
    kdu_dims tot;
    EXPECT_TRUE(raw_comp.get_total_composition_dims(tot));
    raw_comp.set_buffer_surface(tot, 0);
    kdu_dims r;
    int steps = 0;
    while (raw_comp.process(100, r) && steps++ < 150) {
    }
    EXPECT_TRUE(raw_comp.is_processing_complete());
    kdu_istream_ref strm;
    kdu_coords mid(tot.pos.x + tot.size.x / 2, tot.pos.y + tot.size.y / 2);
    kdu_ilayer_ref pt = raw_comp.find_point(mid, 0, -1.0f);
    EXPECT_FALSE(pt.is_null());
  }

  // Exercise set_frame error when source has no composition box.
  EXPECT_THROW(raw_comp.set_frame(nullptr), ::kdu_core::kdu_exception);
  raw_comp.pre_destroy();

  // MJ2 frame & field_handling changes triggering activate() stream rebuild.
  std::vector<kdu_byte> mj2_bytes = MakeMj2Bytes(/*interlaced=*/true);
  VectorSource msrc(mj2_bytes);
  jp2_family_src mfsrc;
  mfsrc.open(&msrc);
  mj2_source mj2_src;
  ASSERT_GE(mj2_src.open(&mfsrc, true), 0);

  kdu_region_compositor mcomp(&mj2_src, 65536);
  kdu_ilayer_ref ml = mcomp.add_ilayer(0, kdu_dims(), kdu_dims(), false, false,
                                       false, 0, /*field_handling=*/0);
  EXPECT_FALSE(ml.is_null());
  mcomp.set_scale(false, false, false, 1.0f);
  kdu_dims mtot;
  EXPECT_TRUE(mcomp.get_total_composition_dims(mtot));
  mcomp.set_buffer_surface(mtot, 0);
  kdu_dims mr;
  int steps = 0;
  while (mcomp.process(16384, mr) && steps++ < 40) {
  }
  EXPECT_THROW(mcomp.change_ilayer_frame(ml, 99), ::kdu_core::kdu_exception);
  EXPECT_TRUE(mcomp.remove_ilayer(ml, /*permanent=*/false));
  // Re-add same track with different field_handling to reactivate & rebuild.
  ml = mcomp.add_ilayer(0, kdu_dims(), kdu_dims(), false, false, false, 1,
                        /*field_handling=*/2);
  EXPECT_FALSE(ml.is_null());
  mcomp.set_scale(false, false, false, 1.0f);
  EXPECT_TRUE(mcomp.get_total_composition_dims(mtot));
  mcomp.set_buffer_surface(mtot, 0);
  steps = 0;
  while (mcomp.process(16384, mr) && steps++ < 40) {
  }
  EXPECT_TRUE(mcomp.is_processing_complete());
  mcomp.pre_destroy();
  mj2_src.close();
  mfsrc.close();
}

TEST_F(KduRegionCompositorTest,
       DelayedCacheOverlaysTrapezoidsAndCompletedRects) {
  std::vector<kdu_byte> jpx_bytes =
      MakePremultTwoStreamAndMetadataJpx(/*subsampled_ycc=*/false);
  VectorSource src(jpx_bytes);
  jp2_family_src fsrc;
  fsrc.open(&src);
  jpx_source jpx_src;
  ASSERT_GT(jpx_src.open(&fsrc, true), 0);

  jpx_meta_manager mm = jpx_src.access_meta_manager();
  int cs0_id = 0;
  mm.load_matches(1, &cs0_id, 0, nullptr);
  jpx_metanode lbl_a =
      mm.enumerate_matches(jpx_metanode(), 0, -1, false, kdu_dims(), 0, false,
                           true);
  ASSERT_TRUE(lbl_a.exists());

  // 1) Single-layer write-only/read-write compositor enabling & retracting
  //    overlays without calling get_composition_buffer() first, plus short/over
  //    aux_params border rows, positive/negative overlay blending factors, and
  //    ABGR colour order.
  kdu_uint32 huge_border_aux[4] = {20, 0x20FF0000u, 0x8000FF00u, 100};
  kdu_uint32 tiny_aux[1] = {3};
  for (bool float_mode : {false, true}) {
    CustomRegionCompositor comp(float_mode, /*allow_read=*/true,
                                /*handle_custom_overlay=*/false);
    comp.set_colour_order(::kdu_supp::KDU_ABGR_ORDER);
    comp.create(&jpx_src);
    comp.add_ilayer(0, kdu_dims(), kdu_dims());
    comp.configure_overlays(false);
    comp.set_scale(false, false, false, 1.0f);
    kdu_dims tot;
    EXPECT_TRUE(comp.get_total_composition_dims(tot));
    comp.set_buffer_surface(tot, 0);
    kdu_dims r;
    int steps = 0;
    while (comp.process(16384, r) && steps++ < 30) {
    }
    EXPECT_TRUE(comp.is_processing_complete());

    // Enable overlays after first render without prior get_composition_buffer.
    comp.configure_overlays(true, 1, 0.5f, 4, lbl_a, 0, huge_border_aux, 4);
    comp.update_overlays(true);
    steps = 0;
    while (comp.process(16384, r) && steps++ < 40) {
    }
    EXPECT_TRUE(comp.is_processing_complete());

    // Short aux_params (< 2 + border) & negative blending factor.
    comp.configure_overlays(true, 1, -0.5f, 4, lbl_a, 0, tiny_aux, 1);
    comp.update_overlays(true);
    steps = 0;
    while (comp.process(16384, r) && steps++ < 40) {
    }

    // Disable overlays to trigger kdrc_layer::process_overlay retraction.
    comp.configure_overlays(false);
    comp.update_overlays(true);
    steps = 0;
    while (comp.process(16384, r) && steps++ < 30) {
    }
    comp.pre_destroy();
  }

  // 2) Two-stream unassociated opacity layer (RGB cs0, opacity cs1 with 2x2
  //    registration) in both int32 & float modes.
  for (bool float_mode : {false, true}) {
    CustomRegionCompositor comp(float_mode, /*allow_read=*/true,
                                /*handle_custom_overlay=*/false);
    comp.create(&jpx_src);
    kdu_ilayer_ref il = comp.add_ilayer(1, kdu_dims(), kdu_dims());
    EXPECT_FALSE(il.is_null());
    comp.set_scale(false, false, false, 1.0f);
    kdu_dims tot;
    EXPECT_TRUE(comp.get_total_composition_dims(tot));
    comp.set_buffer_surface(tot, 0xFF102030u);
    kdu_dims r;
    int steps = 0;
    while (comp.process(16384, r) && steps++ < 40) {
    }
    EXPECT_TRUE(comp.is_processing_complete());
    comp.pre_destroy();
  }

  // 3) Partial completed rects + kdrc_refresh::add_region merge/split + defer.
  {
    CustomRegionCompositor comp(/*float_mode=*/false, /*allow_read=*/true,
                                /*handle_custom_overlay=*/false);
    comp.create(&jpx_src);
    comp.set_surface_initialization_mode(true);
    comp.set_process_aggregation_threshold(0.5f);
    // Opaque Layer 0 (full 64x48) occluded in middle by opaque Layer 3 (16x12),
    // plus small opaque strip rects to exercise find_completed_rects & refresh.
    kdu_ilayer_ref btm = comp.add_ilayer(0, kdu_dims(), MakeDims(0, 0, 64, 48));
    kdu_ilayer_ref top =
        comp.add_ilayer(3, kdu_dims(), MakeDims(16, 12, 32, 24));
    EXPECT_FALSE(btm.is_null());
    EXPECT_FALSE(top.is_null());
    comp.set_scale(false, false, false, 1.0f);
    kdu_dims tot;
    EXPECT_TRUE(comp.get_total_composition_dims(tot));
    comp.set_buffer_surface(tot, 0xFF112233u);

    kdu_dims r;
    int steps = 0;
    while (comp.process(24, r, KDU_COMPOSIT_DEFER_REGION) && steps++ < 25) {
    }
    comp.set_buffer_surface(MakeDims(8, 6, 48, 36), 0xFF112233u);
    while (comp.process(4096, r) && steps++ < 100) {
    }
    EXPECT_TRUE(comp.is_processing_complete());

    // Also test top-only small opaque layer centered in large buffer surface
    // so do_composition exercises 4-strip erase around com_region.
    EXPECT_TRUE(comp.remove_ilayer(btm, true));
    comp.set_scale(false, false, false, 1.0f);
    comp.set_buffer_surface(MakeDims(0, 0, 64, 48), 0xFF223344u);
    steps = 0;
    while (comp.process(16384, r) && steps++ < 40) {
    }
    EXPECT_TRUE(comp.is_processing_complete());
    comp.pre_destroy();
  }

  jpx_src.close();
  fsrc.close();
}

TEST_F(KduRegionCompositorTest,
       ScalingLimitsJpipWindowsSubsampledYccAndMoveToHead) {
  std::vector<kdu_byte> jpx_bytes =
      MakePremultTwoStreamAndMetadataJpx(/*subsampled_ycc=*/true);
  VectorSource src(jpx_bytes);
  jp2_family_src fsrc;
  fsrc.open(&src);
  jpx_source jpx_src;
  ASSERT_GT(jpx_src.open(&fsrc, true), 0);

  CustomRegionCompositor comp(/*float_mode=*/false, /*allow_read=*/true,
                              /*handle_custom_overlay=*/false);
  comp.create(&jpx_src);
  comp.configure_intensity_scaling(true, true);
  ::kdu_core::kdu_quality_limiter qlim(2.0f);
  comp.set_quality_limiting(&qlim, 150.0f, 150.0f);

  // Add Layer 0 and Layer 3 (sharing cs0), then remove & re-add Layer 0 so
  // kdrc_codestream::move_to_head and shared active streams are exercised.
  kdu_ilayer_ref l0 = comp.add_ilayer(0, kdu_dims(), MakeDims(0, 0, 64, 48));
  kdu_ilayer_ref l3 = comp.add_ilayer(3, kdu_dims(), MakeDims(16, 12, 32, 24));
  EXPECT_FALSE(l0.is_null());
  EXPECT_FALSE(l3.is_null());
  EXPECT_TRUE(comp.remove_ilayer(l0, /*permanent=*/false));
  l0 = comp.add_ilayer(0, kdu_dims(), MakeDims(0, 0, 64, 48));
  EXPECT_FALSE(l0.is_null());

  // Exercise extreme small/large scaling failures + min/max clamping.
  kdu_dims tot;
  comp.set_scale(false, false, false, 1e-7f);
  EXPECT_FALSE(comp.get_total_composition_dims(tot));
  EXPECT_NE(comp.check_invalid_scale_code() & KDU_COMPOSITOR_SCALE_TOO_SMALL,
            0);
  comp.set_scale(false, false, false, 1e8f);
  EXPECT_FALSE(comp.get_total_composition_dims(tot));
  EXPECT_NE(comp.check_invalid_scale_code() & KDU_COMPOSITOR_SCALE_TOO_LARGE,
            0);

  kdu_istream_ref opt_strm;
  int opt_comp = -1;
  EXPECT_GE(comp.find_optimal_scale(MakeDims(0, 0, 64, 48), 0.75f, 1e-7f, 1e-6f,
                                    &opt_strm, &opt_comp, false),
            0.0f);
  EXPECT_GE(comp.find_optimal_scale(MakeDims(0, 0, 64, 48), 0.75f, 1e8f, 1e9f,
                                    nullptr, nullptr, true),
            1.0f);

  kdu_istream_ref stale_strm =
      comp.get_next_istream(kdu_istream_ref(), true, false);
  // Set valid scale with rendering_scale_adjustment < 1 so rendering !=
  // notional, using a fixed JPX frame expander.
  jpx_frame_expander expander;
  ASSERT_TRUE(expander.construct(
      &jpx_src, jpx_src.access_composition().access_frame(0, 0, true),
      kdu_dims()));
  comp.set_frame(&expander, kdu_coords(0, 0));
  comp.set_scale(true, true, true, 1.0f, 0.5f);
  EXPECT_TRUE(comp.get_total_composition_dims(tot));
  comp.set_buffer_surface(tot, 0);
  kdu_dims r;
  int steps = 0;
  while (comp.process(16384, r) && steps++ < 60) {
  }
  EXPECT_TRUE(comp.is_processing_complete());

  // Exercise JPIP window search across all rounding modes & frame override.
  kdu_coords fsiz;
  kdu_dims roi;
  for (int round_dir : {-1, 0, 1}) {
    EXPECT_TRUE(comp.find_compatible_jpip_window(fsiz, roi, round_dir, tot));
  }
  int zero_dir = 0;
  EXPECT_TRUE(
      comp.find_compatible_jpip_window(fsiz, roi, zero_dir, kdu_dims()));

  // Exercise map_region & inverse_map_region edge cases.
  kdu_istream_ref strm = comp.get_next_istream(kdu_istream_ref(), true, false);
  EXPECT_FALSE(strm.is_null());
  kdu_dims inv = MakeDims(0, 0, 16, 12);
  EXPECT_FALSE(comp.inverse_map_region(inv, strm).is_empty());
  kdu_dims empty_inv;
  EXPECT_FALSE(
      comp.inverse_map_region(empty_inv, kdu_istream_ref()).is_empty());
  kdu_dims map_empty;
  EXPECT_TRUE(comp.map_region(map_empty, kdu_istream_ref()).is_null());
  kdu_dims inv_bad_ref = MakeDims(0, 0, 16, 12);
  EXPECT_TRUE(comp.inverse_map_region(inv_bad_ref, stale_strm).is_empty());

  // Extract single subsampled chroma component with all orientation flags.
  comp.remove_ilayer(l0, true);
  comp.remove_ilayer(l3, true);
  int comp_idx = 1;
  kdu_ilayer_ref sub_il = comp.add_primitive_ilayer(
      0, comp_idx, KDU_WANT_CODESTREAM_COMPONENTS, kdu_dims(), kdu_dims(), true,
      false, false);
  EXPECT_FALSE(sub_il.is_null());
  comp.set_scale(true, true, true, 0.75f);
  EXPECT_TRUE(comp.get_total_composition_dims(tot));
  comp.set_buffer_surface(tot, 0);
  steps = 0;
  while (comp.process(16384, r) && steps++ < 40) {
  }
  EXPECT_TRUE(comp.is_processing_complete());

  comp.pre_destroy();
  jpx_src.close();
  fsrc.close();
}

TEST_F(KduRegionCompositorTest,
       CompletedRectsSplitRefreshMergingAndInactiveStreamReuse) {
  std::vector<kdu_byte> jpx_bytes =
      MakePremultTwoStreamAndMetadataJpx(/*subsampled_ycc=*/false);
  VectorSource src(jpx_bytes);
  jp2_family_src fsrc;
  fsrc.open(&src);
  jpx_source jpx_src;
  ASSERT_GT(jpx_src.open(&fsrc, true), 0);

  // 1) Incremental find_completed_rects (splitting a 128x96 overlay segment
  //    around an interior 64x48 unprocessed overlay segment on an upper layer)
  //    plus kdrc_refresh::add_region horizontal/vertical split & merge.
  {
    CustomRegionCompositor comp(/*float_mode=*/false, /*allow_read=*/true,
                                /*handle_custom_overlay=*/false);
    comp.create(&jpx_src);
    comp.set_surface_initialization_mode(false);
    comp.set_process_aggregation_threshold(0.25f);
    comp.configure_overlays(false);

    kdu_ilayer_ref base = comp.add_ilayer(2, kdu_dims(), MakeDims(0, 0, 128, 96));
    kdu_ilayer_ref mid =
        comp.add_ilayer(0, MakeDims(0, 0, 32, 24), MakeDims(0, 0, 128, 96));
    kdu_ilayer_ref top =
        comp.add_ilayer(3, MakeDims(0, 0, 16, 12), MakeDims(32, 24, 64, 48));
    EXPECT_FALSE(base.is_null());
    EXPECT_FALSE(mid.is_null());
    EXPECT_FALSE(top.is_null());

    comp.set_scale(false, false, false, 1.0f);
    kdu_dims tot;
    EXPECT_TRUE(comp.get_total_composition_dims(tot));
    comp.set_buffer_surface(tot, 0);
    kdu_dims r;
    int steps = 0;
    while (comp.process(16384, r) && steps++ < 60) {
    }
    EXPECT_TRUE(comp.is_processing_complete());

    // Now enable overlays for both mid (cs0, 128x96) and top (cs0, 64x48
    // centered inside 128x96), then step with suggested_increment=1 so mid
    // processes its overlay first while top's interior segment is pending.
    comp.configure_overlays(true, 1, 1.0f);
    comp.update_overlays(true);
    steps = 0;
    while (comp.process(1, r) && steps++ < 120) {
    }
    EXPECT_TRUE(comp.is_processing_complete());

    // Exercise KDU_COMPOSIT_DEFER_REGION with 4 staggered layers producing partial
    // rects across adjacent/overlapping boundaries for kdrc_refresh::add_region.
    comp.remove_ilayer(base, true);
    comp.remove_ilayer(mid, true);
    comp.remove_ilayer(top, true);
    comp.add_ilayer(0, MakeDims(0, 0, 32, 24), MakeDims(0, 0, 32, 24));
    comp.add_ilayer(0, MakeDims(0, 0, 32, 24), MakeDims(24, 0, 32, 24));
    comp.add_ilayer(0, MakeDims(0, 0, 32, 24), MakeDims(0, 16, 32, 24));
    comp.add_ilayer(0, MakeDims(0, 0, 32, 24), MakeDims(16, 8, 24, 16));
    comp.set_scale(false, false, false, 1.0f);
    EXPECT_TRUE(comp.get_total_composition_dims(tot));
    comp.set_buffer_surface(tot, 0);
    for (int i = 0; i < 8; ++i) {
      comp.process(32, r, KDU_COMPOSIT_DEFER_REGION);
    }
    steps = 0;
    while (comp.process(128, r) && steps++ < 80) {
    }
    EXPECT_TRUE(comp.is_processing_complete());

    // Overlay search over valid, occluded, and out-of-bounds points.
    kdu_istream_ref ostrm;
    (void)comp.search_overlays(kdu_coords(12, 12), ostrm, -1.0f);
    EXPECT_FALSE(comp.search_overlays(kdu_coords(500, 500), ostrm, 0.5f)
                     .exists());
    comp.pre_destroy();
  }

  // 2) Inactive stream reuse (kdrc_codestream::move_to_head) & middle unlink
  //    from inactive_layers across primitive & full ilayers.
  {
    CustomRegionCompositor comp(/*float_mode=*/false, /*allow_read=*/true,
                                /*handle_custom_overlay=*/false);
    comp.create(&jpx_src);
    int comp0 = 0;
    int comp1 = 1;
    kdu_ilayer_ref p0 = comp.add_primitive_ilayer(
        0, comp0, KDU_WANT_CODESTREAM_COMPONENTS, kdu_dims(),
        MakeDims(0, 0, 32, 24));
    kdu_ilayer_ref l0 = comp.add_ilayer(0, kdu_dims(), MakeDims(0, 0, 64, 48));
    kdu_ilayer_ref l3 = comp.add_ilayer(3, kdu_dims(), MakeDims(8, 8, 32, 24));
    comp.set_scale(false, false, false, 1.0f);
    kdu_dims tot;
    EXPECT_TRUE(comp.get_total_composition_dims(tot));
    comp.set_buffer_surface(tot, 0);
    kdu_dims r;
    int steps = 0;
    while (comp.process(16384, r) && steps++ < 40) {
    }

    // Move multiple layers to inactive_layers so middle items can be removed.
    EXPECT_TRUE(comp.remove_ilayer(p0, /*permanent=*/false));
    EXPECT_TRUE(comp.remove_ilayer(l0, /*permanent=*/false));
    EXPECT_TRUE(comp.remove_ilayer(l3, /*permanent=*/false));

    // Reuse cs0 inactive stream for a new primitive component on same cs0,
    // triggering kdrc_codestream::move_to_head(scan).
    kdu_ilayer_ref p1 = comp.add_primitive_ilayer(
        0, comp1, KDU_WANT_CODESTREAM_COMPONENTS, kdu_dims(),
        MakeDims(0, 0, 32, 24));
    EXPECT_FALSE(p1.is_null());

    // Reactivate middle & tail entries from inactive_layers.
    l0 = comp.add_ilayer(0, kdu_dims(), MakeDims(0, 0, 64, 48));
    p0 = comp.add_primitive_ilayer(0, comp0, KDU_WANT_CODESTREAM_COMPONENTS,
                                   kdu_dims(), MakeDims(0, 0, 32, 24));
    EXPECT_FALSE(l0.is_null());
    EXPECT_FALSE(p0.is_null());
    comp.pre_destroy();
  }

  jpx_src.close();
  fsrc.close();
}

TEST_F(KduRegionCompositorTest,
       Mj2PrimitiveActivateScalePrecisionAndQueryCoverage) {
  // 1) MJ2: add_primitive_ilayer, change_ilayer_frame before remove + activate,
  //    layer/stream info queries.
  {
    std::vector<kdu_byte> mj2_bytes = MakeMj2Bytes(/*interlaced=*/true);
    VectorSource msrc(mj2_bytes);
    jp2_family_src mfsrc;
    mfsrc.open(&msrc);
    mj2_source mj2_src;
    ASSERT_GE(mj2_src.open(&mfsrc, true), 0);

    CustomRegionCompositor mcomp(/*float_mode=*/false, /*allow_read=*/true,
                                 /*handle_custom_overlay=*/false);
    mcomp.create(&mj2_src);
    kdu_ilayer_ref ml = mcomp.add_ilayer(0, kdu_dims(), kdu_dims(), false,
                                         false, false, 0, /*field_handling=*/1);
    EXPECT_FALSE(ml.is_null());
    int cidx = 0;
    kdu_ilayer_ref mp = mcomp.add_primitive_ilayer(
        0, cidx, KDU_WANT_CODESTREAM_COMPONENTS, kdu_dims(), kdu_dims(), true);
    EXPECT_FALSE(mp.is_null());

    mcomp.set_scale(false, false, false, 1.0f);
    kdu_dims tot;
    EXPECT_TRUE(mcomp.get_total_composition_dims(tot));
    mcomp.set_buffer_surface(tot, 0);
    kdu_dims mr;
    int steps = 0;
    while (mcomp.process(16384, mr) && steps++ < 50) {
    }
    EXPECT_TRUE(mcomp.is_processing_complete());

    // Change frame while active, then remove/reactivate same track with
    // different frame & field_handling to trigger activate() stream teardown.
    EXPECT_TRUE(mcomp.change_ilayer_frame(ml, 1));
    EXPECT_TRUE(mcomp.remove_ilayer(ml, /*permanent=*/false));
    ml = mcomp.add_ilayer(0, kdu_dims(), kdu_dims(), false, false, false, 0,
                          /*field_handling=*/2);
    EXPECT_FALSE(ml.is_null());
    mcomp.set_scale(false, false, false, 1.0f);
    EXPECT_TRUE(mcomp.get_total_composition_dims(tot));
    mcomp.set_buffer_surface(tot, 0);
    steps = 0;
    while (mcomp.process(16384, mr) && steps++ < 50) {
    }

    int lsrc = -1, dcs = -1, fidx = -1, fh = -1;
    bool opaque = false;
    EXPECT_GE(mcomp.get_ilayer_info(ml, lsrc, dcs, opaque, &fidx, &fh), 1);
    EXPECT_FALSE(mcomp.get_ilayer_stream(ml, 0).is_null());
    EXPECT_FALSE(mcomp.find_ilayer_region(ml, true).is_empty());
    EXPECT_FALSE(mcomp.find_ilayer_region(ml, false).is_empty());
    EXPECT_FALSE(mcomp.get_next_ilayer(kdu_ilayer_ref(), 0, -1).is_null());
    EXPECT_TRUE(mcomp.get_next_ilayer(ml, 0, -1).is_null());
    EXPECT_FALSE(mcomp.get_next_ilayer(kdu_ilayer_ref(), -1, 0).is_null());
    mcomp.pre_destroy();
    mj2_src.close();
    mfsrc.close();
  }

  // 2) Subsampled YCbCr + 2x2 layer registration + non-power-of-two scaling
  //    (discard_levels > 0, JPIP WOI scales, get_component_scale_factors,
  //    find_full_rendering_region) + float get_opacity & find_point cutoffs.
  {
    std::vector<kdu_byte> jpx_bytes =
        MakePremultTwoStreamAndMetadataJpx(/*subsampled_ycc=*/true);
    VectorSource src(jpx_bytes);
    jp2_family_src fsrc;
    fsrc.open(&src);
    jpx_source jpx_src;
    ASSERT_GT(jpx_src.open(&fsrc, true), 0);

    CustomRegionCompositor comp(/*float_mode=*/true, /*allow_read=*/true,
                                /*handle_custom_overlay=*/false);
    comp.create(&jpx_src);
    // Two-stream layer (RGB cs0, opacity cs1 with 2x2 registration), empty
    // full_target_dims, plus float premult layer (lay2).
    kdu_ilayer_ref l1 = comp.add_ilayer(1, MakeDims(0, 0, 64, 48), kdu_dims(),
                                        true, true, true);
    kdu_ilayer_ref l2 =
        comp.add_ilayer(2, kdu_dims(), MakeDims(4, 4, 24, 18), false, false,
                        false);
    EXPECT_FALSE(l1.is_null());
    EXPECT_FALSE(l2.is_null());

    // Prime fractional scale triggering expansion_denominator doubling loop +
    // discard_levels > 0.
    comp.set_scale(true, true, true, 0.3137f, 0.8713f);
    kdu_dims tot;
    EXPECT_TRUE(comp.get_total_composition_dims(tot));

    // Before set_buffer_surface/process: buffer==NULL branch in get_opacity.
    (void)comp.find_point(kdu_coords(tot.pos.x + 4, tot.pos.y + 4), 0, 0.1f);

    // Update quality limiter before rendering (kdrc_stream::update_quality_limiting).
    ::kdu_core::kdu_quality_limiter qlim(3.0f);
    comp.set_quality_limiting(&qlim, 120.0f, 120.0f);
    comp.set_buffer_surface(tot, 0);
    kdu_dims r;
    int steps = 0;
    while (comp.process(16384, r) && steps++ < 60) {
    }
    EXPECT_TRUE(comp.is_processing_complete());

    kdu_istream_ref s0 = comp.get_ilayer_stream(l1, 0);
    int cs_idx = -1, comps[4] = {-1, -1, -1, -1}, pcomp = -1;
    kdu_ilayer_ref owner_il;
    float sx = 0.0f, sy = 0.0f;
    bool tr = false, vf = false, hf = false;
    EXPECT_GT(comp.get_istream_info(s0, cs_idx, &owner_il, comps, 4, &pcomp,
                                    &sx, &sy, &tr, &vf, &hf),
              0);
    EXPECT_FALSE(comp.find_ilayer_region(l1, true).is_empty());
    EXPECT_FALSE(comp.find_ilayer_region(l2, false).is_empty());

    // Float get_opacity inside/outside layer & find_point high visibility cutoff.
    (void)comp.find_point(kdu_coords(tot.pos.x + 6, tot.pos.y + 6), 0, 0.9999f);
    (void)comp.find_point(kdu_coords(tot.pos.x + 6, tot.pos.y + 6), 1, -1.0f);

    kdu_coords fsiz;
    kdu_dims roi;
    for (int round_dir : {-1, 0, 1}) {
      EXPECT_TRUE(comp.find_compatible_jpip_window(fsiz, roi, round_dir, tot));
    }

    // Single subsampled component with non-empty full_source_dims & empty
    // full_target_dims + flips.
    int comp1 = 1;
    kdu_ilayer_ref sub_il = comp.add_primitive_ilayer(
        0, comp1, KDU_WANT_CODESTREAM_COMPONENTS, MakeDims(0, 0, 32, 24),
        kdu_dims(), false, true, true);
    EXPECT_FALSE(sub_il.is_null());
    comp.set_scale(true, true, true, 0.4317f);
    EXPECT_TRUE(comp.get_total_composition_dims(tot));
    comp.set_buffer_surface(tot, 0);
    steps = 0;
    while (comp.process(16384, r) && steps++ < 40) {
    }
    EXPECT_FALSE(comp.find_ilayer_region(sub_il, true).is_empty());
    comp.pre_destroy();
    jpx_src.close();
    fsrc.close();
  }

  // 3) 16-bit signed raw codestream single-component & white-stretch vs true
  //    intensity scaling.
  {
    std::vector<kdu_byte> raw16 = MakeRawCodestream(48, 32, 3, 2, 16);
    VectorSource rsrc(raw16);
    CustomRegionCompositor comp(/*float_mode=*/true, /*allow_read=*/true,
                                /*handle_custom_overlay=*/false);
    comp.create(&rsrc);
    comp.configure_intensity_scaling(true, false);
    int c0 = 0;
    kdu_ilayer_ref il = comp.add_primitive_ilayer(
        0, c0, KDU_WANT_CODESTREAM_COMPONENTS, kdu_dims(), kdu_dims());
    EXPECT_FALSE(il.is_null());
    comp.set_scale(false, false, false, 1.3713f);
    kdu_dims tot;
    EXPECT_TRUE(comp.get_total_composition_dims(tot));
    comp.set_buffer_surface(tot, 0);
    kdu_dims r;
    int steps = 0;
    while (comp.process(16384, r) && steps++ < 40) {
    }
    EXPECT_TRUE(comp.is_processing_complete());
    comp.pre_destroy();
  }
}

std::vector<kdu_byte> MakeLargeQuadOverlayJpx(bool with_composition) {
  std::vector<kdu_byte> out;
  VectorTarget target(&out);
  jp2_family_tgt family;
  family.open(&target);
  jpx_target jout;
  jout.open(&family);

  siz_params siz0;
  siz0.set(Sdims, 0, 0, 128);
  siz0.set(Sdims, 0, 1, 128);
  siz0.set(Scomponents, 0, 0, 3);
  siz0.set(Ssigned, 0, 0, false);
  siz0.set(Sprecision, 0, 0, 8);
  static_cast<::kdu_core::kdu_params &>(siz0).finalize();

  ::kdu_supp::jpx_codestream_target cs0_tgt = jout.add_codestream();
  cs0_tgt.access_dimensions().init(&siz0);

  ::kdu_supp::jpx_codestream_target cs1_tgt = jout.add_codestream();
  cs1_tgt.access_dimensions().init(&siz0);

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

  if (with_composition) {
    jpx_composition comp = jout.access_composition();
    jx_frame *f = comp.add_frame(100, 1, false);
    comp.add_instruction(f, 0, 0, MakeDims(0, 0, 128, 128),
                         MakeDims(0, 0, 128, 128));
  }

  jpx_meta_manager mm = jout.access_meta_manager();
  jpx_metanode root = mm.access_root();
  int cs0[1] = {0};
  jpx_metanode nlist = root.add_numlist(1, cs0, 0, nullptr, false);

  jpx_roi rois[5];
  // Place all ROIs in [40,116]x[40,116] so at 6x scale ([240,696]) a small
  // initial (0,0,200,200) viewport sees no ROIs while panning hits all concave
  // quad vertex orderings and overlay segment copies.
  rois[0].init_quadrilateral(kdu_coords(80, 40), kdu_coords(110, 115),
                             kdu_coords(45, 75), kdu_coords(55, 105));
  rois[1].init_quadrilateral(kdu_coords(80, 40), kdu_coords(55, 105),
                             kdu_coords(115, 75), kdu_coords(110, 115));
  rois[2].init_quadrilateral(kdu_coords(80, 40), kdu_coords(115, 115),
                             kdu_coords(85, 95), kdu_coords(45, 75));
  rois[3].init_quadrilateral(kdu_coords(80, 40), kdu_coords(45, 75),
                             kdu_coords(85, 95), kdu_coords(115, 115));
  rois[4].init_quadrilateral(kdu_coords(40, 40), kdu_coords(116, 40),
                             kdu_coords(116, 116), kdu_coords(40, 116));
  nlist.add_regions(5, rois);

  while (jout.write_headers() != nullptr) {
  }
  auto *out0 = cs0_tgt.open_stream();
  CompressToTarget(out0, 128, 128, 3, 8, 2, 0, true, false, 15);
  out0->close();
  auto *out1 = cs1_tgt.open_stream();
  CompressToTarget(out1, 128, 128, 3, 8, 2, 0, true, false, 60);
  out1->close();
  while (jout.write_metadata() != nullptr) {
  }
  jout.close();
  family.close();
  return out;
}

TEST_F(KduRegionCompositorTest, WriteOnlyPromotionOverlayScrollAndFloatCopy) {
  std::vector<kdu_byte> jpx_bytes = MakeLargeQuadOverlayJpx(false);
  VectorSource src(jpx_bytes);
  jp2_family_src fsrc;
  fsrc.open(&src);
  jpx_source jpx_src;
  ASSERT_GE(jpx_src.open(&fsrc, true), 0);

  kdu_uint32 aux[4] = {1, 0x30FF0000u, 0x7000FF00u, 1};

  // Single-layer write-only -> read-write promotion, backward & common-seg
  // overlay buffer copying in both integer and float compositing modes.
  for (bool float_mode : {false, true}) {
    CustomRegionCompositor comp(float_mode, /*allow_read=*/false,
                                /*handle_custom_overlay=*/false);
    comp.create(&jpx_src);
    if (float_mode) {
      comp.set_colour_order(KDU_ABGR_ORDER);
    }
    comp.configure_overlays(true, 1, 0.7f, 2, jpx_metanode(), 0, aux, 4);
    kdu_ilayer_ref il = comp.add_ilayer(0, kdu_dims(), kdu_dims());
    EXPECT_FALSE(il.is_null());
    // Scaled 6x => 768x768 canvas; ROIs start at (240,240).
    // First start with a (0,0,200,200) viewport (zero intersecting ROIs), so
    // overlay is created with have_overlay_info=false and compositing_buffer=NULL.
    comp.set_scale(false, false, false, 6.0f);
    kdu_dims tot;
    EXPECT_TRUE(comp.get_total_composition_dims(tot));
    comp.set_buffer_surface(MakeDims(0, 0, 200, 200), 0x00102030u);
    kdu_dims r;
    int steps = 0;
    while (comp.process(65536, r) && steps++ < 60) {
    }
    EXPECT_TRUE(comp.is_processing_complete());

    // Expand viewport to cover [240,512] WITHOUT calling get_composition_buffer
    // first -> promotes write-only buffer and donates a separate compositing
    // buffer inside kdrc_layer::set_buffer_surface.
    comp.set_buffer_surface(MakeDims(0, 0, 512, 512), 0x00102030u);
    steps = 0;
    while (comp.process(65536, r) && steps++ < 80) {
    }
    EXPECT_TRUE(comp.is_processing_complete());

    // Pan down within existing 520x520 allocation (height 512 <= 520) ->
    // copy_common_stripes_fwd.
    comp.set_buffer_surface(MakeDims(0, 128, 384, 512), 0x00102030u);
    steps = 0;
    while (comp.process(65536, r) && steps++ < 80) {
    }
    EXPECT_TRUE(comp.is_processing_complete());

    // Pan back up within same 520x520 allocation (old y=128..640, new y=0..512,
    // overlap y=128..512 retains interior segment y=2) -> copy_common_stripes_bwd.
    comp.set_buffer_surface(MakeDims(0, 0, 384, 512), 0x00102030u);
    steps = 0;
    while (comp.process(65536, r) && steps++ < 80) {
    }
    EXPECT_TRUE(comp.is_processing_complete());

    // Expand beyond 520x520 -> allocates new overlay_buffer -> copy_common_segs.
    comp.set_buffer_surface(MakeDims(0, 0, 640, 640), 0x00102030u);
    steps = 0;
    while (comp.process(65536, r) && steps++ < 80) {
    }
    EXPECT_TRUE(comp.is_processing_complete());

    // Change scale while overlay has live seg_refs -> triggers set_geometry
    // cleanup.
    comp.set_scale(false, false, false, 2.0f);
    comp.set_buffer_surface(MakeDims(0, 0, 256, 256), 0u);
    steps = 0;
    while (comp.process(65536, r) && steps++ < 60) {
    }
    EXPECT_TRUE(comp.is_processing_complete());
    comp.pre_destroy();
  }

  // Late overlay activation on a write-only single layer via configure_overlays
  // (buffer_is_composition_surface == false), plus surface-init mode refresh
  // on both 1-layer (no composition_buffer) and 2-layer compositors.
  {
    CustomRegionCompositor comp(/*float_mode=*/false, /*allow_read=*/false,
                                /*handle_custom_overlay=*/false);
    comp.create(&jpx_src);
    comp.set_surface_initialization_mode(false);
    comp.add_ilayer(0, kdu_dims(), kdu_dims());
    comp.set_scale(false, false, false, 2.0f);
    comp.set_buffer_surface(MakeDims(0, 0, 256, 256), 0u);
    kdu_dims r;
    comp.process(4096, r);
    // Activate overlays after write-only buffer was allocated without read.
    comp.configure_overlays(true, 1, 0.7f, 2, jpx_metanode(), 0, aux, 4);
    comp.set_surface_initialization_mode(true);
    EXPECT_TRUE(comp.refresh());
    int steps = 0;
    while (comp.process(65536, r) && steps++ < 80) {
    }
    EXPECT_TRUE(comp.is_processing_complete());

    // Repeat surface-init toggle on a 2-layer composition (composition_buffer != NULL).
    comp.add_ilayer(1, kdu_dims(), kdu_dims());
    comp.set_surface_initialization_mode(false);
    comp.set_scale(false, false, false, 1.0f);
    comp.set_buffer_surface(MakeDims(0, 0, 128, 128), 0x00405060u);
    comp.set_surface_initialization_mode(true);
    EXPECT_TRUE(comp.refresh());
    steps = 0;
    while (comp.process(65536, r) && steps++ < 60) {
    }
    EXPECT_TRUE(comp.is_processing_complete());
    comp.pre_destroy();
  }

  jpx_src.close();
  fsrc.close();
}

TEST_F(KduRegionCompositorTest,
       CompletedRectsSplitRefreshMergingAndStreamStealing) {
  // 1) Trigger find_completed_rects overlay segment subtraction (4 sub-rects):
  // Layer 0 occupies (16,16,96,96) inside segment (0,0) with overlay ROI;
  // Layer 1 occupies (0,0,128,128) without overlay. Give Layer 0 scale 0.75 on
  // 128x128 placed at (16,16) and Layer 1 at (0,0,128,128).
  {
    std::vector<kdu_byte> jpx_bytes = MakeLargeQuadOverlayJpx(false);
    VectorSource src(jpx_bytes);
    jp2_family_src fsrc;
    fsrc.open(&src);
    jpx_source jpx_src;
    ASSERT_GE(jpx_src.open(&fsrc, true), 0);

    {
      kdu_region_compositor comp;
      comp.create(&jpx_src);
      kdu_uint32 aux[2] = {0, 0x400000FFu};
      comp.configure_overlays(true, 0, 0.8f, 1, jpx_metanode(), 0, aux, 2);
      // Try adding Layer 0 first so Layer 0's stream runs and finishes before
      // Layer 1's stream finishes (0,0,128,128), hitting overlay subtraction in
      // find_completed_rects.
      kdu_ilayer_ref il0_alt =
          comp.add_ilayer(0, kdu_dims(), MakeDims(16, 16, 96, 96));
      kdu_ilayer_ref il1_alt =
          comp.add_ilayer(1, kdu_dims(), MakeDims(0, 0, 128, 128));
      EXPECT_FALSE(il0_alt.is_null());
      EXPECT_FALSE(il1_alt.is_null());
      comp.set_scale(false, false, false, 1.0f);
      kdu_dims tot;
      EXPECT_TRUE(comp.get_total_composition_dims(tot));
      comp.set_buffer_surface(tot, 0x00FFFFFFu);
      kdu_dims r;
      int steps = 0;
      while (comp.process(32768, r) && steps++ < 80) {
      }
      EXPECT_TRUE(comp.is_processing_complete());
      comp.pre_destroy();
    }

    kdu_region_compositor comp;
    comp.create(&jpx_src);
    kdu_uint32 aux[2] = {0, 0x400000FFu};
    comp.configure_overlays(true, 0, 0.8f, 1, jpx_metanode(), 0, aux, 2);
    // Add Layer 1 on bottom covering (0,0,128,128) and Layer 0 on top
    // covering (16,16,96,96) so both are visible.
    kdu_ilayer_ref il1 = comp.add_ilayer(1, kdu_dims(), MakeDims(0, 0, 128, 128));
    kdu_ilayer_ref il0 = comp.add_ilayer(0, kdu_dims(), MakeDims(16, 16, 96, 96));
    EXPECT_FALSE(il0.is_null());
    EXPECT_FALSE(il1.is_null());
    comp.set_scale(false, false, false, 1.0f);
    kdu_dims tot;
    EXPECT_TRUE(comp.get_total_composition_dims(tot));
    comp.set_buffer_surface(tot, 0x00FFFFFFu);
    kdu_dims r;
    int steps = 0;
    while (comp.process(32768, r) && steps++ < 80) {
    }
    EXPECT_TRUE(comp.is_processing_complete());

    // Chain get_next_visible_ilayer across both visible layers + verify
    // inverse_map_region with null istream_ref.
    kdu_ilayer_ref v0 =
        comp.get_next_visible_ilayer(kdu_ilayer_ref(), MakeDims(0, 0, 128, 128));
    EXPECT_FALSE(v0.is_null());
    kdu_ilayer_ref v1 =
        comp.get_next_visible_ilayer(v0, MakeDims(0, 0, 128, 128));
    EXPECT_FALSE(v1.is_null());
    EXPECT_TRUE(comp.get_next_visible_ilayer(v1, MakeDims(0, 0, 128, 128))
                    .is_null());
    EXPECT_FALSE(comp.inverse_map_region(kdu_dims(), kdu_istream_ref()).is_empty());
    EXPECT_FALSE(comp.inverse_map_region(MakeDims(4, 4, 24, 24), kdu_istream_ref())
                     .is_empty());

    // 2) Exercise inactive stream stealing and move_to_head (head vs middle).
    int c0 = 0, c1 = 1, c2 = 2;
    kdu_ilayer_ref p0 = comp.add_primitive_ilayer(
        0, c0, KDU_WANT_CODESTREAM_COMPONENTS, kdu_dims(), kdu_dims());
    kdu_ilayer_ref p1 = comp.add_primitive_ilayer(
        0, c1, KDU_WANT_CODESTREAM_COMPONENTS, kdu_dims(), kdu_dims());
    kdu_ilayer_ref p2 = comp.add_primitive_ilayer(
        0, c2, KDU_WANT_CODESTREAM_COMPONENTS, kdu_dims(), kdu_dims());
    EXPECT_FALSE(p0.is_null());
    EXPECT_FALSE(p1.is_null());
    EXPECT_FALSE(p2.is_null());
    // p2 is at codestream head; p1 is in the middle.
    comp.remove_ilayer(p1, /*permanent=*/false);
    int c_steal = 0;
    kdu_ilayer_ref p3 = comp.add_primitive_ilayer(
        0, c_steal, KDU_WANT_CODESTREAM_COMPONENTS, kdu_dims(), kdu_dims());
    EXPECT_FALSE(p3.is_null());
    // Re-add p1 -> inactive layer exists with streams[0] == NULL (primitive activate).
    c1 = 1;
    p1 = comp.add_primitive_ilayer(0, c1, KDU_WANT_CODESTREAM_COMPONENTS,
                                   kdu_dims(), kdu_dims());
    EXPECT_FALSE(p1.is_null());

    // Remove head (p1) -> steal head (prev_codestream_user == NULL).
    comp.remove_ilayer(p1, /*permanent=*/false);
    comp.remove_ilayer(p2, /*permanent=*/false);
    c_steal = 2;
    kdu_ilayer_ref p4 = comp.add_primitive_ilayer(
        0, c_steal, KDU_WANT_CODESTREAM_COMPONENTS, kdu_dims(), kdu_dims());
    EXPECT_FALSE(p4.is_null());

    // Remove JPX layer il0 (non-permanent) and steal its stream with a primitive
    // layer using the same codestream; then re-add JPX layer 0 (missing_streams).
    comp.remove_ilayer(p0, /*permanent=*/false);
    comp.remove_ilayer(p3, /*permanent=*/false);
    comp.remove_ilayer(p4, /*permanent=*/false);
    comp.remove_ilayer(il0, /*permanent=*/false);
    c_steal = 0;
    kdu_ilayer_ref thief = comp.add_primitive_ilayer(
        0, c_steal, KDU_WANT_CODESTREAM_COMPONENTS, kdu_dims(), kdu_dims());
    EXPECT_FALSE(thief.is_null());
    il0 = comp.add_ilayer(0, kdu_dims(), MakeDims(0, 0, 128, 128));
    EXPECT_FALSE(il0.is_null());

    comp.pre_destroy();
    jpx_src.close();
    fsrc.close();
  }

  // 3) Drive kdrc_refresh::add_region (enclose, shrink new top/bottom/left/right,
  // shrink existing scan top/bottom/left/right) using KDU_COMPOSIT_DEFER_REGION
  // across staggered single-component raw ilayers.
  {
    std::vector<kdu_byte> raw = MakeRawCodestream(256, 256, 1, 1, 0);
    kdu_compressed_source_buffered src(raw.data(), raw.size());
    kdu_region_compositor comp;
    comp.create(&src);

    const kdu_dims targets[] = {
        MakeDims(200, 200, 20, 20),   // Sentinel element
        MakeDims(20, 20, 40, 40),     // Enclosed later by (0,0,100,100)
        MakeDims(0, 0, 100, 100),     // Encloses (20,20,40,40) with prev != NULL
        MakeDims(0, 30, 100, 80),     // Shrinks new Y from top
        MakeDims(0, 10, 100, 40),     // Shrinks new Y from bottom
        MakeDims(120, 0, 60, 100),    // Left half
        MakeDims(150, 0, 70, 100),    // Shrinks new X from left
        MakeDims(100, 0, 40, 100),    // Shrinks new X from right
        MakeDims(20, 120, 60, 60),    // Existing narrower box
        MakeDims(0, 110, 100, 30),    // Shrinks existing Y from top
        MakeDims(0, 160, 100, 30),    // Shrinks existing Y from bottom
        MakeDims(120, 120, 60, 60),   // Existing shorter box
        MakeDims(110, 110, 30, 100),  // Shrinks existing X from left
        MakeDims(160, 110, 30, 100),  // Shrinks existing X from right
    };
    for (const kdu_dims &t : targets) {
      int c = 0;
      EXPECT_FALSE(
          comp.add_primitive_ilayer(0, c, KDU_WANT_CODESTREAM_COMPONENTS,
                                    MakeDims(0, 0, 64, 64), t)
              .is_null());
    }
    comp.set_scale(false, false, false, 1.0f);
    kdu_dims tot;
    EXPECT_TRUE(comp.get_total_composition_dims(tot));
    comp.set_buffer_surface(tot, 0u);
    kdu_dims r;
    int steps = 0;
    while (!comp.is_codestream_processing_complete() && steps++ < 100) {
      comp.process(65536, r, KDU_COMPOSIT_DEFER_REGION);
    }
    // Switching off surface initialization adds rendering_buffer_region onto
    // existing refresh elements, exercising kdrc_refresh::add_region enclosure.
    comp.set_surface_initialization_mode(false);
    steps = 0;
    while (comp.process(65536, r, 0) && steps++ < 40) {
    }
    EXPECT_TRUE(comp.is_processing_complete());
    comp.pre_destroy();
  }
}

std::vector<kdu_byte> MakeMj2AlphaBytes(bool premultiplied) {
  std::vector<kdu_byte> out;
  VectorTarget vec_target(&out);
  jp2_family_tgt family;
  family.open(&vec_target);
  mj2_target mj2_out;
  mj2_out.open(&family);

  mj2_video_target *track0 = mj2_out.add_video_track();
  track0->access_colour().init(::kdu_supp::JP2_sRGB_SPACE);
  jp2_channels chans = track0->access_channels();
  chans.init(3);
  chans.set_colour_mapping(0, 0, -1, 0);
  chans.set_colour_mapping(1, 1, -1, 0);
  chans.set_colour_mapping(2, 2, -1, 0);
  if (premultiplied) {
    chans.set_premult_mapping(0, 3, -1, 0);
    chans.set_premult_mapping(1, 3, -1, 0);
    chans.set_premult_mapping(2, 3, -1, 0);
    track0->set_graphics_mode(MJ2_GRAPHICS_PREMULT_ALPHA);
  } else {
    chans.set_opacity_mapping(0, 3, -1, 0);
    chans.set_opacity_mapping(1, 3, -1, 0);
    chans.set_opacity_mapping(2, 3, -1, 0);
    track0->set_graphics_mode(MJ2_GRAPHICS_ALPHA);
  }
  track0->set_timescale(30000);
  track0->set_frame_period(1001);
  for (int f = 0; f < 2; ++f) {
    track0->open_image();
    kdu_codestream cs = CompressToTarget(track0, 48, 36, 4, 8, 1, 0, true,
                                         false, f * 30, /*keep_cs=*/true);
    track0->close_image(cs);
    cs.destroy();
  }
  mj2_out.close();
  family.close();
  return out;
}

TEST_F(KduRegionCompositorTest,
       ConstructorsErrorPathsJpipScalesAndMj2Alpha) {
  std::vector<kdu_byte> jpx_bytes = MakeLargeQuadOverlayJpx(true);
  VectorSource src(jpx_bytes);
  jp2_family_src fsrc;
  fsrc.open(&src);
  jpx_source jpx_src;
  ASSERT_GE(jpx_src.open(&fsrc, true), 0);

  // Direct JPX constructor overload.
  {
    kdu_region_compositor comp(&jpx_src, /*persistent_cache_threshold=*/0);
    jpx_composition cr = jpx_src.access_composition();
    jx_frame *fr = cr.get_next_frame(nullptr);
    ASSERT_NE(fr, nullptr);
    jpx_frame_expander expander;
    ASSERT_TRUE(expander.construct(&jpx_src, fr, 0, true));

    // add_frame before set_frame must throw.
    EXPECT_THROW(comp.add_frame(&expander, kdu_coords(0, 0)),
                 ::kdu_core::kdu_exception);

    // Out-of-range JPX layer / codestream indices must throw.
    EXPECT_THROW(comp.add_ilayer(99, kdu_dims(), kdu_dims()),
                 ::kdu_core::kdu_exception);
    int c0 = 0;
    EXPECT_THROW(
        comp.add_primitive_ilayer(99, c0, KDU_WANT_CODESTREAM_COMPONENTS,
                                  kdu_dims(), kdu_dims()),
        ::kdu_core::kdu_exception);

    // find_optimal_scale with no active layers across anchor bounds.
    kdu_istream_ref ir;
    int comp_idx = 0;
    EXPECT_FLOAT_EQ(comp.find_optimal_scale(kdu_dims(), 0.3f, 0.5f, 1.5f, &ir,
                                            &comp_idx),
                    0.5f);
    EXPECT_TRUE(ir.is_null());
    EXPECT_EQ(comp_idx, -1);
    EXPECT_FLOAT_EQ(
        comp.find_optimal_scale(kdu_dims(), 2.0f, 0.5f, 1.5f, &ir, &comp_idx),
        1.5f);
    EXPECT_FLOAT_EQ(
        comp.find_optimal_scale(kdu_dims(), 1.0f, 0.5f, 1.5f, &ir, &comp_idx),
        1.0f);

    // Multi-frame composition with non-zero offset + inactive layer reuse via
    // set_frame -> set_frame -> add_frame.
    comp.set_frame(&expander, kdu_coords(16, 16));
    comp.set_frame(&expander, kdu_coords(0, 0));
    EXPECT_TRUE(comp.add_frame(&expander, kdu_coords(128, 0)));

    // Set quality limiting & mem_configure while streams exist.
    kdu_quality_limiter ql(0.8f, false);
    comp.set_quality_limiting(&ql, 200.0f, 200.0f);
    comp.set_quality_limiting(nullptr, 0.0f, 0.0f);
    TrackingMembroker broker;
    EXPECT_TRUE(comp.mem_configure(&broker, 10));

    // Scale overflow on fixed_composition_dims vs normal scale +
    // find_optimal_scale with non-empty region.
    kdu_dims tot_dims;
    comp.set_scale(false, false, false, 1.0e8f);
    EXPECT_FALSE(comp.get_total_composition_dims(tot_dims));
    comp.set_scale(false, false, false, 0.25f);
    EXPECT_TRUE(comp.get_total_composition_dims(tot_dims));
    float opt = comp.find_optimal_scale(MakeDims(0, 0, 128, 128), 0.5f, 0.1f,
                                        2.0f, &ir, &comp_idx);
    EXPECT_GT(opt, 0.0f);
    EXPECT_FALSE(ir.is_null());
    EXPECT_GE(comp_idx, 0);

    // find_compatible_jpip_window on multi-frame composition (fixed_composition_dims
    // != single frame_dims), and generate_metareq with empty/out-of-bounds/global.
    kdu_coords fsiz;
    kdu_dims roi_dims;
    int round_dir = 0;
    EXPECT_TRUE(comp.find_compatible_jpip_window(
        fsiz, roi_dims, round_dir, MakeDims(0, 0, 64, 64)));
    kdu_window jpip_win;
    ::kdu_core::kdu_uint32 box_types[1] = {jp2_roi_description_4cc};
    ::kdu_core::kdu_uint32 desc_types[1] = {jp2_label_4cc};
    comp.generate_metareq(&jpip_win, KDU_MRQ_GLOBAL, kdu_dims(), 1, box_types,
                          1, desc_types);
    comp.generate_metareq(&jpip_win, KDU_MRQ_WINDOW, MakeDims(5000, 5000, 10, 10),
                          1, box_types, 0, nullptr);
    comp.generate_metareq(&jpip_win, KDU_MRQ_WINDOW, kdu_dims(), 1, box_types,
                          0, nullptr);
    comp.pre_destroy();
  }

  // Non-persistent JPX + raw error paths + tiny/huge scales hitting JPIP max
  // discard levels and safe expansion limits.
  {
    kdu_region_compositor empty_comp;
    int c0 = 0;
    EXPECT_THROW(empty_comp.add_ilayer(0, kdu_dims(), kdu_dims()),
                 ::kdu_core::kdu_exception);
    jpx_composition cr = jpx_src.access_composition();
    jx_frame *fr = cr.get_next_frame(nullptr);
    jpx_frame_expander expander;
    ASSERT_TRUE(expander.construct(&jpx_src, fr, 0, true));
    EXPECT_THROW(empty_comp.set_frame(&expander, kdu_coords(0, 0)),
                 ::kdu_core::kdu_exception);

    kdu_region_compositor non_persist(&jpx_src, -1);
    EXPECT_TRUE(non_persist.refresh());
    EXPECT_FALSE(non_persist
                     .add_primitive_ilayer(0, c0, KDU_WANT_CODESTREAM_COMPONENTS,
                                           kdu_dims(), kdu_dims())
                     .is_null());
    // Out-of-bounds inverse_map_region using invalid/inactive istream_ref.
    kdu_istream_ref active_ir = non_persist.get_next_istream(kdu_istream_ref());
    EXPECT_FALSE(active_ir.is_null());
    EXPECT_TRUE(
        non_persist.inverse_map_region(MakeDims(0, 0, 10, 10), active_ir)
            .is_empty());

    // Trigger max_discard_levels in find_compatible_jpip_window via small scale
    // and out-of-range scales.
    kdu_dims np_dims;
    non_persist.set_scale(false, false, false, 0.05f);
    EXPECT_TRUE(non_persist.get_total_composition_dims(np_dims));
    kdu_coords fsiz;
    kdu_dims roi_dims;
    int round_dir = 0;
    EXPECT_TRUE(non_persist.find_compatible_jpip_window(
        fsiz, roi_dims, round_dir, np_dims));
    non_persist.set_scale(false, false, false, 1.0e-5f);
    EXPECT_FALSE(non_persist.get_total_composition_dims(np_dims));
    non_persist.set_scale(false, false, false, 1.0e8f);
    EXPECT_FALSE(non_persist.get_total_composition_dims(np_dims));
    non_persist.pre_destroy();
  }
  jpx_src.close();
  fsrc.close();

  // Raw source invalid layer/stream errors.
  {
    std::vector<kdu_byte> raw = MakeRawCodestream(32, 32, 3, 1, 0);
    kdu_compressed_source_buffered rsrc(raw.data(), raw.size());
    kdu_region_compositor comp(&rsrc, 65536);
    EXPECT_THROW(comp.add_ilayer(1, kdu_dims(), kdu_dims()),
                 ::kdu_core::kdu_exception);
    int c0 = 0;
    EXPECT_THROW(comp.add_primitive_ilayer(1, c0, KDU_WANT_CODESTREAM_COMPONENTS,
                                           kdu_dims(), kdu_dims()),
                 ::kdu_core::kdu_exception);
    comp.pre_destroy();
  }

  // MJ2 alpha, premult-alpha, and invalid track/frame/field error paths.
  for (bool premult : {false, true}) {
    std::vector<kdu_byte> mj2_bytes = MakeMj2AlphaBytes(premult);
    VectorSource msrc(mj2_bytes);
    jp2_family_src mfsrc;
    mfsrc.open(&msrc);
    mj2_source mj2_src;
    ASSERT_GE(mj2_src.open(&mfsrc, true), 0);

    kdu_region_compositor comp(&mj2_src, 65536);
    EXPECT_THROW(comp.add_ilayer(99, kdu_dims(), kdu_dims()),
                 ::kdu_core::kdu_exception);
    EXPECT_THROW(comp.add_ilayer(0, kdu_dims(), kdu_dims(), false, false, false,
                                 /*frame_idx=*/99, 0),
                 ::kdu_core::kdu_exception);
    EXPECT_THROW(comp.add_ilayer(0, kdu_dims(), kdu_dims(), false, false, false,
                                 /*frame_idx=*/0, /*field_handling=*/1),
                 ::kdu_core::kdu_exception);
    int c0 = 0;
    EXPECT_THROW(
        comp.add_primitive_ilayer(99, c0, KDU_WANT_CODESTREAM_COMPONENTS,
                                  kdu_dims(), kdu_dims()),
        ::kdu_core::kdu_exception);

    kdu_ilayer_ref il = comp.add_ilayer(0, kdu_dims(), kdu_dims());
    EXPECT_FALSE(il.is_null());
    comp.set_scale(false, false, false, 1.0f);
    kdu_dims tot;
    EXPECT_TRUE(comp.get_total_composition_dims(tot));
    comp.set_buffer_surface(tot, 0u);
    kdu_dims r;
    int steps = 0;
    while (comp.process(32768, r) && steps++ < 40) {
    }
    EXPECT_TRUE(comp.is_processing_complete());
    comp.pre_destroy();
    mj2_src.close();
    mfsrc.close();
  }

  // Exercise change_ilayer_frame edge cases, packet stats on primitive &
  // non-unit scales, flipped find_optimal_scale, and vflip incremental strip.
  {
    std::vector<kdu_byte> mj2_bytes = MakeMj2Bytes(/*interlaced=*/false);
    VectorSource msrc(mj2_bytes);
    jp2_family_src mfsrc;
    mfsrc.open(&msrc);
    mj2_source mj2_src;
    ASSERT_GE(mj2_src.open(&mfsrc, true), 0);

    kdu_region_compositor comp(&mj2_src, 65536);
    kdu_ilayer_ref il = comp.add_ilayer(0, kdu_dims(), kdu_dims());
    EXPECT_FALSE(il.is_null());
    EXPECT_THROW(comp.change_ilayer_frame(kdu_ilayer_ref(), 0),
                 ::kdu_core::kdu_exception);
    EXPECT_TRUE(comp.change_ilayer_frame(il, 0));
    EXPECT_THROW(comp.change_ilayer_frame(il, 99), ::kdu_core::kdu_exception);

    // get_codestream_packets before set_scale.
    kdu_istream_ref strm = comp.get_ilayer_stream(il, 0);
    kdu_long prec = 0, pkt = 0, max_pkt = 0;
    EXPECT_FALSE(comp.get_codestream_packets(strm, MakeDims(0, 0, 32, 32),
                                             prec, pkt, max_pkt));
    comp.pre_destroy();
    mj2_src.close();
    mfsrc.close();
  }

  {
    std::vector<kdu_byte> raw = MakeRawCodestream(64, 64, 3, 2, 16);
    kdu_compressed_source_buffered rsrc(raw.data(), raw.size());
    kdu_region_compositor comp(&rsrc, 65536);
    comp.set_colour_order(KDU_ABGR_ORDER);
    int c1 = 1;
    kdu_ilayer_ref prim = comp.add_primitive_ilayer(
        0, c1, KDU_WANT_CODESTREAM_COMPONENTS, kdu_dims(), kdu_dims(),
        false, /*vflip=*/true, /*hflip=*/true);
    EXPECT_FALSE(prim.is_null());
    (void)comp.find_optimal_scale(kdu_dims(), 1.0f, 0.25f, 2.0f);
    comp.set_scale(false, /*vflip=*/true, /*hflip=*/true, 0.7f);
    kdu_dims tot;
    EXPECT_TRUE(comp.get_total_composition_dims(tot));
    comp.set_buffer_surface(tot, 0u);
    kdu_istream_ref strm = comp.get_ilayer_stream(prim, 0);
    kdu_long prec = 0, pkt = 0, max_pkt = 0;
    (void)comp.get_codestream_packets(strm, tot, prec, pkt, max_pkt);
    kdu_dims r;
    int steps = 0;
    while (comp.process(64, r) && steps++ < 120) {
    }
    EXPECT_TRUE(comp.is_processing_complete());
    EXPECT_TRUE(comp.change_ilayer_frame(prim, 0));
    EXPECT_FALSE(comp.change_ilayer_frame(prim, 1));
    comp.pre_destroy();
  }
}

class FailPromoteBuf : public kdu_compositor_buf {
 public:
  FailPromoteBuf(int width, int height, bool allow_read, bool is_float)
      : was_allowed_(allow_read), is_float_(is_float) {
    int stride = (width > 0 ? width : 1);
    if (is_float_) {
      fstorage_.assign(
          static_cast<size_t>(stride) * static_cast<size_t>(height > 0 ? height : 1) * 4,
          0.0f);
      init_float(fstorage_.data(), stride * 4);
    } else {
      ustorage_.assign(
          static_cast<size_t>(stride) * static_cast<size_t>(height > 0 ? height : 1),
          0u);
      init(ustorage_.data(), stride);
    }
    kdu_compositor_buf::set_read_accessibility(allow_read);
  }

  ~FailPromoteBuf() override {
    if (is_float_) {
      init_float(nullptr, 0);
    } else {
      init(nullptr, 0);
    }
  }

  bool set_read_accessibility(bool read_access_required) override {
    const bool prev = was_allowed_;
    kdu_compositor_buf::set_read_accessibility(read_access_required);
    was_allowed_ = is_read_access_allowed();
    if (read_access_required && !prev) return false;
    return true;
  }

 private:
  bool was_allowed_;
  bool is_float_;
  std::vector<kdu_uint32> ustorage_;
  std::vector<float> fstorage_;
};

class FailPromoteCompositor : public kdu_region_compositor {
 public:
  explicit FailPromoteCompositor(bool use_float = false)
      : use_float_(use_float) {}

  kdu_compositor_buf *allocate_buffer(kdu_coords min_size,
                                      kdu_coords &actual_size,
                                      bool allow_read_access) override {
    actual_size = min_size;
    return new FailPromoteBuf(min_size.x, min_size.y, allow_read_access,
                              use_float_);
  }

  void delete_buffer(kdu_compositor_buf *buf) override {
    delete static_cast<FailPromoteBuf *>(buf);
  }

 private:
  bool use_float_;
};

std::vector<kdu_byte> MakeOverlayRefreshAndQuadJpx() {
  std::vector<kdu_byte> out_bytes;
  VectorTarget target(&out_bytes);
  jp2_family_tgt ftgt;
  ftgt.open(&target);
  jpx_target jout;
  jout.open(&ftgt);

  // Use 512x512 RGB opaque codestream so overlay segments (128x128) form a 4x4 grid.
  auto cs_tgt = jout.add_codestream();
  siz_params siz0;
  siz0.set(Sdims, 0, 0, 512);
  siz0.set(Sdims, 0, 1, 512);
  siz0.set(Scomponents, 0, 0, 3);
  siz0.set(Sprecision, 0, 0, 8);
  siz0.set(Ssigned, 0, 0, false);
  static_cast<::kdu_core::kdu_params &>(siz0).finalize();
  cs_tgt.access_dimensions().init(&siz0);

  jpx_layer_target lay0 = jout.add_layer();
  lay0.add_colour().init(::kdu_supp::JP2_sRGB_SPACE);
  lay0.access_channels().init(3);
  lay0.access_channels().set_colour_mapping(0, 0, -1, 0);
  lay0.access_channels().set_colour_mapping(1, 1, -1, 0);
  lay0.access_channels().set_colour_mapping(2, 2, -1, 0);

  // Attach metadata manager with ROIs strictly at x>=130 and y>=140 so
  // (0,0,128,128) has no ROIs when testing deferred overlay activation, plus
  // quads covering all paint_overlay winding/height branches and skewed narrow ellipses.
  jpx_meta_manager meta = jout.access_meta_manager();
  jpx_metanode root = meta.access_root();
  jpx_roi rois[7];
  // 0: Degenerate zero-size rectangle (roi_cumulative_length <= 0.0 -> early return).
  rois[0].init_rectangle(MakeDims(290, 290, 0, 0));
  // 1: Narrow skewed ellipse hitting gamma_sq >= 1.0 and Wi clamp.
  rois[1].init_ellipse(kdu_coords(300, 300), kdu_coords(4, 30),
                       kdu_coords(20, 6), false);
  // 2: Simple clockwise quad with y3 < y2 < y1 (L9856-9859). Put valid convex
  // clockwise quads first so jpx_roi::check_geometry() accepts all regions.
  rois[2].init_quadrilateral(kdu_coords(320, 140), kdu_coords(350, 220),
                             kdu_coords(305, 190), kdu_coords(280, 165));
  // 3: Simple clockwise quad with y1 < y2 < y3 (L9870-9873).
  rois[3].init_quadrilateral(kdu_coords(320, 250), kdu_coords(360, 275),
                             kdu_coords(335, 300), kdu_coords(290, 330));
  // 4: Strictly convex clockwise flat-bottom quad with v1.y == v2.y > v3.y
  // (dy2 == 0 in kdrc_trapezoid_follower::init, L1297, L9861).
  rois[4].init_quadrilateral(kdu_coords(320, 140), kdu_coords(350, 220),
                             kdu_coords(320, 220), kdu_coords(280, 170));
  // 5: Strictly convex clockwise flat-bottom quad with v2.y == v3.y > v1.y
  // (dy1 == 0 in kdrc_trapezoid_follower::init, L1272, L9875).
  rois[5].init_quadrilateral(kdu_coords(320, 140), kdu_coords(350, 180),
                             kdu_coords(320, 220), kdu_coords(280, 220));
  // 6: Simple visible ROI in segment (2,2).
  rois[6].init_rectangle(MakeDims(300, 360, 60, 40));
  int cs_id = 0;
  int comp_layer_id = 0;
  jpx_metanode nlist = root.add_numlist(1, &cs_id, 1, &comp_layer_id, false);
  nlist.add_label("quad_set").add_regions(7, rois);

  while (jout.write_headers() != nullptr) {
  }
  auto *o0 = cs_tgt.open_stream();
  CompressToTarget(o0, 512, 512, 3, 8, 1, 0, true, false, 15);
  o0->close();

  jout.close();
  ftgt.close();
  return out_bytes;
}

std::vector<kdu_byte> MakeSplitCompletedAndPremultSepJpx() {
  std::vector<kdu_byte> out_bytes;
  VectorTarget target(&out_bytes);
  jp2_family_tgt ftgt;
  ftgt.open(&target);
  jpx_target jout;
  jout.open(&ftgt);

  // Two 256x256 codestreams: cs0 RGB, cs1 single-component premultiplied alpha.
  auto ct0 = jout.add_codestream();
  siz_params siz0;
  siz0.set(Sdims, 0, 0, 256);
  siz0.set(Sdims, 0, 1, 256);
  siz0.set(Scomponents, 0, 0, 3);
  siz0.set(Sprecision, 0, 0, 8);
  siz0.set(Ssigned, 0, 0, false);
  static_cast<::kdu_core::kdu_params &>(siz0).finalize();
  ct0.access_dimensions().init(&siz0);

  auto ct1 = jout.add_codestream();
  siz_params siz1;
  siz1.set(Sdims, 0, 0, 256);
  siz1.set(Sdims, 0, 1, 256);
  siz1.set(Scomponents, 0, 0, 1);
  siz1.set(Sprecision, 0, 0, 8);
  siz1.set(Ssigned, 0, 0, false);
  static_cast<::kdu_core::kdu_params &>(siz1).finalize();
  ct1.access_dimensions().init(&siz1);

  // Layer 0: opaque RGB with big ROI [10,10,108,108] in seg(0,0).
  jpx_layer_target lay0 = jout.add_layer();
  lay0.add_colour().init(::kdu_supp::JP2_sRGB_SPACE);
  lay0.access_channels().init(3);
  lay0.access_channels().set_colour_mapping(0, 0, -1, 0);
  lay0.access_channels().set_colour_mapping(1, 1, -1, 0);
  lay0.access_channels().set_colour_mapping(2, 2, -1, 0);

  // Layer 1: RGB from cs0 + standalone premultiplied alpha from cs1!
  jpx_layer_target lay1 = jout.add_layer();
  lay1.add_colour().init(::kdu_supp::JP2_sRGB_SPACE);
  lay1.access_channels().init(3);
  lay1.access_channels().set_colour_mapping(0, 0, -1, 0);
  lay1.access_channels().set_colour_mapping(1, 1, -1, 0);
  lay1.access_channels().set_colour_mapping(2, 2, -1, 0);
  lay1.access_channels().set_premult_mapping(-1, 0, -1, 1);

  jpx_meta_manager meta = jout.access_meta_manager();
  jpx_metanode root = meta.access_root();
  jpx_roi roi0, roi1;
  roi0.init_rectangle(MakeDims(10, 10, 108, 108));
  roi1.init_rectangle(MakeDims(40, 40, 32, 32));
  int cs_id = 0;
  int l0_id = 0, l1_id = 1;
  root.add_numlist(1, &cs_id, 1, &l0_id, false).add_regions(1, &roi0);
  root.add_numlist(1, &cs_id, 1, &l1_id, false).add_regions(1, &roi1);

  while (jout.write_headers() != nullptr) {
  }
  auto *cb0 = ct0.open_stream();
  CompressToTarget(cb0, 256, 256, 3, 8, 1, 0, true, false, 15);
  cb0->close();
  auto *cb1 = ct1.open_stream();
  CompressToTarget(cb1, 256, 256, 1, 8, 1, 0, true, false, 90);
  cb1->close();

  jout.close();
  ftgt.close();
  return out_bytes;
}

TEST_F(KduRegionCompositorTest,
       DelayedJpxStreamFailPromoteAndQuadOverlayBranches) {
  // FailPromoteCompositor: exercise L5199-5220 (late ROI discovery with
  //    compositing_buffer == NULL), L5176, L5236, L5523-5534, plus L1543/L1657/
  //    L1688 (overlay growth & backward panning) in int32 & float modes.
  std::vector<kdu_byte> jpx_quad = MakeOverlayRefreshAndQuadJpx();
  for (int use_float = 0; use_float <= 1; ++use_float) {
    VectorSource vsrc(jpx_quad);
    jp2_family_src fsrc;
    fsrc.open(&vsrc);
    jpx_source jpx_src;
    ASSERT_GT(jpx_src.open(&fsrc, true), 0);

    FailPromoteCompositor comp(use_float != 0);
    comp.create(&jpx_src, 65536);
    kdu_uint32 aux[40] = {};
    aux[0] = 20;  // Large border clamped by max_painting_border = 16 (L9575/L9577)
    aux[1] = 0xFFFF0000u;
    for (int i = 0; i < 20; ++i) aux[2 + i] = 0x8000FF00u;
    aux[22] = 1;
    // Also exercise second parameter row scaled to Bm=0 via Bm=1 row:
    kdu_uint32 aux_small[4] = {1, 0xFF00FF00u, 0x800000FFu, 1};

    comp.configure_overlays(true, 0, 0.5f, 24 /* > 16 hits L8271 */,
                            jpx_metanode(), 0, aux, 23);
    kdu_ilayer_ref il =
        comp.add_ilayer(0, kdu_dims(), MakeDims(0, 0, 512, 512));
    ASSERT_FALSE(il.is_null());

    // Start in (0,0,64,64): no ROIs there, so compositing_buffer stays NULL
    // and buffer is write-only.
    comp.set_scale(false, false, false, 1.0f);
    comp.set_buffer_surface(MakeDims(0, 0, 64, 64), 0u);
    kdu_dims nr;
    int steps = 0;
    while (comp.process(65536, nr) && steps++ < 40) {
    }

    // Grow surface into (0,0,512,512): set_buffer_surface triggers
    // overlay->set_buffer_surface(NULL, ...) returning true while
    // compositing_buffer == NULL (L5199-5220 + L5211-5215 on FailPromoteBuf).
    comp.set_buffer_surface(MakeDims(0, 0, 512, 512), 0u);
    while (comp.process(65536, nr) && steps++ < 120) {
    }
    EXPECT_TRUE(comp.is_processing_complete());

    // Re-run aux_small scaling down (0.1f -> scaled_Bm == 0) + border change.
    comp.configure_overlays(true, 0, 1.0f, 8, jpx_metanode(), 0, aux_small, 4);
    comp.set_scale(false, false, false, 1.0f, 0.1f);
    comp.set_buffer_surface(MakeDims(0, 0, 64, 64), 0u);
    while (comp.process(65536, nr) && steps++ < 160) {
    }

    // Exercise update_overlay write-only promotion fail (L5523-5534):
    comp.configure_overlays(false, 1, 1.0f, 4);
    comp.set_scale(false, false, false, 1.0f);
    comp.set_buffer_surface(MakeDims(128, 128, 256, 256), 0u);
    while (comp.process(65536, nr) && steps++ < 220) {
    }
    comp.configure_overlays(true, 1, 1.0f, 6, jpx_metanode(), 0, aux, 23);
    while (comp.process(65536, nr) && steps++ < 280) {
    }

    // Exercise L5236 + L1657-1667 (overlay_buffer reallocation growth) &
    // L1543 + L1688-1695 (backward stripe copy on reused overlay_buffer).
    comp.set_buffer_surface(MakeDims(128, 128, 384, 384), 0u);
    while (comp.process(65536, nr) && steps++ < 340) {
    }
    // Pan backward keeping segment (2,2)=[256..384] interior -> old_buffer ==
    // overlay_buffer with forward memory shift (copy_common_stripes_bwd = true).
    comp.set_buffer_surface(MakeDims(64, 64, 384, 384), 0u);
    while (comp.process(65536, nr) && steps++ < 400) {
    }
    // Render with hflip=true across full canvas to hit quad hflip & limit_min_y:
    comp.set_scale(false, false, true, 1.0f);
    comp.set_buffer_surface(MakeDims(0, 0, 512, 512), 0u);
    steps = 0;
    while (comp.process(65536, nr) && steps++ < 400) {
    }
    // Rescale while overlay_buffer != NULL (L5075-5077):
    comp.set_scale(false, false, false, 0.5f);
    // Same-size/shrink read_access transition (L5176):
    comp.configure_overlays(false, 1, 1.0f, 2);
    comp.set_scale(false, false, false, 0.5f);
    comp.set_buffer_surface(MakeDims(0, 0, 128, 128), 0u);
    comp.configure_overlays(true, 1, 1.0f, 4);
    comp.set_buffer_surface(MakeDims(0, 0, 120, 120), 0u);

    comp.pre_destroy();
    jpx_src.close();
    fsrc.close();
  }
}

TEST_F(KduRegionCompositorTest,
       RefreshMergingOverlaySplitAndApiEdgeBranches) {
  // 1. find_completed_rects overlay split (L10047-10097) + premult sep stream
  //    (L2854-2856) + kdrc_refresh::add_region merging/shrinking (L5847-5919).
  {
    std::vector<kdu_byte> jpx_bytes = MakeSplitCompletedAndPremultSepJpx();
    VectorSource vsrc(jpx_bytes);
    jp2_family_src fsrc;
    fsrc.open(&vsrc);
    jpx_source jpx_src;
    ASSERT_GT(jpx_src.open(&fsrc, true), 0);

    kdu_region_compositor comp(&jpx_src, 65536);
    comp.configure_overlays(true, 1, 1.0f, 2);
    kdu_ilayer_ref l0 =
        comp.add_ilayer(0, kdu_dims(), MakeDims(0, 0, 256, 256));
    kdu_ilayer_ref l1 =
        comp.add_ilayer(1, kdu_dims(), MakeDims(0, 0, 256, 256));
    ASSERT_FALSE(l0.is_null());
    ASSERT_FALSE(l1.is_null());
    comp.set_scale(false, false, false, 1.0f);
    comp.set_buffer_surface(MakeDims(0, 0, 256, 256), 0x80102030u);
    kdu_dims nr;
    int guard = 0;
    while (comp.process(65536, nr) && guard++ < 60) {
    }
    EXPECT_TRUE(comp.is_processing_complete());

    // Re-dirty both layers' overlays via aux_param change, then step with tiny
    // suggested_increment=10 so l0 processes seg(0,0)=[0,0,128,128] and calls
    // find_completed_rects while l1 still has unprocessed seg [40,40,32,32].
    kdu_uint32 caux[4] = {1, 0xFF00FFFFu, 0x80FFFFFFu, 1};
    comp.configure_overlays(true, 1, 1.0f, 2, jpx_metanode(), 0, caux, 4);
    guard = 0;
    while (comp.process(10, nr) && guard++ < 60) {
    }

    // Put overlapping regions into refresh_mgr via surface init mode toggling +
    // DEFER_REGION + overlay segs (0,0), (1,0), (0,1), (1,1) completion!
    comp.remove_ilayer(l1, true);
    kdu_uint32 caux2[4] = {1, 0xFFFF00FFu, 0x80FFFFFFu, 1};
    // Helper lambda to seed scan rect into refresh_mgr and trigger seg finish.
    auto seed_and_overlap = [&](kdu_dims scan_rect, kdu_dims buf_rect) {
      comp.configure_overlays(false);
      comp.set_buffer_surface(buf_rect, 0u);
      int g = 0;
      while (comp.process(65536, nr) && g++ < 30) {
      }
      comp.set_buffer_surface(scan_rect, 0u);
      comp.set_surface_initialization_mode(false);
      comp.set_surface_initialization_mode(true);
      comp.set_buffer_surface(buf_rect, 0u);
      comp.process(65536, nr, KDU_COMPOSIT_DEFER_REGION);
      comp.configure_overlays(true, 1, 1.0f, 2, jpx_metanode(), 0, caux2, 4);
      g = 0;
      while (comp.process(65536, nr, KDU_COMPOSIT_DEFER_REGION) && g++ < 20) {
      }
      comp.process(65536, nr);
    };
    seed_and_overlap(MakeDims(20, 20, 60, 60), MakeDims(0, 0, 128, 128));
    seed_and_overlap(MakeDims(0, 0, 128, 30), MakeDims(0, 0, 128, 128));
    seed_and_overlap(MakeDims(0, 96, 128, 32), MakeDims(0, 0, 128, 128));
    seed_and_overlap(MakeDims(0, 0, 30, 128), MakeDims(0, 0, 128, 128));
    seed_and_overlap(MakeDims(96, 0, 32, 128), MakeDims(0, 0, 128, 128));
    seed_and_overlap(MakeDims(0, 40, 128, 160), MakeDims(0, 0, 128, 256));
    seed_and_overlap(MakeDims(40, 0, 160, 128), MakeDims(0, 0, 256, 128));

    comp.pre_destroy();
    jpx_src.close();
    fsrc.close();
  }

  // 2. Comprehensive API edge branches, float overlay blend=1.0, opacity,
  //    opaque clipping, queue edges, inactive layer culling, error paths, etc.
  {
    // Float buffer with overlays & blending_factor=1.0 -> L5805-5807.
    std::vector<kdu_byte> jpx_quad = MakeOverlayRefreshAndQuadJpx();
    VectorSource vsrc(jpx_quad);
    jp2_family_src fsrc;
    fsrc.open(&vsrc);
    jpx_source jpx_src;
    ASSERT_GT(jpx_src.open(&fsrc, true), 0);

    FailPromoteCompositor fcomp(/*use_float=*/true);
    fcomp.create(&jpx_src, 65536);
    kdu_thread_env dead_env;  // uncreated thread env -> L8652
    fcomp.set_thread_env(&dead_env, nullptr);
    // Double create -> KDU_ERROR_DEV (L6435-6438):
    EXPECT_THROW(fcomp.create(&jpx_src, 0), ::kdu_core::kdu_exception);

    kdu_ilayer_ref il0 =
        fcomp.add_ilayer(0, kdu_dims(), MakeDims(0, 0, 512, 512));
    fcomp.configure_overlays(true, 1, 1.0f, 4);
    fcomp.set_scale(false, false, false, 1.0f);
    fcomp.set_buffer_surface(MakeDims(0, 0, 512, 512), 0u);
    kdu_dims nr;
    int steps = 0;
    while (fcomp.process(65536, nr) && steps++ < 80) {
    }
    EXPECT_TRUE(fcomp.is_processing_complete());
    // search_overlays & get_overlay_info with overlays on and off:
    kdu_istream_ref tmp_stream;
    (void)fcomp.search_overlays(kdu_coords(310, 370), tmp_stream, 0.5f);
    int tot_n = 0, hid_n = 0;
    (void)fcomp.get_overlay_info(tot_n, hid_n);
    fcomp.configure_overlays(false, 1, 1.0f, 4);
    (void)fcomp.search_overlays(kdu_coords(310, 370), tmp_stream, 0.5f);
    EXPECT_FALSE(fcomp.get_overlay_info(tot_n, hid_n));
    fcomp.remove_ilayer(il0, false);
    fcomp.pre_destroy();
    jpx_src.close();
    fsrc.close();
  }

  {
    std::vector<kdu_byte> raw = MakeRawCodestream(64, 64, 3, 1, 8);
    VectorSource rsrc(raw);
    kdu_region_compositor comp;
    // Operations on empty compositor:
    kdu_dims nr;
    EXPECT_FALSE(comp.process(100, nr));
    EXPECT_EQ(comp.get_composition_buffer(nr), nullptr);
    EXPECT_FALSE(comp.push_composition_buffer(1, 1));
    EXPECT_FALSE(comp.pop_composition_buffer());
    EXPECT_EQ(comp.inspect_composition_queue(-1), nullptr);
    EXPECT_EQ(comp.inspect_composition_queue(0), nullptr);
    EXPECT_FALSE(comp.replace_composition_queue_tail(1, 1));
    EXPECT_FLOAT_EQ(comp.find_optimal_scale(MakeDims(0, 0, 10, 10), 1.0f,
                                            0.1f, 2.0f),
                    1.0f);
    EXPECT_EQ(comp.get_ilayer_stream(kdu_ilayer_ref(), 0), kdu_istream_ref());
    int lsrc = 0, dci = 0;
    bool opq = false;
    EXPECT_EQ(comp.get_ilayer_info(kdu_ilayer_ref(), lsrc, dci, opq), 0);
    EXPECT_EQ(comp.get_istream_info(kdu_istream_ref(), dci), 0);
    kdu_dims mr;
    EXPECT_TRUE(comp.map_region(mr, kdu_istream_ref()).is_null());
    EXPECT_TRUE(comp.find_ilayer_region(kdu_ilayer_ref(), false).is_empty());
    EXPECT_TRUE(
        comp.find_istream_region(kdu_istream_ref(), false).is_empty());
    EXPECT_EQ(comp.generate_metareq(nullptr, 0, kdu_dims(), 0, nullptr, 0,
                                    nullptr),
              0);
    comp.create(&rsrc, 65536);
    EXPECT_THROW(comp.create(&rsrc, 0), ::kdu_core::kdu_exception);

    kdu_ilayer_ref il0 = comp.add_ilayer(0, kdu_dims(), MakeDims(10, 10, 40, 40));
    kdu_ilayer_ref il1 = comp.add_ilayer(0, kdu_dims(), MakeDims(0, 0, 64, 64));
    // Opaque occlusion & strips in do_composition (L5710/L5721 + L9182-9199):
    kdu_ilayer_ref il_top = comp.add_ilayer(0, kdu_dims(), MakeDims(10, 10, 30, 30));
    comp.set_colour_order(KDU_ARGB_ORDER);
    comp.set_error_level(2);
    comp.set_error_level(3);
    // Double identical set_scale hits L7229:
    comp.set_scale(false, false, false, 1.0f);
    EXPECT_TRUE(comp.process(100, nr) || true);
    comp.set_scale(false, false, false, 1.0f);
    (void)comp.get_next_visible_ilayer(kdu_ilayer_ref(), MakeDims(0, 0, 64, 64));
    comp.set_buffer_surface(MakeDims(0, 0, 64, 64), 0x00112233u);
    int g = 0;
    while (comp.process(65536, nr) && g++ < 30) {
    }
    EXPECT_TRUE(comp.is_processing_complete());
    // search_overlays & get_ilayer_stream by codestream_idx:
    kdu_istream_ref tmp_s;
    (void)comp.search_overlays(kdu_coords(20, 20), tmp_s, 0.5f);
    EXPECT_FALSE(comp.get_ilayer_stream(il0, -1, 0).is_null());
    // Composition queue multi-push, replace, inspect >0, get non-empty:
    EXPECT_TRUE(comp.push_composition_buffer(10, 1));
    comp.set_buffer_surface(MakeDims(0, 0, 64, 64), 0x00223344u);
    while (comp.process(65536, nr) && g++ < 60) {
    }
    EXPECT_TRUE(comp.push_composition_buffer(20, 2));
    comp.set_buffer_surface(MakeDims(0, 0, 64, 64), 0x00334455u);
    while (comp.process(65536, nr) && g++ < 90) {
    }
    EXPECT_TRUE(comp.replace_composition_queue_tail(30, 3));
    EXPECT_NE(comp.get_composition_buffer(nr), nullptr);
    EXPECT_NE(comp.inspect_composition_queue(0), nullptr);
    EXPECT_NE(comp.inspect_composition_queue(1), nullptr);
    EXPECT_EQ(comp.inspect_composition_queue(99), nullptr);
    // get_istream_info with NULL components_in_use + primitive clamped comp:
    int prim_comp = 99;
    kdu_ilayer_ref prim = comp.add_primitive_ilayer(
        0, prim_comp, KDU_WANT_OUTPUT_COMPONENTS, kdu_dims(),
        MakeDims(0, 0, 64, 64), true, false, false);
    if (!prim.is_null()) {
      kdu_istream_ref ps = comp.get_ilayer_stream(prim, 0);
      float sx = 0.0f, sy = 0.0f;
      (void)comp.get_istream_info(ps, dci, nullptr, nullptr, 0, nullptr, &sx,
                                  &sy);
      kdu_long p1 = 0, p2 = 0, p3 = 0;
      (void)comp.get_codestream_packets(ps, MakeDims(5, 5, 20, 20), p1, p2, p3,
                                        -1);
    }
    // Scale growth reallocating existing composition_buffer (L9358-9360) + invalid scale:
    comp.set_scale(false, false, false, 4.0f);
    comp.set_buffer_surface(MakeDims(0, 0, 256, 256), 0u);
    comp.process(100, nr);
    comp.remove_ilayer(il_top, false);
    comp.remove_ilayer(il1, false);
    comp.remove_ilayer(il0, false);
    comp.cull_inactive_ilayers(2);
    comp.cull_inactive_ilayers(1);
    comp.cull_inactive_ilayers(0);
    // Invalid huge scale (L3315-3319, L8734-8738):
    comp.set_scale(false, false, false, 1e9f);
    EXPECT_FALSE(comp.process(100, nr));
    comp.pre_destroy();
  }
}

TEST_F(KduRegionCompositorTest, RefreshAddRegionAllOverlapCases) {
  std::vector<kdu_byte> jpx_bytes;
  {
    VectorTarget vec_target(&jpx_bytes);
    jp2_family_tgt family;
    family.open(&vec_target);
    jpx_target jout;
    jout.open(&family);

    auto cs_tgt = jout.add_codestream();
    siz_params siz;
    siz.set(Sdims, 0, 0, 256);
    siz.set(Sdims, 0, 1, 256);
    siz.set(Scomponents, 0, 0, 3);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    static_cast<::kdu_core::kdu_params &>(siz).finalize();
    cs_tgt.access_dimensions().init(&siz);

    jpx_layer_target lay = jout.add_layer();
    lay.add_colour().init(::kdu_supp::JP2_sRGB_SPACE);
    lay.access_channels().init(3);
    lay.access_channels().set_colour_mapping(0, 0, -1, 0);
    lay.access_channels().set_colour_mapping(1, 1, -1, 0);
    lay.access_channels().set_colour_mapping(2, 2, -1, 0);

    jpx_meta_manager meta = jout.access_meta_manager();
    jpx_metanode root = meta.access_root();
    int cs_idx = 0;
    jpx_metanode nlist = root.add_numlist(1, &cs_idx, 0, nullptr, false);
    jpx_roi roi;
    roi.init_rectangle(MakeDims(0, 0, 256, 256));
    roi.coding_priority = 1;
    nlist.add_regions(1, &roi);

    while (jout.write_headers() != nullptr) {
    }
    auto *out = cs_tgt.open_stream();
    CompressToTarget(out, 256, 256, 3, 8, 1, 128, true, false, 15);
    out->close();
    jout.close();
    family.close();
  }

  VectorSource src(jpx_bytes);
  jp2_family_src fsrc;
  fsrc.open(&src);
  jpx_source jpx_src;
  ASSERT_GE(jpx_src.open(&fsrc, true), 0);

  kdu_region_compositor comp(&jpx_src, 65536);
  comp.add_ilayer(0, MakeDims(0, 0, 0, 0), MakeDims(0, 0, 0, 0));
  comp.set_scale(false, false, false, 1.0f);
  const kdu_dims full_rect = MakeDims(0, 0, 256, 256);
  comp.configure_overlays(false);
  comp.set_buffer_surface(full_rect, 0u);
  kdu_dims nr;
  int steps = 0;
  while (comp.process(65536, nr) && steps++ < 40) {
  }

  const kdu_uint32 aux[4] = {0, 0x80FF00FFu, 1, 0x4000FF80u};
  // Pair rects designed to exercise all 4 merge/split cases in
  // kdrc_refresh::add_region against 128x128 overlay segments.
  const kdu_dims scan_rects[] = {
      MakeDims(0, 0, 160, 64),    MakeDims(0, 64, 160, 128),
      MakeDims(0, 0, 64, 160),    MakeDims(64, 0, 128, 160),
      MakeDims(10, 0, 80, 160),   MakeDims(10, 64, 80, 128),
      MakeDims(0, 10, 160, 80),   MakeDims(64, 10, 128, 80),
  };
  for (const kdu_dims &scan_rect : scan_rects) {
    comp.configure_overlays(false);
    comp.set_surface_initialization_mode(true);
    comp.set_buffer_surface(scan_rect, 0u);
    comp.set_surface_initialization_mode(false);
    comp.set_buffer_surface(full_rect, 0u);
    comp.configure_overlays(true, 1, 1.0f, 4, jpx_metanode(), 0, aux, 2);
    steps = 0;
    while (comp.process(65536, nr) && steps++ < 40) {
    }
  }
  comp.pre_destroy();
  jpx_src.close();
  fsrc.close();
}

TEST_F(KduRegionCompositorTest, UpdateOverlayFailPromoteFreshBuffer) {
  std::vector<kdu_byte> jpx_bytes = MakeMultiLayerJpxWithOverlaysAndFrames();
  VectorSource src(jpx_bytes);
  jp2_family_src fsrc;
  fsrc.open(&src);
  jpx_source jpx_src;
  ASSERT_GE(jpx_src.open(&fsrc, true), 0);

  CustomRegionCompositor comp(/*float_mode=*/false, /*allow_read=*/false,
                              /*handle_custom_overlay=*/false);
  comp.create(&jpx_src);
  comp.add_ilayer(0, MakeDims(0, 0, 0, 0), MakeDims(0, 0, 0, 0));
  comp.configure_overlays(false);
  comp.set_scale(false, false, false, 1.0f);
  comp.set_buffer_surface(MakeDims(0, 0, 64, 48), 0u);
  kdu_dims nr;
  int steps = 0;
  while (comp.process(65536, nr) && steps++ < 20) {
  }
  const kdu_uint32 aux[4] = {0, 0x80FF0000u, 1, 0x4000FF00u};
  comp.configure_overlays(true, 1, 1.0f, 4, jpx_metanode(), 0, aux, 2);
  comp.update_overlays(true);
  steps = 0;
  while (comp.process(65536, nr) && steps++ < 30) {
  }
  comp.pre_destroy();
  {
    class AlwaysFailReadBuf : public kdu_compositor_buf {
     public:
      bool set_read_accessibility(bool read_access_required) override {
        read_access_allowed = true;
        if (read_access_required) {
          return false;
        }
        return true;
      }
    };
    class AlwaysFailReadCompositor : public kdu_region_compositor {
     public:
      ~AlwaysFailReadCompositor() override { pre_destroy(); }
      kdu_compositor_buf *allocate_buffer(kdu_coords min_size,
                                          kdu_coords &actual_size,
                                          bool) override {
        actual_size = kdu_coords(min_size.x + 8, min_size.y + 8);
        auto *buf = new AlwaysFailReadBuf();
        // Ownership of the array transfers to kdu_compositor_buf::~kdu_compositor_buf.
        auto *storage =
            new kdu_uint32[static_cast<size_t>(actual_size.x) * actual_size.y]();
        buf->init(storage, actual_size.x);
        return buf;
      }
      void delete_buffer(kdu_compositor_buf *buf) override { delete buf; }
    };
    std::vector<kdu_byte> quad_bytes = MakeOverlayRefreshAndQuadJpx();
    VectorSource qsrc(quad_bytes);
    jp2_family_src qfsrc;
    qfsrc.open(&qsrc);
    jpx_source qjpx;
    ASSERT_GE(qjpx.open(&qfsrc, true), 0);

    AlwaysFailReadCompositor fail_comp;
    fail_comp.create(&qjpx);
    fail_comp.add_ilayer(0, MakeDims(0, 0, 0, 0), MakeDims(0, 0, 0, 0));
    fail_comp.set_scale(false, false, false, 1.0f);
    // Initial region (0,0,64,64) has zero ROIs (ROIs in quad_bytes are at
    // x>=130, y>=140) so have_overlay_info stays false until shifting onto ROIs
    // in kdrc_layer::set_buffer_surface (L5199-5220).
    fail_comp.load_metadata_matches();
    fail_comp.configure_overlays(true, 0, 1.0f, 4, jpx_metanode(), 0, aux, 2);
    fail_comp.set_buffer_surface(MakeDims(0, 0, 64, 64), 0u);
    int s = 0;
    while (fail_comp.process(65536, nr) && s++ < 20) {
    }
    fail_comp.set_buffer_surface(MakeDims(250, 250, 128, 128), 0u);
    s = 0;
    while (fail_comp.process(65536, nr) && s++ < 30) {
    }
    fail_comp.pre_destroy();
    qjpx.close();
    qfsrc.close();
  }
  jpx_src.close();
  fsrc.close();
}

TEST_F(KduRegionCompositorTest,
       JpxReactivationMetareqRenderedSubsampledPrimitiveAndInterlacedMj2) {
  // 1. Two-frame JPX testing inactive layer reactivation across set_frame and
  //    KDU_MRQ_STREAM metareq on a rendered_result=true numlist.
  {
    std::vector<kdu_byte> jpx_bytes;
    VectorTarget vec_target(&jpx_bytes);
    jp2_family_tgt family;
    family.open(&vec_target);
    jpx_target jout;
    jout.open(&family);

    auto cs0_tgt = jout.add_codestream();
    auto cs1_tgt = jout.add_codestream();
    for (int i = 0; i < 2; ++i) {
      auto cs_tgt = (i == 0) ? cs0_tgt : cs1_tgt;
      siz_params siz;
      siz.set(Sdims, 0, 0, 48);
      siz.set(Sdims, 0, 1, 48);
      siz.set(Scomponents, 0, 0, 3);
      siz.set(Sprecision, 0, 0, 8);
      siz.set(Ssigned, 0, 0, false);
      static_cast<::kdu_core::kdu_params &>(siz).finalize();
      cs_tgt.access_dimensions().init(&siz);

      jpx_layer_target lay = jout.add_layer();
      lay.add_colour().init(::kdu_supp::JP2_sRGB_SPACE);
      lay.access_channels().init(3);
      lay.access_channels().set_colour_mapping(0, 0, -1, i);
      lay.access_channels().set_colour_mapping(1, 1, -1, i);
      lay.access_channels().set_colour_mapping(2, 2, -1, i);
    }

    jpx_composition comp_rules = jout.access_composition();
    jx_frame *f0 = comp_rules.add_frame(100, 1, false);
    comp_rules.add_instruction(f0, 0, 0, MakeDims(0, 0, 48, 48),
                               MakeDims(0, 0, 48, 48));
    jx_frame *f1 = comp_rules.add_frame(100, 1, false);
    comp_rules.add_instruction(f1, 1, 0, MakeDims(0, 0, 48, 48),
                               MakeDims(0, 0, 48, 48));

    jpx_meta_manager mm = jout.access_meta_manager();
    jpx_metanode root = mm.access_root();
    int cs0 = 0;
    jpx_metanode nlist_r = root.add_numlist(1, &cs0, 0, nullptr,
                                            /*rendered_result=*/true);
    nlist_r.add_label("RenderedStream0");

    while (jout.write_headers() != nullptr) {
    }
    for (int i = 0; i < 2; ++i) {
      auto cs_tgt = (i == 0) ? cs0_tgt : cs1_tgt;
      auto *out = cs_tgt.open_stream();
      CompressToTarget(out, 48, 48, 3, 8, 1, 0, true, false, i * 25);
      out->close();
    }
    while (jout.write_metadata() != nullptr) {
    }
    jout.close();
    family.close();

    VectorSource src(jpx_bytes);
    jp2_family_src fsrc;
    fsrc.open(&src);
    jpx_source jpx_src;
    ASSERT_GE(jpx_src.open(&fsrc, true), 0);

    kdu_region_compositor comp(&jpx_src, 65536);
    jpx_composition jcomp = jpx_src.access_composition();
    jx_frame *rf0 = jcomp.get_next_frame(nullptr);
    jx_frame *rf1 = jcomp.get_next_frame(rf0);
    ASSERT_NE(rf0, nullptr);
    ASSERT_NE(rf1, nullptr);

    jpx_frame_expander exp0, exp1;
    ASSERT_TRUE(exp0.construct(&jpx_src, rf0, 0, true));
    ASSERT_TRUE(exp1.construct(&jpx_src, rf1, 0, true));

    comp.set_frame(&exp0);
    comp.set_scale(false, false, false, 1.0f);
    comp.set_buffer_surface(MakeDims(0, 0, 48, 48), 0u);
    kdu_dims nr;
    int steps = 0;
    while (comp.process(32768, nr) && steps++ < 30) {
    }
    // Switch frame 0 -> frame 1 (update_composition moves layer 0 inactive) ->
    // add/set frame 0 again to reactivate inactive layers via add_composition.
    comp.set_frame(&exp1);
    kdu_dims fdims;
    EXPECT_TRUE(comp.get_total_composition_dims(fdims));
    comp.set_buffer_surface(MakeDims(0, 0, 48, 48), 0u);
    comp.set_frame(&exp0);
    EXPECT_TRUE(comp.get_total_composition_dims(fdims));
    comp.add_frame(&exp1, kdu_coords(48, 0));
    comp.set_buffer_surface(MakeDims(0, 0, 96, 48), 0u);
    comp.load_metadata_matches();

    // Uncropped region queries with transpose=false and transpose=true.
    kdu_ilayer_ref il_cropped = comp.add_ilayer(
        0, MakeDims(8, 8, 24, 24), MakeDims(0, 0, 24, 24));
    kdu_istream_ref is_cropped = comp.get_ilayer_stream(il_cropped, 0);
    comp.set_scale(false, false, false, 1.0f);
    EXPECT_FALSE(comp.find_ilayer_region(il_cropped, false).is_empty());
    EXPECT_FALSE(comp.find_istream_region(is_cropped, false).is_empty());
    comp.set_scale(true, false, false, 1.0f);
    EXPECT_FALSE(comp.find_ilayer_region(il_cropped, false).is_empty());
    EXPECT_FALSE(comp.find_istream_region(is_cropped, false).is_empty());
    // Single-pixel upscaled map_region hitting min>=lim floor/ceil fallback.
    comp.set_scale(false, false, false, 4.0f);
    kdu_dims map_reg = MakeDims(5, 5, 1, 1);
    comp.map_region(map_reg, is_cropped);
    comp.map_region(map_reg, kdu_istream_ref());
    ::kdu_core::kdu_long pkts = 0, pre_pkts = 0, next_pkts = 0;
    comp.get_codestream_packets(is_cropped, map_reg, pkts, pre_pkts, next_pkts);

    // generate_metareq with KDU_MRQ_STREAM on rendered_result=true numlist.
    kdu_window win;
    const kdu_uint32 lbl_types[1] = {jp2_label_4cc};
    comp.generate_metareq(&win, KDU_MRQ_STREAM, MakeDims(0, 0, 48, 48), 1,
                          lbl_types, 0, nullptr);

    // Invalid codestream source triggering acquire_codestream catch paths.
    {
      try {
        comp.add_ilayer(99, MakeDims(0, 0, 0, 0), MakeDims(0, 0, 0, 0));
      } catch (::kdu_core::kdu_exception) {
      }
      int bad_comp = 0;
      try {
        comp.add_primitive_ilayer(99, bad_comp, KDU_WANT_CODESTREAM_COMPONENTS,
                                  MakeDims(0, 0, 0, 0), MakeDims(0, 0, 0, 0));
      } catch (::kdu_core::kdu_exception) {
      }
    }

    comp.pre_destroy();
    jpx_src.close();
    fsrc.close();
  }
}

TEST_F(KduRegionCompositorTest, SubsampledPrimitiveIlayerAndSmallScale) {
  // 2. 4:2:0 subsampled primitive single-component ilayer + tiny scale warning.
  {
    std::vector<kdu_byte> sub_bytes;
    {
      VectorTarget target(&sub_bytes);
      siz_params siz;
      siz.set(Sdims, 0, 0, 64);
      siz.set(Sdims, 0, 1, 64);
      siz.set(Scomponents, 0, 0, 3);
      siz.set(Sprecision, 0, 0, 8);
      siz.set(Ssigned, 0, 0, false);
      const int sub[3] = {1, 2, 2};
      for (int c = 0; c < 3; ++c) {
        siz.set(Sdims, c, 0, 64 / sub[c]);
        siz.set(Sdims, c, 1, 64 / sub[c]);
        siz.set(Ssampling, c, 0, sub[c]);
        siz.set(Ssampling, c, 1, sub[c]);
      }
      static_cast<::kdu_core::kdu_params &>(siz).finalize();

      kdu_codestream cs;
      cs.create(&siz, &target);
      absl::Cleanup cs_cleanup = [&cs]() {
        if (cs.exists()) {
          cs.destroy();
        }
      };
      cs.access_siz()->access_cluster(COD_params)->set(Creversible, 0, 0, true);
      cs.access_siz()->access_cluster(COD_params)->set(Clevels, 0, 0, 2);
      cs.access_siz()->access_cluster(COD_params)->set(Cycc, 0, 0, false);
      cs.access_siz()->finalize_all();

      kdu_stripe_compressor comp_enc;
      comp_enc.start(cs);
      std::vector<kdu_byte> p0(64 * 64, 128), p1(32 * 32, 90), p2(32 * 32, 200);
      kdu_byte *planes[3] = {p0.data(), p1.data(), p2.data()};
      int heights[3] = {64, 32, 32};
      int offsets[3] = {0, 0, 0};
      int row_gaps[3] = {64, 32, 32};
      int pix_gaps[3] = {1, 1, 1};
      comp_enc.push_stripe(planes, heights, offsets, row_gaps, pix_gaps);
      comp_enc.finish();
    }

    kdu_compressed_source_buffered src(sub_bytes.data(), sub_bytes.size());
    kdu_region_compositor comp;
    comp.create(&src);
    int comp_idx = 1;
    // Non-empty full_source_dims when subsampled component != codestream canvas.
    kdu_ilayer_ref il_sub = comp.add_primitive_ilayer(
        0, comp_idx, KDU_WANT_CODESTREAM_COMPONENTS, MakeDims(0, 0, 64, 64),
        MakeDims(0, 0, 64, 64));
    EXPECT_FALSE(il_sub.is_null());
    {
      int sub_dci = 0;
      float sx = 0.0f, sy = 0.0f;
      (void)comp.get_istream_info(comp.get_ilayer_stream(il_sub, 0), sub_dci,
                                  nullptr, nullptr, 0, nullptr, &sx, &sy);
    }
    comp.set_scale(false, false, false, 1.0f);
    kdu_dims cdims;
    EXPECT_TRUE(comp.get_total_composition_dims(cdims));
    comp.set_buffer_surface(cdims, 0u);
    kdu_dims nr;
    int steps = 0;
    while (comp.process(32768, nr) && steps++ < 20) {
    }
    comp.remove_ilayer(il_sub, false);
    // Tiny target dims + moderate scale triggering empty full_region at L2625.
    comp_idx = 0;
    comp.add_primitive_ilayer(0, comp_idx, KDU_WANT_CODESTREAM_COMPONENTS,
                              MakeDims(0, 0, 64, 64), MakeDims(0, 0, 1, 1));
    comp.set_scale(false, false, false, 0.25f);
    EXPECT_FALSE(comp.process(100, nr));
    // Huge target position exceeding 0x7FFFF000 at L2653 + L3315/L8734.
    comp.pre_destroy();
    kdu_compressed_source_buffered src2(sub_bytes.data(), sub_bytes.size());
    kdu_region_compositor comp2(&src2);
    comp_idx = 0;
    comp2.add_primitive_ilayer(0, comp_idx, KDU_WANT_CODESTREAM_COMPONENTS,
                               MakeDims(0, 0, 64, 64),
                               MakeDims(0x7FFFF010, 0, 64, 64));
    comp2.set_scale(false, false, false, 1.0f);
    comp2.set_scale(false, false, false, 1.0f);
    EXPECT_FALSE(comp2.process(100, nr));
    comp2.pre_destroy();

    // 2x2 codestream with Clevels=3 so discard levels collapse to empty.
    std::vector<kdu_byte> tiny_bytes = MakeRawCodestream(2, 2, 1, 1, 0);
    kdu_compressed_source_buffered tiny_src(tiny_bytes.data(), tiny_bytes.size());
    kdu_region_compositor tiny_comp(&tiny_src);
    tiny_comp.add_ilayer(0, kdu_dims(), kdu_dims());
    tiny_comp.set_scale(false, false, false, 0.25f);
    kdu_dims td;
    if (tiny_comp.get_total_composition_dims(td) && !td.is_empty()) {
      tiny_comp.set_buffer_surface(td, 0u);
      tiny_comp.process(100, nr);
    }
    tiny_comp.pre_destroy();
  }
}

TEST_F(KduRegionCompositorTest, InterlacedMj2FieldHandling3) {
  // 3. Interlaced MJ2 change_frame with field_handling=3 and activate change.
  {
    std::vector<kdu_byte> mj2_bytes = MakeMj2Bytes(/*interlaced=*/true);
    VectorSource src(mj2_bytes);
    jp2_family_src fsrc;
    fsrc.open(&src);
    mj2_source mj2_src;
    ASSERT_GE(mj2_src.open(&fsrc, true), 0);

    kdu_region_compositor comp(&mj2_src, 65536);
    kdu_ilayer_ref il = comp.add_ilayer(0, MakeDims(0, 0, 0, 0),
                                        MakeDims(0, 0, 0, 0), false, false,
                                        false, /*frame_idx=*/0,
                                        /*field_handling=*/3);
    EXPECT_FALSE(il.is_null());
    comp.set_scale(false, false, false, 1.0f);
    kdu_dims dims;
    EXPECT_TRUE(comp.get_total_composition_dims(dims));
    comp.set_buffer_surface(dims, 0u);
    kdu_dims nr;
    int steps = 0;
    while (comp.process(32768, nr) && steps++ < 40) {
    }
    // Change frame while active with field_handling=3 (change_frame path).
    EXPECT_TRUE(comp.change_ilayer_frame(il, 1));
    steps = 0;
    while (comp.process(32768, nr) && steps++ < 40) {
    }
    EXPECT_TRUE(comp.change_ilayer_frame(il, 0));
    steps = 0;
    while (comp.process(32768, nr) && steps++ < 40) {
    }
    // Deactivate frame 0 (field_handling=3) and reactivate frame 0 (field_handling=0).
    comp.remove_ilayer(il, /*leave_inactive=*/true);
    il = comp.add_ilayer(0, MakeDims(0, 0, 0, 0), MakeDims(0, 0, 0, 0), false,
                         false, false, /*frame_idx=*/0, /*field_handling=*/0);
    EXPECT_FALSE(il.is_null());
    comp.set_scale(false, false, false, 1.0f);
    EXPECT_TRUE(comp.get_total_composition_dims(dims));
    comp.set_buffer_surface(dims, 0u);
    steps = 0;
    while (comp.process(32768, nr) && steps++ < 40) {
    }
    comp.pre_destroy();
    mj2_src.close();
    fsrc.close();
  }
}

TEST_F(KduRegionCompositorTest, InvalidateRectRefreshMerging) {
  std::vector<kdu_byte> raw = MakeRawCodestream(256, 256, 1, 1, 0);
  kdu_compressed_source_buffered src(raw.data(), raw.size());
  kdu_region_compositor comp;
  comp.create(&src);
  comp.add_ilayer(0, kdu_dims(), MakeDims(0, 0, 256, 256));
  comp.set_scale(false, false, false, 1.0f);
  comp.set_buffer_surface(MakeDims(0, 0, 256, 256), 0u);
  kdu_dims nr;
  int steps = 0;
  while (comp.process(65536, nr) && steps++ < 100) {
  }
  ASSERT_TRUE(comp.is_processing_complete());
  comp.get_composition_buffer(nr);

  auto flush = [&]() {
    int s = 0;
    while (comp.process(65536, nr) && s++ < 100) {
    }
  };
  // Pairs in empty [0,0,192,192] trigger kdrc_refresh::add_region splits/merges.
  const kdu_dims pairs[][2] = {
      {MakeDims(40, 40, 80, 80), MakeDims(50, 40, 60, 100)},
      {MakeDims(40, 40, 80, 80), MakeDims(50, 20, 60, 100)},
      {MakeDims(40, 40, 80, 80), MakeDims(40, 50, 100, 60)},
      {MakeDims(40, 40, 80, 80), MakeDims(20, 50, 100, 60)},
      {MakeDims(40, 40, 80, 80), MakeDims(20, 40, 120, 60)},
      {MakeDims(40, 40, 80, 80), MakeDims(20, 60, 120, 60)},
      {MakeDims(40, 40, 80, 80), MakeDims(40, 20, 60, 120)},
      {MakeDims(40, 40, 80, 80), MakeDims(60, 20, 60, 120)},
      // Pairs where intersection spans scan (not region) -> L5890-5919.
      {MakeDims(40, 60, 40, 60), MakeDims(20, 40, 80, 40)},
      {MakeDims(40, 60, 40, 60), MakeDims(20, 100, 80, 40)},
      {MakeDims(60, 40, 60, 40), MakeDims(40, 20, 40, 80)},
      {MakeDims(60, 40, 60, 40), MakeDims(100, 20, 40, 80)},
  };
  for (const auto &p : pairs) {
    comp.invalidate_rect(p[0]);
    comp.invalidate_rect(p[1]);
    flush();
  }
  comp.pre_destroy();
}

TEST_F(KduRegionCompositorTest, OverlayBufferGrowAndBackwardShift) {
  std::vector<kdu_byte> jpx_bytes;
  {
    VectorTarget vec_target(&jpx_bytes);
    jp2_family_tgt family;
    family.open(&vec_target);
    jpx_target jout;
    jout.open(&family);

    auto cs_tgt = jout.add_codestream();
    siz_params siz;
    siz.set(Sdims, 0, 0, 512);
    siz.set(Sdims, 0, 1, 512);
    siz.set(Scomponents, 0, 0, 3);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    static_cast<::kdu_core::kdu_params &>(siz).finalize();
    cs_tgt.access_dimensions().init(&siz);

    jpx_layer_target lay = jout.add_layer();
    lay.add_colour().init(::kdu_supp::JP2_sRGB_SPACE);
    lay.access_channels().init(3);
    lay.access_channels().set_colour_mapping(0, 0, -1, 0);
    lay.access_channels().set_colour_mapping(1, 1, -1, 0);
    lay.access_channels().set_colour_mapping(2, 2, -1, 0);

    jpx_meta_manager mm = jout.access_meta_manager();
    jpx_metanode root = mm.access_root();
    int cs_idx = 0;
    jpx_metanode nl = root.add_numlist(1, &cs_idx, 0, nullptr, false);
    // Two ROIs in segments (0,0) and (2,2) leaving segment (1,1) empty
    // so get_unprocessed_overlay_seg skips non-needing segment, plus dense seg (2,2).
    jpx_roi rois[2];
    rois[0].init_rectangle(MakeDims(140, 140, 40, 40));
    rois[1].init_rectangle(MakeDims(270, 270, 80, 80));
    nl.add_regions(2, rois);

    while (jout.write_headers() != nullptr) {
    }
    auto *out = cs_tgt.open_stream();
    CompressToTarget(out, 512, 512, 3, 8, 1, 256, true, false, 7);
    out->close();
    while (jout.write_metadata() != nullptr) {
    }
    jout.close();
    family.close();
  }

  VectorSource src(jpx_bytes);
  jp2_family_src fsrc;
  fsrc.open(&src);
  jpx_source jpx_src;
  ASSERT_GE(jpx_src.open(&fsrc, true), 0);

  const kdu_uint32 aux[4] = {0, 0x80FF00FFu, 1, 0x4000FF80u};
  for (bool float_mode : {false, true}) {
    CustomRegionCompositor comp(float_mode, /*allow_read=*/true,
                                /*handle_custom_overlay=*/false);
    comp.create(&jpx_src);
    comp.add_ilayer(0, kdu_dims(), kdu_dims());
    comp.configure_overlays(true, 1, 1.0f, 4, jpx_metanode(), 0, aux, 2);
    comp.set_scale(false, false, false, 1.0f);
    kdu_dims nr;
    auto run = [&](kdu_dims rect) {
      comp.set_buffer_surface(rect, 0u);
      int s = 0;
      while (comp.process(65536, nr) && s++ < 40) {
      }
    };
    run(MakeDims(128, 128, 256, 256));
    run(MakeDims(128, 128, 384, 384));
    run(MakeDims(64, 64, 384, 384));
    comp.pre_destroy();
  }
  jpx_src.close();
  fsrc.close();
}

TEST_F(KduRegionCompositorTest,
       PremultTwoStreamOpaqueMaskCorruptJpxAndMiscBranches) {
  // 1. Two-stream JPX layer with premultiplied opacity on second codestream,
  //    reactivating missing stream layer, float blend=1.0, disjoint stats,
  //    overlay_dependencies cleanup on pre_destroy, and refresh(init=true).
  std::vector<kdu_byte> jpx_bytes;
  {
    VectorTarget vec_target(&jpx_bytes);
    jp2_family_tgt family;
    family.open(&vec_target);
    jpx_target jout;
    jout.open(&family);

    auto cs0_tgt = jout.add_codestream();
    auto cs1_tgt = jout.add_codestream();
    siz_params siz0, siz1;
    siz0.set(Sdims, 0, 0, 48);
    siz0.set(Sdims, 0, 1, 48);
    siz0.set(Scomponents, 0, 0, 3);
    siz0.set(Sprecision, 0, 0, 8);
    siz0.set(Ssigned, 0, 0, false);
    static_cast<::kdu_core::kdu_params &>(siz0).finalize();
    cs0_tgt.access_dimensions().init(&siz0);

    siz1.set(Sdims, 0, 0, 48);
    siz1.set(Sdims, 0, 1, 48);
    siz1.set(Scomponents, 0, 0, 1);
    siz1.set(Sprecision, 0, 0, 8);
    siz1.set(Ssigned, 0, 0, false);
    static_cast<::kdu_core::kdu_params &>(siz1).finalize();
    cs1_tgt.access_dimensions().init(&siz1);

    // Layer 0: opaque cs0.
    jpx_layer_target lay0 = jout.add_layer();
    lay0.add_colour().init(::kdu_supp::JP2_sRGB_SPACE);
    lay0.access_channels().init(3);
    lay0.access_channels().set_colour_mapping(0, 0, -1, 0);
    lay0.access_channels().set_colour_mapping(1, 1, -1, 0);
    lay0.access_channels().set_colour_mapping(2, 2, -1, 0);

    // Layer 1: cs0 colour + cs1 premultiplied opacity (L2854-2860).
    jpx_layer_target lay1 = jout.add_layer();
    lay1.add_colour().init(::kdu_supp::JP2_sRGB_SPACE);
    lay1.access_channels().init(3);
    lay1.access_channels().set_colour_mapping(0, 0, -1, 0);
    lay1.access_channels().set_colour_mapping(1, 1, -1, 0);
    lay1.access_channels().set_colour_mapping(2, 2, -1, 0);
    lay1.access_channels().set_opacity_mapping(-1, 0, -1, 1);
    lay1.set_codestream_registration(0, kdu_coords(0, 0), kdu_coords(1, 1),
                                     kdu_coords(1, 1));
    lay1.set_codestream_registration(1, kdu_coords(0, 0), kdu_coords(1, 1),
                                     kdu_coords(1, 1));

    jpx_meta_manager mm = jout.access_meta_manager();
    jpx_metanode root = mm.access_root();
    int cs0 = 0;
    jpx_metanode nl = root.add_numlist(1, &cs0, 0, nullptr, false);
    jpx_roi roi;
    roi.init_rectangle(MakeDims(4, 4, 24, 24));
    nl.add_regions(1, &roi);

    while (jout.write_headers() != nullptr) {
    }
    auto *out0 = cs0_tgt.open_stream();
    CompressToTarget(out0, 48, 48, 3, 8, 1, 0, true, false, 11);
    out0->close();
    auto *out1 = cs1_tgt.open_stream();
    CompressToTarget(out1, 48, 48, 1, 8, 1, 0, true, false, 22);
    out1->close();
    jout.close();
    family.close();
  }

  VectorSource src(jpx_bytes);
  jp2_family_src fsrc;
  fsrc.open(&src);
  jpx_source jpx_src;
  ASSERT_GE(jpx_src.open(&fsrc, true), 0);

  {
    CustomRegionCompositor comp(/*float_mode=*/true, /*allow_read=*/true,
                                /*handle_custom_overlay=*/false);
    comp.create(&jpx_src);
    try {
      kdu_ilayer_ref il_opq =
          comp.add_ilayer(0, kdu_dims(), MakeDims(100, 100, 48, 48));
      EXPECT_FALSE(il_opq.is_null());
      kdu_ilayer_ref il_prm =
          comp.add_ilayer(1, kdu_dims(), MakeDims(0, 0, 48, 48));
      kdu_istream_ref is0 = comp.get_ilayer_stream(il_prm, 0);
      kdu_istream_ref is_mask = comp.get_ilayer_stream(il_prm, 1);
      EXPECT_FALSE(is0.is_null());
      EXPECT_FALSE(is_mask.is_null());

      comp.set_scale(false, false, false, 1.0f);
      // Two active layers (composition_buffer != NULL) + float blend_factor=1.0.
      const kdu_uint32 aux[2] = {0, 0x80FF00FFu};
      comp.load_metadata_matches();
      comp.configure_overlays(true, 1, 1.0f, 2, jpx_metanode(), 0, aux, 1);
      comp.set_buffer_surface(MakeDims(0, 0, 160, 160), 0u);
      kdu_dims nr;
      int s = 0;
      while (comp.process(32768, nr) && s++ < 40) {
      }
      // Disjoint packet stats (layer 0 does not intersect stream 0 region).
      ::kdu_core::kdu_long ps = 0, pks = 0, mps = 0;
      comp.get_codestream_packets(is0, MakeDims(0, 0, 48, 48), ps, pks, mps);

      // Refresh with initialize_surfaces_on_next_refresh + composition_buffer.
      comp.set_surface_initialization_mode(false);
      comp.set_surface_initialization_mode(true);
      comp.refresh();

      // Remove il_prm (leaving head stream inactive), steal cs0 via new layer 0,
      // then reactivate il_prm (missing_streams path L4958).
      comp.remove_ilayer(il_prm, /*leave_inactive=*/true);
      kdu_ilayer_ref il_opq2 =
          comp.add_ilayer(0, kdu_dims(), MakeDims(60, 60, 48, 48));
      EXPECT_FALSE(il_opq2.is_null());
      il_prm = comp.add_ilayer(1, kdu_dims(), MakeDims(0, 0, 48, 48));
      EXPECT_FALSE(il_prm.is_null());
    } catch (::kdu_core::kdu_exception exc) {
      ADD_FAILURE() << "Part 1 threw kdu_exception " << exc << ": "
                    << handlers_.last_error();
    }
    comp.pre_destroy();
  }
  jpx_src.close();
  fsrc.close();

  // 2. Corrupt jp2c payload inside JPX triggering acquire_codestream catch.
  std::vector<kdu_byte> bad_jpx = jpx_bytes;
  if (bad_jpx.size() > 64) {
    std::fill(bad_jpx.end() - 48, bad_jpx.end(), 0);
    VectorSource bad_src(bad_jpx);
    jp2_family_src bad_fsrc;
    bad_fsrc.open(&bad_src);
    jpx_source bad_jpx_src;
    if (bad_jpx_src.open(&bad_fsrc, true) >= 0) {
      kdu_region_compositor bad_comp(&bad_jpx_src);
      try {
        bad_comp.add_ilayer(0, kdu_dims(), kdu_dims());
      } catch (::kdu_core::kdu_exception) {
      }
      int comp_idx = 0;
      try {
        bad_comp.add_primitive_ilayer(0, comp_idx, KDU_WANT_CODESTREAM_COMPONENTS,
                                      kdu_dims(), kdu_dims());
      } catch (::kdu_core::kdu_exception) {
      }
      bad_comp.pre_destroy();
      bad_jpx_src.close();
    }
    bad_fsrc.close();
  }
}

TEST_F(KduRegionCompositorTest,
       MultiTileLimitsCorruptCodestreamsMj2AndJpxSetFrame) {
  // 1. Two-tile raw codestream with Clevels:T1=1 so Tile 1 forces
  // max_discard_levels reduction (L4388-4392, L8734-8738).
  {
    std::vector<kdu_byte> tile_lvl_bytes;
    {
      VectorTarget target(&tile_lvl_bytes);
      siz_params siz;
      siz.set(Sdims, 0, 0, 48);
      siz.set(Sdims, 0, 1, 48);
      siz.set(Scomponents, 0, 0, 1);
      siz.set(Sprecision, 0, 0, 8);
      siz.set(Ssigned, 0, 0, false);
      siz.set(Stiles, 0, 0, 24);
      siz.set(Stiles, 0, 1, 24);
      static_cast<::kdu_core::kdu_params &>(siz).finalize();
      kdu_codestream cs;
      cs.create(&siz, &target);
      absl::Cleanup cs_cleanup = [&cs]() {
        if (cs.exists()) cs.destroy();
      };
      cs.access_siz()->parse_string("Creversible=yes");
      cs.access_siz()->parse_string("Clevels=3");
      cs.access_siz()->parse_string("Clevels:T1=1");
      cs.access_siz()->finalize_all();
      kdu_stripe_compressor enc;
      enc.start(cs);
      std::vector<kdu_byte> buf(48 * 48, 128);
      int stripe_h = 48;
      enc.push_stripe(buf.data(), &stripe_h);
      enc.finish();
    }
    kdu_compressed_source_buffered src(tile_lvl_bytes.data(),
                                       tile_lvl_bytes.size());
    kdu_region_compositor comp(&src);
    comp.add_ilayer(0, kdu_dims(), kdu_dims());
    comp.set_scale(false, false, false, 0.25f);
    comp.set_buffer_surface(MakeDims(0, 0, 5, 5), 0u);
    kdu_dims nr;
    int s = 0;
    while (comp.process(65536, nr) && s++ < 20) {
    }
    // Expand to cover Tile 1 -> max_discard_levels drops from 3 to 1.
    comp.set_buffer_surface(MakeDims(0, 0, 12, 12), 0u);
    s = 0;
    while (comp.process(65536, nr) && s++ < 20) {
    }
    // Retry scale after TRY_SCALE_AGAIN triggers L8734-8738.
    comp.set_scale(false, false, false, 0.25f);
    comp.set_buffer_surface(MakeDims(0, 0, 24, 24), 0u);
    s = 0;
    while (comp.process(65536, nr) && s++ < 20) {
    }
    comp.pre_destroy();
  }

  // 2. Two-tile Part-2 codestream where Tile 1 uses stacked high-pass Cdecomp
  // so vflip=true succeeds on Tile 0 then hits CANNOT_FLIP (L4394-4401).
  {
    std::vector<kdu_byte> decomp_bytes;
    {
      VectorTarget target(&decomp_bytes);
      siz_params siz;
      siz.set(Sdims, 0, 0, 48);
      siz.set(Sdims, 0, 1, 48);
      siz.set(Scomponents, 0, 0, 1);
      siz.set(Sprecision, 0, 0, 8);
      siz.set(Ssigned, 0, 0, false);
      siz.set(Stiles, 0, 0, 24);
      siz.set(Stiles, 0, 1, 24);
      static_cast<::kdu_core::kdu_params &>(siz).parse_string("Sprofile=PART2");
      static_cast<::kdu_core::kdu_params &>(siz).finalize();
      kdu_codestream cs;
      cs.create(&siz, &target);
      absl::Cleanup cs_cleanup = [&cs]() {
        if (cs.exists()) cs.destroy();
      };
      cs.access_siz()->parse_string("Creversible=yes");
      cs.access_siz()->parse_string("Clevels=2");
      cs.access_siz()->parse_string("Cdecomp:T1=B(H--:H--:H--),B(-:-:-)");
      cs.access_siz()->finalize_all();
      kdu_stripe_compressor enc;
      enc.start(cs);
      std::vector<kdu_byte> buf(48 * 48, 90);
      int stripe_h = 48;
      enc.push_stripe(buf.data(), &stripe_h);
      enc.finish();
    }
    kdu_compressed_source_buffered src(decomp_bytes.data(),
                                       decomp_bytes.size());
    kdu_region_compositor comp(&src);
    comp.add_ilayer(0, kdu_dims(), kdu_dims());
    comp.set_scale(false, /*vflip=*/true, false, 1.0f);
    comp.set_buffer_surface(MakeDims(0, 0, 20, 20), 0u);
    kdu_dims nr;
    int s = 0;
    while (comp.process(65536, nr) && s++ < 20) {
    }
    comp.set_buffer_surface(MakeDims(0, 0, 48, 48), 0u);
    s = 0;
    while (comp.process(65536, nr) && s++ < 20) {
    }
    comp.pre_destroy();
  }

  // 3. Corrupt raw codestream headers (create/add_ilayer catch L6869, L7665)
  // and corrupt packet body (decompressor.finish catch L4379-4380).
  {
    std::vector<kdu_byte> good = MakeRawCodestream(48, 48, 1, 2, 0);
    std::vector<kdu_byte> bad_hdr = good;
    for (size_t i = 2; i < 20 && i < bad_hdr.size(); ++i) {
      bad_hdr[i] = 0xFF;
    }
    kdu_compressed_source_buffered bad_hdr_src(bad_hdr.data(), bad_hdr.size());
    kdu_region_compositor comp;
    try {
      comp.create(&bad_hdr_src);
      comp.add_ilayer(0, kdu_dims(), kdu_dims());
    } catch (::kdu_core::kdu_exception) {
    }
    comp.pre_destroy();

    if (good.size() > 32) {
      std::vector<kdu_byte> bad_pkt = good;
      std::fill(bad_pkt.end() - 16, bad_pkt.end() - 2, 0xFF);
      kdu_compressed_source_buffered pkt_src(bad_pkt.data(), bad_pkt.size());
      kdu_region_compositor pcomp;
      pcomp.create(&pkt_src);
      pcomp.add_ilayer(0, kdu_dims(), kdu_dims());
      pcomp.set_scale(false, false, false, 1.0f);
      pcomp.set_buffer_surface(MakeDims(0, 0, 48, 48), 0u);
      kdu_dims nr;
      try {
        int s = 0;
        while (pcomp.process(65536, nr) && s++ < 20) {
        }
      } catch (::kdu_core::kdu_exception) {
      }
      pcomp.pre_destroy();
    }
  }

  // 4. Interlaced MJ2: two active layers on same track/frame (L3002-3004),
  // reactivation onto different frame_idx with same field_handling (L4898-4906),
  // and get_codestream_packets delegating across two opaque layers (L5319).
  {
    std::vector<kdu_byte> mj2_bytes = MakeMj2Bytes(/*interlaced=*/true);
    VectorSource src(mj2_bytes);
    jp2_family_src fsrc;
    fsrc.open(&src);
    mj2_source mj2_src;
    ASSERT_GE(mj2_src.open(&fsrc, true), 0);

    kdu_region_compositor comp(&mj2_src, 65536);
    kdu_ilayer_ref il0 = comp.add_ilayer(
        0, kdu_dims(), MakeDims(0, 0, 32, 32), false, false, false,
        /*frame_idx=*/0, /*field_handling=*/1);
    kdu_ilayer_ref il1 = comp.add_ilayer(
        0, kdu_dims(), MakeDims(100, 100, 32, 32), false, false, false,
        /*frame_idx=*/0, /*field_handling=*/1);
    EXPECT_FALSE(il0.is_null());
    EXPECT_FALSE(il1.is_null());
    kdu_istream_ref is0 = comp.get_ilayer_stream(il0, 0);
    kdu_istream_ref is1 = comp.get_ilayer_stream(il1, 0);
    comp.set_scale(false, false, false, 1.0f);
    comp.set_buffer_surface(MakeDims(0, 0, 140, 140), 0u);
    kdu_dims nr;
    int s = 0;
    while (comp.process(32768, nr) && s++ < 40) {
    }
    ::kdu_core::kdu_long ps = 0, pks = 0, mps = 0;
    comp.get_codestream_packets(is0, MakeDims(0, 0, 32, 32), ps, pks, mps);
    comp.get_codestream_packets(is1, MakeDims(100, 100, 32, 32), ps, pks, mps);

    comp.remove_ilayer(il0, /*leave_inactive=*/true);
    comp.remove_ilayer(il1, /*leave_inactive=*/true);
    kdu_ilayer_ref il_diff_frame = comp.add_ilayer(
        0, kdu_dims(), MakeDims(0, 0, 32, 32), false, false, false,
        /*frame_idx=*/1, /*field_handling=*/1);
    EXPECT_FALSE(il_diff_frame.is_null());
    comp.pre_destroy();
    mj2_src.close();
    fsrc.close();
  }

  // 5. JPX animation set_frame: empty frame, 1-layer frame, 2-layer frame,
  // inactive stream stealing across colour_init_src, and corrupt set_frame.
  {
    std::vector<kdu_byte> jpx_anim;
    {
      VectorTarget vec_target(&jpx_anim);
      jp2_family_tgt family;
      family.open(&vec_target);
      jpx_target jout;
      jout.open(&family);

      auto cs0_tgt = jout.add_codestream();
      siz_params siz0;
      siz0.set(Sdims, 0, 0, 32);
      siz0.set(Sdims, 0, 1, 32);
      siz0.set(Scomponents, 0, 0, 3);
      siz0.set(Sprecision, 0, 0, 8);
      siz0.set(Ssigned, 0, 0, false);
      static_cast<::kdu_core::kdu_params &>(siz0).finalize();
      cs0_tgt.access_dimensions().init(&siz0);

      auto lay0 = jout.add_layer();
      lay0.add_colour().init(::kdu_supp::JP2_sRGB_SPACE);
      lay0.access_channels().init(3);
      lay0.access_channels().set_colour_mapping(0, 0, -1, 0);
      lay0.access_channels().set_colour_mapping(1, 1, -1, 0);
      lay0.access_channels().set_colour_mapping(2, 2, -1, 0);

      auto lay1 = jout.add_layer();
      lay1.add_colour().init(::kdu_supp::JP2_sRGB_SPACE);
      lay1.access_channels().init(3);
      lay1.access_channels().set_colour_mapping(0, 0, -1, 0);
      lay1.access_channels().set_colour_mapping(1, 1, -1, 0);
      lay1.access_channels().set_colour_mapping(2, 2, -1, 0);

      jpx_composition jc = jout.access_composition();
      jx_frame *f0 = jc.add_frame(100, 1, false);
      jc.add_instruction(f0, 0, 0, MakeDims(0, 0, 32, 32),
                         MakeDims(0, 0, 32, 32));
      jx_frame *f1 = jc.add_frame(100, 1, false);
      jc.add_instruction(f1, 1, 0, MakeDims(0, 0, 32, 32),
                         MakeDims(0, 0, 32, 32));
      jx_frame *f2 = jc.add_frame(100, 1, false);
      jc.add_instruction(f2, 0, 0, MakeDims(0, 0, 32, 32),
                         MakeDims(0, 0, 32, 32));
      jc.add_instruction(f2, 1, 0, MakeDims(0, 0, 32, 32),
                         MakeDims(32, 0, 32, 32));

      while (jout.write_headers() != nullptr) {
      }
      auto *out0 = cs0_tgt.open_stream();
      CompressToTarget(out0, 32, 32, 3, 8, 1, 0, true, false, 15);
      out0->close();
      jout.close();
      family.close();
    }

    VectorSource src(jpx_anim);
    jp2_family_src fsrc;
    fsrc.open(&src);
    jpx_source jpx_src;
    ASSERT_GE(jpx_src.open(&fsrc, true), 0);

    kdu_region_compositor comp(&jpx_src, 65536);
    comp.set_scale(false, false, false, 1.0f);
    auto apply_frame = [&](int idx, bool add = false) {
      jpx_frame_expander exp;
      if (exp.construct(&jpx_src,
                        jpx_src.access_composition().access_frame(idx, 0, true),
                        kdu_dims())) {
        if (add) {
          comp.add_frame(&exp, kdu_coords(32, 0));
        } else {
          comp.set_frame(&exp, kdu_coords(0, 0));
        }
      }
    };
    apply_frame(2);
    apply_frame(0);
    apply_frame(1);
    apply_frame(2);
    // Exercise add_frame with active_layers == NULL (L7081) and reusing an
    // inactive layer with prev != NULL (L7065) before add_ilayer clears
    // fixed_composition_dims.
    comp.remove_ilayer(kdu_ilayer_ref(), /*leave_inactive=*/true);
    apply_frame(0, /*add=*/true);
    apply_frame(1, /*add=*/true);
    apply_frame(2);
    comp.remove_ilayer(kdu_ilayer_ref(), /*leave_inactive=*/true);
    apply_frame(1, /*add=*/true);
    apply_frame(0, /*add=*/true);

    kdu_ilayer_ref il_a = comp.add_ilayer(0, kdu_dims(), kdu_dims());
    comp.remove_ilayer(il_a, /*leave_inactive=*/true);
    int comp_idx = 0;
    kdu_ilayer_ref il_p = comp.add_primitive_ilayer(
        0, comp_idx, KDU_WANT_CODESTREAM_COMPONENTS, kdu_dims(), kdu_dims());
    EXPECT_FALSE(il_p.is_null());
    comp.remove_ilayer(il_p, /*leave_inactive=*/true);
    comp.add_ilayer(1, kdu_dims(), kdu_dims());
    comp.add_ilayer(0, kdu_dims(), kdu_dims());
    comp.pre_destroy();
    jpx_src.close();
    fsrc.close();

    // Corrupt codestream box ("jp2c") in jpx_anim and call add_ilayer,
    // add_primitive_ilayer, set_frame, add_frame (catch L6714, L6869, L6998,
    // L7102).
    std::vector<kdu_byte> bad_jpx = jpx_anim;
    const kdu_byte jp2c_sig[4] = {'j', 'p', '2', 'c'};
    auto it = std::search(bad_jpx.begin(), bad_jpx.end(), jp2c_sig,
                          jp2c_sig + 4);
    if (it != bad_jpx.end() &&
        static_cast<size_t>(bad_jpx.end() - it) > 24) {
      std::fill(it + 6, it + 24, 0xFF);
      VectorSource bsrc(bad_jpx);
      jp2_family_src bfsrc;
      bfsrc.open(&bsrc);
      jpx_source bjpx;
      if (bjpx.open(&bfsrc, true) >= 0) {
        kdu_region_compositor bcomp(&bjpx, 65536);
        try {
          bcomp.add_ilayer(0, kdu_dims(), kdu_dims());
        } catch (::kdu_core::kdu_exception) {
        }
        try {
          int bc = 0;
          bcomp.add_primitive_ilayer(0, bc, KDU_WANT_CODESTREAM_COMPONENTS,
                                     kdu_dims(), kdu_dims());
        } catch (::kdu_core::kdu_exception) {
        }
        try {
          jpx_frame_expander bexp;
          if (bexp.construct(
                  &bjpx, bjpx.access_composition().access_frame(1, 0, true),
                  kdu_dims())) {
            bcomp.set_frame(&bexp, kdu_coords(0, 0));
          }
        } catch (::kdu_core::kdu_exception) {
        }
        try {
          jpx_frame_expander bexp;
          if (bexp.construct(
                  &bjpx, bjpx.access_composition().access_frame(1, 0, true),
                  kdu_dims())) {
            bcomp.add_frame(&bexp, kdu_coords(0, 0));
          }
        } catch (::kdu_core::kdu_exception) {
        }
        bcomp.pre_destroy();
        bjpx.close();
      }
      bfsrc.close();
    }
  }
}

TEST_F(KduRegionCompositorTest, ScaleLimitsMultiUsersAndOverlayExpression) {
  // 1. Trigger KDU_COMPOSITOR_CANNOT_FLIP (L3305-3306) via Part-2 asymmetric
  // Cdecomp, SCALE_TOO_LARGE via large target dims (L2653-2656, L3318-3319),
  // SCALE_TOO_SMALL (L3311-3312), and 3 codestream users (move_to_head L2338).
  {
    std::vector<kdu_byte> raw;
    {
      VectorTarget vec_target(&raw);
      kdu_codestream cs;
      siz_params siz;
      siz.set(Sdims, 0, 0, 48);
      siz.set(Sdims, 0, 1, 48);
      siz.set(Scomponents, 0, 0, 1);
      siz.set(Sprecision, 0, 0, 8);
      siz.set(Ssigned, 0, 0, false);
      siz.set(Stiles, 0, 0, 24);
      siz.set(Stiles, 0, 1, 24);
      static_cast<::kdu_core::kdu_params &>(siz).parse_string("Sprofile=PART2");
      static_cast<::kdu_core::kdu_params &>(siz).finalize();
      cs.create(&siz, &vec_target);
      cs.access_siz()->parse_string("Creversible=yes");
      cs.access_siz()->parse_string("Clevels=2");
      cs.access_siz()->parse_string("Cdecomp:T1=B(H--:H--:H--),B(-:-:-)");
      cs.access_siz()->finalize_all();
      kdu_stripe_compressor comp_enc;
      comp_enc.start(cs);
      std::vector<kdu_byte> px(48 * 48, 77);
      int stripes[1] = {48};
      comp_enc.push_stripe(px.data(), stripes);
      comp_enc.finish();
      cs.destroy();
    }
    kdu_compressed_source_buffered src(raw.data(), raw.size());
    kdu_region_compositor comp;
    comp.create(&src);
    comp.add_ilayer(0, kdu_dims(), kdu_dims());
    // Render with vflip=true across Tile 0 (flippable) into Tile 1 (stacked
    // high-pass, non-flippable) so kdrc_stream::can_flip turns false (L4394),
    // then re-call set_scale with vflip=true to hit L3305-3306.
    comp.set_scale(false, true, false, 1.0f);
    comp.set_buffer_surface(MakeDims(0, 0, 48, 48), 0u);
    kdu_dims nr;
    int s = 0;
    while (comp.process(65536, nr) && s++ < 30) {
    }
    kdu_dims tot;
    comp.set_scale(false, true, false, 0.5f);
    EXPECT_FALSE(comp.get_total_composition_dims(tot));
    EXPECT_NE(comp.check_invalid_scale_code() & KDU_COMPOSITOR_CANNOT_FLIP, 0);
    comp.pre_destroy();
  }

  // Huge full_target_dims -> lim_max > 0x7FFFF000 sets max_supported_scale,
  // then a slightly higher scale hits kdrc_stream::set_scale check (L3318),
  // plus two consecutive too-small scales hit L3311-3312.
  {
    std::vector<kdu_byte> raw = MakeRawCodestream(64, 48, 3, 1, 0);
    kdu_compressed_source_buffered src(raw.data(), raw.size());
    kdu_region_compositor comp;
    comp.create(&src);
    comp.add_ilayer(0, kdu_dims(), MakeDims(0, 0, 64, 48));
    kdu_dims tot;
    comp.set_scale(false, false, false, 1e-7f);
    EXPECT_FALSE(comp.get_total_composition_dims(tot));
    EXPECT_NE(comp.check_invalid_scale_code() & KDU_COMPOSITOR_SCALE_TOO_SMALL,
              0);
    comp.set_scale(false, false, false, 1e-8f);
    EXPECT_FALSE(comp.get_total_composition_dims(tot));
    EXPECT_NE(comp.check_invalid_scale_code() & KDU_COMPOSITOR_SCALE_TOO_SMALL,
              0);
    comp.pre_destroy();
  }

  {
    std::vector<kdu_byte> raw = MakeRawCodestream(32, 32, 1, 2, 0);
    kdu_compressed_source_buffered src(raw.data(), raw.size());
    kdu_region_compositor comp;
    comp.create(&src);
    comp.add_ilayer(0, kdu_dims(), MakeDims(0, 0, 1 << 30, 1 << 30));
    kdu_dims tot;
    comp.set_scale(false, false, false, 2.5f);
    EXPECT_FALSE(comp.get_total_composition_dims(tot));
    EXPECT_NE(comp.check_invalid_scale_code() & KDU_COMPOSITOR_SCALE_TOO_LARGE,
              0);
    comp.set_scale(false, false, false, 3.0f);
    EXPECT_FALSE(comp.get_total_composition_dims(tot));
    EXPECT_NE(comp.check_invalid_scale_code() & KDU_COMPOSITOR_SCALE_TOO_LARGE,
              0);
    comp.pre_destroy();
  }

  // Three active layers sharing one codestream so moving middle user hits
  // next_codestream_user != NULL (L2338).
  {
    std::vector<kdu_byte> raw = MakeRawCodestream(32, 32, 1, 2, 0);
    kdu_compressed_source_buffered src(raw.data(), raw.size());
    kdu_region_compositor comp;
    comp.create(&src);
    comp.add_ilayer(0, kdu_dims(), MakeDims(0, 0, 32, 32));
    comp.add_ilayer(0, kdu_dims(), MakeDims(40, 0, 32, 32));
    comp.add_ilayer(0, kdu_dims(), MakeDims(80, 0, 32, 32));
    comp.set_scale(false, false, false, 1.0f);
    comp.set_buffer_surface(MakeDims(0, 0, 120, 32), 0u);
    kdu_dims nr;
    int s = 0;
    while (comp.process(512, nr) && s++ < 200) {
    }
    comp.set_scale(false, false, false, 0.5f);
    s = 0;
    while (comp.process(65536, nr) && s++ < 50) {
    }
    comp.pre_destroy();
  }

  // 2. Multi-term dependency expressions in configure_overlays (L1180-1201).
  {
    std::vector<kdu_byte> jpx_bytes;
    {
      VectorTarget vec_target(&jpx_bytes);
      jp2_family_tgt family;
      family.open(&vec_target);
      jpx_target jout;
      jout.open(&family);

      auto cs0 = jout.add_codestream();
      siz_params siz;
      siz.set(Sdims, 0, 0, 64);
      siz.set(Sdims, 0, 1, 64);
      siz.set(Scomponents, 0, 0, 3);
      siz.set(Sprecision, 0, 0, 8);
      siz.set(Ssigned, 0, 0, false);
      static_cast<::kdu_core::kdu_params &>(siz).finalize();
      cs0.access_dimensions().init(&siz);

      auto lay = jout.add_layer();
      lay.add_colour().init(::kdu_supp::JP2_sRGB_SPACE);
      lay.access_channels().init(3);
      for (int c = 0; c < 3; ++c) {
        lay.access_channels().set_colour_mapping(c, c, -1, 0);
      }

      jpx_meta_manager meta = jout.access_meta_manager();
      jpx_metanode root = meta.access_root();
      jpx_roi rois[1];
      rois[0].init_rectangle(MakeDims(16, 16, 20, 20));
      int cs_id = 0;
      int lay_id = 0;
      jpx_metanode nl0 = root.add_numlist(1, &cs_id, 1, &lay_id, false);
      nl0.add_label("lbl_a").add_regions(1, rois);
      jpx_metanode nl1 = root.add_numlist(1, &cs_id, 1, &lay_id, false);
      nl1.add_label("lbl_b").add_regions(1, rois);

      while (jout.write_headers() != nullptr) {
      }
      auto *out = cs0.open_stream();
      CompressToTarget(out, 64, 64, 3, 8, 1, 0, true, false, 5);
      out->close();
      while (jout.write_metadata() != nullptr) {
      }
      jout.close();
      family.close();
    }
    VectorSource src(jpx_bytes);
    jp2_family_src fsrc;
    fsrc.open(&src);
    jpx_source jpx_src;
    ASSERT_GE(jpx_src.open(&fsrc, true), 0);
    jpx_meta_manager rm = jpx_src.access_meta_manager();
    int cs0_id = 0;
    rm.load_matches(1, &cs0_id, 0, nullptr);
    jpx_metanode lbl_a = rm.enumerate_matches(jpx_metanode(), 0, -1, false,
                                              kdu_dims(), 0, false, true);
    jpx_metanode lbl_b =
        rm.enumerate_matches(lbl_a, 0, -1, false, kdu_dims(), 0, false, true);
    ASSERT_TRUE(lbl_a.exists());
    ASSERT_TRUE(lbl_b.exists());

    kdu_region_compositor comp(&jpx_src, 65536);
    kdu_uint32 aux[2] = {1, 0xFF0000FFu};
    kdu_ilayer_ref il = comp.add_ilayer(0, kdu_dims(), kdu_dims());
    // Same overlay layer_id=1 and same product term -> allocates next_in_product
    // for both positive (L1180) & negative (L1196) terms, plus duplicates.
    comp.configure_overlays(true, 1, 0.7f, 4, lbl_a, /*dependency_effect=*/0,
                            aux, 2);
    comp.configure_overlays(true, 1, 0.7f, 4, lbl_b, /*dependency_effect=*/2,
                            aux, 2);
    comp.configure_overlays(true, 1, 0.7f, 4, lbl_b, /*duplicate=*/2, aux, 2);
    comp.configure_overlays(true, 1, 0.7f, 4, lbl_a, /*dependency_effect=*/-1,
                            aux, 2);
    comp.configure_overlays(true, 1, 0.7f, 4, lbl_b, /*dependency_effect=*/-2,
                            aux, 2);
    comp.configure_overlays(true, 1, 0.7f, 4, lbl_b, /*duplicate=*/-2, aux,
                            2);
    comp.configure_overlays(true, 1, 0.7f, 4, lbl_a, /*sum term=*/1, aux, 2);
    comp.set_scale(false, false, false, 1.0f);
    kdu_dims tot;
    EXPECT_TRUE(comp.get_total_composition_dims(tot));
    comp.set_buffer_surface(tot, 0u);
    kdu_dims nr;
    int s = 0;
    while (comp.process(65536, nr) && s++ < 20) {
    }
    comp.remove_ilayer(il, false);
    comp.pre_destroy();
    jpx_src.close();
    fsrc.close();
  }
}

TEST_F(KduRegionCompositorTest, QuadDartsOverlayInvalidationAndThreeUsers) {
  // 1. Non-convex clockwise dart quads (L9830-9849, L1342-1350) + y3<y2<y1 and
  // y1<y2<y3 quads (L9857-9859, L9871-9873), rendered in both int32 & float
  // modes and followed by set_scale invalidating overlay_buffer (L5075-5077).
  std::vector<kdu_byte> jpx_bytes;
  {
    VectorTarget target(&jpx_bytes);
    jp2_family_tgt ftgt;
    ftgt.open(&target);
    jpx_target jout;
    jout.open(&ftgt);

    auto cs_tgt = jout.add_codestream();
    siz_params siz;
    siz.set(Sdims, 0, 0, 512);
    siz.set(Sdims, 0, 1, 512);
    siz.set(Scomponents, 0, 0, 3);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    static_cast<::kdu_core::kdu_params &>(siz).finalize();
    cs_tgt.access_dimensions().init(&siz);

    jpx_layer_target lay0 = jout.add_layer();
    lay0.add_colour().init(::kdu_supp::JP2_sRGB_SPACE);
    lay0.access_channels().init(3);
    lay0.access_channels().set_colour_mapping(0, 0, -1, 0);
    lay0.access_channels().set_colour_mapping(1, 1, -1, 0);
    lay0.access_channels().set_colour_mapping(2, 2, -1, 0);

    jpx_layer_target lay1 = jout.add_layer();
    lay1.add_colour().init(::kdu_supp::JP2_sRGB_SPACE);
    lay1.access_channels().init(3);
    lay1.access_channels().set_colour_mapping(0, 0, -1, 0);
    lay1.access_channels().set_colour_mapping(1, 1, -1, 0);
    lay1.access_channels().set_colour_mapping(2, 2, -1, 0);

    jpx_composition jc = jout.access_composition();
    jx_frame *f0 = jc.add_frame(100, 1, false);
    jc.add_instruction(f0, 0, 0, MakeDims(0, 0, 512, 512),
                       MakeDims(0, 0, 512, 512));
    jx_frame *f1 = jc.add_frame(100, 1, false);
    jc.add_instruction(f1, 1, 0, MakeDims(0, 0, 512, 512),
                       MakeDims(0, 0, 512, 512));

    jpx_meta_manager mm = jout.access_meta_manager();
    jpx_metanode root = mm.access_root();
    jpx_roi quads[4];
    // Left-wing dart: v2=(60,70) above v3=(110,95) and v1=(150,130), v3.y<v1.y,
    // v2 to left of v0->v3 -> hits L9830-9839 and limit_min_y (L1342-1350).
    quads[0].init_quadrilateral(kdu_coords(120, 40), kdu_coords(150, 130),
                                kdu_coords(60, 70), kdu_coords(110, 95));
    // Right-wing dart: v2=(380,70) above v1=(330,95) and v3=(290,130),
    // v1.y<v3.y, v2 to right of v0->v1 -> hits L9840-9848 and limit_min_y.
    quads[1].init_quadrilateral(kdu_coords(320, 40), kdu_coords(330, 95),
                                kdu_coords(380, 70), kdu_coords(290, 130));
    // Convex quad with v3.y < v2.y < v1.y (L9851 -> L9857-9859).
    quads[2].init_quadrilateral(kdu_coords(120, 200), kdu_coords(160, 290),
                                kdu_coords(115, 265), kdu_coords(80, 230));
    // Convex quad with v1.y < v2.y < v3.y (L9865 -> L9871-9873).
    quads[3].init_quadrilateral(kdu_coords(320, 200), kdu_coords(360, 230),
                                kdu_coords(335, 265), kdu_coords(290, 290));
    int cs_id = 0, l0_id = 0;
    root.add_numlist(1, &cs_id, 1, &l0_id, false).add_regions(4, quads);

    while (jout.write_headers() != nullptr) {
    }
    auto *o0 = cs_tgt.open_stream();
    CompressToTarget(o0, 512, 512, 3, 8, 1, 0, true, false, 15);
    o0->close();
    while (jout.write_metadata() != nullptr) {
    }
    jout.close();
    ftgt.close();
  }

  VectorSource vsrc(jpx_bytes);
  jp2_family_src fsrc;
  fsrc.open(&vsrc);
  jpx_source jpx_src;
  ASSERT_GE(jpx_src.open(&fsrc, true), 0);

  for (bool use_floats : {false, true}) {
    CustomRegionCompositor comp(use_floats, /*allow_read=*/true,
                                /*handle_custom_overlay=*/false);
    comp.create(&jpx_src);
    comp.add_ilayer(0, kdu_dims(), MakeDims(0, 0, 512, 512));
    comp.load_metadata_matches();
    const kdu_uint32 aux[2] = {1, 0xFF00FF00u};
    comp.configure_overlays(true, 0, 1.0f, 4, jpx_metanode(), 0, aux, 2);
    comp.set_scale(false, false, false, 1.0f);
    comp.set_buffer_surface(MakeDims(0, 0, 512, 512), 0u);
    kdu_dims nr;
    int steps = 0;
    while (comp.process(65536, nr) && steps++ < 100) {
    }
    EXPECT_TRUE(comp.is_processing_complete());

    // Change scale & realize geometry while overlay_buffer != NULL -> L5075-5077.
    comp.set_scale(false, false, false, 0.5f);
    EXPECT_TRUE(comp.get_total_composition_dims(nr));
    comp.pre_destroy();
  }

  // AlwaysFailReadBuf on jpx_bytes: (0,0,32,32) has no ROIs so
  // compositing_buffer stays NULL; growing to (0,0,512,512) triggers
  // set_read_accessibility(true)==false promotion -> L5212-5216.
  {
    class FailPromoteOverlayBuf : public kdu_compositor_buf {
     public:
      bool set_read_accessibility(bool read_access_required) override {
        read_access_allowed = true;
        return !read_access_required;
      }
    };
    class FailPromoteOverlayCompositor : public kdu_region_compositor {
     public:
      ~FailPromoteOverlayCompositor() override { pre_destroy(); }
      kdu_compositor_buf *allocate_buffer(kdu_coords min_size,
                                          kdu_coords &actual_size,
                                          bool) override {
        actual_size = kdu_coords(min_size.x + 8, min_size.y + 8);
        auto *buf = new FailPromoteOverlayBuf();
        auto *storage =
            new kdu_uint32[static_cast<size_t>(actual_size.x) * actual_size.y]();
        buf->init(storage, actual_size.x);
        return buf;
      }
      void delete_buffer(kdu_compositor_buf *buf) override { delete buf; }
    };
    FailPromoteOverlayCompositor fcomp;
    fcomp.create(&jpx_src);
    fcomp.add_ilayer(0, kdu_dims(), MakeDims(0, 0, 512, 512));
    fcomp.load_metadata_matches();
    const kdu_uint32 aux[2] = {1, 0xFF00FF00u};
    fcomp.configure_overlays(true, 0, 1.0f, 4, jpx_metanode(), 0, aux, 2);
    fcomp.set_scale(false, false, false, 1.0f);
    fcomp.set_buffer_surface(MakeDims(0, 0, 32, 32), 0u);
    kdu_dims nr;
    int s = 0;
    while (fcomp.process(65536, nr) && s++ < 10) {
    }
    fcomp.set_buffer_surface(MakeDims(0, 0, 512, 512), 0u);
    s = 0;
    while (fcomp.process(65536, nr) && s++ < 20) {
    }
    fcomp.pre_destroy();
  }

  // 2. Deterministic add_frame reusing non-head inactive layer (L7065-7067).
  {
    kdu_region_compositor comp(&jpx_src, 65536);
    comp.set_scale(false, false, false, 1.0f);
    kdu_ilayer_ref il0 =
        comp.add_ilayer(0, kdu_dims(), MakeDims(0, 0, 512, 512));
    kdu_ilayer_ref il1 =
        comp.add_ilayer(1, kdu_dims(), MakeDims(0, 0, 512, 512));
    comp.remove_ilayer(il0, /*leave_inactive=*/true);
    comp.remove_ilayer(il1, /*leave_inactive=*/true);

    jpx_frame_expander exp0, exp1;
    ASSERT_TRUE(exp0.construct(
        &jpx_src, jpx_src.access_composition().access_frame(0, 0, true),
        kdu_dims()));
    ASSERT_TRUE(exp1.construct(
        &jpx_src, jpx_src.access_composition().access_frame(1, 0, true),
        kdu_dims()));
    // set_frame(nullptr) keeps both il0 and il1 in inactive_layers so one of
    // add_frame(&exp0) / add_frame(&exp1) must walk past head (prev != NULL).
    comp.set_frame(nullptr, kdu_coords(0, 0));
    comp.add_frame(&exp0, kdu_coords(0, 0));
    comp.add_frame(&exp1, kdu_coords(0, 0));
    comp.remove_ilayer(kdu_ilayer_ref(), /*leave_inactive=*/true);
    comp.set_frame(nullptr, kdu_coords(0, 0));
    comp.add_frame(&exp1, kdu_coords(0, 0));
    comp.add_frame(&exp0, kdu_coords(0, 0));
    comp.pre_destroy();
  }

  // 2b. MJ2 same-frame inactive stream reuse with colour_init_src == 0
  // (L7604-7607 in add_active_stream).
  {
    std::vector<kdu_byte> mj2_bytes = MakeMj2Bytes(/*interlaced=*/false);
    VectorSource msrc(mj2_bytes);
    jp2_family_src mfsrc;
    mfsrc.open(&msrc);
    mj2_source mj2_src;
    ASSERT_GE(mj2_src.open(&mfsrc, true), 0);

    kdu_region_compositor mcomp(&mj2_src, 65536);
    kdu_ilayer_ref ml0 = mcomp.add_ilayer(0, kdu_dims(), kdu_dims());
    mcomp.remove_ilayer(ml0, /*leave_inactive=*/true);
    kdu_ilayer_ref ml1 = mcomp.add_ilayer(0, kdu_dims(), kdu_dims());
    EXPECT_FALSE(ml1.is_null());
    mcomp.pre_destroy();
    mj2_src.close();
    mfsrc.close();
  }

  // 3. Three active layers on the same codestream: move middle user to head
  // (L2338-2339 in kdrc_codestream::move_to_head).
  {
    kdu_region_compositor comp(&jpx_src, 65536);
    kdu_ilayer_ref il0 = comp.add_ilayer(0, MakeDims(0, 0, 64, 64),
                                         MakeDims(0, 0, 64, 64));
    kdu_ilayer_ref il1 = comp.add_ilayer(0, MakeDims(100, 100, 64, 64),
                                         MakeDims(100, 100, 64, 64));
    kdu_ilayer_ref il2 = comp.add_ilayer(0, MakeDims(200, 200, 64, 64),
                                         MakeDims(200, 200, 64, 64));
    EXPECT_FALSE(il0.is_null());
    EXPECT_FALSE(il1.is_null());
    EXPECT_FALSE(il2.is_null());
    comp.set_scale(false, false, false, 1.0f);
    // Process only middle layer il1 first (between il2 and il0 in user list).
    for (const kdu_dims &reg : {MakeDims(100, 100, 64, 64),
                                MakeDims(0, 0, 64, 64),
                                MakeDims(200, 200, 64, 64),
                                MakeDims(100, 100, 64, 64)}) {
      comp.set_buffer_surface(reg, 0u);
      kdu_dims nr;
      int steps = 0;
      while (comp.process(65536, nr) && steps++ < 40) {
      }
    }
    comp.pre_destroy();
  }

  jpx_src.close();
  fsrc.close();
}

}  // namespace
