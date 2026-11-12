// Unit tests for
// third_party/kakadu/vendor/coresys/transform/multi_transform.cpp.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_params.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_roi_processing.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_sample_processing.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_threads.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

namespace kdu_core {
namespace {

class ThrowingErrorHandler final : public kdu_thread_safe_message {
 public:
  ~ThrowingErrorHandler() noexcept override = default;

  void set_quiet(bool quiet) { quiet_.store(quiet, std::memory_order_relaxed); }

  void put_text(const char* string) override {
    if (!quiet_.load(std::memory_order_relaxed)) {
      std::fputs(string, stderr);
    }
  }

  void flush(bool end_of_message = false) override {
    kdu_thread_safe_message::flush(end_of_message);
    if (end_of_message) {
      throw static_cast<kdu_exception>(KDU_ERROR_EXCEPTION);
    }
  }

 private:
  std::atomic<bool> quiet_{false};
};

ThrowingErrorHandler* GetErrorHandler() {
  static auto* const handler = new ThrowingErrorHandler();
  return handler;
}

class MultiTransformTestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override { kdu_customize_errors(GetErrorHandler()); }
};

const ::testing::Environment* const kMultiTransformEnv =
    ::testing::AddGlobalTestEnvironment(new MultiTransformTestEnvironment());

class VectorTarget final : public kdu_compressed_target {
 public:
  explicit VectorTarget(std::vector<kdu_byte>& out) : out_(out) {}

  bool write(const kdu_byte* buf, int num_bytes) override {
    out_.insert(out_.end(), buf, buf + num_bytes);
    return true;
  }

 private:
  std::vector<kdu_byte>& out_;
};

class VectorSource final : public kdu_compressed_source {
 public:
  explicit VectorSource(const std::vector<kdu_byte>& data) : data_(data) {}

  int get_capabilities() override {
    return KDU_SOURCE_CAP_SEQUENTIAL | KDU_SOURCE_CAP_SEEKABLE;
  }

  int read(kdu_byte* buf, int num_bytes) override {
    if (num_bytes <= 0 || pos_ >= data_.size()) {
      return 0;
    }
    const size_t avail =
        std::min(static_cast<size_t>(num_bytes), data_.size() - pos_);
    std::memcpy(buf, data_.data() + pos_, avail);
    pos_ += avail;
    return static_cast<int>(avail);
  }

  bool seek(kdu_long offset) override {
    if (offset < 0) {
      return false;
    }
    pos_ = std::min(static_cast<size_t>(offset), data_.size());
    return true;
  }

  kdu_long get_pos() override { return static_cast<kdu_long>(pos_); }

 private:
  const std::vector<kdu_byte>& data_;
  size_t pos_ = 0;
};

struct ScopedCodestream {
  siz_params siz;
  kdu_codestream codestream;

  ~ScopedCodestream() {
    if (codestream.exists()) {
      codestream.destroy();
    }
  }
};

struct ScopedTile {
  kdu_tile tile;
  kdu_thread_env* env = nullptr;

  ~ScopedTile() {
    if (tile.exists()) {
      tile.close(env);
    }
  }
};

struct ScopedMultiAnalysis {
  kdu_multi_analysis engine;
  kdu_thread_env* env = nullptr;

  ~ScopedMultiAnalysis() {
    if (engine.exists()) {
      engine.destroy(env);
    }
  }
};

struct ScopedMultiSynthesis {
  kdu_multi_synthesis engine;
  kdu_thread_env* env = nullptr;

  ~ScopedMultiSynthesis() {
    if (engine.exists()) {
      engine.destroy(env);
    }
  }
};

struct ScopedThreadEnv {
  kdu_thread_env env;
  bool created = false;

  void Create(int total_threads) {
    env.create();
    created = true;
    for (int i = 1; i < total_threads; ++i) {
      EXPECT_TRUE(env.add_thread());
    }
  }

  ~ScopedThreadEnv() {
    if (created && env.exists()) {
      env.destroy();
    }
  }
};

class ConstRoiImage;

class ConstRoiNode final : public kdu_roi_node {
 public:
  explicit ConstRoiNode(ConstRoiImage* owner) : owner_(owner) {}
  void pull(kdu_byte buf[], int width) override {
    std::fill(buf, buf + width, 255);
  }
  void release() override;
  void DetachOwner() { owner_ = nullptr; }

 private:
  ConstRoiImage* owner_;
};

class ConstRoiImage final : public kdu_roi_image {
 public:
  ~ConstRoiImage() override {
    for (ConstRoiNode* node : active_nodes_) {
      node->DetachOwner();
      delete node;
    }
  }

  kdu_roi_node* acquire_node(int /*component*/,
                             kdu_dims /*tile_region*/) override {
    auto* node = new ConstRoiNode(this);
    active_nodes_.push_back(node);
    return node;
  }

  void ReleaseNode(ConstRoiNode* node) { std::erase(active_nodes_, node); }

