// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Unit tests for libtiff tif_aux.c (checked arithmetic, checked allocations,
// TIFFGetFieldDefaulted / TIFFVGetFieldDefaulted, clamping, and _TIFFSeekOK).
//
// See also:
//   https://gitlab.com/libtiff/libtiff/-/blob/master/libtiff/tif_aux.c

#include <array>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/tiff/google/util/alloc_limit_guard.h"
#include "third_party/tiff/google/util/raw_tiff_builder.h"
#include "third_party/tiff/google/util/scoped_tiff.h"
#include "third_party/tiff/google/util/tiff_memory_stream.h"
#include "third_party/tiff/libtiff/tiffio.h"
#include "third_party/tiff/libtiff/tiffiop.h"

namespace libtiff {
namespace {

using ::testing::HasSubstr;

// RAII wrapper for per-handle allocations returned by _TIFFCheckMalloc and
// _TIFFCheckRealloc (which allocate via _TIFFreallocExt and must be freed
// with _TIFFfreeExt on the same TIFF* before TIFFClose).
class ScopedTiffExtAlloc {
 public:
  explicit ScopedTiffExtAlloc(TIFF* tif) : tif_(tif) {}

  ~ScopedTiffExtAlloc() { Reset(); }

  ScopedTiffExtAlloc(const ScopedTiffExtAlloc&) = delete;
  ScopedTiffExtAlloc& operator=(const ScopedTiffExtAlloc&) = delete;

  bool Malloc(tmsize_t nmemb, tmsize_t elem_size, const char* what) {
    Reset();
    ptr_ = _TIFFCheckMalloc(tif_, nmemb, elem_size, what);
    return ptr_ != nullptr;
  }

  bool Realloc(tmsize_t nmemb, tmsize_t elem_size, const char* what) {
    void* next = _TIFFCheckRealloc(tif_, ptr_, nmemb, elem_size, what);
    if (next == nullptr) {
      // On failure, _TIFFreallocExt leaves the existing allocation untouched.
      return false;
    }
    ptr_ = next;
    return true;
  }

  void Reset() noexcept {
    if (ptr_ != nullptr) {
      _TIFFfreeExt(tif_, ptr_);
      ptr_ = nullptr;
    }
  }

  void* get() const { return ptr_; }
  uint8_t* bytes() const { return static_cast<uint8_t*>(ptr_); }

 private:
  TIFF* tif_;
  void* ptr_ = nullptr;
};

int CallVGetFieldDefaulted(TIFF* tif, uint32_t tag, ...) {
  va_list ap;
  va_start(ap, tag);
  const int result = TIFFVGetFieldDefaulted(tif, tag, ap);
  va_end(ap);
  return result;
}

UniqueTiff OpenWritableWithRecorder(MemoryTiffStream& stream,
                                    DiagnosticRecorder& recorder,
                                    const char* mode = "w",
                                    tmsize_t max_cumulated_mem_alloc = 0) {
  UniqueTiffOptions opts = MakeUniqueTiffOptions();
  if (opts == nullptr) return nullptr;
  recorder.Install(opts.get());
  if (max_cumulated_mem_alloc > 0) {
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(opts.get(), max_cumulated_mem_alloc);
  }
  return stream.Open(mode, opts.get(), "tif_aux_test.tif");
}

std::vector<uint8_t> BuildMinimalReadableTiffBytes(bool big_endian,
                                                   bool big_tiff) {
  RawTiffBuilder builder(big_endian, big_tiff);
  const std::array<uint8_t, 16> pixel_strip{};
  const uint64_t strip_off = builder.AppendBlob(pixel_strip);
  std::vector<RawIfdEntry> entries;
  entries.push_back(builder.MakeLong(TIFFTAG_IMAGEWIDTH, 4));
  entries.push_back(builder.MakeLong(TIFFTAG_IMAGELENGTH, 4));
  if (big_tiff) {
    entries.push_back(builder.MakeLong8(TIFFTAG_STRIPOFFSETS, strip_off));
    entries.push_back(
        builder.MakeLong8(TIFFTAG_STRIPBYTECOUNTS, pixel_strip.size()));
  } else {
    entries.push_back(builder.MakeLong(TIFFTAG_STRIPOFFSETS,
                                       static_cast<uint32_t>(strip_off)));
    entries.push_back(builder.MakeLong(
        TIFFTAG_STRIPBYTECOUNTS, static_cast<uint32_t>(pixel_strip.size())));
  }
  builder.AddIfd(std::move(entries));
  return builder.Take();
}

TEST(TifAuxTest, Multiply32ValidAndOverflow) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  EXPECT_EQ(_TIFFMultiply32(tif.get(), 0U, 0U, "Mul32"), 0U);
  EXPECT_EQ(_TIFFMultiply32(tif.get(), UINT32_MAX, 0U, "Mul32"), 0U);
  EXPECT_EQ(_TIFFMultiply32(tif.get(), 0U, UINT32_MAX, "Mul32"), 0U);
  EXPECT_EQ(_TIFFMultiply32(tif.get(), 7U, 13U, "Mul32"), 91U);
  EXPECT_EQ(_TIFFMultiply32(tif.get(), UINT32_MAX, 1U, "Mul32"), UINT32_MAX);
  EXPECT_EQ(_TIFFMultiply32(tif.get(), 65535U, 65537U, "Mul32"), UINT32_MAX);
  EXPECT_EQ(_TIFFMultiply32(tif.get(), UINT32_MAX / 2U, 2U, "Mul32"),
            UINT32_MAX - 1U);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);

  EXPECT_EQ(_TIFFMultiply32(tif.get(), (UINT32_MAX / 2U) + 1U, 2U, "Mul32A"),
            0U);
  EXPECT_EQ(_TIFFMultiply32(tif.get(), 65536U, 65536U, "Mul32B"), 0U);
  EXPECT_EQ(_TIFFMultiply32(tif.get(), UINT32_MAX, 2U, "Mul32C"), 0U);

  const std::vector<DiagnosticRecord> errors = stream.diagnostics().errors();
  ASSERT_EQ(errors.size(), 3U);
  EXPECT_EQ(errors[0].module, "Mul32A");
  EXPECT_THAT(errors[0].message, HasSubstr("Integer overflow in Mul32A"));
  EXPECT_EQ(errors[1].module, "Mul32B");
  EXPECT_EQ(errors[2].module, "Mul32C");
}

TEST(TifAuxTest, Multiply64ValidAndOverflow) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  EXPECT_EQ(_TIFFMultiply64(tif.get(), 0ULL, 0ULL, "Mul64"), 0ULL);
  EXPECT_EQ(_TIFFMultiply64(tif.get(), UINT64_MAX, 0ULL, "Mul64"), 0ULL);
  EXPECT_EQ(_TIFFMultiply64(tif.get(), 0ULL, UINT64_MAX, "Mul64"), 0ULL);
  EXPECT_EQ(_TIFFMultiply64(tif.get(), 1ULL << 20, 1ULL << 15, "Mul64"),
            1ULL << 35);
  EXPECT_EQ(_TIFFMultiply64(tif.get(), UINT64_MAX, 1ULL, "Mul64"), UINT64_MAX);
  EXPECT_EQ(_TIFFMultiply64(tif.get(), 0xffffffffULL, 0x100000001ULL, "Mul64"),
            UINT64_MAX);
  EXPECT_EQ(_TIFFMultiply64(tif.get(), UINT64_MAX / 2ULL, 2ULL, "Mul64"),
            UINT64_MAX - 1ULL);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);

  EXPECT_EQ(
      _TIFFMultiply64(tif.get(), (UINT64_MAX / 2ULL) + 1ULL, 2ULL, "Mul64A"),
      0ULL);
  EXPECT_EQ(_TIFFMultiply64(tif.get(), 1ULL << 32, 1ULL << 32, "Mul64B"), 0ULL);
  EXPECT_EQ(_TIFFMultiply64(tif.get(), 0xffffffffULL, 0x100000002ULL, "Mul64C"),
            0ULL);

  const std::vector<DiagnosticRecord> errors = stream.diagnostics().errors();
  ASSERT_EQ(errors.size(), 3U);
  EXPECT_EQ(errors[0].module, "Mul64A");
  EXPECT_THAT(errors[0].message, HasSubstr("Integer overflow in Mul64A"));
  EXPECT_EQ(errors[1].module, "Mul64B");
  EXPECT_EQ(errors[2].module, "Mul64C");
}

