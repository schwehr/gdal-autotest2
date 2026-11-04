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
// White-box tests for HFAField type definition, extraction, and instance
// manipulation (hfa_p.h / hfafield.cpp).
//
// Note: White-box tests depend on hfa_p.h internals, assume little-endian host
// byte order (HFAStandard is a no-op on CPL_LSB), and use POSIX open_memstream.
//
// See also:
//   https://gdal.org/drivers/raster/hfa.html
//   https://github.com/OSGeo/gdal/blob/master/frmts/hfa/hfafield.cpp

#include <array>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/hfa/hfa.h"
#include "third_party/gdal/frmts/hfa/hfa_p.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

// Pointer header (8 bytes) + BASEDATA header (12 bytes) + 64-byte payload.
constexpr int kBdBufSize = 8 + 12 + 64;
constexpr int kIntMax = std::numeric_limits<int>::max();
constexpr int kIntMin = std::numeric_limits<int>::min();

struct FieldTypeCase {
  const char* spec;
  std::string_view expected_dump;
};

constexpr const char* kInvalidSpecs[] = {
    "-1:lbadCount,",
    "1noColon",
    "1:",
    "1:p",
    "1:zbadType,",
    "1:oSubTypeNoComma",
    "1:x{1:lunclosed,",
    "1:x{1:lclosed,}TypeNoComma",
    "1:e-1:a,field,",
    "1:e100001:a,field,",
    "1:e2noColon",
    "1:e2:onlyOneEnum",
    "1:lfieldNoComma",
};
static_assert(std::size(kInvalidSpecs) == 13);

constexpr FieldTypeCase kFieldTypeCases[] = {
    {.spec = "1:1f1,", .expected_dump = "U1"},
    {.spec = "1:2f2,", .expected_dump = "U2"},
    {.spec = "1:4f4,", .expected_dump = "U4"},
    {.spec = "1:cfc,", .expected_dump = "UCHAR"},
    {.spec = "1:CfC,", .expected_dump = "CHAR"},
    {.spec = "1:e2:ONE,TWO,fe,", .expected_dump = "ENUM"},
    {.spec = "1:sfs,", .expected_dump = "USHORT"},
    {.spec = "1:SfS,", .expected_dump = "SHORT"},
    {.spec = "1:tft,", .expected_dump = "TIME"},
    {.spec = "1:lfl,", .expected_dump = "ULONG"},
    {.spec = "1:LfL,", .expected_dump = "LONG"},
    {.spec = "1:fff,", .expected_dump = "FLOAT"},
    {.spec = "1:dfd,", .expected_dump = "DOUBLE"},
    {.spec = "1:mfm,", .expected_dump = "COMPLEX"},
    {.spec = "1:MfM,", .expected_dump = "DCOMPLEX"},
    {.spec = "1:*bfb,", .expected_dump = "BASEDATA"},
    {.spec = "1:oFixedSub,fo,", .expected_dump = "FixedSub"},
    {.spec = "1:xplainInline,", .expected_dump = "InlineType"},
};
static_assert(std::size(kFieldTypeCases) == 18);

template <typename T>
void WriteHfaLe(GByte* dst, size_t offset, T val) {
  HFAStandard(static_cast<int>(sizeof(T)), &val);
  std::memcpy(dst + offset, &val, sizeof(T));
}

template <typename Func>
std::string CaptureMemStream(Func&& dump_fn) {
  char* buf = nullptr;
  size_t size = 0;
  std::unique_ptr<char, FreeDeleter> owned_buf;
  FILE* fp = open_memstream(&buf, &size);
  if (fp == nullptr) {
    ADD_FAILURE() << "open_memstream failed";
    return "";
  }
  dump_fn(fp);
  EXPECT_EQ(0, std::fclose(fp));
  owned_buf.reset(buf);
  return owned_buf == nullptr ? "" : std::string(owned_buf.get(), size);
}

std::string DumpFieldToString(HFAField* field) {
  return CaptureMemStream([&](FILE* fp) { field->Dump(fp); });
}

std::string DumpFieldInstToString(HFAField* field, GByte* data, GUInt32 offset,
                                  int data_size, const char* prefix = "") {
  return CaptureMemStream([&](FILE* fp) {
    field->DumpInstValue(fp, data, offset, data_size, prefix);
  });
}

CPLErr SetStringFieldVal(HFAField& field, const char* name, int idx,
                         std::vector<GByte>& buf, GUInt32 offset, int size,
                         const char* val) {
  if (val == nullptr) {
    return field.SetInstValue(name, idx, buf.data(), offset, size, 's',
                              nullptr);
  }
  std::string mutable_val(val);
  return field.SetInstValue(name, idx, buf.data(), offset, size, 's',
                            mutable_val.data());
}

