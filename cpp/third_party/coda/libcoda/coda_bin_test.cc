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
// Tests CODA binary product file open, reopen, mmap/pread I/O, close, and
// binary cursor read paths (libcoda/coda-bin.c, libcoda/coda-bin-cursor.c).
//
// google3 build caveats:
//   - Built for Linux (WIN32 CreateFile/MapViewOfFile branches in coda-bin.c
//     are compiled out).
//   - Internal OOM branches (malloc/strdup/coda_type_raw_file_singleton ==
//   NULL)
//     and the dead error check on coda_bin_product_close() in coda_bin_close()
//     (coda_bin_product_close() unconditionally returns 0) are not reachable
//     without fault injection.
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-bin.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-bin-internal.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-bin.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda.h

#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"
#include "third_party/stcorp_coda/google/util/zip_builder.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-bin-internal.h"
#include "third_party/stcorp_coda/libcoda/coda-definition.h"
}  // extern "C"

#include "third_party/stcorp_coda/libcoda/coda.h"

namespace stcorp_coda {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

constexpr size_t kLargeBlobBytes = 32768;

constexpr std::string_view kIndexXml = R"(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="BinTestClass">
  <cd:ProductType name="SimpleBin">
    <cd:ProductDefinition id="SimpleBinProd" format="binary" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">BINMAGIC</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="EmptyBin">
    <cd:ProductDefinition id="EmptyBinProd" format="binary" version="1">
      <cd:DetectionRule>
        <cd:MatchSize size="0"/>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
</cd:ProductClass>
)";

constexpr std::string_view kSimpleBinProductXml = R"(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="SimpleBinProd" format="binary">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>8</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="i8">
      <cd:Integer>
        <cd:NativeType>int8</cd:NativeType>
        <cd:BitSize>8</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="u8">
      <cd:Integer>
        <cd:NativeType>uint8</cd:NativeType>
        <cd:BitSize>8</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="i16_be">
      <cd:Integer>
        <cd:NativeType>int16</cd:NativeType>
        <cd:BitSize>16</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="u16_le">
      <cd:Integer>
        <cd:NativeType>uint16</cd:NativeType>
        <cd:BitSize>16</cd:BitSize>
        <cd:LittleEndian/>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="i32_be">
      <cd:Integer>
        <cd:NativeType>int32</cd:NativeType>
        <cd:BitSize>32</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="u32_le">
      <cd:Integer>
        <cd:NativeType>uint32</cd:NativeType>
        <cd:BitSize>32</cd:BitSize>
        <cd:LittleEndian/>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="i64_be">
      <cd:Integer>
        <cd:NativeType>int64</cd:NativeType>
        <cd:BitSize>64</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="u64_le">
      <cd:Integer>
        <cd:NativeType>uint64</cd:NativeType>
        <cd:BitSize>64</cd:BitSize>
        <cd:LittleEndian/>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="f32_le">
      <cd:Float>
        <cd:NativeType>float</cd:NativeType>
        <cd:BitSize>32</cd:BitSize>
        <cd:LittleEndian/>
      </cd:Float>
    </cd:Field>
    <cd:Field name="f64_be">
      <cd:Float>
        <cd:NativeType>double</cd:NativeType>
        <cd:BitSize>64</cd:BitSize>
      </cd:Float>
    </cd:Field>
    <cd:Field name="ch">
      <cd:Text>
        <cd:ByteSize>1</cd:ByteSize>
        <cd:NativeType>char</cd:NativeType>
      </cd:Text>
    </cd:Field>
    <cd:Field name="s5">
      <cd:Integer>
        <cd:NativeType>int8</cd:NativeType>
        <cd:BitSize>5</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="u11">
      <cd:Integer>
        <cd:NativeType>uint16</cd:NativeType>
        <cd:BitSize>11</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="pad3">
      <cd:Raw>
        <cd:BitSize>3</cd:BitSize>
      </cd:Raw>
    </cd:Field>
    <cd:Field name="raw37">
      <cd:Raw>
        <cd:BitSize>37</cd:BitSize>
      </cd:Raw>
    </cd:Field>
    <cd:Field name="arr_i32">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Dimension>3</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>int32</cd:NativeType>
          <cd:BitSize>32</cd:BitSize>
          <cd:LittleEndian/>
        </cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="large_blob">
      <cd:Raw>
        <cd:BitSize>262144</cd:BitSize>
      </cd:Raw>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)";

