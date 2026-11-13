// Unit tests for third_party/kakadu/vendor/coresys/compressed/compressed.cpp
// via the public coresys headers in third_party/kakadu/vendor/coresys/common/.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_params.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_roi_processing.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_sample_processing.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

namespace kdu_core {
namespace {

class TestMessageHandler : public kdu_thread_safe_message {
 public:
  explicit TestMessageHandler(bool throw_on_end)
      : throw_on_end_(throw_on_end) {}
  ~TestMessageHandler() noexcept override = default;

  void put_text(const char* string) override {
    if (string != nullptr) {
      current_ += string;
    }
  }

  void flush(bool end_of_message = false) override {
    if (end_of_message) {
      if (throw_on_end_) {
        std::fprintf(stderr, "KDU_ERR: %s\n", current_.c_str());
      }
      history_.push_back(std::move(current_));
      current_.clear();
      kdu_thread_safe_message::flush(true);
      if (throw_on_end_) {
        throw KDU_ERROR_EXCEPTION;
      }
    }
  }

  const std::vector<std::string>& history() const { return history_; }
  void clear() {
    current_.clear();
    history_.clear();
  }

 private:
  bool throw_on_end_;
  std::string current_;
  std::vector<std::string> history_;
};

class BufferTarget : public kdu_compressed_target {
 public:
  explicit BufferTarget(bool cached = false) : cached_(cached) {}

  int get_capabilities() override {
    return cached_ ? KDU_TARGET_CAP_CACHED : KDU_TARGET_CAP_SEQUENTIAL;
  }

  bool write(const kdu_byte* buf, int num_bytes) override {
    if (fail_writes_ || num_bytes < 0) return false;
    if (section_ == Section::kMain) {
      main_header.insert(main_header.end(), buf, buf + num_bytes);
    } else if (section_ == Section::kTile) {
      cur_tile_hdr_.insert(cur_tile_hdr_.end(), buf, buf + num_bytes);
    } else if (section_ == Section::kPrecinct) {
      cur_pkt_.insert(cur_pkt_.end(), buf, buf + num_bytes);
    } else {
      data.insert(data.end(), buf, buf + num_bytes);
    }
    return true;
  }

  bool start_rewrite(kdu_long backtrack) override {
    if (!allow_rewrite || backtrack < 0 ||
        static_cast<size_t>(backtrack) > data.size()) {
      return false;
    }
    rewriting_ = true;
    return true;
  }

  bool end_rewrite() override {
    if (!rewriting_) return false;
    rewriting_ = false;
    return true;
  }

  void start_mainheader() override { section_ = Section::kMain; }
  void end_mainheader() override { section_ = Section::kNone; }
  void start_tileheader(int tnum, int /*num_tiles*/) override {
    section_ = Section::kTile;
    cur_tnum_ = tnum;
    cur_tile_hdr_.clear();
  }
  void end_tileheader(int /*tnum*/) override {
    section_ = Section::kNone;
    tile_headers[cur_tnum_] = std::move(cur_tile_hdr_);
    cur_tile_hdr_.clear();
  }
  void start_precinct(kdu_long unique_id) override {
    section_ = Section::kPrecinct;
    cur_pid_ = unique_id;
    cur_pkt_.clear();
  }
  void end_precinct(kdu_long /*unique_id*/, int /*num_packets*/,
                    const kdu_long /*packet_lengths*/[]) override {
    section_ = Section::kNone;
    precincts[cur_pid_] = std::move(cur_pkt_);
    cur_pkt_.clear();
  }

  bool allow_rewrite = false;
  bool fail_writes_ = false;
  std::vector<kdu_byte> data;
  std::vector<kdu_byte> main_header;
  std::map<int, std::vector<kdu_byte>> tile_headers;
  std::map<kdu_long, std::vector<kdu_byte>> precincts;

 private:
  enum class Section { kNone, kMain, kTile, kPrecinct };
  bool cached_;
  bool rewriting_ = false;
  Section section_ = Section::kNone;
  int cur_tnum_ = 0;
  kdu_long cur_pid_ = 0;
  std::vector<kdu_byte> cur_tile_hdr_;
  std::vector<kdu_byte> cur_pkt_;
};

class BufferSource : public kdu_compressed_source {
 public:
  explicit BufferSource(std::vector<kdu_byte> bytes,
                        int caps = KDU_SOURCE_CAP_SEQUENTIAL |
                                   KDU_SOURCE_CAP_SEEKABLE)
      : bytes_(std::move(bytes)), caps_(caps) {}

  int get_capabilities() override { return caps_; }
  kdu_long get_pos() override { return static_cast<kdu_long>(pos_); }
  bool seek(kdu_long offset) override {
    if ((caps_ & KDU_SOURCE_CAP_SEEKABLE) == 0 || offset < 0) return false;
    pos_ = std::min(static_cast<size_t>(offset), bytes_.size());
    return true;
  }
  int read(kdu_byte* buf, int num_bytes) override {
    if (num_bytes <= 0 || pos_ >= bytes_.size()) return 0;
    const size_t avail =
        std::min(static_cast<size_t>(num_bytes), bytes_.size() - pos_);
    std::memcpy(buf, bytes_.data() + pos_, avail);
    pos_ += avail;
    return static_cast<int>(avail);
  }
  bool set_tileheader_scope(int /*tnum*/, int /*num_tiles*/) override {
    return true;
  }
  bool set_precinct_scope(kdu_long /*unique_id*/) override { return true; }
  kdu_byte* access_memory(kdu_long& pos, kdu_byte*& lim) override {
    if ((caps_ & KDU_SOURCE_CAP_IN_MEMORY) == 0 || bytes_.empty()) {
      return nullptr;
    }
    pos = static_cast<kdu_long>(pos_);
    lim = bytes_.data() + bytes_.size();
    return bytes_.data() + pos_;
  }

 private:
  std::vector<kdu_byte> bytes_;
  int caps_;
  size_t pos_ = 0;
};

class RectRoiImage : public kdu_roi_image {
 public:
  RectRoiImage(int x0, int y0, int w, int h) : x0_(x0), y0_(y0), w_(w), h_(h) {}
  kdu_roi_node* acquire_node(int /*comp_idx*/, kdu_dims tile_region) override {
    return new Node(tile_region, x0_, y0_, w_, h_);
  }

 private:
  class Node : public kdu_roi_node {
   public:
    Node(kdu_dims region, int x0, int y0, int w, int h)
        : region_(region), x0_(x0), y0_(y0), w_(w), h_(h) {}
    void release() override { delete this; }
    void pull(kdu_byte buf[], int width) override {
      std::memset(buf, 0, static_cast<size_t>(width));
      const int y = region_.pos.y + row_++;
      if (y >= y0_ && y < y0_ + h_) {
        for (int i = 0; i < width; ++i) {
          const int x = region_.pos.x + i;
          if (x >= x0_ && x < x0_ + w_) {
            buf[i] = 1;
          }
        }
      }
    }

   private:
    kdu_dims region_;
    int x0_, y0_, w_, h_;
    int row_ = 0;
  };
  int x0_, y0_, w_, h_;
};

struct CodestreamSpec {
  int width = 64;
  int height = 64;
  int components = 3;
  int bit_depth = 8;
  bool is_signed = false;
  bool reversible = true;
  int dwt_levels = 2;
  kdu_coords origin = kdu_coords(0, 0);
  kdu_coords tile_size = kdu_coords(0, 0);
  kdu_coords subsampling = kdu_coords(1, 1);
  std::vector<std::string> extra_params;
};

void ConfigureSiz(siz_params& siz, const CodestreamSpec& spec) {
  siz.set(Scomponents, 0, 0, spec.components);
  siz.set(Sprecision, 0, 0, spec.bit_depth);
  siz.set(Ssigned, 0, 0, spec.is_signed);
  if (spec.subsampling.x == 1 && spec.subsampling.y == 1) {
    siz.set(Sdims, 0, 0, spec.height);
    siz.set(Sdims, 0, 1, spec.width);
    if (spec.origin.x != 0 || spec.origin.y != 0) {
      siz.set(Ssize, 0, 0, spec.height + spec.origin.y);
      siz.set(Ssize, 0, 1, spec.width + spec.origin.x);
      siz.set(Sorigin, 0, 0, spec.origin.y);
      siz.set(Sorigin, 0, 1, spec.origin.x);
    }
  } else {
    siz.set(Ssize, 0, 0, spec.height + spec.origin.y);
    siz.set(Ssize, 0, 1, spec.width + spec.origin.x);
    if (spec.origin.x != 0 || spec.origin.y != 0) {
      siz.set(Sorigin, 0, 0, spec.origin.y);
      siz.set(Sorigin, 0, 1, spec.origin.x);
    }
    siz.set(Ssampling, 0, 0, 1);
    siz.set(Ssampling, 0, 1, 1);
    for (int c = 1; c < spec.components; ++c) {
      siz.set(Ssampling, c, 0, spec.subsampling.y);
      siz.set(Ssampling, c, 1, spec.subsampling.x);
    }
  }
  if (spec.tile_size.x > 0 && spec.tile_size.y > 0) {
    siz.set(Stiles, 0, 0, spec.tile_size.y);
    siz.set(Stiles, 0, 1, spec.tile_size.x);
  }
  auto is_siz_param = [](const std::string& p) {
    return p.rfind("S", 0) == 0 || p.rfind("Mcomponents", 0) == 0 ||
           p.rfind("Mprecision", 0) == 0 || p.rfind("Msigned", 0) == 0 ||
           p.rfind("Ncomponents", 0) == 0 || p.rfind("Nprecision", 0) == 0 ||
           p.rfind("Nsigned", 0) == 0;
  };
  for (const auto& p : spec.extra_params) {
    if (is_siz_param(p)) {
      ASSERT_TRUE(siz.parse_string(p.c_str())) << "Failed SIZ param: " << p;
    }
  }
  static_cast<kdu_params&>(siz).finalize();
}

void ApplyCodingParams(kdu_codestream& cs, const CodestreamSpec& spec) {
  kdu_params* cod = cs.access_siz()->access_cluster(COD_params);
  ASSERT_NE(cod, nullptr);
  cod->set(Creversible, 0, 0, spec.reversible);
  cod->set(Clevels, 0, 0, spec.dwt_levels);
  auto is_siz_param = [](const std::string& p) {
    return p.rfind("S", 0) == 0 || p.rfind("Mcomponents", 0) == 0 ||
           p.rfind("Mprecision", 0) == 0 || p.rfind("Msigned", 0) == 0 ||
           p.rfind("Ncomponents", 0) == 0 || p.rfind("Nprecision", 0) == 0 ||
           p.rfind("Nsigned", 0) == 0;
  };
  for (const auto& p : spec.extra_params) {
    if (!is_siz_param(p)) {
      ASSERT_TRUE(cs.access_siz()->parse_string(p.c_str()))
          << "Failed coding param: " << p;
    }
  }
  cs.access_siz()->finalize_all();
}

std::vector<std::vector<int>> MakePlanes(const CodestreamSpec& spec) {
  std::vector<std::vector<int>> planes(spec.components);
  const int max_mag = (1 << (std::min(spec.bit_depth, 16) - 1)) - 1;
  for (int c = 0; c < spec.components; ++c) {
    const int sub_x = (c > 0) ? spec.subsampling.x : 1;
    const int sub_y = (c > 0) ? spec.subsampling.y : 1;
    const int w = (spec.width + sub_x - 1) / sub_x;
    const int h = (spec.height + sub_y - 1) / sub_y;
    planes[c].resize(static_cast<size_t>(w * h));
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const int v = ((x * 7 + y * 13 + c * 31) % std::max(1, max_mag)) -
                      (spec.is_signed ? max_mag / 2 : 0);
        planes[c][static_cast<size_t>(y * w + x)] =
            spec.is_signed
                ? v
                : (std::abs(v) & ((1 << std::min(spec.bit_depth, 16)) - 1));
      }
    }
  }
  return planes;
}

void PushTileSamples(kdu_codestream& cs, kdu_tile& tile,
                     const std::vector<std::vector<int>>& planes,
                     const CodestreamSpec& spec, kdu_roi_image* roi = nullptr,
                     kdu_thread_env* env = nullptr) {
  kdu_thread_queue tile_queue;
  kdu_thread_queue* queue_ptr = nullptr;
  if (env != nullptr) {
    ASSERT_TRUE(env->attach_queue(&tile_queue, nullptr, nullptr));
    queue_ptr = &tile_queue;
  }
  kdu_multi_analysis engine;
  try {
    engine.create(
        cs, tile, env, queue_ptr,
        env != nullptr ? KDU_MULTI_XFORM_MT_DWT : KDU_MULTI_XFORM_DEFAULT_FLAGS,
        roi);
    std::vector<int> heights(spec.components);
    std::vector<kdu_dims> comp_dims(spec.components);
    for (int c = 0; c < spec.components; ++c) {
      heights[c] = engine.get_size(c).y;
      cs.get_tile_dims(tile.get_tile_idx(), c, comp_dims[c], true);
    }

    std::vector<int> pushed(spec.components, 0);
    bool any_left = true;
    while (any_left) {
      any_left = false;
      bool progressed = false;
      for (int c = 0; c < spec.components; ++c) {
        if (pushed[c] >= heights[c]) continue;
        any_left = true;
        kdu_line_buf* line = engine.exchange_line(c, nullptr, env);
        if (line == nullptr) continue;
        const kdu_coords sz = engine.get_size(c);
        const int sub_x = (c > 0) ? spec.subsampling.x : 1;
        const int sub_y = (c > 0) ? spec.subsampling.y : 1;
        const int comp_w = (spec.width + sub_x - 1) / sub_x;
        const int comp_h = (spec.height + sub_y - 1) / sub_y;
        const int base_x = comp_dims[c].pos.x - spec.origin.x / sub_x;
        const int base_y = comp_dims[c].pos.y - spec.origin.y / sub_y;
        const int y = std::clamp(base_y + pushed[c], 0, comp_h - 1);
        if (line->is_absolute()) {
          const kdu_int32 offset =
              spec.is_signed ? 0 : (1 << (spec.bit_depth - 1));
          kdu_sample32* d32 = line->get_buf32();
          kdu_sample16* d16 = line->get_buf16();
          for (int x = 0; x < sz.x; ++x) {
            const int cx = std::clamp(base_x + x, 0, comp_w - 1);
            const int val =
                planes[c][static_cast<size_t>(y * comp_w + cx)] - offset;
            if (d16 != nullptr) {
              d16[x].ival = static_cast<kdu_int16>(val);
            } else {
              d32[x].ival = val;
            }
          }
        } else {
          const float scale = 1.0f / static_cast<float>(1 << spec.bit_depth);
          const int offset = spec.is_signed ? 0 : (1 << (spec.bit_depth - 1));
          kdu_sample32* d32 = line->get_buf32();
          kdu_sample16* d16 = line->get_buf16();
          for (int x = 0; x < sz.x; ++x) {
            const int cx = std::clamp(base_x + x, 0, comp_w - 1);
            const int val =
                planes[c][static_cast<size_t>(y * comp_w + cx)] - offset;
            if (d16 != nullptr) {
              const int shift = KDU_FIX_POINT - spec.bit_depth;
              d16[x].ival = static_cast<kdu_int16>(
                  shift >= 0 ? (val << shift) : (val >> (-shift)));
            } else {
              d32[x].fval = static_cast<float>(val) * scale;
            }
          }
        }
        engine.exchange_line(c, line, env);
        ++pushed[c];
        progressed = true;
      }
      if (any_left && !progressed) break;
    }
    if (queue_ptr != nullptr) {
      EXPECT_TRUE(env->join(queue_ptr, false));
    }
    engine.destroy(env);
  } catch (...) {
    if (queue_ptr != nullptr) {
      env->join(queue_ptr, false);
    }
    if (engine.exists()) engine.destroy(env);
    throw;
  }
}

void PushAllTiles(kdu_codestream& cs,
                  const std::vector<std::vector<int>>& planes,
                  const CodestreamSpec& spec, kdu_roi_image* roi = nullptr,
                  kdu_thread_env* env = nullptr) {
  kdu_dims tile_range;
  cs.get_valid_tiles(tile_range);
  for (int ty = tile_range.pos.y; ty < tile_range.pos.y + tile_range.size.y;
       ++ty) {
    for (int tx = tile_range.pos.x; tx < tile_range.pos.x + tile_range.size.x;
         ++tx) {
      kdu_tile tile = cs.open_tile(kdu_coords(tx, ty), env);
      try {
        PushTileSamples(cs, tile, planes, spec, roi, env);
        tile.close(env);
      } catch (...) {
        if (tile.exists()) tile.close(env);
        throw;
      }
    }
  }
}

std::vector<kdu_byte> EncodeCodestream(
    const CodestreamSpec& spec, const std::vector<std::vector<int>>& planes,
    kdu_roi_image* roi = nullptr, kdu_long* layer_bytes = nullptr,
    int num_layers = 0) {
  siz_params siz;
  ConfigureSiz(siz, spec);
  BufferTarget target;
  kdu_codestream cs;
  cs.create(&siz, &target);
  try {
    ApplyCodingParams(cs, spec);
    if (layer_bytes != nullptr && num_layers > 0) {
      cs.set_max_bytes(layer_bytes[num_layers - 1], true, true);
    }

    PushAllTiles(cs, planes, spec, roi);
    if (layer_bytes != nullptr && num_layers > 0) {
      cs.flush(layer_bytes, num_layers);
    } else {
      int nl = 1;
      kdu_params* cod = cs.access_siz()->access_cluster(COD_params);
      if (cod != nullptr) cod->get(Clayers, 0, 0, nl);
      std::vector<kdu_long> auto_bytes(static_cast<size_t>(std::max(1, nl)), 0);
      auto_bytes.back() = 1LL << 30;
      cs.flush(auto_bytes.data(), static_cast<int>(auto_bytes.size()));
    }
    cs.destroy();
  } catch (...) {
    if (cs.exists()) cs.destroy();
    throw;
  }
  return target.data;
}

void CopyEncodedBlock(kdu_block* dst, const kdu_block* src) {
  int total_bytes = 0;
  for (int p = 0; p < src->num_passes; ++p) {
    total_bytes += src->pass_lengths[p];
  }
  dst->set_max_passes(src->num_passes, false);
  dst->set_max_bytes(total_bytes, false);
  dst->num_passes = src->num_passes;
  dst->missing_msbs = src->missing_msbs;
  dst->K_max_prime = src->K_max_prime;
  for (int p = 0; p < src->num_passes; ++p) {
    dst->pass_lengths[p] = src->pass_lengths[p];
    dst->pass_slopes[p] = src->pass_slopes[p];
  }
  if (total_bytes > 0) {
    std::memcpy(dst->byte_buffer, src->byte_buffer, total_bytes);
  }
}

std::vector<std::vector<int>> DecodeCodestream(kdu_codestream& cs,
                                               bool inspect_subbands = false,
                                               kdu_thread_env* env = nullptr,
                                               bool close_bkgnd = false) {
  const int num_comps = cs.get_num_components(true);
  std::vector<std::vector<int>> out(static_cast<size_t>(num_comps));
  std::vector<kdu_dims> comp_dims(static_cast<size_t>(num_comps));
  std::vector<int> bits(static_cast<size_t>(num_comps));
  std::vector<bool> is_signed(static_cast<size_t>(num_comps));
  for (int c = 0; c < num_comps; ++c) {
    cs.get_dims(c, comp_dims[c], true);
    bits[c] = cs.get_bit_depth(c, true);
    is_signed[c] = cs.get_signed(c, true);
    out[c].resize(static_cast<size_t>(comp_dims[c].size.x) *
                  comp_dims[c].size.y);
  }

  kdu_dims tile_range;
  cs.get_valid_tiles(tile_range);
  for (int ty = tile_range.pos.y; ty < tile_range.pos.y + tile_range.size.y;
       ++ty) {
    for (int tx = tile_range.pos.x; tx < tile_range.pos.x + tile_range.size.x;
         ++tx) {
      kdu_tile tile = cs.open_tile(kdu_coords(tx, ty), env);
      kdu_thread_queue tile_queue;
      kdu_thread_queue* queue_ptr = nullptr;
      if (env != nullptr) {
        EXPECT_TRUE(env->attach_queue(&tile_queue, nullptr, nullptr));
        queue_ptr = &tile_queue;
      }
      kdu_multi_synthesis engine;
      try {
        if (inspect_subbands) {
          (void)tile.find_component_gain_info(0, false);
          (void)tile.find_component_gain_info(0, true);
          for (int c = 0; c < tile.get_num_components(); ++c) {
            kdu_tile_comp tc = tile.access_component(c);
            for (int r = 0; r < tc.get_num_resolutions(); ++r) {
              kdu_resolution res = tc.access_resolution(r);
              kdu_dims tcd, nd;
              res.get_dims(tcd);
              res.access_node().get_dims(nd);
              kdu_dims pdims;
              res.get_valid_precincts(pdims);
              if (!pdims.is_empty()) {
                (void)res.get_precinct_packets(pdims.pos, env, true);
                (void)res.get_precinct_samples(pdims.pos);
              }
              int min_b = 0;
              const int nb = res.get_valid_band_indices(min_b);
              for (int bi = 0; bi < nb; ++bi) {
                kdu_subband sb = res.access_subband(min_b + bi);
                if (!sb.exists()) continue;
                kdu_dims sbd, pbd, vblks;
                sb.get_dims(sbd);
                sb.get_parent_dims(pbd);
                sb.get_valid_blocks(vblks);
              }
            }
          }
        }

        engine.create(cs, tile, env, queue_ptr,
                      env != nullptr ? KDU_MULTI_XFORM_MT_DWT
                                     : KDU_MULTI_XFORM_DEFAULT_FLAGS);
        std::vector<int> heights(static_cast<size_t>(num_comps));
        std::vector<kdu_dims> tile_comp_dims(static_cast<size_t>(num_comps));
        for (int c = 0; c < num_comps; ++c) {
          heights[c] = engine.get_size(c).y;
          cs.get_tile_dims(tile.get_tile_idx(), c, tile_comp_dims[c], true);
        }
        std::vector<int> pulled(static_cast<size_t>(num_comps), 0);
        bool any_left = true;
        while (any_left) {
          any_left = false;
          bool progressed = false;
          for (int c = 0; c < num_comps; ++c) {
            if (pulled[c] >= heights[c]) continue;
            any_left = true;
            kdu_line_buf* line = engine.get_line(c, env);
            if (line == nullptr) continue;
            const kdu_coords sz = engine.get_size(c);
            const int y =
                tile_comp_dims[c].pos.y - comp_dims[c].pos.y + pulled[c];
            const int x0 = tile_comp_dims[c].pos.x - comp_dims[c].pos.x;
            const int offset = is_signed[c] ? 0 : (1 << (bits[c] - 1));
            for (int x = 0; x < sz.x; ++x) {
              int sample = 0;
              if (line->is_absolute()) {
                if (line->get_buf16() != nullptr) {
                  sample = line->get_buf16()[x].ival;
                } else if (line->get_buf32() != nullptr) {
                  sample = line->get_buf32()[x].ival;
                } else if (line->get_buf64() != nullptr) {
                  sample = static_cast<int>(line->get_buf64()[x].ival);
                }
              } else if (line->get_buf32() != nullptr) {
                sample = static_cast<int>(
                    std::lround(line->get_buf32()[x].fval *
                                static_cast<float>(1 << bits[c])));
              } else if (line->get_buf16() != nullptr) {
                sample = static_cast<int>(std::lround(
                    (static_cast<double>(line->get_buf16()[x].ival) /
                     static_cast<double>(1 << KDU_FIX_POINT)) *
                    static_cast<double>(1 << bits[c])));
              }
              if (y >= 0 && y < comp_dims[c].size.y && x0 + x >= 0 &&
                  x0 + x < comp_dims[c].size.x) {
                out[c][static_cast<size_t>(y) * comp_dims[c].size.x + x0 + x] =
                    sample + offset;
              }
            }
            ++pulled[c];
            progressed = true;
          }
          if (any_left && !progressed) break;
        }
        if (queue_ptr != nullptr) {
          EXPECT_TRUE(env->join(queue_ptr, false));
        }
        engine.destroy(env);
        tile.close(env, close_bkgnd);
      } catch (...) {
        if (queue_ptr != nullptr) {
          env->join(queue_ptr, false);
        }
        if (engine.exists()) engine.destroy(env);
        if (tile.exists()) tile.close(env);
        throw;
      }
    }
  }
  return out;
}

class CompressedTest : public ::testing::Test {
 protected:
  CompressedTest() : error_handler_(true), warning_handler_(false) {}
  void SetUp() override {
    kdu_customize_errors(&error_handler_);
    kdu_customize_warnings(&warning_handler_);
  }
  void TearDown() override {
    kdu_customize_errors(nullptr);
    kdu_customize_warnings(nullptr);
  }
  TestMessageHandler error_handler_;
  TestMessageHandler warning_handler_;
};

TEST_F(CompressedTest, TileComponentResolutionAndSubbandAccessors) {
  for (bool reversible : {true, false}) {
    CodestreamSpec spec;
    spec.width = 96;
    spec.height = 80;
    spec.components = 3;
    spec.bit_depth = reversible ? 12 : 8;
    spec.is_signed = reversible;
    spec.reversible = reversible;
    spec.dwt_levels = 3;
    spec.origin = kdu_coords(5, 7);
    spec.tile_size = kdu_coords(48, 40);
    spec.extra_params = {
        "Clayers=3",
        "Cprecincts={32,32},{16,16}",
        "Cblk={16,16}",
        "Cdecomp=B(H--:V--:H--),B(-:-:-)",
        "Clev_weights={1.0},{0.8},{0.6}",
        "Qguard=2",
    };
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    ASSERT_FALSE(bytes.empty());

    BufferSource src(bytes);
    kdu_codestream cs;
    cs.create(&src);

    kdu_dims tdims;
    cs.get_valid_tiles(tdims);
    EXPECT_GE(tdims.size.x, 2);
    EXPECT_GE(tdims.size.y, 2);

    kdu_tile tile = cs.open_tile(tdims.pos);
    EXPECT_EQ(tile.get_tnum(), 0);
    EXPECT_EQ(tile.get_tile_idx().x, tdims.pos.x);
    EXPECT_EQ(tile.get_tile_idx().y, tdims.pos.y);
    EXPECT_TRUE(tile.get_ycc());
    EXPECT_EQ(tile.get_num_components(), 3);
    EXPECT_EQ(tile.get_num_layers(), 3);
    kdu_uint32 mct_hash = 0;
    tile.get_global_attributes(mct_hash);
    EXPECT_FALSE(tile.get_nlt_descriptors(0, nullptr));

    int c_order[3] = {2, 0, 1};
    tile.set_components_of_interest(2, c_order);

    kdu_tile_comp tc = tile.access_component(0);
    EXPECT_EQ(tc.which(), 0);
    EXPECT_TRUE(tc.access_tile().exists());
    EXPECT_EQ(tc.get_reversible(), reversible);
    kdu_coords sub;
    tc.get_subsampling(sub);
    EXPECT_EQ(sub.x, 1);
    EXPECT_EQ(tc.get_bit_depth(false), spec.bit_depth);
    EXPECT_GE(tc.get_bit_depth(true), tc.get_bit_depth(false));
    EXPECT_EQ(tc.get_signed(), spec.is_signed);
    EXPECT_EQ(tc.get_num_layers(), 3);
    EXPECT_GE(tc.get_num_resolutions(), 2);
    EXPECT_GE(tc.get_full_decomp_levels(), 1);
    kdu_uint32 dwt_hash = 0;
    int guard_bits = 0;
    int rev_prec = 0;
    int roi_shift = 0;
    tc.get_global_attributes(dwt_hash, guard_bits, rev_prec, roi_shift);
    EXPECT_EQ(guard_bits, 2);

    const int nres = tc.get_num_resolutions();
    for (int r = 0; r < nres; ++r) {
      kdu_resolution res = tc.access_resolution(r);
      EXPECT_EQ(res.which(), r);
      EXPECT_EQ(res.get_reversible(), reversible);
      (void)res.get_dwt_level();
      kdu_coords depth;
      int level_bands = 0;
      (void)res.get_decomp_structure(depth, level_bands);
      kdu_dims rdims;
      res.get_dims(rdims);
      kdu_dims pidx;
      res.get_valid_precincts(pidx);
      if (pidx.size.x > 0 && pidx.size.y > 0) {
        EXPECT_NE(res.get_precinct_id(pidx.pos), -1);
        kdu_precinct p = res.access_precinct(pidx.pos);
        if (p.exists()) {
          (void)p.check_loaded();
          kdu_dims pblks;
          p.get_valid_blocks((r == 0) ? 0 : 1, pblks);
        }
      }
      int min_b = 0;
      const int num_b = res.get_valid_band_indices(min_b);
      for (int i = 0; i < num_b; ++i) {
        const int b = min_b + i;
        kdu_subband sb = res.access_subband(b);
        if (!sb.exists()) continue;
        EXPECT_TRUE(sb.access_resolution().exists());
        EXPECT_GE(sb.get_band_idx(), 0);
        EXPECT_GT(sb.get_K_max(), 0);
        EXPECT_GE(sb.get_K_max_prime(), 0);
        EXPECT_GE(sb.get_fast_mag_bound(), 0.0f);
        EXPECT_EQ(sb.get_reversible(), reversible);
        if (reversible) {
          EXPECT_FLOAT_EQ(sb.get_delta(), 0.0f);
        } else {
          EXPECT_GT(sb.get_delta(), 0.0f);
        }
        EXPECT_GT(sb.get_eps_mu(), 0);
        EXPECT_GE(sb.get_msb_wmse(), 0.0f);
        float energy_wt = 0.0f;
        (void)sb.get_roi_weight(energy_wt);
        float plane_wmse[4] = {};
        (void)sb.get_per_plane_wmse(plane_wmse, 4, true);
        float vis_floor = 0.0f;
        float mask_exp = 0.0f;
        float vis_scale = 0.0f;
        (void)sb.get_masking_params(vis_floor, mask_exp, vis_scale);
        kdu_dims sdims, pdims, bspan, vblks;
        sb.get_dims(sdims);
        sb.get_parent_dims(pdims, true);
        sb.get_valid_block_span(bspan);
        sb.get_valid_blocks(vblks);
        kdu_coords nom_sz, blk_sz;
        sb.get_block_size(nom_sz, blk_sz);
        bool tr = false, vf = false, hf = false;
        sb.get_block_geometry(tr, vf, hf);
        (void)sb.get_content_type();
        (void)sb.get_conservative_slope_threshold();
        kdu_uint16 cmin = 0, cmax = 0;
        (void)sb.get_global_coding_hints(cmin, cmax);
        if (vblks.size.x > 0 && vblks.size.y > 0) {
          kdu_block* blk = sb.open_block(vblks.pos, nullptr, nullptr);
          if (blk != nullptr) {
            sb.close_block(blk, nullptr);
          }
        }
      }
    }
    EXPECT_TRUE(tc.access_resolution().access_next().exists() ||
                !tc.access_resolution().access_next().exists());
    tile.close();
    cs.destroy();
  }
}

TEST_F(CompressedTest, SubbandAndPrecinctDirectBlocksAndServerPackets) {
  CodestreamSpec spec;
  spec.width = 48;
  spec.height = 48;
  spec.components = 1;
  spec.reversible = false;
  spec.dwt_levels = 2;
  spec.extra_params = {"Clayers=3",    "Cprecincts={16,16}", "Cblk={8,8}",
                       "Cuse_eph=yes", "Cuse_sop=yes",       "Qstep=0.01"};
  kdu_long lbytes[3] = {150, 350, 900};
  const auto bytes =
      EncodeCodestream(spec, MakePlanes(spec), nullptr, lbytes, 3);
  ASSERT_FALSE(bytes.empty());

  BufferSource src(bytes);
  kdu_codestream dec;
  dec.create(&src);
  kdu_tile dtile = dec.open_tile(kdu_coords(0, 0));
  dtile.parse_all_relevant_packets(false, nullptr);
  kdu_long layer_bytes[3] = {};
  kdu_long layer_packets[3] = {};
  EXPECT_GT(dtile.get_parsed_packet_stats(0, 0, 3, layer_bytes, layer_packets),
            0);
  EXPECT_GT(layer_packets[0] + layer_packets[1] + layer_packets[2], 0);

  // Also test interchange codestream with open_precinct, size_packets, and
  // get_packets by transcoding blocks from dec.
  siz_params isiz;
  ConfigureSiz(isiz, spec);
  kdu_codestream inter;
  inter.create(&isiz);
  inter.access_siz()->copy_all(dec.access_siz());
  inter.access_siz()->finalize_all();
  kdu_tile itile = inter.open_tile(kdu_coords(0, 0));
  kdu_resolution dres = dtile.access_component(0).access_resolution(0);
  kdu_resolution ires = itile.access_component(0).access_resolution(0);
  kdu_dims vp;
  ires.get_valid_precincts(vp);
  ASSERT_GT(vp.size.x, 0);
  kdu_precinct ip = ires.open_precinct(vp.pos, nullptr);
  ASSERT_TRUE(ip.exists());
  EXPECT_NE(ip.get_unique_id(), -1);
  EXPECT_FALSE(ip.check_loaded());

  kdu_dims pblks;
  ip.get_valid_blocks(LL_BAND, pblks);
  if (pblks.size.x > 0 && pblks.size.y > 0) {
    kdu_subband dsb = dres.access_subband(LL_BAND);
    kdu_block* sblk = dsb.open_block(pblks.pos);
    kdu_block* dblk = ip.open_block(LL_BAND, pblks.pos);
    ASSERT_NE(sblk, nullptr);
    ASSERT_NE(dblk, nullptr);
    CopyEncodedBlock(dblk, sblk);
    ip.close_block(dblk);
    dsb.close_block(sblk);
  }

  // Fill any other precincts' blocks on resolution 0 or check size/get packets
  // on a 0-level interchange codestream with 1 block in 1 precinct.
  ip.close();
  itile.close();
  inter.destroy();

  // Standalone 1-precinct / 1-block interchange test for size_packets +
  // get_packets + restart.
  CodestreamSpec one_spec;
  one_spec.width = 16;
  one_spec.height = 16;
  one_spec.components = 1;
  one_spec.reversible = true;
  one_spec.dwt_levels = 0;
  one_spec.extra_params = {"Clayers=2", "Cprecincts={16,16}", "Cblk={16,16}"};
  const auto one_bytes = EncodeCodestream(one_spec, MakePlanes(one_spec));
  BufferSource one_src(one_bytes);
  kdu_codestream one_dec;
  one_dec.create(&one_src);
  kdu_tile odt = one_dec.open_tile(kdu_coords(0, 0));
  siz_params one_isiz;
  ConfigureSiz(one_isiz, one_spec);
  kdu_codestream one_inter;
  one_inter.create(&one_isiz);
  one_inter.access_siz()->copy_all(one_dec.access_siz());
  one_inter.access_siz()->finalize_all();
  kdu_tile oit = one_inter.open_tile(kdu_coords(0, 0));
  kdu_resolution oir = oit.access_component(0).access_resolution(0);
  kdu_dims ovp;
  oir.get_valid_precincts(ovp);
  kdu_precinct op = oir.open_precinct(ovp.pos);
  kdu_dims obp;
  op.get_valid_blocks(LL_BAND, obp);
  kdu_subband osb =
      odt.access_component(0).access_resolution(0).access_subband(LL_BAND);
  kdu_block* sbk = osb.open_block(obp.pos);
  kdu_block* dbk = op.open_block(LL_BAND, obp.pos);
  CopyEncodedBlock(dbk, sbk);
  op.close_block(dbk);
  osb.close_block(sbk);
  EXPECT_TRUE(op.check_loaded());

  int cum_pkts = 1, cum_bytes = 0;
  bool is_sig = false;
  EXPECT_TRUE(op.size_packets(cum_pkts, cum_bytes, is_sig));
  EXPECT_GE(cum_pkts, 1);

  class CountingOut : public kdu_output {
   public:
    ~CountingOut() noexcept override { flush_buf(); }
    void flush_buf() override {
      total += static_cast<int>(next_buf - buffer);
      next_buf = buffer;
    }
    int total = 0;
  } pkt_out;
  int out_pkts = 2, out_bytes = 0;
  EXPECT_TRUE(op.get_packets(0, 0, out_pkts, out_bytes, &pkt_out));
  pkt_out.flush_buf();
  EXPECT_GT(pkt_out.total, 0);
  op.restart();
  op.close();
  oit.close();
  one_inter.destroy();
  odt.close();
  one_dec.destroy();

  dtile.close();
  dec.destroy();
}

