// Copyright 2026 Google LLC

#include "third_party/kakadu/vendor/apps/compressed_io/mj2.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/kakadu/vendor/apps/compressed_io/jp2.h"
#include "third_party/kakadu/vendor/apps/compressed_io/kdu_video_io.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_params.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_sample_processing.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

namespace {

using kdu_core::kdu_analysis;
using kdu_core::kdu_byte;
using kdu_core::kdu_codestream;
using kdu_core::kdu_compressed_source;
using kdu_core::kdu_compressed_target;
using kdu_core::kdu_coords;
using kdu_core::kdu_dims;
using kdu_core::kdu_exception;
using kdu_core::kdu_int16;
using kdu_core::kdu_int32;
using kdu_core::kdu_line_buf;
using kdu_core::kdu_long;
using kdu_core::kdu_membroker;
using kdu_core::kdu_message;
using kdu_core::kdu_params;
using kdu_core::kdu_push_ifc;
using kdu_core::kdu_push_pull_params;
using kdu_core::kdu_sample_allocator;
using kdu_core::kdu_uint16;
using kdu_core::kdu_uint32;
using kdu_core::siz_params;
using kdu_supp::jp2_box_textualizer;
using kdu_supp::jp2_family_src;
using kdu_supp::jp2_family_tgt;
using kdu_supp::jp2_input_box;
using kdu_supp::jp2_output_box;
using kdu_supp::kdu_field_order;
using kdu_supp::mj2_add_box_descriptions;
using kdu_supp::mj2_source;
using kdu_supp::mj2_target;
using kdu_supp::mj2_video_source;
using kdu_supp::mj2_video_target;

std::string& LastErrorText() {
  static std::string err;
  return err;
}

class ThrowingMessage final : public kdu_message {
 public:
  ~ThrowingMessage() noexcept override = default;
  void put_text(const char* string) override { last_ += string; }
  void flush(bool end_of_message = false) override {
    if (end_of_message) {
      LastErrorText() = last_;
      last_.clear();
      throw static_cast<kdu_exception>(KDU_ERROR_EXCEPTION);
    }
  }

 private:
  std::string last_;
};

class CaptureMessage final : public kdu_message {
 public:
  ~CaptureMessage() noexcept override = default;
  void put_text(const char* string) override { text += string; }
  void flush(bool /*end_of_message*/ = false) override {}
  std::string text;
};

class KakaduTestEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    kdu_core::kdu_customize_errors(&error_handler_);
    kdu_core::kdu_customize_warnings(&warning_handler_);
  }

 private:
  ThrowingMessage error_handler_;
  CaptureMessage warning_handler_;
};

const ::testing::Environment* const kKakaduEnv =
    ::testing::AddGlobalTestEnvironment(new KakaduTestEnvironment());

class MemTarget final : public kdu_compressed_target {
 public:
  bool write(const kdu_byte* buf, int num_bytes) override {
    if (fail_writes_ || num_bytes < 0) return false;
    if (in_rewrite_) {
      if (num_bytes > rewrite_limit_) return false;
      std::memcpy(data_.data() + pos_, buf, static_cast<size_t>(num_bytes));
      pos_ += static_cast<size_t>(num_bytes);
      rewrite_limit_ -= num_bytes;
      return true;
    }
    data_.insert(data_.end(), buf, buf + num_bytes);
    pos_ = data_.size();
    return true;
  }

  bool start_rewrite(kdu_long backtrack) override {
    if (in_rewrite_ || backtrack < 0 ||
        static_cast<size_t>(backtrack) > data_.size()) {
      return false;
    }
    in_rewrite_ = true;
    pos_ = data_.size() - static_cast<size_t>(backtrack);
    rewrite_limit_ = static_cast<int>(backtrack);
    return true;
  }

  bool end_rewrite() override {
    if (!in_rewrite_) return false;
    in_rewrite_ = false;
    pos_ = data_.size();
    rewrite_limit_ = 0;
    return true;
  }

  bool close() override { return true; }

  void set_fail_writes(bool fail) { fail_writes_ = fail; }
  const std::vector<kdu_byte>& data() const { return data_; }
  std::vector<kdu_byte>& mutable_data() { return data_; }

 private:
  std::vector<kdu_byte> data_;
  size_t pos_ = 0;
  bool in_rewrite_ = false;
  bool fail_writes_ = false;
  int rewrite_limit_ = 0;
};

class MemSource final : public kdu_compressed_source {
 public:
  explicit MemSource(const std::vector<kdu_byte>& data, bool seekable = true)
      : data_(data), seekable_(seekable) {}

  int get_capabilities() override {
    return KDU_SOURCE_CAP_SEQUENTIAL |
           (seekable_ ? KDU_SOURCE_CAP_SEEKABLE : 0);
  }

  int read(kdu_byte* buf, int num_bytes) override {
    if (num_bytes <= 0 || pos_ >= data_.size()) return 0;
    const size_t avail = data_.size() - pos_;
    const size_t n = std::min(avail, static_cast<size_t>(num_bytes));
    std::memcpy(buf, data_.data() + pos_, n);
    pos_ += n;
    return static_cast<int>(n);
  }

  bool seek(kdu_long offset) override {
    if (!seekable_ || offset < 0) return false;
    pos_ = std::min(static_cast<size_t>(offset), data_.size());
    return true;
  }

  kdu_long get_pos() override {
    return seekable_ ? static_cast<kdu_long>(pos_) : -1;
  }

 private:
  const std::vector<kdu_byte>& data_;
  size_t pos_ = 0;
  bool seekable_ = true;
};

void AppendBe16(std::vector<kdu_byte>& out, kdu_uint16 v) {
  out.push_back(static_cast<kdu_byte>((v >> 8) & 0xFF));
  out.push_back(static_cast<kdu_byte>(v & 0xFF));
}

void AppendBe32(std::vector<kdu_byte>& out, kdu_uint32 v) {
  out.push_back(static_cast<kdu_byte>((v >> 24) & 0xFF));
  out.push_back(static_cast<kdu_byte>((v >> 16) & 0xFF));
  out.push_back(static_cast<kdu_byte>((v >> 8) & 0xFF));
  out.push_back(static_cast<kdu_byte>(v & 0xFF));
}

void AppendBe64(std::vector<kdu_byte>& out, std::uint64_t v) {
  AppendBe32(out, static_cast<kdu_uint32>(v >> 32));
  AppendBe32(out, static_cast<kdu_uint32>(v & 0xFFFFFFFFU));
}

