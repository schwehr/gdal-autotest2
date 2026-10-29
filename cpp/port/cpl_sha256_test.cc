// Copyright 2018 Google Inc. All Rights Reserved.
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

// Test cpl_sha256.{cpp,h}
//
// See:
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_sha256.h
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_sha256.cpp

#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_sha256.h"

#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/strings/str_cat.h"

namespace autotest2 {
namespace {

// Create a lower case hex string representation of the bytes in data.
std::string HexStr(const GByte *data, int len) {
  std::string s;
  s.reserve(len * 2);
  for (int i = 0; i < len; ++i) {
    absl::StrAppend(&s, absl::Hex(data[i], absl::kZeroPad2));
  }
  return s;
}

TEST(CPLSha256Test, Empty) {
  constexpr char kData[] = "";
  GByte hash[CPL_SHA256_HASH_SIZE] = {};
  CPL_SHA256(reinterpret_cast<const GByte *>(kData), CPL_ARRAYSIZE(kData),
             hash);

  ASSERT_EQ("6e340b9cffb37a989ca544e6bb780a2c78901d3fb33738768511a30617afa01d",
            HexStr(hash, CPL_SHA256_HASH_SIZE));
}

TEST(CPLSha256Test, Small) {
  constexpr char kData[] = "abc123";
  GByte hash[CPL_SHA256_HASH_SIZE] = {};
  CPL_SHA256(reinterpret_cast<const GByte *>(kData), CPL_ARRAYSIZE(kData),
             hash);

  ASSERT_EQ("f85be40449b1a78776cb2eac401f06790b0fece9d0fe2a7f2fa8c0c09e210203",
            HexStr(hash, CPL_SHA256_HASH_SIZE));
}

TEST(CPLSha256Test, Large) {
  const std::string data(10000, 'x');
  GByte hash[CPL_SHA256_HASH_SIZE] = {};
  CPL_SHA256(reinterpret_cast<const GByte *>(data.c_str()), data.size(), hash);

  ASSERT_EQ("e4ee97ec252749d2096447e849628d0d7734f51700416eefbb33574bf0b3ee75",
            HexStr(hash, CPL_SHA256_HASH_SIZE));
}

TEST(CPLSha256Test, ShortKey) {
  GByte digest[CPL_SHA256_HASH_SIZE] = {};
  const std::string kShortKey = "key";
  const std::string kMessage = "The quick brown fox jumps over the lazy dog";

  CPL_HMAC_SHA256(kShortKey.c_str(), kShortKey.size(), kMessage.c_str(),
                  kMessage.size(), digest);

  ASSERT_EQ("f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8",
            HexStr(digest, CPL_SHA256_HASH_SIZE));
}

TEST(CPLSha256Test, LongKey) {
  GByte digest[CPL_SHA256_HASH_SIZE] = {};
  const std::string kShortKey =
      "mysupersupersupersupersupersupersupersupersupersupersupersupersupersup"
      "ersupersupersupersupersupersuperlongkey";
  const std::string kMessage = "msg";

  CPL_HMAC_SHA256(kShortKey.c_str(), kShortKey.size(), kMessage.c_str(),
                  kMessage.size(), digest);

  ASSERT_EQ("a3051520761ed3cb43876b35ce2dd93ac5b332dc3bad898bb32086f7ac71ffc1",
            HexStr(digest, CPL_SHA256_HASH_SIZE));
}

}  // namespace
}  // namespace autotest2
