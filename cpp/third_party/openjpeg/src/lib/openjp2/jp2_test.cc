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
// Tests internal and public OpenJPEG JP2 file-format reader/writer routines
// (src/lib/openjp2/jp2.c), covering box parsing, JP2 header/color/palette/
// channel-definition/codestream boxes, encode/decode round-trips, tile and
// partial-decode dispatch, and stream/validation error paths.
//
// Caveats for google3:
// - USE_JPIP is not defined, so opj_jpip_write_{iptr,cidx,fidx} and related
//   #ifdef USE_JPIP blocks in jp2.c are compiled out; opj_jpip_skip_iptr()
//   remains compiled and emits a zero-filled iptr box when jpip_on = OPJ_TRUE.
// - jp2_dump() delegates directly to j2k_dump(), which emits "Wrong flag\n"
//   when passed OPJ_JP2_INFO or OPJ_JP2_IND.
// - White-box tests inspect opj_jp2_t internals, reinterpret opj_stream_t* to
//   opj_stream_private_t*, pin exact upstream error/warning substrings, and
//   read CIELab parameters smuggled as host-order OPJ_UINT32[9] in
//   icc_profile_buf (with icc_profile_len == 0).
//
// See also:
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/jp2.c
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/jp2.h
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/openjpeg.h

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>  // NOLINT
#include <fstream>
#include <initializer_list>
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
#include "third_party/openjpeg2/google/util/opj_internal_test_util.h"

namespace openjpeg2 {
namespace {

using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;

static_assert(OPJ_TRUE == 1 && OPJ_FALSE == 0);
static_assert(sizeof(opj_jp2_t) > 0);
static_assert(sizeof(opj_jp2_box_t) > 0);
static_assert(sizeof(opj_jp2_cdef_info_t) > 0);
static_assert(sizeof(opj_jp2_cmap_comp_t) > 0);

constexpr uint32_t kDefaultWidth = 8;
constexpr uint32_t kDefaultHeight = 8;
constexpr uint32_t kDefaultNumResolutions = 3;
constexpr uint32_t kJpSignatureMagic = 0x0d0a870aU;
constexpr uint32_t kEnumcsCielab = 14;
constexpr uint32_t kEnumcsSrgb = 16;
constexpr uint32_t kEnumcsGray = 17;
constexpr uint32_t kEnumcsSycc = 18;

struct OpjJp2Deleter {
  void operator()(opj_jp2_t* jp2) const { opj_jp2_destroy(jp2); }
};
using OpjJp2Ptr = std::unique_ptr<opj_jp2_t, OpjJp2Deleter>;

opj_stream_private_t* AsPrivate(const OpjStreamPtr& stream) {
  return reinterpret_cast<opj_stream_private_t*>(stream.get());
}

opj_cparameters_t MakeJp2LosslessParams(
    bool tiled = false, uint32_t tile_w = 8, uint32_t tile_h = 8,
    uint32_t numresolutions = kDefaultNumResolutions) {
  std::optional<TileGeometry> tile_geom;
  if (tiled) {
    tile_geom = TileGeometry{.tdx = tile_w, .tdy = tile_h};
  }
  opj_cparameters_t params = MakeLosslessParams(numresolutions, tile_geom);
  params.tcp_mct = 0;
  return params;
}

std::unique_ptr<DecodeSession> OpenJp2ForDecode(
    std::span<const uint8_t> encoded, OPJ_CODEC_FORMAT format,
    EventCapture* events = nullptr, OPJ_UINT32 dparam_flags = 0,
    size_t stream_buffer_size = OPJ_J2K_STREAM_CHUNK_SIZE) {
  if (stream_buffer_size == OPJ_J2K_STREAM_CHUNK_SIZE) {
    return OpenForDecode(
        encoded, format,
        {.events = events,
         .ignore_pclr_cmap_cdef =
             (dparam_flags & OPJ_DPARAMETERS_IGNORE_PCLR_CMAP_CDEF_FLAG) != 0});
  }
  auto session = std::make_unique<DecodeSession>(encoded);
  session->stream = session->mem_in.CreateStream(stream_buffer_size);
  if (!session->stream) {
    return nullptr;
  }
  session->codec.reset(opj_create_decompress(format));
  if (!session->codec) {
    return nullptr;
  }
  if (events != nullptr) {
    events->AttachTo(session->codec.get());
  }
  opj_dparameters_t dparams;
  opj_set_default_decoder_parameters(&dparams);
  dparams.flags = dparam_flags;
  if (opj_setup_decoder(session->codec.get(), &dparams) != OPJ_TRUE) {
    return nullptr;
  }
  opj_image_t* raw_image = nullptr;
  if (opj_read_header(session->stream.get(), session->codec.get(),
                      &raw_image) != OPJ_TRUE ||
      raw_image == nullptr) {
    opj_image_destroy(raw_image);
    return nullptr;
  }
  session->image.reset(raw_image);
  return session;
}

// Attempts JP2 header read (and optionally decode + end_decompress) on
// synthetic box bytes without fatal assertions, returning whether the full
// requested sequence succeeded.
bool TryDecodeJp2(std::span<const uint8_t> jp2_bytes, bool decode_image,
                  EventCapture* events = nullptr,
                  OpjImagePtr* out_image = nullptr, OPJ_UINT32 dparam_flags = 0,
                  size_t stream_buffer_size = OPJ_J2K_STREAM_CHUNK_SIZE,
                  std::optional<uint64_t> override_stream_length = std::nullopt,
                  std::optional<size_t> max_read_bytes = std::nullopt) {
  MemReadStream mem_stream(jp2_bytes);
  if (max_read_bytes.has_value()) {
    mem_stream.max_bytes_before_fail = *max_read_bytes;
  }
  OpjStreamPtr stream = mem_stream.CreateStream(stream_buffer_size);
  if (!stream) {
    return false;
  }
  if (override_stream_length.has_value()) {
    opj_stream_set_user_data_length(stream.get(), *override_stream_length);
  }
  OpjCodecPtr codec(opj_create_decompress(OPJ_CODEC_JP2));
  if (!codec) {
    return false;
  }
  if (events != nullptr) {
    events->AttachTo(codec.get());
  }
  opj_dparameters_t dparams;
  opj_set_default_decoder_parameters(&dparams);
  dparams.flags = dparam_flags;
  if (opj_setup_decoder(codec.get(), &dparams) != OPJ_TRUE) {
    return false;
  }
  opj_image_t* raw_image = nullptr;
  const OPJ_BOOL header_ok =
      opj_read_header(stream.get(), codec.get(), &raw_image);
  OpjImagePtr image(raw_image);
  if (header_ok != OPJ_TRUE || !image) {
    return false;
  }
  if (decode_image) {
    if (opj_decode(codec.get(), stream.get(), image.get()) != OPJ_TRUE) {
      return false;
    }
    if (opj_end_decompress(codec.get(), stream.get()) != OPJ_TRUE) {
      return false;
    }
  }
  if (out_image != nullptr) {
    *out_image = std::move(image);
  }
  return true;
}

// Synthesizes JP2 boxes and nested box streams in memory.
class Jp2BoxBuilder {
 public:
  struct CmapEntry {
    uint16_t cmp = 0;
    uint8_t mtyp = 0;
    uint8_t pcol = 0;
  };

  struct CdefEntry {
    uint16_t cn = 0;
    uint16_t typ = 0;
    uint16_t asoc = 0;
  };

  static constexpr uint32_t FourCC(std::string_view tag) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(tag[0])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(tag[1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(tag[2])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(tag[3]));
  }

  static void AppendU8(std::vector<uint8_t>& out, uint8_t value) {
    out.push_back(value);
  }

  static void AppendU16BE(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
    out.push_back(static_cast<uint8_t>(value & 0xFFU));
  }

  static void AppendU32BE(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFU));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFU));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
    out.push_back(static_cast<uint8_t>(value & 0xFFU));
  }

  static void AppendU64BE(std::vector<uint8_t>& out, uint64_t value) {
    AppendU32BE(out, static_cast<uint32_t>((value >> 32) & 0xFFFFFFFFULL));
    AppendU32BE(out, static_cast<uint32_t>(value & 0xFFFFFFFFULL));
  }

  static void AppendBytes(std::vector<uint8_t>& out,
                          std::span<const uint8_t> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
  }

  static std::vector<uint8_t> MakeBox(
      uint32_t type, std::span<const uint8_t> payload,
      std::optional<uint32_t> override_length = std::nullopt) {
    std::vector<uint8_t> box;
    const uint32_t length =
        override_length.value_or(static_cast<uint32_t>(8 + payload.size()));
    AppendU32BE(box, length);
    AppendU32BE(box, type);
    AppendBytes(box, payload);
    return box;
  }

  static std::vector<uint8_t> MakeXLBox(
      uint32_t type, std::span<const uint8_t> payload,
      std::optional<uint64_t> override_xl_size = std::nullopt) {
    std::vector<uint8_t> box;
    const uint64_t length =
        override_xl_size.value_or(static_cast<uint64_t>(16 + payload.size()));
    AppendU32BE(box, 1U);
    AppendU32BE(box, type);
    AppendU64BE(box, length);
    AppendBytes(box, payload);
    return box;
  }

  static std::vector<uint8_t> MakeEofBox(uint32_t type,
                                         std::span<const uint8_t> payload) {
    return MakeBox(type, payload, /*override_length=*/0U);
  }

  static std::vector<uint8_t> MakeJpBox(uint32_t magic = kJpSignatureMagic) {
    std::vector<uint8_t> payload;
    AppendU32BE(payload, magic);
    return MakeBox(FourCC("jP  "), payload);
  }

  static std::vector<uint8_t> MakeFtypBox(
      uint32_t brand = FourCC("jp2 "), uint32_t minversion = 0,
      std::span<const uint32_t> compat_list = {}) {
    std::vector<uint8_t> payload;
    AppendU32BE(payload, brand);
    AppendU32BE(payload, minversion);
    for (uint32_t cl : compat_list) {
      AppendU32BE(payload, cl);
    }
    return MakeBox(FourCC("ftyp"), payload);
  }

  static std::vector<uint8_t> MakeIhdrPayload(uint32_t w, uint32_t h,
                                              uint16_t numcomps, uint8_t bpc,
                                              uint8_t compression = 7,
                                              uint8_t unkc = 0,
                                              uint8_t ipr = 0) {
    std::vector<uint8_t> p;
    AppendU32BE(p, h);
    AppendU32BE(p, w);
    AppendU16BE(p, numcomps);
    AppendU8(p, bpc);
    AppendU8(p, compression);
    AppendU8(p, unkc);
    AppendU8(p, ipr);
    return p;
  }

  static std::vector<uint8_t> MakeColrEnumPayload(uint32_t enumcs,
                                                  uint8_t precedence = 0,
                                                  uint8_t approx = 0) {
    std::vector<uint8_t> p;
    AppendU8(p, 1U);
    AppendU8(p, precedence);
    AppendU8(p, approx);
    AppendU32BE(p, enumcs);
    return p;
  }

  static std::vector<uint8_t> MakeColrCielabPayload(uint32_t rl, uint32_t ol,
                                                    uint32_t ra, uint32_t oa,
                                                    uint32_t rb, uint32_t ob,
                                                    uint32_t il) {
    std::vector<uint8_t> p = MakeColrEnumPayload(14U);
    AppendU32BE(p, rl);
    AppendU32BE(p, ol);
    AppendU32BE(p, ra);
    AppendU32BE(p, oa);
    AppendU32BE(p, rb);
    AppendU32BE(p, ob);
    AppendU32BE(p, il);
    return p;
  }

  static std::vector<uint8_t> MakeColrIccPayload(uint8_t meth,
                                                 std::span<const uint8_t> icc,
                                                 uint8_t precedence = 0,
                                                 uint8_t approx = 0) {
    std::vector<uint8_t> p;
    AppendU8(p, meth);
    AppendU8(p, precedence);
    AppendU8(p, approx);
    AppendBytes(p, icc);
    return p;
  }

  static std::vector<uint8_t> MakePclrPayload(
      uint16_t ne, std::span<const uint8_t> col_bi,
      std::span<const uint32_t> entries) {
    std::vector<uint8_t> p;
    AppendU16BE(p, ne);
    AppendU8(p, static_cast<uint8_t>(col_bi.size()));
    for (uint8_t bi : col_bi) {
      AppendU8(p, bi);
    }
    const size_t npc = col_bi.size();
    for (size_t row = 0; row < ne; ++row) {
      for (size_t col = 0; col < npc; ++col) {
        const uint32_t bits = (col_bi[col] & 0x7FU) + 1U;
        const uint32_t nbytes = std::min((bits + 7U) >> 3U, 4U);
        const uint32_t val = entries[row * npc + col];
        for (uint32_t b = 0; b < nbytes; ++b) {
          const uint32_t shift = (nbytes - 1U - b) * 8U;
          AppendU8(p, static_cast<uint8_t>((val >> shift) & 0xFFU));
        }
      }
    }
    return p;
  }

  static std::vector<uint8_t> MakeCmapPayload(
      std::span<const CmapEntry> entries) {
    std::vector<uint8_t> p;
    for (const auto& entry : entries) {
      AppendU16BE(p, entry.cmp);
      AppendU8(p, entry.mtyp);
      AppendU8(p, entry.pcol);
    }
    return p;
  }

  static std::vector<uint8_t> MakeCdefPayload(
      std::span<const CdefEntry> entries) {
    std::vector<uint8_t> p;
    AppendU16BE(p, static_cast<uint16_t>(entries.size()));
    for (const auto& entry : entries) {
      AppendU16BE(p, entry.cn);
      AppendU16BE(p, entry.typ);
      AppendU16BE(p, entry.asoc);
    }
    return p;
  }

  static std::vector<uint8_t> Concat(
      std::initializer_list<std::span<const uint8_t>> chunks) {
    std::vector<uint8_t> out;
    for (const auto chunk : chunks) {
      AppendBytes(out, chunk);
    }
    return out;
  }
};

struct CompSpec {
  uint32_t prec = 8;
  uint32_t sgnd = 0;
  uint32_t dx = 1;
  uint32_t dy = 1;
};

