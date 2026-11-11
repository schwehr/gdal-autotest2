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

// Unit tests for //third_party/netcdf/ncgen/cvt.c.
//
// Exercises the full convert1() type conversion matrix (numeric, char, string,
// opaque, nil, enum constants, fill values, and fatal semerror paths), as well
// as convertstringtochars() and convertFilterID().
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncgen/cvt.c

#include <setjmp.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/ncgen_main_shim.h"

namespace netcdf {
namespace {

void CleanupNcgenGlobals() {
  if (filldatalist != nullptr) {
    filldatalist->readonly = 0;
    freedatalist(filldatalist);
    filldatalist = nullptr;
  }
  if (codebuffer != nullptr) {
    bbFree(codebuffer);
    codebuffer = nullptr;
  }
  if (stmt != nullptr) {
    bbFree(stmt);
    stmt = nullptr;
  }
}

class CvtTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CleanupNcgenGlobals();
    ncgen_shim_reset_globals();
    static char kTestCdlName[] = "cvt_test.cdl";
    cdlname = kTestCdlName;
  }

  void TearDown() override {
    ncgen_shim_set_finalize_jmp(nullptr, nullptr);
    cdlname = nullptr;
    CleanupNcgenGlobals();
  }
};

constexpr nc_type kNumericTargetTypes[] = {
    NC_BYTE, NC_UBYTE, NC_SHORT,  NC_USHORT, NC_INT,
    NC_UINT, NC_INT64, NC_UINT64, NC_FLOAT,  NC_DOUBLE,
};

void ExpectConvertedNumber(const NCConstant& con, int expected_int) {
  switch (con.nctype) {
    case NC_BYTE:
      EXPECT_EQ(static_cast<unsigned char>(expected_int), con.value.uint8v);
      break;
    case NC_UBYTE:
      EXPECT_EQ(static_cast<unsigned char>(expected_int), con.value.uint8v);
      break;
    case NC_SHORT:
      EXPECT_EQ(static_cast<short>(expected_int), con.value.int16v);
      break;
    case NC_USHORT:
      EXPECT_EQ(static_cast<unsigned short>(expected_int), con.value.uint16v);
      break;
    case NC_INT:
      EXPECT_EQ(expected_int, con.value.int32v);
      break;
    case NC_UINT:
      EXPECT_EQ(static_cast<unsigned int>(expected_int), con.value.uint32v);
      break;
    case NC_INT64:
      EXPECT_EQ(static_cast<long long>(expected_int), con.value.int64v);
      break;
    case NC_UINT64:
      EXPECT_EQ(static_cast<unsigned long long>(expected_int),
                con.value.uint64v);
      break;
    case NC_FLOAT:
      EXPECT_NEAR(static_cast<float>(expected_int), con.value.floatv, 1.0f);
      break;
    case NC_DOUBLE:
      EXPECT_NEAR(static_cast<double>(expected_int), con.value.doublev, 1.0);
      break;
    default:
      ADD_FAILURE() << "Unexpected target type: " << con.nctype;
      break;
  }
}

TEST_F(CvtTest, Convert1HandlesFillValuesAcrossTargetTypes) {
  NCConstant src{};
  src.nctype = NC_FILLVALUE;
  src.lineno = 11;

  // Target == NC_FILLVALUE is a no-op except lineno propagation.
  NCConstant fill_dst{};
  fill_dst.nctype = NC_FILLVALUE;
  convert1(&src, &fill_dst);
  EXPECT_EQ(NC_FILLVALUE, fill_dst.nctype);
  EXPECT_EQ(11, fill_dst.lineno);

  // Primitive numeric / char targets.
  NCConstant int_dst{};
  int_dst.nctype = NC_INT;
  convert1(&src, &int_dst);
  EXPECT_EQ(NC_FILL_INT, int_dst.value.int32v);

  NCConstant char_dst{};
  char_dst.nctype = NC_CHAR;
  convert1(&src, &char_dst);
  EXPECT_EQ(NC_FILL_CHAR, char_dst.value.charv);

  // Heap-allocated fill values for NC_STRING and NC_OPAQUE.
  NCConstant str_dst{};
  str_dst.nctype = NC_STRING;
  const auto clean_str =
      absl::MakeCleanup([&str_dst] { clearconstant(&str_dst); });
  convert1(&src, &str_dst);
  ASSERT_NE(str_dst.value.stringv.stringv, nullptr);
  EXPECT_STREQ(NC_FILL_STRING, str_dst.value.stringv.stringv);

  NCConstant opq_dst{};
  opq_dst.nctype = NC_OPAQUE;
  const auto clean_opq =
      absl::MakeCleanup([&opq_dst] { clearconstant(&opq_dst); });
  convert1(&src, &opq_dst);
  ASSERT_NE(opq_dst.value.opaquev.stringv, nullptr);
  EXPECT_EQ(2U, opq_dst.value.opaquev.len);
  EXPECT_STREQ("00", opq_dst.value.opaquev.stringv);
}

