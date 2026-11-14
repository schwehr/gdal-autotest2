// Tests exercising JPEG 2000 PPM (0xFF60) and PPT (0xFF61) packed packet
// header workflows analogous to third_party/kakadu/vendor/apps/kdu_makeppm.
//
// Structural coverage note (LCOV DA: ceiling = 0/0):
// third_party/kakadu/vendor/BUILD neither compiles kdu_makeppm.cpp nor exports
// kdu_makeppm.h in :kdu (kdu_makeppm is an unbuilt standalone demo binary).
// These hermetic in-memory tests validate the :kdu core PPM/PPT parsing,
// multi-segment ordering, JP2 wrapping, round-trip pixel reconstruction, and
// malformed-marker error paths without including kdu_makeppm.h directly.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/types/span.h"
#include "third_party/kakadu/vendor/apps/compressed_io/jp2.h"
#include "third_party/kakadu/vendor/apps/support/kdu_stripe_compressor.h"
#include "third_party/kakadu/vendor/apps/support/kdu_stripe_decompressor.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_compressed.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_messaging.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_params.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_sample_processing.h"
#include "third_party/kakadu/vendor/coresys/common/kdu_ubiquitous.h"

namespace kdu_supp {
namespace {

using kdu_core::kdu_byte;
using kdu_core::kdu_codestream;
using kdu_core::kdu_compressed_source;
using kdu_core::kdu_compressed_target;
using kdu_core::kdu_dims;
using kdu_core::kdu_exception;
using kdu_core::kdu_long;
using kdu_core::kdu_thread_safe_message;
using kdu_core::kdu_uint16;
using kdu_core::kdu_uint32;
using kdu_core::siz_params;

class GlobalErrorHandler final : public kdu_thread_safe_message {
 public:
  ~GlobalErrorHandler() noexcept override = default;
  void put_text(const char*) override {}
  void flush(bool end_of_message = false) override {
    kdu_thread_safe_message::flush(end_of_message);
    if (end_of_message) {
      throw KDU_ERROR_EXCEPTION;
    }
  }
};

class NoopWarningHandler final : public kdu_core::kdu_message {
 public:
  ~NoopWarningHandler() noexcept override = default;
  void put_text(const char*) override {}
};

class KakaduEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    static auto* err = new GlobalErrorHandler();
    static auto* warn = new NoopWarningHandler();
    kdu_core::kdu_customize_errors(err);
    kdu_core::kdu_customize_warnings(warn);
  }
};

const ::testing::Environment* const kKakaduEnv =
    ::testing::AddGlobalTestEnvironment(new KakaduEnvironment());

class MemoryTarget final : public kdu_compressed_target {
 public:
  bool write(const kdu_byte* buf, int num_bytes) override {
    if (num_bytes > 0) {
      data_.insert(data_.end(), buf, buf + num_bytes);
    }
    return true;
  }
  const std::vector<kdu_byte>& data() const { return data_; }

 private:
  std::vector<kdu_byte> data_;
};

class MemorySource final : public kdu_compressed_source {
 public:
  explicit MemorySource(absl::Span<const kdu_byte> data) : data_(data) {}

  int get_capabilities() override {
    return KDU_SOURCE_CAP_SEQUENTIAL | KDU_SOURCE_CAP_SEEKABLE;
  }

  bool seek(kdu_long offset) override {
    if (offset < 0 || static_cast<size_t>(offset) > data_.size()) {
      return false;
    }
    pos_ = static_cast<size_t>(offset);
    return true;
  }

  kdu_long get_pos() override { return static_cast<kdu_long>(pos_); }

  int read(kdu_byte* buf, int num_bytes) override {
    if (num_bytes <= 0 || pos_ >= data_.size()) {
      return 0;
    }
    const size_t n =
        std::min(static_cast<size_t>(num_bytes), data_.size() - pos_);
    std::memcpy(buf, data_.data() + pos_, n);
    pos_ += n;
    return static_cast<int>(n);
  }