// Encodes a deterministic lossless J2K codestream with per-component specs and
// optional explicit sample values (indexed as [comp][pixel]).
std::vector<uint8_t> EncodeJ2kWithSpecs(
    uint32_t width, uint32_t height, std::span<const CompSpec> specs,
    std::span<const std::vector<OPJ_INT32>> custom_samples = {}) {
  std::vector<opj_image_cmptparm_t> cmpt_params(specs.size());
  for (size_t c = 0; c < specs.size(); ++c) {
    cmpt_params[c] = opj_image_cmptparm_t{
        .dx = specs[c].dx,
        .dy = specs[c].dy,
        .w = (width + specs[c].dx - 1U) / specs[c].dx,
        .h = (height + specs[c].dy - 1U) / specs[c].dy,
        .x0 = 0,
        .y0 = 0,
        .prec = specs[c].prec,
        .sgnd = specs[c].sgnd,
    };
  }
  OpjImagePtr image(opj_image_create(static_cast<OPJ_UINT32>(specs.size()),
                                     cmpt_params.data(),
                                     OPJ_CLRSPC_UNSPECIFIED));
  if (!image) {
    ADD_FAILURE() << "opj_image_create failed";
    return {};
  }
  image->x0 = 0;
  image->y0 = 0;
  image->x1 = width;
  image->y1 = height;
  for (size_t c = 0; c < specs.size(); ++c) {
    const uint32_t cw = image->comps[c].w;
    const uint32_t ch = image->comps[c].h;
    const size_t count = static_cast<size_t>(cw) * ch;
    if (!custom_samples.empty() && c < custom_samples.size()) {
      for (size_t i = 0; i < count; ++i) {
        image->comps[c].data[i] =
            custom_samples[c][i % custom_samples[c].size()];
      }
    } else {
      const uint64_t mask = (1ULL << specs[c].prec) - 1ULL;
      const int64_t bias =
          specs[c].sgnd ? static_cast<int64_t>(1ULL << (specs[c].prec - 1U))
                        : 0;
      for (uint32_t y = 0; y < ch; ++y) {
        for (uint32_t x = 0; x < cw; ++x) {
          const uint64_t raw = (x * 17ULL + y * 31ULL + c * 41ULL) & mask;
          image->comps[c].data[y * cw + x] =
              static_cast<OPJ_INT32>(static_cast<int64_t>(raw) - bias);
        }
      }
    }
  }

  opj_cparameters_t cparams = MakeJp2LosslessParams();
  std::optional<std::vector<uint8_t>> j2k =
      CompressImageToBytes(image.get(), OPJ_CODEC_J2K, &cparams);
  if (!j2k.has_value()) {
    ADD_FAILURE() << "CompressImageToBytes failed";
    return {};
  }
  return *j2k;
}

std::vector<uint8_t> BuildStandardJp2(std::span<const uint8_t> jp2h_payload,
                                      std::span<const uint8_t> j2k_codestream) {
  const auto jp = Jp2BoxBuilder::MakeJpBox();
  constexpr std::array<uint32_t, 1> kCompat = {Jp2BoxBuilder::FourCC("jp2 ")};
  const auto ftyp =
      Jp2BoxBuilder::MakeFtypBox(Jp2BoxBuilder::FourCC("jp2 "), 0, kCompat);
  const auto jp2h =
      Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("jp2h"), jp2h_payload);
  const auto jp2c =
      Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("jp2c"), j2k_codestream);
  return Jp2BoxBuilder::Concat({jp, ftyp, jp2h, jp2c});
}

std::vector<OPJ_BYTE> MakeTilePayload(uint32_t tile_index, uint32_t tile_w,
                                      uint32_t tile_h, uint32_t numcomps,
                                      uint8_t max_val = 255) {
  const size_t samples_per_comp = static_cast<size_t>(tile_w) * tile_h;
  std::vector<OPJ_BYTE> bytes(samples_per_comp * numcomps);
  for (uint32_t c = 0; c < numcomps; ++c) {
    for (uint32_t y = 0; y < tile_h; ++y) {
      for (uint32_t x = 0; x < tile_w; ++x) {
        const uint32_t raw =
            (tile_index * 47U + c * 31U + y * 13U + x * 7U) & 0xFFU;
        bytes[c * samples_per_comp + y * tile_w + x] =
            static_cast<OPJ_BYTE>(raw % (static_cast<uint32_t>(max_val) + 1U));
      }
    }
  }
  return bytes;
}

std::vector<uint8_t> EncodeTiled16x16Jp2(
    uint32_t numcomps, OPJ_COLOR_SPACE color_space, OPJ_CODEC_FORMAT format,
    uint8_t max_val = 255, const char* const* extra_opts = nullptr) {
  constexpr uint32_t kWidth = 16;
  constexpr uint32_t kHeight = 16;
  constexpr uint32_t kTileW = 8;
  constexpr uint32_t kTileH = 8;

  std::vector<opj_image_cmptparm_t> cmpt_params(numcomps);
  for (auto& param : cmpt_params) {
    param.dx = 1;
    param.dy = 1;
    param.w = kWidth;
    param.h = kHeight;
    param.prec = 8;
    param.sgnd = 0;
  }
  OpjImagePtr image(
      opj_image_tile_create(numcomps, cmpt_params.data(), color_space));
  if (!image) {
    ADD_FAILURE() << "opj_image_tile_create failed";
    return {};
  }
  image->x0 = 0;
  image->y0 = 0;
  image->x1 = kWidth;
  image->y1 = kHeight;

  opj_cparameters_t cparams =
      MakeJp2LosslessParams(/*tiled=*/true, kTileW, kTileH);
  OpjCodecPtr codec(opj_create_compress(format));
  if (!codec) {
    ADD_FAILURE() << "opj_create_compress failed";
    return {};
  }
  if (opj_setup_encoder(codec.get(), &cparams, image.get()) != OPJ_TRUE) {
    ADD_FAILURE() << "opj_setup_encoder failed";
    return {};
  }
  if (extra_opts != nullptr &&
      opj_encoder_set_extra_options(codec.get(), extra_opts) != OPJ_TRUE) {
    ADD_FAILURE() << "opj_encoder_set_extra_options failed";
    return {};
  }

  MemWriteStream mem_out;
  OpjStreamPtr stream = mem_out.CreateStream();
  if (!stream) {
    ADD_FAILURE() << "CreateStream failed";
    return {};
  }
  if (opj_start_compress(codec.get(), image.get(), stream.get()) != OPJ_TRUE) {
    ADD_FAILURE() << "opj_start_compress failed";
    return {};
  }
  constexpr uint32_t kNumTiles = 4;
  for (uint32_t tile_idx = 0; tile_idx < kNumTiles; ++tile_idx) {
    std::vector<OPJ_BYTE> payload =
        MakeTilePayload(tile_idx, kTileW, kTileH, numcomps, max_val);
    if (opj_write_tile(codec.get(), tile_idx, payload.data(),
                       static_cast<OPJ_UINT32>(payload.size()),
                       stream.get()) != OPJ_TRUE) {
      ADD_FAILURE() << "opj_write_tile failed";
      return {};
    }
  }
  if (opj_end_compress(codec.get(), stream.get()) != OPJ_TRUE) {
    ADD_FAILURE() << "opj_end_compress failed";
    return {};
  }
  stream.reset();
  return mem_out.data;
}

TEST(Jp2Test, LifecycleCreateDestroyAndInvalidArgs) {
  opj_jp2_destroy(nullptr);

  OpjJp2Ptr dec(opj_jp2_create(OPJ_TRUE));
  ASSERT_NE(nullptr, dec);
  EXPECT_NE(nullptr, dec->j2k);
  EXPECT_NE(nullptr, dec->m_validation_list);
  EXPECT_NE(nullptr, dec->m_procedure_list);

  OpjJp2Ptr enc(opj_jp2_create(OPJ_FALSE));
  ASSERT_NE(nullptr, enc);
  EXPECT_NE(nullptr, enc->j2k);

  // Null-pointer guards in opj_jp2_setup_encoder.
  OpjImagePtr img = CreateTestImage(kDefaultWidth, kDefaultHeight, 3);
  ASSERT_NE(nullptr, img);
  opj_cparameters_t cparams = MakeJp2LosslessParams();
  EventCapture events;
  opj_event_mgr_t mgr = MakeEventManager(&events);

  EXPECT_EQ(OPJ_FALSE,
            opj_jp2_setup_encoder(nullptr, &cparams, img.get(), &mgr));
  EXPECT_EQ(OPJ_FALSE,
            opj_jp2_setup_encoder(enc.get(), nullptr, img.get(), &mgr));
  EXPECT_EQ(OPJ_FALSE,
            opj_jp2_setup_encoder(enc.get(), &cparams, nullptr, &mgr));

  // Invalid component count (0 and > 16384).
  opj_image_t zero_comp_img{};
  zero_comp_img.numcomps = 0;
  EXPECT_EQ(OPJ_FALSE,
            opj_jp2_setup_encoder(enc.get(), &cparams, &zero_comp_img, &mgr));
  opj_image_t huge_comp_img{};
  huge_comp_img.numcomps = 16385;
  EXPECT_EQ(OPJ_FALSE,
            opj_jp2_setup_encoder(enc.get(), &cparams, &huge_comp_img, &mgr));
  EXPECT_THAT(events.errors, Not(IsEmpty()));

  // Underlying opj_j2k_setup_encoder failure propagates cleanly.
  opj_cparameters_t bad_cparams = MakeJp2LosslessParams(/*tiled=*/true, 0, 8);
  EXPECT_EQ(OPJ_FALSE,
            opj_jp2_setup_encoder(enc.get(), &bad_cparams, img.get(), &mgr));

  // Null image arguments to opj_jp2_decode and opj_jp2_get_tile fail cleanly.
  MemReadStream empty_in({});
  OpjStreamPtr in_stream = empty_in.CreateStream();
  ASSERT_NE(nullptr, in_stream);
  EXPECT_EQ(OPJ_FALSE,
            opj_jp2_decode(dec.get(), AsPrivate(in_stream), nullptr, &mgr));
  EXPECT_EQ(OPJ_FALSE, opj_jp2_get_tile(dec.get(), AsPrivate(in_stream),
                                        nullptr, &mgr, 0));
}

// Note: MakeJp2LosslessParams sets tcp_mct = 0, so RGB round-trips validate
// color-space box metadata without enabling RCT/ICT.
TEST(Jp2Test, ColorSpaceEncodeDecodeRoundTrips) {
  struct ColorSpaceCase {
    OPJ_COLOR_SPACE input_cs;
    OPJ_COLOR_SPACE expected_cs;
    uint32_t numcomps;
    std::string_view label;
  };
  constexpr ColorSpaceCase kCases[] = {
      {.input_cs = OPJ_CLRSPC_SRGB,
       .expected_cs = OPJ_CLRSPC_SRGB,
       .numcomps = 3,
       .label = "sRGB"},
      {.input_cs = OPJ_CLRSPC_GRAY,
       .expected_cs = OPJ_CLRSPC_GRAY,
       .numcomps = 1,
       .label = "Gray"},
      {.input_cs = OPJ_CLRSPC_SYCC,
       .expected_cs = OPJ_CLRSPC_SYCC,
       .numcomps = 3,
       .label = "sYCC"},
      {.input_cs = OPJ_CLRSPC_EYCC,
       .expected_cs = OPJ_CLRSPC_EYCC,
       .numcomps = 3,
       .label = "eYCC"},
      {.input_cs = OPJ_CLRSPC_CMYK,
       .expected_cs = OPJ_CLRSPC_CMYK,
       .numcomps = 4,
       .label = "CMYK"},
      {.input_cs = OPJ_CLRSPC_UNSPECIFIED,
       .expected_cs = OPJ_CLRSPC_UNKNOWN,
       .numcomps = 1,
       .label = "Unspecified"},
  };

  for (const auto& tc : kCases) {
    SCOPED_TRACE(std::string(tc.label));
    // 1. Create synthetic test image with target color space.
    OpjImagePtr src =
        CreateTestImage(kDefaultWidth, kDefaultHeight, tc.numcomps, /*prec=*/8,
                        /*sgnd=*/0, tc.input_cs);
    ASSERT_NE(nullptr, src);
    const auto expected_pixels = SnapshotImagePixels(src.get());

    // 2. Encode as JP2 in memory.
    opj_cparameters_t cparams = MakeJp2LosslessParams();
    std::optional<std::vector<uint8_t>> jp2 =
        CompressImageToBytes(src.get(), OPJ_CODEC_JP2, &cparams);
    ASSERT_TRUE(jp2.has_value());

    // 3. Decode JP2 and verify exact pixels and mapped color space.
    std::unique_ptr<DecodeSession> session = OpenForDecode(*jp2, OPJ_CODEC_JP2);
    ASSERT_NE(nullptr, session);
    EXPECT_EQ(tc.expected_cs, session->image->color_space);
    ASSERT_EQ(OPJ_TRUE, opj_decode(session->codec.get(), session->stream.get(),
                                   session->image.get()));
    ASSERT_EQ(OPJ_TRUE,
              opj_end_decompress(session->codec.get(), session->stream.get()));
    ExpectImagePixelsEqual(expected_pixels, session->image.get());
  }
}

TEST(Jp2Test, IccProfileAndLargeJp2hReallocRoundTrip) {
  // Exceed OPJ_BOX_SIZE (1024) inside jp2h so opj_jp2_read_header_procedure
  // exercises its opj_realloc() buffer expansion path.
  constexpr size_t kProfileSizes[] = {16U, 1400U};
  for (size_t icc_len : kProfileSizes) {
    SCOPED_TRACE(icc_len);
    OpjImagePtr src = CreateTestImage(kDefaultWidth, kDefaultHeight, 3, 8, 0,
                                      OPJ_CLRSPC_SRGB);
    ASSERT_NE(nullptr, src);
    const auto expected_pixels = SnapshotImagePixels(src.get());

    std::vector<OPJ_BYTE> icc_bytes(icc_len);
    for (size_t i = 0; i < icc_len; ++i) {
      icc_bytes[i] = static_cast<OPJ_BYTE>((i * 29U + 7U) & 0xFFU);
    }
    src->icc_profile_buf = static_cast<OPJ_BYTE*>(opj_malloc(icc_len));
    ASSERT_NE(nullptr, src->icc_profile_buf);
    std::memcpy(src->icc_profile_buf, icc_bytes.data(), icc_len);
    src->icc_profile_len = static_cast<OPJ_UINT32>(icc_len);

    opj_cparameters_t cparams = MakeJp2LosslessParams();
    std::optional<std::vector<uint8_t>> jp2 =
        CompressImageToBytes(src.get(), OPJ_CODEC_JP2, &cparams);
    ASSERT_TRUE(jp2.has_value());

    std::unique_ptr<DecodeSession> session = OpenForDecode(*jp2, OPJ_CODEC_JP2);
    ASSERT_NE(nullptr, session);
    EXPECT_EQ(OPJ_CLRSPC_UNKNOWN, session->image->color_space);
    EXPECT_EQ(icc_len, session->image->icc_profile_len);
    ASSERT_NE(nullptr, session->image->icc_profile_buf);
    EXPECT_THAT(
        std::span<const uint8_t>(session->image->icc_profile_buf, icc_len),
        ElementsAreArray(icc_bytes));
    ASSERT_EQ(OPJ_TRUE, opj_decode(session->codec.get(), session->stream.get(),
                                   session->image.get()));
    ASSERT_EQ(OPJ_TRUE,
              opj_end_decompress(session->codec.get(), session->stream.get()));
    ExpectImagePixelsEqual(expected_pixels, session->image.get());
  }
}

