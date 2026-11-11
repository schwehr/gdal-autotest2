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

// Unit tests for //third_party/netcdf/libdispatch/dnotnc3.c.
//
// Covers the NC_NOTNC3_get_varm and NC_NOTNC3_put_varm dispatch stubs,
// which return NC_ENOTNC4 regardless of arguments or memory types.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/dnotnc3.c

#include <array>
#include <cstddef>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"

extern "C" {
#include "third_party/netcdf/include/ncdispatch.h"
}

namespace netcdf {
namespace {

TEST(Dnotnc3Test, GetVarmReturnsNotNc4AndLeavesBufferUntouched) {
  constexpr std::array<size_t, 2> kStart = {0, 0};
  constexpr std::array<size_t, 2> kEdges = {2, 2};
  constexpr std::array<ptrdiff_t, 2> kStride = {1, 1};
  constexpr std::array<ptrdiff_t, 2> kImap = {2, 1};
  constexpr int kSentinel = -777;
  std::array<int, 4> out = {kSentinel, kSentinel, kSentinel, kSentinel};

  constexpr std::array<nc_type, 8> kTypes = {NC_BYTE,  NC_CHAR,  NC_SHORT,
                                             NC_INT,   NC_FLOAT, NC_DOUBLE,
                                             NC_INT64, NC_STRING};
  for (const nc_type memtype : kTypes) {
    SCOPED_TRACE(memtype);
    EXPECT_EQ(NC_ENOTNC4, NC_NOTNC3_get_varm(0, 0, kStart.data(), kEdges.data(),
                                             kStride.data(), kImap.data(),
                                             out.data(), memtype));
    EXPECT_THAT(out, ::testing::ElementsAre(kSentinel, kSentinel, kSentinel,
                                            kSentinel));
  }

  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC3_get_varm(-1, -1, nullptr, nullptr, nullptr,
                                           nullptr, nullptr, NC_NAT));
}

TEST(Dnotnc3Test, PutVarmReturnsNotNc4) {
  constexpr std::array<size_t, 2> kStart = {0, 0};
  constexpr std::array<size_t, 2> kEdges = {2, 2};
  constexpr std::array<ptrdiff_t, 2> kStride = {1, 1};
  constexpr std::array<ptrdiff_t, 2> kImap = {2, 1};
  constexpr std::array<int, 4> kInput = {10, 20, 30, 40};

  constexpr std::array<nc_type, 8> kTypes = {NC_BYTE,  NC_CHAR,  NC_SHORT,
                                             NC_INT,   NC_FLOAT, NC_DOUBLE,
                                             NC_INT64, NC_STRING};
  for (const nc_type memtype : kTypes) {
    SCOPED_TRACE(memtype);
    EXPECT_EQ(NC_ENOTNC4, NC_NOTNC3_put_varm(0, 0, kStart.data(), kEdges.data(),
                                             kStride.data(), kImap.data(),
                                             kInput.data(), memtype));
  }

  EXPECT_EQ(NC_ENOTNC4, NC_NOTNC3_put_varm(-1, -1, nullptr, nullptr, nullptr,
                                           nullptr, nullptr, NC_NAT));
}

TEST(Dnotnc3Test, DispatchTableSlotsAcceptVarmStubs) {
  NC_Dispatch table{};
  table.get_varm = NC_NOTNC3_get_varm;
  table.put_varm = NC_NOTNC3_put_varm;

  constexpr size_t kStart[1] = {0};
  constexpr size_t kEdges[1] = {1};
  constexpr ptrdiff_t kStride[1] = {1};
  constexpr ptrdiff_t kImap[1] = {1};
  int value = 42;

  EXPECT_EQ(NC_ENOTNC4, table.get_varm(1, 2, kStart, kEdges, kStride, kImap,
                                       &value, NC_INT));
  EXPECT_EQ(42, value);
  EXPECT_EQ(NC_ENOTNC4, table.put_varm(1, 2, kStart, kEdges, kStride, kImap,
                                       &value, NC_INT));
}

}  // namespace
}  // namespace netcdf