 private:
  absl::Span<const kdu_byte> data_;
  size_t pos_ = 0;
};

kdu_uint16 ReadU16BE(const kdu_byte* p) {
  return (static_cast<kdu_uint16>(p[0]) << 8) | p[1];
}

kdu_uint32 ReadU32BE(const kdu_byte* p) {
  return (static_cast<kdu_uint32>(p[0]) << 24) |
         (static_cast<kdu_uint32>(p[1]) << 16) |
         (static_cast<kdu_uint32>(p[2]) << 8) | p[3];
}

void AppendU16BE(std::vector<kdu_byte>& out, kdu_uint16 val) {
  out.push_back(static_cast<kdu_byte>(val >> 8));
  out.push_back(static_cast<kdu_byte>(val & 0xFF));
}

void AppendU32BE(std::vector<kdu_byte>& out, kdu_uint32 val) {
  out.push_back(static_cast<kdu_byte>((val >> 24) & 0xFF));
  out.push_back(static_cast<kdu_byte>((val >> 16) & 0xFF));
  out.push_back(static_cast<kdu_byte>((val >> 8) & 0xFF));
  out.push_back(static_cast<kdu_byte>(val & 0xFF));
}

void WriteU32BEAt(std::vector<kdu_byte>& out, size_t pos, kdu_uint32 val) {
  out[pos + 0] = static_cast<kdu_byte>((val >> 24) & 0xFF);
  out[pos + 1] = static_cast<kdu_byte>((val >> 16) & 0xFF);
  out[pos + 2] = static_cast<kdu_byte>((val >> 8) & 0xFF);
  out[pos + 3] = static_cast<kdu_byte>(val & 0xFF);
}

std::vector<kdu_byte> MakeSyntheticImage(int width, int height, int num_comps) {
  std::vector<kdu_byte> pixels(static_cast<size_t>(width) * height * num_comps);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      for (int c = 0; c < num_comps; ++c) {
        const size_t idx = (static_cast<size_t>(y) * width + x) * num_comps + c;
        pixels[idx] =
            static_cast<kdu_byte>((x * 13 + y * 29 + c * 53 + 7) & 0xFF);
      }
    }
  }
  return pixels;
}

// Encodes a reversible multi-tile / multi-layer codestream with SOP and EPH
// markers so packet headers can be extracted cleanly without touching core
// internals.
std::vector<kdu_byte> EncodeMultiTileSopEph(std::vector<kdu_byte> pixels,
                                            int width, int height,
                                            int num_comps, int tile_w,
                                            int tile_h) {
  MemoryTarget tgt;
  siz_params siz;
  siz.set(Scomponents, 0, 0, num_comps);
  siz.set(Sdims, 0, 0, height);
  siz.set(Sdims, 0, 1, width);
  siz.set(Ssigned, 0, 0, false);
  siz.set(Sprecision, 0, 0, 8);
  std::string tiles_param =
      "Stiles={" + std::to_string(tile_h) + "," + std::to_string(tile_w) + "}";
  siz.parse_string(tiles_param.c_str());
  static_cast<kdu_core::kdu_params&>(siz).finalize();

  kdu_codestream cs;
  absl::Cleanup destroy_cs = [&cs] {
    if (cs.exists()) cs.destroy();
  };
  cs.create(&siz, &tgt);
  kdu_core::kdu_params* cod = cs.access_siz()->access_cluster(COD_params);
  EXPECT_NE(cod, nullptr);
  cod->parse_string("Creversible=yes");
  cod->parse_string("Clevels=2");
  cod->parse_string("Clayers=2");
  cod->parse_string("Cuse_sop=yes");
  cod->parse_string("Cuse_eph=yes");
  cs.access_siz()->finalize_all();

  kdu_stripe_compressor comp;
  comp.start(cs);
  std::array<int, 3> precisions{8, 8, 8};
  std::vector<int> heights(num_comps, height);
  comp.push_stripe(pixels.data(), heights.data(),
                   /*sample_offsets=*/nullptr, /*sample_gaps=*/nullptr,
                   /*row_gaps=*/nullptr, /*precisions=*/precisions.data());
  comp.finish();
  return tgt.data();
}