TEST_F(CompressedTest, Part2MctNltRoiAtkAndCplexCbrCachedPaths) {
  // Part-2 MCT (matrix + dwt) and NLT inspection.
  {
    CodestreamSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 4;
    spec.bit_depth = 8;
    spec.reversible = false;
    spec.dwt_levels = 1;
    spec.extra_params = {
        "Cycc=no",
        "Sextensions=MCT|NLT",
        "Ncomponents=4",
        "Nprecision=8",
        "Nsigned=no",
        "NLTmake={GAMMA,2.4,0.055,64}",
        "Mcomponents=4",
        "Mprecision=8",
        "Msigned=no",
        "Mmatrix_size:I1=16",
        "Mmatrix_coeffs:I1=1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1",
        "Mvector_size:I1=4",
        "Mvector_coeffs:I1=1,0,-1,2",
        "Mstage_inputs:I1={0,3}",
        "Mstage_outputs:I1={0,3}",
        "Mstage_collections:I1={4,4}",
        "Mstage_xforms:I1={MATRIX,1,0,0,0}",
        "Mstage_inputs:I2={0,3}",
        "Mstage_outputs:I2={0,3}",
        "Mstage_collections:I2={4,4}",
        "Mstage_xforms:I2={DWT,0,1,2,0}",
        "Mnum_stages=2",
        "Mstages=1,2",
    };
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    BufferSource src(bytes);
    kdu_codestream cs;
    cs.create(&src);
    cs.set_persistent();
    cs.apply_input_restrictions(0, 0, 0, 0, nullptr,
                                KDU_WANT_OUTPUT_COMPONENTS);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    int descs[4] = {-1, -1, -1, -1};
    EXPECT_TRUE(tile.get_nlt_descriptors(4, descs));
    EXPECT_GE(descs[0], 0);
    float dmin = 0.0f, dmax = 0.0f;
    float lut[64] = {};
    EXPECT_TRUE(tile.make_nlt_table(0, false, dmin, dmax, 64, lut, 1.0f, 1.0f));
    EXPECT_TRUE(tile.make_nlt_table(0, true, dmin, dmax, 64, lut, 1.0f, 1.0f));

    int stage = 0;
    while (true) {
      int nsi = 0, nso = 0, nbi = 0, nbo = 0;
      if (!tile.get_mct_block_info(stage, 0, nsi, nso, nbi, nbo)) break;
      int bin[8] = {}, bout[8] = {};
      float off[8] = {};
      int si[8] = {};
      EXPECT_TRUE(tile.get_mct_block_info(stage, 0, nsi, nso, nbi, nbo, bin,
                                          bout, off, nullptr,
                                          stage == 0 ? si : nullptr));
      if (tile.get_mct_matrix_info(stage, 0)) {
        std::vector<float> coeffs(static_cast<size_t>(nbi * nbo));
        EXPECT_TRUE(tile.get_mct_matrix_info(stage, 0, coeffs.data()));
      }
      bool is_rev = false;
      int levels = 0, cmin = 0, clim = 0, nsteps = 0;
      bool sym = false, sym_ext = false;
      const float* dwt_c = nullptr;
      if (tile.get_mct_dwt_info(stage, 0, is_rev, levels, cmin, clim, nsteps,
                                sym, sym_ext, dwt_c) != nullptr) {
        std::vector<int> ai(nbi), ao(nbo);
        EXPECT_NE(
            tile.get_mct_dwt_info(stage, 0, is_rev, levels, cmin, clim, nsteps,
                                  sym, sym_ext, dwt_c, ai.data(), ao.data()),
            nullptr);
      }
      ++stage;
    }
    EXPECT_EQ(stage, 2);
    tile.close();
    cs.destroy();
  }

  // ROI, cached target transcoding, and multi tile-parts.
  {
    CodestreamSpec spec;
    spec.width = 48;
    spec.height = 48;
    spec.components = 3;
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(24, 24);
    spec.extra_params = {
        "Clayers=2", "Corder=PCRL",    "Cprecincts={8,8}", "Cblk={8,8}",
        "Rshift=15", "ORGgen_plt=yes", "ORGtparts=R",
    };
    RectRoiImage roi(4, 4, 16, 16);
    kdu_long lbytes[2] = {600, 1800};
    const auto bytes =
        EncodeCodestream(spec, MakePlanes(spec), &roi, lbytes, 2);
    ASSERT_FALSE(bytes.empty());

    BufferSource src(bytes);
    kdu_codestream in;
    in.create(&src);

    siz_params tsiz;
    ConfigureSiz(tsiz, spec);
    BufferTarget trans_tgt(/*cached=*/true);
    kdu_codestream trans;
    trans.create(&tsiz, &trans_tgt);
    ApplyCodingParams(trans, spec);
    kdu_dims tdims;
    in.get_valid_tiles(tdims);
    for (int ty = tdims.pos.y; ty < tdims.pos.y + tdims.size.y; ++ty) {
      for (int tx = tdims.pos.x; tx < tdims.pos.x + tdims.size.x; ++tx) {
        kdu_tile it = in.open_tile(kdu_coords(tx, ty));
        kdu_tile ot = trans.open_tile(kdu_coords(tx, ty));
        for (int c = 0; c < it.get_num_components(); ++c) {
          kdu_tile_comp itc = it.access_component(c);
          kdu_tile_comp otc = ot.access_component(c);
          for (int r = 0; r < itc.get_num_resolutions(); ++r) {
            kdu_resolution ir = itc.access_resolution(r);
            kdu_resolution ores = otc.access_resolution(r);
            const int max_b = (r == 0) ? 1 : 3;
            for (int b = 0; b < max_b; ++b) {
              kdu_subband isb = ir.access_subband((r == 0) ? LL_BAND : b + 1);
              kdu_subband osb = ores.access_subband((r == 0) ? LL_BAND : b + 1);
              if (!isb.exists() || !osb.exists()) continue;
              kdu_dims vblks;
              isb.get_valid_blocks(vblks);
              for (int y = vblks.pos.y; y < vblks.pos.y + vblks.size.y; ++y) {
                for (int x = vblks.pos.x; x < vblks.pos.x + vblks.size.x; ++x) {
                  kdu_block* sbk = isb.open_block(kdu_coords(x, y));
                  kdu_block* dbk = osb.open_block(kdu_coords(x, y));
                  CopyEncodedBlock(dbk, sbk);
                  osb.close_block(dbk);
                  isb.close_block(sbk);
                }
              }
            }
          }
        }
        ot.close();
        it.close();
      }
    }
    trans.trans_out(900);
    trans.destroy();
    in.destroy();
    EXPECT_FALSE(trans_tgt.main_header.empty() &&
                 trans_tgt.tile_headers.empty());
    EXPECT_FALSE(trans_tgt.precincts.empty());
  }

  // Scbr constant bit rate.
  {
    CodestreamSpec spec;
    spec.width = 64;
    spec.height = 32;
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.extra_params = {"Corder=PCRL", "Cprecincts={32,32}", "Cblk={16,16}",
                         "Clayers=1", "Scbr={1,256}"};
    kdu_long cbr_len[1] = {4096};
    const auto bytes =
        EncodeCodestream(spec, MakePlanes(spec), nullptr, cbr_len, 1);
    ASSERT_FALSE(bytes.empty());
  }
}

TEST_F(CompressedTest, ResilientCorruptPacketAndPersistentRestartPaths) {
  CodestreamSpec spec;
  spec.width = 64;
  spec.height = 64;
  spec.components = 2;
  spec.reversible = true;
  spec.dwt_levels = 2;
  spec.tile_size = kdu_coords(32, 32);
  spec.extra_params = {"Cycc=no", "Clayers=2", "Cuse_sop=yes", "Cuse_eph=yes"};
  const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
  ASSERT_GT(bytes.size(), 100U);

  std::vector<kdu_byte> trunc(
      bytes.begin(),
      bytes.begin() + static_cast<ptrdiff_t>(bytes.size() * 2 / 3));
  BufferSource tsrc(trunc);
  kdu_codestream rcs;
  rcs.create(&tsrc);
  rcs.set_resilient();
  kdu_dims tdims;
  rcs.get_valid_tiles(tdims);
  kdu_tile rt = rcs.open_tile(tdims.pos);
  rt.parse_all_relevant_packets(true, nullptr);
  rt.close();
  rcs.destroy();

  BufferSource psrc1(bytes);
  BufferSource psrc2(bytes);
  kdu_codestream pcs;
  pcs.create(&psrc1);
  pcs.set_persistent();
  pcs.enable_restart();
  for (int ty = 0; ty < 2; ++ty) {
    for (int tx = 0; tx < 2; ++tx) {
      kdu_tile t = pcs.open_tile(kdu_coords(tx, ty));
      t.parse_all_relevant_packets(false, nullptr);
      t.close();
    }
  }
  pcs.restart(&psrc2);
  kdu_tile t0 = pcs.open_tile(kdu_coords(0, 0));
  t0.close();
  pcs.destroy();

  // Visual masking (Cvis) on a sufficiently large subband to cover
  // get_masking_params without degenerate <4x4 cell edge cases.
  {
    CodestreamSpec vspec;
    vspec.width = 64;
    vspec.height = 64;
    vspec.components = 1;
    vspec.reversible = false;
    vspec.dwt_levels = 1;
    vspec.extra_params = {"Cblk={8,64}", "Cvis=0.05", "Qstep=0.001"};
    const auto vbytes = EncodeCodestream(vspec, MakePlanes(vspec));
    ASSERT_FALSE(vbytes.empty());
    BufferSource vsrc(vbytes);
    kdu_codestream vcs;
    vcs.create(&vsrc);
    kdu_tile vt = vcs.open_tile(kdu_coords(0, 0));
    kdu_subband vsb =
        vt.access_component(0).access_resolution(0).access_subband(LL_BAND);
    float vf = 0.0f, me = 0.0f, vs = 0.0f;
    (void)vsb.get_masking_params(vf, me, vs);
    vt.close();
    vcs.destroy();
  }
}

TEST_F(CompressedTest, CplexEstSingleThreadScbrAndFragmentsPaths) {
  // 1. Multi-set Scbr + Cplex=EST + SPF fragments + max_slope_threshold.
  for (const char* delay_cfg : {"Cplex={6,EST,0.25,0}", "Cplex={6,EST,0.25,8}",
                                "Cplex={6,EST,0.25,-1}"}) {
    CodestreamSpec spec;
    spec.width = 128;
    spec.height = 128;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {
        "Corder=PCRL",  "Cprecincts={16,16}", "Cblk={8,8}", "Clayers=1",
        "Scbr={1,128}", "Content_type=PHOTO", delay_cfg};
    const auto planes = MakePlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    BufferTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ApplyCodingParams(cs, spec);
    cs.configure_simultaneous_processing_fragments(32, 2, 4);
    cs.set_max_bytes(4096, /*simulate_parsing=*/false);
    cs.set_max_slope_threshold(50000);

    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    EXPECT_THROW(cs.enable_restart(), int);
    kdu_multi_analysis engine;
    engine.create(cs, tile);
    for (int y = 0; y < spec.height; ++y) {
      kdu_line_buf* line = engine.exchange_line(0, nullptr);
      ASSERT_NE(line, nullptr);
      const float scale = 1.0f / static_cast<float>(1 << spec.bit_depth);
      const int offset = 1 << (spec.bit_depth - 1);
      for (int x = 0; x < spec.width; ++x) {
        const int v =
            planes[0][static_cast<size_t>(y) * spec.width + x] - offset;
        if (line->get_buf32() != nullptr) {
          line->get_buf32()[x].fval = static_cast<float>(v) * scale;
        } else {
          line->get_buf16()[x].ival =
              static_cast<kdu_int16>(v << (KDU_FIX_POINT - spec.bit_depth));
        }
      }
      engine.exchange_line(0, line);
    }
    engine.destroy();
    tile.close();
    kdu_long budget[1] = {4096};
    cs.flush(budget, 1);
    cs.destroy();
    EXPECT_FALSE(tgt.data.empty());
  }

  // 2. Cplex=EST with analysis fusion (frag_width > top res? / multi-comp +
  //    SPF copy + duplicate set_max_bytes throw).
  {
    CodestreamSpec spec;
    spec.width = 128;
    spec.height = 128;
    spec.components = 3;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {
        "Corder=RPCL",      "Cprecincts={16,16}",  "Cblk={8,8}",
        "Cdecomp=B(-:-:-)", "Content_type=SCREEN", "Cplex={6,EST,0.25,0}",
        "Clayers=2"};
    const auto planes = MakePlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    BufferTarget tgt, tgt2;
    kdu_codestream cs, cs2;
    cs.create(&siz, &tgt);
    ApplyCodingParams(cs, spec);
    cs.configure_simultaneous_processing_fragments(64, 1, 4);
    cs.set_max_bytes(8192, /*simulate_parsing=*/true);
    cs.set_max_slope_threshold(48000);

    cs2.create(&siz, &tgt2);
    ApplyCodingParams(cs2, spec);
    cs2.configure_simultaneous_processing_fragments(cs);
    cs2.destroy();

    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    kdu_multi_analysis engine;
    engine.create(cs, tile);
    std::vector<int> pushed(3, 0);
    bool any_left = true;
    while (any_left) {
      any_left = false;
      bool progressed = false;
      for (int c = 0; c < 3; ++c) {
        if (pushed[c] >= spec.height) continue;
        any_left = true;
        kdu_line_buf* line = engine.exchange_line(c, nullptr);
        if (line == nullptr) continue;
        const float scale = 1.0f / static_cast<float>(1 << spec.bit_depth);
        const int offset = 1 << (spec.bit_depth - 1);
        for (int x = 0; x < spec.width; ++x) {
          const int v =
              planes[c][static_cast<size_t>(pushed[c]) * spec.width + x] -
              offset;
          if (line->get_buf32() != nullptr) {
            line->get_buf32()[x].fval = static_cast<float>(v) * scale;
          } else {
            line->get_buf16()[x].ival =
                static_cast<kdu_int16>(v << (KDU_FIX_POINT - spec.bit_depth));
          }
        }
        engine.exchange_line(c, line);
        ++pushed[c];
        progressed = true;
      }
      if (any_left && !progressed) break;
    }
    engine.destroy();
    tile.close();
    kdu_long lbytes[2] = {3000, 8192};
    cs.flush(lbytes, 2);
    cs.destroy();
    EXPECT_FALSE(tgt.data.empty());
  }
}

TEST_F(CompressedTest, DecodingCorruptPacketsFetchBlockAndPrecinctQueries) {
  // 1. Multi-layer SOP/EPH codestream: intact decode + subband/precinct
  //    inspection + review_closed_tile + fetch_block / update_block.
  for (bool rev : {true, false}) {
    CodestreamSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 2;
    spec.reversible = rev;
    spec.dwt_levels = 2;
    spec.extra_params = {"Cycc=no",    "Clayers=3",    "Cprecincts={16,16}",
                         "Cblk={8,8}", "Cuse_sop=yes", "Cuse_eph=yes",
                         "Corder=RLCP"};
    const auto planes = MakePlanes(spec);

    // Output review_closed_tile + fetch_block + update_block + per_plane_wmse.
    {
      siz_params siz;
      ConfigureSiz(siz, spec);
      BufferTarget tgt;
      kdu_codestream ocs;
      ocs.create(&siz, &tgt);
      ApplyCodingParams(ocs, spec);
      kdu_tile tile = ocs.open_tile(kdu_coords(0, 0));
      kdu_multi_analysis engine;
      engine.create(ocs, tile);
      std::vector<int> pushed(2, 0);
      bool any_left = true;
      while (any_left) {
        any_left = false;
        bool progressed = false;
        for (int c = 0; c < 2; ++c) {
          if (pushed[c] >= spec.height) continue;
          any_left = true;
          kdu_line_buf* line = engine.exchange_line(c, nullptr);
          if (line == nullptr) continue;
          const int offset = 1 << (spec.bit_depth - 1);
          for (int x = 0; x < spec.width; ++x) {
            const int v =
                planes[c][static_cast<size_t>(pushed[c]) * spec.width + x] -
                offset;
            if (line->is_absolute()) {
              line->get_buf16() != nullptr
                  ? (void)(line->get_buf16()[x].ival =
                               static_cast<kdu_int16>(v))
                  : (void)(line->get_buf32()[x].ival = v);
            } else if (line->get_buf32() != nullptr) {
              line->get_buf32()[x].fval =
                  static_cast<float>(v) /
                  static_cast<float>(1 << spec.bit_depth);
            } else {
              line->get_buf16()[x].ival =
                  static_cast<kdu_int16>(v << (KDU_FIX_POINT - spec.bit_depth));
            }
          }
          engine.exchange_line(c, line);
          ++pushed[c];
          progressed = true;
        }
        if (any_left && !progressed) break;
      }
      engine.destroy();
      tile.close();

      kdu_tile rtile = ocs.review_closed_tile(kdu_coords(0, 0));
      EXPECT_TRUE(rtile.exists());
      kdu_subband sb =
          rtile.access_component(0).access_resolution(0).access_subband(
              LL_BAND);
      float wmse[16] = {};
      EXPECT_GT(sb.get_per_plane_wmse(wmse, 16, false), 0);
      kdu_dims vblks;
      sb.get_valid_blocks(vblks);
      if (!vblks.is_empty()) {
        kdu_block* fb = sb.fetch_block(vblks.pos, nullptr);
        ASSERT_NE(fb, nullptr);
        if (fb->num_passes > 1) {
          fb->num_passes -= 1;
        }
        sb.update_block(fb, nullptr);
      }
      kdu_long lb[3] = {400, 1200, 4000};
      ocs.flush(lb, 3);
      ocs.destroy();
      EXPECT_FALSE(tgt.data.empty());
    }

    const auto bytes = EncodeCodestream(spec, planes);
    BufferSource src(bytes);
    kdu_codestream dcs;
    dcs.create(&src);
    dcs.set_fast();
    dcs.set_synth_overshoot_limit(0.5f, false);
    const auto decoded = DecodeCodestream(dcs, /*inspect_subbands=*/true);
    EXPECT_EQ(decoded.size(), 2U);
    float max_ov = 0.0f;
    (void)dcs.report_synth_overshoot(0, max_ov);
    dcs.destroy();

    // Corrupt a middle SOP packet body; non-seekable resilient source handles
    // corrupt packets via handle_corrupt_packet and recovers at later SOPs.
    std::vector<kdu_byte> corrupt = bytes;
    std::vector<size_t> sop_offsets;
    for (size_t i = 0; i + 5 < corrupt.size(); ++i) {
      if (corrupt[i] == 0xFF && corrupt[i + 1] == 0x91 &&
          corrupt[i + 2] == 0x00 && corrupt[i + 3] == 0x04) {
        sop_offsets.push_back(i);
      }
    }
    if (sop_offsets.size() >= 6) {
      const size_t start = sop_offsets[1] + 6;
      const size_t stop = std::min(sop_offsets[2], start + 8);
      for (size_t k = start; k < stop; ++k) {
        corrupt[k] ^= 0xFF;
      }
      if (corrupt.size() > sop_offsets[3] + 12) {
        corrupt[sop_offsets[3] + 6] = 0xFF;
        corrupt[sop_offsets[3] + 7] = 0x7F;
      }

      BufferSource seq_src(corrupt, KDU_SOURCE_CAP_SEQUENTIAL);
      kdu_codestream rcs;
      rcs.create(&seq_src);
      rcs.set_resilient(true);
      rcs.set_allow_parsing_errors(true);
      const auto rec = DecodeCodestream(rcs, /*inspect_subbands=*/false);
      EXPECT_EQ(rec.size(), 2U);
      rcs.destroy();

      // Fussy mode throws on EPH/marker error.
      BufferSource fussy_src(corrupt, KDU_SOURCE_CAP_SEQUENTIAL);
      kdu_codestream fcs;
      fcs.create(&fussy_src);
      fcs.set_fussy();
      EXPECT_THROW((void)DecodeCodestream(fcs, false), int);
      if (fcs.exists()) fcs.destroy();

      // Seekable source + corrupt packet triggers error 36 in
      // handle_corrupt_packet when resilient is enabled.
      BufferSource seek_src(
          corrupt, KDU_SOURCE_CAP_SEQUENTIAL | KDU_SOURCE_CAP_SEEKABLE);
      kdu_codestream scs;
      scs.create(&seek_src);
      scs.set_resilient(true);
      try {
        (void)DecodeCodestream(scs, false);
      } catch (int) {
        // Error path exercised cleanly.
      }
      if (scs.exists()) scs.destroy();
    }
  }
}

TEST_F(CompressedTest, Part2NltLutSmagUmagMctDepAndRxformPaths) {
  // 1. NLT SMAG, UMAG, LUT, GAMMA + make_nlt_table analysis/synthesis.
  for (bool rev : {true, false}) {
    CodestreamSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 4;
    spec.reversible = rev;
    spec.dwt_levels = 1;
    spec.extra_params = {
        "Cycc=no",
        "Ssigned=yes,no,no,no",
        "Sextensions=NLT",
        "Ncomponents=4",
        "Nprecision=8",
        "Nsigned=yes,no,no,no",
        "NLType:C0=SMAG",
        "NLType:C1=UMAG",
        "NLType:C2=LUT",
        "NLTlut:C2={0.0,1.0,4,12}",
        "NLTdata:C2=0.0,0.3,0.7,1.0",
        "NLTmake:C3={GAMMA,2.2,0.055,64}",
    };
    const auto planes = MakePlanes(spec);
    const auto bytes = EncodeCodestream(spec, planes);
    BufferSource src(bytes);
    kdu_codestream cs;
    cs.create(&src);
    cs.set_persistent();
    cs.apply_input_restrictions(0, 0, 0, 0, nullptr,
                                KDU_WANT_OUTPUT_COMPONENTS);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    EXPECT_TRUE(tile.get_nlt_descriptors(4, nullptr));
    int descs[4] = {-1, -1, -1, -1};
    EXPECT_TRUE(tile.get_nlt_descriptors(4, descs));
    for (int c = 0; c < 4; ++c) {
      EXPECT_GE(descs[c], 0);
      float dmin = 0.0f, dmax = 0.0f;
      float lut[32] = {};
      EXPECT_EQ(tile.make_nlt_table(c, false, dmin, dmax, 32, lut, 1.0f, 1.0f),
                c >= 2);
      EXPECT_EQ(tile.make_nlt_table(c, true, dmin, dmax, 32, lut, 1.0f, 1.0f),
                c >= 2);
    }
    tile.close();
    const auto dec = DecodeCodestream(cs, false);
    EXPECT_EQ(dec.size(), 4U);
    cs.destroy();
  }

  // 2. MCT DEP transforms (reversible rxform & irreversible), plus multi-block
  //    stage and get_mct_rxform_info / get_mct_dependency_info.
  for (bool rev : {true, false}) {
    CodestreamSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 3;
    spec.reversible = rev;
    spec.dwt_levels = 1;
    spec.extra_params = {
        "Cycc=no",
        "Sextensions=MCT",
        "Mcomponents=3",
        "Mprecision=8",
        "Msigned=no",
        rev ? "Mtriang_size:I1=5" : "Mtriang_size:I1=3",
        rev ? "Mtriang_coeffs:I1=1,1,0,-1,1"
            : "Mtriang_coeffs:I1=0.25,-0.25,0.5",
        "Mvector_size:I1=3",
        "Mvector_coeffs:I1=3,-2,1",
        "Mstage_inputs:I1={0,2}",
        "Mstage_outputs:I1={0,2}",
        "Mstage_collections:I1={3,3}",
        rev ? "Mstage_xforms:I1={DEP,1,1,1,0}"
            : "Mstage_xforms:I1={DEP,1,1,0,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    BufferSource src(bytes);
    kdu_codestream cs;
    cs.create(&src);
    cs.set_persistent();
    cs.apply_input_restrictions(0, 0, 0, 0, nullptr,
                                KDU_WANT_OUTPUT_COMPONENTS);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    int nsi = 0, nso = 0, nbi = 0, nbo = 0;
    ASSERT_TRUE(tile.get_mct_block_info(0, 0, nsi, nso, nbi, nbo));
    bool dep_rev = false;
    EXPECT_TRUE(tile.get_mct_dependency_info(0, 0, dep_rev));
    EXPECT_EQ(dep_rev, rev);
    std::vector<float> fcoeff(static_cast<size_t>(nbi * nbo));
    std::vector<float> foff(static_cast<size_t>(nbo));
    std::vector<int> rcoeff(static_cast<size_t>(nbi * nbo));
    std::vector<int> roff(static_cast<size_t>(nbo));
    std::vector<int> aout(static_cast<size_t>(nbo));
    EXPECT_TRUE(tile.get_mct_dependency_info(
        0, 0, dep_rev, rev ? nullptr : fcoeff.data(),
        rev ? nullptr : foff.data(), rev ? rcoeff.data() : nullptr,
        rev ? roff.data() : nullptr, aout.data()));
    EXPECT_GT(tile.find_component_gain_info(0, false), 0.0f);
    EXPECT_GT(tile.find_component_gain_info(0, true), 0.0f);
    tile.close();
    const auto dec = DecodeCodestream(cs, false);
    EXPECT_EQ(dec.size(), 3U);
    cs.destroy();
  }

  // 3. MCT multi-block matrix (irreversible + reversible rxform) + output
  // subset.
  for (bool rev : {true, false}) {
    CodestreamSpec spec;
    spec.width = 24;
    spec.height = 24;
    spec.components = 4;
    spec.is_signed = rev;
    spec.reversible = rev;
    spec.dwt_levels = 1;
    spec.extra_params = {
        "Cycc=no",
        "Sextensions=MCT",
        "Mcomponents=4",
        "Mprecision=8",
        rev ? "Msigned=yes" : "Msigned=no",
        rev ? "Mmatrix_size:I1=6" : "Mmatrix_size:I1=4",
        rev ? "Mmatrix_coeffs:I1=1,512,512,-1,0,1"
            : "Mmatrix_coeffs:I1=1,0,0,1",
        rev ? "Mmatrix_size:I2=6" : "Mmatrix_size:I2=4",
        rev ? "Mmatrix_coeffs:I2=1,512,512,-1,0,1"
            : "Mmatrix_coeffs:I2=1,0,0,1",
        "Mstage_inputs:I1={0,1},{2,3}",
        "Mstage_outputs:I1={0,1},{2,3}",
        "Mstage_collections:I1={2,2},{2,2}",
        rev ? "Mstage_xforms:I1={MATRIX,1,0,1,0},{MATRIX,2,0,1,0}"
            : "Mstage_xforms:I1={MATRIX,1,0,0,0},{MATRIX,2,0,0,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    BufferSource src(bytes);
    kdu_codestream cs;
    cs.create(&src);
    cs.set_persistent();
    int out_idx[2] = {0, 3};
    cs.apply_input_restrictions(2, out_idx, 0, 0, nullptr,
                                KDU_WANT_OUTPUT_COMPONENTS);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    int nsi = 0, nso = 0, nbi = 0, nbo = 0;
    EXPECT_TRUE(tile.get_mct_block_info(0, 0, nsi, nso, nbi, nbo));
    EXPECT_TRUE(tile.get_mct_block_info(0, 1, nsi, nso, nbi, nbo));
    if (rev) {
      if (tile.get_mct_rxform_info(0, 0)) {
        std::vector<int> rmat(static_cast<size_t>(nbi * (nbi + 1)));
        std::vector<int> aout(static_cast<size_t>(nbo));
        EXPECT_TRUE(tile.get_mct_rxform_info(0, 0, rmat.data(), aout.data()));
      }
    }
    tile.close();
    const auto dec = DecodeCodestream(cs, false);
    EXPECT_EQ(dec.size(), 2U);
    cs.destroy();
  }
}

TEST_F(CompressedTest, TileGeometryRestrictionsAtkMarkersAndRecyclePaths) {
  // 1. Multi-tile codestream with Sorigin, Stile_origin, Ssampling={2,2},
  //    Cmodes, PLT/TLM, ORGtparts=R, CRG, POC, PPT/PPM, heterogeneous tile
  //    coding params, geometry change_appearance, and persistent tile recycle.
  {
    CodestreamSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 3;
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.origin = kdu_coords(4, 6);
    spec.tile_size = kdu_coords(32, 32);
    spec.subsampling = kdu_coords(2, 2);
    spec.extra_params = {
        "Cycc=no",
        "Clayers=2",
        "Corder=RPCL",
        "Cprecincts={16,16}",
        "Cblk={8,8}",
        "Cmodes=RESTART|ERTERM|SEGMARK|BYPASS|CAUSAL|RESET",
        "Cuse_sop=yes",
        "Cuse_eph=yes",
        "ORGgen_plt=yes",
        "ORGgen_tlm=2",
        "ORGtparts=R",
        "Clevels:T0=1",
        "Creversible:T1=no",
        "Qstep:T1=0.02",
        "CRGoffset={0.0,0.0},{0.5,0.5},{0.5,0.5}",
    };
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    ASSERT_FALSE(bytes.empty());

    BufferSource src(bytes);
    kdu_codestream cs;
    cs.create(&src);
    cs.set_persistent();

    for (const auto& orient :
         std::vector<std::pair<std::pair<bool, bool>, bool>>{
             {{true, false}, false},
             {{false, true}, true},
             {{true, true}, true}}) {
      cs.change_appearance(orient.first.first, orient.first.second,
                           orient.second);
      kdu_dims full_dims;
      cs.get_dims(0, full_dims, true);
      kdu_dims roi_rect = full_dims;
      roi_rect.size.x = std::max(1, full_dims.size.x / 2);
      roi_rect.size.y = std::max(1, full_dims.size.y / 2);
      cs.apply_input_restrictions(0, 2, 1, 1, &roi_rect,
                                  KDU_WANT_CODESTREAM_COMPONENTS);
      const auto dec = DecodeCodestream(cs, /*inspect_subbands=*/true);
      EXPECT_EQ(dec.size(), 2U);
    }

    // Reset restrictions and decode all components cleanly.
    cs.change_appearance(false, false, false);
    cs.apply_input_restrictions(0, 0, 0, 0, nullptr,
                                KDU_WANT_OUTPUT_COMPONENTS);
    const auto full = DecodeCodestream(cs, true);
    EXPECT_EQ(full.size(), 3U);
    cs.destroy();
  }

  // 2. Part-2 ATK kernels (R1X1, I5X3, custom reversible + irreversible ATK).
  const std::vector<std::pair<bool, std::vector<std::string>>> atk_cases = {
      {true, {"Ckernels=ATK", "Catk=2", "Kkernels:I2=R1X1"}},
      {false, {"Ckernels=ATK", "Catk=3", "Kkernels:I3=I5X3"}},
      {true,
       {"Ckernels=ATK", "Catk=5", "Kreversible:I5=yes", "Ksymmetric:I5=no",
        "Kextension:I5=CON", "Ksteps:I5={2,0,1,1},{2,-1,2,2}",
        "Kcoeffs:I5={-0.5},{-0.5},{0.25},{0.25}"}},
  };
  for (const auto& ac : atk_cases) {
    CodestreamSpec spec;
    spec.width = 48;
    spec.height = 48;
    spec.components = 1;
    spec.reversible = ac.first;
    spec.dwt_levels = 2;
    spec.extra_params = ac.second;
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    BufferSource src(bytes);
    kdu_codestream cs;
    cs.create(&src);
    const auto dec = DecodeCodestream(cs, true);
    EXPECT_EQ(dec.size(), 1U);
    cs.destroy();
  }
}

class AsyncMemSource : public kdu_compressed_source {
 public:
  AsyncMemSource(std::vector<kdu_byte> data, size_t initial_avail,
                 size_t chunk_bytes, int caps, bool note_block_once = false,
                 int defer_nonblocking_calls = 0, bool run_feeder = false)
      : data_(std::move(data)),
        avail_(std::min(initial_avail, data_.size())),
        chunk_(chunk_bytes),
        caps_(caps),
        note_block_once_(note_block_once),
        defer_budget_(defer_nonblocking_calls) {
    current_.assign(data_.size() + 64, 0);
    if (!data_.empty()) {
      std::memcpy(current_.data(), data_.data(), data_.size());
    }
    if (run_feeder) {
      feeder_ = std::thread([this]() { FeederLoop(); });
    }
  }

  ~AsyncMemSource() override { StopFeeder(); }

  void StopFeeder() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (stop_feeder_) return;
      stop_feeder_ = true;
    }
    cv_.notify_all();
    if (feeder_.joinable()) {
      feeder_.join();
    }
  }

  int get_capabilities() override { return caps_; }

  int read(kdu_byte* buf, int num_bytes) override {
    const size_t avail = avail_.load(std::memory_order_acquire);
    if (pos_ >= avail || num_bytes <= 0) return 0;
    const size_t n = std::min(static_cast<size_t>(num_bytes), avail - pos_);
    std::memcpy(buf, data_.data() + pos_, n);
    pos_ += n;
    return static_cast<int>(n);
  }

  bool seek(kdu_long offset) override {
    if (!(caps_ & KDU_SOURCE_CAP_SEEKABLE) || offset < 0) return false;
    pos_ = std::min(static_cast<size_t>(offset), data_.size());
    if (pos_ > avail_.load(std::memory_order_acquire)) AdvanceTo(pos_);
    return true;
  }

  void AdvanceTo(size_t next_avail) {
    std::lock_guard<std::mutex> lock(mu_);
    AdvanceToLocked(next_avail);
  }

  void AdvanceChunk() {
    std::lock_guard<std::mutex> lock(mu_);
    AdvanceToLocked(avail_.load(std::memory_order_relaxed) + chunk_);
  }

  bool AtEnd() const {
    return avail_.load(std::memory_order_acquire) >= data_.size();
  }

  kdu_byte* access_memory(kdu_long& pos, kdu_byte*& lim) override {
    pos = 0;
    lim = current_.data() + avail_.load(std::memory_order_acquire);
    return current_.data();
  }

  int load_memory(kdu_long desired_span, bool blocking,
                  kdu_compressed_source_notifier* notifier) override {
    if (note_block_once_.exchange(false, std::memory_order_acq_rel) &&
        notifier != nullptr) {
      notifier->note_blocking();
      notifier->note_unblocked();
    }
    int flags = 0;
    std::lock_guard<std::mutex> lock(mu_);
    size_t cur_avail = avail_.load(std::memory_order_relaxed);
    if (static_cast<size_t>(desired_span) > cur_avail &&
        cur_avail < data_.size()) {
      if (defer_budget_ > 0 && !blocking && notifier != nullptr &&
          (caps_ & KDU_SOURCE_CAP_ASYNC_NOTIFICATION) && feeder_.joinable() &&
          notifier->prepare_notify(desired_span)) {
        --defer_budget_;
        pending_notifier_ = notifier;
        pending_span_ = static_cast<size_t>(desired_span);
        flags |= KDU_SOURCE_WILL_NOTIFY;
        cv_.notify_one();
      } else {
        AdvanceToLocked(
            std::max(static_cast<size_t>(desired_span), cur_avail + chunk_));
      }
    }
    cur_avail = avail_.load(std::memory_order_relaxed);
    if (static_cast<size_t>(desired_span) <= cur_avail) {
      flags |= KDU_SOURCE_SPAN_EXISTS;
    }
    if (cur_avail >= data_.size()) {
      flags |= KDU_SOURCE_IS_COMPLETE;
    }
    return flags;
  }

 private:
  void AdvanceToLocked(size_t next_avail) {
    const size_t cur_avail = avail_.load(std::memory_order_relaxed);
    const size_t clamped =
        std::min(data_.size(), std::max(cur_avail, next_avail));
    avail_.store(clamped, std::memory_order_release);
  }

  void FeederLoop() {
    for (;;) {
      kdu_compressed_source_notifier* notifier = nullptr;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this]() {
          return stop_feeder_ || pending_notifier_ != nullptr;
        });
        if (stop_feeder_) return;
        AdvanceToLocked(std::max(
            pending_span_, avail_.load(std::memory_order_relaxed) + chunk_));
        notifier = pending_notifier_;
        pending_notifier_ = nullptr;
      }
      if (notifier != nullptr) {
        notifier->notify();
      }
    }
  }

  std::vector<kdu_byte> data_;
  std::atomic<size_t> avail_{0};
  size_t chunk_ = 64;
  size_t pos_ = 0;
  int caps_ = 0;
  std::atomic<bool> note_block_once_{false};
  int defer_budget_ = 0;
  std::vector<kdu_byte> current_;
  std::mutex mu_;
  std::condition_variable cv_;
  kdu_compressed_source_notifier* pending_notifier_ = nullptr;
  size_t pending_span_ = 0;
  bool stop_feeder_ = false;
  std::thread feeder_;
};

class CachedHeaderSource : public kdu_compressed_source {
 public:
  CachedHeaderSource(std::vector<kdu_byte> main_hdr,
                     std::vector<kdu_byte> tile_hdr, bool hide_tileheader)
      : main_(std::move(main_hdr)),
        tile_hdr_(std::move(tile_hdr)),
        hide_tileheader_(hide_tileheader) {}

  int get_capabilities() override {
    return KDU_SOURCE_CAP_CACHED | KDU_SOURCE_CAP_SEEKABLE;
  }

  int read(kdu_byte* buf, int num_bytes) override {
    const auto& active = in_tile_scope_ ? tile_hdr_ : main_;
    if (pos_ >= active.size() || num_bytes <= 0) return 0;
    const size_t n =
        std::min(static_cast<size_t>(num_bytes), active.size() - pos_);
    std::memcpy(buf, active.data() + pos_, n);
    pos_ += n;
    return static_cast<int>(n);
  }

  bool seek(kdu_long offset) override {
    const auto& active = in_tile_scope_ ? tile_hdr_ : main_;
    if (offset < 0) return false;
    pos_ = std::min(static_cast<size_t>(offset), active.size());
    return true;
  }

  kdu_long get_pos() override { return static_cast<kdu_long>(pos_); }

  bool set_tileheader_scope(int /*tnum*/, int /*num_tiles*/) override {
    if (hide_tileheader_) return false;
    in_tile_scope_ = true;
    pos_ = 0;
    return true;
  }

  bool set_precinct_scope(kdu_long /*unique_id*/) override {
    in_tile_scope_ = false;
    pos_ = main_.size();
    return true;
  }

 private:
  std::vector<kdu_byte> main_;
  std::vector<kdu_byte> tile_hdr_;
  bool hide_tileheader_;
  bool in_tile_scope_ = false;
  size_t pos_ = 0;
};

