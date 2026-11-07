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
// Tests for Zarr V3 codecs (zarr_v3_codec.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/zarr/zarr_v3_codec.cpp

#include <bit>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/frmts/zarr/zarr_test_util.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/zarr/zarr.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_json.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

TEST(ZarrV3CodecTest, GZipCodec) {
  ZarrV3CodecGZip codec;
  EXPECT_EQ("gzip", codec.GetName());
  EXPECT_EQ(ZarrV3Codec::IOType::BYTES, codec.GetInputType());
  EXPECT_EQ(ZarrV3Codec::IOType::BYTES, codec.GetOutputType());

  ZarrArrayMetadata in_meta;
  in_meta.anBlockSizes = {4, 4};
  in_meta.oElt.nativeSize = 4;
  ZarrArrayMetadata out_meta;

  // 1. GetConfiguration and valid InitFromConfiguration
  auto cfg = ZarrV3CodecGZip::GetConfiguration(6);
  EXPECT_TRUE(codec.InitFromConfiguration(cfg, in_meta, out_meta));
  EXPECT_EQ(in_meta.anBlockSizes, out_meta.anBlockSizes);

  // 2. Clone
  auto clone = codec.Clone();
  ASSERT_NE(nullptr, clone);

  // 3. Error branches in InitFromConfiguration
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    CPLJSONDocument doc;
    doc.LoadMemory(R"json({"level": "bad"})json");
    EXPECT_FALSE(codec.InitFromConfiguration(doc.GetRoot(), in_meta, out_meta));

    doc.LoadMemory(R"json({"level": 15})json");
    EXPECT_FALSE(codec.InitFromConfiguration(doc.GetRoot(), in_meta, out_meta));

    doc.LoadMemory(R"json({"unknown": 1})json");
    EXPECT_FALSE(codec.InitFromConfiguration(doc.GetRoot(), in_meta, out_meta));
  }

  // 4. Encode & Decode roundtrip
  ZarrByteVectorQuickResize src;
  src.resize(64);
  for (size_t i = 0; i < 64; ++i) {
    src[i] = static_cast<GByte>(i % 7);
  }

  ZarrByteVectorQuickResize encoded;
  encoded.resize(256);  // allocate capacity
  EXPECT_TRUE(codec.Encode(src, encoded));
  EXPECT_GT(encoded.size(), 0);

  ZarrByteVectorQuickResize decoded;
  decoded.resize(256);  // allocate capacity
  EXPECT_TRUE(codec.Decode(encoded, decoded));
  EXPECT_EQ(src.size(), decoded.size());
  EXPECT_EQ(0, std::memcmp(src.data(), decoded.data(), src.size()));

  // TODO(b/335317901): Upstream leak in CPLGZipCompress
  // (port/cpl_compressor.cpp:872) omits deflateEnd on failure; enable testing
  // the small-buffer error path on ZarrV3CodecGZip once fixed (output buffer
  // too small is tested under BloscCodec below).
}

TEST(ZarrV3CodecTest, ZstdCodec) {
  ZarrV3CodecZstd codec;
  EXPECT_EQ("zstd", codec.GetName());

  ZarrArrayMetadata in_meta;
  in_meta.anBlockSizes = {8};
  in_meta.oElt.nativeSize = 4;
  ZarrArrayMetadata out_meta;

  // 1. GetConfiguration & Init
  auto cfg = ZarrV3CodecZstd::GetConfiguration(3, true);
  EXPECT_TRUE(codec.InitFromConfiguration(cfg, in_meta, out_meta));

  // 2. Clone
  auto clone = codec.Clone();
  ASSERT_NE(nullptr, clone);

  // 3. Error branches
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    CPLJSONDocument doc;
    doc.LoadMemory(R"json({"level": "not_int"})json");
    EXPECT_FALSE(codec.InitFromConfiguration(doc.GetRoot(), in_meta, out_meta));

    doc.LoadMemory(R"json({"level": 30})json");
    EXPECT_FALSE(codec.InitFromConfiguration(doc.GetRoot(), in_meta, out_meta));

    doc.LoadMemory(R"json({"checksum": "not_bool"})json");
    EXPECT_FALSE(codec.InitFromConfiguration(doc.GetRoot(), in_meta, out_meta));

    doc.LoadMemory(R"json({"unhandled": true})json");
    EXPECT_FALSE(codec.InitFromConfiguration(doc.GetRoot(), in_meta, out_meta));
  }

  // 4. Encode / Decode roundtrip
  ZarrByteVectorQuickResize src;
  src.resize(32);
  for (size_t i = 0; i < 32; ++i) {
    src[i] = static_cast<GByte>(i * 3);
  }

  ZarrByteVectorQuickResize encoded;
  encoded.resize(128);
  EXPECT_TRUE(codec.Encode(src, encoded));

  ZarrByteVectorQuickResize decoded;
  decoded.resize(128);
  EXPECT_TRUE(codec.Decode(encoded, decoded));
  EXPECT_EQ(src.size(), decoded.size());
  EXPECT_EQ(0, std::memcmp(src.data(), decoded.data(), src.size()));
}

