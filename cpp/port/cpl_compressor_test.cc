// Test compression/decompression functions
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_compressor.cpp
//
// Copyright 2024 Google Inc. All Rights Reserved.
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

#include "third_party/gdal/port/cpl_compressor.h"

#include <features.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

bool TestCompressor(const void* input_data, size_t input_size,
                    void** output_data, size_t* output_size,
                    CSLConstList options, void* compressor_user_data) {
  return false;
}

int test_user_data = 0;

void BuildTestCompressor(CPLCompressor* compressor) {
  const char* const metadata[] = {"FOO=VAR", nullptr};

  compressor->nStructVersion = 1;
  compressor->eType = CCT_COMPRESSOR;
  compressor->pszId = "test_compressor";
  compressor->papszMetadata = metadata;
  compressor->pfnFunc = TestCompressor;
  compressor->user_data = &test_user_data;
}

TEST(CplCompressor, RegisteredCompressorCanBeFound) {
  CPLCompressor test_compressor;
  BuildTestCompressor(&test_compressor);

  ASSERT_TRUE(CPLRegisterCompressor(&test_compressor));
  absl::Cleanup registry_cleanup = [] { CPLDestroyCompressorRegistry(); };

  char** compressors = CPLGetCompressors();
  absl::Cleanup cleanup = [compressors] { CSLDestroy(compressors); };

  EXPECT_NE(compressors, nullptr);
  EXPECT_GE(CSLFindString(compressors, test_compressor.pszId), 0);

  const auto retrieved = CPLGetCompressor(test_compressor.pszId);
  ASSERT_NE(retrieved, nullptr);

  EXPECT_STREQ(retrieved->pszId, test_compressor.pszId);
  EXPECT_EQ(CSLCount(retrieved->papszMetadata),
            CSLCount(test_compressor.papszMetadata));
  EXPECT_EQ(retrieved->pfnFunc, TestCompressor);
  EXPECT_EQ(retrieved->user_data, test_compressor.user_data);
}

TEST(CplCompressor, ReturnsFalseWhenCompressorIsRegisteredAgain) {
  CPLCompressor test_compressor;
  BuildTestCompressor(&test_compressor);

  ASSERT_TRUE(CPLRegisterCompressor(&test_compressor));
  absl::Cleanup registry_cleanup = [] { CPLDestroyCompressorRegistry(); };

  {
    WithQuietHandler error_handler;
    EXPECT_FALSE(CPLRegisterCompressor(&test_compressor));
  }
}

TEST(CplCompressor, BuiltinCompressorsMetadataShouldBeValidXml) {
  char** compressors = CPLGetCompressors();
  absl::Cleanup cleanup = [compressors] { CSLDestroy(compressors); };

  // CPLGetCompressors() will set up builtin compressors
  absl::Cleanup registry_cleanup = [] { CPLDestroyCompressorRegistry(); };

  for (auto compressor_id = compressors; *compressor_id; ++compressor_id) {
    const auto compressor = CPLGetCompressor(*compressor_id);
    ASSERT_NE(compressor, nullptr);

    const char* options =
        CSLFetchNameValue(compressor->papszMetadata, "OPTIONS");
    if (options) {
      auto node = CPLParseXMLString(options);
      absl::Cleanup node_cleanup = [node] { CPLDestroyXMLNode(node); };

      EXPECT_TRUE(node);
    }
  }
}

TEST(CplCompressor, GettingNonExistentCompressorShouldReturnNullptr) {
  EXPECT_EQ(CPLGetCompressor("non_existent"), nullptr);

  // CPLGetCompressor() will set up builtin compressors
  CPLDestroyCompressorRegistry();
}

TEST(CplCompressor, RegisteredCompressorIsRemovedAfterRegistryIsDestroyed) {
  CPLCompressor test_compressor;
  BuildTestCompressor(&test_compressor);

  ASSERT_TRUE(CPLRegisterCompressor(&test_compressor));

  CPLDestroyCompressorRegistry();
  EXPECT_EQ(CPLGetCompressor(test_compressor.pszId), nullptr);
}

// Test decompressor functions - basically the same as above, but with the
// decompressor functions.