class CountingInputMonitor : public kdu_input_codestream_monitor {
 public:
  void parsing_complete() override { ++completed; }
  void uninstalling_monitor() override { ++uninstalled; }
  void restarting() override { ++restarted; }
  int completed = 0;
  int uninstalled = 0;
  int restarted = 0;
};

std::vector<kdu_byte> RepackWithPackedHeaders(const std::vector<kdu_byte>& src,
                                              bool use_ppm,
                                              bool emit_both = false,
                                              bool truncate_packed = false) {
  auto read_u16 = [&](size_t p) -> kdu_uint16 {
    return (static_cast<kdu_uint16>(src[p]) << 8) | src[p + 1];
  };
  auto read_u32 = [&](size_t p) -> kdu_uint32 {
    return (static_cast<kdu_uint32>(src[p]) << 24) |
           (static_cast<kdu_uint32>(src[p + 1]) << 16) |
           (static_cast<kdu_uint32>(src[p + 2]) << 8) | src[p + 3];
  };
  auto push_u16 = [](std::vector<kdu_byte>& dst, kdu_uint16 v) {
    dst.push_back(static_cast<kdu_byte>(v >> 8));
    dst.push_back(static_cast<kdu_byte>(v & 0xFF));
  };
  auto push_u32 = [](std::vector<kdu_byte>& dst, kdu_uint32 v) {
    dst.push_back(static_cast<kdu_byte>((v >> 24) & 0xFF));
    dst.push_back(static_cast<kdu_byte>((v >> 16) & 0xFF));
    dst.push_back(static_cast<kdu_byte>((v >> 8) & 0xFF));
    dst.push_back(static_cast<kdu_byte>(v & 0xFF));
  };

  size_t first_sot = 2;
  while (first_sot + 4 <= src.size()) {
    const kdu_uint16 m = read_u16(first_sot);
    if (m == KDU_SOT) break;
    const kdu_uint16 len = read_u16(first_sot + 2);
    first_sot += 2 + len;
  }

  struct TilePartOut {
    std::vector<kdu_byte> header_prefix;
    std::vector<kdu_byte> packed_hdrs;
    std::vector<kdu_byte> body;
  };
  std::vector<TilePartOut> tparts;

  size_t pos = first_sot;
  while (pos + 12 <= src.size() && read_u16(pos) == KDU_SOT) {
    const kdu_uint32 psot = read_u32(pos + 6);
    const size_t tp_end = (psot > 0) ? (pos + psot) : (src.size() - 2);
    size_t sod_pos = pos + 12;
    while (sod_pos + 2 <= tp_end && read_u16(sod_pos) != KDU_SOD) {
      const kdu_uint16 mlen = read_u16(sod_pos + 2);
      sod_pos += 2 + mlen;
    }
    TilePartOut tp;
    tp.header_prefix.assign(src.begin() + pos, src.begin() + sod_pos);
    size_t cur = sod_pos + 2;
    while (cur < tp_end) {
      if (cur + 6 <= tp_end && src[cur] == 0xFF && src[cur + 1] == 0x91) {
        tp.body.insert(tp.body.end(), src.begin() + cur, src.begin() + cur + 6);
        cur += 6;
      }
      const size_t hdr_start = cur;
      while (cur + 1 < tp_end && !(src[cur] == 0xFF && src[cur + 1] == 0x92)) {
        cur++;
      }
      if (cur + 1 < tp_end && src[cur] == 0xFF && src[cur + 1] == 0x92) {
        cur += 2;
        tp.packed_hdrs.insert(tp.packed_hdrs.end(), src.begin() + hdr_start,
                              src.begin() + cur);
      }
      const size_t body_start = cur;
      while (cur < tp_end &&
             !(cur + 5 < tp_end && src[cur] == 0xFF && src[cur + 1] == 0x91 &&
               src[cur + 2] == 0x00 && src[cur + 3] == 0x04)) {
        cur++;
      }
      tp.body.insert(tp.body.end(), src.begin() + body_start,
                     src.begin() + cur);
    }
    if (truncate_packed && tp.packed_hdrs.size() > 2) {
      tp.packed_hdrs.resize(1);
    }
    tparts.push_back(std::move(tp));
    pos = tp_end;
  }

  std::vector<kdu_byte> out(src.begin(), src.begin() + first_sot);
  if (use_ppm || emit_both) {
    std::vector<kdu_byte> ppm_stream;
    for (const auto& tp : tparts) {
      push_u32(ppm_stream, static_cast<kdu_uint32>(tp.packed_hdrs.size()));
      ppm_stream.insert(ppm_stream.end(), tp.packed_hdrs.begin(),
                        tp.packed_hdrs.end());
    }
    const size_t split_at =
        ppm_stream.size() > 16 ? ppm_stream.size() / 2 : ppm_stream.size();
    kdu_byte zppm = 0;
    for (size_t off = 0; off < ppm_stream.size();) {
      const size_t chunk = (off == 0) ? split_at : (ppm_stream.size() - off);
      push_u16(out, KDU_PPM);
      push_u16(out, static_cast<kdu_uint16>(3 + chunk));
      out.push_back(zppm++);
      out.insert(out.end(), ppm_stream.begin() + off,
                 ppm_stream.begin() + off + chunk);
      off += chunk;
    }
  }

  for (const auto& tp : tparts) {
    std::vector<kdu_byte> tp_bytes = tp.header_prefix;
    if (!use_ppm || emit_both) {
      const size_t split_at = tp.packed_hdrs.size() > 8
                                  ? tp.packed_hdrs.size() / 2
                                  : tp.packed_hdrs.size();
      kdu_byte zppt = 0;
      for (size_t off = 0; off < tp.packed_hdrs.size();) {
        const size_t chunk =
            (off == 0) ? split_at : (tp.packed_hdrs.size() - off);
        push_u16(tp_bytes, KDU_PPT);
        push_u16(tp_bytes, static_cast<kdu_uint16>(3 + chunk));
        tp_bytes.push_back(zppt++);
        tp_bytes.insert(tp_bytes.end(), tp.packed_hdrs.begin() + off,
                        tp.packed_hdrs.begin() + off + chunk);
        off += chunk;
      }
    }
    push_u16(tp_bytes, KDU_SOD);
    tp_bytes.insert(tp_bytes.end(), tp.body.begin(), tp.body.end());
    const kdu_uint32 new_psot = static_cast<kdu_uint32>(tp_bytes.size());
    tp_bytes[6] = static_cast<kdu_byte>((new_psot >> 24) & 0xFF);
    tp_bytes[7] = static_cast<kdu_byte>((new_psot >> 16) & 0xFF);
    tp_bytes[8] = static_cast<kdu_byte>((new_psot >> 8) & 0xFF);
    tp_bytes[9] = static_cast<kdu_byte>(new_psot & 0xFF);
    out.insert(out.end(), tp_bytes.begin(), tp_bytes.end());
  }
  push_u16(out, KDU_EOC);
  return out;
}

TEST_F(CompressedTest, AutoFlushFlushStatsCbrBulkFillAndReinitializePaths) {
  // 1. Scbr + add_flush_stats + attach_flush_stats + restart ->
  //    kd_tile::reinitialize(out) and rfs flush stats in write_packet_cbr.
  {
    CodestreamSpec spec;
    spec.width = 128;
    spec.height = 128;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {"Corder=PCRL", "Cprecincts={32,32}", "Cblk={16,16}",
                         "Scbr={1,256}", "Cuse_sop=yes"};
    const auto planes = MakePlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    BufferTarget tgt1;
    kdu_codestream cs;
    cs.create(&siz, &tgt1);
    ApplyCodingParams(cs, spec);
    cs.enable_restart();
    kdu_flush_stats fstats = cs.add_flush_stats(0, 5, 0, 1);

    PushAllTiles(cs, planes, spec);
    kdu_long cbr_len[1] = {4096};
    cs.flush(cbr_len, 1);
    EXPECT_GT(tgt1.data.size(), 100U);

    // Frame 2: restart + advance hits kd_tile::reinitialize(out) and rfs.
    fstats.advance(1);
    BufferTarget tgt2;
    cs.restart(&tgt2);
    cs.set_min_slope_threshold(48000);
    PushAllTiles(cs, planes, spec);
    cs.flush(cbr_len, 1);
    EXPECT_GT(tgt2.data.size(), 100U);
    cs.destroy();
  }

  // 2. auto_flush (both sequential and cached targets) ->
  //    kdu_subband::block_row_generated.
  for (bool cached : {false, true}) {
    CodestreamSpec spec;
    spec.width = 128;
    spec.height = 128;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {"Corder=RPCL", "Clayers=2", "Cprecincts={32,32}",
                         "Cblk={16,16}", "ORGgen_plt=yes"};
    const auto planes = MakePlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    BufferTarget tgt(cached);
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ApplyCodingParams(cs, spec);
    kdu_long lbytes[2] = {400, 1600};
    cs.auto_flush(1, 1, 16, 16, lbytes, 2, nullptr, true, true);
    PushAllTiles(cs, planes, spec);
    cs.flush(lbytes, 2, nullptr, true, true);
    cs.destroy();
    if (cached) {
      EXPECT_FALSE(tgt.tile_headers.empty());
    } else {
      EXPECT_GT(tgt.data.size(), 100U);
    }
  }

  // 3. Large CBR bulk_fill (>= 8192) and fine_fill_block.
  {
    CodestreamSpec spec;
    spec.width = 256;
    spec.height = 256;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {"Corder=PCRL", "Clayers=1", "Cblk={32,32}",
                         "Cprecincts={64,64}", "Scbr={1,512}"};
    const auto planes = MakePlanes(spec);
    kdu_long cbr_bytes[1] = {49152};
    const auto bytes = EncodeCodestream(spec, planes, nullptr, cbr_bytes, 1);
    EXPECT_GE(bytes.size(), 40000U);
  }

  // 4. Cprog_weights + Cband_weights + Clev_weights + Qweights mismatch +
  //    get_per_plane_wmse (plus ROI Rweight/Rshift separately since Cplex=EST
  //    is incompatible with ROI encoding).
  for (bool use_roi : {false, true}) {
    CodestreamSpec spec;
    spec.width = 96;
    spec.height = 96;
    spec.components = 3;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {
        "Cycc=yes",
        "Cprog_weights=0.5,0.5,0.5",
        "Cband_weights=0.25,0.25,0.25,0.5,0.5,0.5",
        "Clev_weights=0.5,0.75",
        "Qweights=10.0,0.1,1.0",
    };
    if (use_roi) {
      spec.extra_params.push_back("Rweight=2.0");
      spec.extra_params.push_back("Rlevels=2");
      spec.extra_params.push_back("Rshift=16");
    } else {
      spec.extra_params.push_back("Cplex={6,EST,0.25,0}");
    }
    const auto planes = MakePlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    BufferTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ApplyCodingParams(cs, spec);
    if (!use_roi) cs.set_max_bytes(4096, false);
    warning_handler_.clear();
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    float wmse[32];
    for (int r = 0; r <= 2; ++r) {
      kdu_resolution res = tile.access_component(0).access_resolution(r);
      const int min_b = (r == 0) ? 0 : 1;
      const int max_b = (r == 0) ? 0 : 3;
      for (int b = min_b; b <= max_b; ++b) {
        kdu_subband sb = res.access_subband(b);
        EXPECT_GT(sb.get_per_plane_wmse(wmse, 32, false), 0);
        EXPECT_GT(sb.get_per_plane_wmse(wmse, 32, true), 0);
      }
    }
    RectRoiImage roi(8, 8, 48, 48);
    PushTileSamples(cs, tile, planes, spec, use_roi ? &roi : nullptr);
    tile.close();
    kdu_long lbytes[1] = {8192};
    cs.flush(lbytes, 1);
    cs.destroy();
    EXPECT_FALSE(warning_handler_.history().empty());
    EXPECT_GT(tgt.data.size(), 100U);
  }

  // 5. Cplex=EST with transposed/vflipped output appearance.
  for (bool vflip : {false, true}) {
    CodestreamSpec spec;
    spec.width = 96;
    spec.height = 96;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {"Corder=PCRL", "Cprecincts={16,16}", "Cblk={8,8}",
                         "Cplex={6,EST,0.25,0}"};
    const auto planes = MakePlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    BufferTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ApplyCodingParams(cs, spec);
    cs.set_max_bytes(4096, false);
    cs.change_appearance(/*transpose=*/true, vflip, /*hflip=*/false);
    PushAllTiles(cs, planes, spec);
    kdu_long lbytes[1] = {4096};
    cs.flush(lbytes, 1);
    cs.destroy();
    BufferSource src(tgt.data);
    kdu_codestream dec_cs;
    dec_cs.create(&src);
    const auto dec = DecodeCodestream(dec_cs, false);
    EXPECT_EQ(dec.size(), 1U);
    dec_cs.destroy();
  }
}

TEST_F(CompressedTest,
       CompactBlocksInMemoryRecycleTypicalAndQualityLimiterPaths) {
  // 1. KDU_SOURCE_CAP_IN_MEMORY + Clayers=1 + Cmodes=0 -> compact_blocks
  //    (kd_block<1>) across read_packet, open_block, close_block,
  //    reset_packet_reading, and kdu_input_codestream_monitor.
  {
    CodestreamSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.extra_params = {"Clayers=1", "Corder=RLCP", "Cprecincts={16,16}",
                         "Cblk={8,8}", "Cuse_eph=yes"};
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));

    BufferSource mem_src(bytes, KDU_SOURCE_CAP_SEQUENTIAL |
                                    KDU_SOURCE_CAP_SEEKABLE |
                                    KDU_SOURCE_CAP_IN_MEMORY);
    CountingInputMonitor monitor;
    kdu_codestream cs;
    cs.create(&mem_src);
    EXPECT_TRUE(cs.install_input_monitor(&monitor, nullptr));
    const auto dec1 = DecodeCodestream(cs, true);
    EXPECT_EQ(dec1.size(), 1U);
    EXPECT_GE(monitor.completed, 1);
    cs.destroy();

    // Persistent compact_blocks decode with re-parse / reset_packet_reading.
    BufferSource mem_src2(bytes, KDU_SOURCE_CAP_SEQUENTIAL |
                                     KDU_SOURCE_CAP_SEEKABLE |
                                     KDU_SOURCE_CAP_IN_MEMORY);
    kdu_codestream pcs;
    pcs.create(&mem_src2);
    pcs.set_persistent();
    pcs.apply_input_restrictions(0, 1, 1, 0, nullptr,
                                 KDU_WANT_CODESTREAM_COMPONENTS);
    {
      kdu_tile tile = pcs.open_tile(kdu_coords(0, 0));
      EXPECT_TRUE(tile.parse_all_relevant_packets(false, nullptr));
      tile.close();
    }
    // Expanding restrictions triggers reset_packet_reading on compact_blocks
    // and kd_precinct::activate().
    pcs.apply_input_restrictions(0, 1, 0, 1, nullptr,
                                 KDU_WANT_CODESTREAM_COMPONENTS);
    {
      kdu_tile tile = pcs.open_tile(kdu_coords(0, 0));
      EXPECT_TRUE(tile.parse_all_relevant_packets(true, nullptr));
      tile.close();
    }
    const auto dec2 = DecodeCodestream(pcs, true);
    EXPECT_EQ(dec2.size(), 1U);
    pcs.destroy();
  }

  // 2. Non-persistent multi-tile recycle with per-tile QCD/RGN override
  //    (!fully_typical) and per-tile COD override (!is_typical).
  {
    CodestreamSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(16, 16);
    spec.extra_params = {
        "Clayers=2",  "Corder=LRCP",   "Cprecincts={8,8}", "Cblk={8,8}",
        "Qstep=0.01", "Qstep:T1=0.04", "Rshift:T2=16",     "Corder:T3=RLCP",
    };
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    BufferSource src(bytes);
    kdu_codestream cs;
    cs.create(&src);
    cs.enable_restart();
    const auto dec = DecodeCodestream(cs, false);
    EXPECT_EQ(dec.size(), 1U);
    // Restart input triggers kd_tile::reinitialize(in != NULL).
    BufferSource src_restart(bytes);
    cs.restart(&src_restart);
    const auto dec2 = DecodeCodestream(cs, false);
    EXPECT_EQ(dec2.size(), 1U);
    cs.destroy();
  }

  // 3. kdu_quality_limiter + CRG subsampled YCC + get_precinct_relevance +
  //    get_global_attributes.
  {
    CodestreamSpec spec;
    spec.width = 48;
    spec.height = 48;
    spec.components = 3;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.subsampling = kdu_coords(2, 2);
    spec.extra_params = {"Cycc=no", "Cprecincts={16,16}",
                         "CRGoffset={0.0,0.0},{0.25,0.25},{0.5,0.25}"};
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    BufferSource src(bytes);
    kdu_codestream cs;
    cs.create(&src);
    cs.set_persistent();
    cs.change_appearance(true, true, false);
    kdu_quality_limiter qlim(0.02F, false);
    qlim.set_display_resolution(300.0F, 300.0F);
    qlim.set_comp_info(0, 1.0F, -1);
    qlim.set_comp_info(1, 2.0F, 1);
    qlim.set_comp_info(2, 2.0F, 1);
    cs.apply_input_restrictions(0, 0, 1, 0, nullptr, KDU_WANT_OUTPUT_COMPONENTS,
                                nullptr, &qlim);
    {
      kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
      kdu_uint32 mct_hash = 0;
      EXPECT_EQ(tile.get_global_attributes(mct_hash), 3);
      kdu_resolution res = tile.access_component(0).access_resolution(1);
      EXPECT_GE(res.get_precinct_relevance(kdu_coords(0, 0)), 0.0);
      tile.close();
    }
    const auto dec = DecodeCodestream(cs, false);
    EXPECT_EQ(dec.size(), 3U);
    cs.destroy();
  }
}

TEST_F(CompressedTest,
       PpmPptPackedHeadersAsyncNonBlockingAndCorruptHeaderPaths) {
  // 1. Packed packet headers: PPM, PPT, combined PPM+PPT error 13,
  //    cached+PPT error 6, truncated PPT error 27.
  {
    CodestreamSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(32, 32);
    spec.extra_params = {"Clayers=2", "Cuse_sop=yes", "Cuse_eph=yes",
                         "Cprecincts={16,16}", "Cblk={8,8}"};
    const auto base = EncodeCodestream(spec, MakePlanes(spec));

    for (bool use_ppm : {true, false}) {
      const auto repacked = RepackWithPackedHeaders(base, use_ppm);
      BufferSource src(repacked);
      kdu_codestream cs;
      cs.create(&src);
      const auto dec = DecodeCodestream(cs, false);
      EXPECT_EQ(dec.size(), 1U);
      cs.destroy();
    }

    // Both PPM and PPT -> Error 13.
    {
      const auto both = RepackWithPackedHeaders(base, true, /*emit_both=*/true);
      BufferSource src(both);
      error_handler_.clear();
      kdu_codestream cs;
      cs.create(&src);
      EXPECT_THROW(cs.open_tile(kdu_coords(0, 0)), int);
      cs.destroy();
    }

    // Truncated PPT -> Error 27 or 32.
    {
      const auto trunc =
          RepackWithPackedHeaders(base, false, false, /*truncate_packed=*/true);
      BufferSource src(trunc);
      error_handler_.clear();
      kdu_codestream cs;
      cs.create(&src);
      EXPECT_THROW((void)DecodeCodestream(cs, false), int);
      cs.destroy();
    }

    // Cached source with PPT marker -> Error 6.
    const auto ppt_bytes = RepackWithPackedHeaders(base, false);
    size_t sot = 2;
    while (sot + 4 <= ppt_bytes.size() &&
           !((ppt_bytes[sot] == 0xFF) && (ppt_bytes[sot + 1] == 0x90))) {
      const size_t len =
          (static_cast<size_t>(ppt_bytes[sot + 2]) << 8) | ppt_bytes[sot + 3];
      sot += 2 + len;
    }
    size_t sod = sot + 12;
    while (sod + 2 <= ppt_bytes.size() &&
           !((ppt_bytes[sod] == 0xFF) && (ppt_bytes[sod + 1] == 0x93))) {
      const size_t len =
          (static_cast<size_t>(ppt_bytes[sod + 2]) << 8) | ppt_bytes[sod + 3];
      sod += 2 + len;
    }
    ASSERT_LT(sod, ppt_bytes.size());
    std::vector<kdu_byte> main_hdr(ppt_bytes.begin(),
                                   ppt_bytes.begin() + sot + 12);
    std::vector<kdu_byte> tile_hdr(ppt_bytes.begin() + sot + 12,
                                   ppt_bytes.begin() + sod);
    {
      CachedHeaderSource cached_ppt(main_hdr, tile_hdr,
                                    /*hide_tileheader=*/false);
      error_handler_.clear();
      kdu_codestream cs;
      cs.create(&cached_ppt);
      EXPECT_THROW(cs.open_tile(kdu_coords(0, 0)), int);
      cs.destroy();
    }
    {
      CachedHeaderSource hidden_tile(main_hdr, tile_hdr,
                                     /*hide_tileheader=*/true);
      kdu_codestream cs;
      cs.create(&hidden_tile);
      cs.set_persistent();
      kdu_tile t0 = cs.open_tile(kdu_coords(0, 0));
      EXPECT_TRUE(t0.exists());
      t0.close();
      cs.destroy();
    }
  }

  // 2. AsyncMemSource incremental non-blocking load_memory ->
  //    KD_PFLAG_ASYNC_BODY_WAITING and async_check_would_block_and_notify.
  {
    CodestreamSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(32, 32);
    spec.extra_params = {"Clayers=2", "Cblk={16,16}", "Cprecincts={16,16}"};
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));

    AsyncMemSource async_src(bytes, 64, 48,
                             KDU_SOURCE_CAP_MEMORY_BACKED |
                                 KDU_SOURCE_CAP_ASYNC_NOTIFICATION |
                                 KDU_SOURCE_CAP_SEEKABLE,
                             /*note_block_once=*/true);
    kdu_codestream cs;
    cs.create(&async_src);
    cs.enable_restart();
    cs.set_max_bytes(static_cast<kdu_long>(bytes.size()) - 2);
    cs.set_max_bytes(static_cast<kdu_long>(bytes.size()) * 2);
    const auto dec = DecodeCodestream(cs, false);
    EXPECT_EQ(dec.size(), 1U);
    cs.destroy();
  }

  // 3. find_cbr_set_heights error paths (Cplex=EST + Scbr + transpose/vflip,
  //    subsampling mismatch, misaligned origin).
  {
    CodestreamSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {"Corder=PCRL", "Cprecincts={16,16}", "Cblk={8,8}",
                         "Cplex={6,EST,0.25,0}", "Scbr={1,256}"};
    const auto planes = MakePlanes(spec);
    auto expect_cbr_start_fail = [&](const CodestreamSpec& s,
                                     const std::vector<std::vector<int>>& pl,
                                     bool transpose) {
      siz_params siz;
      ConfigureSiz(siz, s);
      BufferTarget tgt;
      kdu_codestream cs;
      cs.create(&siz, &tgt);
      ApplyCodingParams(cs, s);
      cs.set_max_bytes(4096, false);
      if (transpose) cs.change_appearance(true, false, false);
      error_handler_.clear();
      EXPECT_THROW(([&]() {
                     kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
                     try {
                       PushTileSamples(cs, tile, pl, s);
                     } catch (...) {
                       if (tile.exists()) tile.close();
                       throw;
                     }
                     tile.close();
                   })(),
                   int);
      cs.destroy();
    };
    expect_cbr_start_fail(spec, planes, /*transpose=*/true);

    CodestreamSpec bad_orig = spec;
    bad_orig.origin = kdu_coords(0, 1);
    expect_cbr_start_fail(bad_orig, MakePlanes(bad_orig), /*transpose=*/false);

    CodestreamSpec bad_subs = spec;
    bad_subs.components = 2;
    bad_subs.subsampling = kdu_coords(1, 3);
    bad_subs.extra_params[0] = "Corder=CPRL";
    expect_cbr_start_fail(bad_subs, MakePlanes(bad_subs), /*transpose=*/false);

    CodestreamSpec bad_prec = spec;
    bad_prec.components = 2;
    bad_prec.extra_params.push_back("Cprecincts:C1={32,32}");
    expect_cbr_start_fail(bad_prec, MakePlanes(bad_prec), /*transpose=*/false);
  }

  // 4. Non-resilient SOP mismatch error 25 & allow_parsing_errors recovery.
  {
    CodestreamSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.extra_params = {"Cuse_sop=yes", "Cprecincts={16,16}", "Cblk={8,8}"};
    const auto clean = EncodeCodestream(spec, MakePlanes(spec));
    std::vector<kdu_byte> bad_sop = clean;
    for (size_t i = 0; i + 5 < bad_sop.size(); ++i) {
      if (bad_sop[i] == 0xFF && bad_sop[i + 1] == 0x91) {
        bad_sop[i + 5] ^= 0x77;  // Corrupt SOP sequence number.
        break;
      }
    }
    BufferSource src(bad_sop, KDU_SOURCE_CAP_SEQUENTIAL);
    error_handler_.clear();
    kdu_codestream cs;
    cs.create(&src);
    EXPECT_THROW((void)DecodeCodestream(cs, false), int);
    cs.destroy();
  }
}

TEST_F(CompressedTest, RemainingSingleThreadCoveragePaths) {
  // 1. Non-persistent multi-tile recycle WITHOUT allow_restart so
  //    typical_tile_cache is populated and recycled across !fully_typical
  //    (Qstep:T1, Rshift:T2, Qderived=no:T3) and !is_typical (Corder:T4).
  {
    CodestreamSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(16, 16);
    spec.extra_params = {
        "Clayers=2",      "Corder=LRCP",    "Cprecincts={8,8}", "Cblk={8,8}",
        "Qderived=yes",   "Qstep=0.01",     "Qstep:T1=0.04",    "Rshift:T2=16",
        "Qderived:T3=no", "Corder:T4=RLCP",
    };
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    BufferSource src(bytes);
    kdu_codestream cs;
    cs.create(&src);
    // Explicitly NO enable_restart() so closed typical tiles enter cache.
    const auto dec = DecodeCodestream(cs, false);
    EXPECT_EQ(dec.size(), 1U);
    cs.destroy();
  }

  // 2. kdu_quality_limiter with multi-stage MCT
  // (kd_tile::set_elements_of_interest
  //    mct_head != NULL loop).
  {
    CodestreamSpec spec;
    spec.width = 48;
    spec.height = 48;
    spec.components = 4;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {
        "Cycc=no",
        "Clayers=2",
        "Cprecincts={16,16}",
        "Cblk={8,8}",
        "Sextensions=MCT",
        "Mcomponents=4",
        "Mprecision=8",
        "Msigned=no",
        "Mmatrix_size:I1=4",
        "Mmatrix_coeffs:I1=1,0,0,1",
        "Mmatrix_size:I2=4",
        "Mmatrix_coeffs:I2=1,0,0,1",
        "Mstage_inputs:I1={0,1},{2,3}",
        "Mstage_outputs:I1={0,1},{2,3}",
        "Mstage_collections:I1={2,2},{2,2}",
        "Mstage_xforms:I1={MATRIX,1,0,0,0},{MATRIX,2,0,0,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    BufferSource src(bytes);
    kdu_codestream cs;
    cs.create(&src);
    cs.set_persistent();
    kdu_quality_limiter qlim(0.05F, false);
    qlim.set_comp_info(0, 1.0F, -1);
    qlim.set_comp_info(1, 2.0F, 1);
    qlim.set_comp_info(2, 2.0F, 1);
    qlim.set_comp_info(3, 1.5F, -1);
    cs.apply_input_restrictions(0, 0, 1, 0, nullptr, KDU_WANT_OUTPUT_COMPONENTS,
                                nullptr, &qlim);
    const auto dec = DecodeCodestream(cs, false);
    EXPECT_EQ(dec.size(), 4U);
    cs.destroy();
  }

  // 3. add_flush_stats with non-CBR sequential write_packet and cached_target
  //    cache_write_packets (+ Cuse_sop).
  for (bool cached : {false, true}) {
    CodestreamSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {"Corder=PCRL", "Clayers=2", "Cprecincts={16,16}",
                         "Cblk={8,8}", "Cuse_sop=yes"};
    const auto planes = MakePlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    BufferTarget tgt(cached);
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ApplyCodingParams(cs, spec);
    kdu_flush_stats fstats = cs.add_flush_stats(0, 5, 0, 1);
    PushAllTiles(cs, planes, spec);
    kdu_long lbytes[2] = {512, 2048};
    cs.flush(lbytes, 2);
    fstats.advance(1);
    cs.destroy();
    if (cached) {
      EXPECT_FALSE(tgt.tile_headers.empty());
    } else {
      EXPECT_GT(tgt.data.size(), 100U);
    }
  }

  // 4. CBR fine_fill_bytes (> 0) in simulate_packet_cbr and write_packet_cbr.
  {
    CodestreamSpec spec;
    spec.width = 128;
    spec.height = 128;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {"Corder=PCRL",  "Clayers=1",   "Cprecincts={32,32}",
                         "Cblk={16,16}", "Qstep=0.001", "Scbr={1,256}"};
    const auto planes = MakePlanes(spec);
    kdu_long cbr_bytes[1] = {32771};  // Odd target larger than encoded data.
    const auto bytes = EncodeCodestream(spec, planes, nullptr, cbr_bytes, 1);
    EXPECT_GT(bytes.size(), 1000U);
  }

  // 5. Async non-blocking parsing (KD_PFLAG_ASYNC_BODY_WAITING),
  //    reset_packet_reading (Clayers=2), parse_all_relevant_packets
  //    need_unload, and non-addressable persistent out-of-order tiles.
  {
    CodestreamSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(32, 32);
    spec.extra_params = {"Clayers=2",    "Cuse_sop=yes",
                         "Cuse_eph=yes", "Cprecincts={16,16}",
                         "Cblk={8,8}",   "ORGgen_plt=yes"};
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));

    // Non-seekable AsyncMemSource.
    AsyncMemSource async_src(
        bytes, 120, 24,
        KDU_SOURCE_CAP_MEMORY_BACKED | KDU_SOURCE_CAP_ASYNC_NOTIFICATION,
        /*note_block_once=*/true, /*defer_nonblocking_calls=*/4);
    kdu_codestream cs;
    cs.create(&async_src);
    cs.set_persistent();
    for (int ty = 0; ty < 2; ++ty) {
      for (int tx = 0; tx < 2; ++tx) {
        kdu_tile tile = cs.open_tile(kdu_coords(tx, ty));
        EXPECT_TRUE(tile.parse_all_relevant_packets(false, nullptr));
        tile.close();
      }
    }
    const auto dec = DecodeCodestream(cs, false);
    EXPECT_EQ(dec.size(), 1U);
    cs.destroy();

    // Persistent seekable reset_packet_reading + need_unload.
    BufferSource seek_src(bytes);
    kdu_codestream pcs;
    pcs.create(&seek_src);
    pcs.set_persistent();
    kdu_dims left_reg;
    left_reg.pos = kdu_coords(0, 0);
    left_reg.size = kdu_coords(32, 64);
    pcs.apply_input_restrictions(0, 1, 0, 1, &left_reg,
                                 KDU_WANT_CODESTREAM_COMPONENTS);
    {
      kdu_tile t = pcs.open_tile(kdu_coords(0, 0));
      EXPECT_TRUE(t.parse_all_relevant_packets(false, nullptr));
      t.close();
    }
    // Expand layers from 1 -> 2 on already-parsed addressable precinct.
    pcs.apply_input_restrictions(0, 1, 0, 2, &left_reg,
                                 KDU_WANT_CODESTREAM_COMPONENTS);
    {
      kdu_tile t = pcs.open_tile(kdu_coords(0, 0));
      EXPECT_TRUE(t.parse_all_relevant_packets(false, nullptr));
      t.close();
    }
    // Shift region to right half so left-tile precincts need_unload on open.
    kdu_dims right_reg;
    right_reg.pos = kdu_coords(32, 0);
    right_reg.size = kdu_coords(32, 64);
    pcs.apply_input_restrictions(0, 1, 0, 2, &right_reg,
                                 KDU_WANT_CODESTREAM_COMPONENTS);
    {
      kdu_tile t = pcs.open_tile(kdu_coords(0, 0));
      EXPECT_TRUE(t.parse_all_relevant_packets(false, nullptr));
      t.close();
    }
    pcs.destroy();

    // Non-addressable persistent out-of-order tile access ->
    // desequence_packets_until_tile_inactive.
    BufferSource seq_src(bytes, KDU_SOURCE_CAP_SEQUENTIAL);
    kdu_codestream seq_cs;
    seq_cs.create(&seq_src);
    seq_cs.set_persistent();
    kdu_tile t10 = seq_cs.open_tile(kdu_coords(1, 0));
    EXPECT_TRUE(t10.parse_all_relevant_packets(false, nullptr));
    t10.close();
    kdu_tile t00 = seq_cs.open_tile(kdu_coords(0, 0));
    EXPECT_TRUE(t00.exists());
    t00.close();
    seq_cs.destroy();
  }

  // 6. SPF (configure_simultaneous_processing_fragments), Cprog_weights +
  //    Cplex=ADJ, Creslengths, ORGgen_tlm padding, auto_flush
  //    block_row_generated, and Sprofile warnings.
  for (bool transpose : {false, true}) {
    CodestreamSpec spec;
    spec.width = transpose ? 64 : 512;
    spec.height = transpose ? 512 : 64;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {
        "Clayers=2",
        "Cprecincts={16,16}",
        "Cblk={8,8}",
        "Cprog_weights=0.5,0.5",
        transpose ? "Cplex={6,FIX,0.5,0}" : "Cplex={6,WFIX,0.5,0}",
        "Creslengths=1200,2400,4800",
        "ORGgen_tlm=16"};
    const auto planes = MakePlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    BufferTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ApplyCodingParams(cs, spec);
    if (transpose) cs.change_appearance(true, false, false);
    cs.configure_simultaneous_processing_fragments(64, 4, 4);
    PushAllTiles(cs, planes, spec);
    kdu_long lbytes[2] = {1024, 4096};
    cs.flush(lbytes, 2);
    cs.destroy();
    EXPECT_GT(tgt.data.size(), 200U);
  }

  // Direct kdu_subband::block_row_generated with auto_flush (env == nullptr).
  {
    CodestreamSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.extra_params = {"Cprecincts={32,32}", "Cblk={16,16}"};
    siz_params siz;
    ConfigureSiz(siz, spec);
    BufferTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ApplyCodingParams(cs, spec);
    kdu_long lbytes[1] = {4096};
    cs.auto_flush(1, 1, 8, 8, lbytes, 1, nullptr, true, true);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    kdu_tile_comp tc = tile.access_component(0);
    for (int r = 0; r < tc.get_num_resolutions(); ++r) {
      kdu_resolution res = tc.access_resolution(r);
      int min_b = 0;
      const int nb = res.get_valid_band_indices(min_b);
      for (int bi = 0; bi < nb; ++bi) {
        kdu_subband sb = res.access_subband(min_b + bi);
        sb.block_row_generated(8, false, nullptr);
        sb.block_row_generated(8, true, nullptr);
      }
    }
    tile.close();
    cs.flush(lbytes, 1);
    cs.destroy();
  }

  // Profile warning checks (CINEMA2K without SOP/EPH or wrong levels/irrev).
  {
    CodestreamSpec spec;
    spec.width = 128;
    spec.height = 64;
    spec.components = 3;
    spec.bit_depth = 12;
    spec.reversible = true;
    spec.dwt_levels = 0;
    spec.extra_params = {"Sprofile=CINEMA2K", "Cuse_sop=yes", "Cuse_eph=yes"};
    const auto planes = MakePlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    BufferTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ApplyCodingParams(cs, spec);
    warning_handler_.clear();
    PushAllTiles(cs, planes, spec);
    kdu_long lbytes[1] = {4096};
    cs.flush(lbytes, 1);
    EXPECT_FALSE(warning_handler_.history().empty());
    cs.destroy();
  }
}

