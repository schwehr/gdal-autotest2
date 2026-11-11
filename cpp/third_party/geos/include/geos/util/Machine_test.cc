// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Unit tests for third_party/geos/include/geos/util/Machine.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/util/Machine.h

#include "third_party/geos/include/geos/util/Machine.h"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/io/ByteOrderValues.h"

namespace geos_google {
namespace {

using geos::io::ByteOrderValues;

static_assert(std::endian::native == std::endian::little ||
              std::endian::native == std::endian::big);

TEST(MachineTest, ByteOrderMatchesStdEndianNative) {
  const int order = getMachineByteOrder();
  EXPECT_TRUE(order == 0 || order == 1);

  if constexpr (std::endian::native == std::endian::little) {
    EXPECT_EQ(order, 1);
    EXPECT_EQ(order, static_cast<int>(ByteOrderValues::ENDIAN_LITTLE));
  } else if constexpr (std::endian::native == std::endian::big) {
    EXPECT_EQ(order, 0);
    EXPECT_EQ(order, static_cast<int>(ByteOrderValues::ENDIAN_BIG));
  }
}

TEST(MachineTest, ByteOrderMatchesNativeIntegerMemoryLayout) {
  const int probe = 1;
  std::array<unsigned char, sizeof(int)> probe_bytes{};
  std::memcpy(probe_bytes.data(), &probe, sizeof(int));

  const int order = getMachineByteOrder();
  EXPECT_EQ(order, static_cast<int>(probe_bytes[0]));

  constexpr std::int32_t kPattern = 0x01020304;
  std::array<unsigned char, sizeof(std::int32_t)> native_bytes{};
  std::memcpy(native_bytes.data(), &kPattern, sizeof(kPattern));

  std::array<unsigned char, sizeof(std::int32_t)> geos_bytes{};
  ByteOrderValues::putInt(kPattern, geos_bytes.data(), order);
  EXPECT_EQ(geos_bytes, native_bytes);
  EXPECT_EQ(ByteOrderValues::getInt(native_bytes.data(), order), kPattern);
}

TEST(MachineTest, RepeatedCallsAreConsistent) {
  const int first = getMachineByteOrder();
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(getMachineByteOrder(), first);
  }
}

}  // namespace
}  // namespace geos_google