TEST_F(CvtTest, Convert1HandlesEnumConstantsAndRejectsConversionToEnum) {
  NCConstant enum_backing{};
  enum_backing.nctype = NC_INT;
  enum_backing.lineno = 20;
  enum_backing.value.int32v = 77;

  Symbol enum_sym{};
  enum_sym.objectclass = NC_TYPE;
  enum_sym.subclass = NC_ECONST;
  enum_sym.typ.econst = &enum_backing;

  NCConstant src{};
  src.nctype = NC_ECONST;
  src.lineno = 9;
  src.value.enumv = &enum_sym;

  NCConstant enum_dst{};
  enum_dst.nctype = NC_ECONST;
  convert1(&src, &enum_dst);
  EXPECT_EQ(&enum_sym, enum_dst.value.enumv);
  EXPECT_EQ(9, enum_dst.lineno);

  NCConstant dbl_dst{};
  dbl_dst.nctype = NC_DOUBLE;
  convert1(&src, &dbl_dst);
  EXPECT_DOUBLE_EQ(77.0, dbl_dst.value.doublev);
  EXPECT_EQ(20, dbl_dst.lineno);

  // Converting a non-enum constant to NC_ECONST invokes semerror().
  NCConstant int_src{};
  int_src.nctype = NC_INT;
  int_src.value.int32v = 5;
  NCConstant bad_dst{};
  bad_dst.nctype = NC_ECONST;

  sigjmp_buf jmp_env{};
  int jmp_ret = -1;
  ncgen_shim_set_finalize_jmp(&jmp_env, &jmp_ret);
  const auto disarm =
      absl::MakeCleanup([] { ncgen_shim_set_finalize_jmp(nullptr, nullptr); });

  volatile bool jumped = false;
  if (sigsetjmp(jmp_env, 1) == 0) {
    convert1(&int_src, &bad_dst);
  } else {
    jumped = true;
  }
  EXPECT_TRUE(jumped);
  EXPECT_EQ(1, jmp_ret);
}