TEST(HfaFieldTest, InitializeValidAndInvalidInputs) {
  // 1. Invalid inputs return nullptr.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    for (const char* spec : kInvalidSpecs) {
      HFAField f;
      EXPECT_EQ(nullptr, f.Initialize(spec)) << spec;
    }
    CPLErrorReset();
  }

  // 2. Valid inline object with nested braces ('x' -> 'o').
  {
    HFAField f;
    const char* rest = f.Initialize(
        "1:x{1:x{1:linner,}InnerType,nested,}OuterType,inlineField,tail");
    ASSERT_NE(nullptr, rest);
    EXPECT_STREQ("tail", rest);
    EXPECT_EQ('o', f.chItemType);
    EXPECT_STREQ("OuterType", f.pszItemObjectType);
    EXPECT_STREQ("inlineField", f.pszFieldName);
  }

  // 3. Valid enum with pointer modifier '*'.
  {
    HFAField f;
    const char* rest = f.Initialize("1:*e3:ALPHA,BETA,GAMMA,myEnum,");
    ASSERT_NE(nullptr, rest);
    EXPECT_EQ('*', f.chPointer);
    EXPECT_EQ('e', f.chItemType);
    EXPECT_STREQ("myEnum", f.pszFieldName);
  }
}

TEST(HfaFieldTest, CompleteDefnAndDumpAllTypes) {
  HFADictionary dict(
      "{1:lwidth,1:lheight,}FixedSub,"
      "{0:pcname,}VarSub,"
      "{1:oFixedSub,sub,}RecType,.");

  // 1. All supported field type codes initialize, complete, and dump.
  for (const FieldTypeCase& tc : kFieldTypeCases) {
    HFAField f;
    ASSERT_NE(nullptr, f.Initialize(tc.spec)) << tc.spec;
    EXPECT_TRUE(f.CompleteDefn(&dict)) << tc.spec;
    EXPECT_THAT(DumpFieldToString(&f), HasSubstr(tc.expected_dump)) << tc.spec;
  }

  // 2. Pointer 'p' -> nBytes = -1.
  {
    HFAField f;
    ASSERT_NE(nullptr, f.Initialize("0:plptrField,"));
    EXPECT_TRUE(f.CompleteDefn(&dict));
    EXPECT_EQ(-1, f.nBytes);
  }

  // 3. Fixed subobject with '*' -> nBytes = 8 + 8 = 16; variable -> -1.
  {
    HFAField f;
    ASSERT_NE(nullptr, f.Initialize("1:*oFixedSub,ptrSub,"));
    EXPECT_TRUE(f.CompleteDefn(&dict));
    EXPECT_EQ(16, f.nBytes);
  }
  {
    HFAField f;
    ASSERT_NE(nullptr, f.Initialize("1:oVarSub,varSub,"));
    EXPECT_TRUE(f.CompleteDefn(&dict));
    EXPECT_EQ(-1, f.nBytes);
  }

  // 4. Overflow and self-recursion checks in CompleteDefn.
  {
    WithQuietHandler quiet;
    CPLErrorReset();

    HFAField big_arr;
    ASSERT_NE(nullptr, big_arr.Initialize("2147483647:lbigArray,"));
    EXPECT_TRUE(big_arr.CompleteDefn(&dict));
    EXPECT_EQ(-1, big_arr.nBytes);

    HFAField big_sub;
    ASSERT_NE(nullptr, big_sub.Initialize("2147483647:oFixedSub,bigSubArray,"));
    EXPECT_TRUE(big_sub.CompleteDefn(&dict));
    EXPECT_EQ(-1, big_sub.nBytes);

    // FixedSub is 8 bytes; 268435455 * 8 = 2147483640 > INT_MAX - 8.
    HFAField near_max_sub;
    ASSERT_NE(nullptr,
              near_max_sub.Initialize("268435455:*oFixedSub,nearMaxSub,"));
    EXPECT_TRUE(near_max_sub.CompleteDefn(&dict));
    EXPECT_EQ(-1, near_max_sub.nBytes);

    HFADictionary rec_dict("{1:oSelfRec,self,}SelfRec,.");
    HFAField self_rec;
    ASSERT_NE(nullptr, self_rec.Initialize("1:oSelfRec,self,"));
    EXPECT_FALSE(self_rec.CompleteDefn(&rec_dict));
    CPLErrorReset();
  }
}

