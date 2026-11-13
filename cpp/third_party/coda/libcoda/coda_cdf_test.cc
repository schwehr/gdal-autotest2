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
// Tests CODA's NASA CDF v3.x backend (libcoda/coda-cdf.c): file recognition,
// reopening, closing, CDR/GDR/VDR/CPR/VXR/VVR/CVVR/ADR/AEDR parsing, endianness
// handling, row/column array ordering, gzip-compressed CVVR decompression,
// sparse records, and corrupt/truncated record rejection.
//
// google3 build caveats:
//   - CODA's built-in CDF reader supports CDF v3.x only (v1/v2 and full-file
//     compression are rejected), IEEE 754 encodings only, and single-file CDFs.
//   - Upstream coda-cdf.c does not populate rdim_sizes from GDR and omits
//     `offset += 340` for rVariables when rnum_dims > 0, so valid rVariable
//     tests keep rdims empty (rnum_dims == 0).
//   - Upstream coda-cdf.c:319-340 allocates `array` for multi-element
//     non-CHAR/UCHAR attributes and writes elements into `*attribute` but
//     forgets `*attribute = (coda_dynamic_type *)array;`, which leaks `array`
//     (and elements 0..N-2) for num_elems >= 2 and leaves `*attribute`
//     uninitialized for num_elems == 0; the multi-element attribute test
//     scopes coda_open() with an unconditional absl::LeakCheckDisabler, and
//     num_elems == 0 is avoided for non-CHAR/UCHAR attributes.
//   - Upstream coda-cdf.c:451-457 leaks the parsed `attribute` when a variable
//     attribute entry number exceeds `root_type->num_fields`; that negative
//     test is scoped with an unconditional absl::LeakCheckDisabler.
//   - OOM malloc/strdup checks and WORDS_BIGENDIAN branches in coda-cdf.c are
//     not reachable on little-endian hosts without allocator injection.
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-cdf.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-cdf.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-cdf-internal.h

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/debugging/leak_check.h"
#include "third_party/stcorp_coda/google/util/cdf_builder.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-bin.h"
#include "third_party/stcorp_coda/libcoda/coda-cdf-internal.h"
}  // extern "C"

namespace stcorp_coda {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

class CodaCdfTest : public ::testing::Test {
 protected:
  void SetUp() override {
    restore_def_.Unset();
    coda_set_error(CODA_SUCCESS, nullptr);
    ASSERT_EQ(CODA_SUCCESS, session_.Init());
  }

  void TearDown() override { session_.Reset(); }

  ScopedEnvVar restore_def_{"CODA_DEFINITION"};
  ScopedCodaSession session_;
};

// Truncates `bytes` to `new_size` and updates GDR's EOF field (at
// `gdr_offset + 36`) when the truncated prefix still includes that field, so
// read_GDR()'s EOF check passes and downstream record reads are exercised.
std::vector<uint8_t> TruncateAndPatchEof(std::vector<uint8_t> bytes,
                                         int64_t gdr_offset, size_t new_size) {
  bytes.resize(new_size);
  if (new_size >= static_cast<size_t>(gdr_offset + 44)) {
    CdfBuilder::PatchBe64(bytes, static_cast<size_t>(gdr_offset + 36),
                          static_cast<int64_t>(new_size));
  }
  return bytes;
}

void OpenExpectError(std::string_view filename, std::span<const uint8_t> bytes,
                     int expected_errno,
                     std::string_view expected_substr = "") {
  TempFile file(filename);
  ASSERT_TRUE(file.WriteBytes(bytes));

  ScopedProduct product;
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  EXPECT_EQ(-1, coda_open(file.c_str(), product.OutParam()));
  EXPECT_EQ(nullptr, product.get());
  EXPECT_EQ(expected_errno, coda_errno);
  if (!expected_substr.empty()) {
    EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr(expected_substr));
  }
}

void OpenAndExpectCursorReadOk(std::span<const uint8_t> bytes,
                               std::string_view filename, const char* path,
                               std::vector<int32_t>& out) {
  TempFile file(filename);
  ASSERT_TRUE(file.WriteBytes(bytes));
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));

  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, path));
  long num_elems = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&cursor, &num_elems));
  ASSERT_GT(num_elems, 0);
  out.assign(static_cast<size_t>(num_elems), -999);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32_array(&cursor, out.data(),
                                                       coda_array_ordering_c));
}

TEST_F(CodaCdfTest, RecognizeOpenAndCloseMinimalCdfV3) {
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  // 1. Non-existent path fails with CODA_ERROR_FILE_NOT_FOUND.
  const std::filesystem::path missing_path =
      std::filesystem::path(::testing::TempDir()) / "coda_cdf_missing.cdf";
  {
    std::error_code ec;
    std::filesystem::remove(missing_path, ec);
  }

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_recognize_file(missing_path.c_str(), nullptr, nullptr,
                                    nullptr, nullptr, nullptr));
  EXPECT_EQ(CODA_ERROR_FILE_NOT_FOUND, coda_errno);

  ScopedProduct product;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(missing_path.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_FILE_NOT_FOUND, coda_errno);
  EXPECT_EQ(nullptr, product.get());

  // 2. Non-CDF payload falls back to binary and fails without a definition.
  TempFile non_cdf("coda_cdf_non_cdf.dat");
  ASSERT_TRUE(non_cdf.WriteText("NOT_A_CDF_FILE_PAYLOAD"));
  coda_format detected_fmt = coda_format_cdf;
  ASSERT_EQ(CODA_SUCCESS,
            coda_recognize_file(non_cdf.c_str(), nullptr, &detected_fmt,
                                nullptr, nullptr, nullptr));
  EXPECT_EQ(coda_format_binary, detected_fmt);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(non_cdf.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_UNSUPPORTED_PRODUCT, coda_errno);
  EXPECT_EQ(nullptr, product.get());

  // 3. Minimal valid CDF v3.9.1 with MD5 checksum trailer.
  const std::vector<uint8_t> valid_bytes =
      CdfBuilder()
          .SetVersion(3, 9, 1)
          .SetCopyright("Google CDF test")
          .SetMd5Checksum(true, /*append_trailer=*/true)
          .Build();
  TempFile cdf_file("coda_cdf_minimal.cdf");
  ASSERT_TRUE(cdf_file.WriteBytes(valid_bytes));

  int64_t rec_size = -1;
  coda_format rec_fmt = coda_format_binary;
  const char* prod_class = "sentinel";
  const char* prod_type = "sentinel";
  int prod_version = 99;
  ASSERT_EQ(CODA_SUCCESS,
            coda_recognize_file(cdf_file.c_str(), &rec_size, &rec_fmt,
                                &prod_class, &prod_type, &prod_version));
  EXPECT_EQ(static_cast<int64_t>(valid_bytes.size()), rec_size);
  EXPECT_EQ(coda_format_cdf, rec_fmt);
  EXPECT_EQ(nullptr, prod_class);
  EXPECT_EQ(nullptr, prod_type);
  EXPECT_EQ(-1, prod_version);

  ASSERT_EQ(CODA_SUCCESS, coda_open(cdf_file.c_str(), product.OutParam()));
  const char* open_name = nullptr;
  int64_t open_size = -1;
  coda_format open_fmt = coda_format_binary;
  coda_type* root_type = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_get_product_filename(product.get(), &open_name));
  EXPECT_STREQ(cdf_file.c_str(), open_name);
  ASSERT_EQ(CODA_SUCCESS,
            coda_get_product_file_size(product.get(), &open_size));
  EXPECT_EQ(static_cast<int64_t>(valid_bytes.size()), open_size);
  ASSERT_EQ(CODA_SUCCESS, coda_get_product_format(product.get(), &open_fmt));
  EXPECT_EQ(coda_format_cdf, open_fmt);
  ASSERT_EQ(CODA_SUCCESS,
            coda_get_product_root_type(product.get(), &root_type));
  ASSERT_NE(nullptr, root_type);

  const auto* cdf_prod =
      reinterpret_cast<const coda_cdf_product*>(product.get());
  EXPECT_EQ(3, cdf_prod->cdf_version);
  EXPECT_EQ(9, cdf_prod->cdf_release);
  EXPECT_EQ(1, cdf_prod->cdf_increment);
  EXPECT_EQ(coda_big_endian, cdf_prod->endianness);
  EXPECT_EQ(coda_array_ordering_c, cdf_prod->array_ordering);
  EXPECT_EQ(1, cdf_prod->has_md5_chksum);
  EXPECT_EQ(0, cdf_prod->rnum_dims);
  EXPECT_NE(nullptr, cdf_prod->raw_product);
  EXPECT_EQ(CODA_SUCCESS, coda_close(product.release()));

  // 4. Partial MD5 flag (bit 2 set without bit 3) leaves has_md5_chksum == 0.
  const std::vector<uint8_t> partial_md5 =
      CdfBuilder()
          .SetCdrFlagsOverride(kCdfFlagSingleFile | kCdfFlagMd5Bit1)
          .Build();
  TempFile partial_file("coda_cdf_partial_md5.cdf");
  ASSERT_TRUE(partial_file.WriteBytes(partial_md5));
  ASSERT_EQ(CODA_SUCCESS, coda_open(partial_file.c_str(), product.OutParam()));
  EXPECT_EQ(
      0,
      reinterpret_cast<const coda_cdf_product*>(product.get())->has_md5_chksum);
  EXPECT_EQ(CODA_SUCCESS, coda_close(product.release()));
}

TEST_F(CodaCdfTest, ReopenAndCloseDirectApiAndNonMmapPath) {
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  // 1. Direct coda_bin_open + coda_cdf_reopen + coda_cdf_close succeeds.
  const std::vector<uint8_t> valid_bytes = CdfBuilder().Build();
  TempFile valid_file("coda_cdf_direct_ok.cdf");
  ASSERT_TRUE(valid_file.WriteBytes(valid_bytes));

  coda_product* raw = nullptr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_bin_open(valid_file.c_str(),
                          static_cast<int64_t>(valid_bytes.size()), &raw));
  ASSERT_EQ(0, coda_cdf_reopen(&raw));
  ScopedProduct reopened(raw);
  EXPECT_EQ(coda_format_cdf, reopened.get()->format);
  EXPECT_EQ(CODA_SUCCESS, coda_close(reopened.release()));

  // 2. Direct coda_cdf_reopen on < 8 bytes fails reading magic and cleans up.
  constexpr std::array<uint8_t, 4> kShortMagic = {0xCD, 0xF3, 0x00, 0x01};
  TempFile short_file("coda_cdf_direct_short.cdf");
  ASSERT_TRUE(short_file.WriteBytes(kShortMagic));
  coda_product* short_raw = nullptr;
  ASSERT_EQ(
      CODA_SUCCESS,
      coda_bin_open(short_file.c_str(),
                    static_cast<int64_t>(kShortMagic.size()), &short_raw));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cdf_reopen(&short_raw));
  EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);

  // 3. coda_cdf_close() frees mem_ptr when populated and skips null pointers.
  auto* synthetic =
      static_cast<coda_cdf_product*>(std::calloc(1, sizeof(coda_cdf_product)));
  ASSERT_NE(nullptr, synthetic);
  synthetic->format = coda_format_cdf;
  synthetic->mem_ptr = static_cast<uint8_t*>(std::malloc(16));
  ASSERT_NE(nullptr, synthetic->mem_ptr);
  EXPECT_EQ(0, coda_cdf_close(reinterpret_cast<coda_product*>(synthetic)));

  // 4. Opening and reading a CDF with mmap disabled exercises pread path.
  const int saved_mmap = coda_get_option_use_mmap();
  const absl::Cleanup restore_mmap = [saved_mmap] {
    coda_set_option_use_mmap(saved_mmap);
  };
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(0));

  CdfVarSpec zvar;
  zvar.name = "vals";
  zvar.data_type = CdfDataType::kInt4;
  zvar.data = CdfBuilder::EncodeValues<int32_t>({7, 8}, /*big_endian=*/true);
  const std::vector<uint8_t> pread_bytes = CdfBuilder().AddZVar(zvar).Build();
  std::vector<int32_t> vals;
  ASSERT_NO_FATAL_FAILURE(OpenAndExpectCursorReadOk(
      pread_bytes, "coda_cdf_no_mmap.cdf", "/vals", vals));
  EXPECT_THAT(vals, ElementsAre(7, 8));
}