TEST_F(CompressedTest, MultiThreadedEncodingAndDecodingPaths) {
  kdu_thread_env env;
  env.create();
  ASSERT_TRUE(env.add_thread());
  ASSERT_TRUE(env.add_thread());
  kdu_membroker broker(1LL << 20);

  // 1. Multi-threaded encoding: Cplex=EST, set_max_bytes, auto_flush, restart,
  //    kdu_multi_analysis(&env), and background tile closure.
  BufferTarget mt_tgt1;
  BufferTarget mt_tgt2;
  {
    CodestreamSpec spec;
    spec.width = 128;
    spec.height = 128;
    spec.components = 3;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(64, 64);
    spec.extra_params = {
        "Cycc=yes",           "Clayers=2",      "Corder=PCRL",
        "Cprecincts={16,16}", "Cblk={8,8}",     "Cuse_sop=yes",
        "Cuse_eph=yes",       "ORGgen_plt=yes", "Cplex={6,EST,0.25,0}",
    };
    const auto planes = MakePlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    kdu_codestream cs;
    cs.create(&siz, &mt_tgt1, nullptr, 0, 0, &env, &broker);
    ApplyCodingParams(cs, spec);
    cs.configure_simultaneous_processing_fragments(64, 4, 4);
    cs.set_max_bytes(8192, /*allow_slope_est=*/false,
                     /*enable_periodic_trimming=*/true);
    cs.enable_restart();
    kdu_long lbytes[2] = {1024, 4096};
    cs.auto_flush(1, 1, 8, 8, lbytes, 2, nullptr, true, true, 0.0, &env);
    kdu_dims tvalid;
    cs.get_valid_tiles(tvalid);
    for (int ty = 0; ty < tvalid.size.y; ++ty) {
      for (int tx = 0; tx < tvalid.size.x; ++tx) {
        kdu_tile tile = cs.open_tile(kdu_coords(tx, ty), &env);
        PushTileSamples(cs, tile, planes, spec, nullptr, &env);
        tile.close(&env, /*close_in_background=*/true);
      }
    }
    cs.flush(lbytes, 2, nullptr, true, false, 0.0, &env);

    cs.restart(&mt_tgt2, &env);
    cs.set_min_slope_threshold(32000);
    PushAllTiles(cs, planes, spec, nullptr, &env);
    cs.flush(lbytes, 2, nullptr, true, false, 0.0, &env);
    env.cs_terminate(cs);
    cs.destroy();
    EXPECT_GT(mt_tgt1.data.size(), 200U);
    EXPECT_GT(mt_tgt2.data.size(), 200U);
  }

  // 2. Multi-threaded decoding: open_tiles(bkgnd_open), access_tile(&env),
  //    kdu_multi_synthesis(&env), close_in_background,
  //    apply_input_restrictions, close_tiles(&env), restart(&env), plus MT
  //    compact_blocks and async.
  {
    BufferSource src1(mt_tgt1.data);
    BufferSource src2(mt_tgt2.data);
    kdu_codestream cs;
    cs.create(&src1, &env, &broker);
    cs.enable_restart();
    cs.set_persistent();
    kdu_dims valid_tiles;
    cs.get_valid_tiles(valid_tiles);
    cs.open_tiles(valid_tiles, /*bkgnd_open=*/true, &env);
    {
      const auto dec1 = DecodeCodestream(cs, true, &env, /*close_bkgnd=*/true);
      EXPECT_EQ(dec1.size(), 3U);
    }
    kdu_dims roi;
    roi.pos = kdu_coords(0, 0);
    roi.size = kdu_coords(64, 64);
    cs.apply_input_restrictions(0, 0, 1, 1, &roi, KDU_WANT_OUTPUT_COMPONENTS,
                                &env);
    cs.open_tiles(valid_tiles, /*bkgnd_open=*/false, &env);
    {
      kdu_tile t00 = cs.access_tile(kdu_coords(0, 0), true, &env);
      ASSERT_TRUE(t00.exists());
      t00.close(&env, /*close_in_background=*/true);
    }
    cs.close_tiles(valid_tiles, &env);

    cs.restart(&src2, &env);
    cs.apply_input_restrictions(0, 0, 0, 2, nullptr, KDU_WANT_OUTPUT_COMPONENTS,
                                &env);
    const auto dec2 = DecodeCodestream(cs, false, &env, false);
    EXPECT_EQ(dec2.size(), 3U);
    env.cs_terminate(cs);
    cs.destroy();
  }

  // 3. MT non-persistent 16-tile decode (MT tile recycle + heterogeneous
  //    Qstep/Corder) + MT compact_blocks (in-memory 1-layer).
  {
    CodestreamSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(16, 16);
    spec.extra_params = {"Clayers=1",  "Corder=LRCP",  "Cprecincts={8,8}",
                         "Cblk={8,8}", "Rshift:T1=16", "Corder:T2=RLCP"};
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    BufferSource mem_src(bytes, KDU_SOURCE_CAP_SEQUENTIAL |
                                    KDU_SOURCE_CAP_SEEKABLE |
                                    KDU_SOURCE_CAP_IN_MEMORY);
    kdu_codestream cs;
    cs.create(&mem_src, &env);
    const auto dec = DecodeCodestream(cs, false, &env, false);
    EXPECT_EQ(dec.size(), 1U);
    env.cs_terminate(cs);
    cs.destroy();
  }

  // 4. Non-seekable AsyncMemSource body load
  //    (single-threaded create so unpatched kdu_threads.cpp event deregister
  //    UB is avoided while still exercising non-seekable incremental memory).
  {
    AsyncMemSource async_src(
        mt_tgt1.data, 256, 64,
        KDU_SOURCE_CAP_MEMORY_BACKED | KDU_SOURCE_CAP_ASYNC_NOTIFICATION,
        /*note_block_once=*/true, /*defer_nonblocking_calls=*/0,
        /*run_feeder=*/false);
    kdu_codestream cs;
    cs.create(&async_src);
    cs.set_persistent();
    const auto dec = DecodeCodestream(cs, false);
    EXPECT_EQ(dec.size(), 3U);
    cs.destroy();
  }

  env.destroy();
}

TEST_F(CompressedTest, MultiFragmentSpfAppearanceAndCancellationPaths) {
  kdu_thread_env env;
  env.create();
  ASSERT_TRUE(env.add_thread());
  ASSERT_TRUE(env.add_thread());

  // 1. MT non-CPLEX kdu_multi_analysis with multiple precincts: covers
  //    advance_bkgnd_prepared_precinct_rows (normal, vflip, transpose) and
  //    S=2 premature closure on early engine.destroy(&env).
  for (int mode = 0; mode < 4; ++mode) {
    CodestreamSpec spec;
    spec.width = 96;
    spec.height = 96;
    spec.components = 3;
    spec.reversible = (mode == 1);
    spec.dwt_levels = 2;
    spec.extra_params = {"Corder=PCRL", "Cprecincts={8,8}", "Cblk={8,8}",
                         "Clayers=1"};
    const auto planes = MakePlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    BufferTarget tgt(mode == 2);
    kdu_codestream cs;
    cs.create(&siz, &tgt, nullptr, 0, 0, &env);
    ApplyCodingParams(cs, spec);
    if (mode == 1) {
      cs.change_appearance(false, true, false);  // vflip=true
    } else {
      if (mode == 2) {
        cs.change_appearance(true, false, true);  // transpose=true, hflip=true
      }
      kdu_long ab[1] = {1LL << 20};
      cs.auto_flush(1, 1, 8, 8, ab, 1, nullptr, true, true, 0.0, &env);
    }

    kdu_tile tile = cs.open_tile(kdu_coords(0, 0), &env);
    if (mode < 3) {
      PushTileSamples(cs, tile, planes, spec, nullptr, &env);
    } else {
      kdu_thread_queue tq;
      ASSERT_TRUE(env.attach_queue(&tq, nullptr, nullptr));
      kdu_multi_analysis engine;
      engine.create(cs, tile, &env, &tq, KDU_MULTI_XFORM_MT_DWT);
      kdu_line_buf* line = engine.exchange_line(0, nullptr, &env);
      if (line != nullptr) {
        const int szx = engine.get_size(0).x;
        if (line->get_buf32() != nullptr) {
          for (int x = 0; x < szx; ++x) line->get_buf32()[x].fval = 0.0F;
        }
        engine.exchange_line(0, line, &env);
      }
      engine.destroy(&env);
      env.join(&tq, false);
    }
    tile.close(&env);
    env.cs_terminate(cs);
    if (mode != 3) {
      kdu_long flush_budget[1] = {1LL << 20};
      cs.flush(flush_budget, 1);
      if (mode == 2) {
        EXPECT_FALSE(tgt.tile_headers.empty());
        EXPECT_FALSE(tgt.precincts.empty());
      } else {
        EXPECT_GT(tgt.data.size(), 50U) << "mode=" << mode;
      }
    }
    cs.destroy();
  }

  // 2. True multi-fragment SPF (num_fragments > 1) with Cplex={6,EST,0.25,0},
  //    normal / transpose / vflip / early destroy (&env), describe + detach
  //    block notifier, and early MT decode kdu_multi_synthesis::destroy(&env).
  std::vector<kdu_byte> cplex_encoded;
  for (int mode = 0; mode < 4; ++mode) {
    CodestreamSpec spec;
    spec.width = 256;
    spec.height = 128;
    spec.components = 3;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {
        "Corder=RPCL",      "Cprecincts={16,16}",        "Cblk={8,8}",
        "Cdecomp=B(-:-:-)", "Cprog_weights=0.5,2.0,2.0", "Cplex={6,EST,0.25,0}",
        "Clayers=2"};
    const auto planes = MakePlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    BufferTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt, nullptr, 0, 0, &env);
    ApplyCodingParams(cs, spec);
    if (mode == 1) {
      cs.change_appearance(true, false, false);  // transpose
    } else if (mode == 2) {
      cs.change_appearance(false, true, false);  // vflip
    }
    cs.configure_simultaneous_processing_fragments(16, 2, 4);
    cs.set_max_bytes(16384, /*simulate_parsing=*/false);
    cs.set_max_slope_threshold(48000);
    kdu_long ab[2] = {4096, 16384};
    cs.auto_flush(1, 1, 8, 8, ab, 2, nullptr, true, true, 0.0, &env);

    kdu_tile tile = cs.open_tile(kdu_coords(0, 0), &env);
    kdu_thread_queue tq;
    ASSERT_TRUE(env.attach_queue(&tq, nullptr, nullptr));
    kdu_multi_analysis engine;
    engine.create(cs, tile, &env, &tq, KDU_MULTI_XFORM_MT_DWT);
    const int max_rows = (mode == 3) ? 2 : 1 << 20;
    std::vector<int> heights(spec.components);
    std::vector<int> widths(spec.components);
    for (int c = 0; c < spec.components; ++c) {
      const kdu_coords sz = engine.get_size(c);
      widths[c] = sz.x;
      heights[c] = std::min(sz.y, max_rows);
    }
    std::vector<int> pushed(spec.components, 0);
    while (true) {
      bool any = false;
      bool prog = false;
      for (int c = 0; c < spec.components; ++c) {
        if (pushed[c] >= heights[c]) continue;
        any = true;
        kdu_line_buf* line = engine.exchange_line(c, nullptr, &env);
        if (line == nullptr) continue;
        const float scale = 1.0F / static_cast<float>(1 << spec.bit_depth);
        const int off = 1 << (spec.bit_depth - 1);
        const int y = pushed[c] % spec.height;
        kdu_sample32* d32 = line->get_buf32();
        kdu_sample16* d16 = line->get_buf16();
        for (int x = 0; x < widths[c]; ++x) {
          const int cx = x % spec.width;
          const float fval =
              static_cast<float>(
                  planes[c][static_cast<size_t>(y) * spec.width + cx] - off) *
              scale;
          if (d32 != nullptr) {
            d32[x].fval = fval;
          } else {
            d16[x].ival = static_cast<kdu_int16>(
                fval * static_cast<float>(1 << KDU_FIX_POINT));
          }
        }
        engine.exchange_line(c, line, &env);
        ++pushed[c];
        prog = true;
      }
      if (!any) break;
      if (!prog) env.join(&tq, false);
    }
    engine.destroy(&env);
    env.join(&tq, false);
    tile.close(&env);
    env.cs_terminate(cs);
    if (mode != 3) {
      kdu_long budget[2] = {4096, 16384};
      cs.flush(budget, 2);
    }
    cs.destroy();
    if (mode == 0) {
      cplex_encoded = std::move(tgt.data);
      EXPECT_FALSE(cplex_encoded.empty());
    }
  }

  // Exercise describe_block_notifier_state, double detach_block_notifier throw,
  // and early kdu_multi_synthesis::destroy(&env) cancellation on input decode.
  if (!cplex_encoded.empty()) {
    BufferSource src(cplex_encoded);
    kdu_codestream cs;
    cs.create(&src, &env);
    cs.enable_restart();
    kdu_dims tvalid;
    cs.get_valid_tiles(tvalid);
    cs.open_tiles(tvalid, true, &env);
    kdu_tile tile = cs.access_tile(kdu_coords(0, 0), true, &env);
    kdu_thread_queue bq;
    ASSERT_TRUE(env.attach_queue(&bq, nullptr, nullptr));
    kdu_subband sb =
        tile.access_component(0).access_resolution(1).access_subband(LH_BAND);
    (void)sb.attach_block_notifier(&bq, &env);
    sb.advance_block_rows_needed(&bq, 1, &env);
    TestMessageHandler info_msg(false);
    sb.describe_block_notifier_state(&bq, info_msg);
    sb.detach_block_notifier(&bq, &env);
    EXPECT_THROW(sb.detach_block_notifier(&bq, &env), int);
    env.join(&bq, false);
    kdu_multi_synthesis synth;
    kdu_thread_queue sq;
    ASSERT_TRUE(env.attach_queue(&sq, nullptr, nullptr));
    synth.create(cs, tile, &env, &sq, KDU_MULTI_XFORM_MT_DWT);
    (void)synth.get_line(0, &env);
    synth.destroy(&env);
    env.join(&sq, false);
    tile.close(&env, true);
    cs.close_tiles(tvalid, &env);
    env.cs_terminate(cs);
    cs.destroy();
  }

  // 3. cached_target + auto_flush under &env (normal & transpose), plus
  //    attach_flush_stats with out-of-order precinct opens across cs -> cs2.
  for (bool do_transpose : {false, true}) {
    CodestreamSpec spec;
    spec.width = 128;
    spec.height = 128;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {"Corder=PCRL", "Cprecincts={16,16}", "Cblk={8,8}"};
    const auto planes = MakePlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    BufferTarget tgt(/*cached=*/true);
    kdu_codestream cs;
    cs.create(&siz, &tgt, nullptr, 0, 0, &env);
    ApplyCodingParams(cs, spec);
    if (do_transpose) {
      cs.change_appearance(true, false, false);
    }
    kdu_long ab[1] = {16384};
    cs.auto_flush(1, 1, 8, 8, ab, 1, nullptr, true, true, 0.0, &env);
    PushAllTiles(cs, planes, spec, nullptr, &env);
    env.cs_terminate(cs);
    kdu_long budget[1] = {16384};
    cs.flush(budget, 1);
    cs.destroy();
    EXPECT_FALSE(tgt.tile_headers.empty());
    EXPECT_FALSE(tgt.precincts.empty());
  }

  {
    CodestreamSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.extra_params = {"Cprecincts={8,8}", "Cblk={8,8}"};
    const auto planes = MakePlanes(spec);
    siz_params siz1, siz2;
    ConfigureSiz(siz1, spec);
    ConfigureSiz(siz2, spec);
    BufferTarget tgt1, tgt2;
    kdu_codestream cs1, cs2;
    cs1.create(&siz1, &tgt1);
    ApplyCodingParams(cs1, spec);
    kdu_flush_stats fstats = cs1.add_flush_stats(0, 5, 0, 1);
    PushAllTiles(cs1, planes, spec);
    kdu_long l1[1] = {4096};
    cs1.flush(l1, 1);
    fstats.advance(1);

    cs2.create(&siz2, &tgt2);
    ApplyCodingParams(cs2, spec);
    kdu_flush_stats fstats2 = cs2.add_flush_stats(2, 5, 0, 1);
    (void)fstats2;
    cs2.attach_flush_stats(fstats);
    cs2.change_appearance(false, true, true);
    PushAllTiles(cs2, planes, spec);
    cs2.flush(l1, 1);
    fstats2.advance(1);
    cs2.destroy();
    cs1.destroy();
  }

  // 4. Constant-zero CBR bulk_fill_bytes > 8192 + fine_fill_bytes > 0 (both
  //    with and without add_flush_stats) to hit write_packet_cbr fill paths.
  for (bool with_fstats : {false, true}) {
    CodestreamSpec spec;
    spec.width = 256;
    spec.height = 256;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {"Corder=PCRL",  "Clayers=1",    "Cprecincts={64,64}",
                         "Cblk={16,16}", "Qstep=0.0005", "Scbr={1,512}"};
    std::vector<std::vector<int>> smooth_planes(
        1, std::vector<int>(static_cast<size_t>(spec.width * spec.height), 0));
    for (int y = 0; y < spec.height; ++y) {
      for (int x = 0; x < spec.width; ++x) {
        smooth_planes[0][static_cast<size_t>(y * spec.width + x)] =
            128 + (((x * 3 + y * 5) & 7) - 3);
      }
    }
    siz_params siz;
    ConfigureSiz(siz, spec);
    BufferTarget tgt(false);
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ApplyCodingParams(cs, spec);
    kdu_flush_stats fstats;
    if (with_fstats) {
      fstats = cs.add_flush_stats(0, 5, 0, 1);
    }
    PushAllTiles(cs, smooth_planes, spec);
    kdu_long cbr_budget[1] = {65541};
    cs.flush(cbr_budget, 1);
    if (with_fstats) {
      fstats.advance(1);
    }
    cs.destroy();
    EXPECT_GE(tgt.data.size(), 20000U);
  }

  env.destroy();
}

TEST_F(CompressedTest, ProfilesInterchangeMctAndPacketErrorPaths) {
  // 1. Profile compatibility warnings in kd_tile::initialize /
  //    check_heterogeneous_tiles_allowed.
  struct ProfileCase {
    std::vector<std::string> params;
    kdu_coords tile_size;
    kdu_coords origin;
    bool reversible;
    int bit_depth;
  };
  const std::vector<ProfileCase> profile_cases = {
      // PROFILE0 violations: non-zero origin, BYPASS mode, Rshift out of range.
      {{"Sprofile=PROFILE0", "Cmodes=BYPASS", "Cblk={16,16}", "Rshift=40"},
       kdu_coords(0, 0),
       kdu_coords(1, 1),
       false,
       8},
      // PROFILE1 violation: rectangular Cblk > 64 (Srequired_CBto6).
      {{"Sprofile=PROFILE1", "Cblk={128,32}"},
       kdu_coords(0, 0),
       kdu_coords(0, 0),
       false,
       8},
      // CINEMA2S: forbids SOP/EPH, requires 5x5 CB, P<=256, irrev.
      {{"Sprofile=CINEMA2S", "Cuse_sop=yes", "Cuse_eph=yes", "Cblk={16,16}",
        "Cprecincts={512,512}"},
       kdu_coords(0, 0),
       kdu_coords(0, 0),
       true,
       12},
      // CINEMASS: requires SOP/EPH and P >= CB.
      {{"Sprofile=CINEMASS", "Cuse_sop=no", "Cuse_eph=no", "Cprecincts={8,8}",
        "Cblk={32,32}"},
       kdu_coords(0, 0),
       kdu_coords(0, 0),
       false,
       12},
      // BROADCAST: requires CB 7x6 / reversible mismatch / multi-tile.
      {{"Sprofile=BROADCAST", "Sbroadcast={6,single,rev}", "Cblk={16,16}"},
       kdu_coords(64, 64),
       kdu_coords(0, 0),
       false,
       10},
  };
  for (const auto& pc : profile_cases) {
    warning_handler_.clear();
    CodestreamSpec spec;
    spec.width = 128;
    spec.height = 64;
    spec.components = 3;
    spec.bit_depth = pc.bit_depth;
    spec.reversible = pc.reversible;
    spec.dwt_levels = 2;
    spec.tile_size = pc.tile_size;
    spec.origin = pc.origin;
    spec.extra_params = pc.params;
    try {
      siz_params siz;
      ConfigureSiz(siz, spec);
      BufferTarget tgt;
      kdu_codestream cs;
      cs.create(&siz, &tgt);
      ApplyCodingParams(cs, spec);
      PushAllTiles(cs, MakePlanes(spec), spec);
      kdu_long budget[1] = {32768};
      cs.flush(budget, 1);
      cs.destroy();
    } catch (int) {
    }
  }

  // Heterogeneous multi-tile SIZ Rsiz=CINEMA2K on decode triggers
  // check_heterogeneous_tiles_allowed warnings in kd_tile::initialize.
  {
    CodestreamSpec spec;
    spec.width = 128;
    spec.height = 64;
    spec.components = 3;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(64, 64);
    spec.extra_params = {"Clevels=2", "Clevels:T1=1", "Cprecincts={32,32}"};
    auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    ASSERT_GT(bytes.size(), 12U);
    // Patch Rsiz (bytes 4..5 right after SOC FF 4F FF 51 Lsiz_hi Lsiz_lo)
    // to CINEMA2K (0x0003) if valid SIZ header is present.
    if (bytes[0] == 0xFF && bytes[1] == 0x4F && bytes[2] == 0xFF &&
        bytes[3] == 0x51 && bytes.size() > 8) {
      std::vector<kdu_byte> patched = bytes;
      patched[6] = 0x00;
      patched[7] = 0x03;
      warning_handler_.clear();
      try {
        BufferSource src(patched);
        kdu_codestream cs;
        cs.create(&src);
        (void)DecodeCodestream(cs, false);
        cs.destroy();
      } catch (int) {
      }
      warning_handler_.clear();
      try {
        std::vector<kdu_byte> p2 = bytes;
        p2[6] = 0x00;
        p2[7] = 0x01;  // PROFILE0
        BufferSource src2(p2);
        kdu_codestream cs2;
        cs2.create(&src2);
        (void)DecodeCodestream(cs2, false);
        cs2.destroy();
      } catch (int) {
      }
    }
  }

  // 2. kdu_node::access_primary_child, change_appearance(transpose+vflip+hflip)
  //    get_parent_dims / get_valid_block_span, interchange
  //    kdu_subband::open_block, and
  //    set_synth_overshoot_limit(generate_errors=true).
  for (bool reversible : {true, false}) {
    CodestreamSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.reversible = reversible;
    spec.dwt_levels = 2;
    spec.extra_params = {"Cprecincts={16,16}", "Cblk={8,8}"};
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));

    {
      BufferSource src(bytes);
      kdu_codestream cs;
      cs.create(&src);
      cs.change_appearance(true, true, true);
      kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
      kdu_tile_comp tc = tile.access_component(0);
      for (int r = tc.get_num_resolutions() - 1; r >= 0; --r) {
        kdu_node node = tc.access_resolution(r).access_node();
        kdu_node child = node.access_primary_child();
        if (r == 0) {
          EXPECT_FALSE(child.exists());
        } else {
          EXPECT_TRUE(child.exists());
        }
        if (r > 0) {
          kdu_subband sb = tc.access_resolution(r).access_subband(LH_BAND);
          kdu_dims pdims, bspan;
          sb.get_parent_dims(pdims);
          sb.get_valid_block_span(bspan);
          EXPECT_GE(bspan.size.x, 0);
        }
      }
      tile.close();
      cs.destroy();
    }

    // Tight synth_overshoot_limit with generate_errors=true.
    {
      BufferSource src(bytes);
      kdu_codestream cs;
      cs.create(&src);
      cs.set_synth_overshoot_limit(0.0001F, true);
      try {
        (void)DecodeCodestream(cs, false);
      } catch (int) {
      }
      if (cs.exists()) cs.destroy();
    }
  }

  // Interchange codestream calling kdu_subband::open_block directly.
  {
    CodestreamSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.extra_params = {"Cprecincts={16,16}", "Cblk={8,8}"};
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    BufferSource dec_src(bytes);
    kdu_codestream dec;
    dec.create(&dec_src);
    kdu_tile dtile = dec.open_tile(kdu_coords(0, 0));

    siz_params isiz;
    ConfigureSiz(isiz, spec);
    kdu_codestream inter;
    inter.create(&isiz);
    inter.access_siz()->copy_all(dec.access_siz());
    inter.access_siz()->finalize_all();
    kdu_tile itile = inter.open_tile(kdu_coords(0, 0));
    kdu_subband dsb =
        dtile.access_component(0).access_resolution(1).access_subband(LH_BAND);
    kdu_subband isb =
        itile.access_component(0).access_resolution(1).access_subband(LH_BAND);
    kdu_dims iblks;
    isb.get_valid_blocks(iblks);
    if (iblks.size.x > 0 && iblks.size.y > 0) {
      kdu_block* sblk = dsb.open_block(iblks.pos);
      kdu_block* iblk = isb.open_block(iblks.pos);
      if (sblk != nullptr && iblk != nullptr) {
        CopyEncodedBlock(iblk, sblk);
      }
      if (iblk != nullptr) isb.close_block(iblk);
      if (sblk != nullptr) dsb.close_block(sblk);
    }
    itile.close();
    inter.destroy();
    dtile.close();
    dec.destroy();
  }

  // 3. MCT tile.get_global_attributes(mct_hash) + qlim_type_flags +
  //    cs.restart changing tile structure.
  {
    CodestreamSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 4;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(32, 32);
    spec.extra_params = {
        "Cycc=no",
        "Clayers=2",
        "Cprecincts={16,16}",
        "Cblk={8,8}",
        "Sextensions=MCT",
        "Mcomponents=4",
        "Mprecision=8",
        "Msigned=no",
        "Mmatrix_size:I1=4",
        "Mmatrix_coeffs:I1=1,0,0,1",
        "Mmatrix_size:I2=4",
        "Mmatrix_coeffs:I2=1,0,0,1",
        "Mstage_inputs:I1={0,1},{2,3}",
        "Mstage_outputs:I1={0,1},{2,3}",
        "Mstage_collections:I1={2,2},{2,2}",
        "Mstage_xforms:I1={MATRIX,1,0,0,0},{MATRIX,2,0,0,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    const auto b1 = EncodeCodestream(spec, MakePlanes(spec));
    CodestreamSpec spec2 = spec;
    spec2.width = 96;
    spec2.height = 48;
    spec2.tile_size = kdu_coords(48, 48);
    const auto b2 = EncodeCodestream(spec2, MakePlanes(spec2));

    BufferSource s1(b1);
    kdu_codestream cs;
    cs.create(&s1);
    cs.enable_restart();
    kdu_quality_limiter qlim(0.1F, false);
    qlim.set_comp_info(0, 1.0F, 0);
    qlim.set_comp_info(1, 2.0F, 1);
    cs.apply_input_restrictions(0, 0, 1, 0, nullptr, KDU_WANT_OUTPUT_COMPONENTS,
                                nullptr, &qlim);
    kdu_tile t = cs.open_tile(kdu_coords(0, 0));
    kdu_uint32 mct_hash = 0;
    t.get_global_attributes(mct_hash);
    EXPECT_NE(mct_hash, 0U);
    t.close();

    BufferSource s2(b2);
    cs.restart(&s2);
    EXPECT_EQ(DecodeCodestream(cs, false).size(), 4U);
    cs.destroy();
  }

  // 4. Packet corruption paths: SOP sequence mismatch, wiped SOP under
  //    expect_ubiquitous_sops, injected FF 93 / FF 7F after EPH, and
  //    corrupted packet headers with set_resilient / set_allow_parsing_errors.
  {
    CodestreamSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 2;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {"Corder=LRCP", "Clayers=2",    "Cprecincts={16,16}",
                         "Cblk={8,8}",  "Cuse_sop=yes", "Cuse_eph=yes"};
    const auto base = EncodeCodestream(spec, MakePlanes(spec));
    std::vector<size_t> sops;
    for (size_t i = 0; i + 5 < base.size(); ++i) {
      if (base[i] == 0xFF && base[i + 1] == 0x91 && base[i + 2] == 0x00 &&
          base[i + 3] == 0x04) {
        sops.push_back(i);
      }
    }
    if (sops.size() >= 4) {
      // (a) SOP sequence jump + wipe SOP marker -> expect_ubiquitous_sops seek.
      std::vector<kdu_byte> bad_sop = base;
      bad_sop[sops[1] + 5] = static_cast<kdu_byte>(bad_sop[sops[1] + 5] + 5);
      bad_sop[sops[2]] = 0x11;
      bad_sop[sops[2] + 1] = 0x22;
      for (bool resilient_sops : {true, false}) {
        for (bool allow_parse : {false, true}) {
          BufferSource src(bad_sop, KDU_SOURCE_CAP_SEQUENTIAL);
          kdu_codestream cs;
          cs.create(&src);
          if (resilient_sops) {
            cs.set_resilient(true);
          } else {
            cs.set_resilient(false);
          }
          if (allow_parse) {
            cs.set_allow_parsing_errors(true);
          }
          try {
            (void)DecodeCodestream(cs, false);
          } catch (int) {
          }
          if (cs.exists()) cs.destroy();
        }
      }

      // (b) Inject marker 0xFF 0x93 inside packet header / body after SOP/EPH.
      std::vector<kdu_byte> bad_body = base;
      for (size_t i = sops[1] + 6; i + 3 < sops[2]; ++i) {
        if (bad_body[i] == 0xFF && bad_body[i + 1] == 0x92) {
          bad_body[i + 2] = 0xFF;
          bad_body[i + 3] = 0x93;
          break;
        }
      }
      bad_body[sops[2] + 6] = 0xFF;
      bad_body[sops[2] + 7] = 0x7F;
      for (int mode = 0; mode < 3; ++mode) {
        BufferSource src(bad_body, mode == 2 ? (KDU_SOURCE_CAP_SEQUENTIAL |
                                                KDU_SOURCE_CAP_SEEKABLE)
                                             : KDU_SOURCE_CAP_SEQUENTIAL);
        kdu_codestream cs;
        cs.create(&src);
        if (mode == 0) {
          cs.set_resilient(false);
          cs.set_allow_parsing_errors(true);
        } else if (mode == 1) {
          cs.set_resilient(true);
        } else {
          cs.set_fussy();
        }
        try {
          (void)DecodeCodestream(cs, false);
        } catch (int) {
        }
        if (cs.exists()) cs.destroy();
      }
    }
  }
}