TEST(HfaFieldTest, FixedAndPointerStringFields) {
  HFADictionary dict(".");

  // 1. Fixed string 'c' / single char 'i' and buffer-too-small errors.
  HFAField fixed_str;
  ASSERT_NE(nullptr, fixed_str.Initialize("8:cstrField,"));
  ASSERT_TRUE(fixed_str.CompleteDefn(&dict));
  std::vector<GByte> buf(8, 0);
  EXPECT_EQ(CE_None,
            SetStringFieldVal(fixed_str, "strField", 0, buf, 0, 8, "hello"));
  char* out_str = nullptr;
  EXPECT_TRUE(
      fixed_str.ExtractInstValue(nullptr, 0, buf.data(), 0, 8, 's', &out_str));
  EXPECT_STREQ("hello", out_str);
  EXPECT_THAT(DumpFieldInstToString(&fixed_str, buf.data(), 0, 8),
              HasSubstr("`hello'"));

  int ch_val = 'Z';
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              SetStringFieldVal(fixed_str, "strField", 0, buf, 0, 4, "hi"));
    CPLErrorReset();
  }
  EXPECT_EQ(CE_None,
            SetStringFieldVal(fixed_str, "strField", 0, buf, 0, 8, nullptr));
  EXPECT_EQ(CE_None, fixed_str.SetInstValue("strField", 1, buf.data(), 0, 8,
                                            'i', &ch_val));
  int out_int = 0;
  EXPECT_TRUE(
      fixed_str.ExtractInstValue(nullptr, 1, buf.data(), 0, 8, 'i', &out_int));
  EXPECT_EQ('Z', out_int);
  EXPECT_EQ(CE_None,
            SetStringFieldVal(fixed_str, "strField", 2, buf, 0, 8, "Q"));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, fixed_str.SetInstValue("strField", 2, buf.data(), 0,
                                                 1, 'i', &ch_val));
    EXPECT_FALSE(fixed_str.ExtractInstValue(nullptr, 2, buf.data(), 0, 1, 'i',
                                            &out_int));
    CPLErrorReset();
  }

  // 2. Pointer string '0:pcptrStr,' with nullptr and non-null string.
  HFAField ptr_str;
  ASSERT_NE(nullptr, ptr_str.Initialize("0:pcptrStr,"));
  ASSERT_TRUE(ptr_str.CompleteDefn(&dict));
  std::vector<GByte> pbuf(32, 0);
  EXPECT_EQ(CE_None,
            SetStringFieldVal(ptr_str, "ptrStr", 0, pbuf, 100, 32, "abc"));
  int rem = 0;
  EXPECT_TRUE(ptr_str.ExtractInstValue(nullptr, 0, pbuf.data(), 100, 32, 's',
                                       &out_str, &rem));
  EXPECT_STREQ("abc", out_str);
  EXPECT_EQ(24, rem);

  EXPECT_EQ(CE_None,
            SetStringFieldVal(ptr_str, "ptrStr", 0, pbuf, 100, 32, nullptr));
  EXPECT_THAT(DumpFieldInstToString(&ptr_str, pbuf.data(), 100, 4),
              HasSubstr("(access failed)"));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              SetStringFieldVal(ptr_str, "ptrStr", 0, pbuf, 100, 8, "toolong"));
    EXPECT_FALSE(ptr_str.ExtractInstValue(nullptr, 0, pbuf.data(), 100, 4, 's',
                                          &out_str));
    CPLErrorReset();
  }
}

