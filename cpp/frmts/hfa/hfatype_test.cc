// Copyright 2017 Google LLC
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
// White-box tests for HFAType (hfa_p.h / hfatype.cpp).
//
// Note: White-box tests depend on hfa_p.h internals, assume little-endian host
// byte order (HFAStandard is a no-op on CPL_LSB), and use POSIX open_memstream.
//
// See also:
//   https://gdal.org/drivers/raster/hfa.html
//   https://github.com/OSGeo/gdal/blob/master/frmts/hfa/hfatype.cpp

#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/hfa/hfa_p.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::ContainsRegex;
using ::testing::HasSubstr;

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

// The contents written by Dump for the input line in this test should look like
// this:
//
// HFAType Eimg_StatisticsParameters830/0 bytes
//     Emif_String         p LayerNames[0];
//     BASEDATA            * ExcludedValues[1];
//     Emif_String           AOIname[1];
//     ULONG                 SkipFactorX[1];
//     ULONG                 SkipFactorY[1];
//     Edsc_BinFunction    * BinFunction[1];
TEST(HfatypeTest, Dump) {
  constexpr char kInput[] =
      "{0:poEmif_String,LayerNames,1:*bExcludedValues,1:oEmif_String,AOIname,1:"
      "lSkipFactorX,1:lSkipFactorY,1:*oEdsc_BinFunction,BinFunction,}Eimg_"
      "StatisticsParameters830,.";

  HFAType hfa_type;
  ASSERT_NE(nullptr, hfa_type.Initialize(kInput));
  const std::string contents =
      CaptureMemStream([&](FILE* fp) { hfa_type.Dump(fp); });
  EXPECT_THAT(contents, ContainsRegex(R"RE(Emif_String\s+AOIname)RE"));
}

TEST(HfatypeTest, InitializeEdgeCases) {
  // 1. Empty and brace-less inputs return nullptr.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    HFAType empty_type;
    EXPECT_EQ(nullptr, empty_type.Initialize(""));
    EXPECT_EQ(nullptr, empty_type.Initialize("no_opening_brace"));
    CPLErrorReset();
  }

  // 2. Leading characters before '{' are skipped.
  HFAType skipped_type;
  const char* rest = skipped_type.Initialize("leading_junk{1:lcount,}MyType,.");
  ASSERT_NE(nullptr, rest);
  EXPECT_STREQ(".", rest);
  EXPECT_STREQ("MyType", skipped_type.pszTypeName);

  // 3. Unterminated field list and missing type-name comma return nullptr.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    HFAType bad_field_type;
    EXPECT_EQ(nullptr, bad_field_type.Initialize("{1:lcount"));

    HFAType no_comma_type;
    EXPECT_EQ(nullptr, no_comma_type.Initialize("{1:lcount,}UnterminatedName"));
    EXPECT_STREQ("UnterminatedName", no_comma_type.pszTypeName);
    CPLErrorReset();
  }
}

TEST(HfatypeTest, CompleteDefnRecursionAndOverflow) {
  // 1. Direct self-recursion in CompleteDefn.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    HFADictionary rec_dict("{1:oRecType,self,}RecType,.");
    HFAType* rec_type = rec_dict.FindType("RecType");
    ASSERT_NE(nullptr, rec_type);
    EXPECT_FALSE(rec_type->CompleteDefn(&rec_dict));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Recursion detected"));
    CPLErrorReset();
  }

  // 2. Already-completed type returns true immediately.
  HFADictionary ok_dict("{1:lx,}FixedType,.");
  HFAType* fixed_type = ok_dict.FindType("FixedType");
  ASSERT_NE(nullptr, fixed_type);
  EXPECT_EQ(4, fixed_type->nBytes);
  EXPECT_TRUE(fixed_type->CompleteDefn(&ok_dict));

  // 3. Variable-sized field sets nBytes to -1.
  HFADictionary var_dict("{0:pcstr,}VarType,.");
  HFAType* var_type = var_dict.FindType("VarType");
  ASSERT_NE(nullptr, var_type);
  EXPECT_EQ(-1, var_type->nBytes);

  // 4. Integer overflow of total field sizes clamps nBytes to -1.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    HFADictionary big_dict("{2000000000:ca,2000000000:cb,}BigType,.");
    HFAType* big_type = big_dict.FindType("BigType");
    ASSERT_NE(nullptr, big_type);
    EXPECT_EQ(-1, big_type->nBytes);
    CPLErrorReset();
  }
}