TEST_F(CodaCdfTest, RejectsOlderVersionsAndFullFileCompression) {
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  struct MagicCase {
    std::string_view name;
    CdfMagicPreset preset;
    std::string_view expected_substr;
  };
  constexpr MagicCase kCases[] = {
      {.name = "coda_cdf_v1.cdf",
       .preset = CdfMagicPreset::kV1Uncompressed,
       .expected_substr = "older than 3.0"},
      {.name = "coda_cdf_v2.cdf",
       .preset = CdfMagicPreset::kV2Uncompressed,
       .expected_substr = "older than 3.0"},
      {.name = "coda_cdf_v2_comp.cdf",
       .preset = CdfMagicPreset::kV2Compressed,
       .expected_substr = "older than 3.0"},
      {.name = "coda_cdf_v3_comp.cdf",
       .preset = CdfMagicPreset::kV3Compressed,
       .expected_substr = "full file compression"},
  };

  for (const MagicCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const std::vector<uint8_t> bytes =
        CdfBuilder().SetMagicPreset(tc.preset).Build();
    OpenExpectError(tc.name, bytes, CODA_ERROR_UNSUPPORTED_PRODUCT,
                    tc.expected_substr);

    // Also test direct coda_cdf_reopen().
    TempFile file(tc.name);
    ASSERT_TRUE(file.WriteBytes(bytes));
    coda_product* raw = nullptr;
    ASSERT_EQ(
        CODA_SUCCESS,
        coda_bin_open(file.c_str(), static_cast<int64_t>(bytes.size()), &raw));
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cdf_reopen(&raw));
    EXPECT_EQ(CODA_ERROR_UNSUPPORTED_PRODUCT, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno),
                HasSubstr(tc.expected_substr));
  }
}

TEST_F(CodaCdfTest, EncodingsAndFloatingPointRejection) {
  struct EncodingCase {
    CdfEncoding encoding;
    coda_endianness expected_endianness;
  };
  constexpr EncodingCase kSupportedEncodings[] = {
      {.encoding = CdfEncoding::kNetwork,
       .expected_endianness = coda_big_endian},
      {.encoding = CdfEncoding::kSun, .expected_endianness = coda_big_endian},
      {.encoding = CdfEncoding::kSgi, .expected_endianness = coda_big_endian},
      {.encoding = CdfEncoding::kIbmRs, .expected_endianness = coda_big_endian},
      {.encoding = CdfEncoding::kMac, .expected_endianness = coda_big_endian},
      {.encoding = CdfEncoding::kHp, .expected_endianness = coda_big_endian},
      {.encoding = CdfEncoding::kNext, .expected_endianness = coda_big_endian},
      {.encoding = CdfEncoding::kDecstation,
       .expected_endianness = coda_little_endian},
      {.encoding = CdfEncoding::kIbmPc,
       .expected_endianness = coda_little_endian},
      {.encoding = CdfEncoding::kAlphaOsf1,
       .expected_endianness = coda_little_endian},
      {.encoding = CdfEncoding::kAlphaVmsI,
       .expected_endianness = coda_little_endian},
  };

  for (const EncodingCase& tc : kSupportedEncodings) {
    SCOPED_TRACE(static_cast<int32_t>(tc.encoding));
    const bool be = tc.expected_endianness == coda_big_endian;
    CdfVarSpec zvar;
    zvar.name = "val";
    zvar.data_type = CdfDataType::kInt4;
    zvar.rec_varys = false;
    zvar.data = CdfBuilder::EncodeValue<int32_t>(0x12345678, be);

    CdfAttrSpec attr;
    attr.name = "tag";
    attr.scope = CdfAttrScope::kGlobal;
    attr.entries.push_back(CdfAttrEntrySpec::Scalar<int16_t>(
        0, CdfDataType::kInt2, static_cast<int16_t>(-1234), be));

    const std::vector<uint8_t> bytes = CdfBuilder()
                                           .SetEncoding(tc.encoding)
                                           .AddZVar(zvar)
                                           .AddAttr(attr)
                                           .Build();
    TempFile file("coda_cdf_encoding.cdf");
    ASSERT_TRUE(file.WriteBytes(bytes));
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));
    EXPECT_EQ(
        tc.expected_endianness,
        reinterpret_cast<const coda_cdf_product*>(product.get())->endianness);

    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/val"));
    int32_t v = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &v));
    EXPECT_EQ(0x12345678, v);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "@tag"));
    int16_t a = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&cursor, &a));
    EXPECT_EQ(-1234, a);
  }

  // Unsupported non-IEEE floating point and unknown encodings.
  constexpr int32_t kNonIeeeEncodings[] = {
      static_cast<int32_t>(CdfEncoding::kVax),
      static_cast<int32_t>(CdfEncoding::kAlphaVmsD),
      static_cast<int32_t>(CdfEncoding::kAlphaVmsG),
  };
  for (const int32_t enc : kNonIeeeEncodings) {
    SCOPED_TRACE(enc);
    const std::vector<uint8_t> bytes = CdfBuilder().SetEncodingRaw(enc).Build();
    OpenExpectError("coda_cdf_bad_encoding.cdf", bytes,
                    CODA_ERROR_UNSUPPORTED_PRODUCT, "IEEE 754");
  }
  OpenExpectError("coda_cdf_unknown_encoding.cdf",
                  CdfBuilder().SetEncodingRaw(99).Build(),
                  CODA_ERROR_UNSUPPORTED_PRODUCT, "unsupported encoding");
}

TEST_F(CodaCdfTest, CdrAndGdrValidationErrorsAndTruncations) {
  // 1. Multi-file CDF flag error.
  OpenExpectError("coda_cdf_multifile.cdf",
                  CdfBuilder().SetSingleFile(false).Build(),
                  CODA_ERROR_UNSUPPORTED_PRODUCT, "multi-file CDF");

  // 2. CDR record type and GDR offset bounds errors.
  OpenExpectError("coda_cdf_cdr_rectype.cdf",
                  CdfBuilder().SetCdrRecordTypeOverride(99).Build(),
                  CODA_ERROR_PRODUCT, "CDR record");
  OpenExpectError("coda_cdf_gdr_neg.cdf",
                  CdfBuilder().SetGdrOffsetOverride(-1).Build(),
                  CODA_ERROR_PRODUCT, "GDR record");
  OpenExpectError("coda_cdf_gdr_oob.cdf",
                  CdfBuilder().SetGdrOffsetOverride(100000).Build(),
                  CODA_ERROR_PRODUCT, "GDR record");

  // 3. GDR record type, head offsets, counts, and EOF mismatch errors.
  OpenExpectError("coda_cdf_gdr_rectype.cdf",
                  CdfBuilder().SetGdrRecordTypeOverride(99).Build(),
                  CODA_ERROR_PRODUCT, "GDR record");
  OpenExpectError("coda_cdf_gdr_rvdr_neg.cdf",
                  CdfBuilder().SetRvdrHeadOverride(-1).Build(),
                  CODA_ERROR_PRODUCT, "rVDR record");
  OpenExpectError("coda_cdf_gdr_rvdr_oob.cdf",
                  CdfBuilder().SetRvdrHeadOverride(100000).Build(),
                  CODA_ERROR_PRODUCT, "rVDR record");
  OpenExpectError("coda_cdf_gdr_zvdr_neg.cdf",
                  CdfBuilder().SetZvdrHeadOverride(-1).Build(),
                  CODA_ERROR_PRODUCT, "zVDR record");
  OpenExpectError("coda_cdf_gdr_zvdr_oob.cdf",
                  CdfBuilder().SetZvdrHeadOverride(100000).Build(),
                  CODA_ERROR_PRODUCT, "zVDR record");
  OpenExpectError("coda_cdf_gdr_adr_neg.cdf",
                  CdfBuilder().SetAdrHeadOverride(-1).Build(),
                  CODA_ERROR_PRODUCT, "ADR record");
  OpenExpectError("coda_cdf_gdr_adr_oob.cdf",
                  CdfBuilder().SetAdrHeadOverride(100000).Build(),
                  CODA_ERROR_PRODUCT, "ADR record");
  OpenExpectError("coda_cdf_gdr_nrvars_neg.cdf",
                  CdfBuilder().SetNrVarsOverride(-1).Build(),
                  CODA_ERROR_PRODUCT, "number of variables");
  OpenExpectError("coda_cdf_gdr_numattr_neg.cdf",
                  CdfBuilder().SetNumAttrOverride(-1).Build(),
                  CODA_ERROR_PRODUCT, "number of attributes");
  OpenExpectError("coda_cdf_gdr_rnumdims_neg.cdf",
                  CdfBuilder().SetRnumDimsOverride(-1).Build(),
                  CODA_ERROR_PRODUCT, "number of dimensions");
  OpenExpectError("coda_cdf_gdr_nzvars_neg.cdf",
                  CdfBuilder().SetNzVarsOverride(-1).Build(),
                  CODA_ERROR_PRODUCT, "number of zVariables");
  OpenExpectError("coda_cdf_gdr_eof_mismatch.cdf",
                  CdfBuilder().SetEofOverride(123).Build(), CODA_ERROR_PRODUCT,
                  "does not match file size");

  // 4. Truncated CDR & GDR reads at every read_bytes() offset.
  const CdfBuildResult base = CdfBuilder().BuildWithOffsets();
  constexpr size_t kCdrCutoffs[] = {8, 20, 28, 32, 36, 40, 52};
  for (const size_t len : kCdrCutoffs) {
    SCOPED_TRACE(len);
    ASSERT_LT(len, base.bytes.size());
    std::vector<uint8_t> trunc(
        base.bytes.begin(),
        base.bytes.begin() + static_cast<std::ptrdiff_t>(len));
    OpenExpectError("coda_cdf_cdr_trunc.cdf", trunc,
                    CODA_ERROR_OUT_OF_BOUNDS_READ);
  }

  constexpr size_t kGdrRelCutoffs[] = {8, 12, 20, 28, 36, 44, 48, 56, 60};
  for (const size_t rel : kGdrRelCutoffs) {
    SCOPED_TRACE(rel);
    const size_t len = static_cast<size_t>(base.gdr_offset) + rel;
    ASSERT_LT(len, base.bytes.size());
    std::vector<uint8_t> trunc(
        base.bytes.begin(),
        base.bytes.begin() + static_cast<std::ptrdiff_t>(len));
    OpenExpectError("coda_cdf_gdr_trunc.cdf", trunc,
                    CODA_ERROR_OUT_OF_BOUNDS_READ);
  }
}