TEST(CplDecompressor, ExpectedCompressors) {
  char** decompressors = CPLGetDecompressors();
  absl::Cleanup cleanup = [decompressors] { CSLDestroy(decompressors); };

  // CPLGetDecompressors() will set up builtin compressors
  absl::Cleanup registry_cleanup = [] { CPLDestroyCompressorRegistry(); };

  EXPECT_EQ(CSLFindString(decompressors, "does not exist"), -1);

  EXPECT_GE(CSLFindString(decompressors, "blosc"), 0);
  EXPECT_GE(CSLFindString(decompressors, "gzip"), 0);
  EXPECT_GE(CSLFindString(decompressors, "lzma"), 0);
  EXPECT_GE(CSLFindString(decompressors, "lz4"), 0);
  EXPECT_GE(CSLFindString(decompressors, "zlib"), 0);
  EXPECT_GE(CSLFindString(decompressors, "zstd"), 0);
}

TEST(CplDecompressor, RegisteredDecompressorCanBeFound) {
  CPLCompressor test_decompressor;
  BuildTestCompressor(&test_decompressor);

  ASSERT_TRUE(CPLRegisterDecompressor(&test_decompressor));
  absl::Cleanup registry_cleanup = [] { CPLDestroyCompressorRegistry(); };

  char** decompressors = CPLGetDecompressors();
  absl::Cleanup cleanup = [decompressors] { CSLDestroy(decompressors); };

  EXPECT_NE(decompressors, nullptr);
  EXPECT_GE(CSLFindString(decompressors, test_decompressor.pszId), 0);

  const auto retrieved = CPLGetDecompressor(test_decompressor.pszId);
  ASSERT_NE(retrieved, nullptr);

  EXPECT_STREQ(retrieved->pszId, test_decompressor.pszId);
  EXPECT_EQ(CSLCount(retrieved->papszMetadata),
            CSLCount(test_decompressor.papszMetadata));
  EXPECT_EQ(retrieved->pfnFunc, TestCompressor);
  EXPECT_EQ(retrieved->user_data, test_decompressor.user_data);
}

TEST(CplDecompressor, ReturnsFalseWhenDecompressorIsRegisteredAgain) {
  CPLCompressor test_decompressor;
  BuildTestCompressor(&test_decompressor);

  ASSERT_TRUE(CPLRegisterDecompressor(&test_decompressor));
  absl::Cleanup registry_cleanup = [] { CPLDestroyCompressorRegistry(); };

  {
    WithQuietHandler error_handler;
    EXPECT_FALSE(CPLRegisterDecompressor(&test_decompressor));
  }
}

TEST(CplDecompressor, BuiltinDecompressorsMetadataShouldBeValidXml) {
  char** decompressors = CPLGetDecompressors();
  absl::Cleanup cleanup = [decompressors] { CSLDestroy(decompressors); };

  // CPLGetDecompressors() will set up builtin compressors
  absl::Cleanup registry_cleanup = [] { CPLDestroyCompressorRegistry(); };

  for (auto id = decompressors; *id; ++id) {
    const auto decompressor = CPLGetCompressor(*id);
    ASSERT_NE(decompressor, nullptr);

    const char* options =
        CSLFetchNameValue(decompressor->papszMetadata, "OPTIONS");
    if (options) {
      auto node = CPLParseXMLString(options);
      absl::Cleanup node_cleanup = [node] { CPLDestroyXMLNode(node); };

      EXPECT_TRUE(node);
    }
  }
}

TEST(CplDecompressor, GettingNonExistentDecompressorShouldReturnNullptr) {
  EXPECT_EQ(CPLGetDecompressor("non_existent"), nullptr);

  // CPLGetCompressor() will set up builtin compressors
  CPLDestroyCompressorRegistry();
}

TEST(CplDecompressor, RegisteredDecompressorIsRemovedAfterRegistryIsDestroyed) {
  CPLCompressor test_decompressor;
  BuildTestCompressor(&test_decompressor);

  ASSERT_TRUE(CPLRegisterDecompressor(&test_decompressor));

  CPLDestroyCompressorRegistry();
  EXPECT_EQ(CPLGetDecompressor(test_decompressor.pszId), nullptr);
}