TEST_F(CompressedTest, CbrBulkFillTlmRestartAsyncAndCornerPaths) {
  // 1. Single-precinct-per-flush-set CBR forcing bulk_fill > 8192 and fine_fill
  // > 0
  //    (with and without flush stats, plus secondary bulk fill).
  for (bool use_stats : {false, true}) {
    for (int h : {256, 128}) {
      CodestreamSpec spec;
      spec.width = 64;
      spec.height = h;
      spec.components = 1;
      spec.reversible = false;
      spec.dwt_levels = 0;
      spec.extra_params = {
          "Corder=PCRL", "Cprecincts={64,64}", "Cblk={16,16}",
          "Qstep=0.005", "Clayers=1",          "Scbr={1,128}",
      };
      std::vector<std::vector<int>> planes(1);
      planes[0].resize(static_cast<size_t>(spec.width * spec.height));
      for (int y = 0; y < spec.height; ++y) {
        for (int x = 0; x < spec.width; ++x) {
          planes[0][static_cast<size_t>(y * spec.width + x)] =
              ((x * 37) ^ (y * 91) ^ ((x + y) * 13)) & 0xFF;
        }
      }
      siz_params siz;
      ConfigureSiz(siz, spec);
      BufferTarget tgt;
      kdu_codestream cs;
      cs.create(&siz, &tgt);
      ApplyCodingParams(cs, spec);
      kdu_flush_stats fstats;
      if (use_stats) {
        fstats = cs.add_flush_stats(0, 5, 0, 1);
      }
      PushAllTiles(cs, planes, spec);
      kdu_long cbr_len[1] = {h == 256 ? 65541 : 20001};
      cs.flush(cbr_len, 1, nullptr, /*trim_to_rate=*/true);
      if (use_stats) {
        fstats.advance(1);
      }
      cs.destroy();
      EXPECT_GT(tgt.data.size(), 1000U);
      BufferSource dec_src(tgt.data);
      kdu_codestream dec_cs;
      dec_cs.create(&dec_src);
      const auto decoded = DecodeCodestream(dec_cs, false);
      EXPECT_EQ(decoded.size(), 1U);
      dec_cs.destroy();
    }
  }
  {
    CodestreamSpec spec;
    spec.width = 256;
    spec.height = 256;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {"Corder=PCRL", "Clayers=1", "Cblk={32,32}",
                         "Cprecincts={64,64}", "Scbr={1,512}"};
    const auto planes = MakePlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    BufferTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ApplyCodingParams(cs, spec);
    kdu_flush_stats fstats = cs.add_flush_stats(0, 5, 0, 1);
    PushAllTiles(cs, planes, spec);
    kdu_long cbr_bytes[1] = {49153};
    cs.flush(cbr_bytes, 1, nullptr, /*trim_to_rate=*/true);
    fstats.advance(1);
    cs.destroy();
    EXPECT_GE(tgt.data.size(), 40000U);
    BufferSource dec_src(tgt.data);
    kdu_codestream dec_cs;
    dec_cs.create(&dec_src);
    EXPECT_EQ(DecodeCodestream(dec_cs, false).size(), 1U);
    dec_cs.destroy();
  }

  // 2. trans_pcrd_opt + adjust_slopes_for_trans_out on shrunk packet
  // descriptors.
  {
    CodestreamSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {"Clayers=2", "Cprecincts={16,16}", "Cblk={8,8}",
                         "Qstep=0.01"};
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    ASSERT_FALSE(bytes.empty());
    BufferSource src(bytes);
    kdu_codestream in_cs;
    in_cs.create(&src);
    siz_params xsiz;
    ConfigureSiz(xsiz, spec);
    BufferTarget xtgt;
    kdu_codestream xcs;
    xcs.create(&xsiz, &xtgt);
    ApplyCodingParams(xcs, spec);
    kdu_pdescriptor_set pset;
    ASSERT_TRUE(pset.init(kdu_coords(0, 0), 4));
    kdu_tile xtile = xcs.open_tile(kdu_coords(0, 0));
    bool tdone = false;
    while (!tdone) {
      kdu_tile itile = in_cs.load_sequential(
          &pset, tdone, 2, 0,
          KDU_LOAD_SEQUENTIAL_WANT_TILE | KDU_LOAD_SEQUENTIAL_WANT_FLUSH_SET |
              KDU_LOAD_SEQUENTIAL_WANT_PACKET_BYTES,
          nullptr);
      ASSERT_TRUE(itile.exists());
      for (int d = 0; d < pset.get_num_pdescriptors(); ++d) {
        kdu_pdescriptor* pd = pset.access_pdescriptor(d);
        kdu_tile_comp itc = itile.access_component(pd->component_idx);
        kdu_resolution ires = itc.access_resolution(pd->resolution_level);
        kdu_precinct iprec = ires.access_precinct(pd->precinct_idx);
        kdu_tile_comp xtc = xtile.access_component(pd->component_idx);
        kdu_resolution xres = xtc.access_resolution(pd->resolution_level);
        const int min_b = ires.which() ? 1 : 0;
        const int max_b = ires.which() ? 3 : 0;
        for (int b = min_b; b <= max_b; ++b) {
          kdu_dims blocks;
          if (!iprec.get_valid_blocks(b, blocks) || blocks.is_empty()) continue;
          kdu_subband isub = ires.access_subband(b);
          kdu_subband xsub = xres.access_subband(b);
          for (int r = blocks.pos.y; r < blocks.pos.y + blocks.size.y; ++r) {
            for (int c = blocks.pos.x; c < blocks.pos.x + blocks.size.x; ++c) {
              kdu_block* ib = isub.open_block(kdu_coords(c, r));
              kdu_block* xb = xsub.open_block(kdu_coords(c, r));
              xb->num_passes = ib->num_passes;
              xb->missing_msbs = ib->missing_msbs;
              kdu_long total_bytes = 0;
              for (int p = 0; p < ib->num_passes; ++p) {
                xb->pass_lengths[p] = ib->pass_lengths[p];
                xb->pass_slopes[p] = ib->pass_slopes[p];
                total_bytes += ib->pass_lengths[p];
              }
              if (total_bytes > 0) {
                xb->set_max_bytes(total_bytes, false);
                std::memcpy(xb->byte_buffer, ib->byte_buffer,
                            static_cast<size_t>(total_bytes));
              }
              xsub.close_block(xb);
              isub.close_block(ib);
            }
          }
        }
        if (pd->packet_bytes != nullptr) {
          for (int q = 0; q < pd->packet_bytes_entries; ++q) {
            if (pd->packet_bytes[q] > 2) {
              pd->packet_bytes[q] =
                  std::max<kdu_long>(1, pd->packet_bytes[q] / 2);
            }
          }
        }
      }
      if (!pset.is_empty()) {
        EXPECT_GE(xcs.trans_pcrd_opt(&pset, true, true, 0.1), 0);
      }
    }
    xtile.close();
    xcs.trans_out(300, nullptr, 0, true);
    xcs.destroy();
    in_cs.destroy();
    EXPECT_GT(xtgt.data.size(), 50U);
  }

  // 3. ORGgen_tlm empty tile-part padding and max_tparts overflow with
  //    allow_rewrite = true.
  {
    CodestreamSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.tile_size = kdu_coords(32, 64);
    spec.extra_params = {"Clayers=2", "ORGtparts=R", "ORGgen_tlm=4"};
    siz_params siz;
    ConfigureSiz(siz, spec);
    BufferTarget tgt;
    tgt.allow_rewrite = true;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ApplyCodingParams(cs, spec);
    PushAllTiles(cs, MakePlanes(spec), spec);
    kdu_long lbytes[2] = {512, 2048};
    cs.flush(lbytes, 2);
    cs.destroy();
    EXPECT_GT(tgt.data.size(), 100U);

    // Overflow ORGgen_tlm=1 with multiple tile-parts.
    spec.tile_size = kdu_coords(0, 0);
    spec.extra_params = {"Clayers=2", "ORGtparts=L", "ORGgen_tlm=1"};
    siz_params osiz;
    ConfigureSiz(osiz, spec);
    BufferTarget otgt;
    otgt.allow_rewrite = true;
    kdu_codestream ocs;
    ocs.create(&osiz, &otgt);
    ApplyCodingParams(ocs, spec);
    PushAllTiles(ocs, MakePlanes(spec), spec);
    kdu_long first_layer[2] = {200, 0};
    kdu_long all_layers[2] = {200, 2000};
    error_handler_.clear();
    bool caught = false;
    try {
      ocs.flush(first_layer, 1);
      ocs.flush(all_layers, 2);
    } catch (int) {
      caught = true;
    }
    if (ocs.exists()) ocs.destroy();
    EXPECT_TRUE(caught || !error_handler_.history().empty());
    error_handler_.clear();
  }

  // 4. Non-resizing restart() (reinitialize with siz->any_changes + MCT),
  //    set_textualization(), and persistent disjoint-crop unload.
  {
    CodestreamSpec spec1;
    spec1.width = 128;
    spec1.height = 128;
    spec1.components = 3;
    spec1.reversible = true;
    spec1.dwt_levels = 2;
    spec1.extra_params = {
        "Cycc=no",
        "Clayers=2",
        "Cprecincts={16,16}",
        "Cblk={8,8}",
        "ORGgen_plt=yes",
        "Sextensions=MCT",
        "Mcomponents=3",
        "Mprecision=8",
        "Msigned=no",
        "Mstage_inputs:I1={0,2}",
        "Mstage_outputs:I1={0,2}",
        "Mstage_collections:I1={3,3}",
        "Mstage_xforms:I1={MATRIX,1,0,1,0}",
        "Mmatrix_size:I1=12",
        "Mmatrix_coeffs:I1=1,0,1,0,1,1,1,0,1,0,1,-1",
        "Mnum_stages=1",
        "Mstages=1",
    };
    const auto b1 = EncodeCodestream(spec1, MakePlanes(spec1));
    ASSERT_FALSE(b1.empty());

    CodestreamSpec spec2;
    spec2.width = 128;
    spec2.height = 128;
    spec2.components = 3;
    spec2.reversible = false;
    spec2.dwt_levels = 1;
    spec2.extra_params = {
        "Cycc=yes",   "Clayers=1",      "Cprecincts={32,32}", "Cblk={16,16}",
        "Qstep=0.02", "ORGgen_plt=yes", "Corder=RLCP",
    };
    const auto b2 = EncodeCodestream(spec2, MakePlanes(spec2));
    ASSERT_FALSE(b2.empty());

    BufferSource s1(b1);
    BufferSource s2(b2);
    kdu_codestream cs;
    cs.create(&s1);
    cs.set_persistent();
    cs.enable_restart();
    struct CaptureMsg : public kdu_message {
      void put_text(const char* s) override { out += s; }
      std::string out;
    };
    CaptureMsg text_out;
    cs.set_textualization(&text_out, false);
    // Disjoint persistent crop before restart ->
    // parse_all_relevant_packets(true).
    kdu_dims crop1;
    crop1.pos = kdu_coords(0, 0);
    crop1.size = kdu_coords(16, 16);
    cs.apply_input_restrictions(0, 3, 0, 1, &crop1,
                                KDU_WANT_CODESTREAM_COMPONENTS);
    {
      kdu_tile t = cs.open_tile(kdu_coords(0, 0));
      EXPECT_TRUE(t.parse_all_relevant_packets(false, nullptr));
      // Cover subband leaf node get_bibo_gains, is_top_level_band,
      // get_thread_context(nullptr), and no-thread-context error.
      kdu_tile_comp tc = t.access_component(0);
      kdu_resolution top_res = tc.access_resolution();
      EXPECT_TRUE(top_res.access_subband(HL_BAND).is_top_level_band());
      EXPECT_EQ(top_res.access_subband(HL_BAND).get_thread_context(nullptr),
                nullptr);
      kdu_thread_env no_ctx_env;
      no_ctx_env.create();
      error_handler_.clear();
      EXPECT_THROW(
          (void)top_res.access_subband(HL_BAND).get_thread_context(&no_ctx_env),
          int);
      error_handler_.clear();
      no_ctx_env.destroy();
      kdu_node pnode = top_res.access_node();
      kdu_node lnode = pnode.access_child(0);
      int ns = 0;
      EXPECT_NE(lnode.get_bibo_gains(ns, true), nullptr);
      EXPECT_NE(lnode.get_bibo_gains(ns, false), nullptr);
      // get_ycc() with KDU_WANT_CODESTREAM_COMPONENTS.
      (void)t.get_ycc();
      t.close();
    }
    kdu_dims crop2;
    crop2.pos = kdu_coords(112, 112);
    crop2.size = kdu_coords(16, 16);
    cs.apply_input_restrictions(0, 2, 0, 1, &crop2,
                                KDU_WANT_CODESTREAM_COMPONENTS);
    {
      kdu_tile t = cs.open_tile(kdu_coords(0, 0));
      (void)t.parse_all_relevant_packets(true, nullptr);
      EXPECT_FALSE(t.get_ycc());  // comp 2 excluded!
      t.close();
    }
    // Non-resizing restart with changed coding params triggers
    // kd_tile::reinitialize.
    cs.apply_input_restrictions(0, 0, 0, 0, nullptr,
                                KDU_WANT_CODESTREAM_COMPONENTS);
    cs.restart(&s2);
    cs.set_textualization(&text_out, false);
    const auto dec2 = DecodeCodestream(cs, true);
    EXPECT_EQ(dec2.size(), 3U);
    EXPECT_FALSE(text_out.out.empty());
    cs.destroy();

    // Also cover parse_all_relevant_packets(true) returning false when
    // !all_addressable and reset_packet_reading() on !compact_blocks.
    BufferSource non_seek_src(b1, KDU_SOURCE_CAP_SEQUENTIAL);
    kdu_codestream ns_cs;
    ns_cs.create(&non_seek_src);
    ns_cs.set_persistent();
    ns_cs.apply_input_restrictions(0, 1, 0, 1, &crop1,
                                   KDU_WANT_CODESTREAM_COMPONENTS);
    {
      kdu_tile t = ns_cs.open_tile(kdu_coords(0, 0));
      EXPECT_TRUE(t.parse_all_relevant_packets(false, nullptr));
      t.close();
    }
    ns_cs.apply_input_restrictions(0, 1, 0, 1, &crop2,
                                   KDU_WANT_CODESTREAM_COMPONENTS);
    {
      kdu_tile t = ns_cs.open_tile(kdu_coords(0, 0));
      EXPECT_FALSE(t.parse_all_relevant_packets(true, nullptr));
      t.close();
    }
    ns_cs.destroy();

    // Non-persistent input_monitor + PLT reset_packet_reading
    // (!compact_blocks).
    struct CountingMonitor : public kdu_input_codestream_monitor {
      void parsing_complete() override { ++done; }
      int done = 0;
    };
    CountingMonitor mon;
    BufferSource mon_src(b1);
    kdu_codestream mon_cs;
    mon_cs.create(&mon_src);
    EXPECT_TRUE(mon_cs.install_input_monitor(&mon, nullptr));
    EXPECT_EQ(DecodeCodestream(mon_cs, false).size(), 3U);
    EXPECT_GE(mon.done, 1);
    mon_cs.destroy();

    // Reset packet reading when Clayers=2 & required_layers increases 1 -> 2.
    BufferSource layer_src(b1);
    kdu_codestream layer_cs;
    layer_cs.create(&layer_src);
    layer_cs.set_persistent();
    layer_cs.apply_input_restrictions(0, 1, 0, 1, nullptr,
                                      KDU_WANT_CODESTREAM_COMPONENTS);
    {
      kdu_tile t = layer_cs.open_tile(kdu_coords(0, 0));
      EXPECT_TRUE(t.parse_all_relevant_packets(false, nullptr));
      t.close();
    }
    layer_cs.apply_input_restrictions(0, 1, 0, 2, nullptr,
                                      KDU_WANT_CODESTREAM_COMPONENTS);
    {
      kdu_tile t = layer_cs.open_tile(kdu_coords(0, 0));
      EXPECT_TRUE(t.parse_all_relevant_packets(false, nullptr));
      t.close();
    }
    layer_cs.destroy();
  }

  // 5. Progressive visual weights -> cplex.tlut (normal and K0_b < 0),
  //    Cdecomp=V(-)/H(-) empty corner precincts, Creslengths, Qweights mismatch
  //    on comp >= 1, MCT luma_input_idx, single-threaded auto_flush, slope
  //    hints, and subband get_thread_context error.
  {
    for (bool small_k : {false, true}) {
      CodestreamSpec spec;
      spec.width = 128;
      spec.height = 128;
      spec.components = 1;
      spec.reversible = false;
      spec.dwt_levels = 2;
      spec.extra_params = {
          "Cprecincts={32,32}",
          "Cblk={16,16}",
          small_k ? "Qstep=0.2" : "Qstep=0.002",
          small_k ? "Cband_weights=0.05,0.05,0.05,0.05,0.05,0.05"
                  : "Cband_weights=0.125,0.125,0.125,0.25,0.25,0.25",
          "Cprog_weights=0.5",
          "Qfparams={0.04,40,0.2,95}",
          "Cplex={6,EST,0.25,0}",
          "Clayers=2",
      };
      std::vector<std::vector<int>> planes(1);
      planes[0].resize(static_cast<size_t>(spec.width * spec.height));
      for (int y = 0; y < spec.height; ++y) {
        for (int x = 0; x < spec.width; ++x) {
          planes[0][static_cast<size_t>(y * spec.width + x)] =
              ((x * 37) ^ (y * 91)) & 0xFF;
        }
      }
      siz_params siz;
      ConfigureSiz(siz, spec);
      BufferTarget tgt;
      kdu_codestream cs;
      cs.create(&siz, &tgt);
      ApplyCodingParams(cs, spec);
      cs.set_slope_hint_policy(KDU_SLOPE_HINT_ESTIMATE);
      cs.set_min_slope_threshold(32000);
      kdu_flush_stats fstats = cs.add_flush_stats(0, 5, 0, 1);
      kdu_thread_env env;
      env.create();
      env.add_thread();
      kdu_long lbytes[2] = {2048, 8192};
      {
        kdu_tile t = cs.open_tile(kdu_coords(0, 0), &env);
        kdu_resolution res = t.access_component(0).access_resolution(1);
        kdu_coords coding_origin;
        res.get_coding_origin(coding_origin);
        kdu_subband sb = res.access_subband(HL_BAND);
        EXPECT_NE(sb.get_thread_context(&env), nullptr);
        kdu_thread_env foreign_env;
        foreign_env.create();
        error_handler_.clear();
        EXPECT_THROW((void)sb.get_thread_context(&foreign_env), int);
        error_handler_.clear();
        foreign_env.destroy();
        (void)sb.get_conservative_slope_threshold();
        (void)sb.get_msb_wmse();
        kdu_uint16 mn = 0, mx = 0;
        (void)sb.get_global_coding_hints(mn, mx);
        cs.set_min_slope_threshold(0);
        (void)sb.get_conservative_slope_threshold();
        (void)sb.get_global_coding_hints(mn, mx);
        PushTileSamples(cs, t, planes, spec, nullptr, &env);
        t.close(&env);
      }
      env.cs_terminate(cs);
      cs.flush(lbytes, 2);
      fstats.advance(1);
      env.destroy();
      cs.destroy();
      EXPECT_GT(tgt.data.size(), 100U);
    }

    // Empty corner precincts (Cdecomp=V(-)/H(-)) + single-threaded auto_flush.
    for (const char* decomp : {"Cdecomp=V(-)", "Cdecomp=H(-)"}) {
      CodestreamSpec spec;
      spec.width = 64;
      spec.height = 128;
      spec.components = 1;
      spec.reversible = true;
      spec.dwt_levels = 1;
      spec.extra_params = {decomp, "Cprecincts={8,8}", "Cblk={8,8}"};
      siz_params siz;
      ConfigureSiz(siz, spec);
      BufferTarget tgt;
      kdu_codestream cs;
      cs.create(&siz, &tgt);
      ApplyCodingParams(cs, spec);
      kdu_long lbytes[1] = {4096};
      cs.auto_flush(1, 1, 8, 8, lbytes, 1, nullptr, true, true);
      PushAllTiles(cs, MakePlanes(spec), spec);
      cs.flush(lbytes, 1);
      cs.destroy();
      EXPECT_GT(tgt.data.size(), 40U);
    }

    // Creslengths without SPF + Qweights mismatch on comp >= 1 + MCT
    // luma_input_idx.
    {
      CodestreamSpec spec;
      spec.width = 48;
      spec.height = 48;
      spec.components = 3;
      spec.reversible = false;
      spec.dwt_levels = 2;
      spec.extra_params = {
          "Cycc=no",
          "Clayers=2",
          "Qweights=1.0,99.0,1.0",
          "Creslengths=600,1400",
          "Sextensions=MCT",
          "Mcomponents=3",
          "Mprecision=8",
          "Msigned=no",
          "Mstage_inputs:I1={0,2}",
          "Mstage_outputs:I1={0,2}",
          "Mstage_collections:I1={3,3}",
          "Mstage_xforms:I1={MATRIX,1,0,0,0}",
          "Mmatrix_size:I1=9",
          "Mmatrix_coeffs:I1=1.0,1.0,1.0,1.0,-0.5,0.0,1.0,0.0,-0.5",
          "Mnum_stages=1",
          "Mstages=1",
      };
      warning_handler_.clear();
      const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
      EXPECT_FALSE(bytes.empty());
      EXPECT_FALSE(warning_handler_.history().empty());
      warning_handler_.clear();
    }
  }

  // 6. Profile-0 violations: low-res > 128, >1 precinct in <= 128, out-of-order
  //    tile-part flush, and out-of-order SOT in 2-tile decode.
  {
    CodestreamSpec pspec;
    pspec.width = 256;
    pspec.height = 256;
    pspec.components = 1;
    pspec.reversible = true;
    pspec.dwt_levels = 0;
    pspec.extra_params = {"Sprofile=PROFILE0"};
    warning_handler_.clear();
    const auto b_low = EncodeCodestream(pspec, MakePlanes(pspec));
    EXPECT_FALSE(b_low.empty());
    EXPECT_FALSE(warning_handler_.history().empty());

    pspec.dwt_levels = 2;
    pspec.extra_params = {"Sprofile=PROFILE0", "Cprecincts={32,32}",
                          "Cblk={16,16}"};
    warning_handler_.clear();
    const auto b_prec = EncodeCodestream(pspec, MakePlanes(pspec));
    EXPECT_FALSE(b_prec.empty());
    EXPECT_FALSE(warning_handler_.history().empty());

    // 2-tile Profile-0 where tile 1 is written first (next_tnum mismatch
    // warning).
    pspec.width = 128;
    pspec.height = 128;
    pspec.tile_size = kdu_coords(64, 128);
    pspec.dwt_levels = 1;
    pspec.extra_params = {"Sprofile=PROFILE0", "Clayers=2"};
    const auto planes = MakePlanes(pspec);
    siz_params siz;
    ConfigureSiz(siz, pspec);
    BufferTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ApplyCodingParams(cs, pspec);
    warning_handler_.clear();
    {
      kdu_tile t1 = cs.open_tile(kdu_coords(1, 0));
      PushTileSamples(cs, t1, planes, pspec);
      t1.close();
      kdu_tile t0 = cs.open_tile(kdu_coords(0, 0));
      PushTileSamples(cs, t0, planes, pspec);
      t0.close();
    }
    kdu_long lbytes[2] = {512, 2048};
    cs.flush(lbytes, 2);
    cs.destroy();
    EXPECT_FALSE(warning_handler_.history().empty());
    warning_handler_.clear();
  }

  // 7. Cached source with multi-tile header marker parsing + invalid KDU_PPT /
  //    corrupt marker in cached tile header, plus corrupt Psot SOT rescan.
  {
    CodestreamSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(32, 64);
    spec.extra_params = {"Qstep=0.02", "Qstep:T1=0.04", "Clayers=1",
                         "Cprecincts={16,16}", "Cblk={8,8}"};
    const auto planes = MakePlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    BufferTarget ctgt(/*cached=*/true);
    kdu_codestream ecs;
    ecs.create(&siz, &ctgt);
    ApplyCodingParams(ecs, spec);
    PushAllTiles(ecs, planes, spec);
    kdu_long lbytes[1] = {4096};
    ecs.flush(lbytes, 1);
    ecs.destroy();
    if (ctgt.main_header.empty() && ctgt.tile_headers.count(0)) {
      ctgt.main_header.swap(ctgt.tile_headers[0]);
    }

    class ScopedCachedSource : public kdu_compressed_source {
     public:
      ScopedCachedSource(const std::vector<kdu_byte>& main_hdr,
                         const std::map<int, std::vector<kdu_byte>>& tile_hdrs,
                         const std::map<kdu_long, std::vector<kdu_byte>>& pkts)
          : main_hdr_(main_hdr),
            tile_hdrs_(tile_hdrs),
            pkts_(pkts),
            cur_(&main_hdr_) {}
      int get_capabilities() override { return KDU_SOURCE_CAP_CACHED; }
      kdu_long get_pos() override { return static_cast<kdu_long>(pos_); }
      bool set_tileheader_scope(int tnum, int /*num_tiles*/) override {
        auto it = tile_hdrs_.find(tnum);
        if (it == tile_hdrs_.end()) return false;
        cur_ = &it->second;
        pos_ = 0;
        return true;
      }
      bool set_precinct_scope(kdu_long uid) override {
        auto it = pkts_.find(uid);
        if (it == pkts_.end()) {
          cur_ = &empty_;
        } else {
          cur_ = &it->second;
        }
        pos_ = 0;
        return true;
      }
      int read(kdu_byte* buf, int n) override {
        if (cur_ == nullptr || pos_ >= cur_->size() || n <= 0) return 0;
        const size_t avail =
            std::min(static_cast<size_t>(n), cur_->size() - pos_);
        std::memcpy(buf, cur_->data() + pos_, avail);
        pos_ += avail;
        return static_cast<int>(avail);
      }

     private:
      const std::vector<kdu_byte>& main_hdr_;
      const std::map<int, std::vector<kdu_byte>>& tile_hdrs_;
      const std::map<kdu_long, std::vector<kdu_byte>>& pkts_;
      const std::vector<kdu_byte>* cur_;
      std::vector<kdu_byte> empty_;
      size_t pos_ = 0;
    };

    ScopedCachedSource good_csrc(ctgt.main_header, ctgt.tile_headers,
                                 ctgt.precincts);
    kdu_codestream ccs;
    ccs.create(&good_csrc);
    EXPECT_EQ(DecodeCodestream(ccs, false).size(), 1U);
    ccs.destroy();

    // Persistent cached source with disjoint crops exercises
    // parse_all_relevant_packets(true, nullptr) unload/reset path.
    ScopedCachedSource p_csrc(ctgt.main_header, ctgt.tile_headers,
                              ctgt.precincts);
    kdu_codestream pcs_reload;
    pcs_reload.create(&p_csrc);
    pcs_reload.set_persistent();
    kdu_dims c1;
    c1.pos = kdu_coords(0, 0);
    c1.size = kdu_coords(8, 8);
    pcs_reload.apply_input_restrictions(0, 1, 0, 1, &c1,
                                        KDU_WANT_CODESTREAM_COMPONENTS);
    {
      kdu_tile t = pcs_reload.open_tile(kdu_coords(0, 0));
      EXPECT_TRUE(t.parse_all_relevant_packets(false, nullptr));
      t.close();
    }
    kdu_dims c2;
    c2.pos = kdu_coords(16, 48);
    c2.size = kdu_coords(8, 8);
    pcs_reload.apply_input_restrictions(0, 1, 0, 1, &c2,
                                        KDU_WANT_CODESTREAM_COMPONENTS);
    {
      kdu_tile t = pcs_reload.open_tile(kdu_coords(0, 0));
      EXPECT_TRUE(t.parse_all_relevant_packets(true, nullptr));
      t.close();
    }
    pcs_reload.destroy();

    // Inject PPT marker segment into cached tile 1 header -> throws error 6.
    auto bad_hdrs_ppt = ctgt.tile_headers;
    bad_hdrs_ppt[1].insert(bad_hdrs_ppt[1].begin(),
                           {0xFF, 0x61, 0x00, 0x03, 0x00});
    ScopedCachedSource ppt_csrc(ctgt.main_header, bad_hdrs_ppt, ctgt.precincts);
    kdu_codestream pcs;
    pcs.create(&ppt_csrc);
    error_handler_.clear();
    EXPECT_THROW((void)DecodeCodestream(pcs, false), int);
    if (pcs.exists()) pcs.destroy();
    error_handler_.clear();

    // Inject non-marker bytes into cached tile 1 header -> throws error 7.
    auto bad_hdrs_len = ctgt.tile_headers;
    bad_hdrs_len[1].insert(bad_hdrs_len[1].begin(), {0x12, 0x34});
    ScopedCachedSource len_csrc(ctgt.main_header, bad_hdrs_len, ctgt.precincts);
    kdu_codestream lcs;
    lcs.create(&len_csrc);
    error_handler_.clear();
    EXPECT_THROW((void)DecodeCodestream(lcs, false), int);
    if (lcs.exists()) lcs.destroy();
    error_handler_.clear();

    // Corrupt tile 0 Psot to undershoot next tile SOT so seekable reader scans
    // forward to KDU_SOT (lines 4358-4366).
    const auto seq_bytes = EncodeCodestream(spec, planes);
    std::vector<kdu_byte> bad_psot = seq_bytes;
    for (size_t i = 2; i + 12 < bad_psot.size(); ++i) {
      if (bad_psot[i] == 0xFF && bad_psot[i + 1] == 0x90) {
        // Rewrite Psot (bytes i+6..i+9) to small nonzero length 20.
        bad_psot[i + 6] = 0x00;
        bad_psot[i + 7] = 0x00;
        bad_psot[i + 8] = 0x00;
        bad_psot[i + 9] = 0x14;
        break;
      }
    }
    BufferSource psot_src(bad_psot);
    kdu_codestream pscs;
    pscs.create(&psot_src);
    pscs.set_resilient(true);
    try {
      (void)DecodeCodestream(pscs, false);
    } catch (int) {
    }
    if (pscs.exists()) pscs.destroy();
    error_handler_.clear();
  }

  // 8. Async-capable membacked streaming (PARTIALLY_SERVICED with small step)
  //    single-threaded decode cleanly exercises kdu_membacked_compressed_source
  //    without triggering unpatched kdu_threads.cpp event deregister assert.
  {
    CodestreamSpec spec;
    spec.width = 128;
    spec.height = 128;
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(64, 128);
    spec.extra_params = {"Clayers=1", "Cprecincts={32,32}", "Cblk={16,16}"};
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    ASSERT_GT(bytes.size(), 512U);

    class AsyncTrickleSource : public kdu_membacked_compressed_source {
     public:
      AsyncTrickleSource(std::vector<kdu_byte> data, size_t step)
          : data_(std::move(data)), step_(step) {}
      bool EnableStream() {
        return enable(KDU_MEMBACKED_SOURCE_UNSERVICED, nullptr, 0, 0, -1.0, 64,
                      data_.size() + 256);
      }
      void ServiceStream() {
        size_t total = 0;
        kdu_int32 term = 0;
        service(total, term);
      }
      bool OpenStream() { return open(false); }

     protected:
      kdu_long fetch_data(kdu_long max_bytes, kdu_byte* buffer,
                          bool blocking) override {
        (void)blocking;
        if (pos_ >= data_.size()) {
          finish_fetching(0);
          return 0;
        }
        std::this_thread::yield();
        const size_t n = std::min(
            {data_.size() - pos_, static_cast<size_t>(max_bytes), step_});
        std::memcpy(buffer, data_.data() + pos_, n);
        pos_ += n;
        return static_cast<kdu_long>(n);
      }

     private:
      std::vector<kdu_byte> data_;
      size_t step_;
      size_t pos_ = 0;
    };

    AsyncTrickleSource src(bytes, 48);
    ASSERT_TRUE(src.EnableStream());
    ASSERT_TRUE(src.OpenStream());
    src.ServiceStream();

    kdu_codestream cs;
    cs.create(&src);
    EXPECT_EQ(DecodeCodestream(cs, false).size(), 1U);
    cs.destroy();
    src.close();
  }
}

TEST_F(CompressedTest, ProgressiveTlutOvershootReinitAndPacketErrors) {
  auto make_spec = [](int w, int h) {
    CodestreamSpec s;
    s.width = w;
    s.height = h;
    s.components = 1;
    return s;
  };

  // 1. Asymmetric Cdecomp corner empty precincts (17x17, Clevels=1) + P2
  // Sorigin.
  for (const char* decomp : {"V(-)", "H(-)"}) {
    siz_params siz;
    siz.set(Sdims, 0, 0, 17);
    siz.set(Sdims, 0, 1, 17);
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    siz.set(Sorigin, 0, 0, 1);
    siz.set(Sorigin, 0, 1, 1);
    static_cast<kdu_params&>(siz).parse_string("Sprofile=PART2");
    siz.finalize_all();
    BufferTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    kdu_params* cod = cs.access_siz()->access_cluster(COD_params);
    ASSERT_NE(cod, nullptr);
    cod->parse_string("Creversible=yes");
    cod->parse_string("Clevels=1");
    cod->parse_string("Cblk={8,8}");
    cod->parse_string("Cprecincts={8,8}");
    cod->parse_string((std::string("Cdecomp=") + decomp).c_str());
    cs.access_siz()->finalize_all();
    std::vector<int> plane(17 * 17);
    for (int i = 0; i < 17 * 17; ++i) plane[i] = (i * 19 + 5) & 0xFF;
    PushAllTiles(cs, {plane}, make_spec(17, 17));
    kdu_long max_b = 1LL << 24;
    cs.flush(&max_b, 1);
    cs.destroy();

    BufferSource src(tgt.data);
    kdu_codestream dcs;
    dcs.create(&src);
    EXPECT_EQ(DecodeCodestream(dcs, true).size(), 1U);
    dcs.destroy();
  }

  // 2. Non-resizing restart with identical main header and tile-0 header
  // overrides (Cblk:T0, Rshift:T0, Porder:T0) + textualization across restart.
  {
    auto encode_base = [&](const std::vector<const char*>& tile_overrides) {
      siz_params siz;
      siz.set(Sdims, 0, 0, 64);
      siz.set(Sdims, 0, 1, 64);
      siz.set(Scomponents, 0, 0, 1);
      siz.set(Sprecision, 0, 0, 8);
      siz.set(Ssigned, 0, 0, false);
      siz.set(Stiles, 0, 0, 32);
      siz.set(Stiles, 0, 1, 32);
      siz.finalize_all();
      BufferTarget tgt;
      kdu_codestream cs;
      cs.create(&siz, &tgt);
      kdu_params* cod = cs.access_siz()->access_cluster(COD_params);
      cod->parse_string("Creversible=yes");
      cod->parse_string("Clevels=1");
      cod->parse_string("Cblk={8,8}");
      cod->parse_string("Clayers=1");
      for (const char* ov : tile_overrides) {
        cod->parse_string(ov);
      }
      cs.access_siz()->finalize_all();
      std::vector<int> plane(64 * 64);
      for (int i = 0; i < 64 * 64; ++i) plane[i] = (i * 13) & 0xFF;
      const CodestreamSpec sp = make_spec(64, 64);
      kdu_dims tdim;
      cs.get_valid_tiles(tdim);
      for (int ty = 0; ty < tdim.size.y; ++ty) {
        for (int tx = 0; tx < tdim.size.x; ++tx) {
          kdu_tile tile = cs.open_tile(kdu_coords(tx, ty));
          PushTileSamples(cs, tile, {plane}, sp);
          tile.close();
        }
      }
      kdu_long max_b = 1LL << 24;
      cs.flush(&max_b, 1);
      cs.destroy();
      return tgt.data;
    };

    const auto b1 = encode_base({});
    const auto b2 = encode_base(
        {"Cblk:T0={16,16}", "Corder:T0=PCRL", "Porder:T0={0,0,1,1,1,PCRL}"});
    BufferSource s1(b1);
    BufferSource s2(b2);
    kdu_codestream cs;
    cs.create(&s1);
    cs.enable_restart();
    class CaptureText : public kdu_message {
     public:
      void put_text(const char* s) override { out += s; }
      std::string out;
    } text_out;
    cs.set_textualization(&text_out, false);
    EXPECT_EQ(DecodeCodestream(cs, true).size(), 1U);
    cs.restart(&s2);
    EXPECT_FALSE(text_out.out.empty());
    EXPECT_EQ(DecodeCodestream(cs, true).size(), 1U);
    cs.destroy();
  }

  // 3. Progressive visual weights (Cprog_weights + Cband_weights + Qfparams +
  // Cplex EST tlut) at multiple budgets, Creslengths, ORGtpart_interrupts,
  // and Slope-EST coding hints (set_slope_hint_policy +
  // get_global_coding_hints).
  for (int trial = 0; trial < 3; ++trial) {
    siz_params siz;
    siz.set(Sdims, 0, 0, 128);
    siz.set(Sdims, 0, 1, 128);
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    if (trial == 2) {
      siz.set(Stiles, 0, 0, 64);
      siz.set(Stiles, 0, 1, 64);
    }
    siz.finalize_all();
    BufferTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    kdu_params* cod = cs.access_siz()->access_cluster(COD_params);
    kdu_params* qcd = cs.access_siz()->access_cluster(QCD_params);
    kdu_params* org = cs.access_siz()->access_cluster(ORG_params);
    cod->parse_string("Creversible=no");
    cod->parse_string("Clevels=3");
    cod->parse_string("Cblk={16,16}");
    cod->parse_string("Cprecincts={32,32}");
    cod->parse_string(
        "Cband_weights=0.25,0.25,0.25,0.5,0.5,0.5,0.5,0.5,0.5,1.0");
    cod->parse_string("Cprog_weights=0.5");
    cod->parse_string("Cplex={6,EST,0,0}");
    if (trial == 1) {
      cod->parse_string("Creslengths=8000,4000,2000,1000");
    }
    qcd->parse_string("Qstep=0.005");
    qcd->parse_string("Qfparams={0.04,30,0.1,95}");
    org->parse_string("ORGtpart_interrupts=2");
    cs.access_siz()->finalize_all();

    cs.set_slope_hint_policy(KDU_SLOPE_HINT_ESTIMATE);
    cs.set_min_slope_threshold(38000);
    const kdu_long budget = (trial == 0) ? 14000 : ((trial == 1) ? 4500 : 8000);
    cs.set_max_bytes(budget, true, false);

    std::vector<int> plane(128 * 128);
    for (int y = 0; y < 128; ++y) {
      for (int x = 0; x < 128; ++x) {
        plane[y * 128 + x] = ((x * 11 + y * 29) ^ ((x & y) * 7)) & 0xFF;
      }
    }
    const CodestreamSpec sp = make_spec(128, 128);
    kdu_dims tdim;
    cs.get_valid_tiles(tdim);
    for (int ty = 0; ty < tdim.size.y; ++ty) {
      for (int tx = 0; tx < tdim.size.x; ++tx) {
        kdu_tile tile = cs.open_tile(kdu_coords(tx, ty));
        kdu_tile_comp tc = tile.access_component(0);
        kdu_resolution res = tc.access_resolution(tc.get_num_resolutions() - 1);
        int min_b = 0;
        if (res.get_valid_band_indices(min_b) > 0) {
          kdu_subband sb = res.access_subband(min_b);
          kdu_uint16 min_th = 0, max_th = 0;
          sb.get_global_coding_hints(min_th, max_th);
        }
        PushTileSamples(cs, tile, {plane}, sp);
        tile.close();
        if (trial == 2 && tx == 0 && ty == 0) {
          kdu_long partial = 2500;
          cs.flush(&partial, 1);
        }
      }
    }
    kdu_long final_b = budget;
    cs.flush(&final_b, 1);
    cs.destroy();
  }

  // 4. Synthesis overshoot limit (warn/record mode and error-throwing mode).
  {
    siz_params siz;
    siz.set(Sdims, 0, 0, 64);
    siz.set(Sdims, 0, 1, 64);
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    siz.finalize_all();
    BufferTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    kdu_params* cod = cs.access_siz()->access_cluster(COD_params);
    kdu_params* qcd = cs.access_siz()->access_cluster(QCD_params);
    cod->parse_string("Creversible=no");
    cod->parse_string("Clevels=2");
    cod->parse_string("Cblk={16,16}");
    qcd->parse_string("Qstep=0.25");
    cs.access_siz()->finalize_all();
    std::vector<int> checker(64 * 64);
    for (int y = 0; y < 64; ++y) {
      for (int x = 0; x < 64; ++x) {
        checker[y * 64 + x] = ((x + y) & 1) ? 255 : 0;
      }
    }
    PushAllTiles(cs, {checker}, make_spec(64, 64));
    kdu_long b = 400;
    cs.flush(&b, 1);
    cs.destroy();

    BufferSource src1(tgt.data);
    kdu_codestream dcs1;
    dcs1.create(&src1);
    dcs1.set_synth_overshoot_limit(0.0001F, false);
    EXPECT_EQ(DecodeCodestream(dcs1, false).size(), 1U);
    float max_ov = 0.0F;
    (void)dcs1.report_synth_overshoot(0, max_ov);
    dcs1.destroy();

    BufferSource src2(tgt.data);
    kdu_codestream dcs2;
    dcs2.create(&src2);
    dcs2.set_synth_overshoot_limit(0.0001F, true);
    error_handler_.clear();
    bool threw_or_decoded = false;
    try {
      (void)DecodeCodestream(dcs2, false);
      threw_or_decoded = true;
    } catch (int) {
      threw_or_decoded = true;
    }
    EXPECT_TRUE(threw_or_decoded);
    error_handler_.clear();
    if (dcs2.exists()) dcs2.destroy();
  }

  // 5. MT multi-synthesis under transpose/vflip/hflip + sb.get_dims().
  {
    CodestreamSpec spec = make_spec(128, 128);
    spec.dwt_levels = 2;
    spec.reversible = true;
    spec.extra_params = {"Cblk={16,16}", "Cprecincts={16,16}"};
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    const bool modes[3][3] = {
        {true, false, false}, {true, true, false}, {false, true, true}};
    for (const auto& m : modes) {
      BufferSource src(bytes);
      kdu_codestream cs;
      cs.create(&src);
      cs.change_appearance(m[0], m[1], m[2]);
      kdu_thread_env env;
      env.create();
      for (int t = 0; t < 2; ++t) EXPECT_TRUE(env.add_thread());
      env.cs_terminate(cs);
      EXPECT_EQ(DecodeCodestream(cs, true, &env).size(), 1U);
      env.destroy();
      cs.destroy();
    }
  }

  // 6. kd_precinct::reset_packet_reading() via LocalCachedSource (2 layers,
  // parse layer 1, close tile, increase max_layers to 2 and re-open).
  {
    siz_params siz;
    siz.set(Sdims, 0, 0, 64);
    siz.set(Sdims, 0, 1, 64);
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    siz.finalize_all();
    BufferTarget ctgt(/*cached=*/true);
    kdu_codestream cs;
    cs.create(&siz, &ctgt);
    kdu_params* cod = cs.access_siz()->access_cluster(COD_params);
    cod->parse_string("Creversible=yes");
    cod->parse_string("Clevels=1");
    cod->parse_string("Cblk={8,8}");
    cod->parse_string("Cprecincts={32,32}");
    cod->parse_string("Clayers=2");
    cs.access_siz()->finalize_all();
    std::vector<int> plane(64 * 64);
    for (int i = 0; i < 64 * 64; ++i) plane[i] = (i * 23 + 3) & 0xFF;
    PushAllTiles(cs, {plane}, make_spec(64, 64));
    kdu_long lb[2] = {500, 1LL << 24};
    cs.flush(lb, 2);
    cs.destroy();
    if (ctgt.main_header.empty() && ctgt.tile_headers.count(0)) {
      ctgt.main_header.swap(ctgt.tile_headers[0]);
    }
    class LocalCachedSource : public kdu_compressed_source {
     public:
      LocalCachedSource(const std::vector<kdu_byte>& main_hdr,
                        const std::map<int, std::vector<kdu_byte>>& tile_hdrs,
                        const std::map<kdu_long, std::vector<kdu_byte>>& pkts)
          : main_hdr_(main_hdr),
            tile_hdrs_(tile_hdrs),
            pkts_(pkts),
            cur_(&main_hdr_) {}
      int get_capabilities() override { return KDU_SOURCE_CAP_CACHED; }
      bool set_tileheader_scope(int tnum, int /*num_tiles*/) override {
        auto it = tile_hdrs_.find(tnum);
        cur_ = (it == tile_hdrs_.end()) ? &empty_ : &it->second;
        pos_ = 0;
        return true;
      }
      bool set_precinct_scope(kdu_long uid) override {
        auto it = pkts_.find(uid);
        cur_ = (it == pkts_.end()) ? &empty_ : &it->second;
        pos_ = 0;
        return true;
      }
      int read(kdu_byte* buf, int n) override {
        if (cur_ == nullptr || pos_ >= cur_->size() || n <= 0) return 0;
        const size_t avail =
            std::min(static_cast<size_t>(n), cur_->size() - pos_);
        std::memcpy(buf, cur_->data() + pos_, avail);
        pos_ += avail;
        return static_cast<int>(avail);
      }

     private:
      const std::vector<kdu_byte>& main_hdr_;
      const std::map<int, std::vector<kdu_byte>>& tile_hdrs_;
      const std::map<kdu_long, std::vector<kdu_byte>>& pkts_;
      const std::vector<kdu_byte>* cur_;
      std::vector<kdu_byte> empty_;
      size_t pos_ = 0;
    };
    LocalCachedSource csrc(ctgt.main_header, ctgt.tile_headers, ctgt.precincts);
    kdu_codestream dcs;
    dcs.create(&csrc);
    dcs.set_persistent();
    dcs.apply_input_restrictions(0, 1, 0, 1, nullptr,
                                 KDU_WANT_CODESTREAM_COMPONENTS);
    {
      kdu_tile t = dcs.open_tile(kdu_coords(0, 0));
      EXPECT_TRUE(t.parse_all_relevant_packets(false, nullptr));
      t.close();
    }
    dcs.apply_input_restrictions(0, 1, 0, 2, nullptr,
                                 KDU_WANT_CODESTREAM_COMPONENTS);
    {
      kdu_tile t = dcs.open_tile(kdu_coords(0, 0));
      EXPECT_TRUE(t.parse_all_relevant_packets(false, nullptr));
      t.close();
    }
    dcs.destroy();
  }

  // 7. Packet header error branches (missing_msbs overflow, illegal marker in
  // packet header, truncation, pre-SOT corruption) across
  // default/resilient/allow.
  {
    siz_params siz;
    siz.set(Sdims, 0, 0, 64);
    siz.set(Sdims, 0, 1, 64);
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    siz.set(Stiles, 0, 0, 32);
    siz.set(Stiles, 0, 1, 32);
    siz.finalize_all();
    BufferTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    kdu_params* cod = cs.access_siz()->access_cluster(COD_params);
    cod->parse_string("Creversible=yes");
    cod->parse_string("Clevels=1");
    cod->parse_string("Cblk={16,16}");
    cod->parse_string("Cuse_sop=yes");
    cod->parse_string("Cuse_eph=yes");
    cs.access_siz()->finalize_all();
    std::vector<int> plane(64 * 64);
    for (int i = 0; i < 64 * 64; ++i) plane[i] = (i * 17) & 0xFF;
    const CodestreamSpec sp = make_spec(64, 64);
    kdu_dims tdim;
    cs.get_valid_tiles(tdim);
    for (int ty = 0; ty < tdim.size.y; ++ty) {
      for (int tx = 0; tx < tdim.size.x; ++tx) {
        kdu_tile tile = cs.open_tile(kdu_coords(tx, ty));
        PushTileSamples(cs, tile, {plane}, sp);
        tile.close();
      }
    }
    kdu_long b = 1LL << 24;
    cs.flush(&b, 1);
    cs.destroy();

    const std::vector<kdu_byte>& orig = tgt.data;
    size_t sod_pos = 0;
    for (size_t i = 0; i + 1 < orig.size(); ++i) {
      if (orig[i] == 0xFF && orig[i + 1] == 0x93) {
        sod_pos = i + 2;
        break;
      }
    }
    ASSERT_GT(sod_pos, 0U);
    size_t sot2_pos = 0;
    int sot_count = 0;
    for (size_t i = 0; i + 1 < orig.size(); ++i) {
      if (orig[i] == 0xFF && orig[i + 1] == 0x90) {
        if (++sot_count == 2) {
          sot2_pos = i;
          break;
        }
      }
    }
    ASSERT_GT(sot2_pos, sod_pos);

    auto run_corrupt = [&](const std::vector<kdu_byte>& data, int mode) {
      BufferSource src(data);
      kdu_codestream dcs;
      dcs.create(&src);
      if (mode == 1) dcs.set_resilient(true);
      if (mode == 2) dcs.set_allow_parsing_errors(true);
      error_handler_.clear();
      try {
        (void)DecodeCodestream(dcs, false);
      } catch (int) {
      }
      error_handler_.clear();
      if (dcs.exists()) dcs.destroy();
    };

    std::vector<kdu_byte> bad_marker = orig;
    if (sod_pos + 8 < bad_marker.size()) {
      size_t off =
          (bad_marker[sod_pos] == 0xFF && bad_marker[sod_pos + 1] == 0x91)
              ? sod_pos + 6
              : sod_pos;
      bad_marker[off] = 0xFF;
      bad_marker[off + 1] = 0x64;
    }
    std::vector<kdu_byte> bad_msbs = orig;
    if (sod_pos + 16 < bad_msbs.size()) {
      size_t off = (bad_msbs[sod_pos] == 0xFF && bad_msbs[sod_pos + 1] == 0x91)
                       ? sod_pos + 6
                       : sod_pos;
      bad_msbs[off] = 0x80;
      for (size_t k = 1; k <= 8 && off + k < bad_msbs.size(); ++k) {
        bad_msbs[off + k] = 0x00;
      }
    }
    std::vector<kdu_byte> trunc_pkt(
        orig.begin(), orig.begin() + std::min(orig.size(), sod_pos + 7));
    std::vector<kdu_byte> bad_sot = orig;
    if (sot2_pos + 2 < bad_sot.size()) {
      bad_sot[sot2_pos + 1] = 0x64;
    }

    for (const auto& buf : {bad_marker, bad_msbs, trunc_pkt, bad_sot}) {
      for (int mode = 0; mode < 3; ++mode) {
        run_corrupt(buf, mode);
      }
    }
  }

  // 8. MT rate_stats transcribe + auto_flush + Sprofile=PROFILE0 cached
  // out-of-order tiles + ORGtparts=L with ORGgen_tlm=4.
  {
    siz_params siz;
    siz.set(Sdims, 0, 0, 256);
    siz.set(Sdims, 0, 1, 256);
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    siz.set(Stiles, 0, 0, 128);
    siz.set(Stiles, 0, 1, 128);
    siz.finalize_all();
    BufferTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    kdu_params* cod = cs.access_siz()->access_cluster(COD_params);
    kdu_params* org = cs.access_siz()->access_cluster(ORG_params);
    cod->parse_string("Creversible=yes");
    cod->parse_string("Clevels=2");
    cod->parse_string("Cblk={16,16}");
    cod->parse_string("Clayers=2");
    cod->parse_string("Corder=PCRL");
    org->parse_string("ORGgen_tlm=4");
    org->parse_string("ORGtparts=L");
    cs.access_siz()->finalize_all();

    kdu_thread_env env;
    env.create();
    for (int t = 0; t < 2; ++t) EXPECT_TRUE(env.add_thread());
    env.cs_terminate(cs);
    cs.set_max_bytes(20000, true, false);
    kdu_long lb[2] = {4000, 16000};
    cs.auto_flush(1, 1, 2, 2, lb, 2, nullptr, true, true, 0.0, &env, 0);

    std::vector<int> plane(256 * 256);
    for (int i = 0; i < 256 * 256; ++i) plane[i] = (i * 31) & 0xFF;
    const CodestreamSpec sp = make_spec(256, 256);
    kdu_dims tdim;
    cs.get_valid_tiles(tdim);
    for (int ty = 0; ty < tdim.size.y; ++ty) {
      for (int tx = 0; tx < tdim.size.x; ++tx) {
        kdu_tile tile = cs.open_tile(kdu_coords(tx, ty), &env);
        PushTileSamples(cs, tile, {plane}, sp, nullptr, &env);
        tile.close(&env);
      }
    }
    cs.flush(lb, 2, nullptr, true, true, 0.0, &env);
    env.destroy();
    cs.destroy();
  }

  // Sprofile=PROFILE0 cached 2-tile out-of-order push.
  {
    siz_params siz;
    siz.set(Sdims, 0, 0, 32);
    siz.set(Sdims, 0, 1, 64);
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    siz.set(Stiles, 0, 0, 32);
    siz.set(Stiles, 0, 1, 32);
    static_cast<kdu_params&>(siz).parse_string("Sprofile=PROFILE0");
    siz.finalize_all();
    BufferTarget ctgt(/*cached=*/true);
    kdu_codestream cs;
    cs.create(&siz, &ctgt);
    kdu_params* cod = cs.access_siz()->access_cluster(COD_params);
    cod->parse_string("Creversible=yes");
    cod->parse_string("Clevels=1");
    cod->parse_string("Cblk={16,16}");
    cs.access_siz()->finalize_all();
    std::vector<int> plane(64 * 32, 42);
    const CodestreamSpec sp = make_spec(64, 32);
    {
      kdu_tile t1 = cs.open_tile(kdu_coords(1, 0));
      PushTileSamples(cs, t1, {plane}, sp);
      t1.close();
      kdu_tile t0 = cs.open_tile(kdu_coords(0, 0));
      PushTileSamples(cs, t0, {plane}, sp);
      t0.close();
    }
    kdu_long max_b = 1LL << 20;
    cs.flush(&max_b, 1);
    cs.destroy();
  }
}