TEST(HfaFieldTest, EnumShortAndLongScalarFields) {
  HFADictionary dict(".");

  // 1. Enum 'e' and SHORT 'S'.
  HFAField enum_f;
  ASSERT_NE(nullptr, enum_f.Initialize("1:e3:ZERO,ONE,TWO,enumVal,"));
  ASSERT_TRUE(enum_f.CompleteDefn(&dict));
  std::vector<GByte> sbuf(4, 0);
  EXPECT_EQ(CE_None,
            SetStringFieldVal(enum_f, "enumVal", 0, sbuf, 0, 4, "TWO"));
  char* enum_str = nullptr;
  EXPECT_TRUE(
      enum_f.ExtractInstValue(nullptr, 0, sbuf.data(), 0, 4, 's', &enum_str));
  EXPECT_STREQ("TWO", enum_str);
  EXPECT_THAT(DumpFieldInstToString(&enum_f, sbuf.data(), 0, 4),
              HasSubstr("TWO"));
  EXPECT_THAT(DumpFieldInstToString(&enum_f, sbuf.data(), 0, 1),
              HasSubstr("(access failed)"));

  int one_val = 1;
  int out_enum_int = 0;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              SetStringFieldVal(enum_f, "enumVal", 0, sbuf, 0, 4, "UNKNOWN"));
    EXPECT_EQ(CE_Failure, enum_f.SetInstValue("enumVal", 0, sbuf.data(), 0, 1,
                                              'i', &one_val));
    EXPECT_FALSE(enum_f.ExtractInstValue(nullptr, 0, sbuf.data(), 0, 1, 'i',
                                         &out_enum_int));
    CPLErrorReset();
  }

  HFAField short_f;
  ASSERT_NE(nullptr, short_f.Initialize("1:SsignedShort,"));
  ASSERT_TRUE(short_f.CompleteDefn(&dict));
  int neg = -123;
  EXPECT_EQ(CE_None, short_f.SetInstValue("signedShort", 0, sbuf.data(), 0, 4,
                                          'i', &neg));
  char* num_str = nullptr;
  EXPECT_TRUE(
      short_f.ExtractInstValue(nullptr, 0, sbuf.data(), 0, 4, 's', &num_str));
  EXPECT_STREQ("-123", num_str);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, short_f.SetInstValue("signedShort", 0, sbuf.data(), 0,
                                               1, 'i', &neg));
    EXPECT_FALSE(
        short_f.ExtractInstValue(nullptr, 0, sbuf.data(), 0, 1, 'i', &neg));
    CPLErrorReset();
  }

  // 2. ULONG 'l' and LONG 'L'.
  HFAField ulong_f;
  ASSERT_NE(nullptr, ulong_f.Initialize("1:lulongVal,"));
  ASSERT_TRUE(ulong_f.CompleteDefn(&dict));
  HFAField long_f;
  ASSERT_NE(nullptr, long_f.Initialize("1:LlongVal,"));
  ASSERT_TRUE(long_f.CompleteDefn(&dict));
  std::vector<GByte> lbuf(8, 0);

  int val = 42;
  EXPECT_EQ(CE_None,
            ulong_f.SetInstValue("ulongVal", 0, lbuf.data(), 0, 8, 'i', &val));
  int out = 0;
  EXPECT_TRUE(
      ulong_f.ExtractInstValue(nullptr, 0, lbuf.data(), 0, 8, 'i', &out));
  EXPECT_EQ(42, out);

  int neg_val = -99;
  EXPECT_EQ(CE_None, long_f.SetInstValue("longVal", 0, lbuf.data(), 0, 8, 'i',
                                         &neg_val));
  EXPECT_TRUE(
      long_f.ExtractInstValue(nullptr, 0, lbuf.data(), 0, 8, 'i', &out));
  EXPECT_EQ(-99, out);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, ulong_f.SetInstValue("ulongVal", 0, lbuf.data(), 0, 2,
                                               'i', &val));
    EXPECT_FALSE(
        ulong_f.ExtractInstValue(nullptr, 0, lbuf.data(), 0, 2, 'i', &out));
    EXPECT_EQ(CE_Failure, long_f.SetInstValue("longVal", 0, lbuf.data(), 0, 2,
                                              'i', &neg_val));
    EXPECT_FALSE(
        long_f.ExtractInstValue(nullptr, 0, lbuf.data(), 0, 2, 'i', &out));
    CPLErrorReset();
  }
}

TEST(HfaFieldTest, FloatAndDoubleScalarFields) {
  HFADictionary dict(".");

  // 1. Float32 scalar round-trip and dump formatting.
  HFAField float_f;
  ASSERT_NE(nullptr, float_f.Initialize("1:ffloatVal,"));
  ASSERT_TRUE(float_f.CompleteDefn(&dict));
  std::vector<GByte> buf(8, 0);
  double dval = 12.5;
  EXPECT_EQ(CE_None,
            float_f.SetInstValue("floatVal", 0, buf.data(), 0, 8, 'd', &dval));
  double dout = 0.0;
  EXPECT_TRUE(
      float_f.ExtractInstValue(nullptr, 0, buf.data(), 0, 8, 'd', &dout));
  EXPECT_FLOAT_EQ(12.5f, static_cast<float>(dout));
  EXPECT_THAT(DumpFieldInstToString(&float_f, buf.data(), 0, 8),
              HasSubstr("12.5"));
  EXPECT_THAT(DumpFieldInstToString(&float_f, buf.data(), 0, 2),
              HasSubstr("(access failed)"));

  // 2. Float64 scalar round-trip, NaN/infinity storage, and overflow/buffer
  // errors.
  HFAField double_f;
  ASSERT_NE(nullptr, double_f.Initialize("1:ddoubleVal,"));
  ASSERT_TRUE(double_f.CompleteDefn(&dict));
  double big_d = 1e20;
  EXPECT_EQ(CE_None, double_f.SetInstValue("doubleVal", 0, buf.data(), 0, 8,
                                           'd', &big_d));
  EXPECT_TRUE(
      double_f.ExtractInstValue(nullptr, 0, buf.data(), 0, 8, 'd', &dout));
  EXPECT_DOUBLE_EQ(1e20, dout);

  double neg_big_d = -1e20;
  EXPECT_EQ(CE_None, double_f.SetInstValue("doubleVal", 0, buf.data(), 0, 8,
                                           'd', &neg_big_d));
  double nan_d = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(CE_None, double_f.SetInstValue("doubleVal", 0, buf.data(), 0, 8,
                                           'd', &nan_d));

  double huge = 1e40;
  int iout = 0;
  // SetInstValue('f') clamps double -> float silently (no CPLError on clamp),
  // whereas extracting an out-of-int-range float as 'i' emits CE_Failure.
  EXPECT_EQ(CE_None,
            float_f.SetInstValue("floatVal", 0, buf.data(), 0, 8, 'd', &huge));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(
        float_f.ExtractInstValue(nullptr, 0, buf.data(), 0, 8, 'i', &iout));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Too large for int"));
    EXPECT_EQ(CE_Failure, float_f.SetInstValue("floatVal", 0, buf.data(), 0, 2,
                                               'd', &dval));
    EXPECT_FALSE(
        float_f.ExtractInstValue(nullptr, 0, buf.data(), 0, 2, 'd', &dout));
    EXPECT_FALSE(
        double_f.ExtractInstValue(nullptr, 0, buf.data(), 0, 8, 'i', &iout));
    EXPECT_EQ(CE_Failure, double_f.SetInstValue("doubleVal", 0, buf.data(), 0,
                                                4, 'd', &big_d));
    EXPECT_FALSE(
        double_f.ExtractInstValue(nullptr, 0, buf.data(), 0, 4, 'd', &dout));
    EXPECT_EQ(CE_Failure, double_f.SetInstValue("doubleVal", 0, buf.data(), 0,
                                                8, 'p', &big_d));
    CPLErrorReset();
  }
}