TEST_F(CodaCdfTest, ReadsRVariablesZVariablesAndArrayOrdering) {
  auto build_with_ordering = [](CdfArrayOrdering ordering) {
    // Keep rdims empty per coda-cdf.c rVDR quirk (rnum_dims == 0).
    CdfVarSpec r_scalar;
    r_scalar.name = "r_scalar";
    r_scalar.data_type = CdfDataType::kInt4;
    r_scalar.rec_varys = false;
    r_scalar.data = CdfBuilder::EncodeValue<int32_t>(42, /*big_endian=*/true);

    CdfVarSpec r_series;
    r_series.name = "r_series";
    r_series.space_pad_name = true;
    r_series.data_type = CdfDataType::kDouble;
    r_series.rec_varys = true;
    r_series.data =
        CdfBuilder::EncodeValues<double>({1.5, 2.5, 3.5}, /*big_endian=*/true);

    CdfVarSpec z_0d;
    z_0d.name = "z_0d";
    z_0d.data_type = CdfDataType::kInt2;
    z_0d.rec_varys = true;
    z_0d.data = CdfBuilder::EncodeValues<int16_t>({-10, 20},
                                                  /*big_endian=*/true);

    CdfVarSpec z_2d;
    z_2d.name = "z_2d";
    z_2d.data_type = CdfDataType::kInt4;
    z_2d.dims = {2, 3};
    z_2d.rec_varys = true;
    z_2d.data = CdfBuilder::EncodeValues<int32_t>(
        {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}, /*big_endian=*/true);

    CdfVarSpec z_nonvary_dim;
    z_nonvary_dim.name = "z_nonvary_dim";
    z_nonvary_dim.data_type = CdfDataType::kUint4;
    z_nonvary_dim.dims = {2, 3};
    z_nonvary_dim.dim_varys = {1, 0};
    z_nonvary_dim.rec_varys = false;
    z_nonvary_dim.data =
        CdfBuilder::EncodeValues<uint32_t>({100U, 200U}, /*big_endian=*/true);

    CdfVarSpec z_empty;
    z_empty.name = "z_empty";
    z_empty.data_type = CdfDataType::kInt4;
    z_empty.dims = {2};
    z_empty.rec_varys = true;
    z_empty.vxr_head_override = 0;

    return CdfBuilder()
        .SetArrayOrdering(ordering)
        .AddRVar(r_scalar)
        .AddRVar(r_series)
        .AddZVar(z_0d)
        .AddZVar(z_2d)
        .AddZVar(z_nonvary_dim)
        .AddZVar(z_empty)
        .Build();
  };

  for (const CdfArrayOrdering ordering :
       {CdfArrayOrdering::kRowMajor, CdfArrayOrdering::kColumnMajor}) {
    SCOPED_TRACE(static_cast<int32_t>(ordering));
    const std::vector<uint8_t> bytes = build_with_ordering(ordering);
    TempFile file("coda_cdf_vars.cdf");
    ASSERT_TRUE(file.WriteBytes(bytes));
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));

    const coda_array_ordering expected_coda_order =
        ordering == CdfArrayOrdering::kRowMajor ? coda_array_ordering_c
                                                : coda_array_ordering_fortran;
    EXPECT_EQ(expected_coda_order,
              reinterpret_cast<const coda_cdf_product*>(product.get())
                  ->array_ordering);

    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/r_scalar"));
    int32_t r_scalar_val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &r_scalar_val));
    EXPECT_EQ(42, r_scalar_val);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/r_series"));
    std::vector<double> r_series_vals(3, -1.0);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_double_array(&cursor, r_series_vals.data(),
                                            coda_array_ordering_c));
    EXPECT_THAT(r_series_vals, ElementsAre(1.5, 2.5, 3.5));

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/z_0d"));
    std::vector<int16_t> z0d_vals(2, -999);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_int16_array(&cursor, z0d_vals.data(),
                                           coda_array_ordering_c));
    EXPECT_THAT(z0d_vals, ElementsAre(-10, 20));

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/z_2d"));
    int num_dims = 0;
    long dims[CODA_MAX_NUM_DIMS] = {};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_get_array_dim(&cursor, &num_dims, dims));
    ASSERT_EQ(3, num_dims);
    if (ordering == CdfArrayOrdering::kRowMajor) {
      EXPECT_EQ(2, dims[0]);
      EXPECT_EQ(2, dims[1]);
      EXPECT_EQ(3, dims[2]);
    } else {
      EXPECT_EQ(2, dims[0]);
      EXPECT_EQ(3, dims[1]);
      EXPECT_EQ(2, dims[2]);
    }
    std::vector<int32_t> z2d_vals(12, -999);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_int32_array(&cursor, z2d_vals.data(),
                                           coda_array_ordering_c));
    EXPECT_THAT(z2d_vals, ElementsAre(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12));

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/z_nonvary_dim"));
    std::vector<uint32_t> nonvary_vals(2, 999U);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_uint32_array(&cursor, nonvary_vals.data(),
                                            coda_array_ordering_c));
    EXPECT_THAT(nonvary_vals, ElementsAre(100U, 200U));

    // Empty variable (vxr_head == 0) has missing records on read.
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/z_empty"));
    std::vector<int32_t> empty_vals(2, 0);
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int32_array(&cursor, empty_vals.data(),
                                               coda_array_ordering_c));
    EXPECT_EQ(CODA_ERROR_UNSUPPORTED_PRODUCT, coda_errno);
    coda_set_error(CODA_SUCCESS, nullptr);
  }
}

TEST_F(CodaCdfTest, ReadsAllVariableDataTypesAndSparseRecords) {
  CdfBuilder builder;
  builder.SetEncoding(CdfEncoding::kIbmPc);

  auto add_scalar = [&](std::string name, CdfDataType type,
                        std::vector<uint8_t> data, int32_t num_elems = 1) {
    CdfVarSpec v;
    v.name = std::move(name);
    v.data_type = type;
    v.num_elems = num_elems;
    v.rec_varys = false;
    v.data = std::move(data);
    builder.AddZVar(std::move(v));
  };

  add_scalar("v_i1", CdfDataType::kInt1,
             CdfBuilder::EncodeValue<int8_t>(-12, false));
  add_scalar("v_byte", CdfDataType::kByte,
             CdfBuilder::EncodeValue<int8_t>(34, false));
  add_scalar("v_u1", CdfDataType::kUint1,
             CdfBuilder::EncodeValue<uint8_t>(200U, false));
  add_scalar("v_i2", CdfDataType::kInt2,
             CdfBuilder::EncodeValue<int16_t>(-1234, false));
  add_scalar("v_u2", CdfDataType::kUint2,
             CdfBuilder::EncodeValue<uint16_t>(54321U, false));
  add_scalar("v_i4", CdfDataType::kInt4,
             CdfBuilder::EncodeValue<int32_t>(-123456, false));
  add_scalar("v_u4", CdfDataType::kUint4,
             CdfBuilder::EncodeValue<uint32_t>(3000000000U, false));
  add_scalar("v_i8", CdfDataType::kInt8,
             CdfBuilder::EncodeValue<int64_t>(-9000000000LL, false));
  add_scalar("v_r4", CdfDataType::kReal4,
             CdfBuilder::EncodeValue<float>(1.25f, false));
  add_scalar("v_fl", CdfDataType::kFloat,
             CdfBuilder::EncodeValue<float>(-2.5f, false));
  add_scalar("v_r8", CdfDataType::kReal8,
             CdfBuilder::EncodeValue<double>(3.75, false));
  add_scalar("v_db", CdfDataType::kDouble,
             CdfBuilder::EncodeValue<double>(-4.5, false));
  add_scalar("v_ch", CdfDataType::kChar, {'X'}, /*num_elems=*/1);
  add_scalar("v_str", CdfDataType::kChar, {'H', 'e', 'l', 'l', 'o'},
             /*num_elems=*/5);
  add_scalar("v_uch", CdfDataType::kUchar, {'T', 'e', 's', 't'},
             /*num_elems=*/4);
  add_scalar("v_epoch_scalar", CdfDataType::kEpoch,
             CdfBuilder::EncodeValue<double>(63113904001000.0, false));
  add_scalar("v_tt2000_scalar", CdfDataType::kTimeTt2000,
             CdfBuilder::EncodeValue<int64_t>(43202000000000LL, false));

  // Array of time values (hits time_type_new inside array branch).
  {
    CdfVarSpec v_epoch_arr;
    v_epoch_arr.name = "v_epoch_arr";
    v_epoch_arr.data_type = CdfDataType::kEpoch;
    v_epoch_arr.rec_varys = true;
    v_epoch_arr.data = CdfBuilder::EncodeValues<double>(
        {63113904000000.0, 63113904002000.0}, false);
    builder.AddZVar(std::move(v_epoch_arr));
  }

  // Sparse records (padded & previous) with missing record index 1.
  {
    CdfVarSpec v_sparse;
    v_sparse.name = "v_sparse_padded";
    v_sparse.data_type = CdfDataType::kInt4;
    v_sparse.sparse_records = CdfSparseMethod::kPadded;
    v_sparse.max_rec_override = 2;
    v_sparse.chunks.push_back({
        .first_rec = 0,
        .last_rec = 0,
        .data = CdfBuilder::EncodeValue<int32_t>(111, false),
    });
    v_sparse.chunks.push_back({
        .first_rec = 2,
        .last_rec = 2,
        .data = CdfBuilder::EncodeValue<int32_t>(333, false),
    });
    builder.AddZVar(std::move(v_sparse));

    CdfVarSpec v_prev;
    v_prev.name = "v_sparse_prev";
    v_prev.data_type = CdfDataType::kInt4;
    v_prev.sparse_records = CdfSparseMethod::kPrevious;
    v_prev.data = CdfBuilder::EncodeValues<int32_t>({55, 66}, false);
    builder.AddZVar(std::move(v_prev));
  }

  TempFile file("coda_cdf_all_types.cdf");
  ASSERT_TRUE(file.WriteBytes(builder.Build()));
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));

  coda_cursor cursor;
  auto goto_path = [&](const char* path) {
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, path));
  };

  int8_t i8 = 0;
  ASSERT_NO_FATAL_FAILURE(goto_path("/v_i1"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int8(&cursor, &i8));
  EXPECT_EQ(-12, i8);
  ASSERT_NO_FATAL_FAILURE(goto_path("/v_byte"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int8(&cursor, &i8));
  EXPECT_EQ(34, i8);

  uint8_t u8 = 0;
  ASSERT_NO_FATAL_FAILURE(goto_path("/v_u1"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint8(&cursor, &u8));
  EXPECT_EQ(200U, u8);

  int16_t i16 = 0;
  ASSERT_NO_FATAL_FAILURE(goto_path("/v_i2"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&cursor, &i16));
  EXPECT_EQ(-1234, i16);

  uint16_t u16 = 0;
  ASSERT_NO_FATAL_FAILURE(goto_path("/v_u2"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint16(&cursor, &u16));
  EXPECT_EQ(54321U, u16);

  int32_t i32 = 0;
  ASSERT_NO_FATAL_FAILURE(goto_path("/v_i4"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &i32));
  EXPECT_EQ(-123456, i32);

  uint32_t u32 = 0;
  ASSERT_NO_FATAL_FAILURE(goto_path("/v_u4"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint32(&cursor, &u32));
  EXPECT_EQ(3000000000U, u32);

  int64_t i64 = 0;
  ASSERT_NO_FATAL_FAILURE(goto_path("/v_i8"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int64(&cursor, &i64));
  EXPECT_EQ(-9000000000LL, i64);

  float f32 = 0.0f;
  ASSERT_NO_FATAL_FAILURE(goto_path("/v_r4"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&cursor, &f32));
  EXPECT_FLOAT_EQ(1.25f, f32);
  ASSERT_NO_FATAL_FAILURE(goto_path("/v_fl"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&cursor, &f32));
  EXPECT_FLOAT_EQ(-2.5f, f32);

  double f64 = 0.0;
  ASSERT_NO_FATAL_FAILURE(goto_path("/v_r8"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &f64));
  EXPECT_DOUBLE_EQ(3.75, f64);
  ASSERT_NO_FATAL_FAILURE(goto_path("/v_db"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &f64));
  EXPECT_DOUBLE_EQ(-4.5, f64);

  char ch = '\0';
  ASSERT_NO_FATAL_FAILURE(goto_path("/v_ch"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_char(&cursor, &ch));
  EXPECT_EQ('X', ch);

  ASSERT_NO_FATAL_FAILURE(goto_path("/v_str"));
  char str_buf[6] = {};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&cursor, str_buf, 6));
  EXPECT_STREQ("Hello", str_buf);

  ASSERT_NO_FATAL_FAILURE(goto_path("/v_uch"));
  char uch_buf[5] = {};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&cursor, uch_buf, 5));
  EXPECT_STREQ("Test", uch_buf);

  // Special time variables: read evaluated seconds and raw base values.
  ASSERT_NO_FATAL_FAILURE(goto_path("/v_epoch_scalar"));
  coda_special_type sp = coda_special_no_data;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_special_type(&cursor, &sp));
  EXPECT_EQ(coda_special_time, sp);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &f64));
  EXPECT_NEAR(1.0, f64, 1e-6);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_use_base_type_of_special_type(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &f64));
  EXPECT_DOUBLE_EQ(63113904001000.0, f64);

  ASSERT_NO_FATAL_FAILURE(goto_path("/v_tt2000_scalar"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &f64));
  EXPECT_NEAR(2.0, f64, 1e-6);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_use_base_type_of_special_type(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int64(&cursor, &i64));
  EXPECT_EQ(43202000000000LL, i64);

  ASSERT_NO_FATAL_FAILURE(goto_path("/v_epoch_arr"));
  std::vector<double> epoch_arr(2, -1.0);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_double_array(&cursor, epoch_arr.data(),
                                          coda_array_ordering_c));
  EXPECT_NEAR(0.0, epoch_arr[0], 1e-6);
  EXPECT_NEAR(2.0, epoch_arr[1], 1e-6);

  // Sparse records: records 0 and 2 succeed; missing record 1 fails.
  ASSERT_NO_FATAL_FAILURE(goto_path("/v_sparse_padded"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&cursor, 0));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &i32));
  EXPECT_EQ(111, i32);
  ASSERT_NO_FATAL_FAILURE(goto_path("/v_sparse_padded"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&cursor, 2));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &i32));
  EXPECT_EQ(333, i32);
  ASSERT_NO_FATAL_FAILURE(goto_path("/v_sparse_padded"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&cursor, 1));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_int32(&cursor, &i32));
  EXPECT_EQ(CODA_ERROR_UNSUPPORTED_PRODUCT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);

  ASSERT_NO_FATAL_FAILURE(goto_path("/v_sparse_prev"));
  std::vector<int32_t> prev_vals(2, 0);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_int32_array(&cursor, prev_vals.data(),
                                         coda_array_ordering_c));
  EXPECT_THAT(prev_vals, ElementsAre(55, 66));
}