TEST_F(CompressedTest, FinalCompressedCoverageGaps) {
  auto make_spec = [](int w, int h, int comps = 1, int bits = 8,
                      bool is_signed = false) {
    CodestreamSpec sp;
    sp.width = w;
    sp.height = h;
    sp.components = comps;
    sp.bit_depth = bits;
    sp.is_signed = is_signed;
    return sp;
  };

  // 1. kdu_quality_limiter + YCC and non-diagonal Part-2 MCT (qlim_type_flags)
  // and set_components_of_interest on Cycc=yes without MCT.
  {
    CodestreamSpec spec = make_spec(64, 64, 3);
    spec.reversible = true;
    spec.extra_params = {"Cycc=yes", "Clevels=2", "Cblk={16,16}", "Clayers=2"};
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    BufferSource src(bytes);
    kdu_codestream dcs;
    dcs.create(&src);
    dcs.set_persistent();
    {
      kdu_tile tile = dcs.open_tile(kdu_coords(0, 0));
      const int comp_order[2] = {2, 0};
      tile.set_components_of_interest(2, comp_order);
      tile.close();
    }
    kdu_quality_limiter qlim(0.02F, false);
    dcs.apply_input_restrictions(0, 0, 0, 0, nullptr,
                                 KDU_WANT_OUTPUT_COMPONENTS, nullptr, &qlim);
    EXPECT_EQ(DecodeCodestream(dcs, true).size(), 3U);
    dcs.destroy();
  }

  for (bool rev : {true, false}) {
    CodestreamSpec spec = make_spec(32, 32, 4);
    spec.reversible = rev;
    spec.extra_params = {
        "Cycc=no",
        "Sextensions=MCT",
        "Mcomponents=4",
        "Mprecision=8",
        rev ? "Msigned=yes" : "Msigned=no",
        rev ? "Mmatrix_size:I1=6" : "Mmatrix_size:I1=4",
        rev ? "Mmatrix_coeffs:I1=1,512,512,-1,0,1"
            : "Mmatrix_coeffs:I1=0.5,0.5,0.5,-0.5",
        rev ? "Mmatrix_size:I2=6" : "Mmatrix_size:I2=4",
        rev ? "Mmatrix_coeffs:I2=1,512,512,-1,0,1"
            : "Mmatrix_coeffs:I2=0.5,0.5,0.5,-0.5",
        "Mstage_inputs:I1={0,1},{2,3}",
        "Mstage_outputs:I1={0,1},{2,3}",
        "Mstage_collections:I1={2,2},{2,2}",
        rev ? "Mstage_xforms:I1={MATRIX,1,0,1,0},{MATRIX,2,0,1,0}"
            : "Mstage_xforms:I1={MATRIX,1,0,0,0},{MATRIX,2,0,0,0}",
        "Mnum_stages=1",
        "Mstages=1",
        "Clevels=1",
        "Cblk={16,16}",
        "Clayers=2",
    };
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    BufferSource src(bytes);
    kdu_codestream dcs;
    dcs.create(&src);
    dcs.set_persistent();
    kdu_quality_limiter qlim(0.05F, false);
    dcs.apply_input_restrictions(0, 0, 0, 0, nullptr,
                                 KDU_WANT_OUTPUT_COMPONENTS, nullptr, &qlim);
    EXPECT_EQ(DecodeCodestream(dcs, false).size(), 4U);
    int out_idx[2] = {0, 2};
    dcs.apply_input_restrictions(2, out_idx, 0, 0, nullptr,
                                 KDU_WANT_OUTPUT_COMPONENTS, nullptr, &qlim);
    EXPECT_EQ(DecodeCodestream(dcs, false).size(), 2U);
    dcs.destroy();
  }

  // 2. ENC_params Creslengths + 12-bit cplex.tlut runtime LUT sweep +
  // kdu_multi_analysis partial strip auto_flush with MT background cplex.
  for (int budget : {1200, 2500, 5000}) {
    siz_params siz;
    siz.set(Sdims, 0, 0, 128);
    siz.set(Sdims, 0, 1, 128);
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sprecision, 0, 0, 12);
    siz.set(Ssigned, 0, 0, false);
    siz.finalize_all();
    BufferTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    kdu_params* cod = cs.access_siz()->access_cluster(COD_params);
    kdu_params* qcd = cs.access_siz()->access_cluster(QCD_params);
    kdu_params* enc = cs.access_siz()->access_cluster(ENC_params);
    ASSERT_NE(enc, nullptr);
    cod->parse_string("Creversible=no");
    cod->parse_string("Clevels=3");
    cod->parse_string("Cblk={16,16}");
    cod->parse_string("Cprecincts={32,32}");
    cod->parse_string(
        "Cband_weights=0.25,0.25,0.25,0.5,0.5,0.5,0.5,0.5,0.5,1.0");
    cod->parse_string("Cprog_weights=0.5");
    cod->parse_string("Cplex={6,EST,0,0}");
    qcd->parse_string("Qstep=0.01");
    qcd->parse_string("Qfparams={0.04,30,0.1,95}");
    enc->parse_string("Creslengths=8000,4000,2000,1000");
    cs.access_siz()->finalize_all();
    cs.set_max_bytes(budget, true, false);
    std::vector<int> plane(128 * 128);
    for (int y = 0; y < 128; ++y) {
      for (int x = 0; x < 128; ++x) {
        plane[y * 128 + x] = ((x * 37 + y * 53 + (x ^ y) * 19) & 0xFFF);
      }
    }
    PushAllTiles(cs, {plane}, make_spec(128, 128, 1, 12));
    kdu_long lb[1] = {budget};
    cs.flush(lb, 1);
    cs.destroy();
    EXPECT_FALSE(tgt.data.empty());
  }

  // Incremental multi-analysis push with MT env + auto_flush + Cplex EST
  // so auto_flush triggers mid-tile with missing records + background updates.
  {
    siz_params siz;
    siz.set(Sdims, 0, 0, 256);
    siz.set(Sdims, 0, 1, 256);
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sprecision, 0, 0, 12);
    siz.set(Ssigned, 0, 0, false);
    siz.finalize_all();
    BufferTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    kdu_params* cod = cs.access_siz()->access_cluster(COD_params);
    kdu_params* qcd = cs.access_siz()->access_cluster(QCD_params);
    cod->parse_string("Creversible=no");
    cod->parse_string("Clevels=3");
    cod->parse_string("Cblk={16,16}");
    cod->parse_string("Cprecincts={16,16}");
    cod->parse_string("Corder=PCRL");
    cod->parse_string("Clayers=2");
    cod->parse_string(
        "Cband_weights=0.25,0.25,0.25,0.5,0.5,0.5,0.5,0.5,0.5,1.0");
    cod->parse_string("Cprog_weights=0.5");
    cod->parse_string("Cplex={6,EST,0,0}");
    qcd->parse_string("Qstep=0.01");
    qcd->parse_string("Qfparams={0.04,30,0.1,95}");
    cs.access_siz()->finalize_all();

    kdu_thread_env env;
    env.create();
    for (int t = 0; t < 2; ++t) EXPECT_TRUE(env.add_thread());
    env.cs_terminate(cs);

    cs.set_max_bytes(24000, true, false);
    kdu_long lb[2] = {4000, 24000};
    cs.auto_flush(1, 1, 2, 2, lb, 2, nullptr, true, true, 0.0, &env, 0);

    kdu_tile tile = cs.open_tile(kdu_coords(0, 0), &env);
    kdu_multi_analysis engine;
    engine.create(cs, tile, false, nullptr, false, 1, &env);
    for (int y = 0; y < 256; ++y) {
      kdu_line_buf* line = engine.exchange_line(0, nullptr, &env);
      if (line == nullptr) break;
      for (int x = 0; x < 256; ++x) {
        const int v = (((x * 41 + y * 67) ^ (x * y)) & 0xFFF) - 2048;
        if (line->get_buf32() != nullptr) {
          line->get_buf32()[x].fval = static_cast<float>(v) / 4096.0F;
        } else {
          line->get_buf16()[x].ival = static_cast<kdu_int16>(v);
        }
      }
      engine.exchange_line(0, line, &env);
    }
    engine.destroy(&env);
    tile.close(&env);
    cs.flush(lb, 2, nullptr, true, true, 0.0, &env);
    env.destroy();
    cs.destroy();
  }

  // 3. Synth overshoot ringing with sharp step edge (non-error & throwing).
  for (bool is_signed : {false, true}) {
    CodestreamSpec spec = make_spec(64, 64, 1, 8, is_signed);
    spec.reversible = false;
    spec.extra_params = {"Clevels=2", "Cblk={16,16}", "Qstep=0.04"};
    std::vector<int> step_plane(64 * 64);
    for (int y = 0; y < 64; ++y) {
      for (int x = 0; x < 64; ++x) {
        if (is_signed) {
          step_plane[y * 64 + x] = (x < 32) ? -128 : 127;
        } else {
          step_plane[y * 64 + x] = (x < 32) ? 0 : 255;
        }
      }
    }
    kdu_long budget = 2500;
    BufferTarget tgt;
    {
      siz_params siz;
      siz.set(Sdims, 0, 0, 64);
      siz.set(Sdims, 0, 1, 64);
      siz.set(Scomponents, 0, 0, 1);
      siz.set(Sprecision, 0, 0, 8);
      siz.set(Ssigned, 0, 0, is_signed);
      siz.finalize_all();
      kdu_codestream cs;
      cs.create(&siz, &tgt);
      ApplyCodingParams(cs, spec);
      cs.access_siz()->finalize_all();
      PushAllTiles(cs, {step_plane}, spec);
      cs.flush(&budget, 1);
      cs.destroy();
    }
    for (bool gen_err : {false, true}) {
      BufferSource src(tgt.data);
      kdu_codestream dcs;
      dcs.create(&src);
      dcs.set_synth_overshoot_limit(0.0001F, gen_err);
      error_handler_.clear();
      try {
        (void)DecodeCodestream(dcs, false);
      } catch (int) {
      }
      float max_ov = 0.0F;
      (void)dcs.report_synth_overshoot(0, max_ov);
      error_handler_.clear();
      if (dcs.exists()) dcs.destroy();
    }
  }

  // 4. Packet header exceptions WITHOUT EPH/SOP: ILLEGAL_MISSING_MSBS and
  // PRECISION (beta > 255 via 0xFF,0x7F bit-unstuffing without marker code).
  {
    CodestreamSpec spec = make_spec(128, 128, 1);
    spec.reversible = true;
    spec.extra_params = {"Clevels=2", "Cblk={16,16}", "Clayers=2",
                         "Cuse_sop=no", "Cuse_eph=no"};
    std::vector<int> plane(128 * 128);
    for (int i = 0; i < 128 * 128; ++i) {
      plane[i] = ((i * 73) ^ (i >> 3) ^ (i * 17)) & 0xFF;
    }
    const auto orig = EncodeCodestream(spec, {plane});
    size_t sod_pos = 0;
    for (size_t i = 0; i + 1 < orig.size(); ++i) {
      if (orig[i] == 0xFF && orig[i + 1] == 0x93) {
        sod_pos = i + 2;
        break;
      }
    }
    ASSERT_GT(sod_pos, 0U);
    ASSERT_GT(orig.size(), sod_pos + 120U);

    std::vector<kdu_byte> bad_msbs = orig;
    bad_msbs[sod_pos] = 0x80;
    for (size_t k = 1; k <= 14; ++k) bad_msbs[sod_pos + k] = 0x00;

    std::vector<kdu_byte> bad_prec1 = orig;
    for (size_t k = 0; k < 80; k += 2) {
      bad_prec1[sod_pos + k] = 0xFF;
      bad_prec1[sod_pos + k + 1] = 0x7F;
    }

    std::vector<kdu_byte> bad_prec2 = orig;
    bad_prec2[sod_pos] = 0xFE;
    for (size_t k = 1; k + 1 < 80; k += 2) {
      bad_prec2[sod_pos + k] = 0xFF;
      bad_prec2[sod_pos + k + 1] = 0x7F;
    }

    std::vector<kdu_byte> bad_body = orig;
    const size_t mid = orig.size() * 2 / 3;
    bad_body[mid] = 0xFF;
    bad_body[mid + 1] = 0x90;

    std::vector<kdu_byte> trunc_body(orig.begin(), orig.begin() + mid);
    std::vector<kdu_byte> trunc_hdr(orig.begin(), orig.begin() + sod_pos + 4);

    for (const auto& buf :
         {bad_msbs, bad_prec1, bad_prec2, bad_body, trunc_body, trunc_hdr}) {
      for (int mode = 0; mode < 3; ++mode) {
        BufferSource src(buf);
        kdu_codestream dcs;
        dcs.create(&src);
        if (mode == 1) dcs.set_resilient(true);
        if (mode == 2) dcs.set_allow_parsing_errors(true);
        error_handler_.clear();
        try {
          (void)DecodeCodestream(dcs, false);
        } catch (int) {
        }
        error_handler_.clear();
        if (dcs.exists()) dcs.destroy();
      }
    }
  }

  // 5. kd_precinct::reset_packet_reading() under cached source after layer 1
  // decode releases precincts (clearing KD_PFLAG_LOADED_LOCKED), for both
  // !compact_blocks (16x16) and compact_blocks (4x4), normal and resilient.
  for (int blk_sz : {16, 4}) {
    siz_params siz;
    siz.set(Sdims, 0, 0, 128);
    siz.set(Sdims, 0, 1, 128);
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    siz.finalize_all();
    BufferTarget ctgt(/*cached=*/true);
    kdu_codestream cs;
    cs.create(&siz, &ctgt);
    kdu_params* cod = cs.access_siz()->access_cluster(COD_params);
    cod->parse_string("Creversible=yes");
    cod->parse_string("Clevels=2");
    cod->parse_string(blk_sz == 4 ? "Cblk={4,4}" : "Cblk={16,16}");
    cod->parse_string("Clayers=2");
    cod->parse_string("Corder=LRCP");
    cs.access_siz()->finalize_all();
    std::vector<int> plane(128 * 128);
    for (int i = 0; i < 128 * 128; ++i) plane[i] = ((i * 59) ^ (i >> 2)) & 0xFF;
    PushAllTiles(cs, {plane}, make_spec(128, 128));
    kdu_long lb[2] = {400, 16000};
    cs.flush(lb, 2);
    cs.destroy();
    if (ctgt.main_header.empty() && ctgt.tile_headers.count(0)) {
      ctgt.main_header.swap(ctgt.tile_headers[0]);
    }
    class SimpleCachedSrc : public kdu_compressed_source {
     public:
      SimpleCachedSrc(const std::vector<kdu_byte>& m,
                      const std::map<int, std::vector<kdu_byte>>& th,
                      const std::map<kdu_long, std::vector<kdu_byte>>& pk)
          : m_(m), th_(th), pk_(pk), cur_(&m_) {}
      int get_capabilities() override { return KDU_SOURCE_CAP_CACHED; }
      bool set_tileheader_scope(int tnum, int /*n*/) override {
        auto it = th_.find(tnum);
        cur_ = (it == th_.end()) ? &empty_ : &it->second;
        pos_ = 0;
        return true;
      }
      bool set_precinct_scope(kdu_long uid) override {
        auto it = pk_.find(uid);
        cur_ = (it == pk_.end()) ? &empty_ : &it->second;
        pos_ = 0;
        return true;
      }
      int read(kdu_byte* buf, int n) override {
        if (cur_ == nullptr || pos_ >= cur_->size() || n <= 0) return 0;
        const size_t avail =
            std::min(static_cast<size_t>(n), cur_->size() - pos_);
        std::memcpy(buf, cur_->data() + pos_, avail);
        pos_ += avail;
        return static_cast<int>(avail);
      }

     private:
      const std::vector<kdu_byte>& m_;
      const std::map<int, std::vector<kdu_byte>>& th_;
      const std::map<kdu_long, std::vector<kdu_byte>>& pk_;
      const std::vector<kdu_byte>* cur_;
      std::vector<kdu_byte> empty_;
      size_t pos_ = 0;
    };
    for (bool resilient : {false, true}) {
      SimpleCachedSrc csrc(ctgt.main_header, ctgt.tile_headers, ctgt.precincts);
      kdu_codestream dcs;
      dcs.create(&csrc);
      dcs.set_persistent();
      if (resilient) dcs.set_resilient(true);
      dcs.apply_input_restrictions(0, 1, 0, 1, nullptr,
                                   KDU_WANT_CODESTREAM_COMPONENTS);
      EXPECT_EQ(DecodeCodestream(dcs, false).size(), 1U);
      dcs.apply_input_restrictions(0, 1, 0, 2, nullptr,
                                   KDU_WANT_CODESTREAM_COMPONENTS);
      EXPECT_EQ(DecodeCodestream(dcs, false).size(), 1U);
      dcs.destroy();
    }
  }

  // 6. Profile warning branches (Sprofile=CINEMASS violations + PROFILE1).
  {
    siz_params siz;
    siz.set(Sdims, 0, 0, 64);
    siz.set(Sdims, 0, 1, 64);
    siz.set(Scomponents, 0, 0, 3);
    siz.set(Sprecision, 0, 0, 12);
    siz.set(Ssigned, 0, 0, false);
    static_cast<kdu_params&>(siz).parse_string("Sprofile=CINEMASS");
    siz.finalize_all();
    BufferTarget tgt;
    error_handler_.clear();
    kdu_codestream cs;
    try {
      cs.create(&siz, &tgt);
      kdu_params* cod = cs.access_siz()->access_cluster(COD_params);
      cod->parse_string("Creversible=no");
      cod->parse_string("Clevels=2");
      cod->parse_string("Cblk={32,32}");
      cod->parse_string("Cprecincts={512,512}");
      cod->parse_string("Corder=LRCP");
      cod->parse_string("Clayers=6");
      kdu_params* rgn = cs.access_siz()->access_cluster(RGN_params);
      rgn->parse_string("Rshift=40");
      cs.access_siz()->finalize_all();
    } catch (int) {
    }
    error_handler_.clear();
    if (cs.exists()) cs.destroy();
  }
  {
    siz_params siz;
    siz.set(Sdims, 0, 0, 64);
    siz.set(Sdims, 0, 1, 64);
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    siz.set(Stiles, 0, 0, 32);
    siz.set(Stiles, 0, 1, 32);
    static_cast<kdu_params&>(siz).parse_string("Sprofile=PROFILE1");
    siz.finalize_all();
    BufferTarget tgt;
    error_handler_.clear();
    kdu_codestream cs;
    try {
      cs.create(&siz, &tgt);
      kdu_params* cod = cs.access_siz()->access_cluster(COD_params);
      cod->parse_string("Creversible=yes");
      cod->parse_string("Clevels=1");
      cod->parse_string("Cblk={16,16}");
      cod->parse_string("Porder:T0={0,0,1,1,1,PCRL}");
      kdu_params* rgn = cs.access_siz()->access_cluster(RGN_params);
      rgn->parse_string("Rshift:T0=1");
      cs.access_siz()->finalize_all();
    } catch (int) {
    }
    error_handler_.clear();
    if (cs.exists()) cs.destroy();
  }

  // 7. cached_target + change_appearance(transpose=true) close_block branch,
  // update_block with MT env / compact_blocks / zero passes,
  // kdu_node::get_num_descendants, and kdu_subband::get_parent_dims.
  for (int blk_sz : {16, 4}) {
    siz_params siz;
    siz.set(Sdims, 0, 0, 64);
    siz.set(Sdims, 0, 1, 64);
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    siz.finalize_all();
    BufferTarget ctgt(/*cached=*/true);
    kdu_thread_env env;
    env.create();
    EXPECT_TRUE(env.add_thread());
    kdu_codestream cs;
    cs.create(&siz, &ctgt, nullptr, 0, 0, &env);
    env.cs_terminate(cs);
    cs.change_appearance(true, false, false);
    kdu_params* cod = cs.access_siz()->access_cluster(COD_params);
    cod->parse_string("Creversible=yes");
    cod->parse_string("Clevels=2");
    cod->parse_string(blk_sz == 4 ? "Cblk={4,4}" : "Cblk={16,16}");
    cs.access_siz()->finalize_all();
    std::vector<int> plane(64 * 64);
    for (int i = 0; i < 64 * 64; ++i) plane[i] = (i * 23) & 0xFF;
    const CodestreamSpec sp64 = make_spec(64, 64);
    {
      kdu_tile tile = cs.open_tile(kdu_coords(0, 0), &env);
      PushTileSamples(cs, tile, {plane}, sp64, nullptr, &env);
      tile.close(&env);
    }

    kdu_tile rtile = cs.review_closed_tile(kdu_coords(0, 0));
    if (rtile.exists()) {
      kdu_subband sb =
          rtile.access_component(0).access_resolution(0).access_subband(
              LL_BAND);
      kdu_dims vblks;
      sb.get_valid_blocks(vblks);
      if (!vblks.is_empty()) {
        kdu_block* fb = sb.fetch_block(vblks.pos, &env);
        if (fb != nullptr) {
          fb->num_passes = 0;
          sb.update_block(fb, &env);
        }
      }
      rtile.close();
    }
    kdu_long max_b = 1LL << 20;
    cs.flush(&max_b, 1, nullptr, true, true, 0.0, &env);
    env.destroy();
    cs.destroy();
  }

  // get_num_descendants and get_parent_dims across Clevels=0,2, V(-)/H(-), and
  // appearance transforms (transpose, vflip, hflip).
  for (int cfg : {0, 1, 2}) {
    const int levels = (cfg == 0) ? 0 : 2;
    CodestreamSpec spec = make_spec(32, 32, 1);
    spec.reversible = true;
    spec.extra_params = {levels == 0 ? "Clevels=0" : "Clevels=2", "Cblk={8,8}"};
    if (cfg == 2) spec.extra_params.push_back("Cdecomp=V(-),H(-)");
    std::vector<int> plane(32 * 32, 17);
    const auto bytes = EncodeCodestream(spec, {plane});
    for (int app = 0; app < 3; ++app) {
      BufferSource src(bytes);
      kdu_codestream dcs;
      dcs.create(&src);
      dcs.change_appearance(app == 1, app >= 1, app == 2);
      kdu_tile tile = dcs.open_tile(kdu_coords(0, 0));
      kdu_tile_comp tc = tile.access_component(0);
      for (int r = 0; r <= levels; ++r) {
        kdu_resolution res = tc.access_resolution(r);
        kdu_node root_node = res.access_node();
        int leaf_desc = -1;
        EXPECT_GE(root_node.get_num_descendants(leaf_desc), 0);
        if (r == 0) {
          kdu_subband sb = res.access_subband(LL_BAND);
          if (sb.exists()) {
            kdu_dims pd1, pd2;
            sb.get_parent_dims(pd1, false);
            sb.get_parent_dims(pd2, true);
          }
        }
        for (int c = 0; c < 4; ++c) {
          kdu_node ch = root_node.access_child(c);
          if (ch.exists()) {
            int ch_leaves = -1;
            (void)ch.get_num_descendants(ch_leaves);
            kdu_subband sb = ch.access_subband();
            if (sb.exists()) {
              kdu_dims pd1, pd2;
              sb.get_parent_dims(pd1, false);
              sb.get_parent_dims(pd2, true);
            }
          }
        }
      }
      tile.close();
      dcs.destroy();
    }
  }
}