TEST(HfaFieldTest, BaseDataClampingAndDump) {
  HFADictionary dict(".");

  HFAField bd;
  ASSERT_NE(nullptr, bd.Initialize("1:*bvalueBD,"));
  ASSERT_TRUE(bd.CompleteDefn(&dict));

  std::vector<GByte> buf(kBdBufSize, 0);

  // 1. Set rows = 2 (-1), cols = 4 (-2), type = EPT_f64 (-3).
  int rows = 2;
  int cols = 4;
  int type_f64 = EPT_f64;
  EXPECT_EQ(CE_None, bd.SetInstValue("valueBD", -1, buf.data(), 0, kBdBufSize,
                                     'i', &rows));
  EXPECT_EQ(CE_None, bd.SetInstValue("valueBD", -2, buf.data(), 0, kBdBufSize,
                                     'i', &cols));
  EXPECT_EQ(CE_None, bd.SetInstValue("valueBD", -3, buf.data(), 0, kBdBufSize,
                                     'i', &type_f64));
  EXPECT_EQ(8, bd.GetInstCount(buf.data(), kBdBufSize));

  double dval = 7.5;
  EXPECT_EQ(CE_None, bd.SetInstValue("valueBD", 0, buf.data(), 0, kBdBufSize,
                                     'd', &dval));
  double dout = 0.0;
  EXPECT_TRUE(
      bd.ExtractInstValue(nullptr, 0, buf.data(), 0, kBdBufSize, 'd', &dout));
  EXPECT_DOUBLE_EQ(7.5, dout);

  // 2. Test EPT_f64 -> 'i' clamping for normal, > INT_MAX, < INT_MIN, and NaN.
  //    Pins upstream quirk: f64 NaN clamps to INT_MAX (+ CE_Warning), whereas
  //    f32 NaN (tested in BaseDataExtractAllElementTypes) clamps to 0.
  int iout = 0;
  EXPECT_TRUE(
      bd.ExtractInstValue(nullptr, 0, buf.data(), 0, kBdBufSize, 'i', &iout));
  EXPECT_EQ(7, iout);

  double dmax = 1e15;
  double dmin = -1e15;
  double dnan = std::numeric_limits<double>::quiet_NaN();
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_None, bd.SetInstValue("valueBD", 1, buf.data(), 0, kBdBufSize,
                                       'd', &dmax));
    EXPECT_TRUE(
        bd.ExtractInstValue(nullptr, 1, buf.data(), 0, kBdBufSize, 'i', &iout));
    EXPECT_EQ(kIntMax, iout);

    EXPECT_EQ(CE_None, bd.SetInstValue("valueBD", 2, buf.data(), 0, kBdBufSize,
                                       'd', &dmin));
    EXPECT_TRUE(
        bd.ExtractInstValue(nullptr, 2, buf.data(), 0, kBdBufSize, 'i', &iout));
    EXPECT_EQ(kIntMin, iout);

    EXPECT_EQ(CE_None, bd.SetInstValue("valueBD", 3, buf.data(), 0, kBdBufSize,
                                       'd', &dnan));
    EXPECT_TRUE(
        bd.ExtractInstValue(nullptr, 3, buf.data(), 0, kBdBufSize, 'i', &iout));
    EXPECT_EQ(kIntMax, iout);
    CPLErrorReset();
  }

  // 3. DumpInstValue on valid, empty, and invalid-type BASEDATA.
  EXPECT_THAT(DumpFieldInstToString(&bd, buf.data(), 0, kBdBufSize),
              HasSubstr("BASEDATA(valueBD): 4x2 of f64"));
  EXPECT_THAT(DumpFieldInstToString(&bd, buf.data(), 0, 12),
              HasSubstr("empty"));

  int type_u8 = EPT_u8;
  EXPECT_EQ(CE_None, bd.SetInstValue("valueBD", -3, buf.data(), 0, kBdBufSize,
                                     'i', &type_u8));
  int u8_val = 200;
  EXPECT_EQ(CE_None, bd.SetInstValue("valueBD", 0, buf.data(), 0, kBdBufSize,
                                     'i', &u8_val));
  EXPECT_TRUE(
      bd.ExtractInstValue(nullptr, 0, buf.data(), 0, kBdBufSize, 'i', &iout));
  EXPECT_EQ(200, iout);

  int type_s16 = EPT_s16;
  EXPECT_EQ(CE_None, bd.SetInstValue("valueBD", -3, buf.data(), 0, kBdBufSize,
                                     'i', &type_s16));
  int bad_index = -4;
  int bad_type = 999;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, bd.SetInstValue("valueBD", 0, buf.data(), 0,
                                          kBdBufSize, 'i', &u8_val));
    EXPECT_EQ(CE_Failure, bd.SetInstValue("valueBD", -4, buf.data(), 0,
                                          kBdBufSize, 'i', &bad_index));
    EXPECT_EQ(CE_Failure, bd.SetInstValue("valueBD", -3, buf.data(), 0,
                                          kBdBufSize, 'i', &bad_type));
    CPLErrorReset();
  }
  EXPECT_THAT(DumpFieldInstToString(&bd, buf.data(), 0, kBdBufSize),
              HasSubstr("invalid type"));
}