std::vector<kdu_byte> MakeBox(kdu_uint32 fourcc,
                              const std::vector<kdu_byte>& payload) {
  std::vector<kdu_byte> out;
  AppendBe32(out, static_cast<kdu_uint32>(payload.size() + 8));
  AppendBe32(out, fourcc);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

void AppendBox(std::vector<kdu_byte>& out, kdu_uint32 fourcc,
               const std::vector<kdu_byte>& payload) {
  auto box = MakeBox(fourcc, payload);
  out.insert(out.end(), box.begin(), box.end());
}

void CompressImage(kdu_codestream& codestream, kdu_compressed_target& tgt,
                   int width, int height, int num_comps = 3,
                   kdu_long max_bytes = 65536, bool with_tlm = false) {
  siz_params siz;
  siz.set(Sdims, 0, 0, height);
  siz.set(Sdims, 0, 1, width);
  siz.set(Scomponents, 0, 0, num_comps);
  siz.set(Sprecision, 0, 0, 8);
  siz.set(Ssigned, 0, 0, false);
  static_cast<kdu_params&>(siz).finalize_all();
  codestream.create(&siz, &tgt);
  kdu_params* cod = codestream.access_siz()->access_cluster(COD_params);
  ASSERT_TRUE(cod->parse_string("Clevels=1"));
  ASSERT_TRUE(cod->parse_string("Clayers=1"));
  if (with_tlm) {
    kdu_params* org = codestream.access_siz()->access_cluster(ORG_params);
    ASSERT_TRUE(org->parse_string("ORGgen_tlm=1"));
  }
  codestream.access_siz()->finalize_all();

  auto tile = codestream.open_tile(kdu_coords(0, 0));
  kdu_sample_allocator alloc;
  kdu_push_pull_params pp_params;
  std::vector<kdu_push_ifc> engines(num_comps);
  for (int c = 0; c < num_comps; ++c) {
    auto res = tile.access_component(c).access_resolution();
    engines[c] = kdu_analysis(res, &alloc, pp_params, KDU_SAMPLE_PREC16, 1.0F,
                              0, nullptr);
  }
  kdu_line_buf line;
  line.pre_create(&alloc, width, KDU_SAMPLE_PREC16, 0, 0);
  alloc.finalize(codestream);
  line.create();
  for (int c = 0; c < num_comps; ++c) {
    for (int r = 0; r < height; ++r) {
      for (int i = 0; i < width; ++i) {
        line.get_buf16()[i].ival =
            static_cast<std::int16_t>((r * 7 + i * 3 + c * 11) & 0x7F);
      }
      engines[c].push(line);
    }
    engines[c].destroy();
  }
  line.destroy();
  tile.close();
  codestream.flush(&max_bytes, 1);
}

std::vector<kdu_byte> EncodeSingleCodestream(int width, int height,
                                             int num_comps = 3,
                                             kdu_long max_bytes = 65536) {
  MemTarget mtgt;
  kdu_codestream cs;
  CompressImage(cs, mtgt, width, height, num_comps, max_bytes, false);
  cs.destroy();
  return mtgt.data();
}

// Helper to write a simple 8x8 SIZ-only jp2h payload for synthetic MJ2 files.
std::vector<kdu_byte> MakeMinimalJp2hPayload(int width, int height, int comps,
                                             int bits) {
  std::vector<kdu_byte> ihdr;
  AppendBe32(ihdr, static_cast<kdu_uint32>(height));
  AppendBe32(ihdr, static_cast<kdu_uint32>(width));
  AppendBe16(ihdr, static_cast<kdu_uint16>(comps));
  ihdr.push_back(static_cast<kdu_byte>(bits - 1));
  ihdr.push_back(7);
  ihdr.push_back(0);
  ihdr.push_back(0);

  std::vector<kdu_byte> colr = {1, 0, 0, 0, 0, 0, 16};
  std::vector<kdu_byte> jp2h;
  AppendBox(jp2h, jp2_image_header_4cc, ihdr);
  AppendBox(jp2h, jp2_colour_4cc, colr);
  return jp2h;
}

std::vector<kdu_byte> MakeMatrixBytes(const std::array<double, 9>& m) {
  std::vector<kdu_byte> out;
  for (int col = 0; col < 3; ++col) {
    for (int row = 0; row < 3; ++row) {
      double val = m[3 * row + col];
      kdu_int32 fixed = (row < 2) ? static_cast<kdu_int32>(val * 65536.0)
                                  : static_cast<kdu_int32>(val * (1 << 30));
      AppendBe32(out, static_cast<kdu_uint32>(fixed));
    }
  }
  return out;
}

TEST(Mj2Test, UninitializedAndValidationErrors) {
  jp2_box_textualizer tex;
  mj2_add_box_descriptions(tex);

  mj2_video_source empty_vsrc;
  EXPECT_EQ(empty_vsrc.get_track_idx(), 0U);
  EXPECT_EQ(empty_vsrc.get_compositing_order(), 0);
  EXPECT_EQ(empty_vsrc.get_graphics_mode(), 0);
  kdu_int16 r = 1, g = 2, b = 3;
  EXPECT_EQ(empty_vsrc.get_graphics_mode(r, g, b), 0);
  EXPECT_EQ(r, 0);
  EXPECT_EQ(g, 0);
  EXPECT_EQ(b, 0);
  double pw = 1.0, ph = 1.0, mat[9] = {1.0};
  EXPECT_FALSE(empty_vsrc.get_geometry(pw, ph, mat, true));
  kdu_dims pre;
  bool tr = true, vf = true, hf = true;
  EXPECT_FALSE(empty_vsrc.get_cardinal_geometry(pre, tr, vf, hf, true));
  EXPECT_FALSE(empty_vsrc.access_dimensions().exists());
  EXPECT_FALSE(empty_vsrc.access_resolution().exists());
  EXPECT_FALSE(empty_vsrc.access_colour().exists());
  EXPECT_FALSE(empty_vsrc.access_palette().exists());
  EXPECT_FALSE(empty_vsrc.access_channels().exists());
  EXPECT_EQ(empty_vsrc.get_membroker(), nullptr);
  EXPECT_EQ(empty_vsrc.get_timescale(), 0U);
  EXPECT_EQ(empty_vsrc.get_field_order(), kdu_supp::KDU_FIELDS_NONE);
  empty_vsrc.set_field_mode(0);
  EXPECT_EQ(empty_vsrc.get_num_frames(), 0);
  EXPECT_EQ(empty_vsrc.get_duration(), 0);
  EXPECT_FALSE(empty_vsrc.seek_to_frame(0));
  EXPECT_EQ(empty_vsrc.time_to_frame(0), 0);
  EXPECT_EQ(empty_vsrc.get_frame_instant(), 0);
  EXPECT_EQ(empty_vsrc.get_frame_instant(0), 0);
  EXPECT_EQ(empty_vsrc.get_frame_period(), 0);
  EXPECT_EQ(empty_vsrc.get_frame_period(0), 0);
  EXPECT_EQ(empty_vsrc.get_stream_idx(0), 0);
  EXPECT_FALSE(empty_vsrc.can_open_stream(0));
  jp2_input_box tmp_box;
  EXPECT_EQ(empty_vsrc.open_stream(0, &tmp_box), -1);
  EXPECT_EQ(empty_vsrc.open_image(), -1);
  empty_vsrc.close_image();
  EXPECT_EQ(empty_vsrc.access_image_box(), nullptr);
  EXPECT_EQ(empty_vsrc.get_capabilities(), 0);
  kdu_byte dummy_buf[4] = {};
  EXPECT_EQ(empty_vsrc.read(dummy_buf, 4), 0);
  EXPECT_THROW(empty_vsrc.seek(0), kdu_exception);
  EXPECT_THROW(empty_vsrc.get_pos(), kdu_exception);

  mj2_video_target empty_vtgt;
  EXPECT_EQ(empty_vtgt.get_track_idx(), 0U);
  EXPECT_FALSE(empty_vtgt.set_compositing_order(1));
  EXPECT_FALSE(empty_vtgt.set_graphics_mode(MJ2_GRAPHICS_COPY));
  EXPECT_FALSE(empty_vtgt.access_colour().exists());
  EXPECT_FALSE(empty_vtgt.access_palette().exists());
  EXPECT_FALSE(empty_vtgt.access_resolution().exists());
  EXPECT_FALSE(empty_vtgt.access_channels().exists());
  EXPECT_FALSE(empty_vtgt.set_timescale(30));
  EXPECT_FALSE(empty_vtgt.set_field_order(kdu_supp::KDU_FIELDS_NONE));
  EXPECT_FALSE(empty_vtgt.set_max_frames_per_chunk(2));
  EXPECT_FALSE(empty_vtgt.set_frame_period(1));
  empty_vtgt.open_image();
  kdu_codestream empty_cs;
  EXPECT_THROW(empty_vtgt.close_image(empty_cs), kdu_exception);
  EXPECT_EQ(empty_vtgt.get_membroker(), nullptr);
  EXPECT_FALSE(empty_vtgt.start_rewrite(0));
  EXPECT_FALSE(empty_vtgt.end_rewrite());
  EXPECT_THROW(empty_vtgt.write(dummy_buf, 1), kdu_exception);

  mj2_source empty_msrc;
  EXPECT_FALSE(empty_msrc.exists());
  EXPECT_TRUE(!empty_msrc);
  empty_msrc.close();
  EXPECT_EQ(empty_msrc.get_ultimate_src(), nullptr);
  EXPECT_EQ(empty_msrc.get_metadata_memory(), 0);
  EXPECT_TRUE(!empty_msrc.get_movie_dims());
  kdu_uint32 tidx = 9;
  int fidx = 9, fldidx = 9;
  EXPECT_TRUE(empty_msrc.find_stream(0, tidx, fidx, fldidx));
  int count = 9;
  EXPECT_TRUE(empty_msrc.count_codestreams(count));
  EXPECT_EQ(count, 0);

  mj2_target empty_mtgt;
  EXPECT_FALSE(empty_mtgt.exists());
  EXPECT_TRUE(!empty_mtgt);
  empty_mtgt.close();
  empty_mtgt.destroy();
  EXPECT_EQ(empty_mtgt.get_metadata_memory(), 0);

  // Target validation errors while open.
  {
    MemTarget mtgt;
    jp2_family_tgt ftgt;
    ftgt.open(&mtgt);
    mj2_target tgt;
    tgt.open(&ftgt);
    EXPECT_TRUE(tgt.exists());
    EXPECT_FALSE(!tgt);
    EXPECT_THROW(tgt.open(&ftgt), kdu_exception);

    auto* vt = tgt.add_video_track();
    ASSERT_NE(vt, nullptr);
    EXPECT_THROW(vt->close_image(empty_cs), kdu_exception);
    EXPECT_THROW(vt->write(dummy_buf, 1), kdu_exception);
    EXPECT_THROW(vt->set_graphics_mode(1234), kdu_exception);
    EXPECT_THROW(vt->set_timescale(0), kdu_exception);
    EXPECT_THROW(vt->open_image(), kdu_exception);

    // Close track with 0 frames triggers empty frame error.
    vt->access_colour().init(kdu_supp::JP2_sRGB_SPACE);
    EXPECT_THROW(tgt.close(), kdu_exception);
    tgt.destroy();
    ftgt.close();
  }

  // After opening an image, modifying timescale/field_order or double open
  // or write without closing fails.
  {
    MemTarget mtgt;
    jp2_family_tgt ftgt;
    ftgt.open(&mtgt);
    mj2_target tgt;
    tgt.open(&ftgt);
    auto* vt = tgt.add_video_track();
    vt->set_timescale(30);
    vt->set_frame_period(1);
    vt->open_image();
    EXPECT_THROW(vt->open_image(), kdu_exception);
    EXPECT_THROW(vt->set_timescale(60), kdu_exception);
    EXPECT_THROW(vt->set_field_order(kdu_supp::KDU_FIELDS_NONE), kdu_exception);
    tgt.destroy();
    ftgt.close();
  }

  // Excessive display resolution triggers error in write_sample_entry_box.
  {
    MemTarget mtgt;
    jp2_family_tgt ftgt;
    ftgt.open(&mtgt);
    mj2_target tgt;
    tgt.open(&ftgt);
    auto* vt = tgt.add_video_track();
    vt->access_colour().init(kdu_supp::JP2_sRGB_SPACE);
    vt->access_resolution().init(1.0F);
    vt->access_resolution().set_resolution(1000000000.0F, true);
    vt->set_timescale(10);
    vt->set_frame_period(1);
    kdu_codestream cs;
    vt->open_image();
    CompressImage(cs, *vt, 8, 8, 3);
    vt->close_image(cs);
    cs.destroy();
    EXPECT_THROW(tgt.close(), kdu_exception);
    tgt.destroy();
    ftgt.close();
  }

  // Mismatched second frame dimensions and > 0x7FFF dimensions.
  {
    MemTarget mtgt;
    jp2_family_tgt ftgt;
    ftgt.open(&mtgt);
    mj2_target tgt;
    tgt.open(&ftgt);
    auto* vt = tgt.add_video_track();
    vt->access_colour().init(kdu_supp::JP2_sRGB_SPACE);
    vt->set_timescale(10);
    vt->set_frame_period(1);
    kdu_codestream cs;
    vt->open_image();
    CompressImage(cs, *vt, 8, 8, 3);
    vt->close_image(cs);
    cs.destroy();
    vt->open_image();
    CompressImage(cs, *vt, 8, 10, 3);
    EXPECT_THROW(vt->close_image(cs), kdu_exception);
    cs.destroy();
    tgt.destroy();
    ftgt.close();
  }

  // Dimensions > 0x7FFF progressive and interlaced sum > 0x7FFF.
  {
    MemTarget mtgt;
    jp2_family_tgt ftgt;
    ftgt.open(&mtgt);
    mj2_target tgt;
    tgt.open(&ftgt);
    auto* vt = tgt.add_video_track();
    vt->set_timescale(10);
    vt->set_frame_period(1);
    siz_params siz;
    siz.set(Sdims, 0, 0, 32768);
    siz.set(Sdims, 0, 1, 8);
    siz.set(Scomponents, 0, 0, 1);
    siz.set(Sprecision, 0, 0, 8);
    siz.set(Ssigned, 0, 0, false);
    static_cast<kdu_params&>(siz).finalize_all();
    kdu_codestream cs;
    MemTarget dummy_t;
    cs.create(&siz, &dummy_t);
    vt->open_image();
    EXPECT_THROW(vt->close_image(cs), kdu_exception);
    cs.destroy();
    tgt.destroy();
    ftgt.close();
  }
  {
    MemTarget mtgt;
    jp2_family_tgt ftgt;
    ftgt.open(&mtgt);
    mj2_target tgt;
    tgt.open(&ftgt);
    auto* vt = tgt.add_video_track();
    vt->set_field_order(kdu_supp::KDU_FIELDS_TOP_FIRST);
    vt->set_timescale(10);
    vt->set_frame_period(1);
    auto make_tall_cs = [](kdu_codestream& cs, MemTarget& dt) {
      siz_params siz;
      siz.set(Sdims, 0, 0, 20000);
      siz.set(Sdims, 0, 1, 8);
      siz.set(Scomponents, 0, 0, 1);
      siz.set(Sprecision, 0, 0, 8);
      siz.set(Ssigned, 0, 0, false);
      static_cast<kdu_params&>(siz).finalize_all();
      cs.create(&siz, &dt);
    };
    kdu_codestream cs1, cs2;
    MemTarget dummy1, dummy2;
    make_tall_cs(cs1, dummy1);
    make_tall_cs(cs2, dummy2);
    vt->open_image();
    vt->close_image(cs1);
    vt->open_image();
    EXPECT_THROW(vt->close_image(cs2), kdu_exception);
    cs1.destroy();
    cs2.destroy();
    tgt.destroy();
    ftgt.close();
  }
}

TEST(Mj2Test, MultiTrackRoundTripSeekingAndStreams) {
  MemTarget mtgt;
  kdu_membroker mb;
  {
    jp2_family_tgt ftgt;
    ftgt.open(&mtgt, &mb);
    mj2_target tgt;
    tgt.open(&ftgt, &mb);
    EXPECT_EQ(tgt.get_metadata_memory(), 0);

    // Track 1: progressive, varying period/chunking/sizes, TLM rewrite,
    // blue screen, resolution, palette & channel setup.
    auto* vt1 = tgt.add_video_track();
    ASSERT_NE(vt1, nullptr);
    EXPECT_EQ(vt1->get_track_idx(), 1U);
    EXPECT_EQ(vt1->get_membroker(), &mb);
    EXPECT_TRUE(vt1->set_compositing_order(-2));
    EXPECT_TRUE(vt1->set_graphics_mode(MJ2_GRAPHICS_BLUE_SCREEN, 10, 20, 30));
    vt1->access_colour().init(kdu_supp::JP2_sRGB_SPACE);
    vt1->access_resolution().init(1.0F);
    vt1->access_resolution().set_resolution(2835.0F, false);
    EXPECT_TRUE(vt1->access_channels().exists());
    EXPECT_TRUE(vt1->access_palette().exists());
    EXPECT_TRUE(vt1->set_timescale(100));
    EXPECT_TRUE(vt1->set_field_order(kdu_supp::KDU_FIELDS_NONE));
    EXPECT_TRUE(vt1->set_max_frames_per_chunk(2));

    const kdu_long budgets[] = {300, 900, 1500, 450, 1100};
    const kdu_long periods[] = {30, 30, 50, 50, 80};
    for (int i = 0; i < 5; ++i) {
      vt1->set_frame_period(periods[i]);
      kdu_codestream cs;
      vt1->open_image();
      CompressImage(cs, *vt1, 16, 16, 3, budgets[i], i == 0);
      vt1->close_image(cs);
      cs.destroy();
    }

    // Track 2: interlaced TOP_FIRST, alpha graphics mode, odd height (9 ->
    // 5+4), also exercises repeating last field on incomplete final frame if
    // flushed.
    auto* vt2 = tgt.add_video_track();
    ASSERT_NE(vt2, nullptr);
    EXPECT_EQ(vt2->get_track_idx(), 2U);
    EXPECT_TRUE(vt2->set_compositing_order(3));
    EXPECT_TRUE(vt2->set_graphics_mode(MJ2_GRAPHICS_ALPHA));
    vt2->access_colour().init(kdu_supp::JP2_sRGB_SPACE);
    vt2->set_timescale(50);
    vt2->set_field_order(kdu_supp::KDU_FIELDS_TOP_FIRST);
    vt2->set_frame_period(2);
    for (int f = 0; f < 2; ++f) {
      for (int fld = 0; fld < 2; ++fld) {
        const int fh = (fld == 0) ? 5 : 4;
        kdu_codestream cs;
        vt2->open_image();
        CompressImage(cs, *vt2, 8, fh, 3, 400 + f * 100);
        vt2->close_image(cs);
        cs.destroy();
      }
    }
    // Add one unpaired field to exercise repeat-last-field path on flush.
    {
      kdu_codestream cs;
      vt2->open_image();
      CompressImage(cs, *vt2, 8, 5, 3, 400);
      vt2->close_image(cs);
      cs.destroy();
    }

    // Track 3: interlaced TOP_SECOND, premult alpha, 8 -> 4+4.
    auto* vt3 = tgt.add_video_track();
    ASSERT_NE(vt3, nullptr);
    EXPECT_EQ(vt3->get_track_idx(), 3U);
    EXPECT_TRUE(vt3->set_graphics_mode(MJ2_GRAPHICS_PREMULT_ALPHA));
    vt3->access_colour().init(kdu_supp::JP2_sYCC_SPACE);
    vt3->set_timescale(25);
    vt3->set_field_order(kdu_supp::KDU_FIELDS_TOP_SECOND);
    vt3->set_frame_period(1);
    for (int fld = 0; fld < 2; ++fld) {
      kdu_codestream cs;
      vt3->open_image();
      CompressImage(cs, *vt3, 8, 4, 3, 400);
      vt3->close_image(cs);
      cs.destroy();
    }

    // Track 4: identical tiny frames -> default_size != 0 in STSZ, component
    // alpha.
    auto* vt4 = tgt.add_video_track();
    ASSERT_NE(vt4, nullptr);
    EXPECT_TRUE(vt4->set_graphics_mode(MJ2_GRAPHICS_COMPONENT_ALPHA));
    vt4->access_colour().init(kdu_supp::JP2_sLUM_SPACE);
    vt4->set_timescale(25);
    vt4->set_frame_period(1);
    auto tiny = EncodeSingleCodestream(8, 8, 1);
    siz_params tiny_siz;
    tiny_siz.set(Sdims, 0, 0, 8);
    tiny_siz.set(Sdims, 0, 1, 8);
    tiny_siz.set(Scomponents, 0, 0, 1);
    tiny_siz.set(Sprecision, 0, 0, 8);
    tiny_siz.set(Ssigned, 0, 0, false);
    static_cast<kdu_params&>(tiny_siz).finalize_all();
    kdu_codestream tiny_cs;
    MemTarget dummy_t;
    tiny_cs.create(&tiny_siz, &dummy_t);
    for (int f = 0; f < 3; ++f) {
      vt4->open_image();
      EXPECT_TRUE(vt4->write(tiny.data(), static_cast<int>(tiny.size())));
      vt4->close_image(tiny_cs);
    }
    tiny_cs.destroy();

    EXPECT_GT(tgt.get_metadata_memory(), 0);
    tgt.close();
    EXPECT_FALSE(tgt.exists());
    ftgt.close();
  }

  // Read back and exercise every mj2_source / mj2_video_source method.
  MemSource msrc(mtgt.data());
  jp2_family_src fsrc;
  fsrc.open(&msrc, &mb);
  mj2_source msrc_mj2;
  ASSERT_EQ(msrc_mj2.open(&fsrc, false, &mb), 1);
  EXPECT_TRUE(msrc_mj2.exists());
  EXPECT_FALSE(!msrc_mj2);
  EXPECT_EQ(msrc_mj2.get_ultimate_src(), &fsrc);
  EXPECT_GT(msrc_mj2.get_metadata_memory(), 0);
  EXPECT_THROW(msrc_mj2.open(&fsrc), kdu_exception);

  kdu_dims mdims = msrc_mj2.get_movie_dims();
  EXPECT_EQ(mdims.size.x, 16);
  EXPECT_EQ(mdims.size.y, 16);
  EXPECT_EQ(msrc_mj2.get_movie_dims().size.x, 16);

  EXPECT_EQ(msrc_mj2.get_next_track(0), 1U);
  EXPECT_EQ(msrc_mj2.get_next_track(1), 2U);
  EXPECT_EQ(msrc_mj2.get_next_track(4), 0U);
  EXPECT_EQ(msrc_mj2.get_track_type(1), MJ2_TRACK_IS_VIDEO);
  EXPECT_EQ(msrc_mj2.get_track_type(99), MJ2_TRACK_NON_EXISTENT);
  EXPECT_EQ(msrc_mj2.access_video_track(99), nullptr);

  int cs_count = 0;
  EXPECT_TRUE(msrc_mj2.count_codestreams(cs_count));
  // Track1(5) + Track2(3*2=6) + Track3(1*2=2) + Track4(3) = 16
  EXPECT_EQ(cs_count, 16);

  kdu_uint32 ft = 0;
  int ff = 0, ffld = 0;
  EXPECT_TRUE(msrc_mj2.find_stream(0, ft, ff, ffld));
  EXPECT_GE(ft, 1U);
  EXPECT_TRUE(msrc_mj2.find_stream(100, ft, ff, ffld));
  EXPECT_EQ(ft, 0U);

  auto* vs1 = msrc_mj2.access_video_track(1);
  ASSERT_NE(vs1, nullptr);
  EXPECT_EQ(vs1->get_track_idx(), 1U);
  EXPECT_EQ(vs1->get_compositing_order(), -2);
  kdu_int16 r = 0, g = 0, b = 0;
  EXPECT_EQ(vs1->get_graphics_mode(r, g, b), MJ2_GRAPHICS_BLUE_SCREEN);
  EXPECT_EQ(r, 10);
  EXPECT_EQ(g, 20);
  EXPECT_EQ(b, 30);
  EXPECT_EQ(vs1->get_graphics_mode(), MJ2_GRAPHICS_BLUE_SCREEN);
  EXPECT_EQ(vs1->get_membroker(), &mb);
  EXPECT_EQ(vs1->get_timescale(), 100U);
  EXPECT_EQ(vs1->get_field_order(), kdu_supp::KDU_FIELDS_NONE);
  EXPECT_EQ(vs1->get_num_frames(), 5);
  EXPECT_EQ(vs1->get_duration(), 240);
  EXPECT_TRUE(vs1->access_dimensions().exists());
  EXPECT_TRUE(vs1->access_resolution().exists());
  EXPECT_TRUE(vs1->access_colour().exists());
  EXPECT_TRUE(vs1->access_palette().exists());
  EXPECT_TRUE(vs1->access_channels().exists());

  double pw = 0, ph = 0, mat[9] = {};
  EXPECT_TRUE(vs1->get_geometry(pw, ph, mat, false));
  EXPECT_EQ(pw, 16.0);
  EXPECT_EQ(ph, 16.0);
  EXPECT_TRUE(vs1->get_geometry(pw, ph, mat, true));

  EXPECT_EQ(vs1->time_to_frame(-5), 0);
  EXPECT_EQ(vs1->time_to_frame(35), 1);
  EXPECT_EQ(vs1->time_to_frame(10000), 4);
  EXPECT_EQ(vs1->time_to_frame(10), 0);
  EXPECT_EQ(vs1->get_frame_instant(-1), 0);
  EXPECT_EQ(vs1->get_frame_instant(5), 0);
  EXPECT_EQ(vs1->get_frame_instant(3), 110);
  EXPECT_EQ(vs1->get_frame_period(-1), 0);
  EXPECT_EQ(vs1->get_frame_period(5), 0);
  EXPECT_EQ(vs1->get_frame_period(2), 50);

  // Seek backwards and forwards, check get_frame_instant/period before & during
  // open.
  EXPECT_FALSE(vs1->seek_to_frame(-1));
  EXPECT_FALSE(vs1->seek_to_frame(5));
  EXPECT_TRUE(vs1->seek_to_frame(3));
  EXPECT_EQ(vs1->get_frame_instant(), 110);
  EXPECT_EQ(vs1->get_frame_period(), 50);
  EXPECT_EQ(vs1->get_stream_idx(0), vs1->get_stream_idx(0));
  EXPECT_EQ(vs1->get_stream_idx(1), -1);
  EXPECT_TRUE(vs1->can_open_stream(0));
  EXPECT_FALSE(vs1->can_open_stream(1));

  // Read frame 3, check capabilities/pos/seek/read/access_image_box.
  kdu_byte hdr_buf[4] = {};
  EXPECT_EQ(vs1->read(hdr_buf, 4), 0);
  EXPECT_THROW(vs1->seek(0), kdu_exception);
  EXPECT_THROW(vs1->get_pos(), kdu_exception);
  EXPECT_EQ(vs1->open_image(), 3);
  EXPECT_THROW(vs1->open_image(), kdu_exception);
  EXPECT_EQ(vs1->get_frame_instant(), 110);
  EXPECT_EQ(vs1->get_frame_period(), 50);
  EXPECT_NE(vs1->access_image_box(), nullptr);
  EXPECT_NE(vs1->get_capabilities(), 0);
  EXPECT_GT(vs1->read(hdr_buf, 2), 0);
  EXPECT_EQ(vs1->get_pos(), 2);
  EXPECT_TRUE(vs1->seek(0));
  EXPECT_EQ(vs1->get_pos(), 0);
  {
    jp2_input_box blocked;
    EXPECT_THROW(vs1->open_stream(0, &blocked), kdu_exception);
  }
  vs1->close_image();

  // Read frame 4 then EOF.
  EXPECT_EQ(vs1->open_image(), 4);
  vs1->close_image();
  EXPECT_EQ(vs1->open_image(), -1);
  EXPECT_EQ(vs1->get_frame_period(), 0);
  EXPECT_EQ(vs1->get_stream_idx(0), -1);
  EXPECT_FALSE(vs1->can_open_stream(0));
  {
    jp2_input_box box;
    EXPECT_EQ(vs1->open_stream(0, &box), -1);
  }
  EXPECT_TRUE(vs1->seek_to_frame(1));
  EXPECT_TRUE(vs1->seek_to_frame(0));
  {
    jp2_input_box box;
    EXPECT_EQ(vs1->open_stream(0, &box), 0);
    box.close();
  }

  // Track 2: interlaced field modes & open_stream on field 1 before field 0.
  auto* vs2 = msrc_mj2.access_video_track(2);
  ASSERT_NE(vs2, nullptr);
  EXPECT_EQ(vs2->get_field_order(), kdu_supp::KDU_FIELDS_TOP_FIRST);
  EXPECT_EQ(vs2->get_num_frames(), 3);
  EXPECT_GE(vs2->get_stream_idx(1), 0);
  {
    jp2_input_box b1;
    EXPECT_EQ(vs2->open_stream(1, &b1), 0);
    b1.close();
    jp2_input_box b0;
    EXPECT_EQ(vs2->open_stream(0, &b0), 0);
    b0.close();
    EXPECT_EQ(vs2->open_stream(1, &b1), 0);
    b1.close();
  }
  vs2->set_field_mode(1);
  EXPECT_EQ(vs2->open_image(), 0);
  vs2->close_image();
  vs2->set_field_mode(2);
  EXPECT_TRUE(vs2->seek_to_frame(1));
  EXPECT_EQ(vs2->open_image(), 1);
  vs2->close_image();
  EXPECT_EQ(vs2->open_image(), 1);
  vs2->close_image();
  vs2->set_field_mode(0);

  // Track 4: default_size != 0 sample reads (forward & backward).
  auto* vs4 = msrc_mj2.access_video_track(4);
  ASSERT_NE(vs4, nullptr);
  EXPECT_EQ(vs4->get_num_frames(), 3);
  EXPECT_EQ(vs4->open_image(), 0);
  vs4->close_image();
  EXPECT_TRUE(vs4->seek_to_frame(2));
  EXPECT_EQ(vs4->open_image(), 2);
  vs4->close_image();
  EXPECT_TRUE(vs4->seek_to_frame(0));
  EXPECT_EQ(vs4->open_image(), 0);
  vs4->close_image();

  msrc_mj2.close();
  EXPECT_FALSE(msrc_mj2.exists());
  fsrc.close();
}

TEST(Mj2Test, LargeChunkBufferAndOver1024Samples) {
  MemTarget mtgt;
  {
    jp2_family_tgt ftgt;
    ftgt.open(&mtgt);
    mj2_target tgt;
    tgt.open(&ftgt);

    // Track 1: 120,000 bytes in 1 frame + cross-block start_rewrite to exercise
    // > 100KB mj_chunk_store linked list.
    auto* vt1 = tgt.add_video_track();
    vt1->access_colour().init(kdu_supp::JP2_sLUM_SPACE);
    vt1->set_timescale(30);
    vt1->set_frame_period(1);
    auto tiny = EncodeSingleCodestream(8, 8, 1);
    std::vector<kdu_byte> big_payload(120000, 0xAA);
    std::memcpy(big_payload.data(), tiny.data(), tiny.size());

    siz_params tiny_siz;
    tiny_siz.set(Sdims, 0, 0, 8);
    tiny_siz.set(Sdims, 0, 1, 8);
    tiny_siz.set(Scomponents, 0, 0, 1);
    tiny_siz.set(Sprecision, 0, 0, 8);
    tiny_siz.set(Ssigned, 0, 0, false);
    static_cast<kdu_params&>(tiny_siz).finalize_all();
    kdu_codestream tiny_cs;
    MemTarget dummy_t;
    tiny_cs.create(&tiny_siz, &dummy_t);

    vt1->open_image();
    EXPECT_TRUE(
        vt1->write(big_payload.data(), static_cast<int>(big_payload.size())));
    EXPECT_FALSE(vt1->start_rewrite(-1));
    EXPECT_TRUE(vt1->start_rewrite(110000));
    std::vector<kdu_byte> patch(100050, 0xBB);
    EXPECT_TRUE(vt1->write(patch.data(), static_cast<int>(patch.size())));
    EXPECT_TRUE(vt1->end_rewrite());
    EXPECT_FALSE(vt1->start_rewrite(9999999));
    // Note: failed start_rewrite leaving partial rewrite=NULL still recovers if
    // not writing; here rewrite reached NULL so start_rewrite returned false.
    vt1->close_image(tiny_cs);

    // Track 2: 1030 frames (>1024) of varying sizes and 1 frame per chunk to
    // span > 1 store block in both mj_sample_sizes and mj_chunk_offsets.
    auto* vt2 = tgt.add_video_track();
    vt2->access_colour().init(kdu_supp::JP2_sLUM_SPACE);
    vt2->set_timescale(2000);
    vt2->set_frame_period(1);
    for (int f = 0; f < 1030; ++f) {
      vt2->open_image();
      EXPECT_TRUE(vt2->write(tiny.data(), static_cast<int>(tiny.size())));
      if (f & 1) {
        kdu_byte pad = static_cast<kdu_byte>(f & 0xFF);
        EXPECT_TRUE(vt2->write(&pad, 1));
      }
      vt2->close_image(tiny_cs);
    }

    tiny_cs.destroy();
    tgt.close();
    ftgt.close();
  }

  MemSource msrc(mtgt.data());
  jp2_family_src fsrc;
  fsrc.open(&msrc);
  mj2_source msrc_mj2;
  ASSERT_EQ(msrc_mj2.open(&fsrc), 1);

  auto* vs1 = msrc_mj2.access_video_track(1);
  ASSERT_NE(vs1, nullptr);
  EXPECT_EQ(vs1->open_image(), 0);
  std::vector<kdu_byte> readback(120000);
  EXPECT_EQ(vs1->read(readback.data(), static_cast<int>(readback.size())),
            120000);
  EXPECT_EQ(readback[10000], 0xBB);
  vs1->close_image();

  auto* vs2 = msrc_mj2.access_video_track(2);
  ASSERT_NE(vs2, nullptr);
  EXPECT_EQ(vs2->get_num_frames(), 1030);
  EXPECT_TRUE(vs2->seek_to_frame(1027));
  EXPECT_EQ(vs2->open_image(), 1027);
  vs2->close_image();
  EXPECT_TRUE(vs2->seek_to_frame(3));
  EXPECT_EQ(vs2->open_image(), 3);
  vs2->close_image();

  msrc_mj2.close();
  fsrc.close();
}

TEST(Mj2Test, Synthetic64BitTransformsAndMalformedInputs) {
  auto tiny = EncodeSingleCodestream(8, 8, 1);

  auto build_synthetic_mj2 = [&](const std::vector<kdu_byte>& moov_payload,
                                 bool append_mdat = true) {
    std::vector<kdu_byte> file;
    AppendBox(file, jp2_signature_4cc, {0x0D, 0x0A, 0x87, 0x0A});
    std::vector<kdu_byte> ftyp;
    AppendBe32(ftyp, mj2_brand);
    AppendBe32(ftyp, 0);
    AppendBe32(ftyp, mj2_brand);
    AppendBox(file, jp2_file_type_4cc, ftyp);

    const kdu_uint32 codestream_offset =
        static_cast<kdu_uint32>(file.size() + 8);
    if (append_mdat) {
      std::vector<kdu_byte> jp2c = MakeBox(jp2_codestream_4cc, tiny);
      AppendBox(file, jp2_mdat_4cc, jp2c);
    }
    AppendBox(file, mj2_movie_4cc, moov_payload);
    return std::pair<std::vector<kdu_byte>, kdu_uint32>(std::move(file),
                                                        codestream_offset);
  };

  auto open_and_close = [](const std::vector<kdu_byte>& bytes,
                           bool ret_incompat) {
    MemSource msrc(bytes);
    jp2_family_src fsrc;
    fsrc.open(&msrc);
    mj2_source mj;
    int res = 0;
    try {
      res = mj.open(&fsrc, ret_incompat);
    } catch (...) {
      mj.close();
      fsrc.close();
      throw;
    }
    mj.close();
    fsrc.close();
    return res;
  };

  // Signature, ftyp, brand, missing mvhd checks.
  {
    std::vector<kdu_byte> bad_sig = {0,   0,   0, 12, 'j', 'P',
                                     ' ', ' ', 1, 2,  3,   4};
    EXPECT_EQ(open_and_close(bad_sig, true), -1);
    EXPECT_THROW(open_and_close(bad_sig, false), kdu_exception);

    std::vector<kdu_byte> no_ftyp;
    AppendBox(no_ftyp, jp2_signature_4cc, {0x0D, 0x0A, 0x87, 0x0A});
    EXPECT_EQ(open_and_close(no_ftyp, true), -1);
    EXPECT_THROW(open_and_close(no_ftyp, false), kdu_exception);

    std::vector<kdu_byte> wrong_brand;
    AppendBox(wrong_brand, jp2_signature_4cc, {0x0D, 0x0A, 0x87, 0x0A});
    std::vector<kdu_byte> ftyp;
    AppendBe32(ftyp, jp2_brand);
    AppendBe32(ftyp, 0);
    AppendBe32(ftyp, jp2_brand);
    AppendBox(wrong_brand, jp2_file_type_4cc, ftyp);
    EXPECT_EQ(open_and_close(wrong_brand, true), -1);
    EXPECT_THROW(open_and_close(wrong_brand, false), kdu_exception);

    std::vector<kdu_byte> no_mvhd = wrong_brand;
    std::vector<kdu_byte> ftyp_mj2;
    AppendBe32(ftyp_mj2, mj2_brand);
    AppendBe32(ftyp_mj2, 0);
    AppendBe32(ftyp_mj2, mj2_brand);
    no_mvhd.clear();
    AppendBox(no_mvhd, jp2_signature_4cc, {0x0D, 0x0A, 0x87, 0x0A});
    AppendBox(no_mvhd, jp2_file_type_4cc, ftyp_mj2);
    EXPECT_EQ(open_and_close(no_mvhd, true), -1);
    EXPECT_THROW(open_and_close(no_mvhd, false), kdu_exception);
  }

  // Valid synthetic file with:
  // - version 1 (64-bit) mvhd with rotated/skewed movie matrix
  // - Track 1: version 1 tkhd + mdhd + typo 'mhdr' + 'co64' +
  // transposed/flipped
  //   matrix + fiel(1,0) + vmhd with unknown graphics mode
  // - Track 2: negative translation to exercise movie_dims min/max expansion
  // - Track 3: disabled track (tkhd flags=0)
  // - Track 4: non-video handler ('soun')
  // - Track 5: external dref url (disabled) + track 6: 2-entry dref (disabled)
  // - Track 7: 2-entry stsd (disabled) + non-visual stsd entry
  {
    std::vector<kdu_byte> mdat_jp2c = MakeBox(jp2_codestream_4cc, tiny);
    kdu_uint32 offset_placeholder = 0;

    auto make_mvhd_v1 = [&]() {
      std::vector<kdu_byte> p;
      AppendBe32(p, 0x01000000);
      AppendBe64(p, 100);
      AppendBe64(p, 200);
      AppendBe32(p, 1000);
      AppendBe64(p, 5000);
      AppendBe32(p, 0x00010000);
      AppendBe16(p, 0x0100);
      AppendBe16(p, 0);
      AppendBe32(p, 0);
      AppendBe32(p, 0);
      // 90 deg rotation + slight skew + offset
      auto mat =
          MakeMatrixBytes({0.0, 1.0, 2.0, -1.0, 0.2, 3.0, 0.0, 0.0, 1.0});
      p.insert(p.end(), mat.begin(), mat.end());
      for (int i = 0; i < 6; ++i) AppendBe32(p, 0);
      AppendBe32(p, 10);
      return p;
    };

    auto make_tkhd_v1 = [&](kdu_uint32 flags, kdu_uint32 tid,
                            const std::array<double, 9>& m, double pw,
                            double ph) {
      std::vector<kdu_byte> p;
      AppendBe32(p, (1U << 24) | flags);
      AppendBe64(p, 10);
      AppendBe64(p, 20);
      AppendBe32(p, tid);
      AppendBe32(p, 0);
      AppendBe64(p, 1000);
      AppendBe32(p, 0);
      AppendBe32(p, 0);
      AppendBe16(p, 0);
      AppendBe16(p, 0);
      AppendBe16(p, 0x0100);
      AppendBe16(p, 0);
      auto mat = MakeMatrixBytes(m);
      p.insert(p.end(), mat.begin(), mat.end());
      AppendBe32(p,
                 static_cast<kdu_uint32>(static_cast<kdu_int32>(pw * 65536.0)));
      AppendBe32(p,
                 static_cast<kdu_uint32>(static_cast<kdu_int32>(ph * 65536.0)));
      return p;
    };

    auto make_mdhd_v1 = [&]() {
      std::vector<kdu_byte> p;
      AppendBe32(p, 0x01000000);
      AppendBe64(p, 1);
      AppendBe64(p, 2);
      AppendBe32(p, 100);
      AppendBe64(p, 200);
      AppendBe16(p, 0);
      AppendBe16(p, 0);
      return p;
    };

    auto make_hdlr = [&](kdu_uint32 htype) {
      std::vector<kdu_byte> p;
      AppendBe32(p, 0);
      AppendBe32(p, 0);
      AppendBe32(p, htype);
      AppendBe32(p, 0);
      AppendBe32(p, 0);
      AppendBe32(p, 0);
      return p;
    };

    auto make_vmhd = [&](kdu_uint32 ver_flags, kdu_uint16 mode) {
      std::vector<kdu_byte> p;
      AppendBe32(p, ver_flags);
      AppendBe16(p, mode);
      AppendBe16(p, 0);
      AppendBe16(p, 0);
      AppendBe16(p, 0);
      return p;
    };

    auto make_dref = [&](kdu_uint32 entries, kdu_uint32 url_flags) {
      std::vector<kdu_byte> p;
      AppendBe32(p, 0);
      AppendBe32(p, entries);
      std::vector<kdu_byte> up;
      AppendBe32(up, url_flags);
      AppendBox(p, mj2_url_4cc, up);
      return p;
    };

    auto make_mjp2 = [&](bool add_fiel1) {
      std::vector<kdu_byte> p(6, 0);
      AppendBe16(p, 1);  // data ref index
      AppendBe16(p, 0);
      AppendBe16(p, 0);
      AppendBe32(p, 0);
      AppendBe32(p, 0);
      AppendBe32(p, 0);
      AppendBe16(p, 8);
      AppendBe16(p, 8);
      AppendBe32(p, 72 << 16);
      AppendBe32(p, 72 << 16);
      AppendBe32(p, 0);
      AppendBe16(p, 1);
      for (int i = 0; i < 32; ++i) p.push_back(0);
      AppendBe16(p, 24);
      AppendBe16(p, static_cast<kdu_uint16>(-1));
      AppendBox(p, jp2_header_4cc, MakeMinimalJp2hPayload(8, 8, 1, 8));
      if (add_fiel1) {
        AppendBox(p, mj2_field_coding_4cc, {1, 0});
      }
      return p;
    };

    auto make_stbl_co64 = [&](kdu_uint32 cs_offset) {
      std::vector<kdu_byte> stbl;
      std::vector<kdu_byte> stsd;
      AppendBe32(stsd, 0);
      AppendBe32(stsd, 1);
      AppendBox(stsd, mj2_visual_sample_entry_4cc, make_mjp2(true));
      AppendBox(stbl, mj2_sample_description_4cc, stsd);

      std::vector<kdu_byte> stts;
      AppendBe32(stts, 0);
      AppendBe32(stts, 1);
      AppendBe32(stts, 1);
      AppendBe32(stts, 100);
      AppendBox(stbl, mj2_time_to_sample_4cc, stts);

      std::vector<kdu_byte> stsc;
      AppendBe32(stsc, 0);
      AppendBe32(stsc, 1);
      AppendBe32(stsc, 1);
      AppendBe32(stsc, 1);
      AppendBe32(stsc, 1);
      AppendBox(stbl, mj2_sample_to_chunk_4cc, stsc);

      std::vector<kdu_byte> stsz;
      AppendBe32(stsz, 0);
      AppendBe32(stsz, static_cast<kdu_uint32>(mdat_jp2c.size()));
      AppendBe32(stsz, 1);
      AppendBox(stbl, mj2_sample_size_4cc, stsz);

      std::vector<kdu_byte> co64;
      AppendBe32(co64, 0);
      AppendBe32(co64, 1);
      AppendBe64(co64, cs_offset);
      AppendBox(stbl, mj2_chunk_offset64_4cc, co64);
      return stbl;
    };

    auto assemble_track = [&](kdu_uint32 tid, kdu_uint32 tkhd_flags,
                              const std::array<double, 9>& mat,
                              kdu_uint32 htype, kdu_uint32 vmhd_vf,
                              kdu_uint16 vmhd_mode, kdu_uint32 dref_entries,
                              kdu_uint32 url_flags, kdu_uint32 stsd_entries,
                              kdu_uint32 cs_offset) {
      std::vector<kdu_byte> trak;
      AppendBox(trak, mj2_track_header_4cc,
                make_tkhd_v1(tkhd_flags, tid, mat, 8.0, 8.0));
      std::vector<kdu_byte> mdia;
      AppendBox(mdia, mj2_media_header_typo_4cc, make_mdhd_v1());
      AppendBox(mdia, mj2_media_handler_4cc, make_hdlr(htype));
      std::vector<kdu_byte> minf;
      AppendBox(minf, mj2_video_media_header_4cc,
                make_vmhd(vmhd_vf, vmhd_mode));
      std::vector<kdu_byte> dinf;
      AppendBox(dinf, mj2_data_reference_4cc,
                make_dref(dref_entries, url_flags));
      AppendBox(minf, mj2_data_information_4cc, dinf);
      if (stsd_entries != 1) {
        std::vector<kdu_byte> stbl;
        std::vector<kdu_byte> stsd;
        AppendBe32(stsd, 0);
        AppendBe32(stsd, stsd_entries);
        AppendBox(stbl, mj2_sample_description_4cc, stsd);
        AppendBox(minf, mj2_sample_table_4cc, stbl);
      } else {
        AppendBox(minf, mj2_sample_table_4cc, make_stbl_co64(cs_offset));
      }
      AppendBox(mdia, mj2_media_information_4cc, minf);
      AppendBox(trak, mj2_media_4cc, mdia);
      return trak;
    };

    // Build once to get exact codestream offset inside mdat.
    auto header_probe = build_synthetic_mj2({}, true);
    offset_placeholder = header_probe.second;

    std::vector<kdu_byte> moov;
    AppendBox(moov, mj2_movie_header_4cc, make_mvhd_v1());
    // Track 1: transposed & negative axes + bad vmhd mode warning
    AppendBox(moov, mj2_track_4cc,
              assemble_track(
                  1, 3, {0.0, -1.0, 10.0, 1.0, 0.0, 10.0, 0.0, 0.0, 1.0},
                  mj2_video_handler_4cc, 1, 99, 1, 1, 1, offset_placeholder));
    // Track 2: larger + negative shifted bounds + unknown vmhd flags=0 warning
    AppendBox(moov, mj2_track_4cc,
              assemble_track(
                  2, 3, {-1.0, 0.0, -20.0, 0.0, -1.0, -20.0, 0.0, 0.0, 1.0},
                  mj2_video_handler_4cc, 0, 0, 1, 1, 1, offset_placeholder));
    // Track 3: disabled via tkhd flags=0
    AppendBox(
        moov, mj2_track_4cc,
        assemble_track(3, 0, {1, 0, 0, 0, 1, 0, 0, 0, 1}, mj2_video_handler_4cc,
                       1, 0, 1, 1, 1, offset_placeholder));
    // Track 4: non-video handler ('soun')
    AppendBox(moov, mj2_track_4cc,
              assemble_track(4, 3, {1, 0, 0, 0, 1, 0, 0, 0, 1}, 0x736f756e, 1,
                             0, 1, 1, 1, offset_placeholder));

    auto [bytes, cs_off] = build_synthetic_mj2(moov, true);
    ASSERT_EQ(cs_off, offset_placeholder);

    MemSource msrc(bytes);
    jp2_family_src fsrc;
    fsrc.open(&msrc);
    mj2_source mj;
    ASSERT_EQ(mj.open(&fsrc), 1);
    EXPECT_FALSE(!mj.get_movie_dims());
    EXPECT_EQ(mj.get_track_type(4), MJ2_TRACK_IS_OTHER);
    auto* v1 = mj.access_video_track(1);
    ASSERT_NE(v1, nullptr);
    kdu_dims pre;
    bool tr = false, vf = false, hf = false;
    EXPECT_TRUE(v1->get_cardinal_geometry(pre, tr, vf, hf, false));
    EXPECT_TRUE(v1->get_cardinal_geometry(pre, tr, vf, hf, true));
    EXPECT_EQ(v1->open_image(), 0);
    v1->close_image();
    mj.close();
    fsrc.close();

    // Disabled tracks (external URL dref, multi-entry dref, multi-entry stsd).
    // Tested separately because mj2_source::get_movie_dims() dereferences
    // scan->video_track->get_source() without checking initialized/disabled.
    std::vector<kdu_byte> moov_disabled;
    AppendBox(moov_disabled, mj2_movie_header_4cc, make_mvhd_v1());
    AppendBox(
        moov_disabled, mj2_track_4cc,
        assemble_track(5, 3, {1, 0, 0, 0, 1, 0, 0, 0, 1}, mj2_video_handler_4cc,
                       1, 0, 1, 0, 1, offset_placeholder));
    AppendBox(
        moov_disabled, mj2_track_4cc,
        assemble_track(6, 3, {1, 0, 0, 0, 1, 0, 0, 0, 1}, mj2_video_handler_4cc,
                       1, 0, 2, 1, 1, offset_placeholder));
    AppendBox(
        moov_disabled, mj2_track_4cc,
        assemble_track(7, 3, {1, 0, 0, 0, 1, 0, 0, 0, 1}, mj2_video_handler_4cc,
                       1, 0, 1, 1, 2, offset_placeholder));
    auto [dis_bytes, dis_off] = build_synthetic_mj2(moov_disabled, true);
    ASSERT_EQ(dis_off, offset_placeholder);
    MemSource dis_msrc(dis_bytes);
    jp2_family_src dis_fsrc;
    dis_fsrc.open(&dis_msrc);
    mj2_source mj_dis;
    ASSERT_EQ(mj_dis.open(&dis_fsrc), 1);
    EXPECT_EQ(mj_dis.access_video_track(5), nullptr);
    EXPECT_EQ(mj_dis.access_video_track(6), nullptr);
    EXPECT_EQ(mj_dis.access_video_track(7), nullptr);
    mj_dis.close();
    dis_fsrc.close();
  }

  // Malformed box & corrupt stream error paths.
  {
    std::vector<kdu_byte> mvhd_ok(100, 0);
    // mvhd bad version (2), truncated mvhd, extra bytes in mvhd.
    {
      std::vector<kdu_byte> mvhd_badver = {2, 0, 0, 0};
      std::vector<kdu_byte> moov;
      AppendBox(moov, mj2_movie_header_4cc, mvhd_badver);
      EXPECT_THROW(open_and_close(build_synthetic_mj2(moov).first, false),
                   kdu_exception);

      std::vector<kdu_byte> mvhd_short(20, 0);
      moov.clear();
      AppendBox(moov, mj2_movie_header_4cc, mvhd_short);
      EXPECT_THROW(open_and_close(build_synthetic_mj2(moov).first, false),
                   kdu_exception);

      std::vector<kdu_byte> mvhd_long(120, 0);
      moov.clear();
      AppendBox(moov, mj2_movie_header_4cc, mvhd_long);
      EXPECT_THROW(open_and_close(build_synthetic_mj2(moov).first, false),
                   kdu_exception);
    }

    // Track missing required boxes / malformed subboxes.
    auto test_bad_moov = [&](const std::vector<kdu_byte>& trak_payload) {
      std::vector<kdu_byte> moov;
      AppendBox(moov, mj2_movie_header_4cc, mvhd_ok);
      AppendBox(moov, mj2_track_4cc, trak_payload);
      EXPECT_THROW(open_and_close(build_synthetic_mj2(moov).first, false),
                   kdu_exception);
    };

    // Empty trak -> missing tkhd.
    test_bad_moov({});
    // tkhd bad version, short tkhd, long tkhd.
    {
      std::vector<kdu_byte> p;
      AppendBox(p, mj2_track_header_4cc, {2, 0, 0, 0});
      test_bad_moov(p);
      p.clear();
      AppendBox(p, mj2_track_header_4cc, std::vector<kdu_byte>(20, 0));
      test_bad_moov(p);
      p.clear();
      AppendBox(p, mj2_track_header_4cc, std::vector<kdu_byte>(120, 0));
      test_bad_moov(p);
    }

    // Valid v0 tkhd (flags=1).
    std::vector<kdu_byte> tkhd_v0(84, 0);
    tkhd_v0[3] = 1;
    tkhd_v0[15] = 1;

    // Missing mdhd.
    {
      std::vector<kdu_byte> trak;
      AppendBox(trak, mj2_track_header_4cc, tkhd_v0);
      test_bad_moov(trak);
    }
    // Malformed mdhd (bad ver, short, long) & missing hdlr.
    {
      std::vector<kdu_byte> mdhd_v0(24, 0);
      std::vector<kdu_byte> hdlr(24, 0);
      hdlr[8] = 'v';
      hdlr[9] = 'i';
      hdlr[10] = 'd';
      hdlr[11] = 'e';

      for (const auto& bad_mdhd :
           {std::vector<kdu_byte>{2, 0, 0, 0}, std::vector<kdu_byte>(12, 0),
            std::vector<kdu_byte>(40, 0)}) {
        std::vector<kdu_byte> trak, mdia;
        AppendBox(trak, mj2_track_header_4cc, tkhd_v0);
        AppendBox(mdia, mj2_media_header_4cc, bad_mdhd);
        AppendBox(trak, mj2_media_4cc, mdia);
        test_bad_moov(trak);
      }
      // Missing hdlr
      {
        std::vector<kdu_byte> trak, mdia;
        AppendBox(trak, mj2_track_header_4cc, tkhd_v0);
        AppendBox(mdia, mj2_media_header_4cc, mdhd_v0);
        AppendBox(trak, mj2_media_4cc, mdia);
        test_bad_moov(trak);
      }
      // Bad hdlr (ver!=0, short)
      for (const auto& bad_hdlr :
           {std::vector<kdu_byte>{1, 0, 0, 0}, std::vector<kdu_byte>(8, 0)}) {
        std::vector<kdu_byte> trak, mdia;
        AppendBox(trak, mj2_track_header_4cc, tkhd_v0);
        AppendBox(mdia, mj2_media_header_4cc, mdhd_v0);
        AppendBox(mdia, mj2_media_handler_4cc, bad_hdlr);
        AppendBox(trak, mj2_media_4cc, mdia);
        test_bad_moov(trak);
      }
      // Bad dref, stsd, stts, stsc, stsz, stco boxes inside minf.
      auto wrap_minf = [&](const std::vector<kdu_byte>& minf) {
        std::vector<kdu_byte> trak, mdia;
        AppendBox(trak, mj2_track_header_4cc, tkhd_v0);
        AppendBox(mdia, mj2_media_header_4cc, mdhd_v0);
        AppendBox(mdia, mj2_media_handler_4cc, hdlr);
        AppendBox(mdia, mj2_media_information_4cc, minf);
        AppendBox(trak, mj2_media_4cc, mdia);
        test_bad_moov(trak);
      };
      // Bad vmhd (truncated)
      {
        std::vector<kdu_byte> minf;
        AppendBox(minf, mj2_video_media_header_4cc, {0, 0, 0, 1, 0, 0});
        wrap_minf(minf);
      }
      // Bad dref (ver!=0, missing url)
      {
        std::vector<kdu_byte> minf, dinf;
        AppendBox(dinf, mj2_data_reference_4cc, {1, 0, 0, 0});
        AppendBox(minf, mj2_data_information_4cc, dinf);
        wrap_minf(minf);
      }
      {
        std::vector<kdu_byte> minf, dinf;
        AppendBox(dinf, mj2_data_reference_4cc, {0, 0, 0, 0, 0, 0, 0, 1});
        AppendBox(minf, mj2_data_information_4cc, dinf);
        wrap_minf(minf);
      }
      // Bad stsd (missing entry / missing jp2h / bad fiel)
      {
        std::vector<kdu_byte> minf, stbl;
        AppendBox(stbl, mj2_sample_description_4cc, {0, 0, 0, 0, 0, 0, 0, 1});
        AppendBox(minf, mj2_sample_table_4cc, stbl);
        wrap_minf(minf);
      }
      {
        std::vector<kdu_byte> minf, stbl, stsd;
        AppendBe32(stsd, 0);
        AppendBe32(stsd, 1);
        std::vector<kdu_byte> mjp2(78, 0);
        mjp2[7] = 1;
        AppendBox(stsd, mj2_visual_sample_entry_4cc, mjp2);
        AppendBox(stbl, mj2_sample_description_4cc, stsd);
        AppendBox(minf, mj2_sample_table_4cc, stbl);
        wrap_minf(minf);
      }
      // Bad stts / stsc / stsz / stco / co64 and duplicates.
      auto make_valid_stsd = [&]() {
        std::vector<kdu_byte> stsd;
        AppendBe32(stsd, 0);
        AppendBe32(stsd, 1);
        std::vector<kdu_byte> mjp2(78, 0);
        mjp2[7] = 1;
        AppendBox(mjp2, jp2_header_4cc, MakeMinimalJp2hPayload(8, 8, 1, 8));
        AppendBox(stsd, mj2_visual_sample_entry_4cc, mjp2);
        return stsd;
      };
      // Multiple stsz
      {
        std::vector<kdu_byte> minf, stbl;
        AppendBox(stbl, mj2_sample_description_4cc, make_valid_stsd());
        AppendBox(stbl, mj2_sample_size_4cc,
                  {0, 0, 0, 0, 0, 0, 0, 10, 0, 0, 0, 0});
        AppendBox(stbl, mj2_sample_size_4cc,
                  {0, 0, 0, 0, 0, 0, 0, 10, 0, 0, 0, 0});
        AppendBox(minf, mj2_sample_table_4cc, stbl);
        wrap_minf(minf);
      }
      // Bad stsz (ver!=0, truncated samples, extra bytes)
      for (const auto& bsz :
           {std::vector<kdu_byte>{1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
            std::vector<kdu_byte>{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
            std::vector<kdu_byte>{0, 0, 0, 0, 0, 0, 0, 5, 0, 0, 0, 0, 0}}) {
        std::vector<kdu_byte> minf, stbl;
        AppendBox(stbl, mj2_sample_size_4cc, bsz);
        AppendBox(minf, mj2_sample_table_4cc, stbl);
        wrap_minf(minf);
      }
      // Bad stco / co64 (duplicate, ver!=0, short, long, >32bit in stco)
      {
        std::vector<kdu_byte> minf, stbl;
        AppendBox(stbl, mj2_chunk_offset_4cc, {0, 0, 0, 0, 0, 0, 0, 0});
        AppendBox(stbl, mj2_chunk_offset_4cc, {0, 0, 0, 0, 0, 0, 0, 0});
        AppendBox(minf, mj2_sample_table_4cc, stbl);
        wrap_minf(minf);
      }
      for (const auto& bco :
           {std::vector<kdu_byte>{1, 0, 0, 0, 0, 0, 0, 0},
            std::vector<kdu_byte>{0, 0, 0, 0, 0, 0, 0, 1},
            std::vector<kdu_byte>{0, 0, 0, 0, 0, 0, 0, 0, 99}}) {
        std::vector<kdu_byte> minf, stbl;
        AppendBox(stbl, mj2_chunk_offset_4cc, bco);
        AppendBox(minf, mj2_sample_table_4cc, stbl);
        wrap_minf(minf);
      }
      // Bad co64 truncated
      {
        std::vector<kdu_byte> minf, stbl;
        AppendBox(stbl, mj2_chunk_offset64_4cc,
                  {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
        AppendBox(minf, mj2_sample_table_4cc, stbl);
        wrap_minf(minf);
      }
      // Bad stsc (duplicate, ver!=0, truncated, desc!=1, non-inc, extra)
      {
        std::vector<kdu_byte> minf, stbl;
        AppendBox(stbl, mj2_sample_to_chunk_4cc, {0, 0, 0, 0, 0, 0, 0, 0});
        AppendBox(stbl, mj2_sample_to_chunk_4cc, {0, 0, 0, 0, 0, 0, 0, 0});
        AppendBox(minf, mj2_sample_table_4cc, stbl);
        wrap_minf(minf);
      }
      for (const auto& bsc :
           {std::vector<kdu_byte>{1, 0, 0, 0, 0, 0, 0, 0},
            std::vector<kdu_byte>{0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1},
            std::vector<kdu_byte>{0, 0, 0, 0, 0, 0, 0, 1, 0, 0,
                                  0, 1, 0, 0, 0, 1, 0, 0, 0, 2},
            std::vector<kdu_byte>{0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0,
                                  2, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0,
                                  0, 1, 0, 0, 0, 1, 0, 0, 0, 1},
            std::vector<kdu_byte>{0, 0, 0, 0, 0, 0, 0, 0, 9}}) {
        std::vector<kdu_byte> minf, stbl;
        AppendBox(stbl, mj2_sample_to_chunk_4cc, bsc);
        AppendBox(minf, mj2_sample_table_4cc, stbl);
        wrap_minf(minf);
      }
      // Bad stts (duplicate, ver!=0, truncated, extra)
      {
        std::vector<kdu_byte> minf, stbl;
        AppendBox(stbl, mj2_time_to_sample_4cc, {0, 0, 0, 0, 0, 0, 0, 0});
        AppendBox(stbl, mj2_time_to_sample_4cc, {0, 0, 0, 0, 0, 0, 0, 0});
        AppendBox(minf, mj2_sample_table_4cc, stbl);
        wrap_minf(minf);
      }
      for (const auto& bts :
           {std::vector<kdu_byte>{1, 0, 0, 0, 0, 0, 0, 0},
            std::vector<kdu_byte>{0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1},
            std::vector<kdu_byte>{0, 0, 0, 0, 0, 0, 0, 0, 9}}) {
        std::vector<kdu_byte> minf, stbl;
        AppendBox(stbl, mj2_time_to_sample_4cc, bts);
        AppendBox(minf, mj2_sample_table_4cc, stbl);
        wrap_minf(minf);
      }
    }
  }
}

}  // namespace