TEST(Jp2Test, MixedBitDepthBpccRoundTripAndErrors) {
  // 1. Encode 3 components with different bit depths/signedness so encoder
  // sets ihdr.bpc = 255 and emits a bpcc box.
  constexpr std::array<CompSpec, 3> kSpecs = {{
      {.prec = 8, .sgnd = 0},
      {.prec = 10, .sgnd = 1},
      {.prec = 12, .sgnd = 0},
  }};
  std::array<opj_image_cmptparm_t, 3> cmpt_params{};
  for (size_t c = 0; c < kSpecs.size(); ++c) {
    cmpt_params[c] = opj_image_cmptparm_t{
        .dx = 1,
        .dy = 1,
        .w = kDefaultWidth,
        .h = kDefaultHeight,
        .x0 = 0,
        .y0 = 0,
        .prec = kSpecs[c].prec,
        .sgnd = kSpecs[c].sgnd,
    };
  }
  OpjImagePtr src(opj_image_create(3, cmpt_params.data(), OPJ_CLRSPC_SRGB));
  ASSERT_NE(nullptr, src);
  src->x0 = 0;
  src->y0 = 0;
  src->x1 = kDefaultWidth;
  src->y1 = kDefaultHeight;
  for (size_t c = 0; c < 3; ++c) {
    const int64_t bias = kSpecs[c].sgnd ? (1LL << (kSpecs[c].prec - 1U)) : 0;
    const uint64_t mask = (1ULL << kSpecs[c].prec) - 1ULL;
    for (uint32_t i = 0; i < kDefaultWidth * kDefaultHeight; ++i) {
      const int64_t val =
          static_cast<int64_t>((i * 19U + c * 7U) & mask) - bias;
      src->comps[c].data[i] = static_cast<OPJ_INT32>(val);
    }
  }
  const auto expected_pixels = SnapshotImagePixels(src.get());

  opj_cparameters_t cparams = MakeJp2LosslessParams();
  std::optional<std::vector<uint8_t>> jp2 =
      CompressImageToBytes(src.get(), OPJ_CODEC_JP2, &cparams);
  ASSERT_TRUE(jp2.has_value());

  std::unique_ptr<DecodeSession> session = OpenForDecode(*jp2, OPJ_CODEC_JP2);
  ASSERT_NE(nullptr, session);
  ASSERT_EQ(OPJ_TRUE, opj_decode(session->codec.get(), session->stream.get(),
                                 session->image.get()));
  ASSERT_EQ(OPJ_TRUE,
            opj_end_decompress(session->codec.get(), session->stream.get()));
  ASSERT_EQ(3u, session->image->numcomps);
  for (size_t c = 0; c < 3; ++c) {
    EXPECT_EQ(kSpecs[c].prec, session->image->comps[c].prec);
    EXPECT_EQ(kSpecs[c].sgnd, session->image->comps[c].sgnd);
  }
  ExpectImagePixelsEqual(expected_pixels, session->image.get());

  // 2. Synthesize JP2 with uniform ihdr.bpc != 255 AND a bpcc box -> warns.
  const std::vector<uint8_t> j2k =
      EncodeJ2kWithSpecs(kDefaultWidth, kDefaultHeight, kSpecs);
  ASSERT_FALSE(j2k.empty());
  {
    const auto ihdr = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("ihdr"),
        Jp2BoxBuilder::MakeIhdrPayload(kDefaultWidth, kDefaultHeight, 3,
                                       /*bpc=*/7));
    constexpr std::array<uint8_t, 3> kBpccVals = {7, 0x89, 11};
    const auto bpcc =
        Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("bpcc"), kBpccVals);
    const auto colr =
        Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("colr"),
                               Jp2BoxBuilder::MakeColrEnumPayload(kEnumcsSrgb));
    const auto jp2_warn =
        BuildStandardJp2(Jp2BoxBuilder::Concat({ihdr, bpcc, colr}), j2k);

    EventCapture events;
    OpjImagePtr decoded;
    EXPECT_TRUE(
        TryDecodeJp2(jp2_warn, /*decode_image=*/true, &events, &decoded));
    EXPECT_THAT(events.warnings,
                Contains(HasSubstr("BPCC header box is available")));
  }

  // 3. Mixed precision + alpha triggers both bpcc and cdef in write_jp2h.
  {
    std::array<opj_image_cmptparm_t, 2> ga_params = {
        opj_image_cmptparm_t{.dx = 1,
                             .dy = 1,
                             .w = kDefaultWidth,
                             .h = kDefaultHeight,
                             .x0 = 0,
                             .y0 = 0,
                             .prec = 8,
                             .sgnd = 0},
        opj_image_cmptparm_t{.dx = 1,
                             .dy = 1,
                             .w = kDefaultWidth,
                             .h = kDefaultHeight,
                             .x0 = 0,
                             .y0 = 0,
                             .prec = 10,
                             .sgnd = 0},
    };
    OpjImagePtr ga(opj_image_create(2, ga_params.data(), OPJ_CLRSPC_GRAY));
    ASSERT_NE(nullptr, ga);
    ga->x1 = kDefaultWidth;
    ga->y1 = kDefaultHeight;
    ga->comps[1].alpha = 1;
    for (uint32_t i = 0; i < kDefaultWidth * kDefaultHeight; ++i) {
      ga->comps[0].data[i] = static_cast<OPJ_INT32>((i * 5U) & 0xFFU);
      ga->comps[1].data[i] = static_cast<OPJ_INT32>((i * 11U) & 0x3FFU);
    }
    const auto expected_ga = SnapshotImagePixels(ga.get());
    std::optional<std::vector<uint8_t>> ga_jp2 =
        CompressImageToBytes(ga.get(), OPJ_CODEC_JP2, &cparams);
    ASSERT_TRUE(ga_jp2.has_value());
    OpjImagePtr decoded_ga;
    ASSERT_TRUE(TryDecodeJp2(*ga_jp2, true, nullptr, &decoded_ga));
    ExpectImagePixelsEqual(expected_ga, decoded_ga.get());
    EXPECT_EQ(1u, decoded_ga->comps[1].alpha);
  }

  // 4. bpcc size != numcomps (2 bytes for 3 components, or 4 bytes) fails.
  for (size_t bad_len : {2U, 4U}) {
    const auto ihdr = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("ihdr"),
        Jp2BoxBuilder::MakeIhdrPayload(kDefaultWidth, kDefaultHeight, 3, 255));
    std::vector<uint8_t> bad_bpcc_payload(bad_len, 7U);
    const auto bpcc =
        Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("bpcc"), bad_bpcc_payload);
    const auto jp2_bad =
        BuildStandardJp2(Jp2BoxBuilder::Concat({ihdr, bpcc}), j2k);

    EventCapture events;
    EXPECT_FALSE(TryDecodeJp2(jp2_bad, /*decode_image=*/false, &events));
    EXPECT_THAT(events.errors, Contains(HasSubstr("Bad BPCC header box")));
  }
}

TEST(Jp2Test, AlphaChannelAutoCdefEncodeAndSetupWarnings) {
  struct AutoCdefCase {
    OPJ_COLOR_SPACE color_space;
    uint32_t numcomps;
    uint32_t alpha_index;
    std::string_view label;
  };
  constexpr AutoCdefCase kValidCases[] = {
      {.color_space = OPJ_CLRSPC_SRGB,
       .numcomps = 4,
       .alpha_index = 3,
       .label = "RGBA"},
      {.color_space = OPJ_CLRSPC_SYCC,
       .numcomps = 4,
       .alpha_index = 3,
       .label = "sYCCA"},
      {.color_space = OPJ_CLRSPC_GRAY,
       .numcomps = 2,
       .alpha_index = 1,
       .label = "GrayAlpha"},
      // Extra component between gray and alpha hits unspecified (65535) cdef.
      {.color_space = OPJ_CLRSPC_GRAY,
       .numcomps = 3,
       .alpha_index = 2,
       .label = "GrayUnknownAlpha"},
  };

  for (const auto& tc : kValidCases) {
    SCOPED_TRACE(std::string(tc.label));
    OpjImagePtr src = CreateTestImage(kDefaultWidth, kDefaultHeight,
                                      tc.numcomps, 8, 0, tc.color_space);
    ASSERT_NE(nullptr, src);
    src->comps[tc.alpha_index].alpha = 1;
    const auto expected_pixels = SnapshotImagePixels(src.get());

    opj_cparameters_t cparams = MakeJp2LosslessParams();
    std::optional<std::vector<uint8_t>> jp2 =
        CompressImageToBytes(src.get(), OPJ_CODEC_JP2, &cparams);
    ASSERT_TRUE(jp2.has_value());

    std::unique_ptr<DecodeSession> session = OpenForDecode(*jp2, OPJ_CODEC_JP2);
    ASSERT_NE(nullptr, session);
    ASSERT_EQ(OPJ_TRUE, opj_decode(session->codec.get(), session->stream.get(),
                                   session->image.get()));
    ASSERT_EQ(OPJ_TRUE,
              opj_end_decompress(session->codec.get(), session->stream.get()));
    ExpectImagePixelsEqual(expected_pixels, session->image.get());
    EXPECT_EQ(1u, session->image->comps[tc.alpha_index].alpha);
    EXPECT_EQ(0u, session->image->comps[0].alpha);
    if (tc.color_space == OPJ_CLRSPC_GRAY && tc.numcomps == 3) {
      EXPECT_EQ(65535u, session->image->comps[1].alpha);
    }
  }

  // Exercise all 4 warning paths in opj_jp2_setup_encoder cdef generation.
  struct WarningCase {
    OPJ_COLOR_SPACE color_space;
    uint32_t numcomps;
    uint32_t alpha_mask;
    std::string_view expected_substr;
  };
  constexpr WarningCase kWarningCases[] = {
      {.color_space = OPJ_CLRSPC_CMYK,
       .numcomps = 5,
       .alpha_mask = (1U << 4),
       .expected_substr = "unknown enumcs"},
      {.color_space = OPJ_CLRSPC_SRGB,
       .numcomps = 3,
       .alpha_mask = (1U << 2),
       .expected_substr = "not enough image components"},
      {.color_space = OPJ_CLRSPC_SRGB,
       .numcomps = 4,
       .alpha_mask = (1U << 0),
       .expected_substr = "conflicts with color channel"},
      {.color_space = OPJ_CLRSPC_SRGB,
       .numcomps = 4,
       .alpha_mask = (1U << 2) | (1U << 3),
       .expected_substr = "Multiple alpha channels"},
  };
  for (const auto& tc : kWarningCases) {
    SCOPED_TRACE(std::string(tc.expected_substr));
    OpjImagePtr src = CreateTestImage(kDefaultWidth, kDefaultHeight,
                                      tc.numcomps, 8, 0, tc.color_space);
    ASSERT_NE(nullptr, src);
    for (uint32_t c = 0; c < tc.numcomps; ++c) {
      if ((tc.alpha_mask & (1U << c)) != 0U) {
        src->comps[c].alpha = 1;
      }
    }
    EventCapture events;
    opj_cparameters_t cparams = MakeJp2LosslessParams();
    std::optional<std::vector<uint8_t>> jp2 = CompressImageToBytes(
        src.get(), OPJ_CODEC_JP2, &cparams, nullptr, &events);
    ASSERT_TRUE(jp2.has_value());
    EXPECT_THAT(events.warnings, Contains(HasSubstr(tc.expected_substr)));
  }
}

TEST(Jp2Test, SignatureAndFileTypeBoxesValidAndMalformed) {
  constexpr std::array<CompSpec, 1> kGraySpec = {{{.prec = 8, .sgnd = 0}}};
  const std::vector<uint8_t> j2k =
      EncodeJ2kWithSpecs(kDefaultWidth, kDefaultHeight, kGraySpec);
  ASSERT_FALSE(j2k.empty());
  const auto ihdr = Jp2BoxBuilder::MakeBox(
      Jp2BoxBuilder::FourCC("ihdr"),
      Jp2BoxBuilder::MakeIhdrPayload(kDefaultWidth, kDefaultHeight, 1, 7));
  const auto colr = Jp2BoxBuilder::MakeBox(
      Jp2BoxBuilder::FourCC("colr"), Jp2BoxBuilder::MakeColrEnumPayload(17));
  const auto jp2h = Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("jp2h"),
                                           Jp2BoxBuilder::Concat({ihdr, colr}));
  const auto jp2c = Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("jp2c"), j2k);
  const auto jp = Jp2BoxBuilder::MakeJpBox();

  // 1. Valid ftyp with 0 compat entries and with multiple compat entries.
  auto verify_ftyp = [&](uint32_t brand, uint32_t minversion,
                         std::span<const uint32_t> compat) {
    const auto ftyp_box = Jp2BoxBuilder::MakeFtypBox(brand, minversion, compat);
    const auto stream_bytes = Jp2BoxBuilder::Concat({jp, ftyp_box, jp2h, jp2c});
    OpjJp2Ptr dec(opj_jp2_create(OPJ_TRUE));
    ASSERT_NE(nullptr, dec);
    opj_dparameters_t dparams;
    opj_set_default_decoder_parameters(&dparams);
    opj_jp2_setup_decoder(dec.get(), &dparams);
    MemReadStream mem_in(stream_bytes);
    OpjStreamPtr stream = mem_in.CreateStream();
    opj_event_mgr_t mgr = MakeEventManager();
    opj_image_t* raw_img = nullptr;
    const OPJ_BOOL ok =
        opj_jp2_read_header(AsPrivate(stream), dec.get(), &raw_img, &mgr);
    OpjImagePtr img(raw_img);
    ASSERT_EQ(OPJ_TRUE, ok);
    EXPECT_EQ(brand, dec->brand);
    EXPECT_EQ(minversion, dec->minversion);
    EXPECT_EQ(compat.size(), dec->numcl);
    if (compat.empty()) {
      EXPECT_EQ(nullptr, dec->cl);
    } else {
      ASSERT_NE(nullptr, dec->cl);
      EXPECT_THAT(std::span<const OPJ_UINT32>(dec->cl, dec->numcl),
                  ElementsAreArray(compat));
    }
  };
  verify_ftyp(Jp2BoxBuilder::FourCC("jpx "), 3, {});
  constexpr std::array<uint32_t, 2> kCompat = {
      Jp2BoxBuilder::FourCC("jp2 "),
      Jp2BoxBuilder::FourCC("jpx "),
  };
  verify_ftyp(Jp2BoxBuilder::FourCC("jp2 "), 1, kCompat);

  // 2. Signature & ftyp error cases.
  EXPECT_FALSE(TryDecodeJp2({}, /*decode_image=*/false));
  const std::vector<uint8_t> short_hdr = {0x00, 0x00, 0x00, 0x0C};
  EXPECT_FALSE(TryDecodeJp2(short_hdr, /*decode_image=*/false));

  // Wrong magic in jP  .
  EXPECT_FALSE(TryDecodeJp2(
      Jp2BoxBuilder::Concat({Jp2BoxBuilder::MakeJpBox(0x12345678U),
                             Jp2BoxBuilder::MakeFtypBox(), jp2h, jp2c}),
      false));

  // Wrong size (!= 4) in jP  .
  EXPECT_FALSE(
      TryDecodeJp2(Jp2BoxBuilder::Concat(
                       {Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("jP  "),
                                               std::array<uint8_t, 6>{}),
                        Jp2BoxBuilder::MakeFtypBox(), jp2h, jp2c}),
                   false));

  // Duplicate jP  box (state != JP2_STATE_NONE).
  EXPECT_FALSE(TryDecodeJp2(
      Jp2BoxBuilder::Concat({jp, jp, Jp2BoxBuilder::MakeFtypBox(), jp2h, jp2c}),
      false));

  // Unknown box first (missing signature) or second (missing ftyp).
  const auto xml = Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("xml "),
                                          std::array<uint8_t, 4>{1, 2, 3, 4});
  EXPECT_FALSE(TryDecodeJp2(
      Jp2BoxBuilder::Concat({xml, Jp2BoxBuilder::MakeFtypBox(), jp2h, jp2c}),
      false));
  EXPECT_FALSE(
      TryDecodeJp2(Jp2BoxBuilder::Concat({jp, xml, jp2h, jp2c}), false));

  // ftyp as first box (state != JP2_STATE_SIGNATURE) and duplicate ftyp.
  const auto ftyp = Jp2BoxBuilder::MakeFtypBox();
  EXPECT_FALSE(TryDecodeJp2(Jp2BoxBuilder::Concat({ftyp, jp2h, jp2c}), false));
  EXPECT_FALSE(
      TryDecodeJp2(Jp2BoxBuilder::Concat({jp, ftyp, ftyp, jp2h, jp2c}), false));

  // ftyp < 8 payload bytes and ftyp payload not a multiple of 4.
  EXPECT_FALSE(TryDecodeJp2(
      Jp2BoxBuilder::Concat(
          {jp,
           Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("ftyp"),
                                  std::array<uint8_t, 4>{1, 2, 3, 4}),
           jp2h, jp2c}),
      false));
  EXPECT_FALSE(TryDecodeJp2(
      Jp2BoxBuilder::Concat(
          {jp,
           Jp2BoxBuilder::MakeBox(
               Jp2BoxBuilder::FourCC("ftyp"),
               std::array<uint8_t, 10>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}),
           jp2h, jp2c}),
      false));
}