TEST(ZarrV3CodecTest, BloscCodec) {
  ZarrV3CodecBlosc codec;
  EXPECT_EQ("blosc", codec.GetName());

  ZarrArrayMetadata in_meta;
  in_meta.anBlockSizes = {16};
  in_meta.oElt.nativeSize = 4;
  ZarrArrayMetadata out_meta;

  // 1. GetConfiguration & Init
  auto cfg = ZarrV3CodecBlosc::GetConfiguration("zstd", 5, "shuffle", 4, 0);
  EXPECT_TRUE(codec.InitFromConfiguration(cfg, in_meta, out_meta));

  // 2. Clone
  auto clone = codec.Clone();
  ASSERT_NE(nullptr, clone);

  // 3. Different shuffle modes
  for (const char* shuffle : {"noshuffle", "bitshuffle"}) {
    auto c = ZarrV3CodecBlosc::GetConfiguration("zlib", 3, shuffle, 4, 0);
    EXPECT_TRUE(codec.InitFromConfiguration(c, in_meta, out_meta));
  }

  // 4. Error branches
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    CPLJSONDocument doc;
    doc.LoadMemory(R"json({"cname": 123})json");
    EXPECT_FALSE(codec.InitFromConfiguration(doc.GetRoot(), in_meta, out_meta));

    doc.LoadMemory(R"json({"cname": "zlib", "clevel": "bad"})json");
    EXPECT_FALSE(codec.InitFromConfiguration(doc.GetRoot(), in_meta, out_meta));

    doc.LoadMemory(R"json({"cname": "zlib", "clevel": 20})json");
    EXPECT_FALSE(codec.InitFromConfiguration(doc.GetRoot(), in_meta, out_meta));

    doc.LoadMemory(R"json({"cname": "zlib", "shuffle": "invalid_mode"})json");
    EXPECT_FALSE(codec.InitFromConfiguration(doc.GetRoot(), in_meta, out_meta));

    doc.LoadMemory(
        R"json({"cname": "zlib", "shuffle": "shuffle", "typesize": "bad"})json");
    EXPECT_FALSE(codec.InitFromConfiguration(doc.GetRoot(), in_meta, out_meta));

    doc.LoadMemory(
        R"json({"cname": "zlib", "shuffle": "shuffle", "blocksize": "bad"})json");
    EXPECT_FALSE(codec.InitFromConfiguration(doc.GetRoot(), in_meta, out_meta));

    doc.LoadMemory(
        R"json({"cname": "zlib", "shuffle": "shuffle", "unknown": 1})json");
    EXPECT_FALSE(codec.InitFromConfiguration(doc.GetRoot(), in_meta, out_meta));
  }

  // 5. Encode / Decode roundtrip
  ZarrByteVectorQuickResize src;
  src.resize(64);
  for (size_t i = 0; i < 64; ++i) {
    src[i] = static_cast<GByte>(i);
  }

  ZarrByteVectorQuickResize encoded;
  encoded.resize(256);
  EXPECT_TRUE(codec.Encode(src, encoded));

  ZarrByteVectorQuickResize decoded;
  decoded.resize(256);
  EXPECT_TRUE(codec.Decode(encoded, decoded));
  EXPECT_EQ(src.size(), decoded.size());
  EXPECT_EQ(0, std::memcmp(src.data(), decoded.data(), src.size()));

  // 6. Output buffer too small error
  // (exercises zarr_v3_codec.cpp:61 and 88 cleanly without leaking,
  // as Blosc decompressor cleanly returns output size).
  ZarrByteVectorQuickResize tiny_dst;
  tiny_dst.resize(1);
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(codec.Encode(src, tiny_dst));
    EXPECT_FALSE(codec.Decode(encoded, tiny_dst));
  }
}