const char test_compressor_input[] = "test input string";
const char test_decompressor_input[] =
    "\x11\x00\x00\x00\xf0\x02\x74\x65\x73\x74\x20\x69\x6e\x70\x75\x74\x20\x73"
    "\x74\x72\x69\x6e\x67";
const char* const test_compressor_options[] = {
    "TYPESIZE=1",
    nullptr,
};

TEST(Lz4Compressor, CompressorGetMinimumBufferSize) {
  const CPLCompressor* test_compressor = CPLGetCompressor("lz4");
  ASSERT_NE(test_compressor, nullptr);

  size_t required_buffer_size = 0;
  EXPECT_TRUE(test_compressor->pfnFunc(
      test_compressor_input, strlen(test_compressor_input), nullptr,
      &required_buffer_size, test_compressor_options,
      test_compressor->user_data));
  EXPECT_GT(required_buffer_size, 0);
}

TEST(Lz4Compressor, CompressorCanAllocateBuffer) {
  const CPLCompressor* test_compressor = CPLGetCompressor("lz4");
  ASSERT_NE(test_compressor, nullptr);

  size_t required_buffer_size = 0;
  ASSERT_TRUE(test_compressor->pfnFunc(
      test_compressor_input, strlen(test_compressor_input), nullptr,
      &required_buffer_size, test_compressor_options,
      test_compressor->user_data));
  ASSERT_GT(required_buffer_size, 0);

  void* output_buffer = nullptr;
  size_t buffer_size = 0;
  EXPECT_TRUE(test_compressor->pfnFunc(
      test_compressor_input, strlen(test_compressor_input), &output_buffer,
      &buffer_size, test_compressor_options, test_compressor->user_data));
  absl::Cleanup output_cleanup = [output_buffer] { VSIFree(output_buffer); };

  EXPECT_GT(buffer_size, 0);
  EXPECT_NE(output_buffer, nullptr);
  EXPECT_LT(buffer_size, required_buffer_size);
}

TEST(Lz4Compressor, CompressionWillFailIfBufferSizeTooSmall) {
  const CPLCompressor* test_compressor = CPLGetCompressor("lz4");
  ASSERT_NE(test_compressor, nullptr);

  size_t buffer_size = 1;
  char buffer[1] = {0};
  void* output_buffer = &buffer;
  EXPECT_FALSE(test_compressor->pfnFunc(
      test_compressor_input, strlen(test_compressor_input), &output_buffer,
      &buffer_size, test_compressor_options, test_compressor->user_data));
  EXPECT_EQ(buffer_size, 0);
  EXPECT_EQ(output_buffer, &buffer);
}

TEST(Lz4Compressor, CompressionShouldWork) {
  const CPLCompressor* test_compressor = CPLGetCompressor("lz4");
  ASSERT_NE(test_compressor, nullptr);

  size_t required_buffer_size = 0;
  ASSERT_TRUE(test_compressor->pfnFunc(
      test_compressor_input, strlen(test_compressor_input), nullptr,
      &required_buffer_size, test_compressor_options,
      test_compressor->user_data));
  ASSERT_GT(required_buffer_size, 0);

  std::vector<char> compression_output(required_buffer_size);
  size_t compressed_size = required_buffer_size;
  void* compression_output_buffer = compression_output.data();
  EXPECT_TRUE(test_compressor->pfnFunc(
      test_compressor_input, strlen(test_compressor_input),
      &compression_output_buffer, &compressed_size, test_compressor_options,
      test_compressor->user_data));
  EXPECT_GT(compressed_size, 0);
  EXPECT_EQ(compression_output_buffer, compression_output.data());

  compression_output.resize(compressed_size + 1);
  std::vector<char> expected_compression_output(
      std::begin(test_decompressor_input), std::end(test_decompressor_input));
  EXPECT_EQ(compression_output, expected_compression_output);
}