TEST(TifAuxTest, Add64ValidOverflowAndSilentNullArgs) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  EXPECT_EQ(_TIFFAdd64(tif.get(), 0ULL, 0ULL, "Add64"), 0ULL);
  EXPECT_EQ(_TIFFAdd64(tif.get(), 1234ULL, 5678ULL, "Add64"), 6912ULL);
  EXPECT_EQ(_TIFFAdd64(tif.get(), UINT64_MAX, 0ULL, "Add64"), UINT64_MAX);
  EXPECT_EQ(_TIFFAdd64(tif.get(), UINT64_MAX - 10ULL, 10ULL, "Add64"),
            UINT64_MAX);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);

  EXPECT_EQ(_TIFFAdd64(tif.get(), UINT64_MAX, 1ULL, "Add64Overflow"), 0ULL);
  EXPECT_EQ(_TIFFAdd64(tif.get(), (1ULL << 63) + 10ULL, 1ULL << 63, "Add64B"),
            0ULL);
  ASSERT_EQ(stream.diagnostics().error_count(), 2U);
  EXPECT_THAT(stream.diagnostics().errors()[0].message,
              HasSubstr("Integer overflow in Add64Overflow"));

  // Overflow with null tif or null module must suppress error reporting.
  stream.diagnostics().Clear();
  EXPECT_EQ(_TIFFAdd64(nullptr, UINT64_MAX, 1ULL, "Add64NoTif"), 0ULL);
  EXPECT_EQ(_TIFFAdd64(tif.get(), UINT64_MAX, 1ULL, nullptr), 0ULL);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifAuxTest, MultiplySSizeValidInvalidOverflowAndSilentNullArgs) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  EXPECT_EQ(_TIFFMultiplySSize(tif.get(), 17, 19, "MulSS"), 323);
  EXPECT_EQ(_TIFFMultiplySSize(tif.get(), TIFF_TMSIZE_T_MAX, 1, "MulSS"),
            TIFF_TMSIZE_T_MAX);
  EXPECT_EQ(_TIFFMultiplySSize(tif.get(), TIFF_TMSIZE_T_MAX / 3, 3, "MulSS"),
            (TIFF_TMSIZE_T_MAX / 3) * 3);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);

  EXPECT_EQ(_TIFFMultiplySSize(tif.get(), 0, 5, "MulSSZeroFirst"), 0);
  EXPECT_EQ(_TIFFMultiplySSize(tif.get(), 5, 0, "MulSSZeroSecond"), 0);
  EXPECT_EQ(_TIFFMultiplySSize(tif.get(), -1, 5, "MulSSNegFirst"), 0);
  EXPECT_EQ(_TIFFMultiplySSize(tif.get(), 5, -2, "MulSSNegSecond"), 0);
  EXPECT_EQ(_TIFFMultiplySSize(tif.get(), (TIFF_TMSIZE_T_MAX / 2) + 1, 2,
                               "MulSSOverflow"),
            0);

  const std::vector<DiagnosticRecord> errors = stream.diagnostics().errors();
  ASSERT_EQ(errors.size(), 5U);
  EXPECT_THAT(errors[0].message,
              HasSubstr("Invalid argument to _TIFFMultiplySSize()"));
  EXPECT_THAT(errors[3].message,
              HasSubstr("Invalid argument to _TIFFMultiplySSize()"));
  EXPECT_THAT(errors[4].message,
              HasSubstr("Integer overflow in MulSSOverflow"));

  // Null tif or null module must remain silent on both invalid args & overflow.
  stream.diagnostics().Clear();
  EXPECT_EQ(_TIFFMultiplySSize(nullptr, 0, 5, "Silent"), 0);
  EXPECT_EQ(_TIFFMultiplySSize(tif.get(), -1, 5, nullptr), 0);
  EXPECT_EQ(_TIFFMultiplySSize(nullptr, TIFF_TMSIZE_T_MAX, 2, "Silent"), 0);
  EXPECT_EQ(_TIFFMultiplySSize(tif.get(), TIFF_TMSIZE_T_MAX, 2, nullptr), 0);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifAuxTest, AddSSizeValidInvalidOverflowAndSilentNullArgs) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  EXPECT_EQ(_TIFFAddSSize(tif.get(), 0, 0, "AddSS"), 0);
  EXPECT_EQ(_TIFFAddSSize(tif.get(), 0, TIFF_TMSIZE_T_MAX, "AddSS"),
            TIFF_TMSIZE_T_MAX);
  EXPECT_EQ(_TIFFAddSSize(tif.get(), TIFF_TMSIZE_T_MAX, 0, "AddSS"),
            TIFF_TMSIZE_T_MAX);
  EXPECT_EQ(_TIFFAddSSize(tif.get(), 123, 456, "AddSS"), 579);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);

  EXPECT_EQ(_TIFFAddSSize(tif.get(), -1, 5, "AddSSNegA"), 0);
  EXPECT_EQ(_TIFFAddSSize(tif.get(), 5, -1, "AddSSNegB"), 0);
  EXPECT_EQ(_TIFFAddSSize(tif.get(), TIFF_TMSIZE_T_MAX, 1, "AddSSOverflow"), 0);

  const std::vector<DiagnosticRecord> errors = stream.diagnostics().errors();
  ASSERT_EQ(errors.size(), 3U);
  EXPECT_THAT(errors[0].message,
              HasSubstr("Invalid argument to _TIFFAddSSize()"));
  EXPECT_THAT(errors[1].message,
              HasSubstr("Invalid argument to _TIFFAddSSize()"));
  EXPECT_THAT(errors[2].message,
              HasSubstr("Integer overflow in AddSSOverflow"));

  stream.diagnostics().Clear();
  EXPECT_EQ(_TIFFAddSSize(nullptr, -1, 5, "Silent"), 0);
  EXPECT_EQ(_TIFFAddSSize(tif.get(), 5, -1, nullptr), 0);
  EXPECT_EQ(_TIFFAddSSize(nullptr, TIFF_TMSIZE_T_MAX, 1, "Silent"), 0);
  EXPECT_EQ(_TIFFAddSSize(tif.get(), TIFF_TMSIZE_T_MAX, 1, nullptr), 0);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifAuxTest, CastUInt64ToSSizeAndUInt32) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  EXPECT_EQ(_TIFFCastUInt64ToSSize(tif.get(), 0ULL, "CastSS"), 0);
  EXPECT_EQ(_TIFFCastUInt64ToSSize(tif.get(), 987654ULL, "CastSS"), 987654);
  EXPECT_EQ(_TIFFCastUInt64ToSSize(
                tif.get(), static_cast<uint64_t>(TIFF_TMSIZE_T_MAX), "CastSS"),
            TIFF_TMSIZE_T_MAX);

  EXPECT_EQ(_TIFFCastUInt64ToUInt32(tif.get(), 0ULL, "CastU32"), 0U);
  EXPECT_EQ(_TIFFCastUInt64ToUInt32(tif.get(), UINT32_MAX, "CastU32"),
            UINT32_MAX);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);

  EXPECT_EQ(
      _TIFFCastUInt64ToSSize(
          tif.get(), static_cast<uint64_t>(TIFF_TMSIZE_T_MAX) + 1ULL, "CastSS"),
      0);
  EXPECT_EQ(_TIFFCastUInt64ToSSize(tif.get(), UINT64_MAX, "CastSS"), 0);
  EXPECT_EQ(_TIFFCastUInt64ToUInt32(
                tif.get(), static_cast<uint64_t>(UINT32_MAX) + 1ULL, "CastU32"),
            0U);
  EXPECT_EQ(_TIFFCastUInt64ToUInt32(tif.get(), UINT64_MAX, "CastU32"), 0U);

  const std::vector<DiagnosticRecord> errors = stream.diagnostics().errors();
  ASSERT_EQ(errors.size(), 4U);
  EXPECT_EQ(errors[0].module, "CastSS");
  EXPECT_THAT(errors[0].message, HasSubstr("Integer overflow"));
  EXPECT_EQ(errors[2].module, "CastU32");
  EXPECT_THAT(errors[2].message, HasSubstr("Integer overflow"));

  stream.diagnostics().Clear();
  EXPECT_EQ(_TIFFCastUInt64ToSSize(nullptr, UINT64_MAX, "Silent"), 0);
  EXPECT_EQ(_TIFFCastUInt64ToSSize(tif.get(), UINT64_MAX, nullptr), 0);
  EXPECT_EQ(_TIFFCastUInt64ToUInt32(nullptr, UINT64_MAX, "Silent"), 0U);
  EXPECT_EQ(_TIFFCastUInt64ToUInt32(tif.get(), UINT64_MAX, nullptr), 0U);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifAuxTest, ComputeRowOffsetBitOffsetAndRowSize64) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  // _TIFFComputeRowOffset returns 0 immediately when row == 0.
  EXPECT_EQ(_TIFFComputeRowOffset(tif.get(), 0, 0U, "RowOff"), 0);
  EXPECT_EQ(_TIFFComputeRowOffset(tif.get(), -16, 0U, "RowOff"), 0);
  EXPECT_EQ(_TIFFComputeRowOffset(tif.get(), TIFF_TMSIZE_T_MAX, 0U, "RowOff"),
            0);
  EXPECT_EQ(_TIFFComputeRowOffset(tif.get(), 1024, 5U, "RowOff"), 5120);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);

  EXPECT_EQ(_TIFFComputeRowOffset(tif.get(), -16, 2U, "RowOffNeg"), 0);
  EXPECT_EQ(
      _TIFFComputeRowOffset(tif.get(), TIFF_TMSIZE_T_MAX, 2U, "RowOffOverflow"),
      0);
  EXPECT_EQ(stream.diagnostics().error_count(), 2U);
  stream.diagnostics().Clear();

  // _TIFFComputeBitOffset
  EXPECT_EQ(_TIFFComputeBitOffset(tif.get(), 0U, 3U, 8U, "BitOff"), 0ULL);
  EXPECT_EQ(_TIFFComputeBitOffset(tif.get(), 10U, 0U, 8U, "BitOff"), 0ULL);
  EXPECT_EQ(_TIFFComputeBitOffset(tif.get(), 10U, 3U, 0U, "BitOff"), 0ULL);
  EXPECT_EQ(_TIFFComputeBitOffset(tif.get(), 17U, 3U, 16U, "BitOff"), 816ULL);
  EXPECT_EQ(_TIFFComputeBitOffset(tif.get(), 0x40000000U, 4U, 32U, "BitOff"),
            (1ULL << 37));
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);

  // _TIFFComputeRowSize64
  EXPECT_EQ(_TIFFComputeRowSize64(tif.get(), 0U, 3U, 8U, "RowSize"), 0ULL);
  EXPECT_EQ(_TIFFComputeRowSize64(tif.get(), 8U, 0U, 8U, "RowSize"), 0ULL);
  EXPECT_EQ(_TIFFComputeRowSize64(tif.get(), 8U, 3U, 0U, "RowSize"), 0ULL);
  EXPECT_EQ(_TIFFComputeRowSize64(tif.get(), 1U, 1U, 1U, "RowSize"), 1ULL);
  EXPECT_EQ(_TIFFComputeRowSize64(tif.get(), 8U, 1U, 1U, "RowSize"), 1ULL);
  EXPECT_EQ(_TIFFComputeRowSize64(tif.get(), 9U, 1U, 1U, "RowSize"), 2ULL);
  EXPECT_EQ(_TIFFComputeRowSize64(tif.get(), 5U, 3U, 12U, "RowSize"), 23ULL);
  EXPECT_EQ(_TIFFComputeRowSize64(tif.get(), 0x10000000U, 4U, 32U, "RowSize"),
            1ULL << 32);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifAuxTest, CheckMallocAndCheckReallocSuccessGrowAndShrink) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  ScopedTiffExtAlloc alloc(tif.get());
  ASSERT_TRUE(alloc.Malloc(16, sizeof(uint32_t), "initial buffer"));
  for (size_t i = 0; i < 64; ++i) {
    alloc.bytes()[i] = static_cast<uint8_t>(i ^ 0x5aU);
  }

  ASSERT_TRUE(alloc.Realloc(64, sizeof(uint32_t), "grown buffer"));
  for (size_t i = 0; i < 64; ++i) {
    EXPECT_EQ(alloc.bytes()[i], static_cast<uint8_t>(i ^ 0x5aU));
  }
  std::memset(alloc.bytes() + 64, 0x3c, 192);

  ASSERT_TRUE(alloc.Realloc(8, sizeof(uint32_t), "shrunk buffer"));
  for (size_t i = 0; i < 32; ++i) {
    EXPECT_EQ(alloc.bytes()[i], static_cast<uint8_t>(i ^ 0x5aU));
  }
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifAuxTest, CheckReallocRejectsZeroNegativeOverflowAndGoogleIntMaxCap) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  struct BadAllocCase {
    tmsize_t nmemb;
    tmsize_t elem_size;
    std::string_view what;
  };
  constexpr tmsize_t kOverIntMax = static_cast<tmsize_t>(INT_MAX) + 1;
  const auto kBadCases = std::to_array<BadAllocCase>({
      {.nmemb = 0, .elem_size = 4, .what = "zero nmemb"},
      {.nmemb = 4, .elem_size = 0, .what = "zero elem_size"},
      {.nmemb = -1, .elem_size = 4, .what = "negative nmemb"},
      {.nmemb = 4, .elem_size = -1, .what = "negative elem_size"},
      {.nmemb = kOverIntMax, .elem_size = 1, .what = "nmemb over INT_MAX"},
      {.nmemb = 1, .elem_size = kOverIntMax, .what = "elem_size over INT_MAX"},
      {.nmemb = 65536, .elem_size = 65536, .what = "product 4GiB over INT_MAX"},
      {.nmemb = INT_MAX, .elem_size = 2, .what = "product 2*INT_MAX"},
  });

  for (const BadAllocCase& c : kBadCases) {
    SCOPED_TRACE(c.what);
    stream.diagnostics().Clear();
    void* ptr = _TIFFCheckMalloc(tif.get(), c.nmemb, c.elem_size,
                                 std::string(c.what).c_str());
    EXPECT_EQ(ptr, nullptr);
    if (ptr != nullptr) {
      _TIFFfreeExt(tif.get(), ptr);
    }
    ASSERT_EQ(stream.diagnostics().error_count(), 1U);
    EXPECT_THAT(stream.diagnostics().errors()[0].message,
                HasSubstr("Failed to allocate memory for"));
  }

  // Realloc failure on an existing block must return nullptr and keep the old
  // block valid.
  ScopedTiffExtAlloc existing(tif.get());
  ASSERT_TRUE(existing.Malloc(8, 1, "existing"));
  std::memset(existing.bytes(), 0xab, 8);
  stream.diagnostics().Clear();
  EXPECT_FALSE(existing.Realloc(kOverIntMax, 1, "over INT_MAX realloc"));
  EXPECT_EQ(stream.diagnostics().error_count(), 1U);
  for (size_t i = 0; i < 8; ++i) {
    EXPECT_EQ(existing.bytes()[i], 0xabU);
  }
}

