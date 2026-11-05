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
// Unit tests for ogrfielddefn.cpp.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrfielddefn.cpp

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_p.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

const auto& kAllTypes = {OFTInteger,    OFTIntegerList,    OFTReal,
                         OFTRealList,   OFTString,         OFTStringList,
                         OFTWideString, OFTWideStringList, OFTBinary,
                         OFTDate,       OFTTime,           OFTDateTime,
                         OFTInteger64,  OFTInteger64List};

TEST(OgrFieldDefnTest, NullptrAndIntCheckDefaults) {
  const auto fd = std::make_unique<OGRFieldDefn>(nullptr, OFTInteger);
  ASSERT_NE(nullptr, fd);
  EXPECT_STREQ("", fd->GetNameRef());
  EXPECT_STREQ("", fd->GetAlternativeNameRef());
  EXPECT_EQ(OFTInteger, fd->GetType());
  EXPECT_STREQ("Integer", fd->GetFieldTypeName(fd->GetType()));
  EXPECT_EQ(OFSTNone, fd->GetSubType());
  EXPECT_STREQ("None", fd->GetFieldSubTypeName(fd->GetSubType()));
  EXPECT_EQ(OJUndefined, fd->GetJustify());
  EXPECT_EQ(0, fd->GetWidth());
  EXPECT_EQ(0, fd->GetPrecision());
  EXPECT_EQ(0, fd->GetTZFlag());
  EXPECT_EQ(nullptr, fd->GetDefault());
  EXPECT_FALSE(fd->IsDefaultDriverSpecific());
  EXPECT_FALSE(fd->IsIgnored());
  EXPECT_TRUE(fd->IsNullable());
  EXPECT_FALSE(fd->IsUnique());
  EXPECT_TRUE(fd->GetDomainName().empty());
  EXPECT_TRUE(fd->GetComment().empty());
}

TEST(OgrFieldDefnTest, ChangeName) {
  auto fd = std::make_unique<OGRFieldDefn>("a", OFTIntegerList);
  ASSERT_NE(nullptr, fd);
  EXPECT_STREQ("a", fd->GetNameRef());
  fd->SetName("b");
  EXPECT_STREQ("b", fd->GetNameRef());
  fd->SetName(nullptr);
  EXPECT_STREQ("", fd->GetNameRef());

  EXPECT_EQ(OFTIntegerList, fd->GetType());
  EXPECT_STREQ("IntegerList", fd->GetFieldTypeName(fd->GetType()));
  fd->SetType(OFTReal);
  EXPECT_EQ(OFTReal, fd->GetType());
  EXPECT_STREQ("Real", fd->GetFieldTypeName(fd->GetType()));
}

TEST(OgrFieldDefnTest, AlternativeName) {
  auto fd = std::make_unique<OGRFieldDefn>("name", OFTString);
  EXPECT_STREQ("", fd->GetAlternativeNameRef());
  fd->SetAlternativeName("alt_name");
  EXPECT_STREQ("alt_name", fd->GetAlternativeNameRef());
  fd->SetAlternativeName(nullptr);
  EXPECT_STREQ("", fd->GetAlternativeNameRef());
}

TEST(OgrFieldDefnTest, TypeCompatibility) {
  for (const auto& t : kAllTypes) {
    EXPECT_TRUE(OGR_AreTypeSubTypeCompatible(t, OFSTNone));
  }

  for (const auto& t : kAllTypes) {
    if (t == OFTInteger || t == OFTIntegerList) continue;
    EXPECT_FALSE(OGR_AreTypeSubTypeCompatible(t, OFSTBoolean));
    EXPECT_FALSE(OGR_AreTypeSubTypeCompatible(t, OFSTInt16));
  }
  EXPECT_TRUE(OGR_AreTypeSubTypeCompatible(OFTInteger, OFSTBoolean));
  EXPECT_TRUE(OGR_AreTypeSubTypeCompatible(OFTIntegerList, OFSTBoolean));
  EXPECT_TRUE(OGR_AreTypeSubTypeCompatible(OFTInteger, OFSTInt16));
  EXPECT_TRUE(OGR_AreTypeSubTypeCompatible(OFTIntegerList, OFSTInt16));

  for (const auto& t : kAllTypes) {
    if (t == OFTReal || t == OFTRealList) continue;
    EXPECT_FALSE(OGR_AreTypeSubTypeCompatible(t, OFSTFloat32));
  }
  EXPECT_TRUE(OGR_AreTypeSubTypeCompatible(OFTReal, OFSTFloat32));
  EXPECT_TRUE(OGR_AreTypeSubTypeCompatible(OFTRealList, OFSTFloat32));
}

TEST(OgrFieldDefnTest, ChangeTypes) {
  const std::vector<std::pair<OGRFieldType, std::string>> types = {
      {OFTInteger, "Integer"},
      {OFTIntegerList, "IntegerList"},
      {OFTRealList, "RealList"},
      {OFTStringList, "StringList"},
      {OFTWideString, "(unknown)"},
      {OFTWideStringList, "(unknown)"},
      {OFTBinary, "Binary"},
      {OFTDate, "Date"},
      {OFTTime, "Time"},
      {OFTDateTime, "DateTime"},
      {OFTInteger64, "Integer64"},
      {OFTInteger64List, "Integer64List"}};

  auto fd = std::make_unique<OGRFieldDefn>("a", OFTInteger);
  ASSERT_NE(nullptr, fd);

  for (const auto& t : types) {
    fd->SetType(t.first);
    EXPECT_EQ(OFSTNone, fd->GetSubType());
    EXPECT_EQ(t.first, fd->GetType());
    EXPECT_EQ(t.second, fd->GetFieldTypeName(fd->GetType()));
  }
}