TEST(HfaFieldTest, BaseDataExtractAllElementTypes) {
  HFADictionary dict(".");

  HFAField bd;
  ASSERT_NE(nullptr, bd.Initialize("1:*bvalueBD,"));
  ASSERT_TRUE(bd.CompleteDefn(&dict));

  std::vector<GByte> buf(kBdBufSize, 0);
  auto set_bd_header = [&](GInt32 r, GInt32 c, GInt16 t) {
    WriteHfaLe(buf.data(), 8, r);
    WriteHfaLe(buf.data(), 12, c);
    WriteHfaLe(buf.data(), 16, t);
  };

  int iout = 0;

  // 1. Sub-byte and integer element types.
  set_bd_header(1, 8, EPT_u1);
  buf[20] = 0x01;
  EXPECT_TRUE(
      bd.ExtractInstValue(nullptr, 0, buf.data(), 0, kBdBufSize, 'i', &iout));
  EXPECT_EQ(1, iout);
  EXPECT_TRUE(
      bd.ExtractInstValue(nullptr, 1, buf.data(), 0, kBdBufSize, 'i', &iout));
  EXPECT_EQ(0, iout);

  set_bd_header(1, 4, EPT_u2);
  buf[20] = 0x1b;
  EXPECT_TRUE(
      bd.ExtractInstValue(nullptr, 0, buf.data(), 0, kBdBufSize, 'i', &iout));
  EXPECT_EQ(0x1b & 0x03, iout);

  set_bd_header(1, 4, EPT_u4);
  buf[20] = 0x53;
  EXPECT_TRUE(
      bd.ExtractInstValue(nullptr, 0, buf.data(), 0, kBdBufSize, 'i', &iout));
  EXPECT_EQ(0x53 & 0x0f, iout);

  set_bd_header(1, 4, EPT_s8);
  buf[20] = static_cast<GByte>(-42);
  EXPECT_TRUE(
      bd.ExtractInstValue(nullptr, 0, buf.data(), 0, kBdBufSize, 'i', &iout));
  EXPECT_EQ(-42, iout);

  set_bd_header(1, 4, EPT_s16);
  WriteHfaLe(buf.data(), 20, GInt16{-1234});
  EXPECT_TRUE(
      bd.ExtractInstValue(nullptr, 0, buf.data(), 0, kBdBufSize, 'i', &iout));
  EXPECT_EQ(-1234, iout);

  set_bd_header(1, 4, EPT_u16);
  WriteHfaLe(buf.data(), 20, GUInt16{50000});
  EXPECT_TRUE(
      bd.ExtractInstValue(nullptr, 0, buf.data(), 0, kBdBufSize, 'i', &iout));
  EXPECT_EQ(50000, iout);

  set_bd_header(1, 4, EPT_s32);
  WriteHfaLe(buf.data(), 20, GInt32{-100000});
  EXPECT_TRUE(
      bd.ExtractInstValue(nullptr, 0, buf.data(), 0, kBdBufSize, 'i', &iout));
  EXPECT_EQ(-100000, iout);

  set_bd_header(1, 4, EPT_u32);
  WriteHfaLe(buf.data(), 20, GUInt32{123456});
  EXPECT_TRUE(
      bd.ExtractInstValue(nullptr, 0, buf.data(), 0, kBdBufSize, 'i', &iout));
  EXPECT_EQ(123456, iout);

  // 2. Float32 element clamping; FloatToIntClamp maps f32 NaN -> 0 (unlike
  // f64).
  set_bd_header(1, 4, EPT_f32);
  constexpr auto kFvals = std::to_array<float>(
      {3.25f, std::numeric_limits<float>::quiet_NaN(), 1e20f, -1e20f});
  for (size_t i = 0; i < kFvals.size(); ++i) {
    WriteHfaLe(buf.data(), 20 + i * sizeof(float), kFvals[i]);
  }
  EXPECT_TRUE(
      bd.ExtractInstValue(nullptr, 0, buf.data(), 0, kBdBufSize, 'i', &iout));
  EXPECT_EQ(3, iout);
  EXPECT_TRUE(
      bd.ExtractInstValue(nullptr, 1, buf.data(), 0, kBdBufSize, 'i', &iout));
  EXPECT_EQ(0, iout);
  EXPECT_TRUE(
      bd.ExtractInstValue(nullptr, 2, buf.data(), 0, kBdBufSize, 'i', &iout));
  EXPECT_EQ(kIntMax, iout);
  EXPECT_TRUE(
      bd.ExtractInstValue(nullptr, 3, buf.data(), 0, kBdBufSize, 'i', &iout));
  EXPECT_EQ(kIntMin, iout);

  // 3. Truncated payloads, unsupported complex extraction, and negative rows.
  struct TruncatedCase {
    EPTType type;
    int truncated_size;
  };
  constexpr TruncatedCase kTruncatedCases[] = {
      {.type = EPT_u1, .truncated_size = 20},
      {.type = EPT_u2, .truncated_size = 20},
      {.type = EPT_u4, .truncated_size = 20},
      {.type = EPT_s8, .truncated_size = 20},
      {.type = EPT_s16, .truncated_size = 21},
      {.type = EPT_u16, .truncated_size = 21},
      {.type = EPT_s32, .truncated_size = 22},
      {.type = EPT_u32, .truncated_size = 22},
      {.type = EPT_f32, .truncated_size = 22},
  };
  static_assert(std::size(kTruncatedCases) == 9);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    for (const TruncatedCase& tc : kTruncatedCases) {
      set_bd_header(1, 4, static_cast<GInt16>(tc.type));
      EXPECT_FALSE(bd.ExtractInstValue(nullptr, 0, buf.data(), 0,
                                       tc.truncated_size, 'i', &iout))
          << "type=" << tc.type;
    }

    set_bd_header(1, 4, EPT_c64);
    EXPECT_FALSE(
        bd.ExtractInstValue(nullptr, 0, buf.data(), 0, kBdBufSize, 'i', &iout));

    set_bd_header(-1, 4, EPT_u8);
    EXPECT_EQ(0, bd.GetInstCount(buf.data(), kBdBufSize));
    EXPECT_FALSE(bd.ExtractInstValue(nullptr, -1, buf.data(), 0, kBdBufSize,
                                     'i', &iout));
    CPLErrorReset();
  }
}

