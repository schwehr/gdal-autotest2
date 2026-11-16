// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests internal and public JPEG 2000 codestream (J2K) encoding, decoding,
// marker parsing/serialization, profile compliance, tile/region/component
// decoding, codestream dump/index extraction, and malformed-stream handling
// (src/lib/openjp2/j2k.c and src/lib/openjp2/j2k.h).
//
// Caveats:
// - USE_JPWL, USE_JPSEC, USE_JPIP, WIP_REMOVE_MSD, TODO_MSD, DEBUG_VERBOSE,
//   and OPJ_DISABLE_TPSOT_FIX are not defined in google3 builds.
// - Upstream opj_j2k_setup_encoder resets parameters->mct_data to NULL on
//   success after copying; caller cleanup must still free it on early errors.
// - Upstream opj_j2k_read_cod rejects COD SGcod(C) > 1 even though opj_set_MCT
//   writes mct = 2, so decoding custom Part-2 MCT codestreams requires patching
//   that header byte to 0.
// - Upstream j2k_get_cstr_index leaks tile_index[i].marker if called on a
//   codestream where tile markers were read while nb_tps == 0 (e.g. TNsot ==
//   0).
// - Upstream j2k_get_cstr_info copies numresolutions bytes (not sizeof(UINT32)
//   multiples) into tccp_info->prch/prcw; tests do not validate those arrays.
// - opj_j2k_start_compress transfers ownership of image->comps[i].data into
//   m_private_image and sets comps[i].data to NULL; callers must snapshot
//   expected samples before compression.
// - google3 targets are little-endian (OPJ_BIG_ENDIAN undefined) and compile
//   with -DMUTEX_pthread=1 and -funsigned-char.
// - Tests alias opaque opj_codec_t* / opj_stream_t* handles to upstream
//   opj_codec_private_t* / opj_stream_private_t* (openjpeg.c) and poke
//   internal opj_j2k_t / opj_cp_t / opj_tccp_t fields; upstream struct layout
//   changes require re-auditing ExtractJ2kHandle and internal field writes.
// - EventCapture assertions pin upstream j2k.c / pi.c diagnostic substrings.
// - ScopedEnvVar calls setenv()/unsetenv(), which is only TSan-safe when no
//   OpenJPEG worker thread pool is alive across the call.
//
// See also:
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/j2k.c
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/j2k.h
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/openjpeg.h

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/openjpeg2/google/util/alloc_fault_injector.h"

// Include opj_internal_test_util.h (which includes opj_internal.h after
// test_util.h) after C++ stdlib, GoogleTest, and Abseil headers;
// opj_internal.h defines OPJ_SKIP_POISON, and OpenJPEG's OPJ_FSEEK macro does
// not redefine stdio symbols.
#include "third_party/openjpeg2/google/util/opj_internal_test_util.h"

namespace openjpeg2 {
namespace {

using ::testing::Contains;
using ::testing::ElementsAreArray;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;

struct OpjJ2kDeleter {
  void operator()(opj_j2k_t* j2k) const { opj_j2k_destroy(j2k); }
};
using OpjJ2kPtr = std::unique_ptr<opj_j2k_t, OpjJ2kDeleter>;

// Max observed abs error for 24x24 rgb_97_lossy at rate 10 is 245 on both
// x86 SSE2 and --cpu=arm scalar (10:1 total-size budget leaves ~40 payload
// bytes after headers on high-frequency synthetic tiles); headroom 2 -> 247.
constexpr OPJ_INT32 kLossy97Rate10MaxAbsError = 247;
// Verified upper bound for comp 260 with roi_shift=2 across x86 SSE2 and
// --cpu=arm scalar paths (quantization + ROI bitshift).
constexpr OPJ_INT32 kRoiComp260MaxAbsError = 8;

// IMF RSIZ encodes (sublevel << 4) | mainlevel in the low byte (openjpeg.h).
constexpr uint16_t kImfMainLevel1SubLevel1 = 0x0011U;
constexpr uint16_t ImfRsiz(uint16_t profile, uint8_t mainlevel,
                           uint8_t sublevel) {
  return static_cast<uint16_t>(
      profile | (static_cast<uint16_t>(sublevel) << 4) | mainlevel);
}
static_assert(OPJ_GET_IMF_MAINLEVEL(ImfRsiz(OPJ_PROFILE_IMF_2K, 5, 3)) == 5);
static_assert(OPJ_GET_IMF_SUBLEVEL(ImfRsiz(OPJ_PROFILE_IMF_2K, 5, 3)) == 3);

constexpr const char* kTlmOpts[] = {"TLM=YES", nullptr};
constexpr const char* kPltOpts[] = {"PLT=YES", nullptr};
constexpr const char* kTlmPltOpts[] = {"TLM=YES", "PLT=YES", nullptr};

class ScopedEnvVar {
 public:
  explicit ScopedEnvVar(const char* name) : name_(name) {
    const char* prev = std::getenv(name_);
    if (prev != nullptr) {
      had_prev_ = true;
      prev_value_ = prev;
    }
  }

  ~ScopedEnvVar() {
    if (had_prev_) {
      setenv(name_, prev_value_.c_str(), 1);
    } else {
      unsetenv(name_);
    }
  }

  ScopedEnvVar(const ScopedEnvVar&) = delete;
  ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

  void Set(const char* value) {
    if (value != nullptr) {
      setenv(name_, value, 1);
    } else {
      unsetenv(name_);
    }
  }

  void Unset() { unsetenv(name_); }

 private:
  const char* name_;
  bool had_prev_ = false;
  std::string prev_value_;
};

// Public opj_codec_t* is implemented by opj_codec_private_t* (openjpeg.c).
opj_j2k_t* ExtractJ2kHandle(opj_codec_t* codec) {
  auto* priv = reinterpret_cast<opj_codec_private_t*>(codec);
  return priv != nullptr ? static_cast<opj_j2k_t*>(priv->m_codec) : nullptr;
}

void AppendBeFloat(std::vector<uint8_t>& out, float v) {
  OPJ_BYTE buf[4];
  opj_write_float(buf, v);
  out.insert(out.end(), std::begin(buf), std::end(buf));
}

void AppendBeDouble(std::vector<uint8_t>& out, double v) {
  OPJ_BYTE buf[8];
  opj_write_double(buf, v);
  out.insert(out.end(), std::begin(buf), std::end(buf));
}

void AppendMctElement(std::vector<uint8_t>& out, uint8_t elem_type,
                      double value) {
  if (elem_type == 0U) {
    AppendBe16(out, static_cast<uint16_t>(value));
  } else if (elem_type == 1U) {
    AppendBe32(out, static_cast<uint32_t>(value));
  } else if (elem_type == 2U) {
    AppendBeFloat(out, static_cast<float>(value));
  } else {
    AppendBeDouble(out, value);
  }
}

struct MarkerSegment {
  uint16_t id = 0;
  std::vector<uint8_t> payload;  // Excludes 2-byte id and 2-byte Lmarker.
};

struct TilePartSegment {
  uint16_t isot = 0;
  uint8_t tpsot = 0;
  uint8_t tnsot = 1;
  bool force_psot = false;
  uint32_t forced_psot = 0;
  bool emit_sod = true;
  std::vector<MarkerSegment> tph_markers;
  std::vector<uint8_t> sod_data;

  uint32_t TilePartLength() const {
    uint32_t tph_bytes = 0;
    for (const MarkerSegment& m : tph_markers) {
      tph_bytes += 4U + static_cast<uint32_t>(m.payload.size());
    }
    const uint32_t sod_bytes = emit_sod ? 2U : 0U;
    return 12U + tph_bytes + sod_bytes + static_cast<uint32_t>(sod_data.size());
  }
};

struct ParsedJ2kStream {
  std::vector<MarkerSegment> main_markers;
  std::vector<TilePartSegment> tile_parts;
  bool has_eoc = true;

  MarkerSegment* FindMainMarker(uint16_t id) {
    const auto it = std::ranges::find_if(
        main_markers, [id](const MarkerSegment& m) { return m.id == id; });
    return it != main_markers.end() ? &*it : nullptr;
  }