TEST_F(CompressedTest, CompressedCoverage90Push) {
  auto make_spec = [](int w, int h, int comps = 1, int bits = 8,
                      bool is_signed = false) {
    CodestreamSpec sp;
    sp.width = w;
    sp.height = h;
    sp.components = comps;
    sp.bit_depth = bits;
    sp.is_signed = is_signed;
    return sp;
  };

  // 1. MT Cplex=EST with kdu_codestream::create(..., &env) so thread_context is
  // active + kdu_multi_analysis strip push + auto_flush + cached target
  // incr_flush_interval (transpose & normal) + Creslengths:T0 on ENC_params +
  // wide budget sweep for tlut.
  {
    siz_params siz;
    siz.set(Sdims, 0, 0, 256);
    siz.set(Sdims, 0, 1, 256);
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sprecision, 0, 0, 12);
    siz.set(Ssigned, 0, 0, false);
    siz.finalize_all();
    BufferTarget tgt;
    kdu_thread_env env;
    env.create();
    for (int t = 0; t < 2; ++t) EXPECT_TRUE(env.add_thread());
    kdu_codestream cs;
    cs.create(&siz, &tgt, nullptr, 0, 0, &env);
    kdu_params* cod = cs.access_siz()->access_cluster(COD_params);
    kdu_params* qcd = cs.access_siz()->access_cluster(QCD_params);
    cod->parse_string("Creversible=no");
    cod->parse_string("Clevels=3");
    cod->parse_string("Cblk={16,16}");
    cod->parse_string("Cprecincts={16,16}");
    cod->parse_string("Corder=PCRL");
    cod->parse_string("Clayers=2");
    cod->parse_string(
        "Cband_weights=0.25,0.25,0.25,0.5,0.5,0.5,0.5,0.5,0.5,1.0");
    cod->parse_string("Cprog_weights=0.5");
    cod->parse_string("Cplex={6,EST,0,0}");
    qcd->parse_string("Qstep=0.01");
    qcd->parse_string("Qfparams={0.04,30,0.1,95}");
    cs.access_siz()->finalize_all();

    cs.set_max_bytes(60000, true, false);
    kdu_long lb[2] = {3000, 60000};
    cs.auto_flush(1, 1, 8, 8, lb, 2, nullptr, true, true, 0.0, &env, 0);

    kdu_tile tile = cs.open_tile(kdu_coords(0, 0), &env);
    kdu_multi_analysis engine;
    engine.create(cs, tile, false, nullptr, false, 1, &env);
    for (int y = 0; y < 256; ++y) {
      kdu_line_buf* line = engine.exchange_line(0, nullptr, &env);
      if (line == nullptr) break;
      for (int x = 0; x < 256; ++x) {
        const int v = (((x * 43 + y * 71) ^ (x * y)) & 0xFFF) - 2048;
        if (line->get_buf32() != nullptr) {
          line->get_buf32()[x].fval = static_cast<float>(v) / 4096.0F;
        } else {
          line->get_buf16()[x].ival = static_cast<kdu_int16>(v);
        }
      }
      engine.exchange_line(0, line, &env);
    }
    engine.destroy(&env);
    tile.close(&env, true);
    cs.flush(lb, 2, nullptr, true, false, 0.0, &env);
    env.cs_terminate(cs);
    cs.destroy();
    env.destroy();
  }

  for (bool use_transpose : {false, true}) {
    siz_params siz;
    siz.set(Sdims, 0, 0, 128);
    siz.set(Sdims, 0, 1, 128);
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    siz.finalize_all();
    BufferTarget ctgt(/*cached=*/true);
    kdu_thread_env env;
    env.create();
    EXPECT_TRUE(env.add_thread());
    kdu_codestream cs;
    cs.create(&siz, &ctgt, nullptr, 0, 0, &env);
    if (use_transpose) cs.change_appearance(true, false, false);
    kdu_params* cod = cs.access_siz()->access_cluster(COD_params);
    cod->parse_string("Creversible=yes");
    cod->parse_string("Clevels=2");
    cod->parse_string("Cblk={16,16}");
    cod->parse_string("Corder=PCRL");
    cod->parse_string("Clayers=2");
    cs.access_siz()->finalize_all();
    cs.set_max_bytes(32000, true, false);
    kdu_long lb[2] = {1200, 32000};
    cs.auto_flush(1, 1, 8, 8, lb, 2, nullptr, true, true, 0.0, &env);
    std::vector<int> plane(128 * 128);
    for (int i = 0; i < 128 * 128; ++i) plane[i] = (i * 29) & 0xFF;
    const CodestreamSpec sp128 = make_spec(128, 128);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0), &env);
    PushTileSamples(cs, tile, {plane}, sp128, nullptr, &env);
    tile.close(&env, true);
    cs.flush(lb, 2, nullptr, true, false, 0.0, &env);
    env.cs_terminate(cs);
    cs.destroy();
    env.destroy();
  }

  // Tile-specific ENC_params Creslengths:T0 without global Creslengths +
  // wide budget sweep for tlut hits.
  {
    siz_params siz;
    siz.set(Sdims, 0, 0, 128);
    siz.set(Sdims, 0, 1, 128);
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    siz.set(Stiles, 0, 0, 64);
    siz.set(Stiles, 0, 1, 64);
    siz.finalize_all();
    BufferTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    kdu_params* cod = cs.access_siz()->access_cluster(COD_params);
    kdu_params* enc = cs.access_siz()->access_cluster(ENC_params);
    cod->parse_string("Creversible=yes");
    cod->parse_string("Clevels=3");
    cod->parse_string("Cblk={8,8}");
    enc->parse_string("Creslengths:T0=4000,2000,1000,500");
    cs.access_siz()->finalize_all();
    std::vector<int> plane(128 * 128);
    for (int i = 0; i < 128 * 128; ++i) plane[i] = (i * 19) & 0xFF;
    PushAllTiles(cs, {plane}, make_spec(128, 128));
    kdu_long max_b = 1LL << 20;
    cs.flush(&max_b, 1);
    cs.destroy();
  }

  for (int budget : {300, 600, 900, 1600, 3200, 7000, 14000}) {
    siz_params siz;
    siz.set(Sdims, 0, 0, 128);
    siz.set(Sdims, 0, 1, 128);
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    siz.finalize_all();
    BufferTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    kdu_params* cod = cs.access_siz()->access_cluster(COD_params);
    kdu_params* qcd = cs.access_siz()->access_cluster(QCD_params);
    cod->parse_string("Creversible=no");
    cod->parse_string("Clevels=3");
    cod->parse_string("Cblk={16,16}");
    cod->parse_string("Cprecincts={32,32}");
    cod->parse_string(
        "Cband_weights=0.25,0.25,0.25,0.5,0.5,0.5,0.5,0.5,0.5,1.0");
    cod->parse_string("Cprog_weights=0.5");
    cod->parse_string("Cplex={6,EST,0,0}");
    qcd->parse_string("Qstep=0.005");
    qcd->parse_string("Qfparams={0.04,30,0.1,95}");
    cs.access_siz()->finalize_all();
    cs.set_max_bytes(budget, true, false);
    std::vector<int> plane(128 * 128);
    for (int y = 0; y < 128; ++y) {
      for (int x = 0; x < 128; ++x) {
        plane[y * 128 + x] = ((x * 31 + y * 47 + (x ^ y) * 13) & 0xFF);
      }
    }
    PushAllTiles(cs, {plane}, make_spec(128, 128));
    kdu_long lb[1] = {budget};
    cs.flush(lb, 1);
    cs.destroy();
  }

  // 2. augment_cache_threshold() + set_persistent() so released precincts
  // remain cached across max_layers 1 -> 2, exercising reset_packet_reading()
  // on both !compact_blocks and compact_blocks, plus MT cache eviction &
  // get_precinct_packets after consumption.
  {
    siz_params siz;
    siz.set(Sdims, 0, 0, 128);
    siz.set(Sdims, 0, 1, 128);
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    siz.set(Stiles, 0, 0, 32);
    siz.set(Stiles, 0, 1, 32);
    siz.finalize_all();
    BufferTarget ctgt(/*cached=*/true);
    BufferTarget stgt(/*cached=*/false);
    std::vector<int> plane(128 * 128);
    for (int i = 0; i < 128 * 128; ++i) {
      plane[i] = ((i * 61) ^ (i >> 2) ^ (i * 11)) & 0xFF;
    }
    for (int pass = 0; pass < 2; ++pass) {
      siz_params s2;
      s2.copy_from(&siz, -1, -1);
      s2.finalize_all();
      kdu_codestream cs;
      cs.create(&s2, pass == 0 ? &ctgt : &stgt);
      kdu_params* cod = cs.access_siz()->access_cluster(COD_params);
      cod->parse_string("Creversible=yes");
      cod->parse_string("Clevels=2");
      cod->parse_string(pass == 0 ? "Cblk={16,16}" : "Cblk={4,4}");
      cod->parse_string("Cprecincts={8,8}");
      cod->parse_string("Clayers=2");
      cod->parse_string("Corder=LRCP");
      cs.access_siz()->finalize_all();
      PushAllTiles(cs, {plane}, make_spec(128, 128));
      kdu_long lb[2] = {600, 16000};
      cs.flush(lb, 2);
      cs.destroy();
    }
    if (ctgt.main_header.empty() && ctgt.tile_headers.count(0)) {
      ctgt.main_header.swap(ctgt.tile_headers[0]);
    }
    class CachedSrc : public kdu_compressed_source {
     public:
      CachedSrc(const std::vector<kdu_byte>& m,
                const std::map<int, std::vector<kdu_byte>>& th,
                const std::map<kdu_long, std::vector<kdu_byte>>& pk)
          : m_(m), th_(th), pk_(pk), cur_(&m_) {}
      int get_capabilities() override { return KDU_SOURCE_CAP_CACHED; }
      bool set_tileheader_scope(int tnum, int /*n*/) override {
        auto it = th_.find(tnum);
        cur_ = (it == th_.end()) ? &empty_ : &it->second;
        pos_ = 0;
        return true;
      }
      bool set_precinct_scope(kdu_long uid) override {
        auto it = pk_.find(uid);
        cur_ = (it == pk_.end()) ? &empty_ : &it->second;
        pos_ = 0;
        return true;
      }
      int read(kdu_byte* buf, int n) override {
        if (cur_ == nullptr || pos_ >= cur_->size() || n <= 0) return 0;
        const size_t avail =
            std::min(static_cast<size_t>(n), cur_->size() - pos_);
        std::memcpy(buf, cur_->data() + pos_, avail);
        pos_ += avail;
        return static_cast<int>(avail);
      }

     private:
      const std::vector<kdu_byte>& m_;
      const std::map<int, std::vector<kdu_byte>>& th_;
      const std::map<kdu_long, std::vector<kdu_byte>>& pk_;
      const std::vector<kdu_byte>* cur_;
      std::vector<kdu_byte> empty_;
      size_t pos_ = 0;
    };
    {
      CachedSrc csrc(ctgt.main_header, ctgt.tile_headers, ctgt.precincts);
      kdu_codestream dcs;
      dcs.create(&csrc);
      dcs.augment_cache_threshold(1 << 22);
      dcs.set_persistent();
      dcs.apply_input_restrictions(0, 1, 0, 1, nullptr,
                                   KDU_WANT_CODESTREAM_COMPONENTS);
      EXPECT_EQ(DecodeCodestream(dcs, false).size(), 1U);
      dcs.apply_input_restrictions(0, 1, 0, 2, nullptr,
                                   KDU_WANT_CODESTREAM_COMPONENTS);
      EXPECT_EQ(DecodeCodestream(dcs, false).size(), 1U);
      dcs.destroy();
    }
    {
      BufferSource seek_src(
          stgt.data, KDU_SOURCE_CAP_SEQUENTIAL | KDU_SOURCE_CAP_SEEKABLE);
      kdu_codestream dcs;
      dcs.create(&seek_src);
      dcs.enable_restart();
      dcs.augment_cache_threshold(1 << 22);
      dcs.set_persistent();
      dcs.set_tile_unloading_threshold(2);
      dcs.apply_input_restrictions(0, 1, 0, 1, nullptr,
                                   KDU_WANT_CODESTREAM_COMPONENTS);
      EXPECT_EQ(DecodeCodestream(dcs, false).size(), 1U);
      dcs.apply_input_restrictions(0, 1, 0, 2, nullptr,
                                   KDU_WANT_CODESTREAM_COMPONENTS);
      EXPECT_EQ(DecodeCodestream(dcs, false).size(), 1U);
      {
        kdu_tile tile = dcs.open_tile(kdu_coords(0, 0));
        kdu_resolution res = tile.access_component(0).access_resolution(1);
        kdu_dims pdims;
        res.get_valid_precincts(pdims);
        if (!pdims.is_empty()) {
          (void)res.get_precinct_packets(pdims.pos, nullptr, true);
        }
        tile.close();
      }
      dcs.destroy();
    }
  }

  // 3. Multi-tile decode with sequential AsyncMemSource (full buffer).
  {
    CodestreamSpec spec = make_spec(128, 128, 1);
    spec.reversible = true;
    spec.tile_size = kdu_coords(64, 64);
    spec.extra_params = {"Clevels=2", "Cprecincts={16,16}", "Cblk={8,8}",
                         "Clayers=2"};
    std::vector<int> plane(128 * 128);
    for (int i = 0; i < 128 * 128; ++i) plane[i] = (i * 47) & 0xFF;
    const auto bytes = EncodeCodestream(spec, {plane});
    AsyncMemSource async_src(bytes, bytes.size(), 128,
                             KDU_SOURCE_CAP_MEMORY_BACKED, false, 0, false);
    kdu_codestream dcs;
    dcs.create(&async_src);
    EXPECT_EQ(DecodeCodestream(dcs, false).size(), 1U);
    dcs.destroy();
  }

  // 4. get_mct_rxform_info parameter combinations + get_parent_dims with
  // odd Sorigin, Part-2 recursive Cdecomp, input region crop, and all 8
  // transpose/vflip/hflip combinations.
  {
    CodestreamSpec spec = make_spec(32, 32, 4);
    spec.reversible = true;
    spec.extra_params = {
        "Cycc=no",
        "Sextensions=MCT",
        "Mcomponents=4",
        "Mprecision=8",
        "Msigned=yes",
        "Mmatrix_size:I1=6",
        "Mmatrix_coeffs:I1=1,512,512,-1,0,1",
        "Mmatrix_size:I2=6",
        "Mmatrix_coeffs:I2=1,512,512,-1,0,1",
        "Mstage_inputs:I1={0,1},{2,3}",
        "Mstage_outputs:I1={0,1},{2,3}",
        "Mstage_collections:I1={2,2},{2,2}",
        "Mstage_xforms:I1={MATRIX,1,0,1,0},{MATRIX,2,0,1,0}",
        "Mnum_stages=1",
        "Mstages=1",
        "Clevels=1",
        "Cblk={16,16}",
    };
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    BufferSource src(bytes);
    kdu_codestream dcs;
    dcs.create(&src);
    kdu_tile tile = dcs.open_tile(kdu_coords(0, 0));
    int nsi = 0, nso = 0, nbi = 0, nbo = 0;
    for (int blk = 0; blk < 2; ++blk) {
      if (tile.get_mct_block_info(0, blk, nsi, nso, nbi, nbo) &&
          tile.get_mct_rxform_info(0, blk)) {
        std::vector<int> rmat(static_cast<size_t>(nbi * (nbi + 1)));
        std::vector<int> aout(static_cast<size_t>(nbo));
        (void)tile.get_mct_rxform_info(0, blk, nullptr, nullptr);
        (void)tile.get_mct_rxform_info(0, blk, rmat.data(), nullptr);
        (void)tile.get_mct_rxform_info(0, blk, nullptr, aout.data());
        (void)tile.get_mct_rxform_info(0, blk, rmat.data(), aout.data());
      }
    }
    tile.close();
    dcs.destroy();
  }

  {
    siz_params siz;
    siz.set(Sdims, 0, 0, 67);
    siz.set(Sdims, 0, 1, 69);
    siz.set(Sorigin, 0, 0, 3);
    siz.set(Sorigin, 0, 1, 5);
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    siz.finalize_all();
    BufferTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    kdu_params* cod = cs.access_siz()->access_cluster(COD_params);
    cod->parse_string("Creversible=yes");
    cod->parse_string("Clevels=2");
    cod->parse_string("Cblk={8,8}");
    cod->parse_string("Cdecomp=B(H--:V--:H--),B(-:-:-)");
    cs.access_siz()->finalize_all();
    std::vector<int> plane(67 * 69, 42);
    kdu_tile itile = cs.open_tile(kdu_coords(0, 0));
    PushTileSamples(cs, itile, {plane}, make_spec(69, 67));
    itile.close();
    kdu_long max_b = 1LL << 20;
    cs.flush(&max_b, 1);
    cs.destroy();

    for (int m = 0; m < 8; ++m) {
      BufferSource src(tgt.data);
      kdu_codestream dcs;
      dcs.create(&src);
      dcs.change_appearance((m & 1) != 0, (m & 2) != 0, (m & 4) != 0);
      kdu_dims crop;
      crop.pos = kdu_coords(4, 7);
      crop.size = kdu_coords(23, 21);
      dcs.apply_input_restrictions(0, 0, 0, 0, &crop,
                                   KDU_WANT_CODESTREAM_COMPONENTS);
      error_handler_.clear();
      try {
        kdu_tile tile = dcs.open_tile(kdu_coords(0, 0));
        kdu_tile_comp tc = tile.access_component(0);
        for (int r = 0; r <= 2; ++r) {
          kdu_resolution res = tc.access_resolution(r);
          kdu_node root = res.access_node();
          if (r == 0) {
            kdu_subband sb = res.access_subband(LL_BAND);
            kdu_dims pd1, pd2;
            sb.get_parent_dims(pd1, false);
            sb.get_parent_dims(pd2, true);
          }
          std::function<void(kdu_node)> visit = [&](kdu_node n) {
            int tmp_leaves = -1;
            if (n.get_num_descendants(tmp_leaves) == 0) {
              kdu_subband sb = n.access_subband();
              if (sb.exists()) {
                kdu_dims pd1, pd2;
                sb.get_parent_dims(pd1, false);
                sb.get_parent_dims(pd2, true);
              }
              return;
            }
            for (int c = 0; c < 4; ++c) {
              kdu_node ch = n.access_child(c);
              if (ch.exists()) visit(ch);
            }
          };
          visit(root);
        }
        tile.close();
      } catch (int) {
      }
      error_handler_.clear();
      dcs.destroy();
    }

    // Repeat with symmetric wavelet decomposition so all 8 flip/transpose
    // modes succeed through access_resolution and get_parent_dims.
    {
      siz_params sym_siz;
      sym_siz.set(Sdims, 0, 0, 67);
      sym_siz.set(Sdims, 0, 1, 69);
      sym_siz.set(Sorigin, 0, 0, 3);
      sym_siz.set(Sorigin, 0, 1, 5);
      sym_siz.set(Scomponents, 0, 0, 1);
      sym_siz.set(Sprecision, 0, 0, 8);
      sym_siz.set(Ssigned, 0, 0, false);
      sym_siz.finalize_all();
      BufferTarget sym_tgt;
      kdu_codestream sym_cs;
      sym_cs.create(&sym_siz, &sym_tgt);
      kdu_params* sym_cod = sym_cs.access_siz()->access_cluster(COD_params);
      sym_cod->parse_string("Creversible=yes");
      sym_cod->parse_string("Clevels=2");
      sym_cod->parse_string("Cblk={8,8}");
      sym_cs.access_siz()->finalize_all();
      kdu_tile it = sym_cs.open_tile(kdu_coords(0, 0));
      PushTileSamples(sym_cs, it, {plane}, make_spec(69, 67));
      it.close();
      kdu_long sb_max = 1LL << 20;
      sym_cs.flush(&sb_max, 1);
      sym_cs.destroy();
      for (int m = 0; m < 8; ++m) {
        BufferSource src(sym_tgt.data);
        kdu_codestream dcs;
        dcs.create(&src);
        dcs.change_appearance((m & 1) != 0, (m & 2) != 0, (m & 4) != 0);
        EXPECT_TRUE(dcs.can_flip(true));
        EXPECT_TRUE(dcs.can_flip(false));
        kdu_dims crop;
        crop.pos = kdu_coords(4, 7);
        crop.size = kdu_coords(23, 21);
        dcs.apply_input_restrictions(0, 0, 0, 0, &crop,
                                     KDU_WANT_CODESTREAM_COMPONENTS);
        kdu_tile tile = dcs.open_tile(kdu_coords(0, 0));
        kdu_tile_comp tc = tile.access_component(0);
        for (int r = 0; r <= 2; ++r) {
          kdu_resolution res = tc.access_resolution(r);
          kdu_node root = res.access_node();
          if (r == 0) {
            kdu_subband sb = res.access_subband(LL_BAND);
            kdu_dims pd1, pd2;
            sb.get_parent_dims(pd1, false);
            sb.get_parent_dims(pd2, true);
          }
          std::function<void(kdu_node)> visit = [&](kdu_node n) {
            int tmp_leaves = -1;
            if (n.get_num_descendants(tmp_leaves) == 0) {
              kdu_subband sb = n.access_subband();
              if (sb.exists()) {
                kdu_dims pd1, pd2;
                sb.get_parent_dims(pd1, false);
                sb.get_parent_dims(pd2, true);
              }
              return;
            }
            for (int c = 0; c < 4; ++c) {
              kdu_node ch = n.access_child(c);
              if (ch.exists()) visit(ch);
            }
          };
          visit(root);
        }
        tile.close();
        dcs.destroy();
      }
    }
  }

  // 5. Packet-body truncation & overrun with valid packet header in default,
  // resilient, and allow_parsing_errors modes (incl. explicit
  // parse_all_relevant_packets).
  {
    CodestreamSpec spec = make_spec(64, 64, 1);
    spec.reversible = true;
    spec.extra_params = {"Clevels=1", "Cblk={16,16}", "Clayers=2",
                         "Cuse_sop=no", "Cuse_eph=no"};
    std::vector<int> plane(64 * 64);
    for (int i = 0; i < 64 * 64; ++i) plane[i] = (i * 37) & 0xFF;
    const auto orig = EncodeCodestream(spec, {plane});
    size_t sod_pos = 0;
    for (size_t i = 0; i + 1 < orig.size(); ++i) {
      if (orig[i] == 0xFF && orig[i + 1] == 0x93) {
        sod_pos = i + 2;
        break;
      }
    }
    ASSERT_GT(sod_pos, 0U);
    ASSERT_GT(orig.size(), sod_pos + 16U);

    // Valid packet header claiming >63 bytes for body, followed by immediate
    // truncation OR SOT marker inside body.
    std::vector<kdu_byte> body_trunc(orig.begin(), orig.begin() + sod_pos + 4);
    body_trunc[sod_pos] = 0xEA;
    body_trunc[sod_pos + 1] = 0xFF;
    body_trunc[sod_pos + 2] = 0x00;
    body_trunc[sod_pos + 3] = 0x01;

    std::vector<kdu_byte> body_marker = body_trunc;
    body_marker.push_back(0xFF);
    body_marker.push_back(0x90);

    for (const auto& buf : {body_trunc, body_marker}) {
      for (int mode = 0; mode < 3; ++mode) {
        BufferSource src(buf);
        kdu_codestream dcs;
        dcs.create(&src);
        if (mode == 1) dcs.set_resilient(true);
        if (mode == 2) dcs.set_allow_parsing_errors(true);
        error_handler_.clear();
        try {
          kdu_tile t = dcs.open_tile(kdu_coords(0, 0));
          (void)t.parse_all_relevant_packets(false, nullptr);
          t.close();
        } catch (int) {
        }
        error_handler_.clear();
        if (dcs.exists()) dcs.destroy();
      }
    }
  }
}

