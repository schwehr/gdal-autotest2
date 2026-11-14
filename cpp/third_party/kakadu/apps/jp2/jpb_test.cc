// Copyright 2026 Google LLC

#include "third_party/kakadu/vendor/apps/compressed_io/jpb.h"

#include <algorithm>
#include <array>
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
using kdu_core::kdu_exception;
using kdu_core::kdu_line_buf;
using kdu_core::kdu_long;
using kdu_core::kdu_membroker;
using kdu_core::kdu_message;
using kdu_core::kdu_params;
using kdu_core::kdu_push_ifc;
using kdu_core::kdu_push_pull_params;
using kdu_core::kdu_sample16;
using kdu_core::kdu_sample_allocator;
using kdu_core::kdu_uint16;
using kdu_core::kdu_uint32;
using kdu_core::siz_params;
using kdu_supp::jp2_box_textualizer;
using kdu_supp::jp2_family_src;
using kdu_supp::jp2_family_tgt;
using kdu_supp::jp2_input_box;
using kdu_supp::jpb_add_box_descriptions;
using kdu_supp::jpb_source;
using kdu_supp::jpb_target;
using kdu_supp::kdu_field_order;

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
    if (num_bytes < 0) return false;
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

  const std::vector<kdu_byte>& data() const { return data_; }
  std::vector<kdu_byte>& mutable_data() { return data_; }

 private:
  std::vector<kdu_byte> data_;
  size_t pos_ = 0;
  bool in_rewrite_ = false;
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

kdu_uint32 BcdTc(int hh, int mm, int ss, int ff) {
  auto bcd = [](int v) -> kdu_uint32 {
    return static_cast<kdu_uint32>(((v / 10) << 4) | (v % 10));
  };
  return (bcd(hh) << 24) | (bcd(mm) << 16) | (bcd(ss) << 8) | bcd(ff);
}

void CompressTinyImage(kdu_codestream& codestream, kdu_compressed_target& tgt,
                       const std::vector<const char*>& params) {
  siz_params siz;
  siz.set(Sdims, 0, 0, 8);
  siz.set(Sdims, 0, 1, 8);
  siz.set(Scomponents, 0, 0, 1);
  siz.set(Sprecision, 0, 0, 8);
  siz.set(Ssigned, 0, 0, false);
  for (const char* opt : params) {
    if (opt[0] == 'S') {
      ASSERT_TRUE(siz.parse_string(opt)) << opt;
    }
  }
  static_cast<kdu_params&>(siz).finalize_all();
  codestream.create(&siz, &tgt);
  kdu_params* cod = codestream.access_siz()->access_cluster(COD_params);
  kdu_params* org = codestream.access_siz()->access_cluster(ORG_params);
  for (const char* opt : params) {
    if (opt[0] == 'C') {
      ASSERT_TRUE(cod->parse_string(opt)) << opt;
    } else if (opt[0] == 'O') {
      ASSERT_TRUE(org->parse_string(opt)) << opt;
    }
  }
  codestream.access_siz()->finalize_all();
  auto tile = codestream.open_tile(kdu_coords(0, 0));
  const int num_comps = codestream.get_num_components();
  kdu_sample_allocator alloc;
  kdu_push_pull_params pp_params;
  std::vector<kdu_push_ifc> engines(num_comps);
  for (int c = 0; c < num_comps; ++c) {
    auto res = tile.access_component(c).access_resolution();
    engines[c] = kdu_analysis(res, &alloc, pp_params, KDU_SAMPLE_PREC16, 1.0F,
                              0, nullptr);
  }
  kdu_line_buf line;
  line.pre_create(&alloc, 8, KDU_SAMPLE_PREC16, 0, 0);
  alloc.finalize(codestream);
  line.create();
  for (int i = 0; i < 8; ++i) {
    line.get_buf16()[i].ival = static_cast<std::int16_t>(i * 10);
  }
  for (int c = 0; c < num_comps; ++c) {
    for (int r = 0; r < 8; ++r) {
      engines[c].push(line);
    }
    engines[c].destroy();
  }
  line.destroy();
  tile.close();
  kdu_long max_bytes = 65536;
  codestream.flush(&max_bytes, 1);
}