struct MarkerSeg {
  kdu_uint16 code;
  std::vector<kdu_byte> body;
};

struct TilePart {
  kdu_uint16 tile_idx;
  kdu_byte tp_idx;
  kdu_byte num_tps;
  std::vector<MarkerSeg> extra_markers;
  std::vector<kdu_byte> stripped_body;
  std::vector<kdu_byte> packed_hdrs;
};

struct RepackOptions {
  bool use_ppt = false;
  bool keep_sop = false;
  bool use_eph = false;
  size_t max_pp_chunk = 0;  // 0 = single PPM/PPT marker segment per scope.
};

void PatchScodFlags(std::vector<MarkerSeg>& segs, bool keep_sop, bool use_eph) {
  for (MarkerSeg& seg : segs) {
    if (seg.code == 0xFF52 && !seg.body.empty()) {
      seg.body[0] &= static_cast<kdu_byte>(~0x06U);
      if (keep_sop) seg.body[0] |= 0x02U;
      if (use_eph) seg.body[0] |= 0x04U;
    }
  }
}

void AppendMarkerSeg(std::vector<kdu_byte>& out, kdu_uint16 code,
                     const std::vector<kdu_byte>& body) {
  AppendU16BE(out, code);
  if (!body.empty() || (code != 0xFF4F && code != 0xFF93 && code != 0xFFD9 &&
                        code != 0xFF92 && (code < 0xFF30 || code > 0xFF3F))) {
    AppendU16BE(out, static_cast<kdu_uint16>(body.size() + 2));
    out.insert(out.end(), body.begin(), body.end());
  }
}