 private:
  std::vector<ConstRoiNode*> active_nodes_;
};

void ConstRoiNode::release() {
  if (owner_ != nullptr) {
    owner_->ReleaseNode(this);
  }
  delete this;
}

class DependencyTrackingQueue final : public kdu_thread_queue {
 public:
  bool update_dependencies(kdu_int32 /*new_dependencies*/,
                           kdu_int32 /*delta_max_dependencies*/,
                           kdu_thread_entity* /*caller*/) override {
    updates.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  std::atomic<int> updates{0};
};

struct ComponentSpec {
  int width = 32;
  int height = 24;
  int precision = 8;
  bool is_signed = false;
  int sub_x = 1;
  int sub_y = 1;
};

struct MultiOptions {
  std::vector<ComponentSpec> comps;
  int ox = 0;
  int oy = 0;
  int tile_w = 0;
  int tile_h = 0;
  bool reversible = true;
  bool ycc = true;
  int dwt_levels = 1;
  std::vector<std::string> extra_params;
  int enc_flags = KDU_MULTI_XFORM_DEFAULT_FLAGS;
  int dec_flags = KDU_MULTI_XFORM_DEFAULT_FLAGS;
  int buffer_rows = 1;
  bool use_compat_create = false;
  int num_threads = 0;
  bool use_dep_tracking_queue = false;
  bool use_external_allocator = false;
  bool use_roi = false;
  int frag_width = 0;
  int dec_max_lines = -1;
  int dbuf_rows_override = -1;
  kdu_component_access_mode access_mode = KDU_WANT_OUTPUT_COMPONENTS;
  std::vector<int> comps_of_interest;
};

std::vector<std::vector<int64_t>> MakeMultiImages(
    const std::vector<ComponentSpec>& comps) {
  std::vector<std::vector<int64_t>> images(comps.size());
  for (size_t c = 0; c < comps.size(); ++c) {
    const int w = comps[c].width;
    const int h = comps[c].height;
    const int bits = comps[c].precision;
    const int64_t mod = (int64_t{1} << std::min(bits - 1, 14)) - 1;
    const int64_t dc = comps[c].is_signed ? 0 : (int64_t{1} << (bits - 1));
    images[c].resize(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const int64_t v = ((x * 7 + y * 13 + static_cast<int>(c) * 29) %
                           std::max<int64_t>(mod, 3)) -
                          (mod / 2);
        images[c][static_cast<size_t>(y) * w + x] = dc + v;
      }
    }
  }
  return images;
}

void WriteLineSample(kdu_line_buf* line, const int64_t* src, int width,
                     int precision, bool is_signed) {
  const int64_t dc_offset = is_signed ? 0 : (int64_t{1} << (precision - 1));
  const double inv_scale = 1.0 / static_cast<double>(int64_t{1} << precision);
  if (line->get_buf16() != nullptr) {
    kdu_sample16* buf = line->get_buf16();
    if (line->is_absolute()) {
      for (int x = 0; x < width; ++x) {
        buf[x].ival = static_cast<kdu_int16>(src[x] - dc_offset);
      }
    } else {
      for (int x = 0; x < width; ++x) {
        const double norm = static_cast<double>(src[x] - dc_offset) * inv_scale;
        buf[x].ival =
            static_cast<kdu_int16>(std::lround(norm * (1 << KDU_FIX_POINT)));
      }
    }
  } else if (line->get_buf64() != nullptr) {
    kdu_sample64* buf = line->get_buf64();
    for (int x = 0; x < width; ++x) {
      buf[x].ival = static_cast<kdu_int64>(src[x] - dc_offset);
    }
  } else {
    kdu_sample32* buf = line->get_buf32();
    if (line->is_absolute()) {
      for (int x = 0; x < width; ++x) {
        buf[x].ival = static_cast<kdu_int32>(src[x] - dc_offset);
      }
    } else {
      for (int x = 0; x < width; ++x) {
        buf[x].fval = static_cast<float>(
            static_cast<double>(src[x] - dc_offset) * inv_scale);
      }
    }
  }
}

void ReadLineSample(kdu_line_buf* line, int width, int precision,
                    bool is_signed, std::vector<int64_t>* int_out,
                    std::vector<double>* float_out) {
  const int64_t dc_offset = is_signed ? 0 : (int64_t{1} << (precision - 1));
  const double scale = static_cast<double>(int64_t{1} << precision);
  if (line->get_buf16() != nullptr) {
    const kdu_sample16* buf = line->get_buf16();
    if (line->is_absolute()) {
      for (int x = 0; x < width; ++x) {
        const int64_t val = static_cast<int64_t>(buf[x].ival) + dc_offset;
        int_out->push_back(val);
        float_out->push_back(static_cast<double>(val));
      }
    } else {
      for (int x = 0; x < width; ++x) {
        const double norm = static_cast<double>(buf[x].ival) /
                            static_cast<double>(1 << KDU_FIX_POINT);
        const double val = norm * scale + static_cast<double>(dc_offset);
        float_out->push_back(val);
        int_out->push_back(static_cast<int64_t>(std::llround(val)));
      }
    }
  } else if (line->get_buf64() != nullptr) {
    const kdu_sample64* buf = line->get_buf64();
    for (int x = 0; x < width; ++x) {
      const int64_t val = static_cast<int64_t>(buf[x].ival) + dc_offset;
      int_out->push_back(val);
      float_out->push_back(static_cast<double>(val));
    }
  } else {
    const kdu_sample32* buf = line->get_buf32();
    if (line->is_absolute()) {
      for (int x = 0; x < width; ++x) {
        const int64_t val = static_cast<int64_t>(buf[x].ival) + dc_offset;
        int_out->push_back(val);
        float_out->push_back(static_cast<double>(val));
      }
    } else {
      for (int x = 0; x < width; ++x) {
        const double val = static_cast<double>(buf[x].fval) * scale +
                           static_cast<double>(dc_offset);
        float_out->push_back(val);
        int_out->push_back(static_cast<int64_t>(std::llround(val)));
      }
    }
  }
}

bool IsSizParam(const std::string& p) {
  auto starts_with = [&](const char* prefix) {
    return p.rfind(prefix, 0) == 0;
  };
  return starts_with("S") || starts_with("Mcomponents") ||
         starts_with("Mprecision") || starts_with("Msigned") ||
         starts_with("Ncomponents") || starts_with("Nprecision") ||
         starts_with("Nsigned");
}

void ConfigureEncoderCodestream(const MultiOptions& opts, siz_params* siz) {
  const int num_comps = static_cast<int>(opts.comps.size());
  ASSERT_TRUE(
      siz->parse_string(("Scomponents=" + std::to_string(num_comps)).c_str()));
  int full_w = 0;
  int full_h = 0;
  for (int c = 0; c < num_comps; ++c) {
    full_w = std::max(full_w, opts.comps[c].width * opts.comps[c].sub_x);
    full_h = std::max(full_h, opts.comps[c].height * opts.comps[c].sub_y);
  }
  siz->set(Sorigin, 0, 0, opts.oy);
  siz->set(Sorigin, 0, 1, opts.ox);
  siz->set(Ssize, 0, 0, opts.oy + full_h);
  siz->set(Ssize, 0, 1, opts.ox + full_w);
  for (int c = 0; c < num_comps; ++c) {
    siz->set(Sdims, c, 0, opts.comps[c].height);
    siz->set(Sdims, c, 1, opts.comps[c].width);
    siz->set(Ssampling, c, 0, opts.comps[c].sub_y);
    siz->set(Ssampling, c, 1, opts.comps[c].sub_x);
    siz->set(Sprecision, c, 0, opts.comps[c].precision);
    siz->set(Ssigned, c, 0, opts.comps[c].is_signed);
  }
  if (opts.tile_w > 0 && opts.tile_h > 0) {
    siz->set(Stiles, 0, 0, opts.tile_h);
    siz->set(Stiles, 0, 1, opts.tile_w);
    siz->set(Stile_origin, 0, 0, opts.oy);
    siz->set(Stile_origin, 0, 1, opts.ox);
  }
  for (const std::string& p : opts.extra_params) {
    if (IsSizParam(p)) {
      EXPECT_TRUE(siz->parse_string(p.c_str())) << p;
    }
  }
  static_cast<kdu_params*>(siz)->finalize();
}

std::vector<kdu_byte> EncodeMultiComponent(
    const MultiOptions& opts, const std::vector<std::vector<int64_t>>& src) {
  std::vector<kdu_byte> bitstream;
  VectorTarget target(bitstream);
  ScopedThreadEnv thread_ctx;
  kdu_thread_env* env_ptr = nullptr;
  DependencyTrackingQueue dep_queue;
  kdu_thread_queue root_queue;
  kdu_thread_queue* queue_ptr = nullptr;
  if (opts.num_threads > 0) {
    thread_ctx.Create(opts.num_threads);
    env_ptr = &thread_ctx.env;
    queue_ptr = opts.use_dep_tracking_queue
                    ? static_cast<kdu_thread_queue*>(&dep_queue)
                    : &root_queue;
  }

  kdu_membroker broker(int64_t{64} << 20);
  kdu_sample_allocator ext_allocator;
  if (opts.use_external_allocator) {
    ext_allocator.configure(&broker);
  }
  ScopedCodestream cs;
  ConfigureEncoderCodestream(opts, &cs.siz);
  cs.codestream.create(&cs.siz, &target, nullptr, 0, 0, env_ptr);
  siz_params* params = cs.codestream.access_siz();
  EXPECT_TRUE(params->parse_string(opts.reversible ? "Creversible=yes"
                                                   : "Creversible=no"));
  EXPECT_TRUE(params->parse_string(opts.ycc ? "Cycc=yes" : "Cycc=no"));
  EXPECT_TRUE(params->parse_string(
      ("Clevels=" + std::to_string(opts.dwt_levels)).c_str()));
  for (const std::string& p : opts.extra_params) {
    if (!IsSizParam(p)) {
      EXPECT_TRUE(params->parse_string(p.c_str())) << p;
    }
  }
  params->finalize_all();
  if (opts.use_roi) {
    cs.codestream.configure_simultaneous_processing_fragments(0, 0, 0);
  } else if (opts.frag_width > 0) {
    cs.codestream.configure_simultaneous_processing_fragments(opts.frag_width,
                                                              2, 4);
  }

  ConstRoiImage roi_img;
  kdu_dims tile_indices;
  cs.codestream.get_valid_tiles(tile_indices);

  for (int ty = tile_indices.pos.y;
       ty < tile_indices.pos.y + tile_indices.size.y; ++ty) {
    for (int tx = tile_indices.pos.x;
         tx < tile_indices.pos.x + tile_indices.size.x; ++tx) {
      ScopedTile tile;
      tile.env = env_ptr;
      tile.tile = cs.codestream.open_tile(kdu_coords(tx, ty), env_ptr);
      if (env_ptr != nullptr) {
        env_ptr->attach_queue(queue_ptr, nullptr, "multi_ana_root");
      }

      const int num_out = cs.codestream.get_num_components(true);
      std::vector<int> precisions(num_out);
      std::vector<bool> is_signed(num_out);
      std::vector<kdu_dims> comp_dims(num_out);
      for (int c = 0; c < num_out; ++c) {
        precisions[c] = cs.codestream.get_bit_depth(c, true, false);
        is_signed[c] = cs.codestream.get_signed(c, true);
        cs.codestream.get_dims(c, comp_dims[c], true);
      }

      ScopedMultiAnalysis ana;
      ana.env = env_ptr;
      EXPECT_FALSE(ana.engine.exists());
      EXPECT_TRUE(!ana.engine);
      if (opts.use_compat_create) {
        ana.engine.create(cs.codestream, tile.tile,
                          (opts.enc_flags & KDU_MULTI_XFORM_PRECISE) != 0,
                          opts.use_roi ? &roi_img : nullptr,
                          (opts.enc_flags & KDU_MULTI_XFORM_FAST) != 0,
                          opts.buffer_rows, env_ptr, queue_ptr,
                          (opts.enc_flags & KDU_MULTI_XFORM_MT_DWT) != 0);
      } else {
        kdu_push_pull_params extra_pp;
        const kdu_push_pull_params* pp_ptr = nullptr;
        if (opts.dbuf_rows_override >= 0) {
          extra_pp.set_spatial_xform_dbuf_rows(opts.dbuf_rows_override);
          pp_ptr = &extra_pp;
        }
        ana.engine.create(
            cs.codestream, tile.tile, env_ptr, queue_ptr, opts.enc_flags,
            opts.use_roi ? &roi_img : nullptr, opts.buffer_rows,
            opts.use_external_allocator ? &ext_allocator : nullptr, pp_ptr,
            opts.use_external_allocator ? nullptr : &broker);
      }
      EXPECT_TRUE(ana.engine.exists());
      EXPECT_FALSE(!ana.engine);

      std::vector<kdu_coords> sizes(num_out);
      std::vector<int> rows_done(num_out, 0);
      std::vector<kdu_byte> lflags(num_out);
      for (int c = 0; c < num_out; ++c) {
        sizes[c] = ana.engine.get_size(c);
        lflags[c] = ana.engine.get_line_flags(c);
        EXPECT_EQ(lflags[c] & ~(KDU_LINE_BUF_SHORTS | KDU_LINE_BUF_VPRECISE |
                                KDU_LINE_BUF_ABSOLUTE),
                  0);
      }

      kdu_dims tile_out_dims;
      bool any_left = true;
      while (any_left) {
        any_left = false;
        for (int c = 0; c < num_out; ++c) {
          if (rows_done[c] >= sizes[c].y) {
            continue;
          }
          any_left = true;
          kdu_line_buf* line = ana.engine.exchange_line(c, nullptr, env_ptr);
          if (line != nullptr) {
            cs.codestream.get_tile_dims(kdu_coords(tx, ty), c, tile_out_dims,
                                        true);
            const int img_w = comp_dims[c].size.x;
            const int y =
                tile_out_dims.pos.y - comp_dims[c].pos.y + rows_done[c];
            const int x_off = tile_out_dims.pos.x - comp_dims[c].pos.x;
            WriteLineSample(line,
                            &src[c][static_cast<size_t>(y) * img_w + x_off],
                            sizes[c].x, precisions[c], is_signed[c]);
            ana.engine.exchange_line(c, line, env_ptr);
            rows_done[c]++;
          }
        }
      }
      if (env_ptr != nullptr) {
        env_ptr->join(queue_ptr, true);
      }
      ana.engine.destroy();
      tile.tile.close(env_ptr);
      if (env_ptr != nullptr) {
        env_ptr->terminate(queue_ptr, false);
      }
    }
  }

  kdu_long max_bytes = 1 << 22;
  cs.codestream.flush(&max_bytes, 1, nullptr, true, true, 0.0, env_ptr);
  if (env_ptr != nullptr) {
    env_ptr->cs_terminate(cs.codestream);
  }
  cs.codestream.destroy();
  if (opts.use_dep_tracking_queue && opts.num_threads > 0) {
    EXPECT_GT(dep_queue.updates.load(std::memory_order_relaxed), 0);
  }
  return bitstream;
}

struct MultiDecodeResult {
  std::vector<std::vector<int64_t>> int_samples;
  std::vector<std::vector<double>> float_samples;
  int num_components = 0;
  std::vector<kdu_coords> comp_sizes;
};

MultiDecodeResult DecodeMultiComponent(const std::vector<kdu_byte>& bitstream,
                                       const MultiOptions& opts) {
  MultiDecodeResult res;
  kdu_membroker broker(int64_t{64} << 20);
  kdu_sample_allocator ext_allocator;
  if (opts.use_external_allocator) {
    ext_allocator.configure(&broker);
  }
  VectorSource source(bitstream);
  ScopedThreadEnv thread_ctx;
  kdu_thread_env* env_ptr = nullptr;
  DependencyTrackingQueue dep_queue;
  kdu_thread_queue root_queue;
  kdu_thread_queue* queue_ptr = nullptr;
  if (opts.num_threads > 0) {
    thread_ctx.Create(opts.num_threads);
    env_ptr = &thread_ctx.env;
    queue_ptr = opts.use_dep_tracking_queue
                    ? static_cast<kdu_thread_queue*>(&dep_queue)
                    : &root_queue;
  }

  ScopedCodestream cs;
  cs.codestream.create(&source, env_ptr);
  if (opts.access_mode != KDU_WANT_OUTPUT_COMPONENTS) {
    cs.codestream.apply_input_restrictions(0, 0, 0, 0, nullptr,
                                           opts.access_mode, env_ptr);
  }

  const bool want_out = (opts.access_mode == KDU_WANT_OUTPUT_COMPONENTS);
  res.num_components = cs.codestream.get_num_components(want_out);
  res.int_samples.resize(res.num_components);
  res.float_samples.resize(res.num_components);
  res.comp_sizes.resize(res.num_components);

  std::vector<int> precisions(res.num_components);
  std::vector<bool> is_signed(res.num_components);
  for (int c = 0; c < res.num_components; ++c) {
    kdu_dims full_dims;
    cs.codestream.get_dims(c, full_dims, want_out);
    res.comp_sizes[c] = full_dims.size;
    precisions[c] = cs.codestream.get_bit_depth(c, want_out, false);
    is_signed[c] = cs.codestream.get_signed(c, want_out);
  }

  kdu_dims tile_indices;
  cs.codestream.get_valid_tiles(tile_indices);
  for (int ty = tile_indices.pos.y;
       ty < tile_indices.pos.y + tile_indices.size.y; ++ty) {
    for (int tx = tile_indices.pos.x;
         tx < tile_indices.pos.x + tile_indices.size.x; ++tx) {
      ScopedTile tile;
      tile.env = env_ptr;
      tile.tile = cs.codestream.open_tile(kdu_coords(tx, ty), env_ptr);
      if (!opts.comps_of_interest.empty()) {
        std::vector<int> idx = opts.comps_of_interest;
        tile.tile.set_components_of_interest(static_cast<int>(idx.size()),
                                             idx.data());
      }
      if (env_ptr != nullptr) {
        env_ptr->attach_queue(queue_ptr, nullptr, "multi_syn_root");
      }

      ScopedMultiSynthesis syn;
      syn.env = env_ptr;
      EXPECT_FALSE(syn.engine.exists());
      EXPECT_TRUE(!syn.engine);
      if (opts.use_compat_create) {
        syn.engine.create(cs.codestream, tile.tile,
                          (opts.dec_flags & KDU_MULTI_XFORM_PRECISE) != 0,
                          (opts.dec_flags & KDU_MULTI_XFORM_SKIPYCC) != 0,
                          (opts.dec_flags & KDU_MULTI_XFORM_FAST) != 0,
                          opts.buffer_rows, env_ptr, queue_ptr,
                          (opts.dec_flags & KDU_MULTI_XFORM_MT_DWT) != 0);
      } else {
        kdu_push_pull_params extra_pp;
        const kdu_push_pull_params* pp_ptr = nullptr;
        if (opts.dbuf_rows_override >= 0) {
          extra_pp.set_spatial_xform_dbuf_rows(opts.dbuf_rows_override);
          pp_ptr = &extra_pp;
        }
        syn.engine.create(
            cs.codestream, tile.tile, env_ptr, queue_ptr, opts.dec_flags,
            opts.buffer_rows,
            opts.use_external_allocator ? &ext_allocator : nullptr, pp_ptr,
            opts.use_external_allocator ? nullptr : &broker);
      }
      EXPECT_TRUE(syn.engine.exists());
      EXPECT_FALSE(!syn.engine);
      while (!syn.engine.start(env_ptr)) {
      }

      std::vector<kdu_coords> tile_sizes(res.num_components);
      std::vector<int> rows_left(res.num_components, 0);
      for (int c = 0; c < res.num_components; ++c) {
        tile_sizes[c] = syn.engine.get_size(c);
        rows_left[c] = opts.dec_max_lines >= 0
                           ? std::min(opts.dec_max_lines, tile_sizes[c].y)
                           : tile_sizes[c].y;
        EXPECT_EQ(syn.engine.get_line_flags(c) &
                      ~(KDU_LINE_BUF_SHORTS | KDU_LINE_BUF_VPRECISE |
                        KDU_LINE_BUF_ABSOLUTE),
                  0);
      }

      bool any_left = true;
      while (any_left) {
        any_left = false;
        for (int c = 0; c < res.num_components; ++c) {
          if (rows_left[c] <= 0) {
            continue;
          }
          any_left = true;
          kdu_line_buf* line = syn.engine.get_line(c, env_ptr);
          if (line != nullptr) {
            ReadLineSample(line, tile_sizes[c].x, precisions[c], is_signed[c],
                           &res.int_samples[c], &res.float_samples[c]);
            rows_left[c]--;
          }
        }
      }

      if (env_ptr != nullptr) {
        env_ptr->terminate(queue_ptr, true);
      }
      syn.engine.destroy(env_ptr);
      tile.tile.close(env_ptr);
      if (env_ptr != nullptr) {
        env_ptr->terminate(queue_ptr, false);
      }
    }
  }

  if (env_ptr != nullptr) {
    env_ptr->cs_terminate(cs.codestream);
  }
  cs.codestream.destroy();
  if (opts.use_dep_tracking_queue && opts.num_threads > 0) {
    EXPECT_GT(dep_queue.updates.load(std::memory_order_relaxed), 0);
  }
  return res;
}

TEST(MultiTransformTest, Part1RctIctSubsamplingAndFlags) {
  // 1. Reversible RCT across default (16-bit), PRECISE (32-bit), and
  // VPRECISE (64-bit) plus compatibility create overload.
  for (int flags : {KDU_MULTI_XFORM_DEFAULT_FLAGS, KDU_MULTI_XFORM_PRECISE,
                    KDU_MULTI_XFORM_VPRECISE}) {
    MultiOptions opts;
    opts.comps = {{32, 24, 10, false, 1, 1},
                  {32, 24, 10, false, 1, 1},
                  {32, 24, 10, false, 1, 1}};
    opts.reversible = true;
    opts.ycc = true;
    opts.dwt_levels = 2;
    opts.enc_flags = flags;
    opts.dec_flags = flags;
    opts.use_compat_create = (flags == KDU_MULTI_XFORM_DEFAULT_FLAGS);
    const auto src = MakeMultiImages(opts.comps);
    const auto bs = EncodeMultiComponent(opts, src);
    const auto dec = DecodeMultiComponent(bs, opts);
    ASSERT_EQ(dec.num_components, 3);
    for (int c = 0; c < 3; ++c) {
      EXPECT_EQ(dec.int_samples[c], src[c])
          << "comp=" << c << " flags=" << flags;
    }
  }

  // 2. Irreversible ICT with 0-level DWT, 4th alpha component, FAST/PRECISE,
  // SKIPYCC, and KDU_WANT_CODESTREAM_COMPONENTS.
  {
    MultiOptions opts;
    opts.comps = {{24, 16, 8, false, 1, 1},
                  {24, 16, 8, false, 1, 1},
                  {24, 16, 8, false, 1, 1},
                  {24, 16, 8, true, 1, 1}};
    opts.reversible = false;
    opts.ycc = true;
    opts.dwt_levels = 0;
    opts.enc_flags = KDU_MULTI_XFORM_FAST;
    opts.dec_flags = KDU_MULTI_XFORM_FAST;
    const auto src = MakeMultiImages(opts.comps);
    const auto bs = EncodeMultiComponent(opts, src);
    const auto dec = DecodeMultiComponent(bs, opts);
    ASSERT_EQ(dec.num_components, 4);
    for (int c = 0; c < 4; ++c) {
      for (size_t i = 0; i < src[c].size(); ++i) {
        EXPECT_NEAR(dec.float_samples[c][i], static_cast<double>(src[c][i]),
                    8.0);
      }
    }

    MultiOptions skip = opts;
    skip.enc_flags = KDU_MULTI_XFORM_PRECISE;
    skip.dec_flags = KDU_MULTI_XFORM_PRECISE | KDU_MULTI_XFORM_SKIPYCC;
    skip.access_mode = KDU_WANT_CODESTREAM_COMPONENTS;
    const auto dec_cs = DecodeMultiComponent(bs, skip);
    ASSERT_EQ(dec_cs.num_components, 4);
    EXPECT_EQ(dec_cs.comp_sizes[0].x, 24);
  }

  // 3. Subsampled YCbCr (4:2:0) with Cycc=no and staggered origin.
  {
    MultiOptions sub;
    sub.comps = {{32, 24, 8, false, 1, 1},
                 {16, 12, 8, false, 2, 2},
                 {16, 12, 8, false, 2, 2}};
    sub.ox = 2;
    sub.oy = 2;
    sub.reversible = true;
    sub.ycc = false;
    sub.dwt_levels = 1;
    const auto src = MakeMultiImages(sub.comps);
    const auto bs = EncodeMultiComponent(sub, src);
    const auto dec = DecodeMultiComponent(bs, sub);
    ASSERT_EQ(dec.num_components, 3);
    for (int c = 0; c < 3; ++c) {
      EXPECT_EQ(dec.int_samples[c], src[c]);
    }
  }
}

TEST(MultiTransformTest, Part2MctMatrixRxformDepDwtAndNullBlocks) {
  // 1. Reversible RXFORM (2 components + negative final divisor + offset
  // vector) across 16-bit, 32-bit, and 64-bit, plus 24-bit 64-bit accumulator
  // branch.
  for (int flags : {KDU_MULTI_XFORM_DEFAULT_FLAGS, KDU_MULTI_XFORM_PRECISE,
                    KDU_MULTI_XFORM_VPRECISE}) {
    MultiOptions opts;
    opts.comps = {{24, 16, 8, false, 1, 1}, {24, 16, 8, false, 1, 1}};
    opts.reversible = true;
    opts.ycc = false;
    opts.enc_flags = flags;
    opts.dec_flags = flags;
    opts.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=2",
        "Mprecision=8",
        "Msigned=no",
        "Mmatrix_size:I1=6",
        "Mmatrix_coeffs:I1=1,2,2,-1,0,-1",
        "Mvector_size:I1=2",
        "Mvector_coeffs:I1=3,-2",
        "Mstage_inputs:I1={0,1}",
        "Mstage_outputs:I1={0,1}",
        "Mstage_collections:I1={2,2}",
        "Mstage_xforms:I1={MATRIX,1,1,1,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    const auto src = MakeMultiImages(opts.comps);
    const auto bs = EncodeMultiComponent(opts, src);
    const auto dec = DecodeMultiComponent(bs, opts);
    ASSERT_EQ(dec.num_components, 2);
    for (int c = 0; c < 2; ++c) {
      EXPECT_EQ(dec.int_samples[c], src[c]) << "flags=" << flags;
    }
  }

  // 24-bit rxform with downshift=9 triggers 64-bit accumulator in 32-bit mode.
  {
    MultiOptions opts;
    opts.comps = {{16, 12, 20, true, 1, 1}, {16, 12, 20, true, 1, 1}};
    opts.reversible = true;
    opts.ycc = false;
    opts.enc_flags = KDU_MULTI_XFORM_PRECISE;
    opts.dec_flags = KDU_MULTI_XFORM_PRECISE;
    opts.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=2",
        "Mprecision=20",
        "Msigned=yes",
        "Mmatrix_size:I1=6",
        "Mmatrix_coeffs:I1=1,512,512,-1,0,1",
        "Mstage_inputs:I1={0,1}",
        "Mstage_outputs:I1={0,1}",
        "Mstage_collections:I1={2,2}",
        "Mstage_xforms:I1={MATRIX,1,0,1,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    const auto src = MakeMultiImages(opts.comps);
    const auto bs = EncodeMultiComponent(opts, src);
    const auto dec = DecodeMultiComponent(bs, opts);
    EXPECT_EQ(dec.int_samples[0], src[0]);
    EXPECT_EQ(dec.int_samples[1], src[1]);
  }

  // 2. Irreversible MATRIX chained with a NULL stage expanding 3->5 with
  // constant-0 extra outputs (16-bit and 32-bit PRECISE).
  for (int flags : {KDU_MULTI_XFORM_DEFAULT_FLAGS, KDU_MULTI_XFORM_PRECISE}) {
    MultiOptions opts;
    opts.comps = {{20, 16, 8, false, 1, 1},
                  {20, 16, 8, false, 1, 1},
                  {20, 16, 8, false, 1, 1}};
    opts.reversible = false;
    opts.ycc = false;
    opts.enc_flags = flags;
    opts.dec_flags = flags;
    opts.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=5",
        "Mprecision=8",
        "Msigned=no",
        "Mmatrix_size:I1=9",
        "Mmatrix_coeffs:I1=1.0,0.25,0.0,0.0,1.0,-0.25,0.25,0.0,1.0",
        "Mvector_size:I1=3",
        "Mvector_coeffs:I1=3,-2,1",
        "Mstage_inputs:I1={0,2}",
        "Mstage_outputs:I1={0,2}",
        "Mstage_collections:I1={3,3}",
        "Mstage_xforms:I1={MATRIX,1,1,0,0}",
        "Mstage_inputs:I2={0,2}",
        "Mstage_outputs:I2={0,4}",
        "Mstage_collections:I2={3,5}",
        "Mstage_xforms:I2={MATRIX,0,0,0,0}",
        "Mnum_stages=2",
        "Mstages=1,2",
    };
    const auto src = MakeMultiImages(opts.comps);
    // Input must provide all 5 output components (extra 3 and 4 are constant).
    std::vector<std::vector<int64_t>> five_src = {
        src[0], src[1], src[2], std::vector<int64_t>(20 * 16, 128),
        std::vector<int64_t>(20 * 16, 128)};
    const auto bs = EncodeMultiComponent(opts, five_src);
    const auto dec = DecodeMultiComponent(bs, opts);
    ASSERT_EQ(dec.num_components, 5);
    EXPECT_EQ(dec.comp_sizes[3].x, 20);
    EXPECT_EQ(dec.comp_sizes[4].x, 20);
    for (int out_c : {0, 1, 2}) {
      for (size_t i = 0; i < five_src[out_c].size(); ++i) {
        EXPECT_NEAR(dec.float_samples[out_c][i],
                    static_cast<double>(five_src[out_c][i]), 8.0);
      }
    }
  }

