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
// Tests generic array and slice operations (mfhdf/src/array.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/mfhdf/src/array.c

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

// GoogleTest defines SUCCEED() and FAIL() as macros; undefine them before
// including HDF4 headers to avoid macro collision under --config=no_modules.
#ifdef SUCCEED
#undef SUCCEED
#endif
#ifdef FAIL
#undef FAIL
#endif

#include "third_party/hdf4/hdf/src/hdf.h"
// clang-format off
#include "third_party/hdf4/mfhdf/src/nc_priv.h"
#include "third_party/hdf4/mfhdf/src/hdf_xdr_priv.h"
// clang-format on

namespace hdf4 {
namespace {

using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::Ge;
using ::testing::Gt;
using ::testing::IsNull;
using ::testing::Ne;
using ::testing::NotNull;

std::string GetTempFilePath(std::string_view filename) {
  return (std::filesystem::path(::testing::TempDir()) / filename).string();
}

class ArrayTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_ncopts_ = ncopts;
    saved_ncerr_ = ncerr;
    ncopts = 0;
    ncerr = NC_NOERR;
  }

  void TearDown() override {
    HEclear();
    ncopts = saved_ncopts_;
    ncerr = saved_ncerr_;
  }

 private:
  int saved_ncopts_ = 0;
  int saved_ncerr_ = 0;
};

TEST_F(ArrayTest, TypeLengthsMatchExpectedSizes) {
  // On-disk sizes (NC_xtypelen)
  EXPECT_THAT(NC_xtypelen(NC_BYTE), Eq(1));
  EXPECT_THAT(NC_xtypelen(NC_CHAR), Eq(1));
  EXPECT_THAT(NC_xtypelen(NC_SHORT), Eq(2));
  EXPECT_THAT(NC_xtypelen(NC_LONG), Eq(4));
  EXPECT_THAT(NC_xtypelen(NC_FLOAT), Eq(4));
  EXPECT_THAT(NC_xtypelen(NC_DOUBLE), Eq(8));
  EXPECT_THAT(NC_xtypelen(NC_UNSPECIFIED), Eq(0));
  EXPECT_THAT(NC_xtypelen(NC_STRING), Gt(0));
  EXPECT_THAT(NC_xtypelen(NC_DIMENSION), Gt(0));
  EXPECT_THAT(NC_xtypelen(NC_VARIABLE), Gt(0));
  EXPECT_THAT(NC_xtypelen(NC_ATTRIBUTE), Gt(0));
  EXPECT_THAT(NC_xtypelen(static_cast<nc_type>(999)), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EBADTYPE));

  // In-memory sizes (NC_typelen)
  EXPECT_THAT(NC_typelen(NC_BYTE), Eq(sizeof(char)));
  EXPECT_THAT(NC_typelen(NC_CHAR), Eq(sizeof(char)));
  EXPECT_THAT(NC_typelen(NC_SHORT), Eq(sizeof(short)));
  EXPECT_THAT(NC_typelen(NC_LONG), Eq(sizeof(int32_t)));
  EXPECT_THAT(NC_typelen(NC_FLOAT), Eq(sizeof(float)));
  EXPECT_THAT(NC_typelen(NC_DOUBLE), Eq(sizeof(double)));
  EXPECT_THAT(NC_typelen(NC_STRING), Eq(sizeof(NC_string*)));
  EXPECT_THAT(NC_typelen(NC_DIMENSION), Eq(sizeof(NC_dim*)));
  EXPECT_THAT(NC_typelen(NC_VARIABLE), Eq(sizeof(NC_var*)));
  EXPECT_THAT(NC_typelen(NC_ATTRIBUTE), Eq(sizeof(NC_attr*)));
  EXPECT_THAT(NC_typelen(static_cast<nc_type>(999)), Eq(0u));

  // Public interface (nctypelen)
  EXPECT_THAT(nctypelen(NC_BYTE), Eq(static_cast<int>(sizeof(char))));
  EXPECT_THAT(nctypelen(NC_CHAR), Eq(static_cast<int>(sizeof(char))));
  EXPECT_THAT(nctypelen(NC_SHORT), Eq(static_cast<int>(sizeof(short))));
  EXPECT_THAT(nctypelen(NC_LONG), Eq(static_cast<int>(sizeof(int32_t))));
  EXPECT_THAT(nctypelen(NC_FLOAT), Eq(static_cast<int>(sizeof(float))));
  EXPECT_THAT(nctypelen(NC_DOUBLE), Eq(static_cast<int>(sizeof(double))));
  EXPECT_THAT(nctypelen(static_cast<nc_type>(999)), Eq(-1));
}