constexpr std::string_view kEmptyBinProductXml = R"(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="EmptyBinProd" format="binary">
  <cd:Record/>
</cd:ProductDefinition>
)";

// RAII wrapper for products opened directly via coda_bin_open().
class ScopedBinProduct {
 public:
  ScopedBinProduct() = default;
  ~ScopedBinProduct() { Reset(); }

  ScopedBinProduct(const ScopedBinProduct&) = delete;
  ScopedBinProduct& operator=(const ScopedBinProduct&) = delete;

  void Reset(coda_product* product = nullptr) {
    if (product_ != nullptr) {
      coda_bin_close(product_);
    }
    product_ = product;
  }

  coda_product** OutParam() {
    Reset();
    return &product_;
  }

  coda_product* get() const { return product_; }
  coda_bin_product* bin_get() const {
    return reinterpret_cast<coda_bin_product*>(product_);
  }

 private:
  coda_product* product_ = nullptr;
};

void AppendBe16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(v & 0xFF));
}

void AppendLe16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void AppendBe32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(v & 0xFF));
}

void AppendLe32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void AppendBe64(std::vector<uint8_t>& out, uint64_t v) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
  }
}

void AppendLe64(std::vector<uint8_t>& out, uint64_t v) {
  for (int shift = 0; shift <= 56; shift += 8) {
    out.push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
  }
}

std::vector<uint8_t> BuildTestCodadefBytes() {
  const std::array<std::pair<std::string_view, std::string_view>, 2> kEntries =
      {{
          {"products/SimpleBinProd.xml", kSimpleBinProductXml},
          {"products/EmptyBinProd.xml", kEmptyBinProductXml},
      }};
  return BuildCodadefBytes(kIndexXml, kEntries);
}

uint8_t LargeBlobByte(size_t index) {
  return static_cast<uint8_t>((index * 37 + 13) & 0xFF);
}

// Builds a binary payload matching SimpleBinProd, including a 32 KiB tail
// exceeding OS page size (4 KiB) and CODA detection/read buffers.
std::vector<uint8_t> BuildSimpleBinProductBytes() {
  std::vector<uint8_t> out;
  out.reserve(82 + kLargeBlobBytes);

  // magic (8 bytes): "BINMAGIC"
  constexpr std::string_view kMagic = "BINMAGIC";
  out.insert(out.end(), kMagic.begin(), kMagic.end());
  // i8 = -42, u8 = 200
  out.push_back(static_cast<uint8_t>(int8_t{-42}));
  out.push_back(200);
  // i16_be = -12345, u16_le = 0xBEEF
  AppendBe16(out, static_cast<uint16_t>(int16_t{-12345}));
  AppendLe16(out, 0xBEEF);
  // i32_be = -0x12345678, u32_le = 0xCAFEBABE
  AppendBe32(out, static_cast<uint32_t>(int32_t{-0x12345678}));
  AppendLe32(out, 0xCAFEBABEU);
  // i64_be = -0x0102030405060708, u64_le = 0x1122334455667788
  AppendBe64(out, static_cast<uint64_t>(int64_t{-0x0102030405060708LL}));
  AppendLe64(out, 0x1122334455667788ULL);
  // f32_le = -12.5f, f64_be = 3.25
  AppendLe32(out, std::bit_cast<uint32_t>(-12.5f));
  AppendBe64(out, std::bit_cast<uint64_t>(3.25));
  // ch = 'Z'
  out.push_back('Z');
  // s5 = -7 (0b11001) + u11 = 0x5A3 (0b10110100011) -> 0xCD, 0xA3
  out.push_back(0xCD);
  out.push_back(0xA3);
  // pad3 = 0b101 + raw37 (0x15, 0xA3, 0x7E, 0x49, 0xC2) -> 5 bytes
  out.push_back(0xB5);
  out.push_back(0xA3);
  out.push_back(0x7E);
  out.push_back(0x49);
  out.push_back(0xC2);
  // arr_i32[2][3] = {10, 20, 30, 40, 50, 60}
  for (const int32_t v : {10, 20, 30, 40, 50, 60}) {
    AppendLe32(out, static_cast<uint32_t>(v));
  }
  // large_blob: 32 KiB deterministic pattern
  for (size_t i = 0; i < kLargeBlobBytes; ++i) {
    out.push_back(LargeBlobByte(i));
  }
  return out;
}