  // 3. Reversible DEP and irreversible DEP transforms across 16-bit & 32-bit.
  for (int flags : {KDU_MULTI_XFORM_DEFAULT_FLAGS, KDU_MULTI_XFORM_PRECISE}) {
    MultiOptions dep_rev;
    dep_rev.comps = {{16, 12, 8, false, 1, 1},
                     {16, 12, 8, false, 1, 1},
                     {16, 12, 8, false, 1, 1}};
    dep_rev.reversible = true;
    dep_rev.ycc = false;
    dep_rev.enc_flags = flags;
    dep_rev.dec_flags = flags;
    dep_rev.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=3",
        "Mprecision=8",
        "Msigned=no",
        "Mtriang_size:I1=5",
        "Mtriang_coeffs:I1=1,1,0,-1,1",
        "Mvector_size:I1=3",
        "Mvector_coeffs:I1=3,-2,1",
        "Mstage_inputs:I1={0,2}",
        "Mstage_outputs:I1={0,2}",
        "Mstage_collections:I1={3,3}",
        "Mstage_xforms:I1={DEP,1,1,1,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    const auto src = MakeMultiImages(dep_rev.comps);
    const auto bs_rev = EncodeMultiComponent(dep_rev, src);
    const auto dec_rev = DecodeMultiComponent(bs_rev, dep_rev);
    for (int c = 0; c < 3; ++c) {
      EXPECT_EQ(dec_rev.int_samples[c], src[c]);
    }

    MultiOptions dep_irv = dep_rev;
    dep_irv.reversible = false;
    dep_irv.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=3",
        "Mprecision=8",
        "Msigned=no",
        "Mtriang_size:I1=3",
        "Mtriang_coeffs:I1=0.25,-0.25,0.5",
        "Mvector_size:I1=3",
        "Mvector_coeffs:I1=3,-2,1",
        "Mstage_inputs:I1={0,2}",
        "Mstage_outputs:I1={0,2}",
        "Mstage_collections:I1={3,3}",
        "Mstage_xforms:I1={DEP,1,1,0,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    const auto bs_irv = EncodeMultiComponent(dep_irv, src);
    const auto dec_irv = DecodeMultiComponent(bs_irv, dep_irv);
    for (int c = 0; c < 3; ++c) {
      for (size_t i = 0; i < src[c].size(); ++i) {
        EXPECT_NEAR(dec_irv.float_samples[c][i], static_cast<double>(src[c][i]),
                    8.0);
      }
    }
  }

  // 4. Component DWT stage: reversible W5X3 and irreversible W9X7 with non-zero
  // component origin and component-of-interest subset on decode.
  for (int flags : {KDU_MULTI_XFORM_DEFAULT_FLAGS, KDU_MULTI_XFORM_PRECISE}) {
    MultiOptions dwt_rev;
    dwt_rev.comps = {{16, 12, 8, false, 1, 1},
                     {16, 12, 8, false, 1, 1},
                     {16, 12, 8, false, 1, 1},
                     {16, 12, 8, false, 1, 1},
                     {16, 12, 8, false, 1, 1}};
    dwt_rev.reversible = true;
    dwt_rev.ycc = false;
    dwt_rev.enc_flags = flags;
    dwt_rev.dec_flags = flags;
    dwt_rev.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=5",
        "Mprecision=8",
        "Msigned=no",
        "Mvector_size:I1=5",
        "Mvector_coeffs:I1=3,-2,1,0,2",
        "Mstage_inputs:I1={0,4}",
        "Mstage_outputs:I1={0,4}",
        "Mstage_collections:I1={5,5}",
        "Mstage_xforms:I1={DWT,1,1,2,1}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    const auto src = MakeMultiImages(dwt_rev.comps);
    const auto bs_rev = EncodeMultiComponent(dwt_rev, src);
    const auto dec_rev = DecodeMultiComponent(bs_rev, dwt_rev);
    for (int c = 0; c < 5; ++c) {
      EXPECT_EQ(dec_rev.int_samples[c], src[c]);
    }

    // Decode only components {1, 3} via set_components_of_interest.
    MultiOptions subset = dwt_rev;
    subset.comps_of_interest = {1, 3};
    const auto dec_sub = DecodeMultiComponent(bs_rev, subset);
    EXPECT_EQ(dec_sub.int_samples[1], src[1]);
    EXPECT_EQ(dec_sub.int_samples[3], src[3]);

    MultiOptions dwt_irv = dwt_rev;
    dwt_irv.reversible = false;
    dwt_irv.extra_params[9] = "Mstage_xforms:I1={DWT,0,1,2,0}";
    const auto bs_irv = EncodeMultiComponent(dwt_irv, src);
    const auto dec_irv = DecodeMultiComponent(bs_irv, dwt_irv);
    for (int c = 0; c < 5; ++c) {
      for (size_t i = 0; i < src[c].size(); ++i) {
        EXPECT_NEAR(dec_irv.float_samples[c][i], static_cast<double>(src[c][i]),
                    10.0);
      }
    }
  }
}