TEST(Jp2Test, Jp2hSuperboxAndIhdrVariantsAndErrors) {
  constexpr std::array<CompSpec, 1> kGraySpec = {{{.prec = 8, .sgnd = 0}}};
  const std::vector<uint8_t> j2k =
      EncodeJ2kWithSpecs(kDefaultWidth, kDefaultHeight, kGraySpec);
  ASSERT_FALSE(j2k.empty());
  const auto jp = Jp2BoxBuilder::MakeJpBox();
  const auto ftyp = Jp2BoxBuilder::MakeFtypBox();
  const auto colr = Jp2BoxBuilder::MakeBox(
      Jp2BoxBuilder::FourCC("colr"), Jp2BoxBuilder::MakeColrEnumPayload(17));
  const auto jp2c = Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("jp2c"), j2k);

  // 1. Valid jp2h with XLBox ihdr, C != 7, duplicate ihdr, and res/resc/resd
  // sub-boxes (unknown to jp2.c).
  {
    const auto ihdr_xl = Jp2BoxBuilder::MakeXLBox(
        Jp2BoxBuilder::FourCC("ihdr"),
        Jp2BoxBuilder::MakeIhdrPayload(kDefaultWidth, kDefaultHeight, 1, 7,
                                       /*compression=*/0));
    const auto ihdr_dup = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("ihdr"),
        Jp2BoxBuilder::MakeIhdrPayload(kDefaultWidth, kDefaultHeight, 1, 7));
    const auto resc = Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("resc"),
                                             std::array<uint8_t, 10>{});
    const auto resd = Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("resd"),
                                             std::array<uint8_t, 10>{});
    const auto res = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("res "), Jp2BoxBuilder::Concat({resc, resd}));
    const auto jp2_bytes = Jp2BoxBuilder::Concat(
        {jp, ftyp,
         Jp2BoxBuilder::MakeBox(
             Jp2BoxBuilder::FourCC("jp2h"),
             Jp2BoxBuilder::Concat({ihdr_xl, ihdr_dup, colr, res})),
         jp2c});
    EventCapture events;
    OpjImagePtr decoded;
    EXPECT_TRUE(
        TryDecodeJp2(jp2_bytes, /*decode_image=*/true, &events, &decoded));
    EXPECT_THAT(events.infos, Contains(HasSubstr("not a conforming JP2")));
    EXPECT_THAT(events.warnings, Contains(HasSubstr("Ignoring ihdr box")));
  }

  // 2. jp2h before ftyp fails.
  {
    const auto ihdr = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("ihdr"),
        Jp2BoxBuilder::MakeIhdrPayload(kDefaultWidth, kDefaultHeight, 1, 7));
    const auto jp2h = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("jp2h"), Jp2BoxBuilder::Concat({ihdr, colr}));
    EXPECT_FALSE(
        TryDecodeJp2(Jp2BoxBuilder::Concat({jp, jp2h, ftyp, jp2c}), false));
  }

  // 3. Missing jp2h (EOF right after ftyp).
  {
    EventCapture events;
    EXPECT_FALSE(
        TryDecodeJp2(Jp2BoxBuilder::Concat({jp, ftyp}), false, &events));
    EXPECT_THAT(events.errors, Contains(HasSubstr("JP2H box missing")));
  }

  // 4. Empty jp2h and jp2h without ihdr fail.
  EXPECT_FALSE(TryDecodeJp2(
      Jp2BoxBuilder::Concat(
          {jp, ftyp, Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("jp2h"), {}),
           jp2c}),
      false));
  EXPECT_FALSE(TryDecodeJp2(
      Jp2BoxBuilder::Concat(
          {jp, ftyp,
           Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("jp2h"), colr), jp2c}),
      false));

  // 5. Malformed ihdr payloads: bad size, zero w/h/numcomps, numcomps > 16384,
  // and SIZ vs IHDR dimension mismatch.
  const std::vector<std::vector<uint8_t>> bad_ihdr_boxes = {
      Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("ihdr"),
                             std::array<uint8_t, 12>{}),
      Jp2BoxBuilder::MakeBox(
          Jp2BoxBuilder::FourCC("ihdr"),
          Jp2BoxBuilder::MakeIhdrPayload(0, kDefaultHeight, 1, 7)),
      Jp2BoxBuilder::MakeBox(
          Jp2BoxBuilder::FourCC("ihdr"),
          Jp2BoxBuilder::MakeIhdrPayload(kDefaultWidth, 0, 1, 7)),
      Jp2BoxBuilder::MakeBox(
          Jp2BoxBuilder::FourCC("ihdr"),
          Jp2BoxBuilder::MakeIhdrPayload(kDefaultWidth, kDefaultHeight, 0, 7)),
      Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("ihdr"),
                             Jp2BoxBuilder::MakeIhdrPayload(
                                 kDefaultWidth, kDefaultHeight, 16385, 7)),
      Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("ihdr"),
                             Jp2BoxBuilder::MakeIhdrPayload(
                                 kDefaultWidth + 4U, kDefaultHeight, 1, 7)),
  };
  for (const auto& ihdr_box : bad_ihdr_boxes) {
    EXPECT_FALSE(TryDecodeJp2(
        Jp2BoxBuilder::Concat(
            {jp, ftyp,
             Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("jp2h"),
                                    Jp2BoxBuilder::Concat({ihdr_box, colr})),
             jp2c}),
        false));
  }

  // 6. Malformed box headers inside jp2h (opj_jp2_read_boxhdr_char branches).
  const auto valid_ihdr = Jp2BoxBuilder::MakeBox(
      Jp2BoxBuilder::FourCC("ihdr"),
      Jp2BoxBuilder::MakeIhdrPayload(kDefaultWidth, kDefaultHeight, 1, 7));
  const std::vector<std::vector<uint8_t>> bad_subboxes = {
      // Child length > remaining jp2h payload.
      Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("free"),
                             std::array<uint8_t, 4>{}, /*override_length=*/40U),
      // < 8 bytes trailing inside jp2h.
      std::vector<uint8_t>{0x00, 0x00, 0x00, 0x08},
      // LBox == 0 inside jp2h (undefined box size).
      Jp2BoxBuilder::MakeEofBox(Jp2BoxBuilder::FourCC("free"), {}),
      // LBox < 8 inside jp2h.
      Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("free"), {}, 4U),
      // LBox == 1 XLBox with < 16 bytes remaining in jp2h.
      Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("free"),
                             std::array<uint8_t, 4>{}, 1U),
      // LBox == 1 XLBox with upper 32 bits != 0 (> 2^32).
      Jp2BoxBuilder::MakeXLBox(Jp2BoxBuilder::FourCC("free"), {},
                               0x0000000100000020ULL),
      // LBox == 1 XLBox with 64-bit size == 0.
      Jp2BoxBuilder::MakeXLBox(Jp2BoxBuilder::FourCC("free"), {}, 0ULL),
      // LBox == 1 XLBox with 64-bit size < 16.
      Jp2BoxBuilder::MakeXLBox(Jp2BoxBuilder::FourCC("free"), {}, 12ULL),
  };
  for (const auto& bad_child : bad_subboxes) {
    const auto jp2h_bad =
        Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("jp2h"),
                               Jp2BoxBuilder::Concat({valid_ihdr, bad_child}));
    EXPECT_FALSE(
        TryDecodeJp2(Jp2BoxBuilder::Concat({jp, ftyp, jp2h_bad, jp2c}), false));
  }
}

TEST(Jp2Test, ColrBoxVariantsCielabIgnoredMethAndErrors) {
  constexpr std::array<CompSpec, 3> kRgbSpec = {{
      {.prec = 8, .sgnd = 0},
      {.prec = 8, .sgnd = 0},
      {.prec = 8, .sgnd = 0},
  }};
  const std::vector<uint8_t> j2k =
      EncodeJ2kWithSpecs(kDefaultWidth, kDefaultHeight, kRgbSpec);
  ASSERT_FALSE(j2k.empty());
  const auto ihdr = Jp2BoxBuilder::MakeBox(
      Jp2BoxBuilder::FourCC("ihdr"),
      Jp2BoxBuilder::MakeIhdrPayload(kDefaultWidth, kDefaultHeight, 3, 7));

  // 1. CIELab (enumcs == 14) with 35-byte full parameters.
  {
    const auto colr_lab =
        Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("colr"),
                               Jp2BoxBuilder::MakeColrCielabPayload(
                                   100, 10, 200, 20, 300, 30, 0x00443530U));
    OpjImagePtr decoded;
    EventCapture events;
    ASSERT_TRUE(TryDecodeJp2(
        BuildStandardJp2(Jp2BoxBuilder::Concat({ihdr, colr_lab}), j2k), true,
        &events, &decoded));
    EXPECT_EQ(OPJ_CLRSPC_UNKNOWN, decoded->color_space);
    EXPECT_EQ(0u, decoded->icc_profile_len);
    ASSERT_NE(nullptr, decoded->icc_profile_buf);
    // jp2.c smuggles CIELab parameters as host-order OPJ_UINT32[9] in
    // icc_profile_buf with icc_profile_len == 0.
    std::array<OPJ_UINT32, 9> cielab{};
    std::memcpy(cielab.data(), decoded->icc_profile_buf, sizeof(cielab));
    EXPECT_THAT(cielab, ElementsAre(kEnumcsCielab, 0u, 100u, 10u, 200u, 20u,
                                    300u, 30u, 0x00443530U));
  }

  // 2. CIELab with 7-byte default parameters and 11-byte bad-size warning.
  {
    const auto colr_lab7 = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("colr"),
        Jp2BoxBuilder::MakeColrEnumPayload(kEnumcsCielab));
    OpjImagePtr decoded;
    ASSERT_TRUE(TryDecodeJp2(
        BuildStandardJp2(Jp2BoxBuilder::Concat({ihdr, colr_lab7}), j2k), false,
        nullptr, &decoded));
    ASSERT_NE(nullptr, decoded->icc_profile_buf);
    std::array<OPJ_UINT32, 9> cielab{};
    std::memcpy(cielab.data(), decoded->icc_profile_buf, sizeof(cielab));
    EXPECT_EQ(kEnumcsCielab, cielab[0]);
    EXPECT_EQ(0x44454600U, cielab[1]);
    EXPECT_EQ(0x00443530U, cielab[8]);
  }
  {
    std::vector<uint8_t> lab11 =
        Jp2BoxBuilder::MakeColrEnumPayload(kEnumcsCielab);
    lab11.resize(11U, 0);
    const auto colr_lab11 =
        Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("colr"), lab11);
    EventCapture events;
    OpjImagePtr decoded;
    ASSERT_TRUE(TryDecodeJp2(
        BuildStandardJp2(Jp2BoxBuilder::Concat({ihdr, colr_lab11}), j2k), false,
        &events, &decoded));
    EXPECT_THAT(events.warnings, Contains(HasSubstr("CIELab, bad size")));
  }

  // 3. meth == 1 with size > 7 on sRGB (warns), unknown enumcs, meth == 3
  // ignored before meth == 1, and duplicate colr ignored (first wins).
  {
    std::vector<uint8_t> srgb_padded = Jp2BoxBuilder::MakeColrEnumPayload(16U);
    srgb_padded.push_back(0);
    srgb_padded.push_back(0);
    const auto colr_padded =
        Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("colr"), srgb_padded);
    const auto colr_gray = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("colr"), Jp2BoxBuilder::MakeColrEnumPayload(17U));
    EventCapture events;
    OpjImagePtr decoded;
    ASSERT_TRUE(TryDecodeJp2(
        BuildStandardJp2(Jp2BoxBuilder::Concat({ihdr, colr_padded, colr_gray}),
                         j2k),
        false, &events, &decoded));
    EXPECT_EQ(OPJ_CLRSPC_SRGB, decoded->color_space);
    EXPECT_THAT(events.warnings, Contains(HasSubstr("Bad COLR header box")));
    EXPECT_THAT(events.infos,
                Contains(HasSubstr("ignore all Colour Specification boxes")));
  }
  {
    const auto colr_meth3 = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("colr"),
        Jp2BoxBuilder::MakeColrIccPayload(/*meth=*/3U,
                                          std::array<uint8_t, 4>{1, 2, 3, 4}));
    const auto colr_sycc = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("colr"), Jp2BoxBuilder::MakeColrEnumPayload(18U));
    EventCapture events;
    OpjImagePtr decoded;
    ASSERT_TRUE(TryDecodeJp2(
        BuildStandardJp2(Jp2BoxBuilder::Concat({ihdr, colr_meth3, colr_sycc}),
                         j2k),
        false, &events, &decoded));
    EXPECT_EQ(OPJ_CLRSPC_SYCC, decoded->color_space);
    EXPECT_THAT(events.infos, Contains(HasSubstr("not a regular value")));
  }
  {
    const auto colr_unk = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("colr"), Jp2BoxBuilder::MakeColrEnumPayload(99U));
    OpjImagePtr decoded;
    ASSERT_TRUE(TryDecodeJp2(
        BuildStandardJp2(Jp2BoxBuilder::Concat({ihdr, colr_unk}), j2k), false,
        nullptr, &decoded));
    EXPECT_EQ(OPJ_CLRSPC_UNKNOWN, decoded->color_space);
  }

  // 4. Malformed colr sizes: < 3 bytes, meth==1 with < 7 bytes, meth==2 with
  // empty ICC (size == 3 -> opj_calloc(1, 0) returns NULL).
  const std::vector<std::vector<uint8_t>> bad_colr_payloads = {
      {1U, 0U},
      {1U, 0U, 0U, 0U, 16U},
      {2U, 0U, 0U},
  };
  for (const auto& payload : bad_colr_payloads) {
    const auto bad_colr =
        Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("colr"), payload);
    EXPECT_FALSE(TryDecodeJp2(
        BuildStandardJp2(Jp2BoxBuilder::Concat({ihdr, bad_colr}), j2k), false));
  }
}

