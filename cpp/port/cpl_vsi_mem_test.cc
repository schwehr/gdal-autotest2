// Copyright 2017 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stddef.h>

#include <memory>
#include <vector>

#include "gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "port/cpl_conv.h"
#include "port/cpl_port.h"
#include "port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr int kSuccess = 0;

TEST(VSIBufferTest, NoTake) {
  constexpr char kFilename[] = "/vsimem/a";
  std::vector<GByte> src = {0x00, 0xff};

  // Do not take ownership.
  VSILFILE *f = VSIFileFromMemBuffer(kFilename, &src[0], src.size(), false);
  ASSERT_NE(nullptr, f);
  auto closer = absl::MakeCleanup([f] { VSIFCloseL(f); });

  VSIStatBufL stat_buf;
  ASSERT_EQ(kSuccess, VSIStatL(kFilename, &stat_buf));
  ASSERT_EQ(2, stat_buf.st_size);

  vsi_l_offset data_length;
  char *tmp = reinterpret_cast<char *>(
      VSIGetMemFileBuffer(kFilename, &data_length, false));
  ASSERT_NE(nullptr, tmp);
  EXPECT_EQ(2, data_length);
  EXPECT_EQ(0, tmp[0]);
  EXPECT_EQ(0xff, tmp[1]);
}

TEST(VSIBufferTest, TakeOwnershipWriting) {
  constexpr char kFilename[] = "/vsimem/b";
  constexpr size_t size = 2;
  std::unique_ptr<GByte[]> src(new GByte[size]);
  src[0] = 0x00;
  src[1] = 0xaa;

  VSILFILE *f = VSIFileFromMemBuffer(kFilename, src.get(), size, true);
  ASSERT_NE(nullptr, f);
  auto closer = absl::MakeCleanup([f] { VSIFCloseL(f); });

  vsi_l_offset data_length;
  char *tmp = reinterpret_cast<char *>(
      VSIGetMemFileBuffer(kFilename, &data_length, false));
  ASSERT_NE(nullptr, tmp);
  EXPECT_EQ(2, data_length);
  EXPECT_EQ(0, tmp[0]);
  EXPECT_EQ(0xaa, tmp[1]);
}

TEST(VSIBufferTest, TakeOwnershipReading) {
  constexpr char kFilename[] = "/vsimem/c";
  std::vector<GByte> src = {0x00, 0x08};

  VSILFILE *f = VSIFOpenL(kFilename, "wb");
  ASSERT_NE(nullptr, f);
  ASSERT_EQ(1, VSIFWriteL(&src[0], src.size(), 1, f));
  ASSERT_EQ(kSuccess, VSIFCloseL(f));

  vsi_l_offset data_length;
  char *tmp = reinterpret_cast<char *>(
      VSIGetMemFileBuffer(kFilename, &data_length, true));
  ASSERT_NE(nullptr, tmp);
  EXPECT_EQ(2, data_length);
  EXPECT_EQ(0, tmp[0]);
  EXPECT_EQ(0x08, tmp[1]);
  CPLFree(tmp);
}

}  // namespace
}  // namespace autotest2