TEST(OgrFieldDefnTest, ChangeSubTypesValid) {
  const std::vector<std::pair<OGRFieldType, OGRFieldSubType>> types = {
      {OFTInteger, OFSTBoolean},     {OFTInteger, OFSTInt16},
      {OFTIntegerList, OFSTBoolean}, {OFTIntegerList, OFSTInt16},
      {OFTReal, OFSTFloat32},        {OFTRealList, OFSTFloat32},
      {OFTString, OFSTJSON},         {OFTString, OFSTUUID}};

  auto fd = std::make_unique<OGRFieldDefn>("a", OFTInteger);
  ASSERT_NE(nullptr, fd);

  for (const auto& t : types) {
    fd->SetType(t.first);
    fd->SetSubType(t.second);
    EXPECT_EQ(t.second, fd->GetSubType());
  }
}

TEST(OgrFieldDefnTest, ChangeSubTypesInvalid) {
  auto fd = std::make_unique<OGRFieldDefn>("a", OFTString);
  ASSERT_NE(nullptr, fd);
  {
    WithQuietHandler holder;
    fd->SetSubType(OFSTBoolean);
  }
  EXPECT_EQ(OFSTNone, fd->GetSubType());
}

TEST(OgrFieldDefnTest, Justify) {
  auto fd = std::make_unique<OGRFieldDefn>("a", OFTInteger);
  ASSERT_NE(nullptr, fd);
  EXPECT_EQ(OJUndefined, fd->GetJustify());
  fd->SetJustify(OJLeft);
  EXPECT_EQ(OJLeft, fd->GetJustify());
  fd->SetJustify(OJRight);
  EXPECT_EQ(OJRight, fd->GetJustify());
  fd->SetJustify(OJUndefined);
  EXPECT_EQ(OJUndefined, fd->GetJustify());
}

TEST(OgrFieldDefnTest, WidthAndPrecision) {
  auto fd = std::make_unique<OGRFieldDefn>("a", OFTInteger);
  ASSERT_NE(nullptr, fd);
  EXPECT_EQ(0, fd->GetWidth());
  fd->SetWidth(123456789);
  EXPECT_EQ(123456789, fd->GetWidth());
  fd->SetWidth(-1);
  EXPECT_EQ(0, fd->GetWidth());

  EXPECT_EQ(0, fd->GetPrecision());
  fd->SetPrecision(123456789);
  EXPECT_EQ(123456789, fd->GetPrecision());
  fd->SetPrecision(-1);
  EXPECT_EQ(-1, fd->GetPrecision());
}

TEST(OgrFieldDefnTest, TZFlag) {
  auto fd = std::make_unique<OGRFieldDefn>("dt", OFTDateTime);
  EXPECT_EQ(0, fd->GetTZFlag());
  fd->SetTZFlag(1);
  EXPECT_EQ(1, fd->GetTZFlag());
  fd->SetTZFlag(100);
  EXPECT_EQ(100, fd->GetTZFlag());
  fd->SetTZFlag(0);
  EXPECT_EQ(0, fd->GetTZFlag());

  // Invalid timezone flags
  {
    WithQuietHandler holder;
    fd->SetTZFlag(-1);
    fd->SetTZFlag(2);
    fd->SetTZFlag(99);
    fd->SetTZFlag(101);
  }
}

TEST(OgrFieldDefnTest, Set) {
  auto fd = std::make_unique<OGRFieldDefn>("a", OFTInteger);
  fd->Set("b", OFTReal, 1, 2, OJLeft);
  EXPECT_STREQ("b", fd->GetNameRef());
  EXPECT_EQ(OFTReal, fd->GetType());
  EXPECT_EQ(1, fd->GetWidth());
  EXPECT_EQ(2, fd->GetPrecision());
  EXPECT_EQ(OJLeft, fd->GetJustify());

  fd->Set("\xEE", OFTString);
  EXPECT_STREQ("\xEE", fd->GetNameRef());
  EXPECT_EQ(OFTString, fd->GetType());
  EXPECT_EQ(0, fd->GetWidth());
  EXPECT_EQ(0, fd->GetPrecision());
  EXPECT_EQ(OJUndefined, fd->GetJustify());
}

TEST(OgrFieldDefnTest, Default) {
  auto fd = std::make_unique<OGRFieldDefn>("a", OFTInteger);
  EXPECT_EQ(nullptr, fd->GetDefault());

  fd->SetDefault("a");
  EXPECT_STREQ("a", fd->GetDefault());
  EXPECT_TRUE(fd->IsDefaultDriverSpecific());

  for (const auto& d :
       {"'a'", "-1", "+2", "123.456", "NULL", "CURRENT_TIMESTAMP",
        "CURRENT_TIME", "CURRENT_DATE", "''", "'escaped''quote'"}) {
    fd->SetDefault(d);
    EXPECT_FALSE(fd->IsDefaultDriverSpecific()) << "Failed for: " << d;
  }

  // Trailing whitespace or incomplete literals
  fd->SetDefault("'unclosed");
  EXPECT_TRUE(fd->IsDefaultDriverSpecific());

  fd->SetDefault(nullptr);
  EXPECT_EQ(nullptr, fd->GetDefault());
  EXPECT_FALSE(fd->IsDefaultDriverSpecific());
}