TEST(Jp2Test, PaletteAndComponentMappingExpansionAndClamping) {
  // 1. 8-bit 1-comp indexed -> 3-comp RGB palette expansion + IGNORE flag.
  {
    constexpr std::array<CompSpec, 1> kIdxSpec = {{{.prec = 8, .sgnd = 0}}};
    std::vector<OPJ_INT32> indices(kDefaultWidth * kDefaultHeight);
    for (size_t i = 0; i < indices.size(); ++i) {
      indices[i] = static_cast<OPJ_INT32>(i % 4U);
    }
    const std::vector<std::vector<OPJ_INT32>> comp_data = {indices};
    const std::vector<uint8_t> j2k =
        EncodeJ2kWithSpecs(kDefaultWidth, kDefaultHeight, kIdxSpec, comp_data);
    ASSERT_FALSE(j2k.empty());

    const auto ihdr = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("ihdr"),
        Jp2BoxBuilder::MakeIhdrPayload(kDefaultWidth, kDefaultHeight, 1, 7));
    const auto colr = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("colr"), Jp2BoxBuilder::MakeColrEnumPayload(16));
    constexpr std::array<uint8_t, 3> kBi8 = {7, 7, 7};
    constexpr std::array<uint32_t, 12> kEntries = {
        10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120,
    };
    const auto pclr = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("pclr"),
        Jp2BoxBuilder::MakePclrPayload(4, kBi8, kEntries));
    constexpr std::array<Jp2BoxBuilder::CmapEntry, 3> kCmap = {{
        {.cmp = 0, .mtyp = 1, .pcol = 0},
        {.cmp = 0, .mtyp = 1, .pcol = 1},
        {.cmp = 0, .mtyp = 1, .pcol = 2},
    }};
    const auto cmap = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("cmap"), Jp2BoxBuilder::MakeCmapPayload(kCmap));
    const auto jp2 =
        BuildStandardJp2(Jp2BoxBuilder::Concat({ihdr, colr, pclr, cmap}), j2k);

    OpjImagePtr expanded;
    ASSERT_TRUE(TryDecodeJp2(jp2, /*decode_image=*/true, nullptr, &expanded));
    ASSERT_EQ(3u, expanded->numcomps);
    for (uint32_t i = 0; i < kDefaultWidth * kDefaultHeight; ++i) {
      const uint32_t idx = i % 4U;
      EXPECT_EQ(static_cast<OPJ_INT32>(kEntries[idx * 3U + 0U]),
                expanded->comps[0].data[i]);
      EXPECT_EQ(static_cast<OPJ_INT32>(kEntries[idx * 3U + 1U]),
                expanded->comps[1].data[i]);
      EXPECT_EQ(static_cast<OPJ_INT32>(kEntries[idx * 3U + 2U]),
                expanded->comps[2].data[i]);
    }

    // OPJ_DPARAMETERS_IGNORE_PCLR_CMAP_CDEF_FLAG skips palette expansion.
    OpjImagePtr raw_only;
    ASSERT_TRUE(TryDecodeJp2(jp2, /*decode_image=*/true, nullptr, &raw_only,
                             OPJ_DPARAMETERS_IGNORE_PCLR_CMAP_CDEF_FLAG));
    ASSERT_EQ(1u, raw_only->numcomps);
    ExpectImagePixelsEqual(comp_data, raw_only.get());

    // Issue 235/447 auto-correction: numcomps == 1 with all mtyp = 0, pcol = 0.
    constexpr std::array<Jp2BoxBuilder::CmapEntry, 3> kWeirdCmap = {{
        {.cmp = 0, .mtyp = 0, .pcol = 0},
        {.cmp = 0, .mtyp = 0, .pcol = 0},
        {.cmp = 0, .mtyp = 0, .pcol = 0},
    }};
    const auto weird_cmap =
        Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("cmap"),
                               Jp2BoxBuilder::MakeCmapPayload(kWeirdCmap));
    const auto weird_jp2 = BuildStandardJp2(
        Jp2BoxBuilder::Concat({ihdr, colr, pclr, weird_cmap}), j2k);
    EventCapture events;
    OpjImagePtr fixed;
    ASSERT_TRUE(TryDecodeJp2(weird_jp2, true, &events, &fixed));
    EXPECT_THAT(events.warnings, Contains(HasSubstr("Trying to correct")));
    ASSERT_EQ(3u, fixed->numcomps);
    EXPECT_EQ(static_cast<OPJ_INT32>(kEntries[0]), fixed->comps[0].data[0]);

    // pclr without cmap triggers opj_jp2_free_pclr and keeps 1 raw component.
    const auto pclr_only_jp2 =
        BuildStandardJp2(Jp2BoxBuilder::Concat({ihdr, colr, pclr}), j2k);
    OpjImagePtr unexpanded;
    ASSERT_TRUE(TryDecodeJp2(pclr_only_jp2, true, nullptr, &unexpanded));
    EXPECT_EQ(1u, unexpanded->numcomps);
  }

  // 2. Multi-byte 12/16-bit + signed palette entries, signed index clamping
  // (< 0 and > top_k), and 38-bit bytes_to_read clamp.
  {
    constexpr std::array<CompSpec, 1> kSignedSpec = {{{.prec = 8, .sgnd = 1}}};
    std::vector<OPJ_INT32> src_vals(kDefaultWidth * kDefaultHeight, 0);
    src_vals[0] = -5;
    src_vals[1] = 1;
    src_vals[2] = 10;
    const std::vector<std::vector<OPJ_INT32>> comp_data = {src_vals};
    const std::vector<uint8_t> j2k = EncodeJ2kWithSpecs(
        kDefaultWidth, kDefaultHeight, kSignedSpec, comp_data);
    ASSERT_FALSE(j2k.empty());

    const auto ihdr = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("ihdr"),
        Jp2BoxBuilder::MakeIhdrPayload(kDefaultWidth, kDefaultHeight, 1, 0x87));
    const auto colr = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("colr"), Jp2BoxBuilder::MakeColrEnumPayload(16));
    // Col 0: 12-bit unsigned (Bi=11), Col 1: 16-bit signed (Bi=0x80|15),
    // Col 2: 38-bit unsigned (Bi=37, hits bytes_to_read > 4 clamp).
    constexpr std::array<uint8_t, 3> kBiMulti = {11, 0x8F, 37};
    constexpr std::array<uint32_t, 9> kEntries = {
        100, 1000, 50000, 200, 2000, 60000, 300, 3000, 70000,
    };
    const auto pclr = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("pclr"),
        Jp2BoxBuilder::MakePclrPayload(3, kBiMulti, kEntries));
    constexpr std::array<Jp2BoxBuilder::CmapEntry, 3> kCmap = {{
        {.cmp = 0, .mtyp = 1, .pcol = 0},
        {.cmp = 0, .mtyp = 1, .pcol = 1},
        {.cmp = 0, .mtyp = 1, .pcol = 2},
    }};
    const auto cmap = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("cmap"), Jp2BoxBuilder::MakeCmapPayload(kCmap));
    const auto jp2 =
        BuildStandardJp2(Jp2BoxBuilder::Concat({ihdr, colr, pclr, cmap}), j2k);

    OpjImagePtr decoded;
    ASSERT_TRUE(TryDecodeJp2(jp2, true, nullptr, &decoded));
    ASSERT_EQ(3u, decoded->numcomps);
    EXPECT_EQ(12u, decoded->comps[0].prec);
    EXPECT_EQ(0u, decoded->comps[0].sgnd);
    EXPECT_EQ(16u, decoded->comps[1].prec);
    EXPECT_EQ(1u, decoded->comps[1].sgnd);
    EXPECT_EQ(38u, decoded->comps[2].prec);
    // Negative (-5) clamps to entry 0; 10 (> top_k=2) clamps to entry 2.
    EXPECT_EQ(100, decoded->comps[0].data[0]);
    EXPECT_EQ(200, decoded->comps[0].data[1]);
    EXPECT_EQ(300, decoded->comps[0].data[2]);
    EXPECT_EQ(50000, decoded->comps[2].data[0]);
    EXPECT_EQ(70000, decoded->comps[2].data[2]);
  }

  // 3. Mixed mtyp == 1 (palette RGB from comp 0) + mtyp == 0 (direct alpha
  // from comp 1) on a 2-component codestream.
  {
    constexpr std::array<CompSpec, 2> kTwoSpecs = {{
        {.prec = 8, .sgnd = 0},
        {.prec = 8, .sgnd = 0},
    }};
    std::vector<OPJ_INT32> idx(kDefaultWidth * kDefaultHeight, 1);
    idx[0] = 0;
    std::vector<OPJ_INT32> alpha(kDefaultWidth * kDefaultHeight, 250);
    alpha[0] = 123;
    const std::vector<std::vector<OPJ_INT32>> comp_data = {idx, alpha};
    const std::vector<uint8_t> j2k =
        EncodeJ2kWithSpecs(kDefaultWidth, kDefaultHeight, kTwoSpecs, comp_data);
    ASSERT_FALSE(j2k.empty());

    const auto ihdr = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("ihdr"),
        Jp2BoxBuilder::MakeIhdrPayload(kDefaultWidth, kDefaultHeight, 2, 7));
    const auto colr = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("colr"), Jp2BoxBuilder::MakeColrEnumPayload(16));
    constexpr std::array<uint8_t, 4> kBi4 = {7, 7, 7, 7};
    constexpr std::array<uint32_t, 8> kEntries = {
        11, 22, 33, 0, 44, 55, 66, 0,
    };
    const auto pclr = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("pclr"),
        Jp2BoxBuilder::MakePclrPayload(2, kBi4, kEntries));
    constexpr std::array<Jp2BoxBuilder::CmapEntry, 4> kCmap = {{
        {.cmp = 0, .mtyp = 1, .pcol = 0},
        {.cmp = 0, .mtyp = 1, .pcol = 1},
        {.cmp = 0, .mtyp = 1, .pcol = 2},
        {.cmp = 1, .mtyp = 0, .pcol = 0},
    }};
    const auto cmap = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("cmap"), Jp2BoxBuilder::MakeCmapPayload(kCmap));
    const auto jp2 =
        BuildStandardJp2(Jp2BoxBuilder::Concat({ihdr, colr, pclr, cmap}), j2k);

    OpjImagePtr decoded;
    ASSERT_TRUE(TryDecodeJp2(jp2, true, nullptr, &decoded));
    ASSERT_EQ(4u, decoded->numcomps);
    EXPECT_EQ(11, decoded->comps[0].data[0]);
    EXPECT_EQ(44, decoded->comps[0].data[1]);
    EXPECT_EQ(123, decoded->comps[3].data[0]);
    EXPECT_EQ(250, decoded->comps[3].data[1]);
  }
}