TEST_F(CodaCdfTest, VdrValidationErrorsAndTruncations) {
  auto make_zvar = [] {
    CdfVarSpec v;
    v.name = "z";
    v.data_type = CdfDataType::kInt4;
    v.dims = {2};
    v.data = CdfBuilder::EncodeValues<int32_t>({1, 2}, /*big_endian=*/true);
    return v;
  };

  // 1. Wrong record types for rVDR and zVDR.
  {
    CdfVarSpec r = make_zvar();
    r.dims.clear();
    r.record_type_override = 8;
    OpenExpectError("coda_cdf_rvdr_type.cdf", CdfBuilder().AddRVar(r).Build(),
                    CODA_ERROR_PRODUCT, "rVDR record");

    CdfVarSpec z = make_zvar();
    z.record_type_override = 3;
    OpenExpectError("coda_cdf_zvdr_type.cdf", CdfBuilder().AddZVar(z).Build(),
                    CODA_ERROR_PRODUCT, "zVDR record");
  }

  // 2. Bad offsets and header field bounds inside VDR.
  {
    CdfVarSpec z = make_zvar();
    z.vdr_next_override = -1;
    OpenExpectError("coda_cdf_vdr_next_neg.cdf",
                    CdfBuilder().AddZVar(z).Build(), CODA_ERROR_PRODUCT,
                    "VDR record");

    z = make_zvar();
    z.vdr_next_override = 100000;
    OpenExpectError("coda_cdf_vdr_next_oob.cdf",
                    CdfBuilder().AddZVar(z).Build(), CODA_ERROR_PRODUCT,
                    "VDR record");

    z = make_zvar();
    z.vxr_head_override = -1;
    OpenExpectError("coda_cdf_vdr_vxrhead_neg.cdf",
                    CdfBuilder().AddZVar(z).Build(), CODA_ERROR_PRODUCT,
                    "VXR record");

    z = make_zvar();
    z.vxr_head_override = 100000;
    OpenExpectError("coda_cdf_vdr_vxrhead_oob.cdf",
                    CdfBuilder().AddZVar(z).Build(), CODA_ERROR_PRODUCT,
                    "VXR record");

    z = make_zvar();
    z.vxr_tail_override = -1;
    OpenExpectError("coda_cdf_vdr_vxrtail_neg.cdf",
                    CdfBuilder().AddZVar(z).Build(), CODA_ERROR_PRODUCT,
                    "VXR record");

    z = make_zvar();
    z.vxr_tail_override = 100000;
    OpenExpectError("coda_cdf_vdr_vxrtail_oob.cdf",
                    CdfBuilder().AddZVar(z).Build(), CODA_ERROR_PRODUCT,
                    "VXR record");

    z = make_zvar();
    z.num_elems_override = -1;
    OpenExpectError("coda_cdf_vdr_numelems.cdf",
                    CdfBuilder().AddZVar(z).Build(), CODA_ERROR_PRODUCT,
                    "number of elements");

    z = make_zvar();
    z.var_num_override = -1;
    OpenExpectError("coda_cdf_vdr_varnum_neg.cdf",
                    CdfBuilder().AddZVar(z).Build(), CODA_ERROR_PRODUCT,
                    "variable number");

    z = make_zvar();
    z.var_num_override = 5;
    OpenExpectError("coda_cdf_vdr_varnum_mismatch.cdf",
                    CdfBuilder().AddZVar(z).Build(), CODA_ERROR_PRODUCT,
                    "invalid number");

    z = make_zvar();
    z.cpr_offset_override = -2;
    OpenExpectError("coda_cdf_vdr_cpr_neg.cdf", CdfBuilder().AddZVar(z).Build(),
                    CODA_ERROR_PRODUCT, "CPR/SPR record");

    z = make_zvar();
    z.cpr_offset_override = 100000;
    OpenExpectError("coda_cdf_vdr_cpr_oob.cdf", CdfBuilder().AddZVar(z).Build(),
                    CODA_ERROR_PRODUCT, "CPR/SPR record");
  }

  // 3. Data type, num_dims, non-varying max_rec, and chained VDR failure.
  {
    CdfVarSpec z = make_zvar();
    z.raw_data_type_override = static_cast<int32_t>(CdfDataType::kEpoch16);
    OpenExpectError("coda_cdf_vdr_epoch16.cdf", CdfBuilder().AddZVar(z).Build(),
                    CODA_ERROR_UNSUPPORTED_PRODUCT, "EPOCH16");

    z = make_zvar();
    z.raw_data_type_override = 99;
    OpenExpectError("coda_cdf_vdr_bad_dtype.cdf",
                    CdfBuilder().AddZVar(z).Build(),
                    CODA_ERROR_INVALID_ARGUMENT, "invalid CDF data type");

    z = make_zvar();
    z.num_dims_override = -1;
    OpenExpectError("coda_cdf_vdr_numdims_neg.cdf",
                    CdfBuilder().AddZVar(z).Build(), CODA_ERROR_PRODUCT,
                    "invalid number of dimensions");

    z = make_zvar();
    z.num_dims_override = CODA_MAX_NUM_DIMS + 1;
    OpenExpectError("coda_cdf_vdr_numdims_max.cdf",
                    CdfBuilder().AddZVar(z).Build(), CODA_ERROR_PRODUCT,
                    "too many dimensions");

    z = make_zvar();
    z.rec_varys = false;
    z.max_rec_override = 2;
    OpenExpectError("coda_cdf_vdr_nonvary_maxrec.cdf",
                    CdfBuilder().AddZVar(z).Build(), CODA_ERROR_PRODUCT,
                    "non-varying record dimension");

    // Chained 2nd VDR failure (rVDR chain and zVDR chain).
    CdfVarSpec r0;
    r0.name = "r0";
    r0.data_type = CdfDataType::kInt4;
    r0.data = CdfBuilder::EncodeValue<int32_t>(1, true);
    CdfVarSpec r1 = r0;
    r1.name = "r1";
    r1.raw_data_type_override = static_cast<int32_t>(CdfDataType::kEpoch16);
    OpenExpectError("coda_cdf_rvdr_chain_fail.cdf",
                    CdfBuilder().AddRVar(r0).AddRVar(r1).Build(),
                    CODA_ERROR_UNSUPPORTED_PRODUCT);

    CdfVarSpec z0 = make_zvar();
    CdfVarSpec z1 = make_zvar();
    z1.name = "z1";
    z1.raw_data_type_override = static_cast<int32_t>(CdfDataType::kEpoch16);
    OpenExpectError("coda_cdf_zvdr_chain_fail.cdf",
                    CdfBuilder().AddZVar(z0).AddZVar(z1).Build(),
                    CODA_ERROR_UNSUPPORTED_PRODUCT);
  }

  // 4. Truncated zVDR at every read_bytes() offset with patched GDR EOF.
  //    Leave data empty (vxr_head == 0) so cutoffs at 340..348 reach the
  //    num_dims / zdim_sizes / dim_varys reads instead of tripping vxr_head >=
  //    file_size (CODA_ERROR_PRODUCT).
  CdfVarSpec z_trunc;
  z_trunc.name = "z";
  z_trunc.data_type = CdfDataType::kInt4;
  z_trunc.dims = {2};
  const CdfBuildResult res = CdfBuilder().AddZVar(z_trunc).BuildWithOffsets();
  ASSERT_EQ(1u, res.zvar_offsets.size());
  const int64_t vdr_off = res.zvar_offsets[0];
  constexpr size_t kVdrRelCutoffs[] = {8,  12, 20, 24, 28, 36,  44,  48,
                                       64, 68, 72, 80, 84, 340, 344, 348};
  for (const size_t rel : kVdrRelCutoffs) {
    SCOPED_TRACE(rel);
    const size_t new_size = static_cast<size_t>(vdr_off) + rel;
    ASSERT_LT(new_size, res.bytes.size());
    OpenExpectError("coda_cdf_vdr_trunc.cdf",
                    TruncateAndPatchEof(res.bytes, res.gdr_offset, new_size),
                    CODA_ERROR_OUT_OF_BOUNDS_READ);
  }
}