TEST(OgrFieldDefnTest, IgnoredNullableUnique) {
  auto fd = std::make_unique<OGRFieldDefn>("a", OFTInteger);
  EXPECT_FALSE(fd->IsIgnored());
  fd->SetIgnored(true);
  EXPECT_TRUE(fd->IsIgnored());
  fd->SetIgnored(false);
  EXPECT_FALSE(fd->IsIgnored());

  EXPECT_TRUE(fd->IsNullable());
  fd->SetNullable(false);
  EXPECT_FALSE(fd->IsNullable());
  fd->SetNullable(true);
  EXPECT_TRUE(fd->IsNullable());

  EXPECT_FALSE(fd->IsUnique());
  fd->SetUnique(true);
  EXPECT_TRUE(fd->IsUnique());
  fd->SetUnique(false);
  EXPECT_FALSE(fd->IsUnique());
}

TEST(OgrFieldDefnTest, DomainNameAndComment) {
  auto fd = std::make_unique<OGRFieldDefn>("a", OFTInteger);
  EXPECT_TRUE(fd->GetDomainName().empty());
  fd->SetDomainName("my_domain");
  EXPECT_EQ("my_domain", fd->GetDomainName());

  EXPECT_TRUE(fd->GetComment().empty());
  fd->SetComment("my_comment");
  EXPECT_EQ("my_comment", fd->GetComment());
}

TEST(OgrFieldDefnTest, IsSame) {
  auto fd = std::make_unique<OGRFieldDefn>("a", OFTInteger);
  EXPECT_TRUE(fd->IsSame(fd.get()));
  {
    const auto other = std::make_unique<OGRFieldDefn>("a", OFTInteger);
    EXPECT_TRUE(fd->IsSame(other.get()));
  }

  {
    auto other = std::make_unique<OGRFieldDefn>("a", OFTReal);
    EXPECT_FALSE(fd->IsSame(other.get()));
  }

  {
    auto other = std::make_unique<OGRFieldDefn>("b", OFTInteger);
    EXPECT_FALSE(fd->IsSame(other.get()));
  }

  {
    auto other = std::make_unique<OGRFieldDefn>("a", OFTInteger);
    other->SetSubType(OFSTBoolean);
    EXPECT_FALSE(fd->IsSame(other.get()));
  }

  {
    auto other = std::make_unique<OGRFieldDefn>("a", OFTInteger);
    other->SetWidth(1);
    EXPECT_FALSE(fd->IsSame(other.get()));
  }

  {
    auto other = std::make_unique<OGRFieldDefn>("a", OFTInteger);
    other->SetPrecision(2);
    EXPECT_FALSE(fd->IsSame(other.get()));
  }

  {
    auto other = std::make_unique<OGRFieldDefn>("a", OFTInteger);
    other->SetNullable(false);
    EXPECT_FALSE(fd->IsSame(other.get()));
  }

  // Unique and DomainName are not checked by IsSame
  {
    auto other = std::make_unique<OGRFieldDefn>("a", OFTInteger);
    other->SetUnique(true);
    EXPECT_TRUE(fd->IsSame(other.get()));
  }

  {
    auto other = std::make_unique<OGRFieldDefn>("a", OFTInteger);
    other->SetAlternativeName("alt");
    EXPECT_FALSE(fd->IsSame(other.get()));
  }

  {
    auto other = std::make_unique<OGRFieldDefn>("a", OFTInteger);
    other->SetDomainName("dom");
    EXPECT_TRUE(fd->IsSame(other.get()));
  }

  {
    auto other = std::make_unique<OGRFieldDefn>("a", OFTInteger);
    other->SetTZFlag(1);
    EXPECT_FALSE(fd->IsSame(other.get()));
  }

  {
    auto other = std::make_unique<OGRFieldDefn>("a", OFTInteger);
    other->SetComment("comm");
    EXPECT_FALSE(fd->IsSame(other.get()));
  }
}

TEST(OgrFieldDefnTest, CopyConstructorAndAssignment) {
  OGRFieldDefn fd("orig", OFTInteger);
  fd.SetAlternativeName("alt");
  fd.SetSubType(OFSTInt16);
  fd.SetJustify(OJRight);
  fd.SetWidth(10);
  fd.SetPrecision(2);
  fd.SetTZFlag(1);
  fd.SetDefault("'default'");
  fd.SetNullable(false);
  fd.SetUnique(true);
  fd.SetDomainName("my_domain");
  fd.SetComment("my_comment");

  // Pointer-based copy constructor
  {
    OGRFieldDefn copy(&fd);
    EXPECT_STREQ("orig", copy.GetNameRef());
    EXPECT_STREQ("alt", copy.GetAlternativeNameRef());
    EXPECT_EQ(OFTInteger, copy.GetType());
    EXPECT_EQ(OFSTInt16, copy.GetSubType());
    EXPECT_EQ(OJRight, copy.GetJustify());
    EXPECT_EQ(10, copy.GetWidth());
    EXPECT_EQ(2, copy.GetPrecision());
    EXPECT_EQ(1, copy.GetTZFlag());
    EXPECT_STREQ("'default'", copy.GetDefault());
    EXPECT_FALSE(copy.IsNullable());
    EXPECT_TRUE(copy.IsUnique());
    EXPECT_EQ("my_domain", copy.GetDomainName());
    EXPECT_EQ("my_comment", copy.GetComment());
  }

  // Reference-based copy constructor
  {
    OGRFieldDefn copy(fd);
    EXPECT_TRUE(fd.IsSame(&copy));
  }

  // Assignment operator
  {
    OGRFieldDefn assigned("other", OFTString);
    assigned = fd;
    EXPECT_TRUE(fd.IsSame(&assigned));

    // Self-assignment
    assigned = *&assigned;
    EXPECT_TRUE(fd.IsSame(&assigned));
  }
}