TEST(Jp2Test, PaletteCmapAndCdefErrorBranches) {
  constexpr std::array<CompSpec, 2> kTwoSpecs = {{
      {.prec = 8, .sgnd = 0},
      {.prec = 8, .sgnd = 0},
  }};
  const std::vector<uint8_t> j2k =
      EncodeJ2kWithSpecs(kDefaultWidth, kDefaultHeight, kTwoSpecs);
  ASSERT_FALSE(j2k.empty());
  const auto ihdr = Jp2BoxBuilder::MakeBox(
      Jp2BoxBuilder::FourCC("ihdr"),
      Jp2BoxBuilder::MakeIhdrPayload(kDefaultWidth, kDefaultHeight, 2, 7));
  const auto colr = Jp2BoxBuilder::MakeBox(
      Jp2BoxBuilder::FourCC("colr"), Jp2BoxBuilder::MakeColrEnumPayload(16));

  constexpr std::array<uint8_t, 3> kBi3 = {7, 7, 7};
  constexpr std::array<uint32_t, 6> kEntries = {1, 2, 3, 4, 5, 6};
  const auto valid_pclr =
      Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("pclr"),
                             Jp2BoxBuilder::MakePclrPayload(2, kBi3, kEntries));
  constexpr std::array<Jp2BoxBuilder::CmapEntry, 3> kValidCmapEntries = {{
      {.cmp = 0, .mtyp = 1, .pcol = 0},
      {.cmp = 0, .mtyp = 1, .pcol = 1},
      {.cmp = 0, .mtyp = 1, .pcol = 2},
  }};
  const auto valid_cmap =
      Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("cmap"),
                             Jp2BoxBuilder::MakeCmapPayload(kValidCmapEntries));

  // 1. PCLR read errors (header-level failures).
  {
    std::vector<uint8_t> trunc_pclr =
        Jp2BoxBuilder::MakePclrPayload(2, kBi3, kEntries);
    trunc_pclr.pop_back();
    const std::vector<std::vector<uint8_t>> bad_pclr_boxes = {
        Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("pclr"),
                               std::array<uint8_t, 2>{0, 1}),
        Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("pclr"),
                               std::array<uint8_t, 4>{0, 0, 1, 7}),
        Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("pclr"),
                               std::array<uint8_t, 4>{0x04, 0x01, 1, 7}),
        Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("pclr"),
                               std::array<uint8_t, 3>{0, 2, 0}),
        Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("pclr"),
                               std::array<uint8_t, 4>{0, 2, 2, 7}),
        Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("pclr"), trunc_pclr),
    };
    for (const auto& box : bad_pclr_boxes) {
      EXPECT_FALSE(TryDecodeJp2(
          BuildStandardJp2(Jp2BoxBuilder::Concat({ihdr, colr, box}), j2k),
          false));
    }
    // Duplicate PCLR box.
    EXPECT_FALSE(TryDecodeJp2(
        BuildStandardJp2(
            Jp2BoxBuilder::Concat({ihdr, colr, valid_pclr, valid_pclr}), j2k),
        false));
  }

  // 2. CMAP read errors (header-level failures).
  {
    // CMAP before PCLR.
    EXPECT_FALSE(TryDecodeJp2(
        BuildStandardJp2(Jp2BoxBuilder::Concat({ihdr, colr, valid_cmap}), j2k),
        false));
    // Duplicate CMAP.
    EXPECT_FALSE(TryDecodeJp2(
        BuildStandardJp2(Jp2BoxBuilder::Concat(
                             {ihdr, colr, valid_pclr, valid_cmap, valid_cmap}),
                         j2k),
        false));
    // Truncated CMAP (< nr_channels * 4).
    const auto short_cmap = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("cmap"), std::array<uint8_t, 8>{});
    EXPECT_FALSE(TryDecodeJp2(
        BuildStandardJp2(
            Jp2BoxBuilder::Concat({ihdr, colr, valid_pclr, short_cmap}), j2k),
        false));
  }

  // 3. opj_jp2_check_color CMAP validation failures during opj_decode.
  const std::vector<std::array<Jp2BoxBuilder::CmapEntry, 3>> invalid_cmaps = {
      // cmp >= numcomps
      {{{.cmp = 5, .mtyp = 1, .pcol = 0},
        {.cmp = 0, .mtyp = 1, .pcol = 1},
        {.cmp = 0, .mtyp = 1, .pcol = 2}}},
      // Invalid mtyp == 2
      {{{.cmp = 0, .mtyp = 2, .pcol = 0},
        {.cmp = 0, .mtyp = 1, .pcol = 1},
        {.cmp = 0, .mtyp = 1, .pcol = 2}}},
      // pcol >= nr_channels
      {{{.cmp = 0, .mtyp = 1, .pcol = 5},
        {.cmp = 0, .mtyp = 1, .pcol = 1},
        {.cmp = 0, .mtyp = 1, .pcol = 2}}},
      // Direct mapping with pcol != 0
      {{{.cmp = 0, .mtyp = 1, .pcol = 0},
        {.cmp = 1, .mtyp = 0, .pcol = 1},
        {.cmp = 0, .mtyp = 1, .pcol = 2}}},
      // Palette mapping to column already used by direct mapping (mapped twice)
      {{{.cmp = 0, .mtyp = 0, .pcol = 0},
        {.cmp = 0, .mtyp = 1, .pcol = 0},
        {.cmp = 0, .mtyp = 1, .pcol = 2}}},
      // Implementation limitation mtyp == 1 && pcol != i (also triggers missing
      // mapping check)
      {{{.cmp = 0, .mtyp = 1, .pcol = 1},
        {.cmp = 0, .mtyp = 1, .pcol = 0},
        {.cmp = 0, .mtyp = 1, .pcol = 2}}},
  };
  for (const auto& bad_cmap_entries : invalid_cmaps) {
    const auto cmap_box = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("cmap"),
        Jp2BoxBuilder::MakeCmapPayload(bad_cmap_entries));
    EventCapture events;
    EXPECT_FALSE(TryDecodeJp2(
        BuildStandardJp2(
            Jp2BoxBuilder::Concat({ihdr, colr, valid_pclr, cmap_box}), j2k),
        /*decode_image=*/true, &events));
    EXPECT_THAT(events.errors, Not(IsEmpty()));
  }

  // 4. CDEF read errors and opj_jp2_check_color CDEF errors.
  {
    const auto cdef_short = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("cdef"), std::array<uint8_t, 1>{0});
    const auto cdef_zero_n = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("cdef"), std::array<uint8_t, 2>{0, 0});
    const auto cdef_trunc = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("cdef"), std::array<uint8_t, 4>{0, 1, 0, 0});
    for (const auto& bad_cdef : {cdef_short, cdef_zero_n, cdef_trunc}) {
      EXPECT_FALSE(TryDecodeJp2(
          BuildStandardJp2(Jp2BoxBuilder::Concat({ihdr, colr, bad_cdef}), j2k),
          false));
    }

    constexpr std::array<Jp2BoxBuilder::CdefEntry, 2> kValid2 = {{
        {.cn = 0, .typ = 0, .asoc = 1},
        {.cn = 1, .typ = 1, .asoc = 0},
    }};
    const auto valid_cdef = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("cdef"), Jp2BoxBuilder::MakeCdefPayload(kValid2));
    // Duplicate CDEF.
    EXPECT_FALSE(TryDecodeJp2(
        BuildStandardJp2(
            Jp2BoxBuilder::Concat({ihdr, colr, valid_cdef, valid_cdef}), j2k),
        false));

    // CDEF invalid cn, invalid asoc, and incomplete channel definitions.
    const std::vector<std::vector<Jp2BoxBuilder::CdefEntry>> bad_cdef_lists = {
        {{.cn = 5, .typ = 0, .asoc = 1}, {.cn = 1, .typ = 1, .asoc = 0}},
        {{.cn = 0, .typ = 0, .asoc = 5}, {.cn = 1, .typ = 1, .asoc = 0}},
        {{.cn = 0, .typ = 0, .asoc = 1}},
    };
    for (const auto& list : bad_cdef_lists) {
      const auto cdef_box = Jp2BoxBuilder::MakeBox(
          Jp2BoxBuilder::FourCC("cdef"), Jp2BoxBuilder::MakeCdefPayload(list));
      EventCapture events;
      EXPECT_FALSE(TryDecodeJp2(
          BuildStandardJp2(Jp2BoxBuilder::Concat({ihdr, colr, cdef_box}), j2k),
          /*decode_image=*/true, &events));
      EXPECT_THAT(events.errors, Not(IsEmpty()));
    }
  }
}

TEST(Jp2Test, CdefChannelReorderingAlphaTypesAndWithPalette) {
  // 1. 3-comp BGR -> RGB swap via cdef.
  {
    constexpr std::array<CompSpec, 3> kRgbSpec = {{
        {.prec = 8, .sgnd = 0},
        {.prec = 8, .sgnd = 0},
        {.prec = 8, .sgnd = 0},
    }};
    const std::vector<std::vector<OPJ_INT32>> comp_data = {
        std::vector<OPJ_INT32>(kDefaultWidth * kDefaultHeight, 10),
        std::vector<OPJ_INT32>(kDefaultWidth * kDefaultHeight, 20),
        std::vector<OPJ_INT32>(kDefaultWidth * kDefaultHeight, 30),
    };
    const std::vector<uint8_t> j2k =
        EncodeJ2kWithSpecs(kDefaultWidth, kDefaultHeight, kRgbSpec, comp_data);
    ASSERT_FALSE(j2k.empty());

    const auto ihdr = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("ihdr"),
        Jp2BoxBuilder::MakeIhdrPayload(kDefaultWidth, kDefaultHeight, 3, 7));
    const auto colr = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("colr"), Jp2BoxBuilder::MakeColrEnumPayload(16));
    constexpr std::array<Jp2BoxBuilder::CdefEntry, 3> kBgrToRgb = {{
        {.cn = 0, .typ = 0, .asoc = 3},
        {.cn = 1, .typ = 0, .asoc = 2},
        {.cn = 2, .typ = 0, .asoc = 1},
    }};
    const auto cdef =
        Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("cdef"),
                               Jp2BoxBuilder::MakeCdefPayload(kBgrToRgb));
    OpjImagePtr decoded;
    ASSERT_TRUE(TryDecodeJp2(
        BuildStandardJp2(Jp2BoxBuilder::Concat({ihdr, colr, cdef}), j2k), true,
        nullptr, &decoded));
    ASSERT_EQ(3u, decoded->numcomps);
    EXPECT_EQ(30, decoded->comps[0].data[0]);
    EXPECT_EQ(20, decoded->comps[1].data[0]);
    EXPECT_EQ(10, decoded->comps[2].data[0]);

    // Also test a cdef list where a later entry shares cn with an earlier
    // swapped channel so both info[j].cn == cn and info[j].cn == acn trigger.
    constexpr std::array<Jp2BoxBuilder::CdefEntry, 4> kRepeatedCn = {{
        {.cn = 0, .typ = 0, .asoc = 2},
        {.cn = 0, .typ = 0, .asoc = 1},
        {.cn = 1, .typ = 0, .asoc = 2},
        {.cn = 2, .typ = 0, .asoc = 3},
    }};
    const auto cdef_rep =
        Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("cdef"),
                               Jp2BoxBuilder::MakeCdefPayload(kRepeatedCn));
    OpjImagePtr decoded_rep;
    ASSERT_TRUE(TryDecodeJp2(
        BuildStandardJp2(Jp2BoxBuilder::Concat({ihdr, colr, cdef_rep}), j2k),
        true, nullptr, &decoded_rep));
    ASSERT_EQ(3u, decoded_rep->numcomps);
  }

  // 2. Combined pclr + cmap + cdef: 4-channel palette (B, G, R, premul-alpha)
  // reordered to R, G, B + typ=2 premultiplied alpha, plus unspecified
  // asoc=65535 check.
  {
    constexpr std::array<CompSpec, 1> kIdxSpec = {{{.prec = 8, .sgnd = 0}}};
    const std::vector<std::vector<OPJ_INT32>> comp_data = {
        std::vector<OPJ_INT32>(kDefaultWidth * kDefaultHeight, 1),
    };
    const std::vector<uint8_t> j2k =
        EncodeJ2kWithSpecs(kDefaultWidth, kDefaultHeight, kIdxSpec, comp_data);
    ASSERT_FALSE(j2k.empty());

    const auto ihdr = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("ihdr"),
        Jp2BoxBuilder::MakeIhdrPayload(kDefaultWidth, kDefaultHeight, 1, 7));
    const auto colr = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("colr"), Jp2BoxBuilder::MakeColrEnumPayload(16));
    constexpr std::array<uint8_t, 4> kBi4 = {7, 7, 7, 7};
    constexpr std::array<uint32_t, 8> kEntries = {
        1, 2, 3, 255, 11, 22, 33, 200,
    };
    const auto pclr = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("pclr"),
        Jp2BoxBuilder::MakePclrPayload(2, kBi4, kEntries));
    constexpr std::array<Jp2BoxBuilder::CmapEntry, 4> kCmap = {{
        {.cmp = 0, .mtyp = 1, .pcol = 0},
        {.cmp = 0, .mtyp = 1, .pcol = 1},
        {.cmp = 0, .mtyp = 1, .pcol = 2},
        {.cmp = 0, .mtyp = 1, .pcol = 3},
    }};
    const auto cmap = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("cmap"), Jp2BoxBuilder::MakeCmapPayload(kCmap));
    constexpr std::array<Jp2BoxBuilder::CdefEntry, 4> kCdef = {{
        {.cn = 0, .typ = 0, .asoc = 3},
        {.cn = 1, .typ = 0, .asoc = 2},
        {.cn = 2, .typ = 0, .asoc = 1},
        {.cn = 3, .typ = 2, .asoc = 0},
    }};
    const auto cdef = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("cdef"), Jp2BoxBuilder::MakeCdefPayload(kCdef));
    OpjImagePtr decoded;
    ASSERT_TRUE(TryDecodeJp2(
        BuildStandardJp2(Jp2BoxBuilder::Concat({ihdr, colr, pclr, cmap, cdef}),
                         j2k),
        true, nullptr, &decoded));
    ASSERT_EQ(4u, decoded->numcomps);
    EXPECT_EQ(33, decoded->comps[0].data[0]);
    EXPECT_EQ(22, decoded->comps[1].data[0]);
    EXPECT_EQ(11, decoded->comps[2].data[0]);
    EXPECT_EQ(200, decoded->comps[3].data[0]);
    EXPECT_EQ(2u, decoded->comps[3].alpha);

    constexpr std::array<Jp2BoxBuilder::CdefEntry, 4> kCdefUnk = {{
        {.cn = 0, .typ = 0, .asoc = 1},
        {.cn = 1, .typ = 0, .asoc = 2},
        {.cn = 2, .typ = 0, .asoc = 3},
        {.cn = 3, .typ = 65535, .asoc = 65535},
    }};
    const auto cdef_unk =
        Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("cdef"),
                               Jp2BoxBuilder::MakeCdefPayload(kCdefUnk));
    OpjImagePtr decoded_unk;
    ASSERT_TRUE(TryDecodeJp2(
        BuildStandardJp2(
            Jp2BoxBuilder::Concat({ihdr, colr, pclr, cmap, cdef_unk}), j2k),
        true, nullptr, &decoded_unk));
    EXPECT_EQ(65535u, decoded_unk->comps[3].alpha);
  }
}