TEST(MultiTransformTest, Part2NonLinearPointTransformsSmagUmagGammaAndLut) {
  // 1. SMAG & UMAG across 16-bit, 32-bit, and 64-bit, plus GAMMA at 16/32-bit.
  // Note: 64-bit VPRECISE with GAMMA/LUT is omitted because upstream
  // `kd_multi_nlt()` in `multi_transform_local.h` omits `usmag64_func = NULL`.
  for (int flags : {KDU_MULTI_XFORM_DEFAULT_FLAGS, KDU_MULTI_XFORM_PRECISE,
                    KDU_MULTI_XFORM_VPRECISE}) {
    MultiOptions rev_nlt;
    rev_nlt.reversible = true;
    rev_nlt.ycc = false;
    rev_nlt.enc_flags = flags;
    rev_nlt.dec_flags = flags;
    if (flags == KDU_MULTI_XFORM_VPRECISE) {
      rev_nlt.comps = {{20, 16, 8, true, 1, 1}, {20, 16, 8, false, 1, 1}};
      rev_nlt.extra_params = {
          "Sextensions=NLT", "Ncomponents=2",  "Nprecision=8",
          "Nsigned=yes,no",  "NLType:C0=SMAG", "NLType:C1=UMAG",
      };
    } else {
      rev_nlt.comps = {{20, 16, 8, true, 1, 1},
                       {20, 16, 8, false, 1, 1},
                       {20, 16, 8, false, 1, 1},
                       {20, 16, 8, false, 1, 1}};
      rev_nlt.extra_params = {
          "Sextensions=NLT",
          "Ncomponents=4",
          "Nprecision=8",
          "Nsigned=yes,no,no,no",
          "NLType:C0=SMAG",
          "NLType:C1=UMAG",
          "NLTmake:C2={GAMMA,2.2,0.055,64}",
          "NLTmake:C3={GAMMA,2.2,0.055,64}",
      };
    }
    const auto src = MakeMultiImages(rev_nlt.comps);
    const auto bs = EncodeMultiComponent(rev_nlt, src);
    const auto dec = DecodeMultiComponent(bs, rev_nlt);
    ASSERT_EQ(dec.num_components, static_cast<int>(rev_nlt.comps.size()));
    EXPECT_EQ(dec.int_samples[0], src[0]);
    EXPECT_EQ(dec.int_samples[1], src[1]);
    if (flags != KDU_MULTI_XFORM_VPRECISE) {
      for (int c : {2, 3}) {
        for (size_t i = 0; i < src[c].size(); ++i) {
          EXPECT_NEAR(static_cast<double>(dec.int_samples[c][i]),
                      static_cast<double>(src[c][i]), 4.0);
        }
      }
    }
  }

  // 2. Irreversible NLT with SMAG, UMAG, GAMMA, and shared LUT (16-bit +
  // 32-bit).
  for (int flags : {KDU_MULTI_XFORM_DEFAULT_FLAGS, KDU_MULTI_XFORM_PRECISE}) {
    MultiOptions irv_nlt;
    irv_nlt.comps = {{20, 16, 8, true, 1, 1},
                     {20, 16, 8, false, 1, 1},
                     {20, 16, 8, false, 1, 1},
                     {20, 16, 8, false, 1, 1}};
    irv_nlt.reversible = false;
    irv_nlt.ycc = false;
    irv_nlt.enc_flags = flags;
    irv_nlt.dec_flags = flags;
    irv_nlt.extra_params = {
        "Sextensions=NLT",
        "Ncomponents=4",
        "Nprecision=8",
        "Nsigned=yes,no,no,no",
        "NLType:C0=SMAG",
        "NLType:C1=UMAG",
        "NLType:C2=LUT",
        "NLTlut:C2={0.0,1.0,4,12}",
        "NLTdata:C2=0.0,0.3,0.7,1.0",
        "NLType:C3=LUT",
        "NLTlut:C3={0.0,1.0,4,12}",
        "NLTdata:C3=0.0,0.3,0.7,1.0",
    };
    const auto src = MakeMultiImages(irv_nlt.comps);
    const auto bs = EncodeMultiComponent(irv_nlt, src);
    const auto dec = DecodeMultiComponent(bs, irv_nlt);
    ASSERT_EQ(dec.num_components, 4);
    EXPECT_EQ(dec.comp_sizes[0].x, 20);
    EXPECT_EQ(dec.comp_sizes[2].x, 20);
  }
}

TEST(MultiTransformTest, MultiThreadedDwtDelayedStartDependenciesAndMemory) {
  // 1. Multi-threaded DWT (MT_DWT / DBUF), buffer_rows=-1, dependency-tracking
  // parent queue, external sample allocator, and subsampled components.
  {
    MultiOptions mt;
    mt.comps = {{48, 128, 8, false, 1, 1},
                {24, 64, 8, false, 2, 2},
                {24, 64, 8, false, 2, 2}};
    mt.reversible = false;
    mt.ycc = false;
    mt.dwt_levels = 1;
    mt.extra_params = {"Cblk={4,4}"};
    mt.enc_flags = KDU_MULTI_XFORM_MT_DWT | KDU_MULTI_XFORM_PRECISE;
    mt.dec_flags = KDU_MULTI_XFORM_MT_DWT | KDU_MULTI_XFORM_PRECISE |
                   KDU_MULTI_XFORM_DELAYED_START;
    mt.buffer_rows = -1;
    mt.num_threads = 3;
    mt.use_dep_tracking_queue = true;
    mt.use_external_allocator = true;
    const auto src = MakeMultiImages(mt.comps);
    const auto bs = EncodeMultiComponent(mt, src);
    const auto dec = DecodeMultiComponent(bs, mt);
    ASSERT_EQ(dec.num_components, 3);
    for (int c = 0; c < 3; ++c) {
      for (size_t i = 0; i < src[c].size(); ++i) {
        EXPECT_NEAR(dec.float_samples[c][i], static_cast<double>(src[c][i]),
                    8.0);
      }
    }
  }

  // 2. Single-threaded local DWT with dependency-tracking queue attached
  // (exercises sync_dwt_propagate_dependencies and buffer_rows=0 clamping).
  {
    MultiOptions sync_dep;
    sync_dep.comps = {{32, 96, 8, false, 1, 1},
                      {32, 96, 8, false, 1, 1},
                      {32, 96, 8, false, 1, 1}};
    sync_dep.reversible = true;
    sync_dep.ycc = true;
    sync_dep.extra_params = {"Cblk={4,4}"};
    sync_dep.enc_flags = KDU_MULTI_XFORM_DEFAULT_FLAGS;
    sync_dep.dec_flags = KDU_MULTI_XFORM_DEFAULT_FLAGS;
    sync_dep.buffer_rows = 0;
    sync_dep.num_threads = 2;
    sync_dep.use_dep_tracking_queue = true;
    const auto src = MakeMultiImages(sync_dep.comps);
    const auto bs = EncodeMultiComponent(sync_dep, src);
    const auto dec = DecodeMultiComponent(bs, sync_dep);
    for (int c = 0; c < 3; ++c) {
      EXPECT_EQ(dec.int_samples[c], src[c]);
    }
  }

  // 3. ROI image encoding + early synthesis termination + multi-fragment enc.
  {
    MultiOptions roi_opts;
    roi_opts.comps = {{40, 28, 8, false, 1, 1},
                      {40, 28, 8, false, 1, 1},
                      {40, 28, 8, false, 1, 1}};
    roi_opts.reversible = true;
    roi_opts.ycc = true;
    roi_opts.use_roi = true;
    roi_opts.extra_params = {"Rshift=14"};
    roi_opts.num_threads = 2;
    roi_opts.dec_flags = KDU_MULTI_XFORM_MT_DWT | KDU_MULTI_XFORM_DELAYED_START;
    roi_opts.dec_max_lines = 4;  // Terminate before reading all lines.
    const auto src = MakeMultiImages(roi_opts.comps);
    const auto bs = EncodeMultiComponent(roi_opts, src);
    const auto partial = DecodeMultiComponent(bs, roi_opts);
    ASSERT_EQ(partial.num_components, 3);
    EXPECT_EQ(partial.int_samples[0].size(), size_t{40} * 4);
  }

  {
    MultiOptions frag_opts;
    frag_opts.comps = {{64, 32, 8, false, 1, 1},
                       {64, 32, 8, false, 1, 1},
                       {64, 32, 8, false, 1, 1}};
    frag_opts.reversible = true;
    frag_opts.ycc = true;
    frag_opts.dwt_levels = 3;
    frag_opts.frag_width = 16;
    frag_opts.num_threads = 2;
    frag_opts.extra_params = {"Cblk={16,16}"};
    const auto src = MakeMultiImages(frag_opts.comps);
    const auto bs = EncodeMultiComponent(frag_opts, src);
    const auto dec = DecodeMultiComponent(bs, frag_opts);
    for (int c = 0; c < 3; ++c) {
      EXPECT_EQ(dec.int_samples[c], src[c]);
    }
  }

  // 4. Error handling: tight membroker exhaustion throws kdu_exception cleanly.
  {
    MultiOptions tiny;
    tiny.comps = {{32, 24, 8, false, 1, 1},
                  {32, 24, 8, false, 1, 1},
                  {32, 24, 8, false, 1, 1}};
    ScopedCodestream cs;
    ConfigureEncoderCodestream(tiny, &cs.siz);
    std::vector<kdu_byte> out;
    VectorTarget target(out);
    cs.codestream.create(&cs.siz, &target);
    cs.codestream.access_siz()->finalize_all();
    ScopedTile tile;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
    kdu_membroker tiny_broker(0);
    kdu_multi_analysis ana;
    GetErrorHandler()->set_quiet(true);
    EXPECT_THROW(ana.create(cs.codestream, tile.tile, nullptr, nullptr,
                            KDU_MULTI_XFORM_DEFAULT_FLAGS, nullptr, 1, nullptr,
                            nullptr, &tiny_broker),
                 kdu_exception);
    GetErrorHandler()->set_quiet(false);
    if (ana.exists()) {
      ana.destroy();
    }
    tile.tile.close();
    cs.codestream.destroy();
  }
}

std::vector<kdu_byte> EncodeRawCodestreamWithMctHeader(
    const MultiOptions& opts, const std::vector<std::vector<int64_t>>& src) {
  std::vector<kdu_byte> bitstream;
  VectorTarget target(bitstream);
  ScopedCodestream cs;
  ConfigureEncoderCodestream(opts, &cs.siz);
  cs.codestream.create(&cs.siz, &target);
  siz_params* params = cs.codestream.access_siz();
  EXPECT_TRUE(params->parse_string(opts.reversible ? "Creversible=yes"
                                                   : "Creversible=no"));
  EXPECT_TRUE(params->parse_string(opts.ycc ? "Cycc=yes" : "Cycc=no"));
  EXPECT_TRUE(params->parse_string(
      ("Clevels=" + std::to_string(opts.dwt_levels)).c_str()));
  for (const std::string& p : opts.extra_params) {
    if (!IsSizParam(p)) {
      EXPECT_TRUE(params->parse_string(p.c_str())) << p;
    }
  }
  params->finalize_all();

  ScopedTile tile;
  tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
  const int num_cs = cs.codestream.get_num_components(false);
  kdu_sample_allocator allocator;
  std::vector<kdu_push_ifc> engines(num_cs);
  std::vector<kdu_line_buf> lines(num_cs);
  std::vector<kdu_dims> dims(num_cs);
  std::vector<int> precisions(num_cs);
  std::vector<bool> is_signed(num_cs);
  for (int c = 0; c < num_cs; ++c) {
    precisions[c] = cs.codestream.get_bit_depth(c, false, false);
    is_signed[c] = cs.codestream.get_signed(c, false);
    kdu_tile_comp tc = tile.tile.access_component(c);
    kdu_resolution res = tc.access_resolution();
    res.get_dims(dims[c]);
    engines[c] = kdu_analysis(res, &allocator, false);
    lines[c].pre_create(&allocator, dims[c].size.x,
                        opts.reversible ? KDU_LINE_BUF_ABSOLUTE : 0, 0, 0);
  }
  allocator.finalize(cs.codestream);
  for (int c = 0; c < num_cs; ++c) {
    lines[c].create();
    const int w = dims[c].size.x;
    const int h = dims[c].size.y;
    for (int y = 0; y < h; ++y) {
      WriteLineSample(&lines[c], &src[c][static_cast<size_t>(y) * w], w,
                      precisions[c], is_signed[c]);
      engines[c].push(lines[c], nullptr);
    }
    engines[c].finish(nullptr);
    engines[c].destroy();
    lines[c].destroy();
  }
  tile.tile.close();
  kdu_long max_bytes = 1 << 20;
  cs.codestream.flush(&max_bytes, 1, nullptr, true, true, 0.0, nullptr);
  cs.codestream.destroy();
  return bitstream;
}

TEST(MultiTransformTest, Part2NltPrecisionShiftsAndBoundaryClamp) {
  // Exercise upshift (precision >= KDU_FIX_POINT=13) and exact shift=0
  // paths in local_smag_int16 / local_umag_int16, plus clipping of out-of-range
  // samples and reversible 16-bit SMAG/UMAG.
  for (bool rev : {false, true}) {
    for (int prec : {13, 14}) {
      MultiOptions opts;
      opts.comps = {{20, 12, prec, true, 1, 1}, {20, 12, prec, false, 1, 1}};
      opts.reversible = rev;
      opts.ycc = false;
      opts.enc_flags = KDU_MULTI_XFORM_DEFAULT_FLAGS;
      opts.dec_flags = KDU_MULTI_XFORM_DEFAULT_FLAGS;
      const std::string pstr = "Nprecision=" + std::to_string(prec);
      opts.extra_params = {
          "Sextensions=NLT", "Ncomponents=2",  pstr,
          "Nsigned=yes,no",  "NLType:C0=SMAG", "NLType:C1=UMAG",
      };
      auto src = MakeMultiImages(opts.comps);
      const int64_t limit = int64_t{1} << (prec - 1);
      // Include extremes near/beyond precision bounds to cover clamping
      // branches.
      src[0][0] = -limit - 200;
      src[0][1] = limit + 200;
      src[0][2] = -limit + 1;
      src[1][0] = -100;
      src[1][1] = (int64_t{1} << prec) + 200;
      const auto bs = EncodeMultiComponent(opts, src);
      const auto dec = DecodeMultiComponent(bs, opts);
      ASSERT_EQ(dec.num_components, 2);
      if (rev) {
        EXPECT_EQ(dec.int_samples[0][2], -limit + 1);
      } else {
        EXPECT_NEAR(dec.float_samples[0][2], static_cast<double>(-limit + 1),
                    32.0);
      }
    }
  }

  // Exercise 16-bit precision in both reversible and PRECISE paths.
  for (int flags : {KDU_MULTI_XFORM_DEFAULT_FLAGS, KDU_MULTI_XFORM_PRECISE}) {
    MultiOptions opts;
    opts.comps = {{16, 10, 16, true, 1, 1}, {16, 10, 16, false, 1, 1}};
    opts.reversible = true;
    opts.ycc = false;
    opts.enc_flags = flags;
    opts.dec_flags = flags;
    opts.extra_params = {
        "Sextensions=NLT", "Ncomponents=2",  "Nprecision=16",
        "Nsigned=yes,no",  "NLType:C0=SMAG", "NLType:C1=UMAG",
    };
    const auto src = MakeMultiImages(opts.comps);
    const auto bs = EncodeMultiComponent(opts, src);
    const auto dec = DecodeMultiComponent(bs, opts);
    EXPECT_EQ(dec.int_samples[0], src[0]);
    EXPECT_EQ(dec.int_samples[1], src[1]);
  }
}