TEST(OgrFieldDefnTest, Sealing) {
  OGRFieldDefn fd("field", OFTInteger);
  fd.Seal();

  {
    WithQuietHandler holder;
    fd.SetName("new_name");
    fd.SetAlternativeName("new_alt");
    fd.SetType(OFTReal);
    fd.SetSubType(OFSTFloat32);
    fd.SetJustify(OJLeft);
    fd.SetWidth(5);
    fd.SetPrecision(2);
    fd.SetTZFlag(1);
    fd.Set("abc", OFTString);
    fd.SetDefault("123");
    fd.SetNullable(false);
    fd.SetUnique(true);
    fd.SetDomainName("domain");
    fd.SetComment("comment");
  }

  EXPECT_STREQ("field", fd.GetNameRef());
  EXPECT_EQ(OFTInteger, fd.GetType());

  // TemporaryUnsealer via member function
  {
    auto unsealer = fd.GetTemporaryUnsealer();
    fd.SetName("unsealed_name");
    EXPECT_STREQ("unsealed_name", fd.GetNameRef());
  }

  fd.Unseal();
  fd.SetName("final_name");
  EXPECT_STREQ("final_name", fd.GetNameRef());
}

TEST(OgrFieldDefnTest, GetFieldTypeByNameAndSubTypeByName) {
  struct TypeMapping {
    const char* name;
    OGRFieldType type;
  };

  const TypeMapping kTypeMappings[] = {
      {"Integer", OFTInteger},     {"IntegerList", OFTIntegerList},
      {"Real", OFTReal},           {"RealList", OFTRealList},
      {"String", OFTString},       {"StringList", OFTStringList},
      {"Binary", OFTBinary},       {"Date", OFTDate},
      {"Time", OFTTime},           {"DateTime", OFTDateTime},
      {"Integer64", OFTInteger64}, {"Integer64List", OFTInteger64List},
      {"WideString", OFTString},   {"WideStringList", OFTString},
      {"InvalidType", OFTString},
  };

  for (const auto& tm : kTypeMappings) {
    EXPECT_EQ(tm.type, OGRFieldDefn::GetFieldTypeByName(tm.name));
    EXPECT_EQ(tm.type, OGR_GetFieldTypeByName(tm.name));
  }

  struct SubTypeMapping {
    const char* name;
    OGRFieldSubType subtype;
  };

  const SubTypeMapping kSubTypeMappings[] = {
      {"None", OFSTNone},       {"Boolean", OFSTBoolean}, {"Int16", OFSTInt16},
      {"Float32", OFSTFloat32}, {"JSON", OFSTJSON},       {"UUID", OFSTUUID},
      {"InvalidSub", OFSTNone},
  };

  for (const auto& sm : kSubTypeMappings) {
    EXPECT_EQ(sm.subtype, OGRFieldDefn::GetFieldSubTypeByName(sm.name));
    EXPECT_EQ(sm.subtype, OGR_GetFieldSubTypeByName(sm.name));
  }

  EXPECT_STREQ("JSON", OGRFieldDefn::GetFieldSubTypeName(OFSTJSON));
  EXPECT_STREQ("UUID", OGRFieldDefn::GetFieldSubTypeName(OFSTUUID));
  EXPECT_STREQ("None", OGRFieldDefn::GetFieldSubTypeName(OFSTNone));
  EXPECT_STREQ("(unknown)", OGRFieldDefn::GetFieldTypeName(OFTWideString));
}