TEST(Jp2Test, TopLevelUnknownMisplacedCodestreamAndTrailingBoxes) {
  constexpr std::array<CompSpec, 1> kGraySpec = {{{.prec = 8, .sgnd = 0}}};
  const std::vector<uint8_t> j2k =
      EncodeJ2kWithSpecs(kDefaultWidth, kDefaultHeight, kGraySpec);
  ASSERT_FALSE(j2k.empty());
  const auto jp = Jp2BoxBuilder::MakeJpBox();
  const auto ftyp = Jp2BoxBuilder::MakeFtypBox();
  const auto ihdr = Jp2BoxBuilder::MakeBox(
      Jp2BoxBuilder::FourCC("ihdr"),
      Jp2BoxBuilder::MakeIhdrPayload(kDefaultWidth, kDefaultHeight, 1, 7));
  const auto colr_gray =
      Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("colr"),
                             Jp2BoxBuilder::MakeColrEnumPayload(kEnumcsGray));
  const auto colr_sycc =
      Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("colr"),
                             Jp2BoxBuilder::MakeColrEnumPayload(kEnumcsSycc));
  const auto jp2h_no_colr =
      Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("jp2h"), ihdr);
  const auto jp2h_full = Jp2BoxBuilder::MakeBox(
      Jp2BoxBuilder::FourCC("jp2h"), Jp2BoxBuilder::Concat({ihdr, colr_gray}));
  const auto xml =
      Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("xml "),
                             std::array<uint8_t, 6>{'<', 'a', '/', '>'});
  const auto empty_xml =
      Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("xml "), {});
  const auto uuid = Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("uuid"),
                                           std::array<uint8_t, 16>{1, 2, 3, 4});
  const auto uinf = Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("uinf"),
                                           std::array<uint8_t, 8>{});
  const auto jp2c = Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("jp2c"), j2k);

  // 1. Misplaced colr before jp2h (ignored because jp2_state lacks HEADER or
  // UNKNOWN), followed by xml/empty_xml, jp2h without colr, uuid/uinf,
  // misplaced colr after jp2h (applied), and jp2c with LBox == 0 (to EOF).
  {
    const auto jp2c_eof =
        Jp2BoxBuilder::MakeEofBox(Jp2BoxBuilder::FourCC("jp2c"), j2k);
    const auto jp2_bytes =
        Jp2BoxBuilder::Concat({jp, ftyp, colr_gray, xml, empty_xml,
                               jp2h_no_colr, uuid, uinf, colr_sycc, jp2c_eof});
    EventCapture events;
    OpjImagePtr decoded;
    ASSERT_TRUE(TryDecodeJp2(jp2_bytes, true, &events, &decoded));
    EXPECT_EQ(OPJ_CLRSPC_SYCC, decoded->color_space);
    EXPECT_THAT(events.warnings,
                Contains(HasSubstr("Header box not read yet")));
  }

  // 2. jp2c with 64-bit XLBox (LBox == 1).
  {
    const auto jp2c_xl =
        Jp2BoxBuilder::MakeXLBox(Jp2BoxBuilder::FourCC("jp2c"), j2k);
    const auto jp2_bytes =
        Jp2BoxBuilder::Concat({jp, ftyp, jp2h_full, jp2c_xl});
    OpjImagePtr decoded;
    EXPECT_TRUE(TryDecodeJp2(jp2_bytes, true, nullptr, &decoded));
    ASSERT_NE(nullptr, decoded);
    EXPECT_EQ(1u, decoded->numcomps);
    EXPECT_EQ(kDefaultWidth, decoded->x1 - decoded->x0);
    EXPECT_EQ(kDefaultHeight, decoded->y1 - decoded->y0);
    EXPECT_EQ(17, decoded->comps[0].data[1]);
    EXPECT_EQ(31, decoded->comps[0].data[kDefaultWidth]);
  }

  // 3. Trailing boxes after jp2c via tile-by-tile loop (consumes EOC so
  // opj_end_decompress enters opj_jp2_read_header_procedure after jp2c):
  // valid trailing xml, truncated trailing xml (warns and returns OPJ_TRUE),
  // and second jp2c box.
  auto verify_trailing_after_jp2c = [&](std::span<const uint8_t> trailing_box,
                                        bool expect_warning) {
    const auto jp2_bytes =
        Jp2BoxBuilder::Concat({jp, ftyp, jp2h_full, jp2c, trailing_box});
    EventCapture events;
    std::unique_ptr<DecodeSession> session =
        OpenJp2ForDecode(jp2_bytes, OPJ_CODEC_JP2, &events);
    ASSERT_NE(nullptr, session);
    for (;;) {
      OPJ_UINT32 tile_idx = 0;
      OPJ_UINT32 data_size = 0;
      OPJ_INT32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
      OPJ_UINT32 nb_comps = 0;
      OPJ_BOOL go_on = OPJ_FALSE;
      ASSERT_EQ(OPJ_TRUE, opj_read_tile_header(session->codec.get(),
                                               session->stream.get(), &tile_idx,
                                               &data_size, &x0, &y0, &x1, &y1,
                                               &nb_comps, &go_on));
      if (go_on == OPJ_FALSE) {
        break;
      }
      std::vector<OPJ_BYTE> tile_buf(data_size, 0);
      ASSERT_EQ(OPJ_TRUE, opj_decode_tile_data(session->codec.get(), tile_idx,
                                               tile_buf.data(), data_size,
                                               session->stream.get()));
    }
    EXPECT_EQ(OPJ_TRUE,
              opj_end_decompress(session->codec.get(), session->stream.get()));
    if (expect_warning) {
      EXPECT_THAT(events.warnings,
                  Contains(HasSubstr("Problem with skipping JPEG2000 box")));
    }
  };
  verify_trailing_after_jp2c(xml, /*expect_warning=*/false);
  verify_trailing_after_jp2c(
      Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("xml "),
                             std::array<uint8_t, 4>{}, /*override_length=*/80U),
      /*expect_warning=*/true);
  verify_trailing_after_jp2c(jp2c, /*expect_warning=*/false);

  // 4. Error branches in opj_jp2_read_boxhdr & opj_jp2_read_header_procedure.
  // jp2c before jp2h.
  EXPECT_FALSE(TryDecodeJp2(Jp2BoxBuilder::Concat({jp, ftyp, jp2c}), false));

  // Missing jp2c (EOF after jp2h containing icc, pclr, cmap, cdef -> verifies
  // opj_jp2_destroy cleans up all color allocations without leaks).
  {
    const auto icc_colr = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("colr"),
        Jp2BoxBuilder::MakeColrIccPayload(2, std::array<uint8_t, 8>{1, 2}));
    constexpr std::array<uint8_t, 3> kBi3 = {7, 7, 7};
    constexpr std::array<uint32_t, 3> kEnt = {1, 2, 3};
    const auto pclr =
        Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("pclr"),
                               Jp2BoxBuilder::MakePclrPayload(1, kBi3, kEnt));
    constexpr std::array<Jp2BoxBuilder::CmapEntry, 3> kCmap = {{
        {.cmp = 0, .mtyp = 1, .pcol = 0},
        {.cmp = 0, .mtyp = 1, .pcol = 1},
        {.cmp = 0, .mtyp = 1, .pcol = 2},
    }};
    const auto cmap = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("cmap"), Jp2BoxBuilder::MakeCmapPayload(kCmap));
    constexpr std::array<Jp2BoxBuilder::CdefEntry, 3> kCdef = {{
        {.cn = 0, .typ = 0, .asoc = 1},
        {.cn = 1, .typ = 0, .asoc = 2},
        {.cn = 2, .typ = 0, .asoc = 3},
    }};
    const auto cdef = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("cdef"), Jp2BoxBuilder::MakeCdefPayload(kCdef));
    const auto jp2h_rich = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("jp2h"),
        Jp2BoxBuilder::Concat({ihdr, icc_colr, pclr, cmap, cdef}));
    EXPECT_FALSE(
        TryDecodeJp2(Jp2BoxBuilder::Concat({jp, ftyp, jp2h_rich}), false));
  }

  // LBox == 0 with stream user_data_length > 2^32.
  {
    const auto jp2c_eof_hdr =
        Jp2BoxBuilder::MakeEofBox(Jp2BoxBuilder::FourCC("jp2c"), {});
    const auto bytes =
        Jp2BoxBuilder::Concat({jp, ftyp, jp2h_full, jp2c_eof_hdr});
    EventCapture events;
    EXPECT_FALSE(TryDecodeJp2(bytes, false, &events, nullptr, 0,
                              OPJ_J2K_STREAM_CHUNK_SIZE,
                              /*override_stream_length=*/0x100000100ULL));
    EXPECT_THAT(events.errors, Contains(HasSubstr("higher than 2^32")));
  }

  // Non-jp2c box with XLBox == 0, box with LBox < 8, XLBox < 16, XLBox > 2^32,
  // truncated XLBox, known box > byte_left, truncated misplaced box before
  // jp2h, and truncated unknown xml box before jp2c.
  EXPECT_FALSE(TryDecodeJp2(
      Jp2BoxBuilder::Concat(
          {jp, ftyp,
           Jp2BoxBuilder::MakeXLBox(Jp2BoxBuilder::FourCC("xml "), {}, 0ULL),
           jp2h_full, jp2c}),
      false));
  EXPECT_FALSE(TryDecodeJp2(
      Jp2BoxBuilder::Concat(
          {jp, ftyp,
           Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("xml "), {}, 4U),
           jp2h_full, jp2c}),
      false));
  EXPECT_FALSE(TryDecodeJp2(
      Jp2BoxBuilder::Concat(
          {jp, ftyp,
           Jp2BoxBuilder::MakeXLBox(Jp2BoxBuilder::FourCC("xml "), {}, 12ULL),
           jp2h_full, jp2c}),
      false));
  EXPECT_FALSE(
      TryDecodeJp2(Jp2BoxBuilder::Concat(
                       {jp, ftyp,
                        Jp2BoxBuilder::MakeXLBox(Jp2BoxBuilder::FourCC("xml "),
                                                 {}, 0x0000000100000020ULL),
                        jp2h_full, jp2c}),
                   false));
  {
    std::vector<uint8_t> trunc_xl = Jp2BoxBuilder::Concat(
        {jp, ftyp,
         Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("xml "),
                                std::array<uint8_t, 4>{}, 1U)});
    EXPECT_FALSE(TryDecodeJp2(trunc_xl, false));
  }
  EXPECT_FALSE(TryDecodeJp2(
      Jp2BoxBuilder::Concat(
          {jp, ftyp,
           Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("jp2h"), ihdr, 200U)}),
      false));
  EXPECT_FALSE(TryDecodeJp2(
      Jp2BoxBuilder::Concat(
          {jp, ftyp,
           Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("colr"),
                                  std::array<uint8_t, 4>{}, 200U)}),
      false));
  EXPECT_FALSE(TryDecodeJp2(
      Jp2BoxBuilder::Concat(
          {jp, ftyp,
           Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("xml "),
                                  std::array<uint8_t, 4>{}, 200U)}),
      false));

  // Stream read failure mid-box (small stream buffer + max_read_bytes).
  EXPECT_FALSE(TryDecodeJp2(Jp2BoxBuilder::Concat({jp, ftyp, jp2h_full, jp2c}),
                            false, nullptr, nullptr, 0,
                            /*stream_buffer_size=*/4U, std::nullopt,
                            /*max_read_bytes=*/16U));

  // Corrupted jp2c payload fails inside opj_jp2_decode.
  {
    std::vector<uint8_t> bad_j2k = j2k;
    ASSERT_GT(bad_j2k.size(), 10u);
    bad_j2k.resize(bad_j2k.size() - 6U);
    // Truncate 6 trailing bytes after SOT so read_header succeeds and decode
    // fails.
    const auto bad_jp2c =
        Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("jp2c"), bad_j2k);
    const auto bad_jp2 = Jp2BoxBuilder::Concat({jp, ftyp, jp2h_full, bad_jp2c});
    ASSERT_TRUE(TryDecodeJp2(bad_jp2, /*decode_image=*/false));
    EventCapture events;
    EXPECT_FALSE(TryDecodeJp2(bad_jp2, /*decode_image=*/true, &events));
    EXPECT_THAT(events.errors,
                Contains(HasSubstr("Failed to decode the codestream")));
  }
}

TEST(Jp2Test, TilePartialDecodeStrictThreadsAndExtraOptions) {
  constexpr const char* kInvalidOpts[] = {"BAD_OPT=1", nullptr};
  constexpr const char* kValidOpts[] = {"PLT=YES", "TLM=YES", nullptr};

  // 1. Extra options check & 2x2 tiled 16x16 JP2 encode via opj_write_tile.
  {
    OpjImagePtr dummy = CreateTestImage(8, 8, 3);
    ASSERT_NE(nullptr, dummy);
    OpjCodecPtr comp(opj_create_compress(OPJ_CODEC_JP2));
    ASSERT_NE(nullptr, comp);
    opj_cparameters_t cparams = MakeJp2LosslessParams();
    ASSERT_EQ(OPJ_TRUE, opj_setup_encoder(comp.get(), &cparams, dummy.get()));
    EXPECT_EQ(OPJ_TRUE, opj_encoder_set_extra_options(comp.get(), nullptr));
    EXPECT_EQ(OPJ_FALSE,
              opj_encoder_set_extra_options(comp.get(), kInvalidOpts));
  }

  const std::vector<uint8_t> tiled_jp2 = EncodeTiled16x16Jp2(
      /*numcomps=*/3, OPJ_CLRSPC_SRGB, OPJ_CODEC_JP2, 255, kValidOpts);
  ASSERT_FALSE(tiled_jp2.empty());

  // 2. Sequential decode via opj_jp2_read_tile_header + opj_jp2_decode_tile.
  {
    std::unique_ptr<DecodeSession> session =
        OpenForDecode(tiled_jp2, OPJ_CODEC_JP2);
    ASSERT_NE(nullptr, session);
    uint32_t tile_count = 0;
    for (;;) {
      OPJ_UINT32 tile_idx = 0;
      OPJ_UINT32 data_size = 0;
      OPJ_INT32 tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0;
      OPJ_UINT32 nb_comps = 0;
      OPJ_BOOL go_on = OPJ_FALSE;
      ASSERT_EQ(OPJ_TRUE, opj_read_tile_header(session->codec.get(),
                                               session->stream.get(), &tile_idx,
                                               &data_size, &tx0, &ty0, &tx1,
                                               &ty1, &nb_comps, &go_on));
      if (go_on == OPJ_FALSE) {
        break;
      }
      EXPECT_EQ(tile_count, tile_idx);
      EXPECT_EQ(8u * 8u * 3u, data_size);
      std::vector<OPJ_BYTE> buf(data_size, 0);
      ASSERT_EQ(OPJ_TRUE,
                opj_decode_tile_data(session->codec.get(), tile_idx, buf.data(),
                                     data_size, session->stream.get()));
      EXPECT_THAT(buf, ElementsAreArray(MakeTilePayload(tile_idx, 8, 8, 3)));
      ++tile_count;
    }
    EXPECT_EQ(4u, tile_count);
    EXPECT_EQ(OPJ_TRUE,
              opj_end_decompress(session->codec.get(), session->stream.get()));
  }

  // 3. Random-access tile decode (opj_jp2_get_tile) & out-of-range check.
  {
    std::unique_ptr<DecodeSession> session =
        OpenForDecode(tiled_jp2, OPJ_CODEC_JP2);
    ASSERT_NE(nullptr, session);
    ASSERT_EQ(OPJ_TRUE,
              opj_get_decoded_tile(session->codec.get(), session->stream.get(),
                                   session->image.get(), 2));
    EXPECT_EQ(0u, session->image->x0);
    EXPECT_EQ(8u, session->image->y0);
    EXPECT_EQ(8u, session->image->x1);
    EXPECT_EQ(16u, session->image->y1);
    EXPECT_EQ(OPJ_FALSE,
              opj_get_decoded_tile(session->codec.get(), session->stream.get(),
                                   session->image.get(), 4));
  }

  // 4. Repeated opj_jp2_get_tile on a 2x2 tiled palette JP2 image.
  {
    const std::vector<uint8_t> j2k_tiled = EncodeTiled16x16Jp2(
        /*numcomps=*/1, OPJ_CLRSPC_UNSPECIFIED, OPJ_CODEC_J2K, /*max_val=*/3);
    ASSERT_FALSE(j2k_tiled.empty());
    const auto ihdr =
        Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("ihdr"),
                               Jp2BoxBuilder::MakeIhdrPayload(16, 16, 1, 7));
    const auto colr =
        Jp2BoxBuilder::MakeBox(Jp2BoxBuilder::FourCC("colr"),
                               Jp2BoxBuilder::MakeColrEnumPayload(kEnumcsSrgb));
    constexpr std::array<uint8_t, 3> kBi3 = {7, 7, 7};
    constexpr std::array<uint32_t, 12> kEntries = {
        10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120,
    };
    const auto pclr = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("pclr"),
        Jp2BoxBuilder::MakePclrPayload(4, kBi3, kEntries));
    constexpr std::array<Jp2BoxBuilder::CmapEntry, 3> kCmap = {{
        {.cmp = 0, .mtyp = 1, .pcol = 0},
        {.cmp = 0, .mtyp = 1, .pcol = 1},
        {.cmp = 0, .mtyp = 1, .pcol = 2},
    }};
    const auto cmap = Jp2BoxBuilder::MakeBox(
        Jp2BoxBuilder::FourCC("cmap"), Jp2BoxBuilder::MakeCmapPayload(kCmap));
    const auto palette_jp2 = BuildStandardJp2(
        Jp2BoxBuilder::Concat({ihdr, colr, pclr, cmap}), j2k_tiled);

    std::unique_ptr<DecodeSession> session =
        OpenForDecode(palette_jp2, OPJ_CODEC_JP2);
    ASSERT_NE(nullptr, session);
    ASSERT_EQ(OPJ_TRUE,
              opj_get_decoded_tile(session->codec.get(), session->stream.get(),
                                   session->image.get(), 0));
    ASSERT_EQ(3u, session->image->numcomps);
    // Tile 0 starts with palette index (0*47)%4 = 0 and index 1 = (7)%4 = 3.
    EXPECT_EQ(10, session->image->comps[0].data[0]);
    EXPECT_EQ(20, session->image->comps[1].data[0]);
    EXPECT_EQ(30, session->image->comps[2].data[0]);
    EXPECT_EQ(100, session->image->comps[0].data[1]);
    EXPECT_EQ(110, session->image->comps[1].data[1]);
    EXPECT_EQ(120, session->image->comps[2].data[1]);
    ASSERT_EQ(OPJ_TRUE,
              opj_get_decoded_tile(session->codec.get(), session->stream.get(),
                                   session->image.get(), 3));
    ASSERT_EQ(3u, session->image->numcomps);
    // Tile 3 starts with palette index (3*47)%4 = 1 and index 1 = (148)%4 = 0.
    EXPECT_EQ(40, session->image->comps[0].data[0]);
    EXPECT_EQ(50, session->image->comps[1].data[0]);
    EXPECT_EQ(60, session->image->comps[2].data[0]);
    EXPECT_EQ(10, session->image->comps[0].data[1]);
    EXPECT_EQ(20, session->image->comps[1].data[1]);
    EXPECT_EQ(30, session->image->comps[2].data[1]);

    // opj_set_decoded_components bypasses palette expansion.
    std::unique_ptr<DecodeSession> comp_session =
        OpenForDecode(palette_jp2, OPJ_CODEC_JP2);
    ASSERT_NE(nullptr, comp_session);
    constexpr OPJ_UINT32 kComp0 = 0;
    ASSERT_EQ(OPJ_TRUE, opj_set_decoded_components(comp_session->codec.get(), 1,
                                                   &kComp0, OPJ_FALSE));
    ASSERT_EQ(OPJ_TRUE,
              opj_decode(comp_session->codec.get(), comp_session->stream.get(),
                         comp_session->image.get()));
    ASSERT_EQ(1u, comp_session->image->numcomps);
    EXPECT_EQ(0, comp_session->image->comps[0].data[0]);
  }

  // 5. Partial decode area, reduced resolution factor, and multi-thread decode.
  {
    std::unique_ptr<DecodeSession> session = OpenForDecode(
        tiled_jp2, OPJ_CODEC_JP2,
        {.threads = 2, .area = std::array<int32_t, 4>{4, 4, 12, 12}});
    ASSERT_NE(nullptr, session);
    ASSERT_EQ(OPJ_TRUE, opj_decode(session->codec.get(), session->stream.get(),
                                   session->image.get()));
    EXPECT_EQ(8u, session->image->comps[0].w);
    EXPECT_EQ(8u, session->image->comps[0].h);
  }
  {
    std::unique_ptr<DecodeSession> session =
        OpenForDecode(tiled_jp2, OPJ_CODEC_JP2, {.resolution_factor = 1});
    ASSERT_NE(nullptr, session);
    ASSERT_EQ(OPJ_TRUE, opj_decode(session->codec.get(), session->stream.get(),
                                   session->image.get()));
    EXPECT_EQ(8u, session->image->comps[0].w);
    EXPECT_EQ(8u, session->image->comps[0].h);
  }

  // 6. Strict vs non-strict mode on JP2 with truncated EOC.
  {
    OpjImagePtr src = CreateTestImage(8, 8, 1, 8, 0, OPJ_CLRSPC_GRAY);
    ASSERT_NE(nullptr, src);
    opj_cparameters_t cparams = MakeJp2LosslessParams();
    std::optional<std::vector<uint8_t>> jp2 =
        CompressImageToBytes(src.get(), OPJ_CODEC_JP2, &cparams);
    ASSERT_TRUE(jp2.has_value());
    ASSERT_GE(jp2->size(), 2u);
    ASSERT_EQ(0xFFU, (*jp2)[jp2->size() - 2]);
    ASSERT_EQ(0xD9U, (*jp2)[jp2->size() - 1]);
    jp2->resize(jp2->size() - 2);

    std::unique_ptr<DecodeSession> strict_session =
        OpenForDecode(*jp2, OPJ_CODEC_JP2, {.strict = true});
    ASSERT_NE(nullptr, strict_session);
    EXPECT_EQ(OPJ_FALSE, opj_decode(strict_session->codec.get(),
                                    strict_session->stream.get(),
                                    strict_session->image.get()));

    std::unique_ptr<DecodeSession> lenient_session =
        OpenForDecode(*jp2, OPJ_CODEC_JP2, {.strict = false});
    ASSERT_NE(nullptr, lenient_session);
    EXPECT_EQ(OPJ_TRUE, opj_decode(lenient_session->codec.get(),
                                   lenient_session->stream.get(),
                                   lenient_session->image.get()));
  }
}