// Extracts SOP/EPH-delimited packet headers from each tile-part and re-emits a
// valid JPEG 2000 codestream carrying either main-header PPM (0xFF60) or
// tile-part PPT (0xFF61) marker segments.
std::vector<kdu_byte> RepackCodestreamWithPp(absl::Span<const kdu_byte> src,
                                             const RepackOptions& opts) {
  EXPECT_GE(src.size(), 4U);
  EXPECT_EQ(ReadU16BE(src.data()), 0xFF4F);

  size_t pos = 2;
  std::vector<MarkerSeg> main_hdr;
  while (pos + 2 <= src.size()) {
    const kdu_uint16 code = ReadU16BE(src.data() + pos);
    if (code == 0xFF90) break;  // SOT starts tile-part stream.
    pos += 2;
    const kdu_uint16 lmar = ReadU16BE(src.data() + pos);
    EXPECT_GE(lmar, 2);
    const size_t body_len = static_cast<size_t>(lmar) - 2;
    main_hdr.push_back(
        {code, std::vector<kdu_byte>(src.begin() + pos + 2,
                                     src.begin() + pos + 2 + body_len)});
    pos += lmar;
  }
  PatchScodFlags(main_hdr, opts.keep_sop, opts.use_eph);

  std::vector<TilePart> tparts;
  while (pos + 2 <= src.size()) {
    const kdu_uint16 code = ReadU16BE(src.data() + pos);
    if (code == 0xFFD9) {
      pos += 2;
      break;
    }
    EXPECT_EQ(code, 0xFF90);
    const size_t sot_pos = pos;
    const kdu_uint16 sot_len = ReadU16BE(src.data() + pos + 2);
    EXPECT_EQ(sot_len, 10);
    const kdu_uint16 tile_idx = ReadU16BE(src.data() + pos + 4);
    const kdu_uint32 psot = ReadU32BE(src.data() + pos + 6);
    const kdu_byte tp_idx = src[pos + 10];
    const kdu_byte num_tps = src[pos + 11];
    const size_t tp_end = (psot == 0) ? (src.size() - 2) : (sot_pos + psot);
    pos += 2 + sot_len;

    TilePart tp{.tile_idx = tile_idx, .tp_idx = tp_idx, .num_tps = num_tps};
    while (pos + 2 <= tp_end) {
      const kdu_uint16 mcode = ReadU16BE(src.data() + pos);
      pos += 2;
      if (mcode == 0xFF93) break;  // SOD
      const kdu_uint16 lmar = ReadU16BE(src.data() + pos);
      const size_t blen = static_cast<size_t>(lmar) - 2;
      tp.extra_markers.push_back(
          {mcode, std::vector<kdu_byte>(src.begin() + pos + 2,
                                        src.begin() + pos + 2 + blen)});
      pos += lmar;
    }
    PatchScodFlags(tp.extra_markers, opts.keep_sop, opts.use_eph);

    // Every packet inside `[pos, tp_end)` begins with SOP (0xFF91 00 04 Nsop)
    // and terminates its packet header at EPH (0xFF92).
    while (pos < tp_end) {
      if (pos + 6 <= tp_end && src[pos] == 0xFF && src[pos + 1] == 0x91) {
        if (opts.keep_sop) {
          tp.stripped_body.insert(tp.stripped_body.end(), src.begin() + pos,
                                  src.begin() + pos + 6);
        }
        pos += 6;
        // Extract packet header up to EPH (0xFF92).
        const size_t hdr_start = pos;
        size_t eph_pos = pos;
        while (eph_pos + 1 < tp_end &&
               !(src[eph_pos] == 0xFF && src[eph_pos + 1] == 0x92)) {
          ++eph_pos;
        }
        EXPECT_LT(eph_pos + 1, tp_end);
        tp.packed_hdrs.insert(tp.packed_hdrs.end(), src.begin() + hdr_start,
                              src.begin() + eph_pos);
        if (opts.use_eph) {
          AppendU16BE(tp.packed_hdrs, 0xFF92);
        }
        pos = eph_pos + 2;
      } else {
        tp.stripped_body.push_back(src[pos++]);
      }
    }
    tparts.push_back(std::move(tp));
  }

  std::vector<kdu_byte> out;
  AppendU16BE(out, 0xFF4F);  // SOC
  for (const MarkerSeg& seg : main_hdr) {
    AppendMarkerSeg(out, seg.code, seg.body);
  }

  if (!opts.use_ppt) {
    // Build single byte stream of `[Nppm(4), pkt_hdrs...]` entries. Notice
    // each 4-byte Nppm + header payload stays inside its own chunk when split.
    std::vector<std::vector<kdu_byte>> ppm_bodies(1);
    for (const TilePart& tp : tparts) {
      const size_t needed = 4 + tp.packed_hdrs.size();
      if (opts.max_pp_chunk > 0 && !ppm_bodies.back().empty() &&
          ppm_bodies.back().size() + needed > opts.max_pp_chunk) {
        ppm_bodies.emplace_back();
      }
      AppendU32BE(ppm_bodies.back(),
                  static_cast<kdu_uint32>(tp.packed_hdrs.size()));
      ppm_bodies.back().insert(ppm_bodies.back().end(), tp.packed_hdrs.begin(),
                               tp.packed_hdrs.end());
    }
    for (size_t z = 0; z < ppm_bodies.size(); ++z) {
      std::vector<kdu_byte> seg_body;
      seg_body.push_back(static_cast<kdu_byte>(z));
      seg_body.insert(seg_body.end(), ppm_bodies[z].begin(),
                      ppm_bodies[z].end());
      AppendMarkerSeg(out, 0xFF60, seg_body);
    }
  }

  for (const TilePart& tp : tparts) {
    std::vector<MarkerSeg> tp_segs = tp.extra_markers;
    if (opts.use_ppt) {
      const size_t chunk =
          (opts.max_pp_chunk > 0) ? opts.max_pp_chunk : tp.packed_hdrs.size();
      size_t off = 0;
      kdu_byte z = 0;
      while (off < tp.packed_hdrs.size()) {
        const size_t len = std::min(chunk, tp.packed_hdrs.size() - off);
        std::vector<kdu_byte> seg_body;
        seg_body.push_back(z++);
        seg_body.insert(seg_body.end(), tp.packed_hdrs.begin() + off,
                        tp.packed_hdrs.begin() + off + len);
        tp_segs.push_back({0xFF61, std::move(seg_body)});
        off += len;
      }
    }
    size_t tp_bytes = 12 + 2 + tp.stripped_body.size();  // SOT + SOD + body
    for (const MarkerSeg& seg : tp_segs) {
      tp_bytes += 4 + seg.body.size();
    }
    AppendU16BE(out, 0xFF90);
    AppendU16BE(out, 10);
    AppendU16BE(out, tp.tile_idx);
    AppendU32BE(out, static_cast<kdu_uint32>(tp_bytes));
    out.push_back(tp.tp_idx);
    out.push_back(tp.num_tps);
    for (const MarkerSeg& seg : tp_segs) {
      AppendMarkerSeg(out, seg.code, seg.body);
    }
    AppendU16BE(out, 0xFF93);  // SOD
    out.insert(out.end(), tp.stripped_body.begin(), tp.stripped_body.end());
  }
  AppendU16BE(out, 0xFFD9);  // EOC
  return out;
}