TEST(OgrFieldDefnTest, OGRUpdateFieldType) {
  // OFTInteger
  {
    OGRFieldDefn fd("f", OFTInteger);
    fd.SetSubType(OFSTBoolean);
    OGRUpdateFieldType(&fd, OFTInteger, OFSTBoolean);
    EXPECT_EQ(OFSTBoolean, fd.GetSubType());

    OGRUpdateFieldType(&fd, OFTInteger, OFSTNone);
    EXPECT_EQ(OFSTNone, fd.GetSubType());

    OGRUpdateFieldType(&fd, OFTInteger64, OFSTNone);
    EXPECT_EQ(OFTInteger64, fd.GetType());
  }

  {
    OGRFieldDefn fd("f", OFTInteger);
    OGRUpdateFieldType(&fd, OFTReal, OFSTNone);
    EXPECT_EQ(OFTReal, fd.GetType());
  }

  {
    OGRFieldDefn fd("f", OFTInteger);
    OGRUpdateFieldType(&fd, OFTIntegerList, OFSTBoolean);
    EXPECT_EQ(OFTIntegerList, fd.GetType());
    EXPECT_EQ(OFSTNone, fd.GetSubType());
  }

  {
    OGRFieldDefn fd("f", OFTInteger);
    OGRUpdateFieldType(&fd, OFTInteger64List, OFSTNone);
    EXPECT_EQ(OFTInteger64List, fd.GetType());
  }

  {
    OGRFieldDefn fd("f", OFTInteger);
    OGRUpdateFieldType(&fd, OFTRealList, OFSTNone);
    EXPECT_EQ(OFTRealList, fd.GetType());
  }

  {
    OGRFieldDefn fd("f", OFTInteger);
    OGRUpdateFieldType(&fd, OFTStringList, OFSTNone);
    EXPECT_EQ(OFTStringList, fd.GetType());
  }

  {
    OGRFieldDefn fd("f", OFTInteger);
    OGRUpdateFieldType(&fd, OFTBinary, OFSTNone);
    EXPECT_EQ(OFTString, fd.GetType());
  }

  // OFTInteger64
  {
    OGRFieldDefn fd("f", OFTInteger64);
    OGRUpdateFieldType(&fd, OFTReal, OFSTNone);
    EXPECT_EQ(OFTReal, fd.GetType());
  }

  {
    OGRFieldDefn fd("f", OFTInteger64);
    OGRUpdateFieldType(&fd, OFTIntegerList, OFSTNone);
    EXPECT_EQ(OFTInteger64List, fd.GetType());
  }

  {
    OGRFieldDefn fd("f", OFTInteger64);
    OGRUpdateFieldType(&fd, OFTRealList, OFSTNone);
    EXPECT_EQ(OFTRealList, fd.GetType());
  }

  {
    OGRFieldDefn fd("f", OFTInteger64);
    OGRUpdateFieldType(&fd, OFTStringList, OFSTNone);
    EXPECT_EQ(OFTStringList, fd.GetType());
  }

  {
    OGRFieldDefn fd("f", OFTInteger64);
    OGRUpdateFieldType(&fd, OFTDate, OFSTNone);
    EXPECT_EQ(OFTString, fd.GetType());
  }

  {
    OGRFieldDefn fd("f", OFTInteger64);
    OGRUpdateFieldType(&fd, OFTInteger, OFSTNone);
    EXPECT_EQ(OFTInteger64, fd.GetType());
  }

  // OFTReal
  {
    OGRFieldDefn fd("f", OFTReal);
    OGRUpdateFieldType(&fd, OFTIntegerList, OFSTNone);
    EXPECT_EQ(OFTRealList, fd.GetType());
  }

  {
    OGRFieldDefn fd("f", OFTReal);
    OGRUpdateFieldType(&fd, OFTInteger64List, OFSTNone);
    EXPECT_EQ(OFTRealList, fd.GetType());
  }

  {
    OGRFieldDefn fd("f", OFTReal);
    OGRUpdateFieldType(&fd, OFTStringList, OFSTNone);
    EXPECT_EQ(OFTStringList, fd.GetType());
  }

  {
    OGRFieldDefn fd("f", OFTReal);
    OGRUpdateFieldType(&fd, OFTBinary, OFSTNone);
    EXPECT_EQ(OFTString, fd.GetType());
  }

  // OFTIntegerList
  {
    OGRFieldDefn fd("f", OFTIntegerList);
    fd.SetSubType(OFSTBoolean);
    OGRUpdateFieldType(&fd, OFTIntegerList, OFSTNone);
    EXPECT_EQ(OFSTNone, fd.GetSubType());
  }

  {
    OGRFieldDefn fd("f", OFTIntegerList);
    OGRUpdateFieldType(&fd, OFTInteger64, OFSTNone);
    EXPECT_EQ(OFTInteger64List, fd.GetType());
  }

  {
    OGRFieldDefn fd("f", OFTIntegerList);
    OGRUpdateFieldType(&fd, OFTReal, OFSTNone);
    EXPECT_EQ(OFTRealList, fd.GetType());
  }

  {
    OGRFieldDefn fd("f", OFTIntegerList);
    OGRUpdateFieldType(&fd, OFTString, OFSTNone);
    EXPECT_EQ(OFTStringList, fd.GetType());
  }

  {
    OGRFieldDefn fd("f", OFTIntegerList);
    OGRUpdateFieldType(&fd, OFTInteger, OFSTNone);
    EXPECT_EQ(OFTIntegerList, fd.GetType());
  }

  // OFTInteger64List
  {
    OGRFieldDefn fd("f", OFTInteger64List);
    OGRUpdateFieldType(&fd, OFTReal, OFSTNone);
    EXPECT_EQ(OFTRealList, fd.GetType());
  }

  {
    OGRFieldDefn fd("f", OFTInteger64List);
    OGRUpdateFieldType(&fd, OFTRealList, OFSTNone);
    EXPECT_EQ(OFTRealList, fd.GetType());
  }

  {
    OGRFieldDefn fd("f", OFTInteger64List);
    OGRUpdateFieldType(&fd, OFTString, OFSTNone);
    EXPECT_EQ(OFTStringList, fd.GetType());
  }

  {
    OGRFieldDefn fd("f", OFTInteger64List);
    OGRUpdateFieldType(&fd, OFTInteger64, OFSTNone);
    EXPECT_EQ(OFTInteger64List, fd.GetType());
  }

  // OFTRealList
  {
    OGRFieldDefn fd("f", OFTRealList);
    OGRUpdateFieldType(&fd, OFTString, OFSTNone);
    EXPECT_EQ(OFTStringList, fd.GetType());
  }

  {
    OGRFieldDefn fd("f", OFTRealList);
    OGRUpdateFieldType(&fd, OFTReal, OFSTNone);
    EXPECT_EQ(OFTRealList, fd.GetType());
  }

  // OFTDateTime
  {
    OGRFieldDefn fd("f", OFTDateTime);
    OGRUpdateFieldType(&fd, OFTDate, OFSTNone);
    EXPECT_EQ(OFTDateTime, fd.GetType());
    OGRUpdateFieldType(&fd, OFTTime, OFSTNone);
    EXPECT_EQ(OFTString, fd.GetType());
  }

  // OFTDate and OFTTime
  {
    OGRFieldDefn fd("f", OFTDate);
    OGRUpdateFieldType(&fd, OFTDateTime, OFSTNone);
    EXPECT_EQ(OFTDateTime, fd.GetType());
  }

  {
    OGRFieldDefn fd("f", OFTTime);
    OGRUpdateFieldType(&fd, OFTDateTime, OFSTNone);
    EXPECT_EQ(OFTDateTime, fd.GetType());
  }

  {
    OGRFieldDefn fd("f", OFTDate);
    OGRUpdateFieldType(&fd, OFTInteger, OFSTNone);
    EXPECT_EQ(OFTString, fd.GetType());
  }

  // OFTString
  {
    OGRFieldDefn fd("f", OFTString);
    OGRUpdateFieldType(&fd, OFTStringList, OFSTNone);
    EXPECT_EQ(OFTStringList, fd.GetType());
  }
}