TEST(HfaFieldTest, SubobjectsGetInstBytesAndTruncationDump) {
  HFADictionary dict(
      "{1:lx,1:ly,}FixedPair,"
      "{0:pclabel,}VarLabel,.");

  // 1. Fixed-size subobject array.
  {
    HFAField f;
    ASSERT_NE(nullptr, f.Initialize("2:oFixedPair,pairs,"));
    ASSERT_TRUE(f.CompleteDefn(&dict));
    std::vector<GByte> buf(16, 0);
    int x1 = 77;
    EXPECT_EQ(CE_None, f.SetInstValue("x", 1, buf.data(), 0, 16, 'i', &x1));
    int out = 0;
    EXPECT_TRUE(f.ExtractInstValue("x", 1, buf.data(), 0, 16, 'i', &out));
    EXPECT_EQ(77, out);

    EXPECT_THAT(DumpFieldInstToString(&f, buf.data(), 0, 16), HasSubstr("77"));
    EXPECT_THAT(DumpFieldInstToString(&f, buf.data(), 0, 4),
                HasSubstr("(access failed)"));

    GByte* raw_ptr = nullptr;
    EXPECT_TRUE(
        f.ExtractInstValue(nullptr, 1, buf.data(), 0, 16, 'p', &raw_ptr));
    EXPECT_EQ(buf.data() + 8, raw_ptr);

    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure,
                f.SetInstValue("x", kIntMax, buf.data(), 0, 16, 'i', &x1));
      EXPECT_FALSE(f.ExtractInstValue("x", 1, buf.data(), 0, 4, 'i', &out));
      CPLErrorReset();
    }
  }

  // 2. Variable-size subobject array (direct and pointer).
  {
    HFAField f;
    ASSERT_NE(nullptr, f.Initialize("2:oVarLabel,labels,"));
    ASSERT_TRUE(f.CompleteDefn(&dict));
    std::vector<GByte> buf(64, 0);
    // First VarLabel: count=4 ("one\0"), offset=8 -> total 12 bytes.
    WriteHfaLe(buf.data(), 0, GInt32{4});
    WriteHfaLe(buf.data(), 4, GInt32{8});
    std::memcpy(buf.data() + 8, "one", 4);

    // Second VarLabel starts at offset 12.
    EXPECT_EQ(CE_None, SetStringFieldVal(f, "label", 1, buf, 0, 64, "two"));
    char* lbl = nullptr;
    EXPECT_TRUE(f.ExtractInstValue("label", 1, buf.data(), 0, 64, 's', &lbl));
    EXPECT_STREQ("two", lbl);

    std::set<HFAField*> visited;
    EXPECT_EQ(12, f.GetInstBytes(buf.data(), 64, visited));

    visited.insert(&f);
    EXPECT_EQ(-1, f.GetInstBytes(buf.data(), 64, visited));

    HFAField pf;
    ASSERT_NE(nullptr, pf.Initialize("0:*oVarLabel,ptrLabels,"));
    ASSERT_TRUE(pf.CompleteDefn(&dict));
    std::vector<GByte> pbuf(64, 0);
    WriteHfaLe(pbuf.data(), 0, GInt32{2});
    WriteHfaLe(pbuf.data(), 4, GInt32{8});
    std::memcpy(pbuf.data() + 8, buf.data(), 24);
    std::set<HFAField*> pvisited;
    EXPECT_EQ(32, pf.GetInstBytes(pbuf.data(), 64, pvisited));
    EXPECT_EQ(-1, pf.GetInstBytes(pbuf.data(), 10, pvisited));
  }

  // 3. Unresolved subobject type in ExtractInstValue returns false silently.
  {
    HFAField f;
    ASSERT_NE(nullptr, f.Initialize("1:oUnresolvedType,missing,"));
    EXPECT_FALSE(f.ExtractInstValue("x", 0, nullptr, 0, 0, 'i', nullptr));
  }

  // 4. Array > 16 entries truncates DumpInstValue output, and 0 entries prints
  //    "(no values)".
  {
    HFAField many;
    ASSERT_NE(nullptr, many.Initialize("20:lmanyInts,"));
    ASSERT_TRUE(many.CompleteDefn(&dict));
    std::vector<GByte> buf(80, 0);
    EXPECT_THAT(DumpFieldInstToString(&many, buf.data(), 0, 80),
                HasSubstr("remaining instances omitted"));

    HFAField zero;
    ASSERT_NE(nullptr, zero.Initialize("0:plzeroInts,"));
    ASSERT_TRUE(zero.CompleteDefn(&dict));
    std::vector<GByte> zbuf(8, 0);
    EXPECT_THAT(DumpFieldInstToString(&zero, zbuf.data(), 0, 8),
                HasSubstr("(no values)"));
    EXPECT_EQ(0, zero.GetInstCount(zbuf.data(), 2));
    std::set<HFAField*> visited;
    EXPECT_EQ(-1, zero.GetInstBytes(zbuf.data(), 2, visited));
    EXPECT_EQ(8, zero.GetInstBytes(zbuf.data(), 8, visited));
  }

  // 5. GetInstBytes on BASEDATA field.
  {
    HFAField bd;
    ASSERT_NE(nullptr, bd.Initialize("1:*bvalueBD,"));
    ASSERT_TRUE(bd.CompleteDefn(&dict));
    std::vector<GByte> buf(32, 0);
    WriteHfaLe(buf.data(), 0, GInt32{1});
    WriteHfaLe(buf.data(), 8, GInt32{2});
    WriteHfaLe(buf.data(), 12, GInt32{3});
    WriteHfaLe(buf.data(), 16, GInt16{EPT_s16});
    std::set<HFAField*> visited;
    // 8 (ptr) + 12 (bd header) + 2*3*2 (data) = 32 bytes.
    EXPECT_EQ(32, bd.GetInstBytes(buf.data(), 32, visited));
    EXPECT_EQ(-1, bd.GetInstBytes(buf.data(), 12, visited));

    {
      WithQuietHandler quiet;
      CPLErrorReset();
      WriteHfaLe(buf.data(), 16, GInt16{999});
      EXPECT_EQ(-1, bd.GetInstBytes(buf.data(), 32, visited));
      CPLErrorReset();
    }

    WriteHfaLe(buf.data(), 16, GInt16{EPT_s16});
    WriteHfaLe(buf.data(), 8, GInt32{-5});
    EXPECT_EQ(-1, bd.GetInstBytes(buf.data(), 32, visited));
  }
}

// TODO: b/335317901 - Cover remaining pointer-field and overflow edge cases in
// hfafield.cpp.

}  // namespace
}  // namespace autotest2