std::vector<kdu_byte> WrapCodestreamInJp2(absl::Span<const kdu_byte> j2k,
                                          int width, int height,
                                          int num_comps) {
  MemoryTarget mem_tgt;
  jp2_family_tgt fam_tgt;
  fam_tgt.open(&mem_tgt);
  jp2_target jp2_out;
  jp2_out.open(&fam_tgt);

  siz_params siz;
  siz.set(Scomponents, 0, 0, num_comps);
  siz.set(Sdims, 0, 0, height);
  siz.set(Sdims, 0, 1, width);
  siz.set(Ssigned, 0, 0, false);
  siz.set(Sprecision, 0, 0, 8);
  static_cast<kdu_core::kdu_params&>(siz).finalize();

  jp2_out.access_dimensions().init(&siz);
  jp2_out.access_colour().init((num_comps == 1) ? JP2_sLUM_SPACE
                                                : JP2_sRGB_SPACE);
  jp2_out.write_header();
  jp2_out.open_codestream(true);
  EXPECT_TRUE(jp2_out.write(j2k.data(), static_cast<int>(j2k.size())));
  jp2_out.close();
  fam_tgt.close();
  return mem_tgt.data();
}

std::vector<kdu_byte> DecodeCodestreamPixels(kdu_codestream& cs, int width,
                                             int height, int num_comps) {
  std::vector<kdu_byte> out(static_cast<size_t>(width) * height * num_comps);
  kdu_stripe_decompressor decomp;
  decomp.start(cs);
  std::array<int, 3> precisions{8, 8, 8};
  std::vector<int> heights(num_comps, height);
  decomp.pull_stripe(out.data(), heights.data(), /*sample_offsets=*/nullptr,
                     /*sample_gaps=*/nullptr, /*row_gaps=*/nullptr,
                     precisions.data());
  decomp.finish();
  return out;
}

std::vector<kdu_byte> DecodeRawJ2k(absl::Span<const kdu_byte> j2k, int width,
                                   int height, int num_comps) {
  MemorySource src(j2k);
  kdu_codestream cs;
  absl::Cleanup destroy_cs = [&cs] {
    if (cs.exists()) cs.destroy();
  };
  cs.create(&src);
  return DecodeCodestreamPixels(cs, width, height, num_comps);
}