TEST(TifAuxTest, CheckMallocAndReallocHonorAllocationLimits) {
  MemoryTiffStream stream;
  DiagnosticRecorder recorder;
  UniqueTiff tif = OpenWritableWithRecorder(stream, recorder, "w",
                                            /*max_cumulated=*/1 << 24);
  ASSERT_NE(tif, nullptr);

  // Single-allocation limit.
  {
    ScopedSingleMemAllocLimit single_limit(tif.get(), 64);
    ScopedTiffExtAlloc buf(tif.get());
    EXPECT_TRUE(buf.Malloc(64, 1, "at single limit"));
    EXPECT_EQ(recorder.error_count(), 0U);

    EXPECT_FALSE(buf.Realloc(65, 1, "realloc over single limit"));
    EXPECT_GE(recorder.error_count(), 1U);
    recorder.Clear();

    ScopedTiffExtAlloc failed(tif.get());
    EXPECT_FALSE(failed.Malloc(65, 1, "malloc over single limit"));
    EXPECT_GE(recorder.error_count(), 1U);
    recorder.Clear();
  }

  // Cumulated allocation budget remaining.
  {
    ScopedTiffExtAlloc initial(tif.get());
    ASSERT_TRUE(initial.Malloc(32, 1, "initial"));
    std::memset(initial.bytes(), 0x55, 32);

    ScopedCumulatedMemAllocRemaining remaining(tif.get(), 16);
    EXPECT_TRUE(initial.Realloc(40, 1, "small grow within budget"));
    EXPECT_EQ(recorder.error_count(), 0U);
    for (size_t i = 0; i < 32; ++i) {
      EXPECT_EQ(initial.bytes()[i], 0x55U);
    }

    EXPECT_FALSE(initial.Realloc(64, 1, "grow over remaining budget"));
    EXPECT_GE(recorder.error_count(), 1U);
    recorder.Clear();

    ScopedTiffExtAlloc extra(tif.get());
    EXPECT_FALSE(extra.Malloc(16, 1, "new alloc over remaining budget"));
    EXPECT_GE(recorder.error_count(), 1U);
    recorder.Clear();
  }
}