TEST(JpbTest, TargetOpenValidationAndErrors) {
  MemTarget mtgt;
  jp2_family_tgt ftgt;
  ftgt.open(&mtgt);

  jpb_target jbt;
  EXPECT_FALSE(jbt.exists());
  EXPECT_TRUE(!jbt);
  EXPECT_FALSE(jbt.close());
  EXPECT_EQ(jbt.get_membroker(), nullptr);
  EXPECT_EQ(jbt.get_next_timecode(), 0U);
  EXPECT_EQ(jbt.get_last_timecode(), 0U);

  EXPECT_THROW(
      jbt.open(&ftgt, 0, 1, kdu_supp::KDU_FIELDS_NONE, JPB_SRGB_SPACE, 1000000),
      kdu_exception);
  EXPECT_THROW(jbt.open(&ftgt, 30, 0, kdu_supp::KDU_FIELDS_NONE, JPB_SRGB_SPACE,
                        1000000),
               kdu_exception);
  EXPECT_THROW(jbt.open(&ftgt, 30, 1, kdu_supp::KDU_FIELDS_NONE,
                        JPB_BT2020_SPACE, 1000000),
               kdu_exception);
  EXPECT_THROW(
      jbt.open(&ftgt, 30, 1, kdu_supp::KDU_FIELDS_NONE, JPB_SRGB_SPACE, 0),
      kdu_exception);
  // Closed-state guards before any successful open.
  EXPECT_THROW(jbt.open_image(), kdu_exception);
  EXPECT_THROW(jbt.close_image(kdu_codestream()), kdu_exception);

  // Exceeding 100 fps and invalid initial timecode fields (close after throw).
  for (kdu_uint32 bad_tc : {0x24000000U, 0x00600000U, 0x00006000U, 0x0A000000U,
                            BcdTc(0, 0, 0, 30)}) {
    jpb_target bad_jbt;
    EXPECT_THROW(
        bad_jbt.open(&ftgt, 30, 1, kdu_supp::KDU_FIELDS_NONE, JPB_SRGB_SPACE,
                     1000000, bad_tc, JPB_TIMEFLAG_NDF),
        kdu_exception);
    bad_jbt.close();
  }
  {
    jpb_target fast_jbt;
    EXPECT_THROW(fast_jbt.open(&ftgt, 120, 1, kdu_supp::KDU_FIELDS_NONE,
                               JPB_SRGB_SPACE, 1000000),
                 kdu_exception);
    fast_jbt.close();
  }

  ftgt.close();
}