TEST_F(ArrayTest, ArrayFillInitializesAllTypes) {
  // 1. Byte
  uint8_t byte_buf[4] = {0};
  NC_arrayfill(byte_buf, sizeof(byte_buf), NC_BYTE);
  EXPECT_THAT(byte_buf[0], Eq(static_cast<uint8_t>(FILL_BYTE)));

  // 2. Char
  char char_buf[4] = {0};
  NC_arrayfill(char_buf, sizeof(char_buf), NC_CHAR);
  EXPECT_THAT(char_buf[0], Eq(FILL_CHAR));

  // 3. Short
  short short_buf[2] = {0};
  NC_arrayfill(short_buf, sizeof(short_buf), NC_SHORT);
  EXPECT_THAT(short_buf[0], Eq(static_cast<short>(FILL_SHORT)));

  // 4. Long
  int32_t long_buf[2] = {0};
  NC_arrayfill(long_buf, sizeof(long_buf), NC_LONG);
  EXPECT_THAT(long_buf[0], Eq(static_cast<int32_t>(FILL_LONG)));

  // 5. Float
  float float_buf[2] = {0.0f};
  NC_arrayfill(float_buf, sizeof(float_buf), NC_FLOAT);
  EXPECT_THAT(float_buf[0], Eq(FILL_FLOAT));

  // 6. Double
  double double_buf[2] = {0.0};
  NC_arrayfill(double_buf, sizeof(double_buf), NC_DOUBLE);
  EXPECT_THAT(double_buf[0], Eq(FILL_DOUBLE));

  // 7. Default (unknown type) fills with 0xff
  uint8_t unk_buf[4] = {0};
  NC_arrayfill(unk_buf, sizeof(unk_buf), static_cast<nc_type>(999));
  EXPECT_THAT(unk_buf[0], Eq(0xff));
}

TEST_F(ArrayTest, NewArrayLifecycleAndCopying) {
  // Zero count
  NC_array* a0 = NC_new_array(NC_LONG, 0, nullptr);
  ASSERT_THAT(a0, NotNull());
  EXPECT_THAT(a0->count, Eq(0u));
  EXPECT_THAT(a0->values, IsNull());
  EXPECT_THAT(NC_free_array(a0), Eq(SUCCEED));

  // Populated with default fill (values == nullptr)
  NC_array* afill = NC_new_array(NC_LONG, 2, nullptr);
  ASSERT_THAT(afill, NotNull());
  EXPECT_THAT(afill->count, Eq(2u));
  ASSERT_THAT(afill->values, NotNull());
  EXPECT_THAT(reinterpret_cast<int32_t*>(afill->values)[0],
              Eq(static_cast<int32_t>(FILL_LONG)));
  EXPECT_THAT(NC_free_array(afill), Eq(SUCCEED));

  // Populated with copy
  constexpr std::array<int32_t, 3> kSrc = {10, 20, 30};
  NC_array* acopy = NC_new_array(NC_LONG, 3, kSrc.data());
  ASSERT_THAT(acopy, NotNull());
  EXPECT_THAT(acopy->count, Eq(3u));
  ASSERT_THAT(acopy->values, NotNull());
  EXPECT_THAT(reinterpret_cast<int32_t*>(acopy->values)[1], Eq(20));

  // NC_copy_arrayvals
  std::array<int32_t, 3> dst = {0, 0, 0};
  NC_copy_arrayvals(reinterpret_cast<char*>(dst.data()), acopy);
  EXPECT_THAT(dst, ElementsAre(10, 20, 30));

  EXPECT_THAT(NC_free_array(acopy), Eq(SUCCEED));
}