TEST(OgrFieldDefnTest, CAPIFieldDefn) {
  OGRFieldDefnH h = OGR_Fld_Create("test_c", OFTInteger);
  ASSERT_NE(nullptr, h);

  EXPECT_STREQ("test_c", OGR_Fld_GetNameRef(h));
  OGR_Fld_SetName(h, "new_name");
  EXPECT_STREQ("new_name", OGR_Fld_GetNameRef(h));

  EXPECT_STREQ("", OGR_Fld_GetAlternativeNameRef(h));
  OGR_Fld_SetAlternativeName(h, "alt");
  EXPECT_STREQ("alt", OGR_Fld_GetAlternativeNameRef(h));

  EXPECT_EQ(OFTInteger, OGR_Fld_GetType(h));
  OGR_Fld_SetType(h, OFTReal);
  EXPECT_EQ(OFTReal, OGR_Fld_GetType(h));

  EXPECT_EQ(OFSTNone, OGR_Fld_GetSubType(h));
  OGR_Fld_SetSubType(h, OFSTFloat32);
  EXPECT_EQ(OFSTFloat32, OGR_Fld_GetSubType(h));

  EXPECT_EQ(OJUndefined, OGR_Fld_GetJustify(h));
  OGR_Fld_SetJustify(h, OJRight);
  EXPECT_EQ(OJRight, OGR_Fld_GetJustify(h));

  EXPECT_EQ(0, OGR_Fld_GetWidth(h));
  OGR_Fld_SetWidth(h, 15);
  EXPECT_EQ(15, OGR_Fld_GetWidth(h));

  EXPECT_EQ(0, OGR_Fld_GetPrecision(h));
  OGR_Fld_SetPrecision(h, 3);
  EXPECT_EQ(3, OGR_Fld_GetPrecision(h));

  EXPECT_EQ(0, OGR_Fld_GetTZFlag(h));
  OGR_Fld_SetTZFlag(h, 1);
  EXPECT_EQ(1, OGR_Fld_GetTZFlag(h));

  OGR_Fld_Set(h, "set_name", OFTString, 25, 0, OJLeft);
  EXPECT_STREQ("set_name", OGR_Fld_GetNameRef(h));
  EXPECT_EQ(OFTString, OGR_Fld_GetType(h));
  EXPECT_EQ(25, OGR_Fld_GetWidth(h));
  EXPECT_EQ(0, OGR_Fld_GetPrecision(h));
  EXPECT_EQ(OJLeft, OGR_Fld_GetJustify(h));

  EXPECT_FALSE(OGR_Fld_IsIgnored(h));
  OGR_Fld_SetIgnored(h, TRUE);
  EXPECT_TRUE(OGR_Fld_IsIgnored(h));

  EXPECT_TRUE(OGR_Fld_IsNullable(h));
  OGR_Fld_SetNullable(h, FALSE);
  EXPECT_FALSE(OGR_Fld_IsNullable(h));

  EXPECT_FALSE(OGR_Fld_IsUnique(h));
  OGR_Fld_SetUnique(h, TRUE);
  EXPECT_TRUE(OGR_Fld_IsUnique(h));

  EXPECT_EQ(nullptr, OGR_Fld_GetDefault(h));
  OGR_Fld_SetDefault(h, "'default_val'");
  EXPECT_STREQ("'default_val'", OGR_Fld_GetDefault(h));
  EXPECT_FALSE(OGR_Fld_IsDefaultDriverSpecific(h));

  EXPECT_STREQ("", OGR_Fld_GetDomainName(h));
  OGR_Fld_SetDomainName(h, "c_domain");
  EXPECT_STREQ("c_domain", OGR_Fld_GetDomainName(h));
  OGR_Fld_SetDomainName(h, nullptr);
  EXPECT_STREQ("", OGR_Fld_GetDomainName(h));

  EXPECT_STREQ("", OGR_Fld_GetComment(h));
  OGR_Fld_SetComment(h, "c_comment");
  EXPECT_STREQ("c_comment", OGR_Fld_GetComment(h));
  OGR_Fld_SetComment(h, nullptr);
  EXPECT_STREQ("", OGR_Fld_GetComment(h));

  OGR_Fld_Destroy(h);
}