TEST(MultiTransformTest,
     Part2MixedReversibilityMultiStageDwtNormalizingAndBitDepths) {
  // 1. Reversible codestream decoded through irreversible MCT stages
  // (covers dep->reversible conversions in MATRIX, DWT, DEP, and
  // kd_multi_line::copy across FAST 16-bit, PRECISE 32-bit, and VPRECISE
  // 64-bit).
  {
    for (int prec : {8, 16}) {
      for (int dec_flags :
           {KDU_MULTI_XFORM_DEFAULT_FLAGS, KDU_MULTI_XFORM_PRECISE,
            KDU_MULTI_XFORM_VPRECISE}) {
        if (prec == 16 && dec_flags == KDU_MULTI_XFORM_DEFAULT_FLAGS) {
          // Covers 16-bit FAST/DEFAULT with src_bit_depth=16 >
          // KDU_FIX_POINT=13.
        }
        MultiOptions raw_opts;
        raw_opts.comps = {{16, 12, prec, false, 1, 1},
                          {16, 12, prec, false, 1, 1},
                          {16, 12, prec, false, 1, 1}};
        raw_opts.reversible = true;
        raw_opts.ycc = false;
        raw_opts.extra_params = {
            "Sextensions=MCT",
            "Mcomponents=3",
            "Mprecision=" + std::to_string(prec),
            "Msigned=no",
            "Mmatrix_size:I1=9",
            "Mmatrix_coeffs:I1=1.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,1.0",
            "Mvector_size:I1=3",
            "Mvector_coeffs:I1=2,-1,1",
            "Mstage_inputs:I1={0,2}",
            "Mstage_outputs:I1={0,2}",
            "Mstage_collections:I1={3,3}",
            "Mstage_xforms:I1={MATRIX,1,1,0,0}",
            "Mnum_stages=1",
            "Mstages=1",
        };
        const auto cs_src = MakeMultiImages(raw_opts.comps);
        const auto bs = EncodeRawCodestreamWithMctHeader(raw_opts, cs_src);
        MultiOptions dec_opts = raw_opts;
        dec_opts.dec_flags = dec_flags;
        dec_opts.dbuf_rows_override = 2;
        const auto dec = DecodeMultiComponent(bs, dec_opts);
        ASSERT_EQ(dec.num_components, 3);
      }
    }

    // Reversible codestream + irreversible DWT stage (5 components).
    for (int dec_flags : {KDU_MULTI_XFORM_FAST, KDU_MULTI_XFORM_PRECISE,
                          KDU_MULTI_XFORM_VPRECISE}) {
      MultiOptions raw_dwt;
      raw_dwt.comps = {{16, 12, 8, false, 1, 1},
                       {16, 12, 8, false, 1, 1},
                       {16, 12, 8, false, 1, 1},
                       {16, 12, 8, false, 1, 1},
                       {16, 12, 8, false, 1, 1}};
      raw_dwt.reversible = true;
      raw_dwt.ycc = false;
      raw_dwt.extra_params = {
          "Sextensions=MCT",
          "Mcomponents=5",
          "Mprecision=8",
          "Msigned=no",
          "Mstage_inputs:I1={0,4}",
          "Mstage_outputs:I1={0,4}",
          "Mstage_collections:I1={5,5}",
          "Mstage_xforms:I1={DWT,0,0,2,0}",
          "Mnum_stages=1",
          "Mstages=1",
      };
      const auto cs_src = MakeMultiImages(raw_dwt.comps);
      const auto bs = EncodeRawCodestreamWithMctHeader(raw_dwt, cs_src);
      MultiOptions dec_opts = raw_dwt;
      dec_opts.dec_flags = dec_flags;
      const auto dec = DecodeMultiComponent(bs, dec_opts);
      ASSERT_EQ(dec.num_components, 5);
    }
  }

  // 2. Irreversible 16-component 4-level DWT triggers normalizing_shift > 0 in
  // both 16-bit FAST and 32-bit PRECISE paths, plus multi-stage DWT -> MATRIX
  // bit-depth propagation.
  for (int flags : {KDU_MULTI_XFORM_FAST, KDU_MULTI_XFORM_PRECISE}) {
    MultiOptions opts;
    opts.comps.assign(16, {12, 10, 8, false, 1, 1});
    opts.reversible = false;
    opts.ycc = false;
    opts.enc_flags = flags;
    opts.dec_flags = flags;
    std::string mat = "Mmatrix_coeffs:I1=";
    for (int r = 0; r < 16; ++r) {
      for (int c = 0; c < 16; ++c) {
        if (r != 0 || c != 0) mat += ",";
        mat += (r == c) ? "1.0" : "0.0";
      }
    }
    opts.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=16",
        "Mprecision=8",
        "Msigned=no",
        "Mmatrix_size:I1=256",
        mat,
        "Mstage_inputs:I1={0,15}",
        "Mstage_outputs:I1={0,15}",
        "Mstage_collections:I1={16,16}",
        "Mstage_xforms:I1={DWT,0,0,4,0}",
        "Mstage_inputs:I2={0,15}",
        "Mstage_outputs:I2={0,15}",
        "Mstage_collections:I2={16,16}",
        "Mstage_xforms:I2={MATRIX,1,0,0,0}",
        "Mnum_stages=2",
        "Mstages=1,2",
    };
    const auto src = MakeMultiImages(opts.comps);
    const auto bs = EncodeMultiComponent(opts, src);
    const auto dec = DecodeMultiComponent(bs, opts);
    ASSERT_EQ(dec.num_components, 16);
    for (size_t i = 0; i < src[0].size(); ++i) {
      EXPECT_NEAR(dec.float_samples[0][i], static_cast<double>(src[0][i]),
                  12.0);
    }
  }

  // 3. Single-component DWT block with odd canvas offset (canvas_size == 1,
  // canvas_min & 1 == 1) across 16-bit, 32-bit, and 64-bit reversible.
  for (int flags : {KDU_MULTI_XFORM_DEFAULT_FLAGS, KDU_MULTI_XFORM_PRECISE,
                    KDU_MULTI_XFORM_VPRECISE}) {
    MultiOptions odd_dwt;
    odd_dwt.comps = {{16, 12, 8, false, 1, 1}, {16, 12, 8, false, 1, 1}};
    odd_dwt.reversible = true;
    odd_dwt.ycc = false;
    odd_dwt.enc_flags = flags;
    odd_dwt.dec_flags = flags;
    odd_dwt.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=2",
        "Mprecision=8",
        "Msigned=no",
        "Mstage_inputs:I1={0,0},{1,1}",
        "Mstage_outputs:I1={0,0},{1,1}",
        "Mstage_collections:I1={1,1},{1,1}",
        "Mstage_xforms:I1={MATRIX,0,0,1,0},{DWT,1,0,1,1}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    const auto src = MakeMultiImages(odd_dwt.comps);
    const auto bs = EncodeMultiComponent(odd_dwt, src);
    const auto dec = DecodeMultiComponent(bs, odd_dwt);
    EXPECT_EQ(dec.int_samples[0], src[0]);
    EXPECT_EQ(dec.int_samples[1], src[1]);
  }

  // 4. Reversible DEP with 64-bit VPRECISE and 32-bit mode + 64-bit
  // accumulator (20-bit depth + divisor=16384 -> downshift=14 -> 34 > 32),
  // plus overdetermined 2->3 rectangular irreversible MATRIX and constant-0 ->
  // DWT Stage 2 (dep == NULL inside kd_multi_dwt_block).
  {
    MultiOptions dep64;
    dep64.comps = {{16, 12, 10, true, 1, 1},
                   {16, 12, 10, true, 1, 1},
                   {16, 12, 10, true, 1, 1}};
    dep64.reversible = true;
    dep64.ycc = false;
    dep64.enc_flags = KDU_MULTI_XFORM_VPRECISE;
    dep64.dec_flags = KDU_MULTI_XFORM_VPRECISE;
    dep64.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=3",
        "Mprecision=10",
        "Msigned=yes",
        "Mtriang_size:I1=5",
        "Mtriang_coeffs:I1=1,1,1,-1,2",
        "Mvector_size:I1=3",
        "Mvector_coeffs:I1=5,-3,2",
        "Mstage_inputs:I1={0,2}",
        "Mstage_outputs:I1={0,2}",
        "Mstage_collections:I1={3,3}",
        "Mstage_xforms:I1={DEP,1,1,1,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    const auto src64 = MakeMultiImages(dep64.comps);
    const auto bs64 = EncodeMultiComponent(dep64, src64);
    const auto dec64 = DecodeMultiComponent(bs64, dep64);
    for (int c = 0; c < 3; ++c) {
      EXPECT_EQ(dec64.int_samples[c], src64[c]);
    }

    MultiOptions dep32_64 = dep64;
    dep32_64.comps = {{16, 12, 20, true, 1, 1}, {16, 12, 20, true, 1, 1}};
    dep32_64.enc_flags = KDU_MULTI_XFORM_PRECISE;
    dep32_64.dec_flags = KDU_MULTI_XFORM_PRECISE;
    dep32_64.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=2",
        "Mprecision=20",
        "Msigned=yes",
        "Mtriang_size:I1=2",
        "Mtriang_coeffs:I1=1,16384",
        "Mvector_size:I1=2",
        "Mvector_coeffs:I1=0,0",
        "Mstage_inputs:I1={0,1}",
        "Mstage_outputs:I1={0,1}",
        "Mstage_collections:I1={2,2}",
        "Mstage_xforms:I1={DEP,1,1,1,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    const auto src_dep32 = MakeMultiImages(dep32_64.comps);
    const auto bs_dep32 = EncodeMultiComponent(dep32_64, src_dep32);
    const auto dec_dep32 = DecodeMultiComponent(bs_dep32, dep32_64);
    EXPECT_EQ(dec_dep32.int_samples[0], src_dep32[0]);
    EXPECT_EQ(dec_dep32.int_samples[1], src_dep32[1]);
  }

  // Overdetermined 2->3 matrix and Stage-2 DWT receiving constant NULL inputs.
  {
    MultiOptions overdet;
    overdet.comps = {{16, 12, 8, false, 1, 1}, {16, 12, 8, false, 1, 1}};
    overdet.reversible = false;
    overdet.ycc = false;
    overdet.enc_flags = KDU_MULTI_XFORM_PRECISE;
    overdet.dec_flags = KDU_MULTI_XFORM_PRECISE;
    overdet.dbuf_rows_override = 2;
    overdet.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=3",
        "Mprecision=8",
        "Msigned=no",
        "Mmatrix_size:I1=6",
        "Mmatrix_coeffs:I1=1.0,0.0,0.0,1.0,0.5,0.5",
        "Mstage_inputs:I1={0,1}",
        "Mstage_outputs:I1={0,2}",
        "Mstage_collections:I1={2,3}",
        "Mstage_xforms:I1={MATRIX,1,0,0,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    const auto s2 = MakeMultiImages(overdet.comps);
    std::vector<int64_t> avg_src(16 * 12);
    for (size_t i = 0; i < avg_src.size(); ++i) {
      avg_src[i] = (s2[0][i] + s2[1][i]) / 2;
    }
    std::vector<std::vector<int64_t>> three_src = {s2[0], s2[1], avg_src};
    const auto bs = EncodeMultiComponent(overdet, three_src);
    const auto dec = DecodeMultiComponent(bs, overdet);
    ASSERT_EQ(dec.num_components, 3);
  }
}