TEST_F(ArrayTest, ReArrayRecyclesMemoryCorrectly) {
  // Allocate capacity for 4 longs (16 bytes)
  constexpr std::array<int32_t, 4> kInitial = {1, 2, 3, 4};
  NC_array* arr = NC_new_array(NC_LONG, 4, kInitial.data());
  ASSERT_THAT(arr, NotNull());
  absl::Cleanup free_arr = [arr] {
    EXPECT_THAT(NC_free_array(arr), Eq(SUCCEED));
  };

  // 1. Re-use with smaller count and new values (capacity 4 -> count 3)
  constexpr std::array<int32_t, 3> kSmaller = {100, 200, 300};
  NC_array* r1 = NC_re_array(arr, NC_LONG, 3, kSmaller.data());
  EXPECT_THAT(r1, Eq(arr));
  EXPECT_THAT(arr->count, Eq(3u));
  EXPECT_THAT(reinterpret_cast<int32_t*>(arr->values)[0], Eq(100));

  // 2. Re-use with default fill (values == nullptr) (capacity 3 -> count 2)
  NC_array* rfill = NC_re_array(arr, NC_LONG, 2, nullptr);
  EXPECT_THAT(rfill, Eq(arr));
  EXPECT_THAT(arr->count, Eq(2u));
  EXPECT_THAT(reinterpret_cast<int32_t*>(arr->values)[0],
              Eq(static_cast<int32_t>(FILL_LONG)));

  // 3. Re-use with count 0
  NC_array* r0 = NC_re_array(arr, NC_LONG, 0, nullptr);
  EXPECT_THAT(r0, Eq(arr));
  EXPECT_THAT(arr->count, Eq(0u));

  // 4. Growing beyond capacity fails and returns NULL
  EXPECT_THAT(NC_re_array(arr, NC_LONG, 10, nullptr), IsNull());
}

TEST_F(ArrayTest, FreeArrayHandlesHandleTypesAndUnknownType) {
  EXPECT_THAT(NC_free_array(nullptr), Eq(SUCCEED));

  // 1. Free NC_STRING array
  NC_array* arr_str = NC_new_array(NC_STRING, 2, nullptr);
  ASSERT_THAT(arr_str, NotNull());
  NC_string** str_ptrs = reinterpret_cast<NC_string**>(arr_str->values);
  str_ptrs[0] = NC_new_string(3, "abc");
  str_ptrs[1] = NC_new_string(4, "defg");
  ASSERT_THAT(str_ptrs[0], NotNull());
  ASSERT_THAT(str_ptrs[1], NotNull());
  EXPECT_THAT(NC_free_array(arr_str), Eq(SUCCEED));

  // 2. Free NC_DIMENSION array
  NC_array* arr_dim = NC_new_array(NC_DIMENSION, 1, nullptr);
  ASSERT_THAT(arr_dim, NotNull());
  NC_dim** dim_ptrs = reinterpret_cast<NC_dim**>(arr_dim->values);
  dim_ptrs[0] = NC_new_dim("dim0", 10);
  ASSERT_THAT(dim_ptrs[0], NotNull());
  EXPECT_THAT(NC_free_array(arr_dim), Eq(SUCCEED));

  // 3. Free NC_ATTRIBUTE array
  NC_array* arr_att = NC_new_array(NC_ATTRIBUTE, 1, nullptr);
  ASSERT_THAT(arr_att, NotNull());
  NC_attr** att_ptrs = reinterpret_cast<NC_attr**>(arr_att->values);
  constexpr char kAttVal[] = "hi";
  att_ptrs[0] = NC_new_attr("attr_hi", NC_CHAR, 2, kAttVal);
  ASSERT_THAT(att_ptrs[0], NotNull());
  EXPECT_THAT(NC_free_array(arr_att), Eq(SUCCEED));

  // 4. Free NC_VARIABLE array
  NC_array* arr_var = NC_new_array(NC_VARIABLE, 1, nullptr);
  ASSERT_THAT(arr_var, NotNull());
  NC_var** var_ptrs = reinterpret_cast<NC_var**>(arr_var->values);
  constexpr int kDims[] = {0};
  var_ptrs[0] = NC_new_var("var0", NC_LONG, 1, kDims);
  EXPECT_THAT(NC_free_array(arr_var), Eq(SUCCEED));

  // 5. Free array with unknown type triggers default advice
  NC_array* arr_unk = NC_new_array(NC_BYTE, 2, nullptr);
  ASSERT_THAT(arr_unk, NotNull());
  arr_unk->type = static_cast<nc_type>(888);
  EXPECT_THAT(NC_free_array(arr_unk), Eq(SUCCEED));
  EXPECT_THAT(ncerr, Eq(NC_EBADTYPE));
}