TEST(OgrFieldDefnTest, CodedFieldDomain) {
  // C++ class
  {
    std::vector<OGRCodedValue> values;
    OGRCodedValue cv1{CPLStrdup("1"), CPLStrdup("One")};
    OGRCodedValue cv2{CPLStrdup("2"), CPLStrdup("Two")};
    values.push_back(cv1);
    values.push_back(cv2);

    auto domain = std::make_unique<OGRCodedFieldDomain>(
        "CodedDomain", "My description", OFTInteger, OFSTNone,
        std::move(values));
    EXPECT_EQ(OFDT_CODED, domain->GetDomainType());
    EXPECT_EQ("CodedDomain", domain->GetName());
    EXPECT_EQ("My description", domain->GetDescription());
    EXPECT_EQ(OFTInteger, domain->GetFieldType());
    EXPECT_EQ(OFSTNone, domain->GetFieldSubType());
    EXPECT_EQ(OFDSP_DEFAULT_VALUE, domain->GetSplitPolicy());
    domain->SetSplitPolicy(OFDSP_GEOMETRY_RATIO);
    EXPECT_EQ(OFDSP_GEOMETRY_RATIO, domain->GetSplitPolicy());
    EXPECT_EQ(OFDMP_DEFAULT_VALUE, domain->GetMergePolicy());
    domain->SetMergePolicy(OFDMP_SUM);
    EXPECT_EQ(OFDMP_SUM, domain->GetMergePolicy());

    const OGRCodedValue* enumeration = domain->GetEnumeration();
    ASSERT_NE(nullptr, enumeration);
    EXPECT_STREQ("1", enumeration[0].pszCode);
    EXPECT_STREQ("One", enumeration[0].pszValue);
    EXPECT_STREQ("2", enumeration[1].pszCode);
    EXPECT_STREQ("Two", enumeration[1].pszValue);
    EXPECT_EQ(nullptr, enumeration[2].pszCode);

    auto clone = std::unique_ptr<OGRFieldDomain>(domain->Clone());
    ASSERT_NE(nullptr, clone);
    EXPECT_EQ("CodedDomain", clone->GetName());
  }

  // C API
  {
    OGRCodedValue enumeration[3];
    enumeration[0].pszCode = const_cast<char*>("A");
    enumeration[0].pszValue = const_cast<char*>("Alpha");
    enumeration[1].pszCode = const_cast<char*>("B");
    enumeration[1].pszValue = const_cast<char*>("Beta");
    enumeration[2].pszCode = nullptr;
    enumeration[2].pszValue = nullptr;

    OGRFieldDomainH hDomain = OGR_CodedFldDomain_Create(
        "CodedC", "CDesc", OFTString, OFSTNone, enumeration);
    ASSERT_NE(nullptr, hDomain);
    EXPECT_STREQ("CodedC", OGR_FldDomain_GetName(hDomain));
    EXPECT_STREQ("CDesc", OGR_FldDomain_GetDescription(hDomain));
    EXPECT_EQ(OFDT_CODED, OGR_FldDomain_GetDomainType(hDomain));
    EXPECT_EQ(OFTString, OGR_FldDomain_GetFieldType(hDomain));
    EXPECT_EQ(OFSTNone, OGR_FldDomain_GetFieldSubType(hDomain));

    OGR_FldDomain_SetSplitPolicy(hDomain, OFDSP_DUPLICATE);
    EXPECT_EQ(OFDSP_DUPLICATE, OGR_FldDomain_GetSplitPolicy(hDomain));
    OGR_FldDomain_SetMergePolicy(hDomain, OFDMP_GEOMETRY_WEIGHTED);
    EXPECT_EQ(OFDMP_GEOMETRY_WEIGHTED, OGR_FldDomain_GetMergePolicy(hDomain));

    const OGRCodedValue* enumOut = OGR_CodedFldDomain_GetEnumeration(hDomain);
    ASSERT_NE(nullptr, enumOut);
    EXPECT_STREQ("A", enumOut[0].pszCode);
    EXPECT_STREQ("Alpha", enumOut[0].pszValue);

    OGR_FldDomain_Destroy(hDomain);
  }
}