// Helper to test generic compressor and decompressor roundtrips, modes, and
// error handling.
void TestGenericCompressorRoundtrip(const char* id, CSLConstList comp_opts,
                                    CSLConstList decomp_opts) {
  WithQuietHandler quiet;
  const CPLCompressor* comp = CPLGetCompressor(id);
  const CPLCompressor* decomp = CPLGetDecompressor(id);
  ASSERT_NE(comp, nullptr) << id;
  ASSERT_NE(decomp, nullptr) << id;

  // Highly compressible payload (2000 bytes) to exercise reallocation loops
  // when decompressing with *output_data == nullptr.
  std::vector<char> input(2000, 'A');
  for (size_t i = 0; i < input.size(); i += 17) {
    input[i] = static_cast<char>(i & 0xFF);
  }

  // Invalid API calls.
  EXPECT_FALSE(comp->pfnFunc(input.data(), input.size(), nullptr, nullptr,
                             comp_opts, comp->user_data));

  // Query upper bound size.
  size_t bound_size = 0;
  EXPECT_TRUE(comp->pfnFunc(input.data(), input.size(), nullptr, &bound_size,
                            comp_opts, comp->user_data));
  EXPECT_GT(bound_size, 0U);

  // Compress with too-small buffer (skip zlib/gzip as CPLZLibDeflate and
  // CPLGZipCompress omit deflateEnd when preallocated output is too small).
  char small_buf[2] = {0};
  void* small_ptr = small_buf;
  size_t small_size = 1;
  if (strcmp(id, "zlib") != 0 && strcmp(id, "gzip") != 0) {
    EXPECT_FALSE(comp->pfnFunc(input.data(), input.size(), &small_ptr,
                               &small_size, comp_opts, comp->user_data));
  }

  // Compress with auto-allocated buffer.
  void* comp_auto = nullptr;
  size_t comp_auto_size = 0;
  ASSERT_TRUE(comp->pfnFunc(input.data(), input.size(), &comp_auto,
                            &comp_auto_size, comp_opts, comp->user_data));
  absl::Cleanup free_comp_auto = [comp_auto] { VSIFree(comp_auto); };
  ASSERT_NE(comp_auto, nullptr);
  ASSERT_GT(comp_auto_size, 0U);

  // Decompressor invalid API call with nullptr output_data and nullptr
  // output_size.
  EXPECT_FALSE(decomp->pfnFunc(comp_auto, comp_auto_size, nullptr, nullptr,
                               decomp_opts, decomp->user_data));

  // Compress with pre-allocated buffer.
  std::vector<char> comp_prealloc(bound_size);
  void* comp_pre_ptr = comp_prealloc.data();
  size_t comp_pre_size = bound_size;
  ASSERT_TRUE(comp->pfnFunc(input.data(), input.size(), &comp_pre_ptr,
                            &comp_pre_size, comp_opts, comp->user_data));
  EXPECT_EQ(comp_pre_size, comp_auto_size);

  // Decompress size query.
  size_t decomp_bound = 0;
  EXPECT_TRUE(decomp->pfnFunc(comp_auto, comp_auto_size, nullptr, &decomp_bound,
                              decomp_opts, decomp->user_data));
  EXPECT_EQ(decomp_bound, input.size());

  // Decompress with too-small buffer.
  small_ptr = small_buf;
  small_size = 1;
  EXPECT_FALSE(decomp->pfnFunc(comp_auto, comp_auto_size, &small_ptr,
                               &small_size, decomp_opts, decomp->user_data));

  // Decompress with auto-allocated buffer.
  void* decomp_auto = nullptr;
  size_t decomp_auto_size = 0;
  ASSERT_TRUE(decomp->pfnFunc(comp_auto, comp_auto_size, &decomp_auto,
                              &decomp_auto_size, decomp_opts,
                              decomp->user_data));
  absl::Cleanup free_decomp_auto = [decomp_auto] { VSIFree(decomp_auto); };
  ASSERT_EQ(decomp_auto_size, input.size());
  EXPECT_EQ(0, memcmp(decomp_auto, input.data(), input.size()));

  // Decompress with pre-allocated buffer.
  std::vector<char> decomp_pre(input.size() + 64);
  void* decomp_pre_ptr = decomp_pre.data();
  size_t decomp_pre_size = decomp_pre.size();
  ASSERT_TRUE(decomp->pfnFunc(comp_auto, comp_auto_size, &decomp_pre_ptr,
                              &decomp_pre_size, decomp_opts,
                              decomp->user_data));
  ASSERT_EQ(decomp_pre_size, input.size());
  EXPECT_EQ(0, memcmp(decomp_pre.data(), input.data(), input.size()));

  // Decompress corrupt data (auto-alloc and pre-alloc). These calls are
  // intentionally outcome-agnostic: codecs without an integrity check (e.g.
  // lz4) may "successfully" decode garbage, so only the invariants that hold
  // for every codec are asserted. They mainly serve as crash/leak probes under
  // sanitizers.
  std::vector<char> corrupt(
      static_cast<const char*>(comp_auto),
      static_cast<const char*>(comp_auto) + comp_auto_size);
  for (size_t i = corrupt.size() / 2; i < corrupt.size(); ++i) {
    corrupt[i] ^= 0xFF;
  }
  void* corrupt_auto = nullptr;
  size_t corrupt_auto_size = 0;
  if (decomp->pfnFunc(corrupt.data(), corrupt.size(), &corrupt_auto,
                      &corrupt_auto_size, decomp_opts, decomp->user_data)) {
    EXPECT_NE(corrupt_auto, nullptr);
    VSIFree(corrupt_auto);
  }
  decomp_pre_ptr = decomp_pre.data();
  decomp_pre_size = decomp_pre.size();
  if (decomp->pfnFunc(corrupt.data(), corrupt.size(), &decomp_pre_ptr,
                      &decomp_pre_size, decomp_opts, decomp->user_data)) {
    EXPECT_EQ(decomp_pre_ptr, decomp_pre.data());
    EXPECT_LE(decomp_pre_size, decomp_pre.size());
  }
}