TEST_F(CodaCdfTest, CompressionCprCvvrVxrTreesAndErrors) {
  // 1. Valid gzip CVVR compressed variable, CPR offset 0 with uncompressed VVR,
  //    CPR skipped (-1) with VVR last_rec > max_rec clamping, linked VXR chain,
  //    nested VXR tree, VXR entry offset == 0, CVVR first_rec >= num_records
  //    skip, and CVVR partial_read (last_rec >= num_records).
  {
    CdfBuilder b;

    CdfVarSpec gzip_var;
    gzip_var.name = "gzip_var";
    gzip_var.data_type = CdfDataType::kInt4;
    gzip_var.has_compression = true;
    gzip_var.compress_data = true;
    gzip_var.data = CdfBuilder::EncodeValues<int32_t>({10, 20, 30, 40}, true);
    b.AddZVar(std::move(gzip_var));

    // has_compression = true, cpr_offset = 0, uncompressed VVR payload.
    CdfVarSpec vvr_in_comp;
    vvr_in_comp.name = "vvr_in_comp";
    vvr_in_comp.data_type = CdfDataType::kInt4;
    vvr_in_comp.has_compression = true;
    vvr_in_comp.emit_cpr = false;
    vvr_in_comp.cpr_offset_override = 0;
    vvr_in_comp.compress_data = false;
    vvr_in_comp.data = CdfBuilder::EncodeValues<int32_t>({5, 6}, true);
    b.AddZVar(std::move(vvr_in_comp));

    // has_compression = true, cpr_offset = -1, VVR with last_rec > max_rec.
    CdfVarSpec vvr_clamped;
    vvr_clamped.name = "vvr_clamped";
    vvr_clamped.data_type = CdfDataType::kInt4;
    vvr_clamped.has_compression = true;
    vvr_clamped.emit_cpr = false;
    vvr_clamped.cpr_offset_override = -1;
    vvr_clamped.max_rec_override = 1;
    vvr_clamped.chunks.push_back({
        .first_rec = 0,
        .last_rec = 3,
        .data = CdfBuilder::EncodeValues<int32_t>({100, 200, 300, 400}, true),
        .compressed = false,
    });
    b.AddZVar(std::move(vvr_clamped));

    // Linked VXR chain (max_vxr_entries = 1, nested_vxr_tree = false).
    CdfVarSpec linked_vxr;
    linked_vxr.name = "linked_vxr";
    linked_vxr.data_type = CdfDataType::kInt4;
    linked_vxr.max_vxr_entries = 1;
    linked_vxr.nested_vxr_tree = false;
    for (int32_t r = 0; r < 3; ++r) {
      linked_vxr.chunks.push_back({
          .first_rec = r,
          .last_rec = r,
          .data = CdfBuilder::EncodeValue<int32_t>((r + 1) * 10, true),
      });
    }
    b.AddZVar(std::move(linked_vxr));

    // Nested VXR tree (max_vxr_entries = 2, nested_vxr_tree = true, 5 chunks).
    CdfVarSpec nested_vxr;
    nested_vxr.name = "nested_vxr";
    nested_vxr.data_type = CdfDataType::kInt4;
    nested_vxr.max_vxr_entries = 2;
    nested_vxr.nested_vxr_tree = true;
    for (int32_t r = 0; r < 5; ++r) {
      nested_vxr.chunks.push_back({
          .first_rec = r,
          .last_rec = r,
          .data = CdfBuilder::EncodeValue<int32_t>(r + 100, true),
      });
    }
    b.AddZVar(std::move(nested_vxr));

    // VXR entry with vr_offset_override = 0 alongside a valid entry.
    {
      auto vxr = std::make_shared<CdfVxrNode>();
      vxr->entries.push_back(
          {.first_rec = 0, .last_rec = 0, .vr_offset_override = 0});
      vxr->entries.push_back({
          .first_rec = 0,
          .last_rec = 0,
          .chunk = CdfVarChunkSpec{.first_rec = 0,
                                   .last_rec = 0,
                                   .data = CdfBuilder::EncodeValue<int32_t>(
                                       777, true)},
      });
      CdfVarSpec zero_vr;
      zero_vr.name = "zero_vr";
      zero_vr.data_type = CdfDataType::kInt4;
      zero_vr.vxr_root = vxr;
      b.AddZVar(std::move(zero_vr));
    }

    // CVVR skip when first_rec >= num_records.
    {
      CdfVarSpec cvvr_skip;
      cvvr_skip.name = "cvvr_skip";
      cvvr_skip.data_type = CdfDataType::kInt4;
      cvvr_skip.has_compression = true;
      cvvr_skip.max_rec_override = 0;
      cvvr_skip.chunks.push_back({
          .first_rec = 0,
          .last_rec = 0,
          .data = CdfBuilder::EncodeValue<int32_t>(888, true),
          .compressed = true,
      });
      cvvr_skip.chunks.push_back({
          .first_rec = 1,
          .last_rec = 1,
          .data = CdfBuilder::EncodeValue<int32_t>(999, true),
          .compressed = true,
      });
      b.AddZVar(std::move(cvvr_skip));
    }

    // CVVR partial_read (last_rec >= num_records).
    {
      CdfVarSpec cvvr_partial;
      cvvr_partial.name = "cvvr_partial";
      cvvr_partial.data_type = CdfDataType::kInt4;
      cvvr_partial.has_compression = true;
      cvvr_partial.max_rec_override = 1;
      cvvr_partial.chunks.push_back({
          .first_rec = 0,
          .last_rec = 3,
          .data = CdfBuilder::EncodeValues<int32_t>({11, 22, 33, 44}, true),
          .compressed = true,
      });
      b.AddZVar(std::move(cvvr_partial));
    }

    // Multi-chunk compressed variable with 2 VVR chunks and another with 2
    // CVVR chunks so `variable->data != NULL` reuse is covered in both paths.
    {
      CdfVarSpec vvr_multi_comp;
      vvr_multi_comp.name = "vvr_multi_comp";
      vvr_multi_comp.data_type = CdfDataType::kInt4;
      vvr_multi_comp.has_compression = true;
      vvr_multi_comp.chunks.push_back({
          .first_rec = 0,
          .last_rec = 0,
          .data = CdfBuilder::EncodeValue<int32_t>(12, true),
          .compressed = false,
      });
      vvr_multi_comp.chunks.push_back({
          .first_rec = 1,
          .last_rec = 1,
          .data = CdfBuilder::EncodeValue<int32_t>(34, true),
          .compressed = false,
      });
      b.AddZVar(std::move(vvr_multi_comp));

      CdfVarSpec cvvr_multi;
      cvvr_multi.name = "cvvr_multi";
      cvvr_multi.data_type = CdfDataType::kInt4;
      cvvr_multi.has_compression = true;
      cvvr_multi.chunks.push_back({
          .first_rec = 0,
          .last_rec = 0,
          .data = CdfBuilder::EncodeValue<int32_t>(56, true),
          .compressed = true,
      });
      cvvr_multi.chunks.push_back({
          .first_rec = 1,
          .last_rec = 1,
          .data = CdfBuilder::EncodeValue<int32_t>(78, true),
          .compressed = true,
      });
      b.AddZVar(std::move(cvvr_multi));
    }

    TempFile file("coda_cdf_comp_valid.cdf");
    ASSERT_TRUE(file.WriteBytes(b.Build()));
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));

    auto read_arr = [&](const char* path, long count) {
      std::vector<int32_t> vals(static_cast<size_t>(count), -999);
      coda_cursor cursor;
      EXPECT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
      EXPECT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, path));
      EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_int32_array(
                                  &cursor, vals.data(), coda_array_ordering_c));
      return vals;
    };

    EXPECT_THAT(read_arr("/gzip_var", 4), ElementsAre(10, 20, 30, 40));
    EXPECT_THAT(read_arr("/vvr_in_comp", 2), ElementsAre(5, 6));
    EXPECT_THAT(read_arr("/vvr_clamped", 2), ElementsAre(100, 200));
    EXPECT_THAT(read_arr("/linked_vxr", 3), ElementsAre(10, 20, 30));
    EXPECT_THAT(read_arr("/nested_vxr", 5),
                ElementsAre(100, 101, 102, 103, 104));
    EXPECT_THAT(read_arr("/zero_vr", 1), ElementsAre(777));
    EXPECT_THAT(read_arr("/cvvr_skip", 1), ElementsAre(888));
    EXPECT_THAT(read_arr("/cvvr_partial", 2), ElementsAre(11, 22));
    EXPECT_THAT(read_arr("/vvr_multi_comp", 2), ElementsAre(12, 34));
    EXPECT_THAT(read_arr("/cvvr_multi", 2), ElementsAre(56, 78));
  }

  // 2. CPR validation errors and truncations.
  {
    CdfVarSpec z;
    z.name = "z";
    z.data_type = CdfDataType::kInt4;
    z.has_compression = true;
    z.data = CdfBuilder::EncodeValue<int32_t>(1, true);
    z.cpr_record_type_override = 99;
    OpenExpectError("coda_cdf_cpr_rectype.cdf", CdfBuilder().AddZVar(z).Build(),
                    CODA_ERROR_PRODUCT, "CPR record");

    for (const CdfCompression comp :
         {CdfCompression::kRle, CdfCompression::kHuff,
          CdfCompression::kAhuff}) {
      SCOPED_TRACE(static_cast<int32_t>(comp));
      z = CdfVarSpec{};
      z.name = "z";
      z.data_type = CdfDataType::kInt4;
      z.has_compression = true;
      z.compression = comp;
      z.data = CdfBuilder::EncodeValue<int32_t>(1, true);
      OpenExpectError("coda_cdf_cpr_ctype.cdf", CdfBuilder().AddZVar(z).Build(),
                      CODA_ERROR_UNSUPPORTED_PRODUCT,
                      "Unsupported compression");
    }

    // Leave data empty (vxr_head == 0) so truncation reaches read_CPR()
    // instead of tripping vxr_head >= file_size (CODA_ERROR_PRODUCT).
    z = CdfVarSpec{};
    z.name = "z";
    z.data_type = CdfDataType::kInt4;
    z.has_compression = true;
    const CdfBuildResult cpr_res = CdfBuilder().AddZVar(z).BuildWithOffsets();
    // 0-dim zVDR is 344 bytes; CPR immediately follows it.
    const int64_t cpr_off = cpr_res.zvar_offsets[0] + 344;
    for (const size_t rel : {size_t{8}, size_t{12}}) {
      SCOPED_TRACE(rel);
      OpenExpectError("coda_cdf_cpr_trunc.cdf",
                      TruncateAndPatchEof(cpr_res.bytes, cpr_res.gdr_offset,
                                          static_cast<size_t>(cpr_off) + rel),
                      CODA_ERROR_OUT_OF_BOUNDS_READ);
    }
  }

  // 3. VXR validation errors, chain errors, and truncations.
  {
    auto make_vxr_var = [](std::shared_ptr<CdfVxrNode> root) {
      CdfVarSpec z;
      z.name = "z";
      z.data_type = CdfDataType::kInt4;
      z.vxr_root = std::move(root);
      return z;
    };

    auto bad_type = std::make_shared<CdfVxrNode>();
    bad_type->record_type_override = 99;
    OpenExpectError("coda_cdf_vxr_rectype.cdf",
                    CdfBuilder().AddZVar(make_vxr_var(bad_type)).Build(),
                    CODA_ERROR_PRODUCT, "VXR record");

    auto bad_next_neg = std::make_shared<CdfVxrNode>();
    bad_next_neg->vxr_next_override = -1;
    OpenExpectError("coda_cdf_vxr_next_neg.cdf",
                    CdfBuilder().AddZVar(make_vxr_var(bad_next_neg)).Build(),
                    CODA_ERROR_PRODUCT, "VXR record");

    auto bad_next_oob = std::make_shared<CdfVxrNode>();
    bad_next_oob->vxr_next_override = 100000;
    OpenExpectError("coda_cdf_vxr_next_oob.cdf",
                    CdfBuilder().AddZVar(make_vxr_var(bad_next_oob)).Build(),
                    CODA_ERROR_PRODUCT, "VXR record");

    auto bad_nentries = std::make_shared<CdfVxrNode>();
    bad_nentries->n_entries_override = -1;
    OpenExpectError("coda_cdf_vxr_nentries.cdf",
                    CdfBuilder().AddZVar(make_vxr_var(bad_nentries)).Build(),
                    CODA_ERROR_PRODUCT, "number of entries");

    auto make_single_entry_vxr = [](CdfVxrEntry entry) {
      auto n = std::make_shared<CdfVxrNode>();
      n->entries.push_back(std::move(entry));
      return n;
    };

    OpenExpectError(
        "coda_cdf_vxr_first.cdf",
        CdfBuilder()
            .AddZVar(make_vxr_var(make_single_entry_vxr(
                {.first_rec = -1, .last_rec = 0, .vr_offset_override = 0})))
            .Build(),
        CODA_ERROR_PRODUCT, "first record");

    OpenExpectError(
        "coda_cdf_vxr_last.cdf",
        CdfBuilder()
            .AddZVar(make_vxr_var(make_single_entry_vxr(
                {.first_rec = 0, .last_rec = -1, .vr_offset_override = 0})))
            .Build(),
        CODA_ERROR_PRODUCT, "last record");

    OpenExpectError(
        "coda_cdf_vxr_vroff_neg.cdf",
        CdfBuilder()
            .AddZVar(make_vxr_var(make_single_entry_vxr(
                {.first_rec = 0, .last_rec = 0, .vr_offset_override = -1})))
            .Build(),
        CODA_ERROR_PRODUCT, "VR record");

    OpenExpectError(
        "coda_cdf_vxr_vroff_oob.cdf",
        CdfBuilder()
            .AddZVar(make_vxr_var(make_single_entry_vxr(
                {.first_rec = 0, .last_rec = 0, .vr_offset_override = 100000})))
            .Build(),
        CODA_ERROR_PRODUCT, "VR record");

    // Error in second VXR in linked chain.
    auto chain_head = std::make_shared<CdfVxrNode>();
    chain_head->entries.push_back(
        {.first_rec = 0, .last_rec = 0, .vr_offset_override = 0});
    chain_head->next_vxr = bad_type;
    OpenExpectError("coda_cdf_vxr_chain_err.cdf",
                    CdfBuilder().AddZVar(make_vxr_var(chain_head)).Build(),
                    CODA_ERROR_PRODUCT, "VXR record");

    // Truncated VXR header and entry table (0-dim uncompressed zVDR = 344
    // bytes).
    CdfVarSpec vxr_trunc_var;
    vxr_trunc_var.name = "z";
    vxr_trunc_var.data_type = CdfDataType::kInt4;
    vxr_trunc_var.data = CdfBuilder::EncodeValue<int32_t>(1, true);
    const CdfBuildResult vxr_res =
        CdfBuilder().AddZVar(vxr_trunc_var).BuildWithOffsets();
    const int64_t vxr_off = vxr_res.zvar_offsets[0] + 344;
    for (const size_t rel : {size_t{8}, size_t{12}, size_t{20}, size_t{24},
                             size_t{28}, size_t{32}, size_t{36}}) {
      SCOPED_TRACE(rel);
      OpenExpectError("coda_cdf_vxr_trunc.cdf",
                      TruncateAndPatchEof(vxr_res.bytes, vxr_res.gdr_offset,
                                          static_cast<size_t>(vxr_off) + rel),
                      CODA_ERROR_OUT_OF_BOUNDS_READ);
    }

    // Truncated VR header (VXR has 1 entry = 44 bytes).
    const int64_t vr_off = vxr_off + 44;
    OpenExpectError("coda_cdf_vr_trunc.cdf",
                    TruncateAndPatchEof(vxr_res.bytes, vxr_res.gdr_offset,
                                        static_cast<size_t>(vr_off) + 8),
                    CODA_ERROR_OUT_OF_BOUNDS_READ);
  }

  // 4. VR / VVR / CVVR error branches.
  {
    // Invalid VR record type.
    CdfVarSpec bad_vr;
    bad_vr.name = "z";
    bad_vr.data_type = CdfDataType::kInt4;
    bad_vr.chunks.push_back({.first_rec = 0,
                             .last_rec = 0,
                             .data = CdfBuilder::EncodeValue<int32_t>(1, true),
                             .record_type_override = 99});
    OpenExpectError("coda_cdf_vr_rectype.cdf",
                    CdfBuilder().AddZVar(bad_vr).Build(), CODA_ERROR_PRODUCT,
                    "VVR record");

    // Truncated VVR payload when has_compression = true.
    CdfVarSpec vvr_trunc;
    vvr_trunc.name = "z";
    vvr_trunc.data_type = CdfDataType::kInt4;
    vvr_trunc.has_compression = true;
    vvr_trunc.compress_data = false;
    vvr_trunc.data = CdfBuilder::EncodeValue<int32_t>(1, true);
    const CdfBuildResult vvr_res =
        CdfBuilder().AddZVar(vvr_trunc).BuildWithOffsets();
    // 0-dim zVDR (344) + CPR (28) + VXR (44) = +416, VVR header = 12.
    const int64_t vvr_off = vvr_res.zvar_offsets[0] + 344 + 28 + 44;
    OpenExpectError("coda_cdf_vvr_comp_trunc.cdf",
                    TruncateAndPatchEof(vvr_res.bytes, vvr_res.gdr_offset,
                                        static_cast<size_t>(vvr_off) + 14),
                    CODA_ERROR_OUT_OF_BOUNDS_READ);

    // Unexpected CVVR on uncompressed variable.
    CdfVarSpec cvvr_uncomp;
    cvvr_uncomp.name = "z";
    cvvr_uncomp.data_type = CdfDataType::kInt4;
    cvvr_uncomp.has_compression = false;
    cvvr_uncomp.chunks.push_back({
        .first_rec = 0,
        .last_rec = 0,
        .data = CdfBuilder::EncodeValue<int32_t>(1, true),
        .compressed = true,
    });
    OpenExpectError("coda_cdf_cvvr_uncomp.cdf",
                    CdfBuilder().AddZVar(cvvr_uncomp).Build(),
                    CODA_ERROR_FILE_READ, "unexpected CVVR");

    // CVVR with csize < 20.
    CdfVarSpec cvvr_small;
    cvvr_small.name = "z";
    cvvr_small.data_type = CdfDataType::kInt4;
    cvvr_small.has_compression = true;
    cvvr_small.chunks.push_back({
        .first_rec = 0,
        .last_rec = 0,
        .compressed = true,
        .raw_cvvr_payload = std::vector<uint8_t>(10, 0),
        .csize_override = 10,
    });
    OpenExpectError("coda_cdf_cvvr_small.cdf",
                    CdfBuilder().AddZVar(cvvr_small).Build(),
                    CODA_ERROR_PRODUCT, "invalid compressed data block");

    // CVVR truncations at csize header (+16) and payload (+24 + 4).
    CdfVarSpec cvvr_trunc;
    cvvr_trunc.name = "z";
    cvvr_trunc.data_type = CdfDataType::kInt4;
    cvvr_trunc.has_compression = true;
    cvvr_trunc.compress_data = true;
    cvvr_trunc.data = CdfBuilder::EncodeValues<int32_t>({1, 2, 3, 4}, true);
    const CdfBuildResult cvvr_res =
        CdfBuilder().AddZVar(cvvr_trunc).BuildWithOffsets();
    const int64_t cvvr_off = cvvr_res.zvar_offsets[0] + 344 + 28 + 44;
    OpenExpectError("coda_cdf_cvvr_trunc_csize.cdf",
                    TruncateAndPatchEof(cvvr_res.bytes, cvvr_res.gdr_offset,
                                        static_cast<size_t>(cvvr_off) + 16),
                    CODA_ERROR_OUT_OF_BOUNDS_READ);
    OpenExpectError("coda_cdf_cvvr_trunc_data.cdf",
                    TruncateAndPatchEof(cvvr_res.bytes, cvvr_res.gdr_offset,
                                        static_cast<size_t>(cvvr_off) + 28),
                    CODA_ERROR_OUT_OF_BOUNDS_READ);

    // CVVR with zlib (non-gzip) payload >= 20 bytes -> Z_DATA_ERROR.
    std::vector<uint8_t> varied(64);
    for (size_t i = 0; i < varied.size(); ++i) {
      varied[i] = static_cast<uint8_t>((i * 73U) ^ (i >> 2));
    }
    std::vector<uint8_t> zlib_payload;
    ASSERT_TRUE(CdfBuilder::ZlibCompress(varied, zlib_payload));
    ASSERT_GE(zlib_payload.size(), 20u);

    CdfVarSpec cvvr_zlib;
    cvvr_zlib.name = "z";
    cvvr_zlib.data_type = CdfDataType::kUint1;
    cvvr_zlib.has_compression = true;
    cvvr_zlib.max_rec_override = static_cast<int32_t>(varied.size() - 1);
    cvvr_zlib.chunks.push_back({
        .first_rec = 0,
        .last_rec = static_cast<int32_t>(varied.size() - 1),
        .compressed = true,
        .raw_cvvr_payload = zlib_payload,
    });
    OpenExpectError(
        "coda_cdf_cvvr_zlib.cdf", CdfBuilder().AddZVar(cvvr_zlib).Build(),
        CODA_ERROR_FILE_READ, "invalid or incomplete compressed data");

    // Truncated valid gzip stream >= 20 bytes with partial_read == 0 ->
    // Z_BUF_ERROR default decompression error branch.
    std::vector<uint8_t> incompressible(256);
    uint32_t rng = 0x13572468U;
    for (uint8_t& byte : incompressible) {
      rng = rng * 1664525U + 1013904223U;
      byte = static_cast<uint8_t>((rng >> 24) & 0xFFU);
    }
    std::vector<uint8_t> gzip_payload;
    ASSERT_TRUE(CdfBuilder::GzipCompress(incompressible, gzip_payload));
    ASSERT_GT(gzip_payload.size(), 40u);
    gzip_payload.resize(24);

    CdfVarSpec cvvr_incomp;
    cvvr_incomp.name = "z";
    cvvr_incomp.data_type = CdfDataType::kUint1;
    cvvr_incomp.has_compression = true;
    cvvr_incomp.max_rec_override =
        static_cast<int32_t>(incompressible.size() - 1);
    cvvr_incomp.chunks.push_back({
        .first_rec = 0,
        .last_rec = static_cast<int32_t>(incompressible.size() - 1),
        .compressed = true,
        .raw_cvvr_payload = gzip_payload,
    });
    OpenExpectError("coda_cdf_cvvr_incomplete_gzip.cdf",
                    CdfBuilder().AddZVar(cvvr_incomp).Build(),
                    CODA_ERROR_FILE_READ);
  }
}