TEST(TifAuxTest, GetFieldDefaultedStandardDefaultsOnNewDirectory) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  uint32_t subfiletype = 99;
  uint16_t bps = 99;
  uint16_t compression = 99;
  uint16_t threshholding = 99;
  uint16_t fillorder = 99;
  uint16_t orientation = 99;
  uint16_t spp = 99;
  uint32_t rowsperstrip = 0;
  uint16_t minsample = 99;
  uint16_t maxsample = 99;
  uint16_t planarconfig = 99;
  uint16_t resunit = 99;
  uint16_t dot0 = 99;
  uint16_t dot1 = 99;
  uint16_t inkset = 99;
  uint16_t num_inks = 99;
  uint16_t extra_count = 99;
  const uint16_t* extra_info = &bps;
  uint16_t matteing = 99;
  uint32_t tiledepth = 99;
  uint16_t datatype = 99;
  uint16_t sampleformat = 99;
  uint32_t imagedepth = 99;
  const float* ycbcr_coeffs = nullptr;
  uint16_t ycbcr_sub_h = 99;
  uint16_t ycbcr_sub_v = 99;
  uint16_t ycbcr_pos = 99;
  const float* whitepoint = nullptr;

  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_SUBFILETYPE, &subfiletype),
            1);
  EXPECT_EQ(subfiletype, 0U);

  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_BITSPERSAMPLE, &bps), 1);
  EXPECT_EQ(bps, 1);

  // TIFFDefaultDirectory sets COMPRESSION_NONE via TIFFSetField.
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_COMPRESSION, &compression),
            1);
  EXPECT_EQ(compression, COMPRESSION_NONE);

  EXPECT_EQ(
      TIFFGetFieldDefaulted(tif.get(), TIFFTAG_THRESHHOLDING, &threshholding),
      1);
  EXPECT_EQ(threshholding, THRESHHOLD_BILEVEL);

  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_FILLORDER, &fillorder), 1);
  EXPECT_EQ(fillorder, FILLORDER_MSB2LSB);

  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_ORIENTATION, &orientation),
            1);
  EXPECT_EQ(orientation, ORIENTATION_TOPLEFT);

  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_SAMPLESPERPIXEL, &spp), 1);
  EXPECT_EQ(spp, 1);

  EXPECT_EQ(
      TIFFGetFieldDefaulted(tif.get(), TIFFTAG_ROWSPERSTRIP, &rowsperstrip), 1);
  EXPECT_EQ(rowsperstrip, std::numeric_limits<uint32_t>::max());

  EXPECT_EQ(
      TIFFGetFieldDefaulted(tif.get(), TIFFTAG_MINSAMPLEVALUE, &minsample), 1);
  EXPECT_EQ(minsample, 0);

  EXPECT_EQ(
      TIFFGetFieldDefaulted(tif.get(), TIFFTAG_MAXSAMPLEVALUE, &maxsample), 1);
  EXPECT_EQ(maxsample, 1);

  EXPECT_EQ(
      TIFFGetFieldDefaulted(tif.get(), TIFFTAG_PLANARCONFIG, &planarconfig), 1);
  EXPECT_EQ(planarconfig, PLANARCONFIG_CONTIG);

  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_RESOLUTIONUNIT, &resunit),
            1);
  EXPECT_EQ(resunit, RESUNIT_INCH);

  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_DOTRANGE, &dot0, &dot1),
            1);
  EXPECT_EQ(dot0, 0);
  EXPECT_EQ(dot1, 1);

  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_INKSET, &inkset), 1);
  EXPECT_EQ(inkset, INKSET_CMYK);

  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_NUMBEROFINKS, &num_inks),
            1);
  EXPECT_EQ(num_inks, 4);

  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_EXTRASAMPLES, &extra_count,
                                  &extra_info),
            1);
  EXPECT_EQ(extra_count, 0);
  EXPECT_EQ(extra_info, nullptr);

  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_MATTEING, &matteing), 1);
  EXPECT_EQ(matteing, 0);

  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_TILEDEPTH, &tiledepth), 1);
  EXPECT_EQ(tiledepth, 1U);

  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_DATATYPE, &datatype), 1);
  EXPECT_EQ(datatype, static_cast<uint16_t>(SAMPLEFORMAT_UINT - 1));

  EXPECT_EQ(
      TIFFGetFieldDefaulted(tif.get(), TIFFTAG_SAMPLEFORMAT, &sampleformat), 1);
  EXPECT_EQ(sampleformat, SAMPLEFORMAT_UINT);

  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_IMAGEDEPTH, &imagedepth),
            1);
  EXPECT_EQ(imagedepth, 1U);

  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_YCBCRCOEFFICIENTS,
                                  &ycbcr_coeffs),
            1);
  ASSERT_NE(ycbcr_coeffs, nullptr);
  EXPECT_FLOAT_EQ(ycbcr_coeffs[0], 0.299f);
  EXPECT_FLOAT_EQ(ycbcr_coeffs[1], 0.587f);
  EXPECT_FLOAT_EQ(ycbcr_coeffs[2], 0.114f);

  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_YCBCRSUBSAMPLING,
                                  &ycbcr_sub_h, &ycbcr_sub_v),
            1);
  EXPECT_EQ(ycbcr_sub_h, 2);
  EXPECT_EQ(ycbcr_sub_v, 2);

  EXPECT_EQ(
      TIFFGetFieldDefaulted(tif.get(), TIFFTAG_YCBCRPOSITIONING, &ycbcr_pos),
      1);
  EXPECT_EQ(ycbcr_pos, YCBCRPOSITION_CENTERED);

  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_WHITEPOINT, &whitepoint),
            1);
  ASSERT_NE(whitepoint, nullptr);
  const float d50_sum = D50_X0 + D50_Y0 + D50_Z0;
  EXPECT_FLOAT_EQ(whitepoint[0], D50_X0 / d50_sum);
  EXPECT_FLOAT_EQ(whitepoint[1], D50_Y0 / d50_sum);

  // Direct TIFFVGetFieldDefaulted call.
  uint16_t direct_orient = 0;
  EXPECT_EQ(
      CallVGetFieldDefaulted(tif.get(), TIFFTAG_ORIENTATION, &direct_orient),
      1);
  EXPECT_EQ(direct_orient, ORIENTATION_TOPLEFT);

  // Non-defaultable tags return 0 when unset.
  uint32_t width = 0;
  float xres = 0.0f;
  const char* artist = nullptr;
  const char* inknames = nullptr;
  uint32_t unknown = 0;
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_IMAGEWIDTH, &width), 0);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_XRESOLUTION, &xres), 0);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_ARTIST, &artist), 0);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_INKNAMES, &inknames), 0);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), 65500U, &unknown), 0);
  EXPECT_EQ(TIFFUnsetField(tif.get(), TIFFTAG_COMPRESSION), 1);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_COMPRESSION, &compression),
            0);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifAuxTest, GetFieldDefaultedReturnsExplicitlySetTags) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SUBFILETYPE, FILETYPE_PAGE), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, 8), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_PACKBITS),
            1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_THRESHHOLDING, THRESHHOLD_HALFTONE),
            1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_FILLORDER, FILLORDER_LSB2MSB), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ORIENTATION, ORIENTATION_BOTLEFT),
            1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, 3), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_ROWSPERSTRIP, 32U), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_MINSAMPLEVALUE, 12), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_MAXSAMPLEVALUE, 240), 1);
  ASSERT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_RESOLUTIONUNIT, RESUNIT_CENTIMETER),
            1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_DOTRANGE, 10, 245), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_INKSET, INKSET_MULTIINK), 1);
  static constexpr char kInkNames[] = "Cyan\0Magenta\0Yellow";
  ASSERT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_INKNAMES, sizeof(kInkNames), kInkNames),
      1);
  uint16_t extra_type = EXTRASAMPLE_UNASSALPHA;
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_EXTRASAMPLES, 1, &extra_type), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_TILEDEPTH, 4U), 1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP),
            1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_IMAGEDEPTH, 8U), 1);
  std::array<float, 3> custom_ycbcr = {0.2126f, 0.7152f, 0.0722f};
  ASSERT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_YCBCRCOEFFICIENTS, custom_ycbcr.data()),
      1);
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_YCBCRSUBSAMPLING, 4, 1), 1);
  ASSERT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_YCBCRPOSITIONING, YCBCRPOSITION_COSITED),
      1);
  std::array<float, 2> custom_wp = {0.3127f, 0.3290f};
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_WHITEPOINT, custom_wp.data()), 1);
  std::array<float, 6> custom_rbw = {1.0f, 254.0f, 2.0f, 253.0f, 3.0f, 252.0f};
  ASSERT_EQ(
      TIFFSetField(tif.get(), TIFFTAG_REFERENCEBLACKWHITE, custom_rbw.data()),
      1);

  uint32_t u32 = 0;
  uint16_t u16 = 0;
  uint16_t u16_b = 0;
  const uint16_t* u16_ptr = nullptr;
  const float* f_ptr = nullptr;
  const char* s_ptr = nullptr;

  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_SUBFILETYPE, &u32), 1);
  EXPECT_EQ(u32, FILETYPE_PAGE);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_BITSPERSAMPLE, &u16), 1);
  EXPECT_EQ(u16, 8);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_COMPRESSION, &u16), 1);
  EXPECT_EQ(u16, COMPRESSION_PACKBITS);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_THRESHHOLDING, &u16), 1);
  EXPECT_EQ(u16, THRESHHOLD_HALFTONE);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_FILLORDER, &u16), 1);
  EXPECT_EQ(u16, FILLORDER_LSB2MSB);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_ORIENTATION, &u16), 1);
  EXPECT_EQ(u16, ORIENTATION_BOTLEFT);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_SAMPLESPERPIXEL, &u16), 1);
  EXPECT_EQ(u16, 3);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_ROWSPERSTRIP, &u32), 1);
  EXPECT_EQ(u32, 32U);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_MINSAMPLEVALUE, &u16), 1);
  EXPECT_EQ(u16, 12);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_MAXSAMPLEVALUE, &u16), 1);
  EXPECT_EQ(u16, 240);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_PLANARCONFIG, &u16), 1);
  EXPECT_EQ(u16, PLANARCONFIG_SEPARATE);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_RESOLUTIONUNIT, &u16), 1);
  EXPECT_EQ(u16, RESUNIT_CENTIMETER);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_DOTRANGE, &u16, &u16_b),
            1);
  EXPECT_EQ(u16, 10);
  EXPECT_EQ(u16_b, 245);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_INKSET, &u16), 1);
  EXPECT_EQ(u16, INKSET_MULTIINK);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_NUMBEROFINKS, &u16), 1);
  EXPECT_EQ(u16, 3);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_INKNAMES, &s_ptr), 1);
  ASSERT_NE(s_ptr, nullptr);
  EXPECT_EQ(std::memcmp(s_ptr, kInkNames, sizeof(kInkNames)), 0);
  EXPECT_EQ(
      TIFFGetFieldDefaulted(tif.get(), TIFFTAG_EXTRASAMPLES, &u16, &u16_ptr),
      1);
  EXPECT_EQ(u16, 1);
  ASSERT_NE(u16_ptr, nullptr);
  EXPECT_EQ(u16_ptr[0], EXTRASAMPLE_UNASSALPHA);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_MATTEING, &u16), 1);
  EXPECT_EQ(u16, 0);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_TILEDEPTH, &u32), 1);
  EXPECT_EQ(u32, 4U);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_SAMPLEFORMAT, &u16), 1);
  EXPECT_EQ(u16, SAMPLEFORMAT_IEEEFP);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_DATATYPE, &u16), 1);
  EXPECT_EQ(u16, 3);  // DATATYPE_IEEEFP
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_IMAGEDEPTH, &u32), 1);
  EXPECT_EQ(u32, 8U);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_YCBCRCOEFFICIENTS, &f_ptr),
            1);
  ASSERT_NE(f_ptr, nullptr);
  EXPECT_FLOAT_EQ(f_ptr[0], 0.2126f);
  EXPECT_FLOAT_EQ(f_ptr[1], 0.7152f);
  EXPECT_FLOAT_EQ(f_ptr[2], 0.0722f);
  EXPECT_EQ(
      TIFFGetFieldDefaulted(tif.get(), TIFFTAG_YCBCRSUBSAMPLING, &u16, &u16_b),
      1);
  EXPECT_EQ(u16, 4);
  EXPECT_EQ(u16_b, 1);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_YCBCRPOSITIONING, &u16),
            1);
  EXPECT_EQ(u16, YCBCRPOSITION_COSITED);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_WHITEPOINT, &f_ptr), 1);
  ASSERT_NE(f_ptr, nullptr);
  EXPECT_FLOAT_EQ(f_ptr[0], 0.3127f);
  EXPECT_FLOAT_EQ(f_ptr[1], 0.3290f);
  EXPECT_EQ(
      TIFFGetFieldDefaulted(tif.get(), TIFFTAG_REFERENCEBLACKWHITE, &f_ptr), 1);
  ASSERT_NE(f_ptr, nullptr);
  EXPECT_FLOAT_EQ(f_ptr[0], 1.0f);
  EXPECT_FLOAT_EQ(f_ptr[5], 252.0f);

  // Associated alpha sets Matteing to 1.
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_MATTEING, 1), 1);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_MATTEING, &u16), 1);
  EXPECT_EQ(u16, 1);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifAuxTest, GetFieldDefaultedMaxSampleValueAndDotRangeByBitDepth) {
  struct BitDepthCase {
    uint16_t bps;
    uint16_t expected_max;
    uint16_t expected_dot_max;
  };
  constexpr auto kCases = std::to_array<BitDepthCase>({
      {.bps = 0, .expected_max = 0, .expected_dot_max = 0},
      {.bps = 1, .expected_max = 1, .expected_dot_max = 1},
      {.bps = 8, .expected_max = 255, .expected_dot_max = 255},
      {.bps = 16, .expected_max = 65535, .expected_dot_max = 65535},
      {.bps = 24, .expected_max = 65535, .expected_dot_max = 65535},
      {.bps = 32, .expected_max = 65535, .expected_dot_max = 65535},
  });

  for (const BitDepthCase& c : kCases) {
    SCOPED_TRACE(c.bps);
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, c.bps), 1);

    uint16_t max_val = 99;
    uint16_t dot_min = 99;
    uint16_t dot_max = 99;
    EXPECT_EQ(
        TIFFGetFieldDefaulted(tif.get(), TIFFTAG_MAXSAMPLEVALUE, &max_val), 1);
    EXPECT_EQ(max_val, c.expected_max);
    EXPECT_EQ(
        TIFFGetFieldDefaulted(tif.get(), TIFFTAG_DOTRANGE, &dot_min, &dot_max),
        1);
    EXPECT_EQ(dot_min, 0);
    EXPECT_EQ(dot_max, c.expected_dot_max);
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  }
}