TEST_F(CvtTest, Convert1CoversCharByteUbyteAndNumericSources) {
  auto check_all_targets = [](const NCConstant& src, bool allow_char_dst,
                              int expected_int, std::string_view expected_str) {
    NCConstant src_copy = src;
    if (allow_char_dst) {
      NCConstant char_dst{};
      char_dst.nctype = NC_CHAR;
      convert1(&src_copy, &char_dst);
      EXPECT_EQ(static_cast<char>(expected_int), char_dst.value.charv);
    }
    for (nc_type dst_type : kNumericTargetTypes) {
      NCConstant dst{};
      dst.nctype = dst_type;
      convert1(&src_copy, &dst);
      ExpectConvertedNumber(dst, expected_int);
    }
    NCConstant str_dst{};
    str_dst.nctype = NC_STRING;
    const auto clean_str =
        absl::MakeCleanup([&str_dst] { clearconstant(&str_dst); });
    convert1(&src_copy, &str_dst);
    ASSERT_NE(str_dst.value.stringv.stringv, nullptr);
    EXPECT_EQ(expected_str, std::string_view(str_dst.value.stringv.stringv,
                                             str_dst.value.stringv.len));
  };

  {
    SCOPED_TRACE("NC_CHAR");
    NCConstant src{};
    src.nctype = NC_CHAR;
    src.value.charv = 'A';
    check_all_targets(src, /*allow_char_dst=*/true, 65, "A");
  }
  {
    SCOPED_TRACE("NC_BYTE");
    NCConstant src{};
    src.nctype = NC_BYTE;
    src.value.uint8v = 42;
    check_all_targets(src, /*allow_char_dst=*/true, 42, "42");
  }
  {
    SCOPED_TRACE("NC_UBYTE");
    NCConstant src{};
    src.nctype = NC_UBYTE;
    src.value.uint8v = 42;
    check_all_targets(src, /*allow_char_dst=*/true, 42, "42");
  }
  {
    SCOPED_TRACE("NC_USHORT");
    NCConstant src{};
    src.nctype = NC_USHORT;
    src.value.uint16v = 42;
    check_all_targets(src, /*allow_char_dst=*/false, 42, "42");
  }
  {
    SCOPED_TRACE("NC_UINT");
    NCConstant src{};
    src.nctype = NC_UINT;
    src.value.uint32v = 42U;
    check_all_targets(src, /*allow_char_dst=*/false, 42, "42");
  }
  {
    SCOPED_TRACE("NC_UINT64");
    NCConstant src{};
    src.nctype = NC_UINT64;
    src.value.uint64v = 42ULL;
    check_all_targets(src, /*allow_char_dst=*/false, 42, "42");
  }
  {
    SCOPED_TRACE("NC_SHORT");
    NCConstant src{};
    src.nctype = NC_SHORT;
    src.value.int16v = 42;
    check_all_targets(src, /*allow_char_dst=*/false, 42, "42");
  }
  {
    SCOPED_TRACE("NC_INT");
    NCConstant src{};
    src.nctype = NC_INT;
    src.value.int32v = 42;
    check_all_targets(src, /*allow_char_dst=*/false, 42, "42");
  }
  {
    SCOPED_TRACE("NC_INT64");
    NCConstant src{};
    src.nctype = NC_INT64;
    src.value.int64v = 42LL;
    check_all_targets(src, /*allow_char_dst=*/false, 42, "42");
  }
  {
    SCOPED_TRACE("NC_FLOAT");
    NCConstant src{};
    src.nctype = NC_FLOAT;
    src.value.floatv = 42.5f;
    check_all_targets(src, /*allow_char_dst=*/false, 42, "42.5");

    NCConstant nan_src{};
    nan_src.nctype = NC_FLOAT;
    nan_src.value.floatv = std::numeric_limits<float>::quiet_NaN();
    NCConstant dbl_dst{};
    dbl_dst.nctype = NC_DOUBLE;
    convert1(&nan_src, &dbl_dst);
    EXPECT_TRUE(std::isnan(dbl_dst.value.doublev));
  }
  {
    SCOPED_TRACE("NC_DOUBLE");
    NCConstant src{};
    src.nctype = NC_DOUBLE;
    src.value.doublev = 42.5;
    check_all_targets(src, /*allow_char_dst=*/false, 42, "42.5");

    NCConstant nan_src{};
    nan_src.nctype = NC_DOUBLE;
    nan_src.value.doublev = std::numeric_limits<double>::quiet_NaN();
    NCConstant flt_dst{};
    flt_dst.nctype = NC_FLOAT;
    convert1(&nan_src, &flt_dst);
    EXPECT_TRUE(std::isnan(flt_dst.value.floatv));
  }
}

