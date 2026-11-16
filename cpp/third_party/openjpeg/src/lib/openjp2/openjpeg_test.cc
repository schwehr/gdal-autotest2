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
// Tests public OpenJPEG codec, stream, tile, MCT, and image lifecycle APIs
// (src/lib/openjp2/openjpeg.c).
//
// Caveats:
// - _WIN32 and USE_JPWL branches in openjpeg.c are not compiled in google3, and
//   opj_get_jp2_metadata / opj_get_jp2_index in openjpeg.h have no impl.
// - google3 always builds with -DMUTEX_pthread=1, and opj_config.h is
//   Copybara-excluded and may lag METADATA (version checks pin
//   OPJ_VERSION_MAJOR).
// - Several assertions pin exact upstream error/dump message text.
//
// See also:
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/openjpeg.c
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/openjpeg.h

#include "third_party/openjpeg2/src/lib/openjp2/openjpeg.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/openjpeg2/google/util/test_util.h"

namespace openjpeg2 {
namespace {

using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;

static_assert(OPJ_TRUE == 1 && OPJ_FALSE == 0);
static_assert(OPJ_STREAM_READ == OPJ_TRUE && OPJ_STREAM_WRITE == OPJ_FALSE);

constexpr uint32_t kImageWidth = 16;
constexpr uint32_t kImageHeight = 16;
constexpr uint32_t kNumComps = 3;
constexpr uint32_t kTileWidth = 8;
constexpr uint32_t kTileHeight = 8;
constexpr uint32_t kNumResolutions = 3;

struct CodecFormatCase {
  OPJ_CODEC_FORMAT format;
  std::string_view name;
  bool expect_supported;
};

constexpr CodecFormatCase kCodecFormatCases[] = {
    {.format = OPJ_CODEC_J2K,
     .name = "OPJ_CODEC_J2K",
     .expect_supported = true},
    {.format = OPJ_CODEC_JP2,
     .name = "OPJ_CODEC_JP2",
     .expect_supported = true},
    {.format = OPJ_CODEC_UNKNOWN,
     .name = "OPJ_CODEC_UNKNOWN",
     .expect_supported = false},
    {.format = OPJ_CODEC_JPT,
     .name = "OPJ_CODEC_JPT",
     .expect_supported = false},
    {.format = OPJ_CODEC_JPP,
     .name = "OPJ_CODEC_JPP",
     .expect_supported = false},
    {.format = OPJ_CODEC_JPX,
     .name = "OPJ_CODEC_JPX",
     .expect_supported = false},
    // One past OPJ_CODEC_JPX (6) in openjpeg.h; bump if upstream adds formats.
    {.format = static_cast<OPJ_CODEC_FORMAT>(7),
     .name = "unsupported_enum",
     .expect_supported = false},
};

constexpr OPJ_CODEC_FORMAT kSupportedFormats[] = {
    OPJ_CODEC_J2K,
    OPJ_CODEC_JP2,
};

opj_cparameters_t MakeOpenjpegLosslessParams(bool tiled = false) {
  std::optional<TileGeometry> tile_geom;
  if (tiled) {
    tile_geom = TileGeometry{.tdx = kTileWidth, .tdy = kTileHeight};
  }
  opj_cparameters_t params = MakeLosslessParams(kNumResolutions, tile_geom);
  params.tcp_mct = 0;
  return params;
}

std::vector<OPJ_BYTE> MakeTilePayload(uint32_t tile_index, uint32_t tile_w,
                                      uint32_t tile_h, uint32_t numcomps) {
  const size_t samples_per_comp = static_cast<size_t>(tile_w) * tile_h;
  std::vector<OPJ_BYTE> bytes(samples_per_comp * numcomps);
  for (uint32_t c = 0; c < numcomps; ++c) {
    for (uint32_t y = 0; y < tile_h; ++y) {
      for (uint32_t x = 0; x < tile_w; ++x) {
        const uint32_t value =
            (tile_index * 47u + c * 31u + y * 13u + x * 7u) & 0xFFu;
        bytes[c * samples_per_comp + y * tile_w + x] =
            static_cast<OPJ_BYTE>(value);
      }
    }
  }
  return bytes;
}

std::vector<uint8_t> EncodeMultiTileImage(OPJ_CODEC_FORMAT format) {
  std::array<opj_image_cmptparm_t, kNumComps> cmpt_params{};
  for (auto& param : cmpt_params) {
    param.dx = 1;
    param.dy = 1;
    param.w = kImageWidth;
    param.h = kImageHeight;
    param.prec = 8;
    param.sgnd = 0;
  }
  OpjImagePtr image(
      opj_image_tile_create(kNumComps, cmpt_params.data(), OPJ_CLRSPC_SRGB));
  if (!image) {
    ADD_FAILURE() << "opj_image_tile_create failed";
    return {};
  }
  image->x0 = 0;
  image->y0 = 0;
  image->x1 = kImageWidth;
  image->y1 = kImageHeight;
  for (OPJ_UINT32 c = 0; c < image->numcomps; ++c) {
    EXPECT_EQ(nullptr, image->comps[c].data);
  }

  opj_cparameters_t cparams = MakeOpenjpegLosslessParams(/*tiled=*/true);
  OpjCodecPtr codec(opj_create_compress(format));
  if (!codec) {
    ADD_FAILURE() << "opj_create_compress failed";
    return {};
  }
  if (opj_setup_encoder(codec.get(), &cparams, image.get()) != OPJ_TRUE) {
    ADD_FAILURE() << "opj_setup_encoder failed";
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
        MakeTilePayload(tile_idx, kTileWidth, kTileHeight, kNumComps);
    if (opj_write_tile(codec.get(), tile_idx, payload.data(),
                       static_cast<OPJ_UINT32>(payload.size()),
                       stream.get()) != OPJ_TRUE) {
      ADD_FAILURE() << "opj_write_tile failed for tile " << tile_idx;
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

void VerifyFileStreamRoundTrip(OPJ_CODEC_FORMAT format,
                               std::string_view filename,
                               std::optional<OPJ_SIZE_T> chunk_size) {
  OpjImagePtr src = CreateTestImage(kImageWidth, kImageHeight, kNumComps);
  ASSERT_NE(nullptr, src);
  const std::vector<std::vector<OPJ_INT32>> expected =
      SnapshotImagePixels(src.get());

  auto open_file_stream = [&](const char* path, OPJ_BOOL is_read) {
    if (chunk_size.has_value()) {
      return OpjStreamPtr(
          opj_stream_create_file_stream(path, *chunk_size, is_read));
    }
    return OpjStreamPtr(opj_stream_create_default_file_stream(path, is_read));
  };

  TempFile temp_file(filename);
  {
    OpjCodecPtr comp(opj_create_compress(format));
    ASSERT_NE(nullptr, comp);
    opj_cparameters_t cparams = MakeOpenjpegLosslessParams();
    ASSERT_EQ(OPJ_TRUE, opj_setup_encoder(comp.get(), &cparams, src.get()));
    OpjStreamPtr write_stream =
        open_file_stream(temp_file.path_str().c_str(), OPJ_STREAM_WRITE);
    ASSERT_NE(nullptr, write_stream);
    ASSERT_EQ(OPJ_TRUE,
              opj_start_compress(comp.get(), src.get(), write_stream.get()));
    ASSERT_EQ(OPJ_TRUE, opj_encode(comp.get(), write_stream.get()));
    ASSERT_EQ(OPJ_TRUE, opj_end_compress(comp.get(), write_stream.get()));
    // Destroy write stream before reopening so buffered output is flushed.
    write_stream.reset();
  }
  EXPECT_TRUE(std::filesystem::exists(temp_file.path()));
  EXPECT_GT(std::filesystem::file_size(temp_file.path()), 0u);

  OpjStreamPtr read_stream =
      open_file_stream(temp_file.path_str().c_str(), OPJ_STREAM_READ);
  ASSERT_NE(nullptr, read_stream);
  OpjCodecPtr decomp(opj_create_decompress(format));
  ASSERT_NE(nullptr, decomp);
  opj_dparameters_t dparams;
  opj_set_default_decoder_parameters(&dparams);
  ASSERT_EQ(OPJ_TRUE, opj_setup_decoder(decomp.get(), &dparams));
  opj_image_t* raw_decoded = nullptr;
  ASSERT_EQ(OPJ_TRUE,
            opj_read_header(read_stream.get(), decomp.get(), &raw_decoded));
  OpjImagePtr decoded(raw_decoded);
  ASSERT_NE(nullptr, decoded);
  ASSERT_EQ(OPJ_TRUE,
            opj_decode(decomp.get(), read_stream.get(), decoded.get()));
  ASSERT_EQ(OPJ_TRUE, opj_end_decompress(decomp.get(), read_stream.get()));
  ExpectImagePixelsEqual(expected, decoded.get());
}

TEST(OpenjpegTest, VersionAndThreadSupportSmoke) {
  const char* version = opj_version();
  ASSERT_NE(nullptr, version);
  const std::string_view version_sv(version);
  EXPECT_FALSE(version_sv.empty());
  // opj_config.h is Copybara-excluded and may lag METADATA; pin major only.
  EXPECT_TRUE(version_sv.starts_with(std::to_string(OPJ_VERSION_MAJOR) + "."));
  // google3 always builds -DMUTEX_pthread=1.
  EXPECT_EQ(OPJ_TRUE, opj_has_thread_support());
  EXPECT_GE(opj_get_num_cpus(), 1);
}

TEST(OpenjpegTest, CreateCodecFormatsAndDestroyNullSafe) {
  opj_destroy_codec(nullptr);

  for (const auto& tc : kCodecFormatCases) {
    SCOPED_TRACE(std::string(tc.name));
    OpjCodecPtr decomp(opj_create_decompress(tc.format));
    OpjCodecPtr comp(opj_create_compress(tc.format));
    if (tc.expect_supported) {
      EXPECT_NE(nullptr, decomp.get());
      EXPECT_NE(nullptr, comp.get());
    } else {
      EXPECT_EQ(nullptr, decomp.get());
      EXPECT_EQ(nullptr, comp.get());
    }
  }
}

TEST(OpenjpegTest, EventHandlersRejectNullAndDispatchEvents) {
  EventCapture capture;
  EXPECT_EQ(OPJ_FALSE,
            opj_set_info_handler(nullptr, &EventCapture::OnInfo, &capture));
  EXPECT_EQ(OPJ_FALSE, opj_set_warning_handler(
                           nullptr, &EventCapture::OnWarning, &capture));
  EXPECT_EQ(OPJ_FALSE,
            opj_set_error_handler(nullptr, &EventCapture::OnError, &capture));

  OpjCodecPtr comp(opj_create_compress(OPJ_CODEC_J2K));
  ASSERT_NE(nullptr, comp);
  EXPECT_EQ(OPJ_TRUE,
            opj_set_info_handler(comp.get(), &EventCapture::OnInfo, &capture));
  EXPECT_EQ(OPJ_TRUE, opj_set_warning_handler(
                          comp.get(), &EventCapture::OnWarning, &capture));
  EXPECT_EQ(OPJ_TRUE, opj_set_error_handler(comp.get(), &EventCapture::OnError,
                                            &capture));

  // Passing a compressor to opj_setup_decoder triggers an error event.
  opj_dparameters_t dparams;
  opj_set_default_decoder_parameters(&dparams);
  EXPECT_EQ(OPJ_FALSE, opj_setup_decoder(comp.get(), &dparams));
  EXPECT_THAT(capture.errors, ElementsAre(HasSubstr("not a decompressor")));

  // Encode a valid image and trigger info + warning events via decode area.
  OpjImagePtr src = CreateTestImage(kImageWidth, kImageHeight, kNumComps);
  ASSERT_NE(nullptr, src);
  opj_cparameters_t cparams = MakeOpenjpegLosslessParams();
  std::optional<std::vector<uint8_t>> encoded =
      CompressImageToBytes(src.get(), OPJ_CODEC_J2K, &cparams);
  ASSERT_TRUE(encoded.has_value());

  capture.Clear();
  std::unique_ptr<DecodeSession> session =
      OpenForDecode(*encoded, OPJ_CODEC_J2K, {.events = &capture});
  ASSERT_NE(nullptr, session);
  EXPECT_EQ(OPJ_TRUE, opj_set_decode_area(session->codec.get(),
                                          session->image.get(), 0, 0, 0, 0));
  EXPECT_EQ(OPJ_TRUE,
            opj_set_decode_area(session->codec.get(), session->image.get(), 0,
                                0, 999, 999));
  EXPECT_THAT(capture.infos, Not(IsEmpty()));
  EXPECT_THAT(capture.warnings, Not(IsEmpty()));
}

TEST(OpenjpegTest, DefaultParametersResetSentinelValuesAndHandleNull) {
  opj_set_default_decoder_parameters(nullptr);
  opj_set_default_encoder_parameters(nullptr);

  opj_dparameters_t dparams;
  std::memset(&dparams, 0xA5, sizeof(dparams));
  opj_set_default_decoder_parameters(&dparams);
  EXPECT_EQ(0u, dparams.cp_layer);
  EXPECT_EQ(0u, dparams.cp_reduce);
  EXPECT_EQ(-1, dparams.decod_format);
  EXPECT_EQ(-1, dparams.cod_format);
  EXPECT_EQ(0u, dparams.flags);
  EXPECT_EQ(0u, dparams.DA_x0);

  opj_cparameters_t cparams;
  std::memset(&cparams, 0xA5, sizeof(cparams));
  opj_set_default_encoder_parameters(&cparams);
  EXPECT_EQ(OPJ_PROFILE_NONE, cparams.rsiz);
  EXPECT_EQ(0, cparams.max_comp_size);
  EXPECT_EQ(6, cparams.numresolution);
  EXPECT_EQ(64, cparams.cblockw_init);
  EXPECT_EQ(64, cparams.cblockh_init);
  EXPECT_EQ(OPJ_LRCP, cparams.prog_order);
  EXPECT_EQ(-1, cparams.roi_compno);
  EXPECT_EQ(1, cparams.subsampling_dx);
  EXPECT_EQ(1, cparams.subsampling_dy);
  EXPECT_EQ(0, cparams.tp_on);
  EXPECT_EQ(-1, cparams.decod_format);
  EXPECT_EQ(-1, cparams.cod_format);
  EXPECT_EQ(0, cparams.tcp_numlayers);
  EXPECT_FLOAT_EQ(0.0f, cparams.tcp_rates[0]);
  EXPECT_EQ(0, cparams.cp_disto_alloc);
  EXPECT_EQ(0, cparams.cp_fixed_alloc);
  EXPECT_EQ(0, cparams.cp_fixed_quality);
  EXPECT_EQ(OPJ_FALSE, cparams.jpip_on);
  EXPECT_EQ(nullptr, cparams.mct_data);
}

TEST(OpenjpegTest, CodecSetThreadsValidationAndMultiThreadedRoundTrip) {
  EXPECT_EQ(OPJ_FALSE, opj_codec_set_threads(nullptr, 1));

  for (OPJ_CODEC_FORMAT format : kSupportedFormats) {
    SCOPED_TRACE(format);
    OpjCodecPtr comp(opj_create_compress(format));
    OpjCodecPtr decomp(opj_create_decompress(format));
    ASSERT_NE(nullptr, comp);
    ASSERT_NE(nullptr, decomp);
    EXPECT_EQ(OPJ_FALSE, opj_codec_set_threads(comp.get(), -1));
    EXPECT_EQ(OPJ_FALSE, opj_codec_set_threads(decomp.get(), -1));
    EXPECT_EQ(OPJ_TRUE, opj_codec_set_threads(comp.get(), 0));
    EXPECT_EQ(OPJ_TRUE, opj_codec_set_threads(decomp.get(), 0));
    EXPECT_EQ(OPJ_TRUE, opj_codec_set_threads(comp.get(), 2));
    EXPECT_EQ(OPJ_TRUE, opj_codec_set_threads(decomp.get(), 2));
  }

  // Run an end-to-end 2-thread encode/decode round-trip (TSan-checked).
  OpjImagePtr src = CreateTestImage(kImageWidth, kImageHeight, kNumComps);
  ASSERT_NE(nullptr, src);
  const std::vector<std::vector<OPJ_INT32>> expected =
      SnapshotImagePixels(src.get());

  opj_cparameters_t cparams = MakeOpenjpegLosslessParams(/*tiled=*/true);
  OpjCodecPtr comp(opj_create_compress(OPJ_CODEC_J2K));
  ASSERT_NE(nullptr, comp);
  ASSERT_EQ(OPJ_TRUE, opj_setup_encoder(comp.get(), &cparams, src.get()));
  ASSERT_EQ(OPJ_TRUE, opj_codec_set_threads(comp.get(), 2));

  MemWriteStream mem_out;
  OpjStreamPtr out_stream = mem_out.CreateStream();
  ASSERT_NE(nullptr, out_stream);
  ASSERT_EQ(OPJ_TRUE,
            opj_start_compress(comp.get(), src.get(), out_stream.get()));
  ASSERT_EQ(OPJ_TRUE, opj_encode(comp.get(), out_stream.get()));
  ASSERT_EQ(OPJ_TRUE, opj_end_compress(comp.get(), out_stream.get()));
  out_stream.reset();

  MemReadStream mem_in(mem_out.data);
  OpjStreamPtr in_stream = mem_in.CreateStream();
  ASSERT_NE(nullptr, in_stream);
  OpjCodecPtr decomp(opj_create_decompress(OPJ_CODEC_J2K));
  ASSERT_NE(nullptr, decomp);
  opj_dparameters_t dparams;
  opj_set_default_decoder_parameters(&dparams);
  ASSERT_EQ(OPJ_TRUE, opj_setup_decoder(decomp.get(), &dparams));
  ASSERT_EQ(OPJ_TRUE, opj_codec_set_threads(decomp.get(), 2));

  opj_image_t* raw_decoded = nullptr;
  ASSERT_EQ(OPJ_TRUE,
            opj_read_header(in_stream.get(), decomp.get(), &raw_decoded));
  OpjImagePtr decoded(raw_decoded);
  ASSERT_NE(nullptr, decoded);
  ASSERT_EQ(OPJ_TRUE, opj_decode(decomp.get(), in_stream.get(), decoded.get()));
  ASSERT_EQ(OPJ_TRUE, opj_end_decompress(decomp.get(), in_stream.get()));
  ExpectImagePixelsEqual(expected, decoded.get());
}

TEST(OpenjpegTest, NullAndWrongDirectionGuardsReturnFalse) {
  OpjImagePtr dummy_image = CreateTestImage(8, 8, 1);
  ASSERT_NE(nullptr, dummy_image);
  MemWriteStream mem_out;
  OpjStreamPtr write_stream = mem_out.CreateStream();
  ASSERT_NE(nullptr, write_stream);
  MemReadStream mem_in(std::array<uint8_t, 4>{0, 0, 0, 0});
  OpjStreamPtr read_stream = mem_in.CreateStream();
  ASSERT_NE(nullptr, read_stream);

  EventCapture comp_events;
  EventCapture decomp_events;
  OpjCodecPtr comp(opj_create_compress(OPJ_CODEC_J2K));
  OpjCodecPtr decomp(opj_create_decompress(OPJ_CODEC_J2K));
  ASSERT_NE(nullptr, comp);
  ASSERT_NE(nullptr, decomp);
  comp_events.AttachTo(comp.get());
  decomp_events.AttachTo(decomp.get());

  opj_dparameters_t dparams;
  opj_set_default_decoder_parameters(&dparams);
  opj_cparameters_t cparams = MakeOpenjpegLosslessParams();

  // Decompression wrappers with null args or a compressor handle.
  EXPECT_EQ(OPJ_FALSE, opj_setup_decoder(nullptr, &dparams));
  EXPECT_EQ(OPJ_FALSE, opj_setup_decoder(decomp.get(), nullptr));
  EXPECT_EQ(OPJ_FALSE, opj_setup_decoder(comp.get(), &dparams));

  EXPECT_EQ(OPJ_FALSE, opj_decoder_set_strict_mode(nullptr, OPJ_TRUE));
  EXPECT_EQ(OPJ_FALSE, opj_decoder_set_strict_mode(comp.get(), OPJ_TRUE));

  opj_image_t* out_image = nullptr;
  EXPECT_EQ(OPJ_FALSE, opj_read_header(nullptr, decomp.get(), &out_image));
  EXPECT_EQ(OPJ_FALSE, opj_read_header(read_stream.get(), nullptr, &out_image));
  EXPECT_EQ(OPJ_FALSE,
            opj_read_header(read_stream.get(), comp.get(), &out_image));
  EXPECT_EQ(nullptr, out_image);

  constexpr OPJ_UINT32 kCompIdx = 0;
  EXPECT_EQ(OPJ_FALSE,
            opj_set_decoded_components(nullptr, 1, &kCompIdx, OPJ_FALSE));
  EXPECT_EQ(OPJ_FALSE,
            opj_set_decoded_components(comp.get(), 1, &kCompIdx, OPJ_FALSE));
  EXPECT_EQ(OPJ_FALSE,
            opj_set_decoded_components(decomp.get(), 1, &kCompIdx, OPJ_TRUE));

  EXPECT_EQ(OPJ_FALSE,
            opj_decode(nullptr, read_stream.get(), dummy_image.get()));
  EXPECT_EQ(OPJ_FALSE, opj_decode(decomp.get(), nullptr, dummy_image.get()));
  EXPECT_EQ(OPJ_FALSE,
            opj_decode(comp.get(), read_stream.get(), dummy_image.get()));

  EXPECT_EQ(OPJ_FALSE,
            opj_set_decode_area(nullptr, dummy_image.get(), 0, 0, 8, 8));
  EXPECT_EQ(OPJ_FALSE,
            opj_set_decode_area(comp.get(), dummy_image.get(), 0, 0, 8, 8));

  OPJ_UINT32 tile_idx = 0;
  OPJ_UINT32 data_size = 0;
  OPJ_INT32 tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0;
  OPJ_UINT32 nb_comps = 0;
  OPJ_BOOL should_go_on = OPJ_FALSE;
  EXPECT_EQ(OPJ_FALSE, opj_read_tile_header(
                           nullptr, read_stream.get(), &tile_idx, &data_size,
                           &tx0, &ty0, &tx1, &ty1, &nb_comps, &should_go_on));
  EXPECT_EQ(OPJ_FALSE, opj_read_tile_header(decomp.get(), nullptr, &tile_idx,
                                            &data_size, &tx0, &ty0, &tx1, &ty1,
                                            &nb_comps, &should_go_on));
  EXPECT_EQ(OPJ_FALSE, opj_read_tile_header(
                           decomp.get(), read_stream.get(), nullptr, &data_size,
                           &tx0, &ty0, &tx1, &ty1, &nb_comps, &should_go_on));
  EXPECT_EQ(OPJ_FALSE, opj_read_tile_header(
                           decomp.get(), read_stream.get(), &tile_idx, nullptr,
                           &tx0, &ty0, &tx1, &ty1, &nb_comps, &should_go_on));
  EXPECT_EQ(OPJ_FALSE, opj_read_tile_header(
                           comp.get(), read_stream.get(), &tile_idx, &data_size,
                           &tx0, &ty0, &tx1, &ty1, &nb_comps, &should_go_on));

  std::array<OPJ_BYTE, 16> tile_buf{};
  EXPECT_EQ(OPJ_FALSE, opj_decode_tile_data(nullptr, 0, tile_buf.data(), 16,
                                            read_stream.get()));
  EXPECT_EQ(OPJ_FALSE, opj_decode_tile_data(decomp.get(), 0, nullptr, 16,
                                            read_stream.get()));
  EXPECT_EQ(OPJ_FALSE, opj_decode_tile_data(decomp.get(), 0, tile_buf.data(),
                                            16, nullptr));
  EXPECT_EQ(OPJ_FALSE, opj_decode_tile_data(comp.get(), 0, tile_buf.data(), 16,
                                            read_stream.get()));

  EXPECT_EQ(OPJ_FALSE, opj_get_decoded_tile(nullptr, read_stream.get(),
                                            dummy_image.get(), 0));
  EXPECT_EQ(OPJ_FALSE,
            opj_get_decoded_tile(decomp.get(), nullptr, dummy_image.get(), 0));
  EXPECT_EQ(OPJ_FALSE, opj_get_decoded_tile(comp.get(), read_stream.get(),
                                            dummy_image.get(), 0));

  // opj_set_decoded_resolution_factor lacks an is_decompressor check in
  // openjpeg.c, so only test nullptr here (not a compressor handle).
  EXPECT_EQ(OPJ_FALSE, opj_set_decoded_resolution_factor(nullptr, 1));

  EXPECT_EQ(OPJ_FALSE, opj_end_decompress(nullptr, read_stream.get()));
  EXPECT_EQ(OPJ_FALSE, opj_end_decompress(decomp.get(), nullptr));
  EXPECT_EQ(OPJ_FALSE, opj_end_decompress(comp.get(), read_stream.get()));

  // Compression wrappers with null args or a decompressor handle.
  EXPECT_EQ(OPJ_FALSE, opj_setup_encoder(nullptr, &cparams, dummy_image.get()));
  EXPECT_EQ(OPJ_FALSE,
            opj_setup_encoder(comp.get(), nullptr, dummy_image.get()));
  EXPECT_EQ(OPJ_FALSE, opj_setup_encoder(comp.get(), &cparams, nullptr));
  EXPECT_EQ(OPJ_FALSE,
            opj_setup_encoder(decomp.get(), &cparams, dummy_image.get()));

  constexpr const char* kOptions[] = {"PLT=YES", nullptr};
  EXPECT_EQ(OPJ_FALSE, opj_encoder_set_extra_options(nullptr, kOptions));
  EXPECT_EQ(OPJ_FALSE, opj_encoder_set_extra_options(decomp.get(), kOptions));

  EXPECT_EQ(OPJ_FALSE,
            opj_start_compress(nullptr, dummy_image.get(), write_stream.get()));
  EXPECT_EQ(OPJ_FALSE,
            opj_start_compress(comp.get(), dummy_image.get(), nullptr));
  EXPECT_EQ(OPJ_FALSE, opj_start_compress(decomp.get(), dummy_image.get(),
                                          write_stream.get()));

  EXPECT_EQ(OPJ_FALSE, opj_encode(nullptr, write_stream.get()));
  EXPECT_EQ(OPJ_FALSE, opj_encode(comp.get(), nullptr));
  EXPECT_EQ(OPJ_FALSE, opj_encode(decomp.get(), write_stream.get()));

  EXPECT_EQ(OPJ_FALSE, opj_write_tile(nullptr, 0, tile_buf.data(), 16,
                                      write_stream.get()));
  EXPECT_EQ(OPJ_FALSE,
            opj_write_tile(comp.get(), 0, nullptr, 16, write_stream.get()));
  EXPECT_EQ(OPJ_FALSE,
            opj_write_tile(comp.get(), 0, tile_buf.data(), 16, nullptr));
  EXPECT_EQ(OPJ_FALSE, opj_write_tile(decomp.get(), 0, tile_buf.data(), 16,
                                      write_stream.get()));

  EXPECT_EQ(OPJ_FALSE, opj_end_compress(nullptr, write_stream.get()));
  EXPECT_EQ(OPJ_FALSE, opj_end_compress(comp.get(), nullptr));
  EXPECT_EQ(OPJ_FALSE, opj_end_compress(decomp.get(), write_stream.get()));

  // Introspection/dump null guards.
  opj_dump_codec(nullptr, OPJ_IMG_INFO, nullptr);
  EXPECT_EQ(nullptr, opj_get_cstr_info(nullptr));
  opj_destroy_cstr_info(nullptr);
  EXPECT_EQ(nullptr, opj_get_cstr_index(nullptr));
  opj_codestream_index_t* null_index = nullptr;
  opj_destroy_cstr_index(&null_index);

  EXPECT_THAT(comp_events.errors, Not(IsEmpty()));
  EXPECT_THAT(decomp_events.errors, Not(IsEmpty()));
}

TEST(OpenjpegTest, EncodeDecodeRoundTripAndExtraOptionsJ2kAndJp2) {
  constexpr const char* kValidOptions[] = {
      "PLT=YES",
      "TLM=YES",
      "GUARD_BITS=2",
      nullptr,
  };
  constexpr const char* kInvalidOptions[] = {"UNKNOWN_OPTION=1", nullptr};

  for (OPJ_CODEC_FORMAT format : kSupportedFormats) {
    SCOPED_TRACE(format);
    OpjImagePtr src = CreateTestImage(kImageWidth, kImageHeight, kNumComps);
    ASSERT_NE(nullptr, src);
    const std::vector<std::vector<OPJ_INT32>> expected =
        SnapshotImagePixels(src.get());

    // 1. Validate extra options handling (NULL, invalid, and valid arrays).
    opj_cparameters_t cparams = MakeOpenjpegLosslessParams();
    OpjCodecPtr comp(opj_create_compress(format));
    ASSERT_NE(nullptr, comp);
    ASSERT_EQ(OPJ_TRUE, opj_setup_encoder(comp.get(), &cparams, src.get()));
    EXPECT_EQ(OPJ_TRUE, opj_encoder_set_extra_options(comp.get(), nullptr));
    EXPECT_EQ(OPJ_FALSE,
              opj_encoder_set_extra_options(comp.get(), kInvalidOptions));
    ASSERT_EQ(OPJ_TRUE,
              opj_encoder_set_extra_options(comp.get(), kValidOptions));

    // 2. Encode to memory stream.
    MemWriteStream mem_out;
    OpjStreamPtr out_stream = mem_out.CreateStream();
    ASSERT_NE(nullptr, out_stream);
    ASSERT_EQ(OPJ_TRUE,
              opj_start_compress(comp.get(), src.get(), out_stream.get()));
    ASSERT_EQ(OPJ_TRUE, opj_encode(comp.get(), out_stream.get()));
    ASSERT_EQ(OPJ_TRUE, opj_end_compress(comp.get(), out_stream.get()));
    out_stream.reset();

    // 3. Decode back and verify exact lossless pixel recovery.
    std::unique_ptr<DecodeSession> session =
        OpenForDecode(mem_out.data, format);
    ASSERT_NE(nullptr, session);
    ASSERT_EQ(OPJ_TRUE, opj_decode(session->codec.get(), session->stream.get(),
                                   session->image.get()));
    ASSERT_EQ(OPJ_TRUE,
              opj_end_decompress(session->codec.get(), session->stream.get()));
    ExpectImagePixelsEqual(expected, session->image.get());
  }
}

TEST(OpenjpegTest, StrictModeHandlesTruncatedEoc) {
  OpjImagePtr src = CreateTestImage(kImageWidth, kImageHeight, kNumComps);
  ASSERT_NE(nullptr, src);
  const std::vector<std::vector<OPJ_INT32>> expected =
      SnapshotImagePixels(src.get());

  opj_cparameters_t cparams = MakeOpenjpegLosslessParams();
  std::optional<std::vector<uint8_t>> encoded =
      CompressImageToBytes(src.get(), OPJ_CODEC_J2K, &cparams);
  ASSERT_TRUE(encoded.has_value());
  ASSERT_GE(encoded->size(), 2u);
  // Strip trailing 2-byte EOC (0xFF 0xD9) to exercise strict vs lenient mode.
  ASSERT_EQ(0xFF, (*encoded)[encoded->size() - 2]);
  ASSERT_EQ(0xD9, (*encoded)[encoded->size() - 1]);
  encoded->resize(encoded->size() - 2);

  // Strict mode enabled (OPJ_TRUE): truncated EOC fails decode with error.
  {
    EventCapture events;
    std::unique_ptr<DecodeSession> session =
        OpenForDecode(*encoded, OPJ_CODEC_J2K, {.events = &events});
    ASSERT_NE(nullptr, session);
    EXPECT_EQ(OPJ_TRUE,
              opj_decoder_set_strict_mode(session->codec.get(), OPJ_TRUE));
    EXPECT_EQ(OPJ_FALSE, opj_decode(session->codec.get(), session->stream.get(),
                                    session->image.get()));
    EXPECT_THAT(events.errors, Not(IsEmpty()));
  }

  // Strict mode disabled (OPJ_FALSE): truncated EOC warns and recovers pixels.
  {
    EventCapture events;
    std::unique_ptr<DecodeSession> session =
        OpenForDecode(*encoded, OPJ_CODEC_J2K, {.events = &events});
    ASSERT_NE(nullptr, session);
    EXPECT_EQ(OPJ_TRUE,
              opj_decoder_set_strict_mode(session->codec.get(), OPJ_FALSE));
    EXPECT_EQ(OPJ_TRUE, opj_decode(session->codec.get(), session->stream.get(),
                                   session->image.get()));
    EXPECT_EQ(OPJ_TRUE,
              opj_end_decompress(session->codec.get(), session->stream.get()));
    EXPECT_THAT(events.warnings, Not(IsEmpty()));
    ExpectImagePixelsEqual(expected, session->image.get());
  }

  // Also verify strict mode toggle succeeds on JP2 decompressor.
  OpjCodecPtr jp2_decomp(opj_create_decompress(OPJ_CODEC_JP2));
  ASSERT_NE(nullptr, jp2_decomp);
  EXPECT_EQ(OPJ_TRUE, opj_decoder_set_strict_mode(jp2_decomp.get(), OPJ_TRUE));
  EXPECT_EQ(OPJ_TRUE, opj_decoder_set_strict_mode(jp2_decomp.get(), OPJ_FALSE));
}

TEST(OpenjpegTest, SetDecodeAreaSubregionAndWholeImage) {
  for (OPJ_CODEC_FORMAT format : kSupportedFormats) {
    SCOPED_TRACE(format);
    OpjImagePtr src = CreateTestImage(kImageWidth, kImageHeight, kNumComps);
    ASSERT_NE(nullptr, src);
    const std::vector<std::vector<OPJ_INT32>> full_pixels =
        SnapshotImagePixels(src.get());

    opj_cparameters_t cparams = MakeOpenjpegLosslessParams();
    std::optional<std::vector<uint8_t>> encoded =
        CompressImageToBytes(src.get(), format, &cparams);
    ASSERT_TRUE(encoded.has_value());

    // 1. Subregion [4, 12) x [4, 12).
    {
      std::unique_ptr<DecodeSession> session = OpenForDecode(*encoded, format);
      ASSERT_NE(nullptr, session);
      EXPECT_EQ(OPJ_FALSE,
                opj_set_decode_area(session->codec.get(), session->image.get(),
                                    -1, 0, 8, 8));
      ASSERT_EQ(OPJ_TRUE,
                opj_set_decode_area(session->codec.get(), session->image.get(),
                                    4, 4, 12, 12));
      ASSERT_EQ(OPJ_TRUE,
                opj_decode(session->codec.get(), session->stream.get(),
                           session->image.get()));
      ASSERT_EQ(OPJ_TRUE, opj_end_decompress(session->codec.get(),
                                             session->stream.get()));

      EXPECT_EQ(4u, session->image->x0);
      EXPECT_EQ(4u, session->image->y0);
      EXPECT_EQ(12u, session->image->x1);
      EXPECT_EQ(12u, session->image->y1);
      for (OPJ_UINT32 c = 0; c < kNumComps; ++c) {
        ASSERT_EQ(8u, session->image->comps[c].w);
        ASSERT_EQ(8u, session->image->comps[c].h);
        for (uint32_t y = 0; y < 8; ++y) {
          for (uint32_t x = 0; x < 8; ++x) {
            EXPECT_EQ(full_pixels[c][(y + 4) * kImageWidth + (x + 4)],
                      session->image->comps[c].data[y * 8 + x]);
          }
        }
      }
    }

    // 2. All-zero bounds (0, 0, 0, 0) decodes the whole image.
    {
      std::unique_ptr<DecodeSession> session = OpenForDecode(*encoded, format);
      ASSERT_NE(nullptr, session);
      ASSERT_EQ(OPJ_TRUE,
                opj_set_decode_area(session->codec.get(), session->image.get(),
                                    0, 0, 0, 0));
      ASSERT_EQ(OPJ_TRUE,
                opj_decode(session->codec.get(), session->stream.get(),
                           session->image.get()));
      ASSERT_EQ(OPJ_TRUE, opj_end_decompress(session->codec.get(),
                                             session->stream.get()));
      ExpectImagePixelsEqual(full_pixels, session->image.get());
    }
  }
}

TEST(OpenjpegTest, SetDecodedComponentsSelectsSubset) {
  for (OPJ_CODEC_FORMAT format : kSupportedFormats) {
    SCOPED_TRACE(format);
    OpjImagePtr src = CreateTestImage(kImageWidth, kImageHeight, kNumComps);
    ASSERT_NE(nullptr, src);
    const std::vector<std::vector<OPJ_INT32>> full_pixels =
        SnapshotImagePixels(src.get());

    opj_cparameters_t cparams = MakeOpenjpegLosslessParams();
    std::optional<std::vector<uint8_t>> encoded =
        CompressImageToBytes(src.get(), format, &cparams);
    ASSERT_TRUE(encoded.has_value());

    // Calling before opj_read_header must fail.
    {
      OpjCodecPtr pre_codec(opj_create_decompress(format));
      ASSERT_NE(nullptr, pre_codec);
      constexpr OPJ_UINT32 kIdx = 0;
      EXPECT_EQ(OPJ_FALSE, opj_set_decoded_components(pre_codec.get(), 1, &kIdx,
                                                      OPJ_FALSE));
    }

    std::unique_ptr<DecodeSession> session = OpenForDecode(*encoded, format);
    ASSERT_NE(nullptr, session);

    constexpr OPJ_UINT32 kOutOfRange = kNumComps;
    constexpr std::array<OPJ_UINT32, 2> kDuplicate = {1, 1};
    EXPECT_EQ(OPJ_FALSE, opj_set_decoded_components(session->codec.get(), 1,
                                                    &kOutOfRange, OPJ_FALSE));
    EXPECT_EQ(OPJ_FALSE,
              opj_set_decoded_components(session->codec.get(), 2,
                                         kDuplicate.data(), OPJ_FALSE));

    constexpr std::array<OPJ_UINT32, 2> kSubset = {2, 0};
    ASSERT_EQ(OPJ_TRUE,
              opj_set_decoded_components(
                  session->codec.get(), static_cast<OPJ_UINT32>(kSubset.size()),
                  kSubset.data(), OPJ_FALSE));
    ASSERT_EQ(OPJ_TRUE, opj_decode(session->codec.get(), session->stream.get(),
                                   session->image.get()));
    ASSERT_EQ(OPJ_TRUE,
              opj_end_decompress(session->codec.get(), session->stream.get()));

    const std::vector<std::vector<OPJ_INT32>> expected_subset = {
        full_pixels[2],
        full_pixels[0],
    };
    ExpectImagePixelsEqual(expected_subset, session->image.get());
  }
}

TEST(OpenjpegTest, SetDecodedResolutionFactorReducesDimensions) {
  for (OPJ_CODEC_FORMAT format : kSupportedFormats) {
    SCOPED_TRACE(format);
    OpjImagePtr src = CreateTestImage(kImageWidth, kImageHeight, kNumComps);
    ASSERT_NE(nullptr, src);
    opj_cparameters_t cparams = MakeOpenjpegLosslessParams();
    std::optional<std::vector<uint8_t>> encoded =
        CompressImageToBytes(src.get(), format, &cparams);
    ASSERT_TRUE(encoded.has_value());

    // Calling before opj_read_header fails because m_private_image is null.
    {
      OpjCodecPtr pre_codec(opj_create_decompress(format));
      ASSERT_NE(nullptr, pre_codec);
      EXPECT_EQ(OPJ_FALSE,
                opj_set_decoded_resolution_factor(pre_codec.get(), 1));
    }

    std::unique_ptr<DecodeSession> session = OpenForDecode(*encoded, format);
    ASSERT_NE(nullptr, session);
    // Factor >= numresolutions must fail.
    EXPECT_EQ(OPJ_FALSE, opj_set_decoded_resolution_factor(session->codec.get(),
                                                           kNumResolutions));
    ASSERT_EQ(OPJ_TRUE,
              opj_set_decoded_resolution_factor(session->codec.get(), 1));
    ASSERT_EQ(OPJ_TRUE, opj_decode(session->codec.get(), session->stream.get(),
                                   session->image.get()));
    ASSERT_EQ(OPJ_TRUE,
              opj_end_decompress(session->codec.get(), session->stream.get()));

    ASSERT_EQ(kNumComps, session->image->numcomps);
    for (OPJ_UINT32 c = 0; c < kNumComps; ++c) {
      EXPECT_EQ(kImageWidth / 2, session->image->comps[c].w);
      EXPECT_EQ(kImageHeight / 2, session->image->comps[c].h);
      EXPECT_EQ(1u, session->image->comps[c].factor);
      EXPECT_NE(nullptr, session->image->comps[c].data);
    }
  }
}

TEST(OpenjpegTest, MultiTileWriteReadAndGetDecodedTile) {
  constexpr uint32_t kNumTiles = 4;
  constexpr uint32_t kExpectedTileBytes = kTileWidth * kTileHeight * kNumComps;

  for (OPJ_CODEC_FORMAT format : kSupportedFormats) {
    SCOPED_TRACE(format);
    // 1. Encode 2x2 tiled image via opj_image_tile_create + opj_write_tile.
    const std::vector<uint8_t> encoded = EncodeMultiTileImage(format);
    ASSERT_FALSE(encoded.empty());

    // 2. Decode all tiles sequentially via opj_read_tile_header +
    // opj_decode_tile_data.
    {
      std::unique_ptr<DecodeSession> session = OpenForDecode(encoded, format);
      ASSERT_NE(nullptr, session);

      uint32_t decoded_count = 0;
      for (;;) {
        OPJ_UINT32 tile_index = 0;
        OPJ_UINT32 data_size = 0;
        OPJ_INT32 tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0;
        OPJ_UINT32 nb_comps = 0;
        OPJ_BOOL should_go_on = OPJ_FALSE;
        ASSERT_EQ(OPJ_TRUE, opj_read_tile_header(
                                session->codec.get(), session->stream.get(),
                                &tile_index, &data_size, &tx0, &ty0, &tx1, &ty1,
                                &nb_comps, &should_go_on));
        if (should_go_on == OPJ_FALSE) {
          break;
        }
        EXPECT_EQ(decoded_count, tile_index);
        EXPECT_EQ(kExpectedTileBytes, data_size);
        EXPECT_EQ(kNumComps, nb_comps);
        const OPJ_INT32 expected_x0 =
            static_cast<OPJ_INT32>((tile_index % 2) * kTileWidth);
        const OPJ_INT32 expected_y0 =
            static_cast<OPJ_INT32>((tile_index / 2) * kTileHeight);
        EXPECT_EQ(expected_x0, tx0);
        EXPECT_EQ(expected_y0, ty0);
        EXPECT_EQ(expected_x0 + static_cast<OPJ_INT32>(kTileWidth), tx1);
        EXPECT_EQ(expected_y0 + static_cast<OPJ_INT32>(kTileHeight), ty1);

        std::vector<OPJ_BYTE> actual_tile(data_size, 0xA5);
        ASSERT_EQ(OPJ_TRUE,
                  opj_decode_tile_data(session->codec.get(), tile_index,
                                       actual_tile.data(), data_size,
                                       session->stream.get()));
        EXPECT_THAT(actual_tile,
                    ElementsAreArray(MakeTilePayload(tile_index, kTileWidth,
                                                     kTileHeight, kNumComps)));
        ++decoded_count;
      }
      EXPECT_EQ(kNumTiles, decoded_count);
      EXPECT_EQ(OPJ_TRUE, opj_end_decompress(session->codec.get(),
                                             session->stream.get()));
    }

    // 3. Random-access decode of tile 2 via opj_get_decoded_tile.
    {
      std::unique_ptr<DecodeSession> session = OpenForDecode(encoded, format);
      ASSERT_NE(nullptr, session);

      constexpr OPJ_UINT32 kTargetTile = 2;
      ASSERT_EQ(OPJ_TRUE, opj_get_decoded_tile(
                              session->codec.get(), session->stream.get(),
                              session->image.get(), kTargetTile));
      EXPECT_EQ(0u, session->image->x0);
      EXPECT_EQ(kTileHeight, session->image->y0);
      EXPECT_EQ(kTileWidth, session->image->x1);
      EXPECT_EQ(kTileHeight * 2, session->image->y1);

      const std::vector<OPJ_BYTE> expected_tile =
          MakeTilePayload(kTargetTile, kTileWidth, kTileHeight, kNumComps);
      const size_t samples_per_comp =
          static_cast<size_t>(kTileWidth) * kTileHeight;
      for (OPJ_UINT32 c = 0; c < kNumComps; ++c) {
        ASSERT_EQ(kTileWidth, session->image->comps[c].w);
        ASSERT_EQ(kTileHeight, session->image->comps[c].h);
        ASSERT_NE(nullptr, session->image->comps[c].data);
        for (size_t i = 0; i < samples_per_comp; ++i) {
          EXPECT_EQ(
              static_cast<OPJ_INT32>(expected_tile[c * samples_per_comp + i]),
              session->image->comps[c].data[i]);
        }
      }

      // Out-of-bounds tile index fails cleanly.
      EXPECT_EQ(OPJ_FALSE, opj_get_decoded_tile(
                               session->codec.get(), session->stream.get(),
                               session->image.get(), kNumTiles));
    }
  }
}

TEST(OpenjpegTest, SetMctSetsParametersAndEncodesDecodes) {
  // 1. Zero components triggers opj_malloc(0) == nullptr -> OPJ_FALSE.
  opj_cparameters_t zero_params;
  opj_set_default_encoder_parameters(&zero_params);
  EXPECT_EQ(OPJ_FALSE, opj_set_MCT(&zero_params, nullptr, nullptr, 0));
  EXPECT_EQ(nullptr, zero_params.mct_data);

  // 2. Non-Part2 rsiz gets OPJ_PROFILE_PART2 | OPJ_EXTENSION_MCT.
  std::array<OPJ_FLOAT32, 9> matrix = {
      1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
  };
  std::array<OPJ_INT32, 3> dc_shift = {128, 128, 128};

  opj_cparameters_t part2_params;
  opj_set_default_encoder_parameters(&part2_params);
  part2_params.rsiz = OPJ_PROFILE_PART2;
  absl::Cleanup free_part2_mct = [&part2_params] {
    std::free(part2_params.mct_data);
    part2_params.mct_data = nullptr;
  };
  ASSERT_EQ(OPJ_TRUE, opj_set_MCT(&part2_params, matrix.data(), dc_shift.data(),
                                  kNumComps));
  EXPECT_EQ(OPJ_PROFILE_PART2 | OPJ_EXTENSION_MCT, part2_params.rsiz);
  EXPECT_EQ(1, part2_params.irreversible);
  EXPECT_EQ(2, part2_params.tcp_mct);
  EXPECT_NE(nullptr, part2_params.mct_data);

  // 3. Round-trip J2K encode/decode starting from OPJ_PROFILE_NONE.
  OpjImagePtr src = CreateTestImage(kImageWidth, kImageHeight, kNumComps);
  ASSERT_NE(nullptr, src);
  const std::vector<std::vector<OPJ_INT32>> expected =
      SnapshotImagePixels(src.get());

  opj_cparameters_t cparams = MakeOpenjpegLosslessParams();
  cparams.rsiz = OPJ_PROFILE_NONE;
  absl::Cleanup free_mct = [&cparams] {
    std::free(cparams.mct_data);
    cparams.mct_data = nullptr;
  };
  ASSERT_EQ(OPJ_TRUE,
            opj_set_MCT(&cparams, matrix.data(), dc_shift.data(), kNumComps));
  EXPECT_EQ(OPJ_PROFILE_PART2 | OPJ_EXTENSION_MCT, cparams.rsiz);

  std::optional<std::vector<uint8_t>> encoded =
      CompressImageToBytes(src.get(), OPJ_CODEC_J2K, &cparams);
  ASSERT_TRUE(encoded.has_value());
  EXPECT_FALSE(encoded->empty());
  // opj_j2k_setup_encoder frees and clears parameters->mct_data.
  EXPECT_EQ(nullptr, cparams.mct_data);

  // Upstream opj_j2k_write_cod (j2k.c) emits SGcod(C)=2 for Part-2 MCT, while
  // opj_j2k_read_cod rejects l_tcp->mct > 1 on the unmodified codestream.
  {
    MemReadStream raw_mem(*encoded);
    OpjStreamPtr raw_stream = raw_mem.CreateStream();
    ASSERT_NE(nullptr, raw_stream);
    OpjCodecPtr raw_codec(opj_create_decompress(OPJ_CODEC_J2K));
    ASSERT_NE(nullptr, raw_codec);
    EventCapture raw_events;
    raw_events.AttachTo(raw_codec.get());
    opj_dparameters_t dparams;
    opj_set_default_decoder_parameters(&dparams);
    ASSERT_EQ(OPJ_TRUE, opj_setup_decoder(raw_codec.get(), &dparams));
    opj_image_t* raw_image = nullptr;
    EXPECT_EQ(OPJ_FALSE,
              opj_read_header(raw_stream.get(), raw_codec.get(), &raw_image));
    opj_image_destroy(raw_image);
    EXPECT_THAT(
        raw_events.errors,
        Contains(HasSubstr("Invalid multiple component transformation")));
  }

  // Patch the COD marker's SGcod(C) byte (offset + 8 from 0xFF 0x52:
  // marker 2B + Lcod 2B + Scod 1B + prog 1B + layers 2B) from 2 to 0 so
  // opj_j2k_read_cod proceeds to read the Part-2 MCT/MCC/MCO markers.
  constexpr uint16_t kMarkerCod = 0xFF52;
  constexpr uint16_t kMarkerSot = 0xFF90;
  std::vector<uint8_t> patched = *encoded;
  size_t cod_sgcod_c_offset = 0;
  for (size_t pos = 2; pos + 4 <= patched.size();) {
    if (patched[pos] != 0xFF) {
      break;
    }
    const uint16_t marker = ReadBe16(patched, pos);
    if (marker == kMarkerSot) {
      break;
    }
    const uint16_t len = ReadBe16(patched, pos + 2);
    if (marker == kMarkerCod && pos + 8 < patched.size()) {
      cod_sgcod_c_offset = pos + 8;
      break;
    }
    pos += 2 + static_cast<size_t>(len);
  }
  ASSERT_NE(0u, cod_sgcod_c_offset);
  ASSERT_EQ(2u, patched[cod_sgcod_c_offset]);
  patched[cod_sgcod_c_offset] = 0u;

  std::unique_ptr<DecodeSession> session =
      OpenForDecode(patched, OPJ_CODEC_J2K);
  ASSERT_NE(nullptr, session);
  ScopedCstrInfo cstr_info(opj_get_cstr_info(session->codec.get()));
  ASSERT_TRUE(cstr_info);
  EXPECT_EQ(0u, cstr_info->m_default_tile_info.mct);
  ASSERT_NE(nullptr, cstr_info->m_default_tile_info.tccp_info);
  EXPECT_EQ(0u, cstr_info->m_default_tile_info.tccp_info[0].qmfbid);

  ASSERT_EQ(OPJ_TRUE, opj_decode(session->codec.get(), session->stream.get(),
                                 session->image.get()));
  ASSERT_EQ(OPJ_TRUE,
            opj_end_decompress(session->codec.get(), session->stream.get()));

  // Empirical max abs error across x86 SSE2 and --cpu=arm scalar paths for
  // 9-7 irreversible DWT + MCT on 8-bit synthetic samples is <= 3.
  constexpr int32_t kIrreversibleMctTolerance = 3;
  ASSERT_EQ(kNumComps, session->image->numcomps);
  for (OPJ_UINT32 c = 0; c < kNumComps; ++c) {
    const size_t count = static_cast<size_t>(kImageWidth) * kImageHeight;
    ASSERT_NE(nullptr, session->image->comps[c].data);
    for (size_t i = 0; i < count; ++i) {
      EXPECT_LE(std::abs(expected[c][i] - session->image->comps[c].data[i]),
                kIrreversibleMctTolerance);
    }
  }
}

TEST(OpenjpegTest, DumpCodecAndCodestreamInfoAndIndex) {
  for (OPJ_CODEC_FORMAT format : kSupportedFormats) {
    SCOPED_TRACE(format);
    const std::vector<uint8_t> encoded = EncodeMultiTileImage(format);
    ASSERT_FALSE(encoded.empty());

    std::unique_ptr<DecodeSession> session = OpenForDecode(encoded, format);
    ASSERT_NE(nullptr, session);

    // 1. Inspect codestream info after header read.
    ScopedCstrInfo info(opj_get_cstr_info(session->codec.get()));
    ASSERT_TRUE(info);
    EXPECT_EQ(kNumComps, info->nbcomps);
    EXPECT_EQ(2u, info->tw);
    EXPECT_EQ(2u, info->th);
    EXPECT_EQ(kTileWidth, info->tdx);
    EXPECT_EQ(kTileHeight, info->tdy);
    ASSERT_NE(nullptr, info->m_default_tile_info.tccp_info);
    EXPECT_EQ(kNumResolutions,
              info->m_default_tile_info.tccp_info[0].numresolutions);

    // 2. Decode so per-tile marker/tile-part index entries are populated.
    ASSERT_EQ(OPJ_TRUE, opj_decode(session->codec.get(), session->stream.get(),
                                   session->image.get()));
    ASSERT_EQ(OPJ_TRUE,
              opj_end_decompress(session->codec.get(), session->stream.get()));

    ScopedCstrIndex index(opj_get_cstr_index(session->codec.get()));
    ASSERT_TRUE(index);
    EXPECT_GT(index->main_head_end, index->main_head_start);
    EXPECT_GT(index->marknum, 0u);
    EXPECT_EQ(4u, index->nb_of_tiles);

    // 3. Dump codec metadata and index into a per-test temp file.
    const std::string dump_name = (format == OPJ_CODEC_J2K)
                                      ? "openjpeg_dump_j2k.txt"
                                      : "openjpeg_dump_jp2.txt";
    TempFile dump_file(dump_name);
    {
      ScopedFile out{std::fopen(dump_file.path_str().c_str(), "w")};
      ASSERT_NE(nullptr, out.fp);
      opj_dump_codec(
          session->codec.get(),
          OPJ_IMG_INFO | OPJ_J2K_MH_INFO | OPJ_J2K_TCH_INFO | OPJ_J2K_MH_IND,
          out.fp);
    }
    std::ifstream ifs(dump_file.path());
    ASSERT_TRUE(ifs.is_open());
    const std::string dump_text((std::istreambuf_iterator<char>(ifs)),
                                std::istreambuf_iterator<char>());
    EXPECT_THAT(dump_text, HasSubstr("Image info"));
    EXPECT_THAT(dump_text, HasSubstr("Codestream info"));
    EXPECT_THAT(dump_text, HasSubstr("Codestream index"));
  }
}

TEST(OpenjpegTest, FileStreamWriteReadTruncatedAndErrors) {
  // 1. Invalid filenames, missing files, and 0-byte stream buffer errors.
  EXPECT_EQ(nullptr,
            opj_stream_create_file_stream(nullptr, 1024, OPJ_STREAM_READ));
  EXPECT_EQ(nullptr,
            opj_stream_create_default_file_stream(nullptr, OPJ_STREAM_WRITE));

  TempFile missing_file("openjpeg_missing_file.jp2");
  EXPECT_EQ(nullptr,
            opj_stream_create_file_stream(missing_file.path_str().c_str(), 1024,
                                          OPJ_STREAM_READ));
  EXPECT_EQ(nullptr, opj_stream_create_default_file_stream(
                         missing_file.path_str().c_str(), OPJ_STREAM_READ));

  TempDir missing_parent_root("openjpeg_missing_parent");
  const std::filesystem::path bad_write_path =
      missing_parent_root.path() / "nonexistent_subdir" / "out.jp2";
  EXPECT_EQ(nullptr,
            opj_stream_create_file_stream(bad_write_path.string().c_str(), 1024,
                                          OPJ_STREAM_WRITE));

  // Existing file with p_size == 0 triggers opj_stream_create failure + fclose.
  TempFile zero_buf_file("openjpeg_zero_buf.jp2");
  {
    ScopedFile f{std::fopen(zero_buf_file.path_str().c_str(), "wb")};
    ASSERT_NE(nullptr, f.fp);
  }
  EXPECT_EQ(nullptr, opj_stream_create_file_stream(
                         zero_buf_file.path_str().c_str(), 0, OPJ_STREAM_READ));

  // 2. Empty file read exercises opj_read_from_file EOF sentinel (size_t -1).
  {
    OpjStreamPtr empty_stream(opj_stream_create_default_file_stream(
        zero_buf_file.path_str().c_str(), OPJ_STREAM_READ));
    ASSERT_NE(nullptr, empty_stream);
    OpjCodecPtr decomp(opj_create_decompress(OPJ_CODEC_J2K));
    ASSERT_NE(nullptr, decomp);
    opj_dparameters_t dparams;
    opj_set_default_decoder_parameters(&dparams);
    ASSERT_EQ(OPJ_TRUE, opj_setup_decoder(decomp.get(), &dparams));
    opj_image_t* raw_image = nullptr;
    EXPECT_EQ(OPJ_FALSE,
              opj_read_header(empty_stream.get(), decomp.get(), &raw_image));
    OpjImagePtr cleanup_image(raw_image);
  }

  // 3. Corrupt short file triggers read + error handling.
  TempFile corrupt_file("openjpeg_corrupt.jp2");
  {
    ScopedFile f{std::fopen(corrupt_file.path_str().c_str(), "wb")};
    ASSERT_NE(nullptr, f.fp);
    constexpr std::array<uint8_t, 4> kGarbage = {0x12, 0x34, 0x56, 0x78};
    ASSERT_EQ(kGarbage.size(),
              std::fwrite(kGarbage.data(), 1, kGarbage.size(), f.fp));
  }
  {
    OpjStreamPtr corrupt_stream(opj_stream_create_default_file_stream(
        corrupt_file.path_str().c_str(), OPJ_STREAM_READ));
    ASSERT_NE(nullptr, corrupt_stream);
    OpjCodecPtr decomp(opj_create_decompress(OPJ_CODEC_JP2));
    ASSERT_NE(nullptr, decomp);
    EventCapture events;
    events.AttachTo(decomp.get());
    opj_dparameters_t dparams;
    opj_set_default_decoder_parameters(&dparams);
    ASSERT_EQ(OPJ_TRUE, opj_setup_decoder(decomp.get(), &dparams));
    opj_image_t* raw_image = nullptr;
    EXPECT_EQ(OPJ_FALSE,
              opj_read_header(corrupt_stream.get(), decomp.get(), &raw_image));
    OpjImagePtr cleanup_image(raw_image);
    EXPECT_THAT(events.errors, Not(IsEmpty()));
  }

  // 4. Write & read JP2 via small-chunk opj_stream_create_file_stream
  // (exercises write/skip/seek/close/read/data_length callbacks).
  ASSERT_NO_FATAL_FAILURE(VerifyFileStreamRoundTrip(
      OPJ_CODEC_JP2, "openjpeg_file_stream_jp2.jp2", /*chunk_size=*/64));

  // 5. Write & read J2K via opj_stream_create_default_file_stream.
  ASSERT_NO_FATAL_FAILURE(VerifyFileStreamRoundTrip(
      OPJ_CODEC_J2K, "openjpeg_default_file_stream.j2k", std::nullopt));
}

TEST(OpenjpegTest, ImageDataAllocAndFreeAndTileCreateIntegration) {
  opj_image_data_free(nullptr);
  EXPECT_EQ(nullptr, opj_image_data_alloc(0));

  void* raw_buf = opj_image_data_alloc(256);
  ASSERT_NE(nullptr, raw_buf);
  EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(raw_buf) % 16u);
  std::memset(raw_buf, 0x5A, 256);
  opj_image_data_free(raw_buf);

  // Verify opj_image_tile_create buffers allocated via opj_image_data_alloc
  // are cleanly freed by opj_image_destroy.
  opj_image_cmptparm_t param{};
  param.dx = 1;
  param.dy = 1;
  param.w = 8;
  param.h = 8;
  param.prec = 8;
  param.sgnd = 0;
  OpjImagePtr image(opj_image_tile_create(1, &param, OPJ_CLRSPC_GRAY));
  ASSERT_NE(nullptr, image);
  ASSERT_NE(nullptr, image->comps);
  EXPECT_EQ(nullptr, image->comps[0].data);

  auto* samples =
      static_cast<OPJ_INT32*>(opj_image_data_alloc(64 * sizeof(OPJ_INT32)));
  ASSERT_NE(nullptr, samples);
  std::fill_n(samples, 64, 42);
  image->comps[0].data = samples;
  EXPECT_EQ(42, image->comps[0].data[63]);
}

// TODO(schwehr): Cover remaining defensive and allocation failure branches
// in openjpeg.c.

}  // namespace
}  // namespace openjpeg2