TEST(ZarrV3CodecTest, BytesCodec) {
  ZarrV3CodecBytes codec;
  EXPECT_EQ("bytes", codec.GetName());
  EXPECT_EQ(ZarrV3Codec::IOType::ARRAY, codec.GetInputType());
  EXPECT_EQ(ZarrV3Codec::IOType::BYTES, codec.GetOutputType());

  ZarrArrayMetadata in_meta;
  in_meta.anBlockSizes = {4};
  in_meta.oElt.nativeSize = 4;
  ZarrArrayMetadata out_meta;

  // 1. GetConfiguration & Init
  auto cfg_little = ZarrV3CodecBytes::GetConfiguration(true);
  EXPECT_TRUE(codec.InitFromConfiguration(cfg_little, in_meta, out_meta));
  if constexpr (std::endian::native == std::endian::little) {
    EXPECT_TRUE(codec.IsNoOp());
  }

  auto cfg_big = ZarrV3CodecBytes::GetConfiguration(false);
  EXPECT_TRUE(codec.InitFromConfiguration(cfg_big, in_meta, out_meta));
  if constexpr (std::endian::native == std::endian::little) {
    EXPECT_FALSE(codec.IsNoOp());
  }

  auto clone = codec.Clone();
  ASSERT_NE(nullptr, clone);

  // 2. Error branches
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    CPLJSONDocument doc;
    doc.LoadMemory(R"json({"endian": "middle"})json");
    EXPECT_FALSE(codec.InitFromConfiguration(doc.GetRoot(), in_meta, out_meta));

    doc.LoadMemory(R"json({"endian": 123})json");
    EXPECT_FALSE(codec.InitFromConfiguration(doc.GetRoot(), in_meta, out_meta));

    doc.LoadMemory(R"json({"other": "key"})json");
    EXPECT_FALSE(codec.InitFromConfiguration(doc.GetRoot(), in_meta, out_meta));
  }

  // 3. Byte swapping Encode/Decode across sizes (2, 4, 8, and COMPLEX_IEEEFP)
  for (size_t native_sz : {size_t{2}, size_t{4}, size_t{8}}) {
    in_meta.anBlockSizes = {2};
    in_meta.oElt.nativeSize = native_sz;
    in_meta.oElt.nativeType = DtypeElt::NativeType::SIGNED_INT;
    ASSERT_TRUE(codec.InitFromConfiguration(cfg_big, in_meta, out_meta));

    ZarrByteVectorQuickResize src;
    src.resize(2 * native_sz);
    for (size_t i = 0; i < src.size(); ++i) {
      src[i] = static_cast<GByte>(i + 1);
    }

    ZarrByteVectorQuickResize encoded;
    EXPECT_TRUE(codec.Encode(src, encoded));
    EXPECT_EQ(src.size(), encoded.size());

    ZarrByteVectorQuickResize decoded;
    EXPECT_TRUE(codec.Decode(encoded, decoded));
    EXPECT_EQ(0, std::memcmp(src.data(), decoded.data(), src.size()));
  }

  // COMPLEX_IEEEFP (e.g. complex64: nativeSize = 8, 2 elements of 4 bytes)
  {
    in_meta.anBlockSizes = {1};
    in_meta.oElt.nativeSize = 8;
    in_meta.oElt.nativeType = DtypeElt::NativeType::COMPLEX_IEEEFP;
    ASSERT_TRUE(codec.InitFromConfiguration(cfg_big, in_meta, out_meta));

    ZarrByteVectorQuickResize src;
    src.resize(8);
    for (size_t i = 0; i < 8; ++i) {
      src[i] = static_cast<GByte>(i);
    }

    ZarrByteVectorQuickResize encoded;
    EXPECT_TRUE(codec.Encode(src, encoded));
    ZarrByteVectorQuickResize decoded;
    EXPECT_TRUE(codec.Decode(encoded, decoded));
    EXPECT_EQ(0, std::memcmp(src.data(), decoded.data(), src.size()));
  }

  // Input buffer too small
  {
    ZarrByteVectorQuickResize tiny_src;
    tiny_src.resize(1);
    ZarrByteVectorQuickResize dst;
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(codec.Encode(tiny_src, dst));
  }
}