TEST_F(CodaCdfTest, AttributesAllTypesScopesAndErrors) {
  // 1. Valid global & variable attributes across all data types in both
  //    big-endian and little-endian encodings.
  for (const CdfEncoding enc : {CdfEncoding::kNetwork, CdfEncoding::kIbmPc}) {
    SCOPED_TRACE(static_cast<int32_t>(enc));
    const bool be = IsBigEndianCdfEncoding(static_cast<int32_t>(enc));

    CdfBuilder b;
    b.SetEncoding(enc);

    CdfVarSpec rvar;
    rvar.name = "rvar";
    rvar.data_type = CdfDataType::kInt4;
    rvar.data = CdfBuilder::EncodeValue<int32_t>(10, be);
    b.AddRVar(rvar);

    CdfVarSpec zvar;
    zvar.name = "zvar";
    zvar.data_type = CdfDataType::kInt4;
    zvar.data = CdfBuilder::EncodeValue<int32_t>(20, be);
    b.AddZVar(zvar);

    auto add_global = [&](std::string name, CdfAttrEntrySpec entry,
                          CdfAttrScope scope = CdfAttrScope::kGlobal,
                          bool space_pad = false) {
      CdfAttrSpec a;
      a.name = std::move(name);
      a.scope = scope;
      a.space_pad_name = space_pad;
      a.entries.push_back(std::move(entry));
      b.AddAttr(std::move(a));
    };

    add_global("a_i1",
               CdfAttrEntrySpec::Scalar<int8_t>(0, CdfDataType::kInt1, -7, be),
               CdfAttrScope::kGlobal, /*space_pad=*/true);
    add_global("a_byte",
               CdfAttrEntrySpec::Scalar<int8_t>(0, CdfDataType::kByte, 19, be),
               CdfAttrScope::kGlobalAssumed);
    add_global("a_u1", CdfAttrEntrySpec::Scalar<uint8_t>(0, CdfDataType::kUint1,
                                                         250U, be));
    add_global("a_i2", CdfAttrEntrySpec::Scalar<int16_t>(0, CdfDataType::kInt2,
                                                         -321, be));
    add_global("a_u2", CdfAttrEntrySpec::Scalar<uint16_t>(
                           0, CdfDataType::kUint2, 65000U, be));
    add_global("a_i4", CdfAttrEntrySpec::Scalar<int32_t>(0, CdfDataType::kInt4,
                                                         -987654, be));
    add_global("a_u4", CdfAttrEntrySpec::Scalar<uint32_t>(
                           0, CdfDataType::kUint4, 4000000000U, be));
    add_global("a_i8", CdfAttrEntrySpec::Scalar<int64_t>(0, CdfDataType::kInt8,
                                                         -5000000000LL, be));
    add_global("a_tt2000",
               CdfAttrEntrySpec::Scalar<int64_t>(0, CdfDataType::kTimeTt2000,
                                                 123456789012LL, be));
    add_global("a_r4", CdfAttrEntrySpec::Scalar<float>(0, CdfDataType::kReal4,
                                                       1.5f, be));
    add_global("a_fl", CdfAttrEntrySpec::Scalar<float>(0, CdfDataType::kFloat,
                                                       -2.25f, be));
    add_global("a_r8", CdfAttrEntrySpec::Scalar<double>(0, CdfDataType::kReal8,
                                                        3.125, be));
    add_global("a_epoch", CdfAttrEntrySpec::Scalar<double>(
                              0, CdfDataType::kEpoch, 63113904000000.0, be));
    add_global("a_db", CdfAttrEntrySpec::Scalar<double>(0, CdfDataType::kDouble,
                                                        -7.5, be));
    add_global("a_char", CdfAttrEntrySpec::String(0, "global_text"));
    {
      CdfAttrEntrySpec uchar_entry = CdfAttrEntrySpec::String(0, "uchar_text");
      uchar_entry.data_type = CdfDataType::kUchar;
      add_global("a_uchar", std::move(uchar_entry));
    }
    add_global("a_empty_str", CdfAttrEntrySpec::String(0, ""));
    // Space-only name exercises rtrim() terminating at length == 0.
    add_global(" ", CdfAttrEntrySpec::String(0, "ws"));

    // ADR with 0 entries (agredr_head == 0).
    b.AddAttr({.name = "a_no_entries", .scope = CdfAttrScope::kGlobal});

    // Variable attributes: AgrEDR for rVar (index 0) and AzEDR for zVar (1).
    {
      CdfAttrSpec var_attr;
      var_attr.name = "units";
      var_attr.scope = CdfAttrScope::kVariable;
      var_attr.entries.push_back(
          CdfAttrEntrySpec::String(0, "m/s", /*is_z_entry=*/false));
      b.AddAttr(std::move(var_attr));

      CdfAttrSpec zvar_attr;
      zvar_attr.name = "scale";
      zvar_attr.scope = CdfAttrScope::kVariableAssumed;
      zvar_attr.entries.push_back(CdfAttrEntrySpec::Scalar<double>(
          1, CdfDataType::kDouble, 0.5, be, /*is_z_entry=*/true));
      b.AddAttr(std::move(zvar_attr));
    }

    TempFile file("coda_cdf_attrs.cdf");
    ASSERT_TRUE(file.WriteBytes(b.Build()));
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(file.c_str(), product.OutParam()));

    coda_cursor cursor;
    auto goto_path = [&](const char* path) {
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, path));
    };

    int8_t i8 = 0;
    ASSERT_NO_FATAL_FAILURE(goto_path("@a_i1"));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int8(&cursor, &i8));
    EXPECT_EQ(-7, i8);
    ASSERT_NO_FATAL_FAILURE(goto_path("@a_byte"));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int8(&cursor, &i8));
    EXPECT_EQ(19, i8);

    uint8_t u8 = 0;
    ASSERT_NO_FATAL_FAILURE(goto_path("@a_u1"));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint8(&cursor, &u8));
    EXPECT_EQ(250U, u8);

    int16_t i16 = 0;
    ASSERT_NO_FATAL_FAILURE(goto_path("@a_i2"));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&cursor, &i16));
    EXPECT_EQ(-321, i16);

    uint16_t u16 = 0;
    ASSERT_NO_FATAL_FAILURE(goto_path("@a_u2"));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint16(&cursor, &u16));
    EXPECT_EQ(65000U, u16);

    int32_t i32 = 0;
    ASSERT_NO_FATAL_FAILURE(goto_path("@a_i4"));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &i32));
    EXPECT_EQ(-987654, i32);

    uint32_t u32 = 0;
    ASSERT_NO_FATAL_FAILURE(goto_path("@a_u4"));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint32(&cursor, &u32));
    EXPECT_EQ(4000000000U, u32);

    int64_t i64 = 0;
    ASSERT_NO_FATAL_FAILURE(goto_path("@a_i8"));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int64(&cursor, &i64));
    EXPECT_EQ(-5000000000LL, i64);
    ASSERT_NO_FATAL_FAILURE(goto_path("@a_tt2000"));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int64(&cursor, &i64));
    EXPECT_EQ(123456789012LL, i64);

    float f32 = 0.0f;
    ASSERT_NO_FATAL_FAILURE(goto_path("@a_r4"));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&cursor, &f32));
    EXPECT_FLOAT_EQ(1.5f, f32);
    ASSERT_NO_FATAL_FAILURE(goto_path("@a_fl"));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&cursor, &f32));
    EXPECT_FLOAT_EQ(-2.25f, f32);

    double f64 = 0.0;
    ASSERT_NO_FATAL_FAILURE(goto_path("@a_r8"));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &f64));
    EXPECT_DOUBLE_EQ(3.125, f64);
    ASSERT_NO_FATAL_FAILURE(goto_path("@a_epoch"));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &f64));
    EXPECT_DOUBLE_EQ(63113904000000.0, f64);
    ASSERT_NO_FATAL_FAILURE(goto_path("@a_db"));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &f64));
    EXPECT_DOUBLE_EQ(-7.5, f64);

    char buf[32] = {};
    ASSERT_NO_FATAL_FAILURE(goto_path("@a_char"));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&cursor, buf, 32));
    EXPECT_STREQ("global_text", buf);
    ASSERT_NO_FATAL_FAILURE(goto_path("@a_uchar"));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&cursor, buf, 32));
    EXPECT_STREQ("uchar_text", buf);
    ASSERT_NO_FATAL_FAILURE(goto_path("@a_empty_str"));
    long str_len = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&cursor, &str_len));
    EXPECT_EQ(0, str_len);
    ASSERT_NO_FATAL_FAILURE(goto_path("@unnamed"));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&cursor, buf, 32));
    EXPECT_STREQ("ws", buf);

    ASSERT_NO_FATAL_FAILURE(goto_path("/rvar@units"));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&cursor, buf, 32));
    EXPECT_STREQ("m/s", buf);

    ASSERT_NO_FATAL_FAILURE(goto_path("/zvar@scale"));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &f64));
    EXPECT_DOUBLE_EQ(0.5, f64);
  }

  // Multi-element non-CHAR/UCHAR attribute (num_elems >= 2):
  // TODO(schwehr): coda-cdf.c:319-340 allocates `array` and elements
  // 0..num_elems-2 without assigning `*attribute = (coda_dynamic_type *)array;`
  // at line 340, leaking `array` + earlier elements and leaving `*attribute`
  // pointing to the final scalar element.
  for (const bool be : {true, false}) {
    SCOPED_TRACE(be);
    CdfAttrSpec arr_attr;
    arr_attr.name = "a_arr";
    arr_attr.scope = CdfAttrScope::kGlobal;
    arr_attr.entries.push_back(CdfAttrEntrySpec::Array<int32_t>(
        0, CdfDataType::kInt4, {10, 20, 30}, be));
    TempFile arr_file("coda_cdf_attr_multi_elem.cdf");
    ASSERT_TRUE(arr_file.WriteBytes(
        CdfBuilder()
            .SetEncoding(be ? CdfEncoding::kNetwork : CdfEncoding::kIbmPc)
            .AddAttr(std::move(arr_attr))
            .Build()));
    ScopedProduct arr_product;
    {
      const absl::LeakCheckDisabler leak_disabler;
      ASSERT_EQ(CODA_SUCCESS,
                coda_open(arr_file.c_str(), arr_product.OutParam()));
    }

    coda_cursor arr_cursor;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_set_product(&arr_cursor, arr_product.get()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&arr_cursor, "@a_arr"));
    int32_t last_val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&arr_cursor, &last_val));
    EXPECT_EQ(30, last_val);

    std::vector<int32_t> arr_buf(3, -999);
    coda_set_error(CODA_SUCCESS, nullptr);
    const absl::Cleanup clear_err = [] {
      coda_set_error(CODA_SUCCESS, nullptr);
    };
    EXPECT_EQ(-1, coda_cursor_read_int32_array(&arr_cursor, arr_buf.data(),
                                               coda_array_ordering_c));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  }

  // 2. ADR validation errors, chained ADR failure, and truncations.
  {
    CdfAttrSpec a;
    a.name = "bad";
    a.record_type_override = 99;
    OpenExpectError("coda_cdf_adr_rectype.cdf", CdfBuilder().AddAttr(a).Build(),
                    CODA_ERROR_PRODUCT, "ADR record");

    a = {.name = "bad", .adr_next_override = -1};
    OpenExpectError("coda_cdf_adr_next_neg.cdf",
                    CdfBuilder().AddAttr(a).Build(), CODA_ERROR_PRODUCT,
                    "ADR record");

    a = {.name = "bad", .adr_next_override = 100000};
    OpenExpectError("coda_cdf_adr_next_oob.cdf",
                    CdfBuilder().AddAttr(a).Build(), CODA_ERROR_PRODUCT,
                    "ADR record");

    a = {.name = "bad", .agredr_head_override = -1};
    OpenExpectError("coda_cdf_adr_agredr_neg.cdf",
                    CdfBuilder().AddAttr(a).Build(), CODA_ERROR_PRODUCT,
                    "AgrEDR record");

    a = {.name = "bad", .agredr_head_override = 100000};
    OpenExpectError("coda_cdf_adr_agredr_oob.cdf",
                    CdfBuilder().AddAttr(a).Build(), CODA_ERROR_PRODUCT,
                    "AgrEDR record");

    a = {.name = "bad", .attr_num_override = -1};
    OpenExpectError("coda_cdf_adr_attrnum.cdf", CdfBuilder().AddAttr(a).Build(),
                    CODA_ERROR_PRODUCT, "attribute number");

    a = {.name = "bad", .ngr_entries_override = -1};
    OpenExpectError("coda_cdf_adr_ngr.cdf", CdfBuilder().AddAttr(a).Build(),
                    CODA_ERROR_PRODUCT, "g/rEntries");

    a = {.name = "bad", .azedr_head_override = -1};
    OpenExpectError("coda_cdf_adr_azedr_neg.cdf",
                    CdfBuilder().AddAttr(a).Build(), CODA_ERROR_PRODUCT,
                    "AzEDR record");

    a = {.name = "bad", .azedr_head_override = 100000};
    OpenExpectError("coda_cdf_adr_azedr_oob.cdf",
                    CdfBuilder().AddAttr(a).Build(), CODA_ERROR_PRODUCT,
                    "AzEDR record");

    a = {.name = "bad", .nz_entries_override = -1};
    OpenExpectError("coda_cdf_adr_nz.cdf", CdfBuilder().AddAttr(a).Build(),
                    CODA_ERROR_PRODUCT, "zEntries");

    a = {.name = "bad",
         .scope = CdfAttrScope::kGlobal,
         .nz_entries_override = 1};
    OpenExpectError("coda_cdf_adr_gadr_nz.cdf", CdfBuilder().AddAttr(a).Build(),
                    CODA_ERROR_PRODUCT, "non-zero NzEntries");

    // 2nd ADR in chain fails.
    CdfAttrSpec good_adr = {.name = "good", .scope = CdfAttrScope::kGlobal};
    CdfAttrSpec bad_adr = {.name = "bad2", .record_type_override = 99};
    OpenExpectError("coda_cdf_adr_chain_fail.cdf",
                    CdfBuilder().AddAttr(good_adr).AddAttr(bad_adr).Build(),
                    CODA_ERROR_PRODUCT, "ADR record");

    // Truncated ADR at every read_bytes() offset.
    CdfAttrSpec trunc_adr = {.name = "a", .scope = CdfAttrScope::kGlobal};
    trunc_adr.entries.push_back(CdfAttrEntrySpec::String(0, "hello"));
    const CdfBuildResult adr_res =
        CdfBuilder().AddAttr(trunc_adr).BuildWithOffsets();
    const int64_t adr_off = adr_res.adr_offsets[0];
    constexpr size_t kAdrRelCutoffs[] = {8,  12, 20, 28, 32, 36,
                                         40, 48, 56, 60, 68};
    for (const size_t rel : kAdrRelCutoffs) {
      SCOPED_TRACE(rel);
      OpenExpectError("coda_cdf_adr_trunc.cdf",
                      TruncateAndPatchEof(adr_res.bytes, adr_res.gdr_offset,
                                          static_cast<size_t>(adr_off) + rel),
                      CODA_ERROR_OUT_OF_BOUNDS_READ);
    }

    // Truncated AEDR header and text payload (ADR size = 324 bytes).
    const int64_t aedr_off = adr_off + 324;
    for (const size_t rel : {size_t{8}, size_t{12}, size_t{20}, size_t{24},
                             size_t{28}, size_t{32}, size_t{58}}) {
      SCOPED_TRACE(rel);
      OpenExpectError("coda_cdf_aedr_trunc_text.cdf",
                      TruncateAndPatchEof(adr_res.bytes, adr_res.gdr_offset,
                                          static_cast<size_t>(aedr_off) + rel),
                      CODA_ERROR_OUT_OF_BOUNDS_READ);
    }
  }

  // 3. AEDR validation errors, chained AEDR failure, and numeric truncations.
  {
    auto build_with_entry = [](CdfAttrEntrySpec e,
                               CdfAttrScope scope = CdfAttrScope::kGlobal) {
      CdfAttrSpec a;
      a.name = "attr";
      a.scope = scope;
      a.entries.push_back(std::move(e));
      return CdfBuilder().AddAttr(std::move(a)).Build();
    };

    CdfAttrEntrySpec e = CdfAttrEntrySpec::String(0, "x");
    e.record_type_override = 99;
    OpenExpectError("coda_cdf_aedr_rectype.cdf", build_with_entry(e),
                    CODA_ERROR_PRODUCT, "AEDR record");

    e = CdfAttrEntrySpec::String(0, "x");
    e.aedr_next_override = -1;
    OpenExpectError("coda_cdf_aedr_next_neg.cdf", build_with_entry(e),
                    CODA_ERROR_PRODUCT, "AEDR record");

    e = CdfAttrEntrySpec::String(0, "x");
    e.aedr_next_override = 100000;
    OpenExpectError("coda_cdf_aedr_next_oob.cdf", build_with_entry(e),
                    CODA_ERROR_PRODUCT, "AEDR record");

    e = CdfAttrEntrySpec::String(0, "x");
    e.attr_num_override = -1;
    OpenExpectError("coda_cdf_aedr_attrnum.cdf", build_with_entry(e),
                    CODA_ERROR_PRODUCT, "attribute number in AEDR");

    e = CdfAttrEntrySpec::String(0, "x");
    e.entry_num_override = -1;
    OpenExpectError("coda_cdf_aedr_entrynum.cdf", build_with_entry(e),
                    CODA_ERROR_PRODUCT, "entry number in AEDR");

    e = CdfAttrEntrySpec::String(0, "x");
    e.num_elems_override = -1;
    OpenExpectError("coda_cdf_aedr_numelems.cdf", build_with_entry(e),
                    CODA_ERROR_PRODUCT, "number of entries in AEDR");

    e = CdfAttrEntrySpec::String(0, "x");
    e.raw_data_type_override = static_cast<int32_t>(CdfDataType::kEpoch16);
    OpenExpectError("coda_cdf_aedr_epoch16.cdf", build_with_entry(e),
                    CODA_ERROR_UNSUPPORTED_PRODUCT, "EPOCH16");

    e = CdfAttrEntrySpec::String(0, "x");
    e.raw_data_type_override = 99;
    OpenExpectError("coda_cdf_aedr_bad_dtype.cdf", build_with_entry(e),
                    CODA_ERROR_INVALID_ARGUMENT, "invalid CDF data type");

    // Chained 2nd AEDR failure.
    {
      CdfAttrSpec a;
      a.name = "attr";
      a.scope = CdfAttrScope::kGlobal;
      a.entries.push_back(CdfAttrEntrySpec::String(0, "ok"));
      CdfAttrEntrySpec bad = CdfAttrEntrySpec::String(0, "bad");
      bad.record_type_override = 99;
      a.entries.push_back(std::move(bad));
      OpenExpectError("coda_cdf_aedr_chain_fail.cdf",
                      CdfBuilder().AddAttr(std::move(a)).Build(),
                      CODA_ERROR_PRODUCT, "AEDR record");
    }

    // Variable attribute entry number out of range of available variables.
    {
      CdfAttrSpec a;
      a.name = "vattr";
      a.scope = CdfAttrScope::kVariable;
      a.entries.push_back(CdfAttrEntrySpec::String(2, "oob", true));
      TempFile file("coda_cdf_aedr_var_oob.cdf");
      ASSERT_TRUE(file.WriteBytes(CdfBuilder().AddAttr(std::move(a)).Build()));
      ScopedProduct product;
      coda_set_error(CODA_SUCCESS, nullptr);
      const absl::Cleanup clear_err = [] {
        coda_set_error(CODA_SUCCESS, nullptr);
      };
      int rc = 0;
      {
        // TODO(schwehr): coda-cdf.c:451-457 returns -1 without deleting
        // `attribute` when variable attribute entry `num` is out of range.
        const absl::LeakCheckDisabler leak_disabler;
        rc = coda_open(file.c_str(), product.OutParam());
      }
      EXPECT_EQ(-1, rc);
      EXPECT_EQ(nullptr, product.get());
      EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);
      EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr("outside range"));
    }

    // Truncated scalar numeric and multi-element numeric array AEDR payloads.
    {
      CdfAttrSpec scalar_attr;
      scalar_attr.name = "s";
      scalar_attr.entries.push_back(
          CdfAttrEntrySpec::Scalar<int32_t>(0, CdfDataType::kInt4, 42, true));
      const CdfBuildResult s_res =
          CdfBuilder().AddAttr(std::move(scalar_attr)).BuildWithOffsets();
      const int64_t s_aedr = s_res.adr_offsets[0] + 324;
      OpenExpectError("coda_cdf_aedr_trunc_scalar.cdf",
                      TruncateAndPatchEof(s_res.bytes, s_res.gdr_offset,
                                          static_cast<size_t>(s_aedr) + 58),
                      CODA_ERROR_OUT_OF_BOUNDS_READ);

      CdfAttrSpec arr_attr;
      arr_attr.name = "arr";
      arr_attr.entries.push_back(CdfAttrEntrySpec::Array<int32_t>(
          0, CdfDataType::kInt4, {10, 20}, true));
      const CdfBuildResult arr_res =
          CdfBuilder().AddAttr(std::move(arr_attr)).BuildWithOffsets();
      const int64_t arr_aedr = arr_res.adr_offsets[0] + 324;
      // Truncate after first 4-byte element so read_attribute fails on i == 1.
      OpenExpectError(
          "coda_cdf_aedr_trunc_arr.cdf",
          TruncateAndPatchEof(arr_res.bytes, arr_res.gdr_offset,
                              static_cast<size_t>(arr_aedr) + 56 + 4),
          CODA_ERROR_OUT_OF_BOUNDS_READ);
    }
  }
}

// TODO(schwehr): Remaining uncovered lines in libcoda/coda-cdf.c are OOM
// checks, assert(0) defaults, WORDS_BIGENDIAN paths, and rVDR rnum_dims > 0.

}  // namespace
}  // namespace stcorp_coda