TEST(TifAuxTest, GetFieldDefaultedPredictorWithAndWithoutPlugin) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  // Without a predictor-capable codec, tif_data is NULL and querying Predictor
  // emits an error and returns 0.
  uint16_t predictor = 99;
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_PREDICTOR, &predictor), 0);
  EXPECT_EQ(predictor, 0);
  ASSERT_EQ(stream.diagnostics().error_count(), 1U);
  EXPECT_THAT(
      stream.diagnostics().errors()[0].message,
      HasSubstr("Cannot get \"Predictor\" tag as plugin is not configured"));
  stream.diagnostics().Clear();

  // LZW configures TIFFPredictorInit with default predictor = PREDICTOR_NONE.
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_COMPRESSION, COMPRESSION_LZW), 1);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_PREDICTOR, &predictor), 0);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_PREDICTOR, &predictor), 1);
  EXPECT_EQ(predictor, PREDICTOR_NONE);

  // Explicit predictor setting is returned via TIFFVGetField.
  ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL),
            1);
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_PREDICTOR, &predictor), 1);
  EXPECT_EQ(predictor, PREDICTOR_HORIZONTAL);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

TEST(TifAuxTest, GetFieldDefaultedTransferFunctionVariantsAndOom) {
  // 1-sample grayscale 8-bit default gamma 2.2 LUT + cached reuse.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, 8), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, 1), 1);

    const uint16_t* tf0 = nullptr;
    const uint16_t* tf1 = nullptr;
    const uint16_t* tf2 = nullptr;
    EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_TRANSFERFUNCTION, &tf0,
                                    &tf1, &tf2),
              1);
    ASSERT_NE(tf0, nullptr);
    EXPECT_EQ(tf1, nullptr);
    EXPECT_EQ(tf2, nullptr);
    EXPECT_EQ(tf0[0], 0);
    EXPECT_EQ(tf0[255], 65535);
    EXPECT_EQ(tf0[128], static_cast<uint16_t>(std::floor(
                            65535.0 * std::pow(128.0 / 255.0, 2.2) + 0.5)));

    // Second call returns the cached pointer without reallocating.
    const uint16_t* tf0_cached = nullptr;
    EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_TRANSFERFUNCTION,
                                    &tf0_cached, &tf1, &tf2),
              1);
    EXPECT_EQ(tf0_cached, tf0);
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  }

  // 3-sample RGB allocates 3 independent identical LUTs.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, 8), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, 3), 1);

    const uint16_t* tf0 = nullptr;
    const uint16_t* tf1 = nullptr;
    const uint16_t* tf2 = nullptr;
    EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_TRANSFERFUNCTION, &tf0,
                                    &tf1, &tf2),
              1);
    ASSERT_NE(tf0, nullptr);
    ASSERT_NE(tf1, nullptr);
    ASSERT_NE(tf2, nullptr);
    EXPECT_NE(tf0, tf1);
    EXPECT_NE(tf1, tf2);
    EXPECT_EQ(std::memcmp(tf0, tf1, 256 * sizeof(uint16_t)), 0);
    EXPECT_EQ(std::memcmp(tf0, tf2, 256 * sizeof(uint16_t)), 0);
  }

  // 2 samples with 1 extra sample (2 - 1 == 1) requests only 1 LUT.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, 4), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, 2), 1);
    uint16_t extra = EXTRASAMPLE_UNASSALPHA;
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_EXTRASAMPLES, 1, &extra), 1);

    const uint16_t* tf0 = nullptr;
    const uint16_t* tf1 = nullptr;
    const uint16_t* tf2 = nullptr;
    EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_TRANSFERFUNCTION, &tf0,
                                    &tf1, &tf2),
              1);
    ASSERT_NE(tf0, nullptr);
    EXPECT_EQ(tf0[0], 0);
    EXPECT_EQ(tf0[15], 65535);
    EXPECT_EQ(tf1, nullptr);
    EXPECT_EQ(tf2, nullptr);
  }

  // Explicitly set TransferFunction is returned via TIFFVGetField.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, 2), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, 1), 1);
    std::array<uint16_t, 4> custom_tf = {10, 200, 3000, 40000};
    ASSERT_EQ(
        TIFFSetField(tif.get(), TIFFTAG_TRANSFERFUNCTION, custom_tf.data()), 1);

    const uint16_t* tf0 = nullptr;
    const uint16_t* tf1 = nullptr;
    const uint16_t* tf2 = nullptr;
    EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_TRANSFERFUNCTION, &tf0,
                                    &tf1, &tf2),
              1);
    ASSERT_NE(tf0, nullptr);
    EXPECT_EQ(tf0[0], 10);
    EXPECT_EQ(tf0[3], 40000);
  }

  // BitsPerSample > 24 DoS guard refuses default TransferFunction allocation.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, 32), 1);

    const uint16_t* tf0 = nullptr;
    const uint16_t* tf1 = nullptr;
    const uint16_t* tf2 = nullptr;
    EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_TRANSFERFUNCTION, &tf0,
                                    &tf1, &tf2),
              0);
    EXPECT_EQ(tf0, nullptr);
    ASSERT_EQ(stream.diagnostics().error_count(), 1U);
    EXPECT_THAT(stream.diagnostics().errors()[0].message,
                HasSubstr("No space for \"TransferFunction\" tag"));
  }

  // OOM on tf[0], tf[1], and tf[2] cleanly cleans up partial allocations.
  {
    MemoryTiffStream stream;
    DiagnosticRecorder recorder;
    UniqueTiff tif = OpenWritableWithRecorder(stream, recorder, "w",
                                              /*max_cumulated=*/1 << 24);
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, 8), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, 3), 1);

    const uint16_t* tf0 = nullptr;
    const uint16_t* tf1 = nullptr;
    const uint16_t* tf2 = nullptr;

    // 8-bit LUT requires 256 * 2 = 512 bytes per channel.
    {
      ScopedSingleMemAllocLimit limit(tif.get(), 256);
      EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_TRANSFERFUNCTION, &tf0,
                                      &tf1, &tf2),
                0);
      EXPECT_GE(recorder.error_count(), 1U);
      recorder.Clear();
    }

    // Allow tf[0] (512 bytes), fail tf[1] (1024 > 600).
    {
      ScopedCumulatedMemAllocRemaining budget(tif.get(), 600);
      EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_TRANSFERFUNCTION, &tf0,
                                      &tf1, &tf2),
                0);
      EXPECT_GE(recorder.error_count(), 1U);
      recorder.Clear();
    }

    // Allow tf[0] + tf[1] (1024 bytes), fail tf[2] (1536 > 1100).
    {
      ScopedCumulatedMemAllocRemaining budget(tif.get(), 1100);
      EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_TRANSFERFUNCTION, &tf0,
                                      &tf1, &tf2),
                0);
      EXPECT_GE(recorder.error_count(), 1U);
      recorder.Clear();
    }
  }
}

