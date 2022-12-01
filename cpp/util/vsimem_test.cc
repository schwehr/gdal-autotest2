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

#include <string>
#include <vector>

#include "autotest2/cpp/util/vsimem.h"
#include "gunit.h"

namespace autotest2 {
namespace {

TEST(VsimemTest, VsiMemTempWrapper) {
  const char kFilename[] = "/vsimem/a";
  {
    const std::string kData = "b";
    VsiMemTempWrapper wrapper(kFilename, kData);
    VSILFILE *file = VSIFOpenL(kFilename, "rb");
    EXPECT_NE(nullptr, file);
    std::vector<char> buf(10, 0);
    EXPECT_EQ(1, VSIFReadL(&buf[0], 1, buf.size(), file));
    EXPECT_EQ(0, VSIFCloseL(file));
  }

  EXPECT_EQ(nullptr, VSIFOpenL(kFilename, "rb"));
}

TEST(VsimemTest, VsiMemMaybeTempWrapper) {
  const char kFilename[] = "/vsimem/c";
  const std::string kData = "d";
  {
    VsiMemMaybeTempWrapper wrapper(kFilename, kData, false);
    EXPECT_EQ(nullptr, VSIFOpenL(kFilename, "rb"));
  }

  {
    VsiMemMaybeTempWrapper wrapper(kFilename, kData, true);
    VSILFILE *file = VSIFOpenL(kFilename, "rb");
    EXPECT_NE(nullptr, file);
    std::vector<char> buf(10, 0);
    EXPECT_EQ(1, VSIFReadL(&buf[0], 1, buf.size(), file));
    EXPECT_EQ(0, VSIFCloseL(file));
  }

  EXPECT_EQ(nullptr, VSIFOpenL(kFilename, "rb"));
}

}  // namespace
}  // namespace autotest2