TEST(ZarrV3CodecTest, TransposeCodec) {
  ZarrArrayMetadata in_meta;
  in_meta.anBlockSizes = {2, 3};
  in_meta.oElt.nativeSize = 4;
  ZarrArrayMetadata out_meta;

  // 1. Order = "C" (IsNoOp = true)
  {
    ZarrV3CodecTranspose codec;
    EXPECT_EQ("transpose", codec.GetName());
    EXPECT_EQ(ZarrV3Codec::IOType::ARRAY, codec.GetInputType());
    EXPECT_EQ(ZarrV3Codec::IOType::ARRAY, codec.GetOutputType());

    CPLJSONObject cfg_c;
    cfg_c.Add("order", "C");
    EXPECT_TRUE(codec.InitFromConfiguration(cfg_c, in_meta, out_meta));
    EXPECT_TRUE(codec.IsNoOp());
  }

  // 2. Order = "F" (IsNoOp = false)
  CPLJSONObject cfg_f;
  cfg_f.Add("order", "F");
  {
    ZarrV3CodecTranspose codec;
    EXPECT_TRUE(codec.InitFromConfiguration(cfg_f, in_meta, out_meta));
    EXPECT_FALSE(codec.IsNoOp());
    EXPECT_EQ((std::vector<size_t>{3, 2}), out_meta.anBlockSizes);

    auto clone = codec.Clone();
    ASSERT_NE(nullptr, clone);
  }

  // 3. Order as array [1, 0]
  {
    ZarrV3CodecTranspose codec;
    auto cfg_arr =
        ZarrV3CodecTranspose::GetConfiguration(std::vector<int>{1, 0});
    EXPECT_TRUE(codec.InitFromConfiguration(cfg_arr, in_meta, out_meta));
    EXPECT_FALSE(codec.IsNoOp());
  }

  // 4. Error branches in InitFromConfiguration
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    auto ExpectBadConfig = [&](std::string_view json_str) {
      CPLJSONDocument doc;
      doc.LoadMemory(std::string(json_str));
      ZarrV3CodecTranspose bad_codec;
      EXPECT_FALSE(
          bad_codec.InitFromConfiguration(doc.GetRoot(), in_meta, out_meta));
    };

    ExpectBadConfig(R"json({"order": "invalid"})json");
    ExpectBadConfig(R"json({"order": [0]})json");
    ExpectBadConfig(R"json({"order": [0, 0]})json");
    ExpectBadConfig(R"json({"order": [0, 2]})json");
    ExpectBadConfig(R"json({"order": 123})json");
    ExpectBadConfig(R"json({"unhandled": "key"})json");
  }

  // 5. Encode / Decode roundtrip for element sizes 1, 2, 4, 8, 12 (12 triggers
  // memcpy branch)
  for (size_t elt_sz :
       {size_t{1}, size_t{2}, size_t{4}, size_t{8}, size_t{12}}) {
    ZarrV3CodecTranspose codec;
    in_meta.anBlockSizes = {2, 3};
    in_meta.oElt.nativeSize = elt_sz;
    ASSERT_TRUE(codec.InitFromConfiguration(cfg_f, in_meta, out_meta));

    ZarrByteVectorQuickResize src;
    src.resize(2 * 3 * elt_sz);
    for (size_t i = 0; i < src.size(); ++i) {
      src[i] = static_cast<GByte>((i * 7 + 1) & 0xFF);
    }

    ZarrByteVectorQuickResize encoded;
    EXPECT_TRUE(codec.Encode(src, encoded));
    EXPECT_EQ(src.size(), encoded.size());

    ZarrByteVectorQuickResize decoded;
    EXPECT_TRUE(codec.Decode(encoded, decoded));
    EXPECT_EQ(src.size(), decoded.size());
    EXPECT_EQ(0, std::memcmp(src.data(), decoded.data(), src.size()));
  }

  // 6. Small input buffer error
  {
    ZarrV3CodecTranspose codec;
    ASSERT_TRUE(codec.InitFromConfiguration(cfg_f, in_meta, out_meta));
    ZarrByteVectorQuickResize tiny_src;
    tiny_src.resize(2);
    ZarrByteVectorQuickResize dst;
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(codec.Encode(tiny_src, dst));
  }
}