TEST(CplCompressor, InvalidStructVersionRejected) {
  WithQuietHandler quiet;
  CPLCompressor invalid_comp;
  BuildTestCompressor(&invalid_comp);
  invalid_comp.nStructVersion = 0;
  EXPECT_FALSE(CPLRegisterCompressor(&invalid_comp));
  EXPECT_FALSE(CPLRegisterDecompressor(&invalid_comp));
}

TEST(CplCompressor, BloscVariantsAndErrors) {
  WithQuietHandler quiet;
  absl::Cleanup registry_cleanup = [] { CPLDestroyCompressorRegistry(); };

  const char* opts_byte[] = {"CNAME=lz4",    "CLEVEL=3",
                             "SHUFFLE=BYTE", "BLOCKSIZE=4096",
                             "TYPESIZE=1",   "NUM_THREADS=ALL_CPUS",
                             nullptr};
  const char* decomp_threads[] = {"NUM_THREADS=ALL_CPUS", nullptr};
  TestGenericCompressorRoundtrip("blosc", opts_byte, decomp_threads);

  const char* opts_none[] = {"SHUFFLE=NONE", "NUM_THREADS=2", nullptr};
  const char* decomp_num[] = {"NUM_THREADS=2", nullptr};
  TestGenericCompressorRoundtrip("blosc", opts_none, decomp_num);

  // Invalid BLOCKSIZE (also exercises SHUFFLE=BIT option parsing).
  const CPLCompressor* comp = CPLGetCompressor("blosc");
  ASSERT_NE(comp, nullptr);
  const char* bad_blocksize[] = {"SHUFFLE=BIT", "BLOCKSIZE=-1", nullptr};
  void* out = nullptr;
  size_t out_size = 0;
  EXPECT_FALSE(comp->pfnFunc("abcd", 4, &out, &out_size, bad_blocksize,
                             comp->user_data));

  // Truncated input to blosc decompressor.
  const CPLCompressor* decomp = CPLGetDecompressor("blosc");
  ASSERT_NE(decomp, nullptr);
  EXPECT_FALSE(
      decomp->pfnFunc("ab", 2, nullptr, &out_size, nullptr, decomp->user_data));
}