TEST(MultiTransformTest, ErrorPathsAndQueueTermination) {
  GetErrorHandler()->set_quiet(true);
  auto expect_ana_throw = [](const MultiOptions& opts, const char* label,
                             int num_threads = 0) {
    std::vector<kdu_byte> out;
    VectorTarget target(out);
    ScopedThreadEnv thread_ctx;
    kdu_thread_env* env_ptr = nullptr;
    if (num_threads > 0) {
      thread_ctx.Create(num_threads);
      env_ptr = &thread_ctx.env;
    }
    ScopedCodestream cs;
    ConfigureEncoderCodestream(opts, &cs.siz);
    cs.codestream.create(&cs.siz, &target, nullptr, 0, 0, env_ptr);
    siz_params* params = cs.codestream.access_siz();
    params->parse_string(opts.reversible ? "Creversible=yes"
                                         : "Creversible=no");
    params->parse_string(opts.ycc ? "Cycc=yes" : "Cycc=no");
    params->parse_string(
        ("Clevels=" + std::to_string(opts.dwt_levels)).c_str());
    for (const std::string& p : opts.extra_params) {
      if (!IsSizParam(p)) {
        params->parse_string(p.c_str());
      }
    }
    params->finalize_all();
    ScopedTile tile;
    tile.env = env_ptr;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), env_ptr);
    kdu_multi_analysis ana;
    EXPECT_THROW(
        ana.create(cs.codestream, tile.tile, env_ptr, nullptr, opts.enc_flags),
        kdu_exception)
        << label;
    if (ana.exists()) {
      ana.destroy(env_ptr);
    }
    tile.tile.close(env_ptr);
    if (env_ptr != nullptr) {
      env_ptr->cs_terminate(cs.codestream);
    }
    cs.codestream.destroy();
  };

  // Near-singular MATRIX on encode.
  {
    MultiOptions bad;
    bad.comps = {{16, 12, 8, false, 1, 1}, {16, 12, 8, false, 1, 1}};
    bad.reversible = false;
    bad.ycc = false;
    bad.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=2",
        "Mprecision=8",
        "Msigned=no",
        "Mmatrix_size:I1=4",
        "Mmatrix_coeffs:I1=1,1,1,1",
        "Mstage_inputs:I1={0,1}",
        "Mstage_outputs:I1={0,1}",
        "Mstage_collections:I1={2,2}",
        "Mstage_xforms:I1={MATRIX,1,0,0,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    expect_ana_throw(bad, "singular_matrix");
  }

  // Underdetermined MATRIX (3 inputs -> 2 outputs) on encode.
  {
    MultiOptions bad;
    bad.comps = {{16, 12, 8, false, 1, 1},
                 {16, 12, 8, false, 1, 1},
                 {16, 12, 8, false, 1, 1}};
    bad.reversible = false;
    bad.ycc = false;
    bad.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=2",
        "Mprecision=8",
        "Msigned=no",
        "Mmatrix_size:I1=6",
        "Mmatrix_coeffs:I1=1,0,0,0,1,0",
        "Mstage_inputs:I1={0,2}",
        "Mstage_outputs:I1={0,1}",
        "Mstage_collections:I1={3,2}",
        "Mstage_xforms:I1={MATRIX,1,0,0,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    expect_ana_throw(bad, "underdetermined_matrix");
  }

  // Irreversible MATRIX and irreversible DWT on Creversible=yes codestream.
  {
    MultiOptions bad;
    bad.comps = {{16, 12, 8, false, 1, 1}, {16, 12, 8, false, 1, 1}};
    bad.reversible = true;
    bad.ycc = false;
    bad.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=2",
        "Mprecision=8",
        "Msigned=no",
        "Mmatrix_size:I1=4",
        "Mmatrix_coeffs:I1=1,0,0,1",
        "Mstage_inputs:I1={0,1}",
        "Mstage_outputs:I1={0,1}",
        "Mstage_collections:I1={2,2}",
        "Mstage_xforms:I1={MATRIX,1,0,0,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    expect_ana_throw(bad, "irrev_matrix_on_rev_cs");

    MultiOptions bad_dwt = bad;
    bad_dwt.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=2",
        "Mprecision=8",
        "Msigned=no",
        "Mstage_inputs:I1={0,1}",
        "Mstage_outputs:I1={0,1}",
        "Mstage_collections:I1={2,2}",
        "Mstage_xforms:I1={DWT,0,0,1,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    expect_ana_throw(bad_dwt, "irrev_dwt_on_rev_cs");
  }

  // RXFORM non-power-of-2 divisor on encode and DEP non-power-of-2 divisor on
  // encode and decode.
  {
    MultiOptions bad_rx;
    bad_rx.comps = {{16, 12, 8, false, 1, 1}, {16, 12, 8, false, 1, 1}};
    bad_rx.reversible = true;
    bad_rx.ycc = false;
    bad_rx.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=2",
        "Mprecision=8",
        "Msigned=no",
        "Mmatrix_size:I1=6",
        "Mmatrix_coeffs:I1=1,1,1,-1,0,3",
        "Mstage_inputs:I1={0,1}",
        "Mstage_outputs:I1={0,1}",
        "Mstage_collections:I1={2,2}",
        "Mstage_xforms:I1={MATRIX,1,0,1,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    const auto cs_src = MakeMultiImages(bad_rx.comps);
    EXPECT_THROW(EncodeMultiComponent(bad_rx, cs_src), kdu_exception);

    const auto rx_bs = EncodeRawCodestreamWithMctHeader(bad_rx, cs_src);
    EXPECT_THROW(DecodeMultiComponent(rx_bs, bad_rx), kdu_exception);

    MultiOptions bad_dep = bad_rx;
    bad_dep.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=2",
        "Mprecision=8",
        "Msigned=no",
        "Mtriang_size:I1=2",
        "Mtriang_coeffs:I1=1,3",
        "Mvector_size:I1=2",
        "Mvector_coeffs:I1=0,0",
        "Mstage_inputs:I1={0,1}",
        "Mstage_outputs:I1={0,1}",
        "Mstage_collections:I1={2,2}",
        "Mstage_xforms:I1={DEP,1,1,1,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    EXPECT_THROW(EncodeMultiComponent(bad_dep, cs_src), kdu_exception);

    const auto dep_bs = EncodeRawCodestreamWithMctHeader(bad_dep, cs_src);
    EXPECT_THROW(DecodeMultiComponent(dep_bs, bad_dep), kdu_exception);
  }

  // Size inconsistency, bit-depth inconsistency, DWT output bit-depth mismatch,
  // and reversible block on irreversible codestream.
  {
    MultiOptions bad_size;
    bad_size.comps = {{16, 12, 8, false, 1, 1}, {8, 12, 8, false, 2, 1}};
    bad_size.reversible = false;
    bad_size.ycc = false;
    bad_size.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=2",
        "Mprecision=8",
        "Msigned=no",
        "Mmatrix_size:I1=4",
        "Mmatrix_coeffs:I1=1,0,0,1",
        "Mstage_inputs:I1={0,1}",
        "Mstage_outputs:I1={0,1}",
        "Mstage_collections:I1={2,2}",
        "Mstage_xforms:I1={MATRIX,1,0,0,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    expect_ana_throw(bad_size, "bad_size");

    MultiOptions bad_mprec;
    bad_mprec.comps = {{16, 12, 8, false, 1, 1}, {16, 12, 8, false, 1, 1}};
    bad_mprec.reversible = true;
    bad_mprec.ycc = false;
    bad_mprec.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=2",
        "Mprecision=10",
        "Msigned=no",
        "Mstage_inputs:I1={0,1}",
        "Mstage_outputs:I1={0,1}",
        "Mstage_collections:I1={2,2}",
        "Mstage_xforms:I1={MATRIX,0,0,0,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    expect_ana_throw(bad_mprec, "bad_mprec");

    MultiOptions bad_dwt_depth;
    bad_dwt_depth.comps = {{16, 12, 8, false, 1, 1}, {16, 12, 10, false, 1, 1}};
    bad_dwt_depth.reversible = false;
    bad_dwt_depth.ycc = false;
    bad_dwt_depth.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=2",
        "Mprecision=8,10",
        "Msigned=no",
        "Mstage_inputs:I1={0,1}",
        "Mstage_outputs:I1={0,1}",
        "Mstage_collections:I1={2,2}",
        "Mstage_xforms:I1={DWT,0,0,1,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    expect_ana_throw(bad_dwt_depth, "bad_dwt_depth");

    // Reversible MCT decoded on irreversible codestream (0x23080501).
    MultiOptions irv_rev_mct;
    irv_rev_mct.comps = {{16, 12, 8, false, 1, 1}, {16, 12, 8, false, 1, 1}};
    irv_rev_mct.reversible = false;
    irv_rev_mct.ycc = false;
    irv_rev_mct.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=2",
        "Mprecision=8",
        "Msigned=no",
        "Mmatrix_size:I1=6",
        "Mmatrix_coeffs:I1=1,1,1,-1,0,2",
        "Mstage_inputs:I1={0,1}",
        "Mstage_outputs:I1={0,1}",
        "Mstage_collections:I1={2,2}",
        "Mstage_xforms:I1={MATRIX,1,0,1,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    const auto cs_src = MakeMultiImages(irv_rev_mct.comps);
    const auto bs_bad = EncodeRawCodestreamWithMctHeader(irv_rev_mct, cs_src);
    VectorSource vs_bad(bs_bad);
    ScopedCodestream cs;
    cs.codestream.create(&vs_bad);
    ScopedTile tile;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
    kdu_multi_synthesis syn;
    EXPECT_THROW(syn.create(cs.codestream, tile.tile), kdu_exception);
    if (syn.exists()) syn.destroy();
    tile.tile.close();
    cs.codestream.destroy();
  }

  // Tight membroker exhaustion on kdu_multi_synthesis::create and missing
  // thread env on MT_DWT exchange_line / get_line + early terminate_queues.
  {
    MultiOptions valid;
    valid.comps = {{32, 32, 8, false, 1, 1},
                   {32, 32, 8, false, 1, 1},
                   {32, 32, 8, false, 1, 1}};
    valid.reversible = true;
    valid.ycc = true;
    valid.enc_flags = KDU_MULTI_XFORM_MT_DWT;
    valid.dec_flags = KDU_MULTI_XFORM_MT_DWT;
    valid.buffer_rows = 1;
    valid.num_threads = 2;
    const auto src = MakeMultiImages(valid.comps);
    const auto bs = EncodeMultiComponent(valid, src);

    // kdu_multi_synthesis tiny membroker(0) throws 0x10071701.
    VectorSource vs(bs);
    ScopedCodestream cs;
    cs.codestream.create(&vs);
    ScopedTile tile;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), nullptr);
    kdu_membroker tiny_broker(0);
    kdu_multi_synthesis syn;
    EXPECT_THROW(syn.create(cs.codestream, tile.tile, nullptr, nullptr,
                            KDU_MULTI_XFORM_DEFAULT_FLAGS, 1, nullptr, nullptr,
                            &tiny_broker),
                 kdu_exception);
    if (syn.exists()) syn.destroy();
    tile.tile.close();
    cs.codestream.destroy();

    // MT_DWT get_line(c, nullptr) throws 0x09081104 / 0x09081103.
    VectorSource vs2(bs);
    ScopedThreadEnv tenv;
    tenv.Create(2);
    kdu_thread_queue q;
    ScopedCodestream cs2;
    cs2.codestream.create(&vs2, &tenv.env);
    ScopedTile tile2;
    tile2.env = &tenv.env;
    tile2.tile = cs2.codestream.open_tile(kdu_coords(0, 0), &tenv.env);
    tenv.env.attach_queue(&q, nullptr, "err_syn_q");
    ScopedMultiSynthesis syn2;
    syn2.env = &tenv.env;
    syn2.engine.create(cs2.codestream, tile2.tile, &tenv.env, &q,
                       KDU_MULTI_XFORM_MT_DWT, 1);
    while (!syn2.engine.start(&tenv.env)) {
    }
    EXPECT_THROW(syn2.engine.get_line(0, nullptr), kdu_exception);
    tenv.env.terminate(&q, true);
    syn2.engine.destroy(&tenv.env);
    tile2.tile.close(&tenv.env);
    tenv.env.terminate(&q, false);
    tenv.env.cs_terminate(cs2.codestream);
    cs2.codestream.destroy();

    // MT_DWT exchange_line(..., nullptr) on stripe boundary + terminate_queues.
    std::vector<kdu_byte> tmp_bs;
    VectorTarget tgt(tmp_bs);
    ScopedThreadEnv tenv2;
    tenv2.Create(2);
    kdu_thread_queue q2;
    ScopedCodestream cs3;
    ConfigureEncoderCodestream(valid, &cs3.siz);
    cs3.codestream.create(&cs3.siz, &tgt, nullptr, 0, 0, &tenv2.env);
    cs3.codestream.access_siz()->parse_string("Creversible=yes");
    cs3.codestream.access_siz()->parse_string("Cycc=yes");
    cs3.codestream.access_siz()->parse_string("Clevels=1");
    cs3.codestream.access_siz()->finalize_all();
    ScopedTile tile3;
    tile3.env = &tenv2.env;
    tile3.tile = cs3.codestream.open_tile(kdu_coords(0, 0), &tenv2.env);
    tenv2.env.attach_queue(&q2, nullptr, "err_ana_q");
    ScopedMultiAnalysis ana;
    ana.env = &tenv2.env;
    ana.engine.create(cs3.codestream, tile3.tile, &tenv2.env, &q2,
                      KDU_MULTI_XFORM_MT_DWT, nullptr, 1);
    for (int c = 0; c < 3; ++c) {
      kdu_line_buf* l = ana.engine.exchange_line(c, nullptr, &tenv2.env);
      if (l != nullptr) {
        WriteLineSample(l, src[c].data(), 32, 8, false);
        ana.engine.exchange_line(c, l, &tenv2.env);
      }
    }
    // Stripe 2 start with env=nullptr triggers 0x09081102 / 0x09081101.
    EXPECT_THROW(
        {
          for (int row = 1; row < 32; ++row) {
            for (int c = 0; c < 3; ++c) {
              kdu_line_buf* l = ana.engine.exchange_line(c, nullptr, nullptr);
              if (l != nullptr) {
                WriteLineSample(l, &src[c][row * 32], 32, 8, false);
                ana.engine.exchange_line(c, l, nullptr);
              }
            }
          }
        },
        kdu_exception);
    tenv2.env.terminate(&q2, true);
    ana.engine.destroy(&tenv2.env);
    tile3.tile.close(&tenv2.env);
    tenv2.env.terminate(&q2, false);
    tenv2.env.cs_terminate(cs3.codestream);
    cs3.codestream.destroy();
  }
  GetErrorHandler()->set_quiet(false);
}