TEST(TifAuxTest, GetFieldDefaultedReferenceBlackWhiteVariantsAndOom) {
  // YCbCr default ReferenceBlackWhite + cached reuse.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR),
              1);

    const float* rbw = nullptr;
    EXPECT_EQ(
        TIFFGetFieldDefaulted(tif.get(), TIFFTAG_REFERENCEBLACKWHITE, &rbw), 1);
    ASSERT_NE(rbw, nullptr);
    EXPECT_FLOAT_EQ(rbw[0], 0.0f);
    EXPECT_FLOAT_EQ(rbw[1], 255.0f);
    EXPECT_FLOAT_EQ(rbw[2], 128.0f);
    EXPECT_FLOAT_EQ(rbw[3], 255.0f);
    EXPECT_FLOAT_EQ(rbw[4], 128.0f);
    EXPECT_FLOAT_EQ(rbw[5], 255.0f);

    const float* rbw_cached = nullptr;
    EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_REFERENCEBLACKWHITE,
                                    &rbw_cached),
              1);
    EXPECT_EQ(rbw_cached, rbw);
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  }

  // RGB 8-bit default ReferenceBlackWhite.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, 8), 1);

    const float* rbw = nullptr;
    EXPECT_EQ(
        TIFFGetFieldDefaulted(tif.get(), TIFFTAG_REFERENCEBLACKWHITE, &rbw), 1);
    ASSERT_NE(rbw, nullptr);
    for (int i = 0; i < 3; ++i) {
      EXPECT_FLOAT_EQ(rbw[2 * i], 0.0f);
      EXPECT_FLOAT_EQ(rbw[2 * i + 1], 255.0f);
    }
  }

  // 64-bit sample depth uses UINT64_MAX fallback branch.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB), 1);
    ASSERT_EQ(TIFFSetField(tif.get(), TIFFTAG_BITSPERSAMPLE, 64), 1);

    const float* rbw = nullptr;
    EXPECT_EQ(
        TIFFGetFieldDefaulted(tif.get(), TIFFTAG_REFERENCEBLACKWHITE, &rbw), 1);
    ASSERT_NE(rbw, nullptr);
    for (int i = 0; i < 3; ++i) {
      EXPECT_FLOAT_EQ(rbw[2 * i], 0.0f);
      EXPECT_FLOAT_EQ(rbw[2 * i + 1], static_cast<float>(UINT64_MAX));
    }
  }

  // OOM allocating 6 * sizeof(float) = 24 bytes returns 0 cleanly.
  {
    MemoryTiffStream stream;
    UniqueTiff tif = stream.Open("w");
    ASSERT_NE(tif, nullptr);
    ScopedSingleMemAllocLimit limit(tif.get(), 16);
    const float* rbw = nullptr;
    EXPECT_EQ(
        TIFFGetFieldDefaulted(tif.get(), TIFFTAG_REFERENCEBLACKWHITE, &rbw), 0);
    EXPECT_EQ(rbw, nullptr);
    EXPECT_GE(stream.diagnostics().error_count(), 1U);
  }
}