TEST_F(ArrayTest, IncrArrayExtendsHandleArray) {
  EXPECT_THAT(NC_incr_array(nullptr, nullptr), IsNull());
  EXPECT_THAT(ncerr, Eq(NC_EINVAL));

  NC_array* arr = NC_new_array(NC_STRING, 0, nullptr);
  ASSERT_THAT(arr, NotNull());
  absl::Cleanup free_arr = [arr] {
    EXPECT_THAT(NC_free_array(arr), Eq(SUCCEED));
  };

  NC_string* s1 = NC_new_string(4, "test");
  uint8_t* res = NC_incr_array(arr, reinterpret_cast<uint8_t*>(&s1));
  ASSERT_THAT(res, NotNull());
  EXPECT_THAT(arr->count, Eq(1u));
  EXPECT_THAT(reinterpret_cast<NC_string**>(arr->values)[0], Eq(s1));
}

TEST_F(ArrayTest, XlenArrayCalculatesSizesForAllTypes) {
  EXPECT_THAT(NC_xlen_array(nullptr), Eq(8));

  // 1. Byte / Char
  NC_array* ab = NC_new_array(NC_BYTE, 3, nullptr);
  EXPECT_THAT(NC_xlen_array(ab), Eq(12));  // 8 + 3 = 11 -> padded to 12
  EXPECT_THAT(NC_free_array(ab), Eq(SUCCEED));

  // 2. Short
  NC_array* as = NC_new_array(NC_SHORT, 3, nullptr);
  EXPECT_THAT(NC_xlen_array(as), Eq(16));  // 8 + 3*2 = 14 -> padded to 16
  EXPECT_THAT(NC_free_array(as), Eq(SUCCEED));

  // 3. Long / Float
  NC_array* al = NC_new_array(NC_LONG, 2, nullptr);
  EXPECT_THAT(NC_xlen_array(al), Eq(16));  // 8 + 2*4 = 16
  EXPECT_THAT(NC_free_array(al), Eq(SUCCEED));

  // 4. Double
  NC_array* ad = NC_new_array(NC_DOUBLE, 2, nullptr);
  EXPECT_THAT(NC_xlen_array(ad), Eq(24));  // 8 + 2*8 = 24
  EXPECT_THAT(NC_free_array(ad), Eq(SUCCEED));

  // 5. Dimension handle array
  NC_array* adim = NC_new_array(NC_DIMENSION, 1, nullptr);
  NC_dim* dim0 = NC_new_dim("dim0", 10);
  reinterpret_cast<NC_dim**>(adim->values)[0] = dim0;
  EXPECT_THAT(NC_xlen_array(adim), Gt(8));
  EXPECT_THAT(NC_free_array(adim), Eq(SUCCEED));

  // 6. Attribute handle array
  NC_array* aatt = NC_new_array(NC_ATTRIBUTE, 1, nullptr);
  constexpr char kAtt[] = "x";
  NC_attr* att0 = NC_new_attr("att0", NC_CHAR, 1, kAtt);
  reinterpret_cast<NC_attr**>(aatt->values)[0] = att0;
  EXPECT_THAT(NC_xlen_array(aatt), Gt(8));
  EXPECT_THAT(NC_free_array(aatt), Eq(SUCCEED));

  // 7. Variable handle array
  NC_array* avar = NC_new_array(NC_VARIABLE, 1, nullptr);
  constexpr int kDims[] = {0};
  NC_var* var0 = NC_new_var("var0", NC_LONG, 1, kDims);
  reinterpret_cast<NC_var**>(avar->values)[0] = var0;
  EXPECT_THAT(NC_xlen_array(avar), Gt(8));
  EXPECT_THAT(NC_free_array(avar), Eq(SUCCEED));
}

