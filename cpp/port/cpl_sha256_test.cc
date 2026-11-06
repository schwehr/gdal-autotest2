// Copyright 2018 Google LLC
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
// Tests SHA256 and HMAC-SHA256 helpers (cpl_sha256.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_sha256.h
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_sha256.cpp

#include "third_party/gdal/port/cpl_sha256.h"

#include <cstddef>
#include <format>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

// Create a lower case hex string representation of the bytes in data.
std::string HexStr(const GByte* data, size_t len) {
  std::string s;
  s.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    s += std::format("{:02x}", data[i]);
  }
  return s;
}

TEST(CPLSha256Test, Sha256KnownVectors) {
  struct TestCase {
    std::string_view input;
    std::string_view expected_hex;
  };
  // Includes both standard NUL-free FIPS 180-4 vectors and explicit NUL-byte
  // vectors (preserving the legacy CPL_ARRAYSIZE trailing-NUL hashes).
  constexpr TestCase kCases[] = {
      {.input = "",
       .expected_hex =
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {.input = "abc",
       .expected_hex =
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
      {.input = "abc123",
       .expected_hex =
           "6ca13d52ca70c883e0f0bb101e425a89e8624de51db2d2392593af6a84118090"},
      {.input = {"\0", 1},
       .expected_hex =
           "6e340b9cffb37a989ca544e6bb780a2c78901d3fb33738768511a30617afa01d"},
      {.input = {"abc123\0", 7},
       .expected_hex =
           "f85be40449b1a78776cb2eac401f06790b0fece9d0fe2a7f2fa8c0c09e210203"},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.expected_hex);
    GByte hash[CPL_SHA256_HASH_SIZE] = {};
    CPL_SHA256(reinterpret_cast<const GByte*>(tc.input.data()), tc.input.size(),
               hash);
    EXPECT_EQ(tc.expected_hex, HexStr(hash, CPL_SHA256_HASH_SIZE));
  }
}

TEST(CPLSha256Test, Large) {
  const std::string data(10000, 'x');
  GByte hash[CPL_SHA256_HASH_SIZE] = {};
  CPL_SHA256(reinterpret_cast<const GByte*>(data.data()), data.size(), hash);

  EXPECT_EQ("e4ee97ec252749d2096447e849628d0d7734f51700416eefbb33574bf0b3ee75",
            HexStr(hash, CPL_SHA256_HASH_SIZE));
}

TEST(CPLSha256Test, HmacSha256) {
  // CPL_SHA256Update casts input buffers > 63 bytes directly to const GUInt32*,
  // so long string literals in .rodata must be 4-byte aligned to avoid UBSan
  // misaligned-pointer traps.
  alignas(GUInt32) static constexpr char kLongKey[] =
      "mysupersupersupersupersupersupersupersupersupersupersupersupersuper"
      "supersupersupersupersupersupersuperlongkey";
  struct HmacCase {
    std::string_view key;
    std::string_view message;
    std::string_view expected_hex;
  };
  constexpr HmacCase kCases[] = {
      {.key = "key",
       .message = "The quick brown fox jumps over the lazy dog",
       .expected_hex =
           "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8"},
      {.key = kLongKey,
       .message = "msg",
       .expected_hex =
           "a3051520761ed3cb43876b35ce2dd93ac5b332dc3bad898bb32086f7ac71ffc1"},
  };
  for (const auto& tc : kCases) {
    GByte digest[CPL_SHA256_HASH_SIZE] = {};
    CPL_HMAC_SHA256(tc.key.data(), tc.key.size(), tc.message.data(),
                    tc.message.size(), digest);
    EXPECT_EQ(tc.expected_hex, HexStr(digest, CPL_SHA256_HASH_SIZE));
  }
}

}  // namespace
}  // namespace autotest2
