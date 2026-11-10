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

#ifndef THIRD_PARTY_LERC_GOOGLE_UTIL_LERC_TEST_UTIL_H_
#define THIRD_PARTY_LERC_GOOGLE_UTIL_LERC_TEST_UTIL_H_

#include <bit>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

#include "third_party/absl/types/span.h"

namespace lerc {

static_assert(std::endian::native == std::endian::little,
              "LERC tests assume little-endian host byte order (SWAPB off).");

// Deleter for malloc/calloc buffers allocated inside LercLib (such as
// fpl_EsriHuffman, fpl_Compression, and fpl_Lerc2Ext).
struct FreeDeleter {
  void operator()(void* ptr) const noexcept { std::free(ptr); }
};

using OwnedMallocBytes = std::unique_ptr<unsigned char, FreeDeleter>;
using OwnedMallocChars = std::unique_ptr<char, FreeDeleter>;

template <typename T>
inline void AppendPod(std::vector<unsigned char>& out, const T& val) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(&val);
  out.insert(out.end(), bytes, bytes + sizeof(T));
}

template <typename T>
inline void WritePodAt(std::vector<unsigned char>& out, std::size_t offset,
                       const T& val) {
  std::memcpy(&out[offset], &val, sizeof(T));
}

template <typename T>
[[nodiscard]] inline T ReadPod(absl::Span<const unsigned char> buf,
                               std::size_t offset) {
  T val{};
  std::memcpy(&val, &buf[offset], sizeof(T));
  return val;
}

// LERC2 header & v3+ checksum offsets: "Lerc2 " (6B) + version (4B) +
// checksum (4B).
inline constexpr std::size_t kLerc2VersionOffset = 6U;
inline constexpr std::size_t kLerc2ChecksumOffset =
    kLerc2VersionOffset + sizeof(int);
inline constexpr std::size_t kLerc2BodyOffset =
    kLerc2ChecksumOffset + sizeof(unsigned int);

inline constexpr std::size_t kLerc2V2BlobSizeOffset = 26U;
inline constexpr std::size_t kLerc2V3BlobSizeOffset = 30U;

inline constexpr std::size_t kLerc2NRowsOffset = kLerc2BodyOffset;
inline constexpr std::size_t kLerc2NColsOffset =
    kLerc2NRowsOffset + sizeof(int);
inline constexpr std::size_t kLerc2MicroBlockSizeOffset = 30U;
inline constexpr std::size_t kLerc2BlobSizeOffset =
    kLerc2MicroBlockSizeOffset + sizeof(int);
inline constexpr std::size_t kLerc2DataTypeOffset =
    kLerc2BlobSizeOffset + sizeof(int);

inline constexpr std::size_t kLerc2V2HeaderBytes = 58U;
inline constexpr std::size_t kLerc2V3HeaderBytes = 62U;
inline constexpr std::size_t kLerc2V4HeaderBytes = 66U;
inline constexpr std::size_t kLerc2V6HeaderBytes = 90U;
inline constexpr std::size_t kLerc2V6NumBytesMaskOffset = kLerc2V6HeaderBytes;

static_assert(kLerc2V6HeaderBytes ==
              kLerc2BodyOffset + 8U * sizeof(int) + 4U + 5U * sizeof(double));

// BitStuffer / BitStuffer2 numBitsByte bitfields: bits 6..7 encode byte width
// of numElements (0->4B, 1->2B, 2->1B, 3->invalid/0B); bit 5 flags LUT mode.
inline constexpr unsigned char kBitStufferFourByteCount = 0U << 6;
inline constexpr unsigned char kBitStufferTwoByteCount = 1U << 6;
inline constexpr unsigned char kBitStufferOneByteCount = 2U << 6;
inline constexpr unsigned char kBitStufferInvalidCount = 3U << 6;
inline constexpr unsigned char kBitStufferLutFlag = 1U << 5;