TEST_F(ArrayTest, XdrArrayEncodeOperations) {
  const std::string file_path = GetTempFilePath("array_encode.xdr");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  // 1. Encode null array and populated array
  {
    const int fd = open(file_path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
    ASSERT_THAT(fd, Ne(-1));
    XDR xdrs{};
    const int create_rc = hdf_xdr_create(&xdrs, fd, O_RDWR, XDR_ENCODE);
    if (xdrs.x_private == nullptr) {
      close(fd);
    }
    absl::Cleanup destroy_xdr = [&xdrs] { hdf_xdr_destroy(&xdrs); };
    ASSERT_THAT(create_rc, Ge(0));

    // Encoding null array creates an unspecified 0-count array
    NC_array* arr_null = nullptr;
    EXPECT_THAT(xdr_NC_array(&xdrs, &arr_null), Eq(TRUE));
    EXPECT_THAT(NC_free_array(arr_null), Eq(SUCCEED));

    // Encoding a populated array pins upstream behavior (temp_count is 0)
    int32_t longs[2] = {10, 20};
    NC_array* arr_l = NC_new_array(NC_LONG, 2, longs);
    EXPECT_THAT(xdr_NC_array(&xdrs, &arr_l), Eq(TRUE));
    EXPECT_THAT(NC_free_array(arr_l), Eq(SUCCEED));
  }

  // 2. Decode back the encoded arrays
  {
    const int fd = open(file_path.c_str(), O_RDONLY);
    ASSERT_THAT(fd, Ne(-1));
    XDR xdrs{};
    const int create_rc = hdf_xdr_create(&xdrs, fd, O_RDONLY, XDR_DECODE);
    if (xdrs.x_private == nullptr) {
      close(fd);
    }
    absl::Cleanup destroy_xdr = [&xdrs] { hdf_xdr_destroy(&xdrs); };
    ASSERT_THAT(create_rc, Ge(0));

    NC_array* dec_null = nullptr;
    EXPECT_THAT(xdr_NC_array(&xdrs, &dec_null), Eq(TRUE));
    EXPECT_THAT(dec_null, IsNull());

    NC_array* dec_l = nullptr;
    EXPECT_THAT(xdr_NC_array(&xdrs, &dec_l), Eq(TRUE));
    ASSERT_THAT(dec_l, NotNull());
    EXPECT_THAT(dec_l->type, Eq(NC_LONG));
    EXPECT_THAT(dec_l->count, Eq(0u));  // Encoded with 0 count
    EXPECT_THAT(NC_free_array(dec_l), Eq(SUCCEED));
  }

  // 3. Encode failure on read-only stream
  {
    const int fd = open(file_path.c_str(), O_RDONLY);
    ASSERT_THAT(fd, Ne(-1));
    XDR xdrs{};
    const int create_rc = hdf_xdr_create(&xdrs, fd, O_RDONLY, XDR_ENCODE);
    if (xdrs.x_private == nullptr) {
      close(fd);
    }
    absl::Cleanup destroy_xdr = [&xdrs] { hdf_xdr_destroy(&xdrs); };
    ASSERT_THAT(create_rc, Ge(0));

    NC_array* dummy = nullptr;
    EXPECT_THAT(xdr_NC_array(&xdrs, &dummy), Eq(FALSE));
    EXPECT_THAT(NC_free_array(dummy), Eq(SUCCEED));
  }
}

TEST_F(ArrayTest, XdrArrayDecodeAndFreeAcrossAllTypes) {
  const std::string file_path = GetTempFilePath("array_all_types.xdr");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  // 1. Manually serialize (type, count, elements) for all types
  {
    const int fd = open(file_path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
    ASSERT_THAT(fd, Ne(-1));
    XDR xdrs{};
    const int create_rc = hdf_xdr_create(&xdrs, fd, O_RDWR, XDR_ENCODE);
    if (xdrs.x_private == nullptr) {
      close(fd);
    }
    absl::Cleanup destroy_xdr = [&xdrs] { hdf_xdr_destroy(&xdrs); };
    ASSERT_THAT(create_rc, Ge(0));

    // NC_BYTE
    int type = NC_BYTE;
    unsigned count = 3;
    EXPECT_THAT(hdf_xdr_int(&xdrs, &type), Eq(TRUE));
    EXPECT_THAT(hdf_xdr_u_int(&xdrs, &count), Eq(TRUE));
    char bytes[3] = {1, 2, 3};
    EXPECT_THAT(hdf_xdr_opaque(&xdrs, bytes, 3), Eq(TRUE));

    // NC_SHORT
    type = NC_SHORT;
    count = 2;
    EXPECT_THAT(hdf_xdr_int(&xdrs, &type), Eq(TRUE));
    EXPECT_THAT(hdf_xdr_u_int(&xdrs, &count), Eq(TRUE));
    short shorts[2] = {100, -200};
    EXPECT_THAT(xdr_shorts(&xdrs, shorts, 2), Eq(TRUE));

    // NC_LONG
    type = NC_LONG;
    count = 2;
    EXPECT_THAT(hdf_xdr_int(&xdrs, &type), Eq(TRUE));
    EXPECT_THAT(hdf_xdr_u_int(&xdrs, &count), Eq(TRUE));
    int l0 = 100000, l1 = -200000;
    EXPECT_THAT(hdf_xdr_int(&xdrs, &l0), Eq(TRUE));
    EXPECT_THAT(hdf_xdr_int(&xdrs, &l1), Eq(TRUE));

    // NC_FLOAT
    type = NC_FLOAT;
    count = 2;
    EXPECT_THAT(hdf_xdr_int(&xdrs, &type), Eq(TRUE));
    EXPECT_THAT(hdf_xdr_u_int(&xdrs, &count), Eq(TRUE));
    float f0 = 1.5f, f1 = -2.5f;
    EXPECT_THAT(hdf_xdr_float(&xdrs, &f0), Eq(TRUE));
    EXPECT_THAT(hdf_xdr_float(&xdrs, &f1), Eq(TRUE));

    // NC_DOUBLE
    type = NC_DOUBLE;
    count = 1;
    EXPECT_THAT(hdf_xdr_int(&xdrs, &type), Eq(TRUE));
    EXPECT_THAT(hdf_xdr_u_int(&xdrs, &count), Eq(TRUE));
    double d0 = 3.14159265;
    EXPECT_THAT(hdf_xdr_double(&xdrs, &d0), Eq(TRUE));

    // NC_STRING
    type = NC_STRING;
    count = 1;
    EXPECT_THAT(hdf_xdr_int(&xdrs, &type), Eq(TRUE));
    EXPECT_THAT(hdf_xdr_u_int(&xdrs, &count), Eq(TRUE));
    NC_string* str_val = NC_new_string(4, "test");
    EXPECT_THAT(xdr_NC_string(&xdrs, &str_val), Eq(TRUE));
    EXPECT_THAT(NC_free_string(str_val), Eq(SUCCEED));

    // NC_DIMENSION
    type = NC_DIMENSION;
    count = 1;
    EXPECT_THAT(hdf_xdr_int(&xdrs, &type), Eq(TRUE));
    EXPECT_THAT(hdf_xdr_u_int(&xdrs, &count), Eq(TRUE));
    NC_dim* dim_val = NC_new_dim("dim_test", 50);
    EXPECT_THAT(xdr_NC_dim(&xdrs, &dim_val), Eq(TRUE));
    EXPECT_THAT(NC_free_dim(dim_val), Eq(SUCCEED));

    // NC_ATTRIBUTE
    type = NC_ATTRIBUTE;
    count = 1;
    EXPECT_THAT(hdf_xdr_int(&xdrs, &type), Eq(TRUE));
    EXPECT_THAT(hdf_xdr_u_int(&xdrs, &count), Eq(TRUE));
    constexpr char kAttPayload[] = "att_val";
    NC_attr* att_val = NC_new_attr("att_name", NC_CHAR, 7, kAttPayload);
    EXPECT_THAT(xdr_NC_attr(&xdrs, &att_val), Eq(TRUE));
    EXPECT_THAT(NC_free_attr(att_val), Eq(SUCCEED));

    // NC_VARIABLE
    type = NC_VARIABLE;
    count = 1;
    EXPECT_THAT(hdf_xdr_int(&xdrs, &type), Eq(TRUE));
    EXPECT_THAT(hdf_xdr_u_int(&xdrs, &count), Eq(TRUE));
    constexpr int kDimIds[] = {0};
    NC_var* var_val = NC_new_var("var_test", NC_LONG, 1, kDimIds);
    EXPECT_THAT(xdr_NC_var(&xdrs, &var_val), Eq(TRUE));
    EXPECT_THAT(NC_free_var(var_val), Eq(SUCCEED));
  }

  // 2. Decode back each array using xdr_NC_array and verify
  {
    const int fd = open(file_path.c_str(), O_RDONLY);
    ASSERT_THAT(fd, Ne(-1));
    XDR xdrs{};
    const int create_rc = hdf_xdr_create(&xdrs, fd, O_RDONLY, XDR_DECODE);
    if (xdrs.x_private == nullptr) {
      close(fd);
    }
    absl::Cleanup destroy_xdr = [&xdrs] { hdf_xdr_destroy(&xdrs); };
    ASSERT_THAT(create_rc, Ge(0));

    // NC_BYTE
    NC_array* dec_b = nullptr;
    EXPECT_THAT(xdr_NC_array(&xdrs, &dec_b), Eq(TRUE));
    ASSERT_THAT(dec_b, NotNull());
    EXPECT_THAT(dec_b->count, Eq(3u));
    EXPECT_THAT(reinterpret_cast<uint8_t*>(dec_b->values)[1], Eq(2));
    EXPECT_THAT(NC_free_array(dec_b), Eq(SUCCEED));

    // NC_SHORT
    NC_array* dec_s = nullptr;
    EXPECT_THAT(xdr_NC_array(&xdrs, &dec_s), Eq(TRUE));
    ASSERT_THAT(dec_s, NotNull());
    EXPECT_THAT(dec_s->count, Eq(2u));
    EXPECT_THAT(reinterpret_cast<short*>(dec_s->values)[1], Eq(-200));
    EXPECT_THAT(NC_free_array(dec_s), Eq(SUCCEED));

    // NC_LONG
    NC_array* dec_l = nullptr;
    EXPECT_THAT(xdr_NC_array(&xdrs, &dec_l), Eq(TRUE));
    ASSERT_THAT(dec_l, NotNull());
    EXPECT_THAT(dec_l->count, Eq(2u));
    EXPECT_THAT(reinterpret_cast<int32_t*>(dec_l->values)[0], Eq(100000));
    EXPECT_THAT(NC_free_array(dec_l), Eq(SUCCEED));

    // NC_FLOAT
    NC_array* dec_f = nullptr;
    EXPECT_THAT(xdr_NC_array(&xdrs, &dec_f), Eq(TRUE));
    ASSERT_THAT(dec_f, NotNull());
    EXPECT_THAT(dec_f->count, Eq(2u));
    EXPECT_THAT(reinterpret_cast<float*>(dec_f->values)[0], Eq(1.5f));
    EXPECT_THAT(NC_free_array(dec_f), Eq(SUCCEED));

    // NC_DOUBLE
    NC_array* dec_d = nullptr;
    EXPECT_THAT(xdr_NC_array(&xdrs, &dec_d), Eq(TRUE));
    ASSERT_THAT(dec_d, NotNull());
    EXPECT_THAT(dec_d->count, Eq(1u));
    EXPECT_THAT(reinterpret_cast<double*>(dec_d->values)[0], Eq(3.14159265));
    // Test XDR_FREE on decoded array
    xdrs.x_op = XDR_FREE;
    EXPECT_THAT(xdr_NC_array(&xdrs, &dec_d), Eq(TRUE));
    xdrs.x_op = XDR_DECODE;

    // NC_STRING
    NC_array* dec_str = nullptr;
    EXPECT_THAT(xdr_NC_array(&xdrs, &dec_str), Eq(TRUE));
    ASSERT_THAT(dec_str, NotNull());
    EXPECT_THAT(dec_str->count, Eq(1u));
    EXPECT_THAT(reinterpret_cast<NC_string**>(dec_str->values)[0]->values,
                testing::StrEq("test"));
    EXPECT_THAT(NC_free_array(dec_str), Eq(SUCCEED));

    // NC_DIMENSION
    NC_array* dec_dim = nullptr;
    EXPECT_THAT(xdr_NC_array(&xdrs, &dec_dim), Eq(TRUE));
    ASSERT_THAT(dec_dim, NotNull());
    EXPECT_THAT(dec_dim->count, Eq(1u));
    EXPECT_THAT(reinterpret_cast<NC_dim**>(dec_dim->values)[0]->size, Eq(50));
    EXPECT_THAT(NC_free_array(dec_dim), Eq(SUCCEED));

    // NC_ATTRIBUTE
    NC_array* dec_att = nullptr;
    EXPECT_THAT(xdr_NC_array(&xdrs, &dec_att), Eq(TRUE));
    ASSERT_THAT(dec_att, NotNull());
    EXPECT_THAT(dec_att->count, Eq(1u));
    EXPECT_THAT(reinterpret_cast<NC_attr**>(dec_att->values)[0]->name->values,
                testing::StrEq("att_name"));
    EXPECT_THAT(NC_free_array(dec_att), Eq(SUCCEED));

    // NC_VARIABLE
    NC_array* dec_var = nullptr;
    EXPECT_THAT(xdr_NC_array(&xdrs, &dec_var), Eq(TRUE));
    ASSERT_THAT(dec_var, NotNull());
    EXPECT_THAT(dec_var->count, Eq(1u));
    EXPECT_THAT(reinterpret_cast<NC_var**>(dec_var->values)[0]->name->values,
                testing::StrEq("var_test"));
    EXPECT_THAT(NC_free_array(dec_var), Eq(SUCCEED));
  }
}

TEST_F(ArrayTest, XdrArrayDecodeFailures) {
  // 1. Truncated stream failing in loop (NC_LONG count = 2 with only 1 long in
  // payload)
  {
    const std::string file_path = GetTempFilePath("array_fail_loop.xdr");
    absl::Cleanup remove_file = [&file_path] {
      std::filesystem::remove(file_path);
    };

    {
      const int fd =
          open(file_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
      ASSERT_THAT(fd, Ne(-1));
      // type = NC_LONG (4), count = 2, only 1 long payload (4 bytes: 0, 0, 0,
      // 42)
      const uint8_t data[] = {0, 0, 0, 4, 0, 0, 0, 2, 0, 0, 0, 42};
      EXPECT_THAT(write(fd, data, sizeof(data)), Eq(12));
      close(fd);
    }

    const int fd = open(file_path.c_str(), O_RDONLY);
    ASSERT_THAT(fd, Ne(-1));
    XDR xdrs{};
    const int create_rc = hdf_xdr_create(&xdrs, fd, O_RDONLY, XDR_DECODE);
    if (xdrs.x_private == nullptr) {
      close(fd);
    }
    absl::Cleanup destroy_xdr = [&xdrs] { hdf_xdr_destroy(&xdrs); };
    ASSERT_THAT(create_rc, Ge(0));

    NC_array* arr = nullptr;
    EXPECT_THAT(xdr_NC_array(&xdrs, &arr), Eq(FALSE));
    EXPECT_THAT(ncerr, Eq(NC_EXDR));
    if (arr != nullptr) {
      EXPECT_THAT(NC_free_array(arr), Eq(SUCCEED));
    }
  }

  // 2. Truncated stream failing in func (NC_BYTE count = 4 with only 1 byte
  // payload)
  {
    const std::string file_path = GetTempFilePath("array_fail_func.xdr");
    absl::Cleanup remove_file = [&file_path] {
      std::filesystem::remove(file_path);
    };

    {
      const int fd =
          open(file_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
      ASSERT_THAT(fd, Ne(-1));
      // type = NC_BYTE (1), count = 4, only 1 byte payload ('x')
      const uint8_t data[] = {0, 0, 0, 1, 0, 0, 0, 4, 'x'};
      EXPECT_THAT(write(fd, data, sizeof(data)), Eq(9));
      close(fd);
    }

    const int fd = open(file_path.c_str(), O_RDONLY);
    ASSERT_THAT(fd, Ne(-1));
    XDR xdrs{};
    const int create_rc = hdf_xdr_create(&xdrs, fd, O_RDONLY, XDR_DECODE);
    if (xdrs.x_private == nullptr) {
      close(fd);
    }
    absl::Cleanup destroy_xdr = [&xdrs] { hdf_xdr_destroy(&xdrs); };
    ASSERT_THAT(create_rc, Ge(0));

    NC_array* arr = nullptr;
    EXPECT_THAT(xdr_NC_array(&xdrs, &arr), Eq(FALSE));
    EXPECT_THAT(ncerr, Eq(NC_EXDR));
    if (arr != nullptr) {
      EXPECT_THAT(NC_free_array(arr), Eq(SUCCEED));
    }
  }
}

TEST_F(ArrayTest, XdrArrayUnknownTypeFails) {
  const std::string file_path = GetTempFilePath("array_unk.xdr");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  // Write type = 777 (unknown), count = 2
  {
    const int fd = open(file_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    ASSERT_THAT(fd, Ne(-1));
    const uint8_t data[] = {0, 0, 3, 9, 0, 0, 0, 2};  // 777, 2
    EXPECT_THAT(write(fd, data, sizeof(data)), Eq(8));
    close(fd);
  }

  const int fd = open(file_path.c_str(), O_RDONLY);
  ASSERT_THAT(fd, Ne(-1));
  XDR xdrs{};
  const int create_rc = hdf_xdr_create(&xdrs, fd, O_RDONLY, XDR_DECODE);
  if (xdrs.x_private == nullptr) {
    close(fd);
  }
  absl::Cleanup destroy_xdr = [&xdrs] { hdf_xdr_destroy(&xdrs); };
  ASSERT_THAT(create_rc, Ge(0));

  NC_array* arr = nullptr;
  EXPECT_THAT(xdr_NC_array(&xdrs, &arr), Eq(FALSE));
  EXPECT_THAT(ncerr, Eq(NC_EBADTYPE));
  if (arr != nullptr) {
    EXPECT_THAT(NC_free_array(arr), Eq(SUCCEED));
  }
}

// TODO(schwehr): Uncovered lines in array.c (~91.9% covered) are defensive OOM
// and XDR allocation/encode failure branches.

}  // namespace
}  // namespace hdf4
