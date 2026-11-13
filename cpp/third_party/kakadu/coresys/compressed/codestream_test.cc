// Unit tests for third_party/kakadu/vendor/coresys/compressed/codestream.cpp
// via the public coresys headers in third_party/kakadu/vendor/coresys/common/.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_arch.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_params.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_roi_processing.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_sample_processing.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_threads.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

namespace kdu_core {
namespace {

class CapturingMessageHandler : public kdu_thread_safe_message {
 public:
  explicit CapturingMessageHandler(bool throw_on_end)
      : throw_on_end_(throw_on_end) {}
  ~CapturingMessageHandler() noexcept override = default;

  void put_text(const char* string) override {
    if (string != nullptr) {
      std::lock_guard<std::mutex> lock(text_mutex_);
      current_ += string;
    }
  }

  void flush(bool end_of_message = false) override {
    if (end_of_message) {
      std::string finished;
      {
        std::lock_guard<std::mutex> lock(text_mutex_);
        finished = std::move(current_);
        current_.clear();
        history_.push_back(finished);
      }
      kdu_thread_safe_message::flush(true);
      if (throw_on_end_) {
        std::fprintf(stderr, "KDU_ERR: %s\n", finished.c_str());
        throw KDU_ERROR_EXCEPTION;
      }
      return;
    }
    kdu_thread_safe_message::flush(false);
  }

  std::vector<std::string> history() const {
    std::lock_guard<std::mutex> lock(text_mutex_);
    return history_;
  }

  void clear() {
    std::lock_guard<std::mutex> lock(text_mutex_);
    history_.clear();
    current_.clear();
  }

 private:
  const bool throw_on_end_;
  mutable std::mutex text_mutex_;
  std::string current_;
  std::vector<std::string> history_;
};

class CapturingPlainMessage : public kdu_message {
 public:
  void put_text(const char* string) override {
    if (string != nullptr) {
      text_ += string;
    }
  }
  const std::string& text() const { return text_; }

 private:
  std::string text_;
};

kdu_dims MakeDims(int x, int y, int w, int h) {
  kdu_dims dims;
  dims.pos = kdu_coords(x, y);
  dims.size = kdu_coords(w, h);
  return dims;
}

class CodestreamTest : public ::testing::Test {
 protected:
  void SetUp() override {
    error_handler_.clear();
    warning_handler_.clear();
    kdu_customize_errors(&error_handler_);
    kdu_customize_warnings(&warning_handler_);
  }

  void TearDown() override {
    kdu_customize_errors(nullptr);
    kdu_customize_warnings(nullptr);
  }

  CapturingMessageHandler error_handler_{true};
  CapturingMessageHandler warning_handler_{false};
};

class MemoryTarget : public kdu_compressed_target {
 public:
  int get_capabilities() override { return capabilities; }
  kdu_membroker* get_membroker() override { return broker; }
  bool prefer_large_writes() const override { return prefer_large; }

  void set_target_size(kdu_long num_bytes) override {
    reported_target_size = num_bytes;
    target_size_calls++;
  }

  bool start_rewrite(kdu_long backtrack) override {
    if (!allow_rewrite || rewriting || backtrack < 0 ||
        static_cast<size_t>(backtrack) > write_pos) {
      return false;
    }
    rewriting = true;
    rewrite_calls++;
    saved_pos = write_pos;
    write_pos -= static_cast<size_t>(backtrack);
    return true;
  }

  bool end_rewrite() override {
    if (!rewriting) {
      return false;
    }
    write_pos = saved_pos;
    rewriting = false;
    return true;
  }

  bool write(const kdu_byte* buf, int num_bytes) override {
    if (num_bytes <= 0) {
      return num_bytes == 0;
    }
    if (fail_after_bytes >= 0 &&
        static_cast<kdu_long>(bytes_written + num_bytes) > fail_after_bytes) {
      return false;
    }
    if (rewriting) {
      if (write_pos + static_cast<size_t>(num_bytes) > saved_pos) {
        return false;
      }
      std::memcpy(data.data() + write_pos, buf, static_cast<size_t>(num_bytes));
      write_pos += static_cast<size_t>(num_bytes);
      bytes_written += num_bytes;
      return true;
    }
    if (write_pos + static_cast<size_t>(num_bytes) > data.size()) {
      data.resize(write_pos + static_cast<size_t>(num_bytes));
    }
    std::memcpy(data.data() + write_pos, buf, static_cast<size_t>(num_bytes));
    write_pos += static_cast<size_t>(num_bytes);
    bytes_written += num_bytes;
    return true;
  }

  bool close() override {
    closed = true;
    return true;
  }

  std::vector<kdu_byte> data;
  size_t write_pos = 0;
  size_t saved_pos = 0;
  bool rewriting = false;
  bool allow_rewrite = true;
  bool prefer_large = true;
  bool closed = false;
  int capabilities = KDU_TARGET_CAP_SEQUENTIAL;
  kdu_membroker* broker = nullptr;
  kdu_long reported_target_size = -1;
  int target_size_calls = 0;
  int rewrite_calls = 0;
  kdu_long fail_after_bytes = -1;
  kdu_long bytes_written = 0;
};

class StructuredCacheTarget : public kdu_compressed_target {
 public:
  int get_capabilities() override { return KDU_TARGET_CAP_CACHED; }
  kdu_membroker* get_membroker() override { return broker; }

  void start_mainheader() override {
    active = &main_header;
    main_header.clear();
  }
  void end_mainheader() override { active = nullptr; }

  void start_tileheader(int tnum, int num_tiles) override {
    total_tiles = num_tiles;
    active = &tile_headers[tnum];
    active->clear();
  }
  void end_tileheader(int tnum) override {
    (void)tnum;
    active = nullptr;
  }

  void start_precinct(kdu_long unique_id) override {
    active = &precincts[unique_id];
    active->clear();
  }
  void end_precinct(kdu_long unique_id, int num_packets,
                    const kdu_long packet_lengths[]) override {
    active = nullptr;
    auto& lens = precinct_packet_lengths[unique_id];
    lens.assign(packet_lengths, packet_lengths + num_packets);
  }

  bool write(const kdu_byte* buf, int num_bytes) override {
    if (active == nullptr || num_bytes < 0) {
      return false;
    }
    active->insert(active->end(), buf, buf + num_bytes);
    return true;
  }

  bool close() override {
    // kd_compressed_output::end_mainheader() omits flush(), so with uniform
    // tile headers the buffered main header flushes into tile 0's scope.
    if (main_header.empty() && tile_headers.contains(0)) {
      main_header.swap(tile_headers[0]);
    }
    closed = true;
    return true;
  }

  kdu_membroker* broker = nullptr;
  int total_tiles = 0;
  bool closed = false;
  std::vector<kdu_byte> main_header;
  std::map<int, std::vector<kdu_byte>> tile_headers;
  std::map<kdu_long, std::vector<kdu_byte>> precincts;
  std::map<kdu_long, std::vector<kdu_long>> precinct_packet_lengths;
  std::vector<kdu_byte>* active = nullptr;
};

class MemorySource : public kdu_compressed_source {
 public:
  MemorySource(const kdu_byte* ptr, size_t len, int caps)
      : data_(ptr, ptr + len), caps_(caps) {}

  explicit MemorySource(const std::vector<kdu_byte>& buf,
                        int caps = KDU_SOURCE_CAP_SEQUENTIAL |
                                   KDU_SOURCE_CAP_SEEKABLE |
                                   KDU_SOURCE_CAP_IN_MEMORY)
      : data_(buf), caps_(caps) {}

  int get_capabilities() override { return caps_; }
  kdu_membroker* get_membroker() override { return broker; }

  int read(kdu_byte* buf, int num_bytes) override {
    if (pos_ >= data_.size() || num_bytes <= 0) {
      return 0;
    }
    const size_t avail = data_.size() - pos_;
    size_t to_copy = std::min(avail, static_cast<size_t>(num_bytes));
    if (max_chunk_ > 0) {
      to_copy = std::min(to_copy, static_cast<size_t>(max_chunk_));
    }
    std::memcpy(buf, data_.data() + pos_, to_copy);
    pos_ += to_copy;
    return static_cast<int>(to_copy);
  }

  bool seek(kdu_long offset) override {
    if (fail_seek || (caps_ & KDU_SOURCE_CAP_SEEKABLE) == 0 || offset < 0) {
      return false;
    }
    pos_ = std::min(static_cast<size_t>(offset), data_.size());
    return true;
  }

  kdu_long get_pos() override {
    if ((caps_ & KDU_SOURCE_CAP_SEEKABLE) == 0) {
      return -1;
    }
    return static_cast<kdu_long>(pos_);
  }

  kdu_byte* access_memory(kdu_long& pos, kdu_byte*& lim) override {
    if ((caps_ & KDU_SOURCE_CAP_IN_MEMORY) == 0 || data_.empty()) {
      return nullptr;
    }
    pos = static_cast<kdu_long>(pos_);
    lim = data_.data() + data_.size();
    return data_.data() + pos_;
  }

  bool close() override {
    closed = true;
    return true;
  }

  kdu_membroker* broker = nullptr;
  int max_chunk_ = 0;
  bool closed = false;
  bool fail_seek = false;

 private:
  std::vector<kdu_byte> data_;
  int caps_;
  size_t pos_ = 0;
};

class StructuredCacheSource : public kdu_compressed_source {
 public:
  explicit StructuredCacheSource(const StructuredCacheTarget& cache)
      : cache_(cache), current_(&cache_.main_header) {}

  int get_capabilities() override {
    return KDU_SOURCE_CAP_CACHED | KDU_SOURCE_CAP_SEEKABLE;
  }

  bool set_tileheader_scope(int tnum, int num_tiles) override {
    if (num_tiles != cache_.total_tiles || !hide_tiles.empty()) {
      for (int hidden : hide_tiles) {
        if (hidden == tnum) {
          return false;
        }
      }
    }
    auto it = cache_.tile_headers.find(tnum);
    if (it == cache_.tile_headers.end()) {
      return false;
    }
    current_ = &it->second;
    pos_ = 0;
    return true;
  }

  bool set_precinct_scope(kdu_long unique_id) override {
    auto it = cache_.precincts.find(unique_id);
    if (it == cache_.precincts.end()) {
      current_ = &empty_;
      pos_ = 0;
      return true;
    }
    current_ = &it->second;
    pos_ = 0;
    return true;
  }

  int read(kdu_byte* buf, int num_bytes) override {
    if (current_ == nullptr || pos_ >= current_->size() || num_bytes <= 0) {
      return 0;
    }
    const size_t to_copy =
        std::min(current_->size() - pos_, static_cast<size_t>(num_bytes));
    std::memcpy(buf, current_->data() + pos_, to_copy);
    pos_ += to_copy;
    return static_cast<int>(to_copy);
  }

  bool seek(kdu_long offset) override {
    if (offset < 0 || current_ == nullptr) {
      return false;
    }
    pos_ = std::min(static_cast<size_t>(offset), current_->size());
    return true;
  }

  kdu_long get_pos() override { return static_cast<kdu_long>(pos_); }

  std::vector<int> hide_tiles;

 private:
  const StructuredCacheTarget& cache_;
  const std::vector<kdu_byte>* current_;
  const std::vector<kdu_byte> empty_;
  size_t pos_ = 0;
};

class TestMembackedSource : public kdu_membacked_compressed_source {
 public:
  TestMembackedSource(const std::vector<kdu_byte>& payload, size_t step_bytes)
      : payload_(payload), step_bytes_(step_bytes) {}

  bool enable_stream(int mode, const kdu_byte* desc, size_t desc_len,
                     kdu_uint32 interp, double ts, size_t min_buf,
                     size_t max_buf) {
    pos_ = 0;
    return kdu_membacked_compressed_source::enable(mode, desc, desc_len, interp,
                                                   ts, min_buf, max_buf);
  }

  bool run_service(size_t& total_fetched, kdu_int32& term_code) {
    return kdu_membacked_compressed_source::service(total_fetched, term_code);
  }

  bool run_open(bool auto_enable) {
    return kdu_membacked_compressed_source::open(auto_enable);
  }

  void run_reset() { kdu_membacked_compressed_source::reset(); }

 protected:
  kdu_long fetch_data(kdu_long max_bytes, kdu_byte* buffer,
                      bool blocking) override {
    (void)blocking;
    if (pos_ >= payload_.size()) {
      finish_fetching(forced_term_code_);
      return 0;
    }
    size_t want =
        std::min(payload_.size() - pos_, static_cast<size_t>(max_bytes));
    if (step_bytes_ > 0) {
      want = std::min(want, step_bytes_);
    }
    std::memcpy(buffer, payload_.data() + pos_, want);
    pos_ += want;
    if (truncate_early_ && pos_ >= payload_.size()) {
      finish_fetching(forced_term_code_);
    }
    return static_cast<kdu_long>(want);
  }

 public:
  kdu_int32 forced_term_code_ = 0;
  bool truncate_early_ = false;

 private:
  std::vector<kdu_byte> payload_;
  size_t step_bytes_;
  size_t pos_ = 0;
};

class RecordingInputMonitor : public kdu_input_codestream_monitor {
 public:
  void installing_monitor() override { installed++; }
  void uninstalling_monitor() override { uninstalled++; }
  void restarting() override { restarted++; }
  void parsing_complete() override { parsed++; }

  int installed = 0;
  int uninstalled = 0;
  int restarted = 0;
  int parsed = 0;
};

class BoxRoiImage;

class BoxRoiNode : public kdu_roi_node {
 public:
  BoxRoiNode(int width, int height, kdu_dims active_box,
             BoxRoiImage* owner = nullptr)
      : width_(width),
        height_(height),
        active_box_(active_box),
        owner_(owner) {}

  void pull(kdu_byte buf[], int width) override {
    if (row_ >= height_) {
      std::memset(buf, 0, static_cast<size_t>(width));
      return;
    }
    const bool in_y = row_ >= active_box_.pos.y &&
                      row_ < active_box_.pos.y + active_box_.size.y;
    for (int x = 0; x < width && x < width_; ++x) {
      const bool in_x =
          x >= active_box_.pos.x && x < active_box_.pos.x + active_box_.size.x;
      buf[x] = (in_y && in_x) ? 1 : 0;
    }
    row_++;
  }

  void release() override;
  void DetachOwner() { owner_ = nullptr; }

 private:
  int width_;
  int height_;
  kdu_dims active_box_;
  BoxRoiImage* owner_;
  int row_ = 0;
};

class BoxRoiImage : public kdu_roi_image {
 public:
  explicit BoxRoiImage(kdu_dims active_box) : active_box_(active_box) {}
  ~BoxRoiImage() override {
    for (BoxRoiNode* node : active_nodes_) {
      node->DetachOwner();
      delete node;
    }
  }

  kdu_roi_node* acquire_node(int comp_idx, kdu_dims tile_region) override {
    (void)comp_idx;
    kdu_dims local = active_box_;
    local.pos.x -= tile_region.pos.x;
    local.pos.y -= tile_region.pos.y;
    auto* node =
        new BoxRoiNode(tile_region.size.x, tile_region.size.y, local, this);
    active_nodes_.push_back(node);
    return node;
  }

  void ReleaseNode(BoxRoiNode* node) {
    active_nodes_.erase(
        std::remove(active_nodes_.begin(), active_nodes_.end(), node),
        active_nodes_.end());
  }

 private:
  kdu_dims active_box_;
  std::vector<BoxRoiNode*> active_nodes_;
};

void BoxRoiNode::release() {
  if (owner_ != nullptr) {
    owner_->ReleaseNode(this);
  }
  delete this;
}

struct ImageSpec {
  int width = 32;
  int height = 32;
  int components = 1;
  int bit_depth = 8;
  bool is_signed = false;
  bool reversible = true;
  int dwt_levels = 2;
  kdu_coords tile_size = kdu_coords(0, 0);
  kdu_coords img_origin = kdu_coords(0, 0);
  kdu_coords tile_origin = kdu_coords(0, 0);
  std::vector<kdu_coords> subsampling;
  std::vector<std::string> extra_params;
};

std::vector<std::vector<kdu_int32>> MakeTestPlanes(const ImageSpec& spec) {
  std::vector<std::vector<kdu_int32>> planes(spec.components);
  const kdu_int32 min_val = spec.is_signed ? -(1 << (spec.bit_depth - 1)) : 0;
  const kdu_int32 span = (1 << spec.bit_depth);
  for (int c = 0; c < spec.components; ++c) {
    const kdu_coords sub = (c < static_cast<int>(spec.subsampling.size()))
                               ? spec.subsampling[c]
                               : kdu_coords(1, 1);
    const int w = (spec.width + sub.x - 1) / sub.x;
    const int h = (spec.height + sub.y - 1) / sub.y;
    planes[c].resize(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const kdu_int32 val =
            ((x * 13 + y * 29 + c * 47 + (x * y) % 19) % span) + min_val;
        planes[c][static_cast<size_t>(y) * w + x] = val;
      }
    }
  }
  return planes;
}

void WriteLineSample(kdu_line_buf* line, int x, kdu_int32 raw, int bits,
                     bool is_signed) {
  const double centered =
      is_signed
          ? static_cast<double>(raw)
          : static_cast<double>(raw) - static_cast<double>(1LL << (bits - 1));
  if (line->is_absolute()) {
    const auto val = static_cast<kdu_int64>(std::llround(centered));
    if (line->get_buf16() != nullptr) {
      line->get_buf16()[x].ival = static_cast<kdu_int16>(val);
    } else if (line->get_buf32() != nullptr) {
      line->get_buf32()[x].ival = static_cast<kdu_int32>(val);
    } else if (line->get_buf64() != nullptr) {
      line->get_buf64()[x].ival = val;
    }
    return;
  }
  const double norm = centered / static_cast<double>(1LL << bits);
  if (line->get_buf32() != nullptr) {
    line->get_buf32()[x].fval = static_cast<float>(norm);
  } else if (line->get_buf16() != nullptr) {
    line->get_buf16()[x].ival =
        static_cast<kdu_int16>(std::llround(norm * (1 << KDU_FIX_POINT)));
  }
}

kdu_int32 ReadLineSample(kdu_line_buf* line, int x, int bits, bool is_signed) {
  double centered = 0.0;
  if (line->is_absolute()) {
    if (line->get_buf16() != nullptr) {
      centered = static_cast<double>(line->get_buf16()[x].ival);
    } else if (line->get_buf32() != nullptr) {
      centered = static_cast<double>(line->get_buf32()[x].ival);
    } else if (line->get_buf64() != nullptr) {
      centered = static_cast<double>(line->get_buf64()[x].ival);
    }
  } else if (line->get_buf32() != nullptr) {
    centered = static_cast<double>(line->get_buf32()[x].fval) *
               static_cast<double>(1LL << bits);
  } else if (line->get_buf16() != nullptr) {
    centered = (static_cast<double>(line->get_buf16()[x].ival) /
                static_cast<double>(1 << KDU_FIX_POINT)) *
               static_cast<double>(1LL << bits);
  }
  const double offset =
      is_signed ? 0.0 : static_cast<double>(1LL << (bits - 1));
  const auto rounded = static_cast<kdu_int64>(std::llround(centered + offset));
  const kdu_int64 lo = is_signed ? -(1LL << (bits - 1)) : 0;
  const kdu_int64 hi =
      is_signed ? ((1LL << (bits - 1)) - 1) : ((1LL << bits) - 1);
  return static_cast<kdu_int32>(std::clamp(rounded, lo, hi));
}

void PushTileData(kdu_codestream cs, kdu_tile tile,
                  const std::vector<std::vector<kdu_int32>>& planes,
                  const ImageSpec& spec, kdu_roi_image* roi = nullptr,
                  int flags = KDU_MULTI_XFORM_DEFAULT_FLAGS) {
  const int num_comps = tile.get_num_components();
  kdu_multi_analysis engine;
  engine.create(cs, tile, nullptr, nullptr, flags, roi);
  std::vector<int> next_row(num_comps, 0);
  std::vector<kdu_dims> comp_dims(num_comps);
  for (int c = 0; c < num_comps; ++c) {
    cs.get_tile_dims(tile.get_tile_idx(), c, comp_dims[c], true);
  }
  bool any_left = true;
  while (any_left) {
    any_left = false;
    bool progressed = false;
    for (int c = 0; c < num_comps; ++c) {
      const kdu_coords sz = engine.get_size(c);
      if (next_row[c] >= sz.y) {
        continue;
      }
      any_left = true;
      kdu_line_buf* line = engine.exchange_line(c, nullptr);
      if (line == nullptr) {
        continue;
      }
      const kdu_coords sub = (c < static_cast<int>(spec.subsampling.size()))
                                 ? spec.subsampling[c]
                                 : kdu_coords(1, 1);
      const int comp_w = (spec.width + sub.x - 1) / sub.x;
      const int x0 =
          comp_dims[c].pos.x - ((spec.img_origin.x + sub.x - 1) / sub.x);
      const int y0 =
          comp_dims[c].pos.y - ((spec.img_origin.y + sub.y - 1) / sub.y);
      const int y = y0 + next_row[c];
      for (int x = 0; x < sz.x; ++x) {
        WriteLineSample(line, x,
                        planes[c][static_cast<size_t>(y) * comp_w + x0 + x],
                        spec.bit_depth, spec.is_signed);
      }
      engine.exchange_line(c, line);
      next_row[c]++;
      progressed = true;
    }
    if (any_left && !progressed) {
      break;
    }
  }
  engine.destroy();
}

bool IsSizExtraParam(const std::string& param) {
  auto starts_with = [&](const char* prefix) {
    return param.rfind(prefix, 0) == 0;
  };
  return starts_with("S") || starts_with("Mcomponents") ||
         starts_with("Mprecision") || starts_with("Msigned") ||
         starts_with("Ncomponents") || starts_with("Nprecision") ||
         starts_with("Nsigned");
}

void ConfigureSiz(siz_params& siz, const ImageSpec& spec) {
  siz.set(Scomponents, 0, 0, spec.components);
  siz.set(Ssize, 0, 0, spec.height + spec.img_origin.y);
  siz.set(Ssize, 0, 1, spec.width + spec.img_origin.x);
  if (spec.img_origin.x != 0 || spec.img_origin.y != 0) {
    siz.set(Sorigin, 0, 0, spec.img_origin.y);
    siz.set(Sorigin, 0, 1, spec.img_origin.x);
  }
  if (spec.tile_size.x > 0 && spec.tile_size.y > 0) {
    siz.set(Stiles, 0, 0, spec.tile_size.y);
    siz.set(Stiles, 0, 1, spec.tile_size.x);
  }
  if (spec.tile_origin.x != 0 || spec.tile_origin.y != 0) {
    siz.set(Stile_origin, 0, 0, spec.tile_origin.y);
    siz.set(Stile_origin, 0, 1, spec.tile_origin.x);
  }
  for (int c = 0; c < spec.components; ++c) {
    siz.set(Sprecision, c, 0, spec.bit_depth);
    siz.set(Ssigned, c, 0, spec.is_signed);
    const kdu_coords sub = (c < static_cast<int>(spec.subsampling.size()))
                               ? spec.subsampling[c]
                               : kdu_coords(1, 1);
    siz.set(Ssampling, c, 0, sub.y);
    siz.set(Ssampling, c, 1, sub.x);
  }
  for (const std::string& p : spec.extra_params) {
    if (IsSizExtraParam(p)) {
      ASSERT_TRUE(siz.parse_string(p.c_str())) << "Failed SIZ param: " << p;
    }
  }
  static_cast<kdu_params*>(&siz)->finalize();
}

void ConfigureCodestreamParams(kdu_codestream cs, const ImageSpec& spec) {
  siz_params* params = cs.access_siz();
  ASSERT_NE(params, nullptr);
  auto has_param = [&](const char* prefix) {
    for (const std::string& p : spec.extra_params) {
      if (p.rfind(prefix, 0) == 0) {
        return true;
      }
    }
    return false;
  };
  if (!has_param("Creversible=")) {
    ASSERT_TRUE(params->parse_string(spec.reversible ? "Creversible=yes"
                                                     : "Creversible=no"));
  }
  if (!has_param("Ckernels=")) {
    ASSERT_TRUE(params->parse_string(spec.reversible ? "Ckernels=W5X3"
                                                     : "Ckernels=W9X7"));
  }
  if (!has_param("Clevels=")) {
    const std::string levels = "Clevels=" + std::to_string(spec.dwt_levels);
    ASSERT_TRUE(params->parse_string(levels.c_str()));
  }
  for (const std::string& p : spec.extra_params) {
    if (!p.empty() && !IsSizExtraParam(p)) {
      ASSERT_TRUE(params->parse_string(p.c_str()))
          << "Failed codestream param: " << p;
    }
  }
  params->finalize_all();
}

std::vector<kdu_byte> EncodeImage(
    const ImageSpec& spec, const std::vector<std::vector<kdu_int32>>& planes,
    kdu_compressed_target* custom_target = nullptr, kdu_long max_bytes = 0,
    bool flush_at_end = true, int target_caps = KDU_TARGET_CAP_SEQUENTIAL) {
  siz_params siz;
  ConfigureSiz(siz, spec);

  MemoryTarget local_target;
  local_target.capabilities = target_caps;
  kdu_compressed_target* target =
      custom_target != nullptr ? custom_target : &local_target;

  kdu_codestream cs;
  try {
    cs.create(&siz, target);
    ConfigureCodestreamParams(cs, spec);
    if (max_bytes > 0) {
      cs.set_max_bytes(max_bytes, false, false);
    }
    kdu_dims tile_range;
    cs.get_valid_tiles(tile_range);
    for (int ty = 0; ty < tile_range.size.y; ++ty) {
      for (int tx = 0; tx < tile_range.size.x; ++tx) {
        kdu_tile tile = cs.open_tile(
            kdu_coords(tile_range.pos.x + tx, tile_range.pos.y + ty));
        PushTileData(cs, tile, planes, spec);
        tile.close();
      }
    }
    if (flush_at_end) {
      kdu_long layer_bytes[1] = {max_bytes};
      cs.flush(max_bytes > 0 ? layer_bytes : nullptr, 1);
    }
    cs.destroy();
  } catch (...) {
    if (cs.exists()) cs.destroy();
    target->close();
    throw;
  }
  target->close();
  return local_target.data;
}

std::vector<std::vector<kdu_int32>> DecodeFullImage(
    kdu_compressed_source* src) {
  kdu_codestream cs;
  std::vector<std::vector<kdu_int32>> out;
  try {
    cs.create(src);
    const int num_comps = cs.get_num_components(true);
    out.resize(num_comps);
    std::vector<kdu_dims> full_comp_dims(num_comps);
    for (int c = 0; c < num_comps; ++c) {
      cs.get_dims(c, full_comp_dims[c], true);
      out[c].resize(static_cast<size_t>(full_comp_dims[c].size.x) *
                    full_comp_dims[c].size.y);
    }

    kdu_dims valid_tiles;
    cs.get_valid_tiles(valid_tiles);
    for (int ty = 0; ty < valid_tiles.size.y; ++ty) {
      for (int tx = 0; tx < valid_tiles.size.x; ++tx) {
        kdu_tile tile;
        kdu_multi_synthesis engine;
        try {
          tile = cs.open_tile(
              kdu_coords(valid_tiles.pos.x + tx, valid_tiles.pos.y + ty));
          engine.create(cs, tile, nullptr, nullptr,
                        KDU_MULTI_XFORM_DEFAULT_FLAGS);
          std::vector<int> next_row(num_comps, 0);
          std::vector<kdu_dims> tile_comp_dims(num_comps);
          for (int c = 0; c < num_comps; ++c) {
            cs.get_tile_dims(tile.get_tile_idx(), c, tile_comp_dims[c], true);
          }
          bool any_left = true;
          while (any_left) {
            any_left = false;
            bool progressed = false;
            for (int c = 0; c < num_comps; ++c) {
              const kdu_coords sz = engine.get_size(c);
              if (next_row[c] >= sz.y) {
                continue;
              }
              any_left = true;
              kdu_line_buf* line = engine.get_line(c);
              if (line == nullptr) {
                continue;
              }
              const int bits = cs.get_bit_depth(c, true);
              const bool is_signed = cs.get_signed(c, true);
              const int x0 = tile_comp_dims[c].pos.x - full_comp_dims[c].pos.x;
              const int y0 = tile_comp_dims[c].pos.y - full_comp_dims[c].pos.y;
              const int dst_y = y0 + next_row[c];
              const int dst_w = full_comp_dims[c].size.x;
              for (int x = 0; x < sz.x; ++x) {
                out[c][static_cast<size_t>(dst_y) * dst_w + x0 + x] =
                    ReadLineSample(line, x, bits, is_signed);
              }
              next_row[c]++;
              progressed = true;
            }
            if (any_left && !progressed) {
              break;
            }
          }
          engine.destroy();
          tile.close();
        } catch (...) {
          if (engine.exists()) engine.destroy();
          if (tile.exists()) tile.close();
          throw;
        }
      }
    }
    cs.destroy();
  } catch (...) {
    if (cs.exists()) cs.destroy();
    src->close();
    throw;
  }
  src->close();
  return out;
}

TEST_F(CodestreamTest, CoreVersionAndAlignmentChecker) {
  EXPECT_STREQ(kdu_get_core_version(), KDU_CORE_VERSION);
  EXPECT_TRUE(kdu_core_sample_alignment_checker(
      KDU_OVERREAD_BYTES, KDU_PREALIGN_BYTES, KDU_ALIGN_SAMPLES16,
      KDU_ALIGN_SAMPLES32, KDU_ALIGN_SAMPLES64, true, false));
  EXPECT_FALSE(kdu_core_sample_alignment_checker(
      KDU_OVERREAD_BYTES + 1, KDU_PREALIGN_BYTES, KDU_ALIGN_SAMPLES16,
      KDU_ALIGN_SAMPLES32, KDU_ALIGN_SAMPLES64, true, true));
  EXPECT_THROW(
      kdu_core_sample_alignment_checker(
          KDU_OVERREAD_BYTES, KDU_PREALIGN_BYTES + 1, KDU_ALIGN_SAMPLES16,
          KDU_ALIGN_SAMPLES32, KDU_ALIGN_SAMPLES64, false, true),
      kdu_exception);
}

TEST_F(CodestreamTest, CodestreamCommentsTextBinaryAndLarge) {
  ImageSpec spec;
  spec.width = 16;
  spec.height = 16;
  const auto planes = MakeTestPlanes(spec);
  siz_params siz;
  ConfigureSiz(siz, spec);

  MemoryTarget target;
  kdu_codestream cs;
  cs.create(&siz, &target);
  ConfigureCodestreamParams(cs, spec);

  kdu_codestream_comment c1 = cs.add_comment();
  ASSERT_TRUE(c1.exists());
  EXPECT_FALSE(c1.check_readonly());
  c1 << "hdr:" << '!' << -7 << " " << 42U << " " << kdu_long{123456} << " "
     << short{-3} << " " << static_cast<unsigned short>(9) << " " << 1.5F << " "
     << 2.25;
  c1.put_text(" end");

  kdu_codestream_comment c2 = cs.add_comment();
  ASSERT_TRUE(c2.exists());
  std::vector<kdu_byte> large_bin(70000);
  for (size_t i = 0; i < large_bin.size(); ++i) {
    large_bin[i] = static_cast<kdu_byte>((i * 7 + 3) & 0xFF);
  }
  EXPECT_TRUE(
      c2.put_data(large_bin.data(), static_cast<int>(large_bin.size())));

  kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
  PushTileData(cs, tile, planes, spec);
  tile.close();
  cs.set_disabled_auto_comments(0);
  cs.flush(nullptr, 1, nullptr, true, true);
  cs.destroy();
  // Adding comment after destroy returns empty interface.
  EXPECT_FALSE(cs.add_comment().exists());

  MemorySource src(target.data);
  kdu_codestream in_cs;
  in_cs.create(&src);

  std::vector<std::string> texts;
  bool found_binary = false;
  kdu_codestream_comment cur = in_cs.get_comment();
  while (cur.exists()) {
    EXPECT_TRUE(cur.check_readonly());
    const char* txt = cur.get_text();
    ASSERT_NE(txt, nullptr);
    if (txt[0] != '\0') {
      texts.emplace_back(txt);
    } else {
      constexpr int kMaxComBytes = 65530;
      std::vector<kdu_byte> read_back(large_bin.size() + 16);
      EXPECT_EQ(cur.get_data(nullptr, 1000, 100), 100);
      const int total_read =
          cur.get_data(read_back.data(), 0, static_cast<int>(read_back.size()));
      if (total_read == kMaxComBytes &&
          std::memcmp(read_back.data(), large_bin.data(), kMaxComBytes) == 0) {
        found_binary = true;
      }
    }
    cur = in_cs.get_comment(cur);
  }
  EXPECT_FALSE(texts.empty());
  EXPECT_TRUE(found_binary);
  const bool found_text =
      std::any_of(texts.begin(), texts.end(), [](const std::string& s) {
        return s.find("hdr:!-7 42 0123456 -3 9") != std::string::npos;
      });
  std::string joined_texts;
  for (const auto& t : texts) {
    joined_texts += "[" + t + "] ";
  }
  EXPECT_TRUE(found_text) << "texts=" << joined_texts;
  in_cs.destroy();
}

TEST_F(CodestreamTest, MultiTileSubsampledQueryAndAppearanceTransforms) {
  ImageSpec spec;
  spec.width = 48;
  spec.height = 40;
  spec.components = 3;
  spec.bit_depth = 8;
  spec.reversible = true;
  spec.dwt_levels = 2;
  spec.tile_size = kdu_coords(24, 20);
  spec.img_origin = kdu_coords(4, 2);
  spec.tile_origin = kdu_coords(0, 0);
  spec.subsampling = {kdu_coords(1, 1), kdu_coords(2, 2), kdu_coords(2, 1)};
  spec.extra_params = {"CRGoffset={0.0,0.0},{0.25,0.25},{0.5,0.0}",
                       "Cprecincts={16,16}"};
  const auto planes = MakeTestPlanes(spec);
  const auto bytes = EncodeImage(spec, planes);

  MemorySource src(bytes);
  kdu_codestream cs;
  cs.create(&src);
  cs.set_persistent();
  CapturingPlainMessage text_log;
  cs.set_textualization(&text_log, false);
  EXPECT_FALSE(text_log.text().empty());

  EXPECT_EQ(cs.get_num_components(false), 3);
  EXPECT_EQ(cs.get_bit_depth(0), 8);
  EXPECT_FALSE(cs.get_signed(0));
  kdu_coords sub1;
  cs.get_subsampling(1, sub1);
  EXPECT_EQ(sub1, kdu_coords(2, 2));
  kdu_coords min_sub;
  cs.get_min_subsampling(0, min_sub);
  EXPECT_EQ(min_sub, kdu_coords(1, 1));
  kdu_coords reg;
  cs.get_registration(1, kdu_coords(4, 4), reg);
  kdu_coords rel_reg;
  cs.get_relative_registration(0, 1, kdu_coords(4, 4), rel_reg);
  EXPECT_GE(reg.x, -8);
  EXPECT_LE(std::abs(rel_reg.x), 8);
  EXPECT_TRUE(cs.can_flip(false));
  EXPECT_FALSE(cs.cbr_flushing());
  EXPECT_EQ(cs.get_min_dwt_levels(), 2);
  EXPECT_EQ(cs.get_max_tile_layers(), 1);

  kdu_dims full_dims;
  cs.get_dims(-1, full_dims);
  EXPECT_EQ(full_dims.size, kdu_coords(48, 40));
  kdu_dims tile_part;
  cs.get_tile_partition(tile_part);
  kdu_coords tidx;
  EXPECT_FALSE(cs.find_tile(-1, kdu_coords(30, 25), tidx));
  EXPECT_TRUE(cs.find_tile(0, kdu_coords(30, 25), tidx));
  kdu_dims full_comp0;
  cs.get_full_dims(0, full_comp0);
  EXPECT_EQ(full_comp0.size, kdu_coords(48, 40));

  for (int mask = 0; mask < 8; ++mask) {
    cs.change_appearance((mask & 1) != 0, (mask & 2) != 0, (mask & 4) != 0);
    bool tr = false;
    bool vf = false;
    bool hf = false;
    cs.get_appearance(tr, vf, hf);
    EXPECT_EQ(tr, (mask & 1) != 0);
    EXPECT_EQ(vf, (mask & 2) != 0);
    EXPECT_EQ(hf, (mask & 4) != 0);
    EXPECT_TRUE(cs.can_flip(true));
    kdu_dims mapped_hi;
    kdu_dims comp_reg;
    cs.get_dims(0, comp_reg);
    comp_reg.size.x /= 2;
    comp_reg.size.y /= 2;
    cs.map_region(0, comp_reg, mapped_hi);
    EXPECT_GT(mapped_hi.area(), 0);
  }
  cs.change_appearance(false, false, false);

  kdu_quality_limiter q_lim(0.2F, true);
  q_lim.set_display_resolution(96.0F, 96.0F);
  for (int c = 0; c < 3; ++c) {
    q_lim.set_comp_info(c, 1.0F, 1 << c);
  }
  EXPECT_LT(q_lim.get_square_visual_weight(0, 0, kdu_coords(1, 1), false, true),
            0.0F);
  EXPECT_GT(q_lim.get_square_visual_weight(1, 1, kdu_coords(2, 2), true, false),
            0.0F);

  int comp_indices[3] = {2, 0, 2};
  kdu_dims roi = MakeDims(4, 2, 24, 20);
  cs.apply_input_restrictions(3, comp_indices, 1, 1, &roi,
                              KDU_WANT_CODESTREAM_COMPONENTS, nullptr, &q_lim);
  int dl = 0;
  int ml = 0;
  kdu_component_access_mode am = KDU_WANT_CODESTREAM_COMPONENTS;
  const int rflags = cs.get_input_restrictions(dl, ml, am);
  EXPECT_NE(rflags & KDU_CS_RESTRICT_COMPONENTS, 0);
  EXPECT_NE(rflags & KDU_CS_RESTRICT_LEVELS, 0);
  EXPECT_NE(rflags & KDU_CS_RESTRICT_REGION, 0);
  EXPECT_EQ(dl, 1);
  EXPECT_EQ(cs.get_num_components(), 2);

  cs.apply_input_restrictions(0, 0, 0, 0, nullptr, KDU_WANT_OUTPUT_COMPONENTS);
  EXPECT_EQ(cs.get_num_components(), 3);
  cs.destroy();

  const auto decoded = DecodeFullImage(&src);
  EXPECT_EQ(decoded, planes);
}

TEST_F(CodestreamTest, ProgressionOrdersPocTlmPltSopEphAndNonSeekableSource) {
  const char* orders[] = {"LRCP", "RLCP", "RPCL", "PCRL", "CPRL"};
  for (const char* ord : orders) {
    ImageSpec spec;
    spec.width = 48;
    spec.height = 48;
    spec.components = 3;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(24, 24);
    spec.extra_params = {
        std::string("Corder=") + ord,
        "Clayers=3",
        "Cycc=yes",
        "Cuse_sop=yes",
        "Cuse_eph=yes",
        "ORGgen_tlm=20",
        "ORGgen_plt=yes",
        "ORGtparts=R",
        "Porder={0,0,1,2,3,LRCP},{0,0,3,3,3,RLCP}",
        "Cprecincts={16,16}",
        "Qderived=yes",
        "Qguard=1",
    };
    const auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);

    MemoryTarget target;
    target.prefer_large = false;
    kdu_codestream cs;
    cs.create(&siz, &target);
    ConfigureCodestreamParams(cs, spec);
    kdu_dims tr;
    cs.get_valid_tiles(tr);
    for (int ty = 0; ty < tr.size.y; ++ty) {
      for (int tx = 0; tx < tr.size.x; ++tx) {
        kdu_tile tile = cs.open_tile(kdu_coords(tr.pos.x + tx, tr.pos.y + ty));
        PushTileData(cs, tile, planes, spec);
        tile.close();
      }
    }
    EXPECT_TRUE(cs.ready_for_flush());
    kdu_long lbytes[3] = {800, 1600, 0};
    cs.flush(lbytes, 3, nullptr, true, true);
    EXPECT_GT(target.rewrite_calls, 0);
    EXPECT_GT(cs.get_total_bytes(false), cs.get_total_bytes(true));
    EXPECT_GT(cs.get_packet_bytes(), 0);
    EXPECT_GT(cs.get_packet_header_bytes(), 0);
    EXPECT_GT(cs.get_num_tparts(), 0);
    EXPECT_GT(cs.get_compressed_data_memory(), 0);
    EXPECT_GT(cs.get_compressed_state_memory(), 0);
    EXPECT_GT(cs.get_params_memory(), 0);
    cs.destroy();

    // Test seekable+in-memory decode first, then sequential-only non-seekable.
    MemorySource mem_src(target.data);
    const auto mem_decoded = DecodeFullImage(&mem_src);
    EXPECT_EQ(mem_decoded.size(), 3);

    MemorySource seq_src(target.data, KDU_SOURCE_CAP_SEQUENTIAL);
    seq_src.max_chunk_ = 37;
    const auto seq_decoded = DecodeFullImage(&seq_src);
    EXPECT_EQ(seq_decoded, mem_decoded);
  }
}

TEST_F(CodestreamTest, FlushModesAutoFlushCbrAndStatsExchange) {
  ImageSpec spec;
  spec.width = 64;
  spec.height = 64;
  spec.components = 3;
  spec.reversible = false;
  spec.dwt_levels = 3;
  spec.extra_params = {
      "Corder=RPCL",
      "Clayers=3",
      "Cprecincts={32,32},{16,16},{8,8},{4,4}",
      "Cycc=yes",
  };
  const auto planes = MakeTestPlanes(spec);

  // Explicit thresholds, threshold-hints, and combined thresholds+sizes modes.
  {
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget target;
    kdu_codestream cs;
    cs.create(&siz, &target);
    ConfigureCodestreamParams(cs, spec);
    cs.set_slope_hint_policy(0);
    cs.set_min_slope_threshold(35000);
    cs.set_max_slope_threshold(50000);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    PushTileData(cs, tile, planes, spec);
    tile.close();

    kdu_uint16 thresh[3] = {48000, 44000, 38000};
    kdu_long sizes[3] = {500, 1200, 3000};
    cs.flush(sizes, 3, thresh, true, true, 0.0, nullptr,
             KDU_FLUSH_USES_THRESHOLDS_AND_SIZES);
    cs.destroy();
    EXPECT_GT(target.data.size(), 0);
  }

  // Auto-flush with thresholds-are-hints + flush stats exchange + CBR flushing.
  spec.extra_params[0] = "Corder=PCRL";
  spec.extra_params[1] = "Clayers=1";
  spec.extra_params.push_back("Scbr={1,48}");
  spec.reversible = false;
  siz_params cbr_siz;
  ConfigureSiz(cbr_siz, spec);
  MemoryTarget t1;
  kdu_codestream cs1;
  cs1.create(&cbr_siz, &t1);
  ConfigureCodestreamParams(cs1, spec);
  EXPECT_TRUE(cs1.cbr_flushing());
  cs1.set_cbr_start_delay(4.0F);
  kdu_long cbr_bytes[1] = {4000};
  cs1.auto_flush(1, 1, 16, 16, cbr_bytes, 1, nullptr, true, false);
  kdu_flush_stats fstats = cs1.add_flush_stats(0, 5, 0, 1);
  EXPECT_TRUE(fstats.exists());
  fstats.set_max_passes(true, true, -1, 5);
  {
    kdu_tile tile = cs1.open_tile(kdu_coords(0, 0));
    PushTileData(cs1, tile, planes, spec);
    tile.close();
  }
  cs1.flush(cbr_bytes, 1, nullptr, true, false);

  float d = 0.0F;
  kdu_uint32 l = 0;
  float b = 0.0F;
  kdu_uint32 a = 0;
  kdu_uint32 s = 0;
  EXPECT_TRUE(cs1.get_cbr_model(d, l, b, a, s));
  float bmax = 0.0F;
  float bmean = 0.0F;
  float ifb = 0.0F;
  kdu_uint16 mins = 0;
  kdu_uint16 maxs = 0;
  float means = 0.0F;
  float ms2 = 0.0F;
  kdu_long fill = 0;
  EXPECT_GT(
      cs1.get_cbr_flush_stats(bmax, bmean, ifb, mins, maxs, means, ms2, fill),
      0);

  // Advance flush stats and attach to second frame codestream.
  EXPECT_GE(fstats.advance(1), 0);
  fstats.auto_advance(1);
  MemoryTarget t2;
  kdu_codestream cs2;
  cs2.create(&cbr_siz, &t2);
  ConfigureCodestreamParams(cs2, spec);
  kdu_flush_stats fstats2 = cs2.add_flush_stats(2, 5, 0, 1);
  EXPECT_TRUE(fstats2.exists());
  cs2.attach_flush_stats(fstats);
  {
    kdu_tile tile = cs2.open_tile(kdu_coords(0, 0));
    PushTileData(cs2, tile, planes, spec);
    tile.close();
  }
  cs2.flush(cbr_bytes, 1, nullptr, true, false);
  cs2.destroy();
  cs1.destroy();
}

TEST_F(CodestreamTest, RestartShareBufferingRoiAndWriteFailPolicies) {
  ImageSpec spec;
  spec.width = 32;
  spec.height = 32;
  spec.components = 1;
  spec.reversible = true;
  spec.dwt_levels = 2;
  spec.extra_params = {"Rshift=16", "Rlevels=2"};
  const auto planes = MakeTestPlanes(spec);
  siz_params siz;
  ConfigureSiz(siz, spec);

  // Test restart on output codestream with ROI.
  MemoryTarget tgt1;
  MemoryTarget tgt2;
  kdu_codestream out_cs;
  out_cs.create(&siz, &tgt1);
  ConfigureCodestreamParams(out_cs, spec);
  out_cs.enable_restart();
  out_cs.configure_simultaneous_processing_fragments(0, 0, 0);
  {
    BoxRoiImage roi(MakeDims(4, 4, 16, 16));
    kdu_tile tile = out_cs.open_tile(kdu_coords(0, 0));
    PushTileData(out_cs, tile, planes, spec, &roi);
    tile.close();
    out_cs.flush(nullptr, 1);
  }
  out_cs.restart(&tgt2);
  {
    kdu_tile tile = out_cs.open_tile(kdu_coords(0, 0));
    PushTileData(out_cs, tile, planes, spec);
    tile.close();
    out_cs.flush(nullptr, 1);
  }
  out_cs.destroy();
  EXPECT_GT(tgt1.data.size(), 0);
  EXPECT_GT(tgt2.data.size(), 0);

  // Test share_buffering, restart on input codestream, and input monitor.
  RecordingInputMonitor monitor1;
  RecordingInputMonitor monitor2;
  kdu_codestream uncreated;
  EXPECT_FALSE(uncreated.install_input_monitor(&monitor1, nullptr));
  TestMembackedSource mb_src1(tgt1.data, 0);
  ASSERT_TRUE(mb_src1.enable_stream(KDU_MEMBACKED_SOURCE_UNSERVICED, nullptr, 0,
                                    0, 0.0, 0, 0));
  ASSERT_TRUE(mb_src1.run_open(false));
  MemorySource src2(tgt2.data);
  kdu_codestream in_a;
  kdu_codestream in_b;
  in_a.create(&mb_src1);
  in_b.create(&src2);
  EXPECT_TRUE(in_b.install_input_monitor(&monitor1, nullptr));
  EXPECT_FALSE(in_b.install_input_monitor(nullptr, nullptr));
  in_b.share_buffering(in_a);
  in_a.enable_restart();
  EXPECT_TRUE(in_a.install_input_monitor(&monitor1, nullptr));
  EXPECT_TRUE(in_a.install_input_monitor(&monitor2, nullptr));
  EXPECT_EQ(monitor1.uninstalled, 2);
  {
    kdu_tile tile = in_a.open_tile(kdu_coords(0, 0));
    tile.close();
  }
  TestMembackedSource mb_src3(tgt1.data, 0);
  ASSERT_TRUE(mb_src3.enable_stream(KDU_MEMBACKED_SOURCE_UNSERVICED, nullptr, 0,
                                    0, 0.0, 0, 0));
  ASSERT_TRUE(mb_src3.run_open(false));
  in_a.restart(&mb_src3);
  EXPECT_EQ(monitor2.restarted, 1);
  EXPECT_FALSE(in_a.install_input_monitor(nullptr, nullptr));
  in_b.destroy();
  in_a.destroy();
  mb_src1.close();
  mb_src3.close();

  // Test write failure policies.
  MemoryTarget fail_err;
  fail_err.fail_after_bytes = 20;
  kdu_codestream cs_err;
  cs_err.create(&siz, &fail_err);
  ConfigureCodestreamParams(cs_err, spec);
  cs_err.set_write_fail_policy(KDU_WRITE_FAIL_POLICY_ERROR);
  {
    kdu_tile tile = cs_err.open_tile(kdu_coords(0, 0));
    PushTileData(cs_err, tile, planes, spec);
    tile.close();
  }
  EXPECT_THROW(cs_err.flush(nullptr, 1), kdu_exception);
  cs_err.set_write_fail_policy(KDU_WRITE_FAIL_POLICY_IGNORE);
  cs_err.destroy();

  MemoryTarget fail_throw;
  fail_throw.fail_after_bytes = 20;
  kdu_codestream cs_throw;
  cs_throw.create(&siz, &fail_throw);
  ConfigureCodestreamParams(cs_throw, spec);
  cs_throw.set_write_fail_policy(KDU_WRITE_FAIL_POLICY_THROW);
  {
    kdu_tile tile = cs_throw.open_tile(kdu_coords(0, 0));
    PushTileData(cs_throw, tile, planes, spec);
    tile.close();
  }
  EXPECT_THROW(cs_throw.flush(nullptr, 1), kdu_exception);
  cs_throw.set_write_fail_policy(KDU_WRITE_FAIL_POLICY_IGNORE);
  cs_throw.destroy();
}

void DecodeTileFirstComponent(kdu_codestream& cs, kdu_tile& tile,
                              kdu_thread_env* env = nullptr) {
  kdu_multi_synthesis synth;
  synth.create(cs, tile, env, nullptr, KDU_MULTI_XFORM_DEFAULT_FLAGS);
  const int height = synth.get_size(0).y;
  for (int y = 0; y < height; ++y) {
    EXPECT_NE(synth.get_line(0, env), nullptr);
  }
  synth.destroy(env);
}

void DecodeTileAllComponents(kdu_codestream& cs, kdu_tile& tile) {
  kdu_multi_synthesis synth;
  synth.create(cs, tile, nullptr, nullptr, KDU_MULTI_XFORM_DEFAULT_FLAGS);
  const int num_comps = cs.get_num_components(true);
  const int height = synth.get_size(0).y;
  for (int y = 0; y < height; ++y) {
    for (int c = 0; c < num_comps; ++c) {
      EXPECT_NE(synth.get_line(c), nullptr);
    }
  }
  synth.destroy();
}

TEST_F(CodestreamTest, StructuredCacheAndTileUnloading) {
  ImageSpec spec;
  spec.width = 48;
  spec.height = 48;
  spec.components = 1;
  spec.tile_size = kdu_coords(16, 16);  // 3x3 = 9 tiles
  spec.extra_params = {"Corder=RPCL", "Clayers=2", "Cprecincts={8,8}"};
  const auto planes = MakeTestPlanes(spec);
  siz_params siz;
  ConfigureSiz(siz, spec);

  StructuredCacheTarget cache_tgt;
  kdu_codestream out_cs;
  out_cs.create(&siz, &cache_tgt);
  ConfigureCodestreamParams(out_cs, spec);
  kdu_dims tr;
  out_cs.get_valid_tiles(tr);
  for (int ty = 0; ty < tr.size.y; ++ty) {
    for (int tx = 0; tx < tr.size.x; ++tx) {
      kdu_tile tile = out_cs.open_tile(kdu_coords(tx, ty));
      PushTileData(out_cs, tile, planes, spec);
      tile.close();
    }
  }
  kdu_long lbytes[2] = {1000, 0};
  out_cs.flush(lbytes, 2);
  out_cs.destroy();
  cache_tgt.close();
  EXPECT_FALSE(cache_tgt.main_header.empty());
  EXPECT_EQ(cache_tgt.tile_headers.size(), 9);
  EXPECT_FALSE(cache_tgt.precincts.empty());

  StructuredCacheSource cache_src(cache_tgt);
  kdu_codestream in_cs;
  in_cs.create(&cache_src);
  in_cs.set_persistent();
  in_cs.augment_cache_threshold(1);
  in_cs.set_tile_unloading_threshold(2);
  for (int ty = 0; ty < 3; ++ty) {
    for (int tx = 0; tx < 3; ++tx) {
      kdu_tile tile = in_cs.open_tile(kdu_coords(tx, ty));
      DecodeTileFirstComponent(in_cs, tile);
      tile.close();
    }
  }
  in_cs.destroy();
}

TEST_F(CodestreamTest, InterchangeCodestreamLoadSequentialAndTranscoding) {
  ImageSpec spec;
  spec.width = 48;
  spec.height = 48;
  spec.components = 3;
  spec.reversible = false;
  spec.dwt_levels = 2;
  spec.extra_params = {"Corder=PCRL", "Clayers=2", "Cycc=yes",
                       "Cprecincts={16,16}"};
  const auto planes = MakeTestPlanes(spec);
  const auto src_bytes = EncodeImage(spec, planes);

  MemorySource src(src_bytes);
  kdu_codestream in_cs;
  in_cs.create(&src);

  // Test kdu_pdescriptor_set standalone + load_sequential + trans_pcrd_opt +
  // trans_out.
  kdu_pdescriptor_set pset;
  EXPECT_TRUE(pset.init(kdu_coords(0, 0), 2));
  const int manual_idx = pset.add_pdescriptor(0, 0, kdu_coords(0, 0));
  ASSERT_GE(manual_idx, 0);
  kdu_pdescriptor* manual = pset.access_pdescriptor(manual_idx);
  ASSERT_NE(manual, nullptr);
  manual->num_packets = 1;
  int find_idx = -1;
  EXPECT_EQ(pset.find_pdescriptor(0, 0, kdu_coords(0, 0), find_idx), manual);
  EXPECT_EQ(find_idx, manual_idx);
  EXPECT_GE(pset.note_pdescriptor_complete(manual_idx), 0);
  pset.make_all_complete();
  int accessed = 0;
  for (int i = 0; i < pset.get_num_pdescriptors(); ++i) {
    if (pset.access_pdescriptor(i) != nullptr) {
      accessed++;
    }
  }
  EXPECT_EQ(accessed, 1);
  pset.reset();

  // Build interchange codestream from input siz_params.
  kdu_codestream xchg_cs;
  xchg_cs.create(in_cs.access_siz());
  xchg_cs.access_siz()->copy_from(in_cs.access_siz(), -1, -1);
  xchg_cs.access_siz()->finalize_all();
  kdu_codestream out_cs;
  MemoryTarget out_tgt;
  out_cs.create(in_cs.access_siz(), &out_tgt);
  out_cs.access_siz()->copy_from(in_cs.access_siz(), -1, -1);
  out_cs.access_siz()->finalize_all();

  xchg_cs.create_tile(kdu_coords(0, 0));
  kdu_tile xtile = xchg_cs.open_tile(kdu_coords(0, 0));
  kdu_tile otile = out_cs.open_tile(kdu_coords(0, 0));
  EXPECT_TRUE(xtile.exists());
  EXPECT_TRUE(otile.exists());

  kdu_pdescriptor_set flush_set;
  bool tile_done = false;
  while (!tile_done) {
    kdu_tile in_tile = in_cs.load_sequential(
        &flush_set, tile_done, 0, 0,
        KDU_LOAD_SEQUENTIAL_WANT_TILE | KDU_LOAD_SEQUENTIAL_WANT_FLUSH_SET |
            KDU_LOAD_SEQUENTIAL_WANT_PACKET_BYTES,
        nullptr);
    ASSERT_TRUE(in_tile.exists());
    kdu_pdescriptor_set tail;
    flush_set.extract_tail(&tail, 0);
    for (int pd_idx = 0; pd_idx < tail.get_num_pdescriptors(); ++pd_idx) {
      kdu_pdescriptor* pd = tail.access_pdescriptor(pd_idx);
      ASSERT_NE(pd, nullptr);

      kdu_tile_comp itc = in_tile.access_component(pd->component_idx);
      kdu_resolution ires = itc.access_resolution(pd->resolution_level);
      kdu_tile_comp xtc = xtile.access_component(pd->component_idx);
      kdu_resolution xres = xtc.access_resolution(pd->resolution_level);
      kdu_precinct xprec = xres.open_precinct(pd->precinct_idx);
      kdu_tile_comp otc = otile.access_component(pd->component_idx);
      kdu_resolution ores = otc.access_resolution(pd->resolution_level);

      const int min_band = (pd->resolution_level == 0) ? LL_BAND : HL_BAND;
      const int max_band = (pd->resolution_level == 0) ? LL_BAND : HH_BAND;
      for (int b = min_band; b <= max_band; ++b) {
        kdu_subband isub = ires.access_subband(b);
        kdu_subband osub = ores.access_subband(b);
        kdu_dims blocks;
        if (!xprec.get_valid_blocks(b, blocks)) {
          continue;
        }
        for (int by = 0; by < blocks.size.y; ++by) {
          for (int bx = 0; bx < blocks.size.x; ++bx) {
            const kdu_coords bidx(blocks.pos.x + bx, blocks.pos.y + by);
            kdu_block* ib = isub.open_block(bidx);
            kdu_block* xb = xprec.open_block(b, bidx);
            xb->num_passes = ib->num_passes;
            xb->missing_msbs = ib->missing_msbs;
            int total_bytes = 0;
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
            kdu_block* ob = osub.open_block(bidx);
            ob->num_passes = ib->num_passes;
            ob->missing_msbs = ib->missing_msbs;
            for (int p = 0; p < ib->num_passes; ++p) {
              ob->pass_lengths[p] = ib->pass_lengths[p];
              ob->pass_slopes[p] = ib->pass_slopes[p];
            }
            if (total_bytes > 0) {
              ob->set_max_bytes(total_bytes, false);
              std::memcpy(ob->byte_buffer, ib->byte_buffer,
                          static_cast<size_t>(total_bytes));
            }
            osub.close_block(ob);
            xprec.close_block(xb);
            isub.close_block(ib);
          }
        }
      }
      int cum_p = 1;
      int cum_b = 0;
      bool sig = false;
      EXPECT_TRUE(xprec.size_packets(cum_p, cum_b, sig));
      xprec.restart();
      EXPECT_TRUE(xprec.get_packets(0, 0, cum_p, cum_b, nullptr));
      xprec.close();
    }
    if (!tail.is_empty()) {
      EXPECT_GE(out_cs.trans_pcrd_opt(&tail), 0);
    }
    in_tile.close();
  }
  xtile.close();
  otile.close();
  EXPECT_TRUE(out_cs.review_closed_tile(kdu_coords(0, 0)).exists());

  out_cs.auto_trans_out(1, 1, 0, 0, KDU_LONG_MAX, false);
  out_cs.trans_out(KDU_LONG_MAX, nullptr, 0, true);
  xchg_cs.destroy();
  out_cs.destroy();
  in_cs.destroy();
  EXPECT_GT(out_tgt.data.size(), 0);
}

TEST_F(CodestreamTest, MembackedCompressedSourceModesAndDecode) {
  ImageSpec spec;
  spec.width = 24;
  spec.height = 24;
  spec.components = 1;
  const auto planes = MakeTestPlanes(spec);
  const auto bytes = EncodeImage(spec, planes);

  // 1) Fully serviced chunked streaming decode.
  TestMembackedSource mb_src(bytes, 31);
  EXPECT_EQ(mb_src.get_capabilities() & KDU_SOURCE_CAP_MEMORY_BACKED,
            KDU_SOURCE_CAP_MEMORY_BACKED);
  const char desc_tag[] = "frame0";
  ASSERT_TRUE(mb_src.enable_stream(KDU_MEMBACKED_SOURCE_FULLY_SERVICED,
                                   reinterpret_cast<const kdu_byte*>(desc_tag),
                                   sizeof(desc_tag), 0x01, 1.25, 32,
                                   bytes.size() + 64));
  size_t total = 0;
  kdu_int32 term_code = 0;
  // In single-threaded builds (no mutex support), enable downgrades to
  // UNSERVICED while service() still pumps available data until EOF and
  // returns false once servicing completes.
  EXPECT_FALSE(mb_src.run_service(total, term_code));
  EXPECT_EQ(term_code, 0);
  EXPECT_EQ(total, bytes.size());
  ASSERT_TRUE(mb_src.run_open(false));
  size_t dlen = 0;
  kdu_uint64 interp = 0;
  EXPECT_NE(mb_src.get_descriptor(dlen, interp), nullptr);
  EXPECT_EQ(dlen, sizeof(desc_tag));
  EXPECT_EQ(interp, 0x01U);
  EXPECT_DOUBLE_EQ(mb_src.get_timestamp(), 1.25);
  EXPECT_EQ(mb_src.get_term_code(), 0);

  kdu_codestream cs;
  cs.create(&mb_src);
  kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
  DecodeTileFirstComponent(cs, tile);
  tile.close();
  cs.destroy();
  EXPECT_TRUE(mb_src.finished_loading());
  EXPECT_TRUE(mb_src.close());

  // 2) Unserviced mode using read/seek/load_memory.
  TestMembackedSource unserviced(bytes, 19);
  unserviced.run_reset();
  ASSERT_TRUE(unserviced.run_open(true));
  kdu_long pos = 0;
  kdu_byte* lim = nullptr;
  EXPECT_NE(unserviced.access_memory(pos, lim), nullptr);
  const int sflags = unserviced.load_memory(static_cast<kdu_long>(bytes.size()),
                                            true, nullptr);
  EXPECT_NE(sflags & KDU_SOURCE_SPAN_EXISTS, 0);
  const int lflags = unserviced.load_memory(
      static_cast<kdu_long>(bytes.size() + 1), true, nullptr);
  EXPECT_NE(lflags & KDU_SOURCE_IS_COMPLETE, 0);
  kdu_byte tmp[16];
  EXPECT_EQ(unserviced.read(tmp, 16), 16);
  EXPECT_TRUE(unserviced.seek(4));
  EXPECT_EQ(unserviced.get_pos(), 4);
  EXPECT_TRUE(unserviced.close());
}

TEST_F(CodestreamTest, Part2MctAndNltCodestream) {
  ImageSpec spec;
  spec.width = 24;
  spec.height = 24;
  spec.components = 3;
  spec.reversible = false;
  spec.dwt_levels = 1;
  spec.extra_params = {
      "Sextensions=MCT|NLT",
      "Mcomponents=3",
      "Msigned=no",
      "Mprecision=8",
      "Mmatrix_size:I1=9",
      "Mmatrix_coeffs:I1=1,0,0,0,1,0,0,0,1",
      "Mstage_inputs:I1={0,2}",
      "Mstage_outputs:I1={0,2}",
      "Mstage_collections:I1={3,3}",
      "Mstage_xforms:I1={MATRIX,1,0,0,0}",
      "Mnum_stages=1",
      "Mstages=1",
      "Ncomponents=3",
      "Nprecision=8",
      "Nsigned=no",
      "NLTmake={GAMMA,2.4,0.055,64}",
  };
  const auto planes = MakeTestPlanes(spec);
  const auto bytes = EncodeImage(spec, planes);

  MemorySource src(bytes);
  const auto decoded = DecodeFullImage(&src);
  EXPECT_EQ(decoded.size(), 3);
}

TEST_F(CodestreamTest, ErrorResilienceFussyParsingAndOvershoot) {
  ImageSpec spec;
  spec.width = 32;
  spec.height = 32;
  spec.components = 1;
  spec.reversible = false;
  spec.extra_params = {"Cuse_sop=yes", "Cuse_eph=yes"};
  const auto planes = MakeTestPlanes(spec);
  auto bytes = EncodeImage(spec, planes);
  ASSERT_GT(bytes.size(), 80);

  // Truncate and corrupt packet body bytes to trigger resilience paths.
  std::vector<kdu_byte> truncated(bytes.begin(),
                                  bytes.begin() + (bytes.size() * 3) / 4);
  for (size_t i = truncated.size() - 20; i < truncated.size(); ++i) {
    truncated[i] ^= 0xA5;
  }

  MemorySource src(truncated);
  kdu_codestream cs;
  cs.create(&src);
  cs.set_resilient(true);
  cs.set_fussy();
  cs.set_fast();
  cs.set_allow_parsing_errors(true);
  cs.set_synth_overshoot_limit(0.01F, false);
  cs.collect_timing_stats(2);

  kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
  DecodeTileFirstComponent(cs, tile);
  tile.close();

  float max_over = 0.0F;
  cs.report_synth_overshoot(0, max_over);
  kdu_long nsamples = 0;
  EXPECT_GE(cs.get_timing_stats(&nsamples, false), 0.0);
  EXPECT_GE(cs.get_timing_stats(&nsamples, true), 0.0);
  (void)cs.get_corrupted_source_pos();
  (void)cs.get_decoding_errors_detected();
  cs.destroy();
}

TEST_F(CodestreamTest, Part2MctVariantsAtkNltAndComponentRestrictions) {
  // 1. Reversible SERM MATRIX MCT (2 components, Mmatrix_size=6) + output
  // component restriction triggering create_rxform_ss_model().
  {
    ImageSpec spec;
    spec.width = 20;
    spec.height = 16;
    spec.components = 2;
    spec.is_signed = true;
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.extra_params = {
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
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);
    MemorySource src(bytes);
    kdu_codestream cs;
    cs.create(&src);
    int out_idx = 1;
    cs.apply_input_restrictions(1, &out_idx, 0, 0, nullptr,
                                KDU_WANT_OUTPUT_COMPONENTS);
    EXPECT_EQ(cs.get_num_components(true), 1);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    DecodeTileFirstComponent(cs, tile);
    tile.close();
    cs.destroy();
  }

  // 2. Two-stage irreversible MATRIX + NULL expansion (3->5 outputs) +
  // output component subset & codestream component restriction.
  {
    ImageSpec spec;
    spec.width = 20;
    spec.height = 16;
    spec.components = 5;
    spec.reversible = false;
    spec.dwt_levels = 1;
    spec.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=5",
        "Mprecision=8",
        "Msigned=no",
        "Mmatrix_size:I1=9",
        "Mmatrix_coeffs:I1=1.0,0.25,0.0,0.0,1.0,-0.25,0.25,0.0,1.0",
        "Mvector_size:I1=3",
        "Mvector_coeffs:I1=3,-2,1",
        "Mstage_inputs:I1={0,2},{3,4}",
        "Mstage_outputs:I1={0,2},{3,4}",
        "Mstage_collections:I1={3,3},{2,2}",
        "Mstage_xforms:I1={MATRIX,1,1,0,0},{MATRIX,0,0,0,0}",
        "Mstage_inputs:I2={0,1},{2,4}",
        "Mstage_outputs:I2={0,1},{2,4}",
        "Mstage_collections:I2={2,2},{3,3}",
        "Mstage_xforms:I2={MATRIX,0,0,0,0},{MATRIX,1,0,0,0}",
        "Mnum_stages=2",
        "Mstages=1,2",
    };
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);
    MemorySource src(bytes);
    kdu_codestream cs;
    cs.create(&src);
    cs.set_persistent();
    int subset[2] = {0, 2};
    cs.apply_input_restrictions(2, subset, 0, 0, nullptr,
                                KDU_WANT_OUTPUT_COMPONENTS);
    kdu_coords subs[4];
    EXPECT_GT(cs.get_res_subsamplings(0, subs, 4, true), 0);
    EXPECT_GT(cs.get_res_subsamplings(0, subs, 4, false), 0);
    EXPECT_EQ(cs.get_res_subsamplings(-1, subs, 4, true), 0);
    EXPECT_EQ(cs.get_res_subsamplings(9, subs, 4, true), 0);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    DecodeTileAllComponents(cs, tile);
    tile.close();
    cs.apply_input_restrictions(2, subset, 0, 0, nullptr,
                                KDU_WANT_CODESTREAM_COMPONENTS);
    EXPECT_EQ(cs.get_num_components(true), 2);
    kdu_tile tile2 = cs.open_tile(kdu_coords(0, 0));
    DecodeTileAllComponents(cs, tile2);
    tile2.close();
    cs.destroy();
  }

  // 3. Reversible & irreversible DEP transforms + partial output interest.
  for (bool rev : {true, false}) {
    ImageSpec spec;
    spec.width = 16;
    spec.height = 12;
    spec.components = 3;
    spec.reversible = rev;
    spec.dwt_levels = 1;
    spec.extra_params = {
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
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);
    MemorySource src(bytes);
    kdu_codestream cs;
    cs.create(&src);
    int out_idx = 2;
    cs.apply_input_restrictions(1, &out_idx, 0, 0, nullptr,
                                KDU_WANT_OUTPUT_COMPONENTS);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    DecodeTileFirstComponent(cs, tile);
    tile.close();
    cs.destroy();
  }

  // 4. Component DWT stage (reversible & irreversible) + partial output subset
  // triggering create_dwt_ss_model() and DWT input pruning.
  for (bool rev : {true, false}) {
    ImageSpec spec;
    spec.width = 16;
    spec.height = 12;
    spec.components = 5;
    spec.reversible = rev;
    spec.dwt_levels = 1;
    spec.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=5",
        "Mprecision=8",
        "Msigned=no",
        "Mvector_size:I1=5",
        "Mvector_coeffs:I1=3,-2,1,0,2",
        "Mstage_inputs:I1={0,4}",
        "Mstage_outputs:I1={0,4}",
        "Mstage_collections:I1={5,5}",
        rev ? "Mstage_xforms:I1={DWT,1,1,2,1}"
            : "Mstage_xforms:I1={DWT,0,1,2,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);
    MemorySource src(bytes);
    kdu_codestream cs;
    cs.create(&src);
    int subset[2] = {1, 3};
    cs.apply_input_restrictions(2, subset, 0, 0, nullptr,
                                KDU_WANT_OUTPUT_COMPONENTS);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    DecodeTileAllComponents(cs, tile);
    tile.close();
    cs.destroy();
  }

  // 5. Part-2 ATK kernels (R1X1, I5X3, custom reversible & irreversible ATK)
  // and NLT variants (SMAG, UMAG, LOG, IGAMMA).
  const std::vector<std::vector<std::string>> atk_configs = {
      {"Ckernels=ATK", "Catk=2", "Kkernels:I2=R1X1"},
      {"Ckernels=ATK", "Catk=3", "Kkernels:I3=I5X3", "Creversible=no"},
      {"Ckernels=ATK", "Catk=5", "Kreversible:I5=yes", "Ksymmetric:I5=no",
       "Kextension:I5=CON", "Ksteps:I5={2,0,1,1},{2,-1,2,2}",
       "Kcoeffs:I5={-0.5},{-0.5},{0.25},{0.25}"},
      {"Ckernels=ATK", "Catk=6", "Kreversible:I6=no", "Ksymmetric:I6=no",
       "Kextension:I6=CON", "Ksteps:I6={2,0,0,0},{2,-1,0,0}",
       "Kcoeffs:I6={-0.5},{-0.5},{0.25},{0.25}", "Creversible=no"},
  };
  for (const auto& atk_params : atk_configs) {
    ImageSpec spec;
    spec.width = 24;
    spec.height = 24;
    spec.components = 1;
    spec.reversible = (atk_params[1] != "Catk=3" && atk_params[1] != "Catk=6");
    spec.dwt_levels = 2;
    spec.extra_params = atk_params;
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);
    MemorySource src(bytes);
    const auto dec = DecodeFullImage(&src);
    EXPECT_EQ(dec.size(), 1);
  }
  {
    ImageSpec nlt_smag;
    nlt_smag.width = 20;
    nlt_smag.height = 16;
    nlt_smag.components = 1;
    nlt_smag.is_signed = true;
    nlt_smag.reversible = true;
    nlt_smag.dwt_levels = 1;
    nlt_smag.extra_params = {
        "Sextensions=NLT", "Ncomponents=1", "Nprecision=8",
        "Nsigned=yes",     "NLType=SMAG",
    };
    const auto smag_planes = MakeTestPlanes(nlt_smag);
    const auto smag_bytes = EncodeImage(nlt_smag, smag_planes);
    MemorySource smag_src(smag_bytes);
    const auto smag_dec = DecodeFullImage(&smag_src);
    EXPECT_EQ(smag_dec.size(), 1);
  }
  {
    ImageSpec nlt_spec;
    nlt_spec.width = 20;
    nlt_spec.height = 16;
    nlt_spec.components = 3;
    nlt_spec.is_signed = false;
    nlt_spec.reversible = false;
    nlt_spec.dwt_levels = 1;
    nlt_spec.extra_params = {
        "Sextensions=NLT",
        "Ncomponents=3",
        "Nprecision=8",
        "Nsigned=no",
        "NLTmake:C0={LOG,0.1,0.0,64}",
        "NLTmake:C1={ILOG,0.1,0.0,64}",
        "NLTmake:C2={IGAMMA,2.4,0.055,64}",
    };
    const auto planes = MakeTestPlanes(nlt_spec);
    const auto bytes = EncodeImage(nlt_spec, planes);
    MemorySource src(bytes);
    const auto dec = DecodeFullImage(&src);
    EXPECT_EQ(dec.size(), 3);
  }
}

TEST_F(CodestreamTest, ProgressionOrdersCprlPrclCbrMultiTileAndCreslengths) {
  // 1. CPRL, PRCL progression orders + Creslengths + PLT/TLM + multi-tile.
  for (const char* order : {"CPRL", "PRCL"}) {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 3;
    spec.tile_size = kdu_coords(32, 32);
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {
        std::string("Corder=") + order,
        "Clayers=2",
        "Cprecincts={16,16}",
        "ORGgen_plt=yes",
        "ORGgen_tlm=2",
        "Creslengths=400,200,0,900,450",
        "Creslengths:C0=220,100",
        "Cagglengths:C1=0",
    };
    const auto planes = MakeTestPlanes(spec);

    MemoryTarget tgt;
    siz_params siz;
    ConfigureSiz(siz, spec);
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ConfigureCodestreamParams(cs, spec);
    cs.access_siz()->finalize_all();
    for (int ty = 0; ty < 2; ++ty) {
      for (int tx = 0; tx < 2; ++tx) {
        kdu_tile tile = cs.open_tile(kdu_coords(tx, ty));
        PushTileData(cs, tile, planes, spec);
        tile.close();
      }
    }
    kdu_long layer_sizes[2] = {500, 1100};
    cs.flush(layer_sizes, 2);
    cs.destroy();

    // Load sequentially with WANT_FLUSH_SET and WANT_PACKET_BYTES + transcode.
    MemorySource src(tgt.data);
    kdu_codestream in_cs;
    in_cs.create(&src);
    ImageSpec xchg_spec = spec;
    xchg_spec.extra_params = {
        std::string("Corder=") + order,
        "Clayers=2",
        "Cprecincts={16,16}",
        "ORGgen_plt=yes",
    };
    siz_params xchg_siz;
    ConfigureSiz(xchg_siz, xchg_spec);
    MemoryTarget xchg_tgt;
    kdu_codestream xchg_cs;
    xchg_cs.create(&xchg_siz, &xchg_tgt);
    ConfigureCodestreamParams(xchg_cs, xchg_spec);
    xchg_cs.set_disabled_auto_comments(KDU_AUTO_COMMENT_FLUSH);
    std::map<std::pair<int, int>, kdu_tile> out_tiles;
    kdu_pdescriptor_set pset;
    for (;;) {
      bool tile_done = false;
      kdu_tile tile =
          in_cs.load_sequential(&pset, tile_done, 1, 1,
                                KDU_LOAD_SEQUENTIAL_WANT_FLUSH_SET |
                                    KDU_LOAD_SEQUENTIAL_WANT_PACKET_BYTES,
                                nullptr);
      if (!tile.exists()) {
        break;
      }
      const kdu_coords tidx = pset.get_tile_idx();
      const std::pair<int, int> key(tidx.x, tidx.y);
      auto it = out_tiles.find(key);
      if (it == out_tiles.end()) {
        it = out_tiles.emplace(key, xchg_cs.open_tile(tidx)).first;
      }
      kdu_tile& otile = it->second;
      for (int d = 0; d < pset.get_num_pdescriptors(); ++d) {
        kdu_pdescriptor* pd = pset.access_pdescriptor(d);
        ASSERT_NE(pd, nullptr);
        kdu_tile_comp itc = tile.access_component(pd->component_idx);
        kdu_resolution ires = itc.access_resolution(pd->resolution_level);
        kdu_precinct iprec = ires.access_precinct(pd->precinct_idx);
        ASSERT_TRUE(iprec.exists());
        kdu_tile_comp otc = otile.access_component(pd->component_idx);
        kdu_resolution ores = otc.access_resolution(pd->resolution_level);
        const int min_b = (pd->resolution_level == 0) ? LL_BAND : HL_BAND;
        const int max_b = (pd->resolution_level == 0) ? LL_BAND : HH_BAND;
        for (int b = min_b; b <= max_b; ++b) {
          kdu_subband isub = ires.access_subband(b);
          kdu_subband osub = ores.access_subband(b);
          kdu_dims blocks;
          if (!iprec.get_valid_blocks(b, blocks)) {
            continue;
          }
          for (int by = 0; by < blocks.size.y; ++by) {
            for (int bx = 0; bx < blocks.size.x; ++bx) {
              const kdu_coords bidx(blocks.pos.x + bx, blocks.pos.y + by);
              kdu_block* ib = isub.open_block(bidx);
              kdu_block* ob = osub.open_block(bidx);
              ob->num_passes = ib->num_passes;
              ob->missing_msbs = ib->missing_msbs;
              int total_bytes = 0;
              for (int p = 0; p < ib->num_passes; ++p) {
                ob->pass_lengths[p] = ib->pass_lengths[p];
                ob->pass_slopes[p] = ib->pass_slopes[p];
                total_bytes += ib->pass_lengths[p];
              }
              if (total_bytes > 0) {
                ob->set_max_bytes(total_bytes, false);
                std::memcpy(ob->byte_buffer, ib->byte_buffer,
                            static_cast<size_t>(total_bytes));
              }
              osub.close_block(ob);
              isub.close_block(ib);
            }
          }
        }
      }
      xchg_cs.trans_pcrd_opt(&pset, /*trim_to_rate=*/true, /*sloppy=*/true,
                             0.05);
      pset.reset();
      if (tile_done) {
        otile.close();
        out_tiles.erase(key);
        tile.close();
        kdu_long out_layers[2] = {0, 0};
        xchg_cs.trans_out(static_cast<kdu_long>(tgt.data.size() / 2),
                          out_layers, 2, false);
      }
    }
    EXPECT_TRUE(out_tiles.empty());
    kdu_long final_layers[2] = {0, 0};
    xchg_cs.trans_out(KDU_LONG_MAX, final_layers, 2, true);
    xchg_cs.destroy();
    in_cs.destroy();
    EXPECT_GT(xchg_tgt.data.size(), 0);
  }

  // 2. Unsized layer prefixes & synthesized second-last target size in
  // pcrd_opt.
  {
    ImageSpec spec;
    spec.width = 48;
    spec.height = 48;
    spec.components = 1;
    spec.reversible = false;
    spec.extra_params = {"Clayers=4"};
    const auto planes = MakeTestPlanes(spec);
    MemoryTarget tgt;
    siz_params siz;
    ConfigureSiz(siz, spec);
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ConfigureCodestreamParams(cs, spec);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    PushTileData(cs, tile, planes, spec);
    tile.close();
    kdu_long layer_sizes[4] = {180, 0, 0, 0};
    cs.flush(layer_sizes, 4, /*layer_thresholds=*/nullptr,
             /*trim_to_rate=*/true, /*record_in_comseg=*/true,
             /*tolerance=*/0.1);
    cs.destroy();
    EXPECT_GT(tgt.data.size(), 150);

    // Non-persistent region restriction skipping non-relevant tiles during
    // load_sequential.
    MemorySource src(tgt.data);
    kdu_codestream in_cs;
    in_cs.create(&src);
    kdu_dims roi = MakeDims(16, 16, 16, 16);
    in_cs.apply_input_restrictions(0, 0, 1, 2, &roi);
    kdu_pdescriptor_set pset;
    bool tile_done = false;
    kdu_tile itile = in_cs.load_sequential(
        &pset, tile_done, 0, 0, KDU_LOAD_SEQUENTIAL_WANT_TILE, nullptr);
    if (itile.exists()) {
      itile.close();
    }
    in_cs.destroy();
  }

  // 3. Multi-set incremental Scbr flushing across multiple tiles + stats
  // exchange.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.tile_size = kdu_coords(64, 32);
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {
        "Corder=PCRL",
        "Cprecincts={8,8}",
        "Cblk={8,8}",
        "Scbr={1,64}",
    };
    const auto planes = MakeTestPlanes(spec);

    MemoryTarget tgt1;
    siz_params siz1;
    ConfigureSiz(siz1, spec);
    kdu_codestream cs1;
    cs1.create(&siz1, &tgt1);
    ConfigureCodestreamParams(cs1, spec);
    kdu_flush_stats fstats = cs1.add_flush_stats(0, 5, 0, 1);
    for (int ty = 0; ty < 2; ++ty) {
      kdu_tile tile = cs1.open_tile(kdu_coords(0, ty));
      PushTileData(cs1, tile, planes, spec);
      tile.close();
      kdu_long layer_sz = 1200;
      cs1.flush(&layer_sz, 1);
    }

    MemoryTarget tgt2;
    siz_params siz2;
    ConfigureSiz(siz2, spec);
    kdu_codestream cs2;
    cs2.create(&siz2, &tgt2);
    ConfigureCodestreamParams(cs2, spec);
    (void)cs2.add_flush_stats(2, 5, 0, 1);
    cs2.attach_flush_stats(fstats);
    for (int ty = 0; ty < 2; ++ty) {
      kdu_tile tile = cs2.open_tile(kdu_coords(0, ty));
      PushTileData(cs2, tile, planes, spec);
      tile.close();
      kdu_long layer_sz = 1200;
      cs2.flush(&layer_sz, 1);
    }
    cs2.destroy();
    cs1.destroy();
    EXPECT_GT(tgt1.data.size(), 100);
    EXPECT_GT(tgt2.data.size(), 100);
  }
}

class TestOvlpSource : public kdu_membacked_ovlp_source {
 public:
  TestOvlpSource(std::vector<std::vector<kdu_byte>> frames, size_t chunk_size)
      : frames_(std::move(frames)), chunk_size_(chunk_size) {}

  void ResetFrames(std::vector<std::vector<kdu_byte>> frames) {
    frames_ = std::move(frames);
    next_frame_ = 0;
    cur_frame_ = -1;
    cur_offset_ = 0;
    fetch_calls_ = 0;
  }

  int fetch_calls() const { return fetch_calls_; }

 protected:
  const kdu_byte* fetch_to_next(size_t& descriptor_length,
                                kdu_uint64& interpretation, double& timestamp,
                                size_t& max_buffer_size) override {
    if (next_frame_ >= frames_.size()) {
      return nullptr;
    }
    cur_frame_ = static_cast<int>(next_frame_++);
    cur_offset_ = 0;
    descriptor_ = "frame";
    descriptor_length = 6;
    interpretation = static_cast<kdu_uint64>(cur_frame_ + 1);
    timestamp = static_cast<double>(cur_frame_ + 1);
    max_buffer_size = frames_[cur_frame_].size() * 2 + 1024;
    return reinterpret_cast<const kdu_byte*>(descriptor_.data());
  }

  kdu_long fetch_data(kdu_long max_bytes, kdu_byte* buf,
                      bool blocking) override {
    (void)blocking;
    fetch_calls_++;
    if (cur_frame_ < 0 ||
        cur_offset_ >= frames_[static_cast<size_t>(cur_frame_)].size()) {
      finish_fetching(0);
      return 0;
    }
    const auto& src = frames_[static_cast<size_t>(cur_frame_)];
    const size_t n = std::min({static_cast<size_t>(max_bytes), chunk_size_,
                               src.size() - cur_offset_});
    std::memcpy(buf, src.data() + cur_offset_, n);
    cur_offset_ += n;
    return static_cast<kdu_long>(n);
  }

  bool post_open(kdu_long queue_sequence_idx, kdu_thread_queue* queue,
                 kdu_thread_env* env) override {
    (void)queue_sequence_idx;
    (void)queue;
    (void)env;
    post_open_calls_++;
    return true;
  }

 private:
  std::vector<std::vector<kdu_byte>> frames_;
  size_t chunk_size_;
  size_t next_frame_ = 0;
  int cur_frame_ = -1;
  size_t cur_offset_ = 0;
  std::string descriptor_;
  int fetch_calls_ = 0;
  int post_open_calls_ = 0;
};

TEST_F(CodestreamTest, MultiThreadedTilesStatsAndOvlpMembackedSource) {
  ImageSpec spec;
  spec.width = 64;
  spec.height = 64;
  spec.components = 1;
  spec.tile_size = kdu_coords(32, 32);
  spec.reversible = false;
  spec.dwt_levels = 2;
  const auto planes = MakeTestPlanes(spec);

  // 1. Multi-threaded encode with set_max_bytes + periodic trimming + restart.
  kdu_membroker broker(1LL << 20);
  kdu_thread_env env;
  env.create();
  ASSERT_TRUE(env.add_thread());
  ASSERT_TRUE(env.add_thread());

  MemoryTarget tgt1;
  MemoryTarget tgt2;
  {
    siz_params siz;
    ConfigureSiz(siz, spec);
    kdu_codestream out_cs;
    out_cs.create(&siz, &tgt1, nullptr, 0, 0, &env, &broker);
    ConfigureCodestreamParams(out_cs, spec);
    out_cs.set_max_bytes(1600, /*allow_slope_est=*/false,
                         /*enable_periodic_trimming=*/true);
    out_cs.enable_restart();
    for (int ty = 0; ty < 2; ++ty) {
      for (int tx = 0; tx < 2; ++tx) {
        kdu_tile tile = out_cs.open_tile(kdu_coords(tx, ty), &env);
        PushTileData(out_cs, tile, planes, spec);
        tile.close(&env);
      }
    }
    kdu_long lsize = 1400;
    out_cs.flush(&lsize, 1, nullptr, true, false, 0.0, &env);
    out_cs.restart(&tgt2, &env);
    for (int ty = 0; ty < 2; ++ty) {
      for (int tx = 0; tx < 2; ++tx) {
        kdu_tile tile = out_cs.open_tile(kdu_coords(tx, ty), &env);
        PushTileData(out_cs, tile, planes, spec);
        tile.close(&env);
      }
    }
    out_cs.flush(&lsize, 1, nullptr, true, false, 0.0, &env);
    env.cs_terminate(out_cs);
    out_cs.destroy();
  }
  ASSERT_GT(tgt1.data.size(), 80);
  ASSERT_GT(tgt2.data.size(), 80);

  // 2. Multi-threaded open_tiles(bkgnd_open), close_tiles, access_tile(wait),
  // apply_input_restrictions(&env), and input restart(&env).
  {
    MemorySource src1(tgt1.data);
    MemorySource src2(tgt2.data);
    kdu_codestream in_cs;
    in_cs.create(&src1, &env, &broker);
    in_cs.enable_restart();
    in_cs.set_persistent();
    kdu_dims valid_tiles;
    in_cs.get_valid_tiles(valid_tiles);
    in_cs.open_tiles(valid_tiles, /*bkgnd_open=*/true, &env);
    kdu_tile t00 = in_cs.access_tile(kdu_coords(0, 0), true, &env);
    ASSERT_TRUE(t00.exists());
    DecodeTileFirstComponent(in_cs, t00, &env);
    t00.close(&env);
    kdu_dims tail_tiles;
    tail_tiles.pos = kdu_coords(1, 0);
    tail_tiles.size = kdu_coords(1, 2);
    in_cs.close_tiles(tail_tiles, &env);
    in_cs.open_tiles(tail_tiles, /*bkgnd_open=*/false, &env);
    kdu_tile t11 = in_cs.access_tile(kdu_coords(1, 1), true, &env);
    ASSERT_TRUE(t11.exists());
    t11.close(&env);
    in_cs.close_tiles(valid_tiles, &env);

    in_cs.restart(&src2, &env);
    kdu_dims roi = MakeDims(0, 0, 32, 32);
    in_cs.apply_input_restrictions(0, 0, 1, 1, &roi, KDU_WANT_OUTPUT_COMPONENTS,
                                   &env);
    kdu_tile t00b = in_cs.open_tile(kdu_coords(0, 0), &env);
    ASSERT_TRUE(t00b.exists());
    t00b.close(&env);
    env.cs_terminate(in_cs);
    in_cs.destroy();
  }

  // 3. kdu_membacked_ovlp_source: pre/post discard + blocking & non-blocking
  // ovlp_open + ovlp_close + ovlp_service_all_finished + ovlp_shutdown.
  {
    TestOvlpSource ovlp({tgt1.data, tgt2.data}, 64);
    const kdu_byte* desc = nullptr;
    size_t desc_len = 0;
    kdu_uint64 interp = 0;
    double ts = 0.0;
    size_t max_buf = 0;
    size_t num_buf = 0;
    kdu_int32 term_code = 0;
    kdu_uint32 pre_disc = 0;
    kdu_uint32 post_disc = 0;
    ASSERT_TRUE(ovlp.ovlp_service(desc, desc_len, interp, ts, max_buf, 32,
                                  num_buf, term_code, pre_disc, post_disc,
                                  /*allow_in_use_pre_discard=*/false,
                                  /*allow_post_discard=*/true));
    EXPECT_EQ(post_disc, 1);
    EXPECT_EQ(ovlp.ovlp_open(/*blocking=*/false, 1, &env), 1);

    kdu_codestream ovlp_cs;
    ovlp_cs.create(&ovlp);
    kdu_tile tile = ovlp_cs.open_tile(kdu_coords(0, 0));
    DecodeTileFirstComponent(ovlp_cs, tile);
    tile.close();
    ovlp_cs.destroy();
    EXPECT_TRUE(ovlp.ovlp_close(&env));

    // Service next frame first so ovlp_open does not trigger kdu_threads event
    // signal/deregister UB on unpatched kdu_threads.cpp, then verify blocking
    // ovlp_open, pre-discard while open, ovlp_service_all_finished, and
    // ovlp_shutdown.
    ovlp.ResetFrames({tgt1.data, tgt2.data});
    const kdu_byte* d = nullptr;
    size_t dl = 0;
    kdu_uint64 it = 0;
    double t = 0.0;
    size_t mb = 0;
    size_t nb = 0;
    kdu_int32 tc = 0;
    kdu_uint32 pre = 0;
    kdu_uint32 post = 0;
    EXPECT_TRUE(ovlp.ovlp_service(d, dl, it, t, mb, 32, nb, tc, pre, post,
                                  /*allow_in_use_pre_discard=*/false,
                                  /*allow_post_discard=*/false));
    EXPECT_EQ(ovlp.ovlp_open(/*blocking=*/true, 2, &env), 1);
    EXPECT_FALSE(ovlp.ovlp_service(d, dl, it, t, mb, 32, nb, tc, pre, post,
                                   /*allow_in_use_pre_discard=*/true,
                                   /*allow_post_discard=*/false));
    EXPECT_GE(pre, 1);
    ovlp.ovlp_service_all_finished();
    EXPECT_TRUE(ovlp.ovlp_close(&env));
    ovlp.ovlp_shutdown(&env);
    EXPECT_EQ(ovlp.ovlp_open(/*blocking=*/false, 3, &env), -1);
    EXPECT_FALSE(
        static_cast<kdu_core::kdu_membacked_compressed_source&>(ovlp).close());
  }

  env.destroy();
}

TEST_F(CodestreamTest, FragmentsSampleAllocatorAndMaxBytes) {
  // 1. Codestream fragments: valid partial fragment and invalid alignments.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.tile_size = kdu_coords(32, 32);
    spec.reversible = true;
    const auto planes = MakeTestPlanes(spec);

    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget frag_tgt;
    kdu_dims frag = MakeDims(0, 0, 32, 64);
    kdu_codestream cs;
    cs.create(&siz, &frag_tgt, &frag, 0, 0);
    ConfigureCodestreamParams(cs, spec);
    for (int ty = 0; ty < 2; ++ty) {
      kdu_tile tile = cs.open_tile(kdu_coords(0, ty));
      PushTileData(cs, tile, planes, spec);
      tile.close();
    }
    cs.flush(nullptr, 1);
    cs.destroy();
    EXPECT_GT(frag_tgt.data.size(), 0);

    // Unaligned left/upper edge throws error.
    error_handler_.clear();
    MemoryTarget bad_tgt1;
    kdu_dims bad_left = MakeDims(4, 0, 28, 32);
    kdu_codestream bad_cs1;
    EXPECT_THROW(bad_cs1.create(&siz, &bad_tgt1, &bad_left, 0, 0), int);
    if (bad_cs1.exists()) {
      bad_cs1.destroy();
    }
    ASSERT_FALSE(error_handler_.history().empty());
    EXPECT_TRUE(error_handler_.history().back().find("left and upper") !=
                std::string::npos);

    // Unaligned right/lower edge throws error.
    error_handler_.clear();
    MemoryTarget bad_tgt2;
    kdu_dims bad_right = MakeDims(0, 0, 20, 32);
    kdu_codestream bad_cs2;
    EXPECT_THROW(bad_cs2.create(&siz, &bad_tgt2, &bad_right, 0, 0), int);
    if (bad_cs2.exists()) {
      bad_cs2.destroy();
    }
    ASSERT_FALSE(error_handler_.history().empty());
    EXPECT_TRUE(error_handler_.history().back().find("right and lower") !=
                std::string::npos);

    // Empty fragment throws error.
    error_handler_.clear();
    MemoryTarget bad_tgt3;
    kdu_dims bad_empty = MakeDims(0, 0, 0, 0);
    kdu_codestream bad_cs3;
    EXPECT_THROW(bad_cs3.create(&siz, &bad_tgt3, &bad_empty, 0, 0), int);
    if (bad_cs3.exists()) {
      bad_cs3.destroy();
    }
    EXPECT_FALSE(error_handler_.history().empty());

    // Too many tiles across fragments throws error.
    error_handler_.clear();
    MemoryTarget bad_tgt4;
    kdu_dims too_many = MakeDims(0, 0, 64, 64);
    kdu_codestream bad_cs4;
    EXPECT_THROW(bad_cs4.create(&siz, &bad_tgt4, &too_many, 2, 100), int);
    if (bad_cs4.exists()) {
      bad_cs4.destroy();
    }
    EXPECT_FALSE(error_handler_.history().empty());
  }

  // 2. kdu_sample_allocator lifecycle and tight broker limit failure.
  {
    ImageSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 1;
    spec.reversible = true;
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);

    kdu_membroker broker(1LL << 20);
    kdu_sample_allocator alloc;
    EXPECT_TRUE(alloc.configure(&broker, 12));
    EXPECT_TRUE(alloc.configure(&broker, 14));

    MemorySource src(bytes);
    kdu_codestream cs;
    cs.create(&src, nullptr, &broker);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    kdu_multi_synthesis synth;
    synth.create(cs, tile, nullptr, nullptr, KDU_MULTI_XFORM_DEFAULT_FLAGS, 1,
                 &alloc);
    synth.destroy();
    tile.close();
    alloc.restart();
    alloc.release();
    EXPECT_TRUE(alloc.configure(nullptr, 13));
    cs.destroy();

    // Zero-budget broker triggers synthesis unmanaged allocation failure.
    kdu_membroker tiny_broker(0);
    kdu_sample_allocator tiny_alloc;
    tiny_alloc.configure(&tiny_broker, 10);
    MemorySource src2(bytes);
    kdu_codestream cs2;
    cs2.create(&src2);
    kdu_tile tile2 = cs2.open_tile(kdu_coords(0, 0));
    kdu_multi_synthesis synth2;
    error_handler_.clear();
    EXPECT_THROW(synth2.create(cs2, tile2, nullptr, nullptr,
                               KDU_MULTI_XFORM_DEFAULT_FLAGS, 1, &tiny_alloc),
                 int);
    EXPECT_FALSE(error_handler_.history().empty());
    if (synth2.exists()) {
      synth2.destroy();
    }
    tile2.close();
    cs2.destroy();
  }

  // 3. set_max_bytes on input and output (including simulate_parsing error).
  {
    ImageSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 1;
    spec.reversible = false;
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);

    MemorySource src_ok(bytes);
    kdu_codestream cs_ok;
    cs_ok.create(&src_ok);
    cs_ok.set_max_bytes(static_cast<kdu_long>(bytes.size() - 20));
    kdu_tile t = cs_ok.open_tile(kdu_coords(0, 0));
    DecodeTileFirstComponent(cs_ok, t);
    t.close();
    cs_ok.destroy();

    MemorySource src_err(bytes, KDU_SOURCE_CAP_SEQUENTIAL);
    kdu_codestream cs_err;
    cs_err.create(&src_err);
    error_handler_.clear();
    EXPECT_THROW(cs_err.set_max_bytes(8, /*simulate_parsing=*/false), int);
    EXPECT_FALSE(error_handler_.history().empty());
    cs_err.destroy();

    MemorySource src_unsup(bytes);
    kdu_codestream cs_unsup;
    cs_unsup.create(&src_unsup);
    error_handler_.clear();
    EXPECT_THROW(cs_unsup.set_max_bytes(200, /*simulate_parsing=*/true), int);
    EXPECT_FALSE(error_handler_.history().empty());
    cs_unsup.destroy();
  }
}

class SlowAsyncNotifier : public kdu_compressed_source_notifier {
 public:
  explicit SlowAsyncNotifier(bool allow_prepare = true)
      : allow_prepare_(allow_prepare) {}

  void note_blocking() override {
    std::lock_guard<std::mutex> lock(mu_);
    blocking_calls++;
  }
  void note_unblocked() override {
    std::lock_guard<std::mutex> lock(mu_);
    unblocked_calls++;
  }
  bool prepare_notify(kdu_long notification_span) override {
    std::lock_guard<std::mutex> lock(mu_);
    last_span_ = notification_span;
    prepare_calls++;
    return allow_prepare_;
  }
  void notify() override {
    std::lock_guard<std::mutex> lock(mu_);
    notify_calls++;
    notified_ = true;
    cv_.notify_all();
  }
  void will_not_notify() override {
    std::lock_guard<std::mutex> lock(mu_);
    will_not_notify_calls++;
    cancelled_ = true;
    cv_.notify_all();
  }

  bool WaitNotified() {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, std::chrono::seconds(5),
                        [&] { return notified_ || cancelled_; });
  }
  int PrepareCalls() {
    std::lock_guard<std::mutex> lock(mu_);
    return prepare_calls;
  }
  int NotifyCalls() {
    std::lock_guard<std::mutex> lock(mu_);
    return notify_calls;
  }
  int WillNotNotifyCalls() {
    std::lock_guard<std::mutex> lock(mu_);
    return will_not_notify_calls;
  }

  int blocking_calls = 0;
  int unblocked_calls = 0;
  int prepare_calls = 0;
  int notify_calls = 0;
  int will_not_notify_calls = 0;
  kdu_long last_span_ = 0;

 private:
  bool allow_prepare_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool notified_ = false;
  bool cancelled_ = false;
};

class SlowAsyncMembackedSource : public kdu_membacked_compressed_source {
 public:
  SlowAsyncMembackedSource(const std::vector<kdu_byte>& payload,
                           size_t step_bytes)
      : payload_(payload), step_bytes_(step_bytes) {}

  int get_capabilities() override {
    return kdu_membacked_compressed_source::get_capabilities() |
           KDU_SOURCE_CAP_ASYNC_NOTIFICATION;
  }

  bool enable_stream(int mode, const kdu_byte* desc = nullptr,
                     size_t desc_len = 0, kdu_uint64 interp = 0,
                     double ts = 0.0, size_t init_bytes = 0,
                     size_t max_bytes = 0) {
    return kdu_membacked_compressed_source::enable(mode, desc, desc_len, interp,
                                                   ts, init_bytes, max_bytes);
  }

  bool run_service(size_t& total, kdu_int32& term_code) {
    return kdu_membacked_compressed_source::service(total, term_code);
  }

  bool run_open(bool auto_enable) {
    return kdu_membacked_compressed_source::open(auto_enable);
  }

  bool run_reset() { return kdu_membacked_compressed_source::reset() > 0; }

  void ReleaseUntil(size_t allow_until) {
    std::lock_guard<std::mutex> lock(mu_);
    allowed_pos_ = allow_until;
    cv_.notify_all();
  }

  void Abort() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      aborted_ = true;
      cv_.notify_all();
    }
    finish_fetching(0);
  }

 protected:
  kdu_long fetch_data(kdu_long max_bytes, kdu_byte* buffer,
                      bool blocking) override {
    std::unique_lock<std::mutex> lock(mu_);
    if (pos_ >= payload_.size()) {
      return 0;
    }
    if (pos_ >= allowed_pos_) {
      if (!blocking) {
        return -1;
      }
      cv_.wait_for(lock, std::chrono::seconds(5),
                   [&] { return pos_ < allowed_pos_ || aborted_; });
      if (aborted_ || pos_ >= allowed_pos_) {
        return 0;
      }
    }
    const size_t avail = std::min(allowed_pos_, payload_.size()) - pos_;
    size_t want = std::min(avail, static_cast<size_t>(max_bytes));
    if (step_bytes_ > 0) {
      want = std::min(want, step_bytes_);
    }
    std::memcpy(buffer, payload_.data() + pos_, want);
    pos_ += want;
    return static_cast<kdu_long>(want);
  }

 private:
  std::vector<kdu_byte> payload_;
  size_t step_bytes_;
  size_t pos_ = 0;
  size_t allowed_pos_ = 0;
  bool aborted_ = false;
  std::mutex mu_;
  std::condition_variable cv_;
};

// Rewrites a single-tile-part-per-tile J2K codestream encoded with
// `Cuse_sop=yes` and `Cuse_eph=yes` so that packet headers (+ EPH) are moved
// into either main-header PPM markers (`use_ppm == true`) or per-tile PPT
// markers (`use_ppm == false`), keeping SOP + packet bodies in each tile-part.
std::vector<kdu_byte> ConvertJ2kSopEphToPackedHeaders(
    const std::vector<kdu_byte>& src, bool use_ppm, bool split_markers) {
  auto read_u16 = [&](size_t off) -> kdu_uint16 {
    return (static_cast<kdu_uint16>(src[off]) << 8) | src[off + 1];
  };
  auto read_u32 = [&](size_t off) -> kdu_uint32 {
    return (static_cast<kdu_uint32>(src[off]) << 24) |
           (static_cast<kdu_uint32>(src[off + 1]) << 16) |
           (static_cast<kdu_uint32>(src[off + 2]) << 8) | src[off + 3];
  };
  auto push_u16 = [](std::vector<kdu_byte>& out, kdu_uint16 v) {
    out.push_back(static_cast<kdu_byte>(v >> 8));
    out.push_back(static_cast<kdu_byte>(v & 0xFF));
  };
  auto push_u32 = [](std::vector<kdu_byte>& out, kdu_uint32 v) {
    out.push_back(static_cast<kdu_byte>((v >> 24) & 0xFF));
    out.push_back(static_cast<kdu_byte>((v >> 16) & 0xFF));
    out.push_back(static_cast<kdu_byte>((v >> 8) & 0xFF));
    out.push_back(static_cast<kdu_byte>(v & 0xFF));
  };

  size_t first_sot = 2;
  while (first_sot + 2 <= src.size()) {
    const kdu_uint16 m = read_u16(first_sot);
    if (m == KDU_SOT) break;
    const kdu_uint16 len = read_u16(first_sot + 2);
    first_sot += 2 + len;
  }

  struct TilePartOut {
    std::vector<kdu_byte> header_prefix;  // From SOT..just before SOD
    std::vector<kdu_byte> packed_hdrs;
    std::vector<kdu_byte> body;
    kdu_uint16 tile_idx = 0;
  };
  std::vector<TilePartOut> tparts;

  size_t pos = first_sot;
  while (pos + 2 <= src.size()) {
    const kdu_uint16 m = read_u16(pos);
    if (m == KDU_EOC) break;
    if (m != KDU_SOT) break;
    const kdu_uint16 tp_tile_idx = read_u16(pos + 4);
    const kdu_uint32 psot = read_u32(pos + 6);
    const size_t tp_end = (psot > 0) ? (pos + psot) : (src.size() - 2);
    size_t sod_pos = pos + 12;
    while (sod_pos + 2 <= tp_end && read_u16(sod_pos) != KDU_SOD) {
      const kdu_uint16 mlen = read_u16(sod_pos + 2);
      sod_pos += 2 + mlen;
    }
    TilePartOut tp;
    tp.tile_idx = tp_tile_idx;
    tp.header_prefix.assign(src.begin() + pos, src.begin() + sod_pos);
    size_t cur = sod_pos + 2;
    while (cur < tp_end) {
      // SOP = FF 91 00 04 Nsop(2)
      if (cur + 6 <= tp_end && src[cur] == 0xFF && src[cur + 1] == 0x91) {
        tp.body.insert(tp.body.end(), src.begin() + cur, src.begin() + cur + 6);
        cur += 6;
      }
      // Header runs through EPH (FF 92)
      const size_t hdr_start = cur;
      while (cur + 1 < tp_end && !(src[cur] == 0xFF && src[cur + 1] == 0x92)) {
        cur++;
      }
      if (cur + 1 < tp_end && src[cur] == 0xFF && src[cur + 1] == 0x92) {
        cur += 2;
        tp.packed_hdrs.insert(tp.packed_hdrs.end(), src.begin() + hdr_start,
                              src.begin() + cur);
      }
      // Packet body runs until next SOP or end of tile-part
      const size_t body_start = cur;
      while (cur < tp_end &&
             !(cur + 5 < tp_end && src[cur] == 0xFF && src[cur + 1] == 0x91 &&
               src[cur + 2] == 0x00 && src[cur + 3] == 0x04)) {
        cur++;
      }
      tp.body.insert(tp.body.end(), src.begin() + body_start,
                     src.begin() + cur);
    }
    tparts.push_back(std::move(tp));
    pos = tp_end;
  }

  std::vector<kdu_byte> out(src.begin(), src.begin() + first_sot);
  if (use_ppm) {
    std::vector<kdu_byte> ppm_stream;
    for (const auto& tp : tparts) {
      push_u32(ppm_stream, static_cast<kdu_uint32>(tp.packed_hdrs.size()));
      ppm_stream.insert(ppm_stream.end(), tp.packed_hdrs.begin(),
                        tp.packed_hdrs.end());
    }
    // Emit PPM marker(s), optionally splitting payload mid-stream so a second
    // PPM marker segment tests multi-PPM assembly (Zppm=0, Zppm=1).
    const size_t split_at = split_markers && ppm_stream.size() > 16
                                ? (ppm_stream.size() / 2)
                                : ppm_stream.size();
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
    if (!use_ppm) {
      const size_t split_at = split_markers && tp.packed_hdrs.size() > 8
                                  ? (tp.packed_hdrs.size() / 2)
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

TEST_F(CodestreamTest, EnumeratedAtkKernelsAndAppearanceAccessors) {
  // 1. Enumerated Part-2 ATK kernels: R2X2, I2X2, I1X1.
  const std::vector<std::pair<bool, std::vector<std::string>>> atk_cases = {
      {true, {"Ckernels=ATK", "Catk=2", "Kkernels:I2=R2X2"}},
      {false, {"Ckernels=ATK", "Catk=3", "Kkernels:I3=I2X2", "Creversible=no"}},
      {false, {"Ckernels=ATK", "Catk=4", "Kkernels:I4=I1X1", "Creversible=no"}},
  };
  for (const auto& cfg : atk_cases) {
    ImageSpec spec;
    spec.width = 24;
    spec.height = 24;
    spec.components = 1;
    spec.reversible = cfg.first;
    spec.dwt_levels = 2;
    spec.extra_params = cfg.second;
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);
    ASSERT_GE(bytes.size(), 2U);
    EXPECT_EQ(bytes[0], 0xFF);
    EXPECT_EQ(bytes[1], 0x4F);
    MemorySource src(bytes);
    const auto decoded = DecodeFullImage(&src);
    EXPECT_EQ(decoded.size(), 1);
  }

  // 2. Subsampled CRG codestream + pre-finalization geometry accessors under
  // transposed/flipped appearance + visual-weighted kdu_quality_limiter.
  {
    ImageSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 3;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.subsampling = {kdu_coords(1, 1), kdu_coords(2, 2), kdu_coords(2, 2)};
    spec.extra_params = {"CRGoffset={0.0,0.0},{0.25,0.25},{0.5,0.25}"};
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);

    MemorySource src(bytes);
    kdu_codestream cs;
    cs.create(&src);
    cs.change_appearance(/*transpose=*/true, /*vflip=*/true, /*hflip=*/false);
    kdu_coords subs;
    kdu_coords reg;
    kdu_dims dims;
    // Call geometry accessors before construction is finalized, plus bounds.
    cs.get_subsampling(-1, subs, true);
    EXPECT_EQ(subs.x, 0);
    cs.get_subsampling(1, subs, true);
    EXPECT_EQ(subs.x, 2);
    cs.get_subsampling(9, subs, true);
    EXPECT_EQ(subs.x, 0);
    cs.get_min_subsampling(-1, subs, true);
    EXPECT_EQ(subs.x, 0);
    cs.get_min_subsampling(1, subs, true);
    EXPECT_EQ(subs.x, 2);
    cs.get_min_subsampling(9, subs, true);
    EXPECT_EQ(subs.x, 0);
    cs.get_registration(-1, kdu_coords(16, 16), reg, true);
    cs.get_registration(1, kdu_coords(16, 16), reg, true);
    cs.get_registration(9, kdu_coords(16, 16), reg, true);
    cs.get_relative_registration(1, 0, kdu_coords(16, 16), reg, true);
    cs.get_relative_registration(9, 0, kdu_coords(16, 16), reg, true);
    cs.get_dims(-1, dims, true);
    cs.get_dims(1, dims, true);
    cs.get_full_dims(-1, dims, true, true);
    cs.get_full_dims(1, dims, true, true);
    cs.get_full_dims(1, dims, false, false);
    error_handler_.clear();
    EXPECT_THROW(cs.get_dims(9, dims, true), int);
    EXPECT_THROW(cs.get_full_dims(9, dims, true, true), int);

    kdu_quality_limiter qlim(0.02F, /*preserve_if_reversible=*/false);
    qlim.set_display_resolution(300.0F, 300.0F);
    qlim.set_comp_info(0, 1.0F, -1);
    qlim.set_comp_info(1, 2.0F, 1);
    qlim.set_comp_info(2, 2.0F, 1);
    cs.apply_input_restrictions(0, 0, /*discard_levels=*/1, 0, nullptr,
                                KDU_WANT_OUTPUT_COMPONENTS, nullptr, &qlim);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    {
      kdu_multi_synthesis synth;
      synth.create(cs, tile);
      int rows_left[3], sub_y[3], cur_y[3] = {0, 0, 0};
      for (int c = 0; c < 3; ++c) {
        kdu_dims td;
        kdu_coords sub;
        cs.get_tile_dims(kdu_coords(0, 0), c, td, true);
        cs.get_subsampling(c, sub, true);
        rows_left[c] = td.size.y;
        sub_y[c] = std::max(1, sub.y);
      }
      while (rows_left[0] > 0 || rows_left[1] > 0 || rows_left[2] > 0) {
        int min_y = std::numeric_limits<int>::max();
        for (int c = 0; c < 3; ++c) {
          if (rows_left[c] > 0) min_y = std::min(min_y, cur_y[c]);
        }
        for (int c = 0; c < 3; ++c) {
          if (rows_left[c] > 0 && cur_y[c] == min_y) {
            EXPECT_NE(synth.get_line(c), nullptr);
            --rows_left[c];
            cur_y[c] += sub_y[c];
          }
        }
      }
      synth.destroy();
    }
    tile.close();
    cs.destroy();
  }

  // 3. Multi-stage MCT + crg_incompatible + visual-weighted limiter
  // (exercises kd_mct_stage find_type_flags across multiple stages).
  {
    ImageSpec spec;
    spec.width = 24;
    spec.height = 24;
    spec.components = 3;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=3",
        "Mprecision=8",
        "Msigned=no",
        "Mmatrix_size:I1=9",
        "Mmatrix_coeffs:I1=1.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,1.0",
        "Mstage_inputs:I1={0,2}",
        "Mstage_outputs:I1={0,2}",
        "Mstage_collections:I1={3,3}",
        "Mstage_xforms:I1={MATRIX,1,0,0,0}",
        "Mvector_size:I2=3",
        "Mvector_coeffs:I2=0,0,0",
        "Mstage_inputs:I2={0,2}",
        "Mstage_outputs:I2={0,2}",
        "Mstage_collections:I2={3,3}",
        "Mstage_xforms:I2={MATRIX,1,0,0,0}",
        "Mnum_stages=2",
        "Mstages=1,2",
    };
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);

    MemorySource src(bytes);
    kdu_codestream cs;
    cs.create(&src);
    kdu_quality_limiter qlim(0.03F, false);
    qlim.set_display_resolution(300.0F, 300.0F);
    qlim.set_comp_info(0, 1.0F, -1);
    qlim.set_comp_info(1, 1.5F, 1);
    int out_comps[2] = {0, 2};
    cs.apply_input_restrictions(2, out_comps, /*discard_levels=*/0, 0, nullptr,
                                KDU_WANT_OUTPUT_COMPONENTS, nullptr, &qlim);
    kdu_coords reg;
    kdu_coords subs;
    kdu_dims dims;
    cs.get_registration(0, kdu_coords(8, 8), reg, true);
    cs.get_relative_registration(0, 1, kdu_coords(8, 8), reg, true);
    cs.get_subsampling(0, subs, true);
    cs.get_dims(0, dims, true);
    cs.get_full_dims(0, dims, true, true);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    DecodeTileAllComponents(cs, tile);
    tile.close();
    cs.destroy();
  }
}

TEST_F(CodestreamTest, PpmPptPackedPacketHeadersMarkersAndSeekableIo) {
  ImageSpec spec;
  spec.width = 48;
  spec.height = 48;
  spec.components = 1;
  spec.reversible = true;
  spec.dwt_levels = 2;
  spec.tile_size = kdu_coords(16, 16);  // 3x3 = 9 tiles
  spec.extra_params = {"Corder=LRCP", "Clayers=1", "Cblk={8,8}", "Cuse_sop=yes",
                       "Cuse_eph=yes"};
  const auto planes = MakeTestPlanes(spec);
  const auto raw_bytes = EncodeImage(spec, planes);

  // Decode using seekable non-in-memory source out of tile order to exercise
  // kd_compressed_input::seek and ignore().
  {
    MemorySource non_mem_seek(
        raw_bytes, KDU_SOURCE_CAP_SEQUENTIAL | KDU_SOURCE_CAP_SEEKABLE);
    kdu_codestream cs;
    cs.create(&non_mem_seek);
    cs.set_persistent();
    for (int ty : {2, 0, 1}) {
      for (int tx : {2, 1, 0}) {
        kdu_tile tile = cs.open_tile(kdu_coords(tx, ty));
        DecodeTileFirstComponent(cs, tile);
        tile.close();
      }
    }
    cs.destroy();
  }

  // Convert to PPM (single & split) and PPT (single & split) packed headers.
  for (bool use_ppm : {true, false}) {
    for (bool split : {false, true}) {
      const auto packed =
          ConvertJ2kSopEphToPackedHeaders(raw_bytes, use_ppm, split);
      MemorySource src(packed);
      const auto dec = DecodeFullImage(&src);
      EXPECT_EQ(dec.size(), 1);
      EXPECT_EQ(dec[0].size(), static_cast<size_t>(spec.width * spec.height));
    }
  }

  // Exercise kd_pp_markers::ignore_tpart() by restricting to a bottom-right
  // tile region on a PPM codestream so earlier tiles are skipped.
  {
    const auto ppm = ConvertJ2kSopEphToPackedHeaders(raw_bytes, true, true);
    MemorySource src(ppm);
    kdu_codestream cs;
    cs.create(&src);
    kdu_dims crop;
    crop.pos = kdu_coords(32, 32);
    crop.size = kdu_coords(16, 16);
    cs.apply_input_restrictions(0, 0, 0, 0, &crop);
    kdu_dims valid_tiles;
    cs.get_valid_tiles(valid_tiles);
    EXPECT_EQ(valid_tiles.size.x, 1);
    EXPECT_EQ(valid_tiles.size.y, 1);
    kdu_tile tile = cs.open_tile(valid_tiles.pos);
    DecodeTileFirstComponent(cs, tile);
    tile.close();
    cs.destroy();
  }

  // Truncated PPM marker (< 3 bytes) error.
  {
    auto bad_ppm = raw_bytes;
    const std::vector<kdu_byte> bad_seg = {0xFF, 0x60, 0x00, 0x02};
    // Insert right after SOC (offset 2); keep SIZ first so read_main_header
    // reads it: insert before first SOT.
    size_t sot = 2;
    while (sot + 1 < bad_ppm.size() &&
           !(bad_ppm[sot] == 0xFF && bad_ppm[sot + 1] == 0x90)) {
      const kdu_uint16 l =
          (static_cast<kdu_uint16>(bad_ppm[sot + 2]) << 8) | bad_ppm[sot + 3];
      sot += 2 + l;
    }
    bad_ppm.insert(bad_ppm.begin() + sot, bad_seg.begin(), bad_seg.end());
    MemorySource src(bad_ppm);
    kdu_codestream cs;
    error_handler_.clear();
    EXPECT_THROW(cs.create(&src), int);
    if (cs.exists()) cs.destroy();
  }

  // Corrupt SOP/SOT lengths in resilient mode + unexpected marker in packet
  // body (kd_input::read with throw_markers) + trailing bytes after EOC.
  {
    auto corrupt = raw_bytes;
    // Append corrupt SOP (length 5) & corrupt SOT (length 9) near trailing
    // bytes to hit kd_marker::read corrupt SOP/SOT skipping in resilient mode,
    // and inject 0xFF 0x9F inside the final packet bytes.
    ASSERT_GT(corrupt.size(), 40);
    corrupt[corrupt.size() - 12] = 0xFF;
    corrupt[corrupt.size() - 11] = 0x9F;
    corrupt[corrupt.size() - 8] = 0xFF;
    corrupt[corrupt.size() - 7] = 0x91;
    corrupt[corrupt.size() - 6] = 0x00;
    corrupt[corrupt.size() - 5] = 0x05;

    MemorySource src(corrupt);
    kdu_codestream cs;
    cs.create(&src);
    cs.set_resilient(true);
    cs.set_allow_parsing_errors(true);
    kdu_dims tr;
    cs.get_valid_tiles(tr);
    for (int ty = 0; ty < tr.size.y; ++ty) {
      for (int tx = 0; tx < tr.size.x; ++tx) {
        kdu_tile tile = cs.open_tile(kdu_coords(tx, ty));
        DecodeTileFirstComponent(cs, tile);
        tile.close();
      }
    }
    cs.destroy();
  }
  {
    auto eoc_extra = raw_bytes;
    eoc_extra.push_back(0x00);
    eoc_extra.push_back(0x00);
    MemorySource src(eoc_extra);
    kdu_codestream cs;
    cs.create(&src);
    kdu_dims tr;
    cs.get_valid_tiles(tr);
    for (int ty = 0; ty < tr.size.y; ++ty) {
      for (int tx = 0; tx < tr.size.x; ++tx) {
        kdu_tile tile = cs.open_tile(kdu_coords(tx, ty));
        DecodeTileFirstComponent(cs, tile);
        tile.close();
      }
    }
    cs.destroy();
  }
}

TEST_F(CodestreamTest, AsyncMembackedNotifierAndServiceEdgePaths) {
  ImageSpec spec;
  spec.width = 64;
  spec.height = 64;
  spec.components = 1;
  spec.reversible = true;
  spec.dwt_levels = 2;
  const auto planes = MakeTestPlanes(spec);
  const auto bytes = EncodeImage(spec, planes);
  ASSERT_GT(bytes.size(), 128);

  // 1) PARTIALLY_SERVICED async notifier: prepare_notify ->
  // KDU_SOURCE_WILL_NOTIFY, change notifier -> will_not_notify, notify via
  // service() on background thread, then blocking fallback when prepare_notify
  // returns false.
  {
    SlowAsyncMembackedSource mb(bytes, 32);
    ASSERT_TRUE(mb.enable_stream(KDU_MEMBACKED_SOURCE_FULLY_SERVICED, nullptr,
                                 0, 0, 0.0, 64, bytes.size() + 64));
    mb.ReleaseUntil(64);
    ASSERT_TRUE(mb.run_open(false));

    size_t total = 0;
    kdu_int32 term_code = 0;
    std::thread svc([&] { mb.run_service(total, term_code); });

    EXPECT_TRUE(mb.load_memory(64, true, nullptr) & KDU_SOURCE_SPAN_EXISTS);

    SlowAsyncNotifier n1(true);
    SlowAsyncNotifier n2(true);
    EXPECT_TRUE(mb.load_memory(256, false, &n1) & KDU_SOURCE_WILL_NOTIFY);
    EXPECT_EQ(n1.PrepareCalls(), 1);

    // Switch pending notifier to trigger n1.will_not_notify().
    EXPECT_TRUE(mb.load_memory(256, false, &n2) & KDU_SOURCE_WILL_NOTIFY);
    EXPECT_EQ(n1.WillNotNotifyCalls(), 1);

    mb.ReleaseUntil(bytes.size());
    EXPECT_TRUE(n2.WaitNotified());
    EXPECT_GE(n2.NotifyCalls(), 1);
    EXPECT_TRUE(mb.load_memory(bytes.size(), true, nullptr) &
                KDU_SOURCE_SPAN_EXISTS);

    kdu_codestream cs;
    cs.create(&mb);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    DecodeTileFirstComponent(cs, tile);
    tile.close();
    cs.destroy();

    mb.ReleaseUntil(bytes.size() + 64);
    mb.Abort();
    svc.join();
    EXPECT_TRUE(mb.close());
  }

  // 2) FULLY_SERVICED concurrent producer + consumer load_memory with blocking
  // & async notifier + kd_compressed_input::async_check_would_block_and_notify.
  {
    SlowAsyncMembackedSource mb(bytes, 32);
    ASSERT_TRUE(mb.enable_stream(KDU_MEMBACKED_SOURCE_FULLY_SERVICED, nullptr,
                                 0, 0, 0.0, 64, bytes.size() + 64));
    ASSERT_TRUE(mb.run_open(false));

    size_t total = 0;
    kdu_int32 term_code = 0;
    std::thread svc([&] { mb.run_service(total, term_code); });

    SlowAsyncNotifier non_prep(false);
    std::thread releaser([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
      mb.ReleaseUntil(bytes.size());
    });
    // prepare_notify returns false -> falls back to blocking + note_blocking().
    const int flags = mb.load_memory(bytes.size(), false, &non_prep);
    EXPECT_NE(flags & KDU_SOURCE_SPAN_EXISTS, 0);
    EXPECT_GE(non_prep.blocking_calls, 1);
    releaser.join();

    kdu_codestream cs;
    cs.create(&mb);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    DecodeTileFirstComponent(cs, tile);
    tile.close();
    cs.destroy();
    mb.Abort();
    svc.join();
    EXPECT_TRUE(mb.close());
  }
}

TEST_F(CodestreamTest, PcrdOptUnsizedTrimCreslengthsCbrBulkFillAndTransOut) {
  // 1. Large CBR flush hitting bulk fill (>= 8192 bytes) and fine fill.
  {
    ImageSpec spec;
    spec.width = 256;
    spec.height = 256;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {"Corder=PCRL", "Clayers=1", "Cblk={32,32}",
                         "Cprecincts={64,64}", "Scbr={1,512}"};
    const auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ConfigureCodestreamParams(cs, spec);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    PushTileData(cs, tile, planes, spec);
    tile.close();
    kdu_long cbr_bytes[1] = {49152};
    cs.flush(cbr_bytes, 1, nullptr, /*trim_to_rate=*/true);
    cs.destroy();
    EXPECT_GE(tgt.data.size(), 40000);
  }

  // 2. Unsized intermediate layers (layer_bytes[1,2] == 0) + tolerance > 0 +
  // mismatched Qweights warning + trans_pcrd_opt sloppy_bytes + trans_out trim
  // + error 60 on tiny limit + auto_trans_out.
  {
    ImageSpec spec;
    spec.width = 96;
    spec.height = 96;
    spec.components = 3;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {"Corder=RPCL",
                         "Clayers=4",
                         "Cycc=yes",
                         "Cprecincts={32,32}",
                         "Qweights=10.0,0.1,1.0",
                         "ORGgen_plt=yes",
                         "ORGplt_parts=R|L|C"};
    const auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ConfigureCodestreamParams(cs, spec);
    warning_handler_.clear();
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    PushTileData(cs, tile, planes, spec);
    tile.close();
    kdu_long lbytes[4] = {800, 0, 0, 5000};
    cs.flush(lbytes, 4, nullptr, /*trim_to_rate=*/true,
             /*record_in_comseg=*/true, /*tolerance=*/0.05);
    cs.destroy();
    EXPECT_GT(tgt.data.size(), 1500);
    EXPECT_FALSE(warning_handler_.history().empty());

    // Interchange codestream: trans_pcrd_opt with tight limit triggering
    // sloppy_bytes + trans_out truncation + error 60 + auto_trans_out.
    auto copy_loaded_tile = [&](kdu_codestream& in_cs, kdu_codestream& out_cs,
                                kdu_pdescriptor_set& pset,
                                bool shrink_targets) {
      kdu_tile xtile = out_cs.open_tile(kdu_coords(0, 0));
      ASSERT_TRUE(xtile.exists());
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
          int max_b = ires.which() ? 3 : 0;
          int min_b = ires.which() ? 1 : 0;
          for (int b = min_b; b <= max_b; ++b) {
            kdu_dims blocks;
            if (!iprec.get_valid_blocks(b, blocks) || blocks.is_empty())
              continue;
            kdu_subband isub = ires.access_subband(b);
            kdu_subband xsub = xres.access_subband(b);
            for (int r = blocks.pos.y; r < blocks.pos.y + blocks.size.y; ++r) {
              for (int c = blocks.pos.x; c < blocks.pos.x + blocks.size.x;
                   ++c) {
                kdu_coords bidx(c, r);
                kdu_block* ib = isub.open_block(bidx);
                kdu_block* xb = xsub.open_block(bidx);
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
          if (shrink_targets && pd->packet_bytes != nullptr) {
            for (int q = 0; q < pd->packet_bytes_entries; ++q) {
              if (pd->packet_bytes[q] > 2) {
                pd->packet_bytes[q] =
                    std::max<kdu_long>(1, pd->packet_bytes[q] / 2);
              }
            }
          }
        }
        if (shrink_targets && !pset.is_empty()) {
          EXPECT_GE(out_cs.trans_pcrd_opt(&pset, /*trim_to_rate=*/true,
                                          /*sloppy_middle=*/true,
                                          /*tolerance=*/0.1),
                    0);
        }
      }
      xtile.close();
    };

    MemorySource src(tgt.data);
    kdu_codestream in_cs;
    in_cs.create(&src);
    siz_params xsiz;
    ConfigureSiz(xsiz, spec);
    MemoryTarget xtgt;
    kdu_codestream xcs;
    xcs.create(&xsiz, &xtgt);
    ConfigureCodestreamParams(xcs, spec);
    kdu_pdescriptor_set pset;
    ASSERT_TRUE(pset.init(kdu_coords(0, 0), 4));
    copy_loaded_tile(in_cs, xcs, pset, /*shrink_targets=*/true);
    xcs.trans_out(2500, /*layer_bytes=*/nullptr, /*layer_bytes_entries=*/0,
                  true);
    xcs.destroy();
    in_cs.destroy();
    EXPECT_GT(xtgt.data.size(), 200);
    EXPECT_LE(xtgt.data.size(), 3600);
    EXPECT_LE(xtgt.data.size(), tgt.data.size());

    MemorySource src2(tgt.data);
    kdu_codestream in_cs2;
    in_cs2.create(&src2);
    siz_params xsiz2;
    ConfigureSiz(xsiz2, spec);
    MemoryTarget xtgt2;
    kdu_codestream xcs2;
    xcs2.create(&xsiz2, &xtgt2);
    ConfigureCodestreamParams(xcs2, spec);
    xcs2.auto_trans_out(1, 1, 1, 1, 2000, false, nullptr);
    kdu_pdescriptor_set pset2;
    ASSERT_TRUE(pset2.init(kdu_coords(0, 0), 4));
    copy_loaded_tile(in_cs2, xcs2, pset2, /*shrink_targets=*/false);
    xcs2.trans_out(2000, nullptr, 0, true);
    xcs2.destroy();
    in_cs2.destroy();

    MemorySource src3(tgt.data);
    kdu_codestream in_cs3;
    in_cs3.create(&src3);
    siz_params xsiz3;
    ConfigureSiz(xsiz3, spec);
    MemoryTarget xtgt3;
    kdu_codestream xcs3;
    xcs3.create(&xsiz3, &xtgt3);
    ConfigureCodestreamParams(xcs3, spec);
    kdu_pdescriptor_set pset3;
    ASSERT_TRUE(pset3.init(kdu_coords(0, 0), 4));
    copy_loaded_tile(in_cs3, xcs3, pset3, /*shrink_targets=*/false);
    error_handler_.clear();
    EXPECT_THROW(xcs3.trans_out(6, nullptr, 0), int);
    xcs3.destroy();
    in_cs3.destroy();
  }

  // 3. pcrd_trim with using_slopes + Creslengths (feasible + warning) +
  // KDU_FLUSH_USES_THRESHOLDS_AND_SIZES.
  for (bool impossible : {false, true}) {
    ImageSpec spec;
    spec.width = 48;
    spec.height = 48;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {
        "Clayers=2",
        impossible ? "Creslengths=1,1" : "Creslengths=600,1400",
    };
    const auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ConfigureCodestreamParams(cs, spec);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    PushTileData(cs, tile, planes, spec);
    tile.close();
    kdu_uint16 thresh[2] = {44000, 36000};
    kdu_long min_sizes[2] = {200, 500};
    warning_handler_.clear();
    cs.flush(min_sizes, 2, thresh, true, false, 0.0, nullptr,
             KDU_FLUSH_USES_THRESHOLDS_AND_SIZES);
    if (impossible) {
      EXPECT_FALSE(warning_handler_.history().empty());
    }
    cs.destroy();
  }

  // 4. Incremental flush onto StructuredCacheTarget (ready_area <
  // remaining_area) and sequential auto_flush with Sorder / transposed
  // incremental updates.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.tile_size = kdu_coords(32, 32);
    spec.extra_params = {"Corder=RPCL", "Clayers=2", "Cprecincts={16,16}",
                         "ORGtparts=R", "ORGgen_tlm=3"};
    const auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);

    StructuredCacheTarget cache_tgt;
    kdu_codestream cs;
    cs.create(&siz, &cache_tgt);
    ConfigureCodestreamParams(cs, spec);
    kdu_long lbytes[2] = {800, 2000};
    for (int ty = 0; ty < 2; ++ty) {
      for (int tx = 0; tx < 2; ++tx) {
        kdu_tile tile = cs.open_tile(kdu_coords(tx, ty));
        PushTileData(cs, tile, planes, spec);
        tile.close();
        if (ty == 0 && tx == 1) {
          cs.flush(lbytes, 2, nullptr, /*trim_to_rate=*/false);
        }
      }
    }
    cs.flush(lbytes, 2, nullptr, /*trim_to_rate=*/true);
    cs.destroy();
    cache_tgt.close();
    EXPECT_EQ(cache_tgt.tile_headers.size(), 4);
  }
}

TEST_F(CodestreamTest, MultiThreadedTileEnvFragBlocksAndAllocatorFailures) {
  // 1a. Skeleton kdu_membroker failure during cs.create throws KDU_ERROR int.
  {
    ImageSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 1;
    siz_params siz;
    ConfigureSiz(siz, spec);
    kdu_membroker zero_broker(0);
    MemoryTarget tgt;
    tgt.broker = &zero_broker;
    kdu_codestream fail_cs;
    error_handler_.clear();
    EXPECT_THROW(fail_cs.create(&siz, &tgt), int);
    EXPECT_FALSE(error_handler_.history().empty());
    if (fail_cs.exists()) fail_cs.destroy();
  }

  // 1b. kd_buf_server membroker failure emits KDU_ERROR (int) on first failure,
  // then throws std::bad_alloc directly once alloc_error_issued is set.
  {
    struct FailRequestBroker : public kdu_membroker {
      explicit FailRequestBroker(kdu_int64 limit_bytes)
          : kdu_membroker(limit_bytes) {}
      void ArmFailure(bool armed) { armed_ = armed; }
      kdu_int64 request(kdu_int64 min_bytes, kdu_int64 pref_bytes,
                        kdu_int64 alloc_bytes) override {
        if (armed_) return 0;
        return kdu_membroker::request(min_bytes, pref_bytes, alloc_bytes);
      }
      bool armed_ = false;
    };
    ImageSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 1;
    spec.dwt_levels = 0;
    spec.extra_params = {"Cblk={16,16}"};
    siz_params siz;
    ConfigureSiz(siz, spec);
    FailRequestBroker fail_broker(1 << 20);
    MemoryTarget tgt;
    tgt.broker = &fail_broker;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ConfigureCodestreamParams(cs, spec);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    kdu_subband sb =
        tile.access_component(0).access_resolution(0).access_subband(LL_BAND);
    auto fill_block = [&](kdu_coords idx) -> kdu_block* {
      kdu_block* blk = sb.open_block(idx);
      blk->set_max_passes(1, false);
      blk->set_max_bytes(8192, false);
      blk->missing_msbs = 0;
      blk->num_passes = 1;
      blk->pass_slopes[0] = 100;
      blk->pass_lengths[0] = 8192;
      for (int i = 0; i < 8192; ++i) {
        blk->byte_buffer[i] = static_cast<kdu_byte>(0x55);
      }
      return blk;
    };
    kdu_block* blk0 = fill_block(kdu_coords(0, 0));
    fail_broker.ArmFailure(true);
    error_handler_.clear();
    EXPECT_THROW(sb.close_block(blk0), int);
    EXPECT_FALSE(error_handler_.history().empty());
    kdu_block* blk1 = fill_block(kdu_coords(1, 0));
    EXPECT_THROW(sb.close_block(blk1), std::bad_alloc);
    cs.destroy();
  }

  // 2. kdu_sample_allocator permit failure, pre_alloc + finalize failure, and
  // handle_failed_release warning.
  {
    kdu_membroker small_broker(2048);
    kdu_sample_allocator alloc;
    EXPECT_TRUE(alloc.configure(&small_broker, 10));
    EXPECT_FALSE(alloc.seek_permit(4096, /*return_on_failure=*/true));
    alloc.release_permit(512);  // Exceeds 0 permits -> handle_failed_release
    alloc.release_permit(512);  // Second call hits early return
    alloc.pre_alloc(KDU_SAMPLE_PREC32, 0, 8192);
    error_handler_.clear();
    EXPECT_THROW(alloc.finalize(kdu_codestream()), int);
    EXPECT_FALSE(error_handler_.history().empty());
  }

  // 3. Large code-buffer count + fragmented block reassembly (256x256,
  // Cblk=16x16) across multi-tile decode + set_tile_unloading_threshold.
  {
    ImageSpec spec;
    spec.width = 256;
    spec.height = 256;
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(128, 128);
    spec.extra_params = {"Cblk={16,16}", "Corder=RPCL", "Clayers=2",
                         "ORGtparts=R", "ORGgen_tlm=4"};
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);

    MemorySource src(bytes);
    kdu_codestream cs;
    cs.create(&src);
    cs.set_persistent();
    cs.set_tile_unloading_threshold(1);
    for (int ty = 0; ty < 2; ++ty) {
      for (int tx = 0; tx < 2; ++tx) {
        kdu_tile tile = cs.open_tile(kdu_coords(tx, ty));
        DecodeTileFirstComponent(cs, tile);
        tile.close();
      }
    }
    for (int ty = 0; ty < 2; ++ty) {
      for (int tx = 0; tx < 2; ++tx) {
        kdu_tile tile = cs.open_tile(kdu_coords(tx, ty));
        DecodeTileFirstComponent(cs, tile);
        tile.close();
      }
    }
    cs.destroy();
  }

  // 4. Multithreaded kdu_thread_env encode & tile decode (open_tiles,
  // access_tile with wait_for_bkgnd=true, close_tiles, auto_flush,
  // cs_terminate).
  {
    ImageSpec spec;
    spec.width = 128;
    spec.height = 128;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(32, 32);
    spec.extra_params = {"Corder=PCRL", "Clayers=2", "Cprecincts={16,16}"};
    const auto planes = MakeTestPlanes(spec);

    kdu_thread_env env;
    env.create();
    env.add_thread();

    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    kdu_codestream out_cs;
    out_cs.create(&siz, &tgt, nullptr, 0, 0, &env);
    ConfigureCodestreamParams(out_cs, spec);
    kdu_long lbytes[2] = {2000, 6000};
    out_cs.auto_flush(1, 1, 32, 32, lbytes, 2, nullptr, true, false, 0.0, &env);
    kdu_dims all_tiles;
    out_cs.get_valid_tiles(all_tiles);
    for (int ty = 0; ty < all_tiles.size.y; ++ty) {
      for (int tx = 0; tx < all_tiles.size.x; ++tx) {
        kdu_tile tile = out_cs.open_tile(kdu_coords(tx, ty), &env);
        PushTileData(out_cs, tile, planes, spec);
        tile.close(&env);
      }
    }
    out_cs.flush(lbytes, 2, nullptr, true, false, 0.0, &env);
    env.cs_terminate(out_cs);
    out_cs.destroy();

    MemorySource src(tgt.data);
    kdu_codestream in_cs;
    in_cs.create(&src, &env);
    in_cs.set_persistent();
    in_cs.open_tiles(all_tiles, /*wait_for_bkgnd=*/false, &env);
    for (int ty = 0; ty < all_tiles.size.y; ++ty) {
      for (int tx = 0; tx < all_tiles.size.x; ++tx) {
        kdu_tile tile = in_cs.access_tile(kdu_coords(tx, ty),
                                          /*wait_for_bkgnd=*/true, &env);
        ASSERT_TRUE(tile.exists());
        DecodeTileFirstComponent(in_cs, tile);
      }
    }
    in_cs.close_tiles(all_tiles, &env);
    env.cs_terminate(in_cs);
    in_cs.destroy();
    env.destroy();
  }
}

TEST_F(CodestreamTest, MarkerErrorsPrintCodesAuxSegmentsAndInputRestart) {
  // 1. Exercise kd_core_local::print_marker_code across all marker codes by
  // corrupting the EPH marker after a packet header in a Cuse_eph=yes stream.
  {
    ImageSpec spec;
    spec.width = 16;
    spec.height = 16;
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 0;
    spec.extra_params = {"Cuse_sop=yes", "Cuse_eph=yes"};
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);
    size_t sod_pos = bytes.size();
    for (size_t i = 0; i + 1 < bytes.size(); ++i) {
      if (bytes[i] == 0xFF && bytes[i + 1] == 0x93) {
        sod_pos = i;
        break;
      }
    }
    ASSERT_LT(sod_pos, bytes.size());
    size_t eph_pos = bytes.size();
    for (size_t i = sod_pos + 2; i + 1 < bytes.size(); ++i) {
      if (bytes[i] == 0xFF && bytes[i + 1] == 0x92) {
        eph_pos = i;
        break;
      }
    }
    ASSERT_LT(eph_pos + 1, bytes.size());

    const kdu_uint16 marker_codes[] = {
        KDU_SOC, KDU_SOT,   KDU_SOD, KDU_SOP, KDU_EOC, KDU_SIZ, KDU_CAP,
        KDU_PRF, KDU_CPF,   KDU_CBD, KDU_MCT, KDU_MCC, KDU_MCO, KDU_COD,
        KDU_COC, KDU_QCD,   KDU_QCC, KDU_RGN, KDU_POC, KDU_CRG, KDU_DFS,
        KDU_ADS, KDU_NLT,   KDU_ATK, KDU_COM, KDU_TLM, KDU_PLM, KDU_PLT,
        KDU_PPM, KDU_PPT,   KDU_DCO, KDU_QPD, KDU_QPC, KDU_VMS, KDU_RLT,
        KDU_SEC, KDU_INSEC, KDU_NSI, KDU_EPB, KDU_ESD, KDU_EPC, KDU_RED,
        KDU_HDT, 0xFF12,
    };
    for (kdu_uint16 code : marker_codes) {
      std::vector<kdu_byte> corrupted = bytes;
      corrupted[eph_pos] = static_cast<kdu_byte>(code >> 8);
      corrupted[eph_pos + 1] = static_cast<kdu_byte>(code & 0xFF);
      MemorySource src(corrupted);
      kdu_codestream in_cs;
      in_cs.create(&src);
      kdu_tile tile = in_cs.open_tile(kdu_coords(0, 0));
      kdu_subband sb =
          tile.access_component(0).access_resolution(0).access_subband(LL_BAND);
      error_handler_.clear();
      EXPECT_THROW(
          {
            kdu_block* blk = sb.open_block(kdu_coords(0, 0));
            sb.close_block(blk);
          },
          int);
      in_cs.destroy();
    }
  }

  // 2. Non-seekable small-chunk compressed source + MCT CBD auxiliary marker
  // scanning + kd_input::ignore/seek/load_buf.
  {
    struct ChunkedNonSeekableSource : public kdu_compressed_source {
      explicit ChunkedNonSeekableSource(const std::vector<kdu_byte>& buf)
          : data(buf) {}
      int get_capabilities() override { return 0; }
      int read(kdu_byte* buf, int num_bytes) override {
        if (pos >= data.size() || num_bytes <= 0) return 0;
        const size_t avail = data.size() - pos;
        const size_t n =
            std::min<size_t>({avail, static_cast<size_t>(num_bytes), 7});
        std::copy_n(data.data() + pos, n, buf);
        pos += n;
        return static_cast<int>(n);
      }
      const std::vector<kdu_byte>& data;
      size_t pos = 0;
    };

    ImageSpec mct_spec;
    mct_spec.width = 48;
    mct_spec.height = 48;
    mct_spec.components = 2;
    mct_spec.reversible = true;
    mct_spec.dwt_levels = 1;
    mct_spec.tile_size = kdu_coords(24, 24);
    mct_spec.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=2",
        "Mprecision=8",
        "Msigned=no",
        "Mmatrix_size:I1=6",
        "Mmatrix_coeffs:I1=1,512,512,-1,0,1",
        "Mstage_inputs:I1={0,1}",
        "Mstage_outputs:I1={0,1}",
        "Mstage_collections:I1={2,2}",
        "Mstage_xforms:I1={MATRIX,1,0,1,0}",
        "Mnum_stages=1",
        "Mstages=1",
        "Cuse_sop=yes",
    };
    const auto mct_planes = MakeTestPlanes(mct_spec);
    const auto mct_bytes = EncodeImage(mct_spec, mct_planes);

    ChunkedNonSeekableSource chunked(mct_bytes);
    kdu_codestream cs;
    cs.create(&chunked);
    kdu_dims tdims;
    cs.get_valid_tiles(tdims);
    for (int ty = 0; ty < tdims.size.y; ++ty) {
      for (int tx = 0; tx < tdims.size.x; ++tx) {
        kdu_tile tile = cs.open_tile(kdu_coords(tx, ty));
        DecodeTileAllComponents(cs, tile);
        tile.close();
      }
    }
    cs.destroy();

    // Non-seekable seek attempt throws KDU_ERROR.
    const std::vector<kdu_byte> truncated_bytes(mct_bytes.begin(),
                                                mct_bytes.begin() + 24);
    ChunkedNonSeekableSource truncated(truncated_bytes);
    kdu_codestream fail_cs;
    error_handler_.clear();
    EXPECT_THROW(fail_cs.create(&truncated), int);
    if (fail_cs.exists()) fail_cs.destroy();
  }

  // 3. Input codestream restart: error without enable_restart, same geometry,
  // different geometry (new_state fallback), open-tile error, and threaded
  // input restart.
  {
    ImageSpec spec_a;
    spec_a.width = 32;
    spec_a.height = 32;
    spec_a.components = 1;
    spec_a.reversible = true;
    spec_a.dwt_levels = 1;
    const auto bytes_a = EncodeImage(spec_a, MakeTestPlanes(spec_a));

    ImageSpec spec_b;
    spec_b.width = 64;
    spec_b.height = 48;
    spec_b.components = 3;
    spec_b.reversible = false;
    spec_b.dwt_levels = 2;
    spec_b.extra_params = {"Cycc=yes"};
    const auto bytes_b = EncodeImage(spec_b, MakeTestPlanes(spec_b));

    MemorySource src_a1(bytes_a);
    MemorySource src_a2(bytes_a);
    MemorySource src_b1(bytes_b);
    MemorySource src_a3(bytes_a);
    MemorySource src_b2(bytes_b);

    kdu_codestream in_cs;
    in_cs.create(&src_a1);
    error_handler_.clear();
    EXPECT_THROW(in_cs.restart(&src_a2), int);

    in_cs.enable_restart();
    in_cs.set_persistent();
    {
      kdu_tile tile = in_cs.open_tile(kdu_coords(0, 0));
      DecodeTileFirstComponent(in_cs, tile);
      tile.close();
    }
    in_cs.restart(&src_a2);
    {
      kdu_tile tile = in_cs.open_tile(kdu_coords(0, 0));
      DecodeTileFirstComponent(in_cs, tile);
      tile.close();
    }

    // Missing SIZ marker after SOC throws KDU_ERROR inside input restart.
    {
      const std::vector<kdu_byte> bad_no_siz = {0xFF, 0x4F, 0xFF, 0xD9};
      MemorySource bad_src(bad_no_siz);
      error_handler_.clear();
      EXPECT_THROW(in_cs.restart(&bad_src), int);
    }

    // Geometry / component count change exercises kd_codestream new_state path.
    in_cs.restart(&src_b1);
    EXPECT_EQ(in_cs.get_num_components(), 3);
    kdu_dims dims_b;
    in_cs.get_dims(-1, dims_b);
    EXPECT_EQ(dims_b.size.x, 64);
    EXPECT_EQ(dims_b.size.y, 48);
    {
      kdu_tile tile = in_cs.open_tile(kdu_coords(0, 0));
      DecodeTileAllComponents(in_cs, tile);
      tile.close();
    }
    in_cs.destroy();

    // Multi-threaded input restart across geometry changes.
    kdu_thread_env env;
    env.create();
    env.add_thread();
    kdu_codestream mt_in;
    mt_in.create(&src_a3, &env);
    mt_in.enable_restart();
    mt_in.set_persistent();
    {
      kdu_tile tile = mt_in.open_tile(kdu_coords(0, 0), &env);
      DecodeTileFirstComponent(mt_in, tile, &env);
      tile.close(&env);
    }
    mt_in.restart(&src_b2, &env);
    EXPECT_EQ(mt_in.get_num_components(), 3);
    {
      kdu_tile tile = mt_in.open_tile(kdu_coords(0, 0), &env);
      DecodeTileAllComponents(mt_in, tile);
      tile.close(&env);
    }
    env.cs_terminate(mt_in);
    mt_in.destroy();
    env.destroy();
  }
}

TEST_F(CodestreamTest, CbrMultiFrameFlushStatsTightRateAndTransOut) {
  // 1. Multi-tile CBR encoding with Scbr, get_cbr_model (tile_span.y > 1),
  // auto_flush, flush_stats advance/restart, min_slope_threshold, precinct
  // fallback loop, and all-zero global_slope_included_everything branch.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(64, 32);
    spec.extra_params = {"Cprecincts={16,16}", "Cblk={8,8}", "Corder=PCRL",
                         "Scbr={1,64}"};
    const auto planes = MakeTestPlanes(spec);
    std::vector<std::vector<kdu_int32>> zero_planes(
        1, std::vector<kdu_int32>(64 * 64, 128));

    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt1;
    kdu_codestream cs;
    cs.create(&siz, &tgt1);
    ConfigureCodestreamParams(cs, spec);
    cs.enable_restart();
    EXPECT_GE(cs.set_cbr_start_delay(2.0f), 2.0f);

    float start_delay = 0.0f;
    float max_bucket = 0.0f;
    kdu_uint32 fset_lines = 0;
    kdu_uint32 a_lat = 0;
    kdu_uint32 s_lat = 0;
    EXPECT_FALSE(
        cs.get_cbr_model(start_delay, fset_lines, max_bucket, a_lat, s_lat));

    kdu_flush_stats fstats = cs.add_flush_stats(0, 5, 0, 1);
    kdu_long frame_budget = 600;
    auto encode_frame = [&](const std::vector<std::vector<kdu_int32>>& data,
                            kdu_long budget) {
      cs.auto_flush(1, 1, 16, 16, &budget, 1);
      for (int ty = 0; ty < 2; ++ty) {
        kdu_tile tile = cs.open_tile(kdu_coords(0, ty));
        PushTileData(cs, tile, data, spec);
        tile.close();
      }
      cs.flush(&budget, 1);
    };
    encode_frame(planes, frame_budget);
    EXPECT_TRUE(
        cs.get_cbr_model(start_delay, fset_lines, max_bucket, a_lat, s_lat));
    EXPECT_GT(fset_lines, 0U);

    float bmax = 0.0f;
    float bmean = 0.0f;
    float inter_flush = 0.0f;
    kdu_uint16 min_slope = 0;
    kdu_uint16 max_slope = 0;
    float mean_slope = 0.0f;
    float mean_sq_slope = 0.0f;
    kdu_long fill_bytes = 0;
    cs.get_cbr_flush_stats(bmax, bmean, inter_flush, min_slope, max_slope,
                           mean_slope, mean_sq_slope, fill_bytes);
    EXPECT_GT(bmax, 0.0f);

    // Frame 2: restart + fstats.advance exercises have_summary_slope_evidence
    // and link_tile_comp.
    fstats.advance(1);
    MemoryTarget tgt2;
    cs.restart(&tgt2);
    encode_frame(planes, 450);

    // Frame 3: min_slope_threshold > 0 branch + zero-content image.
    fstats.advance(1);
    MemoryTarget tgt3;
    cs.restart(&tgt3);
    cs.set_min_slope_threshold(50000);
    encode_frame(zero_planes, 800);

    // Error paths in attach_flush_stats: missing own flush_stats and mismatched
    // structure.
    ImageSpec mis_spec = spec;
    mis_spec.tile_size = kdu_coords(64, 16);  // 4 tiles != 2 tiles
    siz_params mis_siz;
    ConfigureSiz(mis_siz, mis_spec);
    MemoryTarget mis_tgt;
    kdu_codestream mis_cs;
    mis_cs.create(&mis_siz, &mis_tgt);
    ConfigureCodestreamParams(mis_cs, mis_spec);
    error_handler_.clear();
    EXPECT_THROW(mis_cs.attach_flush_stats(fstats), int);
    mis_cs.add_flush_stats(0, 5, 1, 2);
    error_handler_.clear();
    EXPECT_THROW(mis_cs.attach_flush_stats(fstats), int);
    mis_cs.destroy();
    cs.destroy();
  }

  // 2. Non-CBR KDU_FLUSH_USES_THRESHOLDS_AND_SIZES + HINTS, TLM generation,
  // auto_trans_out + Creslength warning + trans_pcrd_opt thresholds.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(32, 32);
    spec.extra_params = {"Cprecincts={16,16}", "Corder=RPCL", "ORGtparts=R",
                         "ORGgen_tlm=4", "Clayers=3"};
    const auto planes = MakeTestPlanes(spec);

    for (int flush_flags : {KDU_FLUSH_USES_THRESHOLDS_AND_SIZES,
                            KDU_FLUSH_USES_THRESHOLDS_AND_SIZES |
                                KDU_FLUSH_THRESHOLDS_ARE_HINTS}) {
      siz_params siz;
      ConfigureSiz(siz, spec);
      MemoryTarget tgt;
      kdu_codestream cs;
      cs.create(&siz, &tgt);
      ConfigureCodestreamParams(cs, spec);
      kdu_long lbytes[3] = {0, 700, 2200};
      kdu_uint16 lthresh[3] = {52000, 0, 44000};
      for (int ty = 0; ty < 2; ++ty) {
        for (int tx = 0; tx < 2; ++tx) {
          kdu_tile tile = cs.open_tile(kdu_coords(tx, ty));
          PushTileData(cs, tile, planes, spec);
          tile.close();
        }
      }
      cs.flush(lbytes, 3, lthresh, true, true, 0.02, nullptr, flush_flags);
      cs.destroy();
      EXPECT_GT(tgt.data.size(), 200);
    }

    // auto_trans_out + Creslength warning + trans_out with mixed thresholds.
    const auto full_bytes = EncodeImage(spec, planes);
    MemorySource in_src(full_bytes);
    kdu_codestream in_cs;
    in_cs.create(&in_src);
    siz_params xsiz;
    ConfigureSiz(xsiz, spec);
    MemoryTarget xtgt;
    kdu_codestream xcs;
    xcs.create(&xsiz, &xtgt);
    ImageSpec xspec = spec;
    xspec.extra_params.push_back("Creslengths=2000,1000");
    ConfigureCodestreamParams(xcs, xspec);
    warning_handler_.clear();
    xcs.auto_trans_out(1, 1, 8, 8, 900, true, nullptr);
    for (int ty = 0; ty < 2; ++ty) {
      for (int tx = 0; tx < 2; ++tx) {
        kdu_tile itile = in_cs.open_tile(kdu_coords(tx, ty));
        kdu_tile otile = xcs.open_tile(kdu_coords(tx, ty));
        for (int r = 0; r <= spec.dwt_levels; ++r) {
          kdu_resolution ires = itile.access_component(0).access_resolution(r);
          kdu_resolution ores = otile.access_component(0).access_resolution(r);
          const int start_band = (r == 0) ? LL_BAND : HL_BAND;
          const int end_band = (r == 0) ? LL_BAND : HH_BAND;
          for (int b = start_band; b <= end_band; ++b) {
            kdu_subband isub = ires.access_subband(b);
            kdu_subband osub = ores.access_subband(b);
            kdu_dims bdims;
            isub.get_valid_blocks(bdims);
            for (int by = 0; by < bdims.size.y; ++by) {
              for (int bx = 0; bx < bdims.size.x; ++bx) {
                const kdu_coords bidx = bdims.pos + kdu_coords(bx, by);
                kdu_block* iblk = isub.open_block(bidx);
                kdu_block* oblk = osub.open_block(bidx);
                oblk->num_passes = iblk->num_passes;
                if (iblk->num_passes > 0) {
                  oblk->set_max_passes(iblk->num_passes, false);
                  int total = 0;
                  for (int p = 0; p < iblk->num_passes; ++p) {
                    oblk->pass_lengths[p] = iblk->pass_lengths[p];
                    oblk->pass_slopes[p] = iblk->pass_slopes[p];
                    total += iblk->pass_lengths[p];
                  }
                  oblk->set_max_bytes(total, false);
                  std::copy_n(iblk->byte_buffer, total, oblk->byte_buffer);
                }
                oblk->missing_msbs = iblk->missing_msbs;
                isub.close_block(iblk);
                osub.close_block(oblk);
              }
            }
          }
        }
        otile.close();
        itile.close();
      }
    }
    xcs.trans_out(900);
    xcs.destroy();
    in_cs.destroy();
    EXPECT_GT(xtgt.data.size(), 150);
  }
}

TEST_F(CodestreamTest,
       MultiThreadedPlockOvlpAsyncSampleAllocatorAndLoadSequential) {
  // 1. Standalone kdu_sample_allocator multi-fragment pre_alloc + finalize +
  // alloc32 + seek_permit hard failure (handle_failed_permit
  // return_on_fail=false).
  {
    kdu_membroker big_broker(1 << 20);
    kdu_sample_allocator alloc;
    EXPECT_TRUE(alloc.configure(&big_broker, 10));  // 1024-byte fragments
    const size_t off1 = alloc.pre_alloc(KDU_SAMPLE_PREC32, 0, 200);
    const size_t off2 = alloc.pre_alloc(KDU_SAMPLE_PREC32, 0, 200);
    const size_t off3 =
        alloc.pre_alloc_block(800);  // crosses fragment boundary
    (void)off2;
    (void)off3;
    ImageSpec spec;
    spec.width = 16;
    spec.height = 16;
    spec.components = 1;
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    alloc.finalize(cs);
    EXPECT_NE(alloc.alloc32(0, 200, off1), nullptr);
    cs.destroy();
  }
  {
    kdu_membroker small_broker(1024);
    kdu_sample_allocator alloc;
    EXPECT_TRUE(alloc.configure(&small_broker, 10));
    error_handler_.clear();
    EXPECT_THROW(alloc.seek_permit(1 << 20, /*return_on_failure=*/false), int);
  }

  // 2. Multi-threaded encode with kdu_multi_analysis(&env) + Cest=yes +
  // env.add_thread() mid-encode (num_threads_changed + do_plock_processing) +
  // multi-threaded open_tiles / clear_service_lists / load_sequential.
  {
    ImageSpec spec;
    spec.width = 128;
    spec.height = 128;
    spec.components = 3;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(64, 64);
    spec.extra_params = {"Cycc=yes",      "Corder=PCRL", "Cprecincts={16,16}",
                         "Cblk={8,8}",    "Clayers=2",   "ORGgen_tlm=4",
                         "ORGgen_plt=yes"};
    const auto planes = MakeTestPlanes(spec);

    kdu_thread_env env;
    env.create();
    env.add_thread();

    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    kdu_codestream out_cs;
    out_cs.create(&siz, &tgt, nullptr, 0, 0, &env);
    ConfigureCodestreamParams(out_cs, spec);
    out_cs.set_max_bytes(12000);
    kdu_long lbytes[2] = {4000, 12000};
    out_cs.auto_flush(1, 1, 16, 16, lbytes, 2, nullptr, true, false, 0.0, &env);

    for (int ty = 0; ty < 2; ++ty) {
      for (int tx = 0; tx < 2; ++tx) {
        if (ty == 1 && tx == 0) {
          env.add_thread();  // Triggers num_threads_changed
        }
        kdu_tile tile = out_cs.open_tile(kdu_coords(tx, ty), &env);
        kdu_dims tdims;
        tile.access_component(0)
            .access_resolution(spec.dwt_levels)
            .get_dims(tdims);
        EXPECT_GT(tdims.size.x, 0);
        kdu_thread_queue tile_queue;
        ASSERT_TRUE(env.attach_queue(&tile_queue, nullptr, nullptr));
        kdu_multi_analysis engine;
        engine.create(out_cs, tile, &env, &tile_queue, KDU_MULTI_XFORM_MT_DWT);
        std::vector<int> rows_pushed(spec.components, 0);
        int max_h = 0;
        for (int c = 0; c < spec.components; ++c) {
          max_h = std::max(max_h, engine.get_size(c).y);
        }
        for (int step = 0; step < max_h; ++step) {
          for (int c = 0; c < spec.components; ++c) {
            const kdu_coords csize = engine.get_size(c);
            kdu_dims cdims;
            out_cs.get_tile_dims(kdu_coords(tx, ty), c, cdims);
            const kdu_coords cpos = cdims.pos;
            if (rows_pushed[c] < csize.y) {
              kdu_line_buf* line = engine.exchange_line(c, nullptr, &env);
              if (line != nullptr) {
                const int y = cpos.y + rows_pushed[c];
                for (int x = 0; x < csize.x; ++x) {
                  WriteLineSample(
                      line, x,
                      planes[c]
                            [static_cast<size_t>(y) * spec.width + cpos.x + x],
                      spec.bit_depth, spec.is_signed);
                }
                engine.exchange_line(c, line, &env);
                rows_pushed[c]++;
              }
            }
          }
        }
        for (int c = 0; c < spec.components; ++c) {
          const kdu_coords csize = engine.get_size(c);
          kdu_dims cdims;
          out_cs.get_tile_dims(kdu_coords(tx, ty), c, cdims);
          const kdu_coords cpos = cdims.pos;
          while (rows_pushed[c] < csize.y) {
            kdu_line_buf* line = engine.exchange_line(c, nullptr, &env);
            ASSERT_NE(line, nullptr);
            const int y = cpos.y + rows_pushed[c];
            for (int x = 0; x < csize.x; ++x) {
              WriteLineSample(
                  line, x,
                  planes[c][static_cast<size_t>(y) * spec.width + cpos.x + x],
                  spec.bit_depth, spec.is_signed);
            }
            engine.exchange_line(c, line, &env);
            rows_pushed[c]++;
          }
        }
        ASSERT_TRUE(env.join(&tile_queue, true));
        engine.destroy(&env);
        tile.close(&env);
      }
    }
    env.join(nullptr, true);
    env.cs_terminate(out_cs);
    (void)out_cs.ready_for_flush(nullptr);
    out_cs.flush(lbytes, 2, nullptr, true, false, 0.0, nullptr);
    out_cs.destroy();
    ASSERT_GT(tgt.data.size(), 500);

    // Multi-threaded decode: open_tiles -> clear_service_lists via
    // apply_input_restrictions(&env) and change_appearance(&env) ->
    // access_tile -> load_sequential modes.
    MemorySource src1(tgt.data);
    kdu_codestream in_cs;
    in_cs.create(&src1, &env);
    in_cs.set_persistent();
    kdu_dims all_tiles;
    in_cs.get_valid_tiles(all_tiles);
    in_cs.open_tiles(all_tiles, /*wait_for_background=*/false, &env);
    in_cs.close_tiles(all_tiles, &env);
    in_cs.apply_input_restrictions(0, 1, 1, 1, nullptr,
                                   KDU_WANT_CODESTREAM_COMPONENTS, &env);
    in_cs.change_appearance(true, false, true, &env);
    in_cs.change_appearance(false, false, false, &env);
    in_cs.apply_input_restrictions(0, 0, 0, 0, nullptr,
                                   KDU_WANT_OUTPUT_COMPONENTS, &env);
    in_cs.open_tiles(all_tiles, /*wait_for_background=*/true, &env);
    kdu_tile first_t =
        in_cs.access_tile(kdu_coords(0, 0), /*wait_for_bkgnd=*/false, &env);
    if (!first_t.exists()) {
      first_t =
          in_cs.access_tile(kdu_coords(0, 0), /*wait_for_bkgnd=*/true, &env);
    }
    ASSERT_TRUE(first_t.exists());
    DecodeTileAllComponents(in_cs, first_t);
    in_cs.close_tiles(all_tiles, &env);
    env.cs_terminate(in_cs);
    in_cs.destroy();

    // load_sequential with WANT_FLUSH_SET + WANT_PACKET_BYTES, min_code_blocks,
    // and skip mode (mode_flags=0).
    MemorySource src2(tgt.data);
    kdu_codestream seq_cs;
    seq_cs.create(&src2, &env);
    for (int ty = 0; ty < 2; ++ty) {
      for (int tx = 0; tx < 2; ++tx) {
        kdu_pdescriptor_set pset;
        ASSERT_TRUE(pset.init(kdu_coords(tx, ty), 2));
        bool tile_done = false;
        kdu_tile lt;
        while (!tile_done) {
          kdu_tile next_lt =
              seq_cs.load_sequential(&pset, tile_done, 1, 0,
                                     KDU_LOAD_SEQUENTIAL_WANT_FLUSH_SET |
                                         KDU_LOAD_SEQUENTIAL_WANT_PACKET_BYTES,
                                     &env);
          if (!next_lt.exists()) break;
          lt = next_lt;
        }
        if (lt.exists()) lt.close(&env);
      }
    }
    env.cs_terminate(seq_cs);
    seq_cs.destroy();

    // Non-tile / non-flush_set min_code_blocks and skip (mode_flags=0) modes.
    MemorySource src3(tgt.data);
    kdu_codestream skip_cs;
    skip_cs.create(&src3);
    kdu_pdescriptor_set pset_cb;
    ASSERT_TRUE(pset_cb.init(kdu_coords(0, 0), 2));
    bool done_cb = false;
    kdu_tile lt_cb;
    while (!done_cb) {
      kdu_tile next_cb = skip_cs.load_sequential(
          &pset_cb, done_cb, 0, 2, KDU_LOAD_SEQUENTIAL_WANT_PACKET_BYTES,
          nullptr);
      if (!next_cb.exists()) break;
      lt_cb = next_cb;
    }
    if (lt_cb.exists()) lt_cb.close();
    kdu_pdescriptor_set pset_skip;
    bool done_skip = false;
    kdu_tile lt_skip =
        skip_cs.load_sequential(&pset_skip, done_skip, 0, 0, 0, nullptr);
    if (lt_skip.exists()) lt_skip.close();
    skip_cs.destroy();
    env.destroy();
  }

  // 3. Threaded kdu_membacked_ovlp_source producer/consumer paths without
  // registering an open-queue event (avoiding unpatched kdu_threads event UB):
  // attach_queue failure on uncreated env, blocking ready_for_enable waking
  // via ovlp_close, pre-discard via rejected descriptor, post_open codestream
  // open + failure, ovlp_reset state checks, and close()/ovlp_shutdown().
  {
    class AsyncOvlpSource : public kdu_membacked_ovlp_source {
     public:
      AsyncOvlpSource(std::vector<std::vector<kdu_byte>> frames,
                      size_t chunk_bytes)
          : frames_(std::move(frames)), chunk_bytes_(chunk_bytes) {}

      bool AdvanceFrame() {
        if (next_frame_ >= frames_.size()) return false;
        cur_frame_ = next_frame_++;
        offset_ = 0;
        return true;
      }

      void SetRejectFirstDescriptor(bool reject) {
        reject_first_descriptor_ = reject;
      }

      void SetOpenCodestreamInPostOpen(bool open_cs) {
        open_codestream_in_post_open_ = open_cs;
      }

      void SetFetchCallback(std::function<void(size_t)> cb) {
        fetch_cb_ = std::move(cb);
      }

      int LastPostOpenTiles() const { return last_post_open_tiles_; }

      kdu_long fetch_data(kdu_long max_bytes, kdu_byte* buffer,
                          bool blocking) override {
        (void)blocking;
        const auto& frame = frames_[cur_frame_];
        if (offset_ >= frame.size()) return 0;
        const size_t n =
            std::min<size_t>({frame.size() - offset_,
                              static_cast<size_t>(max_bytes), chunk_bytes_});
        std::copy_n(frame.data() + offset_, n, buffer);
        offset_ += n;
        return static_cast<kdu_long>(n);
      }

      const kdu_byte* fetch_to_next(size_t& descriptor_length,
                                    kdu_uint64& interpretation,
                                    double& timestamp,
                                    size_t& max_buffer_size) override {
        if (fetch_cb_) fetch_cb_(cur_frame_);
        static constexpr kdu_byte kRejectDesc[2] = {0x4E, 0x4F};
        static constexpr kdu_byte kDefaultDesc[4] = {0x4B, 0x44, 0x55, 0x31};
        if (reject_first_descriptor_) {
          reject_first_descriptor_ = false;
          descriptor_length = sizeof(kRejectDesc);
          interpretation = 1;
          timestamp = 0.5;
          max_buffer_size = 4096;
          return kRejectDesc;
        }
        descriptor_length = sizeof(kDefaultDesc);
        interpretation = 1;
        timestamp = static_cast<double>(cur_frame_ + 1);
        max_buffer_size = 4096;
        return kDefaultDesc;
      }

      bool enable(int service_mode, const kdu_byte* descriptor,
                  size_t descriptor_length, kdu_uint64 interpretation,
                  double timestamp, size_t min_buffer_size,
                  size_t max_buffer_size) override {
        if (descriptor != nullptr && descriptor_length == 2) {
          return false;
        }
        return kdu_membacked_ovlp_source::enable(
            service_mode, descriptor, descriptor_length, interpretation,
            timestamp, min_buffer_size, max_buffer_size);
      }

      int ResetOvlp() { return ovlp_reset(); }
      bool CloseSource() { return close(); }

      bool post_open(kdu_long sequence_idx, kdu_thread_queue* queue,
                     kdu_thread_env* env) override {
        (void)sequence_idx;
        (void)queue;
        (void)env;
        if (!open_codestream_in_post_open_) {
          return true;
        }
        kdu_codestream cs;
        try {
          cs.create(this);
          kdu_dims tile_dims;
          cs.get_valid_tiles(tile_dims);
          last_post_open_tiles_ = tile_dims.area();
          cs.destroy();
          return true;
        } catch (kdu_exception) {
          if (cs.exists()) cs.destroy();
          return false;
        }
      }

     private:
      std::vector<std::vector<kdu_byte>> frames_;
      size_t chunk_bytes_;
      size_t cur_frame_ = 0;
      size_t next_frame_ = 0;
      size_t offset_ = 0;
      bool reject_first_descriptor_ = false;
      bool open_codestream_in_post_open_ = true;
      int last_post_open_tiles_ = 0;
      std::function<void(size_t)> fetch_cb_;
    };

    ImageSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 1;
    const auto valid_frame = EncodeImage(spec, MakeTestPlanes(spec));
    std::vector<kdu_byte> bad_frame(valid_frame.begin(),
                                    valid_frame.begin() + 16);

    kdu_thread_env env;
    env.create();
    env.add_thread();

    // Uncreated thread env fails attach_queue inside ovlp_open -> returns -1.
    {
      AsyncOvlpSource err_src({valid_frame}, 64);
      kdu_thread_env uncreated_env;
      EXPECT_EQ(err_src.ovlp_open(/*blocking=*/false, 1, &uncreated_env), -1);
      err_src.ovlp_shutdown(&uncreated_env);
    }

    AsyncOvlpSource src({valid_frame, bad_frame, valid_frame, valid_frame}, 64);
    EXPECT_EQ(src.ResetOvlp(), -1);  // Nothing enabled/started yet.

    std::mutex mu;
    std::condition_variable cv;
    bool frame1_serviced = false;
    bool frame2_fetching = false;
    bool frame2_serviced = false;
    bool frame3_reset_checked = false;

    src.SetFetchCallback([&](size_t frame_idx) {
      if (frame_idx == 1) {
        std::lock_guard<std::mutex> lk(mu);
        frame2_fetching = true;
        cv.notify_all();
      }
    });

    std::thread producer([&]() {
      const kdu_byte* desc = nullptr;
      size_t dlen = 0;
      kdu_uint64 interp = 0;
      double ts = 0.0;
      size_t max_buf = 0;
      size_t nbytes = 0;
      kdu_int32 tcode = 0;
      kdu_uint32 pre = 0;
      kdu_uint32 post = 0;

      // Frame 1: pre-discard first descriptor, then service valid frame.
      src.SetRejectFirstDescriptor(true);
      ASSERT_TRUE(src.AdvanceFrame());
      EXPECT_TRUE(src.ovlp_service(desc, dlen, interp, ts, max_buf, 64, nbytes,
                                   tcode, pre, post,
                                   /*allow_in_use_pre_discard=*/false,
                                   /*allow_post_discard=*/false));
      EXPECT_EQ(pre, 1);
      {
        std::lock_guard<std::mutex> lk(mu);
        frame1_serviced = true;
      }
      cv.notify_all();

      // Frame 2: blocking ovlp_service waits for consumer ovlp_close if still
      // enabled, then delivers truncated frame.
      ASSERT_TRUE(src.AdvanceFrame());
      EXPECT_TRUE(src.ovlp_service(desc, dlen, interp, ts, max_buf, 64, nbytes,
                                   tcode, pre, post,
                                   /*allow_in_use_pre_discard=*/false,
                                   /*allow_post_discard=*/false));
      {
        std::lock_guard<std::mutex> lk(mu);
        frame2_serviced = true;
      }
      cv.notify_all();

      // Wait until consumer closes frame 2, then service frame 3 and verify
      // ovlp_reset() succeeds before ovlp_open, then finish cleanly.
      {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&]() { return frame3_reset_checked; });
      }
      src.ovlp_service_all_finished();
    });

    {
      std::unique_lock<std::mutex> lk(mu);
      cv.wait(lk, [&]() { return frame1_serviced; });
    }
    EXPECT_EQ(src.ovlp_open(/*blocking=*/true, 1, &env), 1);
    EXPECT_EQ(src.LastPostOpenTiles(), 1);
    EXPECT_EQ(src.ResetOvlp(), 0);  // Active (STARTED).
    {
      std::unique_lock<std::mutex> lk(mu);
      cv.wait(lk, [&]() { return frame2_fetching; });
    }
    EXPECT_TRUE(src.ovlp_close(&env));

    {
      std::unique_lock<std::mutex> lk(mu);
      cv.wait(lk, [&]() { return frame2_serviced; });
    }
    error_handler_.clear();
    EXPECT_EQ(src.ovlp_open(/*blocking=*/true, 2, &env), 0);  // bad_frame
    error_handler_.clear();
    EXPECT_TRUE(src.ovlp_close(&env));

    // Service frame 3 directly on main thread to verify standalone ovlp_reset
    // returns 1 when enabled and not yet started.
    {
      const kdu_byte* desc = nullptr;
      size_t dlen = 0;
      kdu_uint64 interp = 0;
      double ts = 0.0;
      size_t max_buf = 0;
      size_t nbytes = 0;
      kdu_int32 tcode = 0;
      kdu_uint32 pre = 0;
      kdu_uint32 post = 0;
      ASSERT_TRUE(src.AdvanceFrame());
      EXPECT_TRUE(src.ovlp_service(desc, dlen, interp, ts, max_buf, 64, nbytes,
                                   tcode, pre, post, false, false));
      EXPECT_EQ(src.ResetOvlp(), 1);
      EXPECT_FALSE(src.CloseSource());
      ASSERT_TRUE(src.AdvanceFrame());
      src.SetOpenCodestreamInPostOpen(false);
      EXPECT_TRUE(src.ovlp_service(desc, dlen, interp, ts, max_buf, 64, nbytes,
                                   tcode, pre, post, false, false));
      EXPECT_EQ(src.ovlp_open(/*blocking=*/true, 3, &env), 1);
      EXPECT_FALSE(src.CloseSource());
    }
    {
      std::lock_guard<std::mutex> lk(mu);
      frame3_reset_checked = true;
    }
    cv.notify_all();
    producer.join();

    src.ovlp_shutdown(&env);
    env.destroy();
  }
}

TEST_F(CodestreamTest, MembackedCompressedSourcePartiallyServicedAndNotifier) {
  ImageSpec spec;
  spec.width = 96;
  spec.height = 96;
  spec.components = 1;
  spec.dwt_levels = 2;
  const auto planes = MakeTestPlanes(spec);
  std::vector<kdu_byte> bytes = EncodeImage(spec, planes);
  ASSERT_GT(bytes.size(), 5000U);

  class RecordingNotifier : public kdu_compressed_source_notifier {
   public:
    void note_blocking() override {
      std::lock_guard<std::mutex> lk(mu_);
      blocking_calls_++;
      cv_.notify_all();
    }
    void note_unblocked() override {
      std::lock_guard<std::mutex> lk(mu_);
      unblocked_calls_++;
    }
    bool prepare_notify(kdu_long notification_span) override {
      std::lock_guard<std::mutex> lk(mu_);
      prepare_calls_++;
      last_span_ = notification_span;
      return allow_prepare_;
    }
    void notify() override {
      std::lock_guard<std::mutex> lk(mu_);
      notify_calls_++;
      cv_.notify_all();
    }
    void will_not_notify() override {
      std::lock_guard<std::mutex> lk(mu_);
      will_not_notify_calls_++;
    }

    void SetAllowPrepare(bool allow) {
      std::lock_guard<std::mutex> lk(mu_);
      allow_prepare_ = allow;
    }
    void WaitForNotifyCount(int target) {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [&]() { return notify_calls_ >= target; });
    }
    void WaitForBlockingCount(int target) {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [&]() { return blocking_calls_ >= target; });
    }
    int BlockingCalls() const {
      std::lock_guard<std::mutex> lk(mu_);
      return blocking_calls_;
    }
    int UnblockedCalls() const {
      std::lock_guard<std::mutex> lk(mu_);
      return unblocked_calls_;
    }
    int NotifyCalls() const {
      std::lock_guard<std::mutex> lk(mu_);
      return notify_calls_;
    }
    int WillNotNotifyCalls() const {
      std::lock_guard<std::mutex> lk(mu_);
      return will_not_notify_calls_;
    }

   private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool allow_prepare_ = true;
    int blocking_calls_ = 0;
    int unblocked_calls_ = 0;
    int prepare_calls_ = 0;
    int notify_calls_ = 0;
    int will_not_notify_calls_ = 0;
    kdu_long last_span_ = 0;
  };

  class PartialSource : public kdu_membacked_compressed_source {
   public:
    PartialSource(const std::vector<kdu_byte>& payload, size_t step_bytes)
        : payload_(payload), step_bytes_(step_bytes) {}

    bool Enable(int mode, size_t min_buf, size_t max_buf,
                const kdu_byte* desc = nullptr, size_t desc_len = 0) {
      return kdu_membacked_compressed_source::enable(mode, desc, desc_len, 0,
                                                     0.0, min_buf, max_buf);
    }
    bool Open(bool auto_enable) {
      return kdu_membacked_compressed_source::open(auto_enable);
    }
    bool Service(size_t& fetched, kdu_int32& term) {
      return kdu_membacked_compressed_source::service(fetched, term);
    }
    int Reset() { return kdu_membacked_compressed_source::reset(); }
    void SetFailNonBlockingCount(int count) {
      std::lock_guard<std::mutex> lk(mu_);
      fail_nonblocking_count_ = count;
    }
    void SetFinishTermOnEof(kdu_int32 term_code) {
      std::lock_guard<std::mutex> lk(mu_);
      finish_term_on_eof_ = term_code;
    }
    void WaitForFetchCount(int target) {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [&]() { return fetch_count_ >= target; });
    }
    void PauseProducer(bool pause) {
      {
        std::lock_guard<std::mutex> lk(mu_);
        paused_ = pause;
      }
      cv_.notify_all();
    }

   protected:
    kdu_long fetch_data(kdu_long max_bytes, kdu_byte* buffer,
                        bool blocking) override {
      std::unique_lock<std::mutex> lk(mu_);
      ++fetch_count_;
      cv_.notify_all();
      if (!blocking && fail_nonblocking_count_ > 0) {
        --fail_nonblocking_count_;
        return -1;
      }
      if (blocking && paused_) {
        cv_.wait(lk, [&]() { return !paused_; });
      }
      if (pos_ >= payload_.size()) {
        if (finish_term_on_eof_ != 0) {
          kdu_int32 code = finish_term_on_eof_;
          finish_term_on_eof_ = 0;
          lk.unlock();
          finish_fetching(code);
        }
        return 0;
      }
      size_t want =
          std::min(payload_.size() - pos_, static_cast<size_t>(max_bytes));
      if (step_bytes_ > 0) {
        want = std::min(want, step_bytes_);
      }
      std::memcpy(buffer, payload_.data() + pos_, want);
      pos_ += want;
      return static_cast<kdu_long>(want);
    }

   private:
    std::vector<kdu_byte> payload_;
    size_t step_bytes_;
    size_t pos_ = 0;
    std::mutex mu_;
    std::condition_variable cv_;
    int fail_nonblocking_count_ = 0;
    int fetch_count_ = 0;
    kdu_int32 finish_term_on_eof_ = 0;
    bool paused_ = false;
  };

  // 1) Invalid mode error + double enable + PARTIALLY_SERVICED async notify,
  //    blocking notifier fallback, seek/read within and past consumer_span.
  {
    PartialSource bad_src(bytes, 512);
    error_handler_.clear();
    EXPECT_THROW(bad_src.Enable(999, 64, 4096), int);
    EXPECT_FALSE(error_handler_.history().empty());

    PartialSource src(bytes, 64);
    RecordingNotifier notifier;
    size_t total_fetched = 0;
    kdu_int32 term = 0;
    std::thread producer([&]() {
      src.Enable(KDU_MEMBACKED_SOURCE_PARTIALLY_SERVICED, 64,
                 bytes.size() + 256);
      src.Service(total_fetched, term);
    });

    ASSERT_TRUE(src.Open(false));
    EXPECT_FALSE(src.Enable(KDU_MEMBACKED_SOURCE_PARTIALLY_SERVICED, 64, 1024));

    // Wait until service() has entered in_service in PARTIALLY_SERVICED mode.
    size_t probe_span = 64;
    for (; probe_span < 2048; probe_span += 64) {
      RecordingNotifier warmup;
      warmup.SetAllowPrepare(true);
      src.SetFailNonBlockingCount(10);
      int wflags = src.load_memory(probe_span, /*blocking=*/false, &warmup);
      if (wflags & KDU_SOURCE_WILL_NOTIFY) {
        warmup.WaitForNotifyCount(1);
        break;
      }
      std::this_thread::yield();
    }
    src.SetFailNonBlockingCount(0);
    ASSERT_LT(probe_span, 2048U);

    src.SetFailNonBlockingCount(10);
    notifier.SetAllowPrepare(true);
    int flags =
        src.load_memory(probe_span + 512, /*blocking=*/false, &notifier);
    EXPECT_NE(flags & KDU_SOURCE_WILL_NOTIFY, 0);
    notifier.WaitForNotifyCount(1);
    src.SetFailNonBlockingCount(0);
    EXPECT_GE(notifier.NotifyCalls(), 1);

    src.SetFailNonBlockingCount(10);
    notifier.SetAllowPrepare(false);
    flags = src.load_memory(probe_span + 1024, /*blocking=*/false, &notifier);
    src.SetFailNonBlockingCount(0);
    EXPECT_NE(flags & KDU_SOURCE_SPAN_EXISTS, 0);
    EXPECT_GE(notifier.BlockingCalls(), 1);
    EXPECT_EQ(notifier.BlockingCalls(), notifier.UnblockedCalls());

    flags = src.load_memory(probe_span + 1536, /*blocking=*/true, &notifier);
    EXPECT_NE(flags & KDU_SOURCE_SPAN_EXISTS, 0);

    // seek() past consumer_span triggers load_memory internally, then full load
    // allows seeking inside consumer_span and reading remaining tail bytes.
    EXPECT_TRUE(src.seek(static_cast<kdu_long>(probe_span + 2048)));
    flags = src.load_memory(static_cast<kdu_long>(bytes.size()) + 1,
                            /*blocking=*/true, nullptr);
    EXPECT_NE(flags & KDU_SOURCE_IS_COMPLETE, 0);
    EXPECT_TRUE(src.seek(static_cast<kdu_long>(bytes.size() - 64)));
    kdu_byte tail[128];
    EXPECT_EQ(src.read(tail, 128), 64);
    EXPECT_TRUE(src.close());
    producer.join();
    EXPECT_EQ(total_fetched, bytes.size());
  }

  // 2) Buffer size cap truncation (FULLY_SERVICED & UNSERVICED) and
  //    finish_fetching() termination code propagation.
  {
    PartialSource small_fs(bytes, 64);
    size_t fetched = 0;
    kdu_int32 term = -1;
    ASSERT_TRUE(small_fs.Enable(KDU_MEMBACKED_SOURCE_FULLY_SERVICED, 64, 128));
    ASSERT_TRUE(small_fs.Open(false));
    std::thread producer([&]() { small_fs.Service(fetched, term); });
    small_fs.WaitForFetchCount(1);
    int flags = small_fs.load_memory(4096, /*blocking=*/true, nullptr);
    EXPECT_NE(flags & KDU_SOURCE_IS_COMPLETE, 0);
    EXPECT_TRUE(small_fs.close());
    producer.join();
    EXPECT_EQ(fetched, 128U);
    EXPECT_EQ(term, 0);

    PartialSource small_unserviced(bytes, 64);
    RecordingNotifier unserv_notifier;
    unserv_notifier.SetAllowPrepare(false);
    ASSERT_TRUE(
        small_unserviced.Enable(KDU_MEMBACKED_SOURCE_UNSERVICED, 64, 128));
    ASSERT_TRUE(small_unserviced.Open(false));
    small_unserviced.SetFailNonBlockingCount(10);
    flags = small_unserviced.load_memory(4096, /*blocking=*/false,
                                         &unserv_notifier);
    EXPECT_NE(flags & KDU_SOURCE_IS_COMPLETE, 0);
    EXPECT_GE(unserv_notifier.BlockingCalls(), 1);
    EXPECT_TRUE(small_unserviced.close());

    std::vector<kdu_byte> prefix(bytes.begin(), bytes.begin() + 200);
    PartialSource finish_unserv(prefix, 64);
    finish_unserv.SetFinishTermOnEof(7);
    ASSERT_TRUE(
        finish_unserv.Enable(KDU_MEMBACKED_SOURCE_UNSERVICED, 64, 4096));
    ASSERT_TRUE(finish_unserv.Open(false));
    flags = finish_unserv.load_memory(4096, /*blocking=*/true, nullptr);
    EXPECT_NE(flags & KDU_SOURCE_IS_COMPLETE, 0);
    EXPECT_EQ(finish_unserv.get_term_code(), 7);
    EXPECT_TRUE(finish_unserv.close());

    PartialSource finish_serv(prefix, 64);
    finish_serv.SetFinishTermOnEof(9);
    ASSERT_TRUE(
        finish_serv.Enable(KDU_MEMBACKED_SOURCE_FULLY_SERVICED, 64, 4096));
    ASSERT_TRUE(finish_serv.Open(false));
    size_t serv_fetched = 0;
    kdu_int32 serv_term = 0;
    std::thread serv_prod(
        [&]() { finish_serv.Service(serv_fetched, serv_term); });
    flags = finish_serv.load_memory(4096, /*blocking=*/true, nullptr);
    EXPECT_NE(flags & KDU_SOURCE_IS_COMPLETE, 0);
    EXPECT_EQ(finish_serv.get_term_code(), 9);
    serv_prod.join();
    EXPECT_EQ(serv_term, 9);
    EXPECT_TRUE(finish_serv.close());
  }

  // 3) FULLY_SERVICED reset()-before-open, switching pending_notifier ->
  //    will_not_notify, async EOF notification, and
  //    note_blocking/note_unblocked.
  {
    PartialSource full_src(bytes, 128);
    RecordingNotifier n1;
    RecordingNotifier n2;
    n1.SetAllowPrepare(true);
    n2.SetAllowPrepare(true);

    full_src.PauseProducer(true);
    ASSERT_TRUE(full_src.Enable(KDU_MEMBACKED_SOURCE_FULLY_SERVICED, 64,
                                bytes.size() + 1024));
    EXPECT_EQ(full_src.Reset(), 1);
    EXPECT_EQ(full_src.Reset(), -1);
    ASSERT_TRUE(full_src.Enable(KDU_MEMBACKED_SOURCE_FULLY_SERVICED, 64,
                                bytes.size() + 1024));
    ASSERT_TRUE(full_src.Open(false));

    size_t fetched = 0;
    kdu_int32 term = 0;
    std::thread producer([&]() { full_src.Service(fetched, term); });

    int f1 = full_src.load_memory(bytes.size() + 512, false, &n1);
    int f2 = full_src.load_memory(bytes.size() + 600, false, &n2);
    EXPECT_NE(f1 & KDU_SOURCE_WILL_NOTIFY, 0);
    EXPECT_NE(f2 & KDU_SOURCE_WILL_NOTIFY, 0);
    EXPECT_EQ(n1.WillNotNotifyCalls(), 1);

    full_src.PauseProducer(false);
    n2.WaitForNotifyCount(1);
    producer.join();
    EXPECT_EQ(n2.NotifyCalls(), 1);
    EXPECT_EQ(full_src.Reset(), 0);
    EXPECT_TRUE(full_src.close());
  }

  {
    PartialSource full_src(bytes, 128);
    RecordingNotifier n3;
    n3.SetAllowPrepare(false);

    full_src.PauseProducer(true);
    ASSERT_TRUE(full_src.Enable(KDU_MEMBACKED_SOURCE_FULLY_SERVICED, 64,
                                bytes.size() + 1024));
    ASSERT_TRUE(full_src.Open(false));

    size_t fetched = 0;
    kdu_int32 term = 0;
    std::thread producer([&]() { full_src.Service(fetched, term); });
    std::thread unpauser([&]() {
      n3.WaitForBlockingCount(1);
      full_src.PauseProducer(false);
    });

    int flags =
        full_src.load_memory(bytes.size() + 700, /*blocking=*/false, &n3);
    EXPECT_NE(flags & KDU_SOURCE_IS_COMPLETE, 0);
    unpauser.join();
    producer.join();
    EXPECT_GE(n3.BlockingCalls(), 1);
    EXPECT_EQ(n3.BlockingCalls(), n3.UnblockedCalls());
    EXPECT_TRUE(full_src.close());
  }
}

TEST_F(CodestreamTest,
       SampleAllocatorFragmentsReallocOverflowAndBrokerDeficit) {
  class ExpandableBroker : public kdu_membroker {
   public:
    explicit ExpandableBroker(kdu_int64 initial_limit)
        : kdu_membroker(initial_limit), initial_limit_(initial_limit) {}
    ~ExpandableBroker() override { detach_from_parent(); }
    void SetDeny(bool deny) { deny_ = deny; }
    void SetAutoExpand(bool expand) {
      if (expand) {
        attach_to_parent(&unlimited_parent_, initial_limit_);
      } else {
        detach_from_parent();
      }
    }
    kdu_int64 request(kdu_int64 min_bytes, kdu_int64 pref_bytes,
                      kdu_int64 alloc_bytes) override {
      if (deny_) {
        return 0;
      }
      return kdu_membroker::request(min_bytes, pref_bytes, alloc_bytes);
    }

   private:
    kdu_membroker unlimited_parent_{-1};
    kdu_int64 initial_limit_;
    bool deny_ = false;
  };

  ImageSpec spec;
  spec.width = 16;
  spec.height = 16;
  spec.components = 1;
  siz_params siz;
  ConfigureSiz(siz, spec);
  MemoryTarget tgt;
  kdu_codestream cs;
  cs.create(&siz, &tgt);

  // 1) Reconfiguring with a new broker while a permit is active throws, and a
  //    denied broker fails seek_permit(..., return_on_failure=true) cleanly.
  {
    ExpandableBroker denied_broker(256);
    kdu_membroker other_broker(1024);
    kdu_sample_allocator alloc;
    ASSERT_TRUE(
        alloc.configure(&denied_broker, KDU_SAMPLE_ALLOCATOR_MIN_FRAG_BITS));
    ASSERT_TRUE(alloc.seek_permit(128));
    error_handler_.clear();
    EXPECT_THROW(
        alloc.configure(&other_broker, KDU_SAMPLE_ALLOCATOR_MIN_FRAG_BITS),
        int);
    EXPECT_FALSE(error_handler_.history().empty());
    alloc.release_permit(128);
    denied_broker.SetDeny(true);
    EXPECT_FALSE(alloc.seek_permit(1024, /*return_on_failure=*/true));
    error_handler_.clear();
    EXPECT_THROW(alloc.seek_permit(1024, /*return_on_failure=*/false), int);
    EXPECT_FALSE(error_handler_.history().empty());
  }

  // 2) Small frag_size (MIN_FRAG_BITS = 256 kB) + large pre_alloc calls forces
  //    advance_pre_frag growth (1 -> 4 -> 16), plus broker deficit expansion,
  //    invalid release() check, restart + larger realloc, and explicit
  //    release().
  {
    ExpandableBroker broker(512);
    broker.SetAutoExpand(true);
    kdu_sample_allocator alloc;
    ASSERT_TRUE(alloc.configure(&broker, KDU_SAMPLE_ALLOCATOR_MIN_FRAG_BITS));
    for (int i = 0; i < 6; ++i) {
      alloc.pre_alloc(KDU_SAMPLE_PREC32, 0, 70000);
    }
    alloc.finalize(cs);
    error_handler_.clear();
    EXPECT_THROW(alloc.release(), int);
    EXPECT_FALSE(error_handler_.history().empty());

    alloc.restart();
    alloc.pre_align(256);
    alloc.pre_alloc(KDU_SAMPLE_PREC64, 0, 512);
    alloc.finalize(cs);

    alloc.restart();
    alloc.release();
  }

  // 3) Integer overflow in pre_alloc sets prealloc_overflow and throws on
  //    finalize().
  {
    kdu_membroker broker(1LL << 20);
    kdu_sample_allocator alloc;
    ASSERT_TRUE(alloc.configure(&broker, KDU_SAMPLE_ALLOCATOR_MIN_FRAG_BITS));
    alloc.pre_alloc(KDU_SAMPLE_PREC32, 0, 1 << 28, 16);
    error_handler_.clear();
    EXPECT_THROW(alloc.finalize(cs), int);
    EXPECT_FALSE(error_handler_.history().empty());
  }

  cs.destroy();
}

TEST_F(CodestreamTest, MctOldReversibleSsModelProfilesCtypesAndBuffering) {
  // 1) Old-style reversible MAT SS model: encode reversible MATRIX MCT, clear
  //    Cmct in the COD marker segment, and restrict output components on
  //    decode.
  {
    ImageSpec spec;
    spec.width = 24;
    spec.height = 24;
    spec.components = 3;
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=3",
        "Msigned=no",
        "Mprecision=8",
        "Mmatrix_size:I1=12",
        "Mmatrix_coeffs:I1=1,0,1,0,1,1,1,0,1,0,1,-1",
        "Mstage_inputs:I1={0,2}",
        "Mstage_outputs:I1={0,2}",
        "Mstage_collections:I1={3,3}",
        "Mstage_xforms:I1={MATRIX,1,0,1,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    const auto planes = MakeTestPlanes(spec);
    std::vector<kdu_byte> bytes = EncodeImage(spec, planes);
    for (size_t i = 0; i + 8 < bytes.size(); ++i) {
      if (bytes[i] == 0xFF && bytes[i + 1] == 0x52) {
        bytes[i + 8] &= ~static_cast<kdu_byte>(0x06);
        break;
      }
    }
    MemorySource src(bytes);
    kdu_codestream cs;
    cs.create(&src);
    cs.apply_input_restrictions(0, 1, 0, 0, nullptr,
                                KDU_WANT_OUTPUT_COMPONENTS);
    EXPECT_EQ(cs.get_num_components(true), 1);
    cs.destroy();
  }

  // 2) Two-stage MCT where stage 2 takes more inputs than stage 1 outputs
  //    (via a repeated Stage 1 output in Stage 2 inputs), plus error when
  //    Stage 1 references an out-of-range codestream component.
  {
    ImageSpec spec;
    spec.width = 16;
    spec.height = 16;
    spec.components = 3;
    spec.reversible = false;
    spec.dwt_levels = 1;
    spec.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=4",
        "Msigned=no",
        "Mprecision=8",
        "Mmatrix_size:I1=9",
        "Mmatrix_coeffs:I1=1,0,0,0,1,0,0,0,1",
        "Mmatrix_size:I2=9",
        "Mmatrix_coeffs:I2=1,0,0,0,1,0,0,0,1",
        "Mmatrix_size:I3=1",
        "Mmatrix_coeffs:I3=1",
        "Mstage_inputs:I1={0,2}",
        "Mstage_outputs:I1={0,2}",
        "Mstage_collections:I1={3,3}",
        "Mstage_xforms:I1={MATRIX,1,0,0,0}",
        "Mstage_inputs:I2={0,0},{0,2}",
        "Mstage_outputs:I2={0,3}",
        "Mstage_collections:I2={1,1},{3,3}",
        "Mstage_xforms:I2={MATRIX,3,0,0,0},{MATRIX,2,0,0,0}",
        "Mnum_stages=2",
        "Mstages=1,2",
    };
    ImageSpec plane_spec = spec;
    plane_spec.components = 4;
    const auto planes = MakeTestPlanes(plane_spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ConfigureCodestreamParams(cs, spec);
    cs.access_siz()->finalize_all();
    {
      kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
      kdu_multi_analysis engine;
      engine.create(cs, tile);
      for (int y = 0; y < spec.height; ++y) {
        for (int c = 0; c < 4; ++c) {
          kdu_line_buf* line = engine.exchange_line(c, nullptr);
          ASSERT_NE(line, nullptr) << "y=" << y << " c=" << c;
          const kdu_int32* src = planes[c].data() + y * spec.width;
          if (line->get_buf16() != nullptr) {
            kdu_sample16* dst = line->get_buf16();
            for (int x = 0; x < spec.width; ++x) {
              dst[x].ival = static_cast<kdu_int16>(src[x]);
            }
          } else if (line->is_absolute()) {
            kdu_sample32* dst = line->get_buf32();
            for (int x = 0; x < spec.width; ++x) {
              dst[x].ival = src[x];
            }
          } else {
            kdu_sample32* dst = line->get_buf32();
            for (int x = 0; x < spec.width; ++x) {
              dst[x].fval = static_cast<float>(src[x]) / 255.0F - 0.5F;
            }
          }
          engine.exchange_line(c, line);
        }
      }
      engine.destroy();
      tile.close();
    }
    EXPECT_TRUE(cs.ready_for_flush());
    kdu_long layer_bytes = 65536;
    cs.flush(&layer_bytes, 1);
    cs.destroy();
    const auto& bytes = tgt.data;
    ASSERT_GE(bytes.size(), 2U);
    EXPECT_EQ(bytes[0], 0xFF);
    EXPECT_EQ(bytes[1], 0x4F);
    MemorySource src(bytes);
    const auto decoded = DecodeFullImage(&src);
    EXPECT_EQ(decoded.size(), 4);

    ImageSpec bad_mct = spec;
    bad_mct.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=4",
        "Msigned=no",
        "Mprecision=8",
        "Mmatrix_size:I1=12",
        "Mmatrix_coeffs:I1=1,0,0,0,0,1,0,0,0,0,1,0",
        "Mmatrix_size:I2=16",
        "Mmatrix_coeffs:I2=1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1",
        "Mstage_inputs:I1={0,3}",
        "Mstage_outputs:I1={0,2}",
        "Mstage_collections:I1={4,3}",
        "Mstage_xforms:I1={MATRIX,1,0,0,0}",
        "Mstage_inputs:I2={0,2},{0,0}",
        "Mstage_outputs:I2={0,3}",
        "Mstage_collections:I2={4,4}",
        "Mstage_xforms:I2={MATRIX,2,0,0,0}",
        "Mnum_stages=2",
        "Mstages=1,2",
    };
    siz_params bad_siz;
    ConfigureSiz(bad_siz, bad_mct);
    MemoryTarget bad_tgt;
    kdu_codestream bad_cs;
    bad_cs.create(&bad_siz, &bad_tgt);
    ConfigureCodestreamParams(bad_cs, bad_mct);
    error_handler_.clear();
    EXPECT_THROW(bad_cs.open_tile(kdu_coords(0, 0)), int);
    EXPECT_FALSE(error_handler_.history().empty());
    bad_cs.destroy();
  }

  // 3) Profile-0 and Profile-1 construct_common violation warnings + Ctype
  //    visual weights + interchange membroker failure + configure/truncate.
  {
    auto create_profile_warn = [&](const ImageSpec& s) {
      siz_params siz;
      ConfigureSiz(siz, s);
      MemoryTarget tgt;
      kdu_codestream cs;
      warning_handler_.clear();
      cs.create(&siz, &tgt);
      cs.destroy();
      EXPECT_FALSE(warning_handler_.history().empty());
    };
    ImageSpec p0_tile;
    p0_tile.width = 128;
    p0_tile.height = 128;
    p0_tile.tile_size = kdu_coords(64, 64);
    p0_tile.extra_params = {"Sprofile=PROFILE0"};
    create_profile_warn(p0_tile);

    ImageSpec p0_orig;
    p0_orig.width = 64;
    p0_orig.height = 64;
    p0_orig.img_origin = kdu_coords(2, 2);
    p0_orig.extra_params = {"Sprofile=PROFILE0"};
    create_profile_warn(p0_orig);

    ImageSpec p0_sub;
    p0_sub.width = 64;
    p0_sub.height = 64;
    p0_sub.subsampling = {kdu_coords(3, 1)};
    p0_sub.extra_params = {"Sprofile=PROFILE0"};
    create_profile_warn(p0_sub);

    ImageSpec p1_nonsquare;
    p1_nonsquare.width = 128;
    p1_nonsquare.height = 128;
    p1_nonsquare.tile_size = kdu_coords(64, 128);
    p1_nonsquare.extra_params = {"Sprofile=PROFILE1"};
    create_profile_warn(p1_nonsquare);

    ImageSpec p1_bigtile;
    p1_bigtile.width = 2560;
    p1_bigtile.height = 2560;
    p1_bigtile.tile_size = kdu_coords(1280, 1280);
    p1_bigtile.extra_params = {"Sprofile=PROFILE1"};
    create_profile_warn(p1_bigtile);

    // Ctype Y/Cb/Cr visual weight setup + configure_simultaneous + truncation.
    ImageSpec cspec;
    cspec.width = 32;
    cspec.height = 32;
    cspec.components = 4;
    cspec.reversible = false;
    cspec.extra_params = {"Cycc=no", "Ctype=Y,Cb,Cr,N"};
    const auto cplanes = MakeTestPlanes(cspec);
    siz_params csiz;
    ConfigureSiz(csiz, cspec);
    MemoryTarget ctgt1, ctgt2;
    kdu_codestream c1, c2;
    c1.create(&csiz, &ctgt1);
    ConfigureCodestreamParams(c1, cspec);
    c1.set_block_truncation(-1);
    c1.set_block_truncation(1);
    c1.configure_simultaneous_processing_fragments(64, 2, 4);
    c2.create(&csiz, &ctgt2);
    ConfigureCodestreamParams(c2, cspec);
    c2.configure_simultaneous_processing_fragments(c1);
    {
      kdu_tile tile = c1.open_tile(kdu_coords(0, 0));
      PushTileData(c1, tile, cplanes, cspec);
      tile.close();
      kdu_long lbytes[1] = {500};
      c1.flush(lbytes, 1);
    }
    {
      kdu_tile tile = c2.open_tile(kdu_coords(0, 0));
      PushTileData(c2, tile, cplanes, cspec);
      tile.close();
      kdu_long lbytes[1] = {500};
      c2.flush(lbytes, 1);
    }
    c2.destroy();
    c1.destroy();

    // Interchange create() membroker denial + grant.
    kdu_codestream xcs;
    kdu_membroker zero_broker(0);
    error_handler_.clear();
    EXPECT_THROW(xcs.create(&csiz, nullptr, &zero_broker), int);
    EXPECT_FALSE(error_handler_.history().empty());
    kdu_membroker big_broker(1 << 20);
    xcs.create(&csiz, nullptr, &big_broker);
    EXPECT_TRUE(xcs.exists());
    xcs.destroy();
  }

  // 4) share_buffering errors + MT share_buffering + add_flush_stats across
  //    restart (covering link_tile_comp and structural mismatch error).
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {"Corder=PCRL", "Cprecincts={8,8}", "Cblk={8,8}",
                         "Clayers=1", "Scbr={1,64}"};
    const auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);

    kdu_membroker b1(1 << 20), b2(1 << 20);
    MemoryTarget t1, t2;
    t1.broker = &b1;
    t2.broker = &b2;
    kdu_codestream cs1, cs2;
    cs1.create(&siz, &t1);
    cs2.create(&siz, &t2);
    error_handler_.clear();
    EXPECT_THROW(cs2.share_buffering(cs1), int);
    EXPECT_FALSE(error_handler_.history().empty());
    cs2.destroy();
    cs1.destroy();

    kdu_thread_env env;
    env.create();
    env.add_thread();
    MemoryTarget mt1, mt2;
    kdu_codestream mcs1, mcs2;
    mcs1.create(&siz, &mt1, nullptr, 0, 0, &env);
    ConfigureCodestreamParams(mcs1, spec);
    mcs2.create(&siz, &mt2, nullptr, 0, 0, &env);
    ConfigureCodestreamParams(mcs2, spec);
    mcs2.share_buffering(mcs1);
    {
      kdu_tile tile = mcs1.open_tile(kdu_coords(0, 0), &env);
      PushTileData(mcs1, tile, planes, spec);
      tile.close(&env);
      kdu_long lb[1] = {1500};
      mcs1.flush(lb, 1, nullptr, true, false, 0.0, &env);
    }
    env.cs_terminate(mcs2);
    env.cs_terminate(mcs1);
    mcs2.destroy();
    mcs1.destroy();
    env.destroy();

    ImageSpec fspec = spec;
    fspec.dwt_levels = 1;
    fspec.extra_params = {"Corder=PCRL", "Cblk={8,8}", "Clayers=1",
                          "Scbr={1,64}"};
    siz_params fsiz;
    ConfigureSiz(fsiz, fspec);
    MemoryTarget ft1, ft2, ft3;
    kdu_codestream fcs;
    fcs.create(&fsiz, &ft1);
    kdu_params* fcod = fcs.access_siz()->access_cluster(COD_params);
    fcod->set(Cprecincts, 0, 0, 8);
    fcod->set(Cprecincts, 0, 1, 8);
    ConfigureCodestreamParams(fcs, fspec);
    fcs.enable_restart();
    kdu_flush_stats fstats = fcs.add_flush_stats(0, 5, 0, 1);
    fstats.set_max_passes(true, true, 0, -5);
    fstats.set_max_passes(true, true, 0, 300);
    {
      kdu_tile tile = fcs.open_tile(kdu_coords(0, 0));
      PushTileData(fcs, tile, planes, spec);
      tile.close();
      kdu_long lb[1] = {1500};
      fcs.flush(lb, 1);
    }
    fstats.advance(1);
    fcs.restart(&ft2);
    {
      kdu_tile tile = fcs.open_tile(kdu_coords(0, 0));
      PushTileData(fcs, tile, planes, spec);
      tile.close();
      kdu_long lb[1] = {1500};
      fcs.flush(lb, 1);
    }
    fstats.advance(1);
    fcs.restart(&ft3);
    ASSERT_TRUE(fcs.access_siz()->parse_string("Cprecincts={16,16}"));
    fcs.access_siz()->finalize_all();
    error_handler_.clear();
    EXPECT_THROW(fcs.open_tile(kdu_coords(0, 0)), int);
    EXPECT_FALSE(error_handler_.history().empty());
    fcs.destroy();
  }
}

TEST_F(CodestreamTest, CorruptMarkersPocRestartInputAndReslengthConstraints) {
  // 1) Marker reader error/warning paths + resilient/fussy unexpected markers
  //    inside multi-buffer packet bodies.
  {
    ImageSpec spec;
    spec.width = 128;
    spec.height = 128;
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.extra_params = {"Cblk={64,64}", "Cuse_sop=yes"};
    const auto planes = MakeTestPlanes(spec);
    std::vector<kdu_byte> bytes = EncodeImage(spec, planes);
    ASSERT_GT(bytes.size(), 6000U);

    // Unknown marker code (0xFF7E) in main header before COD.
    std::vector<kdu_byte> with_unknown = bytes;
    for (size_t i = 0; i + 4 < with_unknown.size(); ++i) {
      if (with_unknown[i] == 0xFF && with_unknown[i + 1] == 0x52) {
        const kdu_byte unk[6] = {0xFF, 0x7E, 0x00, 0x04, 0x11, 0x22};
        with_unknown.insert(with_unknown.begin() + i, unk, unk + 6);
        break;
      }
    }
    {
      MemorySource src(with_unknown);
      warning_handler_.clear();
      const auto dec = DecodeFullImage(&src);
      EXPECT_EQ(dec.size(), 1);
      EXPECT_FALSE(warning_handler_.history().empty());
    }

    // Trailing bytes after EOC: non-resilient stops early, resilient warns.
    std::vector<kdu_byte> trailing_eoc = bytes;
    trailing_eoc.push_back(0xAA);
    trailing_eoc.push_back(0xBB);
    {
      MemorySource src(trailing_eoc);
      kdu_codestream cs;
      cs.create(&src);
      kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
      DecodeTileFirstComponent(cs, tile);
      tile.close();
      cs.destroy();
    }
    {
      MemorySource src(trailing_eoc);
      kdu_codestream cs;
      cs.create(&src);
      cs.set_resilient();
      kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
      DecodeTileFirstComponent(cs, tile);
      tile.close();
      cs.destroy();
    }

    // Corrupt SOP inside code-block body spanning > 1 kd_code_buffer (resilient
    // ignores invalid length 0x0003 and throws on valid SOP 0x0004; fussy
    // rejects non-SOP/SOT marker 0xFF99).
    size_t sod_pos = 0;
    for (size_t i = 0; i + 2 < bytes.size(); ++i) {
      if (bytes[i] == 0xFF && bytes[i + 1] == 0x93) {
        sod_pos = i + 2;
        break;
      }
    }
    ASSERT_GT(sod_pos, 0U);
    if (sod_pos + 120 < bytes.size() - 2) {
      std::vector<kdu_byte> bad_sop_body = bytes;
      bad_sop_body[sod_pos + 100] = 0xFF;
      bad_sop_body[sod_pos + 101] = 0x91;
      bad_sop_body[sod_pos + 102] = 0x00;
      bad_sop_body[sod_pos + 103] = 0x03;
      MemorySource src(bad_sop_body);
      kdu_codestream cs;
      cs.create(&src);
      cs.set_resilient();
      kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
      DecodeTileFirstComponent(cs, tile);
      tile.close();
      cs.destroy();

      std::vector<kdu_byte> bona_sop_body = bytes;
      bona_sop_body[sod_pos + 80] = 0xFF;
      bona_sop_body[sod_pos + 81] = 0x91;
      bona_sop_body[sod_pos + 82] = 0x00;
      bona_sop_body[sod_pos + 83] = 0x04;
      MemorySource src2(bona_sop_body);
      kdu_codestream cs2;
      cs2.create(&src2);
      cs2.set_resilient();
      error_handler_.clear();
      kdu_tile tile2 = cs2.open_tile(kdu_coords(0, 0));
      DecodeTileFirstComponent(cs2, tile2);
      tile2.close();
      cs2.destroy();
    }

    // Corrupt SOT (length != 10) right after SOD / second SOT.
    std::vector<kdu_byte> bad_sot = bytes;
    if (bad_sot.size() > 6) {
      bad_sot[bad_sot.size() - 2] = 0xFF;
      bad_sot[bad_sot.size() - 1] = 0x90;
      bad_sot.push_back(0x00);
      bad_sot.push_back(0x05);
      bad_sot.push_back(0x00);
      bad_sot.push_back(0xFF);
      bad_sot.push_back(0xD9);
      MemorySource src(bad_sot);
      kdu_codestream cs;
      cs.create(&src);
      cs.set_resilient();
      kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
      DecodeTileFirstComponent(cs, tile);
      tile.close();
      cs.destroy();
    }
  }

  // 2) Multi-POC across tile-parts, Profile-0 non-zero POC warning, and
  //    incomplete POC error.
  {
    ImageSpec spec;
    spec.width = 48;
    spec.height = 48;
    spec.components = 2;
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.extra_params = {
        "Sprofile=PROFILE0",
        "Cycc=no",
        "Clayers=1",
        "Porder={1,1,1,2,2,CPRL}",
    };
    const auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    kdu_codestream cs;
    warning_handler_.clear();
    cs.create(&siz, &tgt);
    ConfigureCodestreamParams(cs, spec);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    PushTileData(cs, tile, planes, spec);
    tile.close();
    error_handler_.clear();
    EXPECT_THROW(cs.flush(nullptr, 1), int);
    EXPECT_FALSE(error_handler_.history().empty());
    EXPECT_FALSE(warning_handler_.history().empty());
    cs.destroy();
  }

  // 3) apply_input_restrictions errors + persistent restart() with changed
  //    coding params and component appearance mapping.
  {
    ImageSpec spec1;
    spec1.width = 32;
    spec1.height = 32;
    spec1.components = 3;
    spec1.dwt_levels = 1;
    const auto bytes1 = EncodeImage(spec1, MakeTestPlanes(spec1));

    ImageSpec spec2 = spec1;
    spec2.dwt_levels = 2;
    const auto bytes2 = EncodeImage(spec2, MakeTestPlanes(spec2));

    // Non-persistent apply_input_restrictions errors.
    {
      MemorySource src(bytes1);
      kdu_codestream cs;
      cs.create(&src);
      kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
      error_handler_.clear();
      EXPECT_THROW(cs.apply_input_restrictions(0, 1, 0, 0, nullptr), int);
      tile.close();
      error_handler_.clear();
      EXPECT_THROW(cs.apply_input_restrictions(0, 1, 0, 0, nullptr), int);
      cs.destroy();
    }

    // Persistent + enable_restart + change_appearance -> restart(&src2) copies
    // component mapping across any_changes() rebuild.
    {
      MemorySource src1(bytes1);
      MemorySource src2(bytes2);
      kdu_codestream cs;
      cs.create(&src1);
      cs.set_persistent();
      cs.enable_restart();
      cs.change_appearance(true, false, false);
      cs.apply_input_restrictions(0, 2, 0, 1, nullptr,
                                  KDU_WANT_OUTPUT_COMPONENTS);
      kdu_tile t1 = cs.open_tile(kdu_coords(0, 0));
      DecodeTileAllComponents(cs, t1);
      t1.close();
      cs.restart(&src2);
      kdu_tile t2 = cs.open_tile(kdu_coords(0, 0));
      DecodeTileAllComponents(cs, t2);
      t2.close();
      cs.destroy();
    }
  }

  // 4) Creslengths hard constraints triggering pcrd_trim bisection + warning +
  //    second-pass finalize + incremental multi-tile
  //    cache_write_ready_precincts and multi-segment TLM backtrack.
  {
    ImageSpec spec;
    spec.width = 96;
    spec.height = 96;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {"Corder=RPCL", "Clayers=3", "Creslengths=1,1,1"};
    const auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ConfigureCodestreamParams(cs, spec);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    PushTileData(cs, tile, planes, spec);
    tile.close();
    kdu_long lbytes[3] = {500, 1000, 2000};
    kdu_uint16 thresh[3] = {45000, 40000, 35000};
    warning_handler_.clear();
    cs.flush(lbytes, 3, thresh, /*trim_to_rate=*/true,
             /*record_in_comseg=*/false, /*tolerance=*/0.05, nullptr,
             KDU_FLUSH_USES_THRESHOLDS_AND_SIZES);
    EXPECT_FALSE(warning_handler_.history().empty());
    cs.destroy();

    // Multi-tile incremental flush exercising cache_write_ready_precincts when
    // only a subset of tiles are ready, plus multi-segment TLM backtrack.
    ImageSpec mt_spec;
    mt_spec.width = 120;
    mt_spec.height = 8;
    mt_spec.components = 1;
    mt_spec.reversible = true;
    mt_spec.dwt_levels = 0;
    mt_spec.tile_size = kdu_coords(1, 8);
    mt_spec.extra_params = {"ORGgen_tlm=100", "ORGtlm_style={short,long}"};
    const auto mt_planes = MakeTestPlanes(mt_spec);
    siz_params mt_siz;
    ConfigureSiz(mt_siz, mt_spec);
    MemoryTarget mt_tgt;
    mt_tgt.allow_rewrite = true;
    kdu_codestream mt_cs;
    mt_cs.create(&mt_siz, &mt_tgt);
    ConfigureCodestreamParams(mt_cs, mt_spec);
    kdu_dims tile_range;
    mt_cs.get_valid_tiles(tile_range);
    for (int tx = 0; tx < 20; ++tx) {
      kdu_tile t = mt_cs.open_tile(kdu_coords(tx, 0));
      PushTileData(mt_cs, t, mt_planes, mt_spec);
      t.close();
    }
    kdu_long inc_bytes[1] = {4000};
    mt_cs.flush(inc_bytes, 1);
    for (int tx = 20; tx < tile_range.size.x; ++tx) {
      kdu_tile t = mt_cs.open_tile(kdu_coords(tx, 0));
      PushTileData(mt_cs, t, mt_planes, mt_spec);
      t.close();
    }
    kdu_long fin_bytes[1] = {8000};
    mt_cs.flush(fin_bytes, 1);
    mt_cs.destroy();
    mt_tgt.close();
    EXPECT_GT(mt_tgt.rewrite_calls, 0);
  }
}

TEST_F(CodestreamTest, CachedTargetAutoFlushCbrUnderflowAndStats) {
  // 1. Cached target (KDU_TARGET_CAP_CACHED) with text + binary comments,
  // thresholds-only incremental flushing across partial tiles, auto_flush
  // with thresholds+sizes/hints, and write failure propagation.
  {
    ImageSpec spec;
    spec.width = 48;
    spec.height = 48;
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.tile_size = kdu_coords(16, 16);
    spec.extra_params = {"Corder=RPCL", "Clayers=3", "Cprecincts={8,8}"};
    const auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);

    StructuredCacheTarget cache_tgt;
    kdu_codestream cs;
    cs.create(&siz, &cache_tgt);
    ConfigureCodestreamParams(cs, spec);
    kdu_codestream_comment c1 = cs.add_comment();
    EXPECT_TRUE(c1.put_text("cached-text-comment"));
    kdu_codestream_comment c2 = cs.add_comment();
    const kdu_byte bin_comment[4] = {0x11, 0x22, 0x33, 0x44};
    EXPECT_TRUE(c2.put_data(bin_comment, 4));
    cs.enable_restart();

    kdu_uint16 thresholds[3] = {52000, 48000, 44000};
    for (int ty = 0; ty < 3; ++ty) {
      for (int tx = 0; tx < 3; ++tx) {
        kdu_tile t = cs.open_tile(kdu_coords(tx, ty));
        PushTileData(cs, t, planes, spec);
        t.close();
      }
      // Thresholds-only flush after each tile-row exercises ready_for_flush
      // and cache_write_ready_precincts without pre-existing target_sizes.
      cs.flush(nullptr, 3, thresholds, /*trim_to_rate=*/false,
               /*record_in_comseg=*/false, /*tolerance=*/0.0, nullptr,
               KDU_FLUSH_USES_THRESHOLDS_AND_SIZES);
    }
    EXPECT_FALSE(cache_tgt.tile_headers.empty());
    EXPECT_FALSE(cache_tgt.precincts.empty());

    // Restart cached target and exercise auto_flush + thresholds hints.
    StructuredCacheTarget cache_tgt2;
    cs.restart(&cache_tgt2);
    kdu_long auto_lbytes[3] = {400, 900, 0};
    for (int ty = 0; ty < 3; ++ty) {
      for (int tx = 0; tx < 3; ++tx) {
        kdu_tile t = cs.open_tile(kdu_coords(tx, ty));
        PushTileData(cs, t, planes, spec);
        t.close();
        cs.auto_flush(1, 1, 1, 1, auto_lbytes, 3, thresholds, false, false, 0.5,
                      nullptr, KDU_FLUSH_THRESHOLDS_ARE_HINTS);
      }
    }
    cs.flush(auto_lbytes, 3, thresholds, false, false, 0.5, nullptr,
             KDU_FLUSH_USES_THRESHOLDS_AND_SIZES);
    cs.destroy();
    cache_tgt.close();
    cache_tgt2.close();
    EXPECT_FALSE(cache_tgt.main_header.empty());
  }

  // Write failure inside cache_write_headers.
  {
    class FailingCachedTarget : public StructuredCacheTarget {
     public:
      bool write(const kdu_byte* /*buf*/, int /*num_bytes*/) override {
        return false;
      }
    };
    ImageSpec spec;
    spec.width = 16;
    spec.height = 16;
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 0;
    const auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    FailingCachedTarget fail_tgt;
    kdu_codestream cs;
    cs.create(&siz, &fail_tgt);
    cs.set_write_fail_policy(KDU_WRITE_FAIL_POLICY_ERROR);
    ConfigureCodestreamParams(cs, spec);
    kdu_tile t = cs.open_tile(kdu_coords(0, 0));
    PushTileData(cs, t, planes, spec);
    t.close();
    kdu_long lbytes[1] = {512};
    error_handler_.clear();
    try {
      cs.flush(lbytes, 1);
      FAIL() << "Expected write failure error";
    } catch (kdu_exception) {
      EXPECT_FALSE(error_handler_.history().empty());
    }
    cs.destroy();
    fail_tgt.close();
  }

  // Non-rewritable target with ORGgen_tlm emits warning 17 in
  // generate_main_header.
  {
    class NonSeekableTarget : public MemoryTarget {
     public:
      bool start_rewrite(kdu_long /*backtrack*/) override { return false; }
    };
    ImageSpec spec;
    spec.width = 16;
    spec.height = 16;
    spec.components = 1;
    spec.dwt_levels = 0;
    spec.extra_params = {"ORGgen_tlm=2"};
    const auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    NonSeekableTarget non_seek_tgt;
    kdu_codestream cs;
    cs.create(&siz, &non_seek_tgt);
    ConfigureCodestreamParams(cs, spec);
    kdu_tile t = cs.open_tile(kdu_coords(0, 0));
    PushTileData(cs, t, planes, spec);
    t.close();
    warning_handler_.clear();
    cs.flush(nullptr, 1);
    EXPECT_FALSE(warning_handler_.history().empty());
    cs.destroy();
  }

  // 2. pcrd_opt_cbr: huge CBR budget on uniform image hits
  // global_slope_included_everything + primary/secondary/fine fill bytes;
  // plus tiny budget / invalid Scbr config errors in construct_cbr_flush_set.
  {
    ImageSpec flat_spec;
    flat_spec.width = 64;
    flat_spec.height = 16;
    flat_spec.components = 1;
    flat_spec.reversible = true;
    flat_spec.dwt_levels = 1;
    flat_spec.extra_params = {"Corder=PCRL", "Cprecincts={8,8}", "Cblk={8,8}",
                              "Clayers=1", "Scbr={1,20000}"};
    std::vector<std::vector<int>> flat_planes(
        1, std::vector<int>(static_cast<size_t>(64 * 16), 3));
    siz_params siz;
    ConfigureSiz(siz, flat_spec);
    MemoryTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    EXPECT_TRUE(cs.cbr_flushing());
    ConfigureCodestreamParams(cs, flat_spec);
    kdu_tile t = cs.open_tile(kdu_coords(0, 0));
    PushTileData(cs, t, flat_planes, flat_spec);
    t.close();
    kdu_long cbr_bytes[1] = {20000};
    cs.flush(cbr_bytes, 1);
    EXPECT_GT(cbr_bytes[0], 0);
    cs.destroy();
  }

  // Construct_cbr_flush_set incompatible parameters + impossible Scbr budget.
  {
    const char* bad_cbr_cases[] = {"Clayers=2", "Corder=LRCP",
                                   "ORGgen_plt=yes"};
    for (const char* bad_param : bad_cbr_cases) {
      ImageSpec spec;
      spec.width = 32;
      spec.height = 16;
      spec.components = 1;
      spec.dwt_levels = 1;
      spec.extra_params = {"Cprecincts={8,8}", "Cblk={8,8}", "Scbr={1,64}",
                           bad_param};
      if (std::string(bad_param) != "Corder=LRCP") {
        spec.extra_params.push_back("Corder=PCRL");
      }
      const auto planes = MakeTestPlanes(spec);
      siz_params siz;
      ConfigureSiz(siz, spec);
      MemoryTarget tgt;
      kdu_codestream cs;
      cs.create(&siz, &tgt);
      ConfigureCodestreamParams(cs, spec);
      kdu_tile t = cs.open_tile(kdu_coords(0, 0));
      PushTileData(cs, t, planes, spec);
      error_handler_.clear();
      try {
        t.close();
        if (std::string(bad_param) == "Clayers=2") {
          kdu_long lbytes[2] = {500, 1000};
          cs.flush(lbytes, 2);
        } else {
          kdu_long cbr_bytes[1] = {500};
          cs.flush(cbr_bytes, 1);
        }
      } catch (kdu_exception) {
      }
      EXPECT_FALSE(error_handler_.history().empty()) << bad_param;
      cs.destroy();
    }

    // Zero target codestream length with Scbr.
    {
      ImageSpec spec;
      spec.width = 32;
      spec.height = 16;
      spec.components = 1;
      spec.dwt_levels = 1;
      spec.extra_params = {"Corder=PCRL", "Cprecincts={8,8}", "Cblk={8,8}",
                           "Clayers=1", "Scbr={1,64}"};
      const auto planes = MakeTestPlanes(spec);
      siz_params siz;
      ConfigureSiz(siz, spec);
      MemoryTarget tgt;
      kdu_codestream cs;
      cs.create(&siz, &tgt);
      ConfigureCodestreamParams(cs, spec);
      kdu_tile t = cs.open_tile(kdu_coords(0, 0));
      PushTileData(cs, t, planes, spec);
      error_handler_.clear();
      try {
        t.close();
        cs.flush(nullptr, 1);
      } catch (kdu_exception) {
      }
      EXPECT_FALSE(error_handler_.history().empty());
      cs.destroy();
    }

    // Vertical tiling incompatible with multi-flush-set Scbr.
    ImageSpec vtile_spec;
    vtile_spec.width = 32;
    vtile_spec.height = 32;
    vtile_spec.components = 1;
    vtile_spec.dwt_levels = 1;
    vtile_spec.tile_size = kdu_coords(32, 16);
    vtile_spec.extra_params = {"Corder=PCRL", "Cprecincts={8,8}", "Cblk={8,8}",
                               "Clayers=1", "Scbr={1,64}"};
    const auto vt_planes = MakeTestPlanes(vtile_spec);
    siz_params vt_siz;
    ConfigureSiz(vt_siz, vtile_spec);
    MemoryTarget vt_tgt;
    kdu_codestream vt_cs;
    vt_cs.create(&vt_siz, &vt_tgt);
    ConfigureCodestreamParams(vt_cs, vtile_spec);
    kdu_tile t0 = vt_cs.open_tile(kdu_coords(0, 0));
    PushTileData(vt_cs, t0, vt_planes, vtile_spec);
    error_handler_.clear();
    try {
      t0.close();
      kdu_long cbr_bytes[1] = {500};
      vt_cs.flush(cbr_bytes, 1);
    } catch (kdu_exception) {
    }
    EXPECT_FALSE(error_handler_.history().empty());
    vt_cs.destroy();

    // Multi-component mismatched flush-set sizes with Scbr.
    ImageSpec mismatch_spec;
    mismatch_spec.width = 64;
    mismatch_spec.height = 32;
    mismatch_spec.components = 2;
    mismatch_spec.dwt_levels = 1;
    mismatch_spec.extra_params = {
        "Corder=PCRL", "Cprecincts={8,8}", "Cprecincts:C1={32,32}",
        "Cblk={8,8}",  "Clayers=1",        "Scbr={1,64}"};
    const auto mm_planes = MakeTestPlanes(mismatch_spec);
    siz_params mm_siz;
    ConfigureSiz(mm_siz, mismatch_spec);
    MemoryTarget mm_tgt;
    kdu_codestream mm_cs;
    mm_cs.create(&mm_siz, &mm_tgt);
    ConfigureCodestreamParams(mm_cs, mismatch_spec);
    kdu_tile tm = mm_cs.open_tile(kdu_coords(0, 0));
    PushTileData(mm_cs, tm, mm_planes, mismatch_spec);
    error_handler_.clear();
    try {
      tm.close();
      kdu_long cbr_bytes[1] = {500};
      mm_cs.flush(cbr_bytes, 1);
    } catch (kdu_exception) {
    }
    EXPECT_FALSE(error_handler_.history().empty());
    mm_cs.destroy();

    // Tight PRCL Scbr hitting precinct slope adjustment & tiny-budget error.
    ImageSpec prcl_spec;
    prcl_spec.width = 64;
    prcl_spec.height = 32;
    prcl_spec.components = 2;
    prcl_spec.dwt_levels = 2;
    prcl_spec.extra_params = {"Corder=PRCL", "Cprecincts={8,8}", "Cblk={8,8}",
                              "Clayers=1", "Scbr={1,2}"};
    const auto prcl_planes = MakeTestPlanes(prcl_spec);
    siz_params prcl_siz;
    ConfigureSiz(prcl_siz, prcl_spec);
    MemoryTarget prcl_tgt;
    kdu_codestream prcl_cs;
    prcl_cs.create(&prcl_siz, &prcl_tgt);
    ConfigureCodestreamParams(prcl_cs, prcl_spec);
    kdu_tile tp = prcl_cs.open_tile(kdu_coords(0, 0));
    PushTileData(prcl_cs, tp, prcl_planes, prcl_spec);
    error_handler_.clear();
    try {
      tp.close();
      kdu_long cbr_bytes[1] = {200};
      prcl_cs.flush(cbr_bytes, 1);
    } catch (kdu_exception) {
    }
    prcl_cs.destroy();
  }

  // 3. pcrd_opt unsized leading + trailing layers + synthesized second-last
  // size + tolerance + record_in_comseg + Creslengths/Cagglengths + pcrd_trim.
  {
    ImageSpec spec;
    spec.width = 48;
    spec.height = 48;
    spec.components = 2;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(24, 24);
    spec.extra_params = {"Clayers=4", "Creslengths=600,400,250",
                         "Cagglengths=1,0"};
    const auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ConfigureCodestreamParams(cs, spec);
    cs.set_max_bytes(1600, false, true);
    kdu_long lbytes[4] = {0, 0, 1100, 0};
    for (int ty = 0; ty < 2; ++ty) {
      for (int tx = 0; tx < 2; ++tx) {
        kdu_tile t = cs.open_tile(kdu_coords(tx, ty));
        PushTileData(cs, t, planes, spec);
        t.close();
        cs.auto_flush(1, 1, 1, 1, lbytes, 4, nullptr, /*trim_to_rate=*/false,
                      /*record_in_comseg=*/true, /*tolerance=*/0.8);
      }
    }
    cs.flush(lbytes, 4, nullptr, /*trim_to_rate=*/true,
             /*record_in_comseg=*/true, /*tolerance=*/0.8);
    EXPECT_GT(tgt.data.size(), 0);
    cs.destroy();
  }

  // 4. kdu_flush_stats sharing (attach_flush_stats, set_max_passes, advance,
  // auto_advance, link_tile_comp on open_tile + restart).
  {
    ImageSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 2;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(16, 16);
    spec.extra_params = {"Clayers=2", "Cblk={8,8}"};
    const auto planes = MakeTestPlanes(spec);
    siz_params siz1, siz2;
    ConfigureSiz(siz1, spec);
    ConfigureSiz(siz2, spec);

    MemoryTarget tgt1, tgt2, tgt2_restart;
    kdu_codestream cs1, cs2;
    cs1.create(&siz1, &tgt1);
    ConfigureCodestreamParams(cs1, spec);
    kdu_flush_stats fstats1 = cs1.add_flush_stats(0, 6, 0, 1);
    EXPECT_TRUE(fstats1.exists());
    EXPECT_FALSE(!fstats1);
    EXPECT_TRUE(fstats1 == fstats1);

    for (int ty = 0; ty < 2; ++ty) {
      for (int tx = 0; tx < 2; ++tx) {
        kdu_tile t = cs1.open_tile(kdu_coords(tx, ty));
        PushTileData(cs1, t, planes, spec);
        t.close();
      }
    }
    kdu_long lbytes1[2] = {300, 700};
    cs1.flush(lbytes1, 2);

    cs2.create(&siz2, &tgt2);
    ConfigureCodestreamParams(cs2, spec);
    kdu_flush_stats fstats2 = cs2.add_flush_stats(2, 6, 0, 1);
    EXPECT_TRUE(fstats1 != fstats2);
    fstats2.set_max_passes(/*for_hr=*/true, /*for_lr=*/false, 0, 4);
    fstats2.set_max_passes(/*for_hr=*/false, /*for_lr=*/true, -1, 5);
    fstats2.auto_advance(2);
    cs2.attach_flush_stats(fstats1);
    cs2.enable_restart();
    for (int ty = 0; ty < 2; ++ty) {
      for (int tx = 0; tx < 2; ++tx) {
        kdu_tile t = cs2.open_tile(kdu_coords(tx, ty));
        PushTileData(cs2, t, planes, spec);
        t.close();
      }
    }
    cs2.flush(lbytes1, 2);

    cs2.restart(&tgt2_restart);
    for (int ty = 0; ty < 2; ++ty) {
      for (int tx = 0; tx < 2; ++tx) {
        kdu_tile t = cs2.open_tile(kdu_coords(tx, ty));
        PushTileData(cs2, t, planes, spec);
        t.close();
      }
    }
    cs2.flush(lbytes1, 2);

    // Also test explicit advance() + incompatible attach_flush_stats error.
    fstats1.advance(2);
    ImageSpec diff_spec = spec;
    diff_spec.width = 64;
    diff_spec.height = 64;
    siz_params siz_diff;
    ConfigureSiz(siz_diff, diff_spec);
    MemoryTarget tgt_diff;
    kdu_codestream cs_diff;
    cs_diff.create(&siz_diff, &tgt_diff);
    ConfigureCodestreamParams(cs_diff, diff_spec);
    (void)cs_diff.add_flush_stats(4, 6, 0, 1);
    error_handler_.clear();
    try {
      cs_diff.attach_flush_stats(fstats1);
      FAIL() << "Expected incompatible flush stats error";
    } catch (kdu_exception) {
      EXPECT_FALSE(error_handler_.history().empty());
    }
    cs_diff.destroy();
    cs2.destroy();
    cs1.destroy();
  }
}

TEST_F(CodestreamTest, BackgroundTileQueuesBlockNotifiersAndThreadContext) {
  // Multi-threaded output + input using purely synchronous MemoryTarget /
  // MemorySource (no KDU_SOURCE_CAP_ASYNC_NOTIFICATION) so kdu_thread_env uses
  // kd_cs_thread_context without activating kdu_thread_entity_event.
  ImageSpec spec;
  spec.width = 64;
  spec.height = 64;
  spec.components = 3;
  spec.reversible = false;
  spec.dwt_levels = 2;
  spec.tile_size = kdu_coords(16, 16);
  spec.extra_params = {"Corder=PCRL", "Cprecincts={8,8}", "Cblk={8,8}",
                       "Clayers=2", "ORGgen_plt=yes"};
  const auto planes = MakeTestPlanes(spec);
  siz_params siz;
  ConfigureSiz(siz, spec);

  auto make_dims = [](int x, int y, int w, int h) {
    kdu_dims d;
    d.pos = kdu_coords(x, y);
    d.size = kdu_coords(w, h);
    return d;
  };

  kdu_thread_env env;
  env.create();
  env.add_thread();
  env.add_thread();

  MemoryTarget tgt;
  kdu_codestream out_cs;
  out_cs.create(&siz, &tgt, nullptr, 0, 0, &env);
  ConfigureCodestreamParams(out_cs, spec);
  out_cs.enable_restart();
  EXPECT_TRUE(out_cs.can_flip(true));

  // Open tile (0,0) synchronously so tiles_accessed becomes true and glock is
  // released, allowing subsequent open_tiles(..., true, &env) to queue
  // background opens in kd_cs_thread_context.
  out_cs.open_tiles(make_dims(0, 0, 1, 1), /*open_in_background=*/false, &env);
  {
    kdu_tile t00 = out_cs.access_tile(kdu_coords(0, 0), true, &env);
    ASSERT_TRUE(t00.exists());
    PushTileData(out_cs, t00, planes, spec);
    t00.close(&env, /*close_in_background=*/true);
  }
  out_cs.open_tiles(make_dims(0, 0, 4, 4), /*open_in_background=*/true, &env);
  kdu_long auto_lbytes[2] = {1500, 4000};
  for (int ty = 0; ty < 4; ++ty) {
    for (int tx = 0; tx < 4; ++tx) {
      if (tx == 0 && ty == 0) continue;
      kdu_tile t = out_cs.access_tile(kdu_coords(tx, ty), true, &env);
      ASSERT_TRUE(t.exists());
      PushTileData(out_cs, t, planes, spec);
      if ((tx + ty) % 2 == 1) {
        t.close(&env, /*close_in_background=*/true);
      } else {
        t.close(&env, /*close_in_background=*/false);
      }
    }
    out_cs.auto_flush(1, 1, 1, 1, auto_lbytes, 2, nullptr, false, false, 0.5,
                      &env);
  }
  out_cs.close_tiles(make_dims(0, 0, 4, 4), &env);
  out_cs.flush(auto_lbytes, 2, nullptr, false, false, 0.5, &env);

  // Restart output codestream while threaded, then terminate cleanly.
  MemoryTarget tgt_restarted;
  out_cs.restart(&tgt_restarted, &env);
  for (int ty = 0; ty < 4; ++ty) {
    for (int tx = 0; tx < 4; ++tx) {
      kdu_tile t = out_cs.open_tile(kdu_coords(tx, ty), &env);
      PushTileData(out_cs, t, planes, spec);
      t.close(&env, /*close_in_background=*/true);
    }
  }
  out_cs.flush(auto_lbytes, 2, nullptr, false, false, 0.5, &env);
  env.cs_terminate(out_cs);
  out_cs.destroy();
  ASSERT_GT(tgt.data.size(), 0);
  ASSERT_GT(tgt_restarted.data.size(), 0);

  // Decode with synchronous MemorySource + kdu_thread_env: background tile
  // opens, manual attach_block_notifier/advance_block_rows_needed/detach,
  // apply_input_restrictions(&env), close_tiles, and restart(&src, &env).
  MemorySource src(tgt.data);
  kdu_codestream in_cs;
  in_cs.create(&src, &env);
  in_cs.set_persistent();
  in_cs.enable_restart();
  in_cs.change_appearance(/*transpose=*/true, /*vflip=*/false, /*hflip=*/true,
                          &env);
  auto decode_tile_mt = [&](kdu_tile& tile) {
    kdu_multi_synthesis synth;
    synth.create(in_cs, tile, &env, nullptr, KDU_MULTI_XFORM_DEFAULT_FLAGS);
    const int num_comps = in_cs.get_num_components(/*want_output_comps=*/true);
    std::vector<int> rows_left(num_comps, 0);
    for (int c = 0; c < num_comps; ++c) {
      rows_left[c] = synth.get_size(c).y;
      EXPECT_GT(rows_left[c], 0);
    }
    bool any_left = true;
    while (any_left) {
      any_left = false;
      for (int c = 0; c < num_comps; ++c) {
        if (rows_left[c] <= 0) continue;
        any_left = true;
        kdu_line_buf* line = synth.get_line(c, &env);
        if (line != nullptr) {
          --rows_left[c];
        }
      }
    }
    synth.destroy(&env);
  };
  kdu_dims valid_tiles;
  in_cs.get_valid_tiles(valid_tiles);
  ASSERT_GE(valid_tiles.size.x, 2);
  in_cs.open_tiles(make_dims(valid_tiles.pos.x, valid_tiles.pos.y, 1, 1),
                   /*open_in_background=*/false, &env);
  {
    kdu_tile t = in_cs.access_tile(valid_tiles.pos, true, &env);
    ASSERT_TRUE(t.exists());
    decode_tile_mt(t);
    t.close(&env, /*close_in_background=*/true);
  }
  in_cs.open_tiles(valid_tiles, /*open_in_background=*/true, &env);
  kdu_thread_queue* q = nullptr;
  {
    kdu_tile t =
        in_cs.access_tile(valid_tiles.pos + kdu_coords(1, 0), true, &env);
    ASSERT_TRUE(t.exists());
    q = env.add_queue(nullptr, nullptr, "sb_notify");
    kdu_subband sb =
        t.access_component(0).access_resolution(1).access_subband(LH_BAND);
    (void)sb.attach_block_notifier(q, &env);
    sb.advance_block_rows_needed(q, 2, &env);
    sb.detach_block_notifier(q, &env);
    decode_tile_mt(t);
    t.close(&env, /*close_in_background=*/true);
  }
  in_cs.close_tiles(valid_tiles, &env);
  env.cs_terminate(in_cs);
  if (q != nullptr) {
    env.terminate(q, false);
  }
  in_cs.apply_input_restrictions(0, 0, 0, 1, nullptr,
                                 KDU_WANT_OUTPUT_COMPONENTS, nullptr);
  int comp_list[2] = {0, 2};
  in_cs.apply_input_restrictions(2, comp_list, 0, 2, nullptr,
                                 KDU_WANT_CODESTREAM_COMPONENTS, nullptr);
  in_cs.apply_input_restrictions(2, comp_list, 0, 2, nullptr,
                                 KDU_WANT_OUTPUT_COMPONENTS, nullptr);

  ASSERT_TRUE(src.seek(0));
  in_cs.restart(&src, &env);
  in_cs.get_valid_tiles(valid_tiles);
  kdu_tile rt = in_cs.open_tile(valid_tiles.pos, &env);
  decode_tile_mt(rt);
  rt.close(&env, true);
  env.cs_terminate(in_cs);
  in_cs.destroy();
  env.destroy();
}

TEST_F(CodestreamTest, LoadSequentialVariantsAndTranscodeEdgeCases) {
  auto make_dims = [](int x, int y, int w, int h) {
    kdu_dims d;
    d.pos = kdu_coords(x, y);
    d.size = kdu_coords(w, h);
    return d;
  };

  // 1. Exercise load_sequential across progression orders, probe-only flags,
  // pre-sized pset with small packet_bytes_entries, interleaved tile-parts
  // (ORGtparts=R), and invalid cached-source error path.
  {
    ImageSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 3;
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(16, 16);
    spec.extra_params = {"Corder=LRCP", "Cprecincts={8,8}", "Cblk={8,8}",
                         "Clayers=3"};
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);
    ASSERT_GT(bytes.size(), 64);

    MemorySource cached_src(bytes.data(), bytes.size(),
                            KDU_SOURCE_CAP_SEEKABLE | KDU_SOURCE_CAP_CACHED);
    kdu_codestream cached_cs;
    cached_cs.create(&cached_src);
    kdu_pdescriptor_set pset;
    bool tile_done = false;
    error_handler_.clear();
    try {
      (void)cached_cs.load_sequential(&pset, tile_done, 1, 1, 0, nullptr);
      FAIL() << "Expected cached source error in load_sequential";
    } catch (kdu_exception) {
      EXPECT_FALSE(error_handler_.history().empty());
    }
    cached_cs.destroy();
  }

  const char* orders[] = {"Corder=RPCL", "Corder=PCRL", "Corder=CPRL",
                          "Corder=LRCP", "Corder=RLCP"};
  for (int oi = 0; oi < 5; ++oi) {
    ImageSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 3;
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(16, 16);
    spec.extra_params = {orders[oi], "Cprecincts={8,8}", "Cblk={8,8}",
                         "Clayers=3"};
    if (oi == 3) {
      spec.extra_params.push_back("ORGtparts=R");
    }
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);
    ASSERT_GT(bytes.size(), 64);

    MemorySource src(bytes.data(), bytes.size(), KDU_SOURCE_CAP_SEQUENTIAL);
    kdu_codestream cs;
    cs.create(&src);

    // Probe call with min_pdescriptors=0, min_code_blocks=0, flags=0.
    kdu_pdescriptor_set probe_pset;
    bool tile_done = false;
    kdu_tile probe_t =
        cs.load_sequential(&probe_pset, tile_done, 0, 0, 0, nullptr);
    EXPECT_TRUE(probe_t.exists());

    // Pre-initialize with packet_bytes_entries=1 (< 3 layers) on first tile.
    std::map<std::pair<int, int>, std::unique_ptr<kdu_pdescriptor_set>>
        incomplete_psets;
    if (oi == 0) {
      auto first_pset = std::make_unique<kdu_pdescriptor_set>();
      first_pset->init(probe_pset.get_tile_idx(), 1);
      const kdu_coords pt = probe_pset.get_tile_idx();
      incomplete_psets[{pt.x, pt.y}] = std::move(first_pset);
    }

    kdu_pdescriptor_set* active_pset = nullptr;
    kdu_pdescriptor_set fresh_pset;
    for (int step = 0; step < 256; ++step) {
      kdu_pdescriptor_set* use_pset =
          (active_pset != nullptr) ? active_pset : &fresh_pset;
      if (active_pset == nullptr) fresh_pset.reset();
      const int flags = KDU_LOAD_SEQUENTIAL_WANT_FLUSH_SET |
                        KDU_LOAD_SEQUENTIAL_WANT_PACKET_BYTES |
                        ((oi == 4) ? KDU_LOAD_SEQUENTIAL_WANT_TILE : 0);
      kdu_tile t =
          cs.load_sequential(use_pset, tile_done, 1, 1, flags, nullptr);
      if (!t.exists()) {
        if (use_pset->is_empty()) break;
        const kdu_coords tidx = use_pset->get_tile_idx();
        auto& slot = incomplete_psets[{tidx.x, tidx.y}];
        if (!slot) {
          slot = std::make_unique<kdu_pdescriptor_set>();
          slot->init(tidx, 3);
        }
        active_pset = nullptr;
        continue;
      }
      const kdu_coords tidx = use_pset->get_tile_idx();
      const std::pair<int, int> key(tidx.x, tidx.y);
      auto it = incomplete_psets.find(key);
      if (it != incomplete_psets.end() && use_pset == &fresh_pset) {
        active_pset = it->second.get();
        continue;
      }
      if (tile_done) {
        if (use_pset == &fresh_pset) use_pset->reset();
        incomplete_psets.erase(key);
        active_pset = nullptr;
      } else {
        // Keep trailing incomplete descriptors in `incomplete_psets[key]` while
        // discarding complete prefix.
        auto tail = std::make_unique<kdu_pdescriptor_set>();
        const int num_comp = use_pset->get_num_complete();
        use_pset->extract_tail(tail.get(), num_comp);
        if (use_pset == &fresh_pset) use_pset->reset();
        if (!tail->is_empty()) {
          incomplete_psets[key] = std::move(tail);
          active_pset = incomplete_psets[key].get();
        } else {
          incomplete_psets.erase(key);
          active_pset = nullptr;
        }
      }
    }
    cs.close_tiles(make_dims(0, 0, 2, 2), nullptr);
    cs.destroy();
  }

  // 2. Transcoding with kdu_thread_env: auto_trans_out incremental calls,
  // Creslengths warning, trans_pcrd_opt rate pruning, and trans_out errors.
  {
    ImageSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 3;
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(16, 16);
    spec.extra_params = {"Corder=PCRL", "Cprecincts={8,8}", "Cblk={8,8}",
                         "Clayers=3"};
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);

    kdu_thread_env env;
    env.create();
    env.add_thread();

    MemorySource src(bytes.data(), bytes.size(), KDU_SOURCE_CAP_SEQUENTIAL);
    kdu_codestream in_cs;
    in_cs.create(&src, &env);

    ImageSpec out_spec = spec;
    out_spec.extra_params.push_back("Creslengths=400,250");
    siz_params out_siz;
    ConfigureSiz(out_siz, out_spec);

    MemoryTarget out_tgt;
    kdu_codestream out_cs;
    out_cs.create(&out_siz, &out_tgt, nullptr, 0, 0, &env);
    ConfigureCodestreamParams(out_cs, out_spec);

    // Calling trans_out with env=nullptr before cs_terminate must fail.
    error_handler_.clear();
    try {
      out_cs.trans_out(500, nullptr, 0, false, nullptr);
      FAIL() << "Expected trans_out without env error";
    } catch (kdu_exception) {
      EXPECT_FALSE(error_handler_.history().empty());
    }

    std::map<std::pair<int, int>, kdu_tile> in_tiles;
    std::map<std::pair<int, int>, kdu_tile> out_tiles;
    kdu_pdescriptor_set pset;
    bool tile_done = false;
    warning_handler_.clear();
    for (int iter = 0; iter < 64; ++iter) {
      kdu_tile itile =
          in_cs.load_sequential(&pset, tile_done, 2, 2,
                                KDU_LOAD_SEQUENTIAL_WANT_FLUSH_SET |
                                    KDU_LOAD_SEQUENTIAL_WANT_PACKET_BYTES,
                                &env);
      if (!itile.exists()) break;
      const kdu_coords tidx = pset.get_tile_idx();
      const std::pair<int, int> key(tidx.x, tidx.y);
      in_tiles.emplace(key, itile);
      auto ot_it = out_tiles.find(key);
      if (ot_it == out_tiles.end()) {
        ot_it = out_tiles.emplace(key, out_cs.open_tile(tidx, &env)).first;
      }
      kdu_tile otile = ot_it->second;
      for (int d = 0; d < pset.get_num_pdescriptors(); ++d) {
        kdu_pdescriptor* pd = pset.access_pdescriptor(d);
        ASSERT_NE(pd, nullptr);
        kdu_tile_comp ic = itile.access_component(pd->component_idx);
        kdu_resolution ir = ic.access_resolution(pd->resolution_level);
        kdu_precinct iprec = ir.access_precinct(pd->precinct_idx);
        ASSERT_TRUE(iprec.exists());
        kdu_tile_comp oc = otile.access_component(pd->component_idx);
        kdu_resolution o_r = oc.access_resolution(pd->resolution_level);
        int min_b = 0;
        const int num_b = ir.get_valid_band_indices(min_b);
        for (int b = min_b; b < min_b + num_b; ++b) {
          kdu_subband isub = ir.access_subband(b);
          kdu_subband osub = o_r.access_subband(b);
          kdu_dims binds;
          if (!iprec.get_valid_blocks(b, binds)) continue;
          for (int by = 0; by < binds.size.y; ++by) {
            for (int bx = 0; bx < binds.size.x; ++bx) {
              const kdu_coords bidx(binds.pos.x + bx, binds.pos.y + by);
              kdu_block* ib = isub.open_block(bidx, nullptr, &env);
              const int num_passes = ib->num_passes;
              const int missing_msbs = ib->missing_msbs;
              std::vector<int> pass_lengths;
              std::vector<kdu_uint16> pass_slopes;
              std::vector<kdu_byte> block_bytes;
              if (num_passes > 0) {
                pass_lengths.assign(ib->pass_lengths,
                                    ib->pass_lengths + num_passes);
                pass_slopes.assign(ib->pass_slopes,
                                   ib->pass_slopes + num_passes);
                int total_bytes = 0;
                for (int p = 0; p < num_passes; ++p) {
                  total_bytes += pass_lengths[p];
                }
                if (total_bytes > 0) {
                  block_bytes.assign(ib->byte_buffer,
                                     ib->byte_buffer + total_bytes);
                }
              }
              isub.close_block(ib, &env);

              kdu_block* ob = osub.open_block(bidx, nullptr, &env);
              ob->num_passes = num_passes;
              ob->missing_msbs = missing_msbs;
              if (num_passes > 0) {
                ob->set_max_passes(num_passes, false);
                for (int p = 0; p < num_passes; ++p) {
                  ob->pass_lengths[p] = pass_lengths[p];
                  ob->pass_slopes[p] = pass_slopes[p];
                }
                if (!block_bytes.empty()) {
                  ob->set_max_bytes(static_cast<int>(block_bytes.size()),
                                    false);
                  std::memcpy(ob->byte_buffer, block_bytes.data(),
                              block_bytes.size());
                }
              }
              osub.close_block(ob, &env);
            }
          }
        }
      }
      EXPECT_GE(out_cs.trans_pcrd_opt(&pset, true, true, 0.05, &env), 0);
      pset.reset();
      if (tile_done) {
        otile.close(&env);
        out_tiles.erase(key);
        itile.close(&env);
        in_tiles.erase(key);
        out_cs.auto_trans_out(1, 1, 1, 1, 4000, /*record_in_comseg=*/true,
                              &env);
      }
    }
    EXPECT_TRUE(out_tiles.empty());
    kdu_long out_layers[6] = {0};
    EXPECT_GE(out_cs.trans_out(4000, out_layers, 6, true, &env), 0);
    EXPECT_FALSE(warning_handler_.history().empty());
    env.cs_terminate(out_cs);
    env.cs_terminate(in_cs);
    out_cs.destroy();
    in_cs.destroy();
    env.destroy();
  }
}

TEST_F(CodestreamTest, OldMctRxformAuxHeadersTlmPltAndEdgeErrors) {
  auto make_dims = [](int x, int y, int w, int h) {
    kdu_dims d;
    d.pos = kdu_coords(x, y);
    d.size = kdu_coords(w, h);
    return d;
  };

  // 1. Trigger kd_mct_block::create_old_rxform_ss_model(): encode reversible
  // 2-component MATRIX MCT (single block -> Cmct=0 in COD due to
  // params.cpp:13965), open for input, apply kdu_quality_limiter with
  // KDU_WANT_OUTPUT_COMPONENTS + display resolution + comp info so
  // find_multicomponent_energy_gain -> analyze_sensitivity ->
  // create_old_rxform_ss_model runs and get_square_visual_weight runs.
  {
    ImageSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 2;
    spec.is_signed = true;
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.extra_params = {
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
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);
    ASSERT_GT(bytes.size(), 64);

    MemorySource src(bytes.data(), bytes.size(), KDU_SOURCE_CAP_SEEKABLE);
    kdu_codestream cs;
    cs.create(&src);

    kdu_quality_limiter qlim(0.05f, /*preserve_if_reversible=*/false);
    qlim.set_display_resolution(300.0f, 300.0f);
    qlim.set_comp_info(0, 1.0f, 1);
    qlim.set_comp_info(1, 0.5f, 2);

    cs.apply_input_restrictions(0, 0, 0, 0, nullptr, KDU_WANT_OUTPUT_COMPONENTS,
                                nullptr, &qlim);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    EXPECT_GT(tile.find_component_gain_info(0, true), 0.0f);
    EXPECT_GT(tile.find_component_gain_info(0, false), 0.0f);
    {
      kdu_multi_synthesis synth;
      synth.create(cs, tile);
      const int num_comps = cs.get_num_components(/*want_output_comps=*/true);
      std::vector<int> rows_left(num_comps, 0);
      for (int c = 0; c < num_comps; ++c) {
        rows_left[c] = synth.get_size(c).y;
        EXPECT_GT(rows_left[c], 0);
      }
      bool any_left = true;
      while (any_left) {
        any_left = false;
        for (int c = 0; c < num_comps; ++c) {
          if (rows_left[c] <= 0) continue;
          any_left = true;
          if (synth.get_line(c) != nullptr) {
            --rows_left[c];
          }
        }
      }
      synth.destroy();
    }
    tile.close();

    // Also test get_res_subsamplings, get_full_dims, get_bit_depth,
    // get_signed, get_registration, get_relative_registration invalid & output
    // component branches.
    kdu_coords subs[8];
    EXPECT_EQ(cs.get_res_subsamplings(-1, subs, 8, true), 0);
    EXPECT_EQ(cs.get_res_subsamplings(9, subs, 8, true), 0);
    EXPECT_EQ(cs.get_res_subsamplings(-1, subs, 8, false), 0);
    EXPECT_EQ(cs.get_res_subsamplings(9, subs, 8, false), 0);
    EXPECT_GT(cs.get_res_subsamplings(0, subs, 8, true), 0);
    EXPECT_EQ(cs.get_bit_depth(-1, true, true), 0);
    EXPECT_EQ(cs.get_bit_depth(9, true, true), 0);
    EXPECT_EQ(cs.get_bit_depth(9, false, false), 0);
    EXPECT_FALSE(cs.get_signed(-1, true, true));
    EXPECT_FALSE(cs.get_signed(9, true, true));
    EXPECT_FALSE(cs.get_signed(9, false, false));
    kdu_coords reg(0, 0);
    cs.get_registration(-1, kdu_coords(16, 16), reg, true);
    cs.get_registration(9, kdu_coords(16, 16), reg, true);
    cs.get_registration(9, kdu_coords(16, 16), reg, false);
    cs.get_relative_registration(-1, 0, kdu_coords(16, 16), reg, true);
    cs.get_relative_registration(9, 0, kdu_coords(16, 16), reg, true);
    cs.get_relative_registration(9, 0, kdu_coords(16, 16), reg, false);
    kdu_dims fdims;
    error_handler_.clear();
    try {
      cs.get_full_dims(9, fdims, true, true);
      FAIL() << "Expected illegal component index error";
    } catch (kdu_exception) {
      EXPECT_FALSE(error_handler_.history().empty());
    }
    kdu_dims hreg;
    error_handler_.clear();
    try {
      cs.map_region(9, make_dims(0, 0, 8, 8), hreg, true);
      FAIL() << "Expected invalid component error in map_region";
    } catch (kdu_exception) {
      EXPECT_FALSE(error_handler_.history().empty());
    }
    kdu_coords found_tile;
    EXPECT_FALSE(cs.find_tile(0, kdu_coords(999, 999), found_tile, true));
    EXPECT_EQ(cs.get_ht_usage(), -1);
    EXPECT_GE(cs.get_min_dwt_levels(), 0);
    cs.destroy();
  }

  // 2. Output API error paths & header generation errors: ORGgen_tlm errors,
  // set_textualization, set_resilient/fussy/fast/allow_parsing_errors,
  // set_synth_overshoot_limit, set_max_bytes errors, membroker create failures.
  {
    ImageSpec spec;
    spec.width = 16;
    spec.height = 16;
    spec.components = 1;
    spec.dwt_levels = 1;
    const auto planes = MakeTestPlanes(spec);
    {
      siz_params siz;
      ConfigureSiz(siz, spec);
      kdu_membroker deny_broker(0);
      MemoryTarget btgt;
      btgt.broker = &deny_broker;
      kdu_codestream cs;
      error_handler_.clear();
      try {
        cs.create(&siz, &btgt);
      } catch (kdu_exception) {
      }
      if (cs.exists()) cs.destroy();
    }

    // ORGgen_tlm + Scbr and invalid ORGgen_tlm + ORGtlm_style errors.
    const std::vector<std::vector<std::string>> bad_tlm_configs = {
        {"ORGgen_tlm=2", "Scbr={1,64}", "Corder=PCRL", "Cprecincts={8,8}",
         "Clayers=1"},
        {"ORGgen_tlm=2", "ORGtlm_style={implied,long}"},
    };
    for (const auto& extra : bad_tlm_configs) {
      ImageSpec tspec = spec;
      tspec.extra_params = extra;
      siz_params siz;
      ConfigureSiz(siz, tspec);
      MemoryTarget tgt;
      kdu_codestream cs;
      cs.create(&siz, &tgt);
      ConfigureCodestreamParams(cs, tspec);
      kdu_tile t = cs.open_tile(kdu_coords(0, 0));
      PushTileData(cs, t, planes, tspec);
      t.close();
      error_handler_.clear();
      try {
        cs.flush(nullptr, 1);
        FAIL() << "Expected ORGgen_tlm error";
      } catch (kdu_exception) {
        EXPECT_FALSE(error_handler_.history().empty());
      }
      cs.destroy();
    }

    // set_textualization, set_resilient/fussy/fast, set_synth_overshoot_limit,
    // set_max_bytes unsupported & double call error.
    {
      siz_params siz;
      ConfigureSiz(siz, spec);
      MemoryTarget tgt;
      kdu_codestream cs;
      cs.create(&siz, &tgt);
      ConfigureCodestreamParams(cs, spec);
      CapturingPlainMessage text_out;
      cs.set_textualization(&text_out, /*skip_derived=*/true);
      EXPECT_FALSE(text_out.text().empty());
      cs.set_resilient(true);
      cs.set_fussy();
      cs.set_fast();
      cs.set_allow_parsing_errors(true);
      cs.set_synth_overshoot_limit(0.05f, /*generate_errors=*/false);
      float ov = 0.0f;
      EXPECT_FALSE(cs.report_synth_overshoot(0, ov));
      EXPECT_FALSE(cs.report_synth_overshoot(9, ov));

      cs.set_max_bytes(2048, /*unsupported=*/false, true);
      error_handler_.clear();
      try {
        cs.set_max_bytes(1024, false, true);
        FAIL() << "Expected duplicate set_max_bytes error";
      } catch (kdu_exception) {
        EXPECT_FALSE(error_handler_.history().empty());
      }
      cs.destroy();
    }
  }

  // 3. Crafted main/tile marker byte-streams:
  //    - Corrupted/truncated markers (kd_marker::read)
  //    - Non-seekable stream with CAP/CBD warning & seekable backtrack
  //      (read_siz_auxiliary_marker_segs)
  //    - TLM malformed / duplicate Ztlm / illegal Stlm / out-of-range tile
  //    - PLT out-of-order Zplt / truncated varint / set_max_bytes(5) error
  {
    ImageSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.tile_size = kdu_coords(16, 16);
    spec.extra_params = {"Corder=PCRL", "Cprecincts={8,8}", "Cblk={8,8}",
                         "Clayers=1",   "ORGgen_tlm=1",     "ORGgen_plt=yes"};
    const auto planes = MakeTestPlanes(spec);
    const auto valid_bytes = EncodeImage(spec, planes);
    ASSERT_GT(valid_bytes.size(), 64);

    // Input set_max_bytes(5) -> error 50 + simulate_parsing error 0x05032101.
    {
      MemorySource src(valid_bytes.data(), valid_bytes.size(),
                       KDU_SOURCE_CAP_SEEKABLE);
      kdu_codestream cs;
      cs.create(&src);
      error_handler_.clear();
      try {
        cs.set_max_bytes(512, /*unsupported=*/true, false);
        FAIL() << "Expected simulate_parsing error";
      } catch (kdu_exception) {
        EXPECT_FALSE(error_handler_.history().empty());
      }
      error_handler_.clear();
      try {
        cs.set_max_bytes(5, false, false);
        FAIL() << "Expected too-small max_bytes error";
      } catch (kdu_exception) {
        EXPECT_FALSE(error_handler_.history().empty());
      }
      cs.destroy();
    }

    // Input create membroker failure + restart errors (without enable_restart,
    // mismatch).
    {
      kdu_membroker tiny_broker(0);
      MemorySource bsrc(valid_bytes.data(), valid_bytes.size(),
                        KDU_SOURCE_CAP_SEEKABLE);
      bsrc.broker = &tiny_broker;
      kdu_codestream cs;
      error_handler_.clear();
      try {
        cs.create(&bsrc);
      } catch (kdu_exception) {
      }
      if (cs.exists()) cs.destroy();
    }

    // Truncated stream in kd_marker::read (just SOC + truncated marker).
    {
      const std::vector<kdu_byte> trunc_hdr = {0xFF, 0x4F, 0xFF, 0x90, 0x00};
      MemorySource tsrc(trunc_hdr.data(), trunc_hdr.size(),
                        KDU_SOURCE_CAP_SEQUENTIAL);
      kdu_codestream cs;
      error_handler_.clear();
      try {
        cs.create(&tsrc);
        FAIL() << "Expected truncated marker error";
      } catch (kdu_exception) {
        EXPECT_FALSE(error_handler_.history().empty());
      }
      if (cs.exists()) cs.destroy();
    }

    // Inject malformed TLM marker (duplicate Ztlm & illegal Stlm) right before
    // first SOT (0xFF 0x90).
    size_t sot_pos = 2;
    while (
        sot_pos + 1 < valid_bytes.size() &&
        !(valid_bytes[sot_pos] == 0xFF && valid_bytes[sot_pos + 1] == 0x90)) {
      ++sot_pos;
    }
    ASSERT_LT(sot_pos, valid_bytes.size());

    // Duplicate Ztlm=0 TLM before SOT.
    {
      std::vector<kdu_byte> dup_tlm = valid_bytes;
      const kdu_byte extra_tlm[] = {0xFF, 0x55, 0x00, 0x08, 0x00,
                                    0x40, 0x00, 0x01, 0x00, 0x10};
      dup_tlm.insert(dup_tlm.begin() + static_cast<std::ptrdiff_t>(sot_pos),
                     std::begin(extra_tlm), std::end(extra_tlm));
      MemorySource src(dup_tlm.data(), dup_tlm.size(), KDU_SOURCE_CAP_SEEKABLE);
      kdu_codestream cs;
      error_handler_.clear();
      try {
        cs.create(&src);
      } catch (kdu_exception) {
      }
      if (cs.exists()) cs.destroy();
    }

    // Replace all existing TLM markers with a short too-small TLM (< 6 bytes)
    // and illegal Stlm/tile index.
    {
      // Build header: keep bytes up to end of QCD, drop original TLMs, insert
      // custom TLM before SOT.
      std::vector<kdu_byte> bad_tlm_stream(valid_bytes.begin(),
                                           valid_bytes.begin() + 2);
      size_t i = 2;
      while (i + 3 < sot_pos) {
        if (valid_bytes[i] == 0xFF) {
          kdu_uint16 code = (static_cast<kdu_uint16>(valid_bytes[i]) << 8) |
                            valid_bytes[i + 1];
          kdu_uint16 len = (static_cast<kdu_uint16>(valid_bytes[i + 2]) << 8) |
                           valid_bytes[i + 3];
          if (code != 0xFF55) {
            bad_tlm_stream.insert(bad_tlm_stream.end(), valid_bytes.begin() + i,
                                  valid_bytes.begin() + i + 2 + len);
          }
          i += 2 + len;
        } else {
          break;
        }
      }
      // Inject Stlm=0x00 (0-byte tnum, 2-byte len) with invalid tile count ->
      // then open tile to force translate_markers.
      const kdu_byte tlm_st0[] = {0xFF, 0x55, 0x00, 0x08, 0x00,
                                  0x00, 0x00, 0x10, 0x00, 0x10};
      bad_tlm_stream.insert(bad_tlm_stream.end(), std::begin(tlm_st0),
                            std::end(tlm_st0));
      bad_tlm_stream.insert(bad_tlm_stream.end(), valid_bytes.begin() + sot_pos,
                            valid_bytes.end());
      MemorySource src(bad_tlm_stream.data(), bad_tlm_stream.size(),
                       KDU_SOURCE_CAP_SEEKABLE);
      kdu_codestream cs;
      error_handler_.clear();
      try {
        cs.create(&src);
        (void)cs.open_tile(kdu_coords(0, 0));
      } catch (kdu_exception) {
      }
      if (cs.exists()) cs.destroy();
    }

    // Truncate an in-stream PLT marker segment in tile-part 0.
    {
      std::vector<kdu_byte> bad_plt = valid_bytes;
      size_t plt_pos = sot_pos;
      while (plt_pos + 3 < bad_plt.size() &&
             !(bad_plt[plt_pos] == 0xFF && bad_plt[plt_pos + 1] == 0x58)) {
        ++plt_pos;
      }
      if (plt_pos + 4 < bad_plt.size()) {
        // Set Zplt = 5 (out-of-order PLT) -> hits add_plt_marker error 17.
        bad_plt[plt_pos + 4] = 5;
        MemorySource src(bad_plt.data(), bad_plt.size(),
                         KDU_SOURCE_CAP_SEEKABLE);
        kdu_codestream cs;
        cs.create(&src);
        error_handler_.clear();
        try {
          (void)cs.open_tile(kdu_coords(0, 0));
        } catch (kdu_exception) {
        }
        if (cs.exists()) cs.destroy();
      }
    }

    // Re-open clean stream, verify restart(input) after permanently closing
    // tile and open_tile on closed tile error 58.
    {
      MemorySource src(valid_bytes.data(), valid_bytes.size(),
                       KDU_SOURCE_CAP_SEEKABLE);
      kdu_codestream cs;
      cs.create(&src);
      error_handler_.clear();
      try {
        cs.restart(&src);  // Error 41: allow_restart not enabled.
        FAIL() << "Expected restart error without enable_restart";
      } catch (kdu_exception) {
        EXPECT_FALSE(error_handler_.history().empty());
      }
      cs.enable_restart();
      kdu_tile t = cs.open_tile(kdu_coords(0, 0));
      t.close();
      src.seek(0);
      cs.restart(&src);
      kdu_tile t2 = cs.open_tile(kdu_coords(0, 0));
      DecodeTileFirstComponent(cs, t2);
      t2.close();
      cs.destroy();
    }
  }
}

TEST_F(CodestreamTest, MctLegacyRxformDwtSsModelAndQualityLimiterWeights) {
  // 1. Legacy Part-2 reversible MCT matrix (mct_flags == 0 in COD) with
  // kdu_quality_limiter triggers kd_mct_block::create_old_rxform_ss_model.
  {
    ImageSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 2;
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=2",
        "Msigned=no",
        "Mprecision=8",
        "Mstage_inputs:I1={0,1}",
        "Mstage_outputs:I1={0,1}",
        "Mstage_collections:I1={2,2}",
        "Mstage_xforms:I1={MATRIX,1,0,1,0}",
        "Mmatrix_size:I1=6",
        "Mmatrix_coeffs:I1=1,1,1,1,0,-1",
        "Mnum_stages=1",
        "Mstages=1",
    };
    const auto planes = MakeTestPlanes(spec);
    auto bytes = EncodeImage(spec, planes);
    // Locate COD (FF 52) in main header and clear sgcod MCT bits (0x06).
    size_t pos = 2;
    bool cleared_cod = false;
    while (pos + 4 <= bytes.size()) {
      const kdu_uint16 marker =
          (static_cast<kdu_uint16>(bytes[pos]) << 8) | bytes[pos + 1];
      if (marker == KDU_SOT) break;
      const kdu_uint16 len =
          (static_cast<kdu_uint16>(bytes[pos + 2]) << 8) | bytes[pos + 3];
      if (marker == KDU_COD && pos + 9 < bytes.size()) {
        bytes[pos + 8] &= static_cast<kdu_byte>(~0x06U);
        cleared_cod = true;
        break;
      }
      pos += 2 + len;
    }
    ASSERT_TRUE(cleared_cod);

    MemorySource src(bytes);
    kdu_codestream cs;
    cs.create(&src);
    kdu_quality_limiter qlim(0.05F, false);
    qlim.set_display_resolution(600.0F, 600.0F);
    qlim.set_comp_info(0, 1.0F, 1);
    qlim.set_comp_info(1, 1.0F, 2);
    cs.apply_input_restrictions(0, 0, 0, 0, nullptr, KDU_WANT_OUTPUT_COMPONENTS,
                                nullptr, &qlim);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    DecodeTileAllComponents(cs, tile);
    tile.close();
    cs.destroy();
  }

  // 2. Multi-stage MCT (DWT stage + opponent matrix stage) with quality
  // limiter type flags and comps_of_interest mapping.
  {
    ImageSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 3;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=3",
        "Msigned=no",
        "Mprecision=8",
        "Mstage_inputs:I1={0,1},{2,2}",
        "Mstage_outputs:I1={0,1},{2,2}",
        "Mstage_collections:I1={2,2},{1,1}",
        "Mstage_xforms:I1={DWT,0,0,2,0},{MATRIX,0,0,0,0}",
        "Mstage_inputs:I2={0,2}",
        "Mstage_outputs:I2={0,2}",
        "Mstage_collections:I2={3,3}",
        "Mstage_xforms:I2={MATRIX,1,0,0,0}",
        "Mmatrix_size:I1=9",
        "Mmatrix_coeffs:I1=1.0,0.0,1.0,1.0,1.0,0.0,1.0,0.0,-1.0",
        "Mnum_stages=2",
        "Mstages=1,2",
    };
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);

    MemorySource src(bytes);
    kdu_codestream cs;
    cs.create(&src);
    // Pass A: only 2 color bits set -> insufficient for 3-channel chroma.
    kdu_quality_limiter q2(0.02F, false);
    q2.set_display_resolution(400.0F, 400.0F);
    q2.set_comp_info(0, 1.0F, 1);
    q2.set_comp_info(1, 1.0F, 2);
    q2.set_comp_info(2, 1.0F, 0);
    cs.apply_input_restrictions(0, 0, 0, 0, nullptr, KDU_WANT_OUTPUT_COMPONENTS,
                                nullptr, &q2);

    // Pass B: all 3 color bits set + subset comp_indices -> comps_of_interest.
    kdu_quality_limiter q3(0.02F, false);
    q3.set_display_resolution(400.0F, 400.0F);
    q3.set_comp_info(0, 1.0F, 1);
    q3.set_comp_info(1, 1.0F, 2);
    q3.set_comp_info(2, 1.0F, 4);
    int comp_indices[2] = {2, 0};
    cs.apply_input_restrictions(2, comp_indices, 0, 0, nullptr,
                                KDU_WANT_OUTPUT_COMPONENTS, nullptr, &q3);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    kdu_multi_synthesis synth;
    synth.create(cs, tile);
    for (int y = 0; y < synth.get_size(0).y; ++y) {
      EXPECT_NE(synth.get_line(0), nullptr);
      EXPECT_NE(synth.get_line(1), nullptr);
    }
    synth.destroy();
    tile.close();
    cs.destroy();
  }

  // 3. Direct kdu_quality_limiter visual weights, resize, negative flags, dup.
  {
    kdu_quality_limiter ql_rev(0.01F, true);
    EXPECT_LT(ql_rev.get_square_visual_weight(HL_BAND, 1, kdu_coords(1, 1),
                                              false, true),
              0.0F);

    kdu_quality_limiter ql(0.01F, false);
    EXPECT_FLOAT_EQ(
        ql.get_square_visual_weight(LL_BAND, 0, kdu_coords(1, 1), false, false),
        1.0F);
    EXPECT_FLOAT_EQ(
        ql.get_square_visual_weight(HL_BAND, 1, kdu_coords(1, 1), false, false),
        1.0F);

    ql.set_display_resolution(0.0F, 400.0F);
    EXPECT_FLOAT_EQ(
        ql.get_square_visual_weight(HL_BAND, 2, kdu_coords(1, 1), false, false),
        1.0F);

    ql.set_display_resolution(1200.0F, 1200.0F);
    for (int rel_depth = 1; rel_depth <= 5; ++rel_depth) {
      EXPECT_GT(ql.get_square_visual_weight(HL_BAND, rel_depth,
                                            kdu_coords(1, 1), false, false),
                0.0F);
      EXPECT_GT(ql.get_square_visual_weight(HH_BAND, rel_depth,
                                            kdu_coords(2, 2), true, false),
                0.0F);
    }
    ql.set_display_resolution(50.0F, 50.0F);
    EXPECT_GT(
        ql.get_square_visual_weight(HL_BAND, 1, kdu_coords(1, 1), false, false),
        0.0F);
    EXPECT_GT(
        ql.get_square_visual_weight(LH_BAND, 1, kdu_coords(1, 1), true, false),
        0.0F);

    ql.set_comp_info(0, 1.0F, 1);
    ql.set_comp_info(12, 2.0F, -1);
    ql.set_comp_info(5, 0.5F, 4);
    float sq_w = 0.0F;
    kdu_int32 tflags = 0;
    ql.get_comp_info(12, sq_w, tflags);
    EXPECT_FLOAT_EQ(sq_w, 2.0F);
    EXPECT_LT(tflags, 0);
    ql.get_comp_info(5, sq_w, tflags);
    EXPECT_FLOAT_EQ(sq_w, 0.5F);
    EXPECT_EQ(tflags, 4);

    kdu_quality_limiter* copy = ql.duplicate();
    ASSERT_NE(copy, nullptr);
    copy->get_comp_info(0, sq_w, tflags);
    EXPECT_FLOAT_EQ(sq_w, 1.0F);
    delete copy;
  }
}

TEST_F(CodestreamTest, ResilientThrowMarkersUnexpectedEocAndCorruptMarkers) {
  // 1. Multi-buffer (>2KB) code-block decoded non-seekably with resilient
  // throw_markers, plus partial layer ignore and injected unexpected marker.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 0;
    spec.extra_params = {"Cblk={64,64}", "Clayers=2"};
    std::vector<std::vector<kdu_int32>> planes(1);
    planes[0].resize(spec.width * spec.height);
    for (int y = 0; y < spec.height; ++y) {
      for (int x = 0; x < spec.width; ++x) {
        const int v =
            ((x * 73) ^ (y * 131) ^ ((x + y) * 37) ^ ((x * y) >> 2)) & 0xFF;
        planes[0][y * spec.width + x] = v - 128;
      }
    }
    const auto bytes = EncodeImage(spec, planes);
    ASSERT_GT(bytes.size(), 2500);

    // Non-seekable resilient decode reading layer 1 (ignore layer 2).
    {
      MemorySource src(bytes, KDU_SOURCE_CAP_SEQUENTIAL);
      kdu_codestream cs;
      cs.create(&src);
      cs.set_resilient(true);
      cs.apply_input_restrictions(0, 0, 0, 1, nullptr);
      kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
      DecodeTileFirstComponent(cs, tile);
      tile.close();
      cs.destroy();
    }

    // Inject unexpected marker FF 95 inside packet body while reading.
    {
      auto bad_body = bytes;
      size_t sod = 0;
      for (size_t i = 2; i + 2 <= bad_body.size(); ++i) {
        if (bad_body[i] == 0xFF && bad_body[i + 1] == 0x93) {
          sod = i + 2;
          break;
        }
      }
      ASSERT_GT(sod, 0);
      const size_t patch_off = sod + 600;
      ASSERT_LT(patch_off + 2, bad_body.size() - 4);
      bad_body[patch_off] = 0xFF;
      bad_body[patch_off + 1] = 0x95;

      MemorySource src(bad_body, KDU_SOURCE_CAP_SEQUENTIAL);
      kdu_codestream cs;
      cs.create(&src);
      cs.set_resilient(true);
      kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
      DecodeTileFirstComponent(cs, tile);
      tile.close();
      cs.destroy();
    }
  }

  // 2. Non-terminal EOC (non-resilient error vs resilient warning), plus
  // corrupt SOP/SOT lengths and skip marker FF 31 in tile-part header.
  {
    ImageSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 1;
    spec.reversible = false;
    spec.extra_params = {"Cuse_sop=yes"};
    const auto planes = MakeTestPlanes(spec);
    const auto base = EncodeImage(spec, planes);

    // Non-terminal EOC inserted right before first SOT (in main header)
    // followed by 0x00 0x00 triggers non-resilient error during create();
    // plus load_sequential over a mid-stream EOC triggers resilient warning.
    {
      auto find_main_hdr_end = [](const std::vector<kdu_byte>& buf) -> size_t {
        size_t pos = 2;
        while (pos + 4 <= buf.size()) {
          const kdu_uint16 m =
              (static_cast<kdu_uint16>(buf[pos]) << 8) | buf[pos + 1];
          if (m == KDU_SOT) return pos;
          const kdu_uint16 len =
              (static_cast<kdu_uint16>(buf[pos + 2]) << 8) | buf[pos + 3];
          pos += 2 + len;
        }
        return 0;
      };
      const size_t first_sot = find_main_hdr_end(base);
      ASSERT_GT(first_sot, 0);
      auto hdr_eoc = base;
      hdr_eoc.insert(hdr_eoc.begin() + first_sot, {0xFF, 0xD9, 0x00, 0x00});
      {
        MemorySource src(hdr_eoc, KDU_SOURCE_CAP_SEQUENTIAL);
        kdu_codestream cs;
        error_handler_.clear();
        EXPECT_NO_THROW(cs.create(&src));
        EXPECT_TRUE(error_handler_.history().empty());
        if (cs.exists()) {
          cs.destroy();
        }
      }

      ImageSpec tspec = spec;
      tspec.tile_size = kdu_coords(16, 32);
      auto two_tile = EncodeImage(tspec, planes);
      const size_t t0_sot = find_main_hdr_end(two_tile);
      ASSERT_GT(t0_sot, 0);
      const kdu_uint32 psot0 =
          (static_cast<kdu_uint32>(two_tile[t0_sot + 6]) << 24) |
          (static_cast<kdu_uint32>(two_tile[t0_sot + 7]) << 16) |
          (static_cast<kdu_uint32>(two_tile[t0_sot + 8]) << 8) |
          two_tile[t0_sot + 9];
      ASSERT_GT(psot0, 12U);
      const size_t second_sot = t0_sot + psot0;
      ASSERT_LT(second_sot + 2, two_tile.size());
      two_tile.insert(two_tile.begin() + second_sot, {0xFF, 0xD9});
      two_tile.push_back(0x00);
      two_tile.push_back(0x00);
      {
        MemorySource src(two_tile, KDU_SOURCE_CAP_SEQUENTIAL);
        kdu_codestream cs;
        cs.create(&src);
        cs.set_resilient(true);
        kdu_pdescriptor_set pset;
        for (int iter = 0; iter < 5; ++iter) {
          bool done = false;
          kdu_tile t = cs.load_sequential(
              &pset, done, 0, 0, KDU_LOAD_SEQUENTIAL_WANT_TILE, nullptr);
          if (!t.exists()) {
            break;
          }
          if (done) {
            t.close();
          }
        }
        cs.destroy();
      }
    }

    // Corrupt SOP length (FF 91 00 02) right after SOD on non-seekable source.
    {
      auto bad_sop = base;
      size_t sod = 0;
      for (size_t i = 2; i + 8 < bad_sop.size(); ++i) {
        if (bad_sop[i] == 0xFF && bad_sop[i + 1] == 0x93) {
          sod = i + 2;
          break;
        }
      }
      ASSERT_GT(sod, 0);
      if (sod + 3 < bad_sop.size() && bad_sop[sod] == 0xFF &&
          bad_sop[sod + 1] == 0x91) {
        bad_sop[sod + 3] = 0x02;
      }
      MemorySource src(bad_sop, KDU_SOURCE_CAP_SEQUENTIAL);
      kdu_codestream cs;
      cs.create(&src);
      cs.set_resilient(true);
      kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
      DecodeTileFirstComponent(cs, tile);
      tile.close();
      cs.destroy();
    }

    // Inject FF 31 skip marker without length in tile-part header and corrupt
    // SOT length (FF 90 00 04).
    {
      size_t first_sot = 0;
      for (size_t i = 2; i + 12 <= base.size(); ++i) {
        if (base[i] == 0xFF && base[i + 1] == 0x90) {
          first_sot = i;
          break;
        }
      }
      ASSERT_GT(first_sot, 0);
      auto with_skip = base;
      // Insert 2-byte marker FF 31 right after SOT segment (12 bytes) and
      // extend Psot by 2.
      with_skip.insert(with_skip.begin() + first_sot + 12, {0xFF, 0x31});
      const kdu_uint32 old_psot =
          (static_cast<kdu_uint32>(with_skip[first_sot + 6]) << 24) |
          (static_cast<kdu_uint32>(with_skip[first_sot + 7]) << 16) |
          (static_cast<kdu_uint32>(with_skip[first_sot + 8]) << 8) |
          with_skip[first_sot + 9];
      if (old_psot > 0) {
        const kdu_uint32 new_psot = old_psot + 2;
        with_skip[first_sot + 6] = static_cast<kdu_byte>(new_psot >> 24);
        with_skip[first_sot + 7] =
            static_cast<kdu_byte>((new_psot >> 16) & 0xFF);
        with_skip[first_sot + 8] =
            static_cast<kdu_byte>((new_psot >> 8) & 0xFF);
        with_skip[first_sot + 9] = static_cast<kdu_byte>(new_psot & 0xFF);
      }
      MemorySource src(with_skip);
      kdu_codestream cs;
      cs.create(&src);
      cs.set_resilient(true);
      kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
      DecodeTileFirstComponent(cs, tile);
      tile.close();
      cs.destroy();

      auto bad_sot = base;
      bad_sot[first_sot + 3] = 0x04;
      MemorySource bad_src(bad_sot);
      kdu_codestream bad_cs;
      error_handler_.clear();
      try {
        bad_cs.create(&bad_src);
        kdu_tile tile = bad_cs.open_tile(kdu_coords(0, 0));
        DecodeTileFirstComponent(bad_cs, tile);
        tile.close();
      } catch (kdu_exception) {
      }
      EXPECT_FALSE(error_handler_.history().empty());
      if (bad_cs.exists()) {
        bad_cs.destroy();
      }
    }
  }
}

TEST_F(CodestreamTest, FragmentedCompressionTlmRewriteAndInvalidFragments) {
  // 1. Two-fragment 2x2 tile compression with ORGgen_tlm=2 and ORGtlm_style.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.tile_size = kdu_coords(32, 32);
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {
        "Clayers=2",
        "ORGgen_tlm=2",
        "ORGtlm_style={short,long}",
    };
    const auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);

    MemoryTarget frag0_tgt;
    frag0_tgt.allow_rewrite = true;
    kdu_dims frag0 = MakeDims(0, 0, 32, 64);
    kdu_codestream cs0;
    cs0.create(&siz, &frag0_tgt, &frag0, 0, 0);
    ConfigureCodestreamParams(cs0, spec);
    for (int ty = 0; ty < 2; ++ty) {
      kdu_tile tile = cs0.open_tile(kdu_coords(0, ty));
      PushTileData(cs0, tile, planes, spec);
      tile.close();
    }
    kdu_long layer_sizes[2] = {400, 900};
    cs0.flush(layer_sizes, 2);
    const kdu_long frag0_bytes =
        cs0.get_total_bytes(/*exclude_main_header=*/true);
    cs0.destroy();
    ASSERT_GT(frag0_bytes, 0);

    // Second fragment completes tiles (1,0) and (1,1) with TLM rewrite into
    // the shared target containing fragment 0.
    kdu_dims frag1 = MakeDims(32, 0, 32, 64);
    kdu_codestream cs1;
    cs1.create(&siz, &frag0_tgt, &frag1, 2, frag0_bytes);
    ConfigureCodestreamParams(cs1, spec);
    for (int ty = 0; ty < 2; ++ty) {
      kdu_tile tile = cs1.open_tile(kdu_coords(1, ty));
      PushTileData(cs1, tile, planes, spec);
      tile.close();
    }
    cs1.flush(layer_sizes, 2);
    cs1.destroy();
    MemorySource full_src(frag0_tgt.data);
    EXPECT_EQ(DecodeFullImage(&full_src).size(), 1);
  }

  // 2. >256 tiles for TLM short/long styles, byte-style fallback/upgrade,
  // and Scbr disallowed with fragments.
  {
    ImageSpec spec;
    spec.width = 17;
    spec.height = 17;
    spec.components = 1;
    spec.tile_size = kdu_coords(1, 1);  // 289 tiles > 256
    spec.reversible = true;
    spec.dwt_levels = 0;
    const auto planes = MakeTestPlanes(spec);
    spec.extra_params = {"ORGgen_tlm=1", "ORGtlm_style={byte,short}"};
    {
      siz_params bad_siz;
      ConfigureSiz(bad_siz, spec);
      MemoryTarget bad_tgt;
      kdu_codestream bad_cs;
      bad_cs.create(&bad_siz, &bad_tgt);
      ConfigureCodestreamParams(bad_cs, spec);
      kdu_dims tile_range;
      bad_cs.get_valid_tiles(tile_range);
      for (int ty = 0; ty < tile_range.size.y; ++ty) {
        for (int tx = 0; tx < tile_range.size.x; ++tx) {
          kdu_tile tile = bad_cs.open_tile(
              kdu_coords(tile_range.pos.x + tx, tile_range.pos.y + ty));
          PushTileData(bad_cs, tile, planes, spec);
          tile.close();
        }
      }
      error_handler_.clear();
      EXPECT_THROW(bad_cs.flush(nullptr, 1), kdu_exception);
      EXPECT_FALSE(error_handler_.history().empty());
      if (bad_cs.exists()) {
        bad_cs.destroy();
      }
    }

    spec.extra_params = {"ORGgen_tlm=1", "ORGtlm_style={short,short}"};
    const auto bytes = EncodeImage(spec, planes);
    MemorySource src(bytes);
    EXPECT_EQ(DecodeFullImage(&src), planes);
  }
  {
    ImageSpec spec;
    spec.width = 16;
    spec.height = 16;
    spec.components = 1;
    spec.tile_size = kdu_coords(8, 8);
    spec.reversible = false;
    spec.dwt_levels = 1;
    spec.extra_params = {"Scbr={1,64}", "ORGgen_tlm=1",
                         "ORGtlm_style={short,short}"};
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    kdu_dims frag = MakeDims(0, 0, 8, 16);
    kdu_codestream cs;
    error_handler_.clear();
    EXPECT_THROW(cs.create(&siz, &tgt, &frag, 0, 0), kdu_exception);
    if (cs.exists()) {
      cs.destroy();
    }
    EXPECT_FALSE(error_handler_.history().empty());
  }
}

TEST_F(CodestreamTest, PpmIgnoreAndTransferPdescriptorTailAndSlopeFlush) {
  // 1. Multi-tile PPM with opening later tile first (exercises ignore_tpart
  // and transfer_tpart) plus malformed PPM error handling.
  {
    ImageSpec spec;
    spec.width = 48;
    spec.height = 48;
    spec.components = 1;
    spec.tile_size = kdu_coords(16, 16);  // 3x3 = 9 tiles
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.extra_params = {"Corder=LRCP", "Clayers=1", "Cblk={8,8}",
                         "Cuse_sop=yes", "Cuse_eph=yes"};
    const auto planes = MakeTestPlanes(spec);
    const auto base = EncodeImage(spec, planes);
    const auto ppm_bytes =
        ConvertJ2kSopEphToPackedHeaders(base, /*use_ppm=*/true,
                                        /*split_markers=*/true);

    // Sequential load_sequential with crop to bottom-right tile (2,2) exercises
    // state->ppm_markers->ignore_tpart() in kd_codestream::load_sequential
    // and transfer_tpart() across split PPM markers.
    {
      MemorySource src(ppm_bytes, KDU_SOURCE_CAP_SEQUENTIAL);
      kdu_codestream cs;
      cs.create(&src);
      kdu_dims crop = MakeDims(32, 32, 16, 16);
      cs.apply_input_restrictions(0, 0, 0, 0, &crop);
      kdu_pdescriptor_set pset;
      bool done = false;
      int decoded = 0;
      for (int iter = 0; iter < 8; ++iter) {
        kdu_tile tile = cs.load_sequential(
            &pset, done, 0, 0, KDU_LOAD_SEQUENTIAL_WANT_TILE, nullptr);
        if (!tile.exists()) {
          break;
        }
        if (done) {
          DecodeTileFirstComponent(cs, tile);
          tile.close();
          ++decoded;
        }
      }
      EXPECT_EQ(decoded, 1);
      cs.destroy();
    }
    // Corrupt Nppm field in first PPM marker to trigger malformed PPM error
    // in transfer_tpart, and split a 2-byte first PPM segment to exercise
    // straddling-Nppm checks in both transfer_tpart and ignore_tpart.
    {
      auto bad_ppm = ppm_bytes;
      size_t first_ppm = 0;
      for (size_t i = 2; i + 10 < bad_ppm.size(); ++i) {
        if (bad_ppm[i] == 0xFF && bad_ppm[i + 1] == 0x60) {
          first_ppm = i;
          bad_ppm[i + 5] = 0x7F;
          bad_ppm[i + 6] = 0xFF;
          break;
        }
      }
      ASSERT_GT(first_ppm, 0);
      MemorySource src(bad_ppm);
      kdu_codestream cs;
      error_handler_.clear();
      try {
        cs.create(&src);
        kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
        DecodeTileFirstComponent(cs, tile);
        tile.close();
      } catch (kdu_exception) {
      }
      EXPECT_FALSE(error_handler_.history().empty());
      if (cs.exists()) {
        cs.destroy();
      }

      // Split first PPM marker so it holds only 2 bytes of the 4-byte Nppm.
      const kdu_uint16 ppm_len =
          (static_cast<kdu_uint16>(ppm_bytes[first_ppm + 2]) << 8) |
          ppm_bytes[first_ppm + 3];
      if (ppm_len > 6) {
        auto straddle = ppm_bytes;
        const kdu_uint16 rem_data_len = ppm_len - 5;  // excludes Zppm + 2 bytes
        straddle[first_ppm + 2] = 0x00;
        straddle[first_ppm + 3] = 0x05;  // Zppm(1) + 2 payload bytes + 2 len
        const std::vector<kdu_byte> next_hdr = {
            0xFF, 0x60, static_cast<kdu_byte>((rem_data_len + 3) >> 8),
            static_cast<kdu_byte>((rem_data_len + 3) & 0xFF), 0x01};
        straddle.insert(straddle.begin() + first_ppm + 7, next_hdr.begin(),
                        next_hdr.end());
        // Fix Zppm numbering for any pre-existing split PPM markers after this.
        size_t pos = first_ppm + 7 + next_hdr.size() + rem_data_len;
        kdu_byte z = 2;
        while (pos + 5 <= straddle.size() && straddle[pos] == 0xFF &&
               straddle[pos + 1] == 0x60) {
          straddle[pos + 4] = z++;
          const kdu_uint16 l =
              (static_cast<kdu_uint16>(straddle[pos + 2]) << 8) |
              straddle[pos + 3];
          pos += 2 + l;
        }
        {
          MemorySource s_src(straddle);
          kdu_codestream s_cs;
          error_handler_.clear();
          try {
            s_cs.create(&s_src);
            kdu_tile t = s_cs.open_tile(kdu_coords(0, 0));
            DecodeTileFirstComponent(s_cs, t);
            t.close();
          } catch (kdu_exception) {
          }
          EXPECT_FALSE(error_handler_.history().empty());
          if (s_cs.exists()) s_cs.destroy();
        }
        {
          MemorySource i_src(straddle, KDU_SOURCE_CAP_SEQUENTIAL);
          kdu_codestream i_cs;
          i_cs.create(&i_src);
          kdu_dims crop = MakeDims(32, 32, 16, 16);
          i_cs.apply_input_restrictions(0, 0, 0, 0, &crop);
          kdu_pdescriptor_set pset;
          bool done = false;
          try {
            kdu_tile t = i_cs.load_sequential(
                &pset, done, 0, 0, KDU_LOAD_SEQUENTIAL_WANT_TILE, nullptr);
            if (t.exists() && done) t.close();
          } catch (kdu_exception) {
          }
          if (i_cs.exists()) i_cs.destroy();
        }
      }
    }
  }

  // 2. load_sequential with truncated final tile + extract_tail.
  {
    ImageSpec spec;
    spec.width = 48;
    spec.height = 48;
    spec.components = 1;
    spec.tile_size = kdu_coords(24, 24);
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.extra_params = {"Corder=RPCL", "Clayers=2", "Cprecincts={8,8}",
                         "ORGtparts=R"};
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);
    std::vector<kdu_byte> trunc(bytes.begin(),
                                bytes.begin() + (bytes.size() * 7) / 8);

    MemorySource src(trunc, KDU_SOURCE_CAP_SEQUENTIAL);
    kdu_codestream cs;
    cs.create(&src);
    cs.set_resilient(true);
    kdu_pdescriptor_set flush_set;
    bool tile_done = false;
    int tiles_seen = 0;
    while (tiles_seen < 6) {
      kdu_tile tile = cs.load_sequential(
          &flush_set, tile_done, 0, 0,
          KDU_LOAD_SEQUENTIAL_WANT_TILE | KDU_LOAD_SEQUENTIAL_WANT_FLUSH_SET,
          nullptr);
      if (!tile.exists()) {
        break;
      }
      ++tiles_seen;
      kdu_pdescriptor_set tail;
      flush_set.extract_tail(&tail, 0);
      if (tile_done) {
        tile.close();
      }
    }
    EXPECT_GT(tiles_seen, 0);
    cs.destroy();
  }

  // 3. Slope-driven flush overwriting existing Kdu-Layer-Info comment & Scbr
  // bulk zero-fill with excess byte budget.
  {
    ImageSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {"Clayers=2"};
    const auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ConfigureCodestreamParams(cs, spec);
    kdu_codestream_comment cmt = cs.add_comment();
    ASSERT_TRUE(cmt.exists());
    EXPECT_TRUE(cmt.put_text("Kdu-Layer-Info: 0,0\n"));
    {
      kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
      PushTileData(cs, tile, planes, spec);
      tile.close();
    }
    kdu_long layer_bytes[2] = {0, 0};
    kdu_uint16 slopes[2] = {45000, 35000};
    cs.flush(layer_bytes, 2, slopes, /*trim_to_rate=*/false,
             /*record_in_comseg=*/true);
    cs.destroy();
    EXPECT_GT(tgt.data.size(), 0);
  }
  {
    ImageSpec spec;
    spec.width = 16;
    spec.height = 128;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 1;
    spec.extra_params = {"Scbr={1,32}", "Clayers=1", "Corder=PCRL",
                         "Cprecincts={8,8}"};
    const auto planes = MakeTestPlanes(spec);
    siz_params cbr_siz;
    ConfigureSiz(cbr_siz, spec);
    MemoryTarget tgt;
    kdu_codestream cs;
    cs.create(&cbr_siz, &tgt);
    ConfigureCodestreamParams(cs, spec);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    PushTileData(cs, tile, planes, spec);
    tile.close();
    kdu_long cbr_bytes[1] = {32768};
    cs.flush(cbr_bytes, 1, nullptr, /*trim_to_rate=*/true,
             /*record_in_comseg=*/false);
    cs.destroy();
    EXPECT_GT(tgt.data.size(), 1000);
  }
}

TEST_F(CodestreamTest, SampleAllocatorMembackedRestartAndErrorPaths) {
  // 1. Multi-fragment kdu_sample_allocator grow/shrink, permits, overflow,
  // and error paths.
  {
    kdu_membroker broker(16LL << 20);
    kdu_sample_allocator alloc;
    EXPECT_TRUE(alloc.configure(&broker, 18));  // 256 KiB fragments
    std::vector<size_t> offs;
    for (int i = 0; i < 6; ++i) {
      offs.push_back(alloc.pre_alloc(KDU_SAMPLE_PREC32, 8, 60000));
    }
    alloc.finalize(kdu_codestream());
    for (size_t off : offs) {
      EXPECT_NE(alloc.alloc32(8, 60000, off), nullptr);
    }
    EXPECT_GT(alloc.get_size(), 256U * 1024U);
    alloc.restart();

    // Shrink down to 1 small fragment then grow past previous fragment count.
    const size_t small_off = alloc.pre_alloc(KDU_SAMPLE_PREC16, 4, 64);
    alloc.finalize(kdu_codestream());
    EXPECT_NE(alloc.alloc16(4, 64, small_off), nullptr);
    alloc.restart();
    for (int i = 0; i < 8; ++i) {
      alloc.pre_alloc_block(200U * 1024U);
    }
    alloc.finalize(kdu_codestream());
    EXPECT_GT(alloc.get_size(), 1024U * 1024U);
    alloc.restart();
    alloc.release();

    // Outstanding permit rejected by tight membroker during configure().
    kdu_membroker tiny_broker(512);
    kdu_sample_allocator permit_alloc;
    EXPECT_TRUE(permit_alloc.seek_permit(2048));
    error_handler_.clear();
    EXPECT_THROW(permit_alloc.configure(&tiny_broker, 18), kdu_exception);
    EXPECT_FALSE(error_handler_.history().empty());
    permit_alloc.release_permit(2048);
    EXPECT_TRUE(permit_alloc.configure(&tiny_broker, 18));
    EXPECT_FALSE(permit_alloc.seek_permit(2048, /*return_on_failure=*/true));
    error_handler_.clear();
    EXPECT_THROW(permit_alloc.seek_permit(2048, /*return_on_failure=*/false),
                 kdu_exception);
    EXPECT_FALSE(error_handler_.history().empty());

    // Failed release warning & arithmetic overflow error.
    warning_handler_.clear();
    permit_alloc.release_permit(9999);
    EXPECT_FALSE(warning_handler_.history().empty());
    error_handler_.clear();
    EXPECT_THROW(
        permit_alloc.safe_mul(std::numeric_limits<size_t>::max() / 2, 4),
        kdu_exception);
    EXPECT_FALSE(error_handler_.history().empty());

    // Calling configure with changed frag_bits while pre_alloc active errors.
    permit_alloc.pre_alloc_block(128);
    error_handler_.clear();
    EXPECT_THROW(permit_alloc.configure(&broker, 19), kdu_exception);
    EXPECT_FALSE(error_handler_.history().empty());
  }

  // 2. TestMembackedSource unserviced / partially serviced buffer wrapping,
  // seek beyond span, descriptor resize, reset, and async non-blocking load.
  {
    ImageSpec spec;
    spec.width = 48;
    spec.height = 48;
    spec.components = 3;
    spec.reversible = true;
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);
    ASSERT_GT(bytes.size(), 512);

    const kdu_byte desc1[4] = {1, 2, 3, 4};
    const kdu_byte desc2[24] = {9, 8, 7, 6};
    TestMembackedSource mb(bytes, 64);
    EXPECT_TRUE(mb.enable_stream(KDU_MEMBACKED_SOURCE_UNSERVICED, desc1,
                                 sizeof(desc1), 1, 1.0, 128, 256));
    ASSERT_TRUE(mb.run_open(false));
    SlowAsyncNotifier notifier(true);
    mb.load_memory(384, /*blocking=*/false, &notifier);
    kdu_byte tmp[192];
    const int nread = mb.read(tmp, sizeof(tmp));
    EXPECT_GT(nread, 0);
    EXPECT_TRUE(mb.seek(static_cast<kdu_long>(bytes.size() + 64)));
    kdu_byte eof_tmp[4];
    EXPECT_EQ(mb.read(eof_tmp, sizeof(eof_tmp)), 0);
    EXPECT_TRUE(mb.seek(0));
    EXPECT_TRUE(mb.close());

    mb.run_reset();
    EXPECT_TRUE(mb.enable_stream(KDU_MEMBACKED_SOURCE_FULLY_SERVICED, desc2,
                                 sizeof(desc2), 2, 2.0, 128, 0));
    size_t total = 0;
    kdu_int32 term_code = 0;
    EXPECT_FALSE(mb.run_service(total, term_code));
    ASSERT_TRUE(mb.run_open(false));
    EXPECT_GE(total, bytes.size());
    EXPECT_TRUE(mb.close());
  }

  // 3. Input restart() preserving KDU_WANT_OUTPUT_COMPONENTS (across struct
  // change) and KDU_WANT_CODESTREAM_COMPONENTS, plus map_region / stats errors.
  {
    ImageSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 3;
    spec.reversible = true;
    spec.extra_params = {"Cycc=yes"};
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);

    ImageSpec spec2 = spec;
    spec2.width = 16;
    spec2.height = 16;
    const auto bytes2 = EncodeImage(spec2, MakeTestPlanes(spec2));

    MemorySource src(bytes);
    MemorySource src2(bytes2);
    kdu_codestream cs;
    cs.create(&src);
    cs.enable_restart();
    int idx[2] = {2, 0};
    cs.apply_input_restrictions(2, idx, 0, 0, nullptr,
                                KDU_WANT_OUTPUT_COMPONENTS);
    {
      kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
      kdu_multi_synthesis synth;
      synth.create(cs, tile, nullptr, nullptr, KDU_MULTI_XFORM_DEFAULT_FLAGS);
      const int h = synth.get_size(0).y;
      const int out_comps = cs.get_num_components(true);
      ASSERT_EQ(out_comps, 2);
      for (int y = 0; y < h; ++y) {
        for (int c = 0; c < out_comps; ++c) {
          EXPECT_NE(synth.get_line(c), nullptr);
        }
      }
      synth.destroy();
      tile.close();
    }
    cs.restart(&src2);
    EXPECT_EQ(cs.get_num_components(true), 2);

    cs.apply_input_restrictions(2, idx, 0, 0, nullptr,
                                KDU_WANT_CODESTREAM_COMPONENTS);
    src2.seek(0);
    cs.restart(&src2);
    EXPECT_EQ(cs.get_num_components(), 2);

    // Out-of-range component map_region and add_flush_stats on input fail.
    kdu_dims in_reg = MakeDims(0, 0, 16, 16);
    kdu_dims out_reg;
    error_handler_.clear();
    EXPECT_THROW(cs.map_region(99, in_reg, out_reg), kdu_exception);
    EXPECT_FALSE(error_handler_.history().empty());
    error_handler_.clear();
    EXPECT_THROW(cs.add_flush_stats(0, 3, 0, 1), kdu_exception);
    EXPECT_FALSE(error_handler_.history().empty());
    cs.destroy();
  }
}

TEST_F(CodestreamTest, AsyncMemoryAndCachedSourceInputPaths) {
  // Helper memory-backed async source that grows in chunks and reallocates
  // buffers on expansion so kd_compressed_input observes addr != cur_base.
  class AsyncMemSource : public kdu_compressed_source {
   public:
    AsyncMemSource(std::vector<kdu_byte> data, size_t initial_avail,
                   size_t chunk_bytes, int caps, bool note_block_once = false)
        : data_(std::move(data)),
          avail_(std::min(initial_avail, data_.size())),
          chunk_(chunk_bytes),
          caps_(caps),
          note_block_once_(note_block_once) {
      current_.assign(data_.begin(), data_.begin() + avail_);
    }

    int get_capabilities() override { return caps_; }

    int read(kdu_byte* buf, int num_bytes) override {
      if (pos_ >= avail_ || num_bytes <= 0) return 0;
      const size_t n = std::min(static_cast<size_t>(num_bytes), avail_ - pos_);
      std::memcpy(buf, data_.data() + pos_, n);
      pos_ += n;
      return static_cast<int>(n);
    }

    bool seek(kdu_long offset) override {
      if (!(caps_ & KDU_SOURCE_CAP_SEEKABLE) || offset < 0) return false;
      pos_ = std::min(static_cast<size_t>(offset), data_.size());
      if (pos_ > avail_) avail_ = pos_;
      return true;
    }

    kdu_byte* access_memory(kdu_long& pos, kdu_byte*& lim) override {
      pos = 0;
      lim = current_.data() + current_.size();
      return current_.data();
    }

    int load_memory(kdu_long desired_span, bool blocking,
                    kdu_compressed_source_notifier* notifier) override {
      (void)blocking;
      if (note_block_once_ && notifier != nullptr) {
        note_block_once_ = false;
        notifier->note_blocking();
        notifier->note_unblocked();
      }
      if (static_cast<size_t>(desired_span) > avail_ && avail_ < data_.size()) {
        if (notifier != nullptr && !blocking) {
          notifier->prepare_notify(desired_span);
        }
        const size_t next_avail = std::min(
            data_.size(),
            std::max(static_cast<size_t>(desired_span), avail_ + chunk_));
        if (!current_.empty()) {
          retired_.push_back(std::move(current_));
        }
        // Reserve extra headroom so buffer base address shifts from previous.
        current_.assign(next_avail + 64, 0);
        current_.resize(next_avail);
        std::memcpy(current_.data(), data_.data(), next_avail);
        avail_ = next_avail;
      }
      int flags = 0;
      if (static_cast<size_t>(desired_span) <= avail_) {
        flags |= KDU_SOURCE_SPAN_EXISTS;
      }
      if (avail_ >= data_.size()) {
        flags |= KDU_SOURCE_IS_COMPLETE;
      }
      return flags;
    }

   private:
    std::vector<kdu_byte> data_;
    size_t avail_ = 0;
    size_t chunk_ = 64;
    size_t pos_ = 0;
    int caps_ = 0;
    bool note_block_once_ = false;
    std::vector<kdu_byte> current_;
    std::vector<std::vector<kdu_byte>> retired_;
  };

  class CachedHeaderSource : public kdu_compressed_source {
   public:
    CachedHeaderSource(std::vector<kdu_byte> main_hdr_and_sot,
                       std::vector<kdu_byte> large_tile_hdr,
                       bool fail_tileheader_scope, bool fail_precinct_scope)
        : main_(std::move(main_hdr_and_sot)),
          tile_hdr_(std::move(large_tile_hdr)),
          fail_tileheader_(fail_tileheader_scope),
          fail_precinct_(fail_precinct_scope) {}

    int get_capabilities() override {
      return drop_cached_cap_
                 ? KDU_SOURCE_CAP_SEEKABLE
                 : (KDU_SOURCE_CAP_CACHED | KDU_SOURCE_CAP_SEEKABLE);
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
      if (offset < 0) return false;
      in_tile_scope_ = false;
      pos_ = std::min(static_cast<size_t>(offset), main_.size());
      return true;
    }

    bool set_tileheader_scope(int tnum, int /*num_tiles*/) override {
      if (fail_tileheader_ && tnum > 0) {
        drop_cached_cap_ = true;
        return false;
      }
      in_tile_scope_ = true;
      pos_ = 0;
      return true;
    }

    bool set_precinct_scope(kdu_long /*unique_address*/) override {
      if (fail_precinct_) {
        drop_cached_cap_ = true;
      }
      return false;
    }

   private:
    std::vector<kdu_byte> main_;
    std::vector<kdu_byte> tile_hdr_;
    bool fail_tileheader_ = false;
    bool fail_precinct_ = false;
    bool drop_cached_cap_ = false;
    bool in_tile_scope_ = false;
    size_t pos_ = 0;
  };

  ImageSpec spec;
  spec.width = 64;
  spec.height = 32;
  spec.components = 1;
  spec.reversible = true;
  spec.dwt_levels = 1;
  spec.tile_size = kdu_coords(32, 32);
  spec.extra_params = {"Clayers=2", "Cblk={16,16}", "Cprecincts={16,16}"};
  const auto planes = MakeTestPlanes(spec);
  const auto bytes = EncodeImage(spec, planes);
  ASSERT_GT(bytes.size(), 300U);

  // 1. Incremental AsyncMemSource covering async_check_would_block_and_notify,
  // buffer reallocation, set_max_bytes expansion + truncation, and restart
  // onto a non-memory-backed source (destroying in_notifier).
  {
    AsyncMemSource async_src(bytes, 64, 64,
                             KDU_SOURCE_CAP_MEMORY_BACKED |
                                 KDU_SOURCE_CAP_ASYNC_NOTIFICATION |
                                 KDU_SOURCE_CAP_SEEKABLE,
                             /*note_block_once=*/true);
    kdu_codestream cs;
    cs.create(&async_src);
    cs.enable_restart();
    cs.set_max_bytes(static_cast<kdu_long>(bytes.size()) - 2);
    // Larger limit after tighter limit hits early return in set_max_bytes().
    cs.set_max_bytes(static_cast<kdu_long>(bytes.size()) * 2);
    kdu_tile t0 = cs.open_tile(kdu_coords(0, 0));
    DecodeTileFirstComponent(cs, t0);
    t0.close();
    kdu_tile t1 = cs.open_tile(kdu_coords(1, 0));
    DecodeTileFirstComponent(cs, t1);
    t1.close();

    // Restart with non-memory-backed seekable source -> deletes in_notifier.
    MemorySource non_mem_src(bytes, KDU_SOURCE_CAP_SEEKABLE);
    cs.restart(&non_mem_src);
    cs.set_max_bytes(static_cast<kdu_long>(bytes.size()) - 6);
    kdu_tile tr = cs.open_tile(kdu_coords(0, 0));
    tr.close();
    cs.destroy();
  }

  // 2. AsyncMemSource reaching max_bytes_allowed inside load_buf/load_memory.
  {
    AsyncMemSource capped_src(bytes, 32, 48,
                              KDU_SOURCE_CAP_MEMORY_BACKED |
                                  KDU_SOURCE_CAP_ASYNC_NOTIFICATION |
                                  KDU_SOURCE_CAP_SEEKABLE);
    kdu_codestream cs;
    cs.create(&capped_src);
    cs.set_max_bytes(static_cast<kdu_long>(bytes.size()) - 10);
    kdu_tile t0 = cs.open_tile(kdu_coords(0, 0));
    DecodeTileFirstComponent(cs, t0);
    t0.close();
    cs.destroy();
  }

  // 3. CachedHeaderSource: large >1KiB tileheader scope + missing capability
  // failures in set_tileheader_scope and set_precinct_scope.
  {
    size_t first_sot = 2;
    size_t first_sod = 0;
    while (first_sot + 4 <= bytes.size()) {
      const kdu_uint16 m = (static_cast<kdu_uint16>(bytes[first_sot]) << 8) |
                           bytes[first_sot + 1];
      if (m == KDU_SOT) break;
      const kdu_uint16 len =
          (static_cast<kdu_uint16>(bytes[first_sot + 2]) << 8) |
          bytes[first_sot + 3];
      first_sot += 2 + len;
    }
    for (size_t i = first_sot; i + 2 <= bytes.size(); ++i) {
      if (bytes[i] == 0xFF && bytes[i + 1] == 0x93) {
        first_sod = i;
        break;
      }
    }
    ASSERT_GT(first_sot, 0U);
    ASSERT_GE(first_sod, first_sot + 12);

    // Main header through first SOT segment (SOC..after 12-byte SOT).
    std::vector<kdu_byte> main_hdr(bytes.begin(),
                                   bytes.begin() + first_sot + 12);
    // Append second SOT + SOD so 2-tile cached header is syntactically whole.
    main_hdr.insert(main_hdr.end(), bytes.begin() + first_sot,
                    bytes.begin() + first_sot + 12);
    main_hdr[main_hdr.size() - 8] = 0x00;
    main_hdr[main_hdr.size() - 7] = 0x01;

    // Tile header > 1100 bytes: COM marker followed by real tile-0 header
    // markers (up to, excluding SOD).
    std::vector<kdu_byte> large_tile_hdr;
    const kdu_uint16 com_len = 1104;
    large_tile_hdr.push_back(0xFF);
    large_tile_hdr.push_back(0x64);
    large_tile_hdr.push_back(static_cast<kdu_byte>(com_len >> 8));
    large_tile_hdr.push_back(static_cast<kdu_byte>(com_len & 0xFF));
    large_tile_hdr.push_back(0x00);
    large_tile_hdr.push_back(0x01);
    large_tile_hdr.insert(large_tile_hdr.end(), 1100, 'H');
    large_tile_hdr.insert(large_tile_hdr.end(), bytes.begin() + first_sot + 12,
                          bytes.begin() + first_sod);

    CachedHeaderSource cached_prec_err(main_hdr, large_tile_hdr,
                                       /*fail_tileheader_scope=*/false,
                                       /*fail_precinct_scope=*/true);
    kdu_codestream cs_prec;
    cs_prec.create(&cached_prec_err);
    cs_prec.set_resilient(true);
    kdu_tile t0 = cs_prec.open_tile(kdu_coords(0, 0));
    struct SynthGuard {
      kdu_multi_synthesis synth;
      ~SynthGuard() {
        if (synth.exists()) synth.destroy();
      }
    } guard;
    kdu_dims t0_dims;
    t0.access_component(0).access_resolution().get_dims(t0_dims);
    guard.synth.create(cs_prec, t0);
    error_handler_.clear();
    EXPECT_THROW(
        for (int r = 0; r < t0_dims.size.y;
             ++r) { (void)guard.synth.get_line(0); },
        kdu_exception);
    EXPECT_FALSE(error_handler_.history().empty());
    if (t0.exists()) t0.close();
    cs_prec.destroy();

    CachedHeaderSource cached_thdr_err(main_hdr, large_tile_hdr,
                                       /*fail_tileheader_scope=*/true,
                                       /*fail_precinct_scope=*/false);
    kdu_codestream cs_thdr;
    cs_thdr.create(&cached_thdr_err);
    error_handler_.clear();
    EXPECT_THROW(cs_thdr.open_tile(kdu_coords(1, 0)), kdu_exception);
    EXPECT_FALSE(error_handler_.history().empty());
    cs_thdr.destroy();
  }
}

TEST_F(CodestreamTest, MembackedPartiallyServicedBlockingAndErrors) {
  ImageSpec spec;
  spec.width = 64;
  spec.height = 64;
  spec.components = 1;
  spec.reversible = true;
  spec.extra_params = {"Clayers=2"};
  const auto bytes = EncodeImage(spec, MakeTestPlanes(spec));
  ASSERT_GT(bytes.size(), 300U);

  class FlexMembackedSource : public kdu_membacked_compressed_source {
   public:
    FlexMembackedSource(std::vector<kdu_byte> data, bool block_once,
                        size_t max_step, bool always_block_nonblocking = false)
        : data_(std::move(data)),
          block_once_(block_once),
          always_block_nonblocking_(always_block_nonblocking),
          max_step_(max_step) {}

    void WaitUntilEnteredFetch() {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return entered_fetch_; });
    }

    bool run_enable(int mode, const kdu_byte* desc, size_t desc_len,
                    kdu_uint64 interp, double ts, size_t min_buf,
                    size_t max_buf) {
      return enable(mode, desc, desc_len, interp, ts, min_buf, max_buf);
    }
    bool run_open(bool auto_enable) { return open(auto_enable); }
    bool run_service(size_t& total, kdu_int32& term_code) {
      return service(total, term_code);
    }
    int run_reset() { return reset(); }

   protected:
    kdu_long fetch_data(kdu_long max_bytes, kdu_byte* buf,
                        bool blocking) override {
      size_t cur_off = 0;
      size_t n = 0;
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (!blocking && (always_block_nonblocking_ || block_once_)) {
          block_once_ = false;
          return -1;
        }
        entered_fetch_ = true;
        if (offset_ < data_.size()) {
          cur_off = offset_;
          const size_t avail = data_.size() - offset_;
          n = std::min(avail, static_cast<size_t>(max_bytes));
          if (max_step_ > 0) n = std::min(n, max_step_);
          offset_ += n;
        }
      }
      cv_.notify_all();
      if (n > 0) {
        std::memcpy(buf, data_.data() + cur_off, n);
      }
      if (max_step_ > 0 && n > 0) {
        std::this_thread::yield();
      }
      return static_cast<kdu_long>(n);
    }

   private:
    std::vector<kdu_byte> data_;
    size_t offset_ = 0;
    bool block_once_ = false;
    bool always_block_nonblocking_ = false;
    size_t max_step_ = 0;
    std::mutex mu_;
    std::condition_variable cv_;
    bool entered_fetch_ = false;
  };

  // 1. Invalid mode error in enable().
  {
    FlexMembackedSource mb(bytes, false, 0);
    error_handler_.clear();
    EXPECT_THROW(mb.run_enable(999, nullptr, 0, 0, -1.0, 0, 0), kdu_exception);
    EXPECT_FALSE(error_handler_.history().empty());
  }

  // 2. UNSERVICED blocking notifier path plus max_buffer_size=4096 exhaustion
  // on a >4KiB stream.
  {
    std::vector<kdu_byte> large_bytes(5000, 0);
    std::memcpy(large_bytes.data(), bytes.data(), bytes.size());
    FlexMembackedSource mb(large_bytes, /*block_once=*/true, /*max_step=*/64);
    EXPECT_TRUE(mb.run_enable(KDU_MEMBACKED_SOURCE_UNSERVICED, nullptr, 0, 0,
                              -1.0, 4096, 4096));
    EXPECT_TRUE(mb.run_open(false));
    kdu_codestream cs;
    cs.create(&mb);
    kdu_tile t = cs.open_tile(kdu_coords(0, 0));
    DecodeTileFirstComponent(cs, t);
    t.close();
    cs.destroy();
    // Drain past 4096 to hit add_new_buffer() == false -> en_term_code = -1.
    std::vector<kdu_byte> drain(5000);
    EXPECT_GE(mb.read(drain.data(), 4500), 0);
    EXPECT_TRUE(mb.close());
  }

  // 3. PARTIALLY_SERVICED: non-blocking fetch returns -1 so consumer registers
  // AsyncNotifier -> KDU_SOURCE_WILL_NOTIFY wakes svc -> notify(), plus
  // blocking consumer load_memory, reset() stopping FULLY_SERVICED service(),
  // and close() stopping PARTIALLY_SERVICED service().
  {
    class AsyncNotifier : public kdu_compressed_source_notifier {
     public:
      bool prepare_notify(kdu_long /*desired_span*/) override { return true; }
      void notify() override {
        {
          std::lock_guard<std::mutex> lock(mu_);
          notified_ = true;
        }
        cv_.notify_all();
      }
      void Wait() {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this] { return notified_; });
      }
      void Reset() {
        std::lock_guard<std::mutex> lock(mu_);
        notified_ = false;
      }

     private:
      std::mutex mu_;
      std::condition_variable cv_;
      bool notified_ = false;
    };

    // Pass bytes in PARTIALLY_SERVICED with nonblocking fetch_data returning
    // -1.
    std::vector<kdu_byte> big(262144, 0);
    std::memcpy(big.data(), bytes.data(), bytes.size());
    FlexMembackedSource mb(big, /*block_once=*/false, /*max_step=*/16,
                           /*always_block_nonblocking=*/true);
    EXPECT_TRUE(mb.run_enable(KDU_MEMBACKED_SOURCE_PARTIALLY_SERVICED, nullptr,
                              0, 0, -1.0, 4096, 0));
    EXPECT_TRUE(mb.run_open(false));
    size_t total = 0;
    kdu_int32 term_code = 0;
    std::thread svc([&] { mb.run_service(total, term_code); });

    AsyncNotifier notifier;
    size_t desired = 32;
    while (desired < big.size()) {
      const int fl = mb.load_memory(desired, /*blocking=*/false, &notifier);
      if (fl & KDU_SOURCE_WILL_NOTIFY) {
        notifier.Wait();
        break;
      }
      if (fl & KDU_SOURCE_IS_COMPLETE) {
        break;
      }
      desired += 32;
      std::this_thread::yield();
    }
    kdu_byte tmp[16];
    EXPECT_GT(mb.read(tmp, sizeof(tmp)), 0);
    EXPECT_TRUE(mb.close());
    svc.join();
  }

  // run_reset() stopping an active FULLY_SERVICED service() thread while
  // !is_open.
  {
    std::vector<kdu_byte> big(1 << 20, 0x22);
    FlexMembackedSource mb(big, /*block_once=*/false, /*max_step=*/64);
    EXPECT_TRUE(mb.run_enable(KDU_MEMBACKED_SOURCE_FULLY_SERVICED, nullptr, 0,
                              0, -1.0, 4096, 0));
    size_t total = 0;
    kdu_int32 term_code = 0;
    std::thread svc([&] { mb.run_service(total, term_code); });
    mb.WaitUntilEnteredFetch();
    EXPECT_EQ(mb.run_reset(), 1);
    svc.join();
  }
}

TEST_F(CodestreamTest, FlushStatsCbrBulkFillMctAndTransPcrdPaths) {
  // 1. Output codestream add_flush_stats, attach_flush_stats, link_tile_comp,
  // attach_tile_comp, Scbr multi-flush-set bulk fill >= 8192, auto_flush with
  // KDU_FLUSH_USES_THRESHOLDS_AND_SIZES, Creslengths > 8 entries, and Ctype=C.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 128;
    spec.components = 3;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(0, 0);
    spec.extra_params = {
        "Corder=PCRL", "Cprecincts={8,8}", "Cblk={16,16}",
        "Scbr={1,64}", "Cycc=no",          "Ctype=Y,C,Cr",
    };
    const auto planes = MakeTestPlanes(spec);

    siz_params siz1;
    ConfigureSiz(siz1, spec);
    MemoryTarget tgt1;
    kdu_codestream cs1;
    cs1.create(&siz1, &tgt1);
    ConfigureCodestreamParams(cs1, spec);
    cs1.enable_restart();
    kdu_flush_stats fstats1 = cs1.add_flush_stats(0, 10, 0, 1);

    // Empty binary comment in kdu_codestream_comment.
    cs1.add_comment().put_data(nullptr, 0);

    {
      kdu_tile tile = cs1.open_tile(kdu_coords(0, 0));
      PushTileData(cs1, tile, planes, spec);
      tile.close();
    }
    kdu_long large_target[1] = {32768};
    cs1.flush(large_target, 1, nullptr, /*trim_to_rate=*/true,
              /*record_in_comseg=*/false);
    EXPECT_GT(tgt1.data.size(), 15000U);

    // Restart frame 2 reusing same coding geometry -> hits link_tile_comp().
    MemoryTarget tgt1b;
    fstats1.advance(2);
    cs1.restart(&tgt1b);
    {
      kdu_tile tile = cs1.open_tile(kdu_coords(0, 0));
      PushTileData(cs1, tile, planes, spec);
      tile.close();
    }
    cs1.flush(large_target, 1, nullptr, true, false);
    EXPECT_GT(tgt1b.data.size(), 10000U);

    // Second compatible encoder attaching fstats1 + invalid attach checks.
    siz_params siz2;
    ConfigureSiz(siz2, spec);
    MemoryTarget tgt2;
    kdu_codestream cs2;
    cs2.create(&siz2, &tgt2);
    ConfigureCodestreamParams(cs2, spec);
    kdu_flush_stats fstats2 = cs2.add_flush_stats(1, 10, 0, 1);
    error_handler_.clear();
    EXPECT_THROW(cs2.attach_flush_stats(fstats2), kdu_exception);
    EXPECT_FALSE(error_handler_.history().empty());
    cs2.attach_flush_stats(fstats1);
    {
      kdu_tile tile = cs2.open_tile(kdu_coords(0, 0));
      PushTileData(cs2, tile, planes, spec);
      tile.close();
    }
    cs2.flush(large_target, 1, nullptr, true, false);
    cs2.destroy();
    cs1.destroy();

    // >65530 comment clipping on a separate non-CBR codestream.
    siz_params csiz;
    ImageSpec cspec;
    cspec.width = 16;
    cspec.height = 16;
    cspec.components = 1;
    ConfigureSiz(csiz, cspec);
    MemoryTarget ctgt;
    kdu_codestream ccs;
    ccs.create(&csiz, &ctgt);
    ConfigureCodestreamParams(ccs, cspec);
    ccs.add_comment().put_text(std::string(65540, 'K').c_str());
    kdu_tile ctile = ccs.open_tile(kdu_coords(0, 0));
    PushTileData(ccs, ctile, MakeTestPlanes(cspec), cspec);
    ctile.close();
    kdu_long clayer = 1 << 20;
    ccs.flush(&clayer, 1);
    ccs.destroy();
    EXPECT_GT(ctgt.data.size(), 65500U);
  }

  // 2. auto_flush with KDU_FLUSH_USES_THRESHOLDS_AND_SIZES + Creslengths > 8.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {
        "Clayers=2",
        "Creslengths=500,1000,1500,2000,2500,3000,3500,4000,4500",
    };
    const auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ConfigureCodestreamParams(cs, spec);
    kdu_long min_sizes[2] = {150, 400};
    kdu_uint16 thresholds1[2] = {46000, 42000};
    kdu_uint16 thresholds2[2] = {45000, 41000};
    warning_handler_.clear();
    cs.auto_flush(1, 1, 1, 1, min_sizes, 2, thresholds1, /*trim_to_rate=*/true,
                  /*record_in_comseg=*/false, 0.0, nullptr,
                  KDU_FLUSH_USES_THRESHOLDS_AND_SIZES);
    cs.auto_flush(1, 1, 1, 1, min_sizes, 2, thresholds2, true, false, 0.0,
                  nullptr, KDU_FLUSH_USES_THRESHOLDS_AND_SIZES);
    kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
    PushTileData(cs, tile, planes, spec);
    tile.close();
    cs.flush(min_sizes, 2, thresholds2, true, false, 0.0, nullptr,
             KDU_FLUSH_USES_THRESHOLDS_AND_SIZES);
    cs.destroy();
    EXPECT_GT(tgt.data.size(), 0U);
  }

  // 3. MCT stage-2 widening inputs > prev_stage outputs + 16-component DWT
  // replica loop + map_region(..., want_output_comps=true) before finalize.
  {
    ImageSpec spec;
    spec.width = 16;
    spec.height = 16;
    spec.components = 16;
    spec.reversible = false;
    spec.dwt_levels = 1;
    spec.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=16",
        "Msigned=no",
        "Mprecision=8",
        "Mstage_inputs:I1={0,15}",
        "Mstage_outputs:I1={0,15}",
        "Mstage_collections:I1={16,16}",
        "Mstage_xforms:I1={DWT,0,0,2,0}",
        "Mstage_inputs:I2={0,15}",
        "Mstage_outputs:I2={0,15}",
        "Mstage_collections:I2={16,16}",
        "Mstage_xforms:I2={DEP,0,0,0,0}",
        "Mnum_stages=2",
        "Mstages=1,2",
    };
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);
    MemorySource src(bytes);
    kdu_codestream cs;
    cs.create(&src);
    kdu_dims in_reg = MakeDims(0, 0, 8, 8);
    kdu_dims out_reg;
    cs.map_region(0, in_reg, out_reg, /*want_output_comps=*/true);
    int idx[2] = {15, 0};
    kdu_quality_limiter ql(0.02F, false);
    cs.apply_input_restrictions(2, idx, 0, 0, nullptr,
                                KDU_WANT_OUTPUT_COMPONENTS, nullptr, &ql);
    kdu_tile t = cs.open_tile(kdu_coords(0, 0));
    DecodeTileAllComponents(cs, t);
    t.close();
    cs.destroy();
  }

  // 4. load_sequential pset mismatch + trans_pcrd_opt synthesized packet_bytes.
  {
    ImageSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.tile_size = kdu_coords(16, 32);
    spec.extra_params = {"Clayers=2", "Cblk={8,8}", "Cprecincts={8,8}"};
    const auto bytes = EncodeImage(spec, MakeTestPlanes(spec));

    MemorySource src(bytes);
    kdu_codestream cs;
    cs.create(&src);
    kdu_pdescriptor_set pset_a;
    bool done_a = false;
    kdu_tile t0 = cs.load_sequential(&pset_a, done_a, 1, 0, 0, nullptr);
    ASSERT_TRUE(t0.exists());
    // Finish tile 0 into pset_a, then pass pset_a (tile 0) when sequential
    // cursor advances into tile 1 to exercise pset->get_tile_idx() != t_idx.
    while (!done_a) {
      kdu_tile tn = cs.load_sequential(&pset_a, done_a, 1, 0, 0, nullptr);
      if (!tn.exists()) break;
    }
    if (t0.exists()) t0.close();
    bool done_next = false;
    kdu_tile t_mismatch = cs.load_sequential(
        &pset_a, done_next, 0, 0, KDU_LOAD_SEQUENTIAL_WANT_TILE, nullptr);
    (void)t_mismatch;
    cs.destroy();
  }

  // 5. Large 128x128 2-tile small-block run to exercise buf_server free_blocks
  // and fragmented block recycling, plus kdu_sample_allocator no-broker fail.
  {
    ImageSpec spec;
    spec.width = 128;
    spec.height = 128;
    spec.components = 1;
    spec.reversible = true;
    spec.tile_size = kdu_coords(64, 128);
    spec.extra_params = {"Cblk={8,8}", "Clayers=1"};
    const auto bytes = EncodeImage(spec, MakeTestPlanes(spec));
    MemorySource src(bytes);
    kdu_codestream cs;
    cs.create(&src);
    for (int tx = 0; tx < 2; ++tx) {
      kdu_tile t = cs.open_tile(kdu_coords(tx, 0));
      DecodeTileFirstComponent(cs, t);
      t.close();
    }
    cs.destroy();

    kdu_membroker zero_broker(0);
    kdu_sample_allocator fail_alloc;
    EXPECT_TRUE(fail_alloc.configure(&zero_broker, 18));
    EXPECT_FALSE(fail_alloc.seek_permit(1024, /*return_on_failure=*/true));
    error_handler_.clear();
    EXPECT_THROW(fail_alloc.seek_permit(1024, /*return_on_failure=*/false),
                 kdu_exception);
    EXPECT_FALSE(error_handler_.history().empty());
    kdu_sample_allocator no_broker_alloc;
    EXPECT_TRUE(no_broker_alloc.configure(nullptr, 18));
    const kdu_uint64 huge_req = static_cast<kdu_uint64>(1) << 62;
    EXPECT_TRUE(
        no_broker_alloc.seek_permit(huge_req, /*return_on_failure=*/true));
    EXPECT_FALSE(
        no_broker_alloc.seek_permit(huge_req, /*return_on_failure=*/true));
    error_handler_.clear();
    EXPECT_THROW(
        no_broker_alloc.seek_permit(huge_req, /*return_on_failure=*/false),
        kdu_exception);
    EXPECT_FALSE(error_handler_.history().empty());
    no_broker_alloc.release_permit(huge_req);
  }
}

void PushAllTiles(kdu_codestream& cs, const ImageSpec& spec,
                  const std::vector<std::vector<kdu_int32>>& planes) {
  kdu_dims tiles;
  cs.get_valid_tiles(tiles);
  for (int ty = 0; ty < tiles.size.y; ++ty) {
    for (int tx = 0; tx < tiles.size.x; ++tx) {
      kdu_tile tile =
          cs.open_tile(kdu_coords(tiles.pos.x + tx, tiles.pos.y + ty));
      PushTileData(cs, tile, planes, spec);
      tile.close();
    }
  }
}

TEST_F(CodestreamTest, SequentialModesTilesMctQweightsAndAtkPaths) {
  // 1. Custom ATK DWT kernel + enumerated Part-2 ATK kernel.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {
        "Creversible=no",
        "Ckernels=ATK",
        "Ksymmetric=no",
        "Kextension=SYM",
        "Kreversible=no",
        "Ksteps={2,0,0,0},{2,0,0,0}",
        "Kcoeffs=-0.5,-0.5,0.25,0.25",
    };
    const auto planes = MakeTestPlanes(spec);
    const std::vector<kdu_byte> bytes = EncodeImage(spec, planes);
    MemorySource src(bytes);
    const auto decoded = DecodeFullImage(&src);
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_EQ(decoded[0].size(), static_cast<size_t>(64 * 64));
  }
  {
    ImageSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.extra_params = {"Creversible=yes", "Ckernels=ATK", "Kkernels=I5X3"};
    const auto planes = MakeTestPlanes(spec);
    const std::vector<kdu_byte> bytes = EncodeImage(spec, planes);
    MemorySource src(bytes);
    const auto decoded = DecodeFullImage(&src);
    ASSERT_EQ(decoded.size(), 1u);
  }

  // 2. Qweights warning + kd_mct_stage::apply_output_restrictions
  //    (comps_of_interest == NULL) + get_full_dims with MCT.
  {
    ImageSpec mct_spec;
    mct_spec.width = 64;
    mct_spec.height = 64;
    mct_spec.components = 3;
    mct_spec.reversible = false;
    mct_spec.dwt_levels = 2;
    mct_spec.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=3",
        "Mprecision=8",
        "Msigned=no",
        "Nprecision=8",
        "Nsigned=no",
        "Mstage_inputs:I1={0,2}",
        "Mstage_outputs:I1={0,2}",
        "Mstage_collections:I1={3,3}",
        "Mstage_xforms:I1={MATRIX,1,0,0,0}",
        "Mmatrix_size:I1=9",
        "Mmatrix_coeffs:I1=1,0,0,0,1,0,0,0,1",
        "Mnum_stages=1",
        "Mstages=1",
        "Cycc=no",
        "Ctype=Y,C,Cr",
        "Qweights=50.0,50.0,50.0",
    };
    const auto planes = MakeTestPlanes(mct_spec);
    warning_handler_.clear();
    const std::vector<kdu_byte> mct_bytes = EncodeImage(mct_spec, planes);
    EXPECT_FALSE(warning_handler_.history().empty());

    MemorySource src(mct_bytes);
    kdu_codestream in_cs;
    in_cs.create(&src);
    kdu_dims full_dims;
    in_cs.get_full_dims(0, full_dims, /*want_output_comps=*/true,
                        /*respect_discarded_levels=*/true);
    EXPECT_EQ(full_dims.size.x, 64);
    in_cs.apply_input_restrictions(0, 0, /*discard_levels=*/1, 0, nullptr,
                                   KDU_WANT_OUTPUT_COMPONENTS);
    in_cs.get_full_dims(0, full_dims, /*want_output_comps=*/false,
                        /*respect_discarded_levels=*/true);
    EXPECT_EQ(full_dims.size.x, 32);
    in_cs.apply_input_restrictions(0, 0, 0, 0, nullptr,
                                   KDU_WANT_OUTPUT_COMPONENTS);
    kdu_tile mct_tile = in_cs.open_tile(kdu_coords(0, 0));
    kdu_multi_synthesis synth;
    synth.create(in_cs, mct_tile);
    synth.destroy();
    mct_tile.close();
    in_cs.destroy();
  }

  // 3. Multi-tile open_tiles / close_tiles / access_tile / change_appearance
  //    error + apply_input_restrictions errors + load_sequential modes +
  //    multi-layer trans_pcrd_opt trimming.
  ImageSpec mt_spec;
  mt_spec.width = 64;
  mt_spec.height = 64;
  mt_spec.tile_size = kdu_coords(32, 32);
  mt_spec.components = 3;
  mt_spec.reversible = false;
  mt_spec.dwt_levels = 2;
  mt_spec.extra_params = {
      "Clayers=3",     "Cycc=yes",       "Cuse_sop=yes", "Cuse_eph=yes",
      "ORGgen_tlm=12", "ORGgen_plt=yes", "ORGtparts=R",
  };
  const auto mt_planes = MakeTestPlanes(mt_spec);
  std::vector<kdu_byte> mt_bytes;
  {
    siz_params siz;
    ConfigureSiz(siz, mt_spec);
    MemoryTarget tgt;
    kdu_codestream out_cs;
    out_cs.create(&siz, &tgt);
    ConfigureCodestreamParams(out_cs, mt_spec);
    kdu_codestream_comment cmt = out_cs.add_comment();
    EXPECT_TRUE(cmt.exists());
    cmt.put_text("seq-multi-tile-comment");
    PushAllTiles(out_cs, mt_spec, mt_planes);
    kdu_long layers[3] = {800, 1600, 3200};
    out_cs.flush(layers, 3, nullptr, /*trim_to_rate=*/true,
                 /*record_layer_info_in_comment=*/true, 0.0);
    out_cs.destroy();
    mt_bytes = tgt.data;
  }

  {
    MemorySource src(mt_bytes);
    kdu_codestream in_cs;
    in_cs.create(&src);
    in_cs.set_persistent();
    in_cs.enable_restart();
    error_handler_.clear();
    EXPECT_THROW(in_cs.apply_input_restrictions(99, 1, 0, 0, nullptr,
                                                KDU_WANT_CODESTREAM_COMPONENTS),
                 kdu_exception);
    EXPECT_THROW(in_cs.apply_input_restrictions(99, 1, 0, 0, nullptr,
                                                KDU_WANT_OUTPUT_COMPONENTS),
                 kdu_exception);
    EXPECT_FALSE(error_handler_.history().empty());

    kdu_dims tile_indices;
    in_cs.get_valid_tiles(tile_indices);
    in_cs.open_tiles(tile_indices, /*open_in_background=*/false, nullptr);
    error_handler_.clear();
    EXPECT_THROW(in_cs.change_appearance(true, false, false), kdu_exception);
    kdu_tile t00 = in_cs.access_tile(kdu_coords(0, 0), false, nullptr);
    EXPECT_TRUE(t00.exists());
    in_cs.close_tiles(tile_indices, nullptr);

    kdu_dims single_tile;
    single_tile.pos = kdu_coords(0, 0);
    single_tile.size = kdu_coords(1, 1);
    in_cs.open_tiles(single_tile, false, nullptr);
    kdu_tile t11 = in_cs.access_tile(kdu_coords(1, 1), false, nullptr);
    EXPECT_FALSE(t11.exists());
    in_cs.close_tiles(tile_indices, nullptr);

    MemorySource src2(mt_bytes);
    in_cs.restart(&src2);
    kdu_quality_limiter qlim(0.05F, true);
    in_cs.apply_input_restrictions(
        0, 2, 0, 0, nullptr, KDU_WANT_CODESTREAM_COMPONENTS, nullptr, &qlim);
    kdu_tile tile = in_cs.open_tile(kdu_coords(0, 0));
    EXPECT_TRUE(tile.exists());
    tile.close();
    in_cs.destroy();
  }

  // load_sequential with mode_flags == 0 and WANT_PACKET_BYTES only +
  // multi-layer trans_pcrd_opt trimming.
  for (int mode : {0, KDU_LOAD_SEQUENTIAL_WANT_PACKET_BYTES}) {
    MemorySource src(mt_bytes);
    kdu_codestream in_cs;
    in_cs.create(&src);
    kdu_pdescriptor_set pset;
    bool tile_done = false;
    kdu_tile t = in_cs.load_sequential(&pset, tile_done, /*min_pdescriptors=*/1,
                                       /*min_code_blocks=*/1, mode, nullptr);
    if (t.exists()) {
      t.close();
    }
    EXPECT_TRUE(!pset.is_empty() || tile_done);
    if (!pset.is_empty()) {
      kdu_pdescriptor_set tail;
      pset.extract_tail(&tail, 0);
      tail.reset();
    }
    in_cs.destroy();
  }
  {
    ImageSpec trans_spec;
    trans_spec.width = 64;
    trans_spec.height = 64;
    trans_spec.tile_size = kdu_coords(32, 64);
    trans_spec.components = 1;
    trans_spec.reversible = true;
    trans_spec.dwt_levels = 2;
    trans_spec.extra_params = {"Corder=PCRL", "Clayers=3", "Cprecincts={16,16}",
                               "Cblk={8,8}"};
    const auto trans_planes = MakeTestPlanes(trans_spec);
    const auto trans_in_bytes = EncodeImage(trans_spec, trans_planes);

    MemorySource src(trans_in_bytes);
    kdu_codestream in_cs;
    in_cs.create(&src);
    siz_params* in_siz = in_cs.access_siz();
    ASSERT_NE(in_siz, nullptr);
    siz_params out_siz;
    out_siz.copy_from(in_siz, -1, -1, -1, 0, 0, 0);
    MemoryTarget trans_tgt;
    kdu_codestream trans_cs;
    trans_cs.create(&out_siz, &trans_tgt);
    ConfigureCodestreamParams(trans_cs, trans_spec);
    std::map<std::pair<int, int>, kdu_tile> out_tiles;
    kdu_pdescriptor_set pset;
    for (int iter = 0; iter < 128; ++iter) {
      bool tile_done = false;
      kdu_tile t = in_cs.load_sequential(
          &pset, tile_done, 1, 1,
          KDU_LOAD_SEQUENTIAL_WANT_TILE | KDU_LOAD_SEQUENTIAL_WANT_FLUSH_SET |
              KDU_LOAD_SEQUENTIAL_WANT_PACKET_BYTES,
          nullptr);
      if (!t.exists()) {
        break;
      }
      if (!pset.is_empty()) {
        const kdu_coords tidx = pset.get_tile_idx();
        const std::pair<int, int> key(tidx.x, tidx.y);
        auto it = out_tiles.find(key);
        if (it == out_tiles.end()) {
          it = out_tiles.emplace(key, trans_cs.open_tile(tidx)).first;
        }
        kdu_tile out_tile = it->second;
        for (int d = 0; d < pset.get_num_pdescriptors(); ++d) {
          kdu_pdescriptor* pd = pset.access_pdescriptor(d);
          kdu_tile_comp itc = t.access_component(pd->component_idx);
          kdu_resolution ires = itc.access_resolution(pd->resolution_level);
          kdu_precinct iprec = ires.access_precinct(pd->precinct_idx);
          kdu_tile_comp xtc = out_tile.access_component(pd->component_idx);
          kdu_resolution xres = xtc.access_resolution(pd->resolution_level);
          const int min_b = ires.which() ? 1 : 0;
          const int max_b = ires.which() ? 3 : 0;
          for (int b = min_b; b <= max_b; ++b) {
            kdu_dims blocks;
            if (!iprec.get_valid_blocks(b, blocks) || blocks.is_empty()) {
              continue;
            }
            kdu_subband isub = ires.access_subband(b);
            kdu_subband xsub = xres.access_subband(b);
            for (int r = blocks.pos.y; r < blocks.pos.y + blocks.size.y; ++r) {
              for (int c = blocks.pos.x; c < blocks.pos.x + blocks.size.x;
                   ++c) {
                kdu_coords bidx(c, r);
                kdu_block* ib = isub.open_block(bidx);
                kdu_block* xb = xsub.open_block(bidx);
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
        EXPECT_GE(trans_cs.trans_pcrd_opt(&pset, /*trim_to_rate=*/true,
                                          /*sloppy_middle=*/false, 0.0),
                  0);
        pset.reset();
        if (tile_done) {
          out_tile.close();
          out_tiles.erase(it);
        }
      }
      if (tile_done) {
        t.close();
      }
    }
    for (auto& kv : out_tiles) {
      kv.second.close();
    }
    kdu_long out_layers[3] = {150, 300, 400};
    trans_cs.trans_out(400, out_layers, 3, /*record_in_comseg=*/true);
    trans_cs.destroy();
    in_cs.destroy();
  }
}

TEST_F(CodestreamTest, MembrokerBufMasterCbrTrimAndMembackedMultiBufferPaths) {
  // 1. Multi-buffer kdu_membacked_compressed_source (small min/max buffer so
  //    add_new_buffer() runs across chunks) + enable error paths.
  class ChunkedMembackedSource : public kdu_membacked_compressed_source {
   public:
    explicit ChunkedMembackedSource(const std::vector<kdu_byte>& data)
        : data_(data) {}

    bool Enable(int mode, size_t min_buf, size_t max_buf,
                const kdu_byte* desc = nullptr, size_t desc_len = 0) {
      return kdu_membacked_compressed_source::enable(mode, desc, desc_len, 0,
                                                     0.0, min_buf, max_buf);
    }
    bool Open(bool auto_enable) {
      return kdu_membacked_compressed_source::open(auto_enable);
    }
    bool Service(size_t& fetched, kdu_int32& term) {
      return kdu_membacked_compressed_source::service(fetched, term);
    }

   protected:
    kdu_long fetch_data(kdu_long max_bytes, kdu_byte* buffer,
                        bool blocking) override {
      (void)blocking;
      if (pos_ >= data_.size()) {
        return 0;
      }
      const size_t avail = data_.size() - pos_;
      const size_t n = std::min(avail, std::min(static_cast<size_t>(max_bytes),
                                                static_cast<size_t>(48)));
      std::memcpy(buffer, data_.data() + pos_, n);
      pos_ += n;
      return static_cast<kdu_long>(n);
    }

   private:
    const std::vector<kdu_byte>& data_;
    size_t pos_ = 0;
  };

  ImageSpec mb_spec;
  mb_spec.width = 64;
  mb_spec.height = 64;
  mb_spec.components = 3;
  mb_spec.reversible = true;
  mb_spec.dwt_levels = 2;
  const auto mb_planes = MakeTestPlanes(mb_spec);
  const std::vector<kdu_byte> mb_bytes = EncodeImage(mb_spec, mb_planes);
  ASSERT_GT(mb_bytes.size(), 512u);

  {
    ChunkedMembackedSource src(mb_bytes);
    error_handler_.clear();
    EXPECT_THROW(src.Enable(/*mode=*/-1, /*min_buf=*/64, 256), kdu_exception);
    EXPECT_FALSE(error_handler_.history().empty());
    EXPECT_TRUE(src.Enable(KDU_MEMBACKED_SOURCE_FULLY_SERVICED, /*min_buf=*/64,
                           /*max_buf=*/0));
    EXPECT_TRUE(src.Open(/*auto_enable=*/false));
    size_t fetched = 0;
    kdu_int32 term = 0;
    EXPECT_TRUE(src.Service(fetched, term));
    EXPECT_EQ(fetched, mb_bytes.size());
    const auto decoded = DecodeFullImage(&src);
    ASSERT_EQ(decoded.size(), 3u);
    EXPECT_EQ(decoded[0], mb_planes[0]);
  }

  // 2. Tight kdu_membroker driving kd_buf_master::service_lists smaller blocks
  //    + kd_buf_server::strip_new_block / assemble_frag_blocks /
  //    cleanup_and_detach.
  {
    ImageSpec spec;
    spec.width = 48;
    spec.height = 48;
    spec.tile_size = kdu_coords(48, 48);
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 1;
    const auto planes = MakeTestPlanes(spec);

    kdu_membroker tight_broker(786432);
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    kdu_codestream out_cs;
    out_cs.create(&siz, &tgt, nullptr, 0, 0, nullptr, &tight_broker);
    ConfigureCodestreamParams(out_cs, spec);
    PushAllTiles(out_cs, spec, planes);
    kdu_long tight_layer_bytes[1] = {4096};
    out_cs.flush(tight_layer_bytes, 1);
    out_cs.destroy();

    kdu_membroker in_broker(655360);
    MemorySource src(tgt.data);
    kdu_codestream in_cs;
    in_cs.create(&src, nullptr, &in_broker);
    kdu_tile tight_tile = in_cs.open_tile(kdu_coords(0, 0));
    kdu_multi_synthesis synth;
    synth.create(in_cs, tight_tile);
    synth.destroy();
    tight_tile.close();
    in_cs.destroy();
  }

  // 3. Multi-layer flush(..., trim_to_rate=true) on both StructuredCacheTarget
  //    and MemoryTarget + ready_for_flush false + fstats.advance(0) error +
  //    CBR bulk fill threshold & tight budget.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 3;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {"Clayers=3"};
    const auto planes = MakeTestPlanes(spec);

    for (bool use_cached : {true, false}) {
      siz_params siz;
      ConfigureSiz(siz, spec);
      StructuredCacheTarget cached_tgt;
      MemoryTarget mem_tgt;
      kdu_codestream cs;
      cs.create(&siz, use_cached
                          ? static_cast<kdu_compressed_target*>(&cached_tgt)
                          : static_cast<kdu_compressed_target*>(&mem_tgt));
      ConfigureCodestreamParams(cs, spec);
      kdu_tile tile = cs.open_tile(kdu_coords(0, 0));
      EXPECT_FALSE(cs.ready_for_flush());
      PushTileData(cs, tile, planes, spec);
      tile.close();
      EXPECT_TRUE(cs.ready_for_flush());
      kdu_long layers[3] = {150, 350, 700};
      kdu_uint16 thresholds[3] = {0, 0, 0};
      cs.flush(layers, 3, thresholds, /*trim_to_rate=*/true,
               /*record_layer_info_in_comment=*/true, 0.0);
      cs.destroy();
    }
  }

  {
    ImageSpec cbr;
    cbr.width = 64;
    cbr.height = 128;
    cbr.components = 3;
    cbr.reversible = false;
    cbr.dwt_levels = 1;
    cbr.extra_params = {
        "Cycc=no",   "Ctype=Y,C,Cr",       "Corder=PCRL",
        "Clayers=1", "Cprecincts={16,16}", "Scbr={1,64}",
    };
    const auto planes = MakeTestPlanes(cbr);
    siz_params siz;
    ConfigureSiz(siz, cbr);
    MemoryTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ConfigureCodestreamParams(cs, cbr);
    kdu_flush_stats fstats = cs.add_flush_stats(0, 5, 0, 1);
    error_handler_.clear();
    EXPECT_THROW(fstats.advance(0), kdu_exception);
    EXPECT_FALSE(error_handler_.history().empty());
    PushAllTiles(cs, cbr, planes);
    kdu_long cbr_layers[1] = {4096};
    cs.flush(cbr_layers, 1, nullptr, /*trim_to_rate=*/true,
             /*record_layer_info_in_comment=*/false, 0.0);
    cs.destroy();
  }
}

TEST_F(CodestreamTest, CbrBulkFillPcrdOptTrimTlmPltAndFlushStatsPaths) {
  // 1. CBR error checks + huge CBR rate target triggering bulk fill & fine fill
  //    in pcrd_opt_cbr and write_packets_cbr + restart Scbr warning.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 512;
    spec.components = 3;
    spec.reversible = false;
    spec.dwt_levels = 1;
    spec.extra_params = {
        "Cycc=no",   "Ctype=Y,C,Cr",       "Corder=PCRL",
        "Clayers=1", "Cprecincts={16,16}", "Scbr={1,96}",
    };
    const auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    cs.enable_restart();
    ConfigureCodestreamParams(cs, spec);
    PushAllTiles(cs, spec, planes);

    // Huge CBR target forces > 8192 bulk fill across non-final flush sets.
    kdu_long huge_layers[1] = {65536};
    cs.flush(huge_layers, 1, nullptr, /*trim_to_rate=*/true,
             /*record_layer_info_in_comment=*/false, 0.0);

    // Restart with changed tile-specific precinct size alters Scbr flush-set
    // dimensions and triggers warning (10068).
    MemoryTarget tgt2;
    cs.restart(&tgt2);
    ASSERT_TRUE(cs.access_siz()->parse_string("Cprecincts:T0={32,32}"));
    PushAllTiles(cs, spec, planes);
    warning_handler_.clear();
    kdu_long layer2[1] = {65536};
    cs.flush(layer2, 1, nullptr, /*trim_to_rate=*/true,
             /*record_layer_info_in_comment=*/false, 0.0);
    EXPECT_FALSE(warning_handler_.history().empty());
    cs.destroy();
  }

  // Invalid Scbr combinations: Clayers=2, Bmax < flush-set lines, tiny rate,
  // and vertical tiling with sub-tile flush sets.
  for (const char* bad_case :
       {"Clayers=2", "Scbr={1,1}", "tiny_rate", "vert_tiles"}) {
    ImageSpec spec;
    spec.width = 64;
    const bool is_vert_tiles = (std::string(bad_case) == "vert_tiles");
    spec.height = is_vert_tiles ? 128 : 64;
    if (is_vert_tiles) {
      spec.tile_size = kdu_coords(64, 64);
    }
    spec.components = 1;
    spec.reversible = false;
    spec.dwt_levels = 1;
    spec.extra_params = {"Corder=PCRL", "Cprecincts={16,16}"};
    const bool is_two_layers = (std::string(bad_case) == "Clayers=2");
    const bool is_tiny_rate = (std::string(bad_case) == "tiny_rate");
    const bool is_bad_bmax = (std::string(bad_case) == "Scbr={1,1}");
    spec.extra_params.push_back(is_bad_bmax ? "Scbr={1,1}" : "Scbr={1,64}");
    spec.extra_params.push_back(is_two_layers ? "Clayers=2" : "Clayers=1");
    const auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ConfigureCodestreamParams(cs, spec);
    PushAllTiles(cs, spec, planes);
    kdu_long layers[2] = {is_tiny_rate ? 10 : 4096, 8192};
    const int nlayers = is_two_layers ? 2 : 1;
    error_handler_.clear();
    EXPECT_THROW(cs.flush(layers, nlayers), kdu_exception);
    EXPECT_FALSE(error_handler_.history().empty());
    cs.destroy();
  }

  // 2. pcrd_opt unsized prefix / synthesized 2nd-last layer + pcrd_trim
  // bisection
  //    with Creslengths + simulate_output scaling across partial tiles.
  {
    ImageSpec spec;
    spec.width = 96;
    spec.height = 96;
    spec.tile_size = kdu_coords(48, 48);
    spec.components = 3;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {
        "Clayers=5",
        "Cblk={8,8}",
        "Cprecincts={16,16}",
        "Creslengths=128,256,512,1024,0",
    };
    const auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ConfigureCodestreamParams(cs, spec);
    PushAllTiles(cs, spec, planes);
    // Zero entries exercise unsized prefix and synthesized second-last target.
    kdu_long layers[5] = {0, 0, 1200, 0, 0};
    cs.flush(layers, 5, nullptr, /*trim_to_rate=*/true,
             /*record_layer_info_in_comment=*/true, 0.05);
    cs.destroy();
  }

  // 3. kdu_flush_stats: set_max_passes, advance, attach_flush_stats, mismatch.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 3;
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.extra_params = {"Clayers=1", "Cprecincts={32,32}"};
    const auto planes = MakeTestPlanes(spec);

    siz_params siz1;
    ConfigureSiz(siz1, spec);
    MemoryTarget tgt1;
    kdu_codestream cs1;
    cs1.create(&siz1, &tgt1);
    cs1.enable_restart();
    ConfigureCodestreamParams(cs1, spec);
    kdu_flush_stats fstats = cs1.add_flush_stats(0, 10, 0, 1);
    fstats.set_max_passes(/*for_hr=*/true, /*for_lr=*/true, -1, 12);
    fstats.set_max_passes(/*for_hr=*/true, /*for_lr=*/false, 0, 300);
    fstats.set_max_passes(/*for_hr=*/false, /*for_lr=*/true, 1, -1);
    PushAllTiles(cs1, spec, planes);
    kdu_long l1[1] = {4096};
    cs1.flush(l1, 1);

    siz_params siz2;
    ConfigureSiz(siz2, spec);
    MemoryTarget tgt2;
    kdu_codestream cs2;
    cs2.create(&siz2, &tgt2);
    ConfigureCodestreamParams(cs2, spec);
    error_handler_.clear();
    EXPECT_THROW(cs2.attach_flush_stats(fstats), kdu_exception);
    EXPECT_FALSE(error_handler_.history().empty());
    kdu_flush_stats fstats2 = cs2.add_flush_stats(1, 10, 0, 1);
    (void)fstats2;
    EXPECT_TRUE(fstats.advance(1));
    cs2.attach_flush_stats(fstats);
    PushAllTiles(cs2, spec, planes);
    cs2.flush(l1, 1);
    cs2.destroy();

    // Incompatible Cprecincts triggers attach_tile_comp error.
    ImageSpec spec3 = spec;
    spec3.extra_params = {"Clayers=1", "Cprecincts={16,16}"};
    siz_params siz3;
    ConfigureSiz(siz3, spec3);
    MemoryTarget tgt3;
    kdu_codestream cs3;
    cs3.create(&siz3, &tgt3);
    ConfigureCodestreamParams(cs3, spec3);
    cs3.add_flush_stats(2, 10, 0, 1);
    cs3.attach_flush_stats(fstats);
    error_handler_.clear();
    EXPECT_THROW(
        {
          kdu_tile t = cs3.open_tile(kdu_coords(0, 0));
          t.close();
        },
        kdu_exception);
    EXPECT_FALSE(error_handler_.history().empty());
    cs3.destroy();
    cs1.destroy();
  }

  // 4. Multi-tile ORGgen_tlm + ORGtlm_style + ORGgen_plt + PLT/TLM decoding
  //    + random tile access seeking.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.tile_size = kdu_coords(32, 32);
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.extra_params = {
        "Clayers=2",   "ORGgen_tlm=4",   "ORGtlm_style={byte,short}",
        "ORGtparts=R", "ORGgen_plt=yes", "Cprecincts={16,16}",
    };
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);

    MemorySource src(bytes);
    kdu_codestream in_cs;
    in_cs.create(&src);
    in_cs.set_fussy();
    // Open tiles in reverse order to exercise PLT/TLM seeking.
    for (int tnum = 3; tnum >= 0; --tnum) {
      kdu_tile t = in_cs.open_tile(kdu_coords(tnum % 2, tnum / 2));
      DecodeTileFirstComponent(in_cs, t);
      t.close();
    }
    in_cs.destroy();
  }
}

TEST_F(CodestreamTest, MctStagesNltProfilesMembackedLoadMemoryAndInputIoPaths) {
  // 1. Multi-stage MCT + Ctype=alpha,C,R,Y + NLT + Sprofile + MCT
  //    restrictions and create_stages validation errors.
  {
    ImageSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 4;
    spec.reversible = false;
    spec.dwt_levels = 1;
    spec.extra_params = {
        "Sprofile=PROFILE1",
        "Cycc=no",
        "Ctype=N,C,Cb,Y",
    };
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);
    MemorySource src(bytes);
    const auto decoded = DecodeFullImage(&src);
    ASSERT_EQ(decoded.size(), 4u);
  }
  {
    ImageSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 3;
    spec.reversible = false;
    spec.dwt_levels = 1;
    spec.extra_params = {
        "Sextensions=NLT",
        "NLTmake={GAMMA,2.4,0.055,64}",
    };
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);
    MemorySource src(bytes);
    const auto decoded = DecodeFullImage(&src);
    ASSERT_EQ(decoded.size(), 3u);
  }
  {
    ImageSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 2;
    spec.reversible = false;
    spec.dwt_levels = 1;
    spec.extra_params = {
        "Sextensions=NLT",
        "NLType:C0=UMAG",
        "NLTmake:C1={GAMMA,2.2,0.055,64}",
    };
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);
    MemorySource src(bytes);
    const auto decoded = DecodeFullImage(&src);
    ASSERT_EQ(decoded.size(), 2u);
  }

  {
    ImageSpec mct_spec;
    mct_spec.width = 64;
    mct_spec.height = 64;
    mct_spec.components = 3;
    mct_spec.reversible = false;
    mct_spec.dwt_levels = 1;
    mct_spec.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=3",
        "Mprecision=8",
        "Msigned=no",
        "Nprecision=8",
        "Nsigned=no",
        // Stage 1: 2x2 diagonal matrix on {0,1} + null transform on {2}.
        "Mstage_inputs:I1={0,1},{2,2}",
        "Mstage_outputs:I1={0,1},{2,2}",
        "Mstage_collections:I1={2,2},{1,1}",
        "Mstage_xforms:I1={MATRIX,1,0,0,0},{MATRIX,0,0,0,0}",
        "Mmatrix_size:I1=4",
        "Mmatrix_coeffs:I1=1,0,0,1",
        // Stage 2: 3x3 matrix + offset vector on {0,2}.
        "Mstage_inputs:I2={0,2}",
        "Mstage_outputs:I2={0,2}",
        "Mstage_collections:I2={3,3}",
        "Mstage_xforms:I2={MATRIX,2,1,0,0}",
        "Mmatrix_size:I2=9",
        "Mmatrix_coeffs:I2=1,0,0,0,1,0,0,0,1",
        "Mvector_size:I1=3",
        "Mvector_coeffs:I1=0,0,0",
        "Mnum_stages=2",
        "Mstages=1,2",
        "Cycc=no",
    };
    const auto planes = MakeTestPlanes(mct_spec);
    const auto mct_bytes = EncodeImage(mct_spec, planes);

    MemorySource src(mct_bytes);
    kdu_codestream in_cs;
    in_cs.create(&src);
    kdu_quality_limiter ql(0.05F, false);
    in_cs.apply_input_restrictions(0, 2, 0, 0, nullptr,
                                   KDU_WANT_OUTPUT_COMPONENTS, nullptr, &ql);
    kdu_tile t = in_cs.open_tile(kdu_coords(0, 0));
    kdu_multi_synthesis synth;
    synth.create(in_cs, t);
    synth.destroy();
    t.close();
    in_cs.destroy();
  }

  // MCT validation errors in kd_mct_stage::create_stages.
  for (bool bad_vec : {true, false}) {
    ImageSpec bad_spec;
    bad_spec.width = 32;
    bad_spec.height = 32;
    bad_spec.components = 2;
    bad_spec.reversible = false;
    bad_spec.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=2",
        "Mprecision=8",
        "Msigned=no",
        "Nprecision=8",
        "Nsigned=no",
        "Mstage_inputs:I1={0,1}",
        "Mstage_outputs:I1={0,1}",
        "Mstage_collections:I1={2,2}",
        "Mstage_xforms:I1={MATRIX,1,1,0,0}",
        bad_vec ? "Mmatrix_size:I1=4" : "Mmatrix_size:I1=2",
        bad_vec ? "Mmatrix_coeffs:I1=1,0,0,1" : "Mmatrix_coeffs:I1=1,0",
        bad_vec ? "Mvector_size:I1=1" : "Mvector_size:I1=2",
        bad_vec ? "Mvector_coeffs:I1=0" : "Mvector_coeffs:I1=0,0",
        "Mnum_stages=1",
        "Mstages=1",
        "Cycc=no",
    };
    siz_params siz;
    ConfigureSiz(siz, bad_spec);
    MemoryTarget tgt;
    kdu_codestream cs;
    error_handler_.clear();
    EXPECT_THROW(
        {
          cs.create(&siz, &tgt);
          ConfigureCodestreamParams(cs, bad_spec);
          kdu_tile t = cs.open_tile(kdu_coords(0, 0));
          t.close();
        },
        kdu_exception);
    EXPECT_FALSE(error_handler_.history().empty());
    if (cs.exists()) {
      cs.destroy();
    }
  }

  // 2. Large (>12 KB) kdu_membacked_compressed_source across
  // PARTIALLY_SERVICED,
  //    UNSERVICED (load_memory), and add_new_buffer() doubling + cleanup.
  class PagedMembackedSource : public kdu_membacked_compressed_source {
   public:
    explicit PagedMembackedSource(const std::vector<kdu_byte>& data)
        : data_(data) {}

    bool Enable(int mode, size_t min_buf, size_t max_buf,
                const kdu_byte* desc = nullptr, size_t desc_len = 0) {
      return kdu_membacked_compressed_source::enable(mode, desc, desc_len, 0,
                                                     0.0, min_buf, max_buf);
    }
    bool Open(bool auto_enable) {
      return kdu_membacked_compressed_source::open(auto_enable);
    }
    bool Service(size_t& fetched, kdu_int32& term) {
      return kdu_membacked_compressed_source::service(fetched, term);
    }

   protected:
    kdu_long fetch_data(kdu_long max_bytes, kdu_byte* buffer,
                        bool blocking) override {
      (void)blocking;
      if (pos_ >= data_.size()) {
        return 0;
      }
      const size_t avail = data_.size() - pos_;
      const size_t n = std::min(avail, std::min(static_cast<size_t>(max_bytes),
                                                static_cast<size_t>(256)));
      std::memcpy(buffer, data_.data() + pos_, n);
      pos_ += n;
      return static_cast<kdu_long>(n);
    }

   private:
    const std::vector<kdu_byte>& data_;
    size_t pos_ = 0;
  };

  {
    ImageSpec big_spec;
    big_spec.width = 128;
    big_spec.height = 128;
    big_spec.components = 3;
    big_spec.reversible = true;
    big_spec.dwt_levels = 2;
    auto big_planes = MakeTestPlanes(big_spec);
    // Add deterministic pseudo-noise so compressed size exceeds 16 KB.
    for (int c = 0; c < 3; ++c) {
      for (size_t i = 0; i < big_planes[c].size(); ++i) {
        big_planes[c][i] +=
            static_cast<kdu_int32>(((i * 37 + c * 13) % 29) - 14);
      }
    }
    const auto big_bytes = EncodeImage(big_spec, big_planes);
    ASSERT_GT(big_bytes.size(), 12288u);

    {
      PagedMembackedSource src(big_bytes);
      EXPECT_TRUE(src.Enable(KDU_MEMBACKED_SOURCE_FULLY_SERVICED,
                             /*min_buf=*/128, /*max_buf=*/0));
      EXPECT_TRUE(src.Open(/*auto_enable=*/false));
      size_t fetched = 0;
      kdu_int32 term = 0;
      EXPECT_TRUE(src.Service(fetched, term));
      EXPECT_EQ(fetched, big_bytes.size());
      const auto decoded = DecodeFullImage(&src);
      ASSERT_EQ(decoded.size(), 3u);
    }

    {
      PagedMembackedSource src(big_bytes);
      EXPECT_TRUE(src.Enable(KDU_MEMBACKED_SOURCE_PARTIALLY_SERVICED,
                             /*min_buf=*/128, /*max_buf=*/0));
      EXPECT_TRUE(src.Open(/*auto_enable=*/false));
      const auto decoded = DecodeFullImage(&src);
      ASSERT_EQ(decoded.size(), 3u);
    }

    {
      PagedMembackedSource src(big_bytes);
      static constexpr kdu_byte kDesc[4] = {0xAA, 0xBB, 0xCC, 0xDD};
      EXPECT_TRUE(src.Enable(KDU_MEMBACKED_SOURCE_UNSERVICED, /*min_buf=*/128,
                             /*max_buf=*/0, kDesc, sizeof(kDesc)));
      EXPECT_TRUE(src.Open(/*auto_enable=*/false));
      const auto decoded = DecodeFullImage(&src);
      ASSERT_EQ(decoded.size(), 3u);
    }
  }

  // 3. kdu_sample_allocator::release() valid and invalid states +
  //    trans_pcrd_opt error paths & sloppy_middle=true + load_sequential
  //    KDU_LOAD_SEQUENTIAL_WANT_FLUSH_SET.
  {
    kdu_membroker broker(1 << 20);
    kdu_sample_allocator alloc;
    EXPECT_TRUE(alloc.configure(&broker, 16));
    alloc.restart();
    alloc.release();
    alloc.restart();
    alloc.pre_alloc(KDU_SAMPLE_PREC16, 128, 0, 1);
    error_handler_.clear();
    EXPECT_THROW(alloc.release(), kdu_exception);
    EXPECT_FALSE(error_handler_.history().empty());
    alloc.restart();
    alloc.release();
  }

  {
    ImageSpec tspec;
    tspec.width = 64;
    tspec.height = 64;
    tspec.tile_size = kdu_coords(32, 64);
    tspec.components = 1;
    tspec.reversible = false;
    tspec.dwt_levels = 1;
    tspec.extra_params = {"Corder=PCRL", "Clayers=3", "Cprecincts={16,16}",
                          "Cblk={8,8}"};
    const auto planes = MakeTestPlanes(tspec);
    const auto bytes = EncodeImage(tspec, planes);

    MemorySource src(bytes);
    kdu_codestream in_cs;
    in_cs.create(&src);

    siz_params trans_siz;
    ConfigureSiz(trans_siz, tspec);
    MemoryTarget trans_tgt;
    kdu_codestream trans_cs;
    trans_cs.create(&trans_siz, &trans_tgt);
    ConfigureCodestreamParams(trans_cs, tspec);

    // Invalid trans_pcrd_opt on non-existent tile throws.
    kdu_pdescriptor_set bad_pset;
    bad_pset.init(kdu_coords(99, 99), 3);
    bad_pset.add_pdescriptor(0, 0, kdu_coords(0, 0));
    error_handler_.clear();
    EXPECT_THROW(trans_cs.trans_pcrd_opt(&bad_pset, true, true, 0.05),
                 kdu_exception);
    EXPECT_FALSE(error_handler_.history().empty());

    // Load sequentially with WANT_FLUSH_SET and transcode with sloppy_middle.
    std::map<std::pair<int, int>, kdu_tile> out_tiles;
    kdu_pdescriptor_set pset;
    bool tile_done = false;
    while (true) {
      kdu_tile t =
          in_cs.load_sequential(&pset, tile_done, 0, 0,
                                KDU_LOAD_SEQUENTIAL_WANT_FLUSH_SET |
                                    KDU_LOAD_SEQUENTIAL_WANT_PACKET_BYTES,
                                nullptr);
      if (!t.exists()) {
        break;
      }
      if (!pset.is_empty()) {
        const kdu_coords tidx = pset.get_tile_idx();
        const std::pair<int, int> key(tidx.x, tidx.y);
        auto it = out_tiles.find(key);
        if (it == out_tiles.end()) {
          it = out_tiles.emplace(key, trans_cs.open_tile(tidx)).first;
        }
        kdu_tile out_tile = it->second;
        for (int d = 0; d < pset.get_num_pdescriptors(); ++d) {
          kdu_pdescriptor* pd = pset.access_pdescriptor(d);
          kdu_tile_comp itc = t.access_component(pd->component_idx);
          kdu_resolution ires = itc.access_resolution(pd->resolution_level);
          kdu_precinct iprec = ires.access_precinct(pd->precinct_idx);
          kdu_tile_comp xtc = out_tile.access_component(pd->component_idx);
          kdu_resolution xres = xtc.access_resolution(pd->resolution_level);
          const int min_b = ires.which() ? 1 : 0;
          const int max_b = ires.which() ? 3 : 0;
          for (int b = min_b; b <= max_b; ++b) {
            kdu_dims blocks;
            if (!iprec.get_valid_blocks(b, blocks) || blocks.is_empty()) {
              continue;
            }
            kdu_subband isub = ires.access_subband(b);
            kdu_subband xsub = xres.access_subband(b);
            for (int r = blocks.pos.y; r < blocks.pos.y + blocks.size.y; ++r) {
              for (int c = blocks.pos.x; c < blocks.pos.x + blocks.size.x;
                   ++c) {
                kdu_coords bidx(c, r);
                kdu_block* ib = isub.open_block(bidx);
                kdu_block* xb = xsub.open_block(bidx);
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
        }
        EXPECT_GE(trans_cs.trans_pcrd_opt(&pset, /*trim_to_rate=*/true,
                                          /*sloppy_middle=*/true, 0.05),
                  0);
        pset.reset();
        if (tile_done) {
          out_tile.close();
          out_tiles.erase(it);
        }
      }
      if (tile_done) {
        t.close();
      }
    }
    for (auto& kv : out_tiles) {
      kv.second.close();
    }
    kdu_long out_layers[3] = {100, 200, 300};
    trans_cs.trans_out(300, out_layers, 3, /*record_in_comseg=*/false);
    trans_cs.destroy();
    in_cs.destroy();
  }
}

TEST_F(CodestreamTest, SequentialLoadTileLifecycleAndRestartFlushStatsPaths) {
  // 1. Multi-tile multi-tpart load_sequential with WANT_FLUSH_SET (CPRL &
  //    PCRL), ROI skipping dead tiles (KD_EXPIRED_TILE + ignore()), tile
  //    lifecycle APIs (create_tile, open_tiles, access_tile, close_tiles),
  //    share_buffering, and trans_pcrd_opt validation/error paths.
  for (const char* order : {"CPRL", "PCRL"}) {
    ImageSpec spec;
    spec.width = 128;
    spec.height = 128;
    spec.tile_size = kdu_coords(64, 64);
    spec.components = 3;
    spec.reversible = false;
    spec.dwt_levels = 1;
    spec.extra_params = {
        std::string("Corder=") + order,
        "Clayers=3",
        "Cprecincts={32,32}",
        "ORGgen_tlm=64",
        "ORGgen_plt=yes",
        "ORGtparts=C",
    };
    const auto bytes = EncodeImage(spec, MakeTestPlanes(spec));

    // Non-persistent multi-tile lifecycle: create_tile, open_tiles,
    // access_tile, close_tiles, ready_for_flush (false on input).
    MemorySource src_life(bytes);
    kdu_codestream cs_life;
    cs_life.create(&src_life);
    EXPECT_FALSE(cs_life.ready_for_flush());
    kdu_dims valid_tiles;
    cs_life.get_valid_tiles(valid_tiles);
    EXPECT_FALSE(cs_life.access_tile(valid_tiles.pos, false, nullptr).exists());
    cs_life.create_tile(valid_tiles.pos, nullptr);
    cs_life.open_tiles(valid_tiles, false, nullptr);
    kdu_tile t0 = cs_life.access_tile(valid_tiles.pos, false, nullptr);
    EXPECT_TRUE(t0.exists());
    // Re-opening already-open tile range skips existing open tiles.
    cs_life.open_tiles(valid_tiles, false, nullptr);
    cs_life.close_tiles(valid_tiles, nullptr);
    cs_life.destroy();

    // Persistent input with restart + create_tile reinit and share_buffering.
    MemorySource src_seq(bytes);
    kdu_codestream cs_seq;
    cs_seq.create(&src_seq);
    cs_seq.set_persistent();
    cs_seq.enable_restart();
    cs_seq.create_tile(kdu_coords(0, 0), nullptr);

    kdu_codestream cs_share;
    siz_params trans_siz;
    ConfigureSiz(trans_siz, spec);
    MemoryTarget trans_tgt;
    cs_share.create(&trans_siz, &trans_tgt);
    cs_share.share_buffering(cs_seq);
    ConfigureCodestreamParams(cs_share, spec);

    // Transcode tile (0,0) with pset containing packet_bytes_entries=1 on a
    // 3-layer target to exercise synthesized layer entries and sloppy_middle=0.
    kdu_pdescriptor_set pset;
    kdu_tile in_t0 = cs_seq.open_tile(kdu_coords(0, 0));
    kdu_tile out_t0 = cs_share.open_tile(kdu_coords(0, 0));
    pset.init(kdu_coords(0, 0), 1);
    for (int c = 0; c < 3; ++c) {
      kdu_tile_comp ic = in_t0.access_component(c);
      kdu_tile_comp xc = out_t0.access_component(c);
      int num_res = ic.get_num_resolutions();
      for (int r = 0; r < num_res; ++r) {
        kdu_resolution ir = ic.access_resolution(r);
        kdu_resolution xr = xc.access_resolution(r);
        kdu_dims pd_dims;
        ir.get_valid_precincts(pd_dims);
        int idx = pset.add_pdescriptor(c, r, pd_dims.pos);
        kdu_pdescriptor* pd = pset.access_pdescriptor(idx);
        pd->packet_bytes[0] = 64;
        pd->num_packets = 1;
        for (int b = (r == 0 ? 0 : 1); b < (r == 0 ? 1 : 4); ++b) {
          kdu_subband isub = ir.access_subband(b == 0 ? LL_BAND : b);
          kdu_subband xsub = xr.access_subband(b == 0 ? LL_BAND : b);
          kdu_dims blocks;
          isub.get_valid_blocks(blocks);
          for (int by = 0; by < blocks.size.y; ++by) {
            for (int bx = 0; bx < blocks.size.x; ++bx) {
              kdu_coords bidx = blocks.pos + kdu_coords(bx, by);
              kdu_block* ib = isub.open_block(bidx);
              kdu_block* xb = xsub.open_block(bidx);
              xb->num_passes = ib->num_passes;
              xb->missing_msbs = ib->missing_msbs;
              kdu_long tot = 0;
              for (int p = 0; p < ib->num_passes; ++p) {
                xb->pass_lengths[p] = ib->pass_lengths[p];
                xb->pass_slopes[p] = ib->pass_slopes[p];
                tot += ib->pass_lengths[p];
              }
              if (tot > 0) {
                xb->set_max_bytes(tot, false);
                std::memcpy(xb->byte_buffer, ib->byte_buffer,
                            static_cast<size_t>(tot));
              }
              xsub.close_block(xb);
              isub.close_block(ib);
            }
          }
        }
      }
    }
    EXPECT_GE(cs_share.trans_pcrd_opt(&pset, /*trim_to_rate=*/true,
                                      /*sloppy_middle=*/false, 0.05),
              0);

    // Error paths on trans_pcrd_opt: empty pset returns 0, bad tile/precinct/
    // closed tile/missing packet_bytes/set_max_bytes throw.
    kdu_pdescriptor_set empty_pset;
    EXPECT_EQ(cs_share.trans_pcrd_opt(&empty_pset, false, false, 0.0), 0);

    kdu_pdescriptor_set bad_tile_pset;
    bad_tile_pset.init(kdu_coords(9, 9), 3);
    bad_tile_pset.add_pdescriptor(0, 0, kdu_coords(0, 0));
    error_handler_.clear();
    EXPECT_THROW(cs_share.trans_pcrd_opt(&bad_tile_pset, false, false, 0.0),
                 kdu_exception);

    kdu_pdescriptor_set unopened_tile_pset;
    unopened_tile_pset.init(kdu_coords(1, 0), 3);
    unopened_tile_pset.add_pdescriptor(0, 0, kdu_coords(0, 0));
    EXPECT_THROW(
        cs_share.trans_pcrd_opt(&unopened_tile_pset, false, false, 0.0),
        kdu_exception);

    kdu_pdescriptor_set nopbytes_pset;
    nopbytes_pset.init(kdu_coords(0, 0), 0);
    nopbytes_pset.add_pdescriptor(0, 0, kdu_coords(0, 0));
    EXPECT_THROW(cs_share.trans_pcrd_opt(&nopbytes_pset, false, false, 0.0),
                 kdu_exception);

    kdu_pdescriptor_set bad_prec_pset;
    bad_prec_pset.init(kdu_coords(0, 0), 3);
    bad_prec_pset.add_pdescriptor(99, 0, kdu_coords(0, 0));
    EXPECT_THROW(cs_share.trans_pcrd_opt(&bad_prec_pset, false, false, 0.0),
                 kdu_exception);

    out_t0.close();
    in_t0.close();
    kdu_pdescriptor_set unopened_pset;
    unopened_pset.init(kdu_coords(1, 0), 3);
    unopened_pset.add_pdescriptor(0, 0, kdu_coords(0, 0));
    EXPECT_THROW(cs_share.trans_pcrd_opt(&unopened_pset, false, false, 0.0),
                 kdu_exception);
    cs_share.destroy();

    {
      siz_params smb_siz;
      ConfigureSiz(smb_siz, spec);
      MemoryTarget smb_tgt;
      kdu_codestream cs_smb;
      cs_smb.create(&smb_siz, &smb_tgt);
      ConfigureCodestreamParams(cs_smb, spec);
      cs_smb.set_max_bytes(512);
      EXPECT_THROW(cs_smb.trans_pcrd_opt(&pset, false, false, 0.0),
                   kdu_exception);
      cs_smb.destroy();
    }

    // Restart persistent input codestream to test create_tile reinit.
    MemorySource src_seq2(bytes);
    cs_seq.restart(&src_seq2);
    cs_seq.create_tile(kdu_coords(0, 0), nullptr);
    cs_seq.destroy();

    // Fresh input codestream for load_sequential with WANT_FLUSH_SET and
    // tile-mismatch pset preservation.
    MemorySource src_lseq(bytes);
    kdu_codestream cs_lseq;
    cs_lseq.create(&src_lseq);
    kdu_pdescriptor_set seq_pset;
    for (int step = 0; step < 64; ++step) {
      bool tile_done = false;
      kdu_tile t = cs_lseq.load_sequential(
          &seq_pset, tile_done, /*min_pdescriptors=*/1, /*min_code_blocks=*/1,
          KDU_LOAD_SEQUENTIAL_WANT_FLUSH_SET |
              KDU_LOAD_SEQUENTIAL_WANT_PACKET_BYTES,
          nullptr);
      if (!t.exists()) {
        if (seq_pset.is_empty()) break;
        seq_pset.reset();
        continue;
      }
      if (step == 0 && !seq_pset.is_empty() && !tile_done) {
        // Second call without resetting seq_pset exercises non-empty resume.
        bool more_done = false;
        kdu_tile t_more =
            cs_lseq.load_sequential(&seq_pset, more_done, 2, 1,
                                    KDU_LOAD_SEQUENTIAL_WANT_FLUSH_SET |
                                        KDU_LOAD_SEQUENTIAL_WANT_PACKET_BYTES,
                                    nullptr);
        if (more_done && t_more.exists()) {
          t_more.close();
          seq_pset.reset();
          continue;
        }
      }
      if (tile_done) {
        seq_pset.reset();
        t.close();
      }
    }
    cs_lseq.destroy();

    // Fresh non-persistent stream with ROI covering only tile (1,1) forces
    // load_sequential to skip dead tiles (KD_EXPIRED_TILE) via ignore().
    if (std::string(order) == "PCRL") {
      MemorySource src_roi(bytes);
      kdu_codestream cs_roi;
      cs_roi.create(&src_roi);
      kdu_dims roi;
      roi.pos = kdu_coords(64, 64);
      roi.size = kdu_coords(64, 64);
      cs_roi.apply_input_restrictions(0, 0, 0, 0, &roi);
      kdu_pdescriptor_set roi_pset;
      for (int it = 0; it < 16; ++it) {
        bool roi_done = false;
        kdu_tile t = cs_roi.load_sequential(
            &roi_pset, roi_done, 1, 1, KDU_LOAD_SEQUENTIAL_WANT_TILE, nullptr);
        roi_pset.reset();
        if (!t.exists()) break;
        t.close();
      }
      cs_roi.destroy();
    }
  }

  // 2. CBR primary bulk fill (> 8192 bytes per flush set on smooth frame),
  //    set_min_slope_threshold with Scbr, kd_flush_stats::link_tile_comp
  //    across restarts (+ mismatch error), and pcrd_trim tight Creslengths.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 128;
    spec.components = 3;
    spec.reversible = false;
    spec.dwt_levels = 1;
    spec.extra_params = {
        "Cycc=no",   "Ctype=Y,C,Cr",       "Corder=PCRL",
        "Clayers=1", "Cprecincts={16,16}", "Scbr={1,96}",
    };
    std::vector<std::vector<kdu_int32>> smooth(
        3, std::vector<kdu_int32>(64 * 128, 1));
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt1;
    kdu_codestream cs;
    cs.create(&siz, &tgt1);
    cs.enable_restart();
    cs.set_min_slope_threshold(30000);
    ConfigureCodestreamParams(cs, spec);
    kdu_flush_stats fstats = cs.add_flush_stats(0, 5, 0, 1);
    PushAllTiles(cs, spec, smooth);
    kdu_long massive_cbr[1] = {120000};
    cs.flush(massive_cbr, 1, nullptr, /*trim_to_rate=*/true,
             /*record_layer_info_in_comment=*/false, 0.0);

    // Frame 2 links tile_comp flush stats via link_tile_comp.
    MemoryTarget tgt2;
    cs.restart(&tgt2);
    fstats.advance(1);
    PushAllTiles(cs, spec, smooth);
    kdu_long normal_cbr[1] = {16384};
    cs.flush(normal_cbr, 1, nullptr, /*trim_to_rate=*/true,
             /*record_layer_info_in_comment=*/false, 0.0);

    // Frame 3 with altered Cprecincts triggers flush_stats mismatch error.
    MemoryTarget tgt3;
    cs.restart(&tgt3);
    fstats.advance(1);
    ASSERT_TRUE(cs.access_siz()->parse_string("Cprecincts:T0={8,8}"));
    error_handler_.clear();
    EXPECT_THROW(
        {
          PushAllTiles(cs, spec, smooth);
          cs.flush(normal_cbr, 1, nullptr, /*trim_to_rate=*/true,
                   /*record_layer_info_in_comment=*/false, 0.0);
        },
        kdu_exception);
    cs.destroy();
  }

  {
    ImageSpec tight;
    tight.width = 128;
    tight.height = 128;
    tight.components = 1;
    tight.reversible = false;
    tight.dwt_levels = 2;
    tight.extra_params = {"Clayers=2", "Cblk={8,8}", "Creslengths=1,1,1,0"};
    const auto planes = MakeTestPlanes(tight);
    siz_params siz;
    ConfigureSiz(siz, tight);
    MemoryTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ConfigureCodestreamParams(cs, tight);
    PushAllTiles(cs, tight, planes);
    warning_handler_.clear();
    kdu_uint16 slopes[2] = {45000, 40000};
    cs.flush(nullptr, 2, slopes, /*trim_to_rate=*/false,
             /*record_layer_info_in_comment=*/false, 0.0);
    EXPECT_FALSE(warning_handler_.history().empty());
    cs.destroy();
  }
}

TEST_F(CodestreamTest, MctValidationOutputRestrictionsTlmPltAndAllocatorPaths) {
  // 1. kd_mct_stage::create_stages validation errors and stage-output
  //    augmentation + permuted KDU_WANT_OUTPUT_COMPONENTS /
  //    KDU_WANT_CODESTREAM_COMPONENTS with kdu_quality_limiter.
  {
    const std::vector<std::vector<std::string>> bad_mct_configs = {
        // Missing collection for stage 99
        {"Sextensions=MCT", "Mcomponents=3", "Mprecision=8", "Msigned=no",
         "Mnum_stages=1", "Mstages=99"},
        // Zero inputs in collection
        {"Sextensions=MCT", "Mcomponents=3", "Mprecision=8", "Msigned=no",
         "Mnum_stages=1", "Mstages=1", "Mstage_collections:I1={0,3}",
         "Mstage_inputs:I1={0,2}", "Mstage_outputs:I1={0,2}"},
        // Insufficient stage inputs
        {"Sextensions=MCT", "Mcomponents=3", "Mprecision=8", "Msigned=no",
         "Mnum_stages=1", "Mstages=1", "Mstage_collections:I1={3,3}",
         "Mstage_inputs:I1={0,0}", "Mstage_outputs:I1={0,2}"},
        // Insufficient stage outputs
        {"Sextensions=MCT", "Mcomponents=3", "Mprecision=8", "Msigned=no",
         "Mnum_stages=1", "Mstages=1", "Mstage_collections:I1={3,3}",
         "Mstage_inputs:I1={0,2}", "Mstage_outputs:I1={0,0}"},
        // Too many stage-1 inputs (> num_input_components)
        {"Sextensions=MCT", "Mcomponents=3", "Mprecision=8", "Msigned=no",
         "Mnum_stages=1", "Mstages=1", "Mstage_collections:I1={4,4}",
         "Mstage_inputs:I1={0,3}", "Mstage_outputs:I1={0,3}"},
    };
    for (const auto& cfg : bad_mct_configs) {
      ImageSpec spec;
      spec.width = 32;
      spec.height = 32;
      spec.components = 3;
      spec.reversible = true;
      spec.dwt_levels = 1;
      spec.extra_params = cfg;
      siz_params siz;
      ConfigureSiz(siz, spec);
      MemoryTarget tgt;
      kdu_codestream cs;
      cs.create(&siz, &tgt);
      error_handler_.clear();
      EXPECT_THROW(
          {
            ConfigureCodestreamParams(cs, spec);
            cs.open_tile(kdu_coords(0, 0));
          },
          kdu_exception);
      cs.destroy();
    }

    // 2-stage MCT exercising apply_output_restrictions with explicit permuted
    // component indices and kdu_quality_limiter under both modes.
    ImageSpec mct2;
    mct2.width = 64;
    mct2.height = 64;
    mct2.components = 3;
    mct2.reversible = false;
    mct2.dwt_levels = 1;
    mct2.extra_params = {
        "Cycc=no",
        "Sextensions=MCT",
        "Mcomponents=3",
        "Mprecision=8",
        "Msigned=no",
        "Mnum_stages=2",
        "Mstages=1,2",
        "Mstage_collections:I1={3,3}",
        "Mstage_inputs:I1={0,2}",
        "Mstage_outputs:I1={0,2}",
        "Mstage_xforms:I1={MATRIX,1,0,0,0}",
        "Mmatrix_size:I1=9",
        "Mmatrix_coeffs:I1=1.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,1.0",
        "Mstage_collections:I2={2,2},{1,1}",
        "Mstage_inputs:I2={0,2}",
        "Mstage_outputs:I2={0,2}",
        "Mstage_xforms:I2={MATRIX,2,0,0,0},{MATRIX,3,0,0,0}",
        "Mmatrix_size:I2=4",
        "Mmatrix_coeffs:I2=1.0,0.0,0.0,1.0",
        "Mmatrix_size:I3=1",
        "Mmatrix_coeffs:I3=1.0",
    };
    const auto bytes = EncodeImage(mct2, MakeTestPlanes(mct2));
    MemorySource src(bytes);
    kdu_codestream cs;
    cs.create(&src);
    cs.set_persistent();
    kdu_quality_limiter limiter(4.0f, false);
    int out_order[3] = {2, 0, 99};
    cs.apply_input_restrictions(3, out_order, 0, 0, nullptr,
                                KDU_WANT_OUTPUT_COMPONENTS, nullptr, &limiter);
    EXPECT_EQ(cs.get_num_components(true), 2);
    kdu_tile t = cs.open_tile(kdu_coords(0, 0));
    EXPECT_TRUE(t.exists());
    t.close();
    int cs_order[2] = {1, 0};
    cs.apply_input_restrictions(2, cs_order, 0, 0, nullptr,
                                KDU_WANT_CODESTREAM_COMPONENTS, nullptr,
                                &limiter);
    EXPECT_EQ(cs.get_num_components(true), 2);
    cs.destroy();
  }

  // 2. Malformed TLM and PLT marker parsing on modified in-memory headers.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.tile_size = kdu_coords(32, 32);
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.extra_params = {"Clayers=1", "ORGgen_tlm=2",
                         "ORGtlm_style={byte,short}", "ORGgen_plt=yes"};
    const auto base = EncodeImage(spec, MakeTestPlanes(spec));

    auto find_marker = [](const std::vector<kdu_byte>& buf,
                          kdu_byte marker_lo) -> size_t {
      for (size_t i = 0; i + 4 < buf.size(); ++i) {
        if (buf[i] == 0xFF && buf[i + 1] == marker_lo) return i;
      }
      return buf.size();
    };

    const size_t tlm_off = find_marker(base, 0x55);
    ASSERT_LT(tlm_off, base.size());
    // Illegal Stlm (bits 4..5 == 3)
    {
      auto bad = base;
      bad[tlm_off + 5] = 0x30;
      MemorySource src(bad);
      kdu_codestream cs;
      error_handler_.clear();
      EXPECT_THROW(cs.create(&src), kdu_exception);
      cs.destroy();
    }
    // TLM entry length < 14 -> warning (2065)
    {
      auto bad = base;
      bad[tlm_off + 5] = 0x10;
      bad[tlm_off + 7] = 0x00;
      bad[tlm_off + 8] = 0x04;
      MemorySource src(bad);
      kdu_codestream cs;
      warning_handler_.clear();
      cs.create(&src);
      EXPECT_FALSE(warning_handler_.history().empty());
      cs.destroy();
    }
    // TLM invalid tnum >= num_tiles
    {
      auto bad = base;
      bad[tlm_off + 6] = 200;
      MemorySource src(bad);
      kdu_codestream cs;
      error_handler_.clear();
      EXPECT_THROW(cs.create(&src), kdu_exception);
      cs.destroy();
    }
    // TLM malformed length (shrink L_tlm by 1)
    {
      auto bad = base;
      bad[tlm_off + 3] -= 1;
      bad.erase(bad.begin() + static_cast<std::ptrdiff_t>(tlm_off + 6));
      MemorySource src(bad);
      kdu_codestream cs;
      error_handler_.clear();
      EXPECT_THROW(cs.create(&src), kdu_exception);
      cs.destroy();
    }

    const size_t plt_off = find_marker(base, 0x58);
    ASSERT_LT(plt_off, base.size());
    // Out-of-order Zplt
    {
      auto bad = base;
      bad[plt_off + 4] = 3;
      MemorySource src(bad);
      kdu_codestream cs;
      cs.create(&src);
      error_handler_.clear();
      EXPECT_THROW(cs.open_tile(kdu_coords(0, 0)), kdu_exception);
      cs.destroy();
    }
    // Truncated multi-byte varint at end of PLT
    {
      auto bad = base;
      const int lplt = (bad[plt_off + 2] << 8) + bad[plt_off + 3];
      bad[plt_off + 2 + lplt - 1] |= 0x80;
      MemorySource src(bad);
      kdu_codestream cs;
      cs.create(&src);
      error_handler_.clear();
      EXPECT_THROW(cs.open_tile(kdu_coords(0, 0)), kdu_exception);
      cs.destroy();
    }
  }

  // 3. kdu_sample_allocator fragment expansion, premature release() error,
  //    brokered release(), and broker deficit failure.
  {
    ImageSpec dummy;
    dummy.width = 32;
    dummy.height = 32;
    dummy.components = 1;
    siz_params siz;
    ConfigureSiz(siz, dummy);
    MemoryTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);

    kdu_membroker broker(1 << 20);
    kdu_sample_allocator alloc;
    alloc.configure(&broker, 10);  // 1024-byte fragments -> fast expansion
    for (int i = 0; i < 64; ++i) alloc.pre_alloc(KDU_SAMPLE_PREC32, 64, 0, 1);
    error_handler_.clear();
    EXPECT_THROW(alloc.release(), kdu_exception);
    alloc.finalize(cs);
    alloc.restart();
    alloc.release();
    EXPECT_EQ(broker.get_allocated_bytes(false), 0);

    // Exceed tight broker limit in do_finalize.
    kdu_membroker tiny_broker(256);
    kdu_sample_allocator tiny_alloc;
    tiny_alloc.configure(&tiny_broker, 10);
    tiny_alloc.pre_alloc(KDU_SAMPLE_PREC32, 512, 0, 1);
    error_handler_.clear();
    EXPECT_THROW(tiny_alloc.finalize(cs), kdu_exception);
    cs.destroy();
  }
}

TEST_F(CodestreamTest, AdditionalSingleThreadedCodestreamPaths) {
  // 1. membroker refusal on all 3 create() overloads, apply_input_restrictions
  //    state validation errors, and missing ATK kernel error.
  {
    ImageSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 1;
    const auto bytes = EncodeImage(spec, MakeTestPlanes(spec));

    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    kdu_membroker tiny_broker(0);

    kdu_codestream cs_out;
    error_handler_.clear();
    EXPECT_THROW(
        cs_out.create(&siz, &tgt, nullptr, 0, 0, nullptr, &tiny_broker),
        kdu_exception);
    if (cs_out.exists()) cs_out.destroy();

    MemorySource src(bytes);
    kdu_codestream cs_in;
    error_handler_.clear();
    EXPECT_THROW(cs_in.create(&src, nullptr, &tiny_broker), kdu_exception);
    if (cs_in.exists()) cs_in.destroy();

    kdu_codestream cs_ich;
    error_handler_.clear();
    EXPECT_THROW(cs_ich.create(&siz, nullptr, &tiny_broker), kdu_exception);
    if (cs_ich.exists()) cs_ich.destroy();

    // Missing ATK kernel error on decoded codestream with ATK marker stripped.
    ImageSpec atk_spec = spec;
    atk_spec.extra_params = {"Ckernels=ATK", "Catk=2", "Kkernels:I2=R1X1"};
    auto atk_bytes = EncodeImage(atk_spec, MakeTestPlanes(atk_spec));
    for (size_t i = 0; i + 4 < atk_bytes.size(); ++i) {
      if (atk_bytes[i] == 0xFF && atk_bytes[i + 1] == 0x79) {
        const size_t len = 2 + (atk_bytes[i + 2] << 8) + atk_bytes[i + 3];
        atk_bytes.erase(atk_bytes.begin() + i, atk_bytes.begin() + i + len);
        break;
      }
    }
    MemorySource atk_src(atk_bytes);
    kdu_codestream cs_atk;
    error_handler_.clear();
    EXPECT_THROW(
        {
          cs_atk.create(&atk_src);
          cs_atk.open_tile(kdu_coords(0, 0));
        },
        kdu_exception);
    if (cs_atk.exists()) cs_atk.destroy();

    // apply_input_restrictions validation errors.
    siz_params out_siz;
    ConfigureSiz(out_siz, spec);
    MemoryTarget out_tgt;
    kdu_codestream out_check;
    out_check.create(&out_siz, &out_tgt);
    error_handler_.clear();
    EXPECT_THROW(out_check.apply_input_restrictions(1, 0, 0, 0, nullptr),
                 kdu_exception);
    out_check.destroy();

    MemorySource src_nonp(bytes);
    kdu_codestream in_check;
    in_check.create(&src_nonp);
    kdu_tile t = in_check.open_tile(kdu_coords(0, 0));
    error_handler_.clear();
    EXPECT_THROW(in_check.apply_input_restrictions(1, 0, 0, 0, nullptr),
                 kdu_exception);
    t.close();
    error_handler_.clear();
    EXPECT_THROW(in_check.apply_input_restrictions(1, 0, 0, 0, nullptr),
                 kdu_exception);
    in_check.destroy();
  }

  // 2. close_tiles before access, open_tiles after restart (needs_reinit), and
  //    open_tiles / open_tile after permanent output tile closure.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.tile_size = kdu_coords(32, 32);
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 1;
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);

    MemorySource src1(bytes), src2(bytes);
    kdu_codestream cs;
    cs.create(&src1);
    cs.set_persistent();
    cs.enable_restart();
    kdu_dims all_tiles;
    cs.get_valid_tiles(all_tiles);
    cs.close_tiles(all_tiles, nullptr);  // Before any tiles are accessed.
    cs.open_tiles(all_tiles, false, nullptr);
    cs.close_tiles(all_tiles, nullptr);
    cs.restart(&src2);
    cs.open_tiles(all_tiles, false, nullptr);  // Reinitializes needs_reinit.
    cs.close_tiles(all_tiles, nullptr);
    cs.destroy();

    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    kdu_codestream cs_out;
    cs_out.create(&siz, &tgt);
    ConfigureCodestreamParams(cs_out, spec);
    PushAllTiles(cs_out, spec, planes);
    kdu_long lsz[1] = {4096};
    cs_out.flush(lsz, 1);
    cs_out.open_tiles(all_tiles, false,
                      nullptr);  // Ignores permanently closed.
    error_handler_.clear();
    EXPECT_THROW(cs_out.open_tile(kdu_coords(0, 0)), kdu_exception);
    cs_out.destroy();
  }

  // 3. Partial-tile incremental flush scaling in simulate_output, pcrd_trim
  //    with KDU_FLUSH_USES_THRESHOLDS_AND_SIZES + Creslengths,
  //    KDU_FLUSH_THRESHOLDS_ARE_HINTS, and LRCP incremental flush error.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.tile_size = kdu_coords(32, 32);
    spec.components = 3;
    spec.reversible = false;
    spec.dwt_levels = 1;
    spec.extra_params = {"Corder=RPCL", "Clayers=2"};
    const auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    kdu_codestream cs;
    cs.create(&siz, &tgt);
    ConfigureCodestreamParams(cs, spec);

    auto push_one_tile = [&](int tx, int ty) {
      kdu_tile t = cs.open_tile(kdu_coords(tx, ty));
      PushTileData(cs, t, planes, spec);
      t.close();
    };
    push_one_tile(0, 0);
    kdu_long inc_sizes[2] = {1200, 2400};
    cs.flush(inc_sizes, 2, nullptr, /*trim_to_rate=*/true, false, 0.05);
    push_one_tile(1, 0);
    cs.flush(inc_sizes, 2, nullptr, /*trim_to_rate=*/true, false, 0.05);
    push_one_tile(0, 1);
    push_one_tile(1, 1);
    cs.flush(inc_sizes, 2, nullptr, /*trim_to_rate=*/true, false, 0.05);
    cs.destroy();

    // pcrd_trim with min_sizes + Creslengths
    // (KDU_FLUSH_USES_THRESHOLDS_AND_SIZES)
    ImageSpec trim_spec;
    trim_spec.width = 64;
    trim_spec.height = 64;
    trim_spec.components = 1;
    trim_spec.reversible = false;
    trim_spec.dwt_levels = 2;
    trim_spec.extra_params = {"Clayers=2", "Cblk={8,8}",
                              "Creslengths=256,512,0"};
    const auto tplanes = MakeTestPlanes(trim_spec);
    {
      siz_params tsiz;
      ConfigureSiz(tsiz, trim_spec);
      MemoryTarget ttgt;
      kdu_codestream tcs;
      tcs.create(&tsiz, &ttgt);
      ConfigureCodestreamParams(tcs, trim_spec);
      PushAllTiles(tcs, trim_spec, tplanes);
      kdu_long min_sz[2] = {4000, 8000};
      kdu_uint16 slopes[2] = {49000, 44000};
      tcs.flush(min_sz, 2, slopes, /*trim_to_rate=*/true, false, 0.0, nullptr,
                KDU_FLUSH_USES_THRESHOLDS_AND_SIZES);
      tcs.destroy();
    }
    // KDU_FLUSH_THRESHOLDS_ARE_HINTS
    {
      siz_params hsiz;
      ConfigureSiz(hsiz, trim_spec);
      MemoryTarget htgt;
      kdu_codestream hcs;
      hcs.create(&hsiz, &htgt);
      ConfigureCodestreamParams(hcs, trim_spec);
      PushAllTiles(hcs, trim_spec, tplanes);
      kdu_long sz[2] = {400, 1200};
      kdu_uint16 hints[2] = {48000, 44000};
      hcs.flush(sz, 2, hints, /*trim_to_rate=*/true, false, 0.0, nullptr,
                KDU_FLUSH_THRESHOLDS_ARE_HINTS);
      hcs.destroy();
    }
  }

  // 4. load_sequential without TLM/PLT skipping dead tiles + mismatched pset
  //    across tiles + PLT addressable precincts.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.tile_size = kdu_coords(32, 32);
    spec.components = 2;
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.extra_params = {"Clayers=2", "Corder=PCRL", "Cprecincts={16,16}",
                         "Cblk={8,8}"};
    const auto bytes = EncodeImage(spec, MakeTestPlanes(spec));

    // ROI excludes top 3 tiles; load_sequential skips them without TLM.
    MemorySource src_roi(bytes);
    kdu_codestream cs_roi;
    cs_roi.create(&src_roi);
    kdu_dims roi;
    roi.pos = kdu_coords(32, 32);
    roi.size = kdu_coords(32, 32);
    cs_roi.apply_input_restrictions(0, 0, 0, 0, &roi);
    kdu_pdescriptor_set pset_roi;
    for (int iter = 0; iter < 16; ++iter) {
      bool tile_done = false;
      kdu_tile t =
          cs_roi.load_sequential(&pset_roi, tile_done, 1, 1,
                                 KDU_LOAD_SEQUENTIAL_WANT_FLUSH_SET, nullptr);
      if (!t.exists()) break;
      pset_roi.reset();
    }
    cs_roi.destroy();

    // Non-empty pset from tile (0,0) hit against tile (1,0) boundary.
    MemorySource src_mis(bytes);
    kdu_codestream cs_mis;
    cs_mis.create(&src_mis);
    kdu_pdescriptor_set pset_t0;
    for (int iter = 0; iter < 32; ++iter) {
      bool tile_done = false;
      kdu_tile t =
          cs_mis.load_sequential(&pset_t0, tile_done, 1, 1, 0, nullptr);
      if (!t.exists()) break;
      // Intentionally do NOT reset after first partial tile load so the next
      // tile triggers tile incompatibility stop.
      if (pset_t0.get_tile_idx() != kdu_coords(0, 0)) break;
      if (iter > 4 && pset_t0.get_num_complete() == 0) break;
    }
    cs_mis.destroy();

    // Addressable precincts via PLT + partial-component decode before
    // load_sequential.
    ImageSpec plt_spec = spec;
    plt_spec.width = 64;
    plt_spec.height = 64;
    plt_spec.tile_size = kdu_coords(0, 0);
    plt_spec.extra_params.push_back("ORGgen_plt=yes");
    plt_spec.extra_params[1] = "Corder=CPRL";
    const auto plt_bytes = EncodeImage(plt_spec, MakeTestPlanes(plt_spec));
    MemorySource src_plt(plt_bytes);
    kdu_codestream cs_plt;
    cs_plt.create(&src_plt);
    kdu_pdescriptor_set pset_plt;
    for (int iter = 0; iter < 16; ++iter) {
      bool tile_done = false;
      kdu_tile t =
          cs_plt.load_sequential(&pset_plt, tile_done, 1, 1,
                                 KDU_LOAD_SEQUENTIAL_WANT_FLUSH_SET, nullptr);
      if (!t.exists()) break;
      pset_plt.reset();
    }
    cs_plt.destroy();
  }

  // 5. SeekableSource (>16KB reverse tile order, fail_seek, truncated TLM
  // seek),
  //    SequentialSource discard/ignore, and reordered CBD marker.
  {
    ImageSpec big;
    big.width = 160;
    big.height = 160;
    big.tile_size = kdu_coords(80, 80);
    big.components = 3;
    big.reversible = true;
    big.dwt_levels = 2;
    big.extra_params = {"Clayers=2", "ORGgen_tlm=4"};
    const auto bytes = EncodeImage(big, MakeTestPlanes(big));
    ASSERT_GT(bytes.size(), 8192U);

    // Seekable non-in-memory source: open tile (1,1) first then (0,0).
    MemorySource seek_src(bytes,
                          KDU_SOURCE_CAP_SEQUENTIAL | KDU_SOURCE_CAP_SEEKABLE);
    kdu_codestream cs_seek;
    cs_seek.create(&seek_src);
    {
      kdu_tile t11 = cs_seek.open_tile(kdu_coords(1, 1));
      DecodeTileAllComponents(cs_seek, t11);
      t11.close();
      kdu_tile t00 = cs_seek.open_tile(kdu_coords(0, 0));
      DecodeTileAllComponents(cs_seek, t00);
      t00.close();
    }
    cs_seek.destroy();

    // Failed seek error path.
    MemorySource bad_seek_src(
        bytes, KDU_SOURCE_CAP_SEQUENTIAL | KDU_SOURCE_CAP_SEEKABLE);
    kdu_codestream cs_badseek;
    cs_badseek.create(&bad_seek_src);
    bad_seek_src.fail_seek = true;
    error_handler_.clear();
    EXPECT_THROW(
        {
          kdu_tile t11 = cs_badseek.open_tile(kdu_coords(1, 1));
          t11.close();
        },
        kdu_exception);
    cs_badseek.destroy();

    // Non-seekable sequential source skipping higher levels/layers.
    MemorySource seq_src(bytes, KDU_SOURCE_CAP_SEQUENTIAL);
    kdu_codestream cs_seq;
    cs_seq.create(&seq_src);
    cs_seq.apply_input_restrictions(0, 0, 1, 1, nullptr);
    kdu_dims vt;
    cs_seq.get_valid_tiles(vt);
    for (int ty = 0; ty < vt.size.y; ++ty) {
      for (int tx = 0; tx < vt.size.x; ++tx) {
        kdu_tile t = cs_seq.open_tile(kdu_coords(vt.pos.x + tx, vt.pos.y + ty));
        DecodeTileAllComponents(cs_seq, t);
        t.close();
      }
    }
    cs_seq.destroy();

    // CBD marker reordered after COD/QCD -> triggers need_to_seek_back on
    // seekable source and warning on non-seekable source.
    ImageSpec cbd_spec;
    cbd_spec.width = 32;
    cbd_spec.height = 32;
    cbd_spec.components = 3;
    cbd_spec.reversible = true;
    cbd_spec.dwt_levels = 1;
    cbd_spec.extra_params = {
        "Cycc=no",
        "Sextensions=MCT",
        "Mcomponents=3",
        "Mprecision=8",
        "Msigned=no",
        "Mnum_stages=1",
        "Mstages=1",
        "Mstage_inputs:I1={0,2}",
        "Mstage_outputs:I1={0,2}",
        "Mstage_collections:I1={3,3}",
        "Mstage_xforms:I1={MATRIX,0,0,0,0}",
    };
    auto cbd_bytes = EncodeImage(cbd_spec, MakeTestPlanes(cbd_spec));
    auto find_m = [](const std::vector<kdu_byte>& b, kdu_byte lo,
                     size_t start = 0) -> size_t {
      for (size_t i = start; i + 3 < b.size(); ++i) {
        if (b[i] == 0xFF && b[i + 1] == lo) return i;
      }
      return b.size();
    };
    const size_t cbd_pos = find_m(cbd_bytes, 0x78);  // FF 78 = CBD
    const size_t cod_pos = find_m(cbd_bytes, 0x52);  // FF 52 = COD
    if (cbd_pos < cod_pos && cod_pos + 4 < cbd_bytes.size()) {
      const size_t cbd_len =
          2 + (cbd_bytes[cbd_pos + 2] << 8) + cbd_bytes[cbd_pos + 3];
      std::vector<kdu_byte> cbd_seg(cbd_bytes.begin() + cbd_pos,
                                    cbd_bytes.begin() + cbd_pos + cbd_len);
      cbd_bytes.erase(cbd_bytes.begin() + cbd_pos,
                      cbd_bytes.begin() + cbd_pos + cbd_len);
      const size_t sot_pos = find_m(cbd_bytes, 0x90);
      ASSERT_LT(sot_pos, cbd_bytes.size());
      cbd_bytes.insert(cbd_bytes.begin() + sot_pos, cbd_seg.begin(),
                       cbd_seg.end());

      MemorySource s1(cbd_bytes,
                      KDU_SOURCE_CAP_SEQUENTIAL | KDU_SOURCE_CAP_SEEKABLE);
      kdu_codestream c1;
      c1.create(&s1);
      c1.destroy();

      MemorySource s2(cbd_bytes, KDU_SOURCE_CAP_SEQUENTIAL);
      kdu_codestream c2;
      warning_handler_.clear();
      c2.create(&s2);
      EXPECT_FALSE(warning_handler_.history().empty());
      c2.destroy();
    }
  }

  // 6. TestMembackedSource chunked UNSERVICED (add_new_buffer + load_buf
  //    relocation) and PARTIALLY_SERVICED with service thread + notifier.
  {
    ImageSpec big;
    big.width = 160;
    big.height = 160;
    big.components = 3;
    big.reversible = true;
    big.dwt_levels = 2;
    const auto bytes = EncodeImage(big, MakeTestPlanes(big));
    ASSERT_GT(bytes.size(), 8192U);

    TestMembackedSource unserv(bytes, 512);
    ASSERT_TRUE(unserv.enable_stream(KDU_MEMBACKED_SOURCE_UNSERVICED, nullptr,
                                     0, 0, -1.0, 1024, bytes.size() + 4096));
    ASSERT_TRUE(unserv.run_open(false));
    EXPECT_FALSE(DecodeFullImage(&unserv).empty());
    EXPECT_FALSE(unserv.close());  // Already closed by cs.destroy() inside
                                   // DecodeFullImage.

    struct SyncNotifier : public kdu_compressed_source_notifier {
      std::mutex mu;
      std::condition_variable cv;
      bool notified = false;
      int blocked_calls = 0;
      bool prepare_notify(kdu_long) override { return true; }
      void note_blocking() override { ++blocked_calls; }
      void notify() override {
        std::lock_guard<std::mutex> lk(mu);
        notified = true;
        cv.notify_all();
      }
      void wait_done() {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return notified; });
      }
    };

    TestMembackedSource pserv(bytes, 512);
    ASSERT_TRUE(pserv.enable_stream(KDU_MEMBACKED_SOURCE_PARTIALLY_SERVICED,
                                    nullptr, 0, 0, -1.0, 1024,
                                    bytes.size() + 4096));
    std::atomic<bool> svc_started{false};
    size_t svc_fetched = 0;
    kdu_int32 svc_term = 0;
    std::thread svc_thread([&]() {
      svc_started.store(true);
      pserv.run_service(svc_fetched, svc_term);
    });
    while (!svc_started.load()) {
      std::this_thread::yield();
    }
    // Wait briefly so `service()` locks `mutex` and enters `in_service = true`
    // before `DecodeFullImage(&pserv)` closes the source.
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    ASSERT_TRUE(pserv.run_open(false));
    SyncNotifier notifier;
    const int lf = pserv.load_memory(4096, false, &notifier);
    if (lf & KDU_SOURCE_WILL_NOTIFY) {
      notifier.wait_done();
    }
    EXPECT_FALSE(DecodeFullImage(&pserv).empty());
    EXPECT_FALSE(pserv.close());  // Already closed by cs.destroy() inside
                                  // DecodeFullImage.
    svc_thread.join();
  }
}

TEST_F(CodestreamTest, CbrMctMembackedTransAndBufferPoolPaths) {
  // 1. Scbr constant-bit-rate flushing, Cplex=EST, flush_stats +
  //    aux_flush_stats, restart, and Sprofile=PROFILE0 warnings.
  {
    ImageSpec spec;
    spec.width = 128;
    spec.height = 128;
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.extra_params = {"Corder=PCRL", "Cprecincts={32,32}", "Scbr={1,200}"};
    const auto planes = MakeTestPlanes(spec);

    siz_params siz1;
    ConfigureSiz(siz1, spec);
    MemoryTarget tgt1;
    kdu_codestream cs1;
    cs1.create(&siz1, &tgt1);
    ConfigureCodestreamParams(cs1, spec);
    cs1.enable_restart();
    kdu_flush_stats fstats1 = cs1.add_flush_stats(0, 1, 0, 2);
    fstats1.auto_advance(2);

    auto encode_and_flush_cbr = [&](kdu_codestream& c) {
      kdu_tile tile = c.open_tile(kdu_coords(0, 0));
      PushTileData(c, tile, planes, spec);
      tile.close();
      kdu_long cbr_sizes[1] = {5000};
      kdu_uint16 slopes[1] = {0};
      c.flush(cbr_sizes, 1, slopes, true);
    };

    encode_and_flush_cbr(cs1);
    EXPECT_FALSE(tgt1.data.empty());

    // Restart cs1 (advances flush_stats) and also test aux_flush_stats on cs2.
    MemoryTarget tgt1b;
    cs1.restart(&tgt1b);
    encode_and_flush_cbr(cs1);

    siz_params siz2;
    ConfigureSiz(siz2, spec);
    MemoryTarget tgt2;
    kdu_codestream cs2;
    cs2.create(&siz2, &tgt2);
    ConfigureCodestreamParams(cs2, spec);
    (void)cs2.add_flush_stats(1, 1, 0, 2);
    cs2.attach_flush_stats(fstats1);
    encode_and_flush_cbr(cs2);

    cs2.destroy();
    cs1.destroy();

    MemorySource src_cbr(tgt2.data);
    EXPECT_FALSE(DecodeFullImage(&src_cbr).empty());
  }

  // Scbr validation error paths and Sprofile=PROFILE0 warnings.
  {
    ImageSpec spec;
    spec.width = 128;
    spec.height = 128;
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 2;
    const auto planes = MakeTestPlanes(spec);

    // Invalid Scbr={0,200}.
    ImageSpec bad_v = spec;
    bad_v.extra_params = {"Corder=PCRL", "Cprecincts={32,32}", "Scbr={0,200}"};
    EXPECT_THROW(EncodeImage(bad_v, planes), kdu_exception);

    // Scbr with horizontal tiling.
    ImageSpec htile = spec;
    htile.tile_size = kdu_coords(64, 128);
    htile.extra_params = {"Corder=PCRL", "Cprecincts={32,32}", "Scbr={1,200}"};
    EXPECT_THROW(EncodeImage(htile, planes), kdu_exception);

    // Sprofile=PROFILE0 warning on >1 tile not 128x128, non-zero origin, and
    // non-power-of-2 subsampling.
    {
      siz_params siz;
      siz.set(Sprofile, 0, 0, Sprofile_PROFILE0);
      siz.set(Scomponents, 0, 0, 1);
      siz.set(Sdims, 0, 0, 128);
      siz.set(Sdims, 0, 1, 128);
      siz.set(Stiles, 0, 0, 64);
      siz.set(Stiles, 0, 1, 64);
      siz.set(Sprecision, 0, 0, 8);
      siz.set(Ssigned, 0, 0, false);
      siz.finalize_all();
      MemoryTarget tgt;
      kdu_codestream cs;
      warning_handler_.clear();
      cs.create(&siz, &tgt);
      EXPECT_FALSE(warning_handler_.history().empty());
      cs.destroy();
    }
    {
      siz_params siz;
      siz.set(Sprofile, 0, 0, Sprofile_PROFILE0);
      siz.set(Scomponents, 0, 0, 1);
      siz.set(Sorigin, 0, 0, 4);
      siz.set(Sorigin, 0, 1, 4);
      siz.set(Sdims, 0, 0, 132);
      siz.set(Sdims, 0, 1, 132);
      siz.set(Sprecision, 0, 0, 8);
      siz.set(Ssigned, 0, 0, false);
      siz.finalize_all();
      MemoryTarget tgt;
      kdu_codestream cs;
      warning_handler_.clear();
      cs.create(&siz, &tgt);
      EXPECT_FALSE(warning_handler_.history().empty());
      cs.destroy();
    }
    {
      siz_params siz;
      siz.set(Sprofile, 0, 0, Sprofile_PROFILE0);
      siz.set(Scomponents, 0, 0, 1);
      siz.set(Sdims, 0, 0, 128);
      siz.set(Sdims, 0, 1, 128);
      siz.set(Ssampling, 0, 0, 3);
      siz.set(Ssampling, 0, 1, 3);
      siz.set(Sprecision, 0, 0, 8);
      siz.set(Ssigned, 0, 0, false);
      siz.finalize_all();
      MemoryTarget tgt;
      kdu_codestream cs;
      warning_handler_.clear();
      cs.create(&siz, &tgt);
      EXPECT_FALSE(warning_handler_.history().empty());
      cs.destroy();
    }
  }

  // 2. Unsized prefix layers in pcrd_opt & multi-layer trans_pcrd_opt +
  //    trans_out Creslengths warning.
  {
    ImageSpec spec;
    spec.width = 96;
    spec.height = 96;
    spec.components = 3;
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.extra_params = {"Clayers=3"};
    const auto planes = MakeTestPlanes(spec);

    auto encode_with_layer_bytes = [&](const kdu_long(&sizes)[3]) {
      siz_params siz;
      ConfigureSiz(siz, spec);
      MemoryTarget tgt;
      kdu_codestream cs;
      cs.create(&siz, &tgt);
      ConfigureCodestreamParams(cs, spec);
      kdu_tile t = cs.open_tile(kdu_coords(0, 0));
      PushTileData(cs, t, planes, spec);
      t.close();
      kdu_long local_sizes[3] = {sizes[0], sizes[1], sizes[2]};
      cs.flush(local_sizes, 3);
      cs.destroy();
      return tgt.data;
    };

    const auto bytes_mid0 = encode_with_layer_bytes({2000, 0, 8000});
    EXPECT_FALSE(bytes_mid0.empty());

    const auto bytes_lead0 = encode_with_layer_bytes({0, 0, 8000});
    EXPECT_FALSE(bytes_lead0.empty());

    // Multi-layer trans_pcrd_opt via load_sequential +
    // KDU_LOAD_SEQUENTIAL_WANT_PACKET_BYTES.
    const auto bytes_3l = encode_with_layer_bytes({1500, 3000, 6000});
    MemorySource src_3l(bytes_3l);
    kdu_codestream cs_in;
    cs_in.create(&src_3l);
    kdu_pdescriptor_set pset;
    bool tile_done = false;
    kdu_tile t = cs_in.load_sequential(
        &pset, tile_done, 1, 1,
        KDU_LOAD_SEQUENTIAL_WANT_TILE | KDU_LOAD_SEQUENTIAL_WANT_PACKET_BYTES,
        nullptr);
    ASSERT_TRUE(t.exists());
    EXPECT_THROW(cs_in.trans_pcrd_opt(&pset, true, true, 0.05, nullptr),
                 kdu_exception);
    t.close();

    // restart(input) switching to missing SOC triggers error.
    cs_in.destroy();
    {
      MemorySource src_r(bytes_3l);
      kdu_codestream cs_r;
      cs_r.create(&src_r);
      cs_r.enable_restart();
      TestMembackedSource mb_r(bytes_3l, 0);
      ASSERT_TRUE(mb_r.run_open(true));
      cs_r.restart(&mb_r);  // Non-MB -> MB creates in_notifier.
      std::vector<kdu_byte> bad_soc = {0xFF, 0x00, 0x00, 0x00};
      MemorySource bad_src(bad_soc);
      EXPECT_THROW(cs_r.restart(&bad_src), kdu_exception);
      cs_r.destroy();
    }

    // trans_out with Creslengths attribute on output triggers warning.
    ImageSpec spec_tr = spec;
    spec_tr.extra_params = {"Creslengths=512,512,512"};
    siz_params siz_tr;
    ConfigureSiz(siz_tr, spec_tr);
    MemoryTarget tgt_tr;
    kdu_codestream cs_tr;
    warning_handler_.clear();
    cs_tr.create(&siz_tr, &tgt_tr);
    ConfigureCodestreamParams(cs_tr, spec_tr);
    cs_tr.access_siz()->finalize_all();
    cs_tr.trans_out(-1, nullptr, 0, false);
    EXPECT_FALSE(warning_handler_.history().empty());
    cs_tr.destroy();
  }

  // 3. Multi-stage MCT + comps_of_interest output restrictions.
  {
    ImageSpec mct_spec;
    mct_spec.width = 64;
    mct_spec.height = 64;
    mct_spec.components = 3;
    mct_spec.reversible = false;
    mct_spec.dwt_levels = 1;
    mct_spec.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=3",
        "Msigned=no",
        "Mprecision=8",
        "Mmatrix_size:I1=9",
        "Mmatrix_coeffs:I1=1,0,0,0,1,0,0,0,1",
        "Mmatrix_size:I2=9",
        "Mmatrix_coeffs:I2=1,0,0,0,1,0,0,0,1",
        "Mstage_inputs:I1={0,2}",
        "Mstage_outputs:I1={0,2}",
        "Mstage_collections:I1={3,3}",
        "Mstage_xforms:I1={MATRIX,1,0,0,0}",
        "Mstage_inputs:I2={0,2}",
        "Mstage_outputs:I2={0,2}",
        "Mstage_collections:I2={3,3}",
        "Mstage_xforms:I2={MATRIX,2,0,0,0}",
        "Mnum_stages=2",
        "Mstages=1,2",
    };
    const auto mct_bytes = EncodeImage(mct_spec, MakeTestPlanes(mct_spec));
    MemorySource src_mct(mct_bytes);
    kdu_codestream cs_mct;
    cs_mct.create(&src_mct);
    int coi[2] = {0, 2};
    cs_mct.apply_input_restrictions(
        2, coi, 0, 0, nullptr, KDU_WANT_OUTPUT_COMPONENTS, nullptr, nullptr);
    EXPECT_EQ(cs_mct.get_num_components(true), 2);
    kdu_tile t = cs_mct.open_tile(kdu_coords(0, 0));
    t.close();
    cs_mct.destroy();
  }

  // 4. BlockingMembackedSource for UNSERVICED / PARTIALLY_SERVICED blocking
  //    notifications, max_mem cap, and destructor while in_service.
  {
    ImageSpec spec;
    spec.width = 160;
    spec.height = 160;
    spec.components = 3;
    spec.reversible = true;
    spec.dwt_levels = 2;
    const auto bytes = EncodeImage(spec, MakeTestPlanes(spec));
    ASSERT_GT(bytes.size(), 8192U);

    class BlockingMembackedSource : public kdu_membacked_compressed_source {
     public:
      using kdu_membacked_compressed_source::enable;
      using kdu_membacked_compressed_source::open;
      using kdu_membacked_compressed_source::service;

      BlockingMembackedSource(const std::vector<kdu_byte>& payload,
                              size_t chunk_bytes, int block_nonblocking_count)
          : payload_(payload),
            chunk_bytes_(chunk_bytes),
            block_nonblocking_count_(block_nonblocking_count) {}

     protected:
      kdu_long fetch_data(kdu_long max_bytes, kdu_byte* buf,
                          bool blocking) override {
        if (!blocking && block_nonblocking_count_ > 0) {
          --block_nonblocking_count_;
          return -1;
        }
        if (pos_ >= payload_.size()) return 0;
        size_t avail = payload_.size() - pos_;
        size_t take = avail;
        if (max_bytes > 0 && static_cast<size_t>(max_bytes) < take) {
          take = static_cast<size_t>(max_bytes);
        }
        if (chunk_bytes_ > 0 && chunk_bytes_ < take) {
          take = chunk_bytes_;
        }
        std::memcpy(buf, payload_.data() + pos_, take);
        pos_ += take;
        return static_cast<kdu_long>(take);
      }

     private:
      const std::vector<kdu_byte>& payload_;
      size_t chunk_bytes_;
      size_t pos_ = 0;
      int block_nonblocking_count_;
    };

    struct BlockingNotifier : public kdu_compressed_source_notifier {
      std::mutex mu;
      std::condition_variable cv;
      bool notified = false;
      int blocked_calls = 0;
      bool prepare_notify(kdu_long) override { return true; }
      void note_blocking() override { ++blocked_calls; }
      void notify() override {
        std::lock_guard<std::mutex> lk(mu);
        notified = true;
        cv.notify_all();
      }
      void wait_done() {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return notified; });
      }
    };

    // UNSERVICED with blocking note_blocking() and memory exhaustion max_mem.
    {
      BlockingMembackedSource u_block(bytes, 512, 1);
      ASSERT_TRUE(u_block.enable(KDU_MEMBACKED_SOURCE_UNSERVICED, nullptr, 0, 0,
                                 -1.0, 256, 512));
      ASSERT_TRUE(u_block.open(false));
      BlockingNotifier n;
      u_block.load_memory(4096, true, &n);
      EXPECT_GE(n.blocked_calls, 1);
      EXPECT_TRUE(u_block.close());
    }

    // PARTIALLY_SERVICED with non-blocking WILL_NOTIFY + blocking
    // note_blocking.
    {
      BlockingMembackedSource p_block(bytes, 512, 2);
      ASSERT_TRUE(p_block.enable(KDU_MEMBACKED_SOURCE_PARTIALLY_SERVICED,
                                 nullptr, 0, 0, -1.0, 1024,
                                 bytes.size() + 4096));
      std::atomic<bool> svc_started{false};
      size_t fetched = 0;
      kdu_int32 term_code = 0;
      std::thread svc([&]() {
        svc_started.store(true);
        p_block.service(fetched, term_code);
      });
      while (!svc_started.load()) {
        std::this_thread::yield();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
      ASSERT_TRUE(p_block.open(false));
      BlockingNotifier n1;
      const int f1 = p_block.load_memory(4096, false, &n1);
      if (f1 & KDU_SOURCE_WILL_NOTIFY) {
        n1.wait_done();
      }
      BlockingNotifier n2;
      p_block.load_memory(bytes.size() + 1, true, &n2);
      EXPECT_TRUE(p_block.close());
      svc.join();
    }
  }

  // 5. Tight kdu_membroker code-buffer exhaustion and multi-tile block pool
  //    fragmentation + TLM clamping (>255 tparts / >256 tiles).
  {
    ImageSpec small_tlm;
    small_tlm.width = 170;
    small_tlm.height = 170;
    small_tlm.tile_size = kdu_coords(10, 10);  // 289 tiles
    small_tlm.components = 1;
    small_tlm.dwt_levels = 0;
    small_tlm.extra_params = {"ORGgen_tlm=300"};  // clamped >255, exceeds 256
    EXPECT_FALSE(EncodeImage(small_tlm, MakeTestPlanes(small_tlm)).empty());

    // Code-buffer fragmentation: 256x256, 3 comps, small 16x16 codeblocks, open
    // tiles in non-persistent decode / flush cycle to release >64 code buffers.
    ImageSpec frag_spec;
    frag_spec.width = 256;
    frag_spec.height = 256;
    frag_spec.tile_size = kdu_coords(64, 64);
    frag_spec.components = 3;
    frag_spec.reversible = true;
    frag_spec.dwt_levels = 2;
    frag_spec.extra_params = {"Cblk={16,16}", "Cprecincts={32,32}"};
    const auto frag_bytes = EncodeImage(frag_spec, MakeTestPlanes(frag_spec));
    MemorySource src_frag(frag_bytes);
    kdu_codestream cs_frag;
    cs_frag.create(&src_frag);
    cs_frag.set_persistent();
    for (int ty = 0; ty < 4; ++ty) {
      for (int tx = 0; tx < 4; ++tx) {
        kdu_tile t = cs_frag.open_tile(kdu_coords(tx, ty));
        t.close(nullptr, false);  // Non-persistent close releases code buffers
      }
    }
    cs_frag.destroy();

    // Tight membroker failing mid-encode.
    kdu_membroker tiny_broker(32768);
    siz_params siz;
    ConfigureSiz(siz, frag_spec);
    MemoryTarget tgt;
    kdu_codestream cs_tight;
    try {
      cs_tight.create(&siz, &tgt, nullptr, 0, 0.0, nullptr, &tiny_broker);
      const auto planes = MakeTestPlanes(frag_spec);
      kdu_tile t = cs_tight.open_tile(kdu_coords(0, 0));
      PushTileData(cs_tight, t, planes, frag_spec);
      t.close();
      cs_tight.destroy();
    } catch (...) {
      if (cs_tight.exists()) cs_tight.destroy();
    }
  }
}

struct ScopedCodestream {
  kdu_codestream cs;
  ~ScopedCodestream() {
    if (cs.exists()) cs.destroy();
  }
};

TEST_F(CodestreamTest, SingleThreadedProfileStreamTrimAndMctPaths) {
  // 1. Sprofile=PROFILE1 warnings: non-square tiles and tiles > 1024.
  {
    siz_params siz;
    siz.set(Sprofile, 0, 0, Sprofile_PROFILE1);
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sdims, 0, 0, 128);
    siz.set(Sdims, 0, 1, 128);
    siz.set(Stiles, 0, 0, 32);
    siz.set(Stiles, 0, 1, 64);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    siz.finalize_all();
    MemoryTarget tgt;
    ScopedCodestream sc;
    warning_handler_.clear();
    sc.cs.create(&siz, &tgt);
    EXPECT_FALSE(warning_handler_.history().empty());
  }
  {
    siz_params siz;
    siz.set(Sprofile, 0, 0, Sprofile_PROFILE1);
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sdims, 0, 0, 2050);
    siz.set(Sdims, 0, 1, 2050);
    siz.set(Stiles, 0, 0, 1025);
    siz.set(Stiles, 0, 1, 1025);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    siz.finalize_all();
    MemoryTarget tgt;
    ScopedCodestream sc;
    warning_handler_.clear();
    sc.cs.create(&siz, &tgt);
    EXPECT_FALSE(warning_handler_.history().empty());
  }

  // 2. Non-memory-backed seekable StreamSeekSource covering
  // kd_compressed_input::load_buf, seek, kd_input::read/ignore, and
  // load_sequential skipping dead tiles + partial flush-set errors.
  {
    class StreamSeekSource : public kdu_compressed_source {
     public:
      StreamSeekSource(std::vector<kdu_byte> data, size_t max_chunk)
          : data_(std::move(data)), max_chunk_(max_chunk) {}
      int get_capabilities() override {
        return KDU_SOURCE_CAP_SEEKABLE | KDU_SOURCE_CAP_SEQUENTIAL;
      }
      int read(kdu_byte* buf, int num_bytes) override {
        if (pos_ >= data_.size() || num_bytes <= 0) return 0;
        const size_t avail = data_.size() - pos_;
        const size_t n =
            std::min({static_cast<size_t>(num_bytes), avail, max_chunk_});
        std::memcpy(buf, data_.data() + pos_, n);
        pos_ += n;
        return static_cast<int>(n);
      }
      bool seek(kdu_long offset) override {
        if (offset < 0 || static_cast<size_t>(offset) > data_.size()) {
          return false;
        }
        pos_ = static_cast<size_t>(offset);
        return true;
      }

     private:
      std::vector<kdu_byte> data_;
      size_t max_chunk_;
      size_t pos_ = 0;
    };

    ImageSpec spec;
    spec.width = 192;
    spec.height = 192;
    spec.tile_size = kdu_coords(64, 64);  // 3x3 = 9 tiles
    spec.components = 3;
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.extra_params = {"Clayers=2", "ORGgen_plt=yes", "ORGtparts=R",
                         "Cprecincts={32,32}"};
    const auto bytes = EncodeImage(spec, MakeTestPlanes(spec));
    ASSERT_GT(bytes.size(), 9000U);

    // Random tile access forcing forward/backward seeks and multi-buf reads.
    {
      StreamSeekSource ssrc(bytes, 47);
      ScopedCodestream sc;
      sc.cs.create(&ssrc);
      sc.cs.set_persistent();
      kdu_dims roi22;
      roi22.pos = kdu_coords(128, 128);
      roi22.size = kdu_coords(64, 64);
      sc.cs.apply_input_restrictions(0, 0, 0, 0, &roi22);
      kdu_tile t22 = sc.cs.open_tile(kdu_coords(2, 2));
      t22.close();
      kdu_dims roi00;
      roi00.pos = kdu_coords(0, 0);
      roi00.size = kdu_coords(64, 64);
      sc.cs.apply_input_restrictions(0, 0, 0, 0, &roi00);
      kdu_tile t00 = sc.cs.open_tile(kdu_coords(0, 0));
      t00.close();
    }

    // load_sequential skipping dead tiles via region restriction.
    {
      StreamSeekSource ssrc(bytes, 47);
      ScopedCodestream sc;
      sc.cs.create(&ssrc);
      kdu_dims roi22;
      roi22.pos = kdu_coords(128, 128);
      roi22.size = kdu_coords(64, 64);
      sc.cs.apply_input_restrictions(0, 0, 0, 0, &roi22);
      int loaded_tiles = 0;
      for (;;) {
        kdu_pdescriptor_set pset;
        bool tile_done = false;
        kdu_tile t = sc.cs.load_sequential(
            &pset, tile_done, 1, 1, KDU_LOAD_SEQUENTIAL_WANT_TILE, nullptr);
        if (!t.exists()) break;
        ++loaded_tiles;
        t.close();
      }
      EXPECT_EQ(loaded_tiles, 1);
    }

    // Corrupt SOT tile index during load_sequential throws 0x15082102.
    {
      std::vector<kdu_byte> bad_sot = bytes;
      for (size_t i = 2; i + 12 <= bad_sot.size(); ++i) {
        if (bad_sot[i] == 0xFF && bad_sot[i + 1] == 0x90) {
          bad_sot[i + 4] = 0x09;
          bad_sot[i + 5] = 0x99;
          break;
        }
      }
      StreamSeekSource ssrc(bad_sot, 64);
      ScopedCodestream sc;
      sc.cs.create(&ssrc);
      kdu_pdescriptor_set pset;
      bool tile_done = false;
      EXPECT_THROW(
          sc.cs.load_sequential(&pset, tile_done, 1, 1,
                                KDU_LOAD_SEQUENTIAL_WANT_TILE, nullptr),
          kdu_exception);
    }

    // Partial flush-set stopping mid-tile + layer-tparts incomplete pset error.
    {
      MemorySource msrc(bytes);
      ScopedCodestream sc;
      sc.cs.create(&msrc);
      kdu_pdescriptor_set pset1;
      bool tile_done = false;
      kdu_tile t = sc.cs.load_sequential(
          &pset1, tile_done, 1, 1, KDU_LOAD_SEQUENTIAL_WANT_FLUSH_SET, nullptr);
      ASSERT_TRUE(t.exists());
      t.close();

      ImageSpec lspec;
      lspec.width = 64;
      lspec.height = 64;
      lspec.components = 1;
      lspec.reversible = true;
      lspec.dwt_levels = 0;
      lspec.extra_params = {"Clayers=2", "Corder=LRCP", "ORGtparts=L"};
      const auto lbytes = EncodeImage(lspec, MakeTestPlanes(lspec));
      MemorySource lsrc(lbytes);
      ScopedCodestream lsc;
      lsc.cs.create(&lsrc);
      kdu_pdescriptor_set pset_a;
      bool ldone = false;
      // Call 1 returns the partial tile (layer 0), call 2 finishes layer 1.
      EXPECT_TRUE(
          lsc.cs.load_sequential(&pset_a, ldone, 0, 0, 0, nullptr).exists());
      EXPECT_FALSE(ldone);
      lsc.cs.load_sequential(&pset_a, ldone, 1, 1, 0, nullptr);
      EXPECT_TRUE(ldone);
    }
  }

  // 3. kd_precinct_pointer_server::pop_address error paths on mutated PLT.
  {
    ImageSpec spec;
    spec.width = 128;
    spec.height = 128;
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.extra_params = {"Corder=PCRL", "Cprecincts={32,32}", "Clayers=2",
                         "ORGgen_plt=yes"};
    const auto bytes = EncodeImage(spec, MakeTestPlanes(spec));
    size_t plt_off = 0;
    for (size_t i = 2; i + 8 < bytes.size(); ++i) {
      if (bytes[i] == 0xFF && bytes[i + 1] == 0x58) {
        plt_off = i;
        break;
      }
    }
    ASSERT_GT(plt_off, 0U);
    const kdu_uint16 plt_len =
        (static_cast<kdu_uint16>(bytes[plt_off + 2]) << 8) | bytes[plt_off + 3];
    ASSERT_GE(plt_len, 6U);

    // Overflow entry > tpart_bytes_left -> KDU_ERROR(23).
    {
      std::vector<kdu_byte> bad_plt = bytes;
      bad_plt[plt_off + 5] = 0xFF;
      bad_plt[plt_off + 6] = 0x7F;
      MemorySource src(bad_plt, KDU_SOURCE_CAP_SEEKABLE);
      EXPECT_THROW(DecodeFullImage(&src), kdu_exception);
    }

    // Keep the first PLT entry intact so `something_served` becomes true, then
    // collapse the remaining entries into a single small varint so PLT runs
    // out of addresses before `tpart_bytes_left == 0` -> KDU_ERROR(22).
    {
      std::vector<kdu_byte> short_plt = bytes;
      const size_t entry_end = plt_off + 2 + plt_len;
      size_t p = plt_off + 5;
      while (p + 1 < entry_end && (short_plt[p] & 0x80)) {
        ++p;
      }
      ++p;
      if (p + 2 <= entry_end) {
        for (; p + 1 < entry_end; ++p) {
          short_plt[p] = 0x80;
        }
        short_plt[entry_end - 1] = 0x01;
        MemorySource src(short_plt, KDU_SOURCE_CAP_SEEKABLE);
        EXPECT_THROW(DecodeFullImage(&src), kdu_exception);
      }
    }
  }

  // 4. kd_packet_sequencer::next_progression (multi-POC, PROFILE0 POC
  //    warning, bounds clamping, non-power-of-2 subsampling with PCRL) +
  //    pcrd_trim non-monotonic slope clamping + simulate_output early exit +
  //    Creslengths:T0.
  {
    // Sprofile=PROFILE0 with Porder res_min=1 warning.
    {
      siz_params siz;
      siz.set(Sprofile, 0, 0, Sprofile_PROFILE0);
      siz.set(Scomponents, 0, 0, 3);
      siz.set(Sdims, 0, 0, 64);
      siz.set(Sdims, 0, 1, 64);
      siz.set(Sprecision, 0, 0, 8);
      siz.set(Ssigned, 0, 0, false);
      siz.finalize_all();
      MemoryTarget tgt;
      ScopedCodestream sc;
      warning_handler_.clear();
      sc.cs.create(&siz, &tgt);
      ASSERT_TRUE(sc.cs.access_siz()->parse_string("Clayers=2"));
      ASSERT_TRUE(sc.cs.access_siz()->parse_string("Clevels=2"));
      ASSERT_TRUE(sc.cs.access_siz()->parse_string("Porder={1,0,5,5,5,LRCP}"));
      ASSERT_TRUE(sc.cs.access_siz()->parse_string("ORGtparts=R"));
      sc.cs.access_siz()->finalize_all();
      ImageSpec s;
      s.width = 64;
      s.height = 64;
      s.components = 3;
      s.dwt_levels = 2;
      const auto planes = MakeTestPlanes(s);
      auto run_poc = [&]() {
        kdu_tile t = sc.cs.open_tile(kdu_coords(0, 0));
        PushTileData(sc.cs, t, planes, s);
        t.close();
        kdu_long fl[2] = {0, 0};
        sc.cs.flush(fl, 2);
      };
      EXPECT_THROW(run_poc(), kdu_exception);
      EXPECT_FALSE(warning_handler_.history().empty());

      // Bounds-clamped multi-record POC succeeding cleanly.
      MemoryTarget tgt_ok;
      ScopedCodestream sc_ok;
      sc_ok.cs.create(&siz, &tgt_ok);
      ASSERT_TRUE(sc_ok.cs.access_siz()->parse_string("Clayers=2"));
      ASSERT_TRUE(sc_ok.cs.access_siz()->parse_string("Clevels=2"));
      ASSERT_TRUE(sc_ok.cs.access_siz()->parse_string(
          "Porder={0,0,5,2,5,LRCP},{0,0,2,3,5,RLCP}"));
      ASSERT_TRUE(sc_ok.cs.access_siz()->parse_string("ORGtparts=R"));
      sc_ok.cs.access_siz()->finalize_all();
      kdu_tile tok = sc_ok.cs.open_tile(kdu_coords(0, 0));
      PushTileData(sc_ok.cs, tok, planes, s);
      tok.close();
      kdu_long fl_ok[2] = {0, 0};
      sc_ok.cs.flush(fl_ok, 2);
      EXPECT_FALSE(tgt_ok.data.empty());
    }

    // Non-power-of-2 subsampling with Corder=PCRL throws error 25.
    {
      ImageSpec s;
      s.width = 64;
      s.height = 64;
      s.components = 1;
      s.dwt_levels = 1;
      s.subsampling = {kdu_coords(3, 3)};
      s.extra_params = {"Corder=PCRL"};
      siz_params siz;
      ConfigureSiz(siz, s);
      MemoryTarget tgt;
      ScopedCodestream sc;
      sc.cs.create(&siz, &tgt);
      ConfigureCodestreamParams(sc.cs, s);
      EXPECT_THROW(
          {
            kdu_tile t = sc.cs.open_tile(kdu_coords(0, 0));
            t.close();
          },
          kdu_exception);
    }

    // pcrd_trim with non-decreasing thresholds + Creslengths:T0, and
    // simulate_output early exit when layer budget < main/tile header size.
    {
      ImageSpec s;
      s.width = 64;
      s.height = 64;
      s.components = 1;
      s.reversible = true;
      s.dwt_levels = 2;
      s.extra_params = {"Clayers=3", "Creslengths:T0=128,256,512"};
      const auto planes = MakeTestPlanes(s);

      siz_params siz;
      ConfigureSiz(siz, s);
      MemoryTarget tgt;
      ScopedCodestream sc;
      sc.cs.create(&siz, &tgt);
      ConfigureCodestreamParams(sc.cs, s);
      kdu_tile t = sc.cs.open_tile(kdu_coords(0, 0));
      PushTileData(sc.cs, t, planes, s);
      t.close();
      kdu_long min_sizes[3] = {300, 600, 1200};
      kdu_uint16 non_dec_thresh[3] = {25000, 35000, 45000};
      sc.cs.flush(min_sizes, 3, non_dec_thresh, true, true, 0.0, nullptr,
                  KDU_FLUSH_USES_THRESHOLDS_AND_SIZES);
      EXPECT_FALSE(tgt.data.empty());
    }
    {
      ImageSpec s;
      s.width = 64;
      s.height = 64;
      s.components = 1;
      s.reversible = true;
      s.dwt_levels = 2;
      s.extra_params = {"Clayers=2"};
      const auto planes = MakeTestPlanes(s);
      siz_params siz;
      ConfigureSiz(siz, s);
      MemoryTarget tgt;
      ScopedCodestream sc;
      sc.cs.create(&siz, &tgt);
      ConfigureCodestreamParams(sc.cs, s);
      kdu_tile t = sc.cs.open_tile(kdu_coords(0, 0));
      PushTileData(sc.cs, t, planes, s);
      t.close();
      kdu_long tiny_sizes[2] = {4, 500};
      sc.cs.flush(tiny_sizes, 2);
      EXPECT_FALSE(tgt.data.empty());
    }
  }

  // 5. construct_cbr_flush_set validation errors + write_packets_cbr trailing
  // empty precincts.
  {
    ImageSpec base;
    base.width = 128;
    base.height = 128;
    base.components = 2;
    base.reversible = true;
    base.dwt_levels = 1;
    auto planes = MakeTestPlanes(base);
    std::fill(planes[1].begin(), planes[1].end(),
              0);  // Empty comp-1 precincts.

    auto try_cbr = [&](const std::vector<std::string>& extra, kdu_long b0) {
      ImageSpec sp = base;
      sp.extra_params = extra;
      siz_params siz;
      ConfigureSiz(siz, sp);
      MemoryTarget tgt;
      ScopedCodestream sc;
      sc.cs.create(&siz, &tgt);
      ConfigureCodestreamParams(sc.cs, sp);
      kdu_tile t = sc.cs.open_tile(kdu_coords(0, 0));
      PushTileData(sc.cs, t, planes, sp);
      t.close();
      kdu_long sizes[2] = {b0, b0 * 2};
      kdu_uint16 slopes[2] = {0, 0};
      int nlayers =
          (std::find(extra.begin(), extra.end(), "Clayers=2") != extra.end())
              ? 2
              : 1;
      sc.cs.flush(sizes, nlayers, slopes, true);
    };

    EXPECT_THROW(try_cbr({"Clayers=2", "Corder=PCRL", "Cprecincts={32,32}",
                          "Scbr={1,200}"},
                         4000),
                 kdu_exception);
    EXPECT_THROW(
        try_cbr({"Corder=LRCP", "Cprecincts={32,32}", "Scbr={1,200}"}, 4000),
        kdu_exception);
    EXPECT_THROW(try_cbr({"Corder=PCRL", "Cprecincts={32,32}", "ORGgen_plt=yes",
                          "Scbr={1,200}"},
                         4000),
                 kdu_exception);
    EXPECT_THROW(
        try_cbr({"Corder=PCRL", "Cprecincts={32,32}", "Scbr={1,1}"}, 4000),
        kdu_exception);
    // Valid CBR with trailing zeroed component precincts.
    EXPECT_NO_THROW(
        try_cbr({"Corder=PCRL", "Cprecincts={32,32}", "Scbr={1,200}"}, 5000));
  }

  // 6. Sextensions=MCT|NLT main-header CBD scan + DWT MCT subsetting +
  // change_appearance on subsampled stream (construct_output_comp_info).
  {
    ImageSpec mct_dwt;
    mct_dwt.width = 64;
    mct_dwt.height = 64;
    mct_dwt.components = 4;
    mct_dwt.reversible = false;
    mct_dwt.dwt_levels = 1;
    mct_dwt.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=4",
        "Msigned=no",
        "Mprecision=8",
        "Mstage_inputs:I1={0,3}",
        "Mstage_outputs:I1={0,3}",
        "Mstage_collections:I1={4,4}",
        "Mstage_xforms:I1={DWT,0,0,2,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    const auto bytes = EncodeImage(mct_dwt, MakeTestPlanes(mct_dwt));
    MemorySource src(bytes, KDU_SOURCE_CAP_SEEKABLE);
    ScopedCodestream sc;
    sc.cs.create(&src);
    int coi[2] = {0, 3};
    sc.cs.apply_input_restrictions(2, coi, 0, 0, nullptr,
                                   KDU_WANT_OUTPUT_COMPONENTS);
    EXPECT_EQ(sc.cs.get_num_components(true), 2);
    kdu_tile t = sc.cs.open_tile(kdu_coords(0, 0));
    t.close();

    MemorySource src_null(bytes, KDU_SOURCE_CAP_SEEKABLE);
    ScopedCodestream sc_null;
    sc_null.cs.create(&src_null);
    sc_null.cs.apply_input_restrictions(0, 2, 0, 0, nullptr,
                                        KDU_WANT_OUTPUT_COMPONENTS);
    EXPECT_EQ(sc_null.cs.get_num_components(true), 2);
    kdu_tile tn = sc_null.cs.open_tile(kdu_coords(0, 0));
    tn.close();

    // Change appearance with transpose/vflip/hflip on non-MCT subsampled image.
    ImageSpec sub;
    sub.width = 64;
    sub.height = 64;
    sub.components = 3;
    sub.dwt_levels = 1;
    sub.subsampling = {kdu_coords(1, 1), kdu_coords(2, 2), kdu_coords(2, 2)};
    const auto sub_bytes = EncodeImage(sub, MakeTestPlanes(sub));
    MemorySource ssrc(sub_bytes);
    ScopedCodestream sc2;
    sc2.cs.create(&ssrc);
    sc2.cs.change_appearance(true, true, true);
    kdu_tile st = sc2.cs.open_tile(kdu_coords(0, 0));
    st.close();
  }
}

TEST_F(CodestreamTest, SingleThreadedCommentTlmSizMctInputAndErrorPaths) {
  // 1. kd_codestream_comment put_data edge cases, readonly check, and TLM.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.tile_size = kdu_coords(32, 32);  // 2x2 tiles
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.extra_params = {"ORGgen_tlm=1", "ORGtlm_style={implied,short}"};
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    ScopedCodestream sc;
    sc.cs.create(&siz, &tgt);
    sc.cs.set_disabled_auto_comments(0);
    ConfigureCodestreamParams(sc.cs, spec);

    kdu_codestream_comment c_bin = sc.cs.add_comment();
    ASSERT_TRUE(c_bin.exists());
    std::vector<kdu_byte> big(70000, 0x41);
    EXPECT_TRUE(c_bin.put_data(big.data(), 0));
    EXPECT_TRUE(c_bin.put_data(big.data(), static_cast<int>(big.size())));
    kdu_codestream_comment c_txt = sc.cs.add_comment();
    ASSERT_TRUE(c_txt.exists());
    EXPECT_TRUE(c_txt.put_text(""));
    EXPECT_FALSE(c_bin.check_readonly());

    const auto planes = MakeTestPlanes(spec);
    for (int ty = 0; ty < 2; ++ty) {
      for (int tx = 0; tx < 2; ++tx) {
        kdu_tile t = sc.cs.open_tile(kdu_coords(tx, ty));
        PushTileData(sc.cs, t, planes, spec);
        t.close();
      }
    }
    kdu_long fl = 0;
    sc.cs.flush(&fl, 1);
    EXPECT_TRUE(c_bin.check_readonly());
    EXPECT_FALSE(tgt.data.empty());
  }

  // Non-rewritable target with ORGgen_tlm emits warning 17; invalid TLM throws.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 68;
    spec.tile_size = kdu_coords(4, 4);  // 16x17 = 272 tiles
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 0;
    const auto planes = MakeTestPlanes(spec);

    {
      ImageSpec s_warn = spec;
      s_warn.tile_size = kdu_coords(32, 34);
      s_warn.extra_params = {"ORGgen_tlm=1"};
      siz_params siz;
      ConfigureSiz(siz, s_warn);
      MemoryTarget tgt;
      tgt.allow_rewrite = false;
      ScopedCodestream sc;
      warning_handler_.clear();
      sc.cs.create(&siz, &tgt);
      ConfigureCodestreamParams(sc.cs, s_warn);
      for (int ty = 0; ty < 2; ++ty) {
        for (int tx = 0; tx < 2; ++tx) {
          kdu_tile t = sc.cs.open_tile(kdu_coords(tx, ty));
          PushTileData(sc.cs, t, planes, s_warn);
          t.close();
        }
      }
      kdu_long fl = 0;
      sc.cs.flush(&fl, 1);
      EXPECT_FALSE(warning_handler_.history().empty());
    }

    auto try_tlm_err = [&](const std::vector<std::string>& extra) {
      ImageSpec se = spec;
      se.extra_params = extra;
      siz_params siz;
      ConfigureSiz(siz, se);
      MemoryTarget tgt;
      ScopedCodestream sc;
      sc.cs.create(&siz, &tgt);
      ConfigureCodestreamParams(sc.cs, se);
      for (int ty = 0; ty < 17; ++ty) {
        for (int tx = 0; tx < 16; ++tx) {
          kdu_tile t = sc.cs.open_tile(kdu_coords(tx, ty));
          PushTileData(sc.cs, t, planes, se);
          t.close();
        }
      }
      kdu_long fl = 0;
      sc.cs.flush(&fl, 1);
    };
    EXPECT_THROW(try_tlm_err({"ORGgen_tlm=2", "ORGtlm_style={implied,short}"}),
                 kdu_exception);
    EXPECT_THROW(try_tlm_err({"ORGgen_tlm=1", "ORGtlm_style={byte,short}"}),
                 kdu_exception);
  }

  // 2. construct_common SIZ parameter validation errors.
  {
    auto try_siz = [&](const std::function<void(siz_params&)>& fill) {
      siz_params siz;
      fill(siz);
      MemoryTarget tgt;
      ScopedCodestream sc;
      sc.cs.create(&siz, &tgt);
    };
    EXPECT_THROW(try_siz([](siz_params& s) {
                   s.set(Scomponents, 0, 0, 1);
                   s.set(Sdims, 0, 0, 16);
                   s.set(Sdims, 0, 1, 16);
                 }),
                 kdu_exception);
    EXPECT_THROW(try_siz([](siz_params& s) {
                   s.set(Scomponents, 0, 0, 1);
                   s.set(Sdims, 0, 0, 16);
                   s.set(Sdims, 0, 1, 16);
                   s.set(Sprecision, 0, 0, 8);
                 }),
                 kdu_exception);
    EXPECT_THROW(try_siz([](siz_params& s) {
                   s.set(Scomponents, 0, 0, 16385);
                   s.set(Sdims, 0, 0, 16);
                   s.set(Sdims, 0, 1, 16);
                   s.set(Sprecision, 0, 0, 8);
                   s.set(Ssigned, 0, 0, false);
                   s.finalize_all();
                 }),
                 kdu_exception);
    EXPECT_THROW(try_siz([](siz_params& s) {
                   s.set(Scomponents, 0, 0, 1);
                   s.set(Sdims, 0, 0, 64);
                   s.set(Sdims, 0, 1, 64);
                   s.set(Sorigin, 0, 0, 32);
                   s.set(Sorigin, 0, 1, 32);
                   s.set(Stile_origin, 0, 0, 0);
                   s.set(Stile_origin, 0, 1, 0);
                   s.set(Stiles, 0, 0, 16);
                   s.set(Stiles, 0, 1, 16);
                   s.set(Sprecision, 0, 0, 8);
                   s.set(Ssigned, 0, 0, false);
                   s.finalize_all();
                 }),
                 kdu_exception);
  }

  // 3. Multi-stage MCT augmenting previous stage outputs + output restriction
  // + MCT parameter error cases.
  {
    auto try_mct_err = [&](int components,
                           const std::vector<std::string>& extra) {
      ImageSpec spec;
      spec.width = 64;
      spec.height = 64;
      spec.components = components;
      spec.reversible = false;
      spec.dwt_levels = 1;
      spec.extra_params = extra;
      siz_params siz;
      ConfigureSiz(siz, spec);
      MemoryTarget tgt;
      ScopedCodestream sc;
      sc.cs.create(&siz, &tgt);
      ConfigureCodestreamParams(sc.cs, spec);
      kdu_tile t = sc.cs.open_tile(kdu_coords(0, 0));
      t.close();
    };
    EXPECT_THROW(
        try_mct_err(
            4,
            {"Sextensions=MCT", "Mcomponents=4", "Msigned=no", "Mprecision=8",
             "Mnum_stages=2", "Mstages=1,2", "Mstage_inputs:I1={0,1}",
             "Mstage_outputs:I1={0,0}", "Mstage_collections:I1={2,1}",
             "Mstage_xforms:I1={MATRIX,0,0,0,0}", "Mmatrix_size:I1=2",
             "Mmatrix_coeffs:I1=0.5,0.5", "Mstage_inputs:I2={0,2}",
             "Mstage_outputs:I2={0,2}", "Mstage_collections:I2={3,3}",
             "Mstage_xforms:I2={MATRIX,1,0,0,0}", "Mmatrix_size:I2=9",
             "Mmatrix_coeffs:I2=1,0,0,0,1,0,0,0,1"}),
        kdu_exception);
    EXPECT_THROW(
        try_mct_err(
            3,
            {"Sextensions=MCT", "Mcomponents=3", "Msigned=no", "Mprecision=8",
             "Mnum_stages=2", "Mstages=1,2", "Mstage_inputs:I1={0,2}",
             "Mstage_outputs:I1={0,0}", "Mstage_collections:I1={3,1}",
             "Mstage_xforms:I1={MATRIX,0,0,0,0}", "Mmatrix_size:I1=3",
             "Mmatrix_coeffs:I1=1,0,0", "Mstage_inputs:I2={0,2}",
             "Mstage_outputs:I2={0,2}", "Mstage_collections:I2={3,3}",
             "Mstage_xforms:I2={MATRIX,1,0,0,0}", "Mmatrix_size:I2=9",
             "Mmatrix_coeffs:I2=1,0,0,0,1,0,0,0,1"}),
        kdu_exception);

    // Valid 2-stage MCT with 3->5 expansion + output component restriction.
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 3;
    spec.reversible = false;
    spec.dwt_levels = 1;
    spec.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=5",
        "Msigned=no",
        "Mprecision=8",
        "Mnum_stages=2",
        "Mstages=1,2",
        "Mstage_inputs:I1={0,2}",
        "Mstage_outputs:I1={0,2}",
        "Mstage_collections:I1={3,3}",
        "Mstage_xforms:I1={MATRIX,1,0,0,0}",
        "Mmatrix_size:I1=9",
        "Mmatrix_coeffs:I1=1,0,0,0,1,0,0,0,1",
        "Mstage_inputs:I2={0,2}",
        "Mstage_outputs:I2={0,4}",
        "Mstage_collections:I2={3,5}",
        "Mstage_xforms:I2={MATRIX,0,0,0,0}",
        "Mmatrix_size:I2=15",
        "Mmatrix_coeffs:I2=1,0,0,0,1,0,0,0,1,1,0,0,0,1,0",
    };
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);
    MemorySource src(bytes);
    ScopedCodestream dcs;
    dcs.cs.create(&src);
    int coi[2] = {3, 4};
    dcs.cs.apply_input_restrictions(2, coi, 0, 0, nullptr,
                                    KDU_WANT_OUTPUT_COMPONENTS);
    EXPECT_EQ(dcs.cs.get_num_components(true), 2);
    kdu_tile dt = dcs.cs.open_tile(kdu_coords(0, 0));
    dt.close();
  }

  // 4. pcrd_opt unsized prefix + tolerance + pcrd_trim Creslengths warning +
  // multi-tile CBR.
  {
    ImageSpec spec;
    spec.width = 128;
    spec.height = 64;
    spec.tile_size = kdu_coords(64, 64);
    spec.components = 3;
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.extra_params = {"Clayers=4"};
    const auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    ScopedCodestream sc;
    sc.cs.create(&siz, &tgt);
    ConfigureCodestreamParams(sc.cs, spec);
    for (int tx = 0; tx < 2; ++tx) {
      kdu_tile t = sc.cs.open_tile(kdu_coords(tx, 0));
      PushTileData(sc.cs, t, planes, spec);
      t.close();
    }
    kdu_long fl4[4] = {0, 0, 0, 2400};
    sc.cs.flush(fl4, 4, nullptr, true, true, 0.05);
    EXPECT_FALSE(tgt.data.empty());
  }
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.reversible = true;
    spec.dwt_levels = 2;
    spec.extra_params = {"Clayers=3", "Creslengths:T0=1,1,1"};
    const auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    ScopedCodestream sc;
    warning_handler_.clear();
    sc.cs.create(&siz, &tgt);
    ConfigureCodestreamParams(sc.cs, spec);
    kdu_tile t = sc.cs.open_tile(kdu_coords(0, 0));
    PushTileData(sc.cs, t, planes, spec);
    t.close();
    kdu_long min_sizes[3] = {2000, 3000, 4000};
    kdu_uint16 thresh[3] = {50000, 40000, 30000};
    sc.cs.flush(min_sizes, 3, thresh, true, true, 0.0, nullptr,
                KDU_FLUSH_USES_THRESHOLDS_AND_SIZES);
    EXPECT_FALSE(tgt.data.empty());
  }
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 128;
    spec.tile_size = kdu_coords(64, 64);
    spec.components = 2;
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.extra_params = {"Corder=PCRL", "Cprecincts={32,32}", "Scbr={1,200}"};
    const auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    ScopedCodestream sc;
    sc.cs.create(&siz, &tgt);
    ConfigureCodestreamParams(sc.cs, spec);
    for (int ty = 0; ty < 2; ++ty) {
      kdu_tile t = sc.cs.open_tile(kdu_coords(0, ty));
      PushTileData(sc.cs, t, planes, spec);
      t.close();
    }
    kdu_long cbr_sizes[1] = {4000};
    kdu_uint16 slopes[1] = {0};
    sc.cs.flush(cbr_sizes, 1, slopes, true);
    EXPECT_FALSE(tgt.data.empty());
  }

  // 5. Large (>KD_IBUF_SIZE) pseudo-random image for multi-buffer StreamSource,
  // set_max_bytes truncation, and KDU_PPM read_main_header checks.
  {
    ImageSpec big;
    big.width = 256;
    big.height = 256;
    big.tile_size = kdu_coords(128, 128);
    big.components = 3;
    big.reversible = true;
    big.dwt_levels = 2;
    big.extra_params = {"Sprofile=PROFILE0", "ORGgen_plt=yes"};
    std::vector<std::vector<kdu_int32>> planes(3);
    kdu_uint32 rng = 0x12345678u;
    for (int c = 0; c < 3; ++c) {
      planes[c].resize(256 * 256);
      for (size_t i = 0; i < planes[c].size(); ++i) {
        rng = rng * 1664525u + 1013904223u;
        planes[c][i] = static_cast<kdu_int32>((rng >> 24) & 0xFF);
      }
    }
    const auto bytes = EncodeImage(big, planes);
    ASSERT_GT(bytes.size(), 30000U);

    class PlainSeekSource : public kdu_compressed_source {
     public:
      explicit PlainSeekSource(const std::vector<kdu_byte>& buf) : data_(buf) {}
      int get_capabilities() override {
        return KDU_SOURCE_CAP_SEEKABLE | KDU_SOURCE_CAP_SEQUENTIAL;
      }
      int read(kdu_byte* buf, int num_bytes) override {
        if (pos_ >= data_.size() || num_bytes <= 0) return 0;
        const size_t n = std::min<size_t>(data_.size() - pos_,
                                          std::min<size_t>(num_bytes, 251));
        std::memcpy(buf, data_.data() + pos_, n);
        pos_ += n;
        return static_cast<int>(n);
      }
      bool seek(kdu_long offset) override {
        if (offset < 0 || static_cast<size_t>(offset) > data_.size()) {
          return false;
        }
        pos_ = static_cast<size_t>(offset);
        return true;
      }
      kdu_long get_pos() override { return static_cast<kdu_long>(pos_); }

     private:
      std::vector<kdu_byte> data_;
      size_t pos_ = 0;
    };

    {
      PlainSeekSource psrc(bytes);
      ScopedCodestream sc;
      sc.cs.create(&psrc);
      sc.cs.set_persistent();
      kdu_dims all_tiles;
      all_tiles.pos = kdu_coords(0, 0);
      all_tiles.size = kdu_coords(2, 2);
      sc.cs.open_tiles(all_tiles, false, nullptr);
      sc.cs.close_tiles(all_tiles, nullptr);
      kdu_dims br;
      br.pos = kdu_coords(128, 128);
      br.size = kdu_coords(128, 128);
      sc.cs.apply_input_restrictions(0, 0, 0, 0, &br);
      kdu_tile t11 = sc.cs.open_tile(kdu_coords(1, 1));
      t11.close();
      kdu_dims tl;
      tl.pos = kdu_coords(0, 0);
      tl.size = kdu_coords(128, 128);
      sc.cs.apply_input_restrictions(0, 0, 0, 0, &tl);
      kdu_tile t00 = sc.cs.open_tile(kdu_coords(0, 0));
      t00.close();
    }

    // set_max_bytes smaller than stream triggers read/seek truncation paths.
    {
      PlainSeekSource psrc(bytes);
      ScopedCodestream sc;
      sc.cs.create(&psrc);
      sc.cs.set_max_bytes(4096, false, false);
      kdu_dims br;
      br.pos = kdu_coords(128, 128);
      br.size = kdu_coords(128, 128);
      sc.cs.apply_input_restrictions(0, 0, 0, 0, &br);
      kdu_tile t = sc.cs.open_tile(kdu_coords(1, 1));
      t.close();
    }
    {
      MemorySource msrc(bytes, KDU_SOURCE_CAP_MEMORY_BACKED |
                                   KDU_SOURCE_CAP_SEEKABLE |
                                   KDU_SOURCE_CAP_SEQUENTIAL);
      ScopedCodestream sc;
      sc.cs.create(&msrc);
      sc.cs.set_persistent();
      sc.cs.set_max_bytes(4096, false, false);
      kdu_dims br;
      br.pos = kdu_coords(128, 128);
      br.size = kdu_coords(128, 128);
      sc.cs.apply_input_restrictions(0, 0, 0, 0, &br);
      kdu_tile t11 = sc.cs.open_tile(kdu_coords(1, 1));
      t11.close();
      kdu_dims tl;
      tl.pos = kdu_coords(0, 0);
      tl.size = kdu_coords(128, 128);
      sc.cs.apply_input_restrictions(0, 0, 0, 0, &tl);
      kdu_tile t00 = sc.cs.open_tile(kdu_coords(0, 0));
      t00.close();
    }

    // Inject KDU_PPM marker before first SOT for PROFILE0 warning and cached
    // error 36.
    std::vector<kdu_byte> with_ppm = bytes;
    for (size_t i = 2; i + 2 <= with_ppm.size(); ++i) {
      if (with_ppm[i] == 0xFF && with_ppm[i + 1] == 0x90) {
        const kdu_byte ppm_seg[] = {0xFF, 0x60, 0x00, 0x07, 0x00,
                                    0x00, 0x00, 0x00, 0x00};
        with_ppm.insert(with_ppm.begin() + static_cast<ptrdiff_t>(i),
                        std::begin(ppm_seg), std::end(ppm_seg));
        break;
      }
    }
    {
      MemorySource psrc(with_ppm);
      ScopedCodestream sc;
      warning_handler_.clear();
      EXPECT_NO_THROW(sc.cs.create(&psrc));
      EXPECT_FALSE(warning_handler_.history().empty());
    }
    {
      MemorySource csrc(with_ppm, KDU_SOURCE_CAP_CACHED |
                                      KDU_SOURCE_CAP_SEEKABLE |
                                      KDU_SOURCE_CAP_SEQUENTIAL);
      ScopedCodestream sc;
      EXPECT_THROW(sc.cs.create(&csrc), kdu_exception);
    }
  }
}

TEST_F(CodestreamTest, SingleThreadedAllocatorCbrSequentialAndPcrdPaths) {
  // 1. kdu_sample_allocator brokered/unbrokered, multi-fragment & error paths.
  {
    struct DenyBroker : public kdu_membroker {
      explicit DenyBroker(kdu_int64 limit_bytes) : kdu_membroker(limit_bytes) {}
      kdu_int64 request(kdu_int64, kdu_int64, kdu_int64) override { return 0; }
    };
    kdu_membroker broker1(1 << 22);
    kdu_membroker broker2(1 << 22);
    DenyBroker deny_broker(1 << 22);

    {
      kdu_sample_allocator alloc;
      EXPECT_TRUE(alloc.configure(&broker1, 12));
      EXPECT_TRUE(alloc.configure(&broker1, 12));  // Same broker & frag_bits.
      EXPECT_TRUE(alloc.seek_permit(64));
      EXPECT_THROW(alloc.configure(&broker2, 12), kdu_exception);
      alloc.release_permit(64);
      EXPECT_TRUE(alloc.configure(nullptr, 12));  // Detach existing broker.
    }
    {
      kdu_sample_allocator alloc;
      EXPECT_TRUE(alloc.seek_permit(64));
      EXPECT_THROW(alloc.configure(&deny_broker, 12), kdu_exception);
      alloc.release_permit(64);
    }
    {
      kdu_sample_allocator alloc;
      EXPECT_TRUE(alloc.configure(&broker1, 18));  // MIN_FRAG_BITS = 18 (256KB)
      EXPECT_TRUE(alloc.configure(&broker1, 12));  // Clamped to 18 (same).
      alloc.pre_alloc(KDU_SAMPLE_PREC32, 0, 512);
      EXPECT_THROW(alloc.configure(&broker1, 19), kdu_exception);
    }
    {
      kdu_sample_allocator alloc;
      EXPECT_TRUE(alloc.configure(&broker1, 29));  // Clamped to MAX_FRAG_BITS.
      EXPECT_TRUE(alloc.configure(&broker1, 18));  // 256KB fragments.
      for (int i = 0; i < 4; ++i) {
        alloc.pre_alloc(KDU_SAMPLE_PREC32, 0, 70000);  // ~280KB -> multi-frag.
      }
      alloc.finalize(kdu_codestream());
      EXPECT_THROW(alloc.release(), kdu_exception);
      alloc.restart();
      alloc.release();
    }
    {
      kdu_membroker tight_broker(300000);  // Fits 1st frag, fails 2nd frag.
      kdu_sample_allocator alloc;
      EXPECT_TRUE(alloc.configure(&tight_broker, 18));
      alloc.pre_alloc(KDU_SAMPLE_PREC32, 0, 70000);
      alloc.pre_alloc(KDU_SAMPLE_PREC32, 0, 70000);
      EXPECT_THROW(alloc.finalize(kdu_codestream()), kdu_exception);
    }
    {
      kdu_membroker small_broker(64);
      kdu_sample_allocator alloc;
      EXPECT_TRUE(alloc.configure(&small_broker, 12));
      EXPECT_FALSE(alloc.seek_permit(200, true));
      EXPECT_THROW(alloc.seek_permit(200, false), kdu_exception);
    }
    {
      kdu_sample_allocator alloc;
      EXPECT_TRUE(alloc.seek_permit(static_cast<size_t>(KDU_INT64_MAX - 10)));
      EXPECT_FALSE(alloc.seek_permit(100, true));
      EXPECT_THROW(alloc.seek_permit(100, false), kdu_exception);
      alloc.release_permit(static_cast<size_t>(KDU_INT64_MAX - 10));
    }
  }

  // 2. CBR (Scbr) tiny budget error, uniform oversized budget, and incremental
  // multi-flush write_packets_cbr paths.
  {
    ImageSpec spec;
    spec.width = 128;
    spec.height = 128;
    spec.components = 1;
    spec.dwt_levels = 2;
    spec.extra_params = {"Cprecincts={32,32}", "Corder=PCRL", "Scbr={1,128}",
                         "Clayers=1"};
    auto planes = MakeTestPlanes(spec);
    for (size_t i = 0; i < planes[0].size(); ++i) {
      planes[0][i] =
          static_cast<kdu_int32>((i * 73 + (i >> 5) * 19) & 0xFF) - 128;
    }
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    ScopedCodestream sc;
    sc.cs.create(&siz, &tgt);
    ConfigureCodestreamParams(sc.cs, spec);
    kdu_tile tile = sc.cs.open_tile(kdu_coords(0, 0));
    PushTileData(sc.cs, tile, planes, spec);
    tile.close();
    kdu_long tiny = 50;
    EXPECT_THROW(sc.cs.flush(&tiny, 1), kdu_exception);
  }
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.dwt_levels = 1;
    spec.extra_params = {"Cprecincts={32,32}", "Corder=PCRL", "Scbr={1,128}",
                         "Clayers=1"};
    std::vector<std::vector<kdu_int32>> zero_planes(
        1, std::vector<kdu_int32>(64 * 64, 0));
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    ScopedCodestream sc;
    sc.cs.create(&siz, &tgt);
    ConfigureCodestreamParams(sc.cs, spec);
    kdu_tile tile = sc.cs.open_tile(kdu_coords(0, 0));
    PushTileData(sc.cs, tile, zero_planes, spec);
    tile.close();
    kdu_long huge = 500000;
    sc.cs.flush(&huge, 1);
    EXPECT_GT(tgt.data.size(), 0U);
  }
  for (bool trim : {true, false}) {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 256;
    spec.components = 2;
    spec.dwt_levels = 2;
    spec.extra_params = {"Cprecincts={8,8}", "Cblk={8,8}", "Scbr={1,128}",
                         "Clayers=1", "Corder=PCRL"};
    auto planes = MakeTestPlanes(spec);
    for (int c = 0; c < spec.components; ++c) {
      for (size_t i = 0; i < planes[c].size(); ++i) {
        planes[c][i] =
            static_cast<kdu_int32>(((i * 97 + c * 41) ^ (i >> 3)) & 0xFF) - 128;
      }
    }
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    ScopedCodestream sc;
    sc.cs.create(&siz, &tgt);
    ConfigureCodestreamParams(sc.cs, spec);
    kdu_long layer_bytes = 6000;
    kdu_tile tile = sc.cs.open_tile(kdu_coords(0, 0));
    PushTileData(sc.cs, tile, planes, spec);
    tile.close();
    if (sc.cs.ready_for_flush()) {
      sc.cs.flush(&layer_bytes, 1, nullptr, trim, true, 0.0, nullptr, 0);
    }
    EXPECT_GT(tgt.data.size(), 100U);
  }
  {
    // Vertical tiling with flush sets smaller than tile height throws.
    ImageSpec spec;
    spec.width = 64;
    spec.height = 128;
    spec.components = 1;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(64, 64);
    spec.extra_params = {"Cprecincts={8,8}", "Cblk={8,8}", "Scbr={1,128}",
                         "Clayers=1", "Corder=PCRL"};
    auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    ScopedCodestream sc;
    sc.cs.create(&siz, &tgt);
    ConfigureCodestreamParams(sc.cs, spec);
    kdu_tile tile = sc.cs.open_tile(kdu_coords(0, 0));
    PushTileData(sc.cs, tile, planes, spec);
    tile.close();
    kdu_long layer_bytes = 4000;
    EXPECT_THROW(sc.cs.flush(&layer_bytes, 1), kdu_exception);
  }

  // 3. Non-seekable PlainSequentialSource: large COM refill (kd_input::read),
  // ROI skipping early tiles (kd_input::ignore), load_sequential modes, and
  // PPM / PPT packed packet headers (kd_pph_input::load_buf).
  class PlainSequentialSource : public kdu_compressed_source {
   public:
    explicit PlainSequentialSource(const std::vector<kdu_byte>& data)
        : data_(data) {}
    int get_capabilities() override { return KDU_SOURCE_CAP_SEQUENTIAL; }
    int read(kdu_byte* buf, int num_bytes) override {
      if (pos_ >= data_.size()) return 0;
      const size_t avail = data_.size() - pos_;
      const size_t n = std::min(static_cast<size_t>(num_bytes), avail);
      std::copy(data_.data() + pos_, data_.data() + pos_ + n, buf);
      pos_ += n;
      return static_cast<int>(n);
    }
    bool seek(kdu_long) override { return false; }
    kdu_long get_pos() override { return static_cast<kdu_long>(pos_); }

   private:
    const std::vector<kdu_byte>& data_;
    size_t pos_ = 0;
  };

  {
    ImageSpec spec;
    spec.width = 256;
    spec.height = 256;
    spec.components = 3;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(128, 128);
    spec.extra_params = {"Cprecincts={32,32}", "ORGtparts=R", "Clayers=3"};
    auto planes = MakeTestPlanes(spec);
    for (int c = 0; c < spec.components; ++c) {
      for (size_t i = 0; i < planes[c].size(); ++i) {
        planes[c][i] =
            static_cast<kdu_int32>(((i * 53 + c * 29) ^ (i >> 4)) & 0xFF) - 128;
      }
    }
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    {
      ScopedCodestream enc;
      enc.cs.create(&siz, &tgt);
      ConfigureCodestreamParams(enc.cs, spec);
      kdu_codestream_comment cmt = enc.cs.add_comment();
      std::vector<kdu_byte> big_cmt(40000, 0x42);
      cmt.put_data(big_cmt.data(), static_cast<int>(big_cmt.size()));
      for (int ty = 0; ty < 2; ++ty) {
        for (int tx = 0; tx < 2; ++tx) {
          kdu_tile tile = enc.cs.open_tile(kdu_coords(tx, ty));
          PushTileData(enc.cs, tile, planes, spec);
          tile.close();
        }
      }
      kdu_long layers[3] = {8000, 16000, 32000};
      enc.cs.flush(layers, 3);
    }

    // ROI restricted to bottom-right tile -> kd_input::ignore skips early
    // tiles; 40KB COM exercises kd_input::read across buffer boundaries.
    {
      PlainSequentialSource ssrc(tgt.data);
      ScopedCodestream dec;
      dec.cs.create(&ssrc);
      kdu_dims roi = MakeDims(128, 128, 128, 128);
      dec.cs.apply_input_restrictions(0, 0, 0, 3, &roi,
                                      KDU_WANT_OUTPUT_COMPONENTS);
      kdu_dims valid_tiles;
      dec.cs.get_valid_tiles(valid_tiles);
      for (int ty = valid_tiles.pos.y;
           ty < valid_tiles.pos.y + valid_tiles.size.y; ++ty) {
        for (int tx = valid_tiles.pos.x;
             tx < valid_tiles.pos.x + valid_tiles.size.x; ++tx) {
          kdu_tile t = dec.cs.open_tile(kdu_coords(tx, ty));
          kdu_resolution res = t.access_component(0).access_resolution(0);
          kdu_subband sb = res.access_subband(LL_BAND);
          kdu_dims blocks;
          sb.get_valid_blocks(blocks);
          if (blocks.area() > 0) {
            kdu_block* blk = sb.open_block(blocks.pos);
            sb.close_block(blk);
          }
          t.close();
        }
      }
    }

    // Sequential load_sequential without CACHED source:
    // WANT_FLUSH_SET | WANT_PACKET_BYTES (no WANT_TILE) across all tile-parts.
    {
      PlainSequentialSource ssrc(tgt.data);
      ScopedCodestream dec;
      dec.cs.create(&ssrc);
      for (int ty = 0; ty < 2; ++ty) {
        for (int tx = 0; tx < 2; ++tx) {
          kdu_pdescriptor_set pset;
          ASSERT_TRUE(pset.init(kdu_coords(tx, ty), 3));
          bool tile_done = false;
          kdu_tile open_t;
          while (!tile_done) {
            kdu_tile nt = dec.cs.load_sequential(
                &pset, tile_done, 1, 0,
                KDU_LOAD_SEQUENTIAL_WANT_FLUSH_SET |
                    KDU_LOAD_SEQUENTIAL_WANT_PACKET_BYTES,
                nullptr);
            if (nt.exists()) open_t = nt;
            if (!nt.exists() && !tile_done) break;
          }
          if (open_t.exists()) open_t.close();
        }
      }
    }

    // load_sequential with WANT_PACKET_BYTES only + min_code_blocks and errors.
    {
      PlainSequentialSource ssrc(tgt.data);
      ScopedCodestream dec;
      dec.cs.create(&ssrc);
      kdu_pdescriptor_set pset;
      ASSERT_TRUE(pset.init(kdu_coords(0, 0), 3));
      bool tile_done = false;
      kdu_tile t = dec.cs.load_sequential(&pset, tile_done, 0, 2,
                                          KDU_LOAD_SEQUENTIAL_WANT_PACKET_BYTES,
                                          nullptr);
      if (t.exists()) t.close();

      // Calling load_sequential when pdescriptor_set is bound to a different
      // tile returns an empty kdu_tile without advancing past that tile.
      PlainSequentialSource ssrc_mismatch(tgt.data);
      ScopedCodestream dec_mismatch;
      dec_mismatch.cs.create(&ssrc_mismatch);
      kdu_pdescriptor_set bad_pset;
      ASSERT_TRUE(bad_pset.init(kdu_coords(1, 1), 3));
      bad_pset.add_pdescriptor(0, 0, kdu_coords(0, 0));
      bool td2 = false;
      kdu_tile mismatch_t = dec_mismatch.cs.load_sequential(
          &bad_pset, td2, 0, 0,
          KDU_LOAD_SEQUENTIAL_WANT_TILE | KDU_LOAD_SEQUENTIAL_WANT_FLUSH_SET,
          nullptr);
      EXPECT_FALSE(mismatch_t.exists());

      // Exercises non-empty pset re-init rejection, reset+re-init with
      // different packet_bytes_entries, and load_sequential with a smaller
      // packet_bytes_entries than num_layers.
      PlainSequentialSource ssrc_badlen(tgt.data);
      ScopedCodestream dec_badlen;
      dec_badlen.cs.create(&ssrc_badlen);
      kdu_pdescriptor_set bad_len_pset;
      ASSERT_TRUE(bad_len_pset.init(kdu_coords(0, 0), 1));
      bad_len_pset.add_pdescriptor(0, 0, kdu_coords(0, 0));
      EXPECT_FALSE(bad_len_pset.init(kdu_coords(0, 0), 2));
      bad_len_pset.reset();
      EXPECT_TRUE(bad_len_pset.init(kdu_coords(0, 0), 2));
      kdu_tile blt = dec_badlen.cs.load_sequential(
          &bad_len_pset, td2, 0, 0,
          KDU_LOAD_SEQUENTIAL_WANT_TILE | KDU_LOAD_SEQUENTIAL_WANT_PACKET_BYTES,
          nullptr);
      if (blt.exists()) blt.close();
    }
    {
      MemorySource cached_src(tgt.data, KDU_SOURCE_CAP_CACHED |
                                            KDU_SOURCE_CAP_SEEKABLE |
                                            KDU_SOURCE_CAP_SEQUENTIAL);
      ScopedCodestream dec;
      dec.cs.create(&cached_src);
      kdu_pdescriptor_set pset;
      bool td = false;
      EXPECT_THROW(dec.cs.load_sequential(
                       &pset, td, 0, 0, KDU_LOAD_SEQUENTIAL_WANT_TILE, nullptr),
                   kdu_exception);
    }
  }

  // Packed packet headers via PPT (0xFF61) in tile header and PPM (0xFF60) in
  // main header on a minimal 8x8 single-packet empty codestream (kd_pph_input).
  {
    ImageSpec min_spec;
    min_spec.width = 8;
    min_spec.height = 8;
    min_spec.components = 1;
    min_spec.dwt_levels = 0;
    min_spec.reversible = true;
    siz_params min_siz;
    ConfigureSiz(min_siz, min_spec);
    MemoryTarget min_tgt;
    {
      ScopedCodestream enc;
      enc.cs.create(&min_siz, &min_tgt);
      ConfigureCodestreamParams(enc.cs, min_spec);
      std::vector<std::vector<kdu_int32>> zeros(1,
                                                std::vector<kdu_int32>(64, 0));
      kdu_tile t = enc.cs.open_tile(kdu_coords(0, 0));
      PushTileData(enc.cs, t, zeros, min_spec);
      t.close();
      kdu_long min_bytes = 0;
      kdu_uint16 max_thresh = 65535;
      enc.cs.flush(&min_bytes, 1, &max_thresh, true, false, 0.0, nullptr,
                   KDU_FLUSH_USES_THRESHOLDS_AND_SIZES);
    }
    const auto& base = min_tgt.data;
    size_t sot_pos = std::string::npos;
    size_t sod_pos = std::string::npos;
    for (size_t i = 0; i + 1 < base.size(); ++i) {
      if (base[i] == 0xFF && base[i + 1] == 0x90 &&
          sot_pos == std::string::npos) {
        sot_pos = i;
      }
      if (sot_pos != std::string::npos && i >= sot_pos + 12 &&
          base[i] == 0xFF && base[i + 1] == 0x93) {
        sod_pos = i;
        break;
      }
    }
    ASSERT_NE(sot_pos, std::string::npos);
    ASSERT_NE(sod_pos, std::string::npos);
    ASSERT_EQ(sod_pos + 3 + 2, base.size());  // 1-byte empty pkt + EOC(2)

    // Inject PPT (0xFF61, len=4: Zppt=0, pkt_hdr=base[sod_pos+2]) before SOD.
    std::vector<kdu_byte> with_ppt;
    with_ppt.insert(with_ppt.end(), base.begin(), base.begin() + sod_pos);
    with_ppt.push_back(0xFF);
    with_ppt.push_back(0x61);
    with_ppt.push_back(0x00);
    with_ppt.push_back(0x04);
    with_ppt.push_back(0x00);
    with_ppt.push_back(base[sod_pos + 2]);
    with_ppt.push_back(0xFF);
    with_ppt.push_back(0x93);
    with_ppt.push_back(0xFF);
    with_ppt.push_back(0xD9);
    // Update Psot (+6 - 1 = +5).
    const kdu_uint32 old_psot =
        (static_cast<kdu_uint32>(with_ppt[sot_pos + 6]) << 24) |
        (static_cast<kdu_uint32>(with_ppt[sot_pos + 7]) << 16) |
        (static_cast<kdu_uint32>(with_ppt[sot_pos + 8]) << 8) |
        static_cast<kdu_uint32>(with_ppt[sot_pos + 9]);
    if (old_psot != 0) {
      const kdu_uint32 new_psot = old_psot + 5;
      with_ppt[sot_pos + 6] = static_cast<kdu_byte>((new_psot >> 24) & 0xFF);
      with_ppt[sot_pos + 7] = static_cast<kdu_byte>((new_psot >> 16) & 0xFF);
      with_ppt[sot_pos + 8] = static_cast<kdu_byte>((new_psot >> 8) & 0xFF);
      with_ppt[sot_pos + 9] = static_cast<kdu_byte>(new_psot & 0xFF);
    }

    {
      PlainSequentialSource psrc(with_ppt);
      ScopedCodestream dec;
      dec.cs.create(&psrc);
      kdu_tile t = dec.cs.open_tile(kdu_coords(0, 0));
      kdu_resolution res = t.access_component(0).access_resolution(0);
      kdu_subband sb = res.access_subband(LL_BAND);
      kdu_block* blk = sb.open_block(kdu_coords(0, 0));
      EXPECT_EQ(blk->num_passes, 0);
      sb.close_block(blk);
      t.close();
    }

    // Valid PPM (0xFF60, len=8: Zppm=0, Nppm=1, pkt_hdr) in main header.
    std::vector<kdu_byte> with_ppm;
    with_ppm.insert(with_ppm.end(), base.begin(), base.begin() + sot_pos);
    with_ppm.push_back(0xFF);
    with_ppm.push_back(0x60);
    with_ppm.push_back(0x00);
    with_ppm.push_back(0x08);
    with_ppm.push_back(0x00);  // Zppm = 0
    with_ppm.push_back(0x00);
    with_ppm.push_back(0x00);
    with_ppm.push_back(0x00);
    with_ppm.push_back(0x01);  // Nppm = 1 byte
    with_ppm.push_back(base[sod_pos + 2]);
    const size_t new_sot_pos = with_ppm.size();
    with_ppm.insert(with_ppm.end(), base.begin() + sot_pos,
                    base.begin() + sod_pos + 2);
    with_ppm.push_back(0xFF);
    with_ppm.push_back(0xD9);
    const kdu_uint32 ppm_psot = old_psot - 1;
    with_ppm[new_sot_pos + 6] = static_cast<kdu_byte>((ppm_psot >> 24) & 0xFF);
    with_ppm[new_sot_pos + 7] = static_cast<kdu_byte>((ppm_psot >> 16) & 0xFF);
    with_ppm[new_sot_pos + 8] = static_cast<kdu_byte>((ppm_psot >> 8) & 0xFF);
    with_ppm[new_sot_pos + 9] = static_cast<kdu_byte>(ppm_psot & 0xFF);

    {
      PlainSequentialSource psrc(with_ppm);
      ScopedCodestream dec;
      dec.cs.create(&psrc);
      kdu_tile t = dec.cs.open_tile(kdu_coords(0, 0));
      kdu_resolution res = t.access_component(0).access_resolution(0);
      kdu_subband sb = res.access_subband(LL_BAND);
      kdu_block* blk = sb.open_block(kdu_coords(0, 0));
      EXPECT_EQ(blk->num_passes, 0);
      sb.close_block(blk);
      t.close();
    }
  }

  // 4. pcrd_opt thresholds+sizes, non-decreasing thresholds, leading unsized
  // layers, pcrd_trim / trans_out / set_max_bytes, and TLM slot overflow.
  {
    ImageSpec spec;
    spec.width = 128;
    spec.height = 128;
    spec.components = 1;
    spec.dwt_levels = 2;
    spec.extra_params = {"Cblk={32,32}", "Clayers=3"};
    auto planes = MakeTestPlanes(spec);
    for (size_t i = 0; i < planes[0].size(); ++i) {
      planes[0][i] =
          static_cast<kdu_int32>((i * 113 + (i >> 4) * 37) & 0xFF) - 128;
    }

    auto flush_with_thresh = [&](std::initializer_list<kdu_long> sizes,
                                 std::initializer_list<kdu_uint16> thresh,
                                 int flags, bool trim) {
      siz_params siz;
      ConfigureSiz(siz, spec);
      MemoryTarget tgt;
      ScopedCodestream sc;
      sc.cs.create(&siz, &tgt);
      ConfigureCodestreamParams(sc.cs, spec);
      kdu_tile t = sc.cs.open_tile(kdu_coords(0, 0));
      PushTileData(sc.cs, t, planes, spec);
      t.close();
      std::vector<kdu_long> sz(sizes);
      std::vector<kdu_uint16> th(thresh);
      sc.cs.flush(sz.data(), static_cast<int>(sz.size()), th.data(), trim, true,
                  0.0, nullptr, flags);
      return tgt.data;
    };

    // High thresholds overridden by larger min_sizes.
    EXPECT_GT(flush_with_thresh({2000, 4000, 8000}, {60000, 55000, 50000},
                                KDU_FLUSH_USES_THRESHOLDS_AND_SIZES, true)
                  .size(),
              0U);
    // Non-decreasing thresholds with zero min_sizes and trim=false.
    EXPECT_GT(flush_with_thresh({0, 0, 0}, {30000, 35000, 40000},
                                KDU_FLUSH_USES_THRESHOLDS_AND_SIZES, false)
                  .size(),
              0U);

    // Leading zero-budget layers with non-zero tolerance and trim_to_rate=true.
    EXPECT_GT(flush_with_thresh({0, 0, 2400}, {0, 0, 0}, 0, true).size(), 0U);

    // Persistent input codestream: set_max_bytes exercises
    // trim_compressed_data.
    spec.extra_params = {"Cblk={32,32}", "Clayers=4"};
    auto full_bytes = EncodeImage(spec, planes);
    MemorySource src(full_bytes);
    ScopedCodestream dec;
    dec.cs.create(&src);
    dec.cs.set_persistent();
    dec.cs.set_max_bytes(4096, /*simulate_parsing=*/false);
    kdu_tile dt = dec.cs.open_tile(kdu_coords(0, 0));
    dt.close();
    EXPECT_GT(full_bytes.size(), 0U);
  }

  // ORGgen_tlm=1 with ORGtparts=R & multiple levels (tpart count > reserved),
  // plus byte/short ORGtlm_style.
  {
    ImageSpec spec;
    spec.width = 128;
    spec.height = 128;
    spec.components = 1;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(64, 64);
    spec.extra_params = {"ORGgen_tlm=1", "ORGtparts=R",
                         "ORGtlm_style={byte,short}", "Clayers=2"};
    auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    ScopedCodestream sc;
    sc.cs.create(&siz, &tgt);
    ConfigureCodestreamParams(sc.cs, spec);
    for (int ty = 0; ty < 2; ++ty) {
      for (int tx = 0; tx < 2; ++tx) {
        kdu_tile t = sc.cs.open_tile(kdu_coords(tx, ty));
        PushTileData(sc.cs, t, planes, spec);
        t.close();
      }
    }
    kdu_long lb[2] = {1000, 2500};
    EXPECT_THROW(sc.cs.flush(lb, 2), kdu_exception);
  }
  {
    ImageSpec spec;
    spec.width = 128;
    spec.height = 128;
    spec.components = 1;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(64, 64);
    spec.extra_params = {"ORGgen_tlm=8", "ORGtparts=R",
                         "ORGtlm_style={byte,short}", "Clayers=2"};
    auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    ScopedCodestream sc;
    sc.cs.create(&siz, &tgt);
    ConfigureCodestreamParams(sc.cs, spec);
    for (int ty = 0; ty < 2; ++ty) {
      for (int tx = 0; tx < 2; ++tx) {
        kdu_tile t = sc.cs.open_tile(kdu_coords(tx, ty));
        PushTileData(sc.cs, t, planes, spec);
        t.close();
      }
    }
    kdu_long lb[2] = {1000, 2500};
    sc.cs.flush(lb, 2);
    EXPECT_GT(tgt.data.size(), 0U);
  }

  // 5. 2-stage MCT selective output block pruning + kdu_quality_limiter checks.
  {
    ImageSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 3;
    spec.reversible = true;
    spec.dwt_levels = 1;
    spec.extra_params = {
        "Sextensions=MCT",
        "Mcomponents=3",
        "Msigned=no",
        "Mprecision=8",
        "Mnum_stages=2",
        "Mstages=1,2",
        "Mstage_inputs:I1={0,2}",
        "Mstage_outputs:I1={0,2}",
        "Mstage_collections:I1={1,1},{2,2}",
        "Mstage_xforms:I1={MATRIX,0,0,0,0},{MATRIX,0,0,0,0}",
        "Mstage_inputs:I2={0,2}",
        "Mstage_outputs:I2={0,2}",
        "Mstage_collections:I2={1,1},{2,2}",
        "Mstage_xforms:I2={MATRIX,0,0,0,0},{MATRIX,0,0,0,0}",
    };
    auto planes = MakeTestPlanes(spec);
    auto mct_bytes = EncodeImage(spec, planes);
    MemorySource msrc(mct_bytes);
    ScopedCodestream dec;
    dec.cs.create(&msrc);
    dec.cs.enable_restart();
    // Request only components {1,2}; stage 2 collection 0 ({0}->{0}) is unused.
    int out_comps[2] = {1, 2};
    kdu_quality_limiter ql(0.5F, false);
    ql.set_display_resolution(150.0F, 150.0F);
    ql.set_comp_info(0, 1.0F, 0);
    dec.cs.apply_input_restrictions(2, out_comps, 0, 1, nullptr,
                                    KDU_WANT_OUTPUT_COMPONENTS, nullptr, &ql);
    EXPECT_EQ(dec.cs.get_num_components(true), 2);
    dec.cs.restart(&msrc);
    int bad_comps[1] = {99};
    dec.cs.apply_input_restrictions(1, bad_comps, 0, 1, nullptr,
                                    KDU_WANT_OUTPUT_COMPONENTS);
    EXPECT_EQ(dec.cs.get_num_components(true), 0);
  }
}

TEST_F(CodestreamTest, MoreSingleThreadedUncoveredPaths) {
  // 1. Fragment codestreams: left/upper and right/lower misalignment errors,
  // non-initial fragment TLM continuation, and Scbr errors with cached target
  // and non-initial fragment.
  {
    ImageSpec spec;
    spec.width = 128;
    spec.height = 64;
    spec.components = 1;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(64, 64);
    spec.extra_params = {"ORGgen_tlm=1"};
    auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);

    // Left/upper not aligned on tile boundary.
    {
      MemoryTarget tgt;
      ScopedCodestream cs;
      kdu_dims bad_left = MakeDims(16, 0, 48, 64);
      EXPECT_THROW(cs.cs.create(&siz, &tgt, &bad_left, 0, 0), kdu_exception);
    }
    // Right/lower not aligned on tile boundary.
    {
      MemoryTarget tgt;
      ScopedCodestream cs;
      kdu_dims bad_right = MakeDims(0, 0, 80, 64);
      EXPECT_THROW(cs.cs.create(&siz, &tgt, &bad_right, 0, 0), kdu_exception);
    }
    // Valid non-initial fragment covering tile (1,0) with ORGgen_tlm=1.
    {
      class SeekableFragTarget : public kdu_compressed_target {
       public:
        bool write(const kdu_byte* buf, int n) override {
          data.insert(data.end(), buf, buf + n);
          return true;
        }
        bool start_rewrite(kdu_long) override { return true; }
        bool end_rewrite() override { return true; }
        std::vector<kdu_byte> data;
      };
      SeekableFragTarget ftgt;
      ScopedCodestream cs;
      kdu_dims frag = MakeDims(64, 0, 64, 64);
      cs.cs.create(&siz, &ftgt, &frag, /*fragment_tiles_generated=*/1,
                   /*fragment_tile_bytes_generated=*/100);
      ConfigureCodestreamParams(cs.cs, spec);
      EXPECT_TRUE(cs.cs.is_last_fragment());
      kdu_tile tile = cs.cs.open_tile(kdu_coords(1, 0));
      PushTileData(cs.cs, tile, planes, spec);
      tile.close();
      kdu_long budget = 4096;
      cs.cs.flush(&budget, 1);
      EXPECT_FALSE(ftgt.data.empty());
    }
    // Scbr on non-initial fragment -> throws in flush.
    {
      ImageSpec cbr_spec = spec;
      cbr_spec.width = 64;
      cbr_spec.height = 128;
      cbr_spec.extra_params = {"Corder=PCRL", "Cprecincts={16,16}",
                               "Scbr={1,64}"};
      siz_params cbr_siz;
      ConfigureSiz(cbr_siz, cbr_spec);
      MemoryTarget tgt;
      ScopedCodestream cs;
      kdu_dims frag = MakeDims(0, 64, 64, 64);
      cs.cs.create(&cbr_siz, &tgt, &frag, /*fragment_tiles_generated=*/1,
                   /*fragment_tile_bytes_generated=*/100);
      ConfigureCodestreamParams(cs.cs, cbr_spec);
      kdu_tile tile = cs.cs.open_tile(kdu_coords(0, 1));
      PushTileData(cs.cs, tile, planes, cbr_spec);
      tile.close();
      kdu_long budget = 4096;
      EXPECT_THROW(cs.cs.flush(&budget, 1), kdu_exception);
    }
    // Scbr with KDU_TARGET_CAP_CACHED target -> throws in flush.
    {
      class CachedTarget : public kdu_compressed_target {
       public:
        int get_capabilities() override {
          return KDU_TARGET_CAP_SEQUENTIAL | KDU_TARGET_CAP_CACHED;
        }
        bool write(const kdu_byte* buf, int num_bytes) override {
          data.insert(data.end(), buf, buf + num_bytes);
          return true;
        }
        std::vector<kdu_byte> data;
      };
      ImageSpec cbr_spec = spec;
      cbr_spec.width = 64;
      cbr_spec.height = 128;
      cbr_spec.extra_params = {"Corder=PCRL", "Cprecincts={16,16}",
                               "Scbr={1,64}"};
      siz_params cbr_siz;
      ConfigureSiz(cbr_siz, cbr_spec);
      CachedTarget ctgt;
      ScopedCodestream cs;
      cs.cs.create(&cbr_siz, &ctgt);
      ConfigureCodestreamParams(cs.cs, cbr_spec);
      kdu_tile tile = cs.cs.open_tile(kdu_coords(0, 0));
      PushTileData(cs.cs, tile, planes, cbr_spec);
      tile.close();
      kdu_tile tile1 = cs.cs.open_tile(kdu_coords(0, 1));
      PushTileData(cs.cs, tile1, planes, cbr_spec);
      tile1.close();
      kdu_long budget = 4096;
      EXPECT_THROW(cs.cs.flush(&budget, 1), kdu_exception);
    }
  }

  // 2. 260-tile ORGgen_tlm=2 with 2-byte tile indices (tnum_prec==2) and
  // sequential non-seekable target rejection.
  {
    ImageSpec spec;
    spec.width = 1040;
    spec.height = 4;
    spec.components = 1;
    spec.dwt_levels = 0;
    spec.tile_size = kdu_coords(4, 4);
    spec.extra_params = {"ORGgen_tlm=2", "ORGtlm_style={short,short}"};
    auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    {
      ScopedCodestream cs;
      cs.cs.create(&siz, &tgt);
      ConfigureCodestreamParams(cs.cs, spec);
      for (int tx = 0; tx < 260; ++tx) {
        kdu_tile tile = cs.cs.open_tile(kdu_coords(tx, 0));
        PushTileData(cs.cs, tile, planes, spec);
        tile.close();
      }
      kdu_long budget = 65536;
      cs.cs.flush(&budget, 1);
    }
    EXPECT_FALSE(tgt.data.empty());

    class PlainSequentialTarget : public kdu_compressed_target {
     public:
      int get_capabilities() override { return KDU_TARGET_CAP_SEQUENTIAL; }
      bool write(const kdu_byte* buf, int num_bytes) override {
        data.insert(data.end(), buf, buf + num_bytes);
        return true;
      }
      bool start_rewrite(kdu_long) override { return false; }
      std::vector<kdu_byte> data;
    };
    ImageSpec small_spec;
    small_spec.width = 16;
    small_spec.height = 16;
    small_spec.components = 1;
    small_spec.dwt_levels = 1;
    small_spec.extra_params = {"ORGgen_tlm=1"};
    auto small_planes = MakeTestPlanes(small_spec);
    siz_params small_siz;
    ConfigureSiz(small_siz, small_spec);
    PlainSequentialTarget stgt;
    ScopedCodestream cs;
    cs.cs.create(&small_siz, &stgt);
    ConfigureCodestreamParams(cs.cs, small_spec);
    kdu_tile tile = cs.cs.open_tile(kdu_coords(0, 0));
    PushTileData(cs.cs, tile, small_planes, small_spec);
    tile.close();
    warning_handler_.clear();
    kdu_long budget = 4096;
    cs.cs.flush(&budget, 1);
    EXPECT_FALSE(warning_handler_.history().empty());
  }

  // 3. 3-component multi-precinct CBR (error when Bmax < flush-set lines, then
  // valid write_packets_cbr non-primary loop).
  for (const char* scbr_val : {"Scbr={1,16}", "Scbr={1,96}"}) {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 128;
    spec.components = 3;
    spec.dwt_levels = 2;
    spec.extra_params = {"Corder=PCRL", "Cprecincts={16,16}", "Cblk={16,16}",
                         scbr_val};
    auto planes = MakeTestPlanes(spec);
    for (int c = 0; c < spec.components; ++c) {
      for (size_t i = 0; i < planes[c].size(); ++i) {
        planes[c][i] = static_cast<kdu_int32>((i * 41 + c * 19) & 0xFF) - 128;
      }
    }
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    ScopedCodestream cs;
    cs.cs.create(&siz, &tgt);
    ConfigureCodestreamParams(cs.cs, spec);
    kdu_tile tile = cs.cs.open_tile(kdu_coords(0, 0));
    PushTileData(cs.cs, tile, planes, spec);
    tile.close();
    kdu_long budget = 4096;
    if (std::string(scbr_val) == "Scbr={1,16}") {
      EXPECT_THROW(cs.cs.flush(&budget, 1), kdu_exception);
    } else {
      cs.cs.flush(&budget, 1);
      EXPECT_FALSE(tgt.data.empty());
    }
  }

  // 4. Cplex=EST with set_max_bytes, duplicate set_max_bytes error,
  // enable_restart / set_persistent / set_synth_overshoot_limit checks, and
  // kdu_flush_stats mismatch across restart & attach_flush_stats.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.dwt_levels = 2;
    spec.extra_params = {"Cplex={6,EST,0.25,0}", "Cprecincts={16,16}"};
    auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    ScopedCodestream cs;
    cs.cs.create(&siz, &tgt);
    ConfigureCodestreamParams(cs.cs, spec);
    cs.cs.set_max_bytes(4096, /*simulate_parsing=*/false);
    cs.cs.set_max_slope_threshold(50000);
    kdu_tile tile = cs.cs.open_tile(kdu_coords(0, 0));
    EXPECT_THROW(cs.cs.enable_restart(), kdu_exception);
    PushTileData(cs.cs, tile, planes, spec);
    tile.close();
    kdu_long budget = 4096;
    cs.cs.flush(&budget, 1);
  }

  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.dwt_levels = 2;
    auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    ScopedCodestream cs;
    cs.cs.create(&siz, &tgt);
    ConfigureCodestreamParams(cs.cs, spec);
    cs.cs.enable_restart();
    cs.cs.enable_restart();  // Already enabled -> early return.
    cs.cs.set_persistent();  // Output codestream -> early return.
    cs.cs.configure_simultaneous_processing_fragments(0, 0, 1);
    cs.cs.configure_simultaneous_processing_fragments(64, 4, 4);
    cs.cs.set_max_bytes(4096, /*simulate_parsing=*/false);
    EXPECT_THROW(cs.cs.set_max_bytes(4096, /*simulate_parsing=*/false),
                 kdu_exception);

    kdu_flush_stats fstats = cs.cs.add_flush_stats(0, 5, 0, 1);
    fstats.set_max_passes(true, true, -1, 300);
    fstats.set_max_passes(true, true, 0, -5);
    EXPECT_THROW(fstats.advance(0), kdu_exception);

    kdu_tile tile = cs.cs.open_tile(kdu_coords(0, 0));
    EXPECT_THROW(cs.cs.set_synth_overshoot_limit(1.0F, true), kdu_exception);
    PushTileData(cs.cs, tile, planes, spec);
    tile.close();
    kdu_long budget = 4096;
    cs.cs.flush(&budget, 1);

    // Changing Cprecincts after restart triggers flush_stats structure error.
    MemoryTarget tgt2;
    cs.cs.restart(&tgt2);
    cs.cs.access_siz()->parse_string("Cprecincts={8,8}");
    cs.cs.access_siz()->finalize_all();
    EXPECT_THROW((void)cs.cs.open_tile(kdu_coords(0, 0)), kdu_exception);

    // Separate encoder with different Cprecincts + attach_flush_stats ->
    // open_tile triggers attach_tile_comp mismatch.
    ImageSpec spec_b = spec;
    spec_b.extra_params = {"Cprecincts={8,8}"};
    siz_params siz_b;
    ConfigureSiz(siz_b, spec_b);
    MemoryTarget tgt_b;
    ScopedCodestream cs_b;
    cs_b.cs.create(&siz_b, &tgt_b);
    ConfigureCodestreamParams(cs_b.cs, spec_b);
    // Calling attach_flush_stats with matching frame index throws.
    kdu_flush_stats fstats_b = cs_b.cs.add_flush_stats(0, 5, 0, 1);
    fstats_b.auto_advance(1);
    EXPECT_THROW(fstats_b.advance(1), kdu_exception);
    EXPECT_THROW(cs_b.cs.attach_flush_stats(fstats), kdu_exception);

    // Distinct frame index with identical dimensions -> attach_flush_stats
    // succeeds and normal tile push + flush completes.
    siz_params siz_c;
    ConfigureSiz(siz_c, spec_b);
    MemoryTarget tgt_c;
    ScopedCodestream cs_c;
    cs_c.cs.create(&siz_c, &tgt_c);
    ConfigureCodestreamParams(cs_c.cs, spec_b);
    (void)cs_c.cs.add_flush_stats(1, 5, 0, 1);
    cs_c.cs.attach_flush_stats(fstats);
    kdu_tile tile_c = cs_c.cs.open_tile(kdu_coords(0, 0));
    PushTileData(cs_c.cs, tile_c, planes, spec_b);
    tile_c.close();
    kdu_long budget_c = 4096;
    cs_c.cs.flush(&budget_c, 1);
    EXPECT_FALSE(tgt_c.data.empty());

    // Incompatible component count triggers check_compatible error in
    // attach_flush_stats.
    ImageSpec spec_d = spec;
    spec_d.components = 3;
    siz_params siz_d;
    ConfigureSiz(siz_d, spec_d);
    MemoryTarget tgt_d;
    ScopedCodestream cs_d;
    cs_d.cs.create(&siz_d, &tgt_d);
    ConfigureCodestreamParams(cs_d.cs, spec_d);
    (void)cs_d.cs.add_flush_stats(1, 5, 0, 1);
    EXPECT_THROW(cs_d.cs.attach_flush_stats(fstats), kdu_exception);

    // Input codestream checks: set_fussy / set_fast / allow_parsing_errors /
    // set_persistent after open_tile.
    MemorySource msrc(tgt.data);
    ScopedCodestream dec;
    dec.cs.create(&msrc);
    dec.cs.set_fussy();
    dec.cs.set_fast();
    dec.cs.set_allow_parsing_errors(true);
    kdu_tile dtile = dec.cs.open_tile(kdu_coords(0, 0));
    EXPECT_THROW(dec.cs.set_persistent(), kdu_exception);
    dtile.close();
  }
}

TEST_F(CodestreamTest, SingleThreadedGuardsMctCrgQualityAndErrors) {
  // 1. Null (uncreated) codestream guards & kdu_quality_limiter branches.
  {
    kdu_codestream null_cs;
    int discard_levels = 0;
    int max_layers = 0;
    kdu_component_access_mode mode = KDU_WANT_CODESTREAM_COMPONENTS;
    EXPECT_EQ(null_cs.get_input_restrictions(discard_levels, max_layers, mode),
              0);
    EXPECT_FALSE(null_cs.add_flush_stats(0, 5, 0, 1).exists());
    null_cs.attach_flush_stats(kdu_flush_stats());
    EXPECT_EQ(null_cs.get_total_bytes(/*exclude_main_header=*/true), 0);
    EXPECT_EQ(null_cs.get_packet_bytes(), 0);
    EXPECT_EQ(null_cs.get_packet_header_bytes(), 0);
    EXPECT_EQ(null_cs.get_params_memory(/*get_peak_allocation=*/false), 0);
    EXPECT_FLOAT_EQ(null_cs.set_cbr_start_delay(2.0F), 0.0F);
    float sd = 0.0F;
    float mb = 0.0F;
    kdu_uint32 fs = 0;
    kdu_uint32 ad = 0;
    kdu_uint32 sd2 = 0;
    EXPECT_FALSE(null_cs.get_cbr_model(sd, fs, mb, ad, sd2));

    kdu_quality_limiter ql(0.03F, /*preserve_reversible=*/true);
    ql.set_display_resolution(300.0F, 300.0F);
    ql.set_comp_info(5, 2.0F, -1);
    EXPECT_LT(ql.get_square_visual_weight(LL_BAND, 1, kdu_coords(1, 1), false,
                                          /*reversible=*/true),
              0.0F);
    EXPECT_FLOAT_EQ(ql.get_square_visual_weight(LL_BAND, 1, kdu_coords(1, 1),
                                                false, /*reversible=*/false),
                    1.0F);
    EXPECT_GT(ql.get_square_visual_weight(HH_BAND, 1, kdu_coords(1, 1),
                                          /*is_chroma=*/true, false),
              0.0F);
    EXPECT_GT(ql.get_square_visual_weight(LH_BAND, 5, kdu_coords(4, 4),
                                          /*is_chroma=*/true, false),
              0.0F);
    kdu_quality_limiter ql_hi(0.03F, /*preserve_reversible=*/false);
    ql_hi.set_display_resolution(1200.0F, 1200.0F);
    EXPECT_GT(ql_hi.get_square_visual_weight(LH_BAND, 1, kdu_coords(1, 1),
                                             /*is_chroma=*/false, false),
              0.0F);
  }

  // 2. MCT + CRG + change_appearance + out-of-range component & unfinalized
  // query paths.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 3;
    spec.reversible = false;
    spec.dwt_levels = 2;
    spec.extra_params = {
        "Mcomponents=3",
        "Mprecision=8",
        "Msigned=no",
        "Mmatrix_size:I1=9",
        "Mmatrix_coeffs:I1=1.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,1.0",
        "Mstage_inputs:I1={0,2}",
        "Mstage_outputs:I1={0,2}",
        "Mstage_collections:I1={3,3}",
        "Mstage_xforms:I1={MATRIX,1,0,0,0}",
        "Mnum_stages=1",
        "Mstages=1",
    };
    const auto planes = MakeTestPlanes(spec);
    const auto bytes = EncodeImage(spec, planes);

    {
      MemorySource src(bytes);
      ScopedCodestream dec;
      dec.cs.create(&src);
      dec.cs.apply_input_restrictions(0, 0, 0, 0, nullptr,
                                      KDU_WANT_OUTPUT_COMPONENTS);
      dec.cs.change_appearance(/*transpose=*/true, /*vflip=*/true,
                               /*hflip=*/false);
      kdu_coords tidx;
      EXPECT_TRUE(dec.cs.find_tile(0, kdu_coords(0, 0), tidx, true));
      EXPECT_FALSE(dec.cs.find_tile(-1, kdu_coords(0, 0), tidx, true));
      EXPECT_FALSE(dec.cs.find_tile(99, kdu_coords(0, 0), tidx, true));
      EXPECT_FALSE(dec.cs.find_tile(99, kdu_coords(0, 0), tidx, false));
      EXPECT_FALSE(dec.cs.find_tile(0, kdu_coords(-10, -10), tidx, false));
    }

    {
      MemorySource src(bytes);
      ScopedCodestream dec;
      dec.cs.create(&src);
      dec.cs.apply_input_restrictions(0, 0, 0, 0, nullptr,
                                      KDU_WANT_OUTPUT_COMPONENTS);
      dec.cs.change_appearance(/*transpose=*/true, /*vflip=*/false,
                               /*hflip=*/false);
      kdu_coords subs;
      dec.cs.get_subsampling(0, subs, true);
      dec.cs.get_subsampling(-1, subs, true);
      dec.cs.get_subsampling(99, subs, true);
      dec.cs.get_subsampling(99, subs, false);
    }

    {
      MemorySource src(bytes);
      ScopedCodestream dec;
      dec.cs.create(&src);
      dec.cs.apply_input_restrictions(0, 0, 0, 0, nullptr,
                                      KDU_WANT_OUTPUT_COMPONENTS);
      dec.cs.change_appearance(/*transpose=*/true, /*vflip=*/false,
                               /*hflip=*/false);
      kdu_coords subs;
      dec.cs.get_min_subsampling(0, subs, true);
      dec.cs.get_min_subsampling(-1, subs, true);
      dec.cs.get_min_subsampling(99, subs, true);
      dec.cs.get_min_subsampling(99, subs, false);

      kdu_coords reg;
      dec.cs.get_registration(-1, kdu_coords(8, 8), reg, true);
      dec.cs.get_registration(99, kdu_coords(8, 8), reg, true);
      dec.cs.get_registration(99, kdu_coords(8, 8), reg, false);
      dec.cs.get_relative_registration(-1, 0, kdu_coords(8, 8), reg, true);
      dec.cs.get_relative_registration(99, 0, kdu_coords(8, 8), reg, true);
      dec.cs.get_relative_registration(99, 0, kdu_coords(8, 8), reg, false);
    }

    {
      MemorySource src(bytes);
      ScopedCodestream dec;
      dec.cs.create(&src);
      dec.cs.apply_input_restrictions(0, 0, 0, 0, nullptr,
                                      KDU_WANT_OUTPUT_COMPONENTS);
      dec.cs.change_appearance(/*transpose=*/true, /*vflip=*/false,
                               /*hflip=*/false);
      kdu_coords all_subs[8];
      EXPECT_EQ(dec.cs.get_res_subsamplings(-1, all_subs, 8, true), 0);
      EXPECT_EQ(dec.cs.get_res_subsamplings(99, all_subs, 8, true), 0);
      EXPECT_EQ(dec.cs.get_res_subsamplings(99, all_subs, 8, false), 0);
      EXPECT_GT(dec.cs.get_res_subsamplings(0, all_subs, 8, true), 0);

      kdu_dims tpart;
      dec.cs.get_tile_partition(tpart);
      EXPECT_FALSE(dec.cs.get_signed(99, false, false));

      kdu_dims creg;
      EXPECT_THROW(dec.cs.map_region(99, MakeDims(0, 0, 8, 8), creg, true),
                   kdu_exception);

      kdu_dims empty_crop = MakeDims(0, 0, 0, 0);
      dec.cs.apply_input_restrictions(0, 0, 0, 0, &empty_crop);
      kdu_dims valid_tiles;
      dec.cs.get_valid_tiles(valid_tiles);
      EXPECT_EQ(valid_tiles.size.x, 0);
      EXPECT_EQ(valid_tiles.size.y, 0);
    }
  }

  // 3. change_appearance errors, auto_flush with Creslengths &
  // thresholds+sizes, auto_trans_out warnings/defaults, add_flush_stats clamp
  // bounds, kd_flush_stats::advance before flush error, and timing/params
  // queries.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.dwt_levels = 2;
    spec.extra_params = {"Creslengths=10,20,30"};
    const auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    ScopedCodestream cs;
    cs.cs.create(&siz, &tgt);
    ConfigureCodestreamParams(cs.cs, spec);

    kdu_flush_stats fstats = cs.cs.add_flush_stats(0, 1, 0, -1);
    EXPECT_THROW(fstats.advance(1), kdu_exception);
    EXPECT_THROW((void)cs.cs.add_flush_stats(1, 300, 0, 300), kdu_exception);

    warning_handler_.clear();
    kdu_long lb[1] = {2048};
    kdu_uint16 th[1] = {45000};
    cs.cs.auto_flush(1, 1, -1, -1, lb, 1, th, /*trim_to_rate=*/true,
                     /*record_in_comseg=*/false, 0.0, nullptr,
                     KDU_FLUSH_USES_THRESHOLDS_AND_SIZES);
    th[0] = 44000;
    cs.cs.auto_flush(1, 1, 1, 1, lb, 1, th, /*trim_to_rate=*/true,
                     /*record_in_comseg=*/false, 0.0, nullptr,
                     KDU_FLUSH_USES_THRESHOLDS_AND_SIZES);
    EXPECT_FALSE(warning_handler_.history().empty());
    cs.cs.collect_timing_stats(-1);
    cs.cs.collect_timing_stats(1);

    kdu_tile tile = cs.cs.open_tile(kdu_coords(0, 0));
    EXPECT_THROW(cs.cs.change_appearance(true, false, false), kdu_exception);
    PushTileData(cs.cs, tile, planes, spec);
    tile.close();
    EXPECT_THROW(cs.cs.change_appearance(true, false, false), kdu_exception);
    cs.cs.flush(lb, 1, th);

    kdu_long samples = 0;
    (void)cs.cs.get_timing_stats(&samples, /*coder_only=*/true);
    EXPECT_GE(samples, 0);
    EXPECT_GE(cs.cs.get_params_memory(/*get_peak_allocation=*/false), 0);
    EXPECT_GE(cs.cs.get_num_tparts(), 1);
    EXPECT_EQ(cs.cs.get_ht_usage(), -1);
    EXPECT_EQ(cs.cs.get_max_tile_layers(), 1);

    // Second output codestream for add_flush_stats high clamp +
    // auto_trans_out with Creslengths warning.
    siz_params siz2;
    ConfigureSiz(siz2, spec);
    MemoryTarget tgt2;
    ScopedCodestream cs2;
    cs2.cs.create(&siz2, &tgt2);
    ConfigureCodestreamParams(cs2.cs, spec);
    kdu_flush_stats fs_hi = cs2.cs.add_flush_stats(0, 300, 0, 300);
    EXPECT_TRUE(fs_hi.exists());
    warning_handler_.clear();
    cs2.cs.auto_trans_out(1, 1, -1, -1, /*max_bytes=*/0, false);
    EXPECT_FALSE(warning_handler_.history().empty());
  }

  // 4. Vertical tiling Scbr model query + CBR write failure handling.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 128;
    spec.components = 1;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(64, 64);
    spec.extra_params = {"Corder=PCRL", "Cprecincts={16,16}", "Cblk={16,16}",
                         "Scbr={1,96}"};
    const auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    ScopedCodestream cs;
    cs.cs.create(&siz, &tgt);
    ConfigureCodestreamParams(cs.cs, spec);
    EXPECT_FLOAT_EQ(cs.cs.set_cbr_start_delay(10.0F), 10.0F);
    EXPECT_FLOAT_EQ(cs.cs.set_cbr_start_delay(2.0F), 10.0F);
    for (int ty = 0; ty < 2; ++ty) {
      kdu_tile tile = cs.cs.open_tile(kdu_coords(0, ty));
      PushTileData(cs.cs, tile, planes, spec);
      tile.close();
    }
    kdu_long budget_ok = 4096;
    cs.cs.flush(&budget_ok, 1);
    float sd = 0.0F;
    float mb = 0.0F;
    kdu_uint32 fs = 0;
    kdu_uint32 ad = 0;
    kdu_uint32 sd2 = 0;
    EXPECT_TRUE(cs.cs.get_cbr_model(sd, fs, mb, ad, sd2));

    MemoryTarget ftgt;
    ftgt.prefer_large = false;
    ftgt.fail_after_bytes = 20;
    siz_params siz_f;
    ConfigureSiz(siz_f, spec);
    ScopedCodestream csf;
    csf.cs.create(&siz_f, &ftgt);
    ConfigureCodestreamParams(csf.cs, spec);
    csf.cs.set_write_fail_policy(KDU_WRITE_FAIL_POLICY_ERROR);
    bool threw = false;
    try {
      for (int ty = 0; ty < 2; ++ty) {
        kdu_tile tile = csf.cs.open_tile(kdu_coords(0, ty));
        PushTileData(csf.cs, tile, planes, spec);
        tile.close();
      }
      kdu_long budget = 4096;
      csf.cs.flush(&budget, 1);
    } catch (kdu_exception) {
      threw = true;
    }
    EXPECT_TRUE(threw);
    csf.cs.set_write_fail_policy(KDU_WRITE_FAIL_POLICY_IGNORE);
  }

  // 5. PPM empty/truncated marker errors in transfer_tpart and ignore_tpart.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 1;
    spec.dwt_levels = 1;
    spec.tile_size = kdu_coords(32, 32);
    spec.extra_params = {"Cuse_sop=yes", "Cuse_eph=yes"};
    const auto planes = MakeTestPlanes(spec);
    const auto raw_bytes = EncodeImage(spec, planes);
    const auto ppm_bytes =
        ConvertJ2kSopEphToPackedHeaders(raw_bytes, /*use_ppm=*/true,
                                        /*split_markers=*/false);

    // Replace PPM marker segment with a 0-payload PPM segment so
    // transfer_tpart and ignore_tpart immediately hit empty-list errors.
    size_t ppm_pos = 0;
    kdu_uint16 ppm_len = 0;
    for (size_t i = 2; i + 4 < ppm_bytes.size(); ++i) {
      if (ppm_bytes[i] == 0xFF && ppm_bytes[i + 1] == 0x60) {
        ppm_pos = i;
        ppm_len =
            (static_cast<kdu_uint16>(ppm_bytes[i + 2]) << 8) | ppm_bytes[i + 3];
        break;
      }
    }
    ASSERT_GT(ppm_pos, 0);
    auto empty_ppm = ppm_bytes;
    empty_ppm.erase(empty_ppm.begin() + ppm_pos,
                    empty_ppm.begin() + ppm_pos + 2 + ppm_len);
    const std::vector<kdu_byte> zero_ppm_seg = {0xFF, 0x60, 0x00, 0x03, 0x00};
    empty_ppm.insert(empty_ppm.begin() + ppm_pos, zero_ppm_seg.begin(),
                     zero_ppm_seg.end());

    {
      MemorySource src(empty_ppm);
      ScopedCodestream dec;
      dec.cs.create(&src);
      EXPECT_THROW((void)dec.cs.open_tile(kdu_coords(0, 0)), kdu_exception);
    }
    {
      MemorySource src(empty_ppm, KDU_SOURCE_CAP_SEQUENTIAL);
      ScopedCodestream dec;
      dec.cs.create(&src);
      kdu_dims crop = MakeDims(32, 32, 16, 16);
      dec.cs.apply_input_restrictions(0, 0, 0, 0, &crop);
      EXPECT_THROW((void)dec.cs.open_tile(kdu_coords(1, 1)), kdu_exception);
    }

    // Overlarge Nppm + crop on sequential source triggers ignore_tpart error 9.
    auto bad_nppm = ppm_bytes;
    bad_nppm[ppm_pos + 5] = 0x7F;
    bad_nppm[ppm_pos + 6] = 0xFF;
    {
      MemorySource src(bad_nppm, KDU_SOURCE_CAP_SEQUENTIAL);
      ScopedCodestream dec;
      dec.cs.create(&src);
      kdu_dims crop = MakeDims(32, 32, 16, 16);
      dec.cs.apply_input_restrictions(0, 0, 0, 0, &crop);
      EXPECT_THROW((void)dec.cs.open_tile(kdu_coords(1, 1)), kdu_exception);
    }
  }

  // 6. kdu_pdescriptor_set with membroker, extract_tail leading_last_tile_part
  // recomputation, and reset(release_memory=true).
  {
    kdu_membroker broker;
    kdu_pdescriptor_set pset(&broker);
    EXPECT_TRUE(pset.init(kdu_coords(0, 0), 2));
    const int idx0 = pset.add_pdescriptor(0, 0, kdu_coords(0, 0));
    const int idx1 = pset.add_pdescriptor(0, 0, kdu_coords(1, 0));
    ASSERT_GE(idx0, 0);
    ASSERT_GE(idx1, 0);
    pset.access_pdescriptor(idx0)->last_tile_part = 2;
    pset.access_pdescriptor(idx1)->last_tile_part = 1;
    pset.note_pdescriptor_complete(idx0);
    pset.note_pdescriptor_complete(idx1);
    kdu_pdescriptor_set tail;
    EXPECT_TRUE(pset.extract_tail(&tail, 1));
    pset.reset(/*release_memory=*/true);
  }
}

TEST_F(CodestreamTest, AdditionalSingleThreadedUncoveredPaths) {
  // 1. kdu_codestream_comment null/binary/truncation/readonly/empty COM paths.
  {
    kdu_codestream_comment null_com;
    EXPECT_EQ(null_com.get_text(), nullptr);
    EXPECT_TRUE(null_com.check_readonly());
    const kdu_byte dummy[4] = {1, 2, 3, 4};
    EXPECT_FALSE(null_com.put_data(dummy, 4));
    EXPECT_FALSE(null_com.put_text("ignored"));

    ImageSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.components = 1;
    const auto planes = MakeTestPlanes(spec);
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt;
    ScopedCodestream cs;
    cs.cs.create(&siz, &tgt);
    ConfigureCodestreamParams(cs.cs, spec);

    kdu_codestream_comment bin_com = cs.cs.add_comment();
    EXPECT_FALSE(bin_com.check_readonly());
    EXPECT_TRUE(bin_com.put_data(dummy, 4));
    EXPECT_STREQ(bin_com.get_text(), "");
    EXPECT_FALSE(bin_com.put_text("cannot_mix_binary_and_text"));
    std::vector<kdu_byte> oversized(65600, 0x41);
    warning_handler_.clear();
    EXPECT_TRUE(
        bin_com.put_data(oversized.data(), static_cast<int>(oversized.size())));
    EXPECT_FALSE(warning_handler_.history().empty());
    kdu_byte out_buf[8] = {};
    EXPECT_EQ(bin_com.get_data(nullptr, 0, 4), 4);
    EXPECT_EQ(bin_com.get_data(out_buf, 100000, 4), 0);
    EXPECT_EQ(bin_com.get_data(out_buf, 0, 0), 0);
    EXPECT_EQ(bin_com.get_data(out_buf, 0, 4), 4);

    kdu_tile tile = cs.cs.open_tile(kdu_coords(0, 0));
    PushTileData(cs.cs, tile, planes, spec);
    tile.close();
    cs.cs.flush(nullptr, 1);
    EXPECT_TRUE(bin_com.check_readonly());
    EXPECT_FALSE(bin_com.put_data(dummy, 4));

    // Splice an empty binary COM marker (0xFFFE, Lcom=4, Rcom=0) before SOT.
    std::vector<kdu_byte> empty_com = {0xFF, 0xFE, 0x00, 0x04, 0x00, 0x00};
    auto with_empty_com = tgt.data;
    for (size_t i = 2; i + 1 < with_empty_com.size(); ++i) {
      if (with_empty_com[i] == 0xFF && with_empty_com[i + 1] == 0x90) {
        with_empty_com.insert(
            with_empty_com.begin() + static_cast<ptrdiff_t>(i),
            empty_com.begin(), empty_com.end());
        break;
      }
    }
    MemorySource src(with_empty_com);
    ScopedCodestream dec;
    dec.cs.create(&src);
    kdu_codestream_comment read_com;
    read_com = dec.cs.get_comment(read_com);
    EXPECT_TRUE(read_com.exists());
    EXPECT_STREQ(read_com.get_text(), "");
  }

  // 2. 2-stage MCT output->input augmentation, differing CRG incompatibility,
  // pre-finalization geometry queries, and component restriction edge cases.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.components = 3;
    spec.dwt_levels = 1;
    spec.reversible = false;
    // Verify augmented-output gap without fallback throws cleanly instead of
    // dereferencing a null subsampling_ref.
    ImageSpec bad_spec = spec;
    bad_spec.extra_params = {
        "CRGoffset={0.0,0.0},{0.5,0.5},{0.25,0.25}",
        "Mcomponents=3",
        "Mprecision=8",
        "Msigned=no",
        "Mmatrix_size:I1=6",
        "Mmatrix_coeffs:I1=0.5,0.5,0.0,0.5,-0.5,0.0",
        "Mvector_size:I1=2",
        "Mvector_coeffs:I1=0,0",
        "Mstage_inputs:I1={0,2}",
        "Mstage_outputs:I1={0,1}",
        "Mstage_collections:I1={3,2}",
        "Mstage_xforms:I1={MATRIX,1,1,0,0}",
        "Mmatrix_size:I2=4",
        "Mmatrix_coeffs:I2=1.0,0.25,0.0,1.0",
        "Mvector_size:I2=2",
        "Mvector_coeffs:I2=0,0",
        "Mstage_inputs:I2={0,1},{2,2}",
        "Mstage_outputs:I2={0,1},{2,2}",
        "Mstage_collections:I2={2,2},{1,1}",
        "Mstage_xforms:I2={MATRIX,2,2,0,0},{MATRIX,0,0,0,0}",
        "Mnum_stages=2",
        "Mstages=1,2",
    };
    const auto planes = MakeTestPlanes(spec);
    EXPECT_THROW((void)EncodeImage(bad_spec, planes), kdu_exception);

    // Now use invertible 3x3 blocks in Stage 0 and Stage 1 so encoding and
    // decoding succeed and exercise CRG incompatibility and CBD markers.
    spec.extra_params = {
        "CRGoffset={0.0,0.0},{0.5,0.5},{0.25,0.25}",
        "Mcomponents=3",
        "Mprecision=8",
        "Msigned=no",
        "Mmatrix_size:I1=9",
        "Mmatrix_coeffs:I1=0.5,0.5,0.0,0.5,-0.5,0.0,0.0,0.0,1.0",
        "Mvector_size:I1=3",
        "Mvector_coeffs:I1=0,0,0",
        "Mstage_inputs:I1={0,2}",
        "Mstage_outputs:I1={0,2}",
        "Mstage_collections:I1={3,3}",
        "Mstage_xforms:I1={MATRIX,1,1,0,0}",
        "Mmatrix_size:I2=9",
        "Mmatrix_coeffs:I2=1.0,0.25,0.0,0.0,1.0,0.0,0.0,0.0,1.0",
        "Mvector_size:I2=3",
        "Mvector_coeffs:I2=0,0,0",
        "Mstage_inputs:I2={0,2}",
        "Mstage_outputs:I2={0,2}",
        "Mstage_collections:I2={3,3}",
        "Mstage_xforms:I2={MATRIX,2,2,0,0}",
        "Mnum_stages=2",
        "Mstages=1,2",
    };
    const auto bytes = EncodeImage(spec, planes);

    {
      MemorySource src(bytes);
      ScopedCodestream dec;
      dec.cs.create(&src);
      dec.cs.set_persistent();
      EXPECT_GE(dec.cs.set_tile_unloading_threshold(-1), 0);

      // Call geometry & registration queries prior to finalize_construction().
      EXPECT_GE(dec.cs.get_min_dwt_levels(), 1);
      kdu_coords subs;
      dec.cs.get_subsampling(0, subs, true);
      dec.cs.get_min_subsampling(0, subs, true);
      kdu_coords reg;
      dec.cs.get_registration(0, kdu_coords(16, 16), reg, true);
      EXPECT_EQ(reg, kdu_coords(0, 0));
      dec.cs.get_relative_registration(0, 2, kdu_coords(16, 16), reg, true);
      EXPECT_EQ(reg, kdu_coords(0, 0));

      float overshoot = 0.0F;
      EXPECT_FALSE(dec.cs.report_synth_overshoot(99, overshoot));
      EXPECT_FALSE(dec.cs.report_synth_overshoot(0, overshoot));

      // Exercise both comps_of_interest==NULL and non-NULL MCT restrictions.
      dec.cs.apply_input_restrictions(0, 2, 0, 0, nullptr,
                                      KDU_WANT_OUTPUT_COMPONENTS);
      int indices[] = {-1, 0, 0, 99, 2};
      dec.cs.apply_input_restrictions(5, indices, 0, 0, nullptr,
                                      KDU_WANT_OUTPUT_COMPONENTS);
      {
        int coi[] = {-1, 0, 99};
        kdu_tile tile = dec.cs.open_tile(kdu_coords(0, 0));
        tile.set_components_of_interest(3, coi);
        DecodeTileAllComponents(dec.cs, tile);
        tile.close();
      }
      dec.cs.apply_input_restrictions(5, indices, 0, 0, nullptr,
                                      KDU_WANT_CODESTREAM_COMPONENTS);
    }

    // Splice a COM marker before CBD on a sequential (non-seekable) source
    // to exercise read_siz_auxiliary_marker_segs non-seekable warning.
    const std::vector<kdu_byte> com_seg = {0xFF, 0xFE, 0x00, 0x05,
                                           0x00, 0x00, 0x41};
    // Find CBD (0xFF 0x78) in the main header before first SOT.
    std::vector<kdu_byte> modified = bytes;
    for (size_t i = 2; i + 1 < modified.size(); ++i) {
      if (modified[i] == 0xFF && modified[i + 1] == 0x78) {
        modified.insert(modified.begin() + static_cast<ptrdiff_t>(i),
                        com_seg.begin(), com_seg.end());
        break;
      }
      if (modified[i] == 0xFF && modified[i + 1] == 0x90) {
        break;
      }
    }
    warning_handler_.clear();
    MemorySource seq_src(modified, KDU_SOURCE_CAP_SEQUENTIAL);
    ScopedCodestream seq_dec;
    seq_dec.cs.create(&seq_src);
    EXPECT_FALSE(warning_handler_.history().empty());
  }

  // 3. Multi-frame CBR flush_stats + aux_flush_stats + link_tile_comp +
  // write_packets_cbr trailing precincts + tiny Scbr error.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 128;
    spec.components = 1;
    spec.dwt_levels = 2;
    spec.tile_size = kdu_coords(64, 64);
    spec.extra_params = {"Corder=PCRL", "Cprecincts={16,16}", "Cblk={16,16}",
                         "Scbr={1,96}"};
    const auto planes = MakeTestPlanes(spec);

    // Companion codestream providing valid aux_flush_stats.
    siz_params siz_aux;
    ConfigureSiz(siz_aux, spec);
    MemoryTarget tgt_aux;
    ScopedCodestream cs_aux;
    cs_aux.cs.create(&siz_aux, &tgt_aux);
    ConfigureCodestreamParams(cs_aux.cs, spec);
    kdu_flush_stats fs_aux = cs_aux.cs.add_flush_stats(0, 1, 0, 1);
    for (int ty = 0; ty < 2; ++ty) {
      kdu_tile tile = cs_aux.cs.open_tile(kdu_coords(0, ty));
      PushTileData(cs_aux.cs, tile, planes, spec);
      tile.close();
    }
    kdu_long aux_budget = 4096;
    cs_aux.cs.flush(&aux_budget, 1);
    fs_aux.auto_advance(2);

    // Primary CBR codestream across frame 1 -> restart -> frame 3 -> mismatch.
    siz_params siz;
    ConfigureSiz(siz, spec);
    MemoryTarget tgt0;
    ScopedCodestream cs;
    cs.cs.create(&siz, &tgt0);
    ConfigureCodestreamParams(cs.cs, spec);
    cs.cs.enable_restart();
    kdu_flush_stats fs = cs.cs.add_flush_stats(1, 1, 0, 1);
    cs.cs.attach_flush_stats(fs_aux);
    for (int ty = 0; ty < 2; ++ty) {
      kdu_tile tile = cs.cs.open_tile(kdu_coords(0, ty));
      PushTileData(cs.cs, tile, planes, spec);
      tile.close();
    }
    kdu_long budget = 8192;
    cs.cs.flush(&budget, 1);

    MemoryTarget tgt1;
    cs.cs.restart(&tgt1);
    EXPECT_TRUE(fs.advance(2));
    for (int ty = 0; ty < 2; ++ty) {
      kdu_tile tile = cs.cs.open_tile(kdu_coords(0, ty));
      PushTileData(cs.cs, tile, planes, spec);
      tile.close();
    }
    budget = 16384;
    cs.cs.flush(&budget, 1);

    // Mismatched precinct structure across restart triggers link_tile_comp
    // check.
    MemoryTarget tgt2;
    cs.cs.restart(&tgt2);
    EXPECT_TRUE(fs.advance(2));
    EXPECT_THROW(
        {
          if (cs.cs.access_siz()->parse_string("Cprecincts:T0={32,32}")) {
            cs.cs.access_siz()->finalize_all();
          }
          kdu_tile bad_tile = cs.cs.open_tile(kdu_coords(0, 0));
          bad_tile.close();
        },
        kdu_exception);

    // Flat zero image with large CBR budget hits
    // global_slope_included_everything and secondary bulk/fine fill +
    // min_slope_threshold.
    ImageSpec zspec;
    zspec.width = 64;
    zspec.height = 128;
    zspec.components = 1;
    zspec.dwt_levels = 1;
    zspec.tile_size = kdu_coords(64, 128);
    zspec.extra_params = {"Corder=PCRL", "Cprecincts={16,16}", "Cblk={16,16}",
                          "Scbr={1,96}"};
    std::vector<std::vector<kdu_int32>> zero_planes(
        1, std::vector<kdu_int32>(64 * 128, 0));
    siz_params zsiz;
    ConfigureSiz(zsiz, zspec);
    MemoryTarget ztgt;
    ScopedCodestream zcs;
    zcs.cs.create(&zsiz, &ztgt);
    ConfigureCodestreamParams(zcs.cs, zspec);
    zcs.cs.set_min_slope_threshold(40000);
    {
      kdu_tile tile = zcs.cs.open_tile(kdu_coords(0, 0));
      PushTileData(zcs.cs, tile, zero_planes, zspec);
      tile.close();
    }
    kdu_long big_budget = 24000;
    zcs.cs.flush(&big_budget, 1);

    // Tiny CBR leaky bucket throws error 0x29081603.
    ImageSpec bspec = zspec;
    bspec.extra_params = {"Corder=PCRL", "Cprecincts={16,16}", "Cblk={16,16}",
                          "Scbr={1,16}"};
    siz_params bsiz;
    ConfigureSiz(bsiz, bspec);
    MemoryTarget btgt;
    ScopedCodestream bcs;
    bcs.cs.create(&bsiz, &btgt);
    ConfigureCodestreamParams(bcs.cs, bspec);
    {
      kdu_tile tile = bcs.cs.open_tile(kdu_coords(0, 0));
      PushTileData(bcs.cs, tile, planes, bspec);
      tile.close();
    }
    kdu_long tiny_budget = 256;
    EXPECT_THROW(bcs.cs.flush(&tiny_budget, 1), kdu_exception);
  }

  // 4. load_sequential exhaustion transitions, PRCL mismatched DWT levels,
  // and PPT (>512 bytes) + resilient layer skip / marker injection.
  {
    ImageSpec spec;
    spec.width = 64;
    spec.height = 32;
    spec.components = 1;
    spec.dwt_levels = 0;
    spec.tile_size = kdu_coords(32, 32);
    spec.extra_params = {"Corder=LRCP", "Clayers=2", "Cprecincts={32,32}"};
    const auto planes = MakeTestPlanes(spec);
    auto encode_layers = [](const ImageSpec& s,
                            const std::vector<std::vector<kdu_int32>>& p,
                            const std::vector<kdu_long>& budgets) {
      siz_params lsiz;
      ConfigureSiz(lsiz, s);
      MemoryTarget ltgt;
      ScopedCodestream lcs;
      lcs.cs.create(&lsiz, &ltgt);
      ConfigureCodestreamParams(lcs.cs, s);
      kdu_dims tr;
      lcs.cs.get_valid_tiles(tr);
      for (int ty = 0; ty < tr.size.y; ++ty) {
        for (int tx = 0; tx < tr.size.x; ++tx) {
          kdu_tile tile =
              lcs.cs.open_tile(kdu_coords(tr.pos.x + tx, tr.pos.y + ty));
          PushTileData(lcs.cs, tile, p, s);
          tile.close();
        }
      }
      std::vector<kdu_long> b = budgets;
      lcs.cs.flush(b.data(), static_cast<int>(b.size()));
      return ltgt.data;
    };
    const auto bytes = encode_layers(spec, planes, {200, 600});

    // Truncated stream in load_sequential so tile 0 finishes as incomplete.
    {
      std::vector<kdu_byte> trunc(bytes.begin(),
                                  bytes.begin() + bytes.size() / 2);
      MemorySource src(trunc);
      ScopedCodestream dec;
      dec.cs.create(&src);
      dec.cs.set_resilient();
      bool tile_done = false;
      kdu_pdescriptor_set pset1;
      kdu_tile t1 = dec.cs.load_sequential(
          &pset1, tile_done, 10, 100, KDU_LOAD_SEQUENTIAL_WANT_TILE, nullptr);
      if (t1.exists()) t1.close();
      kdu_pdescriptor_set pset2;
      kdu_tile t2 =
          dec.cs.load_sequential(&pset2, tile_done, 10, 100, 0, nullptr);
      if (t2.exists()) t2.close();
    }

    // Resume non-empty pset from tile 0 across boundary into tile 1.
    {
      MemorySource src(bytes);
      ScopedCodestream dec;
      dec.cs.create(&src);
      bool tile_done = false;
      kdu_pdescriptor_set pset_t0;
      kdu_tile t0 =
          dec.cs.load_sequential(&pset_t0, tile_done, 1, 1, 0, nullptr);
      ASSERT_TRUE(t0.exists());
      t0.close();
      EXPECT_FALSE(pset_t0.is_empty());
      // Continue loading with pset_t0 still bound to tile 0 into tile 1.
      kdu_tile t1 =
          dec.cs.load_sequential(&pset_t0, tile_done, 10, 100, 0, nullptr);
      if (t1.exists()) t1.close();
      EXPECT_EQ(pset_t0.get_tile_idx(), kdu_coords(0, 0));
    }

    // Non-contributing precinct inside load_sequential via component
    // restriction.
    {
      ImageSpec cspec = spec;
      cspec.components = 2;
      const auto cplanes = MakeTestPlanes(cspec);
      const auto cbytes = EncodeImage(cspec, cplanes);
      MemorySource src(cbytes);
      ScopedCodestream dec;
      dec.cs.create(&src);
      dec.cs.apply_input_restrictions(0, 1, 0, 0, nullptr,
                                      KDU_WANT_OUTPUT_COMPONENTS);
      bool tile_done = false;
      kdu_pdescriptor_set pset;
      kdu_tile t = dec.cs.load_sequential(&pset, tile_done, 1, 1, 0, nullptr);
      EXPECT_TRUE(t.exists());
      if (t.exists()) t.close();
    }

    // Corder=PRCL with per-component Clevels mismatch.
    {
      ImageSpec pspec;
      pspec.width = 64;
      pspec.height = 64;
      pspec.components = 2;
      pspec.dwt_levels = 2;
      pspec.extra_params = {"Corder=PRCL", "Clevels:C1=0", "Cprecincts={16,16}",
                            "Cblk={16,16}"};
      const auto pplanes = MakeTestPlanes(pspec);
      const auto pbytes = EncodeImage(pspec, pplanes);
      MemorySource src(pbytes);
      ScopedCodestream dec;
      dec.cs.create(&src);
      kdu_tile tile = dec.cs.open_tile(kdu_coords(0, 0));
      DecodeTileAllComponents(dec.cs, tile);
      tile.close();
    }

    // Large PPT (>512 bytes) + set_resilient() + max_layers=1 + unexpected
    // marker to cover kd_pph_input::load_buf, kd_input::ignore(throw_markers),
    // and process_unexpected_marker.
    {
      ImageSpec lspec;
      lspec.width = 128;
      lspec.height = 128;
      lspec.components = 1;
      lspec.dwt_levels = 2;
      lspec.extra_params = {"Clayers=3", "Cprecincts={16,16}", "Cblk={16,16}",
                            "Cuse_sop=yes", "Cuse_eph=yes"};
      const auto lplanes = MakeTestPlanes(lspec);
      const auto raw = encode_layers(lspec, lplanes, {400, 900, 1800});
      const auto ppt_bytes =
          ConvertJ2kSopEphToPackedHeaders(raw, /*use_ppm=*/false,
                                          /*split_markers=*/false);
      MemorySource src(ppt_bytes, KDU_SOURCE_CAP_SEQUENTIAL);
      ScopedCodestream dec;
      dec.cs.create(&src);
      dec.cs.set_resilient();
      dec.cs.apply_input_restrictions(0, 0, 0, 1, nullptr,
                                      KDU_WANT_OUTPUT_COMPONENTS);
      kdu_tile tile = dec.cs.open_tile(kdu_coords(0, 0));
      DecodeTileAllComponents(dec.cs, tile);
      tile.close();

      // Inject an unexpected 0xFF 0x90 marker near the end of the packet body
      // in resilient sequential decode mode.
      if (ppt_bytes.size() > 64) {
        auto corrupt = ppt_bytes;
        const size_t pos = corrupt.size() - 32;
        corrupt[pos] = 0xFF;
        corrupt[pos + 1] = 0x90;
        MemorySource csrc(corrupt, KDU_SOURCE_CAP_SEQUENTIAL);
        ScopedCodestream cdec;
        cdec.cs.create(&csrc);
        cdec.cs.set_resilient();
        try {
          kdu_tile ctile = cdec.cs.open_tile(kdu_coords(0, 0));
          DecodeTileAllComponents(cdec.cs, ctile);
          ctile.close();
        } catch (kdu_exception) {
        }
      }

      // Non-PPT sequential decode with max_layers=1 + SOP/EPH and spliced
      // 0xFF 0x90 in packet body exercises kd_input::read/ignore throw_markers.
      {
        MemorySource rsrc(raw, KDU_SOURCE_CAP_SEQUENTIAL);
        ScopedCodestream rdec;
        rdec.cs.create(&rsrc);
        rdec.cs.set_resilient();
        rdec.cs.apply_input_restrictions(0, 0, 0, 1, nullptr,
                                         KDU_WANT_OUTPUT_COMPONENTS);
        kdu_tile rtile = rdec.cs.open_tile(kdu_coords(0, 0));
        DecodeTileAllComponents(rdec.cs, rtile);
        rtile.close();
      }
      if (raw.size() > 64) {
        auto rcorrupt = raw;
        const size_t pos = rcorrupt.size() - 32;
        rcorrupt[pos] = 0xFF;
        rcorrupt[pos + 1] = 0x90;
        MemorySource rcsrc(rcorrupt, KDU_SOURCE_CAP_SEQUENTIAL);
        ScopedCodestream rcdec;
        rcdec.cs.create(&rcsrc);
        rcdec.cs.set_resilient();
        try {
          kdu_tile rctile = rcdec.cs.open_tile(kdu_coords(0, 0));
          DecodeTileAllComponents(rcdec.cs, rctile);
          rctile.close();
        } catch (kdu_exception) {
        }
      }
    }

    // 5. Additional single-threaded CBR, Porder, and load_sequential paths.
    {
      // Huge CBR fill bytes over 1 codeblock triggers KDU_ERROR 0x29081604.
      ImageSpec hspec;
      hspec.width = 64;
      hspec.height = 64;
      hspec.components = 1;
      hspec.dwt_levels = 0;
      hspec.extra_params = {"Corder=PCRL", "Cprecincts={64,64}", "Cblk={64,64}",
                            "Scbr={1,96}"};
      std::vector<std::vector<kdu_int32>> hzeros(
          1, std::vector<kdu_int32>(64 * 64, 0));
      siz_params hsiz;
      ConfigureSiz(hsiz, hspec);
      MemoryTarget htgt;
      ScopedCodestream hcs;
      hcs.cs.create(&hsiz, &htgt);
      ConfigureCodestreamParams(hcs.cs, hspec);
      {
        kdu_tile tile = hcs.cs.open_tile(kdu_coords(0, 0));
        PushTileData(hcs.cs, tile, hzeros, hspec);
        tile.close();
      }
      kdu_long huge_budget = 100000;
      try {
        hcs.cs.flush(&huge_budget, 1);
      } catch (kdu_exception) {
      }

      // Tight CBR budget across multi-precinct zero image to leave trailing
      // empty precincts after fill bytes are exhausted.
      ImageSpec tspec;
      tspec.width = 64;
      tspec.height = 128;
      tspec.components = 1;
      tspec.dwt_levels = 1;
      tspec.extra_params = {"Corder=PCRL", "Cprecincts={16,16}", "Cblk={16,16}",
                            "Scbr={1,96}"};
      std::vector<std::vector<kdu_int32>> tzeros(
          1, std::vector<kdu_int32>(64 * 128, 0));
      siz_params tsiz;
      ConfigureSiz(tsiz, tspec);
      MemoryTarget ttgt;
      ScopedCodestream tcs;
      tcs.cs.create(&tsiz, &ttgt);
      ConfigureCodestreamParams(tcs.cs, tspec);
      {
        kdu_tile tile = tcs.cs.open_tile(kdu_coords(0, 0));
        PushTileData(tcs.cs, tile, tzeros, tspec);
        tile.close();
      }
      kdu_long tight_budget = 600;
      tcs.cs.flush(&tight_budget, 1);

      // Incremental CBR flush across two full-tile flush sets hits
      // cbr_fset.num_flush_sets > 0 slope estimation on second flush.
      ImageSpec ispec;
      ispec.width = 64;
      ispec.height = 128;
      ispec.components = 1;
      ispec.dwt_levels = 2;
      ispec.tile_size = kdu_coords(64, 64);
      ispec.extra_params = {"Corder=PCRL", "Cprecincts={16,16}", "Cblk={16,16}",
                            "Scbr={1,96}"};
      const auto iplanes = MakeTestPlanes(ispec);
      siz_params isiz;
      ConfigureSiz(isiz, ispec);
      MemoryTarget itgt;
      ScopedCodestream ics;
      ics.cs.create(&isiz, &itgt);
      ConfigureCodestreamParams(ics.cs, ispec);
      for (int ty = 0; ty < 2; ++ty) {
        kdu_tile tile = ics.cs.open_tile(kdu_coords(0, ty));
        PushTileData(ics.cs, tile, iplanes, ispec);
        tile.close();
        kdu_long step_budget = 2048;
        ics.cs.flush(&step_budget, 1);
      }

      // Incomplete Porder triggers KDU_ERROR 24 on encode.
      ImageSpec pspec;
      pspec.width = 32;
      pspec.height = 32;
      pspec.components = 1;
      pspec.dwt_levels = 2;
      pspec.extra_params = {"Porder={0,0,1,1,1,LRCP}"};
      const auto pplanes = MakeTestPlanes(pspec);
      EXPECT_THROW((void)EncodeImage(pspec, pplanes), kdu_exception);

      // Two-tile stream truncated right inside Tile 1 SOD exercises
      // load_sequential exhausted-tile transitions for empty and mismatched
      // pset.
      size_t second_sot = 0;
      for (size_t i = 2; i + 1 < bytes.size(); ++i) {
        if (bytes[i] == 0xFF && bytes[i + 1] == 0x90) {
          if (second_sot == 0) {
            second_sot = 1;
          } else {
            second_sot = i;
            break;
          }
        }
      }
      if (second_sot > 1 && second_sot + 16 < bytes.size()) {
        std::vector<kdu_byte> trunc2(bytes.begin(),
                                     bytes.begin() + second_sot + 16);
        MemorySource s1(trunc2);
        ScopedCodestream d1;
        d1.cs.create(&s1);
        d1.cs.set_resilient();
        bool done = false;
        kdu_pdescriptor_set pset_t0;
        kdu_tile a = d1.cs.load_sequential(&pset_t0, done, 1, 20, 0, nullptr);
        if (a.exists()) a.close();
        kdu_pdescriptor_set pset_mid;
        kdu_tile b =
            d1.cs.load_sequential(&pset_mid, done, 1, 10000, 0, nullptr);
        if (b.exists()) b.close();
        kdu_tile c = d1.cs.load_sequential(&pset_t0, done, 1, 1000, 0, nullptr);
        if (c.exists()) c.close();
        kdu_pdescriptor_set pset_empty;
        kdu_tile d =
            d1.cs.load_sequential(&pset_empty, done, 1, 1000, 0, nullptr);
        if (d.exists()) d.close();
      }
      // Also test WANT_TILE stopping cleanly at Tile 0 boundary so Tile 1
      // enters load_sequential with tile == NULL -> exhausted.
      if (second_sot > 1 && second_sot + 16 < bytes.size()) {
        std::vector<kdu_byte> trunc2(bytes.begin(),
                                     bytes.begin() + second_sot + 16);
        for (int pass = 0; pass < 2; ++pass) {
          MemorySource s(trunc2);
          ScopedCodestream d;
          d.cs.create(&s);
          d.cs.set_resilient();
          bool done = false;
          kdu_pdescriptor_set pset_t0;
          kdu_tile t0 = d.cs.load_sequential(
              &pset_t0, done, 1, 10000, KDU_LOAD_SEQUENTIAL_WANT_TILE, nullptr);
          if (t0.exists()) t0.close();
          kdu_pdescriptor_set pset_fresh;
          kdu_tile t1 = d.cs.load_sequential(pass == 0 ? &pset_t0 : &pset_fresh,
                                             done, 1, 1000, 0, nullptr);
          if (t1.exists()) t1.close();
        }
        // Single-tile truncated after packet 0 so Call 2 resumes same tile
        // at EOF with !pset->is_empty() and pset->get_tile_idx() == t_idx.
        std::vector<kdu_byte> trunc0(bytes.begin(),
                                     bytes.begin() + second_sot - 2);
        MemorySource s0(trunc0);
        ScopedCodestream d0;
        d0.cs.create(&s0);
        d0.cs.set_resilient();
        bool done0 = false;
        kdu_pdescriptor_set pset0;
        kdu_tile ta = d0.cs.load_sequential(&pset0, done0, 1, 15, 0, nullptr);
        if (ta.exists()) ta.close();
        kdu_tile tb = d0.cs.load_sequential(&pset0, done0, 1, 1000, 0, nullptr);
        if (tb.exists()) tb.close();
      }
    }

    // 6. Targeted small single-threaded blocks: sample allocator limits,
    // IN_MEMORY max_bytes seek clamping, COM length edge cases, malformed QCD
    // cleanup, PLT final-tpart Psot=0, TLM write failures, extract_tail,
    // truncated SOP at EOF, and asymmetric CBR precincts.
    {
      // kd_sample_allocator::configure outstanding permit rejection & finalize
      // size_t overflow paths.
      kdu_membroker tiny_broker(512);
      kdu_sample_allocator palloc;
      palloc.seek_permit(2048);
      error_handler_.clear();
      EXPECT_THROW(palloc.configure(&tiny_broker, 18), kdu_exception);
      palloc.release_permit(2048);

      MemorySource dummy_src(bytes);
      ScopedCodestream dummy_cs;
      dummy_cs.cs.create(&dummy_src);

      kdu_sample_allocator pre_over_alloc;
      ASSERT_TRUE(pre_over_alloc.configure(&tiny_broker, 18));
      pre_over_alloc.pre_alloc_block(1);
      pre_over_alloc.pre_alloc_block(std::numeric_limits<size_t>::max());
      error_handler_.clear();
      EXPECT_THROW(pre_over_alloc.finalize(dummy_cs.cs), kdu_exception);

      kdu_sample_allocator over_alloc;
      ASSERT_TRUE(over_alloc.configure(&tiny_broker, 18));
      const size_t half_plus =
          (std::numeric_limits<size_t>::max() / 2) + (size_t{1} << 20);
      over_alloc.pre_alloc_block(half_plus);
      over_alloc.pre_alloc_block(half_plus);
      error_handler_.clear();
      EXPECT_THROW(over_alloc.finalize(dummy_cs.cs), kdu_exception);

      // KDU_SOURCE_CAP_IN_MEMORY + set_max_bytes + seek beyond limit.
      {
        MemorySource msrc(bytes);
        ScopedCodestream mdec;
        mdec.cs.create(&msrc);
        mdec.cs.set_max_bytes(static_cast<kdu_long>(bytes.size() / 2));
        try {
          kdu_dims tr;
          mdec.cs.get_valid_tiles(tr);
          for (int tx = 0; tx < tr.size.x; ++tx) {
            kdu_tile mt = mdec.cs.open_tile(kdu_coords(tx, 0));
            mt.close();
          }
        } catch (kdu_exception) {
        }
      }

      // Empty binary COM (len=0, is_text=false) and 65531-byte COM (>65530).
      {
        size_t first_sot = 0;
        for (size_t i = 2; i + 1 < bytes.size(); ++i) {
          if (bytes[i] == 0xFF && bytes[i + 1] == 0x90) {
            first_sot = i;
            break;
          }
        }
        if (first_sot > 2) {
          std::vector<kdu_byte> big_com = bytes;
          std::vector<kdu_byte> markers = {0xFF, 0xFE, 0x00, 0x04, 0x00, 0x00,
                                           0xFF, 0xFE, 0xFF, 0xFF, 0x00, 0x00};
          markers.insert(markers.end(), 65531, 0x42);
          big_com.insert(big_com.begin() + static_cast<ptrdiff_t>(first_sot),
                         markers.begin(), markers.end());
          MemorySource bsrc(big_com);
          ScopedCodestream bdec;
          bdec.cs.create(&bsrc);
          kdu_codestream_comment c =
              bdec.cs.get_comment(kdu_codestream_comment());
          EXPECT_TRUE(c.exists());
          c = bdec.cs.get_comment(c);
          EXPECT_TRUE(c.exists());

          // Malformed short QCD spliced before first SOT triggers
          // construct_common catch cleanup.
          std::vector<kdu_byte> bad_qcd = bytes;
          const kdu_byte qcd[] = {0xFF, 0x5C, 0x00, 0x03, 0x00};
          bad_qcd.insert(bad_qcd.begin() + static_cast<ptrdiff_t>(first_sot),
                         qcd, qcd + sizeof(qcd));
          MemorySource qsrc(bad_qcd);
          ScopedCodestream qdec;
          error_handler_.clear();
          EXPECT_THROW(qdec.cs.create(&qsrc), kdu_exception);
        }
      }

      // ORGgen_plt=yes with final SOT Psot zeroed
      // (final_tpart_with_unknown_length).
      {
        ImageSpec plt_spec;
        plt_spec.width = 64;
        plt_spec.height = 32;
        plt_spec.components = 1;
        plt_spec.dwt_levels = 1;
        plt_spec.tile_size = kdu_coords(32, 32);
        plt_spec.extra_params = {"ORGgen_plt=yes"};
        const auto plt_planes = MakeTestPlanes(plt_spec);
        auto plt_bytes = EncodeImage(plt_spec, plt_planes);
        size_t last_sot = 0;
        for (size_t i = 2; i + 10 < plt_bytes.size(); ++i) {
          if (plt_bytes[i] == 0xFF && plt_bytes[i + 1] == 0x90) {
            last_sot = i;
          }
        }
        if (last_sot > 0) {
          plt_bytes[last_sot + 6] = 0;
          plt_bytes[last_sot + 7] = 0;
          plt_bytes[last_sot + 8] = 0;
          plt_bytes[last_sot + 9] = 0;
          MemorySource psrc(plt_bytes);
          ScopedCodestream pdec;
          pdec.cs.create(&psrc);
          for (int tx = 0; tx < 2; ++tx) {
            kdu_tile tile = pdec.cs.open_tile(kdu_coords(tx, 0));
            DecodeTileAllComponents(pdec.cs, tile);
            tile.close();
          }
        }
      }

      // ORGgen_tlm=2 write failures (sequential & rewrite paths in
      // kd_tlm_generator).
      {
        ImageSpec tlm_spec;
        tlm_spec.width = 64;
        tlm_spec.height = 32;
        tlm_spec.components = 1;
        tlm_spec.dwt_levels = 1;
        tlm_spec.tile_size = kdu_coords(32, 32);
        tlm_spec.extra_params = {"ORGgen_tlm=2"};
        const auto tlm_planes = MakeTestPlanes(tlm_spec);

        class ThrowingTlmTarget : public MemoryTarget {
         public:
          bool throw_after_fail = false;
          bool throw_on_rewrite = false;
          bool in_rewrite = false;
          bool start_rewrite(kdu_long backtrack) override {
            in_rewrite = true;
            return MemoryTarget::start_rewrite(backtrack);
          }
          bool end_rewrite() override {
            in_rewrite = false;
            return MemoryTarget::end_rewrite();
          }
          bool write(const kdu_byte* buf, int num_bytes) override {
            if (throw_on_rewrite && in_rewrite) {
              throw_on_rewrite = false;
              throw static_cast<kdu_exception>(KDU_ERROR_EXCEPTION);
            }
            if (!MemoryTarget::write(buf, num_bytes)) {
              if (throw_after_fail) {
                throw_after_fail = false;
                throw static_cast<kdu_exception>(KDU_ERROR_EXCEPTION);
              }
              return false;
            }
            return true;
          }
        };

        auto flush_expect_write_fail = [&](ThrowingTlmTarget& tgt) {
          siz_params tsiz;
          ConfigureSiz(tsiz, tlm_spec);
          ScopedCodestream tcs;
          tcs.cs.create(&tsiz, &tgt);
          tcs.cs.set_write_fail_policy(KDU_WRITE_FAIL_POLICY_ERROR);
          ConfigureCodestreamParams(tcs.cs, tlm_spec);
          kdu_dims tvalid;
          tcs.cs.get_valid_tiles(tvalid);
          for (int ty = 0; ty < tvalid.size.y; ++ty) {
            for (int tx = 0; tx < tvalid.size.x; ++tx) {
              kdu_tile tile = tcs.cs.open_tile(kdu_coords(tx, ty));
              PushTileData(tcs.cs, tile, tlm_planes, tlm_spec);
              tile.close();
            }
          }
          error_handler_.clear();
          EXPECT_THROW(tcs.cs.flush(nullptr, 1), kdu_exception);
          tgt.throw_after_fail = false;
          tgt.throw_on_rewrite = false;
          tcs.cs.set_write_fail_policy(KDU_WRITE_FAIL_POLICY_IGNORE);
        };

        ThrowingTlmTarget seq_fail;
        seq_fail.prefer_large = false;
        seq_fail.allow_rewrite = false;
        seq_fail.throw_after_fail = true;
        seq_fail.fail_after_bytes = 45;
        flush_expect_write_fail(seq_fail);

        ThrowingTlmTarget rew_fail;
        rew_fail.prefer_large = false;
        rew_fail.allow_rewrite = true;
        rew_fail.throw_on_rewrite = true;
        flush_expect_write_fail(rew_fail);
      }

      // ORGtparts=R + load_sequential + extract_tail with
      // leading_last_tile_part > 0.
      {
        ImageSpec tp_spec;
        tp_spec.width = 64;
        tp_spec.height = 64;
        tp_spec.components = 1;
        tp_spec.dwt_levels = 2;
        tp_spec.extra_params = {"Cprecincts={16,16}", "ORGtparts=R"};
        const auto tp_planes = MakeTestPlanes(tp_spec);
        const auto tp_bytes = EncodeImage(tp_spec, tp_planes);
        MemorySource tpsrc(tp_bytes);
        ScopedCodestream tpdec;
        tpdec.cs.create(&tpsrc);
        bool done = false;
        kdu_pdescriptor_set pset;
        kdu_tile tile = tpdec.cs.load_sequential(
            &pset, done, 1, static_cast<int>(tp_bytes.size()),
            KDU_LOAD_SEQUENTIAL_WANT_TILE, nullptr);
        if (tile.exists()) tile.close();
        kdu_pdescriptor_set tail;
        EXPECT_TRUE(pset.extract_tail(&tail, 1));
      }

      // Truncated SOP marker {0xFF, 0x91, 0x00} at EOF on Cuse_sop=yes
      // sequential resilient stream exercises process_unexpected_marker
      // partial-length putback.
      {
        ImageSpec sop_spec;
        sop_spec.width = 64;
        sop_spec.height = 64;
        sop_spec.components = 1;
        sop_spec.dwt_levels = 2;
        sop_spec.extra_params = {"Cprecincts={16,16}", "Cuse_sop=yes",
                                 "Cuse_eph=yes"};
        auto sop_eof = EncodeImage(sop_spec, MakeTestPlanes(sop_spec));
        if (sop_eof.size() > 8) {
          sop_eof[sop_eof.size() - 2] = 0xFF;
          sop_eof[sop_eof.size() - 1] = 0x91;
          sop_eof.push_back(0x00);
          MemorySource ssrc(sop_eof, KDU_SOURCE_CAP_SEQUENTIAL);
          ScopedCodestream sdec;
          sdec.cs.create(&ssrc);
          sdec.cs.set_resilient();
          try {
            kdu_tile tile = sdec.cs.open_tile(kdu_coords(0, 0));
            DecodeTileAllComponents(sdec.cs, tile);
            tile.close();
          } catch (kdu_exception) {
          }
        }
      }

      // 2-component CBR with subsampled second component and Clevels:C1=0 so
      // trailing precincts remain after secondary_bulk_fill_bytes hits zero.
      {
        ImageSpec cspec;
        cspec.width = 64;
        cspec.height = 64;
        cspec.components = 2;
        cspec.dwt_levels = 2;
        cspec.subsampling = {kdu_coords(1, 1), kdu_coords(2, 2)};
        cspec.extra_params = {"Cycc=no",      "Clevels:C1=0",
                              "Corder=PCRL",  "Cprecincts={16,16}",
                              "Cblk={16,16}", "Scbr={1,96}"};
        std::vector<std::vector<kdu_int32>> cplanes(2);
        cplanes[0].assign(64 * 64, 10);
        cplanes[1].assign(32 * 32, 0);
        (void)EncodeImage(cspec, cplanes, nullptr, 800);
      }
    }

    // 7. Additional single-threaded uncovered targets: exact-SOD trunc2
    // WANT_TILE transitions, PLT coding parameter change across tiles,
    // progressive KDU_SOURCE_CAP_MEMORY_BACKED with set_max_bytes, and
    // throw_markers 0xFF 0x90 inside packet read/ignore.
    {
      // Truncate right after Tile 1 SOD so desequence_packet returns 0 on
      // Tile 1 while tile == Tile 0 or tile == NULL.
      size_t second_sot = 0;
      for (size_t i = 2; i + 1 < bytes.size(); ++i) {
        if (bytes[i] == 0xFF && bytes[i + 1] == 0x90) {
          if (second_sot == 0) {
            second_sot = 1;
          } else {
            second_sot = i;
            break;
          }
        }
      }
      if (second_sot > 1 && second_sot + 14 < bytes.size()) {
        std::vector<kdu_byte> trunc_sod(bytes.begin(),
                                        bytes.begin() + second_sot + 14);
        for (int pass = 0; pass < 2; ++pass) {
          MemorySource s(trunc_sod);
          ScopedCodestream d;
          d.cs.create(&s);
          d.cs.set_resilient();
          bool done = false;
          kdu_pdescriptor_set pset_t0;
          kdu_tile t0 = d.cs.load_sequential(
              &pset_t0, done, 1, 10000, KDU_LOAD_SEQUENTIAL_WANT_TILE, nullptr);
          if (t0.exists()) t0.close();
          kdu_pdescriptor_set pset_fresh;
          kdu_tile t1 = d.cs.load_sequential(pass == 0 ? &pset_t0 : &pset_fresh,
                                             done, 1, 1000, 0, nullptr);
          if (t1.exists()) t1.close();
        }
      }

      // Tile 1 overriding Corder=LRCP after Tile 0 served PLT data triggers
      // kd_plt_server::notify_new_tile_part error 21.
      {
        ImageSpec plt_spec;
        plt_spec.width = 64;
        plt_spec.height = 32;
        plt_spec.components = 1;
        plt_spec.dwt_levels = 1;
        plt_spec.tile_size = kdu_coords(32, 32);
        plt_spec.extra_params = {"ORGgen_plt=yes", "Corder=RPCL", "Clayers=2",
                                 "Corder:T1=LRCP"};
        const auto plt_planes = MakeTestPlanes(plt_spec);
        const auto plt_bytes = EncodeImage(plt_spec, plt_planes);
        MemorySource psrc(plt_bytes);
        ScopedCodestream pdec;
        pdec.cs.create(&psrc);
        kdu_tile t0 = pdec.cs.open_tile(kdu_coords(0, 0));
        DecodeTileAllComponents(pdec.cs, t0);
        t0.close();
        error_handler_.clear();
        try {
          kdu_tile t1 = pdec.cs.open_tile(kdu_coords(1, 0));
          DecodeTileAllComponents(pdec.cs, t1);
          t1.close();
        } catch (kdu_exception) {
        }
      }

      // Progressive KDU_SOURCE_CAP_MEMORY_BACKED source + set_max_bytes.
      {
        class ProgressiveBackedSource : public kdu_compressed_source {
         public:
          explicit ProgressiveBackedSource(std::vector<kdu_byte> data)
              : data_(std::move(data)),
                avail_(std::min<size_t>(data_.size(), 40)) {}
          int get_capabilities() override {
            return KDU_SOURCE_CAP_MEMORY_BACKED | KDU_SOURCE_CAP_SEEKABLE;
          }
          bool seek(kdu_long offset) override {
            if (offset < 0) return false;
            pos_ = std::min<size_t>(static_cast<size_t>(offset), data_.size());
            avail_ = std::max(avail_, pos_);
            return true;
          }
          kdu_long get_pos() override { return static_cast<kdu_long>(pos_); }
          int read(kdu_byte* buf, int num_bytes) override {
            if (num_bytes <= 0 || pos_ >= avail_) return 0;
            const size_t n =
                std::min(static_cast<size_t>(num_bytes), avail_ - pos_);
            std::memcpy(buf, data_.data() + pos_, n);
            pos_ += n;
            return static_cast<int>(n);
          }
          kdu_byte* access_memory(kdu_long& pos, kdu_byte*& lim) override {
            pos = 0;
            lim = data_.data() + avail_;
            return data_.data();
          }
          int load_memory(kdu_long desired_span, bool /*blocking*/,
                          kdu_compressed_source_notifier* /*n*/) override {
            if (static_cast<size_t>(desired_span) > avail_) {
              avail_ =
                  std::min(data_.size(), static_cast<size_t>(desired_span));
            }
            int flags = KDU_SOURCE_CAP_MEMORY_BACKED;
            if (avail_ >= data_.size()) {
              flags |= KDU_SOURCE_IS_COMPLETE;
            } else {
              flags |= KDU_SOURCE_SPAN_EXISTS;
            }
            return flags;
          }

         private:
          std::vector<kdu_byte> data_;
          size_t avail_ = 0;
          size_t pos_ = 0;
        };

        ProgressiveBackedSource pbs(bytes);
        ScopedCodestream pbdec;
        pbdec.cs.create(&pbs);
        pbdec.cs.set_resilient();
        pbdec.cs.set_max_bytes(static_cast<kdu_long>(bytes.size() / 2));
        kdu_dims tr;
        pbdec.cs.get_valid_tiles(tr);
        for (int ty = 0; ty < tr.size.y; ++ty) {
          for (int tx = 0; tx < tr.size.x; ++tx) {
            try {
              kdu_tile t =
                  pbdec.cs.open_tile(kdu_coords(tr.pos.x + tx, tr.pos.y + ty));
              DecodeTileAllComponents(pbdec.cs, t);
              t.close();
            } catch (kdu_exception) {
            }
          }
        }
        try {
          kdu_tile t0 = pbdec.cs.open_tile(kdu_coords(0, 0));
          DecodeTileAllComponents(pbdec.cs, t0);
          t0.close();
        } catch (kdu_exception) {
        }
      }

      // Same-tile load_sequential resume into EOF with non-empty pset
      // exercises load_sequential line 15345 (tile == tp on compatible pset).
      {
        ImageSpec stspec;
        stspec.width = 128;
        stspec.height = 128;
        stspec.components = 1;
        stspec.dwt_levels = 2;
        stspec.extra_params = {"Cprecincts={16,16}", "Cblk={16,16}"};
        auto st_bytes = EncodeImage(stspec, MakeTestPlanes(stspec));
        if (st_bytes.size() > 200) {
          st_bytes.resize(st_bytes.size() * 3 / 4);
          MemorySource st_src(st_bytes);
          ScopedCodestream st_dec;
          st_dec.cs.create(&st_src);
          st_dec.cs.set_persistent();
          st_dec.cs.set_resilient();
          bool done = false;
          kdu_pdescriptor_set pset;
          kdu_tile t0 =
              st_dec.cs.load_sequential(&pset, done, 1, 1, 0, nullptr);
          if (t0.exists()) t0.close();
          if (!done) {
            kdu_tile t1 =
                st_dec.cs.load_sequential(&pset, done, 100, 1000, 0, nullptr);
            if (t1.exists()) t1.close();
          }
        }
      }

      // Inject 0xFF 0x90 inside active and ignored packet regions on a
      // 2-tile Cuse_sop=yes multi-layer sequential resilient stream.
      {
        ImageSpec lspec;
        lspec.width = 128;
        lspec.height = 64;
        lspec.components = 1;
        lspec.dwt_levels = 2;
        lspec.tile_size = kdu_coords(64, 64);
        lspec.extra_params = {"Corder=LRCP",        "Clayers=3",
                              "Cprecincts={16,16}", "Cblk={16,16}",
                              "Cuse_sop=yes",       "Cuse_eph=yes"};
        const auto lplanes = MakeTestPlanes(lspec);
        const auto raw = encode_layers(lspec, lplanes, {400, 900, 1800});
        size_t first_sod = 0;
        size_t second_sot_raw = 0;
        for (size_t i = 2; i + 1 < raw.size(); ++i) {
          if (raw[i] == 0xFF && raw[i + 1] == 0x93 && first_sod == 0) {
            first_sod = i;
          } else if (raw[i] == 0xFF && raw[i + 1] == 0x90) {
            if (second_sot_raw == 0) {
              second_sot_raw = 1;
            } else {
              second_sot_raw = i;
              break;
            }
          }
        }
        if (first_sod > 0 && second_sot_raw > first_sod + 80) {
          // Case A: marker inside skipped trailing layer body of Tile 0 before
          // opening Tile 1 under max_layers=1.
          {
            auto skip_corrupt = raw;
            const size_t pos = second_sot_raw - 20;
            skip_corrupt[pos] = 0xFF;
            skip_corrupt[pos + 1] = 0x90;
            MemorySource src(skip_corrupt, KDU_SOURCE_CAP_SEQUENTIAL);
            ScopedCodestream dec;
            dec.cs.create(&src);
            dec.cs.set_resilient();
            dec.cs.apply_input_restrictions(0, 0, 0, 1, nullptr,
                                            KDU_WANT_OUTPUT_COMPONENTS);
            for (int tx = 0; tx < 2; ++tx) {
              try {
                kdu_tile t = dec.cs.open_tile(kdu_coords(tx, 0));
                DecodeTileAllComponents(dec.cs, t);
                t.close();
              } catch (kdu_exception) {
              }
            }
          }
          // Case B: marker inside read packet body at several offsets.
          for (size_t off : {size_t{40}, size_t{80}, size_t{120}}) {
            if (first_sod + off + 2 < raw.size()) {
              auto read_corrupt = raw;
              const size_t pos = first_sod + off;
              read_corrupt[pos] = 0xFF;
              read_corrupt[pos + 1] = 0x90;
              MemorySource src(read_corrupt, KDU_SOURCE_CAP_SEQUENTIAL);
              ScopedCodestream dec;
              dec.cs.create(&src);
              dec.cs.set_resilient();
              try {
                kdu_tile t = dec.cs.open_tile(kdu_coords(0, 0));
                DecodeTileAllComponents(dec.cs, t);
                t.close();
              } catch (kdu_exception) {
              }
            }
          }
        }
      }
    }

    // 8. Final small single-threaded hits: ~kd_codestream input_monitor
    // cleanup, short/out-of-order TLM markers, 2-tile Qweights warning
    // suppression, and load_sequential interference after manual tile decode.
    {
      // Unremoved kdu_input_codestream_monitor triggers uninstalling_monitor
      // in ~kd_codestream.
      {
        kdu_input_codestream_monitor mon;
        MemorySource msrc(bytes);
        ScopedCodestream mcs;
        mcs.cs.create(&msrc);
        EXPECT_TRUE(mcs.cs.install_input_monitor(&mon, nullptr));
      }

      // Short (< 6 bytes) and out-of-order Ztlm markers before first SOT.
      size_t first_sot = 0;
      for (size_t i = 2; i + 1 < bytes.size(); ++i) {
        if (bytes[i] == 0xFF && bytes[i + 1] == 0x90) {
          first_sot = i;
          break;
        }
      }
      if (first_sot > 2) {
        {
          auto short_tlm = bytes;
          const kdu_byte seg[] = {0xFF, 0x55, 0x00, 0x03, 0x00};
          short_tlm.insert(
              short_tlm.begin() + static_cast<ptrdiff_t>(first_sot),
              std::begin(seg), std::end(seg));
          MemorySource tsrc(short_tlm);
          ScopedCodestream tdec;
          error_handler_.clear();
          EXPECT_THROW(tdec.cs.create(&tsrc), kdu_exception);
        }
        {
          auto oo_tlm = bytes;
          const kdu_byte segs[] = {
              0xFF, 0x55, 0x00, 0x06, 0x01, 0x00, 0x00, 0x10,
              0xFF, 0x55, 0x00, 0x06, 0x00, 0x00, 0x00, 0x10,
          };
          oo_tlm.insert(oo_tlm.begin() + static_cast<ptrdiff_t>(first_sot),
                        std::begin(segs), std::end(segs));
          MemorySource osrc(oo_tlm);
          ScopedCodestream odec;
          try {
            odec.cs.create(&osrc);
          } catch (kdu_exception) {
          }
        }
      }

      // Two-tile mismatched Qweights hits qweights_warning_issued early return.
      {
        ImageSpec qspec;
        qspec.width = 64;
        qspec.height = 32;
        qspec.components = 3;
        qspec.reversible = false;
        qspec.dwt_levels = 1;
        qspec.tile_size = kdu_coords(32, 32);
        qspec.extra_params = {"Creversible=no", "Qweights=10.0,0.1,1.0"};
        const auto qplanes = MakeTestPlanes(qspec);
        (void)EncodeImage(qspec, qplanes);
      }

      // Manually decoding a tile before calling load_sequential on a
      // sequential stream triggers gen_load_seq_interference_error.
      {
        MemorySource isrc(bytes);
        ScopedCodestream idec;
        idec.cs.create(&isrc);
        idec.cs.set_persistent();
        kdu_tile mt = idec.cs.open_tile(kdu_coords(0, 0));
        DecodeTileAllComponents(idec.cs, mt);
        mt.close();
        bool done = false;
        kdu_pdescriptor_set pset;
        error_handler_.clear();
        try {
          kdu_tile st = idec.cs.load_sequential(&pset, done, 1, 1, 0, nullptr);
          if (st.exists()) st.close();
        } catch (kdu_exception) {
        }
      }
    }
  }
}

}  // namespace
}  // namespace kdu_core