TEST(CplCompressor, LzmaVariantsAndErrors) {
  WithQuietHandler quiet;
  absl::Cleanup registry_cleanup = [] { CPLDestroyCompressorRegistry(); };

  const char* opts[] = {"LEVEL=1", "DELTA=2", nullptr};
  TestGenericCompressorRoundtrip("lzma", opts, nullptr);
}

TEST(CplCompressor, ZstdVariantsAndErrors) {
  WithQuietHandler quiet;
  absl::Cleanup registry_cleanup = [] { CPLDestroyCompressorRegistry(); };

  const char* opts[] = {"LEVEL=3", "CHECKSUM=YES", nullptr};
  TestGenericCompressorRoundtrip("zstd", opts, nullptr);

  const CPLCompressor* comp = CPLGetCompressor("zstd");
  ASSERT_NE(comp, nullptr);
  const char* bad_level[] = {"LEVEL=9999999", nullptr};
  void* out = nullptr;
  size_t out_size = 0;
  // zstd clamps out-of-range compression levels (ZSTD_cParam_clampBounds in
  // ZSTD_CCtxParams_setParameter), so an absurd LEVEL still compresses.
  ASSERT_TRUE(
      comp->pfnFunc("abcd", 4, &out, &out_size, bad_level, comp->user_data));
  EXPECT_NE(out, nullptr);
  EXPECT_GT(out_size, 0U);
  VSIFree(out);

  // Corrupt header for zstd decompressor size query.
  const CPLCompressor* decomp = CPLGetDecompressor("zstd");
  ASSERT_NE(decomp, nullptr);
  EXPECT_FALSE(decomp->pfnFunc("not_zstd_data", 13, nullptr, &out_size, nullptr,
                               decomp->user_data));
}

TEST(CplCompressor, Lz4VariantsAndErrors) {
  WithQuietHandler quiet;
  absl::Cleanup registry_cleanup = [] { CPLDestroyCompressorRegistry(); };

  const char* opts_hdr[] = {"HEADER=YES", "ACCELERATION=2", nullptr};
  TestGenericCompressorRoundtrip("lz4", opts_hdr, opts_hdr);

  const CPLCompressor* comp = CPLGetCompressor("lz4");
  const CPLCompressor* decomp = CPLGetDecompressor("lz4");
  ASSERT_NE(comp, nullptr);
  ASSERT_NE(decomp, nullptr);

  // Test HEADER=NO compressor and decompressor paths.
  // Note: CPLLZ4Compressor overwrites the first 4 bytes with sizeLSB even when
  // HEADER=NO, so to test CPLLZ4Decompressor with HEADER=NO (including its
  // 2x realloc loop) we strip the 4-byte header from a HEADER=YES compressed
  // buffer.
  const char* opts_nohdr[] = {"HEADER=NO", "ACCELERATION=2", nullptr};
  std::vector<char> input(2000, 'A');
  size_t nohdr_bound = 0;
  EXPECT_TRUE(comp->pfnFunc(input.data(), input.size(), nullptr, &nohdr_bound,
                            opts_nohdr, comp->user_data));
  void* nohdr_comp = nullptr;
  size_t nohdr_comp_size = 0;
  EXPECT_TRUE(comp->pfnFunc(input.data(), input.size(), &nohdr_comp,
                            &nohdr_comp_size, opts_nohdr, comp->user_data));
  VSIFree(nohdr_comp);

  void* hdr_comp = nullptr;
  size_t hdr_comp_size = 0;
  ASSERT_TRUE(comp->pfnFunc(input.data(), input.size(), &hdr_comp,
                            &hdr_comp_size, opts_hdr, comp->user_data));
  absl::Cleanup free_hdr_comp = [hdr_comp] { VSIFree(hdr_comp); };
  ASSERT_GT(hdr_comp_size, 4U);
  const char* raw_lz4 = static_cast<const char*>(hdr_comp) + 4;
  const size_t raw_lz4_size = hdr_comp_size - 4;

  // Query size with HEADER=NO (decompresses into temporary buffer).
  size_t queried_size = 0;
  EXPECT_TRUE(decomp->pfnFunc(raw_lz4, raw_lz4_size, nullptr, &queried_size,
                              opts_nohdr, decomp->user_data));
  EXPECT_EQ(queried_size, input.size());

  // Auto-alloc decompress with HEADER=NO (exercises 2x growth loop).
  void* raw_decomp = nullptr;
  size_t raw_decomp_size = 0;
  ASSERT_TRUE(decomp->pfnFunc(raw_lz4, raw_lz4_size, &raw_decomp,
                              &raw_decomp_size, opts_nohdr, decomp->user_data));
  EXPECT_EQ(raw_decomp_size, input.size());
  VSIFree(raw_decomp);

  size_t out_size = 0;
  void* out = nullptr;
  // Too short for header.
  EXPECT_FALSE(
      decomp->pfnFunc("a", 1, &out, &out_size, opts_hdr, decomp->user_data));
  // Negative header size.
  const char neg_hdr[] = "\xff\xff\xff\xff\x00\x00";
  EXPECT_FALSE(decomp->pfnFunc(neg_hdr, sizeof(neg_hdr), &out, &out_size,
                               opts_hdr, decomp->user_data));
  // Huge ratio header size (> 10000x input_size).
  const char huge_hdr[] = "\x00\xe1\xf5\x05\x00\x00";  // 100,000,000
  EXPECT_FALSE(decomp->pfnFunc(huge_hdr, sizeof(huge_hdr), &out, &out_size,
                               opts_hdr, decomp->user_data));
}