TEST(JpbTest, TargetProgressiveTimecodesDmonAndRewrite) {
  kdu_membroker broker;
  MemTarget mtgt;
  jp2_family_tgt ftgt;
  ftgt.open(&mtgt, &broker);

  // Common divisor test: 60000 / 2002 simplifies to 30000 / 1001.
  // Re-opening overwrites nothing until close, exercise open + close.
  jpb_target jbt;
  jbt.open(&ftgt, 60000, 2002, kdu_supp::KDU_FIELDS_NONE, JPB_709_SPACE, 800000,
           BcdTc(0, 0, 59, 28), JPB_TIMEFLAG_DF2);
  EXPECT_TRUE(jbt.exists());
  EXPECT_FALSE(!jbt);
  EXPECT_EQ(jbt.get_membroker(), &broker);
  EXPECT_EQ(jbt.get_next_timecode(), BcdTc(0, 0, 59, 28));

  // Double open closes first cleanly.
  jbt.open(&ftgt, 60000, 1001, kdu_supp::KDU_FIELDS_NONE, JPB_709_SPACE, 800000,
           BcdTc(0, 0, 0, 0), JPB_TIMEFLAG_DF2 | JPB_TIMEFLAG_FRAME_PAIRS);
  jbt.open(&ftgt, 30000, 1001, kdu_supp::KDU_FIELDS_NONE, JPB_709_SPACE, 800000,
           BcdTc(0, 1, 0, 0), JPB_TIMEFLAG_DF2);
  // Notice 00:01:00:00 gets adjusted to drop first 2 frames at non-10th minute.
  EXPECT_EQ(jbt.get_next_timecode(), BcdTc(0, 1, 0, 2));

  // Set out-of-range mastering display coordinates to cover clamping branches.
  jbt.set_mastering_display_info(1.2F, -0.1F, -0.2F, 1.3F, 0.15F, 0.06F,
                                 0.3127F, 0.329F, 0.005, 1000.0);
  jbt.set_mastering_display_info(1.2F, 1.2F, 1.2F, 1.2F, 1.2F, 1.2F, 1.2F, 1.2F,
                                 1e12, 1e-6);

  const std::vector<kdu_byte> payload0 = {0xFF, 0x4F, 0x01, 0x02, 0x03, 0x04};
  jbt.open_image();
  EXPECT_THROW(jbt.open_image(), kdu_exception);
  EXPECT_FALSE(jbt.end_rewrite());
  EXPECT_TRUE(jbt.write(payload0.data(), static_cast<int>(payload0.size())));
  // Rewrite first two bytes then restore write pos.
  EXPECT_FALSE(jbt.start_rewrite(-1));
  EXPECT_FALSE(jbt.start_rewrite(100));
  EXPECT_TRUE(jbt.start_rewrite(4));
  EXPECT_FALSE(jbt.start_rewrite(1));  // Nested rewrite fails.
  const std::vector<kdu_byte> patch = {0xAA, 0xBB};
  EXPECT_TRUE(jbt.write(patch.data(), 2));
  const std::vector<kdu_byte> too_much = {1, 2, 3};
  EXPECT_FALSE(jbt.write(too_much.data(), 3));
  EXPECT_TRUE(jbt.end_rewrite());
  jbt.close_image(kdu_codestream());
  EXPECT_EQ(jbt.get_last_timecode(), BcdTc(0, 1, 0, 2));

  // Set next timecode near minute roll-over + invalid timecode error.
  EXPECT_THROW(jbt.set_next_timecode(BcdTc(0, 0, 0, 30)), kdu_exception);
  jbt.set_next_timecode(BcdTc(0, 9, 59, 29));
  jbt.open_image();
  jbt.write(payload0.data(), static_cast<int>(payload0.size()));
  jbt.close_image(kdu_codestream());
  EXPECT_EQ(jbt.get_last_timecode(), BcdTc(0, 9, 59, 29));
  EXPECT_EQ(jbt.get_next_timecode(), BcdTc(0, 10, 0, 0));

  // Write a third frame hitting 00:01:00:00 adjustment via set_next_timecode.
  jbt.set_next_timecode(BcdTc(0, 1, 0, 0));
  EXPECT_EQ(jbt.get_next_timecode(), BcdTc(0, 1, 0, 2));
  // Disable mastering display via negative max_lum.
  jbt.set_mastering_display_info(0.68F, 0.32F, 0.265F, 0.69F, 0.15F, 0.06F,
                                 0.3127F, 0.329F, -1.0F, 0.0F);
  jbt.open_image();
  jbt.write(payload0.data(), static_cast<int>(payload0.size()));
  jbt.close_image(kdu_codestream());

  // Overflow max_codestream_length check; closing while image open returns
  // false.
  jbt.open_image();
  std::vector<kdu_byte> big(20000, 0x11);
  EXPECT_THROW(jbt.write(big.data(), static_cast<int>(big.size())),
               kdu_exception);
  EXPECT_FALSE(jbt.close());
  EXPECT_FALSE(jbt.close());
  ftgt.close();
}

TEST(JpbTest, TargetSubOneHzAndTimecodeVariants) {
  MemTarget mtgt;
  jp2_family_tgt ftgt;
  ftgt.open(&mtgt);

  // Sub-1Hz rate (1 frame every 2 seconds) exercises skip_seconds path.
  jpb_target jbt;
  jbt.open(&ftgt, 1, 2, kdu_supp::KDU_FIELDS_NONE, JPB_601_SPACE, 800000,
           BcdTc(23, 59, 59, 0), JPB_TIMEFLAG_NDF);
  const kdu_byte b = 0x55;
  jbt.open_image();
  jbt.write(&b, 1);
  jbt.close_image(kdu_codestream());
  EXPECT_EQ(jbt.get_last_timecode(), BcdTc(23, 59, 59, 0));
  EXPECT_EQ(jbt.get_next_timecode(), BcdTc(24, 0, 2, 0));
  EXPECT_TRUE(jbt.close());

  // DF4 at 59.94Hz (60000/1001).
  jbt.open(&ftgt, 60000, 1001, kdu_supp::KDU_FIELDS_NONE, JPB_LUV_SPACE, 800000,
           BcdTc(0, 0, 59, 59), JPB_TIMEFLAG_DF4);
  jbt.open_image();
  jbt.write(&b, 1);
  jbt.close_image(kdu_codestream());
  EXPECT_EQ(jbt.get_next_timecode(), BcdTc(0, 1, 0, 4));
  EXPECT_TRUE(jbt.close());

  // DF2 at 24000/1001 (~23.976fps).
  jbt.open(&ftgt, 24000, 1001, kdu_supp::KDU_FIELDS_NONE, JPB_XYZ_SPACE, 800000,
           0, JPB_TIMEFLAG_DF2);
  jbt.open_image();
  jbt.write(&b, 1);
  jbt.close_image(kdu_codestream());
  EXPECT_TRUE(jbt.close());
  ftgt.close();
}

