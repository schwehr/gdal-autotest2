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
// Tests native VOL connector object token comparison and serialization callbacks
// (third_party/hdf5/src/H5VLnative_token.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5VLnative_token.c

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

#include <cstring>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

extern "C" {
#include "third_party/hdf5/src/H5VLnative_private.h"
}  // extern "C"

namespace hdf5 {
namespace {

TEST(H5VLnativeTokenTest, CompareTokensOrdersEqualAndDistinctValues) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  H5O_token_t tok_low{};
  H5O_token_t tok_high{};
  ASSERT_GE(H5VLnative_addr_to_token(file.get(), 100, &tok_low), 0);
  ASSERT_GE(H5VLnative_addr_to_token(file.get(), 200, &tok_high), 0);

  int cmp = -1;
  ASSERT_GE(H5Otoken_cmp(file.get(), &tok_low, &tok_low, &cmp), 0);
  EXPECT_EQ(cmp, 0);

  ASSERT_GE(H5Otoken_cmp(file.get(), &tok_low, &tok_high, &cmp), 0);
  EXPECT_LT(cmp, 0);

  ASSERT_GE(H5Otoken_cmp(file.get(), &tok_high, &tok_low, &cmp), 0);
  EXPECT_GT(cmp, 0);
}

TEST(H5VLnativeTokenTest, ZeroAddressSerializesAndDeserializesCleanly) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  H5O_token_t zero_token{};
  ASSERT_GE(H5VLnative_addr_to_token(file.get(), 0, &zero_token), 0);

  char* token_str = nullptr;
  ASSERT_GE(H5Otoken_to_str(file.get(), &zero_token, &token_str), 0);
  ASSERT_NE(token_str, nullptr);
  absl::Cleanup free_str = [&token_str] { H5free_memory(token_str); };
  EXPECT_STREQ(token_str, "0");

  H5O_token_t decoded{};
  ASSERT_GE(H5Otoken_from_str(file.get(), token_str, &decoded), 0);

  int cmp = -1;
  ASSERT_GE(H5Otoken_cmp(file.get(), &zero_token, &decoded, &cmp), 0);
  EXPECT_EQ(cmp, 0);

  haddr_t round_trip_addr = HADDR_UNDEF;
  ASSERT_GE(H5VLnative_token_to_addr(file.get(), decoded, &round_trip_addr), 0);
  EXPECT_EQ(round_trip_addr, haddr_t{0});
}

TEST(H5VLnativeTokenTest, ObjectTokenRoundTripsThroughStringViaObjects) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedGroup group(
      H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(group.valid());

  H5O_info2_t oinfo{};
  ASSERT_GE(H5Oget_info3(group.get(), &oinfo, H5O_INFO_BASIC), 0);

  char* token_str = nullptr;
  ASSERT_GE(H5Otoken_to_str(group.get(), &oinfo.token, &token_str), 0);
  ASSERT_NE(token_str, nullptr);
  absl::Cleanup free_str = [&token_str] { H5free_memory(token_str); };
  EXPECT_NE(std::string(token_str), "0");

  H5O_token_t decoded{};
  ASSERT_GE(H5Otoken_from_str(group.get(), token_str, &decoded), 0);

  int cmp = -1;
  ASSERT_GE(H5Otoken_cmp(group.get(), &oinfo.token, &decoded, &cmp), 0);
  EXPECT_EQ(cmp, 0);

  // Also test multi-digit address boundary round-trip.
  constexpr haddr_t kLargeAddr = 1234567890ULL;
  H5O_token_t large_token{};
  ASSERT_GE(H5VLnative_addr_to_token(file.get(), kLargeAddr, &large_token), 0);

  char* large_str = nullptr;
  ASSERT_GE(H5Otoken_to_str(file.get(), &large_token, &large_str), 0);
  ASSERT_NE(large_str, nullptr);
  absl::Cleanup free_large = [&large_str] { H5free_memory(large_str); };
  EXPECT_STREQ(large_str, "1234567890");

  H5O_token_t decoded_large{};
  ASSERT_GE(H5Otoken_from_str(file.get(), large_str, &decoded_large), 0);
  haddr_t out_addr = 0;
  ASSERT_GE(H5VLnative_token_to_addr(file.get(), decoded_large, &out_addr), 0);
  EXPECT_EQ(out_addr, kLargeAddr);
}

TEST(H5VLnativeTokenTest, InvalidObjectTypeFailsSerializationAndParsing) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  void* file_obj = H5VL_object(file.get());
  ASSERT_NE(file_obj, nullptr);

  H5O_token_t token{};
  ASSERT_GE(H5VLnative_addr_to_token(file.get(), 0, &token), 0);

  char* token_str = nullptr;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5VL__native_token_to_str(file_obj, H5I_DATASPACE, &token, &token_str),
        0);
  }
  EXPECT_EQ(token_str, nullptr);

  // Use a numeric token string so sscanf initializes the local addr before
  // H5VL_native_addr_to_token rejects the invalid obj_type.
  H5O_token_t out_token{};
  {
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5VL__native_str_to_token(file_obj, H5I_DATASPACE, "42", &out_token), 0);
  }
}

}  // namespace
}  // namespace hdf5
