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

#ifndef THIRD_PARTY_LERC_GOOGLE_UTIL_LERC_PRIVATE_TEST_UTIL_H_
#define THIRD_PARTY_LERC_GOOGLE_UTIL_LERC_PRIVATE_TEST_UTIL_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "third_party/absl/types/span.h"
#include "third_party/lerc/LercLib/BitStuffer2.h"
#include "third_party/lerc/LercLib/Defines.h"
#include "third_party/lerc/google/util/lerc_test_util.h"

namespace lerc {

struct AlignedBitStuffer2Stream {
  std::vector<std::uint32_t> storage;
  std::size_t start_offset = 0;
  std::size_t byte_length = 0;

  [[nodiscard]] const LercNS::Byte* data() const noexcept {
    return reinterpret_cast<const LercNS::Byte*>(storage.data()) + start_offset;
  }
};

// Encodes `values` with BitStuffer2 v2 (compatible with LERC1 BitStuffer::read
// on little-endian), aligning the post-header payload cursor to 4 bytes so
// BitStuff_Before_Lerc2v3 stays UBSan/ARM-safe, and reserving >= 4 bytes of
// tail headroom because BitStuff_Before_Lerc2v3 memset-zeroes full uint32 words
// up to 3 bytes past ComputeNumBytesNeededSimple.
[[nodiscard]] inline bool EncodeSimpleV2Aligned(
    absl::Span<const unsigned int> values, AlignedBitStuffer2Stream& out) {
  const unsigned int num_elem = static_cast<unsigned int>(values.size());
  const unsigned int max_elem =
      values.empty() ? 0U : *std::max_element(values.begin(), values.end());
  const std::size_t hdr_bytes = BitStuffer2HeaderBytes(num_elem);
  const std::size_t needed_bytes =
      LercNS::BitStuffer2::ComputeNumBytesNeededSimple(num_elem, max_elem);

  out.start_offset = AlignPadTo4(hdr_bytes);
  out.byte_length = needed_bytes;
  out.storage.assign((out.start_offset + needed_bytes + sizeof(std::uint32_t) -
                      1U) / sizeof(std::uint32_t) +
                         2U,
                     0U);

  LercNS::Byte* start =
      reinterpret_cast<LercNS::Byte*>(out.storage.data()) + out.start_offset;
  LercNS::Byte* dst = start;
  const std::vector<unsigned int> vec(values.begin(), values.end());
  LercNS::BitStuffer2 encoder;
  if (!encoder.EncodeSimple(&dst, vec, /*lerc2Version=*/2)) {
    return false;
  }
  return static_cast<std::size_t>(dst - start) == needed_bytes;
}

[[nodiscard]] inline bool AppendEncodedSimpleV2(
    std::vector<LercNS::Byte>& out, absl::Span<const unsigned int> values) {
  AlignedBitStuffer2Stream stream;
  if (!EncodeSimpleV2Aligned(values, stream)) {
    return false;
  }
  out.insert(out.end(), stream.data(), stream.data() + stream.byte_length);
  return true;
}

}  // namespace lerc

#endif  // THIRD_PARTY_LERC_GOOGLE_UTIL_LERC_PRIVATE_TEST_UTIL_H_