TEST(CodaBinTest, DirectBinOpenAndCloseWithMmapAndFdModes) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  const int saved_use_mmap = coda_get_option_use_mmap();
  const absl::Cleanup cleanup_options = [saved_use_mmap] {
    coda_set_option_use_mmap(saved_use_mmap);
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(nullptr));
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  const TempFile data_file("coda_bin_direct.bin");
  const std::vector<uint8_t> payload = BuildSimpleBinProductBytes();
  ASSERT_TRUE(data_file.WriteBytes(payload));

  for (const int use_mmap : {1, 0}) {
    SCOPED_TRACE(use_mmap);
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(use_mmap));

    ScopedBinProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_bin_open(data_file.c_str(),
                                          static_cast<int64_t>(payload.size()),
                                          product.OutParam()));
    ASSERT_NE(nullptr, product.get());

    const coda_bin_product* bin = product.bin_get();
    EXPECT_STREQ(data_file.c_str(), bin->filename);
    EXPECT_EQ(static_cast<int64_t>(payload.size()), bin->file_size);
    EXPECT_EQ(coda_format_binary, bin->format);
    EXPECT_NE(nullptr, bin->root_type);
    EXPECT_EQ(nullptr, bin->product_definition);

    if (use_mmap == 1) {
      EXPECT_EQ(1, bin->use_mmap);
      EXPECT_EQ(-1, bin->fd);
      EXPECT_NE(nullptr, bin->mem_ptr);
      EXPECT_EQ(static_cast<int64_t>(payload.size()), bin->mem_size);
    } else {
      EXPECT_EQ(0, bin->use_mmap);
      EXPECT_GE(bin->fd, 0);
      EXPECT_EQ(nullptr, bin->mem_ptr);
      EXPECT_EQ(0, bin->mem_size);
    }

    // Read across the 32 KiB file directly from the raw binary root cursor.
    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));

    int64_t bit_size = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_bin_cursor_get_bit_size(&cursor, &bit_size));
    EXPECT_EQ(static_cast<int64_t>(payload.size()) * 8, bit_size);

    std::vector<uint8_t> read_back(payload.size(), 0);
    ASSERT_EQ(CODA_SUCCESS, coda_bin_cursor_read_bytes(
                                &cursor, read_back.data(), 0,
                                static_cast<int64_t>(read_back.size())));
    EXPECT_EQ(payload, read_back);

    product.Reset();
  }
}

TEST(CodaBinTest, DirectBinOpenEmptyFileAlwaysUsesFileDescriptor) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  const int saved_use_mmap = coda_get_option_use_mmap();
  const absl::Cleanup cleanup_options = [saved_use_mmap] {
    coda_set_option_use_mmap(saved_use_mmap);
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(nullptr));
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  const TempFile empty_file("coda_bin_empty_direct.bin");
  ASSERT_TRUE(empty_file.WriteBytes({}));

  for (const int use_mmap : {1, 0}) {
    SCOPED_TRACE(use_mmap);
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(use_mmap));

    ScopedBinProduct product;
    ASSERT_EQ(CODA_SUCCESS,
              coda_bin_open(empty_file.c_str(), 0, product.OutParam()));
    ASSERT_NE(nullptr, product.get());

    // file_size == 0 must skip mmap() even when coda_option_use_mmap == 1.
    const coda_bin_product* bin = product.bin_get();
    EXPECT_EQ(0, bin->use_mmap);
    EXPECT_GE(bin->fd, 0);
    EXPECT_EQ(nullptr, bin->mem_ptr);
    EXPECT_EQ(0, bin->mem_size);

    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));

    uint8_t byte_out = 0xAA;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_bin_cursor_read_bytes(&cursor, &byte_out, 0, 1));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
    EXPECT_EQ(0xAA, byte_out);

    // Calling coda_bin_product_close() twice before Reset() closes fd on the
    // first call and is a no-op on fd == -1 on the second call.
    coda_bin_product* raw = product.bin_get();
    ASSERT_EQ(CODA_SUCCESS, coda_bin_product_close(raw));
    EXPECT_EQ(-1, raw->fd);
    EXPECT_EQ(0, raw->use_mmap);
    ASSERT_EQ(CODA_SUCCESS, coda_bin_product_close(raw));

    product.Reset();
  }
}