TEST(HfatypeTest, SetExtractCountAndDumpInstValues) {
  constexpr char kDict[] =
      "{1:lx,1:ly,}Inner,"
      "{2:larr,1:oInner,sub,2:oInner,sub_arr,}Outer,.";
  HFADictionary dict(kDict);
  HFAType* outer = dict.FindType("Outer");
  ASSERT_NE(nullptr, outer);
  EXPECT_EQ(32, outer->nBytes);

  std::vector<GByte> buf(32, 0);
  const int buf_size = static_cast<int>(buf.size());

  // 1. SetInstValue on direct, array, nested, and indexed nested fields.
  int val_arr0 = 11;
  int val_arr1 = 22;
  int val_sub_x = 33;
  int val_sub_arr1_y = 44;
  EXPECT_EQ(CE_None, outer->SetInstValue("arr", buf.data(), 0, buf_size, 'i',
                                         &val_arr0));
  EXPECT_EQ(CE_None, outer->SetInstValue("arr[1]", buf.data(), 0, buf_size, 'i',
                                         &val_arr1));
  EXPECT_EQ(CE_None, outer->SetInstValue("sub.x", buf.data(), 0, buf_size, 'i',
                                         &val_sub_x));
  EXPECT_EQ(CE_None, outer->SetInstValue("sub_arr[1].y", buf.data(), 0,
                                         buf_size, 'i', &val_sub_arr1_y));

  // 2. ExtractInstValue and GetInstCount on all valid field path forms.
  int out = 0;
  int remaining = 0;
  EXPECT_TRUE(outer->ExtractInstValue("arr", buf.data(), 0, buf_size, 'i', &out,
                                      &remaining));
  EXPECT_EQ(11, out);
  EXPECT_TRUE(outer->ExtractInstValue("arr[1]", buf.data(), 0, buf_size, 'i',
                                      &out, &remaining));
  EXPECT_EQ(22, out);
  EXPECT_TRUE(outer->ExtractInstValue("sub.x", buf.data(), 0, buf_size, 'i',
                                      &out, &remaining));
  EXPECT_EQ(33, out);
  EXPECT_TRUE(outer->ExtractInstValue("sub_arr[1].y", buf.data(), 0, buf_size,
                                      'i', &out, &remaining));
  EXPECT_EQ(44, out);

  EXPECT_EQ(2, outer->GetInstCount("arr", buf.data(), 0, buf_size));
  EXPECT_EQ(2, outer->GetInstCount("arr[0]", buf.data(), 0, buf_size));
  EXPECT_EQ(1, outer->GetInstCount("sub.x", buf.data(), 0, buf_size));

  // 3. Missing field lookup errors.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, outer->SetInstValue("missing", buf.data(), 0,
                                              buf_size, 'i', &val_arr0));
    EXPECT_FALSE(outer->ExtractInstValue("missing", buf.data(), 0, buf_size,
                                         'i', &out, &remaining));
    EXPECT_EQ(-1, outer->GetInstCount("missing", buf.data(), 0, buf_size));
    CPLErrorReset();
  }

  // 4. GetInstBytes and DumpInstValue on fixed-size type.
  std::set<HFAField*> visited;
  EXPECT_EQ(32, outer->GetInstBytes(buf.data(), buf_size, visited));

  const std::string dumped = CaptureMemStream([&](FILE* fp) {
    outer->DumpInstValue(fp, buf.data(), 0, buf_size, "  ");
  });
  EXPECT_THAT(dumped, HasSubstr("arr[0] = 11"));
  EXPECT_THAT(dumped, HasSubstr("arr[1] = 22"));
}

TEST(HfatypeTest, VariableLengthTypeAndErrorPaths) {
  constexpr char kDict[] =
      "{2:lvals,}InnerArr,"
      "{0:pcname,1:oInnerArr,inner,1:ltail,}VarOuter,.";
  HFADictionary dict(kDict);
  HFAType* var_outer = dict.FindType("VarOuter");
  ASSERT_NE(nullptr, var_outer);
  EXPECT_EQ(-1, var_outer->nBytes);

  // 1. Build valid little-endian HFA instance buffer:
  //    name: count=4 (LE), offset=8 (LE), "abc\0" (4 bytes) -> 12 bytes
  //    inner.vals: [100, 200] -> 8 bytes
  //    tail: 999 -> 4 bytes
  std::vector<GByte> buf(24, 0);
  const int buf_size = static_cast<int>(buf.size());
  WriteHfaLe(buf.data(), 0, GUInt32{4});
  WriteHfaLe(buf.data(), 4, GUInt32{8});
  std::memcpy(buf.data() + 8, "abc", 4);
  WriteHfaLe(buf.data(), 12, GUInt32{100});
  WriteHfaLe(buf.data(), 16, GUInt32{200});
  WriteHfaLe(buf.data(), 20, GUInt32{999});

  std::set<HFAField*> visited;
  EXPECT_EQ(24, var_outer->GetInstBytes(buf.data(), buf_size, visited));

  // 2. Dot before bracket in field path: "inner.vals[1]".
  int out = 0;
  int remaining = 0;
  EXPECT_TRUE(var_outer->ExtractInstValue("inner.vals[1]", buf.data(), 0,
                                          buf_size, 'i', &out, &remaining));
  EXPECT_EQ(200, out);

  // 3. Truncated buffer (< 4 bytes for pointer count) triggers GetInstBytes
  //    <= 0 across SetInstValue, GetInstCount, ExtractInstValue,
  //    DumpInstValue, and GetInstBytes.
  std::vector<GByte> tiny(2, 0);
  const int tiny_size = static_cast<int>(tiny.size());
  int set_val = 5;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, var_outer->SetInstValue("tail", tiny.data(), 0,
                                                  tiny_size, 'i', &set_val));
    EXPECT_EQ(-1, var_outer->GetInstCount("tail", tiny.data(), 0, tiny_size));
    EXPECT_FALSE(var_outer->ExtractInstValue("tail", tiny.data(), 0, tiny_size,
                                             'i', &out, &remaining));
    visited.clear();
    EXPECT_EQ(-1, var_outer->GetInstBytes(tiny.data(), tiny_size, visited));
    CPLErrorReset();
  }

  const std::string err_dump = CaptureMemStream([&](FILE* fp) {
    var_outer->DumpInstValue(fp, tiny.data(), 0, tiny_size, "");
  });
  EXPECT_THAT(err_dump, HasSubstr("name"));
}

// TODO: b/335317901 - Maintain full line coverage of hfatype.cpp across future
// upstream GDAL upgrades.

}  // namespace
}  // namespace autotest2