TEST(ZarrV3CodecTest, CodecSequence) {
  ZarrArrayMetadata in_meta;
  in_meta.anBlockSizes = {2, 2};
  in_meta.oElt.nativeSize = 4;

  // 1. Error branches in InitFromJson
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    CPLJSONDocument doc1;
    doc1.LoadMemory(R"json("not_array")json");
    ZarrV3CodecSequence bad_seq1(in_meta);
    EXPECT_FALSE(bad_seq1.InitFromJson(doc1.GetRoot()));

    CPLJSONDocument doc2;
    doc2.LoadMemory(R"json([123])json");
    ZarrV3CodecSequence bad_seq2(in_meta);
    EXPECT_FALSE(bad_seq2.InitFromJson(doc2.GetRoot()));

    CPLJSONDocument doc3;
    doc3.LoadMemory(R"json([{"name": "unsupported_codec"}])json");
    ZarrV3CodecSequence bad_seq3(in_meta);
    EXPECT_FALSE(bad_seq3.InitFromJson(doc3.GetRoot()));

    // Invalid chaining: ARRAY codec after BYTES codec
    CPLJSONDocument doc4;
    doc4.LoadMemory(R"json([
      {"name": "bytes", "configuration": {"endian": "little"}},
      {"name": "transpose", "configuration": {"order": "F"}}
    ])json");
    ZarrV3CodecSequence bad_seq4(in_meta);
    EXPECT_FALSE(bad_seq4.InitFromJson(doc4.GetRoot()));
  }

  // 2. Multi-step sequence: transpose -> bytes -> zstd
  ZarrV3CodecSequence seq(in_meta);
  CPLJSONDocument valid_doc;
  ASSERT_TRUE(valid_doc.LoadMemory(R"json([
    {"name": "transpose", "configuration": {"order": "F"}},
    {"name": "bytes", "configuration": {"endian": "big"}},
    {"name": "zstd", "configuration": {"level": 3, "checksum": true}}
  ])json"));
  EXPECT_TRUE(seq.InitFromJson(valid_doc.GetRoot()));
  EXPECT_EQ(3, seq.GetJSon().ToArray().Size());

  auto clone = seq.Clone();
  ASSERT_NE(nullptr, clone);

  // 3. Encode & Decode roundtrip
  ZarrByteVectorQuickResize buf;
  buf.resize(2 * 2 * 4);
  for (size_t i = 0; i < buf.size(); ++i) {
    buf[i] = static_cast<GByte>(i * 5);
  }
  std::vector<GByte> orig_data(buf.data(), buf.data() + buf.size());

  EXPECT_TRUE(seq.Encode(buf));
  EXPECT_TRUE(seq.Decode(buf));
  EXPECT_EQ(orig_data.size(), buf.size());
  EXPECT_EQ(0, std::memcmp(orig_data.data(), buf.data(), orig_data.size()));

  // 4. Decode corrupt buffer fails
  ZarrByteVectorQuickResize corrupt_buf;
  corrupt_buf.resize(10);
  std::memset(corrupt_buf.data(), 0xFF, 10);
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(seq.Decode(corrupt_buf));
  }
}

// TODO: b/335317901 - Test additional custom V3 codec plugins if enabled.

}  // namespace
}  // namespace autotest2