TEST_F(CvtTest, Convert1CoversStringOpaqueNilAndIllegalConversions) {
  char num_str[] = "42";
  NCConstant str_src{};
  str_src.nctype = NC_STRING;
  str_src.value.stringv.len = 2;
  str_src.value.stringv.stringv = num_str;

  NCConstant char_dst{};
  char_dst.nctype = NC_CHAR;
  convert1(&str_src, &char_dst);
  EXPECT_EQ('4', char_dst.value.charv);

  for (nc_type dst_type : kNumericTargetTypes) {
    NCConstant dst{};
    dst.nctype = dst_type;
    convert1(&str_src, &dst);
    ExpectConvertedNumber(dst, 42);
  }

  // NC_STRING -> NC_STRING preserves embedded NUL bytes.
  char embedded_nul[] = {'a', 'b', '\0', 'c', 'd'};
  NCConstant nul_src{};
  nul_src.nctype = NC_STRING;
  nul_src.value.stringv.len = 5;
  nul_src.value.stringv.stringv = embedded_nul;
  NCConstant str_dst{};
  str_dst.nctype = NC_STRING;
  const auto clean_str =
      absl::MakeCleanup([&str_dst] { clearconstant(&str_dst); });
  convert1(&nul_src, &str_dst);
  ASSERT_EQ(5U, str_dst.value.stringv.len);
  ASSERT_NE(str_dst.value.stringv.stringv, nullptr);
  EXPECT_EQ(0, std::memcmp(str_dst.value.stringv.stringv, embedded_nul, 5));
  EXPECT_EQ('\0', str_dst.value.stringv.stringv[5]);

  // NC_OPAQUE source with 8 decoded bytes (16 hex digits) keeps up to 8-byte
  // scalar reads strictly in bounds and naturally aligned.
  char hex_bytes[] = "3f01013f3f01013f";
  NCConstant opq_src{};
  opq_src.nctype = NC_OPAQUE;
  opq_src.value.opaquev.len = 16;
  opq_src.value.opaquev.stringv = hex_bytes;

  NCConstant opq_char{};
  opq_char.nctype = NC_CHAR;
  convert1(&opq_src, &opq_char);
  EXPECT_EQ(static_cast<char>(0x3f), opq_char.value.charv);

  for (nc_type dst_type : kNumericTargetTypes) {
    NCConstant dst{};
    dst.nctype = dst_type;
    convert1(&opq_src, &dst);
    EXPECT_EQ(dst_type, dst.nctype);
  }

  NCConstant opq_dst{};
  opq_dst.nctype = NC_OPAQUE;
  const auto clean_opq =
      absl::MakeCleanup([&opq_dst] { clearconstant(&opq_dst); });
  convert1(&opq_src, &opq_dst);
  EXPECT_EQ(16U, opq_dst.value.opaquev.len);
  ASSERT_NE(opq_dst.value.opaquev.stringv, nullptr);
  EXPECT_STREQ("3f01013f3f01013f", opq_dst.value.opaquev.stringv);

  // NC_NIL source.
  NCConstant nil_src{};
  nil_src.nctype = NC_NIL;
  NCConstant nil_dst{};
  nil_dst.nctype = NC_NIL;
  convert1(&nil_src, &nil_dst);
  EXPECT_EQ(NC_NIL, nil_dst.nctype);

  NCConstant nil_str_dst{};
  nil_str_dst.nctype = NC_STRING;
  convert1(&nil_src, &nil_str_dst);
  EXPECT_EQ(0U, nil_str_dst.value.stringv.len);
  EXPECT_EQ(nullptr, nil_str_dst.value.stringv.stringv);

  // Illegal conversion falls through to default semerror().
  NCConstant int_src{};
  int_src.nctype = NC_INT;
  int_src.value.int32v = 1;
  NCConstant illegal_char_dst{};
  illegal_char_dst.nctype = NC_CHAR;

  sigjmp_buf jmp_env{};
  int jmp_ret = -1;
  ncgen_shim_set_finalize_jmp(&jmp_env, &jmp_ret);
  const auto disarm =
      absl::MakeCleanup([] { ncgen_shim_set_finalize_jmp(nullptr, nullptr); });

  volatile bool jumped = false;
  if (sigsetjmp(jmp_env, 1) == 0) {
    convert1(&int_src, &illegal_char_dst);
  } else {
    jumped = true;
  }
  EXPECT_TRUE(jumped);
  EXPECT_EQ(1, jmp_ret);
}

TEST_F(CvtTest, ConvertStringToCharsAndConvertFilterIdWork) {
  char empty_buf[] = "";
  NCConstant empty_str{};
  empty_str.nctype = NC_STRING;
  empty_str.lineno = 3;
  empty_str.value.stringv.len = 0;
  empty_str.value.stringv.stringv = empty_buf;

  Datalist* empty_dl = convertstringtochars(&empty_str);
  ASSERT_NE(empty_dl, nullptr);
  const auto free_empty_dl =
      absl::MakeCleanup([empty_dl] { reclaimdatalist(empty_dl); });
  EXPECT_EQ(0U, datalistlen(empty_dl));

  char data[] = {'X', '\0', 'Y'};
  NCConstant str{};
  str.nctype = NC_STRING;
  str.lineno = 15;
  str.value.stringv.len = 3;
  str.value.stringv.stringv = data;

  // Upstream convertstringtochars() appends &con for a loop-local NCConstant,
  // leaving dangling stack pointers in `dl->data` on return. Check only
  // datalistlen(dl) and free the Datalist container with freedatalist() instead
  // of dereferencing elements or calling reclaimdatalist().
  Datalist* dl = convertstringtochars(&str);
  ASSERT_NE(dl, nullptr);
  const auto free_dl = absl::MakeCleanup([dl] { freedatalist(dl); });
  EXPECT_EQ(3U, datalistlen(dl));

  EXPECT_EQ(32013U, convertFilterID("32013"));
  EXPECT_EQ(0U, convertFilterID("0"));
  EXPECT_EQ(0U, convertFilterID("gzip"));
  EXPECT_EQ(0U, convertFilterID(""));
}

}  // namespace
}  // namespace netcdf