std::vector<kdu_byte> DecodeJp2(absl::Span<const kdu_byte> jp2_bytes, int width,
                                int height, int num_comps) {
  MemorySource mem_src(jp2_bytes);
  jp2_family_src fam_src;
  fam_src.open(&mem_src);
  jp2_source jp2_in;
  EXPECT_TRUE(jp2_in.open(&fam_src));
  EXPECT_TRUE(jp2_in.read_header());
  kdu_codestream cs;
  absl::Cleanup destroy_cs = [&cs] {
    if (cs.exists()) cs.destroy();
  };
  cs.create(&jp2_in);
  return DecodeCodestreamPixels(cs, width, height, num_comps);
}

size_t FindMarker(absl::Span<const kdu_byte> bytes, size_t start_pos,
                  kdu_uint16 marker) {
  for (size_t i = start_pos; i + 1 < bytes.size(); ++i) {
    if (ReadU16BE(bytes.data() + i) == marker) return i;
  }
  return bytes.size();
}

TEST(KduMakeppmTest, RepacksMultiTileRawAndJp2WithPpmAndPptRoundTrip) {
  // 1. Encode a 16x16 3-component image split into four 8x8 tiles with SOP+EPH.
  constexpr int kWidth = 16;
  constexpr int kHeight = 16;
  constexpr int kComps = 3;
  const std::vector<kdu_byte> orig =
      MakeSyntheticImage(kWidth, kHeight, kComps);
  const std::vector<kdu_byte> baseline =
      EncodeMultiTileSopEph(orig, kWidth, kHeight, kComps, 8, 8);
  EXPECT_EQ(orig, DecodeRawJ2k(baseline, kWidth, kHeight, kComps));

  // 2. Verify all PPM and PPT flag combinations (with/without SOP & EPH, plus
  // multi-segment Zppm/Zppt chunking) decode bit-exact via raw J2K and JP2.
  constexpr auto kCases = std::to_array<RepackOptions>({
      {.use_ppt = false,
       .keep_sop = false,
       .use_eph = false,
       .max_pp_chunk = 0},
      {.use_ppt = false, .keep_sop = true, .use_eph = true, .max_pp_chunk = 32},
      {.use_ppt = true, .keep_sop = false, .use_eph = false, .max_pp_chunk = 0},
      {.use_ppt = true, .keep_sop = true, .use_eph = true, .max_pp_chunk = 8},
      {.use_ppt = true, .keep_sop = false, .use_eph = true, .max_pp_chunk = 0},
  });
  for (size_t i = 0; i < kCases.size(); ++i) {
    SCOPED_TRACE(::testing::Message() << "case " << i);
    const std::vector<kdu_byte> repacked =
        RepackCodestreamWithPp(baseline, kCases[i]);
    const kdu_uint16 expected_marker = kCases[i].use_ppt ? 0xFF61 : 0xFF60;
    EXPECT_LT(FindMarker(repacked, 0, expected_marker), repacked.size());
    EXPECT_EQ(orig, DecodeRawJ2k(repacked, kWidth, kHeight, kComps));

    const std::vector<kdu_byte> jp2 =
        WrapCodestreamInJp2(repacked, kWidth, kHeight, kComps);
    EXPECT_EQ(orig, DecodeJp2(jp2, kWidth, kHeight, kComps));
  }
}