TEST(CodaBinTest, DirectBinOpenFailurePathsMissingFileAndMmapFailure) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  const int saved_use_mmap = coda_get_option_use_mmap();
  const absl::Cleanup cleanup_options = [saved_use_mmap] {
    coda_set_option_use_mmap(saved_use_mmap);
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(nullptr));
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  const std::filesystem::path missing_path =
      std::filesystem::path(::testing::TempDir()) /
      "coda_bin_does_not_exist.bin";
  std::error_code ec;
  std::filesystem::remove(missing_path, ec);

  // 1. Missing file with use_mmap == 1 and file_size > 0 fails at open() inside
  //    the mmap branch.
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(1));
  {
    ScopedBinProduct product;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_bin_open(missing_path.c_str(), 16, product.OutParam()));
    EXPECT_EQ(CODA_ERROR_FILE_OPEN, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr("could not open"));
    EXPECT_EQ(nullptr, product.get());
  }

  // 2. Opening a directory with use_mmap == 1 and file_size > 0 succeeds in
  //    open(O_RDONLY) on Linux and fails in mmap() with MAP_FAILED, exercising
  //    the mmap failure branch and mem_ptr == NULL cleanup in product_close().
  {
    const TempDir dir("coda_bin_mmap_dir");
    ScopedBinProduct product;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_bin_open(dir.c_str(), 4096, product.OutParam()));
    EXPECT_EQ(CODA_ERROR_FILE_OPEN, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr("could not map"));
    EXPECT_EQ(nullptr, product.get());
  }

  // 3. Missing file with file_size == 0 (takes else branch even when
  //    use_mmap == 1) and with use_mmap == 0 fails at open() in the fd branch.
  {
    ScopedBinProduct product;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_bin_open(missing_path.c_str(), 0, product.OutParam()));
    EXPECT_EQ(CODA_ERROR_FILE_OPEN, coda_errno);
    EXPECT_EQ(nullptr, product.get());
  }
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(0));
  {
    ScopedBinProduct product;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_bin_open(missing_path.c_str(), 16, product.OutParam()));
    EXPECT_EQ(CODA_ERROR_FILE_OPEN, coda_errno);
    EXPECT_EQ(nullptr, product.get());
  }
}

TEST(CodaBinTest, DirectReopenWithDefinitionAppliesRootTypeAndReadsFields) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  const int saved_use_mmap = coda_get_option_use_mmap();
  const absl::Cleanup cleanup_options = [saved_use_mmap] {
    coda_set_option_use_mmap(saved_use_mmap);
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  const TempFile codadef_file("coda_bin_reopen.codadef");
  ASSERT_TRUE(codadef_file.WriteBytes(BuildTestCodadefBytes()));
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef_file.c_str()));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  coda_product_definition* def = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_data_dictionary_get_definition(
                              "BinTestClass", "SimpleBin", 1, &def));
  ASSERT_NE(nullptr, def);
  if (!def->initialized) {
    ASSERT_EQ(CODA_SUCCESS, coda_read_product_definition(def));
  }
  ASSERT_NE(nullptr, def->root_type);

  const TempFile data_file("coda_bin_reopen.bin");
  const std::vector<uint8_t> payload = BuildSimpleBinProductBytes();
  ASSERT_TRUE(data_file.WriteBytes(payload));

  for (const int use_mmap : {1, 0}) {
    SCOPED_TRACE(use_mmap);
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(use_mmap));

    ScopedBinProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_bin_open(data_file.c_str(),
                                          static_cast<int64_t>(payload.size()),
                                          product.OutParam()));

    coda_product* raw_ptr = product.get();
    ASSERT_EQ(CODA_SUCCESS, coda_bin_reopen_with_definition(&raw_ptr, def));
    EXPECT_EQ(def, product.bin_get()->product_definition);
    EXPECT_EQ(reinterpret_cast<coda_dynamic_type*>(def->root_type),
              product.bin_get()->root_type);

    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, raw_ptr));
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&cursor, "u32_le"));
    uint32_t u32_val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint32(&cursor, &u32_val));
    EXPECT_EQ(0xCAFEBABEU, u32_val);

    product.Reset();
  }
}