TEST(TifAuxTest, GetFieldDefaultedOnMinimalReadableTiffsAcrossFormats) {
  struct FormatCase {
    bool big_endian;
    bool big_tiff;
    std::string_view label;
  };
  constexpr auto kFormats = std::to_array<FormatCase>({
      {.big_endian = false, .big_tiff = false, .label = "LE ClassicTIFF"},
      {.big_endian = true, .big_tiff = false, .label = "BE ClassicTIFF"},
      {.big_endian = false, .big_tiff = true, .label = "LE BigTIFF"},
      {.big_endian = true, .big_tiff = true, .label = "BE BigTIFF"},
  });

  for (const FormatCase& fmt : kFormats) {
    SCOPED_TRACE(fmt.label);
    MemoryTiffStream stream(
        BuildMinimalReadableTiffBytes(fmt.big_endian, fmt.big_tiff));
    UniqueTiff tif = stream.Open("r");
    ASSERT_NE(tif, nullptr);
    EXPECT_EQ(TIFFIsBigEndian(tif.get()), fmt.big_endian ? 1 : 0);
    EXPECT_EQ(TIFFIsBigTIFF(tif.get()), fmt.big_tiff ? 1 : 0);

    uint16_t orient = 0;
    uint16_t planar = 0;
    uint16_t comp = 0;
    uint16_t bps = 0;
    uint16_t spp = 0;
    uint16_t resunit = 0;
    uint16_t samplefmt = 0;
    EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_ORIENTATION, &orient), 0);
    EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_ORIENTATION, &orient),
              1);
    EXPECT_EQ(orient, ORIENTATION_TOPLEFT);

    EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_PLANARCONFIG, &planar),
              1);
    EXPECT_EQ(planar, PLANARCONFIG_CONTIG);

    EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_COMPRESSION, &comp), 1);
    EXPECT_EQ(comp, COMPRESSION_NONE);

    EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_BITSPERSAMPLE, &bps), 1);
    EXPECT_EQ(bps, 1);

    EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_SAMPLESPERPIXEL, &spp),
              1);
    EXPECT_EQ(spp, 1);

    EXPECT_EQ(
        TIFFGetFieldDefaulted(tif.get(), TIFFTAG_RESOLUTIONUNIT, &resunit), 1);
    EXPECT_EQ(resunit, RESUNIT_INCH);

    EXPECT_EQ(
        TIFFGetFieldDefaulted(tif.get(), TIFFTAG_SAMPLEFORMAT, &samplefmt), 1);
    EXPECT_EQ(samplefmt, SAMPLEFORMAT_UINT);
    EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  }
}

