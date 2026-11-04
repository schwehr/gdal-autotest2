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
// Tests HFADictionary parsing, type registration, and dumping
// (hfadictionary.cpp).
//
// Note: CaptureMemStream uses POSIX open_memstream (Linux/glibc).
//
// See also:
//   https://gdal.org/drivers/raster/hfa.html
//   https://github.com/OSGeo/gdal/blob/master/frmts/hfa/hfadictionary.cpp

#include <cstdio>
#include <format>  // NOLINT
#include <iterator>
#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/frmts/hfa/hfa_p.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

struct ItemSizeCase {
  char type_code;
  int expected_size;
};

constexpr ItemSizeCase kItemSizeCases[] = {
    {.type_code = '1', .expected_size = 1},
    {.type_code = '2', .expected_size = 1},
    {.type_code = '4', .expected_size = 1},
    {.type_code = 'c', .expected_size = 1},
    {.type_code = 'C', .expected_size = 1},
    {.type_code = 'e', .expected_size = 2},
    {.type_code = 's', .expected_size = 2},
    {.type_code = 'S', .expected_size = 2},
    {.type_code = 't', .expected_size = 4},
    {.type_code = 'l', .expected_size = 4},
    {.type_code = 'L', .expected_size = 4},
    {.type_code = 'f', .expected_size = 4},
    {.type_code = 'd', .expected_size = 8},
    {.type_code = 'm', .expected_size = 8},
    {.type_code = 'M', .expected_size = 16},
    {.type_code = 'b', .expected_size = -1},
    {.type_code = 'o', .expected_size = 0},
    {.type_code = 'x', .expected_size = 0},
};
static_assert(std::size(kItemSizeCases) == 18);

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

TEST(HfaDictionaryTest, ParseAndFindExplicitTypes) {
  constexpr char kDict[] =
      "{1:lnumrows,}Edsc_Table,{1:dx,1:dy,}Eprj_Coordinate,.";
  HFADictionary dict(kDict);

  HFAType* table_type = dict.FindType("Edsc_Table");
  ASSERT_NE(nullptr, table_type);
  EXPECT_STREQ("Edsc_Table", table_type->pszTypeName);
  EXPECT_EQ(4, table_type->nBytes);

  HFAType* coord_type = dict.FindType("Eprj_Coordinate");
  ASSERT_NE(nullptr, coord_type);
  EXPECT_STREQ("Eprj_Coordinate", coord_type->pszTypeName);
  EXPECT_EQ(16, coord_type->nBytes);

  EXPECT_EQ(nullptr, dict.FindType("UnknownTypeXYZ"));
  EXPECT_FALSE(dict.bDictionaryTextDirty);
}

TEST(HfaDictionaryTest, MalformedAndEmptyDictionary) {
  // 1. Minimal '.' dictionary has zero types and returns nullptr.
  HFADictionary dot_dict(".");
  EXPECT_EQ(nullptr, dot_dict.FindType("NonExistent"));

  // 2. Unclosed type definition fails Initialize() silently (CPLDebug only)
  //    and leaves the dictionary with no types without leaking.
  HFADictionary bad_dict("{unclosed_type_definition");
  EXPECT_EQ(nullptr, bad_dict.FindType("NonExistent"));
}

TEST(HfaDictionaryTest, AddManyTypesTriggersRealloc) {
  HFADictionary dict(".");
  // nTypesMax grows 0 -> 10 -> 30 on realloc (hfadictionary.cpp); 15 types
  // exercises the initial allocation plus the first growth realloc.
  for (int i = 0; i < 15; ++i) {
    auto type = std::make_unique<HFAType>();
    const std::string def = std::format("{{1:lval,}}CustomType{},.", i);
    ASSERT_NE(nullptr, type->Initialize(def.c_str()));
    dict.AddType(type.release());
  }

  HFAType* found = dict.FindType("CustomType14");
  ASSERT_NE(nullptr, found);
  EXPECT_STREQ("CustomType14", found->pszTypeName);
}

TEST(HfaDictionaryTest, DefaultDefinitionsFallback) {
  // 1. Non-empty initial dictionary text strips trailing '.' before appending
  //    the default definition.
  HFADictionary dict("{1:lnumrows,}Edsc_Table,.");
  EXPECT_FALSE(dict.bDictionaryTextDirty);

  HFAType* map_info = dict.FindType("Eprj_MapInfo");
  ASSERT_NE(nullptr, map_info);
  EXPECT_TRUE(dict.bDictionaryTextDirty);
  EXPECT_NE(nullptr, dict.FindType("Eprj_Coordinate"));
  EXPECT_NE(nullptr, dict.FindType("Eprj_Size"));

  // 2. Empty initial dictionary text exercises the empty osDictionaryText path.
  HFADictionary empty_dict("");
  HFAType* proj = empty_dict.FindType("Eprj_ProParameters");
  ASSERT_NE(nullptr, proj);
  EXPECT_TRUE(empty_dict.bDictionaryTextDirty);
  EXPECT_NE(nullptr, empty_dict.FindType("Eprj_Spheroid"));
}

TEST(HfaDictionaryTest, GetItemSizeAllTypes) {
  for (const ItemSizeCase& tc : kItemSizeCases) {
    EXPECT_EQ(tc.expected_size, HFADictionary::GetItemSize(tc.type_code))
        << "type_code=" << tc.type_code;
  }
}

TEST(HfaDictionaryTest, DumpInMemory) {
  HFADictionary dict("{1:lnumrows,}Edsc_Table,.");

  const std::string output = CaptureMemStream([&](FILE* fp) { dict.Dump(fp); });
  EXPECT_THAT(output, HasSubstr("HFADictionary:"));
  EXPECT_THAT(output, HasSubstr("Edsc_Table"));
  EXPECT_THAT(output, HasSubstr("numrows"));
}

// TODO: b/335317901 - Cover the !poNewType->CompleteDefn(this) cleanup branch
// in HFADictionary::FindType (hfadictionary.cpp:178-182).

}  // namespace
}  // namespace autotest2