TEST(CodaBinTest, PublicOpenCloseAndCursorReadsInBothMmapModes) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  const int saved_use_mmap = coda_get_option_use_mmap();
  const absl::Cleanup cleanup_options = [saved_use_mmap] {
    coda_set_option_use_mmap(saved_use_mmap);
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  const TempFile codadef_file("coda_bin_public.codadef");
  ASSERT_TRUE(codadef_file.WriteBytes(BuildTestCodadefBytes()));
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef_file.c_str()));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  const TempFile data_file("coda_bin_public.bin");
  const std::vector<uint8_t> payload = BuildSimpleBinProductBytes();
  ASSERT_TRUE(data_file.WriteBytes(payload));

  for (const int use_mmap : {1, 0}) {
    SCOPED_TRACE(use_mmap);
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(use_mmap));

    // 1. Open via public coda_open() and verify product metadata.
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(data_file.c_str(), product.OutParam()));

    coda_format format = coda_format_ascii;
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_format(product.get(), &format));
    EXPECT_EQ(coda_format_binary, format);

    int64_t file_size = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_get_product_file_size(product.get(), &file_size));
    EXPECT_EQ(static_cast<int64_t>(payload.size()), file_size);

    const char* product_class = nullptr;
    const char* product_type = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_get_product_class(product.get(), &product_class));
    ASSERT_EQ(CODA_SUCCESS,
              coda_get_product_type(product.get(), &product_type));
    EXPECT_STREQ("BinTestClass", product_class);
    EXPECT_STREQ("SimpleBin", product_type);

    // 2. Read byte-aligned scalar types (BE and LE).
    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));

    long num_elements = 0;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_get_num_elements(&cursor, &num_elements));
    EXPECT_EQ(18, num_elements);

    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&cursor, "magic"));
    long magic_len = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&cursor, &magic_len));
    EXPECT_EQ(8, magic_len);
    std::array<char, 9> magic_buf{};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_string(&cursor, magic_buf.data(),
                                      static_cast<long>(magic_buf.size())));
    EXPECT_STREQ("BINMAGIC", magic_buf.data());
    // Also verify buffer truncation preserves NUL termination.
    std::array<char, 4> short_buf{};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_string(&cursor, short_buf.data(),
                                      static_cast<long>(short_buf.size())));
    EXPECT_STREQ("BIN", short_buf.data());

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/i8"));
    int8_t i8_val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int8(&cursor, &i8_val));
    EXPECT_EQ(-42, i8_val);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/u8"));
    uint8_t u8_val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint8(&cursor, &u8_val));
    EXPECT_EQ(200, u8_val);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/i16_be"));
    int16_t i16_val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&cursor, &i16_val));
    EXPECT_EQ(-12345, i16_val);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/u16_le"));
    uint16_t u16_val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint16(&cursor, &u16_val));
    EXPECT_EQ(0xBEEF, u16_val);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/i32_be"));
    int32_t i32_val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &i32_val));
    EXPECT_EQ(-0x12345678, i32_val);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/u32_le"));
    uint32_t u32_val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint32(&cursor, &u32_val));
    EXPECT_EQ(0xCAFEBABEU, u32_val);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/i64_be"));
    int64_t i64_val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int64(&cursor, &i64_val));
    EXPECT_EQ(-0x0102030405060708LL, i64_val);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/u64_le"));
    uint64_t u64_val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint64(&cursor, &u64_val));
    EXPECT_EQ(0x1122334455667788ULL, u64_val);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/f32_le"));
    float f32_val = 0.0f;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&cursor, &f32_val));
    EXPECT_FLOAT_EQ(-12.5f, f32_val);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/f64_be"));
    double f64_val = 0.0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &f64_val));
    EXPECT_DOUBLE_EQ(3.25, f64_val);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/ch"));
    char ch_val = '\0';
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_char(&cursor, &ch_val));
    EXPECT_EQ('Z', ch_val);

    // 3. Read unaligned bitfields (5-bit signed, 11-bit unsigned).
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/s5"));
    int8_t s5_val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int8(&cursor, &s5_val));
    EXPECT_EQ(-7, s5_val);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/u11"));
    uint16_t u11_val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint16(&cursor, &u11_val));
    EXPECT_EQ(0x5A3, u11_val);

    // 4. Exercise coda_bin_cursor_read_bytes on a non-byte-aligned cursor and
    //    coda_bin_cursor_read_bits across >24-bit shifted chunks.
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/raw37"));
    std::array<uint8_t, 4> unaligned_bytes{};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_bytes(
                                &cursor, unaligned_bytes.data(), 0,
                                static_cast<int64_t>(unaligned_bytes.size())));
    EXPECT_THAT(unaligned_bytes, ElementsAre(0xAD, 0x1B, 0xF2, 0x4E));

    std::array<uint8_t, 5> bits37{};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_bits(&cursor, bits37.data(), 0, 37));
    EXPECT_THAT(bits37, ElementsAre(0x15, 0xA3, 0x7E, 0x49, 0xC2));

    std::array<uint8_t, 4> bits29{};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_bits(&cursor, bits29.data(), 1, 29));
    EXPECT_THAT(bits29, ElementsAre(0x0B, 0x46, 0xFC, 0x93));

    // 5. Read 2D array in both C and Fortran orderings plus partial array.
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/arr_i32"));
    std::array<int32_t, 6> arr_c{};
    arr_c.fill(-999);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32_array(
                                &cursor, arr_c.data(), coda_array_ordering_c));
    EXPECT_THAT(arr_c, ElementsAre(10, 20, 30, 40, 50, 60));

    std::array<int32_t, 6> arr_f{};
    arr_f.fill(-999);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_int32_array(&cursor, arr_f.data(),
                                           coda_array_ordering_fortran));
    EXPECT_THAT(arr_f, ElementsAre(10, 40, 20, 50, 30, 60));

    std::array<int32_t, 3> arr_part{};
    arr_part.fill(-999);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32_partial_array(
                                &cursor, 2, 3, arr_part.data()));
    EXPECT_THAT(arr_part, ElementsAre(30, 40, 50));

    // 6. Read multi-page 32 KiB payload exceeding buffers and verify content.
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/large_blob"));
    std::vector<uint8_t> large_out(kLargeBlobBytes, 0);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_bytes(&cursor, large_out.data(), 0,
                                     static_cast<int64_t>(large_out.size())));
    for (size_t i = 0; i < kLargeBlobBytes; ++i) {
      ASSERT_EQ(LargeBlobByte(i), large_out[i]) << "mismatch at index " << i;
    }

    // Verify a sub-range read crossing multiple 4 KiB page boundaries.
    constexpr int64_t kSliceOffset = 4090;
    constexpr int64_t kSliceLen = 8192;
    std::vector<uint8_t> slice_out(static_cast<size_t>(kSliceLen), 0);
    ASSERT_EQ(CODA_SUCCESS,
              coda_bin_cursor_read_bytes(&cursor, slice_out.data(),
                                         kSliceOffset, kSliceLen));
    for (int64_t i = 0; i < kSliceLen; ++i) {
      ASSERT_EQ(LargeBlobByte(static_cast<size_t>(kSliceOffset + i)),
                slice_out[static_cast<size_t>(i)])
          << "slice mismatch at index " << i;
    }

    // 7. Out-of-bounds read_bytes (past EOF and negative offset).
    uint8_t oob = 0;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_bytes(
                      &cursor, &oob, static_cast<int64_t>(kLargeBlobBytes), 1));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_bin_cursor_read_bytes(&cursor, &oob, -100, 1));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);

    product.Reset();
  }
}