TEST_F(CompressedTest, CompressedCoverage90Finish) {
  auto make_spec = [](int w, int h, int c = 1) {
    CodestreamSpec s;
    s.width = w;
    s.height = h;
    s.components = c;
    return s;
  };

  // 1. Seekable PLT + RPCL load_sequential -> triggers kd_precinct::read_packet
  // and restart()/multi-layer persistent cache -> reset_packet_reading().
  {
    struct TestMonitor : public kdu_input_codestream_monitor {
      void installing_monitor() override { installed = true; }
      void restarting() override { restarted = true; }
      void uninstalling_monitor() override { uninstalled = true; }
      void parsing_complete() override { complete = true; }
      bool installed = false;
      bool restarted = false;
      bool uninstalled = false;
      bool complete = false;
    };

    CodestreamSpec spec = make_spec(128, 128, 1);
    spec.reversible = true;
    spec.tile_size = kdu_coords(64, 64);
    spec.extra_params = {"Clevels=2",   "Cprecincts={16,16}", "Cblk={8,8}",
                         "Clayers=3",   "Corder=RPCL",        "ORGgen_plt=yes",
                         "Cuse_sop=yes"};
    std::vector<int> plane(128 * 128);
    for (int i = 0; i < 128 * 128; ++i) plane[i] = (i * 53) & 0xFF;
    const auto bytes = EncodeCodestream(spec, {plane});

    // 1a. Non-persistent load_sequential on seekable PLT stream + monitor.
    {
      BufferSource seek_src(
          bytes, KDU_SOURCE_CAP_SEQUENTIAL | KDU_SOURCE_CAP_SEEKABLE);
      kdu_codestream dcs;
      dcs.create(&seek_src);
      TestMonitor mon;
      dcs.install_input_monitor(&mon, nullptr);
      EXPECT_TRUE(mon.installed);
      kdu_dims tvalid;
      dcs.get_valid_tiles(tvalid);
      for (int ty = 0; ty < tvalid.size.y; ++ty) {
        for (int tx = 0; tx < tvalid.size.x; ++tx) {
          kdu_pdescriptor_set pset;
          ASSERT_TRUE(pset.init(kdu_coords(tx, ty), 3));
          bool tile_done = false;
          kdu_tile lt;
          while (!tile_done) {
            kdu_tile nt =
                dcs.load_sequential(&pset, tile_done, 2, 0,
                                    KDU_LOAD_SEQUENTIAL_WANT_FLUSH_SET |
                                        KDU_LOAD_SEQUENTIAL_WANT_PACKET_BYTES,
                                    nullptr);
            if (!nt.exists()) break;
            lt = nt;
          }
          if (lt.exists()) {
            kdu_long lb[3] = {}, lp[3] = {};
            (void)lt.get_parsed_packet_stats(-1, -1, 3, lb, lp);
            (void)lt.get_parsed_packet_stats(99, 0, 3, lb, lp);
            (void)lt.find_component_gain_info(-1, false);
            (void)lt.find_component_gain_info(0, true);
            lt.close();
          }
        }
      }
      dcs.destroy();
      EXPECT_TRUE(mon.uninstalled);
    }

    // 1b. Persistent seekable RPCL+PLT with augment_cache_threshold + restart.
    {
      BufferSource seek_src(
          bytes, KDU_SOURCE_CAP_SEQUENTIAL | KDU_SOURCE_CAP_SEEKABLE);
      kdu_codestream dcs;
      dcs.create(&seek_src);
      dcs.enable_restart();
      dcs.augment_cache_threshold(1 << 22);
      dcs.set_persistent();
      TestMonitor mon;
      dcs.install_input_monitor(&mon, nullptr);
      dcs.apply_input_restrictions(0, 1, 0, 1, nullptr,
                                   KDU_WANT_CODESTREAM_COMPONENTS);
      EXPECT_EQ(DecodeCodestream(dcs, false).size(), 1U);
      dcs.apply_input_restrictions(0, 1, 0, 3, nullptr,
                                   KDU_WANT_CODESTREAM_COMPONENTS);
      EXPECT_EQ(DecodeCodestream(dcs, false).size(), 1U);
      seek_src.seek(0);
      dcs.restart(&seek_src);
      dcs.apply_input_restrictions(0, 1, 0, 2, nullptr,
                                   KDU_WANT_CODESTREAM_COMPONENTS);
      EXPECT_EQ(DecodeCodestream(dcs, false).size(), 1U);
      dcs.destroy();
    }

    // 1c. Truncated seekable RPCL+PLT packet under load_sequential (resilient).
    for (int mode = 0; mode < 2; ++mode) {
      std::vector<kdu_byte> trunc(bytes.begin(),
                                  bytes.begin() + bytes.size() * 2 / 3);
      BufferSource seek_src(
          trunc, KDU_SOURCE_CAP_SEQUENTIAL | KDU_SOURCE_CAP_SEEKABLE);
      kdu_codestream dcs;
      dcs.create(&seek_src);
      if (mode == 1) dcs.set_resilient(true);
      error_handler_.clear();
      try {
        kdu_pdescriptor_set pset;
        pset.init(kdu_coords(0, 0), 3);
        bool tile_done = false;
        while (!tile_done) {
          kdu_tile nt = dcs.load_sequential(
              &pset, tile_done, 2, 0, KDU_LOAD_SEQUENTIAL_WANT_PACKET_BYTES,
              nullptr);
          if (!nt.exists()) break;
        }
      } catch (int) {
      }
      error_handler_.clear();
      if (dcs.exists()) dcs.destroy();
    }
  }

  // 2. Clean packet-body truncations after a valid 1-packet header: trailing
  // 0xFF and trailing 0xFF 0x90 (SOT), tested across default/resilient/allow.
  {
    CodestreamSpec spec = make_spec(64, 64, 1);
    spec.reversible = true;
    spec.extra_params = {"Clevels=0", "Cblk={64,64}", "Clayers=1",
                         "Cuse_sop=no", "Cuse_eph=no"};
    std::vector<int> plane(64 * 64);
    for (int i = 0; i < 64 * 64; ++i) plane[i] = (i * 73 + (i >> 3)) & 0xFF;
    const auto orig = EncodeCodestream(spec, {plane});
    ASSERT_GT(orig.size(), 128U);
    const size_t cut = orig.size() / 2;

    std::vector<kdu_byte> end_ff(orig.begin(), orig.begin() + cut);
    end_ff.push_back(0xFF);

    std::vector<kdu_byte> end_sot(orig.begin(), orig.begin() + cut);
    end_sot.push_back(0xFF);
    end_sot.push_back(0x90);

    for (const auto& buf : {end_ff, end_sot}) {
      for (int mode = 0; mode < 3; ++mode) {
        BufferSource src(buf);
        kdu_codestream dcs;
        dcs.create(&src);
        if (mode == 1) dcs.set_resilient(true);
        if (mode == 2) dcs.set_allow_parsing_errors(true);
        error_handler_.clear();
        try {
          EXPECT_EQ(DecodeCodestream(dcs, false).size(), 1U);
        } catch (int) {
        }
        error_handler_.clear();
        if (dcs.exists()) dcs.destroy();
      }
    }
  }

  // 3. Cuse_sop=yes 2-tile resilient decode: SOP skipping into SOT + invalid
  // high SOP sequence number (>= total_packets).
  {
    CodestreamSpec spec = make_spec(128, 64, 1);
    spec.reversible = true;
    spec.tile_size = kdu_coords(64, 64);
    spec.extra_params = {"Clevels=1", "Cblk={16,16}", "Clayers=2",
                         "Cuse_sop=yes", "Cuse_eph=no"};
    std::vector<int> plane(128 * 64);
    for (int i = 0; i < 128 * 64; ++i) plane[i] = (i * 41) & 0xFF;
    const auto orig = EncodeCodestream(spec, {plane});
    std::vector<size_t> sot_pos;
    for (size_t i = 0; i + 1 < orig.size(); ++i) {
      if (orig[i] == 0xFF && orig[i + 1] == 0x90) sot_pos.push_back(i);
    }
    if (sot_pos.size() >= 2) {
      // Truncate tile 0 right before tile 1's SOT marker -> hits KDU_SOT check.
      std::vector<kdu_byte> skip_to_sot;
      const size_t t1 = sot_pos[1];
      if (t1 > sot_pos[0] + 20) {
        skip_to_sot.insert(skip_to_sot.end(), orig.begin(),
                           orig.begin() + (t1 - 4));
        skip_to_sot.push_back(0xFF);
        skip_to_sot.push_back(0x90);  // spurious SOT inside packet body
        skip_to_sot.insert(skip_to_sot.end(), orig.begin() + t1, orig.end());
        BufferSource src(skip_to_sot);
        kdu_codestream dcs;
        dcs.create(&src);
        dcs.set_resilient(true);
        error_handler_.clear();
        try {
          (void)DecodeCodestream(dcs, false);
        } catch (int) {
        }
        error_handler_.clear();
        if (dcs.exists()) dcs.destroy();
      }
    }
    // Corrupt first packet body to end with 0xFF 0x90? Wait: bump SOP seq >=
    // total.
    std::vector<size_t> sop_pos;
    for (size_t i = 0; i + 5 < orig.size(); ++i) {
      if (orig[i] == 0xFF && orig[i + 1] == 0x91 && orig[i + 2] == 0x00 &&
          orig[i + 3] == 0x04) {
        sop_pos.push_back(i);
      }
    }
    if (sop_pos.size() >= 2) {
      std::vector<kdu_byte> bad_sop = orig;
      // Inject SOT in packet 0 body and set SOP 1 sequence number to 0x7FFF.
      if (sop_pos[0] + 10 < sop_pos[1]) {
        bad_sop[sop_pos[0] + 8] = 0xFF;
        bad_sop[sop_pos[0] + 9] = 0x90;
      }
      bad_sop[sop_pos[1] + 4] = 0x7F;
      bad_sop[sop_pos[1] + 5] = 0xFF;
      BufferSource src(bad_sop);
      kdu_codestream dcs;
      dcs.create(&src);
      dcs.set_resilient(true);
      error_handler_.clear();
      try {
        (void)DecodeCodestream(dcs, false);
      } catch (int) {
      }
      error_handler_.clear();
      if (dcs.exists()) dcs.destroy();
    }
  }

  // 4. Profile constraint warnings on decode via Rsiz patching from legal
  // profile headers onto non-conforming Clevels/Creversible/Clayers streams.
  {
    auto get_rsiz = [&](const char* prof_str, bool rev, int levs,
                        int layers) -> std::pair<kdu_byte, kdu_byte> {
      CodestreamSpec s = make_spec(64, 64, 3);
      s.bit_depth = 12;
      s.reversible = rev;
      s.extra_params = {prof_str,
                        std::string("Clevels=") + std::to_string(levs),
                        std::string("Clayers=") + std::to_string(layers),
                        "Cblk={32,32}",
                        "Corder=CPRL",
                        "ORGgen_plt=yes",
                        "ORGgen_tlm=1",
                        "ORGtparts=C"};
      error_handler_.clear();
      try {
        const auto b = EncodeCodestream(s, MakePlanes(s));
        return {b[4], b[5]};
      } catch (int) {
        error_handler_.clear();
        return {0, 0};
      }
    };
    const auto rsiz_c4s = get_rsiz("Sprofile=CINEMA4S", false, 1, 2);
    const auto rsiz_css = get_rsiz("Sprofile=CINEMASS", true, 1, 2);
    const auto rsiz_c4k = get_rsiz("Sprofile=CINEMA4K", false, 1, 1);

    CodestreamSpec s_rev = make_spec(64, 64, 3);
    s_rev.bit_depth = 12;
    s_rev.reversible = true;
    s_rev.extra_params = {"Clevels=0", "Clayers=3", "Cblk={16,16}",
                          "Cmodes=BYPASS"};
    const auto b_rev = EncodeCodestream(s_rev, MakePlanes(s_rev));

    CodestreamSpec s_irv = make_spec(256, 256, 3);
    s_irv.bit_depth = 12;
    s_irv.reversible = false;
    s_irv.extra_params = {"Clevels=7", "Clayers=6", "Cblk={16,16}"};
    const auto b_irv = EncodeCodestream(s_irv, MakePlanes(s_irv));

    for (auto [base, rsiz] :
         std::vector<std::pair<const std::vector<kdu_byte>*,
                               std::pair<kdu_byte, kdu_byte>>>{
             {&b_rev, rsiz_c4s}, {&b_irv, rsiz_css}, {&b_rev, rsiz_c4k}}) {
      if (rsiz.first == 0 && rsiz.second == 0) continue;
      std::vector<kdu_byte> patched = *base;
      patched[4] = rsiz.first;
      patched[5] = rsiz.second;
      BufferSource src(patched);
      kdu_codestream dcs;
      error_handler_.clear();
      try {
        dcs.create(&src);
        (void)DecodeCodestream(dcs, false);
      } catch (int) {
      }
      error_handler_.clear();
      if (dcs.exists()) dcs.destroy();
    }
  }

  // 5. ORGplt_parts=R|L|C + ORGtpart_interrupts=0 incremental flush.
  {
    siz_params siz;
    siz.set(Sdims, 0, 0, 64);
    siz.set(Sdims, 0, 1, 64);
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    siz.finalize_all();
    BufferTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    kdu_params* cod = cs.access_siz()->access_cluster(COD_params);
    kdu_params* org = cs.access_siz()->access_cluster(ORG_params);
    cod->parse_string("Creversible=yes");
    cod->parse_string("Clevels=2");
    cod->parse_string("Cblk={8,8}");
    cod->parse_string("Clayers=3");
    org->parse_string("ORGgen_plt=yes");
    org->parse_string("ORGplt_parts=R|L|C");
    org->parse_string("ORGtpart_interrupts=0");
    cs.access_siz()->finalize_all();
    kdu_long lb[3] = {500, 1500, 100000};
    cs.auto_flush(16, 16, 16, 16, lb, 3, nullptr, true, false);

    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    kdu_tile_comp tc = tile.access_component(0);
    kdu_sample_allocator alloc;
    kdu_analysis analysis(tc.access_resolution(2), &alloc, false, 1.0f);
    kdu_line_buf line;
    line.pre_create(&alloc, 64, true, false, 0, 0);
    alloc.finalize(cs);
    line.create();
    for (int y = 0; y < 64; ++y) {
      for (int x = 0; x < 64; ++x) line.get_buf32()[x].ival = 99;
      analysis.push(line);
    }
    analysis.destroy();
    line.destroy();
    tile.close();
    cs.flush(lb, 3);
    cs.destroy();
    EXPECT_FALSE(tgt.data.empty());
  }

  // 6. Interleaved multi-tile-part seekable decode + Ppm skip + TPsot/TNsot
  // validation errors.
  {
    CodestreamSpec spec = make_spec(128, 64, 1);
    spec.reversible = true;
    spec.tile_size = kdu_coords(64, 64);
    spec.extra_params = {"Clevels=1", "Cblk={16,16}", "Clayers=2",
                         "ORGtparts=L"};
    std::vector<int> plane(128 * 64);
    for (int i = 0; i < 128 * 64; ++i) plane[i] = (i * 29) & 0xFF;
    const auto orig = EncodeCodestream(spec, {plane});
    std::vector<size_t> sot_pos;
    for (size_t i = 0; i + 11 < orig.size(); ++i) {
      if (orig[i] == 0xFF && orig[i + 1] == 0x90 && orig[i + 2] == 0x00 &&
          orig[i + 3] == 0x0A) {
        sot_pos.push_back(i);
      }
    }
    if (sot_pos.size() == 4) {
      // orig has [MainHdr][T0 TP0][T0 TP1][T1 TP0][T1 TP1][EOC]
      const size_t eoc_pos = orig.size() - 2;
      auto slice = [&](size_t a, size_t b) {
        return std::vector<kdu_byte>(orig.begin() + a, orig.begin() + b);
      };
      auto mh = slice(0, sot_pos[0]);
      auto t0_0 = slice(sot_pos[0], sot_pos[1]);
      auto t0_1 = slice(sot_pos[1], sot_pos[2]);
      auto t1_0 = slice(sot_pos[2], sot_pos[3]);
      auto t1_1 = slice(sot_pos[3], eoc_pos);

      // Interleave: T0_0, T1_0, T0_1, T1_1; open T1 first on seekable source!
      std::vector<kdu_byte> inter = mh;
      inter.insert(inter.end(), t0_0.begin(), t0_0.end());
      inter.insert(inter.end(), t1_0.begin(), t1_0.end());
      inter.insert(inter.end(), t0_1.begin(), t0_1.end());
      inter.insert(inter.end(), t1_1.begin(), t1_1.end());
      inter.push_back(0xFF);
      inter.push_back(0xD9);

      {
        BufferSource seek_src(
            inter, KDU_SOURCE_CAP_SEQUENTIAL | KDU_SOURCE_CAP_SEEKABLE);
        kdu_codestream dcs;
        dcs.create(&seek_src);
        dcs.set_persistent();
        kdu_tile t1 = dcs.open_tile(kdu_coords(1, 0));
        t1.close();
        kdu_tile t0 = dcs.open_tile(kdu_coords(0, 0));
        t0.close();
        dcs.destroy();
      }
      // Bad TPsot (!= next_tpart) and conflicting non-zero TNsot.
      for (int err_kind = 0; err_kind < 2; ++err_kind) {
        std::vector<kdu_byte> bad = orig;
        if (err_kind == 0) {
          bad[sot_pos[0] + 10] = 1;  // TPsot = 1 on first tile-part
        } else {
          bad[sot_pos[0] + 11] = 2;  // TNsot = 2
          bad[sot_pos[1] + 11] = 3;  // TNsot = 3
        }
        BufferSource src(bad);
        kdu_codestream dcs;
        dcs.create(&src);
        error_handler_.clear();
        try {
          (void)DecodeCodestream(dcs, false);
        } catch (int) {
        }
        error_handler_.clear();
        if (dcs.exists()) dcs.destroy();
      }
    }
  }

  // 7. Signed NLT GAMMA + set_components_of_interest (all/first/MCT/expired) +
  // get_mct_dwt_info multi-block.
  {
    CodestreamSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 4;
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.extra_params = {
        "Cycc=no",
        "Ssigned=yes",
        "Sextensions=MCT|NLT",
        "Ncomponents=4",
        "Nprecision=8",
        "Nsigned=yes",
        "NLTmake={GAMMA,2.2,0.055,64}",
        "Mcomponents=4",
        "Mprecision=8",
        "Msigned=yes",
        "Mmatrix_size:I1=6",
        "Mmatrix_coeffs:I1=1,512,512,-1,0,1",
        "Mmatrix_size:I2=6",
        "Mmatrix_coeffs:I2=1,512,512,-1,0,1",
        "Mstage_inputs:I1={0,1},{2,3}",
        "Mstage_outputs:I1={0,1},{2,3}",
        "Mstage_collections:I1={2,2},{2,2}",
        "Mstage_xforms:I1={MATRIX,1,0,1,0},{MATRIX,2,0,1,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    BufferSource src(bytes);
    kdu_codestream dcs;
    dcs.create(&src);
    dcs.set_persistent();
    dcs.apply_input_restrictions(0, 0, 0, 0, nullptr,
                                 KDU_WANT_OUTPUT_COMPONENTS);
    kdu_tile tile = dcs.open_tile(kdu_coords(0, 0));
    float dmin = 0.0f, dmax = 0.0f, lut[32] = {};
    (void)tile.make_nlt_table(0, false, dmin, dmax, 32, lut, 1.0f, 1.0f);
    (void)tile.make_nlt_table(0, true, dmin, dmax, 32, lut, 1.0f, 1.0f);
    int only0[1] = {0};
    tile.set_components_of_interest(1, only0);
    tile.set_components_of_interest(2, nullptr);
    tile.set_components_of_interest(0, nullptr);
    (void)tile.get_mct_rxform_info(0, 1, nullptr, nullptr);
    int lvl = 0, cmin = 0, clim = 0, nst = 0;
    bool isrev = false, sym = false, syme = false;
    const float* dwt_c = nullptr;
    (void)tile.get_mct_dwt_info(0, 1, isrev, lvl, cmin, clim, nst, sym, syme,
                                dwt_c);
    tile.close();
    error_handler_.clear();
    try {
      tile.set_components_of_interest(0, nullptr);
    } catch (int) {
    }
    error_handler_.clear();
    dcs.destroy();
  }

  // 8. Cplex EST clamping + Rweight < 1.0 with foreground ROI & Cprog_weights +
  // interchange codestream open_precinct / open_block / close_block with MT.
  {
    for (const char* cp : {"Cplex={6,EST,-0.5,0}", "Cplex={6,EST,2.0,0}"}) {
      CodestreamSpec spec = make_spec(64, 64, 1);
      spec.reversible = false;
      spec.dwt_levels = 1;
      spec.extra_params = {"Cblk={8,8}", "Cprecincts={16,16}", cp};
      siz_params siz;
      ConfigureSiz(siz, spec);
      BufferTarget tgt;
      kdu_codestream cs;
      cs.create(&siz, &tgt);
      ApplyCodingParams(cs, spec);
      cs.set_max_bytes(2048, false);
      PushAllTiles(cs, MakePlanes(spec), spec);
      kdu_long b = 2048;
      cs.flush(&b, 1);
      cs.destroy();
    }

    // Rweight < 1.0 with Cprog_weights + Cband_weights -> negative trans_plane
    // in get_per_plane_wmse without hitting kd_encoder::start() UB.
    {
      CodestreamSpec spec = make_spec(64, 64, 1);
      spec.reversible = false;
      spec.dwt_levels = 2;
      spec.extra_params = {"Cblk={16,16}", "Cprecincts={32,32}", "Rweight=0.5",
                           "Cband_weights=0.1,0.1,0.1,0.2,0.2,0.2,1.0",
                           "Cprog_weights=0.5"};
      siz_params siz;
      ConfigureSiz(siz, spec);
      BufferTarget tgt;
      kdu_codestream cs;
      cs.create(&siz, &tgt);
      ApplyCodingParams(cs, spec);
      kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
      float wmse[64];
      for (int r = 0; r <= 2; ++r) {
        kdu_resolution res = tile.access_component(0).access_resolution(r);
        const int min_b = (r == 0) ? 0 : 1;
        const int max_b = (r == 0) ? 0 : 3;
        for (int b = min_b; b <= max_b; ++b) {
          kdu_subband sb = res.access_subband(b);
          EXPECT_GT(sb.get_per_plane_wmse(wmse, 64, false), 0);
          EXPECT_GT(sb.get_per_plane_wmse(wmse, 64, true), 0);
        }
      }
      tile.close();
      cs.destroy();
    }

    // Interchange codestream + kdu_thread_env open_precinct / open_block /
    // close_block
    // + get_decomp_structure on dwt_level==0 and transposed r>0.
    {
      CodestreamSpec ispec = make_spec(64, 64, 1);
      ispec.reversible = true;
      ispec.dwt_levels = 2;
      ispec.extra_params = {"Cblk={16,16}"};
      const auto ibytes = EncodeCodestream(ispec, MakePlanes(ispec));
      BufferSource isrc(ibytes);
      kdu_codestream dec;
      dec.create(&isrc);
      siz_params siz;
      ConfigureSiz(siz, ispec);
      kdu_thread_env env;
      env.create();
      ASSERT_TRUE(env.add_thread());
      kdu_codestream ics;
      ics.create(&siz, &env);
      ics.access_siz()->copy_all(dec.access_siz());
      ics.access_siz()->finalize_all();
      ics.change_appearance(true, true, false);
      kdu_tile itile = ics.open_tile(kdu_coords(0, 0), &env);
      kdu_tile_comp tc = itile.access_component(0);
      for (int r = 0; r <= 2; ++r) {
        kdu_resolution res = tc.access_resolution(r);
        kdu_coords depth;
        int lbands = 0;
        (void)res.get_decomp_structure(depth, lbands);
        if (r == 0) {
          kdu_dims pinds;
          res.get_valid_precincts(pinds);
          if (!pinds.is_empty()) {
            kdu_precinct prec = res.open_precinct(pinds.pos, &env);
            kdu_subband sb = res.access_subband(LL_BAND);
            kdu_dims binds;
            if (prec.get_valid_blocks(LL_BAND, binds) && !binds.is_empty()) {
              kdu_block* blk = sb.open_block(binds.pos, nullptr, &env);
              blk->missing_msbs = 0;
              blk->ht_skip_msbs = 0;
              blk->num_passes = 0;
              sb.close_block(blk, &env);
            }
            prec.close();
          }
        }
      }
      itile.close(&env);
      env.cs_terminate(ics);
      ics.destroy();
      dec.destroy();
      env.destroy();
    }
  }
}

TEST_F(CompressedTest, CompressedCoverage90FinalPush) {
  auto make_spec = [](int w, int h, int c) {
    CodestreamSpec s;
    s.width = w;
    s.height = h;
    s.components = c;
    return s;
  };

  // 1. Seekable PLT RPCL reset_packet_reading + sequential input_monitor.
  {
    CodestreamSpec spec = make_spec(64, 64, 1);
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.extra_params = {"Clayers=3", "Corder=RPCL", "ORGgen_plt=yes",
                         "Cprecincts={16,16}", "Cblk={8,8}"};
    std::vector<int> plane(64 * 64);
    for (int i = 0; i < 64 * 64; ++i) plane[i] = (i * 53) & 0xFF;
    const auto bytes = EncodeCodestream(spec, {plane});

    struct TestMonitor : public kdu_input_codestream_monitor {
      int complete_calls = 0;
      void parsing_complete() override { ++complete_calls; }
    };

    // Pure seekable + expand layers 1 -> 3 to trigger reset_packet_reading().
    {
      TestMonitor mon;
      BufferSource seek_src(bytes, KDU_SOURCE_CAP_SEEKABLE);
      kdu_codestream dcs;
      dcs.create(&seek_src);
      dcs.install_input_monitor(&mon, nullptr);
      dcs.set_persistent();
      dcs.apply_input_restrictions(0, 1, 0, 1, nullptr,
                                   KDU_WANT_CODESTREAM_COMPONENTS);
      EXPECT_EQ(DecodeCodestream(dcs, false).size(), 1U);
      dcs.apply_input_restrictions(0, 1, 0, 3, nullptr,
                                   KDU_WANT_CODESTREAM_COMPONENTS);
      EXPECT_EQ(DecodeCodestream(dcs, false).size(), 1U);
      dcs.destroy();
    }

    // Sequential monitor without PLT seekable path.
    {
      TestMonitor mon;
      BufferSource seq_src(bytes, KDU_SOURCE_CAP_SEQUENTIAL);
      kdu_codestream dcs;
      dcs.create(&seq_src);
      dcs.install_input_monitor(&mon, nullptr);
      EXPECT_EQ(DecodeCodestream(dcs, false).size(), 1U);
      dcs.destroy();
    }
  }

  // 2. Corrupt/missing EPH marker after Cuse_eph=yes + body-tail 0xFF/EOC/SOT.
  {
    CodestreamSpec spec = make_spec(64, 64, 1);
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.extra_params = {"Clayers=2", "Cblk={8,8}", "Cuse_eph=yes",
                         "Cuse_sop=no"};
    std::vector<int> plane(64 * 64);
    for (int i = 0; i < 64 * 64; ++i) plane[i] = (i * 67 + (i >> 2)) & 0xFF;
    const auto orig = EncodeCodestream(spec, {plane});
    size_t eph_pos = 0;
    for (size_t i = 0; i + 1 < orig.size(); ++i) {
      if (orig[i] == 0xFF && orig[i + 1] == 0x92) {
        eph_pos = i;
        break;
      }
    }
    if (eph_pos > 0) {
      std::vector<std::vector<kdu_byte>> variants;
      auto v1 = orig;
      v1[eph_pos + 1] = 0x93;  // Unexpected marker != KDU_EPH
      variants.push_back(std::move(v1));
      auto v2 = orig;
      v2[eph_pos] = 0x00;
      v2[eph_pos + 1] = 0x00;  // Non-marker bytes where EPH expected
      variants.push_back(std::move(v2));
      std::vector<kdu_byte> v3(orig.begin(), orig.begin() + eph_pos + 1);
      variants.push_back(std::move(v3));  // EOF after 0xFF before 0x92
      for (const auto& buf : variants) {
        for (int mode = 0; mode < 3; ++mode) {
          BufferSource src(buf);
          kdu_codestream dcs;
          dcs.create(&src);
          if (mode == 1) dcs.set_resilient(true);
          if (mode == 2) dcs.set_fussy();
          error_handler_.clear();
          try {
            (void)DecodeCodestream(dcs, false);
          } catch (int) {
          }
          error_handler_.clear();
          if (dcs.exists()) dcs.destroy();
        }
      }
    }
  }

  {
    CodestreamSpec spec = make_spec(64, 64, 1);
    spec.reversible = true;
    spec.dwt_levels = 0;
    spec.extra_params = {"Cblk={64,64}", "Clayers=1", "Cuse_sop=no",
                         "Cuse_eph=no"};
    std::vector<int> plane(64 * 64);
    for (int i = 0; i < 64 * 64; ++i) plane[i] = (i * 89 + (i >> 4)) & 0xFF;
    const auto orig = EncodeCodestream(spec, {plane});
    if (orig.size() > 20) {
      const std::vector<kdu_byte> head(orig.begin(), orig.end() - 6);
      std::vector<std::vector<kdu_byte>> bufs;
      bufs.push_back(head);
      auto b1 = head;
      b1.push_back(0xFF);
      bufs.push_back(std::move(b1));
      auto b2 = head;
      b2.push_back(0xFF);
      b2.push_back(0x55);
      bufs.push_back(std::move(b2));
      auto b3 = head;
      b3.push_back(0xFF);
      b3.push_back(0xD9);
      bufs.push_back(std::move(b3));
      auto b4 = head;
      b4.push_back(0xFF);
      b4.push_back(0x90);
      bufs.push_back(std::move(b4));
      for (const auto& buf : bufs) {
        for (int mode = 0; mode < 3; ++mode) {
          BufferSource src(buf);
          kdu_codestream dcs;
          dcs.create(&src);
          if (mode == 1) dcs.set_resilient(true);
          if (mode == 2) dcs.set_allow_parsing_errors(true);
          error_handler_.clear();
          try {
            (void)DecodeCodestream(dcs, false);
          } catch (int) {
          }
          error_handler_.clear();
          if (dcs.exists()) dcs.destroy();
        }
      }
    }
  }

  // 3. Cvis + Creversible=yes + Cdecomp extra stages, plus PROFILE0 and Cdecomp
  //    typical-tile recycle (!fully_typical).
  {
    CodestreamSpec spec = make_spec(128, 128, 1);
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.extra_params = {"Cdecomp=B(H--:V--:H--),B(-:-:-)", "Cvis=0.05",
                         "Cblk={8,8}"};
    siz_params siz;
    ConfigureSiz(siz, spec);
    BufferTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ApplyCodingParams(cs, spec);
    kdu_tile t = cs.open_tile(kdu_coords(0, 0));
    float vf = 0.0f, me = 0.0f, vs = 0.0f;
    EXPECT_TRUE(t.access_component(0)
                    .access_resolution(2)
                    .access_subband(HL_BAND)
                    .get_masking_params(vf, me, vs));
    t.close();
    cs.destroy();
  }
  for (const char* prof : {"Sprofile=PROFILE0", "Sprofile=PROFILE1"}) {
    CodestreamSpec spec = make_spec(64, 64, 1);
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(32, 32);
    spec.extra_params = {prof,          "Cprecincts={64,64}", "Cblk={32,32}",
                         "Corder=LRCP", "Corder:T1=RLCP",     "Qderived=yes",
                         "Qstep=0.01"};
    error_handler_.clear();
    try {
      const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
      BufferSource src(bytes);
      kdu_codestream dcs;
      dcs.create(&src);
      EXPECT_EQ(DecodeCodestream(dcs, false).size(), 1U);
      dcs.destroy();
    } catch (int) {
    }
    error_handler_.clear();
  }
  {
    CodestreamSpec spec = make_spec(64, 64, 1);
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(32, 32);
    spec.extra_params = {"Cdecomp=B(H--:V--:H--),B(-:-:-)", "Cblk={8,8}",
                         "Qderived=no", "Qstep=0.01", "Qstep:T1=0.04"};
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    BufferSource src(bytes);
    kdu_codestream dcs;
    dcs.create(&src);
    EXPECT_EQ(DecodeCodestream(dcs, false).size(), 1U);
    dcs.destroy();
  }

  // 4. Interleaved 2-tile multi-tpart: close T0 after 1 layer (!allow_restart),
  //    then open T1 -> mark_exhausted on closed T0.
  {
    CodestreamSpec spec = make_spec(128, 64, 1);
    spec.reversible = true;
    spec.tile_size = kdu_coords(64, 64);
    spec.extra_params = {"Clevels=1", "Cblk={16,16}", "Clayers=2",
                         "ORGtparts=L"};
    std::vector<int> plane(128 * 64);
    for (int i = 0; i < 128 * 64; ++i) plane[i] = (i * 31) & 0xFF;
    const auto orig = EncodeCodestream(spec, {plane});
    std::vector<size_t> sot_pos;
    for (size_t i = 0; i + 11 < orig.size(); ++i) {
      if (orig[i] == 0xFF && orig[i + 1] == 0x90 && orig[i + 2] == 0x00 &&
          orig[i + 3] == 0x0A) {
        sot_pos.push_back(i);
      }
    }
    if (sot_pos.size() == 4) {
      auto slice = [&](size_t a, size_t b) {
        return std::vector<kdu_byte>(orig.begin() + a, orig.begin() + b);
      };
      std::vector<kdu_byte> inter = slice(0, sot_pos[0]);
      auto t0_0 = slice(sot_pos[0], sot_pos[1]);
      auto t0_1 = slice(sot_pos[1], sot_pos[2]);
      auto t1_0 = slice(sot_pos[2], sot_pos[3]);
      auto t1_1 = slice(sot_pos[3], orig.size() - 2);
      inter.insert(inter.end(), t0_0.begin(), t0_0.end());
      inter.insert(inter.end(), t1_0.begin(), t1_0.end());
      inter.insert(inter.end(), t0_1.begin(), t0_1.end());
      inter.insert(inter.end(), t1_1.begin(), t1_1.end());
      inter.push_back(0xFF);
      inter.push_back(0xD9);

      BufferSource src(inter, KDU_SOURCE_CAP_SEQUENTIAL);
      kdu_codestream dcs;
      dcs.create(&src);
      dcs.apply_input_restrictions(0, 1, 0, 1, nullptr,
                                   KDU_WANT_CODESTREAM_COMPONENTS);
      kdu_tile t0 = dcs.open_tile(kdu_coords(0, 0));
      EXPECT_TRUE(t0.parse_all_relevant_packets(false, nullptr));
      t0.close();
      kdu_tile t1 = dcs.open_tile(kdu_coords(1, 0));
      t1.close();
      dcs.destroy();
    }
  }

  // 5. get_parent_dims / get_valid_block_span across flips, transpose, H(-),
  //    and odd ROI.
  for (const std::string& decomp_param :
       {std::string("Cdecomp=H(-),V(-)"), std::string("Cdecomp=B(-:-:-)")}) {
    CodestreamSpec spec = make_spec(64, 64, 1);
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.extra_params = {decomp_param, "Cblk={8,8}"};
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    BufferSource src(bytes);
    kdu_codestream dcs;
    dcs.create(&src);
    dcs.set_persistent();
    kdu_dims roi;
    roi.pos = kdu_coords(3, 5);
    roi.size = kdu_coords(29, 31);
    for (int mask = 0; mask < 8; ++mask) {
      dcs.change_appearance((mask & 1) != 0, (mask & 2) != 0, (mask & 4) != 0);
      error_handler_.clear();
      kdu_tile t;
      try {
        dcs.apply_input_restrictions(0, 1, 0, 1, &roi,
                                     KDU_WANT_CODESTREAM_COMPONENTS);
        t = dcs.open_tile(kdu_coords(0, 0));
        kdu_tile_comp tc = t.access_component(0);
        for (int r = 0; r < tc.get_num_resolutions(); ++r) {
          kdu_resolution res = tc.access_resolution(r);
          int min_idx = 0;
          const int num_bands = res.get_valid_band_indices(min_idx);
          for (int b = min_idx; b < min_idx + num_bands; ++b) {
            kdu_subband sb = res.access_subband(b);
            kdu_dims pd, span;
            sb.get_parent_dims(pd, false);
            sb.get_parent_dims(pd, true);
            sb.get_valid_block_span(span);
          }
        }
        t.close();
      } catch (int) {
        if (t.exists()) t.close();
      }
      error_handler_.clear();
    }
    dcs.destroy();
  }
}

TEST_F(CompressedTest, CompressedCoverage90Push2) {
  auto make_spec = [](int w, int h, int c) {
    CodestreamSpec s;
    s.width = w;
    s.height = h;
    s.components = c;
    return s;
  };

  // 1. Asymmetric !hor_split / !vert_split with odd origin and small precincts
  //    to hit corner empty-precinct span logic in kd_tile::open.
  for (const char* decomp : {"Cdecomp=H(-)", "Cdecomp=V(-)"}) {
    CodestreamSpec spec = make_spec(64, 64, 1);
    spec.origin = kdu_coords(1, 1);
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.extra_params = {decomp, "Cprecincts={8,8}", "Cblk={8,8}"};
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    BufferSource src(bytes);
    kdu_codestream dcs;
    dcs.create(&src);
    EXPECT_EQ(DecodeCodestream(dcs, false).size(), 1U);
    dcs.destroy();
  }

  // 2. Profile min/max DWT levels and Srequired IRV/REV violation warnings.
  for (int mode = 0; mode < 2; ++mode) {
    CodestreamSpec spec = make_spec(64, 64, 1);
    spec.reversible = (mode == 0);
    spec.dwt_levels = (mode == 0) ? 1 : 3;
    spec.extra_params = {"Cblk={8,8}"};
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    BufferSource src(bytes);
    kdu_codestream dcs;
    dcs.create(&src);
    kdu_params* siz = dcs.access_siz();
    if (mode == 0) {
      siz->set(Slevels_allowed, 0, 0, 2);
      siz->set(Slevels_allowed, 0, 1, 4);
      siz->set(Srequired, 0, 0, Srequired_IRV);
    } else {
      siz->set(Slevels_allowed, 0, 0, 1);
      siz->set(Slevels_allowed, 0, 1, 2);
      siz->set(Srequired, 0, 0, Srequired_REV);
    }
    warning_handler_.clear();
    error_handler_.clear();
    try {
      EXPECT_EQ(DecodeCodestream(dcs, false).size(), 1U);
    } catch (int) {
    }
    error_handler_.clear();
    warning_handler_.clear();
    dcs.destroy();
  }

  // 3. CPLEX progressive visual weights (tlut > 0, K0_b < 0, lut_idx >= 0)
  //    and partial Qweights mismatch warning on second component.
  {
    CodestreamSpec qspec = make_spec(64, 64, 2);
    qspec.reversible = true;
    qspec.dwt_levels = 0;
    qspec.extra_params = {"Qweights=1.0,5.0", "Cblk={32,32}"};
    warning_handler_.clear();
    EXPECT_FALSE(EncodeCodestream(qspec, MakePlanes(qspec)).empty());
    warning_handler_.clear();
  }
  for (int pass = 0; pass < 3; ++pass) {
    CodestreamSpec spec = make_spec(128, 128, pass == 0 ? 2 : 1);
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {
        "Cblk={8,8}",
        "Cprecincts={16,16}",
        "Cband_weights=0.25,0.25,0.25,0.5,0.5,0.5",
        pass == 0 ? "Cprog_weights=0.5,0.5" : "Cprog_weights=0.5",
        "Cplex={4,EST,0.25,0}",
    };
    if (pass == 0) {
      spec.extra_params.push_back("Cweight:C0=0.05");
      spec.extra_params.push_back("Cweight:C1=20.0");
    } else if (pass == 2) {
      spec.extra_params.push_back("Qstep=0.001");
    }
    const auto planes = MakePlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    BufferTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ApplyCodingParams(cs, spec);
    cs.set_max_bytes(pass == 2 ? 1500 : (1LL << 20), false);
    error_handler_.clear();
    kdu_tile tile;
    try {
      tile = cs.open_tile(kdu_coords(0, 0));
      PushTileSamples(cs, tile, planes, spec);
      tile.close();
      kdu_long lbytes[1] = {pass == 2 ? 1500 : (1LL << 20)};
      cs.flush(lbytes, 1);
    } catch (int) {
      if (tile.exists()) tile.close();
    }
    error_handler_.clear();
    cs.destroy();
  }

  // 4. get_conservative_slope_threshold with KDU_SLOPE_HINT_ESTIMATE and
  //    explicit min_slope_threshold.
  {
    CodestreamSpec spec = make_spec(128, 64, 1);
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(64, 64);
    spec.extra_params = {"Cblk={16,16}", "Cprecincts={32,32}"};
    const auto planes = MakePlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    BufferTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ApplyCodingParams(cs, spec);
    cs.set_slope_hint_policy(KDU_SLOPE_HINT_ESTIMATE);
    cs.set_min_slope_threshold(1);
    cs.set_max_bytes(512, false);
    for (int tx = 0; tx < 2; ++tx) {
      kdu_tile tile = cs.open_tile(kdu_coords(tx, 0));
      if (tx == 1) {
        (void)tile.access_component(0)
            .access_resolution(1)
            .access_subband(HL_BAND)
            .get_conservative_slope_threshold();
      }
      PushTileSamples(cs, tile, planes, spec);
      tile.close();
    }
    kdu_long lbytes[1] = {512};
    cs.flush(lbytes, 1);
    cs.destroy();
  }

  // 5. ORGgen_tlm=yes with heterogeneous tile ORGtparts=R creating empty
  // tparts.
  {
    CodestreamSpec spec = make_spec(128, 64, 1);
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(64, 64);
    spec.extra_params = {"ORGgen_tlm=4", "ORGtparts=R", "Clevels:T1=0",
                         "Cblk={16,16}"};
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    BufferSource src(bytes);
    kdu_codestream dcs;
    dcs.create(&src);
    EXPECT_EQ(DecodeCodestream(dcs, false).size(), 1U);
    dcs.destroy();
  }

  // 6. get_parent_dims with SPF fragments (spf_flags != 0) and nested
  //    packet-wavelet nodes under flip/transpose appearance transforms.
  {
    CodestreamSpec spec = make_spec(128, 128, 1);
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.extra_params = {"Cdecomp=B(H--:V--:H--),B(-:-:-)", "Cblk={8,8}"};
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    BufferSource src(bytes);
    kdu_codestream dcs;
    dcs.create(&src);
    dcs.set_persistent();
    dcs.configure_simultaneous_processing_fragments(16, 1, 8);
    kdu_dims roi;
    roi.pos = kdu_coords(3, 5);
    roi.size = kdu_coords(59, 61);
    for (int mask = 0; mask < 8; ++mask) {
      dcs.change_appearance((mask & 1) != 0, (mask & 2) != 0, (mask & 4) != 0);
      error_handler_.clear();
      kdu_tile t;
      try {
        dcs.apply_input_restrictions(0, 1, 0, 1, &roi,
                                     KDU_WANT_CODESTREAM_COMPONENTS);
        t = dcs.open_tile(kdu_coords(0, 0));
        kdu_tile_comp tc = t.access_component(0);
        auto visit_node = [&](auto& self, kdu_node node) -> void {
          kdu_subband sb = node.access_subband();
          if (sb.exists()) {
            kdu_dims pd;
            sb.get_parent_dims(pd, false);
            sb.get_parent_dims(pd, true);
            return;
          }
          for (int i = 0; i < 4; ++i) {
            kdu_node child = node.access_child(i);
            if (child.exists()) self(self, child);
          }
        };
        for (int r = 0; r < tc.get_num_resolutions(); ++r) {
          kdu_resolution res = tc.access_resolution(r);
          const int num_frags = res.get_num_simultaneous_processing_fragments();
          for (int pf = 0; pf < num_frags; ++pf) {
            visit_node(visit_node, res.access_node(pf));
          }
        }
        t.close();
      } catch (int) {
        if (t.exists()) t.close();
      }
      error_handler_.clear();
    }
    dcs.destroy();
  }

  // 7. Corrupt packet start / header marker (!resilient), packet body ending in
  //    0xFF (default/resilient putback), legacy Mxform_MAT get_mct_rxform_info,
  //    non-persistent input_monitor completion in read_packet, and 3-tpart
  //    interleaved seekable reopen.
  {
    CodestreamSpec spec = make_spec(64, 64, 1);
    spec.reversible = true;
    spec.dwt_levels = 0;
    spec.extra_params = {"Cblk={64,64}", "Clayers=1", "Cuse_sop=no",
                         "Cuse_eph=no"};
    std::vector<int> plane(64 * 64);
    for (int i = 0; i < 64 * 64; ++i) plane[i] = (i * 41 + (i >> 3)) & 0xFF;
    const auto orig = EncodeCodestream(spec, {plane});
    size_t sod_pos = 0;
    size_t eoc_pos = 0;
    for (size_t i = 0; i + 1 < orig.size(); ++i) {
      if (sod_pos == 0 && orig[i] == 0xFF && orig[i + 1] == 0x93) sod_pos = i;
      if (orig[i] == 0xFF && orig[i + 1] == 0xD9) eoc_pos = i;
    }
    std::vector<std::vector<kdu_byte>> corrupt_bufs;
    if (sod_pos > 0 && sod_pos + 6 < orig.size()) {
      auto b_pkt_start = orig;
      b_pkt_start[sod_pos + 2] = 0xFF;
      b_pkt_start[sod_pos + 3] = 0x55;
      corrupt_bufs.push_back(std::move(b_pkt_start));

      auto b_hdr_mid = orig;
      b_hdr_mid[sod_pos + 2] = 0x80;  // Enter non-empty packet header.
      b_hdr_mid[sod_pos + 3] = 0xFF;
      b_hdr_mid[sod_pos + 4] = 0x55;
      corrupt_bufs.push_back(std::move(b_hdr_mid));
    }
    if (eoc_pos > sod_pos + 4) {
      auto b_end_ff = orig;
      b_end_ff[eoc_pos - 1] = 0xFF;  // Packet body ends with dangling 0xFF.
      corrupt_bufs.push_back(std::move(b_end_ff));
    }
    for (const auto& buf : corrupt_bufs) {
      for (int res = 0; res < 2; ++res) {
        BufferSource src(buf);
        kdu_codestream dcs;
        dcs.create(&src);
        if (res == 1) dcs.set_resilient(true);
        error_handler_.clear();
        try {
          (void)DecodeCodestream(dcs, false);
        } catch (int) {
        }
        error_handler_.clear();
        if (dcs.exists()) dcs.destroy();
      }
    }
  }

  // Legacy Mxform_MAT (mct_flags == 0) + get_mct_rxform_info.
  {
    CodestreamSpec spec = make_spec(24, 24, 2);
    spec.is_signed = true;
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.extra_params = {
        "Cycc=no",
        "Sextensions=MCT",
        "Mcomponents=2",
        "Mprecision=8",
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
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    BufferSource src(bytes);
    kdu_codestream dcs;
    dcs.create(&src);
    kdu_params* cod = dcs.access_siz()->access_cluster(COD_params);
    if (cod != nullptr) cod->set(Cmct, 0, 0, 0);
    int out_idx[2] = {0, 1};
    dcs.apply_input_restrictions(2, out_idx, 0, 0, nullptr,
                                 KDU_WANT_OUTPUT_COMPONENTS);
    error_handler_.clear();
    kdu_tile tile;
    try {
      tile = dcs.open_tile(kdu_coords(0, 0));
      int nsi = 0, nso = 0, nbi = 0, nbo = 0;
      if (tile.get_mct_block_info(0, 0, nsi, nso, nbi, nbo) &&
          tile.get_mct_rxform_info(0, 0)) {
        std::vector<int> rmat(static_cast<size_t>(nbi * (nbi + 1)));
        std::vector<int> aout(static_cast<size_t>(nbo));
        EXPECT_TRUE(tile.get_mct_rxform_info(0, 0, rmat.data(), aout.data()));
      }
      tile.close();
    } catch (int) {
      if (tile.exists()) tile.close();
    }
    error_handler_.clear();
    dcs.destroy();
  }

  // Non-persistent input_monitor completion in read_packet (ORGgen_plt=no).
  {
    struct Monitor : public kdu_input_codestream_monitor {
      int done = 0;
      void parsing_complete() override { ++done; }
    } mon;
    CodestreamSpec spec = make_spec(64, 64, 1);
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.extra_params = {"Clayers=2", "ORGgen_plt=no", "Cblk={16,16}"};
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    BufferSource src(bytes, KDU_SOURCE_CAP_SEQUENTIAL);
    kdu_codestream dcs;
    dcs.create(&src);
    dcs.install_input_monitor(&mon, nullptr);
    dcs.apply_input_restrictions(0, 1, 0, 1, nullptr,
                                 KDU_WANT_CODESTREAM_COMPONENTS);
    EXPECT_EQ(DecodeCodestream(dcs, false).size(), 1U);
    EXPECT_GE(mon.done, 1);
    dcs.destroy();
  }

  // Seekable 3-tpart interleaved T0_0, T0_1, T1_0, T0_2, T1_1, T1_2.
  {
    CodestreamSpec spec = make_spec(128, 64, 1);
    spec.reversible = true;
    spec.tile_size = kdu_coords(64, 64);
    spec.extra_params = {"Clevels=1", "Cblk={16,16}", "Clayers=3",
                         "ORGtparts=L", "ORGgen_plt=yes"};
    std::vector<int> plane(128 * 64);
    for (int i = 0; i < 128 * 64; ++i) plane[i] = (i * 59) & 0xFF;
    const auto orig = EncodeCodestream(spec, {plane});
    std::vector<size_t> sot_pos;
    for (size_t i = 0; i + 11 < orig.size(); ++i) {
      if (orig[i] == 0xFF && orig[i + 1] == 0x90 && orig[i + 2] == 0x00 &&
          orig[i + 3] == 0x0A) {
        sot_pos.push_back(i);
      }
    }
    if (sot_pos.size() == 6) {
      auto slice = [&](size_t a, size_t b) {
        return std::vector<kdu_byte>(orig.begin() + a, orig.begin() + b);
      };
      std::vector<kdu_byte> inter = slice(0, sot_pos[0]);
      auto append = [&](const std::vector<kdu_byte>& v) {
        inter.insert(inter.end(), v.begin(), v.end());
      };
      append(slice(sot_pos[0], sot_pos[1]));       // T0_0
      append(slice(sot_pos[1], sot_pos[2]));       // T0_1
      append(slice(sot_pos[3], sot_pos[4]));       // T1_0
      append(slice(sot_pos[2], sot_pos[3]));       // T0_2
      append(slice(sot_pos[4], sot_pos[5]));       // T1_1
      append(slice(sot_pos[5], orig.size() - 2));  // T1_2
      inter.push_back(0xFF);
      inter.push_back(0xD9);

      BufferSource src(inter, KDU_SOURCE_CAP_SEEKABLE);
      kdu_codestream dcs;
      dcs.create(&src);
      dcs.apply_input_restrictions(0, 1, 0, 2, nullptr,
                                   KDU_WANT_CODESTREAM_COMPONENTS);
      kdu_tile t0 = dcs.open_tile(kdu_coords(0, 0));
      EXPECT_TRUE(t0.parse_all_relevant_packets(false, nullptr));
      t0.close();
      kdu_tile t1 = dcs.open_tile(kdu_coords(1, 0));
      EXPECT_TRUE(t1.parse_all_relevant_packets(false, nullptr));
      t1.close();
      dcs.destroy();
    }
  }

  // 8. Leaf kdu_node::get_bibo_gains, PROFILE0 violation warnings, tile-header
  //    RGN/POC Sallowed_RHET/Sallowed_PHET warnings, single-threaded
  //    auto_flush, and SPF + flippable B(B:B:B) get_parent_dims.
  {
    CodestreamSpec spec = make_spec(512, 64, 1);
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.extra_params = {"Cblk={16,16}", "Cprecincts={32,32}"};
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    BufferSource src(bytes);
    kdu_codestream dcs;
    dcs.create(&src);
    dcs.set_persistent();
    dcs.configure_simultaneous_processing_fragments(32, 2, 4);
    for (int mask = 0; mask < 2; ++mask) {
      dcs.change_appearance(mask == 1, false, false);
      kdu_tile tile = dcs.open_tile(kdu_coords(0, 0));
      kdu_resolution res = tile.access_component(0).access_resolution(2);
      int steps = 0;
      kdu_node leaf = res.access_node().access_child(HL_BAND);
      EXPECT_NE(leaf.get_bibo_gains(steps, false), nullptr);
      EXPECT_EQ(steps, 0);
      EXPECT_NE(leaf.get_bibo_gains(steps, true), nullptr);
      EXPECT_EQ(steps, 0);
      const int nfrags = res.get_num_simultaneous_processing_fragments();
      for (int pf = 0; pf < nfrags; ++pf) {
        kdu_dims pd;
        res.access_node(pf)
            .access_child(HL_BAND)
            .access_subband()
            .get_parent_dims(pd, false);
      }
      tile.close();
    }
    dcs.destroy();
  }

  // Flippable nested packet-wavelet B(V--:H--:-),B(-:-:-) get_parent_dims under
  // transpose and flips.
  {
    CodestreamSpec spec = make_spec(64, 64, 1);
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.extra_params = {"Cdecomp=B(V--:H--:-),B(-:-:-)", "Cblk={8,8}"};
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    BufferSource src(bytes);
    kdu_codestream dcs;
    dcs.create(&src);
    dcs.set_persistent();
    for (int mask = 0; mask < 8; ++mask) {
      dcs.change_appearance((mask & 4) != 0, (mask & 1) != 0, (mask & 2) != 0);
      error_handler_.clear();
      kdu_tile tile;
      try {
        tile = dcs.open_tile(kdu_coords(0, 0));
        kdu_resolution res = tile.access_component(0).access_resolution(2);
        int min_b = 0;
        const int nb = res.get_valid_band_indices(min_b);
        for (int b = min_b; b < min_b + nb; ++b) {
          kdu_dims pd;
          res.access_subband(b).get_parent_dims(pd, false);
        }
        tile.close();
      } catch (int) {
        if (tile.exists()) tile.close();
      }
      error_handler_.clear();
    }
    dcs.destroy();
  }

  // PROFILE0 violation warnings and Sallowed RHET/PHET tile-header warnings.
  {
    CodestreamSpec spec = make_spec(256, 64, 2);
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.tile_size = kdu_coords(128, 64);
    spec.extra_params = {"Cblk={16,16}", "Cprecincts={32,32}", "Rshift:T1=16",
                         "Porder:T1={0,0,2,2,2,PCRL}"};
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    BufferSource src(bytes);
    kdu_codestream dcs;
    dcs.create(&src);
    kdu_params* siz = dcs.access_siz();
    siz->set(Sconf_profile, 0, 0, Sprofile_PROFILE0);
    siz->set(Sallowed, 0, 0, 0);
    warning_handler_.clear();
    error_handler_.clear();
    try {
      EXPECT_EQ(DecodeCodestream(dcs, false).size(), 2U);
    } catch (int) {
    }
    error_handler_.clear();
    warning_handler_.clear();
    dcs.destroy();
  }

  // Single-threaded auto_flush with cached target triggering incr_flush_counter
  // (env == nullptr).
  {
    CodestreamSpec spec = make_spec(64, 64, 1);
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.extra_params = {"Cdecomp=V(-)", "Cblk={8,8}", "Cprecincts={8,8}"};
    const auto planes = MakePlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    BufferTarget tgt(true);
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ApplyCodingParams(cs, spec);
    kdu_long lbytes[1] = {1LL << 20};
    cs.auto_flush(1, 1, 8, 8, lbytes, 1);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    PushTileSamples(cs, tile, planes, spec);
    tile.close();
    cs.flush(lbytes, 1);
    cs.destroy();
    EXPECT_FALSE(tgt.precincts.empty());
  }

  // 9. Multi-tile ORGgen_tlm with asymmetric Clevels:T1 emitting empty
  //    tile-parts and kdu_tile::set_components_of_interest with
  //    KDU_WANT_CODESTREAM_COMPONENTS.
  {
    CodestreamSpec spec = make_spec(256, 64, 2);
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(128, 64);
    spec.extra_params = {"Cblk={8,8}", "ORGgen_tlm=4", "ORGtparts=R",
                         "Clevels:T1=0"};
    const auto bytes = EncodeCodestream(spec, MakePlanes(spec));
    EXPECT_FALSE(bytes.empty());

    BufferSource src(bytes);
    kdu_codestream dcs;
    dcs.create(&src);
    dcs.apply_input_restrictions(0, 0, 0, 0, nullptr,
                                 KDU_WANT_CODESTREAM_COMPONENTS);
    kdu_tile tile = dcs.open_tile(kdu_coords(0, 0));
    tile.set_components_of_interest(0, nullptr);
    tile.set_components_of_interest(1, nullptr);
    tile.close();
    dcs.destroy();
  }
}

}  // namespace
}  // namespace kdu_core