TEST(KduMakeppmTest, RejectsMalformedPpmAndPptMarkersCleanly) {
  const std::vector<kdu_byte> orig = MakeSyntheticImage(16, 16, 1);
  const std::vector<kdu_byte> baseline =
      EncodeMultiTileSopEph(orig, 16, 16, 1, 8, 8);
  const std::vector<kdu_byte> valid_ppm = RepackCodestreamWithPp(
      baseline, {.use_ppt = false, .keep_sop = false, .use_eph = false});

  // 1. Malformed main-header PPM (0xFF60) segments.
  const size_t ppm_pos = FindMarker(valid_ppm, 2, 0xFF60);
  ASSERT_LT(ppm_pos + 4, valid_ppm.size());
  const kdu_uint16 ppm_len = ReadU16BE(valid_ppm.data() + ppm_pos + 2);
  ASSERT_GT(ppm_len, 6U);

  // Truncated PPM length (< 3 bytes) triggers kd_pp_markers error.
  std::vector<kdu_byte> short_ppm = valid_ppm;
  short_ppm.erase(short_ppm.begin() + ppm_pos + 4,
                  short_ppm.begin() + ppm_pos + 2 + ppm_len);
  short_ppm[ppm_pos + 2] = 0;
  short_ppm[ppm_pos + 3] = 2;
  EXPECT_THROW(DecodeRawJ2k(short_ppm, 16, 16, 1), kdu_exception);

  // Duplicate Zppm=0 marker in main header triggers duplicate-index error.
  std::vector<kdu_byte> dup_zppm = valid_ppm;
  constexpr auto kExtraPpmSeg =
      std::to_array<kdu_byte>({0xFF, 0x60, 0x00, 0x04, 0x00, 0x00});
  dup_zppm.insert(dup_zppm.begin() + ppm_pos, kExtraPpmSeg.begin(),
                  kExtraPpmSeg.end());
  EXPECT_THROW(DecodeRawJ2k(dup_zppm, 16, 16, 1), kdu_exception);

  // Inflated Nppm byte count inside PPM triggers insufficient-data error.
  std::vector<kdu_byte> bad_nppm = valid_ppm;
  bad_nppm[ppm_pos + 5] = 0x00;
  bad_nppm[ppm_pos + 6] = 0x10;
  bad_nppm[ppm_pos + 7] = 0x00;
  bad_nppm[ppm_pos + 8] = 0x00;
  EXPECT_THROW(DecodeRawJ2k(bad_nppm, 16, 16, 1), kdu_exception);

  // 2. Malformed tile-part PPT (0xFF61) segments (preserving valid Psot).
  const std::vector<kdu_byte> valid_ppt = RepackCodestreamWithPp(
      baseline, {.use_ppt = true, .keep_sop = false, .use_eph = false});
  const size_t sot_pos = FindMarker(valid_ppt, 2, 0xFF90);
  ASSERT_LT(sot_pos + 12, valid_ppt.size());
  const size_t ppt_pos = FindMarker(valid_ppt, sot_pos + 12, 0xFF61);
  ASSERT_LT(ppt_pos + 4, valid_ppt.size());
  const kdu_uint16 ppt_len = ReadU16BE(valid_ppt.data() + ppt_pos + 2);
  ASSERT_GT(ppt_len, 2U);
  const kdu_uint32 psot = ReadU32BE(valid_ppt.data() + sot_pos + 6);
  ASSERT_GT(psot, ppt_len);

  // Truncated PPT length (< 3 bytes) triggers kd_pp_markers error.
  std::vector<kdu_byte> short_ppt = valid_ppt;
  WriteU32BEAt(short_ppt, sot_pos + 6, psot - (ppt_len - 2));
  short_ppt.erase(short_ppt.begin() + ppt_pos + 4,
                  short_ppt.begin() + ppt_pos + 2 + ppt_len);
  short_ppt[ppt_pos + 2] = 0;
  short_ppt[ppt_pos + 3] = 2;
  EXPECT_THROW(DecodeRawJ2k(short_ppt, 16, 16, 1), kdu_exception);

  // Duplicate Zppt=0 marker in tile-part header triggers duplicate-index error.
  std::vector<kdu_byte> dup_zppt = valid_ppt;
  constexpr auto kExtraPptSeg =
      std::to_array<kdu_byte>({0xFF, 0x61, 0x00, 0x04, 0x00, 0x00});
  WriteU32BEAt(dup_zppt, sot_pos + 6,
               psot + static_cast<kdu_uint32>(kExtraPptSeg.size()));
  dup_zppt.insert(dup_zppt.begin() + ppt_pos, kExtraPptSeg.begin(),
                  kExtraPptSeg.end());
  EXPECT_THROW(DecodeRawJ2k(dup_zppt, 16, 16, 1), kdu_exception);
}

}  // namespace
}  // namespace kdu_supp