TEST(CodaBinTest, PublicOpenHandlesEmptyZeroByteFileAndInvalidInputs) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  const int saved_use_mmap = coda_get_option_use_mmap();
  const absl::Cleanup cleanup_options = [saved_use_mmap] {
    coda_set_option_use_mmap(saved_use_mmap);
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  const TempFile empty_file("coda_bin_empty_public.bin");
  ASSERT_TRUE(empty_file.WriteBytes({}));

  // Without a matching .codadef, an empty file opens internally as raw binary
  // and fails in set_definition() with CODA_ERROR_UNSUPPORTED_PRODUCT.
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(nullptr));
  {
    ScopedCodaSession session;
    ASSERT_EQ(CODA_SUCCESS, session.Init());
    for (const int use_mmap : {1, 0}) {
      SCOPED_TRACE(use_mmap);
      ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(use_mmap));

      ScopedProduct product;
      coda_set_error(CODA_SUCCESS, nullptr);
      EXPECT_EQ(-1, coda_open(empty_file.c_str(), product.OutParam()));
      EXPECT_EQ(CODA_ERROR_UNSUPPORTED_PRODUCT, coda_errno);
      EXPECT_EQ(nullptr, product.get());
    }
  }

  // With the EmptyBin definition loaded, a 0-byte file opens and closes
  // cleanly.
  const TempFile codadef_file("coda_bin_empty_public.codadef");
  ASSERT_TRUE(codadef_file.WriteBytes(BuildTestCodadefBytes()));
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef_file.c_str()));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  for (const int use_mmap : {1, 0}) {
    SCOPED_TRACE(use_mmap);
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(use_mmap));

    // 1. Valid 0-byte product via MatchSize(size="0").
    ScopedProduct empty_product;
    ASSERT_EQ(CODA_SUCCESS,
              coda_open(empty_file.c_str(), empty_product.OutParam()));

    int64_t file_size = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_get_product_file_size(empty_product.get(), &file_size));
    EXPECT_EQ(0, file_size);

    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_set_product(&cursor, empty_product.get()));
    uint8_t byte_out = 0x55;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_bytes(&cursor, &byte_out, 0, 1));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
    EXPECT_EQ(0x55, byte_out);
    empty_product.Reset();

    // 2. Truncated file forced via coda_open_as() fails on out-of-bounds read.
    const TempFile truncated_file("coda_bin_truncated.bin");
    constexpr auto kTruncatedBytes =
        std::to_array<uint8_t>({'B', 'I', 'N', 'M', 'A', 'G', 'I', 'C', 0x01});
    ASSERT_TRUE(truncated_file.WriteBytes(kTruncatedBytes));
    ScopedProduct trunc_product;
    ASSERT_EQ(CODA_SUCCESS,
              coda_open_as(truncated_file.c_str(), "BinTestClass", "SimpleBin",
                           1, trunc_product.OutParam()));
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_set_product(&cursor, trunc_product.get()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/i16_be"));
    int16_t i16_val = 0;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int16(&cursor, &i16_val));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
    trunc_product.Reset();

    // 3. Missing file and directory fail with appropriate CODA error codes.
    const std::filesystem::path missing_path =
        std::filesystem::path(::testing::TempDir()) /
        "coda_bin_missing_public.bin";
    std::error_code ec;
    std::filesystem::remove(missing_path, ec);

    ScopedProduct missing_product;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_open(missing_path.c_str(), missing_product.OutParam()));
    EXPECT_EQ(CODA_ERROR_FILE_NOT_FOUND, coda_errno);

    const TempDir dir("coda_bin_public_dir");
    ScopedProduct dir_product;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_open(dir.c_str(), dir_product.OutParam()));
    EXPECT_EQ(CODA_ERROR_FILE_OPEN, coda_errno);
  }

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_close(nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
}

// TODO(schwehr): Add fault-injection coverage for malloc/strdup OOM paths in
// coda_bin_open() if an allocator test seam is introduced.

}  // namespace
}  // namespace stcorp_coda