  void EraseMainMarkers(uint16_t id) {
    std::erase_if(main_markers,
                  [id](const MarkerSegment& m) { return m.id == id; });
  }
};

std::optional<ParsedJ2kStream> ParseCodestream(std::span<const uint8_t> bytes) {
  if (bytes.size() < 4 || ReadBe16(bytes, 0) != J2K_MS_SOC) {
    return std::nullopt;
  }
  ParsedJ2kStream parsed;
  size_t pos = 2;

  while (pos + 2 <= bytes.size()) {
    uint16_t marker = ReadBe16(bytes, pos);
    if (marker == J2K_MS_SOT) {
      break;
    }
    if (marker == J2K_MS_EOC) {
      parsed.has_eoc = true;
      return parsed;
    }
    if (pos + 4 > bytes.size()) {
      return std::nullopt;
    }
    uint16_t lmarker = ReadBe16(bytes, pos + 2);
    if (lmarker < 2 || pos + 2 + lmarker > bytes.size()) {
      return std::nullopt;
    }
    MarkerSegment seg;
    seg.id = marker;
    seg.payload.assign(bytes.begin() + pos + 4,
                       bytes.begin() + pos + 2 + lmarker);
    parsed.main_markers.push_back(std::move(seg));
    pos += 2 + lmarker;
  }

  while (pos + 2 <= bytes.size()) {
    uint16_t marker = ReadBe16(bytes, pos);
    if (marker == J2K_MS_EOC) {
      parsed.has_eoc = true;
      return parsed;
    }
    if (marker != J2K_MS_SOT || pos + 12 > bytes.size()) {
      return std::nullopt;
    }
    const size_t sot_start = pos;
    const uint16_t lsot = ReadBe16(bytes, pos + 2);
    if (lsot != 10) {
      return std::nullopt;
    }
    TilePartSegment tp;
    tp.isot = ReadBe16(bytes, pos + 4);
    const uint32_t psot = ReadBe32(bytes, pos + 6);
    tp.tpsot = bytes[pos + 10];
    tp.tnsot = bytes[pos + 11];
    pos += 12;

    while (pos + 2 <= bytes.size()) {
      uint16_t m = ReadBe16(bytes, pos);
      if (m == J2K_MS_SOD) {
        pos += 2;
        break;
      }
      if (pos + 4 > bytes.size()) {
        return std::nullopt;
      }
      uint16_t lm = ReadBe16(bytes, pos + 2);
      if (lm < 2 || pos + 2 + lm > bytes.size()) {
        return std::nullopt;
      }
      MarkerSegment tph;
      tph.id = m;
      tph.payload.assign(bytes.begin() + pos + 4, bytes.begin() + pos + 2 + lm);
      tp.tph_markers.push_back(std::move(tph));
      pos += 2 + lm;
    }

    size_t tp_end = bytes.size();
    if (psot != 0) {
      if (sot_start + psot > bytes.size() || sot_start + psot < pos) {
        return std::nullopt;
      }
      tp_end = sot_start + psot;
    } else if (bytes.size() >= 2 &&
               ReadBe16(bytes, bytes.size() - 2) == J2K_MS_EOC) {
      tp_end = bytes.size() - 2;
    }
    tp.sod_data.assign(bytes.begin() + pos, bytes.begin() + tp_end);
    pos = tp_end;
    parsed.tile_parts.push_back(std::move(tp));
  }

  parsed.has_eoc = false;
  return parsed;
}

std::vector<uint8_t> BuildCodestream(const ParsedJ2kStream& parsed) {
  std::vector<uint8_t> out;
  AppendBe16(out, J2K_MS_SOC);

  for (const MarkerSegment& seg : parsed.main_markers) {
    AppendBe16(out, seg.id);
    AppendBe16(out, static_cast<uint16_t>(2 + seg.payload.size()));
    out.insert(out.end(), seg.payload.begin(), seg.payload.end());
  }

  for (const TilePartSegment& tp : parsed.tile_parts) {
    const uint32_t psot = tp.force_psot ? tp.forced_psot : tp.TilePartLength();

    AppendBe16(out, J2K_MS_SOT);
    AppendBe16(out, 10);
    AppendBe16(out, tp.isot);
    AppendBe32(out, psot);
    out.push_back(tp.tpsot);
    out.push_back(tp.tnsot);

    for (const MarkerSegment& m : tp.tph_markers) {
      AppendBe16(out, m.id);
      AppendBe16(out, static_cast<uint16_t>(2 + m.payload.size()));
      out.insert(out.end(), m.payload.begin(), m.payload.end());
    }
    if (tp.emit_sod) {
      AppendBe16(out, J2K_MS_SOD);
    }
    out.insert(out.end(), tp.sod_data.begin(), tp.sod_data.end());
  }

  if (parsed.has_eoc) {
    AppendBe16(out, J2K_MS_EOC);
  }
  return out;
}

std::optional<size_t> FindNthMarker(std::span<const uint8_t> bytes, uint16_t id,
                                    int n) {
  int seen = 0;
  for (size_t i = 2; i + 2 <= bytes.size(); ++i) {
    if (ReadBe16(bytes, i) == id && ++seen == n) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<std::vector<uint8_t>> EncodeCustom(
    const opj_image_t* image, opj_cparameters_t* params,
    const char* const* extra_options = nullptr, EventCapture* events = nullptr,
    bool mutate_coc_qcc = false, OPJ_UINT32 mutate_comp = 1) {
  std::function<bool(opj_codec_t*, opj_image_t*)> post_setup;
  if (mutate_coc_qcc) {
    post_setup = [mutate_comp](opj_codec_t* codec, opj_image_t* encode_image) {
      opj_j2k_t* j2k = ExtractJ2kHandle(codec);
      if (j2k == nullptr || mutate_comp >= encode_image->numcomps) {
        return false;
      }
      j2k->m_cp.tcps[0].tccps[mutate_comp].cblksty ^= J2K_CCP_CBLKSTY_RESET;
      j2k->m_cp.tcps[0].tccps[mutate_comp].stepsizes[0].expn += 1;
      return true;
    };
  }
  return CompressImageToBytes(image, OPJ_CODEC_J2K, params, extra_options,
                              events, /*num_threads=*/0, std::move(post_setup));
}

OpjImagePtr DecodeBytes(std::span<const uint8_t> bytes,
                        opj_dparameters_t* custom_params = nullptr,
                        EventCapture* events = nullptr,
                        std::optional<bool> strict_mode = std::nullopt,
                        std::optional<int> threads = std::nullopt) {
  return DecompressBytesToImage(bytes, OPJ_CODEC_J2K,
                                {.custom_params = custom_params,
                                 .events = events,
                                 .strict = strict_mode,
                                 .threads = threads});
}

std::vector<uint8_t> MakeMinimalValidJ2k(uint32_t width = 16,
                                         uint32_t height = 16,
                                         uint32_t numcomps = 3,
                                         uint32_t tile_size = 0) {
  OpjImagePtr image = CreateTestImage(width, height, numcomps);
  if (!image) {
    return {};
  }
  std::optional<TileGeometry> tile_geom;
  if (tile_size > 0) {
    tile_geom = TileGeometry{.tdx = tile_size, .tdy = tile_size};
  }
  opj_cparameters_t params = MakeLosslessParams(/*numresolution=*/3, tile_geom);
  auto encoded = CompressImageToBytes(image.get(), OPJ_CODEC_J2K, &params);
  return encoded.value_or(std::vector<uint8_t>{});
}

// ============================================================================
// Group 1: Progression Order Strings & Encoder Setup Validation
// ============================================================================

TEST(J2kTest, ConvertProgressionOrderStrings) {
  EXPECT_STREQ(opj_j2k_convert_progression_order(OPJ_LRCP), "LRCP");
  EXPECT_STREQ(opj_j2k_convert_progression_order(OPJ_RLCP), "RLCP");
  EXPECT_STREQ(opj_j2k_convert_progression_order(OPJ_RPCL), "RPCL");
  EXPECT_STREQ(opj_j2k_convert_progression_order(OPJ_PCRL), "PCRL");
  EXPECT_STREQ(opj_j2k_convert_progression_order(OPJ_CPRL), "CPRL");
  EXPECT_STREQ(opj_j2k_convert_progression_order(OPJ_PROG_UNKNOWN), "");
}

TEST(J2kTest, SetupEncoderValidationErrors) {
  EventCapture events;
  opj_event_mgr_t mgr = MakeEventManager(&events);

  OpjJ2kPtr j2k(opj_j2k_create_compress());
  ASSERT_NE(j2k, nullptr);
  OpjImagePtr img = CreateTestImage(16, 16, 3);
  ASSERT_NE(img, nullptr);

  opj_cparameters_t params;
  opj_set_default_encoder_parameters(&params);

  EXPECT_EQ(opj_j2k_setup_encoder(nullptr, &params, img.get(), &mgr),
            OPJ_FALSE);
  EXPECT_EQ(opj_j2k_setup_encoder(j2k.get(), nullptr, img.get(), &mgr),
            OPJ_FALSE);
  EXPECT_EQ(opj_j2k_setup_encoder(j2k.get(), &params, nullptr, &mgr),
            OPJ_FALSE);

  // Bad numresolution.
  params.numresolution = 0;
  EXPECT_EQ(opj_j2k_setup_encoder(j2k.get(), &params, img.get(), &mgr),
            OPJ_FALSE);
  params.numresolution = OPJ_J2K_MAXRLVLS + 1;
  EXPECT_EQ(opj_j2k_setup_encoder(j2k.get(), &params, img.get(), &mgr),
            OPJ_FALSE);

  // Bad cblockw_init / cblockh_init.
  struct BadCblock {
    int w;
    int h;
  };
  constexpr BadCblock kBadCblocks[] = {
      {2, 64},    // < 4
      {2048, 4},  // > 1024
      {64, 2},    // < 4
      {4, 2048},  // > 1024
      {64, 128},  // product > 4096
      {12, 64},   // non-power-of-2 w
      {64, 12},   // non-power-of-2 h
  };
  for (const auto& bad : kBadCblocks) {
    opj_set_default_encoder_parameters(&params);
    params.cblockw_init = bad.w;
    params.cblockh_init = bad.h;
    EXPECT_EQ(opj_j2k_setup_encoder(j2k.get(), &params, img.get(), &mgr),
              OPJ_FALSE)
        << bad.w << "x" << bad.h;
  }

  // Bad cp_fixed_alloc constraints.
  int dummy_matrix[3] = {1, 1, 1};
  opj_set_default_encoder_parameters(&params);
  params.cp_fixed_alloc = 1;
  params.cp_matrice = nullptr;
  EXPECT_EQ(opj_j2k_setup_encoder(j2k.get(), &params, img.get(), &mgr),
            OPJ_FALSE);

  opj_set_default_encoder_parameters(&params);
  params.cp_fixed_alloc = 1;
  params.cp_matrice = dummy_matrix;
  params.tcp_numlayers = J2K_TCD_MATRIX_MAX_LAYER_COUNT + 1;
  EXPECT_EQ(opj_j2k_setup_encoder(j2k.get(), &params, img.get(), &mgr),
            OPJ_FALSE);

  opj_set_default_encoder_parameters(&params);
  params.cp_fixed_alloc = 1;
  params.cp_matrice = dummy_matrix;
  params.numresolution = J2K_TCD_MATRIX_MAX_RESOLUTION_COUNT + 1;
  EXPECT_EQ(opj_j2k_setup_encoder(j2k.get(), &params, img.get(), &mgr),
            OPJ_FALSE);

  // Bad tile dimensions and > 65535 tiles (using zero-alloc image_tile_create).
  {
    OpjJ2kPtr j2k_tdx(opj_j2k_create_compress());
    opj_set_default_encoder_parameters(&params);
    params.tile_size_on = OPJ_TRUE;
    params.cp_tdx = 0;
    params.cp_tdy = 8;
    EXPECT_EQ(opj_j2k_setup_encoder(j2k_tdx.get(), &params, img.get(), &mgr),
              OPJ_FALSE);
  }
  {
    OpjJ2kPtr j2k_tdy(opj_j2k_create_compress());
    opj_set_default_encoder_parameters(&params);
    params.tile_size_on = OPJ_TRUE;
    params.cp_tdx = 8;
    params.cp_tdy = 0;
    EXPECT_EQ(opj_j2k_setup_encoder(j2k_tdy.get(), &params, img.get(), &mgr),
              OPJ_FALSE);
  }

  static_assert(300 * 300 > 65535);
  {
    std::array<opj_image_cmptparm_t, 1> cmpt = {
        opj_image_cmptparm_t{.dx = 1,
                             .dy = 1,
                             .w = 300,
                             .h = 300,
                             .x0 = 0,
                             .y0 = 0,
                             .prec = 8,
                             .sgnd = 0}};
    OpjImagePtr big_img(opj_image_tile_create(1, cmpt.data(), OPJ_CLRSPC_GRAY));
    ASSERT_NE(big_img, nullptr);
    big_img->x1 = 300;
    big_img->y1 = 300;
    OpjJ2kPtr j2k_big(opj_j2k_create_compress());
    opj_set_default_encoder_parameters(&params);
    params.tile_size_on = OPJ_TRUE;
    params.cp_tdx = 1;
    params.cp_tdy = 1;
    EXPECT_EQ(
        opj_j2k_setup_encoder(j2k_big.get(), &params, big_img.get(), &mgr),
        OPJ_FALSE);
  }

  // Bad POC compno0 >= numcomps.
  {
    OpjJ2kPtr j2k_poc(opj_j2k_create_compress());
    opj_set_default_encoder_parameters(&params);
    params.numpocs = 1;
    params.POC[0].tile = 1;
    params.POC[0].compno0 = 5;
    params.POC[0].compno1 = 6;
    params.POC[0].resno0 = 0;
    params.POC[0].resno1 = 1;
    params.POC[0].layno1 = 1;
    params.POC[0].prg1 = OPJ_LRCP;
    EXPECT_EQ(opj_j2k_setup_encoder(j2k_poc.get(), &params, img.get(), &mgr),
              OPJ_FALSE);
  }

  // Singular custom MCT matrix inversion failure (cleanup must free mct_data).
  {
    OpjJ2kPtr j2k_mct(opj_j2k_create_compress());
    opj_set_default_encoder_parameters(&params);
    params.rsiz = OPJ_PROFILE_PART2 | OPJ_EXTENSION_MCT;
    params.irreversible = 1;
    std::array<OPJ_FLOAT32, 9> singular = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                                           0.0f, 0.0f, 0.0f, 1.0f};
    std::array<OPJ_INT32, 3> shifts = {0, 0, 0};
    ASSERT_EQ(OPJ_TRUE,
              opj_set_MCT(&params, singular.data(), shifts.data(), 3));
    // Upstream frees+NULLs mct_data on success; this only fires on failure.
    absl::Cleanup free_mct = [&] {
      std::free(params.mct_data);
      params.mct_data = nullptr;
    };
    EXPECT_EQ(opj_j2k_setup_encoder(j2k_mct.get(), &params, img.get(), &mgr),
              OPJ_FALSE);
  }

  // Tile size smaller than 2^(numresolutions - 1) fails encoding_validation in
  // start_compress.
  {
    events.Clear();
    OpjJ2kPtr j2k_res(opj_j2k_create_compress());
    opj_set_default_encoder_parameters(&params);
    params.numresolution = 6;  // Needs 2^5 = 32 >= image 16.
    ASSERT_EQ(opj_j2k_setup_encoder(j2k_res.get(), &params, img.get(), &mgr),
              OPJ_TRUE);
    MemWriteStream mem_out;
    OpjStreamPtr stream = mem_out.CreateStream();
    EXPECT_EQ(opj_j2k_start_compress(
                  j2k_res.get(),
                  reinterpret_cast<opj_stream_private_t*>(stream.get()),
                  img.get(), &mgr),
              OPJ_FALSE);
  }
}

// ============================================================================
// Group 2: Encode / Decode Matrices (Bit-depth, Sign, Subsampling, Tiles)
// ============================================================================

TEST(J2kTest, EncodeDecodeParameterMatrix) {
  struct MatrixCase {
    std::string_view name;
    uint32_t width = 16;
    uint32_t height = 16;
    uint32_t numcomps = 3;
    uint32_t prec = 8;
    uint32_t sgnd = 0;
    OPJ_COLOR_SPACE clrspc = OPJ_CLRSPC_SRGB;
    uint32_t dx = 1;
    uint32_t dy = 1;
    uint32_t img_x0 = 0;
    uint32_t img_y0 = 0;
    bool tile_on = false;
    int tdx = 0;
    int tdy = 0;
    int tx0 = 0;
    int ty0 = 0;
    int irreversible = 0;
    int mct = 0;
    bool expect_lossless = true;
    bool expect_mct_warning = false;
  };

  constexpr MatrixCase kCases[] = {
      {.name = "gray_8u_lossless", .numcomps = 1, .clrspc = OPJ_CLRSPC_GRAY},
      {.name = "rgb_8s_lossless", .sgnd = 1, .mct = 1},
      {.name = "rgb_12u_lossless", .prec = 12, .mct = 1},
      {.name = "rgba_16u_lossless", .numcomps = 4, .prec = 16},
      {.name = "gray_16s_lossless",
       .numcomps = 1,
       .prec = 16,
       .sgnd = 1,
       .clrspc = OPJ_CLRSPC_GRAY},
      {.name = "rgb_97_lossy",
       .width = 24,
       .height = 24,
       .irreversible = 1,
       .mct = 1,
       .expect_lossless = false},
      {.name = "yuv420_mct_disabled_warn",
       .width = 24,
       .height = 24,
       .clrspc = OPJ_CLRSPC_SYCC,
       .dx = 2,
       .dy = 2,
       .mct = 1,
       .expect_mct_warning = true},
      {.name = "multitile_offset_16u",
       .width = 20,
       .height = 20,
       .prec = 16,
       .img_x0 = 4,
       .img_y0 = 4,
       .tile_on = true,
       .tdx = 8,
       .tdy = 8,
       .tx0 = 2,
       .ty0 = 2,
       .mct = 1},
      {.name = "multitile_20u_2x2_tiles",
       .numcomps = 1,
       .prec = 20,
       .clrspc = OPJ_CLRSPC_GRAY,
       .tile_on = true,
       .tdx = 8,
       .tdy = 8},
  };

  for (const auto& tc : kCases) {
    SCOPED_TRACE(std::string(tc.name));
    // 1. Create synthetic test image and snapshot expected pixels.
    OpjImagePtr img =
        CreateTestImage(tc.width, tc.height, tc.numcomps, tc.prec, tc.sgnd,
                        tc.clrspc, tc.dx, tc.dy, tc.img_x0, tc.img_y0);
    ASSERT_NE(img, nullptr);
    const auto expected = SnapshotImagePixels(img.get());

    // 2. Encode codestream in memory.
    opj_cparameters_t params;
    opj_set_default_encoder_parameters(&params);
    params.numresolution = 3;
    params.tcp_numlayers = 1;
    params.cp_disto_alloc = 1;
    params.tcp_rates[0] = tc.irreversible ? 10.0f : 0.0f;
    params.irreversible = tc.irreversible;
    params.tcp_mct = static_cast<char>(tc.mct);
    if (tc.tile_on) {
      params.tile_size_on = OPJ_TRUE;
      params.cp_tdx = tc.tdx;
      params.cp_tdy = tc.tdy;
      params.cp_tx0 = tc.tx0;
      params.cp_ty0 = tc.ty0;
    }

    EventCapture events;
    auto bytes = EncodeCustom(img.get(), &params, nullptr, &events);
    ASSERT_TRUE(bytes.has_value());
    EXPECT_THAT(events.errors, IsEmpty());
    if (tc.expect_mct_warning) {
      EXPECT_THAT(events.warnings, Contains(HasSubstr("Disabling MCT")));
    } else {
      EXPECT_THAT(events.warnings, IsEmpty());
    }

    // 3. Decode codestream and verify pixel fidelity.
    EventCapture dec_events;
    OpjImagePtr decoded = DecodeBytes(*bytes, nullptr, &dec_events);
    ASSERT_NE(decoded, nullptr);
    EXPECT_THAT(dec_events.errors, IsEmpty());
    if (tc.expect_lossless) {
      ExpectImagePixelsEqual(expected, decoded.get());
    } else {
      ASSERT_EQ(decoded->numcomps, tc.numcomps);
      EXPECT_EQ(decoded->x1 - decoded->x0, tc.width);
      EXPECT_EQ(decoded->y1 - decoded->y0, tc.height);
      OPJ_INT32 max_abs_diff = 0;
      for (uint32_t c = 0; c < tc.numcomps; ++c) {
        const size_t count =
            static_cast<size_t>(decoded->comps[c].w) * decoded->comps[c].h;
        ASSERT_EQ(count, expected[c].size());
        for (size_t i = 0; i < count; ++i) {
          max_abs_diff =
              std::max(max_abs_diff,
                       std::abs(decoded->comps[c].data[i] - expected[c][i]));
        }
      }
      EXPECT_LE(max_abs_diff, kLossy97Rate10MaxAbsError);
    }
  }
}

// ============================================================================
// Group 3: Rate Control, Fixed Quality, Fixed Layer Matrix, Size Caps
// ============================================================================

TEST(J2kTest, RateQualityFixedMatrixAndSizeCaps) {
  // Multi-layer decreasing tcp_rates.
  {
    OpjImagePtr img = CreateTestImage(24, 24, 3);
    ASSERT_NE(img, nullptr);
    const auto expected = SnapshotImagePixels(img.get());
    opj_cparameters_t params;
    opj_set_default_encoder_parameters(&params);
    params.numresolution = 3;
    params.cp_disto_alloc = 1;
    params.tcp_numlayers = 3;
    params.tcp_rates[0] = 20.0f;
    params.tcp_rates[1] = 8.0f;
    params.tcp_rates[2] = 0.0f;  // lossless final layer
    auto bytes = EncodeCustom(img.get(), &params);
    ASSERT_TRUE(bytes.has_value());
    OpjImagePtr dec = DecodeBytes(*bytes);
    ExpectImagePixelsEqual(expected, dec.get());
  }

  // Exercise all 4 (rate_i_corr, rate_i_m_1_corr) non-decreasing tcp_rates
  // warning branches in opj_j2k_setup_encoder:
  //   {5.0, 10.0}  -> neither corrected to 1.0
  //   {0.5, 0.2}   -> both corrected to 1.0
  //   {0.5, 5.0}   -> only tcp_rates[i-1] corrected to 1.0
  //   {1.0, 0.5}   -> only tcp_rates[i] corrected to 1.0
  {
    struct RatePair {
      float r0;
      float r1;
      bool expect_corrected;
    };
    constexpr RatePair kWarningPairs[] = {
        {.r0 = 5.0f, .r1 = 10.0f, .expect_corrected = false},
        {.r0 = 0.5f, .r1 = 0.2f, .expect_corrected = true},
        {.r0 = 0.5f, .r1 = 5.0f, .expect_corrected = true},
        {.r0 = 1.0f, .r1 = 0.5f, .expect_corrected = true},
    };
    for (const auto& pair : kWarningPairs) {
      OpjImagePtr img = CreateTestImage(16, 16, 1);
      opj_cparameters_t params;
      opj_set_default_encoder_parameters(&params);
      params.numresolution = 2;
      params.cp_disto_alloc = 1;
      params.tcp_numlayers = 2;
      params.tcp_rates[0] = pair.r0;
      params.tcp_rates[1] = pair.r1;
      EventCapture events;
      auto bytes = EncodeCustom(img.get(), &params, nullptr, &events);
      ASSERT_TRUE(bytes.has_value());
      EXPECT_THAT(events.warnings,
                  Contains(HasSubstr("should be strictly lesser")));
      if (pair.expect_corrected) {
        EXPECT_THAT(events.warnings, Contains(HasSubstr("corrected as")));
      } else {
        EXPECT_THAT(events.warnings, Not(Contains(HasSubstr("corrected as"))));
      }
    }
  }

  // Fixed quality (PSNR) valid & decreasing warning.
  {
    OpjImagePtr img = CreateTestImage(24, 24, 3);
    opj_cparameters_t params;
    opj_set_default_encoder_parameters(&params);
    params.numresolution = 3;
    params.cp_fixed_quality = 1;
    params.cp_disto_alloc = 0;
    params.tcp_numlayers = 3;
    params.tcp_distoratio[0] = 25.0f;
    params.tcp_distoratio[1] = 35.0f;
    params.tcp_distoratio[2] = 0.0f;
    auto bytes = EncodeCustom(img.get(), &params);
    ASSERT_TRUE(bytes.has_value());
    EXPECT_NE(DecodeBytes(*bytes), nullptr);

    OpjImagePtr img2 = CreateTestImage(16, 16, 3);
    params.tcp_numlayers = 2;
    params.tcp_distoratio[0] = 40.0f;
    params.tcp_distoratio[1] = 20.0f;
    EventCapture events;
    ASSERT_TRUE(
        EncodeCustom(img2.get(), &params, nullptr, &events).has_value());
    EXPECT_THAT(events.warnings,
                Contains(HasSubstr("should be strictly greater")));
  }

  // cp_fixed_alloc + cp_matrice.
  {
    OpjImagePtr img = CreateTestImage(24, 24, 3);
    ASSERT_NE(img, nullptr);
    opj_cparameters_t params;
    opj_set_default_encoder_parameters(&params);
    params.numresolution = 3;
    params.tcp_numlayers = 2;
    params.cp_disto_alloc = 0;
    params.cp_fixed_alloc = 1;
    // Size: tcp_numlayers(2) * numresolution(3) * 3 = 18.
    std::array<int, 18> matrice = {
        3, 3, 3, 3, 3, 3, 3, 3, 3,  // layer 0
        6, 6, 6, 6, 6, 6, 6, 6, 6,  // layer 1
    };
    params.cp_matrice = matrice.data();
    auto bytes = EncodeCustom(img.get(), &params);
    ASSERT_TRUE(bytes.has_value());
    EXPECT_NE(DecodeBytes(*bytes), nullptr);
  }

  // max_cs_size capping warning & max_comp_size.
  {
    OpjImagePtr img = CreateTestImage(24, 24, 3);
    opj_cparameters_t params;
    opj_set_default_encoder_parameters(&params);
    params.numresolution = 3;
    params.cp_disto_alloc = 1;
    params.tcp_numlayers = 1;
    params.tcp_rates[0] = 0.0f;  // lossless capped by max_cs_size
    params.max_cs_size = 200;
    params.max_comp_size = 120;
    EventCapture events;
    auto bytes = EncodeCustom(img.get(), &params, nullptr, &events);
    ASSERT_TRUE(bytes.has_value());
    EXPECT_THAT(
        events.warnings,
        Contains(HasSubstr("desired maximum codestream size has limited")));
    EXPECT_NE(DecodeBytes(*bytes), nullptr);
  }
}

// ============================================================================
// Group 4: Comments, SOP/EPH, Codeblock Styles, Precincts, POC, ROI, Tile-Parts
// ============================================================================

TEST(J2kTest, CodingOptionsMarkersAndTileParts) {
  // Custom COM + SOP + EPH + PRT precincts + all 6 cblksty flags + ROI + POC.
  {
    OpjImagePtr img = CreateTestImage(128, 128, 3);
    ASSERT_NE(img, nullptr);
    const auto expected = SnapshotImagePixels(img.get());

    opj_cparameters_t params;
    opj_set_default_encoder_parameters(&params);
    params.numresolution = 4;  // hits res_spec shift down to < 1 clamp
    params.tcp_numlayers = 1;
    params.cp_disto_alloc = 1;
    params.tcp_rates[0] = 0;
    std::string comment = "openjpeg2_j2k_unit_test_comment";
    params.cp_comment = comment.data();
    params.csty = J2K_CP_CSTY_SOP | J2K_CP_CSTY_EPH | J2K_CP_CSTY_PRT;
    params.res_spec = 2;
    params.prcw_init[0] = 32;
    params.prch_init[0] = 32;
    params.prcw_init[1] = 16;
    params.prch_init[1] = 16;
    params.mode = J2K_CCP_CBLKSTY_LAZY | J2K_CCP_CBLKSTY_RESET |
                  J2K_CCP_CBLKSTY_TERMALL | J2K_CCP_CBLKSTY_VSC |
                  J2K_CCP_CBLKSTY_PTERM | J2K_CCP_CBLKSTY_SEGSYM;
    params.roi_compno = 0;
    params.roi_shift = 4;
    params.numpocs = 2;
    params.POC[0].tile = 1;
    params.POC[0].resno0 = 0;
    params.POC[0].compno0 = 0;
    params.POC[0].layno1 = 1;
    params.POC[0].resno1 = 2;
    params.POC[0].compno1 = 3;
    params.POC[0].prg1 = OPJ_RLCP;
    params.POC[1].tile = 1;
    params.POC[1].resno0 = 2;
    params.POC[1].compno0 = 0;
    params.POC[1].layno1 = 1;
    params.POC[1].resno1 = 4;
    params.POC[1].compno1 = 3;
    params.POC[1].prg1 = OPJ_CPRL;

    EventCapture enc_events;
    auto bytes = EncodeCustom(img.get(), &params, nullptr, &enc_events);
    EXPECT_THAT(enc_events.errors, IsEmpty());
    ASSERT_TRUE(bytes.has_value());

    OpjImagePtr dec = DecodeBytes(*bytes);
    ASSERT_NE(dec, nullptr);
    ASSERT_EQ(dec->numcomps, 3U);
    for (OPJ_UINT32 c = 0; c < dec->numcomps; ++c) {
      const size_t count =
          static_cast<size_t>(dec->comps[c].w) * dec->comps[c].h;
      ASSERT_NE(dec->comps[c].data, nullptr);
      ASSERT_EQ(count, expected[c].size());
      std::span<const OPJ_INT32> got(dec->comps[c].data, count);
      if (c == 0) {
        // OpenJPEG writes RGN without scaling encoder coefficients, while
        // t1.c right-shifts decoded coefficients >= (1 << roi_shift) down.
        for (OPJ_INT32 v : got) {
          EXPECT_GE(v, 0);
          EXPECT_LE(v, 255);
        }
        EXPECT_THAT(got, Not(ElementsAreArray(expected[0])));
      } else {
        EXPECT_THAT(got, ElementsAreArray(expected[c])) << "comp=" << c;
      }
    }
  }

  // POC missing packet check logs error inside setup_encoder (check_poc_val).
  {
    OpjImagePtr img = CreateTestImage(16, 16, 3);
    opj_cparameters_t params;
    opj_set_default_encoder_parameters(&params);
    params.numresolution = 3;
    params.tcp_numlayers = 1;
    params.cp_disto_alloc = 1;
    params.numpocs = 1;
    params.POC[0].tile = 1;
    params.POC[0].resno0 = 0;
    params.POC[0].compno0 = 0;
    params.POC[0].layno1 = 1;
    params.POC[0].resno1 = 1;  // Missing res 1, 2
    params.POC[0].compno1 = 3;
    params.POC[0].prg1 = OPJ_LRCP;

    EventCapture events;
    OpjCodecPtr codec(opj_create_compress(OPJ_CODEC_J2K));
    events.AttachTo(codec.get());
    ASSERT_EQ(OPJ_TRUE, opj_setup_encoder(codec.get(), &params, img.get()));
    EXPECT_THAT(events.errors,
                Contains(HasSubstr("Missing packets possible loss of data")));
  }

  // Tile-parts across R, L, C, P progression splits + multi-POC tile-parts.
  struct TpCase {
    OPJ_PROG_ORDER prog;
    char flag;
    int num_layers;
  };
  constexpr TpCase kTpCases[] = {
      {OPJ_LRCP, 'L', 3},
      {OPJ_RLCP, 'R', 2},
      {OPJ_CPRL, 'C', 2},
      {OPJ_PCRL, 'P', 2},
  };
  for (const auto& tc : kTpCases) {
    OpjImagePtr img = CreateTestImage(16, 16, 3);
    ASSERT_NE(img, nullptr);
    const auto expected = SnapshotImagePixels(img.get());
    opj_cparameters_t params;
    opj_set_default_encoder_parameters(&params);
    params.tile_size_on = OPJ_TRUE;
    params.cp_tdx = 8;
    params.cp_tdy = 8;
    params.numresolution = 3;
    params.prog_order = tc.prog;
    params.tp_on = 1;
    params.tp_flag = tc.flag;
    params.cp_disto_alloc = 1;
    params.tcp_numlayers = tc.num_layers;
    for (int l = 0; l < tc.num_layers - 1; ++l) {
      params.tcp_rates[l] = static_cast<float>(20 - l * 10);
    }
    params.tcp_rates[tc.num_layers - 1] = 0.0f;

    auto bytes = EncodeCustom(img.get(), &params);
    ASSERT_TRUE(bytes.has_value());
    OpjImagePtr dec = DecodeBytes(*bytes);
    ExpectImagePixelsEqual(expected, dec.get());
  }

  // Multi-POC + tile-part splitting exercises write_all_tile_parts pino >= 1.
  {
    OpjImagePtr img = CreateTestImage(16, 16, 3);
    ASSERT_NE(img, nullptr);
    const auto expected = SnapshotImagePixels(img.get());
    opj_cparameters_t params;
    opj_set_default_encoder_parameters(&params);
    params.numresolution = 3;
    params.tcp_numlayers = 2;
    params.cp_disto_alloc = 1;
    params.tcp_rates[0] = 10.0f;
    params.tcp_rates[1] = 0.0f;
    params.tp_on = 1;
    params.tp_flag = 'L';
    params.numpocs = 2;
    params.POC[0].tile = 1;
    params.POC[0].resno0 = 0;
    params.POC[0].compno0 = 0;
    params.POC[0].layno1 = 2;
    params.POC[0].resno1 = 2;
    params.POC[0].compno1 = 3;
    params.POC[0].prg1 = OPJ_LRCP;
    params.POC[1].tile = 1;
    params.POC[1].resno0 = 2;
    params.POC[1].compno0 = 0;
    params.POC[1].layno1 = 2;
    params.POC[1].resno1 = 3;
    params.POC[1].compno1 = 3;
    params.POC[1].prg1 = OPJ_RLCP;
    auto bytes = EncodeCustom(img.get(), &params);
    ASSERT_TRUE(bytes.has_value());
    EventCapture dec_events;
    OpjImagePtr dec = DecodeBytes(*bytes, nullptr, &dec_events);
    EXPECT_THAT(dec_events.errors, IsEmpty());
    ExpectImagePixelsEqual(expected, dec.get());
  }
}

// ============================================================================
// Group 5: Extra Options, TLM Scaling (>255 & >10921), Multi-Component Headers
// ============================================================================

TEST(J2kTest, ExtraOptionsTlmLimitsAndMultiComponentHeaders) {
  // Valid extra options: PLT=YES, TLM=YES, GUARD_BITS=1.
  {
    OpjImagePtr img = CreateTestImage(24, 24, 3);
    ASSERT_NE(img, nullptr);
    const auto expected = SnapshotImagePixels(img.get());
    opj_cparameters_t params;
    opj_set_default_encoder_parameters(&params);
    params.numresolution = 4;
    params.csty = J2K_CP_CSTY_PRT;
    params.res_spec = 1;
    params.prcw_init[0] = 8;
    params.prch_init[0] = 8;
    params.cblockw_init = 4;
    params.cblockh_init = 4;
    params.tcp_numlayers = 1;
    params.cp_disto_alloc = 1;

    constexpr const char* kOpts[] = {"PLT=YES", "TLM=YES", "GUARD_BITS=1",
                                     nullptr};
    auto bytes = EncodeCustom(img.get(), &params, kOpts);
    ASSERT_TRUE(bytes.has_value());
    auto parsed = ParseCodestream(*bytes);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_NE(parsed->FindMainMarker(J2K_MS_TLM), nullptr);
    ASSERT_FALSE(parsed->tile_parts.empty());
    bool found_plt = false;
    for (const auto& m : parsed->tile_parts[0].tph_markers) {
      if (m.id == J2K_MS_PLT) {
        found_plt = true;
      }
    }
    EXPECT_TRUE(found_plt);
    EventCapture dec_events;
    OpjImagePtr dec = DecodeBytes(*bytes, nullptr, &dec_events);
    EXPECT_THAT(dec_events.errors, IsEmpty());
    ExpectImagePixelsEqual(expected, dec.get());
  }

  // NO options + nullptr option list + invalid extra options.
  {
    OpjImagePtr img = CreateTestImage(8, 8, 1);
    opj_cparameters_t params;
    opj_set_default_encoder_parameters(&params);
    params.numresolution = 1;
    constexpr const char* kNoOpts[] = {"PLT=NO", "TLM=NO", "GUARD_BITS=0",
                                       nullptr};
    ASSERT_TRUE(EncodeCustom(img.get(), &params, kNoOpts).has_value());

    OpjJ2kPtr j2k(opj_j2k_create_compress());
    EventCapture events;
    opj_event_mgr_t mgr = MakeEventManager(&events);
    OpjImagePtr img2 = CreateTestImage(8, 8, 1);
    ASSERT_EQ(opj_j2k_setup_encoder(j2k.get(), &params, img2.get(), &mgr),
              OPJ_TRUE);
    EXPECT_EQ(opj_j2k_encoder_set_extra_options(j2k.get(), nullptr, &mgr),
              OPJ_TRUE);

    constexpr const char* kBad1[] = {"PLT=MAYBE", nullptr};
    constexpr const char* kBad2[] = {"TLM=MAYBE", nullptr};
    constexpr const char* kBad3[] = {"GUARD_BITS=-1", nullptr};
    constexpr const char* kBad4[] = {"GUARD_BITS=8", nullptr};
    constexpr const char* kBad5[] = {"UNKNOWN=1", nullptr};
    for (const char* const* bad : {kBad1, kBad2, kBad3, kBad4, kBad5}) {
      EXPECT_EQ(opj_j2k_encoder_set_extra_options(j2k.get(), bad, &mgr),
                OPJ_FALSE);
    }
  }

  // > 255 tile-parts for TLM (64x64 with 4x4 tiles = 256 tiles * 1 layer =
  // 256 tile-parts -> Stlm=0x60 with 16-bit Ttlmi).
  {
    OpjImagePtr img = CreateTestImage(64, 64, 1);
    ASSERT_NE(img, nullptr);
    const auto expected = SnapshotImagePixels(img.get());
    opj_cparameters_t params;
    opj_set_default_encoder_parameters(&params);
    params.tile_size_on = OPJ_TRUE;
    params.cp_tdx = 4;
    params.cp_tdy = 4;
    params.numresolution = 2;
    params.cp_disto_alloc = 1;
    params.tcp_numlayers = 1;
    params.tcp_rates[0] = 0.0f;

    auto bytes = EncodeCustom(img.get(), &params, kTlmOpts);
    ASSERT_TRUE(bytes.has_value());
    auto parsed = ParseCodestream(*bytes);
    ASSERT_TRUE(parsed.has_value());
    const MarkerSegment* tlm = parsed->FindMainMarker(J2K_MS_TLM);
    ASSERT_NE(tlm, nullptr);
    ASSERT_GE(tlm->payload.size(), 2U);
    EXPECT_EQ(tlm->payload[1], 0x60);  // ST = 2 (16-bit Ttlmi), SP = 1

    EventCapture dec_events;
    OpjImagePtr dec = DecodeBytes(*bytes, nullptr, &dec_events);
    EXPECT_THAT(dec_events.errors, IsEmpty());
    ExpectImagePixelsEqual(expected, dec.get());
  }

  // > 10921 tile-parts fails cleanly in opj_j2k_write_tlm during
  // start_compress.
  {
    OpjImagePtr img = CreateTestImage(110, 1, 1);
    ASSERT_NE(img, nullptr);
    opj_cparameters_t params;
    opj_set_default_encoder_parameters(&params);
    params.tile_size_on = OPJ_TRUE;
    params.cp_tdx = 1;
    params.cp_tdy = 1;
    params.numresolution = 1;
    params.prog_order = OPJ_LRCP;
    params.tp_on = 1;
    params.tp_flag = 'L';
    params.cp_disto_alloc = 1;
    static_assert(std::size(opj_cparameters_t{}.tcp_rates) >= 100);
    params.tcp_numlayers = 100;
    for (int l = 0; l < 99; ++l) {
      params.tcp_rates[l] = static_cast<float>(100 - l);
    }
    params.tcp_rates[99] = 0.0f;
    EventCapture events;
    EXPECT_FALSE(
        EncodeCustom(img.get(), &params, kTlmOpts, &events).has_value());
    EXPECT_THAT(events.errors, Contains(HasSubstr("10921")));
  }

  // 1-byte COC/QCC write (3 comps) and 2-byte COC/QCC/POC/RGN write + SIZ
  // buffer realloc (> OPJ_J2K_DEFAULT_HEADER_SIZE via 321 comps).
  static_assert(38 + 3 * 321 > OPJ_J2K_DEFAULT_HEADER_SIZE);
  {
    OpjImagePtr img3 = CreateTestImage(8, 8, 3);
    opj_cparameters_t params;
    opj_set_default_encoder_parameters(&params);
    params.numresolution = 2;
    params.tcp_numlayers = 1;
    params.cp_disto_alloc = 1;
    auto b3 = EncodeCustom(img3.get(), &params, nullptr, nullptr, true, 1);
    ASSERT_TRUE(b3.has_value());
    auto p3 = ParseCodestream(*b3);
    ASSERT_TRUE(p3.has_value());
    EXPECT_NE(p3->FindMainMarker(J2K_MS_COC), nullptr);
    EXPECT_NE(p3->FindMainMarker(J2K_MS_QCC), nullptr);
    EXPECT_NE(DecodeBytes(*b3), nullptr);
  }
  {
    OpjImagePtr img321 =
        CreateTestImage(2, 2, 321, 8, 0, OPJ_CLRSPC_UNSPECIFIED);
    ASSERT_NE(img321, nullptr);
    const auto expected = SnapshotImagePixels(img321.get());
    opj_cparameters_t params;
    opj_set_default_encoder_parameters(&params);
    params.numresolution = 1;
    params.tcp_numlayers = 1;
    params.cp_disto_alloc = 1;
    params.roi_compno = 260;
    params.roi_shift = 2;
    params.numpocs = 1;
    params.POC[0].tile = 1;
    params.POC[0].resno0 = 0;
    params.POC[0].compno0 = 0;
    params.POC[0].layno1 = 1;
    params.POC[0].resno1 = 1;
    params.POC[0].compno1 = 321;
    params.POC[0].prg1 = OPJ_LRCP;
    auto b321 =
        EncodeCustom(img321.get(), &params, kTlmOpts, nullptr, true, 260);
    ASSERT_TRUE(b321.has_value());
    auto p321 = ParseCodestream(*b321);
    ASSERT_TRUE(p321.has_value());
    EXPECT_NE(p321->FindMainMarker(J2K_MS_COC), nullptr);
    EXPECT_NE(p321->FindMainMarker(J2K_MS_QCC), nullptr);
    EXPECT_NE(p321->FindMainMarker(J2K_MS_RGN), nullptr);
    ASSERT_FALSE(p321->tile_parts.empty());
    bool found_poc = false;
    for (const auto& m : p321->tile_parts[0].tph_markers) {
      if (m.id == J2K_MS_POC) {
        found_poc = true;
      }
    }
    EXPECT_TRUE(found_poc);
    OpjImagePtr dec321 = DecodeBytes(*b321);
    ASSERT_NE(dec321, nullptr);
    ASSERT_EQ(dec321->numcomps, 321U);
    for (OPJ_UINT32 c = 0; c < dec321->numcomps; ++c) {
      const size_t count =
          static_cast<size_t>(dec321->comps[c].w) * dec321->comps[c].h;
      ASSERT_NE(dec321->comps[c].data, nullptr);
      ASSERT_EQ(count, expected[c].size());
      std::span<const OPJ_INT32> got(dec321->comps[c].data, count);
      if (c == 260) {
        // Empirical max abs error for comp 260 with roi_shift=2 across x86
        // SSE2 and --cpu=arm scalar paths (quantization + ROI bitshift).
        for (size_t i = 0; i < count; ++i) {
          EXPECT_LE(std::abs(got[i] - expected[c][i]), kRoiComp260MaxAbsError)
              << "comp=260 i=" << i;
        }
      } else {
        EXPECT_THAT(got, ElementsAreArray(expected[c])) << "comp=" << c;
      }
    }
  }
}

// ============================================================================
// Group 6: Profiles, Compliance Checks, Rejections, and Custom Part-2 MCT
// ============================================================================

TEST(J2kTest, ProfilesAndComplianceChecks) {
  // Profile 0 and Profile 1.
  for (OPJ_UINT16 prof : {OPJ_PROFILE_0, OPJ_PROFILE_1}) {
    OpjImagePtr img = CreateTestImage(16, 16, 3);
    opj_cparameters_t params;
    opj_set_default_encoder_parameters(&params);
    params.numresolution = 2;
    params.rsiz = prof;
    auto bytes = EncodeCustom(img.get(), &params);
    ASSERT_TRUE(bytes.has_value());
    EXPECT_NE(DecodeBytes(*bytes), nullptr);
  }

  // Compliant Cinema 2K and Cinema 4K round-trips.
  for (OPJ_UINT16 cinema_prof :
       {OPJ_PROFILE_CINEMA_2K, OPJ_PROFILE_CINEMA_4K}) {
    OpjImagePtr img = CreateTestImage(16, 16, 3, 12, 0);
    ASSERT_NE(img, nullptr);
    opj_cparameters_t params;
    opj_set_default_encoder_parameters(&params);
    params.rsiz = cinema_prof;
    params.numresolution = 3;
    params.tcp_numlayers = 2;  // forces to 1 with warning
    params.tcp_rates[0] = 20.0f;
    params.tcp_rates[1] = 10.0f;
    EventCapture events;
    auto bytes = EncodeCustom(img.get(), &params, nullptr, &events);
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(params.rsiz, cinema_prof);
    EXPECT_THAT(events.warnings, Contains(HasSubstr("1 single quality layer")));
    EXPECT_NE(DecodeBytes(*bytes), nullptr);
  }

  // Cinema non-compliant warnings & deprecated cp_cinema / cp_rsiz fallback
  // paths (setup_encoder only on zero-allocation tile image when oversized).
  {
    std::array<opj_image_cmptparm_t, 3> big_cmpts;
    for (auto& c : big_cmpts) {
      c = opj_image_cmptparm_t{.dx = 1,
                               .dy = 1,
                               .w = 5000,
                               .h = 3000,
                               .x0 = 0,
                               .y0 = 0,
                               .prec = 12,
                               .sgnd = 0};
    }
    OpjImagePtr big_img(
        opj_image_tile_create(3, big_cmpts.data(), OPJ_CLRSPC_SRGB));
    ASSERT_NE(big_img, nullptr);
    big_img->x1 = 5000;
    big_img->y1 = 3000;

    for (OPJ_UINT16 prof : {OPJ_PROFILE_CINEMA_2K, OPJ_PROFILE_CINEMA_4K}) {
      OpjCodecPtr codec(opj_create_compress(OPJ_CODEC_J2K));
      EventCapture events;
      events.AttachTo(codec.get());
      opj_cparameters_t params;
      opj_set_default_encoder_parameters(&params);
      params.rsiz = prof;
      params.numresolution = 10;  // forces clamp
      params.max_cs_size = OPJ_CINEMA_24_CS + 100;
      params.max_comp_size = OPJ_CINEMA_24_COMP + 100;
      ASSERT_EQ(opj_setup_encoder(codec.get(), &params, big_img.get()),
                OPJ_TRUE);
      EXPECT_EQ(params.rsiz, OPJ_PROFILE_NONE);
      EXPECT_THAT(events.warnings,
                  Contains(HasSubstr("Number of decomposition levels")));
    }
    {
      OpjImagePtr img16 = CreateTestImage(16, 16, 3, 12, 0);
      OpjCodecPtr codec(opj_create_compress(OPJ_CODEC_J2K));
      EventCapture events;
      events.AttachTo(codec.get());
      opj_cparameters_t params;
      opj_set_default_encoder_parameters(&params);
      params.rsiz = OPJ_PROFILE_CINEMA_4K;
      params.numresolution = 1;  // forces clamp warning (< 2)
      ASSERT_EQ(opj_setup_encoder(codec.get(), &params, img16.get()), OPJ_TRUE);
      EXPECT_THAT(
          events.warnings,
          Contains(HasSubstr("Number of decomposition levels forced to 1")));
    }

    // Wrong comp count / bitdepth / sign for cinema compliance.
    struct CinemaBadImgCase {
      uint32_t comps;
      uint32_t prec;
      uint32_t sgnd;
    };
    constexpr CinemaBadImgCase kCinemaBadCases[] = {
        {.comps = 4U, .prec = 12U, .sgnd = 0U},
        {.comps = 3U, .prec = 8U, .sgnd = 0U},
        {.comps = 3U, .prec = 12U, .sgnd = 1U},
    };
    for (const auto& tc : kCinemaBadCases) {
      OpjImagePtr bad = CreateTestImage(16, 16, tc.comps, tc.prec, tc.sgnd);
      OpjCodecPtr codec(opj_create_compress(OPJ_CODEC_J2K));
      EventCapture events;
      events.AttachTo(codec.get());
      opj_cparameters_t params;
      opj_set_default_encoder_parameters(&params);
      params.rsiz = OPJ_PROFILE_CINEMA_2K;
      ASSERT_EQ(opj_setup_encoder(codec.get(), &params, bad.get()), OPJ_TRUE);
      EXPECT_EQ(params.rsiz, OPJ_PROFILE_NONE);
    }

    // Unsupported S2K, S4K, LTS, BC_*, Part-2 bad extensions, and deprecated
    // fields.
    OpjImagePtr img16 = CreateTestImage(16, 16, 3, 12, 0);
    for (OPJ_UINT16 prof : std::to_array<OPJ_UINT16>({
             OPJ_PROFILE_CINEMA_S2K,
             OPJ_PROFILE_CINEMA_S4K,
             OPJ_PROFILE_CINEMA_LTS,
             OPJ_PROFILE_BC_SINGLE,
             OPJ_PROFILE_BC_MULTI,
             OPJ_PROFILE_BC_MULTI_R,
             OPJ_PROFILE_PART2 | OPJ_EXTENSION_NONE,
             OPJ_PROFILE_PART2 | 0x0200U,
         })) {
      OpjCodecPtr codec(opj_create_compress(OPJ_CODEC_J2K));
      EventCapture events;
      events.AttachTo(codec.get());
      opj_cparameters_t params;
      opj_set_default_encoder_parameters(&params);
      params.rsiz = prof;
      ASSERT_EQ(opj_setup_encoder(codec.get(), &params, img16.get()), OPJ_TRUE);
      EXPECT_EQ(params.rsiz, OPJ_PROFILE_NONE);
    }

    // Deprecated cp_cinema and cp_rsiz modes.
    for (OPJ_CINEMA_MODE mode :
         {OPJ_CINEMA2K_24, OPJ_CINEMA2K_48, OPJ_CINEMA4K_24}) {
      OpjCodecPtr codec(opj_create_compress(OPJ_CODEC_J2K));
      EventCapture events;
      events.AttachTo(codec.get());
      opj_cparameters_t params;
      opj_set_default_encoder_parameters(&params);
      params.cp_cinema = mode;
      ASSERT_EQ(opj_setup_encoder(codec.get(), &params, img16.get()), OPJ_TRUE);
      EXPECT_THAT(
          events.warnings,
          Contains(HasSubstr("Deprecated fields cp_cinema or cp_rsiz")));
    }
    for (OPJ_RSIZ_CAPABILITIES rcap : {OPJ_CINEMA2K, OPJ_CINEMA4K, OPJ_MCT}) {
      OpjCodecPtr codec(opj_create_compress(OPJ_CODEC_J2K));
      EventCapture events;
      events.AttachTo(codec.get());
      opj_cparameters_t params;
      opj_set_default_encoder_parameters(&params);
      params.cp_rsiz = rcap;
      ASSERT_EQ(opj_setup_encoder(codec.get(), &params, img16.get()), OPJ_TRUE);
      EXPECT_THAT(
          events.warnings,
          Contains(HasSubstr("Deprecated fields cp_cinema or cp_rsiz")));
    }
  }

  // Compliant IMF profiles (2K, 4K, 8K lossy and 2K_R, 4K_R, 8K_R reversible).
  {
    struct ImfCase {
      OPJ_UINT16 prof;
      int irreversible;
    };
    constexpr ImfCase kImfCases[] = {
        {.prof = ImfRsiz(OPJ_PROFILE_IMF_2K, 5, 3), .irreversible = 1},
        {.prof = ImfRsiz(OPJ_PROFILE_IMF_4K, 6, 4), .irreversible = 1},
        {.prof = ImfRsiz(OPJ_PROFILE_IMF_8K, 7, 5), .irreversible = 1},
        {.prof = static_cast<uint16_t>(OPJ_PROFILE_IMF_2K_R |
                                       kImfMainLevel1SubLevel1),
         .irreversible = 0},
        {.prof = static_cast<uint16_t>(OPJ_PROFILE_IMF_4K_R |
                                       kImfMainLevel1SubLevel1),
         .irreversible = 0},
        {.prof = static_cast<uint16_t>(OPJ_PROFILE_IMF_8K_R |
                                       kImfMainLevel1SubLevel1),
         .irreversible = 0},
    };
    for (const auto& tc : kImfCases) {
      OpjImagePtr img = CreateTestImage(16, 16, 3, 8, 0);
      opj_cparameters_t params;
      opj_set_default_encoder_parameters(&params);
      params.rsiz = tc.prof;
      params.irreversible = tc.irreversible;
      // Small codestream size triggers IMF sublevel bitrate-cap rate
      // adjustment.
      params.max_cs_size = 500;
      EventCapture events;
      auto bytes = EncodeCustom(img.get(), &params, nullptr, &events);
      ASSERT_TRUE(bytes.has_value());
      EXPECT_EQ(params.rsiz, tc.prof);
      EXPECT_NE(DecodeBytes(*bytes), nullptr);
    }

    // IMF compliance rejection branches (levels, comps, origins, tiles,
    // precision, subsampling, dimensions, ROI, cblock, prog, POC, mode, NL).
    auto run_imf_check = [](std::string_view label, opj_image_t* img,
                            std::string_view expected_substr, auto mutator) {
      SCOPED_TRACE(std::string(label));
      OpjCodecPtr codec(opj_create_compress(OPJ_CODEC_J2K));
      EventCapture events;
      events.AttachTo(codec.get());
      opj_cparameters_t params;
      opj_set_default_encoder_parameters(&params);
      params.rsiz = OPJ_PROFILE_IMF_2K | kImfMainLevel1SubLevel1;
      mutator(&params);
      EXPECT_EQ(opj_setup_encoder(codec.get(), &params, img), OPJ_TRUE);
      EXPECT_EQ(params.rsiz, OPJ_PROFILE_NONE);
      EXPECT_THAT(events.warnings, Contains(HasSubstr(expected_substr)));
    };

    OpjImagePtr img16 = CreateTestImage(16, 16, 3, 8, 0);
    run_imf_check("mainlevel_12", img16.get(), "mainlevel <= 11",
                  [](opj_cparameters_t* p) {
                    p->rsiz = ImfRsiz(OPJ_PROFILE_IMF_2K, 12, 0);
                  });
    run_imf_check("sublevel_9", img16.get(),
                  "sublevel <=", [](opj_cparameters_t* p) {
                    p->rsiz = ImfRsiz(OPJ_PROFILE_IMF_2K, 1, 9);
                  });
    run_imf_check("tile_origin", img16.get(), "tile origin",
                  [](opj_cparameters_t* p) {
                    p->cp_tx0 = 2;
                    p->cp_ty0 = 2;
                  });
    run_imf_check("2k_tile_lt_img", img16.get(), "single tile profiles",
                  [](opj_cparameters_t* p) {
                    p->tile_size_on = OPJ_TRUE;
                    p->cp_tdx = 8;
                    p->cp_tdy = 8;
                  });
    run_imf_check("2k_r_tile_lt_img", img16.get(), "Non-IMF codestream",
                  [](opj_cparameters_t* p) {
                    p->rsiz = OPJ_PROFILE_IMF_2K_R | kImfMainLevel1SubLevel1;
                    p->tile_size_on = OPJ_TRUE;
                    p->cp_tdx = 8;
                    p->cp_tdy = 8;
                  });
    run_imf_check("roi_forbidden", img16.get(), "forbid RGN",
                  [](opj_cparameters_t* p) { p->roi_compno = 0; });
    run_imf_check("cblock_not_32x32", img16.get(), "32x32",
                  [](opj_cparameters_t* p) {
                    p->cblockw_init = 16;
                    p->cblockh_init = 16;
                  });
    run_imf_check("prog_not_cprl", img16.get(), "CPRL",
                  [](opj_cparameters_t* p) { p->prog_order = OPJ_RLCP; });
    run_imf_check("poc_forbidden", img16.get(), "POC",
                  [](opj_cparameters_t* p) {
                    p->numpocs = 1;
                    p->POC[0].tile = 1;
                    p->POC[0].resno0 = 0;
                    p->POC[0].compno0 = 0;
                    p->POC[0].layno1 = 1;
                    p->POC[0].resno1 = 2;
                    p->POC[0].compno1 = 3;
                    p->POC[0].prg1 = OPJ_CPRL;
                  });
    run_imf_check("mode_nonzero", img16.get(), "Non-IMF codestream",
                  [](opj_cparameters_t* p) { p->mode = 1; });
    run_imf_check("2k_r_irreversible", img16.get(), "Non-IMF codestream",
                  [](opj_cparameters_t* p) {
                    p->rsiz = OPJ_PROFILE_IMF_2K_R | kImfMainLevel1SubLevel1;
                    p->irreversible = 1;
                  });
    run_imf_check("two_layers", img16.get(), "Non-IMF codestream",
                  [](opj_cparameters_t* p) { p->tcp_numlayers = 2; });
    run_imf_check("nl_too_large", img16.get(), "Non-IMF codestream",
                  [](opj_cparameters_t* p) { p->numresolution = 8; });
    run_imf_check("bad_precinct_nres", img16.get(), "PPx = PPy = 7",
                  [](opj_cparameters_t* p) {
                    p->csty = J2K_CP_CSTY_PRT;
                    p->res_spec = 1;
                    p->prcw_init[0] = 64;
                    p->prch_init[0] = 64;
                  });
    run_imf_check("bad_precinct_1res", img16.get(), "Non-IMF codestream",
                  [](opj_cparameters_t* p) {
                    p->numresolution = 1;
                    p->csty = J2K_CP_CSTY_PRT;
                    p->res_spec = 1;
                    p->prcw_init[0] = 64;
                    p->prch_init[0] = 64;
                  });

    OpjImagePtr img4c = CreateTestImage(16, 16, 4);
    run_imf_check("4_comps", img4c.get(), "at most 3 components",
                  [](opj_cparameters_t*) {});
    OpjImagePtr img_orig =
        CreateTestImage(16, 16, 3, 8, 0, OPJ_CLRSPC_SRGB, 1, 1, 2, 2);
    run_imf_check("nonzero_origin", img_orig.get(), "origin",
                  [](opj_cparameters_t*) {});
    OpjImagePtr img_signed = CreateTestImage(16, 16, 3, 8, 1);
    run_imf_check("signed", img_signed.get(), "Non-IMF codestream",
                  [](opj_cparameters_t*) {});
    OpjImagePtr img_sub =
        CreateTestImage(16, 16, 3, 8, 0, OPJ_CLRSPC_SYCC, 2, 2);
    run_imf_check("subsampled", img_sub.get(), "Non-IMF codestream",
                  [](opj_cparameters_t*) {});

    // IMF invalid subsampling: XRsiz2 = 3 and mismatched XRsiz3 != XRsiz2.
    std::array<opj_image_cmptparm_t, 3> sub_bad = {
        opj_image_cmptparm_t{.dx = 1,
                             .dy = 1,
                             .w = 16,
                             .h = 16,
                             .x0 = 0,
                             .y0 = 0,
                             .prec = 8,
                             .sgnd = 0},
        opj_image_cmptparm_t{.dx = 3,
                             .dy = 1,
                             .w = 6,
                             .h = 16,
                             .x0 = 0,
                             .y0 = 0,
                             .prec = 8,
                             .sgnd = 0},
        opj_image_cmptparm_t{.dx = 2,
                             .dy = 1,
                             .w = 8,
                             .h = 16,
                             .x0 = 0,
                             .y0 = 0,
                             .prec = 8,
                             .sgnd = 0},
    };
    OpjImagePtr img_sub_bad(
        opj_image_tile_create(3, sub_bad.data(), OPJ_CLRSPC_SRGB));
    ASSERT_NE(img_sub_bad, nullptr);
    img_sub_bad->x1 = 16;
    img_sub_bad->y1 = 16;
    run_imf_check("subsampling_mismatch", img_sub_bad.get(),
                  "Non-IMF codestream", [](opj_cparameters_t*) {});

    std::array<opj_image_cmptparm_t, 3> big9k;
    for (auto& c : big9k) {
      c = opj_image_cmptparm_t{.dx = 2,
                               .dy = 1,
                               .w = 9000,
                               .h = 7000,
                               .x0 = 0,
                               .y0 = 0,
                               .prec = 7,
                               .sgnd = 0};
    }
    OpjImagePtr img9k(opj_image_tile_create(3, big9k.data(), OPJ_CLRSPC_SRGB));
    ASSERT_NE(img9k, nullptr);
    img9k->x1 = 9000;
    img9k->y1 = 7000;
    for (OPJ_UINT16 prof :
         {OPJ_PROFILE_IMF_2K, OPJ_PROFILE_IMF_4K, OPJ_PROFILE_IMF_8K}) {
      run_imf_check(
          "oversized_9k", img9k.get(), "Non-IMF codestream",
          [prof](opj_cparameters_t* p) {
            p->rsiz = static_cast<OPJ_UINT16>(prof | kImfMainLevel1SubLevel1);
          });
    }
  }

  // Custom Part-2 MCT (opj_set_MCT -> CBD/MCT/MCC/MCO) encode + validation.
  {
    std::array<OPJ_FLOAT32, 9> fwd = {
        0.5f, 0.5f,  0.0f,  //
        0.5f, -0.5f, 0.0f, 0.0f, 0.0f, 1.0f,
    };
    std::array<OPJ_INT32, 3> dc_shift = {128, 128, 128};

    // Reversible 5-3 + custom Part-2 MCT must fail mct_validation.
    {
      OpjImagePtr img = CreateTestImage(16, 16, 3);
      opj_cparameters_t params;
      opj_set_default_encoder_parameters(&params);
      params.rsiz = OPJ_PROFILE_PART2 | OPJ_EXTENSION_MCT;
      params.irreversible = 0;
      ASSERT_EQ(OPJ_TRUE, opj_set_MCT(&params, fwd.data(), dc_shift.data(), 3));
      absl::Cleanup free_mct = [&] {
        std::free(params.mct_data);
        params.mct_data = nullptr;
      };
      EXPECT_FALSE(EncodeCustom(img.get(), &params).has_value());
    }

    // Irreversible 9-7 + custom Part-2 MCT encodes CBD/MCT/MCC/MCO markers.
    {
      // 1. Setup Part-2 MCT encoder parameters and encode to codestream.
      OpjImagePtr img = CreateTestImage(16, 16, 3);
      opj_cparameters_t params;
      opj_set_default_encoder_parameters(&params);
      params.numresolution = 3;
      params.rsiz = OPJ_PROFILE_PART2 | OPJ_EXTENSION_MCT;
      params.irreversible = 1;
      params.tcp_numlayers = 1;
      params.cp_disto_alloc = 1;
      params.tcp_rates[0] = 0;
      ASSERT_EQ(OPJ_TRUE, opj_set_MCT(&params, fwd.data(), dc_shift.data(), 3));
      absl::Cleanup free_mct = [&] {
        std::free(params.mct_data);
        params.mct_data = nullptr;
      };
      auto bytes = EncodeCustom(img.get(), &params);
      ASSERT_TRUE(bytes.has_value());

      // 2. Verify emitted Part-2 markers and expected unpatched read_cod error.
      auto parsed = ParseCodestream(*bytes);
      ASSERT_TRUE(parsed.has_value());
      EXPECT_NE(parsed->FindMainMarker(J2K_MS_CBD), nullptr);
      EXPECT_NE(parsed->FindMainMarker(J2K_MS_MCT), nullptr);
      EXPECT_NE(parsed->FindMainMarker(J2K_MS_MCC), nullptr);
      EXPECT_NE(parsed->FindMainMarker(J2K_MS_MCO), nullptr);

      EventCapture bad_events;
      EXPECT_EQ(DecodeBytes(*bytes, nullptr, &bad_events), nullptr);
      EXPECT_THAT(
          bad_events.errors,
          Contains(HasSubstr("Invalid multiple component transformation")));

      // 3. Patch COD SGcod(C) byte (payload offset 4) from 2 to 0 so read_cod
      // succeeds and read_cbd/read_mct/read_mcc/read_mco + clone execute.
      MarkerSegment* cod = parsed->FindMainMarker(J2K_MS_COD);
      ASSERT_NE(cod, nullptr);
      ASSERT_GE(cod->payload.size(), 5U);
      EXPECT_EQ(cod->payload[4], 2);
      cod->payload[4] = 0;
      std::vector<uint8_t> patched = BuildCodestream(*parsed);
      EXPECT_NE(DecodeBytes(patched), nullptr);
    }
  }
}

// ============================================================================
// Group 7: Decode Area, GetTile, Read/DecodeTileData, WriteTile, Reduce/Comps
// ============================================================================

TEST(J2kTest, DecodeAreaTileApiWriteTileResolutionAndComponents) {
  // Multi-tile 24x24 with 8x8 tiles (3x3 tiles) and TLM enabled.
  OpjImagePtr src = CreateTestImage(24, 24, 3);
  ASSERT_NE(src, nullptr);
  const auto full_expected = SnapshotImagePixels(src.get());
  opj_cparameters_t cparams;
  opj_set_default_encoder_parameters(&cparams);
  cparams.tile_size_on = OPJ_TRUE;
  cparams.cp_tdx = 8;
  cparams.cp_tdy = 8;
  cparams.numresolution = 3;
  cparams.tcp_numlayers = 2;
  cparams.cp_disto_alloc = 1;
  cparams.tcp_rates[0] = 10.0f;
  cparams.tcp_rates[1] = 0.0f;
  auto multi_bytes = EncodeCustom(src.get(), &cparams, kTlmOpts);
  ASSERT_TRUE(multi_bytes.has_value());

  // 1. opj_set_decode_area subset with TLM seeking + (0,0,0,0) whole image.
  {
    OpjCodecPtr dec(opj_create_decompress(OPJ_CODEC_J2K));
    opj_dparameters_t dp;
    opj_set_default_decoder_parameters(&dp);
    ASSERT_EQ(opj_setup_decoder(dec.get(), &dp), OPJ_TRUE);
    MemReadStream mem_in(*multi_bytes);
    OpjStreamPtr stream = mem_in.CreateStream();
    opj_image_t* raw_img = nullptr;
    ASSERT_EQ(opj_read_header(stream.get(), dec.get(), &raw_img), OPJ_TRUE);
    OpjImagePtr img(raw_img);

    // Partial region intersecting subset of tiles (with TLM seek).
    ASSERT_EQ(opj_set_decode_area(dec.get(), img.get(), 4, 4, 14, 14),
              OPJ_TRUE);
    ASSERT_EQ(opj_decode(dec.get(), stream.get(), img.get()), OPJ_TRUE);
    ASSERT_EQ(opj_end_decompress(dec.get(), stream.get()), OPJ_TRUE);
    EXPECT_EQ(img->comps[0].w, 10U);
    EXPECT_EQ(img->comps[0].h, 10U);
    for (OPJ_UINT32 y = 0; y < 10; ++y) {
      for (OPJ_UINT32 x = 0; x < 10; ++x) {
        EXPECT_EQ(img->comps[0].data[y * 10 + x],
                  full_expected[0][(y + 4) * 24 + (x + 4)]);
      }
    }
  }
  {
    // (0,0,0,0) decode area + error cases on area bounds & state.
    OpjCodecPtr dec(opj_create_decompress(OPJ_CODEC_J2K));
    EventCapture events;
    events.AttachTo(dec.get());
    opj_dparameters_t dp;
    opj_set_default_decoder_parameters(&dp);
    ASSERT_EQ(opj_setup_decoder(dec.get(), &dp), OPJ_TRUE);
    MemReadStream mem_in(*multi_bytes);
    OpjStreamPtr stream = mem_in.CreateStream();
    opj_image_t* raw_img = nullptr;
    // Before read_header -> must fail cleanly.
    EXPECT_EQ(opj_set_decode_area(dec.get(), src.get(), 0, 0, 8, 8), OPJ_FALSE);
    ASSERT_EQ(opj_read_header(stream.get(), dec.get(), &raw_img), OPJ_TRUE);
    OpjImagePtr img(raw_img);

    EXPECT_EQ(opj_set_decode_area(dec.get(), img.get(), -1, 0, 8, 8),
              OPJ_FALSE);
    EXPECT_EQ(opj_set_decode_area(dec.get(), img.get(), 30, 0, 8, 8),
              OPJ_FALSE);
    EXPECT_EQ(opj_set_decode_area(dec.get(), img.get(), 0, -1, 8, 8),
              OPJ_FALSE);
    EXPECT_EQ(opj_set_decode_area(dec.get(), img.get(), 0, 30, 8, 8),
              OPJ_FALSE);
    EXPECT_EQ(opj_set_decode_area(dec.get(), img.get(), 0, 0, 0, 8), OPJ_FALSE);
    EXPECT_EQ(opj_set_decode_area(dec.get(), img.get(), 0, 0, 8, 0), OPJ_FALSE);

    // Zero rect resets to whole image; out-of-upper-bounds clamps with warning.
    ASSERT_EQ(opj_set_decode_area(dec.get(), img.get(), 0, 0, 100, 100),
              OPJ_TRUE);
    EXPECT_THAT(events.warnings, Contains(HasSubstr("outside the image area")));
    ASSERT_EQ(opj_set_decode_area(dec.get(), img.get(), 0, 0, 0, 0), OPJ_TRUE);
    ASSERT_EQ(opj_decode(dec.get(), stream.get(), img.get()), OPJ_TRUE);
    ExpectImagePixelsEqual(full_expected, img.get());
  }

  // 2. Offset-origin image: end<start errors (x1<x0 / y1<y0) and <image->x0
  //    clamping warnings + repeated tile decode chunking.
  {
    OpjImagePtr off_img =
        CreateTestImage(16, 16, 1, 8, 0, OPJ_CLRSPC_GRAY, 1, 1, 4, 4);
    opj_cparameters_t p;
    opj_set_default_encoder_parameters(&p);
    p.numresolution = 2;
    auto b = EncodeCustom(off_img.get(), &p);
    ASSERT_TRUE(b.has_value());
    OpjCodecPtr dec(opj_create_decompress(OPJ_CODEC_J2K));
    EventCapture events;
    events.AttachTo(dec.get());
    opj_dparameters_t dp;
    opj_set_default_decoder_parameters(&dp);
    ASSERT_EQ(opj_setup_decoder(dec.get(), &dp), OPJ_TRUE);
    MemReadStream mem_in(*b);
    OpjStreamPtr stream = mem_in.CreateStream();
    opj_image_t* raw = nullptr;
    ASSERT_EQ(opj_read_header(stream.get(), dec.get(), &raw), OPJ_TRUE);
    OpjImagePtr img(raw);
    EXPECT_EQ(opj_set_decode_area(dec.get(), img.get(), 4, 4, 2, 8), OPJ_FALSE);
    EXPECT_EQ(opj_set_decode_area(dec.get(), img.get(), 4, 4, 8, 2), OPJ_FALSE);
    ASSERT_EQ(opj_set_decode_area(dec.get(), img.get(), 1, 1, 12, 12),
              OPJ_TRUE);
    EXPECT_THAT(events.warnings, Contains(HasSubstr("outside the image area")));
    // Repeated single-tile decode area chunking (reuses m_cp.tcps[0].m_data).
    ASSERT_EQ(opj_decode(dec.get(), stream.get(), img.get()), OPJ_TRUE);
    ASSERT_EQ(opj_set_decode_area(dec.get(), img.get(), 8, 8, 20, 20),
              OPJ_TRUE);
    ASSERT_EQ(opj_decode(dec.get(), stream.get(), img.get()), OPJ_TRUE);
  }

  // 3. opj_get_decoded_tile out-of-order + errors.
  {
    OpjCodecPtr dec(opj_create_decompress(OPJ_CODEC_J2K));
    opj_dparameters_t dp;
    opj_set_default_decoder_parameters(&dp);
    ASSERT_EQ(opj_setup_decoder(dec.get(), &dp), OPJ_TRUE);
    MemReadStream mem_in(*multi_bytes);
    OpjStreamPtr stream = mem_in.CreateStream();
    opj_image_t* raw = nullptr;
    ASSERT_EQ(opj_read_header(stream.get(), dec.get(), &raw), OPJ_TRUE);
    OpjImagePtr img(raw);

    EXPECT_EQ(opj_get_decoded_tile(dec.get(), stream.get(), nullptr, 0),
              OPJ_FALSE);
    EXPECT_EQ(opj_get_decoded_tile(dec.get(), stream.get(), img.get(), 99),
              OPJ_FALSE);

    OpjImagePtr small_img = CreateTestImage(8, 8, 1);
    EXPECT_EQ(opj_get_decoded_tile(dec.get(), stream.get(), small_img.get(), 0),
              OPJ_FALSE);

    ASSERT_EQ(opj_get_decoded_tile(dec.get(), stream.get(), img.get(), 4),
              OPJ_TRUE);
    EXPECT_EQ(img->x0, 8U);
    EXPECT_EQ(img->y0, 8U);
    EXPECT_EQ(img->x1, 16U);
    EXPECT_EQ(img->y1, 16U);
    for (OPJ_UINT32 y = 0; y < 8; ++y) {
      for (OPJ_UINT32 x = 0; x < 8; ++x) {
        EXPECT_EQ(img->comps[0].data[y * 8 + x],
                  full_expected[0][(y + 8) * 24 + (x + 8)]);
      }
    }
    ASSERT_EQ(opj_get_decoded_tile(dec.get(), stream.get(), img.get(), 0),
              OPJ_TRUE);
    EXPECT_EQ(img->comps[0].data[0], full_expected[0][0]);
  }

  // 4. opj_read_tile_header + opj_decode_tile_data across all 9 tiles.
  {
    OpjCodecPtr dec(opj_create_decompress(OPJ_CODEC_J2K));
    opj_dparameters_t dp;
    opj_set_default_decoder_parameters(&dp);
    ASSERT_EQ(opj_setup_decoder(dec.get(), &dp), OPJ_TRUE);
    MemReadStream mem_in(*multi_bytes);
    OpjStreamPtr stream = mem_in.CreateStream();
    opj_image_t* raw = nullptr;
    ASSERT_EQ(opj_read_header(stream.get(), dec.get(), &raw), OPJ_TRUE);
    OpjImagePtr img(raw);

    OPJ_UINT32 decoded_tiles = 0;
    for (;;) {
      OPJ_UINT32 tile_idx = 0, data_size = 0, nb_comps = 0;
      OPJ_INT32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
      OPJ_BOOL go_on = OPJ_FALSE;
      ASSERT_EQ(
          opj_read_tile_header(dec.get(), stream.get(), &tile_idx, &data_size,
                               &x0, &y0, &x1, &y1, &nb_comps, &go_on),
          OPJ_TRUE);
      if (!go_on) {
        break;
      }
      ASSERT_EQ(nb_comps, 3U);
      std::vector<OPJ_BYTE> tile_buf(data_size, 0xA5);
      // Wrong tile_idx before right tile_idx checks state error cleanly.
      EXPECT_EQ(opj_decode_tile_data(dec.get(), tile_idx + 1, tile_buf.data(),
                                     data_size, stream.get()),
                OPJ_FALSE);
      ASSERT_EQ(opj_decode_tile_data(dec.get(), tile_idx, tile_buf.data(),
                                     data_size, stream.get()),
                OPJ_TRUE);
      // prec=8 packs 1 byte/sample component-planar (comp 0 starts at offset
      // 0).
      EXPECT_EQ(tile_buf[0],
                static_cast<OPJ_BYTE>(full_expected[0][y0 * 24 + x0]));
      ++decoded_tiles;
    }
    EXPECT_EQ(decoded_tiles, 9U);
    EXPECT_EQ(opj_end_decompress(dec.get(), stream.get()), OPJ_TRUE);
  }

  // 5. opj_write_tile multi-tile manual compression + errors.
  {
    std::array<opj_image_cmptparm_t, 3> cmpts;
    for (auto& c : cmpts) {
      c = opj_image_cmptparm_t{.dx = 1,
                               .dy = 1,
                               .w = 16,
                               .h = 16,
                               .x0 = 0,
                               .y0 = 0,
                               .prec = 8,
                               .sgnd = 0};
    }
    OpjImagePtr tile_img(
        opj_image_tile_create(3, cmpts.data(), OPJ_CLRSPC_SRGB));
    ASSERT_NE(tile_img, nullptr);
    tile_img->x1 = 16;
    tile_img->y1 = 16;

    opj_cparameters_t p;
    opj_set_default_encoder_parameters(&p);
    p.tile_size_on = OPJ_TRUE;
    p.cp_tdx = 8;
    p.cp_tdy = 8;
    p.numresolution = 2;
    p.tcp_numlayers = 1;
    p.cp_disto_alloc = 1;

    OpjCodecPtr enc(opj_create_compress(OPJ_CODEC_J2K));
    ASSERT_EQ(opj_setup_encoder(enc.get(), &p, tile_img.get()), OPJ_TRUE);
    MemWriteStream mem_out;
    OpjStreamPtr out_stream = mem_out.CreateStream();
    ASSERT_EQ(opj_start_compress(enc.get(), tile_img.get(), out_stream.get()),
              OPJ_TRUE);

    std::vector<OPJ_BYTE> tile_bytes(8 * 8 * 3, 42);
    // Out-of-order index error and wrong size error.
    EXPECT_EQ(opj_write_tile(enc.get(), 1, tile_bytes.data(),
                             static_cast<OPJ_UINT32>(tile_bytes.size()),
                             out_stream.get()),
              OPJ_FALSE);
    EXPECT_EQ(
        opj_write_tile(enc.get(), 0, tile_bytes.data(), 10, out_stream.get()),
        OPJ_FALSE);

    for (OPJ_UINT32 t = 0; t < 4; ++t) {
      std::fill(tile_bytes.begin(), tile_bytes.end(),
                static_cast<OPJ_BYTE>(10 + t * 20));
      ASSERT_EQ(opj_write_tile(enc.get(), t, tile_bytes.data(),
                               static_cast<OPJ_UINT32>(tile_bytes.size()),
                               out_stream.get()),
                OPJ_TRUE);
    }
    ASSERT_EQ(opj_end_compress(enc.get(), out_stream.get()), OPJ_TRUE);
    out_stream.reset();
    OpjImagePtr dec = DecodeBytes(mem_out.data);
    ASSERT_NE(dec, nullptr);
    EXPECT_EQ(dec->comps[0].data[0], 10);
    EXPECT_EQ(dec->comps[0].data[8], 30);
    EXPECT_EQ(dec->comps[0].data[8 * 16], 50);
    EXPECT_EQ(dec->comps[0].data[8 * 16 + 8], 70);
  }

  // 6. cp_reduce / opj_set_decoded_resolution_factor + cp_layer + components.
  {
    OpjImagePtr sub = CreateTestImage(16, 16, 3, 8, 0, OPJ_CLRSPC_SYCC, 2, 2);
    opj_cparameters_t sub_p;
    opj_set_default_encoder_parameters(&sub_p);
    sub_p.numresolution = 3;
    auto sub_bytes = EncodeCustom(sub.get(), &sub_p);
    ASSERT_TRUE(sub_bytes.has_value());

    // Setup via cp_reduce and cp_layer.
    opj_dparameters_t dp;
    opj_set_default_decoder_parameters(&dp);
    dp.cp_reduce = 1;
    dp.cp_layer = 1;
    OpjImagePtr red_sub = DecodeBytes(*sub_bytes, &dp);
    ASSERT_NE(red_sub, nullptr);
    EXPECT_EQ(red_sub->comps[0].w, 8U);
    EXPECT_EQ(red_sub->comps[1].w, 4U);

    // Post-header opj_set_decoded_resolution_factor heuristic + error check
    // (resolution_factor must be strictly < numresolution == 3).
    OpjCodecPtr dec(opj_create_decompress(OPJ_CODEC_J2K));
    opj_dparameters_t dp2;
    opj_set_default_decoder_parameters(&dp2);
    ASSERT_EQ(opj_setup_decoder(dec.get(), &dp2), OPJ_TRUE);
    EXPECT_EQ(opj_set_decoded_resolution_factor(dec.get(), 1), OPJ_FALSE);
    MemReadStream mem_in(*multi_bytes);
    OpjStreamPtr stream = mem_in.CreateStream();
    opj_image_t* raw = nullptr;
    ASSERT_EQ(opj_read_header(stream.get(), dec.get(), &raw), OPJ_TRUE);
    OpjImagePtr img(raw);
    EXPECT_EQ(opj_set_decoded_resolution_factor(dec.get(), 3), OPJ_FALSE);
    ASSERT_EQ(opj_set_decoded_resolution_factor(dec.get(), 1), OPJ_TRUE);
    ASSERT_EQ(opj_decode(dec.get(), stream.get(), img.get()), OPJ_TRUE);
    EXPECT_EQ(img->comps[0].w, 12U);
    EXPECT_EQ(img->comps[0].h, 12U);
  }

  // 7. Partial component selection & error checks.
  {
    OpjCodecPtr dec(opj_create_decompress(OPJ_CODEC_J2K));
    opj_dparameters_t dp;
    opj_set_default_decoder_parameters(&dp);
    ASSERT_EQ(opj_setup_decoder(dec.get(), &dp), OPJ_TRUE);
    std::array<OPJ_UINT32, 2> comps = {2, 0};
    EXPECT_EQ(opj_set_decoded_components(dec.get(), 2, comps.data(), OPJ_FALSE),
              OPJ_FALSE);  // before read_header
    MemReadStream mem_in(*multi_bytes);
    OpjStreamPtr stream = mem_in.CreateStream();
    opj_image_t* raw = nullptr;
    ASSERT_EQ(opj_read_header(stream.get(), dec.get(), &raw), OPJ_TRUE);
    OpjImagePtr img(raw);

    std::array<OPJ_UINT32, 2> dup_comps = {1, 1};
    std::array<OPJ_UINT32, 1> oob_comps = {5};
    EXPECT_EQ(opj_set_decoded_components(dec.get(), 2, comps.data(), OPJ_TRUE),
              OPJ_FALSE);  // apply_color_transforms unsupported
    EXPECT_EQ(
        opj_set_decoded_components(dec.get(), 2, dup_comps.data(), OPJ_FALSE),
        OPJ_FALSE);
    EXPECT_EQ(
        opj_set_decoded_components(dec.get(), 1, oob_comps.data(), OPJ_FALSE),
        OPJ_FALSE);
    ASSERT_EQ(opj_set_decoded_components(dec.get(), 0, nullptr, OPJ_FALSE),
              OPJ_TRUE);
    ASSERT_EQ(opj_set_decoded_components(dec.get(), 2, comps.data(), OPJ_FALSE),
              OPJ_TRUE);
    ASSERT_EQ(opj_decode(dec.get(), stream.get(), img.get()), OPJ_TRUE);
    EXPECT_EQ(img->numcomps, 2U);
    EXPECT_EQ(img->comps[0].data[0], full_expected[2][0]);
    EXPECT_EQ(img->comps[1].data[0], full_expected[0][0]);
  }
}

// ============================================================================
// Group 8: Threads, Env Vars, Strict Mode, Dump, CstrInfo/Index, Write Failures
// ============================================================================

TEST(J2kTest, ThreadsEnvStrictDumpIndexAndStreamWriteFailures) {
  auto bytes = MakeMinimalValidJ2k(16, 16, 3, 8);
  ASSERT_FALSE(bytes.empty());

  // 1. Thread pool setup via OPJ_NUM_THREADS (read by opj_j2k_create_decompress
  //    into m_tp) and opj_j2k_set_threads.
  {
    ASSERT_EQ(OPJ_TRUE, opj_has_thread_support());  // -DMUTEX_pthread=1
    struct ThreadEnvCase {
      const char* env_val;
      int expected_threads;
    };
    const ThreadEnvCase kEnvCases[] = {
        {.env_val = "ALL_CPUS", .expected_threads = opj_get_num_cpus()},
        {.env_val = "2", .expected_threads = 2},
        {.env_val = "-1", .expected_threads = 0},
    };
    for (const auto& tc : kEnvCases) {
      ScopedEnvVar guard("OPJ_NUM_THREADS");
      guard.Set(tc.env_val);
      OpjJ2kPtr j2k(opj_j2k_create_decompress());
      ASSERT_NE(j2k, nullptr);
      EXPECT_EQ(tc.expected_threads,
                opj_thread_pool_get_thread_count(j2k->m_tp));
      opj_dparameters_t dp;
      opj_set_default_decoder_parameters(&dp);
      opj_j2k_setup_decoder(j2k.get(), &dp);
      EXPECT_EQ(OPJ_TRUE, opj_j2k_set_threads(j2k.get(), 0));
      EXPECT_EQ(0, opj_thread_pool_get_thread_count(j2k->m_tp));
      EXPECT_EQ(OPJ_TRUE, opj_j2k_set_threads(j2k.get(), 2));
      EXPECT_EQ(2, opj_thread_pool_get_thread_count(j2k->m_tp));
      // > INT_MAX hits opj_j2k_set_threads num_threads <= INT_MAX guard.
      EXPECT_EQ(OPJ_FALSE, opj_j2k_set_threads(j2k.get(), UINT32_MAX));
      // Destroy j2k pool before guard restores environ (no worker threads
      // live).
      j2k.reset();
    }
    EXPECT_NE(DecodeBytes(bytes, nullptr, nullptr, std::nullopt, 2), nullptr);
  }

  // 2. j2k_dump (all flags + OPJ_JP2_INFO wrong flag + TLM hide env var) and
  //    j2k_get_cstr_info / j2k_get_cstr_index (pins j2k_dump fprintf text).
  {
    OpjImagePtr src = CreateTestImage(16, 16, 3);
    opj_cparameters_t cp = MakeLosslessParams(/*numresolution=*/3,
                                              TileGeometry{.tdx = 8, .tdy = 8});
    auto tlm_bytes = EncodeCustom(src.get(), &cp, kTlmOpts);
    ASSERT_TRUE(tlm_bytes.has_value());

    TempFile dump_file("j2k_dump.txt");
    {
      OpjCodecPtr dec(opj_create_decompress(OPJ_CODEC_J2K));
      opj_dparameters_t dp;
      opj_set_default_decoder_parameters(&dp);
      dp.flags = OPJ_DPARAMETERS_DUMP_FLAG;
      ASSERT_EQ(opj_setup_decoder(dec.get(), &dp), OPJ_TRUE);

      MemReadStream mem_in(*tlm_bytes);
      OpjStreamPtr stream = mem_in.CreateStream();
      opj_image_t* raw = nullptr;
      ASSERT_EQ(opj_read_header(stream.get(), dec.get(), &raw), OPJ_TRUE);
      OpjImagePtr img(raw);

      ScopedCstrInfo info(opj_get_cstr_info(dec.get()));
      ASSERT_TRUE(info);
      EXPECT_EQ(info->tw, 2U);
      EXPECT_EQ(info->th, 2U);
      EXPECT_EQ(info->nbcomps, 3U);

      opj_j2k_t* j2k = ExtractJ2kHandle(dec.get());
      ASSERT_NE(j2k, nullptr);
      // After read_header (m_tcd != NULL), opj_j2k_set_threads rejects changes.
      EXPECT_EQ(opj_j2k_set_threads(j2k, 2), OPJ_FALSE);
      {
        ScopedFile sf{std::fopen(dump_file.path_str().c_str(), "wb")};
        ASSERT_NE(sf.fp, nullptr);
        j2k_dump(j2k, OPJ_JP2_INFO, sf.fp);
        j2k_dump(j2k,
                 OPJ_IMG_INFO | OPJ_J2K_MH_INFO | OPJ_J2K_TH_INFO |
                     OPJ_J2K_TCH_INFO | OPJ_J2K_MH_IND | OPJ_J2K_TH_IND,
                 sf.fp);
        j2k_dump_image_header(img.get(), OPJ_TRUE, sf.fp);
        j2k_dump_image_comp_header(&img->comps[0], OPJ_TRUE, sf.fp);
        ScopedEnvVar hide_tlm("OJP_DO_NOT_DISPLAY_TILE_INDEX_IF_TLM");
        hide_tlm.Set("1");
        j2k_dump(j2k, OPJ_J2K_MH_IND, sf.fp);
        sf.reset();
      }
      std::ifstream in_file(dump_file.path(), std::ios::binary);
      std::string dump_str((std::istreambuf_iterator<char>(in_file)),
                           std::istreambuf_iterator<char>());
      EXPECT_THAT(dump_str, HasSubstr("Wrong flag"));
      EXPECT_THAT(dump_str, HasSubstr("Codestream info from main header"));
      EXPECT_THAT(dump_str, HasSubstr("Codestream index from main header"));

      // Right after opj_read_header (before SOT markers are processed), tile 0
      // has nb_tps == 0, so j2k_get_cstr_index hits opj_malloc(0) == nullptr on
      // tile 0 and frees temporary allocations cleanly without leaking.
      ScopedCstrIndex early_idx(opj_get_cstr_index(dec.get()));
      EXPECT_FALSE(early_idx);
      ASSERT_EQ(opj_decode(dec.get(), stream.get(), img.get()), OPJ_TRUE);
      ScopedCstrIndex full_idx(opj_get_cstr_index(dec.get()));
      ASSERT_TRUE(full_idx);
      EXPECT_EQ(full_idx->nb_of_tiles, 4U);
    }
  }

  // 3. Strict vs non-strict decoding on truncated EOC and truncated tile data.
  {
    auto parsed = ParseCodestream(bytes);
    ASSERT_TRUE(parsed.has_value());
    parsed->has_eoc = false;
    std::vector<uint8_t> no_eoc = BuildCodestream(*parsed);
    // Strict decode fails when stream ends before expected EOC/marker.
    EXPECT_EQ(DecodeBytes(no_eoc, nullptr, nullptr, true), nullptr);
    // Non-strict decode succeeds with a "Stream too short" warning at EOF.
    EventCapture non_strict_ev;
    EXPECT_NE(DecodeBytes(no_eoc, nullptr, &non_strict_ev, false), nullptr);
    EXPECT_THAT(non_strict_ev.warnings,
                Contains(HasSubstr("Stream too short")));

    // Truncate last 6 bytes (drops 2-byte EOC + 4 bytes of final tile-part).
    ASSERT_GT(bytes.size(), 6U);
    std::vector<uint8_t> truncated = bytes;
    truncated.resize(bytes.size() - 6);
    EXPECT_EQ(DecodeBytes(truncated, nullptr, nullptr, true), nullptr);
    EXPECT_NE(DecodeBytes(truncated, nullptr, nullptr, false), nullptr);
  }

  // 4. Write stream failure tests using 1-byte MemWriteStream buffer + fail
  //    points so marker writers & write_updated_tlm flush failures trigger.
  //    Boundaries: 1 (SOC), 10 (SIZ), 50 (COD), 65 (QCD/RGN), 85 (COM/TLM),
  //    110 (updated TLM / tile data); failing stage intentionally unspecified.
  {
    for (size_t max_bytes : {1U, 10U, 50U, 65U, 85U, 110U}) {
      OpjImagePtr img = CreateTestImage(8, 8, 3);
      opj_cparameters_t params;
      opj_set_default_encoder_parameters(&params);
      params.numresolution = 2;
      params.roi_compno = 0;
      params.roi_shift = 1;
      std::string comment = "x";
      params.cp_comment = comment.data();
      OpjCodecPtr codec(opj_create_compress(OPJ_CODEC_J2K));
      ASSERT_EQ(opj_setup_encoder(codec.get(), &params, img.get()), OPJ_TRUE);
      ASSERT_EQ(opj_encoder_set_extra_options(codec.get(), kTlmOpts), OPJ_TRUE);

      MemWriteStream mem_out;
      mem_out.max_bytes_before_fail = max_bytes;
      OpjStreamPtr stream = mem_out.CreateStream(1);
      bool ok = opj_start_compress(codec.get(), img.get(), stream.get()) &&
                opj_encode(codec.get(), stream.get()) &&
                opj_end_compress(codec.get(), stream.get());
      EXPECT_FALSE(ok);
    }

    // 5. fail_seek on MemWriteStream fails opj_j2k_write_updated_tlm cleanly.
    {
      OpjImagePtr img = CreateTestImage(8, 8, 1);
      opj_cparameters_t params;
      opj_set_default_encoder_parameters(&params);
      params.numresolution = 2;
      OpjCodecPtr codec(opj_create_compress(OPJ_CODEC_J2K));
      ASSERT_EQ(opj_setup_encoder(codec.get(), &params, img.get()), OPJ_TRUE);
      ASSERT_EQ(opj_encoder_set_extra_options(codec.get(), kTlmOpts), OPJ_TRUE);
      MemWriteStream mem_out;
      mem_out.fail_seek = true;
      OpjStreamPtr stream = mem_out.CreateStream();
      ASSERT_EQ(opj_start_compress(codec.get(), img.get(), stream.get()),
                OPJ_TRUE);
      ASSERT_EQ(opj_encode(codec.get(), stream.get()), OPJ_TRUE);
      EXPECT_EQ(opj_end_compress(codec.get(), stream.get()), OPJ_FALSE);
    }
  }
}

// ============================================================================
// Group 9: StreamBuilder Main & Tile-Part Marker Readers (incl. Part-2 MCT)
// ============================================================================

TEST(J2kTest, StreamBuilderMarkersAndPart2MctVariants) {
  auto base_bytes = MakeMinimalValidJ2k(16, 16, 3, 0);
  ASSERT_FALSE(base_bytes.empty());

  // Inject valid MH markers: CAP, CPF, CRG, PLM, COM (binary), POC, plus
  // TPH markers: COC, QCC, RGN, PLT, COM (latin1).
  {
    auto parsed = ParseCodestream(base_bytes);
    ASSERT_TRUE(parsed.has_value());
    parsed->main_markers.push_back(MarkerSegment{J2K_MS_CAP, {0, 0, 0, 0}});
    parsed->main_markers.push_back(MarkerSegment{J2K_MS_CPF, {0, 1}});
    parsed->main_markers.push_back(
        MarkerSegment{J2K_MS_CRG, std::vector<uint8_t>(12, 0)});
    parsed->main_markers.push_back(MarkerSegment{J2K_MS_PLM, {0, 0}});
    parsed->main_markers.push_back(
        MarkerSegment{J2K_MS_COM, {0, 0, 0xDE, 0xAD}});
    // MH POC covering all 3 res, 3 comps, 1 layer.
    parsed->main_markers.push_back(
        MarkerSegment{J2K_MS_POC, {0, 0, 0, 1, 3, 3, OPJ_CPRL}});

    ASSERT_EQ(parsed->tile_parts.size(), 1U);
    auto& tp = parsed->tile_parts[0];
    // TPH QCC for comp 1 (NOQNT for 3 res -> 7 bands, Sqcc=0x40 + 7 expn
    // bytes).
    tp.tph_markers.push_back(MarkerSegment{
        J2K_MS_QCC, {1, 0x40, 0x40, 0x48, 0x48, 0x50, 0x48, 0x48, 0x50}});
    // TPH COC for comp 2 matching COD defaults.
    tp.tph_markers.push_back(
        MarkerSegment{J2K_MS_COC, {2, 0x00, 2, 4, 4, 0, 1}});
    tp.tph_markers.push_back(MarkerSegment{J2K_MS_RGN, {0, 0, 1}});
    tp.tph_markers.push_back(MarkerSegment{J2K_MS_PLT, {0, 0x81, 0x05}});
    tp.tph_markers.push_back(MarkerSegment{J2K_MS_COM, {0, 1, 'h', 'i'}});

    std::vector<uint8_t> rebuilt = BuildCodestream(*parsed);
    EXPECT_NE(DecodeBytes(rebuilt), nullptr);
  }

  // Synthesize unknown markers in MH (chained 0xFF5A + 0xFF5B followed by COM)
  // and invalid unknown/misplaced markers in MH/TPH.
  {
    auto parsed = ParseCodestream(base_bytes);
    ASSERT_TRUE(parsed.has_value());
    std::vector<uint8_t> unk_stream;
    // Place valid MH up to first SOT, insert two unknown 2-byte markers
    // 0xFF5A 0xFF5B right before COM, then append rest.
    const std::optional<size_t> sot_opt =
        FindNthMarker(base_bytes, J2K_MS_SOT, 1);
    ASSERT_TRUE(sot_opt.has_value());
    const size_t sot_pos = *sot_opt;
    unk_stream.insert(unk_stream.end(), base_bytes.begin(),
                      base_bytes.begin() + sot_pos);
    AppendBe16(unk_stream, 0xFF5A);
    AppendBe16(unk_stream, 0xFF5B);
    AppendBe16(unk_stream, J2K_MS_COM);
    AppendBe16(unk_stream, 4);
    AppendBe16(unk_stream, 1);
    unk_stream.insert(unk_stream.end(), base_bytes.begin() + sot_pos,
                      base_bytes.end());
    EventCapture events;
    EXPECT_NE(DecodeBytes(unk_stream, nullptr, &events), nullptr);
    EXPECT_THAT(events.warnings, Contains(HasSubstr("Unknown marker")));

    // Unknown marker right before EOC (wrong state for EOC in MH) -> fails.
    auto bad_unk =
        std::vector<uint8_t>(base_bytes.begin(), base_bytes.begin() + sot_pos);
    AppendBe16(bad_unk, 0xFF5A);
    AppendBe16(bad_unk, J2K_MS_EOC);
    EXPECT_EQ(DecodeBytes(bad_unk), nullptr);

    // Truncated unknown marker -> fails.
    auto short_unk =
        std::vector<uint8_t>(base_bytes.begin(), base_bytes.begin() + sot_pos);
    AppendBe16(short_unk, 0xFF5A);
    EXPECT_EQ(DecodeBytes(short_unk), nullptr);
  }
  {
    // Unknown marker inside TPH hits !l_marker_handler->handler.
    auto parsed = ParseCodestream(base_bytes);
    ASSERT_TRUE(parsed.has_value());
    parsed->tile_parts[0].tph_markers.push_back(MarkerSegment{0xFF5A, {0, 0}});
    EXPECT_EQ(DecodeBytes(BuildCodestream(*parsed)), nullptr);

    // Misplaced MH-only marker (CRG) inside TPH -> fails.
    parsed = ParseCodestream(base_bytes);
    ASSERT_TRUE(parsed.has_value());
    parsed->tile_parts[0].tph_markers.push_back(
        MarkerSegment{J2K_MS_CRG, std::vector<uint8_t>(12, 0)});
    EXPECT_EQ(DecodeBytes(BuildCodestream(*parsed)), nullptr);

    // Misplaced TPH-only marker (PLT) inside MH -> fails.
    parsed = ParseCodestream(base_bytes);
    ASSERT_TRUE(parsed.has_value());
    parsed->main_markers.push_back(MarkerSegment{J2K_MS_PLT, {0, 1}});
    EXPECT_EQ(DecodeBytes(BuildCodestream(*parsed)), nullptr);
  }

  // Test all 4 MCT element types (INT16, INT32, FLOAT, DOUBLE) for both
  // DECORRELATION and OFFSET arrays via StreamBuilder, plus MCT realloc (>10
  // records) and CBD prec > 31 error.
  for (uint8_t elem_type : {0U, 1U, 2U, 3U}) {
    auto parsed = ParseCodestream(base_bytes);
    ASSERT_TRUE(parsed.has_value());

    // MCT #1 (index=1): DECORRELATION (array_type=1) 3x3 identity.
    std::vector<uint8_t> mct_deco;
    AppendBe16(mct_deco, 0);  // Zmct
    AppendBe16(mct_deco,
               static_cast<uint16_t>(1U | (1U << 8) | (elem_type << 10)));
    AppendBe16(mct_deco, 0);  // Ymct
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) {
        AppendMctElement(mct_deco, elem_type, (r == c) ? 1.0 : 0.0);
      }
    }
    parsed->main_markers.push_back(MarkerSegment{J2K_MS_MCT, mct_deco});

    // MCT #2 (index=2): OFFSET (array_type=2) [10, 10, 10].
    std::vector<uint8_t> mct_off;
    AppendBe16(mct_off, 0);  // Zmct
    AppendBe16(mct_off,
               static_cast<uint16_t>(2U | (2U << 8) | (elem_type << 10)));
    AppendBe16(mct_off, 0);  // Ymct
    for (int c = 0; c < 3; ++c) {
      AppendMctElement(mct_off, elem_type, 10.0);
    }
    parsed->main_markers.push_back(MarkerSegment{J2K_MS_MCT, mct_off});

    // MCC #1 referencing deco index 1 and offset index 2 (Wmcc field bytes:
    // irr=0, offset=2, deco=1 -> 0x01 0x02 0x01 or irreversible bit16=0).
    std::vector<uint8_t> mcc;
    AppendBe16(mcc, 0);  // Zmcc
    mcc.push_back(1);    // Imcc = 1
    AppendBe16(mcc, 0);  // Ymcc
    AppendBe16(mcc, 1);  // Qmcc = 1
    mcc.push_back(1);    // Xmcci = 1 (array decorrelation)
    AppendBe16(mcc, 3);  // Nmcc = 3
    mcc.push_back(0);
    mcc.push_back(1);
    mcc.push_back(2);
    AppendBe16(mcc, 3);  // Mmcc = 3
    mcc.push_back(0);
    mcc.push_back(1);
    mcc.push_back(2);
    mcc.push_back(0);  // Tmcc high byte: bit 16 == 0 (irreversible)
    mcc.push_back(2);  // offset index = 2
    mcc.push_back(1);  // decorr index = 1
    parsed->main_markers.push_back(MarkerSegment{J2K_MS_MCC, mcc});

    // Extra MCT records #3..#12 after MCC so opj_realloc triggers fixup loop
    // in opj_j2k_read_mct (new_mct_records != l_tcp->m_mct_records).
    if (elem_type == 0) {
      for (uint16_t k = 3; k <= 12; ++k) {
        std::vector<uint8_t> extra;
        AppendBe16(extra, 0);
        AppendBe16(extra, k);
        AppendBe16(extra, 0);
        AppendBe16(extra, 1);
        parsed->main_markers.push_back(MarkerSegment{J2K_MS_MCT, extra});
      }
    }

    // MCO with 1 stage referencing MCC 1.
    parsed->main_markers.push_back(MarkerSegment{J2K_MS_MCO, {1, 1}});
    // CBD with 3 x 8-bit unsigned.
    parsed->main_markers.push_back(MarkerSegment{J2K_MS_CBD, {0, 3, 7, 7, 7}});

    EXPECT_NE(DecodeBytes(BuildCodestream(*parsed)), nullptr);
  }