TEST(JpbTest, TargetCodestreamProfileChecks) {
  auto run_profile = [](kdu_uint32 bitrate,
                        const std::vector<const char*>& params) {
    MemTarget mtgt;
    jp2_family_tgt ftgt;
    ftgt.open(&mtgt);
    jpb_target jbt;
    kdu_codestream cs;
    auto cleanup = [&]() {
      if (cs.exists()) cs.destroy();
      jbt.close();
      ftgt.close();
    };
    try {
      jbt.open(&ftgt, 60, 1, kdu_supp::KDU_FIELDS_NONE, JPB_SRGB_SPACE,
               bitrate);
      jbt.open_image();
      CompressTinyImage(cs, jbt, params);
      jbt.close_image(cs);
    } catch (...) {
      cleanup();
      throw;
    }
    cleanup();
  };

  const std::vector<const char*> common = {
      "Scomponents=3",       "Sprecision=12", "Ssigned=no",
      "Ssampling={1,1}",     "Clevels=1",     "Clayers=1",
      "Creversible=no",      "Corder=CPRL",   "Cblk={32,32}",
      "Cprecincts={256,256}"};
  auto with_profile = [&](std::vector<const char*> head) {
    head.insert(head.end(), common.begin(), common.end());
    return head;
  };

  auto expect_ok = [&](kdu_uint32 bitrate,
                       const std::vector<const char*>& params) {
    try {
      run_profile(bitrate, params);
    } catch (kdu_exception) {
      ADD_FAILURE() << "Unexpected kdu_exception: " << LastErrorText();
    }
  };

  expect_ok(244000000, with_profile({"Sprofile=CINEMA2K"}));
  expect_ok(244000000, with_profile({"Sprofile=CINEMA4K"}));
  EXPECT_THROW(run_profile(244000001, with_profile({"Sprofile=CINEMA2K"})),
               kdu_exception);

  expect_ok(200000000, with_profile({"Sprofile=BROADCAST",
                                     "Sbroadcast={1,single,irrev}"}));
  expect_ok(400000000, with_profile({"Sprofile=BROADCAST",
                                     "Sbroadcast={4,single,irrev}"}));
  expect_ok(800000000, with_profile({"Sprofile=BROADCAST",
                                     "Sbroadcast={5,single,irrev}"}));
  expect_ok(1600000000, with_profile({"Sprofile=BROADCAST",
                                      "Sbroadcast={6,single,irrev}"}));
  EXPECT_THROW(
      run_profile(1600000001, with_profile({"Sprofile=BROADCAST",
                                            "Sbroadcast={6,single,irrev}"})),
      kdu_exception);
  EXPECT_THROW(run_profile(10000000, {"Sprofile=PART0"}), kdu_exception);
}