TEST(Jp2Test, DumpCodecAndCodestreamInfoAndIndex) {
  const std::vector<uint8_t> jp2 =
      EncodeTiled16x16Jp2(3, OPJ_CLRSPC_SRGB, OPJ_CODEC_JP2);
  ASSERT_FALSE(jp2.empty());

  std::unique_ptr<DecodeSession> session = OpenForDecode(jp2, OPJ_CODEC_JP2);
  ASSERT_NE(nullptr, session);

  ScopedCstrInfo info(opj_get_cstr_info(session->codec.get()));
  ASSERT_TRUE(info);
  EXPECT_EQ(3u, info->nbcomps);
  EXPECT_EQ(2u, info->tw);
  EXPECT_EQ(2u, info->th);
  EXPECT_EQ(8u, info->tdx);
  EXPECT_EQ(8u, info->tdy);

  ASSERT_EQ(OPJ_TRUE, opj_decode(session->codec.get(), session->stream.get(),
                                 session->image.get()));
  ASSERT_EQ(OPJ_TRUE,
            opj_end_decompress(session->codec.get(), session->stream.get()));

  ScopedCstrIndex index(opj_get_cstr_index(session->codec.get()));
  ASSERT_TRUE(index);
  EXPECT_GT(index->main_head_end, index->main_head_start);
  EXPECT_GT(index->marknum, 0u);
  EXPECT_EQ(4u, index->nb_of_tiles);

  TempFile dump_file("jp2_dump_test.txt");
  {
    ScopedFile out{std::fopen(dump_file.path_str().c_str(), "w")};
    ASSERT_NE(nullptr, out.fp);
    opj_dump_codec(
        session->codec.get(),
        OPJ_IMG_INFO | OPJ_J2K_MH_INFO | OPJ_J2K_TCH_INFO | OPJ_J2K_MH_IND,
        out.fp);
    opj_dump_codec(session->codec.get(), OPJ_JP2_INFO | OPJ_JP2_IND, out.fp);
  }
  std::ifstream ifs(dump_file.path());
  ASSERT_TRUE(ifs.is_open());
  const std::string text((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
  EXPECT_THAT(text, HasSubstr("Image info"));
  EXPECT_THAT(text, HasSubstr("Codestream info"));
  EXPECT_THAT(text, HasSubstr("Codestream index"));
  EXPECT_THAT(text, HasSubstr("Wrong flag"));
}

TEST(Jp2Test, EncoderStreamFailureAndValidationPaths) {
  auto make_encoder = [](opj_image_t* img, bool jpip_on = false) {
    OpjJp2Ptr jp2(opj_jp2_create(OPJ_FALSE));
    if (!jp2) {
      return OpjJp2Ptr(nullptr);
    }
    opj_cparameters_t cparams = MakeJp2LosslessParams();
    cparams.jpip_on = jpip_on ? OPJ_TRUE : OPJ_FALSE;
    opj_event_mgr_t mgr = MakeEventManager();
    if (opj_jp2_setup_encoder(jp2.get(), &cparams, img, &mgr) != OPJ_TRUE) {
      return OpjJp2Ptr(nullptr);
    }
    return jp2;
  };

  // 1. Non-seekable stream fails opj_jp2_default_validation.
  {
    OpjImagePtr img = CreateTestImage(8, 8, 3);
    ASSERT_NE(nullptr, img);
    OpjJp2Ptr jp2 = make_encoder(img.get());
    ASSERT_NE(nullptr, jp2);
    MemWriteStream out_state;
    out_state.allow_seek = false;
    OpjStreamPtr stream = out_state.CreateStream();
    opj_event_mgr_t mgr = MakeEventManager();
    EXPECT_EQ(OPJ_FALSE, opj_jp2_start_compress(jp2.get(), AsPrivate(stream),
                                                img.get(), &mgr));
  }

  // 2. Internal opj_jp2_default_validation failure branches.
  auto verify_validation_fail = [&](auto mutator) {
    OpjImagePtr img = CreateTestImage(8, 8, 3);
    ASSERT_NE(nullptr, img);
    OpjJp2Ptr jp2 = make_encoder(img.get());
    ASSERT_NE(nullptr, jp2);
    mutator(jp2.get());
    MemWriteStream out_state;
    OpjStreamPtr stream = out_state.CreateStream();
    opj_event_mgr_t mgr = MakeEventManager();
    EXPECT_EQ(OPJ_FALSE, opj_jp2_start_compress(jp2.get(), AsPrivate(stream),
                                                img.get(), &mgr));
  };
  verify_validation_fail(
      [](opj_jp2_t* jp2) { jp2->jp2_state = JP2_STATE_SIGNATURE; });
  verify_validation_fail(
      [](opj_jp2_t* jp2) { jp2->jp2_img_state = JP2_IMG_STATE_UNKNOWN; });
  verify_validation_fail([](opj_jp2_t* jp2) { jp2->numcl = 0; });
  verify_validation_fail([](opj_jp2_t* jp2) { jp2->w = 0; });
  verify_validation_fail([](opj_jp2_t* jp2) { jp2->h = 0; });
  verify_validation_fail([](opj_jp2_t* jp2) { jp2->comps[0].bpcc = 38; });
  verify_validation_fail([](opj_jp2_t* jp2) { jp2->meth = 0; });
  verify_validation_fail([](opj_jp2_t* jp2) { jp2->meth = 3; });

  // 3. Header writing failures at exact box boundaries: 0 (before 12-byte jp
  // signature box), 12 (after jp / before 20-byte ftyp), 32 (after ftyp /
  // before 8-byte jp2h header), 40 (inside jp2h child boxes), plus skip_jp2c
  // failure.
  for (size_t limit : {size_t{0}, size_t{12}, size_t{32}, size_t{40}}) {
    SCOPED_TRACE(limit);
    OpjImagePtr img = CreateTestImage(8, 8, 3);
    ASSERT_NE(nullptr, img);
    OpjJp2Ptr jp2 = make_encoder(img.get());
    ASSERT_NE(nullptr, jp2);
    MemWriteStream out_state;
    out_state.max_bytes_before_fail = limit;
    OpjStreamPtr stream = out_state.CreateStream(/*buffer_size=*/4U);
    opj_event_mgr_t mgr = MakeEventManager();
    EXPECT_EQ(OPJ_FALSE, opj_jp2_start_compress(jp2.get(), AsPrivate(stream),
                                                img.get(), &mgr));
  }
  {
    OpjImagePtr img = CreateTestImage(8, 8, 3);
    ASSERT_NE(nullptr, img);
    OpjJp2Ptr jp2 = make_encoder(img.get());
    ASSERT_NE(nullptr, jp2);
    MemWriteStream out_state;
    out_state.fail_skip = true;
    OpjStreamPtr stream = out_state.CreateStream();
    opj_event_mgr_t mgr = MakeEventManager();
    EXPECT_EQ(OPJ_FALSE, opj_jp2_start_compress(jp2.get(), AsPrivate(stream),
                                                img.get(), &mgr));
  }

  // 4. jpip_on = OPJ_TRUE works even with USE_JPIP disabled because
  // opj_jpip_skip_iptr() is compiled unconditionally outside #ifdef USE_JPIP
  // and writes a zero-filled placeholder iptr box (success & fail_skip).
  {
    OpjImagePtr img = CreateTestImage(8, 8, 3);
    ASSERT_NE(nullptr, img);
    OpjJp2Ptr jp2 = make_encoder(img.get(), /*jpip_on=*/true);
    ASSERT_NE(nullptr, jp2);
    MemWriteStream out_state;
    out_state.fail_skip = true;
    OpjStreamPtr stream = out_state.CreateStream();
    opj_event_mgr_t mgr = MakeEventManager();
    EXPECT_EQ(OPJ_FALSE, opj_jp2_start_compress(jp2.get(), AsPrivate(stream),
                                                img.get(), &mgr));
  }
  {
    OpjImagePtr img = CreateTestImage(8, 8, 3);
    ASSERT_NE(nullptr, img);
    OpjJp2Ptr jp2 = make_encoder(img.get(), /*jpip_on=*/true);
    ASSERT_NE(nullptr, jp2);
    MemWriteStream out_state;
    OpjStreamPtr stream = out_state.CreateStream();
    opj_event_mgr_t mgr = MakeEventManager();
    ASSERT_EQ(OPJ_TRUE, opj_jp2_start_compress(jp2.get(), AsPrivate(stream),
                                               img.get(), &mgr));
    EXPECT_EQ(OPJ_TRUE, opj_jp2_encode(jp2.get(), AsPrivate(stream), &mgr));
    EXPECT_EQ(OPJ_TRUE,
              opj_jp2_end_compress(jp2.get(), AsPrivate(stream), &mgr));
  }

  // 5. opj_jp2_end_compress & opj_jp2_write_jp2c failure paths:
  // (a) j2k_end_compress fails EOC write, (b) write_jp2c 1st seek fails,
  // (c) write_jp2c 8-byte write fails, (d) write_jp2c 2nd seek fails.
  enum class EndFailMode {
    kJ2kEndCompressWriteFail,
    kJp2cFirstSeekFail,
    kJp2cWriteHeaderFail,
    kJp2cSecondSeekFail,
  };
  for (EndFailMode mode : {
           EndFailMode::kJ2kEndCompressWriteFail,
           EndFailMode::kJp2cFirstSeekFail,
           EndFailMode::kJp2cWriteHeaderFail,
           EndFailMode::kJp2cSecondSeekFail,
       }) {
    OpjImagePtr img = CreateTestImage(8, 8, 3);
    ASSERT_NE(nullptr, img);
    OpjJp2Ptr jp2 = make_encoder(img.get());
    ASSERT_NE(nullptr, jp2);
    MemWriteStream out_state;
    const size_t buf_sz = (mode == EndFailMode::kJp2cSecondSeekFail) ? 64U : 4U;
    OpjStreamPtr stream = out_state.CreateStream(buf_sz);
    EventCapture events;
    opj_event_mgr_t mgr = MakeEventManager(&events);
    ASSERT_EQ(OPJ_TRUE, opj_jp2_start_compress(jp2.get(), AsPrivate(stream),
                                               img.get(), &mgr));
    ASSERT_EQ(OPJ_TRUE, opj_jp2_encode(jp2.get(), AsPrivate(stream), &mgr));

    switch (mode) {
      case EndFailMode::kJ2kEndCompressWriteFail:
        out_state.max_bytes_before_fail = out_state.bytes_written;
        break;
      case EndFailMode::kJp2cFirstSeekFail:
        out_state.fail_seek_on_call = out_state.seek_calls + 1;
        break;
      case EndFailMode::kJp2cWriteHeaderFail:
        out_state.max_bytes_before_fail = out_state.bytes_written + 2U;
        break;
      case EndFailMode::kJp2cSecondSeekFail:
        out_state.fail_seek_on_call = out_state.seek_calls + 2;
        break;
    }
    EXPECT_EQ(OPJ_FALSE,
              opj_jp2_end_compress(jp2.get(), AsPrivate(stream), &mgr));
  }
}

// TODO(schwehr): Cover remaining allocator OOM guards, unreachable defensive
// checks (e.g., has_jp2h != 0 && has_ihdr == 0), and opj_jp2_free_pclr when
// cmap != nullptr in jp2.c.

}  // namespace
}  // namespace openjpeg2
