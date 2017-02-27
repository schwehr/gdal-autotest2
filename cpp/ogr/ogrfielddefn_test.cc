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

#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/port/cpl_port.h"

#include <memory>   // NOLINT(build/include_order)
#include <utility>  // NOLINT(build/include_order)
#include <vector>   // NOLINT(build/include_order)

#include "testing/base/public/gunit.h"
#include "third_party/absl/memory/memory.h"
#include "third_party/gdal/ogr/ogr_core.h"

namespace autotest2 {
namespace {

const auto& kAllTypes = {
    OFTInteger,    OFTIntegerList,    OFTRealList,     OFTString, OFTStringList,
    OFTWideString, OFTWideStringList, OFTBinary,       OFTDate,   OFTTime,
    OFTDateTime,   OFTInteger64,      OFTInteger64List};

TEST(OgrFieldDefnTest, NullptrAndIntCheckDefaults) {
  const auto fd = gtl::MakeUnique<OGRFieldDefn>(nullptr, OFTInteger);
  ASSERT_NE(nullptr, fd);
  EXPECT_STREQ("", fd->GetNameRef());
  EXPECT_EQ(OFTInteger, fd->GetType());
  EXPECT_STREQ("Integer", fd->GetFieldTypeName(fd->GetType()));
  EXPECT_EQ(OFSTNone, fd->GetSubType());
  EXPECT_STREQ("None", fd->GetFieldSubTypeName(fd->GetSubType()));
  EXPECT_EQ(OJUndefined, fd->GetJustify());
  EXPECT_EQ(0, fd->GetWidth());
  EXPECT_EQ(0, fd->GetPrecision());
  EXPECT_EQ(nullptr, fd->GetDefault());
  EXPECT_EQ(FALSE, fd->IsDefaultDriverSpecific());
  EXPECT_EQ(FALSE, fd->IsIgnored());
  EXPECT_EQ(TRUE, fd->IsNullable());
}

TEST(OgrFieldDefnTest, ChangeName) {
  auto fd = gtl::MakeUnique<OGRFieldDefn>("a", OFTIntegerList);
  ASSERT_NE(nullptr, fd);
  EXPECT_STREQ("a", fd->GetNameRef());
  fd->SetName("b");
  EXPECT_STREQ("b", fd->GetNameRef());

  EXPECT_EQ(OFTIntegerList, fd->GetType());
  EXPECT_STREQ("IntegerList", fd->GetFieldTypeName(fd->GetType()));
  fd->SetType(OFTReal);
  EXPECT_EQ(OFTReal, fd->GetType());
  EXPECT_STREQ("Real", fd->GetFieldTypeName(fd->GetType()));
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
  const std::vector<std::pair<OGRFieldType, string>> types = {
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

  auto fd = gtl::MakeUnique<OGRFieldDefn>("a", OFTInteger);
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
      {OFTReal, OFSTFloat32},        {OFTRealList, OFSTFloat32}};

  auto fd = gtl::MakeUnique<OGRFieldDefn>("a", OFTInteger);
  ASSERT_NE(nullptr, fd);

  for (const auto& t : types) {
    fd->SetType(t.first);
    fd->SetSubType(t.second);
    EXPECT_EQ(t.first, fd->GetType());
    EXPECT_EQ(t.second, fd->GetSubType());
  }
}

TEST(OgrFieldDefnTest, ChangeSubTypesInvalid) {
  for (const auto& t : kAllTypes) {
    if (t == OFTInteger || t == OFTIntegerList) continue;
    auto fd = gtl::MakeUnique<OGRFieldDefn>("a", t);
    ASSERT_NE(nullptr, fd);
    fd->SetSubType(OFSTBoolean);
    EXPECT_EQ(OFSTNone, fd->GetSubType());
    fd->SetSubType(OFSTInt16);
    EXPECT_EQ(OFSTNone, fd->GetSubType());
  }

  for (const auto& t : kAllTypes) {
    if (t == OFTReal || t == OFTRealList) continue;
    auto fd = gtl::MakeUnique<OGRFieldDefn>("a", t);
    ASSERT_NE(nullptr, fd);
    fd->SetSubType(OFSTFloat32);
    EXPECT_EQ(OFSTNone, fd->GetSubType());
  }
}

TEST(OgrFieldDefnTest, Justify) {
  auto fd = gtl::MakeUnique<OGRFieldDefn>("a", OFTInteger);
  ASSERT_NE(nullptr, fd);
  EXPECT_EQ(OJUndefined, fd->GetJustify());
  fd->SetJustify(OJLeft);
  EXPECT_EQ(OJLeft, fd->GetJustify());
  fd->SetJustify(OJRight);
  EXPECT_EQ(OJRight, fd->GetJustify());
  fd->SetJustify(OJUndefined);
  EXPECT_EQ(OJUndefined, fd->GetJustify());
}

TEST(OgrFieldDefnTest, Width) {
  auto fd = gtl::MakeUnique<OGRFieldDefn>("a", OFTInteger);
  ASSERT_NE(nullptr, fd);
  EXPECT_EQ(0, fd->GetWidth());
  fd->SetWidth(123456789);
  EXPECT_EQ(123456789, fd->GetWidth());
  fd->SetWidth(-1);
  EXPECT_EQ(0, fd->GetWidth());
}

TEST(OgrFieldDefnTest, Precision) {
  auto fd = gtl::MakeUnique<OGRFieldDefn>("a", OFTInteger);
  ASSERT_NE(nullptr, fd);
  EXPECT_EQ(0, fd->GetPrecision());
  fd->SetPrecision(123456789);
  EXPECT_EQ(123456789, fd->GetPrecision());
  fd->SetPrecision(-1);
  EXPECT_EQ(-1, fd->GetPrecision());
}

TEST(OgrFieldDefnTest, Set) {
  auto fd = gtl::MakeUnique<OGRFieldDefn>("a", OFTInteger);
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
  auto fd = gtl::MakeUnique<OGRFieldDefn>("a", OFTInteger);
  EXPECT_EQ(nullptr, fd->GetDefault());

  fd->SetDefault("a");
  EXPECT_STREQ("a", fd->GetDefault());
  EXPECT_TRUE(fd->IsDefaultDriverSpecific());

  for (const auto& d : {"'a'", "-1", "NULL", "CURRENT_TIMESTAMP",
                        "CURRENT_TIME", "CURRENT_DATE"}) {
    fd->SetDefault(d);
    EXPECT_FALSE(fd->IsDefaultDriverSpecific());
  }

  fd->SetDefault(nullptr);
  EXPECT_EQ(nullptr, fd->GetDefault());
  EXPECT_FALSE(fd->IsDefaultDriverSpecific());
}

TEST(OgrFieldDefnTest, Ignored) {
  auto fd = gtl::MakeUnique<OGRFieldDefn>("a", OFTInteger);
  EXPECT_FALSE(fd->IsIgnored());
  fd->SetIgnored(true);
  EXPECT_TRUE(fd->IsIgnored());
  fd->SetIgnored(false);
  EXPECT_FALSE(fd->IsIgnored());
}

TEST(OgrFieldDefnTest, Nullable) {
  auto fd = gtl::MakeUnique<OGRFieldDefn>("a", OFTInteger);
  EXPECT_TRUE(fd->IsNullable());
  fd->SetNullable(false);
  EXPECT_FALSE(fd->IsNullable());
  fd->SetNullable(true);
  EXPECT_TRUE(fd->IsNullable());
}

TEST(OgrFieldDefnTest, IsSame) {
  auto fd = gtl::MakeUnique<OGRFieldDefn>("a", OFTInteger);
  EXPECT_TRUE(fd->IsSame(fd.get()));
  {
    const auto other = gtl::MakeUnique<OGRFieldDefn>("a", OFTInteger);
    EXPECT_TRUE(fd->IsSame(other.get()));
  }

  {
    auto other = gtl::MakeUnique<OGRFieldDefn>("a", OFTReal);
    EXPECT_FALSE(fd->IsSame(other.get()));
  }

  {
    auto other = gtl::MakeUnique<OGRFieldDefn>("a", OFTInteger);
    other->SetSubType(OFSTBoolean);
    EXPECT_FALSE(fd->IsSame(other.get()));
  }

  // Justify is ignored.
  {
    auto other = gtl::MakeUnique<OGRFieldDefn>("a", OFTInteger);
    other->SetJustify(OJLeft);
    EXPECT_TRUE(fd->IsSame(other.get()));
  }

  {
    auto other = gtl::MakeUnique<OGRFieldDefn>("a", OFTInteger);
    other->SetWidth(1);
    EXPECT_FALSE(fd->IsSame(other.get()));
  }

  {
    auto other = gtl::MakeUnique<OGRFieldDefn>("a", OFTInteger);
    other->SetPrecision(2);
    EXPECT_FALSE(fd->IsSame(other.get()));
  }

  // Default is ignored.
  {
    auto other = gtl::MakeUnique<OGRFieldDefn>("a", OFTInteger);
    other->SetDefault("a");
    EXPECT_TRUE(fd->IsSame(other.get()));
  }

  // Ignored is ignored.
  {
    auto other = gtl::MakeUnique<OGRFieldDefn>("a", OFTInteger);
    other->SetIgnored(false);
    EXPECT_TRUE(fd->IsSame(other.get()));
    other->SetIgnored(true);
    EXPECT_TRUE(fd->IsSame(other.get()));
  }

  {
    auto other = gtl::MakeUnique<OGRFieldDefn>("a", OFTInteger);
    other->SetNullable(false);
    EXPECT_FALSE(fd->IsSame(other.get()));
    other->SetNullable(true);
    EXPECT_TRUE(fd->IsSame(other.get()));
  }
}

TEST(OgrFieldDefnTest, CopyConstructor) {
  auto fd = gtl::MakeUnique<OGRFieldDefn>("a", OFTInteger);
  fd->Set("b", OFTInteger, 1, 2, OJLeft);
  fd->SetSubType(OFSTBoolean);
  fd->SetDefault("c");
  fd->SetIgnored(true);
  fd->SetNullable(false);

  auto copy = gtl::MakeUnique<OGRFieldDefn>(fd.get());

  EXPECT_STREQ("b", copy->GetNameRef());
  EXPECT_EQ(OFTInteger, copy->GetType());
  EXPECT_EQ(OFSTBoolean, copy->GetSubType());
  EXPECT_EQ(OJLeft, copy->GetJustify());
  EXPECT_EQ(1, copy->GetWidth());
  EXPECT_EQ(2, copy->GetPrecision());
  EXPECT_STREQ("c", copy->GetDefault());
  EXPECT_FALSE(copy->IsIgnored());  // Not copied
  EXPECT_FALSE(copy->IsNullable());
}

}  // namespace
}  // namespace autotest2