TEST(CplCompressor, ZlibAndGzipVariantsAndErrors) {
  WithQuietHandler quiet;
  absl::Cleanup registry_cleanup = [] { CPLDestroyCompressorRegistry(); };

  const char* opts[] = {"LEVEL=5", nullptr};
  TestGenericCompressorRoundtrip("zlib", opts, nullptr);
  TestGenericCompressorRoundtrip("gzip", opts, nullptr);

  // Invalid LEVEL in zlib compressor.
  const CPLCompressor* comp = CPLGetCompressor("zlib");
  ASSERT_NE(comp, nullptr);
  const char* bad_level[] = {"LEVEL=999", nullptr};
  void* out = nullptr;
  size_t out_size = 0;
  EXPECT_FALSE(
      comp->pfnFunc("abcd", 4, &out, &out_size, bad_level, comp->user_data));
}

TEST(CplCompressor, DeltaFilterAllTypesAndErrors) {
  WithQuietHandler quiet;
  absl::Cleanup registry_cleanup = [] { CPLDestroyCompressorRegistry(); };

  const CPLCompressor* comp = CPLGetCompressor("delta");
  const CPLCompressor* decomp = CPLGetDecompressor("delta");
  ASSERT_NE(comp, nullptr);
  ASSERT_NE(decomp, nullptr);

  // Prepare 64-byte aligned test data (zeros roundtrip bit-exactly across all
  // integer and little/big-endian IEEE-754 float/double delta types).
  std::vector<uint8_t> data(64, 0);

  // Error cases for delta compressor and decompressor.
  size_t out_size = 0;
  void* out = nullptr;
  char small_buf[16] = {0};
  void* non_null_out = small_buf;
  size_t non_zero_size = sizeof(small_buf);

  for (const CPLCompressor* fn : {comp, decomp}) {
    // Null output_size or null input_data.
    EXPECT_FALSE(fn->pfnFunc(data.data(), data.size(), nullptr, nullptr,
                             nullptr, fn->user_data));
    non_null_out = small_buf;
    non_zero_size = sizeof(small_buf);
    EXPECT_FALSE(fn->pfnFunc(nullptr, data.size(), &non_null_out,
                             &non_zero_size, nullptr, fn->user_data));

    // Missing DTYPE.
    EXPECT_FALSE(fn->pfnFunc(data.data(), data.size(), &out, &out_size, nullptr,
                             fn->user_data));

    // ASTYPE != DTYPE.
    const char* mismatch_opts[] = {"DTYPE=i2", "ASTYPE=i4", nullptr};
    EXPECT_FALSE(fn->pfnFunc(data.data(), data.size(), &out, &out_size,
                             mismatch_opts, fn->user_data));

    // Unsupported DTYPE.
    const char* bad_dtype[] = {"DTYPE=unknown", nullptr};
    EXPECT_FALSE(fn->pfnFunc(data.data(), data.size(), &out, &out_size,
                             bad_dtype, fn->user_data));
  }

  const char* kDTypes[] = {
      "i1",  "u1",  "i2", "<i2", ">i2", "u2", "<u2", ">u2", "i4",
      "<i4", ">i4", "u4", "<u4", ">u4", "i8", "<i8", ">i8", "u8",
      "<u8", ">u8", "f4", "<f4", ">f4", "f8", "<f8", ">f8",
  };

  for (const char* dtype : kDTypes) {
    std::string dtype_opt = std::string("DTYPE=") + dtype;
    std::string astype_opt = std::string("ASTYPE=") + dtype;
    const char* opts[] = {dtype_opt.c_str(), astype_opt.c_str(), nullptr};

    // Query required buffer size.
    size_t req_size = 0;
    ASSERT_TRUE(comp->pfnFunc(data.data(), data.size(), nullptr, &req_size,
                              opts, comp->user_data))
        << dtype;
    EXPECT_EQ(req_size, data.size());

    ASSERT_TRUE(decomp->pfnFunc(data.data(), data.size(), nullptr, &req_size,
                                opts, decomp->user_data))
        << dtype;
    EXPECT_EQ(req_size, data.size());

    // Too small output buffer.
    non_null_out = small_buf;
    non_zero_size = 4;
    EXPECT_FALSE(comp->pfnFunc(data.data(), data.size(), &non_null_out,
                               &non_zero_size, opts, comp->user_data));
    non_null_out = small_buf;
    non_zero_size = 4;
    EXPECT_FALSE(decomp->pfnFunc(data.data(), data.size(), &non_null_out,
                                 &non_zero_size, opts, decomp->user_data));

    // Unaligned input_size (for types > 1 byte).
    if (dtype[0] != 'i' || dtype[1] != '1') {
      if (dtype[0] != 'u' || dtype[1] != '1') {
        void* unaligned_out = nullptr;
        size_t unaligned_size = 0;
        EXPECT_FALSE(comp->pfnFunc(data.data(), 1, &unaligned_out,
                                   &unaligned_size, opts, comp->user_data));
        EXPECT_FALSE(decomp->pfnFunc(data.data(), 1, &unaligned_out,
                                     &unaligned_size, opts, decomp->user_data));
      }
    }

    // Compress with auto-alloc.
    void* encoded = nullptr;
    size_t encoded_size = 0;
    ASSERT_TRUE(comp->pfnFunc(data.data(), data.size(), &encoded, &encoded_size,
                              opts, comp->user_data))
        << dtype;
    absl::Cleanup free_encoded = [encoded] { VSIFree(encoded); };
    ASSERT_EQ(encoded_size, data.size());

    // Decompress with auto-alloc.
    void* decoded = nullptr;
    size_t decoded_size = 0;
    ASSERT_TRUE(decomp->pfnFunc(encoded, encoded_size, &decoded, &decoded_size,
                                opts, decomp->user_data))
        << dtype;
    absl::Cleanup free_decoded = [decoded] { VSIFree(decoded); };
    ASSERT_EQ(decoded_size, data.size());
    EXPECT_EQ(0, memcmp(decoded, data.data(), data.size())) << dtype;

    // Also test pre-allocated output buffers for both comp and decomp.
    std::vector<uint8_t> enc_pre(data.size());
    void* enc_pre_ptr = enc_pre.data();
    size_t enc_pre_size = enc_pre.size();
    ASSERT_TRUE(comp->pfnFunc(data.data(), data.size(), &enc_pre_ptr,
                              &enc_pre_size, opts, comp->user_data));

    std::vector<uint8_t> dec_pre(data.size());
    void* dec_pre_ptr = dec_pre.data();
    size_t dec_pre_size = dec_pre.size();
    ASSERT_TRUE(decomp->pfnFunc(enc_pre.data(), enc_pre.size(), &dec_pre_ptr,
                                &dec_pre_size, opts, decomp->user_data));
    EXPECT_EQ(0, memcmp(dec_pre.data(), data.data(), data.size())) << dtype;
  }
}

}  // namespace
}  // namespace autotest2