TEST(OgrFieldDefnTest, RangeFieldDomain) {
  // C++ class
  {
    OGRField sMin;
    sMin.Integer = 10;
    OGRField sMax;
    sMax.Integer = 50;

    auto domain = std::make_unique<OGRRangeFieldDomain>(
        "RangeDomain", "Range desc", OFTInteger, OFSTNone, sMin, true, sMax,
        false);
    EXPECT_EQ(OFDT_RANGE, domain->GetDomainType());
    EXPECT_EQ("RangeDomain", domain->GetName());
    EXPECT_EQ("Range desc", domain->GetDescription());
    EXPECT_EQ(OFTInteger, domain->GetFieldType());

    bool bMinInc = false;
    EXPECT_EQ(10, domain->GetMin(bMinInc).Integer);
    EXPECT_TRUE(bMinInc);

    bool bMaxInc = true;
    EXPECT_EQ(50, domain->GetMax(bMaxInc).Integer);
    EXPECT_FALSE(bMaxInc);

    auto clone = std::unique_ptr<OGRFieldDomain>(domain->Clone());
    ASSERT_NE(nullptr, clone);
    EXPECT_EQ("RangeDomain", clone->GetName());
  }

  // C API
  {
    OGRField sMin;
    sMin.Real = 1.5;
    OGRField sMax;
    sMax.Real = 99.5;

    OGRFieldDomainH hDomain = OGR_RangeFldDomain_Create(
        "RangeC", "CDesc", OFTReal, OFSTNone, &sMin, false, &sMax, true);
    ASSERT_NE(nullptr, hDomain);

    bool bMinInc = true;
    const OGRField* pMin = OGR_RangeFldDomain_GetMin(hDomain, &bMinInc);
    ASSERT_NE(nullptr, pMin);
    EXPECT_DOUBLE_EQ(1.5, pMin->Real);
    EXPECT_FALSE(bMinInc);

    bool bMaxInc = false;
    const OGRField* pMax = OGR_RangeFldDomain_GetMax(hDomain, &bMaxInc);
    ASSERT_NE(nullptr, pMax);
    EXPECT_DOUBLE_EQ(99.5, pMax->Real);
    EXPECT_TRUE(bMaxInc);

    OGR_FldDomain_Destroy(hDomain);

    // Unset min and max in C API
    hDomain =
        OGR_RangeFldDomain_Create("RangeUnset", nullptr, OFTInteger64, OFSTNone,
                                  nullptr, false, nullptr, false);
    ASSERT_NE(nullptr, hDomain);
    pMin = OGR_RangeFldDomain_GetMin(hDomain, nullptr);
    EXPECT_TRUE(OGR_RawField_IsUnset(pMin));
    pMax = OGR_RangeFldDomain_GetMax(hDomain, nullptr);
    EXPECT_TRUE(OGR_RawField_IsUnset(pMax));
    OGR_FldDomain_Destroy(hDomain);

    // Invalid field type in OGR_RangeFldDomain_Create
    {
      WithQuietHandler holder;
      EXPECT_EQ(nullptr, OGR_RangeFldDomain_Create("BadType", nullptr,
                                                   OFTString, OFSTNone, nullptr,
                                                   false, nullptr, false));
    }
  }
}

TEST(OgrFieldDefnTest, GlobFieldDomain) {
  // C++ class
  {
    auto domain = std::make_unique<OGRGlobFieldDomain>(
        "GlobDomain", "Glob desc", OFTString, OFSTNone, "*.[tT][xX][tT]");
    EXPECT_EQ(OFDT_GLOB, domain->GetDomainType());
    EXPECT_EQ("GlobDomain", domain->GetName());
    EXPECT_EQ("Glob desc", domain->GetDescription());
    EXPECT_EQ(OFTString, domain->GetFieldType());
    EXPECT_EQ("*.[tT][xX][tT]", domain->GetGlob());

    auto clone = std::unique_ptr<OGRFieldDomain>(domain->Clone());
    ASSERT_NE(nullptr, clone);
    EXPECT_EQ("GlobDomain", clone->GetName());
  }

  // C API
  {
    OGRFieldDomainH hDomain = OGR_GlobFldDomain_Create(
        "GlobC", "CDesc", OFTString, OFSTNone, "*.geojson");
    ASSERT_NE(nullptr, hDomain);
    EXPECT_STREQ("*.geojson", OGR_GlobFldDomain_GetGlob(hDomain));
    OGR_FldDomain_Destroy(hDomain);
  }
}

TEST(OgrFieldDefnTest, FieldDomainCrossTypeErrors) {
  // Create a Glob domain and call Coded and Range functions on it
  OGRFieldDomainH hGlob =
      OGR_GlobFldDomain_Create("Glob", nullptr, OFTString, OFSTNone, "*.txt");
  ASSERT_NE(nullptr, hGlob);

  {
    WithQuietHandler holder;
    EXPECT_EQ(nullptr, OGR_CodedFldDomain_GetEnumeration(hGlob));
    const OGRField* pMin = OGR_RangeFldDomain_GetMin(hGlob, nullptr);
    EXPECT_TRUE(OGR_RawField_IsUnset(pMin));
    const OGRField* pMax = OGR_RangeFldDomain_GetMax(hGlob, nullptr);
    EXPECT_TRUE(OGR_RawField_IsUnset(pMax));
  }

  OGR_FldDomain_Destroy(hGlob);

  // Create a Coded domain and call Glob function on it
  OGRCodedValue enumeration[2];
  enumeration[0].pszCode = const_cast<char*>("1");
  enumeration[0].pszValue = const_cast<char*>("One");
  enumeration[1].pszCode = nullptr;
  enumeration[1].pszValue = nullptr;
  OGRFieldDomainH hCoded = OGR_CodedFldDomain_Create(
      "Coded", nullptr, OFTInteger, OFSTNone, enumeration);
  ASSERT_NE(nullptr, hCoded);

  {
    WithQuietHandler holder;
    EXPECT_EQ(nullptr, OGR_GlobFldDomain_GetGlob(hCoded));
  }

  OGR_FldDomain_Destroy(hCoded);
}

}  // namespace
}  // namespace autotest2