TEST(JpbTest, InterlacedRoundTripStreamsSeekingAndTextualizer) {
  MemTarget mtgt;
  jp2_family_tgt ftgt;
  ftgt.open(&mtgt);

  jpb_target jbt;
  jbt.open(&ftgt, 30, 1, kdu_supp::KDU_FIELDS_TOP_FIRST, JPB_SRGB_SPACE, 800000,
           BcdTc(1, 2, 3, 4), JPB_TIMEFLAG_NDF);
  jbt.set_mastering_display_info(0.68F, 0.32F, 0.265F, 0.69F, 0.15F, 0.06F,
                                 0.3127F, 0.329F, 0.001, 1000.0);

  // Write 3 interlaced frames (6 fields total).
  for (int f = 0; f < 3; ++f) {
    const std::array<kdu_byte, 4> field0 = {static_cast<kdu_byte>(0xA0 + f),
                                            0x01, 0x02, 0x03};
    const std::array<kdu_byte, 5> field1 = {static_cast<kdu_byte>(0xB0 + f),
                                            0x11, 0x12, 0x13, 0x14};
    jbt.open_image();
    EXPECT_TRUE(jbt.write(field0.data(), static_cast<int>(field0.size())));
    jbt.close_image(kdu_codestream());

    jbt.open_image();
    EXPECT_TRUE(jbt.write(field1.data(), static_cast<int>(field1.size())));
    jbt.close_image(kdu_codestream());
  }
  // Open only field 0 of frame 4, close early without writing field 1.
  jbt.open_image();
  const kdu_byte partial = 0xCC;
  jbt.write(&partial, 1);
  jbt.close_image(kdu_codestream());
  jbt.close();
  ftgt.close();

  // Verify box textualization of the written ELSM stream.
  MemSource tsrc(mtgt.data());
  jp2_family_src fsrc;
  fsrc.open(&tsrc);
  jp2_box_textualizer tex;
  tex.add_box_type(jpb_mastering_display_4cc, "mastering-display");
  jpb_add_box_descriptions(tex);
  jp2_input_box elsm_box;
  ASSERT_TRUE(elsm_box.open(&fsrc));
  jp2_input_box sub;
  int sub_count = 0;
  if (sub.open(&elsm_box)) {
    do {
      CaptureMessage msg;
      const bool ok =
          tex.textualize_box(&sub, msg, /*xml_embedded=*/false, 1024);
      if (sub.get_box_type() != jp2_codestream_4cc) {
        EXPECT_TRUE(ok);
        EXPECT_FALSE(msg.text.empty());
      } else {
        EXPECT_FALSE(ok);
      }
      ++sub_count;
      sub.close();
    } while (sub.open_next());
  }
  EXPECT_GE(sub_count, 6);
  elsm_box.close();
  fsrc.close();

  // Open via jpb_source and exercise reading, fields, streams, and seeking.
  kdu_membroker broker;
  MemSource msrc(mtgt.data(), /*seekable=*/true);
  fsrc.open(&msrc, &broker);

  jpb_source jps;
  EXPECT_FALSE(jps.exists());
  EXPECT_TRUE(!jps);
  EXPECT_FALSE(jps.close());
  EXPECT_EQ(jps.get_ultimate_src(), nullptr);
  EXPECT_EQ(jps.get_membroker(), nullptr);
  EXPECT_EQ(jps.get_timescale(), 0U);
  EXPECT_EQ(jps.get_frame_instant(), 0);
  EXPECT_EQ(jps.get_frame_period(), 0);
  EXPECT_EQ(jps.get_field_order(), kdu_supp::KDU_FIELDS_NONE);
  EXPECT_EQ(jps.get_frame_space(), JPB_UNKNOWN_SPACE);
  EXPECT_EQ(jps.get_frame_timecode(), 0xFFFFFFFFU);
  EXPECT_EQ(jps.get_capabilities(), 0);
  EXPECT_EQ(jps.get_pos(), -1);
  EXPECT_FALSE(jps.seek(0));
  EXPECT_FALSE(jps.seek_to_frame(0));
  EXPECT_EQ(jps.open_image(), -1);
  jp2_input_box tmp_box;
  EXPECT_EQ(jps.open_stream(0, &tmp_box), -1);
  jps.set_field_mode(0);

  ASSERT_EQ(jps.open(&fsrc), 1);
  EXPECT_TRUE(jps.exists());
  EXPECT_EQ(jps.get_ultimate_src(), &fsrc);
  EXPECT_EQ(jps.get_membroker(), &broker);
  EXPECT_EQ(jps.get_timescale(), 30U);
  EXPECT_EQ(jps.get_frame_period(), 1);
  EXPECT_EQ(jps.get_field_order(), kdu_supp::KDU_FIELDS_TOP_FIRST);
  EXPECT_EQ(jps.get_frame_space(), JPB_SRGB_SPACE);
  EXPECT_EQ(jps.get_frame_timecode(), BcdTc(1, 2, 3, 4));

  float x1 = 0, y1 = 0, x2 = 0, y2 = 0, x3 = 0, y3 = 0, xw = 0, yw = 0;
  double lmin = 0, lmax = 0;
  EXPECT_TRUE(jps.get_mastering_display_info(x1, y1, x2, y2, x3, y3, xw, yw,
                                             lmin, lmax));
  EXPECT_NEAR(lmax, 1000.0, 1.0);
  EXPECT_NEAR(lmin, 0.001, 0.001);

  jps.set_field_mode(-1);
  jps.set_field_mode(3);
  jps.set_field_mode(0);

  // Open stream fields 0 and 1 directly and close.
  jp2_input_box sbox0;
  jp2_input_box sbox1;
  EXPECT_EQ(jps.open_stream(-1, &sbox0), -1);
  EXPECT_EQ(jps.open_stream(2, &sbox0), -1);
  EXPECT_EQ(jps.open_stream(0, &sbox0), 0);
  sbox0.close();
  EXPECT_EQ(jps.open_stream(1, &sbox1), 0);
  sbox1.close();

  // Sequential open_image / read / seek / get_pos / close_image across frames.
  EXPECT_NO_THROW(jps.close_image());
  EXPECT_EQ(jps.open_image(), 0);
  EXPECT_THROW(jps.open_image(), kdu_exception);
  EXPECT_NE(jps.get_capabilities() & KDU_SOURCE_CAP_SEEKABLE, 0);
  EXPECT_EQ(jps.get_pos(), 0);
  kdu_byte buf[8] = {};
  EXPECT_EQ(jps.read(buf, 4), 4);
  EXPECT_EQ(buf[0], 0xA0);
  EXPECT_EQ(jps.get_pos(), 4);
  EXPECT_TRUE(jps.seek(1));
  EXPECT_EQ(jps.read(buf, 1), 1);
  EXPECT_EQ(buf[0], 0x01);
  jps.close_image();

  // Seek forward to frame 2, read field 1 via field_mode=1, then backward to 0.
  EXPECT_TRUE(jps.seek_to_frame(2));
  EXPECT_EQ(jps.get_frame_instant(), 2);
  jps.set_field_mode(1);
  EXPECT_EQ(jps.open_image(), 2);
  EXPECT_EQ(jps.read(buf, 5), 5);
  EXPECT_EQ(buf[0], 0xB2);
  jps.close_image();

  // Exercise field_mode=0 (first field only) on frame 1.
  EXPECT_TRUE(jps.seek_to_frame(1));
  jps.set_field_mode(0);
  EXPECT_EQ(jps.open_image(), 1);
  EXPECT_EQ(jps.read(buf, 4), 4);
  EXPECT_EQ(buf[0], 0xA1);
  jps.close_image();

  // Switch to combined field_mode=2: opens field 0 then field 1 in frame 0.
  EXPECT_TRUE(jps.seek_to_frame(0));
  jps.set_field_mode(2);
  EXPECT_EQ(jps.open_image(), 0);
  jps.close_image();
  EXPECT_EQ(jps.open_image(), 0);
  jps.close_image();

  // Seek beyond EOF fails.
  EXPECT_FALSE(jps.seek_to_frame(10));
  EXPECT_TRUE(jps.close());
  fsrc.close();
}