TEST(TifAuxTest, GetFieldDefaultedInferredExtraSamplesWithoutTag) {
  // Build an 8-bit 4-sample RGB TIFF that omits ExtraSamples. TIFFReadDirectory
  // warns and populates td_extrasamples=1 and td_sampleinfo without setting
  // FIELD_EXTRASAMPLES, so TIFFGetFieldDefaulted falls through to its
  // EXTRASAMPLES and MATTEING cases.
  RawTiffBuilder builder(/*big_endian=*/false, /*big_tiff=*/false);
  const std::array<uint8_t, 16> rgba_pixels{};
  const uint64_t strip_off = builder.AppendBlob(rgba_pixels);
  constexpr std::array<uint16_t, 4> kBitsPerSample = {8, 8, 8, 8};
  builder.AddIfd({
      builder.MakeLong(TIFFTAG_IMAGEWIDTH, 2),
      builder.MakeLong(TIFFTAG_IMAGELENGTH, 2),
      builder.MakeShortArray(TIFFTAG_BITSPERSAMPLE, kBitsPerSample),
      builder.MakeShort(TIFFTAG_COMPRESSION, COMPRESSION_NONE),
      builder.MakeShort(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB),
      builder.MakeLong(TIFFTAG_STRIPOFFSETS, static_cast<uint32_t>(strip_off)),
      builder.MakeShort(TIFFTAG_SAMPLESPERPIXEL, 4),
      builder.MakeLong(TIFFTAG_ROWSPERSTRIP, 2),
      builder.MakeLong(TIFFTAG_STRIPBYTECOUNTS,
                       static_cast<uint32_t>(rgba_pixels.size())),
  });

  MemoryTiffStream stream(builder.Take());
  UniqueTiff tif = stream.Open("r");
  ASSERT_NE(tif, nullptr);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
  ASSERT_EQ(stream.diagnostics().warning_count(), 1U);
  EXPECT_THAT(stream.diagnostics().warnings()[0].message,
              HasSubstr("Defining non-color channels as ExtraSamples"));

  uint16_t extra_count = 0;
  const uint16_t* extra_info = nullptr;
  uint16_t matteing = 99;
  EXPECT_EQ(
      TIFFGetField(tif.get(), TIFFTAG_EXTRASAMPLES, &extra_count, &extra_info),
      0);
  EXPECT_EQ(TIFFGetField(tif.get(), TIFFTAG_MATTEING, &matteing), 0);

  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_EXTRASAMPLES, &extra_count,
                                  &extra_info),
            1);
  EXPECT_EQ(extra_count, 1);
  ASSERT_NE(extra_info, nullptr);
  EXPECT_EQ(extra_info[0], EXTRASAMPLE_UNSPECIFIED);

  matteing = 99;
  EXPECT_EQ(TIFFGetFieldDefaulted(tif.get(), TIFFTAG_MATTEING, &matteing), 1);
  EXPECT_EQ(matteing, 0);
}

TEST(TifAuxTest, ClampDoubleToFloatBoundaries) {
  EXPECT_FLOAT_EQ(_TIFFClampDoubleToFloat(0.0), 0.0f);
  EXPECT_FLOAT_EQ(_TIFFClampDoubleToFloat(-0.0), -0.0f);
  EXPECT_FLOAT_EQ(_TIFFClampDoubleToFloat(123.5), 123.5f);
  EXPECT_FLOAT_EQ(_TIFFClampDoubleToFloat(-42.25), -42.25f);
  EXPECT_FLOAT_EQ(_TIFFClampDoubleToFloat(static_cast<double>(FLT_MAX)),
                  FLT_MAX);
  EXPECT_FLOAT_EQ(_TIFFClampDoubleToFloat(-static_cast<double>(FLT_MAX)),
                  -FLT_MAX);

  EXPECT_FLOAT_EQ(_TIFFClampDoubleToFloat(static_cast<double>(FLT_MAX) * 2.0),
                  FLT_MAX);
  EXPECT_FLOAT_EQ(_TIFFClampDoubleToFloat(DBL_MAX), FLT_MAX);
  EXPECT_FLOAT_EQ(
      _TIFFClampDoubleToFloat(std::numeric_limits<double>::infinity()),
      FLT_MAX);

  EXPECT_FLOAT_EQ(_TIFFClampDoubleToFloat(-static_cast<double>(FLT_MAX) * 2.0),
                  -FLT_MAX);
  EXPECT_FLOAT_EQ(_TIFFClampDoubleToFloat(-DBL_MAX), -FLT_MAX);
  EXPECT_FLOAT_EQ(
      _TIFFClampDoubleToFloat(-std::numeric_limits<double>::infinity()),
      -FLT_MAX);

  EXPECT_TRUE(std::isnan(
      _TIFFClampDoubleToFloat(std::numeric_limits<double>::quiet_NaN())));
}

TEST(TifAuxTest, ClampDoubleToUInt32Boundaries) {
  EXPECT_EQ(_TIFFClampDoubleToUInt32(-1.0), 0U);
  EXPECT_EQ(_TIFFClampDoubleToUInt32(-0.0001), 0U);
  EXPECT_EQ(_TIFFClampDoubleToUInt32(-DBL_MAX), 0U);
  EXPECT_EQ(_TIFFClampDoubleToUInt32(-std::numeric_limits<double>::infinity()),
            0U);

  EXPECT_EQ(_TIFFClampDoubleToUInt32(0.0), 0U);
  EXPECT_EQ(_TIFFClampDoubleToUInt32(-0.0), 0U);
  EXPECT_EQ(_TIFFClampDoubleToUInt32(0.9), 0U);
  EXPECT_EQ(_TIFFClampDoubleToUInt32(42.75), 42U);
  EXPECT_EQ(_TIFFClampDoubleToUInt32(4294967295.0), 0xFFFFFFFFU);

  EXPECT_EQ(_TIFFClampDoubleToUInt32(4294967296.0), 0xFFFFFFFFU);
  EXPECT_EQ(_TIFFClampDoubleToUInt32(1e20), 0xFFFFFFFFU);
  EXPECT_EQ(_TIFFClampDoubleToUInt32(std::numeric_limits<double>::infinity()),
            0xFFFFFFFFU);
  EXPECT_EQ(_TIFFClampDoubleToUInt32(std::numeric_limits<double>::quiet_NaN()),
            0xFFFFFFFFU);
}

TEST(TifAuxTest, SeekOkSuccessBoundsAndFailures) {
  MemoryTiffStream stream;
  UniqueTiff tif = stream.Open("w");
  ASSERT_NE(tif, nullptr);

  EXPECT_EQ(_TIFFSeekOK(tif.get(), 0ULL), 1);
  EXPECT_EQ(_TIFFSeekOK(tif.get(), 64ULL), 1);
  constexpr toff_t kMaxValidSeek = std::numeric_limits<uint64_t>::max() / 2ULL;
  EXPECT_EQ(_TIFFSeekOK(tif.get(), kMaxValidSeek), 1);

  // Offsets > UINT64_MAX / 2 must fail early without invoking tif_seekproc.
  const size_t seeks_before = stream.seek_calls();
  EXPECT_EQ(_TIFFSeekOK(tif.get(), kMaxValidSeek + 1ULL), 0);
  EXPECT_EQ(_TIFFSeekOK(tif.get(), static_cast<toff_t>(-1)), 0);
  EXPECT_EQ(stream.seek_calls(), seeks_before);

  // Underlying seekproc error returns 0.
  stream.FailSeeksAfter(seeks_before);
  EXPECT_EQ(_TIFFSeekOK(tif.get(), 16ULL), 0);
  EXPECT_EQ(stream.seek_calls(), seeks_before + 1U);
  EXPECT_EQ(stream.diagnostics().error_count(), 0U);
}

}  // namespace
}  // namespace libtiff