  // MCT / MCC / MCO / CBD / POC / CRG / PLM / PLT error paths.
  auto check_bad_mh_marker = [&](uint16_t id, std::vector<uint8_t> payload,
                                 bool expect_fail = true) {
    auto parsed = ParseCodestream(base_bytes);
    ASSERT_TRUE(parsed.has_value());
    parsed->main_markers.push_back(MarkerSegment{id, std::move(payload)});
    OpjImagePtr res = DecodeBytes(BuildCodestream(*parsed));
    if (expect_fail) {
      EXPECT_EQ(res, nullptr);
    } else {
      EXPECT_NE(res, nullptr);
    }
  };
  check_bad_mh_marker(J2K_MS_CRG, {0, 0});
  check_bad_mh_marker(J2K_MS_PLM, {});
  check_bad_mh_marker(J2K_MS_POC, {0, 0, 0});
  // > J2K_MAX_POCS (32 entries in POC).
  {
    std::vector<uint8_t> many_pocs;
    for (int i = 0; i < J2K_MAX_POCS; ++i) {
      many_pocs.insert(many_pocs.end(), {0, 0, 0, 1, 3, 3, OPJ_LRCP});
    }
    check_bad_mh_marker(J2K_MS_POC, std::move(many_pocs));
  }
  check_bad_mh_marker(J2K_MS_CBD, {0, 3});
  check_bad_mh_marker(J2K_MS_CBD, {0, 2, 7, 7, 7});
  check_bad_mh_marker(J2K_MS_CBD, {0, 3, 31, 7, 7});  // prec 32 > 31
  check_bad_mh_marker(J2K_MS_MCT, {0});
  check_bad_mh_marker(J2K_MS_MCT, {0, 0, 0, 1, 0, 0}, true);  // Zmct=0, len<=6
  check_bad_mh_marker(J2K_MS_MCT, {0, 1}, false);             // Zmct != 0 warn
  check_bad_mh_marker(J2K_MS_MCT, {0, 0, 0, 1, 0, 1, 0}, false);  // Ymct != 0
  check_bad_mh_marker(J2K_MS_MCC, {0});
  check_bad_mh_marker(J2K_MS_MCC, {0, 0, 1, 0, 0}, true);  // Zmcc=0, len<7
  check_bad_mh_marker(J2K_MS_MCC, {0, 1}, false);          // Zmcc != 0 warn
  check_bad_mh_marker(J2K_MS_MCC, {0, 0, 1, 0, 1, 0, 1}, false);  // Ymcc != 0
  check_bad_mh_marker(J2K_MS_MCC, {0, 0, 1, 0, 0, 0, 2}, false);  // Qmcc > 1
  // Xmcci != 1 warning, Cmcc shuffle warning, Mmcc != Nmcc warning, Wmcc
  // shuffle warning.
  check_bad_mh_marker(J2K_MS_MCC, {0, 0, 1, 0, 0, 0, 1, 2, 0, 3}, false);
  check_bad_mh_marker(J2K_MS_MCC, {0, 0, 1, 0, 0, 0, 1, 1, 0, 3, 2, 1, 0, 0, 3},
                      false);
  check_bad_mh_marker(J2K_MS_MCC, {0, 0, 1, 0, 0, 0, 1, 1, 0, 3, 0, 1, 2, 0, 2},
                      false);
  check_bad_mh_marker(
      J2K_MS_MCC,
      {0, 0, 1, 0, 0, 0, 1, 1, 0, 3, 0, 1, 2, 0, 3, 2, 1, 0, 0, 0, 0}, false);
  // Missing deco MCT index 99 and missing offset MCT index 99 in MCC -> fail.
  check_bad_mh_marker(J2K_MS_MCC, {0, 0, 1, 0, 0, 0, 1, 1, 0, 3, 0,
                                   1, 2, 0, 3, 0, 1, 2, 0, 0, 99});
  check_bad_mh_marker(J2K_MS_MCC, {0, 0, 1, 0, 0, 0, 1, 1, 0,  3, 0,
                                   1, 2, 0, 3, 0, 1, 2, 0, 99, 0});
  check_bad_mh_marker(J2K_MS_MCO, {});
  check_bad_mh_marker(J2K_MS_MCO, {2, 1, 2}, false);       // Nmco > 1 warning
  check_bad_mh_marker(J2K_MS_MCO, {1});                    // length mismatch
  check_bad_mh_marker(J2K_MS_COC, {0});                    // short COC
  check_bad_mh_marker(J2K_MS_COC, {9, 0, 2, 4, 4, 0, 1});  // COC comp OOB
  check_bad_mh_marker(J2K_MS_COC,
                      {1, 0, 2, 4, 4, 0, 1, 99});  // COC trailing byte
  check_bad_mh_marker(J2K_MS_QCC, {});             // empty QCC
  check_bad_mh_marker(J2K_MS_QCC,
                      {9, 0x40, 0x40, 0x48, 0x48, 0x50, 0x48, 0x48, 0x50});
  check_bad_mh_marker(J2K_MS_QCC, {1, 0x42, 0x40, 0x00, 0});
  check_bad_mh_marker(J2K_MS_RGN, {0, 0});     // RGN bad size
  check_bad_mh_marker(J2K_MS_RGN, {9, 0, 1});  // RGN comp OOB