TEST(JpbTest, NonSeekableAndSparseIndexSeeking) {
  // Build a stream with 1030 tiny frames (using raw synthetic boxes for speed)
  // and verify sparse frame index growth (>1024 entries) + non-seekable access.
  std::vector<kdu_byte> frame;
  {
    std::vector<kdu_byte> frat(4, 0);
    frat[0] = 0;
    frat[1] = 1;
    frat[2] = 0;
    frat[3] = 30;
    std::vector<kdu_byte> brat(12, 0);
    brat[3] = 100;
    brat[7] = 2;
    std::vector<kdu_byte> tcod(4, 0);
    std::vector<kdu_byte> bcol = {JPB_SRGB_SPACE};
    std::vector<kdu_byte> jp2c = {0xFF, 0x4F};
    std::vector<kdu_byte> elsm_payload;
    AppendBox(elsm_payload, jpb_frame_rate_4cc, frat);
    AppendBox(elsm_payload, jpb_max_bitrate_4cc, brat);
    AppendBox(elsm_payload, jpb_time_code_4cc, tcod);
    AppendBox(elsm_payload, jpb_colour_4cc, bcol);
    AppendBox(elsm_payload, jp2_codestream_4cc, jp2c);
    frame = MakeBox(jpb_elementary_stream_4cc, elsm_payload);
  }
  std::vector<kdu_byte> stream;
  stream.reserve(frame.size() * 1030 + 64);
  for (int i = 0; i < 1030; ++i) {
    stream.insert(stream.end(), frame.begin(), frame.end());
  }
  AppendBox(stream, mj2_skip_4cc, {0x01, 0x02});

  // Seekable forward past 1024 frames, then backward.
  {
    MemSource msrc(stream, /*seekable=*/true);
    jp2_family_src fsrc;
    fsrc.open(&msrc);
    jpb_source jps;
    ASSERT_EQ(jps.open(&fsrc), 1);
    float a = 0, b = 0, c = 0, d = 0, e = 0, f = 0, g = 0, h = 0;
    double mx = 0, mn = 0;
    EXPECT_FALSE(
        jps.get_mastering_display_info(a, b, c, d, e, f, g, h, mn, mx));
    EXPECT_TRUE(jps.seek_to_frame(1028));
    EXPECT_EQ(jps.get_frame_instant(), 1028);
    EXPECT_TRUE(jps.seek_to_frame(2));
    EXPECT_EQ(jps.open_image(), 2);
    jps.close_image();
    jps.close();
    fsrc.close();
  }

  // Non-seekable source: sequential open works, backward seek throws.
  {
    std::vector<kdu_byte> two_frames;
    two_frames.insert(two_frames.end(), frame.begin(), frame.end());
    two_frames.insert(two_frames.end(), frame.begin(), frame.end());
    MemSource msrc(two_frames, /*seekable=*/false);
    jp2_family_src fsrc;
    fsrc.open(&msrc);
    jpb_source jps;
    ASSERT_EQ(jps.open(&fsrc), 1);
    EXPECT_EQ(jps.get_capabilities() & KDU_SOURCE_CAP_SEEKABLE, 0);
    EXPECT_EQ(jps.open_image(), 0);
    jps.close_image();
    EXPECT_THROW(jps.seek_to_frame(0), kdu_exception);
    EXPECT_EQ(jps.open_image(), 1);
    jps.close_image();
    EXPECT_FALSE(jps.seek_to_frame(2));  // EOF.
    jps.close();
    fsrc.close();
  }
}