// Mirrors Lerc2's private Fletcher-32 implementation over bytes after the
// checksum field.
[[nodiscard]] inline unsigned int ComputeFletcher32(const unsigned char* data,
                                                    int len) {
  unsigned int sum1 = 0xFFFFU;
  unsigned int sum2 = 0xFFFFU;
  unsigned int words = static_cast<unsigned int>(len / 2);

  while (words > 0) {
    unsigned int tlen = (words >= 359U) ? 359U : words;
    words -= tlen;
    do {
      sum1 += static_cast<unsigned int>(*data++) << 8;
      sum2 += sum1 += *data++;
    } while (--tlen);

    sum1 = (sum1 & 0xFFFFU) + (sum1 >> 16);
    sum2 = (sum2 & 0xFFFFU) + (sum2 >> 16);
  }
  if ((len & 1) != 0) {
    sum2 += sum1 += static_cast<unsigned int>(*data) << 8;
  }
  sum1 = (sum1 & 0xFFFFU) + (sum1 >> 16);
  sum2 = (sum2 & 0xFFFFU) + (sum2 >> 16);
  return (sum2 << 16) | sum1;
}

[[nodiscard]] inline bool RecomputeChecksum(std::vector<unsigned char>& blob) {
  if (blob.size() < kLerc2BodyOffset) {
    return false;
  }
  const unsigned int csum =
      ComputeFletcher32(blob.data() + kLerc2BodyOffset,
                        static_cast<int>(blob.size() - kLerc2BodyOffset));
  WritePodAt(blob, kLerc2ChecksumOffset, csum);
  return true;
}

// BitStuffer2 header size in bytes (1B flags + 1/2/4B numElements).
[[nodiscard]] constexpr std::size_t BitStuffer2HeaderBytes(
    std::size_t count) noexcept {
  return 1U + (count < 256U ? 1U : (count < 65536U ? 2U : 4U));
}

// Padding in [0, 3] bytes to align a following payload to a 4-byte boundary.
[[nodiscard]] constexpr std::size_t AlignPadTo4(std::size_t offset) noexcept {
  return (4U - (offset % 4U)) % 4U;
}

// LERC1 CntZImage header/part layout constants & builders.
inline constexpr std::string_view kLerc1Magic = "CntZImage ";
inline constexpr std::size_t kLerc1MagicLen = kLerc1Magic.size();
inline constexpr std::size_t kLerc1VersionOffset = kLerc1MagicLen;
inline constexpr std::size_t kLerc1TypeOffset =
    kLerc1VersionOffset + sizeof(int);
inline constexpr std::size_t kLerc1HeightOffset =
    kLerc1TypeOffset + sizeof(int);
inline constexpr std::size_t kLerc1WidthOffset =
    kLerc1HeightOffset + sizeof(int);
inline constexpr std::size_t kLerc1HeaderBytes =
    kLerc1MagicLen + 4U * sizeof(int) + sizeof(double);
inline constexpr std::size_t kLerc1PartHeaderBytes =
    3U * sizeof(int) + sizeof(float);
inline constexpr std::size_t kLerc1ZTilesVOffset =
    kLerc1HeaderBytes + kLerc1PartHeaderBytes;
inline constexpr std::size_t kLerc1ZTilesHOffset =
    kLerc1ZTilesVOffset + sizeof(int);

static_assert(kLerc1MagicLen == 10);
static_assert(kLerc1HeaderBytes == 34);
static_assert(kLerc1PartHeaderBytes == 16);
static_assert(kLerc1ZTilesVOffset == 50);

inline void AppendLerc1Header(std::vector<unsigned char>& out, int version,
                              int type, int height, int width,
                              double max_z_error) {
  out.insert(out.end(), kLerc1Magic.begin(), kLerc1Magic.end());
  AppendPod(out, version);
  AppendPod(out, type);
  AppendPod(out, height);
  AppendPod(out, width);
  AppendPod(out, max_z_error);
}

inline std::size_t AppendLerc1PartHeader(std::vector<unsigned char>& out,
                                         int tiles_v, int tiles_h,
                                         int num_bytes, float max_val) {
  const std::size_t num_bytes_offset = out.size() + 2U * sizeof(int);
  AppendPod(out, tiles_v);
  AppendPod(out, tiles_h);
  AppendPod(out, num_bytes);
  AppendPod(out, max_val);
  return num_bytes_offset;
}

inline void PatchLerc1PartNumBytes(std::vector<unsigned char>& out,
                                   std::size_t num_bytes_offset,
                                   std::size_t payload_start) {
  const int n = static_cast<int>(out.size() - payload_start);
  WritePodAt(out, num_bytes_offset, n);
}

}  // namespace lerc

#endif  // THIRD_PARTY_LERC_GOOGLE_UTIL_LERC_TEST_UTIL_H_