TEST(MultiTransformTest, Rxform64AccNlt15BitAndSharedLut) {
  // 1. 32-bit RXFORM with downshift=14 (lin_bit_depth=20 => 34 > 32) exercises
  // 64-bit accumulator paths for both positive and negative divisors in
  // perform_transform and perform_inverse.
  {
    MultiOptions opts;
    opts.comps = {{16, 12, 20, true, 1, 1}, {16, 12, 20, true, 1, 1}};
    opts.reversible = true;
    opts.ycc = false;
    opts.enc_flags = KDU_MULTI_XFORM_PRECISE;
    opts.dec_flags = KDU_MULTI_XFORM_PRECISE;
    opts.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=2",
        "Mprecision=20",
        "Msigned=yes",
        "Mmatrix_size:I1=6",
        "Mmatrix_coeffs:I1=1,16384,16384,-1,1,-16384",
        "Mstage_inputs:I1={0,1}",
        "Mstage_outputs:I1={0,1}",
        "Mstage_collections:I1={2,2}",
        "Mstage_xforms:I1={MATRIX,1,0,1,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    const auto src = MakeMultiImages(opts.comps);
    const auto bs = EncodeMultiComponent(opts, src);
    const auto dec = DecodeMultiComponent(bs, opts);
    ASSERT_EQ(dec.num_components, 2);
    EXPECT_EQ(dec.int_samples[0], src[0]);
    EXPECT_EQ(dec.int_samples[1], src[1]);
  }

  // 2. Irreversible 16-bit (KDU_MULTI_XFORM_FAST) NLT at precision=15 (> 13)
  // triggers negative downshift (upshift) on synthesis and positive downshift
  // on analysis inside local_smag_int16 and local_umag_int16.
  {
    MultiOptions nlt15;
    nlt15.comps = {{20, 12, 15, true, 1, 1}, {20, 12, 15, false, 1, 1}};
    nlt15.reversible = false;
    nlt15.ycc = false;
    nlt15.enc_flags = KDU_MULTI_XFORM_FAST;
    nlt15.dec_flags = KDU_MULTI_XFORM_FAST;
    nlt15.extra_params = {
        "Sextensions=NLT", "Ncomponents=2",  "Nprecision=15",
        "Nsigned=yes,no",  "NLType:C0=SMAG", "NLType:C1=UMAG",
    };
    auto src = MakeMultiImages(nlt15.comps);
    src[0][0] = -(int64_t{1} << 14) - 500;
    src[0][1] = (int64_t{1} << 14) + 500;
    src[1][0] = -250;
    src[1][1] = (int64_t{1} << 15) + 500;
    const auto bs = EncodeMultiComponent(nlt15, src);
    const auto dec = DecodeMultiComponent(bs, nlt15);
    ASSERT_EQ(dec.num_components, 2);
    EXPECT_NEAR(dec.float_samples[0][2], static_cast<double>(src[0][2]), 64.0);
    EXPECT_NEAR(dec.float_samples[1][2], static_cast<double>(src[1][2]), 64.0);
  }

  // 3. Shared component-agnostic NLT LUT across multiple output components.
  for (int flags : {KDU_MULTI_XFORM_FAST, KDU_MULTI_XFORM_PRECISE}) {
    MultiOptions shared_nlt;
    shared_nlt.comps = {{20, 16, 8, false, 1, 1}, {20, 16, 8, false, 1, 1}};
    shared_nlt.reversible = false;
    shared_nlt.ycc = false;
    shared_nlt.enc_flags = flags;
    shared_nlt.dec_flags = flags;
    shared_nlt.extra_params = {
        "Sextensions=NLT",
        "Ncomponents=2",
        "Nprecision=8",
        "Nsigned=no",
        "NLType=LUT",
        "NLTlut={0.0,1.0,4,12}",
        "NLTdata=0.0,0.3,0.7,1.0",
    };
    const auto src = MakeMultiImages(shared_nlt.comps);
    const auto bs = EncodeMultiComponent(shared_nlt, src);
    const auto dec = DecodeMultiComponent(bs, shared_nlt);
    ASSERT_EQ(dec.num_components, 2);
    EXPECT_EQ(dec.comp_sizes[0].x, 20);
    EXPECT_EQ(dec.comp_sizes[1].x, 20);
  }
}

TEST(MultiTransformTest, FragmentedMtDwtAndRoiError) {
  // 1. Multi-fragment MT_DWT with num_fragments >= 4 (covers frag_sep logic
  // and 16-bit + 32-bit fragment memcpy loops in advance_line).
  for (bool rev : {true, false}) {
    MultiOptions frag;
    frag.comps = {{256, 32, 8, false, 1, 1},
                  {256, 32, 8, false, 1, 1},
                  {256, 32, 8, false, 1, 1}};
    frag.reversible = rev;
    frag.ycc = true;
    frag.dwt_levels = 1;
    frag.frag_width = 32;
    frag.buffer_rows = -1;
    frag.num_threads = 2;
    frag.enc_flags =
        KDU_MULTI_XFORM_MT_DWT | (rev ? 0 : KDU_MULTI_XFORM_PRECISE);
    frag.dec_flags =
        KDU_MULTI_XFORM_MT_DWT | (rev ? 0 : KDU_MULTI_XFORM_PRECISE);
    frag.extra_params = {"Cblk={16,16}"};
    const auto src = MakeMultiImages(frag.comps);
    const auto bs = EncodeMultiComponent(frag, src);
    const auto dec = DecodeMultiComponent(bs, frag);
    ASSERT_EQ(dec.num_components, 3);
    if (rev) {
      for (int c = 0; c < 3; ++c) {
        EXPECT_EQ(dec.int_samples[c], src[c]);
      }
    } else {
      for (int c = 0; c < 3; ++c) {
        for (size_t i = 0; i < src[c].size(); ++i) {
          EXPECT_NEAR(dec.float_samples[c][i], static_cast<double>(src[c][i]),
                      8.0);
        }
      }
    }
  }

  // 2. ROI encoding combined with multiple simultaneous processing fragments
  // throws 0x10031901 cleanly.
  {
    MultiOptions bad_roi_frag;
    bad_roi_frag.comps = {{256, 32, 8, false, 1, 1}};
    bad_roi_frag.reversible = true;
    bad_roi_frag.ycc = false;
    bad_roi_frag.dwt_levels = 1;
    bad_roi_frag.num_threads = 2;
    bad_roi_frag.extra_params = {"Cblk={16,16}"};
    ScopedThreadEnv tenv;
    tenv.Create(2);
    std::vector<kdu_byte> tmp_bs;
    VectorTarget tgt(tmp_bs);
    ScopedCodestream cs;
    ConfigureEncoderCodestream(bad_roi_frag, &cs.siz);
    cs.codestream.create(&cs.siz, &tgt, nullptr, 0, 0, &tenv.env);
    cs.codestream.access_siz()->parse_string("Creversible=yes");
    cs.codestream.access_siz()->parse_string("Cycc=no");
    cs.codestream.access_siz()->parse_string("Clevels=1");
    cs.codestream.access_siz()->parse_string("Cblk={16,16}");
    cs.codestream.access_siz()->finalize_all();
    cs.codestream.configure_simultaneous_processing_fragments(32, 4, 4);
    ScopedTile tile;
    tile.env = &tenv.env;
    tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), &tenv.env);
    kdu_thread_queue root_q;
    tenv.env.attach_queue(&root_q, nullptr, "roi_frag_q");
    ConstRoiImage roi_img;
    kdu_multi_analysis ana;
    GetErrorHandler()->set_quiet(true);
    EXPECT_THROW(ana.create(cs.codestream, tile.tile, &tenv.env, &root_q,
                            KDU_MULTI_XFORM_MT_DWT, &roi_img, -1),
                 kdu_exception);
    if (ana.exists()) {
      ana.destroy(nullptr);
    }
    GetErrorHandler()->set_quiet(false);
    root_q.force_detach(&tenv.env);
    tile.tile.close(&tenv.env);
    tenv.env.cs_terminate(cs.codestream);
    cs.codestream.destroy();
  }
}

TEST(MultiTransformTest, CbrFlushingAndCplexBkgnd) {
  // 3. Scbr incremental CBR flushing (both MT_DWT triple-buffered and
  // single-threaded proc_stripe_height=1) plus kdu_cplex_share /
  // kdu_cplex_bkgnd (including mismatch incompatibility branches).
  {
    kdu_cplex_share cplex_share;
    kdu_cplex_bkgnd cplex_bkgnd;
    auto run_cbr = [&](int comps_count, int dwt_levs, bool threaded,
                       bool use_cplex) {
      MultiOptions opts;
      opts.comps.assign(comps_count, {128, 128, 8, false, 1, 1});
      opts.reversible = false;
      opts.ycc = false;
      opts.dwt_levels = dwt_levs;
      std::vector<kdu_byte> bs;
      VectorTarget tgt(bs);
      ScopedThreadEnv tenv;
      kdu_thread_env* env_ptr = nullptr;
      kdu_thread_queue root_q;
      kdu_thread_queue* q_ptr = nullptr;
      if (threaded) {
        tenv.Create(2);
        env_ptr = &tenv.env;
        q_ptr = &root_q;
      }
      ScopedCodestream cs;
      ConfigureEncoderCodestream(opts, &cs.siz);
      cs.siz.parse_string("Scbr={1,64}");
      static_cast<kdu_params*>(&cs.siz)->finalize();
      cs.codestream.create(&cs.siz, &tgt, nullptr, 0, 0, env_ptr);
      siz_params* params = cs.codestream.access_siz();
      params->parse_string("Creversible=no");
      params->parse_string("Cycc=no");
      params->parse_string(("Clevels=" + std::to_string(dwt_levs)).c_str());
      params->parse_string("Corder=PCRL");
      params->parse_string("Cprecincts={8,8}");
      params->finalize_all();
      // Disable lockless Slope-EST threshold sharing under TSan; Scbr and
      // Cplex paths exercised here only need the byte target.
      cs.codestream.set_slope_hint_policy(0);
      cs.codestream.set_max_bytes(1 << 15);
      if (threaded) {
        cs.codestream.configure_simultaneous_processing_fragments(32, 4, 4);
      }
      ScopedTile tile;
      tile.env = env_ptr;
      tile.tile = cs.codestream.open_tile(kdu_coords(0, 0), env_ptr);
      if (env_ptr != nullptr) {
        env_ptr->attach_queue(q_ptr, nullptr, "cbr_q");
      }
      kdu_push_pull_params extra_pp;
      if (use_cplex) {
        extra_pp.set_cplex_share(&cplex_share);
        extra_pp.set_cplex_bkgnd(&cplex_bkgnd);
      }
      ScopedMultiAnalysis ana;
      ana.env = env_ptr;
      ana.engine.create(
          cs.codestream, tile.tile, env_ptr, q_ptr,
          threaded ? KDU_MULTI_XFORM_MT_DWT : KDU_MULTI_XFORM_DEFAULT_FLAGS,
          nullptr, threaded ? -1 : 4, nullptr, use_cplex ? &extra_pp : nullptr);
      const auto src = MakeMultiImages(opts.comps);
      std::vector<int> rows(comps_count, 0);
      bool any_left = true;
      while (any_left) {
        any_left = false;
        for (int c = 0; c < comps_count; ++c) {
          if (rows[c] >= 128) {
            continue;
          }
          any_left = true;
          kdu_line_buf* line = ana.engine.exchange_line(c, nullptr, env_ptr);
          if (line != nullptr) {
            WriteLineSample(line, &src[c][static_cast<size_t>(rows[c]) * 128],
                            128, 8, false);
            ana.engine.exchange_line(c, line, env_ptr);
            rows[c]++;
          }
        }
      }
      if (env_ptr != nullptr) {
        env_ptr->join(q_ptr, true);
      }
      ana.engine.destroy(env_ptr);
      tile.tile.close(env_ptr);
      if (env_ptr != nullptr) {
        env_ptr->terminate(q_ptr, false);
      }
      kdu_long max_b = 1 << 15;
      cs.codestream.flush(&max_b, 1, nullptr, true, true, 0.0, env_ptr);
      if (env_ptr != nullptr) {
        env_ptr->cs_terminate(cs.codestream);
      }
      cs.codestream.destroy();
      EXPECT_FALSE(bs.empty());
    };
    // Run larger DWT depth first so `kdu_cplex_bkgnd_comp::max_depth` covers
    // depth 2 before the mismatched depth-1 incompatibility check (avoids
    // Kakadu's `cplex_analysis.cpp` max_depth update omission on growth).
    run_cbr(2, 2, true,
            true);  // Configures cplex_share & cplex_bkgnd + MT_DWT.
    run_cbr(2, 1, true,
            true);  // Mismatched dwt_levels -> comp incompatibility.
    run_cbr(3, 1, false,
            true);  // Mismatched comp count -> bkgnd=NULL & 1-thread cbr.
  }

  // 4. 64-bit VPRECISE reversible MCT DWT covers dst64 lifting paths.
  {
    MultiOptions dwt64;
    dwt64.comps = {{16, 12, 20, true, 1, 1},
                   {16, 12, 20, true, 1, 1},
                   {16, 12, 20, true, 1, 1},
                   {16, 12, 20, true, 1, 1}};
    dwt64.reversible = true;
    dwt64.ycc = false;
    dwt64.enc_flags = KDU_MULTI_XFORM_VPRECISE;
    dwt64.dec_flags = KDU_MULTI_XFORM_VPRECISE;
    dwt64.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=4",
        "Mprecision=20",
        "Msigned=yes",
        "Mstage_inputs:I1={0,3}",
        "Mstage_outputs:I1={0,3}",
        "Mstage_collections:I1={4,4}",
        "Mstage_xforms:I1={DWT,1,0,1,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    const auto src = MakeMultiImages(dwt64.comps);
    const auto bs = EncodeMultiComponent(dwt64, src);
    const auto dec = DecodeMultiComponent(bs, dwt64);
    for (int c = 0; c < 4; ++c) {
      EXPECT_EQ(dec.int_samples[c], src[c]);
    }
  }
}