TEST(JpbTest, TextualizerVariantsAndMalformedInputs) {
  jp2_box_textualizer tex;
  tex.add_box_type(jpb_mastering_display_4cc, "mastering-display");
  jpb_add_box_descriptions(tex);

  // Test textualizer on various valid/edge sub-box payloads.
  auto check_tex = [&](kdu_uint32 fourcc, const std::vector<kdu_byte>& payload,
                       bool xml) -> bool {
    std::vector<kdu_byte> raw;
    AppendBox(raw, fourcc, payload);
    MemSource msrc(raw);
    jp2_family_src fsrc;
    fsrc.open(&msrc);
    jp2_input_box box;
    if (!box.open(&fsrc)) {
      fsrc.close();
      return false;
    }
    CaptureMessage msg;
    const bool ok = tex.textualize_box(&box, msg, xml, 1024);
    box.close();
    fsrc.close();
    return ok;
  };

  EXPECT_TRUE(check_tex(jpb_frame_rate_4cc, {0, 0, 0, 0}, false));
  EXPECT_TRUE(check_tex(jpb_frame_rate_4cc, {0, 1, 0, 30}, true));
  EXPECT_FALSE(check_tex(jpb_frame_rate_4cc, {0, 1}, false));

  EXPECT_TRUE(check_tex(jpb_max_bitrate_4cc,
                        {0, 0, 1, 0, 0, 0, 0, 2, 0, 0, 0, 3}, false));
  EXPECT_FALSE(check_tex(jpb_max_bitrate_4cc, {0, 1}, false));

  EXPECT_TRUE(check_tex(jpb_field_coding_4cc, {1, 0}, false));
  EXPECT_TRUE(check_tex(jpb_field_coding_4cc, {2, 1}, false));
  EXPECT_TRUE(check_tex(jpb_field_coding_4cc, {2, 6}, true));
  EXPECT_TRUE(check_tex(jpb_field_coding_4cc, {2, 9}, false));
  EXPECT_FALSE(check_tex(jpb_field_coding_4cc, {1}, false));

  EXPECT_TRUE(check_tex(jpb_time_code_4cc, {0x01, 0x02, 0x03, 0x84}, false));
  EXPECT_TRUE(check_tex(jpb_time_code_4cc, {0x01, 0x02, 0x03, 0x04}, true));
  EXPECT_FALSE(check_tex(jpb_time_code_4cc, {0x01}, false));

  for (kdu_byte cs = 0; cs <= 9; ++cs) {
    EXPECT_TRUE(check_tex(jpb_colour_4cc, {cs}, false));
  }
  EXPECT_FALSE(check_tex(jpb_colour_4cc, {}, false));

  std::vector<kdu_byte> dmoc(24, 0);
  EXPECT_TRUE(check_tex(jpb_mastering_display_4cc, dmoc, false));
  EXPECT_FALSE(check_tex(jpb_mastering_display_4cc, {0, 1}, false));

  // Non-ELSM first box errors unless return_if_incompatible=true.
  std::vector<kdu_byte> non_elsm;
  AppendBox(non_elsm, jp2_free_4cc, {1, 2, 3, 4});
  {
    MemSource msrc(non_elsm);
    jp2_family_src fsrc;
    fsrc.open(&msrc);
    jpb_source jps1;
    EXPECT_THROW(jps1.open(&fsrc, false), kdu_exception);
    jps1.close();
    jpb_source jps2;
    EXPECT_EQ(jps2.open(&fsrc, true), -1);
    jps2.close();
    fsrc.close();
  }

  // Test malformed ELSM sub-box combinations.
  auto make_elsm = [](kdu_byte num_fields, kdu_byte fiel_order,
                      bool include_frat, bool include_brat, bool include_tcod,
                      bool include_bcol, bool include_skip_before_c2,
                      int num_jp2c) {
    std::vector<kdu_byte> payload;
    if (include_frat) {
      AppendBox(payload, jpb_frame_rate_4cc, {0, 1, 0, 30});
    }
    if (include_brat) {
      AppendBox(payload, jpb_max_bitrate_4cc,
                {0, 0, 1, 0, 0, 0, 0, 2, 0, 0, 0, 2});
    }
    if (num_fields > 0) {
      AppendBox(payload, jpb_field_coding_4cc, {num_fields, fiel_order});
    }
    if (include_tcod) {
      AppendBox(payload, jpb_time_code_4cc, {0, 0, 0, 0});
    }
    if (include_bcol) {
      AppendBox(payload, jpb_colour_4cc, {JPB_709_SPACE});
    }
    if (num_jp2c >= 1) {
      AppendBox(payload, jp2_codestream_4cc, {0xFF, 0x4F});
    }
    if (include_skip_before_c2) {
      AppendBox(payload, mj2_skip_4cc, {0x00});
    }
    if (num_jp2c >= 2) {
      AppendBox(payload, jp2_codestream_4cc, {0xFF, 0x4F});
    }
    return MakeBox(jpb_elementary_stream_4cc, payload);
  };

  // Valid interlaced TOP_SECOND with skip box before second codestream.
  {
    auto valid_t2 = make_elsm(2, 6, true, true, true, true, true, 2);
    MemSource msrc(valid_t2);
    jp2_family_src fsrc;
    fsrc.open(&msrc);
    jpb_source jps;
    ASSERT_EQ(jps.open(&fsrc), 1);
    EXPECT_EQ(jps.get_field_order(), kdu_supp::KDU_FIELDS_TOP_SECOND);
    EXPECT_EQ(jps.open_image(), 0);
    jps.close_image();
    EXPECT_EQ(jps.open_image(), 0);
    jps.close_image();
    jps.close();
    fsrc.close();
  }

  // Missing required boxes or invalid field coding generate errors.
  const std::vector<std::vector<kdu_byte>> bad_elsms = {
      make_elsm(0, 0, false, true, true, true, false, 1),  // Missing frat
      make_elsm(0, 0, true, false, true, true, false, 1),  // Missing brat
      make_elsm(0, 0, true, true, false, true, false, 1),  // Missing tcod
      make_elsm(0, 0, true, true, true, false, false, 1),  // Missing bcol
      make_elsm(0, 0, true, true, true, true, false, 0),   // Missing jp2c
      make_elsm(2, 6, true, true, true, true, false,
                1),  // Only 1 jp2c for 2 fields
      make_elsm(3, 1, true, true, true, true, false, 1),  // Invalid num_fields
      make_elsm(2, 9, true, true, true, true, false, 2),  // Invalid fiel order
  };
  for (const auto& bad : bad_elsms) {
    MemSource msrc(bad);
    jp2_family_src fsrc;
    fsrc.open(&msrc);
    jpb_source jps;
    EXPECT_THROW(jps.open(&fsrc, false), kdu_exception);
    jps.close();
    fsrc.close();
  }
}

}  // namespace