  // PLT errors inside TPH (empty, truncated varint MSB 0x81).
  for (std::vector<uint8_t> bad_plt :
       {std::vector<uint8_t>{}, std::vector<uint8_t>{0, 0x81}}) {
    auto parsed = ParseCodestream(base_bytes);
    ASSERT_TRUE(parsed.has_value());
    parsed->tile_parts[0].tph_markers.push_back(
        MarkerSegment{J2K_MS_PLT, std::move(bad_plt)});
    EXPECT_EQ(DecodeBytes(BuildCodestream(*parsed)), nullptr);
  }
}

// ============================================================================
// Group 10: PPM & PPT Merging (EPH Packet Header Extraction)
// ============================================================================

TEST(J2kTest, StreamBuilderPpmAndPptMerging) {
  // Single-tile 8x8 lossless EPH stream -> 1 packet, first 0xFF 0x92 ends pkt
  // header cleanly.
  OpjImagePtr img = CreateTestImage(8, 8, 1);
  ASSERT_NE(img, nullptr);
  const auto expected = SnapshotImagePixels(img.get());
  opj_cparameters_t params;
  opj_set_default_encoder_parameters(&params);
  params.numresolution = 1;
  params.tcp_numlayers = 1;
  params.cp_disto_alloc = 1;
  params.csty = J2K_CP_CSTY_EPH;
  auto base = EncodeCustom(img.get(), &params);
  ASSERT_TRUE(base.has_value());

  auto parsed_base = ParseCodestream(*base);
  ASSERT_TRUE(parsed_base.has_value());
  ASSERT_EQ(parsed_base->tile_parts.size(), 1U);
  const auto& sod = parsed_base->tile_parts[0].sod_data;
  size_t eph_pos = std::string::npos;
  for (size_t i = 0; i + 1 < sod.size(); ++i) {
    if (sod[i] == 0xFF && sod[i + 1] == 0x92) {
      eph_pos = i;
      break;
    }
  }
  ASSERT_NE(eph_pos, std::string::npos);
  std::vector<uint8_t> pkt_hdr(sod.begin(), sod.begin() + eph_pos + 2);
  std::vector<uint8_t> pkt_body(sod.begin() + eph_pos + 2, sod.end());
  ASSERT_GE(pkt_hdr.size(), 2U);

  // 1. Valid single PPM & non-contiguous split PPM (Zppm=0, then Zppm=2).
  {
    auto p_single = *parsed_base;
    p_single.tile_parts[0].sod_data = pkt_body;
    std::vector<uint8_t> ppm0 = {0};
    AppendBe32(ppm0, static_cast<uint32_t>(pkt_hdr.size()));
    ppm0.insert(ppm0.end(), pkt_hdr.begin(), pkt_hdr.end());
    p_single.main_markers.push_back(MarkerSegment{J2K_MS_PPM, ppm0});
    ExpectImagePixelsEqual(expected,
                           DecodeBytes(BuildCodestream(p_single)).get());

    auto p_split = *parsed_base;
    p_split.tile_parts[0].sod_data = pkt_body;
    std::vector<uint8_t> s0 = {0};
    AppendBe32(s0, static_cast<uint32_t>(pkt_hdr.size()));
    s0.push_back(pkt_hdr[0]);
    std::vector<uint8_t> s2 = {2};
    s2.insert(s2.end(), pkt_hdr.begin() + 1, pkt_hdr.end());
    p_split.main_markers.push_back(MarkerSegment{J2K_MS_PPM, s0});
    p_split.main_markers.push_back(MarkerSegment{J2K_MS_PPM, s2});
    ExpectImagePixelsEqual(expected,
                           DecodeBytes(BuildCodestream(p_split)).get());
  }

  // 2. PPM error cases: short, duplicate Zppm, short Nppm, overflow Nppm,
  //    dangling remaining bytes, and PPM + PPT collision.
  for (const auto& bad_ppm : {
           std::vector<uint8_t>{0},
           std::vector<uint8_t>{0, 1, 2},
           std::vector<uint8_t>{0, 0xFF, 0xFF, 0xFF, 0xFF, 1},
           std::vector<uint8_t>{0, 0, 0, 0, 10, 1},
       }) {
    auto p = *parsed_base;
    p.main_markers.push_back(MarkerSegment{J2K_MS_PPM, bad_ppm});
    EXPECT_EQ(DecodeBytes(BuildCodestream(p)), nullptr);
  }
  {
    auto p = *parsed_base;
    p.main_markers.push_back(MarkerSegment{J2K_MS_PPM, {0, 0, 0, 0, 0}});
    p.main_markers.push_back(MarkerSegment{J2K_MS_PPM, {0, 0, 0, 0, 0}});
    EXPECT_EQ(DecodeBytes(BuildCodestream(p)), nullptr);
  }
  {
    auto p = *parsed_base;
    p.tile_parts[0].sod_data = pkt_body;
    std::vector<uint8_t> ppm0 = {0};
    AppendBe32(ppm0, static_cast<uint32_t>(pkt_hdr.size()));
    ppm0.insert(ppm0.end(), pkt_hdr.begin(), pkt_hdr.end());
    p.main_markers.push_back(MarkerSegment{J2K_MS_PPM, ppm0});
    p.tile_parts[0].tph_markers.push_back(MarkerSegment{J2K_MS_PPT, {0, 0}});
    EXPECT_EQ(DecodeBytes(BuildCodestream(p)), nullptr);
  }

  // 3. Valid PPT (single Zppt=0 & non-contiguous split Zppt=0, Zppt=2) +
  //    errors + repeated get_decoded_tile hitting ppt_buffer != NULL.
  {
    auto p_ppt = *parsed_base;
    p_ppt.tile_parts[0].sod_data = pkt_body;
    std::vector<uint8_t> t0 = {0, pkt_hdr[0]};
    std::vector<uint8_t> t2 = {2};
    t2.insert(t2.end(), pkt_hdr.begin() + 1, pkt_hdr.end());
    p_ppt.tile_parts[0].tph_markers.push_back(MarkerSegment{J2K_MS_PPT, t0});
    p_ppt.tile_parts[0].tph_markers.push_back(MarkerSegment{J2K_MS_PPT, t2});
    std::vector<uint8_t> ppt_bytes = BuildCodestream(p_ppt);
    ExpectImagePixelsEqual(expected, DecodeBytes(ppt_bytes).get());

    // Calling opj_get_decoded_tile twice on PPT tile 0 hits ppt_buffer != NULL.
    OpjCodecPtr dec(opj_create_decompress(OPJ_CODEC_J2K));
    opj_dparameters_t dp;
    opj_set_default_decoder_parameters(&dp);
    ASSERT_EQ(opj_setup_decoder(dec.get(), &dp), OPJ_TRUE);
    MemReadStream mem_in(ppt_bytes);
    OpjStreamPtr stream = mem_in.CreateStream();
    opj_image_t* raw = nullptr;
    ASSERT_EQ(opj_read_header(stream.get(), dec.get(), &raw), OPJ_TRUE);
    OpjImagePtr out_img(raw);
    ASSERT_EQ(opj_get_decoded_tile(dec.get(), stream.get(), out_img.get(), 0),
              OPJ_TRUE);
    EXPECT_EQ(opj_get_decoded_tile(dec.get(), stream.get(), out_img.get(), 0),
              OPJ_FALSE);
  }
  {
    auto p = *parsed_base;
    p.tile_parts[0].tph_markers.push_back(MarkerSegment{J2K_MS_PPT, {0}});
    EXPECT_EQ(DecodeBytes(BuildCodestream(p)), nullptr);
    p.tile_parts[0].tph_markers.back() = MarkerSegment{J2K_MS_PPT, {0, 1}};
    p.tile_parts[0].tph_markers.push_back(MarkerSegment{J2K_MS_PPT, {0, 2}});
    EXPECT_EQ(DecodeBytes(BuildCodestream(p)), nullptr);
  }
}

// ============================================================================
// Group 11: StreamBuilder SIZ / COD / QCD / SOT / SOD / Issue-254 / Corrupted
// ============================================================================

TEST(J2kTest, StreamBuilderCoreHeaderAndTilePartErrors) {
  auto base_bytes = MakeMinimalValidJ2k(16, 16, 3, 0);
  ASSERT_FALSE(base_bytes.empty());

  // 1. Corrupt SOC, short stream, bad marker id, missing SIZ / COD / QCD.
  EXPECT_EQ(DecodeBytes(std::array<uint8_t, 1>{0xFF}), nullptr);
  {
    auto bad_soc = base_bytes;
    bad_soc[0] = 0x00;
    EXPECT_EQ(DecodeBytes(bad_soc), nullptr);
  }
  {
    auto parsed = ParseCodestream(base_bytes);
    ASSERT_TRUE(parsed.has_value());
    for (uint16_t required : {J2K_MS_SIZ, J2K_MS_COD, J2K_MS_QCD}) {
      auto p = *parsed;
      p.EraseMainMarkers(required);
      EXPECT_EQ(DecodeBytes(BuildCodestream(p)), nullptr);
    }
    // Marker length < 2 & non-0xFF marker ID in MH.
    std::vector<uint8_t> bad_len = {0xFF, 0x4F, 0xFF, 0x64, 0x00, 0x01};
    EXPECT_EQ(DecodeBytes(bad_len), nullptr);
    std::vector<uint8_t> bad_id = {0xFF, 0x4F, 0x12, 0x34, 0x00, 0x02};
    EXPECT_EQ(DecodeBytes(bad_id), nullptr);
  }

  // 2. SIZ error variants & check_tile_allocs guard.
  constexpr size_t kSizXsizOff = 2;
  constexpr size_t kSizYsizOff = 6;
  constexpr size_t kSizXOsizOff = 10;
  constexpr size_t kSizXTsizOff = 18;
  constexpr size_t kSizXTOsizOff = 26;
  constexpr size_t kSizCsizOff = 34;
  constexpr size_t kSizComp0Off = 36;
  auto test_siz = [&](auto mutator) -> OpjImagePtr {
    auto parsed = ParseCodestream(base_bytes);
    if (!parsed.has_value()) {
      ADD_FAILURE() << "ParseCodestream failed";
      return nullptr;
    }
    MarkerSegment* siz = parsed->FindMainMarker(J2K_MS_SIZ);
    if (siz == nullptr) {
      ADD_FAILURE() << "SIZ marker not found";
      return nullptr;
    }
    mutator(siz->payload);
    return DecodeBytes(BuildCodestream(*parsed));
  };
  EXPECT_EQ(test_siz([](auto& p) { p.resize(20); }), nullptr);
  EXPECT_EQ(test_siz([](auto& p) { p.push_back(0); }), nullptr);
  // Csiz = 16385 (>= 16385 illegal).
  EXPECT_EQ(test_siz([=](auto& p) {
              p[kSizCsizOff] = 0x40;
              p[kSizCsizOff + 1] = 0x01;
            }),
            nullptr);
  // Csiz mismatch with remaining bytes (set Csiz = 2 while keeping 9 bytes).
  EXPECT_EQ(test_siz([=](auto& p) { p[kSizCsizOff + 1] = 2; }), nullptr);
  // Negative / zero image size (x0 = 16 == x1 = 16).
  EXPECT_EQ(test_siz([=](auto& p) { p[kSizXOsizOff + 3] = 16; }), nullptr);
  // tdx == 0.
  EXPECT_EQ(test_siz([=](auto& p) {
              p[kSizXTsizOff] = p[kSizXTsizOff + 1] = p[kSizXTsizOff + 2] =
                  p[kSizXTsizOff + 3] = 0;
            }),
            nullptr);
  // Illegal tile offset (tx0 = 8 > x0 = 0).
  EXPECT_EQ(test_siz([=](auto& p) { p[kSizXTOsizOff + 3] = 8; }), nullptr);
  // comp 0 dx = 0 invalid, prec = 32 > 31 invalid.
  EXPECT_EQ(test_siz([=](auto& p) { p[kSizComp0Off + 1] = 0; }), nullptr);
  EXPECT_EQ(test_siz([=](auto& p) { p[kSizComp0Off] = 31; }), nullptr);
  // Comp 1 different prec warning in SIZ (prec=10 vs comp 0 prec=8).
  {
    auto parsed = ParseCodestream(base_bytes);
    ASSERT_TRUE(parsed.has_value());
    MarkerSegment* siz = parsed->FindMainMarker(J2K_MS_SIZ);
    ASSERT_NE(siz, nullptr);
    siz->payload[kSizComp0Off + 3] = 9;
    const std::vector<uint8_t> modified = BuildCodestream(*parsed);
    OpjCodecPtr codec(opj_create_decompress(OPJ_CODEC_J2K));
    ASSERT_NE(codec, nullptr);
    EventCapture ev;
    ev.AttachTo(codec.get());
    opj_dparameters_t dparams;
    opj_set_default_decoder_parameters(&dparams);
    ASSERT_EQ(OPJ_TRUE, opj_setup_decoder(codec.get(), &dparams));
    opj_j2k_t* j2k = ExtractJ2kHandle(codec.get());
    ASSERT_NE(j2k, nullptr);
    j2k->m_cp.allow_different_bit_depth_sign = 0;
    MemReadStream mem_in(modified);
    OpjStreamPtr stream = mem_in.CreateStream();
    ASSERT_NE(stream, nullptr);
    opj_image_t* raw_image = nullptr;
    EXPECT_EQ(OPJ_TRUE, opj_read_header(stream.get(), codec.get(), &raw_image));
    opj_image_destroy(raw_image);
    EXPECT_THAT(ev.warnings, Contains(HasSubstr("Despite JP2 BPC!=255")));
  }
  // Too many tiles (xsiz=65536, tdx=1, ysiz=2, tdy=1 -> tw*th > 65535).
  EXPECT_EQ(test_siz([=](auto& p) {
              p[kSizXsizOff] = 0;
              p[kSizXsizOff + 1] = 1;
              p[kSizXsizOff + 2] = 0;
              p[kSizXsizOff + 3] = 0;
              p[kSizXTsizOff + 3] = 1;
              p[kSizXTsizOff + 7] = 1;
            }),
            nullptr);
  // check_tile_allocs.patch guard: 650 x 100 tiles (= 65000 <= 65535) * 20
  // comps triggers numcomps > INT_MAX / (8 * sizeof(opj_tccp_t) * l_nb_tiles)
  // without allocating huge RAM.
  static_assert(sizeof(opj_tccp_t) >= 256);
  EXPECT_EQ(test_siz([=](auto& p) {
              p[kSizXsizOff] = 0;
              p[kSizXsizOff + 1] = 0;
              p[kSizXsizOff + 2] = 0x02;
              p[kSizXsizOff + 3] = 0x8A;  // Xsiz = 650
              p[kSizYsizOff] = 0;
              p[kSizYsizOff + 1] = 0;
              p[kSizYsizOff + 2] = 0;
              p[kSizYsizOff + 3] = 100;  // Ysiz = 100
              p[kSizXTsizOff + 3] = 1;
              p[kSizXTsizOff + 7] = 1;
              p[kSizCsizOff + 1] = 20;
              p.resize(kSizComp0Off + 20 * 3);
              for (int c = 0; c < 20; ++c) {
                p[kSizComp0Off + c * 3] = 7;
                p[kSizComp0Off + c * 3 + 1] = 1;
                p[kSizComp0Off + c * 3 + 2] = 1;
              }
            }),
            nullptr);

  // 3. COD / SPCod / QCD / SQcd / TLM error variants.
  auto test_cod = [&](auto mutator) -> OpjImagePtr {
    auto parsed = ParseCodestream(base_bytes);
    if (!parsed.has_value()) {
      ADD_FAILURE() << "ParseCodestream failed";
      return nullptr;
    }
    MarkerSegment* cod = parsed->FindMainMarker(J2K_MS_COD);
    if (cod == nullptr) {
      ADD_FAILURE() << "COD marker not found";
      return nullptr;
    }
    mutator(cod->payload);
    return DecodeBytes(BuildCodestream(*parsed));
  };
  EXPECT_EQ(test_cod([](auto& p) { p.resize(4); }), nullptr);
  EXPECT_EQ(test_cod([](auto& p) { p[0] = 0x80; }), nullptr);
  EXPECT_EQ(test_cod([](auto& p) { p[1] = 9; }), nullptr);  // bad prg
  EXPECT_EQ(test_cod([](auto& p) {
              p[2] = 0;
              p[3] = 0;
            }),
            nullptr);                                        // numlayers=0
  EXPECT_EQ(test_cod([](auto& p) { p[5] = 33; }), nullptr);  // numres > 33
  EXPECT_EQ(test_cod([](auto& p) { p[6] = 9; }), nullptr);   // cblkw > 10
  EXPECT_EQ(test_cod([](auto& p) {
              p[6] = 5;
              p[7] = 5;
            }),
            nullptr);  // cblkw+cblkh = 14 > 12
  EXPECT_EQ(test_cod([](auto& p) { p[8] |= J2K_CCP_CBLKSTY_HTMIXED; }),
            nullptr);
  EXPECT_EQ(test_cod([](auto& p) { p[9] = 2; }), nullptr);  // qmfbid > 1
  EXPECT_EQ(test_cod([](auto& p) {
              p[0] = J2K_CP_CSTY_PRT;
              p.push_back(0xFF);
              p.push_back(0x0F);  // precinct exp 0 at level > 0
              p.push_back(0xFF);
            }),
            nullptr);
  EXPECT_EQ(test_cod([](auto& p) { p.push_back(0); }), nullptr);

  // QCD SIQNT (scalar derived) decode path + QCC invalid length/comp + TLM
  // ST=3/size mismatch/tile_index OOB warnings.
  {
    OpjImagePtr img97 = CreateTestImage(16, 16, 1);
    opj_cparameters_t cp97;
    opj_set_default_encoder_parameters(&cp97);
    cp97.numresolution = 2;
    cp97.irreversible = 1;
    auto b97 = EncodeCustom(img97.get(), &cp97);
    ASSERT_TRUE(b97.has_value());
    auto p97 = ParseCodestream(*b97);
    ASSERT_TRUE(p97.has_value());
    MarkerSegment* qcd = p97->FindMainMarker(J2K_MS_QCD);
    ASSERT_NE(qcd, nullptr);
    qcd->payload[0] =
        static_cast<uint8_t>((qcd->payload[0] & 0xE0U) | J2K_CCP_QNTSTY_SIQNT);
    qcd->payload.resize(3);
    EXPECT_NE(DecodeBytes(BuildCodestream(*p97)), nullptr);

    // Many subbands > OPJ_J2K_MAXBANDS warning in SQcd.
    qcd->payload = std::vector<uint8_t>(1 + 2 * (OPJ_J2K_MAXBANDS + 2), 0x40);
    qcd->payload[0] = static_cast<uint8_t>((2U << 5) | J2K_CCP_QNTSTY_SEQNT);
    EventCapture warn_ev;
    EXPECT_NE(DecodeBytes(BuildCodestream(*p97), nullptr, &warn_ev), nullptr);
    EXPECT_THAT(warn_ev.warnings,
                Contains(HasSubstr("greater to OPJ_J2K_MAXBANDS")));
  }
  for (auto bad_tlm : {
           std::vector<uint8_t>{0, 0x30},     // ST=3 warning
           std::vector<uint8_t>{0, 0x50, 1},  // size mismatch warning
           std::vector<uint8_t>{0, 0x50, 9, 0, 0, 0, 14},  // tile OOB warning
       }) {
    auto p = ParseCodestream(base_bytes);
    ASSERT_TRUE(p.has_value());
    p->main_markers.push_back(MarkerSegment{J2K_MS_TLM, std::move(bad_tlm)});
    EventCapture ev;
    EXPECT_NE(DecodeBytes(BuildCodestream(*p), nullptr, &ev), nullptr);
    EXPECT_THAT(ev.warnings, Contains(HasSubstr("opj_j2k_read_tlm():")));
  }
  {
    auto p = ParseCodestream(base_bytes);
    ASSERT_TRUE(p.has_value());
    p->main_markers.push_back(MarkerSegment{J2K_MS_TLM, {0}});  // < 2 error
    EXPECT_EQ(DecodeBytes(BuildCodestream(*p)), nullptr);
  }

  // 4. SOT / SOD / Psot=0 / Psot=12 / Issue 254 (TPsot==TNsot) / SPOT6.
  {
    // Psot == 0 valid decode of last tile-part.
    auto p = ParseCodestream(base_bytes);
    ASSERT_TRUE(p.has_value());
    p->tile_parts[0].force_psot = true;
    p->tile_parts[0].forced_psot = 0;
    EXPECT_NE(DecodeBytes(BuildCodestream(*p)), nullptr);

    // Psot == 12 (empty SOT warning) + SOD hits l_sot_length_pb_detected and
    // !l_tcp->m_data cleanly on decode.
    p->tile_parts[0].forced_psot = 12;
    p->tile_parts[0].sod_data.clear();
    p->tile_parts[0].emit_sod = true;
    EventCapture empty_sot_ev;
    EXPECT_EQ(DecodeBytes(BuildCodestream(*p), nullptr, &empty_sot_ev),
              nullptr);
    EXPECT_THAT(empty_sot_ev.warnings,
                Contains(HasSubstr("Empty SOT marker detected")));

    // TPsot(1) >= TNsot(1) after initial TNsot=0 tile-part.
    {
      auto p_tpsot = ParseCodestream(base_bytes);
      ASSERT_TRUE(p_tpsot.has_value());
      p_tpsot->tile_parts[0].tnsot = 0;
      TilePartSegment second = p_tpsot->tile_parts[0];
      second.tpsot = 1;
      second.tnsot = 1;
      p_tpsot->tile_parts.push_back(std::move(second));
      EXPECT_EQ(DecodeBytes(BuildCodestream(*p_tpsot)), nullptr);
    }

    // Invalid Psot = 5 (< 14 and != 12), Isot OOB, TPsot out-of-order, and
    // SOT_length < marker_size + 2 inside TPH.
    struct BadSotCase {
      uint16_t isot = 0;
      uint8_t tpsot = 0;
      uint8_t tnsot = 1;
      uint32_t psot = 0;
      bool has_com_tph = false;
    };
    constexpr BadSotCase kBadSotCases[] = {
        {.psot = 5},
        {.isot = 5},
        {.tpsot = 1, .tnsot = 2},
        {.tnsot = 0, .psot = 14, .has_com_tph = true},
    };
    for (const BadSotCase& tc : kBadSotCases) {
      auto p_bad = ParseCodestream(base_bytes);
      ASSERT_TRUE(p_bad.has_value());
      p_bad->tile_parts[0].isot = tc.isot;
      p_bad->tile_parts[0].tpsot = tc.tpsot;
      p_bad->tile_parts[0].tnsot = tc.tnsot;
      if (tc.psot != 0) {
        p_bad->tile_parts[0].force_psot = true;
        p_bad->tile_parts[0].forced_psot = tc.psot;
      }
      if (tc.has_com_tph) {
        p_bad->tile_parts[0].tph_markers.push_back(
            MarkerSegment{J2K_MS_COM, {0, 1, 'a', 'b', 'c'}});
      }
      EXPECT_EQ(DecodeBytes(BuildCodestream(*p_bad)), nullptr);
    }
  }

  // Issue 254 (TPsot == TNsot interleaved multi-tile-part stream):
  // 2 tiles, each with 3 layer tile-parts interleaved as
  // T0(tp0), T1(tp0), T0(tp1), T1(tp1), T0(tp2), T1(tp2), patched TNsot=2!
  {
    OpjImagePtr img = CreateTestImage(16, 8, 1);
    ASSERT_NE(img, nullptr);
    const auto expected = SnapshotImagePixels(img.get());
    opj_cparameters_t cp = MakeLosslessParams(/*numresolution=*/2,
                                              TileGeometry{.tdx = 8, .tdy = 8});
    cp.prog_order = OPJ_LRCP;
    cp.tp_on = 1;
    cp.tp_flag = 'L';
    cp.tcp_numlayers = 3;
    cp.tcp_rates[0] = 20.0f;
    cp.tcp_rates[1] = 10.0f;
    cp.tcp_rates[2] = 0.0f;
    auto two_tile = EncodeCustom(img.get(), &cp);
    ASSERT_TRUE(two_tile.has_value());
    auto p = ParseCodestream(*two_tile);
    ASSERT_TRUE(p.has_value());
    ASSERT_EQ(p->tile_parts.size(), 6U);

    ParsedJ2kStream issue254 = *p;
    // Order: T0(0), T1(0), T0(1), T1(1), T0(2), T1(2).
    issue254.tile_parts = {p->tile_parts[0], p->tile_parts[3],
                           p->tile_parts[1], p->tile_parts[4],
                           p->tile_parts[2], p->tile_parts[5]};
    for (auto& tp : issue254.tile_parts) {
      tp.tnsot = 2;  // Actual count is 3 -> TPsot(2) == TNsot(2)
    }
    EventCapture events;
    OpjImagePtr dec =
        DecodeBytes(BuildCodestream(issue254), nullptr, &events, false);
    ExpectImagePixelsEqual(expected, dec.get());
    EXPECT_THAT(events.warnings,
                Contains(HasSubstr("Non conformant codestream TPsot==TNsot")));

    // Non-strict missing EOC on 2-tile stream where last tile has TNsot = 0.
    cp.tp_on = 0;
    cp.tcp_numlayers = 1;
    cp.tcp_rates[0] = 0;
    OpjImagePtr img2 = CreateTestImage(16, 8, 1);
    auto two_tile_1tp = EncodeCustom(img2.get(), &cp);
    ASSERT_TRUE(two_tile_1tp.has_value());
    auto p2 = ParseCodestream(*two_tile_1tp);
    ASSERT_TRUE(p2.has_value());
    ASSERT_EQ(p2->tile_parts.size(), 2U);
    p2->tile_parts[1].tnsot = 0;
    p2->has_eoc = false;
    EXPECT_NE(DecodeBytes(BuildCodestream(*p2), nullptr, nullptr, false),
              nullptr);
  }
}

// Exercises opj_j2k_get_imf_max_NL, opj_j2k_set_imf_parameters, and
// opj_j2k_is_imf_compliant across all 2K/4K/8K regular and R profiles without
// allocating large pixel buffers (uses opj_image_tile_create + setup_encoder).
TEST(J2kTest, ImfProfileSetupAndMaxSublevelValidation) {
  auto run_setup = [](OPJ_UINT16 rsiz, OPJ_UINT32 img_w, OPJ_UINT32 img_h,
                      bool tile_on, OPJ_UINT32 tdx, OPJ_UINT32 tdy,
                      int numresolution, EventCapture* events,
                      bool bad_prt_1res = false) {
    OpjImagePtr img(opj_image_create0());
    std::vector<opj_image_cmptparm_t> cmpts(3);
    for (auto& c : cmpts) {
      c = opj_image_cmptparm_t{.dx = 1,
                               .dy = 1,
                               .w = img_w,
                               .h = img_h,
                               .x0 = 0,
                               .y0 = 0,
                               .prec = 8,
                               .sgnd = 0};
    }
    img.reset(opj_image_tile_create(3, cmpts.data(), OPJ_CLRSPC_SRGB));
    EXPECT_NE(img, nullptr);
    img->x0 = 0;
    img->y0 = 0;
    img->x1 = img_w;
    img->y1 = img_h;

    opj_cparameters_t cp;
    opj_set_default_encoder_parameters(&cp);
    cp.rsiz = rsiz;
    cp.numresolution = numresolution;
    if (tile_on) {
      cp.tile_size_on = OPJ_TRUE;
      cp.cp_tdx = static_cast<int>(tdx);
      cp.cp_tdy = static_cast<int>(tdy);
    }
    if (bad_prt_1res) {
      cp.csty = J2K_CP_CSTY_PRT;
      cp.res_spec = 1;
      cp.prcw_init[0] = 64;
      cp.prch_init[0] = 64;
    }

    OpjJ2kPtr j2k(opj_j2k_create_compress());
    EXPECT_NE(j2k, nullptr);
    opj_event_mgr_t mgr = MakeEventManager(events);
    EXPECT_EQ(opj_j2k_setup_encoder(j2k.get(), &cp, img.get(), &mgr), OPJ_TRUE);
    return cp;
  };

  // 1. opj_j2k_get_imf_max_NL tile-size branches (2K_R / 4K_R / 8K_R).
  struct MaxNlCase {
    uint16_t rsiz;
    uint32_t w;
    uint32_t h;
    uint32_t tdx;
    uint32_t tdy;
    int expected_numres;
  };
  constexpr MaxNlCase kMaxNlCases[] = {
      {.rsiz = ImfRsiz(OPJ_PROFILE_IMF_2K_R, 1, 1),
       .w = 2048,
       .h = 1080,
       .tdx = 1024,
       .tdy = 1024,
       .expected_numres = 5},
      {.rsiz = ImfRsiz(OPJ_PROFILE_IMF_2K_R, 1, 1),
       .w = 2048,
       .h = 1080,
       .tdx = 2048,
       .tdy = 2048,
       .expected_numres = 6},
      {.rsiz = ImfRsiz(OPJ_PROFILE_IMF_4K_R, 1, 1),
       .w = 4096,
       .h = 2160,
       .tdx = 1024,
       .tdy = 1024,
       .expected_numres = 5},
      {.rsiz = ImfRsiz(OPJ_PROFILE_IMF_4K_R, 1, 1),
       .w = 4096,
       .h = 2160,
       .tdx = 2048,
       .tdy = 2048,
       .expected_numres = 6},
      {.rsiz = ImfRsiz(OPJ_PROFILE_IMF_4K_R, 1, 1),
       .w = 4096,
       .h = 2160,
       .tdx = 4096,
       .tdy = 4096,
       .expected_numres = 6},
      {.rsiz = ImfRsiz(OPJ_PROFILE_IMF_8K_R, 1, 1),
       .w = 8192,
       .h = 4320,
       .tdx = 1024,
       .tdy = 1024,
       .expected_numres = 5},
      {.rsiz = ImfRsiz(OPJ_PROFILE_IMF_8K_R, 1, 1),
       .w = 8192,
       .h = 4320,
       .tdx = 2048,
       .tdy = 2048,
       .expected_numres = 6},
      {.rsiz = ImfRsiz(OPJ_PROFILE_IMF_8K_R, 1, 1),
       .w = 8192,
       .h = 4320,
       .tdx = 4096,
       .tdy = 4096,
       .expected_numres = 6},
      {.rsiz = ImfRsiz(OPJ_PROFILE_IMF_8K_R, 1, 1),
       .w = 8192,
       .h = 4320,
       .tdx = 8192,
       .tdy = 8192,
       .expected_numres = 6},
  };
  for (const auto& tc : kMaxNlCases) {
    EventCapture ev;
    opj_cparameters_t out =
        run_setup(tc.rsiz, tc.w, tc.h, true, tc.tdx, tc.tdy,
                  OPJ_COMP_PARAM_DEFAULT_NUMRESOLUTION, &ev);
    EXPECT_EQ(out.numresolution, tc.expected_numres);
  }

  // 2. y1 shrink loop & 1-resolution default/invalid precinct checks.
  {
    EventCapture ev;
    opj_cparameters_t out =
        run_setup(ImfRsiz(OPJ_PROFILE_IMF_2K, 1, 1), 32, 2, false, 0, 0,
                  OPJ_COMP_PARAM_DEFAULT_NUMRESOLUTION, &ev);
    EXPECT_EQ(out.numresolution, 2);

    EventCapture ev_1res_ok;
    (void)run_setup(ImfRsiz(OPJ_PROFILE_IMF_2K, 1, 1), 64, 64, false, 0, 0, 1,
                    &ev_1res_ok);
    EXPECT_THAT(ev_1res_ok.warnings,
                Contains(HasSubstr("IMF 2K profile requires 1 <= NL <= 5")));

    EventCapture ev_1res_bad_prt;
    opj_cparameters_t out_bad =
        run_setup(ImfRsiz(OPJ_PROFILE_IMF_2K, 1, 1), 64, 64, false, 0, 0, 1,
                  &ev_1res_bad_prt, true);
    EXPECT_EQ(out_bad.rsiz, OPJ_PROFILE_NONE);
    EXPECT_THAT(ev_1res_bad_prt.warnings,
                Contains(HasSubstr("Non-IMF codestream will be generated")));
  }

  // 3. Explicit out-of-range NL and tile dimensions for 4K/8K/2K_R/4K_R/8K_R.
  struct BadNlCase {
    uint16_t rsiz;
    uint32_t w;
    uint32_t h;
    bool tile_on;
    uint32_t tdx;
    uint32_t tdy;
    int numres;
  };
  constexpr BadNlCase kBadCases[] = {
      {.rsiz = ImfRsiz(OPJ_PROFILE_IMF_2K, 1, 1),
       .w = 128,
       .h = 128,
       .tile_on = true,
       .tdx = 64,
       .tdy = 64,
       .numres = 3},
      {.rsiz = ImfRsiz(OPJ_PROFILE_IMF_4K, 1, 1),
       .w = 256,
       .h = 256,
       .tile_on = false,
       .tdx = 0,
       .tdy = 0,
       .numres = 8},
      {.rsiz = ImfRsiz(OPJ_PROFILE_IMF_8K, 1, 1),
       .w = 512,
       .h = 512,
       .tile_on = false,
       .tdx = 0,
       .tdy = 0,
       .numres = 9},
      {.rsiz = ImfRsiz(OPJ_PROFILE_IMF_2K_R, 1, 1),
       .w = 2048,
       .h = 1080,
       .tile_on = true,
       .tdx = 2048,
       .tdy = 2048,
       .numres = 7},
      {.rsiz = ImfRsiz(OPJ_PROFILE_IMF_2K_R, 1, 1),
       .w = 2048,
       .h = 1080,
       .tile_on = true,
       .tdx = 1024,
       .tdy = 1024,
       .numres = 7},
      {.rsiz = ImfRsiz(OPJ_PROFILE_IMF_4K_R, 1, 1),
       .w = 4096,
       .h = 2160,
       .tile_on = true,
       .tdx = 4096,
       .tdy = 4096,
       .numres = 8},
      {.rsiz = ImfRsiz(OPJ_PROFILE_IMF_4K_R, 1, 1),
       .w = 4096,
       .h = 2160,
       .tile_on = true,
       .tdx = 2048,
       .tdy = 2048,
       .numres = 7},
      {.rsiz = ImfRsiz(OPJ_PROFILE_IMF_4K_R, 1, 1),
       .w = 4096,
       .h = 2160,
       .tile_on = true,
       .tdx = 1024,
       .tdy = 1024,
       .numres = 7},
      {.rsiz = ImfRsiz(OPJ_PROFILE_IMF_8K_R, 1, 1),
       .w = 8192,
       .h = 4320,
       .tile_on = true,
       .tdx = 8192,
       .tdy = 8192,
       .numres = 9},
      {.rsiz = ImfRsiz(OPJ_PROFILE_IMF_8K_R, 1, 1),
       .w = 8192,
       .h = 4320,
       .tile_on = true,
       .tdx = 4096,
       .tdy = 4096,
       .numres = 8},
      {.rsiz = ImfRsiz(OPJ_PROFILE_IMF_8K_R, 1, 1),
       .w = 8192,
       .h = 4320,
       .tile_on = true,
       .tdx = 2048,
       .tdy = 2048,
       .numres = 7},
      {.rsiz = ImfRsiz(OPJ_PROFILE_IMF_8K_R, 1, 1),
       .w = 8192,
       .h = 4320,
       .tile_on = true,
       .tdx = 1024,
       .tdy = 1024,
       .numres = 7},
      {.rsiz = ImfRsiz(OPJ_PROFILE_IMF_8K_R, 1, 1),
       .w = 512,
       .h = 512,
       .tile_on = true,
       .tdx = 512,
       .tdy = 512,
       .numres = 4},
  };
  for (const auto& tc : kBadCases) {
    EventCapture ev;
    opj_cparameters_t out = run_setup(tc.rsiz, tc.w, tc.h, tc.tile_on, tc.tdx,
                                      tc.tdy, tc.numres, &ev);
    if (tc.tdx != 512) {
      EXPECT_EQ(out.rsiz, OPJ_PROFILE_NONE);
      EXPECT_THAT(ev.warnings,
                  Contains(HasSubstr("Non-IMF codestream will be generated")));
    } else {
      EXPECT_NE(out.rsiz, OPJ_PROFILE_NONE);
    }
  }
}

// Covers all mismatch comparisons in opj_j2k_compare_coc and singular MCT
// matrix inversion + mct_validation failure branches.
TEST(J2kTest, ComponentCodingComparisonAndMctEdgeCases) {
  // 1. opj_j2k_compare_coc: differing numresolutions, cblkw, cblkh, qmfbid,
  //    csty, prcw, prch across components 1..7 trigger COC emission.
  {
    OpjImagePtr img = CreateTestImage(16, 16, 8);
    ASSERT_NE(img, nullptr);
    OpjCodecPtr codec(opj_create_compress(OPJ_CODEC_J2K));
    ASSERT_NE(codec, nullptr);
    opj_cparameters_t cp;
    opj_set_default_encoder_parameters(&cp);
    cp.numresolution = 2;
    cp.csty = J2K_CP_CSTY_PRT;
    cp.res_spec = 1;
    cp.prcw_init[0] = 8;
    cp.prch_init[0] = 8;
    ASSERT_EQ(opj_setup_encoder(codec.get(), &cp, img.get()), OPJ_TRUE);
    opj_j2k_t* j2k = ExtractJ2kHandle(codec.get());
    ASSERT_NE(j2k, nullptr);
    auto* tccps = j2k->m_cp.tcps[0].tccps;
    tccps[1].numresolutions = 1;
    tccps[2].cblkw = 3;
    tccps[3].cblkh = 3;
    // Flipping qmfbid on comp 4 without adjusting stepsizes is valid here
    // because only main headers are emitted (start_compress + end_compress).
    tccps[4].qmfbid = 0;
    tccps[5].csty ^= J2K_CCP_CSTY_PRT;
    tccps[6].prcw[1] = 2;
    tccps[7].prch[1] = 2;

    MemWriteStream mem_out;
    OpjStreamPtr stream = mem_out.CreateStream();
    ASSERT_EQ(opj_start_compress(codec.get(), img.get(), stream.get()),
              OPJ_TRUE);
    ASSERT_EQ(opj_end_compress(codec.get(), stream.get()), OPJ_TRUE);
    stream.reset();

    auto parsed = ParseCodestream(mem_out.data);
    ASSERT_TRUE(parsed.has_value());
    int coc_count = 0;
    for (const auto& m : parsed->main_markers) {
      if (m.id == J2K_MS_COC) {
        ++coc_count;
      }
    }
    EXPECT_EQ(coc_count, 7);
  }

  // 2. Singular MCT matrix fails opj_matrix_inversion_f inside setup_encoder.
  {
    OpjImagePtr img = CreateTestImage(8, 8, 3);
    ASSERT_NE(img, nullptr);
    opj_cparameters_t cp;
    opj_set_default_encoder_parameters(&cp);
    absl::Cleanup free_mct = [&cp] {
      std::free(cp.mct_data);
      cp.mct_data = nullptr;
    };
    cp.irreversible = 1;
    std::array<OPJ_FLOAT32, 9> zero_matrix = {};
    std::array<OPJ_INT32, 3> offsets = {0, 0, 0};
    ASSERT_EQ(opj_set_MCT(&cp, zero_matrix.data(), offsets.data(), 3),
              OPJ_TRUE);

    OpjJ2kPtr j2k(opj_j2k_create_compress());
    ASSERT_NE(j2k, nullptr);
    EventCapture ev;
    opj_event_mgr_t mgr = MakeEventManager(&ev);
    EXPECT_EQ(opj_j2k_setup_encoder(j2k.get(), &cp, img.get(), &mgr),
              OPJ_FALSE);
    EXPECT_THAT(
        ev.errors,
        Contains(HasSubstr("Failed to inverse encoder MCT decoding matrix")));
  }

  // 3. Part-2 Rsiz bitmask (0x8200) exercises opj_j2k_mct_validation on decode.
  {
    auto base = MakeMinimalValidJ2k(16, 16, 3, 8);
    auto parsed = ParseCodestream(base);
    ASSERT_TRUE(parsed.has_value());
    MarkerSegment* siz = parsed->FindMainMarker(J2K_MS_SIZ);
    ASSERT_NE(siz, nullptr);
    siz->payload[0] = 0x82;
    siz->payload[1] = 0x00;
    EXPECT_NE(DecodeBytes(BuildCodestream(*parsed)), nullptr);
  }
}

// Covers signed/unsigned 8/16/24-bit paths in opj_j2k_get_tile_data,
// multi-byte PLT varints, and partial-window clipping in update_image_data.
TEST(J2kTest, MultiTileSampleConversionsAndClipping) {
  // 1. Multi-tile (2x2 tiles -> l_reuse_data == OPJ_FALSE) for prec=8 sgnd=1,
  //    prec=12 sgnd=0, prec=12 sgnd=1, and prec=24 sgnd=0.
  struct PrecSgndCase {
    uint32_t prec;
    uint32_t sgnd;
  };
  constexpr PrecSgndCase kPrecSgndCases[] = {
      {.prec = 8, .sgnd = 1},
      {.prec = 12, .sgnd = 0},
      {.prec = 12, .sgnd = 1},
      {.prec = 24, .sgnd = 0},
  };
  for (const auto& tc : kPrecSgndCases) {
    OpjImagePtr img =
        CreateTestImage(16, 16, 1, tc.prec, tc.sgnd, OPJ_CLRSPC_GRAY);
    ASSERT_NE(img, nullptr);
    if (tc.prec == 8 && tc.sgnd == 1) {
      // google3 compiles with -funsigned-char and opj_j2k_get_tile_data /
      // opj_tcd_copy_tile_data cast 8-bit signed samples through OPJ_CHAR
      // (char), so clamp this round-trip input to [0,127].
      const size_t n = static_cast<size_t>(img->comps[0].w) * img->comps[0].h;
      for (size_t k = 0; k < n; ++k) {
        img->comps[0].data[k] = std::abs(img->comps[0].data[k]) & 0x7F;
      }
    }
    const auto expected = SnapshotImagePixels(img.get());
    opj_cparameters_t cp = MakeLosslessParams(/*numresolution=*/2,
                                              TileGeometry{.tdx = 8, .tdy = 8});
    auto bytes = EncodeCustom(img.get(), &cp);
    ASSERT_TRUE(bytes.has_value()) << "prec=" << tc.prec << " sgnd=" << tc.sgnd;
    ExpectImagePixelsEqual(expected, DecodeBytes(*bytes).get());
  }

#if CHAR_MIN == 0
  // Pin -funsigned-char behavior in opj_j2k_get_tile_data /
  // opj_tcd_copy_tile_data / opj_tcd_dc_level_shift_decode: when OPJ_CHAR is
  // unsigned, encoding signed 8-bit sample -1 stores (OPJ_CHAR)(-1) -> 255,
  // which decodes clamped to the signed 8-bit maximum (127) in both
  // opj_decode and opj_decode_tile_data rather than preserving -1.
  {
    OpjImagePtr img = CreateTestImage(16, 16, 1, 8, 1, OPJ_CLRSPC_GRAY);
    ASSERT_NE(img, nullptr);
    const size_t n = static_cast<size_t>(img->comps[0].w) * img->comps[0].h;
    for (size_t k = 0; k < n; ++k) {
      img->comps[0].data[k] = std::abs(img->comps[0].data[k]) & 0x7F;
    }
    img->comps[0].data[0] = -1;
    opj_cparameters_t cp = MakeLosslessParams(/*numresolution=*/1,
                                              TileGeometry{.tdx = 8, .tdy = 8});
    auto bytes = EncodeCustom(img.get(), &cp);
    ASSERT_TRUE(bytes.has_value());

    OpjImagePtr dec = DecompressBytesToImage(*bytes, OPJ_CODEC_J2K);
    ASSERT_NE(dec, nullptr);
    EXPECT_EQ(dec->comps[0].data[0], 127);

    std::unique_ptr<DecodeSession> session =
        OpenForDecode(*bytes, OPJ_CODEC_J2K);
    ASSERT_NE(session, nullptr);
    OPJ_UINT32 tile_idx = 0, data_size = 0, nb_comps = 0;
    OPJ_INT32 tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0;
    OPJ_BOOL go_on = OPJ_FALSE;
    ASSERT_EQ(opj_read_tile_header(session->codec.get(), session->stream.get(),
                                   &tile_idx, &data_size, &tx0, &ty0, &tx1,
                                   &ty1, &nb_comps, &go_on),
              OPJ_TRUE);
    ASSERT_EQ(go_on, OPJ_TRUE);
    std::vector<OPJ_BYTE> tile_buf(data_size, 0);
    ASSERT_EQ(
        opj_decode_tile_data(session->codec.get(), tile_idx, tile_buf.data(),
                             data_size, session->stream.get()),
        OPJ_TRUE);
    EXPECT_EQ(tile_buf[0], 127);
  }
#endif

  // 2. Larger tile with PLT=YES hits multi-byte PLT varints (packet > 127 B).
  {
    OpjImagePtr img = CreateTestImage(128, 128, 3);
    opj_cparameters_t cp = MakeLosslessParams(/*numresolution=*/4);
    auto bytes = EncodeCustom(img.get(), &cp, kPltOpts);
    ASSERT_TRUE(bytes.has_value());
    EXPECT_NE(DecodeBytes(*bytes), nullptr);
  }

  // 3. Partial tile window clipping in opj_j2k_update_image_data (strictly
  //    interior [2,2,6,6] and tile-crossing [4,4,12,12]) + TLM area subset
  //    [0,0,8,8] seek plan.
  {
    OpjImagePtr img = CreateTestImage(16, 16, 1, 8, 0, OPJ_CLRSPC_GRAY);
    const auto expected = SnapshotImagePixels(img.get());
    opj_cparameters_t cp = MakeLosslessParams(/*numresolution=*/2,
                                              TileGeometry{.tdx = 8, .tdy = 8});
    auto bytes = EncodeCustom(img.get(), &cp, kTlmOpts);
    ASSERT_TRUE(bytes.has_value());

    for (std::array<OPJ_INT32, 4> area : {
             std::array<OPJ_INT32, 4>{2, 2, 6, 6},
             std::array<OPJ_INT32, 4>{4, 4, 12, 12},
             std::array<OPJ_INT32, 4>{0, 0, 8, 8},
         }) {
      OpjImagePtr out = DecompressBytesToImage(*bytes, OPJ_CODEC_J2K,
                                               DecodeOptions{.area = area});
      ASSERT_NE(out, nullptr);
      const OPJ_UINT32 w = static_cast<OPJ_UINT32>(area[2] - area[0]);
      const OPJ_UINT32 h = static_cast<OPJ_UINT32>(area[3] - area[1]);
      ASSERT_EQ(out->comps[0].w, w);
      ASSERT_EQ(out->comps[0].h, h);
      for (OPJ_UINT32 y = 0; y < h; ++y) {
        for (OPJ_UINT32 x = 0; x < w; ++x) {
          EXPECT_EQ(
              out->comps[0].data[y * w + x],
              expected[0][static_cast<size_t>(area[1] + y) * 16 + area[0] + x]);
        }
      }
    }
  }
}

// Covers opj_get_decoded_tile TLM multi-tile-part seeking, extra-comp cleanup,
// non-zero origin clipping, opj_j2k_dump after tile header read, and
// opj_write_tile error paths.
TEST(J2kTest, ResolutionFactorTileApiAndCodecDump) {
  // 1. Interleaved 4-tile 2-layer TLM stream for opj_get_decoded_tile seeking.
  {
    OpjImagePtr img =
        CreateTestImage(16, 16, 1, 8, 0, OPJ_CLRSPC_GRAY, 1, 1, 2, 2);
    ASSERT_NE(img, nullptr);
    opj_cparameters_t cp = MakeLosslessParams(/*numresolution=*/2,
                                              TileGeometry{.tdx = 8, .tdy = 8});
    cp.prog_order = OPJ_LRCP;
    cp.tp_on = 1;
    cp.tp_flag = 'L';
    cp.tcp_numlayers = 2;
    cp.tcp_rates[0] = 20.0f;
    cp.tcp_rates[1] = 0.0f;
    auto tlm_bytes = EncodeCustom(img.get(), &cp, kTlmOpts);
    ASSERT_TRUE(tlm_bytes.has_value());

    OpjCodecPtr dec(opj_create_decompress(OPJ_CODEC_J2K));
    opj_dparameters_t dp;
    opj_set_default_decoder_parameters(&dp);
    ASSERT_EQ(opj_setup_decoder(dec.get(), &dp), OPJ_TRUE);
    MemReadStream mem_in(*tlm_bytes);
    OpjStreamPtr stream = mem_in.CreateStream();
    opj_image_t* raw = nullptr;
    ASSERT_EQ(opj_read_header(stream.get(), dec.get(), &raw), OPJ_TRUE);
    OpjImagePtr hdr(raw);

    opj_j2k_t* j2k = ExtractJ2kHandle(dec.get());
    ASSERT_NE(j2k, nullptr);
    EventCapture ev;
    opj_event_mgr_t mgr = MakeEventManager(&ev);
    // Error paths in opj_j2k_get_tile: null image, fewer comps, OOB tile.
    EXPECT_EQ(opj_j2k_get_tile(
                  j2k, reinterpret_cast<opj_stream_private_t*>(stream.get()),
                  nullptr, &mgr, 0),
              OPJ_FALSE);
    std::array<opj_image_cmptparm_t, 2> two_cmpts = {};
    for (auto& c : two_cmpts) {
      c = opj_image_cmptparm_t{.dx = 1,
                               .dy = 1,
                               .w = 8,
                               .h = 8,
                               .x0 = 0,
                               .y0 = 0,
                               .prec = 8,
                               .sgnd = 0};
    }
    OpjImagePtr tile_img(
        opj_image_tile_create(2, two_cmpts.data(), OPJ_CLRSPC_GRAY));
    ASSERT_NE(tile_img, nullptr);
    // Pre-populate comp 1 data to hit extra-comp cleanup in opj_j2k_get_tile.
    tile_img->comps[1].data =
        static_cast<OPJ_INT32*>(opj_image_data_alloc(64 * sizeof(OPJ_INT32)));
    ASSERT_NE(tile_img->comps[1].data, nullptr);

    EXPECT_EQ(opj_get_decoded_tile(dec.get(), stream.get(), tile_img.get(), 99),
              OPJ_FALSE);
    // Decode tile 2 (first tile-part + seek to second) & tile 0 (clips x0,y0).
    EXPECT_EQ(opj_get_decoded_tile(dec.get(), stream.get(), tile_img.get(), 2),
              OPJ_TRUE);
    EXPECT_EQ(tile_img->numcomps, 1U);
    EXPECT_EQ(opj_get_decoded_tile(dec.get(), stream.get(), tile_img.get(), 0),
              OPJ_TRUE);
    EXPECT_EQ(tile_img->x0, 2U);
    EXPECT_EQ(tile_img->y0, 2U);

    // Dump after tile decoding covers tile_index[].marker loop.
    TempFile dump_after("j2k_dump_after_tile.txt");
    {
      ScopedFile sf{std::fopen(dump_after.path_str().c_str(), "wb")};
      ASSERT_NE(sf.fp, nullptr);
      opj_dump_codec(dec.get(),
                     OPJ_IMG_INFO | OPJ_J2K_MH_INFO | OPJ_J2K_TH_INFO |
                         OPJ_J2K_TCH_INFO | OPJ_J2K_MH_IND | OPJ_J2K_TH_IND,
                     sf.fp);
    }
    std::ifstream dump_ifs(dump_after.path(), std::ios::binary);
    const std::string dump_text((std::istreambuf_iterator<char>(dump_ifs)),
                                std::istreambuf_iterator<char>());
    EXPECT_THAT(dump_text, HasSubstr("Codestream index"));
  }

  // 2. Fewer-components error in opj_j2k_get_tile.
  {
    auto bytes3 = MakeMinimalValidJ2k(8, 8, 3, 8);
    OpjCodecPtr dec(opj_create_decompress(OPJ_CODEC_J2K));
    opj_dparameters_t dp;
    opj_set_default_decoder_parameters(&dp);
    ASSERT_EQ(opj_setup_decoder(dec.get(), &dp), OPJ_TRUE);
    MemReadStream mem_in(bytes3);
    OpjStreamPtr stream = mem_in.CreateStream();
    opj_image_t* raw = nullptr;
    ASSERT_EQ(opj_read_header(stream.get(), dec.get(), &raw), OPJ_TRUE);
    OpjImagePtr hdr(raw);
    opj_image_cmptparm_t one_cmpt{.dx = 1,
                                  .dy = 1,
                                  .w = 8,
                                  .h = 8,
                                  .x0 = 0,
                                  .y0 = 0,
                                  .prec = 8,
                                  .sgnd = 0};
    OpjImagePtr small_img(opj_image_tile_create(1, &one_cmpt, OPJ_CLRSPC_GRAY));
    EXPECT_EQ(opj_get_decoded_tile(dec.get(), stream.get(), small_img.get(), 0),
              OPJ_FALSE);
  }

  // 3. opj_write_tile wrong tile index and wrong buffer size errors +
  //    opj_j2k_get_default_thread_count clamp above 2 * num_cpus.
  {
    ScopedEnvVar env_guard("OPJ_NUM_THREADS");
    env_guard.Set("99999");
    OpjImagePtr img = CreateTestImage(16, 16, 1, 8, 0, OPJ_CLRSPC_GRAY);
    OpjCodecPtr codec(opj_create_compress(OPJ_CODEC_J2K));
    ASSERT_NE(codec, nullptr);
    const int eff_cpus = (opj_get_num_cpus() == 0) ? 32 : opj_get_num_cpus();
    EXPECT_EQ(2 * eff_cpus, opj_thread_pool_get_thread_count(
                                ExtractJ2kHandle(codec.get())->m_tp));
    opj_cparameters_t cp = MakeLosslessParams(/*numresolution=*/2,
                                              TileGeometry{.tdx = 8, .tdy = 8});
    ASSERT_EQ(opj_setup_encoder(codec.get(), &cp, img.get()), OPJ_TRUE);
    MemWriteStream mem_out;
    OpjStreamPtr stream = mem_out.CreateStream();
    ASSERT_EQ(opj_start_compress(codec.get(), img.get(), stream.get()),
              OPJ_TRUE);
    std::vector<OPJ_BYTE> tile_data(64, 42);
    EXPECT_EQ(
        opj_write_tile(codec.get(), 99, tile_data.data(), 64, stream.get()),
        OPJ_FALSE);
    EXPECT_EQ(opj_write_tile(codec.get(), 0, tile_data.data(), 1, stream.get()),
              OPJ_FALSE);
  }
}

// Covers main-header / tile-part header validation, SPCod, TLM, unknown
// markers, and EOF/seek error branches across opj_j2k_read_*.
TEST(J2kTest, MalformedMainAndTilePartHeaderRejections) {
  auto base_bytes = MakeMinimalValidJ2k(16, 16, 3, 8);
  ASSERT_FALSE(base_bytes.empty());

  // 1. Missing required SIZ / COD / QCD markers in main header.
  for (uint16_t required : {J2K_MS_SIZ, J2K_MS_COD, J2K_MS_QCD}) {
    auto parsed = ParseCodestream(base_bytes);
    ASSERT_TRUE(parsed.has_value());
    parsed->EraseMainMarkers(required);
    EXPECT_EQ(DecodeBytes(BuildCodestream(*parsed)), nullptr);
  }

  // 2. Main header structural errors: non-compliant marker position (SOD),
  //    short stream at marker size, Lmarker < 2, short marker payload, and
  //    EOF right after a valid marker before next marker ID.
  {
    std::vector<std::vector<uint8_t>> bad_mh_streams = {
        {0xFF, 0x4F, 0xFF, 0x93, 0x00, 0x02},  // SOD in MH
        {0xFF, 0x4F, 0xFF, 0x64},              // EOF before marker len
        {0xFF, 0x4F, 0xFF, 0x64, 0x00, 0x01},  // Lcom < 2
        {0xFF, 0x4F, 0xFF, 0x64, 0x00, 0x10, 0x00, 0x01},  // EOF mid-COM
        {0xFF, 0x4F, 0xFF, 0x64, 0x00, 0x04, 0x00, 0x01},  // EOF after COM
    };
    for (const auto& s : bad_mh_streams) {
      EXPECT_EQ(DecodeBytes(s), nullptr);
    }
  }

  // 3. Large COM marker (> OPJ_J2K_DEFAULT_HEADER_SIZE) exercises header buffer
  //    realloc on both encode (write_com) and decode (read_header_procedure &
  //    TPH marker read).
  {
    OpjImagePtr img = CreateTestImage(8, 8, 1, 8, 0, OPJ_CLRSPC_GRAY);
    opj_cparameters_t cp;
    opj_set_default_encoder_parameters(&cp);
    cp.numresolution = 2;
    std::string big_comment(5000, 'J');
    cp.cp_comment = big_comment.data();
    auto big_com = EncodeCustom(img.get(), &cp);
    ASSERT_TRUE(big_com.has_value());
    EXPECT_NE(DecodeBytes(*big_com), nullptr);

    // Put a 5000-byte COM inside TPH too (covers TPH header realloc).
    auto parsed_com = ParseCodestream(*big_com);
    ASSERT_TRUE(parsed_com.has_value());
    parsed_com->tile_parts[0].tph_markers.push_back(
        MarkerSegment{J2K_MS_COM, std::vector<uint8_t>(5000, 0x41)});
    EXPECT_NE(DecodeBytes(BuildCodestream(*parsed_com)), nullptr);
  }

  // 4. SPCod / SPCoc & SQcd error branches: short COD, numres > MAXRLVLS,
  //    cblkw+cblkh > 12, HTMIXED style, qmfbid > 1, truncated PRT, empty QCD,
  //    cp_reduce >= numresolutions.
  {
    auto test_bad_cod = [&](std::vector<uint8_t> cod_payload,
                            int cp_reduce = 0) {
      auto parsed = ParseCodestream(base_bytes);
      ASSERT_TRUE(parsed.has_value());
      MarkerSegment* cod = parsed->FindMainMarker(J2K_MS_COD);
      ASSERT_NE(cod, nullptr);
      cod->payload = std::move(cod_payload);
      opj_dparameters_t dp;
      opj_set_default_decoder_parameters(&dp);
      dp.cp_reduce = cp_reduce;
      EXPECT_EQ(DecodeBytes(BuildCodestream(*parsed), &dp), nullptr);
    };
    test_bad_cod({0, 0, 0, 1, 0, 0, 0});              // short SPCod (<5 B)
    test_bad_cod({0, 0, 0, 1, 0, 35, 2, 2, 0, 1});    // numres > 33
    test_bad_cod({0, 0, 0, 1, 0, 1, 2, 2, 0, 1}, 5);  // reduce >= numres
    test_bad_cod({0, 0, 0, 1, 0, 1, 8, 8, 0, 1});     // cblkw+cblkh > 12
    test_bad_cod({0, 0, 0, 1, 0, 1, 2, 2, J2K_CCP_CBLKSTY_HTMIXED, 1});
    test_bad_cod({0, 0, 0, 1, 0, 1, 2, 2, 0, 2});        // qmfbid > 1
    test_bad_cod({1, 0, 0, 1, 0, 3, 2, 2, 0, 1, 0x33});  // truncated PRT

    auto p_qcd = ParseCodestream(base_bytes);
    ASSERT_TRUE(p_qcd.has_value());
    p_qcd->FindMainMarker(J2K_MS_QCD)->payload.clear();
    EXPECT_EQ(DecodeBytes(BuildCodestream(*p_qcd)), nullptr);
  }

  // 5. TLM ST=0, out-of-range tile_index, empty TLM, already-invalid second
  //    TLM, plus SIZ zero tdx/tdy.
  {
    auto p = ParseCodestream(base_bytes);
    ASSERT_TRUE(p.has_value());
    // ST=0 (1 entry for tile 0) + invalid ST=3 followed by another TLM.
    p->main_markers.push_back(
        MarkerSegment{J2K_MS_TLM, {0, 0x40, 0, 0, 0, 20}});
    p->main_markers.push_back(MarkerSegment{J2K_MS_TLM, {1, 0x40}});  // 0tp
    p->main_markers.push_back(
        MarkerSegment{J2K_MS_TLM, {2, 0x50, 5, 0, 0, 0, 20}});  // tile>=1
    p->main_markers.push_back(
        MarkerSegment{J2K_MS_TLM, {3, 0x40, 0, 0, 0, 20}});  // ignored invalid
    EXPECT_NE(DecodeBytes(BuildCodestream(*p)), nullptr);

    auto p_td0 = ParseCodestream(base_bytes);
    ASSERT_TRUE(p_td0.has_value());
    MarkerSegment* siz = p_td0->FindMainMarker(J2K_MS_SIZ);
    ASSERT_NE(siz, nullptr);
    siz->payload[18] = siz->payload[19] = siz->payload[20] = siz->payload[21] =
        0;  // tdx = 0
    EXPECT_EQ(DecodeBytes(BuildCodestream(*p_td0)), nullptr);
  }

  // 6. TPH error paths + unknown marker skip + SOT/SOD truncation + TLM
  //    corrupt SOT seek check.
  {
    // Non-compliant SIZ inside TPH, short TPH len, EOF inside TPH marker len,
    // TPH marker len > bytes remaining, EOF before SOD after SOT.
    for (MarkerSegment bad_tph : {
             MarkerSegment{J2K_MS_SIZ, {0, 0}},
             MarkerSegment{J2K_MS_QCC, {}},
         }) {
      auto p = ParseCodestream(base_bytes);
      ASSERT_TRUE(p.has_value());
      p->tile_parts[0].tph_markers.push_back(bad_tph);
      EXPECT_EQ(DecodeBytes(BuildCodestream(*p)), nullptr);
    }

    // Corrupt TPH marker length < 2 & > stream left.
    auto p_tph_hdr = ParseCodestream(base_bytes);
    ASSERT_TRUE(p_tph_hdr.has_value());
    std::vector<uint8_t> raw = BuildCodestream(*p_tph_hdr);
    // Find first SOT (0xFF 0x90), replace SOD (0xFF 0x93) right after SOT with
    // COM + L=1 or L=0xFFFF or EOF.
    for (size_t i = 0; i + 16 <= raw.size(); ++i) {
      if (raw[i] == 0xFF && raw[i + 1] == 0x90) {
        size_t sod_off = i + 12;
        if (sod_off + 2 <= raw.size() && raw[sod_off] == 0xFF &&
            raw[sod_off + 1] == 0x93) {
          std::vector<uint8_t> trunc_after_sot(raw.begin(),
                                               raw.begin() + sod_off);
          EXPECT_EQ(DecodeBytes(trunc_after_sot), nullptr);

          std::vector<uint8_t> tiny_l(raw.begin(), raw.begin() + sod_off);
          tiny_l.insert(tiny_l.end(), {0xFF, 0x64, 0x00, 0x01});
          EXPECT_EQ(DecodeBytes(tiny_l), nullptr);

          std::vector<uint8_t> huge_l(raw.begin(), raw.begin() + sod_off);
          huge_l.insert(huge_l.end(), {0xFF, 0x64, 0x7F, 0xFF, 0x00, 0x01});
          EXPECT_EQ(DecodeBytes(huge_l), nullptr);

          std::vector<uint8_t> eof_at_sod(raw.begin(),
                                          raw.begin() + sod_off + 2);
          EXPECT_EQ(DecodeBytes(eof_at_sod, nullptr, nullptr, false), nullptr);
          break;
        }
      }
    }

    // Unknown marker skipping before SIZ/COD/etc. (0xFF 0x54 unknown marker).
    auto p_unk = ParseCodestream(base_bytes);
    ASSERT_TRUE(p_unk.has_value());
    std::vector<uint8_t> unk_bytes = {0xFF, 0x4F, 0xFF, 0x54, 0x00, 0x02};
    unk_bytes.insert(unk_bytes.end(), base_bytes.begin() + 2, base_bytes.end());
    EXPECT_NE(DecodeBytes(unk_bytes), nullptr);

    // TLM pointing to missing SOT on opj_get_decoded_tile & fail_seek.
    OpjImagePtr img = CreateTestImage(16, 16, 1, 8, 0, OPJ_CLRSPC_GRAY);
    opj_cparameters_t cp = MakeLosslessParams(/*numresolution=*/2,
                                              TileGeometry{.tdx = 8, .tdy = 8});
    auto tlm_bytes = EncodeCustom(img.get(), &cp, kTlmOpts);
    ASSERT_TRUE(tlm_bytes.has_value());

    std::vector<uint8_t> broken_tlm = *tlm_bytes;
    // Corrupt second SOT marker ID (tile 1) to trigger "Did not get expected
    // SOT marker" when seeking via TLM.
    const std::optional<size_t> second_sot =
        FindNthMarker(broken_tlm, J2K_MS_SOT, 2);
    ASSERT_TRUE(second_sot.has_value());
    broken_tlm[*second_sot + 1] = 0x00;
    {
      OpjCodecPtr dec(opj_create_decompress(OPJ_CODEC_J2K));
      opj_dparameters_t dp;
      opj_set_default_decoder_parameters(&dp);
      ASSERT_EQ(opj_setup_decoder(dec.get(), &dp), OPJ_TRUE);
      MemReadStream mem_in(broken_tlm);
      OpjStreamPtr stream = mem_in.CreateStream();
      opj_image_t* raw_hdr = nullptr;
      ASSERT_EQ(opj_read_header(stream.get(), dec.get(), &raw_hdr), OPJ_TRUE);
      OpjImagePtr hdr(raw_hdr);
      opj_image_cmptparm_t cmpt{.dx = 1,
                                .dy = 1,
                                .w = 8,
                                .h = 8,
                                .x0 = 0,
                                .y0 = 0,
                                .prec = 8,
                                .sgnd = 0};
      OpjImagePtr tile_out(opj_image_tile_create(1, &cmpt, OPJ_CLRSPC_GRAY));
      EXPECT_EQ(
          opj_get_decoded_tile(dec.get(), stream.get(), tile_out.get(), 1),
          OPJ_FALSE);
    }
  }
}

// Covers remaining non-OOM edge branches across j2k.c: ihdr mismatch,
// discard_tiles, TPsot/TNsot mismatches, >10 TNsot=0 realloc, missing-tile TLM
// fallback, CompareOffT sort orders, TLM/no-TLM tile seeking failures, Issue
// 254 non-seekable/corrupt paths, unknown marker before SOT/SOD, MCO/add_mct
// size checks, SQcd/SQcc comparisons, encoding_validation checks, and exact
// stream-write failure boundaries.
TEST(J2kTest, DecoderHeaderEdgeStatesAndSmallPrecinctValidation) {
  auto base_bytes = MakeMinimalValidJ2k(16, 16, 3, 8);
  ASSERT_FALSE(base_bytes.empty());

  {
    OpjCodecPtr dec(opj_create_decompress(OPJ_CODEC_J2K));
    opj_dparameters_t dp;
    opj_set_default_decoder_parameters(&dp);
    ASSERT_EQ(opj_setup_decoder(dec.get(), &dp), OPJ_TRUE);
    opj_j2k_t* j2k = ExtractJ2kHandle(dec.get());
    ASSERT_NE(j2k, nullptr);
    j2k->ihdr_w = 99;
    j2k->ihdr_h = 99;
    MemReadStream mem_in(base_bytes);
    OpjStreamPtr stream = mem_in.CreateStream();
    opj_image_t* raw = nullptr;
    EXPECT_EQ(opj_read_header(stream.get(), dec.get(), &raw), OPJ_FALSE);
    opj_image_destroy(raw);
  }
  {
    OpjCodecPtr dec(opj_create_decompress(OPJ_CODEC_J2K));
    opj_dparameters_t dp;
    opj_set_default_decoder_parameters(&dp);
    ASSERT_EQ(opj_setup_decoder(dec.get(), &dp), OPJ_TRUE);
    opj_j2k_t* j2k = ExtractJ2kHandle(dec.get());
    ASSERT_NE(j2k, nullptr);
    j2k->m_specific_param.m_decoder.m_discard_tiles = 1;
    j2k->m_specific_param.m_decoder.m_start_tile_x = 0;
    j2k->m_specific_param.m_decoder.m_start_tile_y = 0;
    j2k->m_specific_param.m_decoder.m_end_tile_x = 16;
    j2k->m_specific_param.m_decoder.m_end_tile_y = 16;
    MemReadStream mem_in(base_bytes);
    OpjStreamPtr stream = mem_in.CreateStream();
    opj_image_t* raw = nullptr;
    EXPECT_EQ(opj_read_header(stream.get(), dec.get(), &raw), OPJ_TRUE);
    opj_image_destroy(raw);

    // opj_read_tile_header on invalid decoder state returns OPJ_FALSE.
    OpjCodecPtr dec2(opj_create_decompress(OPJ_CODEC_J2K));
    ASSERT_EQ(opj_setup_decoder(dec2.get(), &dp), OPJ_TRUE);
    MemReadStream mem2(base_bytes);
    OpjStreamPtr stream2 = mem2.CreateStream();
    OPJ_UINT32 tidx = 0, dsz = 0, nc = 0;
    OPJ_INT32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    OPJ_BOOL go_on = OPJ_FALSE;
    EXPECT_EQ(opj_read_tile_header(dec2.get(), stream2.get(), &tidx, &dsz, &x0,
                                   &y0, &x1, &y1, &nc, &go_on),
              OPJ_FALSE);
    EventCapture ev;
    opj_event_mgr_t mgr = MakeEventManager(&ev);
    EXPECT_EQ(
        opj_j2k_decode(ExtractJ2kHandle(dec2.get()),
                       reinterpret_cast<opj_stream_private_t*>(stream2.get()),
                       nullptr, &mgr),
        OPJ_FALSE);
  }
  {
    // IMF 2K tdx >= w && tdy < h + prcw_init < 1 & size_prcw < 1 branches +
    // encoding_validation numresolutions > 32 & tdy < 2^(numres-1).
    std::vector<opj_image_cmptparm_t> cmpts(3);
    for (auto& c : cmpts) {
      c = opj_image_cmptparm_t{.dx = 1,
                               .dy = 1,
                               .w = 128,
                               .h = 128,
                               .x0 = 0,
                               .y0 = 0,
                               .prec = 8,
                               .sgnd = 0};
    }
    OpjImagePtr imf_img(
        opj_image_tile_create(3, cmpts.data(), OPJ_CLRSPC_SRGB));
    imf_img->x1 = 128;
    imf_img->y1 = 128;
    opj_cparameters_t cp;
    opj_set_default_encoder_parameters(&cp);
    cp.rsiz = OPJ_PROFILE_IMF_2K | kImfMainLevel1SubLevel1;
    cp.tile_size_on = OPJ_TRUE;
    cp.cp_tdx = 128;
    cp.cp_tdy = 64;
    cp.numresolution = 5;
    OpjJ2kPtr j2k(opj_j2k_create_compress());
    EventCapture ev;
    opj_event_mgr_t mgr = MakeEventManager(&ev);
    EXPECT_EQ(opj_j2k_setup_encoder(j2k.get(), &cp, imf_img.get(), &mgr),
              OPJ_TRUE);
    EXPECT_EQ(cp.rsiz, OPJ_PROFILE_NONE);
    EXPECT_THAT(ev.warnings, Contains(HasSubstr("Non-IMF codestream")));

    // Small precinct clamp branches in opj_j2k_setup_encoder.
    OpjImagePtr tiny = CreateTestImage(8, 8, 1, 8, 0, OPJ_CLRSPC_GRAY);
    opj_set_default_encoder_parameters(&cp);
    cp.numresolution = 1;
    cp.csty = J2K_CP_CSTY_PRT;
    cp.res_spec = 1;
    cp.prcw_init[0] = 0;
    cp.prch_init[0] = 0;
    EXPECT_TRUE(EncodeCustom(tiny.get(), &cp).has_value());

    opj_set_default_encoder_parameters(&cp);
    cp.numresolution = 2;
    cp.csty = J2K_CP_CSTY_PRT;
    cp.res_spec = 1;
    cp.prcw_init[0] = 1;
    cp.prch_init[0] = 1;
    OpjJ2kPtr j2k_sp(opj_j2k_create_compress());
    EXPECT_EQ(opj_j2k_setup_encoder(j2k_sp.get(), &cp, tiny.get(), &mgr),
              OPJ_TRUE);

    // opj_j2k_encoding_validation tdy < 2^(numres-1) and numres > 32.
    OpjImagePtr val_img = CreateTestImage(32, 32, 1, 8, 0, OPJ_CLRSPC_GRAY);
    cp = MakeLosslessParams(/*numresolution=*/5,
                            TileGeometry{.tdx = 32, .tdy = 8});
    EXPECT_FALSE(EncodeCustom(val_img.get(), &cp).has_value());

    OpjCodecPtr enc33(opj_create_compress(OPJ_CODEC_J2K));
    opj_set_default_encoder_parameters(&cp);
    cp.numresolution = 2;
    ASSERT_EQ(opj_setup_encoder(enc33.get(), &cp, val_img.get()), OPJ_TRUE);
    ExtractJ2kHandle(enc33.get())->m_cp.tcps[0].tccps[0].numresolutions = 33;
    MemWriteStream mw33;
    OpjStreamPtr s33 = mw33.CreateStream();
    EXPECT_EQ(opj_start_compress(enc33.get(), val_img.get(), s33.get()),
              OPJ_FALSE);
  }
}

TEST(J2kTest, MainAndTilePartHeaderSotSequenceErrors) {
  auto base_bytes = MakeMinimalValidJ2k(16, 16, 3, 8);
  ASSERT_FALSE(base_bytes.empty());

  EXPECT_EQ(DecodeBytes(std::vector<uint8_t>{0xFF, 0x4F}), nullptr);
  EXPECT_EQ(DecodeBytes(std::vector<uint8_t>{0xFF, 0x4F, 0xFF, 0x90, 0x00, 0x0A,
                                             0, 0, 0, 0, 0, 14, 0, 1, 0xFF,
                                             0x93, 0xFF, 0xD9}),
            nullptr);
  EXPECT_EQ(DecodeBytes(std::vector<uint8_t>{0xFF, 0x4F, 0xFF, 0x54, 0xFF, 0x93,
                                             0x00, 0x02}),
            nullptr);

  // Unknown 2-byte tag placed directly before SOT exercises UNK->SOT break.
  const std::optional<size_t> sot_opt =
      FindNthMarker(base_bytes, J2K_MS_SOT, 1);
  ASSERT_TRUE(sot_opt.has_value());
  std::vector<uint8_t> unk_before_sot = base_bytes;
  unk_before_sot.insert(unk_before_sot.begin() + *sot_opt, {0xFF, 0x54});
  EXPECT_NE(DecodeBytes(unk_before_sot), nullptr);

  // SOT TPsot >= m_nb_tile_parts (tnsot=0/2 on second) and TPsot >= TNsot
  // (first tnsot=0, second tnsot=1) on a 2-tile stream so
  // opj_j2k_decode_tiles keeps reading past tile 0 into the malformed second
  // T0 tile-part.
  {
    OpjImagePtr two_tile_img = CreateTestImage(16, 8, 1, 8, 0, OPJ_CLRSPC_GRAY);
    opj_cparameters_t tile_cp = MakeLosslessParams(
        /*numresolution=*/2, TileGeometry{.tdx = 8, .tdy = 8});
    auto two_tile_bytes = EncodeCustom(two_tile_img.get(), &tile_cp);
    ASSERT_TRUE(two_tile_bytes.has_value());
    struct TnsotPair {
      uint8_t first_tnsot;
      uint8_t second_tnsot;
    };
    constexpr TnsotPair kTnsotPairs[] = {
        {.first_tnsot = 1, .second_tnsot = 0},
        {.first_tnsot = 1, .second_tnsot = 2},
        {.first_tnsot = 0, .second_tnsot = 1},
    };
    for (const TnsotPair& pair : kTnsotPairs) {
      auto p = ParseCodestream(*two_tile_bytes);
      ASSERT_TRUE(p.has_value());
      ASSERT_EQ(p->tile_parts.size(), 2U);
      p->tile_parts[0].tnsot = pair.first_tnsot;
      TilePartSegment dup_t0 = p->tile_parts[0];
      dup_t0.tpsot = 1;
      dup_t0.tnsot = pair.second_tnsot;
      p->tile_parts.insert(p->tile_parts.begin() + 1, std::move(dup_t0));
      EXPECT_EQ(DecodeBytes(BuildCodestream(*p)), nullptr);
    }
  }

  // 12 TNsot=0 tile-parts hit tp_index realloc when l_current_part >= 10.
  {
    auto p12 = ParseCodestream(base_bytes);
    ASSERT_TRUE(p12.has_value());
    p12->tile_parts[0].tnsot = 0;
    for (uint8_t idx = 1; idx < 12; ++idx) {
      TilePartSegment extra;
      extra.isot = 0;
      extra.tpsot = idx;
      extra.tnsot = 0;
      extra.emit_sod = true;
      extra.sod_data.clear();
      p12->tile_parts.push_back(std::move(extra));
    }
    // Empty TNsot=0 trailing tile-parts past index 10 hit tp_index realloc
    // while keeping the stream valid and decodable.
    OpjImagePtr dec12 = DecodeBytes(BuildCodestream(*p12));
    ASSERT_NE(dec12, nullptr);
    EXPECT_EQ(dec12->comps[0].w, 16U);
    EXPECT_EQ(dec12->comps[0].h, 16U);
  }
}

TEST(J2kTest, TileSeekingErrorsAndTlmSortOrders) {
  // 24x8 interleaved 3-tile 2-layer TLM stream: missing-tile TLM fallback,
  // SOT TNsot > TLM nb_tps warning, CompareOffT sort (> and ==),
  // MemReadStream::fail_seek_on_call on TLM/non-TLM get_decoded_tile, and
  // corrupt interleaved SOT.
  OpjImagePtr img = CreateTestImage(24, 8, 1, 8, 0, OPJ_CLRSPC_GRAY);
  const auto expected = SnapshotImagePixels(img.get());
  opj_cparameters_t cp =
      MakeLosslessParams(/*numresolution=*/2, TileGeometry{.tdx = 8, .tdy = 8});
  cp.prog_order = OPJ_LRCP;
  cp.tp_on = 1;
  cp.tp_flag = 'L';
  cp.tcp_numlayers = 2;
  cp.tcp_rates[0] = 20.0f;
  cp.tcp_rates[1] = 0.0f;
  auto tlm3 = EncodeCustom(img.get(), &cp, kTlmOpts);
  ASSERT_TRUE(tlm3.has_value());

  // 1. TLM missing tile 1/2 hits build_tp_index_from_tlm error & cleanup +
  //    l_num_parts > nb_tps warning, then still decodes cleanly.
  {
    auto p = ParseCodestream(*tlm3);
    ASSERT_TRUE(p.has_value());
    MarkerSegment* tlm = p->FindMainMarker(J2K_MS_TLM);
    ASSERT_NE(tlm, nullptr);
    tlm->payload = {0, 0x50, 0, 0, 0, 0, 20};
    ExpectImagePixelsEqual(expected, DecodeBytes(BuildCodestream(*p)).get());
  }

  // 2. Interleave 6 tile-parts [T0(0), T1(0), T2(0), T0(1), T1(1), T2(1)] and
  //    re-encode with valid TLM via area [0,0,16,8] to hit CompareOffT return
  //    1.
  auto p_inter = ParseCodestream(*tlm3);
  ASSERT_TRUE(p_inter.has_value());
  ASSERT_EQ(p_inter->tile_parts.size(), 6U);
  ParsedJ2kStream inter = *p_inter;
  inter.tile_parts = {p_inter->tile_parts[0], p_inter->tile_parts[2],
                      p_inter->tile_parts[4], p_inter->tile_parts[1],
                      p_inter->tile_parts[3], p_inter->tile_parts[5]};
  MarkerSegment* tlm_m = inter.FindMainMarker(J2K_MS_TLM);
  ASSERT_NE(tlm_m, nullptr);
  // Patch TLM entries to interleaved order and lengths.
  tlm_m->payload = {0, 0x50};
  for (const auto& tp : inter.tile_parts) {
    tlm_m->payload.push_back(static_cast<uint8_t>(tp.isot));
    AppendBe32(tlm_m->payload, tp.TilePartLength());
  }
  std::vector<uint8_t> inter_bytes = BuildCodestream(inter);
  {
    OpjCodecPtr dec(opj_create_decompress(OPJ_CODEC_J2K));
    opj_dparameters_t dp;
    opj_set_default_decoder_parameters(&dp);
    ASSERT_EQ(opj_setup_decoder(dec.get(), &dp), OPJ_TRUE);
    MemReadStream mem_in(inter_bytes);
    OpjStreamPtr stream = mem_in.CreateStream();
    opj_image_t* raw = nullptr;
    ASSERT_EQ(opj_read_header(stream.get(), dec.get(), &raw), OPJ_TRUE);
    OpjImagePtr out(raw);
    ASSERT_EQ(opj_set_decode_area(dec.get(), out.get(), 0, 0, 16, 8), OPJ_TRUE);
    EXPECT_EQ(opj_decode(dec.get(), stream.get(), out.get()), OPJ_TRUE);
  }

  opj_image_cmptparm_t cmpt{
      .dx = 1, .dy = 1, .w = 8, .h = 8, .x0 = 0, .y0 = 0, .prec = 8, .sgnd = 0};
  // 3. fail_seek_on_call = 1 (tp0 SOT seek), 2 (tp1 next_tp_sot seek), 3 (end).
  for (int fail_idx : {1, 2, 3}) {
    MemReadStream mem_in(inter_bytes);
    mem_in.fail_seek_on_call = fail_idx;
    OpjStreamPtr stream = mem_in.CreateStream();
    OpjCodecPtr dec(opj_create_decompress(OPJ_CODEC_J2K));
    opj_dparameters_t dp;
    opj_set_default_decoder_parameters(&dp);
    ASSERT_EQ(opj_setup_decoder(dec.get(), &dp), OPJ_TRUE);
    opj_image_t* raw = nullptr;
    ASSERT_EQ(opj_read_header(stream.get(), dec.get(), &raw), OPJ_TRUE);
    OpjImagePtr hdr(raw);
    OpjImagePtr tile_img(opj_image_tile_create(1, &cmpt, OPJ_CLRSPC_GRAY));
    EXPECT_EQ(opj_get_decoded_tile(dec.get(), stream.get(), tile_img.get(),
                                   fail_idx == 1 ? 1 : 0),
              OPJ_FALSE);
  }

  // 4. Corrupt second tile-part SOT of tile 0 -> read_tile_header next_tp_sot
  //    error ("Did not get expected SOT marker").
  {
    std::vector<uint8_t> bad_tp2 = inter_bytes;
    const std::optional<size_t> fourth_sot =
        FindNthMarker(bad_tp2, J2K_MS_SOT, 4);
    ASSERT_TRUE(fourth_sot.has_value());
    bad_tp2[*fourth_sot + 1] = 0x00;
    OpjCodecPtr dec(opj_create_decompress(OPJ_CODEC_J2K));
    opj_dparameters_t dp;
    opj_set_default_decoder_parameters(&dp);
    ASSERT_EQ(opj_setup_decoder(dec.get(), &dp), OPJ_TRUE);
    MemReadStream mem_in(bad_tp2);
    OpjStreamPtr stream = mem_in.CreateStream();
    opj_image_t* raw = nullptr;
    ASSERT_EQ(opj_read_header(stream.get(), dec.get(), &raw), OPJ_TRUE);
    OpjImagePtr hdr(raw);
    OpjImagePtr tile_img(opj_image_tile_create(1, &cmpt, OPJ_CLRSPC_GRAY));
    EXPECT_EQ(opj_get_decoded_tile(dec.get(), stream.get(), tile_img.get(), 0),
              OPJ_FALSE);
  }

  // 5. Non-TLM 3-tile stream: get tile 1 directly (skips tile 0 cleanly), then
  //    fail seek to m_last_sot_read_pos when getting tile 1 after tile 0.
  cp.tp_on = 0;
  cp.tcp_numlayers = 1;
  cp.tcp_rates[0] = 0;
  auto no_tlm3 = EncodeCustom(img.get(), &cp);
  ASSERT_TRUE(no_tlm3.has_value());
  {
    OpjCodecPtr dec(opj_create_decompress(OPJ_CODEC_J2K));
    opj_dparameters_t dp;
    opj_set_default_decoder_parameters(&dp);
    ASSERT_EQ(opj_setup_decoder(dec.get(), &dp), OPJ_TRUE);
    EventCapture ev;
    ev.AttachTo(dec.get());
    MemReadStream mem_in(*no_tlm3);
    OpjStreamPtr stream = mem_in.CreateStream();
    opj_image_t* raw = nullptr;
    ASSERT_EQ(opj_read_header(stream.get(), dec.get(), &raw), OPJ_TRUE);
    OpjImagePtr hdr(raw);
    OpjImagePtr tile_img(opj_image_tile_create(1, &cmpt, OPJ_CLRSPC_GRAY));
    EXPECT_EQ(opj_get_decoded_tile(dec.get(), stream.get(), tile_img.get(), 1),
              OPJ_TRUE);
    EXPECT_THAT(ev.errors, IsEmpty());
  }
  {
    OpjCodecPtr dec(opj_create_decompress(OPJ_CODEC_J2K));
    opj_dparameters_t dp;
    opj_set_default_decoder_parameters(&dp);
    ASSERT_EQ(opj_setup_decoder(dec.get(), &dp), OPJ_TRUE);
    MemReadStream mem_in(*no_tlm3);
    OpjStreamPtr stream = mem_in.CreateStream();
    opj_image_t* raw = nullptr;
    ASSERT_EQ(opj_read_header(stream.get(), dec.get(), &raw), OPJ_TRUE);
    OpjImagePtr hdr(raw);
    OpjImagePtr tile_img(opj_image_tile_create(1, &cmpt, OPJ_CLRSPC_GRAY));
    ASSERT_EQ(opj_get_decoded_tile(dec.get(), stream.get(), tile_img.get(), 0),
              OPJ_TRUE);
    mem_in.fail_seek = true;
    EXPECT_EQ(opj_get_decoded_tile(dec.get(), stream.get(), tile_img.get(), 1),
              OPJ_FALSE);
  }

  // 6. decode_tile post-SOD missing-SOT error (extra trailing bytes after
  // tile 0).
  {
    std::vector<uint8_t> two_tile_bad_next = *no_tlm3;
    const std::optional<size_t> second_sot =
        FindNthMarker(two_tile_bad_next, J2K_MS_SOT, 2);
    ASSERT_TRUE(second_sot.has_value());
    ASSERT_GE(two_tile_bad_next.size(), *second_sot + 4U);
    two_tile_bad_next[*second_sot + 1] = 0x00;
    two_tile_bad_next.resize(*second_sot + 4U);
    EXPECT_EQ(DecodeBytes(two_tile_bad_next), nullptr);
  }
}

TEST(J2kTest, Issue254NonSeekableAndCorruptTileParts) {
  OpjImagePtr img = CreateTestImage(16, 8, 1);
  opj_cparameters_t cp =
      MakeLosslessParams(/*numresolution=*/2, TileGeometry{.tdx = 8, .tdy = 8});
  cp.prog_order = OPJ_LRCP;
  cp.tp_on = 1;
  cp.tp_flag = 'L';
  cp.tcp_numlayers = 3;
  cp.tcp_rates[0] = 20.0f;
  cp.tcp_rates[1] = 10.0f;
  cp.tcp_rates[2] = 0.0f;
  auto enc = EncodeCustom(img.get(), &cp);
  ASSERT_TRUE(enc.has_value());
  auto p = ParseCodestream(*enc);
  ASSERT_TRUE(p.has_value());
  ASSERT_EQ(p->tile_parts.size(), 6U);
  ParsedJ2kStream issue254 = *p;
  issue254.tile_parts = {p->tile_parts[0], p->tile_parts[3], p->tile_parts[1],
                         p->tile_parts[4], p->tile_parts[2], p->tile_parts[5]};
  for (auto& tp : issue254.tile_parts) {
    tp.tnsot = 2;
  }
  std::vector<uint8_t> bytes254 = BuildCodestream(issue254);

  // 1. Non-seekable stream returns early from need_nb_tile_parts_correction,
  //    decoding using the declared first 2 tile-parts per tile.
  {
    OpjCodecPtr dec(opj_create_decompress(OPJ_CODEC_J2K));
    opj_dparameters_t dp;
    opj_set_default_decoder_parameters(&dp);
    ASSERT_EQ(opj_setup_decoder(dec.get(), &dp), OPJ_TRUE);
    MemReadStream mem_in(bytes254);
    mem_in.allow_seek = false;
    OpjStreamPtr stream = mem_in.CreateStream();
    opj_image_t* raw = nullptr;
    ASSERT_EQ(opj_read_header(stream.get(), dec.get(), &raw), OPJ_TRUE);
    OpjImagePtr hdr(raw);
    EXPECT_EQ(opj_decode(dec.get(), stream.get(), hdr.get()), OPJ_TRUE);
  }

  // 2. Corrupt tile-part #4 (T1_p1, the first SOT inspected after T0_p1
  //    triggers need_nb_tile_parts_correction) to hit marker/Lsot/Psot/skip
  //    branches.
  const std::optional<size_t> tp1_opt = FindNthMarker(bytes254, J2K_MS_SOT, 4);
  ASSERT_TRUE(tp1_opt.has_value());
  const size_t tp1_off = *tp1_opt;
  ASSERT_GT(tp1_off, 0U);
  {
    std::vector<uint8_t> eof_at_tp1(bytes254.begin(),
                                    bytes254.begin() + tp1_off);
    EXPECT_EQ(DecodeBytes(eof_at_tp1), nullptr);

    std::vector<uint8_t> non_sot_tp1 = bytes254;
    non_sot_tp1[tp1_off + 1] = 0x64;  // COM instead of SOT
    EXPECT_EQ(DecodeBytes(non_sot_tp1), nullptr);

    std::vector<uint8_t> eof_mid_lsot(bytes254.begin(),
                                      bytes254.begin() + tp1_off + 2);
    EXPECT_EQ(DecodeBytes(eof_mid_lsot), nullptr);

    std::vector<uint8_t> bad_lsot = bytes254;
    bad_lsot[tp1_off + 3] = 8;  // != 10
    EXPECT_EQ(DecodeBytes(bad_lsot), nullptr);

    std::vector<uint8_t> eof_mid_sot(bytes254.begin(),
                                     bytes254.begin() + tp1_off + 5);
    EXPECT_EQ(DecodeBytes(eof_mid_sot), nullptr);

    std::vector<uint8_t> small_psot = bytes254;
    small_psot[tp1_off + 6] = 0;
    small_psot[tp1_off + 7] = 0;
    small_psot[tp1_off + 8] = 0;
    small_psot[tp1_off + 9] = 5;  // < 14U
    EXPECT_EQ(DecodeBytes(small_psot), nullptr);

    std::vector<uint8_t> huge_psot = bytes254;
    huge_psot[tp1_off + 6] = 0x10;
    EXPECT_EQ(DecodeBytes(huge_psot), nullptr);
  }
}

TEST(J2kTest, Part2MctMccMcoReallocAndSizeErrors) {
  auto base_bytes = MakeMinimalValidJ2k(16, 16, 3, 8);
  ASSERT_FALSE(base_bytes.empty());

  auto p = ParseCodestream(base_bytes);
  ASSERT_TRUE(p.has_value());
  // 1. Duplicate MCT index 1 overwrites prior data buffer.
  std::vector<uint8_t> mct1 = {0, 0, 0x09, 1, 0, 0};
  mct1.resize(6 + 36, 0);
  p->main_markers.push_back(MarkerSegment{J2K_MS_MCT, mct1});
  p->main_markers.push_back(MarkerSegment{J2K_MS_MCT, mct1});
  // Missing MCC index 99 in MCO -> discarded.
  p->main_markers.push_back(MarkerSegment{J2K_MS_MCO, {1, 99}});
  // MCC with nb_comps=2 != 3 and MCO twice (triggers free of prior deco
  // matrix).
  p->main_markers.push_back(MarkerSegment{
      J2K_MS_MCC, {0, 0, 1, 0, 0, 0, 1, 1, 0, 2, 0, 1, 0, 2, 0, 1, 0, 0, 0}});
  p->main_markers.push_back(MarkerSegment{J2K_MS_MCO, {1, 1}});
  p->main_markers.push_back(MarkerSegment{J2K_MS_MCO, {1, 1}});
  EXPECT_NE(DecodeBytes(BuildCodestream(*p)), nullptr);

  // 2. Deco MCT size mismatch -> opj_j2k_add_mct returns OPJ_FALSE.
  auto p_bad_deco = ParseCodestream(base_bytes);
  ASSERT_TRUE(p_bad_deco.has_value());
  p_bad_deco->main_markers.push_back(
      MarkerSegment{J2K_MS_MCT, {0, 0, 0x09, 1, 0, 0, 1, 2, 3, 4}});
  p_bad_deco->main_markers.push_back(MarkerSegment{
      J2K_MS_MCC,
      {0, 0, 1, 0, 0, 0, 1, 1, 0, 3, 0, 1, 2, 0, 3, 0, 1, 2, 0, 0, 1}});
  p_bad_deco->main_markers.push_back(MarkerSegment{J2K_MS_MCO, {1, 1}});
  EXPECT_EQ(DecodeBytes(BuildCodestream(*p_bad_deco)), nullptr);

  // 3. Offset MCT size mismatch -> opj_j2k_add_mct returns OPJ_FALSE.
  auto p_bad_off = ParseCodestream(base_bytes);
  ASSERT_TRUE(p_bad_off.has_value());
  p_bad_off->main_markers.push_back(MarkerSegment{J2K_MS_MCT, mct1});
  p_bad_off->main_markers.push_back(
      MarkerSegment{J2K_MS_MCT, {0, 0, 0x0A, 2, 0, 0, 1, 2, 3, 4}});
  p_bad_off->main_markers.push_back(MarkerSegment{
      J2K_MS_MCC,
      {0, 0, 1, 0, 0, 0, 1, 1, 0, 3, 0, 1, 2, 0, 3, 0, 1, 2, 0, 2, 1}});
  p_bad_off->main_markers.push_back(MarkerSegment{J2K_MS_MCO, {1, 1}});
  EXPECT_EQ(DecodeBytes(BuildCodestream(*p_bad_off)), nullptr);
}

TEST(J2kTest, SqcdComparisonAndWriteFailureBoundarySweeps) {
  // 1. opj_j2k_compare_SQcd_SQcc mismatch branches.
  OpjImagePtr img4 = CreateTestImage(8, 8, 4);
  OpjCodecPtr codec(opj_create_compress(OPJ_CODEC_J2K));
  opj_cparameters_t cp;
  opj_set_default_encoder_parameters(&cp);
  cp.irreversible = 1;
  cp.numresolution = 2;
  ASSERT_EQ(opj_setup_encoder(codec.get(), &cp, img4.get()), OPJ_TRUE);
  opj_j2k_t* j2k = ExtractJ2kHandle(codec.get());
  auto* tccps = j2k->m_cp.tcps[0].tccps;
  tccps[1].qntsty = J2K_CCP_QNTSTY_NOQNT;
  tccps[2].numgbits = 3;
  tccps[3].stepsizes[0].mant += 1;
  MemWriteStream mw;
  OpjStreamPtr s = mw.CreateStream();
  EXPECT_EQ(opj_start_compress(codec.get(), img4.get(), s.get()), OPJ_TRUE);
  EXPECT_EQ(opj_end_compress(codec.get(), s.get()), OPJ_TRUE);

  // 2. Sampled write-boundary failure sweeps across normal + TLM/PLT/COC/QCC/
  //    RGN/COM + Cinema4K POC + Part-2 MCT encodings. Oracle is ASan/LSan.
  auto sweep_write_failures = [](auto&& setup_fn) {
    MemWriteStream clean_mw;
    ASSERT_TRUE(setup_fn(clean_mw, false));
    const size_t total = clean_mw.data.size();
    for (size_t cap = 0; cap < total; cap += (cap < 80 ? 2 : 8)) {
      MemWriteStream fail_mw;
      fail_mw.max_bytes_before_fail = cap;
      (void)setup_fn(fail_mw, false);
    }
    // Also exercise seek failure during opj_j2k_write_updated_tlm.
    MemWriteStream seek_fail_mw;
    (void)setup_fn(seek_fail_mw, true);
  };

  sweep_write_failures([](MemWriteStream& mw, bool fail_seek_on_end) {
    OpjImagePtr img = CreateTestImage(16, 16, 3);
    OpjCodecPtr c(opj_create_compress(OPJ_CODEC_J2K));
    opj_cparameters_t p;
    opj_set_default_encoder_parameters(&p);
    p.numresolution = 2;
    p.roi_compno = 0;
    p.roi_shift = 2;
    std::string comment = "write_sweep_com";
    p.cp_comment = comment.data();
    if (!opj_setup_encoder(c.get(), &p, img.get()) ||
        !opj_encoder_set_extra_options(c.get(), kTlmPltOpts)) {
      return false;
    }
    ExtractJ2kHandle(c.get())->m_cp.tcps[0].tccps[1].stepsizes[0].expn += 1;
    ExtractJ2kHandle(c.get())->m_cp.tcps[0].tccps[1].cblksty ^=
        J2K_CCP_CBLKSTY_RESET;
    OpjStreamPtr s = mw.CreateStream(32);  // small flush buffer
    if (!opj_start_compress(c.get(), img.get(), s.get()) ||
        !opj_encode(c.get(), s.get())) {
      return false;
    }
    if (fail_seek_on_end) {
      mw.fail_seek = true;
    }
    return opj_end_compress(c.get(), s.get()) == OPJ_TRUE;
  });

  sweep_write_failures([](MemWriteStream& mw, bool) {
    OpjImagePtr img = CreateTestImage(16, 16, 3);
    OpjCodecPtr c(opj_create_compress(OPJ_CODEC_J2K));
    opj_cparameters_t p;
    opj_set_default_encoder_parameters(&p);
    absl::Cleanup free_mct = [&p] {
      std::free(p.mct_data);
      p.mct_data = nullptr;
    };
    p.irreversible = 1;
    p.numresolution = 2;
    std::array<OPJ_FLOAT32, 9> mat = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::array<OPJ_INT32, 3> off = {0, 0, 0};
    if (!opj_set_MCT(&p, mat.data(), off.data(), 3) ||
        !opj_setup_encoder(c.get(), &p, img.get())) {
      return false;
    }
    OpjStreamPtr s = mw.CreateStream(32);
    return opj_start_compress(c.get(), img.get(), s.get()) == OPJ_TRUE &&
           opj_encode(c.get(), s.get()) == OPJ_TRUE &&
           opj_end_compress(c.get(), s.get()) == OPJ_TRUE;
  });
}

TEST(J2kTest, AllocationFailurePathsCleanUpWithoutLeaks) {
  // Ensure single-threaded execution so thread_local
  // ScopedOpjAllocFailCountdown deterministically counts opj_* allocations on
  // the test thread. ASan/LSan serves as the leak oracle across the OOM sweeps
  // below.
  ScopedEnvVar unset_threads("OPJ_NUM_THREADS");
  unset_threads.Unset();

  // 1. Codec constructor OOM sweep.
  for (int k = 0; k <= 8; ++k) {
    ScopedOpjAllocFailCountdown fail_at(k);
    OpjCodecPtr enc(opj_create_compress(OPJ_CODEC_J2K));
    (void)enc;
  }
  for (int k = 0; k <= 10; ++k) {
    ScopedOpjAllocFailCountdown fail_at(k);
    OpjCodecPtr dec(opj_create_decompress(OPJ_CODEC_J2K));
    (void)dec;
  }

  // 2. Encoder setup and compression OOM sweeps (with comment, MCT, TLM/PLT).
  OpjImagePtr src = CreateTestImage(16, 16, 3, 8, 0, OPJ_CLRSPC_SRGB);
  ASSERT_NE(src, nullptr);
  for (int k = 0; k <= 25; ++k) {
    OpjImagePtr cloned = CloneTestImage(src.get());
    ASSERT_NE(cloned, nullptr);
    OpjCodecPtr codec(opj_create_compress(OPJ_CODEC_J2K));
    ASSERT_NE(codec, nullptr);

    opj_cparameters_t cp;
    opj_set_default_encoder_parameters(&cp);
    absl::Cleanup free_mct = [&cp] {
      std::free(cp.mct_data);
      cp.mct_data = nullptr;
    };
    cp.tile_size_on = OPJ_TRUE;
    cp.cp_tdx = 8;
    cp.cp_tdy = 8;
    cp.numresolution = 2;
    cp.irreversible = 1;
    std::string comment = "oom_test_comment";
    cp.cp_comment = comment.data();
    std::array<OPJ_FLOAT32, 9> mat = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::array<OPJ_INT32, 3> off = {1, 2, 3};
    ASSERT_EQ(OPJ_TRUE, opj_set_MCT(&cp, mat.data(), off.data(), 3));

    ScopedOpjAllocFailCountdown fail_at(k);
    if (opj_setup_encoder(codec.get(), &cp, cloned.get()) == OPJ_TRUE) {
      (void)opj_encoder_set_extra_options(codec.get(), kTlmPltOpts);
    }
  }

  // Build reference multi-tile and Part-2 codestreams (without fault injection)
  // for the opj_encode and decoder OOM sweeps below.
  // TODO(schwehr): Add opj_start_compress OOM sweep once upstream
  // opj_copy_image_header guards null m_private_image->comps at k == 1.
  std::vector<uint8_t> rich_stream;
  {
    OpjImagePtr cloned = CloneTestImage(src.get());
    ASSERT_NE(nullptr, cloned.get());
    OpjCodecPtr codec(opj_create_compress(OPJ_CODEC_J2K));
    ASSERT_NE(nullptr, codec.get());
    opj_cparameters_t cp = MakeLosslessParams(/*numresolution=*/2,
                                              TileGeometry{.tdx = 8, .tdy = 8});
    cp.roi_compno = 0;
    cp.roi_shift = 2;
    std::string comment = "rich_oom_stream";
    cp.cp_comment = comment.data();
    ASSERT_EQ(OPJ_TRUE, opj_setup_encoder(codec.get(), &cp, cloned.get()));
    ASSERT_EQ(OPJ_TRUE,
              opj_encoder_set_extra_options(codec.get(), kTlmPltOpts));
    ExtractJ2kHandle(codec.get())->m_cp.tcps[0].tccps[1].stepsizes[0].expn += 1;
    ExtractJ2kHandle(codec.get())->m_cp.tcps[0].tccps[1].cblksty ^=
        J2K_CCP_CBLKSTY_RESET;
    MemWriteStream mw;
    OpjStreamPtr s = mw.CreateStream();
    ASSERT_EQ(OPJ_TRUE, opj_start_compress(codec.get(), cloned.get(), s.get()));
    ASSERT_EQ(OPJ_TRUE, opj_encode(codec.get(), s.get()));
    ASSERT_EQ(OPJ_TRUE, opj_end_compress(codec.get(), s.get()));
    s.reset();
    rich_stream = std::move(mw.data);
  }
  // Attach a Part-2 MCT/MCC/MCO header group as well for decode OOM sweeps.
  std::vector<uint8_t> part2_stream;
  {
    auto p = ParseCodestream(rich_stream);
    ASSERT_TRUE(p.has_value());
    std::vector<uint8_t> mct1 = {0, 0, 0x09, 1, 0, 0};
    mct1.resize(6 + 36, 0);
    p->main_markers.push_back(MarkerSegment{J2K_MS_MCT, mct1});
    p->main_markers.push_back(MarkerSegment{
        J2K_MS_MCC,
        {0, 0, 1, 0, 0, 0, 1, 1, 0, 3, 0, 1, 2, 0, 3, 0, 1, 2, 0, 0, 1}});
    p->main_markers.push_back(MarkerSegment{J2K_MS_MCO, {1, 1}});
    part2_stream = BuildCodestream(*p);
  }

  // Sweep the first 3 allocations of opj_encode (j2k.c tile-data buffer and
  // cstr_index updates) without reaching upstream opj_t2_encode_packet() where
  // opj_tgt_create() OOM triggers a null dereference in opj_tgt_setvalue().
  // TODO(schwehr): Sweep higher opj_encode OOM ordinals once opj_tgt_setvalue
  // guards null opj_tgt_create().
  for (int k : {0, 1, 2}) {
    OpjImagePtr cloned = CloneTestImage(src.get());
    ASSERT_NE(cloned, nullptr);
    OpjCodecPtr codec(opj_create_compress(OPJ_CODEC_J2K));
    ASSERT_NE(codec, nullptr);
    opj_cparameters_t cp = MakeLosslessParams(/*numresolution=*/2,
                                              TileGeometry{.tdx = 8, .tdy = 8});
    MemWriteStream mw;
    OpjStreamPtr s = mw.CreateStream();
    ASSERT_NE(s, nullptr);
    ASSERT_EQ(OPJ_TRUE, opj_setup_encoder(codec.get(), &cp, cloned.get()));
    ASSERT_EQ(OPJ_TRUE, opj_start_compress(codec.get(), cloned.get(), s.get()));
    {
      ScopedOpjAllocFailCountdown fail_at(k);
      EXPECT_EQ(OPJ_FALSE, opj_encode(codec.get(), s.get()));
    }
  }

  // 3. Decoder read_header, get_cstr_info, and get_cstr_index OOM sweeps.
  for (int k = 0; k <= 30; ++k) {
    OpjCodecPtr dec(opj_create_decompress(OPJ_CODEC_J2K));
    ASSERT_NE(dec, nullptr);
    opj_dparameters_t dp;
    opj_set_default_decoder_parameters(&dp);
    ASSERT_EQ(OPJ_TRUE, opj_setup_decoder(dec.get(), &dp));
    MemReadStream mem_in(part2_stream);
    OpjStreamPtr s = mem_in.CreateStream();
    ASSERT_NE(s, nullptr);
    opj_image_t* raw = nullptr;
    {
      ScopedOpjAllocFailCountdown fail_at(k);
      (void)opj_read_header(s.get(), dec.get(), &raw);
    }
    opj_image_destroy(raw);
  }
  {
    OpjCodecPtr dec(opj_create_decompress(OPJ_CODEC_J2K));
    ASSERT_NE(nullptr, dec.get());
    opj_dparameters_t dp;
    opj_set_default_decoder_parameters(&dp);
    ASSERT_EQ(OPJ_TRUE, opj_setup_decoder(dec.get(), &dp));
    MemReadStream mem_in(rich_stream);
    OpjStreamPtr s = mem_in.CreateStream();
    opj_image_t* raw = nullptr;
    ASSERT_EQ(OPJ_TRUE, opj_read_header(s.get(), dec.get(), &raw));
    OpjImagePtr img_out(raw);
    ASSERT_EQ(OPJ_TRUE, opj_decode(dec.get(), s.get(), img_out.get()));

    for (int k = 0; k <= 4; ++k) {
      ScopedOpjAllocFailCountdown fail_at(k);
      ScopedCstrInfo info(opj_get_cstr_info(dec.get()));
    }
    // Fail l_cstr_index, marker, tile_index, and tile_index[0].marker
    // (k=0..3); k >= 4 triggers upstream leaks in j2k_get_cstr_index
    // (tile_index[it_tile].marker / tp_index cleanup omits prior tiles).
    // TODO(schwehr): Sweep k >= 4 once j2k_get_cstr_index frees prior tiles.
    for (int k : {0, 1, 2, 3}) {
      ScopedOpjAllocFailCountdown fail_at(k);
      ScopedCstrIndex idx(opj_get_cstr_index(dec.get()));
    }
  }
}

// TODO(schwehr): Cover remaining defensive/unreachable error branches and
// upstream-buggy OOM ordinals in j2k.c.

}  // namespace
}  // namespace openjpeg2