TEST(MultiTransformTest,
     MixedPrecisionCopyNonBypassedNullConstantsAndPropagation) {
  // 1. Reversible codestream decoded into irreversible DEP stage exercises all
  // reversible->irreversible conversions in kd_multi_line::copy (16-bit
  // upshift==0, upshift>0, downshift>0, 32-bit int->float, and 64-bit
  // int->float).
  for (const auto& cfg : std::vector<std::tuple<int, int, int>>{
           {8, 8, KDU_MULTI_XFORM_FAST},
           {8, 13, KDU_MULTI_XFORM_FAST},
           {8, 15, KDU_MULTI_XFORM_FAST},
           {20, 20, KDU_MULTI_XFORM_PRECISE},
           {20, 20, KDU_MULTI_XFORM_VPRECISE},
       }) {
    const int src_prec = std::get<0>(cfg);
    const int out_prec = std::get<1>(cfg);
    const int flags = std::get<2>(cfg);
    MultiOptions opts;
    opts.comps = {{16, 12, src_prec, false, 1, 1},
                  {16, 12, src_prec, false, 1, 1}};
    opts.reversible = true;
    opts.ycc = false;
    opts.dec_flags = flags;
    const std::string pstr = "Mprecision=" + std::to_string(out_prec);
    opts.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=2",
        pstr,
        "Msigned=no",
        "Mtriang_size:I1=1",
        "Mtriang_coeffs:I1=0.25",
        "Mvector_size:I1=2",
        "Mvector_coeffs:I1=2,-1",
        "Mstage_inputs:I1={0,1}",
        "Mstage_outputs:I1={0,1}",
        "Mstage_collections:I1={2,2}",
        "Mstage_xforms:I1={DEP,1,1,0,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    const auto cs_src = MakeMultiImages(opts.comps);
    const auto bs = EncodeRawCodestreamWithMctHeader(opts, cs_src);
    const auto dec = DecodeMultiComponent(bs, opts);
    ASSERT_EQ(dec.num_components, 2);
    EXPECT_EQ(dec.comp_sizes[0].x, 16);
  }

  // Reversible 64-bit codestream decoded into irreversible MCT DWT covers
  // sp64->float conversion in kd_multi_dwt_block::perform_transform.
  {
    MultiOptions rev_to_dwt;
    rev_to_dwt.comps = {{16, 12, 20, true, 1, 1},
                        {16, 12, 20, true, 1, 1},
                        {16, 12, 20, true, 1, 1}};
    rev_to_dwt.reversible = true;
    rev_to_dwt.ycc = false;
    rev_to_dwt.dec_flags = KDU_MULTI_XFORM_VPRECISE;
    rev_to_dwt.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=3",
        "Mprecision=20",
        "Msigned=yes",
        "Mstage_inputs:I1={0,2}",
        "Mstage_outputs:I1={0,2}",
        "Mstage_collections:I1={3,3}",
        "Mstage_xforms:I1={DWT,0,0,1,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    const auto cs_src = MakeMultiImages(rev_to_dwt.comps);
    const auto bs = EncodeRawCodestreamWithMctHeader(rev_to_dwt, cs_src);
    const auto dec = DecodeMultiComponent(bs, rev_to_dwt);
    ASSERT_EQ(dec.num_components, 3);
    EXPECT_EQ(dec.comp_sizes[0].y, 12);
  }

  // 2. Non-bypassed NULL block (have_offset && num_consumers > 1) sharing
  // codestream component 0 with a Stage-1 MATRIX block exercises NULL
  // passthrough in both kd_multi_analysis::advance_line (with pending_mask!=0
  // and dep->row_idx>=new_row_idx) and kd_multi_synthesis::get_line.
  {
    MultiOptions shared_null;
    shared_null.comps = {{24, 16, 8, false, 1, 1}, {24, 16, 8, false, 1, 1}};
    shared_null.reversible = false;
    shared_null.ycc = false;
    shared_null.num_threads = 2;
    shared_null.buffer_rows = 4;
    shared_null.enc_flags = KDU_MULTI_XFORM_MT_DWT | KDU_MULTI_XFORM_PRECISE;
    shared_null.dec_flags = KDU_MULTI_XFORM_MT_DWT | KDU_MULTI_XFORM_PRECISE;
    shared_null.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=3",
        "Mprecision=8",
        "Msigned=no",
        "Mmatrix_size:I1=4",
        "Mmatrix_coeffs:I1=1.0,0.25,0.0,1.0",
        "Mvector_size:I2=1",
        "Mvector_coeffs:I2=4.0",
        "Mstage_inputs:I1={0,1},{0,0}",
        "Mstage_outputs:I1={0,2}",
        "Mstage_collections:I1={2,2},{1,1}",
        "Mstage_xforms:I1={MATRIX,1,0,0,0},{MATRIX,0,2,0,0}",
        "Mmatrix_size:I2=9",
        "Mmatrix_coeffs:I2=1.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,1.0",
        "Mstage_inputs:I2={0,2}",
        "Mstage_outputs:I2={0,2}",
        "Mstage_collections:I2={3,3}",
        "Mstage_xforms:I2={MATRIX,2,0,0,0}",
        "Mnum_stages=2",
        "Mstages=1,2",
    };
    const auto base = MakeMultiImages(shared_null.comps);
    // For synthesis, S0=c0 and T2=c0+4 => O2 = c0+4; give consistent 3 inputs.
    std::vector<std::vector<int64_t>> three_src = {base[0], base[1], base[0]};
    const auto bs = EncodeMultiComponent(shared_null, three_src);
    const auto dec = DecodeMultiComponent(bs, shared_null);
    ASSERT_EQ(dec.num_components, 3);
    EXPECT_NEAR(dec.float_samples[0][0], static_cast<double>(three_src[0][0]),
                8.0);
  }

  // 3. Constant outputs feeding Stage-2 NULL + MATRIX blocks, duplicate hole
  // constant lines, 64-bit VPRECISE reset() with rev_off == 0 and != 0, and
  // unknown intermediate bit-depth propagation into DWT and DEP blocks.
  {
    MultiOptions const_graph;
    const_graph.comps = {{16, 12, 8, false, 1, 1}};
    const_graph.reversible = false;
    const_graph.ycc = false;
    const_graph.dec_flags = KDU_MULTI_XFORM_PRECISE;
    const_graph.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=3",
        "Mprecision=8",
        "Msigned=no",
        "Mvector_size:I1=3",
        "Mvector_coeffs:I1=1,5,5",
        "Mstage_inputs:I1={0,0}",
        "Mstage_outputs:I1={0,2}",
        "Mstage_collections:I1={1,3}",
        "Mstage_xforms:I1={MATRIX,0,1,0,0}",
        "Mmatrix_size:I2=4",
        "Mmatrix_coeffs:I2=1.0,0.5,0.0,1.0",
        "Mstage_inputs:I2={1,1},{0,0},{2,2}",
        "Mstage_outputs:I2={1,1},{0,0},{2,2}",
        "Mstage_collections:I2={1,1},{2,2}",
        "Mstage_xforms:I2={MATRIX,0,0,0,0},{MATRIX,2,0,0,0}",
        "Mnum_stages=2",
        "Mstages=1,2",
    };
    const auto cs_src = MakeMultiImages(const_graph.comps);
    const auto bs = EncodeRawCodestreamWithMctHeader(const_graph, cs_src);
    const auto dec = DecodeMultiComponent(bs, const_graph);
    EXPECT_EQ(dec.num_components, 3);
  }

  // Constant holes on Stage 1 + Stage 2 producing matching constant lines
  // plus 64-bit VPRECISE reset() with rev_off==0 and rev_off!=0 in DEP.
  for (int rev_off : {0, 7}) {
    MultiOptions reset64;
    reset64.comps = {{16, 12, 20, true, 1, 1}, {16, 12, 20, true, 1, 1}};
    reset64.reversible = true;
    reset64.ycc = false;
    reset64.dec_flags = KDU_MULTI_XFORM_VPRECISE;
    reset64.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=4",
        "Mprecision=20",
        "Msigned=yes",
        "Mstage_inputs:I1={0,1}",
        "Mstage_outputs:I1={0,3}",
        "Mstage_collections:I1={2,4}",
        "Mstage_xforms:I1={MATRIX,0,0,0,0}",
        "Mnum_stages=1",
        "Mstages=1",
        "Mtriang_size:I2=2",
        "Mtriang_coeffs:I2=1,1",
        "Mvector_size:I2=2",
        "Mvector_coeffs:I2=0," + std::to_string(rev_off),
        "Mstage_inputs:I2={0,1}",
        "Mstage_outputs:I2={0,0},{3,3}",
        "Mstage_collections:I2={2,2}",
        "Mstage_xforms:I2={DEP,2,2,1,0}",
        "Mnum_stages:T0=1",
        "Mstages:T0=2",
    };
    const auto cs_src = MakeMultiImages(reset64.comps);
    const auto bs = EncodeRawCodestreamWithMctHeader(reset64, cs_src);
    const auto dec = DecodeMultiComponent(bs, reset64);
    EXPECT_EQ(dec.num_components, 4);
  }

  // Unknown intermediate bit-depths propagated backward from DWT (and DEP)
  // in Stage 2 into Stage-1 MATRIX outputs, across both 32-bit precise float
  // and 16-bit fixed-point (covering mixed-bit-depth copy upshift/downshift).
  for (const std::string& stage2_xform :
       {std::string("Mstage_xforms:I2={DWT,0,0,1,0}"),
        std::string("Mstage_xforms:I2={DEP,2,2,0,0}")}) {
    for (int flags : {KDU_MULTI_XFORM_PRECISE, KDU_MULTI_XFORM_DEFAULT_FLAGS}) {
      SCOPED_TRACE(stage2_xform);
      SCOPED_TRACE(flags);
      MultiOptions prop;
      prop.comps = {{16, 12, 8, false, 1, 1}, {16, 12, 8, false, 1, 1}};
      prop.reversible = false;
      prop.ycc = false;
      prop.enc_flags = flags;
      prop.dec_flags = flags;
      prop.extra_params = {
          "Sextensions=MCT",
          "Mcomponents=2",
          "Mprecision=8",
          "Msigned=no",
          "Mmatrix_size:I1=4",
          "Mmatrix_coeffs:I1=1.0,0.25,0.0,1.0",
          "Mstage_inputs:I1={0,1}",
          "Mstage_outputs:I1={0,1}",
          "Mstage_collections:I1={2,2}",
          "Mstage_xforms:I1={MATRIX,1,0,0,0}",
          "Mtriang_size:I2=1",
          "Mtriang_coeffs:I2=0.5",
          "Mvector_size:I2=2",
          "Mvector_coeffs:I2=0,0",
          "Mstage_inputs:I2={0,1}",
          "Mstage_outputs:I2={0,1}",
          "Mstage_collections:I2={2,2}",
          stage2_xform,
          "Mnum_stages=2",
          "Mstages=1,2",
      };
      const auto src = MakeMultiImages(prop.comps);
      const auto bs = EncodeMultiComponent(prop, src);
      const auto dec = DecodeMultiComponent(bs, prop);
      ASSERT_EQ(dec.num_components, 2);
      EXPECT_NEAR(dec.float_samples[0][0], static_cast<double>(src[0][0]),
                  12.0);
    }
  }

  // Stage-1 NULL feeding Stage-2 MATRIX exercises
  // kd_multi_analysis::advance_line passthrough on a NULL block bound to
  // codestream components (pending_mask).
  {
    MultiOptions null_then_matrix;
    null_then_matrix.comps = {{16, 12, 8, false, 1, 1},
                              {16, 12, 8, false, 1, 1}};
    null_then_matrix.reversible = false;
    null_then_matrix.ycc = false;
    null_then_matrix.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=2",
        "Mprecision=8",
        "Msigned=no",
        "Mstage_inputs:I1={0,1}",
        "Mstage_outputs:I1={0,1}",
        "Mstage_collections:I1={2,2}",
        "Mstage_xforms:I1={MATRIX,0,0,0,0}",
        "Mmatrix_size:I1=4",
        "Mmatrix_coeffs:I1=1.0,0.25,0.0,1.0",
        "Mstage_inputs:I2={0,1}",
        "Mstage_outputs:I2={0,1}",
        "Mstage_collections:I2={2,2}",
        "Mstage_xforms:I2={MATRIX,1,0,0,0}",
        "Mnum_stages=2",
        "Mstages=1,2",
    };
    const auto src = MakeMultiImages(null_then_matrix.comps);
    const auto bs = EncodeMultiComponent(null_then_matrix, src);
    const auto dec = DecodeMultiComponent(bs, null_then_matrix);
    ASSERT_EQ(dec.num_components, 2);
    EXPECT_NEAR(dec.float_samples[0][0], static_cast<double>(src[0][0]), 12.0);
  }
}

}  // namespace
}  // namespace kdu_core
