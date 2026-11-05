// Copyright 2017 Google Inc. All Rights Reserved.
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

// Test OGRFeature.

// clang-format off
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_geomcoordinateprecision.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_featurestyle.h"
#include "third_party/gdal/ogr/ogr_p.h"
#include "third_party/gdal/port/cpl_string.h"
// clang-format on

#include <climits>
#include <cstdint>
#include <cstdio>
#include <initializer_list>  // NOLINT(build/include_order)
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/ogr/ogrfeaturedefn.h"
#include "third_party/gdal/ogr/ogr_core.h"

namespace autotest2 {
namespace {

class OneFeatureTest : public ::testing::Test {
 protected:
  ~OneFeatureTest() override {
    if (fd_ != nullptr) fd_->Release();
    if (f_ != nullptr) {
      for (int i = 0; i < f_->GetFieldCount(); i++) {
        f_->UnsetField(i);
        EXPECT_FALSE(f_->IsFieldSet(i));
        EXPECT_FALSE(f_->IsFieldNull(i));
        EXPECT_FALSE(f_->IsFieldSetAndNotNull(i));
      }
    }
  }

  void CreateFeature(OGRFeatureDefnBuilder* builder) {
    fd_ = builder->Build();
    f_ = std::make_unique<OGRFeature>(fd_);

    // Verify initial state.
    ASSERT_NE(nullptr, fd_);
    ASSERT_NE(nullptr, f_);
    for (int i = 0; i < f_->GetFieldCount(); i++) {
      EXPECT_FALSE(f_->IsFieldSet(i));
      EXPECT_FALSE(f_->IsFieldNull(i));
      EXPECT_FALSE(f_->IsFieldSetAndNotNull(i));

      EXPECT_EQ(0, f_->GetFieldAsInteger(0));
      EXPECT_EQ(0, f_->GetFieldAsInteger64(0));
      EXPECT_DOUBLE_EQ(0.0, f_->GetFieldAsDouble(0));
      EXPECT_STREQ("", f_->GetFieldAsString(0));
    }
  }

  OGRFeatureDefn* fd_ = nullptr;
  std::unique_ptr<OGRFeature> f_;

  // Prevent GDAL error messages from going to stderr.
  WithQuietHandler handler_;
};

TEST_F(OneFeatureTest, Empty) {
  OGRFeatureDefnBuilder builder("schema");
  CreateFeature(&builder);
  EXPECT_EQ(0, f_->GetFieldCount());
  EXPECT_EQ(0, f_->GetGeomFieldCount());
}

TEST_F(OneFeatureTest, FeatureId) {
  OGRFeatureDefnBuilder builder("schema");
  CreateFeature(&builder);
  EXPECT_EQ(OGRNullFID, f_->GetFID());
  f_->SetFID(1);
  EXPECT_EQ(1, f_->GetFID());

  f_->SetFID(std::numeric_limits<GIntBig>::min());
  EXPECT_EQ(std::numeric_limits<GIntBig>::min(), f_->GetFID());
  f_->SetFID(std::numeric_limits<GIntBig>::max());
  EXPECT_EQ(std::numeric_limits<GIntBig>::max(), f_->GetFID());
}

TEST_F(OneFeatureTest, FieldBool) {
  OGRFeatureDefnBuilder builder("schema");
  builder.AddField("b", OFTInteger, OFSTBoolean);
  CreateFeature(&builder);

  f_->SetField(0, FALSE);  // FALSE is 0.
  EXPECT_TRUE(f_->IsFieldSet(0));
  EXPECT_FALSE(f_->IsFieldNull(0));
  EXPECT_TRUE(f_->IsFieldSetAndNotNull(0));
  EXPECT_EQ(FALSE, f_->GetFieldAsInteger(0));
  EXPECT_EQ(FALSE, f_->GetFieldAsInteger64(0));
  EXPECT_DOUBLE_EQ(FALSE, f_->GetFieldAsDouble(0));

  // Anything not 0 is FALSE.
  for (int32_t v : {-1, 1, std::numeric_limits<int32_t>::min(),
                    std::numeric_limits<int32_t>::max()}) {
    f_->SetField(0, v);
    EXPECT_TRUE(f_->IsFieldSet(0));
    EXPECT_FALSE(f_->IsFieldNull(0));
    EXPECT_TRUE(f_->IsFieldSetAndNotNull(0));
    EXPECT_EQ(TRUE, f_->GetFieldAsInteger(0));
    EXPECT_EQ(TRUE, f_->GetFieldAsInteger64(0));
    EXPECT_DOUBLE_EQ(TRUE, f_->GetFieldAsDouble(0));
  }

  f_->SetField(0, 0.0);
  EXPECT_EQ(FALSE, f_->GetFieldAsInteger(0));
  f_->SetField(0, 0.9);
  EXPECT_EQ(FALSE, f_->GetFieldAsInteger(0));
  f_->SetField(0, -0.9);
  EXPECT_EQ(FALSE, f_->GetFieldAsInteger(0));

  f_->SetField(0, 1.0);
  EXPECT_EQ(TRUE, f_->GetFieldAsInteger(0));

  f_->SetField(0, -1.0);
  EXPECT_EQ(TRUE, f_->GetFieldAsInteger(0));
  f_->SetField(0, 2.0);
  EXPECT_EQ(TRUE, f_->GetFieldAsInteger(0));
}

TEST_F(OneFeatureTest, FieldInt16) {
  OGRFeatureDefnBuilder builder("schema");
  builder.AddField("i", OFTInteger, OFSTInt16);
  CreateFeature(&builder);

  std::initializer_list<int64_t> values = {
      0,
      -1,
      1,
      std::numeric_limits<int16_t>::min(),
      std::numeric_limits<int16_t>::max(),
  };
  for (int16_t v : values) {
    f_->SetField(0, v);
    EXPECT_TRUE(f_->IsFieldSet(0));
    EXPECT_FALSE(f_->IsFieldNull(0));
    EXPECT_TRUE(f_->IsFieldSetAndNotNull(0));
    EXPECT_EQ(v, f_->GetFieldAsInteger(0));
    EXPECT_EQ(v, f_->GetFieldAsInteger64(0));
    EXPECT_DOUBLE_EQ(v, f_->GetFieldAsDouble(0));

    f_->UnsetField(0);
    f_->SetField("i", v);
    EXPECT_EQ(v, f_->GetFieldAsInteger(0));

    f_->UnsetField(0);
    f_->SetField(0, static_cast<GIntBig>(v));
    EXPECT_EQ(v, f_->GetFieldAsInteger(0));

    f_->UnsetField(0);
    f_->SetField(0, static_cast<double>(v));
    EXPECT_EQ(v, f_->GetFieldAsInteger(0));
  }

  // Over/under range.
  f_->SetField(0, std::numeric_limits<int32_t>::min());
  EXPECT_EQ(std::numeric_limits<int16_t>::min(), f_->GetFieldAsInteger(0));
  EXPECT_EQ(std::numeric_limits<int16_t>::min(), f_->GetFieldAsInteger64(0));
  f_->SetField(0, std::numeric_limits<int32_t>::max());
  EXPECT_EQ(std::numeric_limits<int16_t>::max(), f_->GetFieldAsInteger(0));
  EXPECT_EQ(std::numeric_limits<int16_t>::max(), f_->GetFieldAsInteger64(0));
}

TEST_F(OneFeatureTest, FieldInt32) {
  OGRFeatureDefnBuilder builder("schema");
  builder.AddField("i", OFTInteger, OFSTNone);
  CreateFeature(&builder);

  for (int32_t v : {0, -1, 1, std::numeric_limits<int32_t>::min(),
                    std::numeric_limits<int32_t>::max()}) {
    f_->SetField(0, v);
    EXPECT_TRUE(f_->IsFieldSet(0));
    EXPECT_FALSE(f_->IsFieldNull(0));
    EXPECT_TRUE(f_->IsFieldSetAndNotNull(0));
    EXPECT_EQ(v, f_->GetFieldAsInteger(0));
    EXPECT_EQ(v, f_->GetFieldAsInteger64(0));
    EXPECT_DOUBLE_EQ(v, f_->GetFieldAsDouble(0));

    f_->UnsetField(0);
    f_->SetField("i", v);
    EXPECT_EQ(v, f_->GetFieldAsInteger(0));

    f_->UnsetField(0);
    f_->SetField(0, static_cast<GIntBig>(v));
    EXPECT_EQ(v, f_->GetFieldAsInteger(0));

    f_->UnsetField(0);
    f_->SetField(0, static_cast<double>(v));
    EXPECT_EQ(v, f_->GetFieldAsInteger(0));
  }

  // Over/under range.
  f_->SetField(0, std::numeric_limits<GIntBig>::min());
  EXPECT_EQ(std::numeric_limits<int32_t>::min(), f_->GetFieldAsInteger(0));
  EXPECT_EQ(std::numeric_limits<int32_t>::min(), f_->GetFieldAsInteger64(0));
  f_->SetField(0, std::numeric_limits<GIntBig>::max());
  EXPECT_EQ(std::numeric_limits<int32_t>::max(), f_->GetFieldAsInteger(0));
  EXPECT_EQ(std::numeric_limits<int32_t>::max(), f_->GetFieldAsInteger64(0));

  f_->SetFieldNull(0);
  EXPECT_TRUE(f_->IsFieldSet(0));
  EXPECT_TRUE(f_->IsFieldNull(0));
  EXPECT_FALSE(f_->IsFieldSetAndNotNull(0));
  EXPECT_EQ(0, f_->GetFieldAsInteger(0));
}

namespace {

int32_t ClampToInt32(int64_t v64) {
  if (v64 < std::numeric_limits<int32_t>::min())
    return std::numeric_limits<int32_t>::min();

  if (v64 > std::numeric_limits<int32_t>::max())
    return std::numeric_limits<int32_t>::max();

  return static_cast<int32_t>(v64);
}

}  // namespace

TEST_F(OneFeatureTest, FieldInt64) {
  OGRFeatureDefnBuilder builder("schema");
  builder.AddField("i", OFTInteger64, OFSTNone);
  CreateFeature(&builder);

  std::initializer_list<int64_t> values = {
      0,
      -1,
      1,
      std::numeric_limits<int32_t>::min(),
      std::numeric_limits<int32_t>::max(),
      std::numeric_limits<int64_t>::min(),
      std::numeric_limits<int64_t>::max(),
  };
  for (GIntBig v : values) {
    f_->SetField(0, v);
    EXPECT_TRUE(f_->IsFieldSet(0));
    EXPECT_FALSE(f_->IsFieldNull(0));
    EXPECT_TRUE(f_->IsFieldSetAndNotNull(0));

    EXPECT_EQ(ClampToInt32(v), f_->GetFieldAsInteger(0));

    EXPECT_EQ(v, f_->GetFieldAsInteger64(0));
    EXPECT_DOUBLE_EQ(v, f_->GetFieldAsDouble(0));

    f_->UnsetField(0);
    f_->SetField("i", v);
    EXPECT_EQ(v, f_->GetFieldAsInteger64(0));

    f_->UnsetField(0);
    f_->SetField(0, static_cast<GIntBig>(v));
    EXPECT_EQ(v, f_->GetFieldAsInteger64(0));

    // Operating at the min and max is not safe when casting through a double.
    f_->UnsetField(0);
    if (v < std::numeric_limits<int32_t>::min()) {
      int64_t v2 =
          static_cast<int64_t>(std::numeric_limits<int32_t>::min()) * 10;
      f_->SetField(0, static_cast<double>(v2));
      EXPECT_EQ(v2, f_->GetFieldAsInteger64(0));
    } else if (v > std::numeric_limits<int32_t>::min()) {
      int64_t v2 =
          static_cast<int64_t>(std::numeric_limits<int32_t>::max()) * 10;
      f_->SetField(0, static_cast<double>(v2));
      EXPECT_EQ(v2, f_->GetFieldAsInteger64(0));
    } else {
      f_->SetField(0, static_cast<double>(v));
      EXPECT_EQ(v, f_->GetFieldAsInteger64(0));
    }
  }

  f_->SetFieldNull(0);
  EXPECT_TRUE(f_->IsFieldSet(0));
  EXPECT_TRUE(f_->IsFieldNull(0));
  EXPECT_FALSE(f_->IsFieldSetAndNotNull(0));
  EXPECT_EQ(0, f_->GetFieldAsInteger(0));
}

TEST_F(OneFeatureTest, FieldFloat32) {
  OGRFeatureDefnBuilder builder("schema");
  builder.AddField("f", OFTReal, OFSTFloat32);
  CreateFeature(&builder);
  for (float v : {0.0f, -0.1f, 0.1f, std::numeric_limits<float>::min(),
                  std::numeric_limits<float>::lowest(),
                  std::numeric_limits<float>::max()}) {
    f_->SetField(0, v);
    EXPECT_TRUE(f_->IsFieldSet(0));
    EXPECT_FALSE(f_->IsFieldNull(0));
    EXPECT_TRUE(f_->IsFieldSetAndNotNull(0));
    EXPECT_DOUBLE_EQ(v, f_->GetFieldAsDouble(0));

    f_->UnsetField(0);
    f_->SetField(0, static_cast<double>(v));
    EXPECT_DOUBLE_EQ(v, f_->GetFieldAsDouble(0));
  }

  // Over/under range.
  // TODO(schwehr): Does not clip.  This seems like a bug.
  f_->SetField(0, std::numeric_limits<double>::lowest());
  EXPECT_DOUBLE_EQ(std::numeric_limits<double>::lowest(),
                   f_->GetFieldAsDouble(0));
  f_->SetField(0, std::numeric_limits<double>::max());
  EXPECT_DOUBLE_EQ(std::numeric_limits<double>::max(), f_->GetFieldAsDouble(0));

  f_->SetField(0, std::numeric_limits<float>::infinity());
  EXPECT_DOUBLE_EQ(std::numeric_limits<double>::infinity(),
                   f_->GetFieldAsDouble(0));

  f_->SetField(0, -std::numeric_limits<float>::infinity());
  EXPECT_DOUBLE_EQ(-std::numeric_limits<double>::infinity(),
                   f_->GetFieldAsDouble(0));

  f_->SetFieldNull(0);
  EXPECT_TRUE(f_->IsFieldSet(0));
  EXPECT_TRUE(f_->IsFieldNull(0));
  EXPECT_FALSE(f_->IsFieldSetAndNotNull(0));
  EXPECT_EQ(0, f_->GetFieldAsDouble(0));
}

TEST_F(OneFeatureTest, FieldFloat64) {
  OGRFeatureDefnBuilder builder("schema");
  builder.AddField("f", OFTReal, OFSTNone);
  CreateFeature(&builder);

  for (double v : {0.0, -0.1, 0.1, std::numeric_limits<double>::min(),
                   std::numeric_limits<double>::lowest(),
                   std::numeric_limits<double>::max()}) {
    f_->SetField(0, v);
    EXPECT_TRUE(f_->IsFieldSet(0));
    EXPECT_FALSE(f_->IsFieldNull(0));
    EXPECT_TRUE(f_->IsFieldSetAndNotNull(0));
    EXPECT_DOUBLE_EQ(v, f_->GetFieldAsDouble(0));

    f_->UnsetField(0);
  }

  f_->SetField(0, std::numeric_limits<double>::infinity());
  EXPECT_DOUBLE_EQ(std::numeric_limits<double>::infinity(),
                   f_->GetFieldAsDouble(0));

  f_->SetField(0, -std::numeric_limits<double>::infinity());
  EXPECT_DOUBLE_EQ(-std::numeric_limits<double>::infinity(),
                   f_->GetFieldAsDouble(0));

  f_->SetFieldNull(0);
  EXPECT_TRUE(f_->IsFieldSet(0));
  EXPECT_TRUE(f_->IsFieldNull(0));
  EXPECT_FALSE(f_->IsFieldSetAndNotNull(0));
  EXPECT_EQ(0, f_->GetFieldAsDouble(0));
}

TEST_F(OneFeatureTest, FieldString) {
  OGRFeatureDefnBuilder builder("schema");
  builder.AddField("f", OFTString, OFSTNone);
  CreateFeature(&builder);

  f_->SetField(0, "");
  EXPECT_STREQ("", f_->GetFieldAsString(0));
  EXPECT_EQ(0, f_->GetFieldAsInteger(0));
  EXPECT_DOUBLE_EQ(0.0, f_->GetFieldAsDouble(0));

  f_->SetField(0, "abc");
  EXPECT_STREQ("abc", f_->GetFieldAsString(0));
  EXPECT_EQ(0, f_->GetFieldAsInteger(0));
  EXPECT_DOUBLE_EQ(0.0, f_->GetFieldAsDouble(0));

  f_->SetField(0, "42");
  EXPECT_STREQ("42", f_->GetFieldAsString(0));
  EXPECT_EQ(42, f_->GetFieldAsInteger(0));
  EXPECT_DOUBLE_EQ(42.0, f_->GetFieldAsDouble(0));

  f_->SetField(0, "-1.2");
  EXPECT_STREQ("-1.2", f_->GetFieldAsString(0));
  EXPECT_EQ(-1, f_->GetFieldAsInteger(0));
  EXPECT_DOUBLE_EQ(-1.2, f_->GetFieldAsDouble(0));

  EXPECT_EQ(nullptr, f_->GetFieldAsSerializedJSon(0));
}

TEST_F(OneFeatureTest, FieldDateTime) {
  OGRFeatureDefnBuilder builder("schema");
  builder.AddField("dt", OFTDateTime, OFSTNone);
  CreateFeature(&builder);

  // The datetime within an OGRField isn't properly initialized.
  int year;
  int month;
  int day;
  int hour;
  int minute;
  int second;
  int tzflag;

  f_->SetField(0, 1501, 2, 3);
  EXPECT_TRUE(f_->GetFieldAsDateTime(0, &year, &month, &day, &hour, &minute,
                                     &second, &tzflag));
  EXPECT_THAT(std::initializer_list<int>(
                  {year, month, day, hour, minute, second, tzflag}),
              testing::ElementsAre(1501, 2, 3, 0, 0, 0, 0));
  EXPECT_STREQ("1501/02/03 00:00:00", f_->GetFieldAsString(0));
  EXPECT_EQ(nullptr, f_->GetFieldAsSerializedJSon(0));

  f_->SetField(0, 1970, 1, 1);
  f_->GetFieldAsDateTime(0, &year, &month, &day, &hour, &minute, &second,
                         &tzflag);
  EXPECT_THAT(std::initializer_list<int>(
                  {year, month, day, hour, minute, second, tzflag}),
              testing::ElementsAre(1970, 1, 1, 0, 0, 0, 0));

  float second_float;
  f_->SetField(0, 2017, 8, 18, 12, 59, 20.123f, 0);
  f_->GetFieldAsDateTime(0, &year, &month, &day, &hour, &minute, &second_float,
                         &tzflag);
  EXPECT_THAT(
      std::initializer_list<int>({year, month, day, hour, minute, tzflag}),
      testing::ElementsAre(2017, 8, 18, 12, 59, 0));
  EXPECT_FLOAT_EQ(second_float, 20.123f);
  EXPECT_STREQ("2017/08/18 12:59:20.123", f_->GetFieldAsString(0));

  EXPECT_STREQ(nullptr, f_->GetFieldAsSerializedJSon(0));

  // TODO(schwehr): Time zones.
}

// TODO(schwehr): Test IntegerList
// TODO(schwehr): Test Integer64List
// TODO(schwehr): Test DoubleList
// TODO(schwehr): Test StringList
// TODO(schwehr): Test Binary

// TODO(schwehr): Testsgeometries
// TODO(schwehr): Test style
// TODO(schwehr): Test dump
// TODO(schwehr): Test Validate
// TODO(schwehr): Test Equal

TEST(FeatureTest, StaticCreateAndDestroy) {
  OGRFeatureDefnBuilder builder("schema");
  auto fd = builder.Build();
  auto fd_cleaner = absl::MakeCleanup([fd] { fd->Release(); });
  auto f = OGRFeature::CreateFeature(fd);
  ASSERT_NE(nullptr, f);
  EXPECT_EQ(0, f->GetFieldCount());
  EXPECT_EQ(0, f->GetGeomFieldCount());
  OGRFeature::DestroyFeature(f);
  // Success if msan does not find trouble.
}

}  // namespace

TEST(FeatureExtendedTest, FieldListsAndBinary) {
  OGRFeatureDefn* poDefn = new OGRFeatureDefn("list_schema");
  poDefn->SetGeomType(wkbNone);
  poDefn->Reference();
  auto defn_cleaner = absl::MakeCleanup([poDefn] { poDefn->Release(); });

  OGRFieldDefn fIntList("int_list", OFTIntegerList);
  OGRFieldDefn fInt64List("int64_list", OFTInteger64List);
  OGRFieldDefn fRealList("real_list", OFTRealList);
  OGRFieldDefn fStrList("str_list", OFTStringList);
  OGRFieldDefn fBinary("bin", OFTBinary);

  poDefn->AddFieldDefn(&fIntList);
  poDefn->AddFieldDefn(&fInt64List);
  poDefn->AddFieldDefn(&fRealList);
  poDefn->AddFieldDefn(&fStrList);
  poDefn->AddFieldDefn(&fBinary);

  auto f = std::make_unique<OGRFeature>(poDefn);

  // IntegerList
  {
    const int vals[] = {10, 20, 30};
    f->SetField(0, 3, vals);
    EXPECT_TRUE(f->IsFieldSet(0));
    int count = 0;
    const int* ret = f->GetFieldAsIntegerList(0, &count);
    ASSERT_EQ(3, count);
    EXPECT_EQ(10, ret[0]);
    EXPECT_EQ(20, ret[1]);
    EXPECT_EQ(30, ret[2]);

    // Conversions
    const GIntBig* ret64 = f->GetFieldAsInteger64List(0, &count);
    EXPECT_EQ(0, count);
    EXPECT_EQ(nullptr, ret64);
    const double* retD = f->GetFieldAsDoubleList(0, &count);
    EXPECT_EQ(0, count);
    EXPECT_EQ(nullptr, retD);
    char** retS = f->GetFieldAsStringList(0);
    EXPECT_EQ(nullptr, retS);
    EXPECT_STREQ("(3:10,20,30)", f->GetFieldAsString(0));
    EXPECT_EQ(0, f->GetFieldAsInteger(0));
    EXPECT_DOUBLE_EQ(0.0, f->GetFieldAsDouble(0));

    // Access by name
    ret = f->GetFieldAsIntegerList("int_list", &count);
    EXPECT_EQ(3, count);
    ret64 = f->GetFieldAsInteger64List("int_list", &count);
    EXPECT_EQ(0, count);
    retD = f->GetFieldAsDoubleList("int_list", &count);
    EXPECT_EQ(0, count);
    EXPECT_STREQ("(3:10,20,30)", f->GetFieldAsString("int_list"));

    // Set by name
    const int new_vals[] = {42};
    f->SetField("int_list", 1, new_vals);
    ret = f->GetFieldAsIntegerList(0, &count);
    ASSERT_EQ(1, count);
    EXPECT_EQ(42, ret[0]);

    // Empty list
    f->SetField(0, 0, static_cast<const int*>(nullptr));
    ret = f->GetFieldAsIntegerList(0, &count);
    EXPECT_EQ(0, count);
    EXPECT_STREQ("(0:)", f->GetFieldAsString(0));
  }

  // Integer64List
  {
    const GIntBig vals[] = {100000000000LL, 200000000000LL};
    f->SetField(1, 2, vals);
    int count = 0;
    const GIntBig* ret = f->GetFieldAsInteger64List(1, &count);
    ASSERT_EQ(2, count);
    EXPECT_EQ(100000000000LL, ret[0]);
    EXPECT_EQ(200000000000LL, ret[1]);

    const int* ret32 = f->GetFieldAsIntegerList(1, &count);
    EXPECT_EQ(0, count);
    const double* retD = f->GetFieldAsDoubleList(1, &count);
    EXPECT_EQ(0, count);
    char** retS = f->GetFieldAsStringList(1);
    EXPECT_EQ(nullptr, retS);
    EXPECT_STREQ("(2:100000000000,200000000000)", f->GetFieldAsString(1));

    // Access by name
    ret = f->GetFieldAsInteger64List("int64_list", &count);
    EXPECT_EQ(2, count);
    ret32 = f->GetFieldAsIntegerList("int64_list", &count);
    EXPECT_EQ(0, count);
    retD = f->GetFieldAsDoubleList("int64_list", &count);
    EXPECT_EQ(0, count);

    // Set by name
    const GIntBig new_vals[] = {99LL};
    f->SetField("int64_list", 1, new_vals);
    ret = f->GetFieldAsInteger64List(1, &count);
    ASSERT_EQ(1, count);
    EXPECT_EQ(99LL, ret[0]);

    // Empty list
    f->SetField(1, 0, static_cast<const GIntBig*>(nullptr));
    ret = f->GetFieldAsInteger64List(1, &count);
    EXPECT_EQ(0, count);
  }

  // RealList
  {
    const double vals[] = {1.5, 2.5, 3.5};
    f->SetField(2, 3, vals);
    int count = 0;
    const double* ret = f->GetFieldAsDoubleList(2, &count);
    ASSERT_EQ(3, count);
    EXPECT_DOUBLE_EQ(1.5, ret[0]);

    const int* ret32 = f->GetFieldAsIntegerList(2, &count);
    EXPECT_EQ(0, count);
    const GIntBig* ret64 = f->GetFieldAsInteger64List(2, &count);
    EXPECT_EQ(0, count);
    char** retS = f->GetFieldAsStringList(2);
    EXPECT_EQ(nullptr, retS);
    EXPECT_STREQ("(3:1.5,2.5,3.5)", f->GetFieldAsString(2));

    // Access by name
    ret = f->GetFieldAsDoubleList("real_list", &count);
    EXPECT_EQ(3, count);
    ret32 = f->GetFieldAsIntegerList("real_list", &count);
    EXPECT_EQ(0, count);
    ret64 = f->GetFieldAsInteger64List("real_list", &count);
    EXPECT_EQ(0, count);

    // Set by name
    const double new_vals[] = {7.7};
    f->SetField("real_list", 1, new_vals);
    ret = f->GetFieldAsDoubleList(2, &count);
    ASSERT_EQ(1, count);
    EXPECT_DOUBLE_EQ(7.7, ret[0]);

    // Empty list
    f->SetField(2, 0, static_cast<const double*>(nullptr));
    ret = f->GetFieldAsDoubleList(2, &count);
    EXPECT_EQ(0, count);
  }

  // StringList
  {
    const char* vals[] = {"apple", "banana", "cherry", nullptr};
    f->SetField(3, const_cast<char* const*>(vals));
    char** ret = f->GetFieldAsStringList(3);
    ASSERT_THAT(ret, testing::NotNull());
    EXPECT_STREQ("apple", ret[0]);
    EXPECT_STREQ("banana", ret[1]);
    EXPECT_STREQ("cherry", ret[2]);
    EXPECT_STREQ("(3:apple,banana,cherry)", f->GetFieldAsString(3));

    int count = 0;
    const int* ret32 = f->GetFieldAsIntegerList(3, &count);
    EXPECT_EQ(0, count);
    EXPECT_EQ(nullptr, ret32);
    const GIntBig* ret64 = f->GetFieldAsInteger64List(3, &count);
    EXPECT_EQ(0, count);
    EXPECT_EQ(nullptr, ret64);
    const double* retD = f->GetFieldAsDoubleList(3, &count);
    EXPECT_EQ(0, count);
    EXPECT_EQ(nullptr, retD);

    // Set by name
    const char* new_vals[] = {"orange", nullptr};
    f->SetField("str_list", const_cast<char* const*>(new_vals));
    ret = f->GetFieldAsStringList(3);
    EXPECT_STREQ("orange", ret[0]);

    // Empty list
    const char* empty_vals[] = {nullptr};
    f->SetField(3, const_cast<char* const*>(empty_vals));
    ret = f->GetFieldAsStringList(3);
    EXPECT_EQ(0, CSLCount(ret));
  }

  // Binary
  {
    const unsigned char bytes[] = {0xDE, 0xAD, 0xBE, 0xEF};
    f->SetField(4, 4, bytes);
    int count = 0;
    GByte* ret = f->GetFieldAsBinary(4, &count);
    ASSERT_EQ(4, count);
    EXPECT_EQ(0xDE, ret[0]);
    EXPECT_EQ(0xAD, ret[1]);
    EXPECT_EQ(0xBE, ret[2]);
    EXPECT_EQ(0xEF, ret[3]);

    EXPECT_STREQ("DEADBEEF", f->GetFieldAsString(4));
    EXPECT_EQ(0, f->GetFieldAsInteger(4));
    EXPECT_EQ(0, f->GetFieldAsInteger64(4));
    EXPECT_DOUBLE_EQ(0.0, f->GetFieldAsDouble(4));
  }
}

TEST(FeatureExtendedTest, DateTimeAndISO8601) {
  OGRFeatureDefn* poDefn = new OGRFeatureDefn("dt_schema");
  poDefn->SetGeomType(wkbNone);
  poDefn->Reference();
  auto defn_cleaner = absl::MakeCleanup([poDefn] { poDefn->Release(); });

  OGRFieldDefn fDate("d", OFTDate);
  OGRFieldDefn fTime("t", OFTTime);
  OGRFieldDefn fDateTime("dt", OFTDateTime);

  poDefn->AddFieldDefn(&fDate);
  poDefn->AddFieldDefn(&fTime);
  poDefn->AddFieldDefn(&fDateTime);

  auto f = std::make_unique<OGRFeature>(poDefn);

  // Date
  f->SetField(0, 2024, 11, 5, 0, 0, 0.0f, 0);
  EXPECT_STREQ("2024/11/05", f->GetFieldAsString(0));
  int y, m, d, h, mn, tz;
  float s;
  EXPECT_TRUE(f->GetFieldAsDateTime(0, &y, &m, &d, &h, &mn, &s, &tz));
  EXPECT_EQ(2024, y);
  EXPECT_EQ(11, m);
  EXPECT_EQ(5, d);

  // Time
  f->SetField(1, 0, 0, 0, 14, 30, 45.5f, 100);  // UTC (100 = 100/4 = 0h offset)
  EXPECT_STREQ("14:30:45.500", f->GetFieldAsString(1));

  // DateTime with positive timezone (UTC+1: tz = 104)
  f->SetField(2, 2024, 11, 5, 15, 30, 25.0f, 104);
  EXPECT_STREQ("2024/11/05 15:30:25+01", f->GetFieldAsString(2));

  // ISO8601 formatting
  const char* iso = f->GetFieldAsISO8601DateTime(2, nullptr);
  EXPECT_STREQ("2024-11-05T15:30:25+01:00", iso);

  // ISO8601 by field name
  iso = f->GetFieldAsISO8601DateTime("dt", nullptr);
  EXPECT_STREQ("2024-11-05T15:30:25+01:00", iso);

  // DateTime with negative timezone (UTC-5: tz = 80)
  f->SetField(2, 2024, 11, 5, 10, 0, 0.0f, 80);
  EXPECT_STREQ("2024-11-05T10:00:00-05:00",
               f->GetFieldAsISO8601DateTime(2, nullptr));

  // DateTime with partial timezone (UTC+5:30: tz = 122)
  f->SetField(2, 2024, 11, 5, 20, 30, 0.0f, 122);
  EXPECT_STREQ("2024-11-05T20:30:00+05:30",
               f->GetFieldAsISO8601DateTime(2, nullptr));

  // DateTime with unknown timezone (tz = 0)
  f->SetField(2, 2024, 11, 5, 12, 0, 0.0f, 0);
  EXPECT_STREQ("2024-11-05T12:00:00", f->GetFieldAsISO8601DateTime(2, nullptr));

  // Integer and Double conversions from DateTime return 0
  EXPECT_EQ(0, f->GetFieldAsInteger(2));
  EXPECT_DOUBLE_EQ(0.0, f->GetFieldAsDouble(2));

  // Set DateTime by name
  f->SetField("dt", 2025, 1, 1, 0, 0, 0.0f, 100);
  EXPECT_STREQ("2025-01-01T00:00:00Z",
               f->GetFieldAsISO8601DateTime("dt", nullptr));
}

TEST(FeatureExtendedTest, Geometries) {
  OGRFeatureDefn* poDefn = new OGRFeatureDefn("geom_schema");
  poDefn->SetGeomType(wkbNone);
  poDefn->Reference();
  auto defn_cleaner = absl::MakeCleanup([poDefn] { poDefn->Release(); });

  OGRGeomFieldDefn gf0("geom0", wkbPoint);
  OGRGeomFieldDefn gf1("geom1", wkbLineString);
  auto srs4326 = new OGRSpatialReference();
  srs4326->importFromEPSG(4326);
  gf0.SetSpatialRef(srs4326);
  srs4326->Release();

  poDefn->AddGeomFieldDefn(&gf0);
  poDefn->AddGeomFieldDefn(&gf1);

  auto f = std::make_unique<OGRFeature>(poDefn);

  EXPECT_EQ(2, f->GetGeomFieldCount());
  EXPECT_EQ(0, f->GetGeomFieldIndex("geom0"));
  EXPECT_EQ(1, f->GetGeomFieldIndex("geom1"));
  EXPECT_EQ(-1, f->GetGeomFieldIndex("nonexistent"));

  EXPECT_STREQ("geom0", f->GetGeomFieldDefnRef(0)->GetNameRef());
  EXPECT_STREQ("geom1", f->GetGeomFieldDefnRef(1)->GetNameRef());

  // SetGeometryDirectly (field 0)
  auto pt = new OGRPoint(10, 20);
  EXPECT_EQ(OGRERR_NONE, f->SetGeometryDirectly(pt));
  ASSERT_THAT(f->GetGeometryRef(), testing::NotNull());
  EXPECT_EQ(wkbPoint, f->GetGeometryRef()->getGeometryType());
  EXPECT_EQ(f->GetGeometryRef(), f->GetGeomFieldRef(0));
  EXPECT_EQ(f->GetGeometryRef(), f->GetGeomFieldRef("geom0"));

  // Const GetGeometryRef
  const OGRFeature* cf = f.get();
  EXPECT_EQ(pt, cf->GetGeometryRef());
  EXPECT_EQ(pt, cf->GetGeomFieldRef(0));
  EXPECT_EQ(pt, cf->GetGeomFieldRef("geom0"));

  // StealGeometry (field 0)
  OGRGeometry* stolen = f->StealGeometry();
  EXPECT_EQ(pt, stolen);
  EXPECT_EQ(nullptr, f->GetGeometryRef());
  delete stolen;

  // SetGeometry const reference
  OGRPoint pt2(30, 40);
  EXPECT_EQ(OGRERR_NONE, f->SetGeometry(&pt2));
  ASSERT_THAT(f->GetGeometryRef(), testing::NotNull());
  EXPECT_DOUBLE_EQ(30.0, f->GetGeometryRef()->toPoint()->getX());

  // SetGeometry unique_ptr
  auto pt3 = std::make_unique<OGRPoint>(50, 60);
  EXPECT_EQ(OGRERR_NONE, f->SetGeometry(std::move(pt3)));
  EXPECT_DOUBLE_EQ(50.0, f->GetGeometryRef()->toPoint()->getX());

  // Field 1: SetGeomFieldDirectly
  auto ls = new OGRLineString();
  ls->addPoint(0, 0);
  ls->addPoint(1, 1);
  EXPECT_EQ(OGRERR_NONE, f->SetGeomFieldDirectly(1, ls));
  EXPECT_EQ(ls, f->GetGeomFieldRef(1));
  EXPECT_EQ(ls, f->GetGeomFieldRef("geom1"));

  // StealGeometry(1)
  stolen = f->StealGeometry(1);
  EXPECT_EQ(ls, stolen);
  EXPECT_EQ(nullptr, f->GetGeomFieldRef(1));
  delete stolen;

  // SetGeomField const ref
  OGRLineString ls2;
  ls2.addPoint(2, 2);
  ls2.addPoint(3, 3);
  EXPECT_EQ(OGRERR_NONE, f->SetGeomField(1, &ls2));
  ASSERT_THAT(f->GetGeomFieldRef(1), testing::NotNull());

  // SetGeomField unique_ptr
  auto ls3 = std::make_unique<OGRLineString>();
  ls3->addPoint(4, 4);
  ls3->addPoint(5, 5);
  EXPECT_EQ(OGRERR_NONE, f->SetGeomField(1, std::move(ls3)));
  ASSERT_THAT(f->GetGeomFieldRef(1), testing::NotNull());

  // SpatialReference: assign SRS to geometry
  auto ptSRS = new OGRPoint(1, 2);
  auto srsWGS84 = new OGRSpatialReference();
  srsWGS84->importFromEPSG(4326);
  ptSRS->assignSpatialReference(srsWGS84);
  srsWGS84->Release();
  f->SetGeomFieldDirectly(0, ptSRS);
  ASSERT_THAT(f->GetGeomFieldRef(0)->getSpatialReference(), testing::NotNull());
  EXPECT_STREQ("WGS 84",
               f->GetGeomFieldRef(0)->getSpatialReference()->GetName());
}

TEST(FeatureExtendedTest, LifecycleAndModification) {
  OGRFeatureDefn* poDefn = new OGRFeatureDefn("mod_schema");
  poDefn->SetGeomType(wkbNone);
  poDefn->Reference();
  auto defn_cleaner = absl::MakeCleanup([poDefn] { poDefn->Release(); });

  OGRFieldDefn fStr("name", OFTString);
  fStr.SetDefault("'default_name'");
  fStr.SetNullable(FALSE);

  OGRFieldDefn fInt("age", OFTInteger);
  fInt.SetDefault("25");

  OGRGeomFieldDefn gf("geom", wkbPoint);
  gf.SetNullable(FALSE);

  poDefn->AddFieldDefn(&fStr);
  poDefn->AddFieldDefn(&fInt);
  poDefn->AddGeomFieldDefn(&gf);

  auto f = std::make_unique<OGRFeature>(poDefn);
  f->SetFID(100);
  f->SetField(0, "Alice");
  f->SetField(1, 30);
  f->SetGeometryDirectly(new OGRPoint(1, 2));
  f->SetStyleString("@pen(c:#FF0000)");
  f->SetNativeData("native_blob");
  f->SetNativeMediaType("application/json");

  // Clone
  std::unique_ptr<OGRFeature> fClone(f->Clone());
  ASSERT_THAT(fClone, testing::NotNull());
  EXPECT_TRUE(fClone->Equal(f.get()));

  // Reset
  f->Reset();
  EXPECT_EQ(OGRNullFID, f->GetFID());
  EXPECT_FALSE(f->IsFieldSet(0));
  EXPECT_FALSE(f->IsFieldSet(1));
  EXPECT_EQ(nullptr, f->GetGeometryRef());
  EXPECT_EQ(nullptr, f->GetStyleString());
  EXPECT_EQ(nullptr, f->GetNativeData());

  // FillUnsetWithDefault
  f->FillUnsetWithDefault(FALSE, nullptr);
  EXPECT_TRUE(f->IsFieldSet(0));
  EXPECT_STREQ("default_name", f->GetFieldAsString(0));
  EXPECT_TRUE(f->IsFieldSet(1));
  EXPECT_EQ(25, f->GetFieldAsInteger(1));

  // Validate
  // Geometry is currently NULL, but gf is NOT NULL -> Validate fails
  EXPECT_FALSE(f->Validate(OGR_F_VAL_ALL, FALSE));

  f->SetGeometryDirectly(new OGRPoint(10, 20));
  EXPECT_TRUE(f->Validate(OGR_F_VAL_ALL, FALSE));

  // Invalidate field 0 by unsetting (nullable is FALSE)
  f->UnsetField(0);
  EXPECT_FALSE(f->Validate(OGR_F_VAL_NULL, FALSE));

  // Validate with error emission (WithQuietHandler prevents stderr spam)
  WithQuietHandler quiet;
  EXPECT_FALSE(f->Validate(OGR_F_VAL_NULL, TRUE));

  // SetFDefnUnsafe
  OGRFeatureDefn* poNewDefn = new OGRFeatureDefn("new_schema");
  poNewDefn->SetGeomType(wkbNone);
  poNewDefn->Reference();
  auto new_defn_cleaner =
      absl::MakeCleanup([poNewDefn] { poNewDefn->Release(); });
  OGRFieldDefn fStr2("name", OFTString);
  poNewDefn->AddFieldDefn(&fStr2);
  f->SetFDefnUnsafe(poNewDefn);
  EXPECT_EQ(poNewDefn, f->GetDefnRef());

  // Restore defn before destruction
  f->SetFDefnUnsafe(poDefn);
}

TEST(FeatureExtendedTest, RemapAndAppend) {
  OGRFeatureDefn* poDefn1 = new OGRFeatureDefn("schema1");
  poDefn1->SetGeomType(wkbNone);
  poDefn1->Reference();
  auto d1_cleaner = absl::MakeCleanup([poDefn1] { poDefn1->Release(); });

  OGRFieldDefn fA("A", OFTInteger);
  OGRFieldDefn fB("B", OFTString);
  OGRFieldDefn fC("C", OFTReal);
  poDefn1->AddFieldDefn(&fA);
  poDefn1->AddFieldDefn(&fB);
  poDefn1->AddFieldDefn(&fC);

  OGRGeomFieldDefn gA("gA", wkbPoint);
  OGRGeomFieldDefn gB("gB", wkbLineString);
  poDefn1->AddGeomFieldDefn(&gA);
  poDefn1->AddGeomFieldDefn(&gB);

  // Defn 2 with reordered fields: C, A, B
  OGRFeatureDefn* poDefn2 = new OGRFeatureDefn("schema2");
  poDefn2->SetGeomType(wkbNone);
  poDefn2->Reference();
  auto d2_cleaner = absl::MakeCleanup([poDefn2] { poDefn2->Release(); });
  poDefn2->AddFieldDefn(&fC);
  poDefn2->AddFieldDefn(&fA);
  poDefn2->AddFieldDefn(&fB);

  OGRGeomFieldDefn gB_copy("gB", wkbLineString);
  OGRGeomFieldDefn gA_copy("gA", wkbPoint);
  poDefn2->AddGeomFieldDefn(&gB_copy);
  poDefn2->AddGeomFieldDefn(&gA_copy);

  auto f = std::make_unique<OGRFeature>(poDefn1);
  f->SetField(0, 10);
  f->SetField(1, "hello");
  f->SetField(2, 3.14);
  f->SetGeomFieldDirectly(0, new OGRPoint(1, 2));
  f->SetGeomFieldDirectly(1, new OGRLineString());

  // Balance reference counts for in-place defn change
  poDefn2->Reference();
  poDefn1->Release();

  // panRemapSource maps each new field to index in old feature:
  // new 0 (C) comes from old 2. New 1 (A) comes from old 0. New 2 (B) from
  // old 1.
  const int panFieldMap[] = {2, 0, 1};
  EXPECT_EQ(OGRERR_NONE, f->RemapFields(poDefn2, panFieldMap));
  EXPECT_EQ(3, f->GetFieldCount());
  EXPECT_DOUBLE_EQ(3.14, f->GetFieldAsDouble(0));
  EXPECT_EQ(10, f->GetFieldAsInteger(1));
  EXPECT_STREQ("hello", f->GetFieldAsString(2));

  // RemapGeomFields: new 0 (gB) comes from old 1. New 1 (gA) comes from old 0.
  const int panGeomMap[] = {1, 0};
  EXPECT_EQ(OGRERR_NONE, f->RemapGeomFields(poDefn2, panGeomMap));
  EXPECT_EQ(2, f->GetGeomFieldCount());
  EXPECT_EQ(wkbLineString, f->GetGeomFieldRef(0)->getGeometryType());
  EXPECT_EQ(wkbPoint, f->GetGeomFieldRef(1)->getGeometryType());

  // AppendField after adding a field to defn
  OGRFieldDefn fExtra("extra", OFTInteger);
  poDefn2->AddFieldDefn(&fExtra);
  f->AppendField();
  EXPECT_FALSE(f->IsFieldSet(3));
}

TEST(FeatureExtendedTest, EqualAndSetFrom) {
  OGRFeatureDefn* poDefn = new OGRFeatureDefn("eq_schema");
  poDefn->SetGeomType(wkbNone);
  poDefn->Reference();
  auto defn_cleaner = absl::MakeCleanup([poDefn] { poDefn->Release(); });

  OGRFieldDefn fInt("int", OFTInteger);
  OGRFieldDefn fStr("str", OFTString);
  OGRFieldDefn fReal("real", OFTReal);
  OGRFieldDefn fList("list", OFTIntegerList);
  OGRGeomFieldDefn gPt("geom", wkbPoint);

  poDefn->AddFieldDefn(&fInt);
  poDefn->AddFieldDefn(&fStr);
  poDefn->AddFieldDefn(&fReal);
  poDefn->AddFieldDefn(&fList);
  poDefn->AddGeomFieldDefn(&gPt);

  auto f1 = std::make_unique<OGRFeature>(poDefn);
  f1->SetFID(1);
  f1->SetField(0, 100);
  f1->SetField(1, "test");
  f1->SetField(2, 2.5);
  const int listVals[] = {1, 2, 3};
  f1->SetField(3, 3, listVals);
  f1->SetGeometryDirectly(new OGRPoint(10, 20));
  f1->SetStyleString("@pen(c:#00FF00)");

  auto f2 = std::make_unique<OGRFeature>(poDefn);
  f2->SetFID(1);
  f2->SetField(0, 100);
  f2->SetField(1, "test");
  f2->SetField(2, 2.5);
  f2->SetField(3, 3, listVals);
  f2->SetGeometryDirectly(new OGRPoint(10, 20));
  f2->SetStyleString("@pen(c:#00FF00)");

  EXPECT_TRUE(f1->Equal(f2.get()));

  // Different FID
  f2->SetFID(2);
  EXPECT_FALSE(f1->Equal(f2.get()));
  f2->SetFID(1);

  // Different field value
  f2->SetField(0, 999);
  EXPECT_FALSE(f1->Equal(f2.get()));
  f2->SetField(0, 100);

  // Different string
  f2->SetField(1, "other");
  EXPECT_FALSE(f1->Equal(f2.get()));
  f2->SetField(1, "test");

  // Different real
  f2->SetField(2, 3.5);
  EXPECT_FALSE(f1->Equal(f2.get()));
  f2->SetField(2, 2.5);

  // Unset vs Set
  f2->UnsetField(0);
  EXPECT_FALSE(f1->Equal(f2.get()));
  f2->SetField(0, 100);

  // Null vs Set
  f2->SetFieldNull(0);
  EXPECT_FALSE(f1->Equal(f2.get()));
  f2->SetField(0, 100);

  // Different geometry
  f2->SetGeometryDirectly(new OGRPoint(50, 60));
  EXPECT_FALSE(f1->Equal(f2.get()));
  f2->SetGeometryDirectly(new OGRPoint(10, 20));

  // Style is not compared by Equal
  f2->SetStyleString("@brush(c:#0000FF)");
  EXPECT_TRUE(f1->Equal(f2.get()));
  f2->SetStyleString("@pen(c:#00FF00)");

  // SetFrom copies fields and geoms, but not FID
  auto f3 = std::make_unique<OGRFeature>(poDefn);
  EXPECT_EQ(OGRERR_NONE, f3->SetFrom(f1.get()));
  f3->SetFID(f1->GetFID());
  EXPECT_TRUE(f3->Equal(f1.get()));

  // SetFieldsFrom (preserves geometries)
  auto f4 = std::make_unique<OGRFeature>(poDefn);
  f4->SetGeometryDirectly(new OGRPoint(99, 99));
  const int map[] = {0, 1, 2, 3};
  EXPECT_EQ(OGRERR_NONE, f4->SetFieldsFrom(f1.get(), map));
  EXPECT_EQ(100, f4->GetFieldAsInteger(0));
  EXPECT_DOUBLE_EQ(99.0, f4->GetGeometryRef()->toPoint()->getX());

  // SetFrom with map
  auto f5 = std::make_unique<OGRFeature>(poDefn);
  EXPECT_EQ(OGRERR_NONE, f5->SetFrom(f1.get(), map, TRUE));
  EXPECT_EQ(100, f5->GetFieldAsInteger(0));
}

TEST(FeatureExtendedTest, StyleAndNativeData) {
  OGRFeatureDefn* poDefn = new OGRFeatureDefn("style_schema");
  poDefn->SetGeomType(wkbNone);
  poDefn->Reference();
  auto defn_cleaner = absl::MakeCleanup([poDefn] { poDefn->Release(); });

  auto f = std::make_unique<OGRFeature>(poDefn);

  // Style string
  f->SetStyleString("@pen(c:#123456)");
  EXPECT_STREQ("@pen(c:#123456)", f->GetStyleString());
  char* dyn_style = CPLStrdup("@brush(c:#654321)");
  f->SetStyleStringDirectly(dyn_style);
  EXPECT_STREQ("@brush(c:#654321)", f->GetStyleString());

  // Style table
  OGRStyleTable oST;
  f->SetStyleTable(&oST);
  EXPECT_NE(nullptr, f->GetStyleTable());

  auto oST2 = new OGRStyleTable();
  f->SetStyleTableDirectly(oST2);
  EXPECT_EQ(oST2, f->GetStyleTable());
  f->SetStyleTableDirectly(nullptr);

  // Native data
  f->SetNativeData("some_native_data");
  EXPECT_STREQ("some_native_data", f->GetNativeData());
  f->SetNativeMediaType("text/plain");
  EXPECT_STREQ("text/plain", f->GetNativeMediaType());
}

TEST(FeatureExtendedTest, DumpReadable) {
  OGRFeatureDefn* poDefn = new OGRFeatureDefn("dump_schema");
  poDefn->SetGeomType(wkbNone);
  poDefn->Reference();
  auto defn_cleaner = absl::MakeCleanup([poDefn] { poDefn->Release(); });

  OGRFieldDefn fInt("id", OFTInteger);
  OGRFieldDefn fStr("name", OFTString);
  poDefn->AddFieldDefn(&fInt);
  poDefn->AddFieldDefn(&fStr);
  OGRGeomFieldDefn gf("geom", wkbPoint);
  poDefn->AddGeomFieldDefn(&gf);

  auto f = std::make_unique<OGRFeature>(poDefn);
  f->SetFID(42);
  f->SetField(0, 1);
  f->SetField(1, "test");
  f->SetGeometryDirectly(new OGRPoint(10, 20));

  // DumpReadableAsString
  std::string s = f->DumpReadableAsString();
  EXPECT_TRUE(s.find("OGRFeature(dump_schema):42") != std::string::npos);
  EXPECT_TRUE(s.find("id (Integer) = 1") != std::string::npos);
  EXPECT_TRUE(s.find("name (String) = test") != std::string::npos);
  EXPECT_TRUE(s.find("POINT (10 20)") != std::string::npos);

  // DumpReadable to file
  FILE* fp = tmpfile();
  ASSERT_THAT(fp, testing::NotNull());
  f->DumpReadable(fp);
  fclose(fp);
}

TEST(FeatureExtendedTest, BinarySerializationRoundTrip) {
  OGRFeatureDefn* poDefn = new OGRFeatureDefn("ser_schema");
  poDefn->SetGeomType(wkbNone);
  poDefn->Reference();
  auto defn_cleaner = absl::MakeCleanup([poDefn] { poDefn->Release(); });

  OGRFieldDefn fInt("i", OFTInteger);
  OGRFieldDefn fInt64("i64", OFTInteger64);
  OGRFieldDefn fReal("r", OFTReal);
  OGRFieldDefn fStr("s", OFTString);
  OGRFieldDefn fBin("b", OFTBinary);
  OGRFieldDefn fDate("d", OFTDate);
  OGRFieldDefn fTime("t", OFTTime);
  OGRFieldDefn fDateTime("dt", OFTDateTime);
  OGRFieldDefn fIntList("il", OFTIntegerList);
  OGRFieldDefn fInt64List("i64l", OFTInteger64List);
  OGRFieldDefn fRealList("rl", OFTRealList);
  OGRFieldDefn fStrList("sl", OFTStringList);
  OGRFieldDefn fNull("null_fld", OFTString);
  OGRFieldDefn fUnset("unset_fld", OFTString);

  poDefn->AddFieldDefn(&fInt);
  poDefn->AddFieldDefn(&fInt64);
  poDefn->AddFieldDefn(&fReal);
  poDefn->AddFieldDefn(&fStr);
  poDefn->AddFieldDefn(&fBin);
  poDefn->AddFieldDefn(&fDate);
  poDefn->AddFieldDefn(&fTime);
  poDefn->AddFieldDefn(&fDateTime);
  poDefn->AddFieldDefn(&fIntList);
  poDefn->AddFieldDefn(&fInt64List);
  poDefn->AddFieldDefn(&fRealList);
  poDefn->AddFieldDefn(&fStrList);
  poDefn->AddFieldDefn(&fNull);
  poDefn->AddFieldDefn(&fUnset);

  OGRGeomFieldDefn gPt("geom0", wkbPoint);
  OGRGeomFieldDefn gPoly("geom1", wkbPolygon);
  poDefn->AddGeomFieldDefn(&gPt);
  poDefn->AddGeomFieldDefn(&gPoly);

  auto f = std::make_unique<OGRFeature>(poDefn);
  f->SetFID(1234567890123LL);
  f->SetField(0, 12345);
  f->SetField(1, 987654321098LL);
  f->SetField(2, 3.1415926535);
  f->SetField(3, "hello world \xc3\xa9");
  const unsigned char binData[] = {0x01, 0x02, 0x03, 0x04};
  f->SetField(4, 4, binData);
  f->SetField(5, 2024, 11, 5);
  f->SetField(6, 0, 0, 0, 14, 30, 45.0f, 100);
  f->SetField(7, 2024, 11, 5, 14, 30, 45.5f, 104);

  const int iList[] = {1, -2, 3000};
  f->SetField(8, 3, iList);
  const GIntBig i64List[] = {100000000000LL, -200000000000LL};
  f->SetField(9, 2, i64List);
  const double rList[] = {1.1, -2.2, 3.3};
  f->SetField(10, 3, rList);
  const char* sList[] = {"foo", "bar", nullptr};
  f->SetField(11, const_cast<char* const*>(sList));

  f->SetFieldNull(12);
  // 13 is left unset

  f->SetGeomFieldDirectly(0, new OGRPoint(10.5, 20.5));
  auto poly = new OGRPolygon();
  auto lr = new OGRLinearRing();
  lr->addPoint(0, 0);
  lr->addPoint(0, 1);
  lr->addPoint(1, 1);
  lr->addPoint(1, 0);
  lr->addPoint(0, 0);
  poly->addRingDirectly(lr);
  f->SetGeomFieldDirectly(1, poly);

  f->SetStyleString("@pen(c:#123456)");

  // Serialize
  std::vector<GByte> abyBuffer;
  EXPECT_TRUE(f->SerializeToBinary(abyBuffer));
  EXPECT_FALSE(abyBuffer.empty());

  // Deserialize into fresh feature
  auto fDeserialized = std::make_unique<OGRFeature>(poDefn);
  EXPECT_TRUE(
      fDeserialized->DeserializeFromBinary(abyBuffer.data(), abyBuffer.size()));

  // Verify equality
  EXPECT_TRUE(fDeserialized->Equal(f.get()));

  // Deserialize with truncated buffers (error handling coverage)
  EXPECT_FALSE(fDeserialized->DeserializeFromBinary(abyBuffer.data(), 0));
  EXPECT_FALSE(fDeserialized->DeserializeFromBinary(abyBuffer.data(),
                                                    abyBuffer.size() / 2));
}

TEST(FeatureExtendedTest, FieldValueAndIterator) {
  OGRFeatureDefn* poDefn = new OGRFeatureDefn("iter_schema");
  poDefn->SetGeomType(wkbNone);
  poDefn->Reference();
  auto defn_cleaner = absl::MakeCleanup([poDefn] { poDefn->Release(); });

  OGRFieldDefn fInt("age", OFTInteger);
  OGRFieldDefn fStr("name", OFTString);
  OGRFieldDefn fReal("weight", OFTReal);
  poDefn->AddFieldDefn(&fInt);
  poDefn->AddFieldDefn(&fStr);
  poDefn->AddFieldDefn(&fReal);

  auto f = std::make_unique<OGRFeature>(poDefn);
  f->SetField(0, 25);
  f->SetField(1, "Bob");
  f->SetField(2, 70.5);

  // Iteration
  int count = 0;
  for (const auto& fld : *f) {
    EXPECT_FALSE(fld.empty());
    EXPECT_FALSE(fld.IsNull());
    EXPECT_FALSE(fld.IsUnset());
    if (count == 0) {
      EXPECT_EQ(25, fld.GetInteger());
      EXPECT_EQ(25, fld.GetAsInteger());
      EXPECT_STREQ("age", fld.GetName());
    } else if (count == 1) {
      EXPECT_STREQ("Bob", fld.GetString());
      EXPECT_STREQ("name", fld.GetName());
    } else if (count == 2) {
      EXPECT_DOUBLE_EQ(70.5, fld.GetDouble());
      EXPECT_STREQ("weight", fld.GetName());
    }
    count++;
  }
  EXPECT_EQ(3, count);

  // operator[] by name and index
  EXPECT_EQ(25, (*f)["age"].GetInteger());
  EXPECT_STREQ("Bob", (*f)["name"].GetString());
  EXPECT_DOUBLE_EQ(70.5, (*f)["weight"].GetDouble());
  EXPECT_EQ(25, (*f)[0].GetInteger());

  // Assignment from another field
  auto f2 = std::make_unique<OGRFeature>(poDefn);
  f2->SetField(0, 42);
  f2->SetField(1, "Charlie");
  (*f)[0] = (*f2)[0];
  EXPECT_EQ(42, f->GetFieldAsInteger(0));

  // SetNull, Unset, clear
  (*f)[0].SetNull();
  EXPECT_TRUE(f->IsFieldNull(0));
  (*f)[0].Unset();
  EXPECT_FALSE(f->IsFieldSet(0));
  (*f)[1].clear();
  EXPECT_FALSE(f->IsFieldSet(1));
}

TEST(FeatureExtendedTest, RawFieldHelpers) {
  OGRField uField;
  OGR_RawField_SetUnset(&uField);
  EXPECT_TRUE(OGR_RawField_IsUnset(&uField));
  EXPECT_FALSE(OGR_RawField_IsNull(&uField));

  OGR_RawField_SetNull(&uField);
  EXPECT_FALSE(OGR_RawField_IsUnset(&uField));
  EXPECT_TRUE(OGR_RawField_IsNull(&uField));
}

TEST(FeatureExtendedTest, CApiAllWrappers) {
  OGRFeatureDefnBuilder builder("capi_schema");
  builder.AddGeom("geom", wkbPoint);
  builder.AddField("int_fld", OFTInteger, OFSTNone);
  builder.AddField("int64_fld", OFTInteger64, OFSTNone);
  builder.AddField("double_fld", OFTReal, OFSTNone);
  builder.AddField("str_fld", OFTString, OFSTNone);
  builder.AddField("int_list", OFTIntegerList, OFSTNone);
  builder.AddField("int64_list", OFTInteger64List, OFSTNone);
  builder.AddField("double_list", OFTRealList, OFSTNone);
  builder.AddField("str_list", OFTStringList, OFSTNone);
  builder.AddField("bin_fld", OFTBinary, OFSTNone);
  builder.AddField("dt_fld", OFTDateTime, OFSTNone);

  OGRFeatureDefnH hDefn = builder.Build();
  auto defn_cleaner = absl::MakeCleanup([hDefn] { OGR_FD_Release(hDefn); });

  OGRFeatureH hFeat = OGR_F_Create(hDefn);
  ASSERT_THAT(hFeat, testing::NotNull());
  auto feat_cleaner = absl::MakeCleanup([hFeat] { OGR_F_Destroy(hFeat); });

  // GetDefnRef
  EXPECT_EQ(hDefn, OGR_F_GetDefnRef(hFeat));

  // FID
  EXPECT_EQ(OGRERR_NONE, OGR_F_SetFID(hFeat, 777));
  EXPECT_EQ(777, OGR_F_GetFID(hFeat));

  // Field Count and Defn
  EXPECT_EQ(10, OGR_F_GetFieldCount(hFeat));
  EXPECT_THAT(OGR_F_GetFieldDefnRef(hFeat, 0), testing::NotNull());
  EXPECT_EQ(0, OGR_F_GetFieldIndex(hFeat, "int_fld"));

  // Geom Field Count and Defn
  EXPECT_EQ(1, OGR_F_GetGeomFieldCount(hFeat));
  EXPECT_THAT(OGR_F_GetGeomFieldDefnRef(hFeat, 0), testing::NotNull());
  EXPECT_EQ(0, OGR_F_GetGeomFieldIndex(hFeat, "geom"));

  // Geometries
  OGRGeometryH hPt = OGR_G_CreateGeometry(wkbPoint);
  OGR_G_SetPoint_2D(hPt, 0, 11, 22);
  EXPECT_EQ(OGRERR_NONE, OGR_F_SetGeometryDirectly(hFeat, hPt));
  EXPECT_THAT(OGR_F_GetGeometryRef(hFeat), testing::NotNull());
  EXPECT_THAT(OGR_F_GetGeomFieldRef(hFeat, 0), testing::NotNull());

  OGRGeometryH hStolen = OGR_F_StealGeometry(hFeat);
  EXPECT_EQ(hPt, hStolen);
  OGR_G_DestroyGeometry(hStolen);

  OGRGeometryH hPt2 = OGR_G_CreateGeometry(wkbPoint);
  OGR_G_SetPoint_2D(hPt2, 0, 33, 44);
  EXPECT_EQ(OGRERR_NONE, OGR_F_SetGeometry(hFeat, hPt2));
  OGR_G_DestroyGeometry(hPt2);

  hStolen = OGR_F_StealGeometryEx(hFeat, 0);
  ASSERT_THAT(hStolen, testing::NotNull());
  OGR_G_DestroyGeometry(hStolen);

  OGRGeometryH hPt3 = OGR_G_CreateGeometry(wkbPoint);
  OGR_G_SetPoint_2D(hPt3, 0, 55, 66);
  EXPECT_EQ(OGRERR_NONE, OGR_F_SetGeomFieldDirectly(hFeat, 0, hPt3));

  OGRGeometryH hPt4 = OGR_G_CreateGeometry(wkbPoint);
  OGR_G_SetPoint_2D(hPt4, 0, 77, 88);
  EXPECT_EQ(OGRERR_NONE, OGR_F_SetGeomField(hFeat, 0, hPt4));
  OGR_G_DestroyGeometry(hPt4);

  // Field Setters & Getters
  OGR_F_SetFieldInteger(hFeat, 0, 123);
  EXPECT_TRUE(OGR_F_IsFieldSet(hFeat, 0));
  EXPECT_FALSE(OGR_F_IsFieldNull(hFeat, 0));
  EXPECT_TRUE(OGR_F_IsFieldSetAndNotNull(hFeat, 0));
  EXPECT_EQ(123, OGR_F_GetFieldAsInteger(hFeat, 0));

  OGR_F_SetFieldInteger64(hFeat, 1, 9876543210LL);
  EXPECT_EQ(9876543210LL, OGR_F_GetFieldAsInteger64(hFeat, 1));

  OGR_F_SetFieldDouble(hFeat, 2, 45.67);
  EXPECT_DOUBLE_EQ(45.67, OGR_F_GetFieldAsDouble(hFeat, 2));

  OGR_F_SetFieldString(hFeat, 3, "hello_capi");
  EXPECT_STREQ("hello_capi", OGR_F_GetFieldAsString(hFeat, 3));

  const int panIntList[] = {1, 2, 3};
  OGR_F_SetFieldIntegerList(hFeat, 4, 3, panIntList);
  int nCount = 0;
  const int* panRet = OGR_F_GetFieldAsIntegerList(hFeat, 4, &nCount);
  EXPECT_EQ(3, nCount);
  EXPECT_EQ(1, panRet[0]);

  const GIntBig panInt64List[] = {100LL, 200LL};
  OGR_F_SetFieldInteger64List(hFeat, 5, 2, panInt64List);
  const GIntBig* panRet64 = OGR_F_GetFieldAsInteger64List(hFeat, 5, &nCount);
  EXPECT_EQ(2, nCount);
  EXPECT_EQ(100LL, panRet64[0]);

  const double padfDoubleList[] = {1.5, 2.5};
  OGR_F_SetFieldDoubleList(hFeat, 6, 2, padfDoubleList);
  const double* padfRet = OGR_F_GetFieldAsDoubleList(hFeat, 6, &nCount);
  EXPECT_EQ(2, nCount);
  EXPECT_DOUBLE_EQ(1.5, padfRet[0]);

  const char* papszStrList[] = {"a", "b", nullptr};
  OGR_F_SetFieldStringList(hFeat, 7, const_cast<char* const*>(papszStrList));
  char** papszRet = OGR_F_GetFieldAsStringList(hFeat, 7);
  EXPECT_STREQ("a", papszRet[0]);

  const unsigned char abyBin[] = {0xAA, 0xBB};
  OGR_F_SetFieldBinary(hFeat, 8, 2, abyBin);
  GByte* pabyRet = OGR_F_GetFieldAsBinary(hFeat, 8, &nCount);
  EXPECT_EQ(2, nCount);
  EXPECT_EQ(0xAA, pabyRet[0]);

  OGR_F_SetFieldDateTime(hFeat, 9, 2024, 11, 5, 12, 30, 45, 100);
  int y, m, d, h, mn, s, tz;
  EXPECT_TRUE(OGR_F_GetFieldAsDateTime(hFeat, 9, &y, &m, &d, &h, &mn, &s, &tz));
  EXPECT_EQ(2024, y);

  float sf;
  OGR_F_SetFieldDateTimeEx(hFeat, 9, 2024, 11, 5, 12, 30, 45.5f, 100);
  EXPECT_TRUE(
      OGR_F_GetFieldAsDateTimeEx(hFeat, 9, &y, &m, &d, &h, &mn, &sf, &tz));
  EXPECT_FLOAT_EQ(45.5f, sf);
  EXPECT_STREQ("2024-11-05T12:30:45.500Z",
               OGR_F_GetFieldAsISO8601DateTime(hFeat, 9, nullptr));

  // SetFieldRaw & GetRawFieldRef
  OGRField* pRaw = OGR_F_GetRawFieldRef(hFeat, 0);
  ASSERT_THAT(pRaw, testing::NotNull());
  OGRField uFieldCopy = *pRaw;
  uFieldCopy.Integer = 555;
  OGR_F_SetFieldRaw(hFeat, 0, &uFieldCopy);
  EXPECT_EQ(555, OGR_F_GetFieldAsInteger(hFeat, 0));

  // Unset and Null
  OGR_F_SetFieldNull(hFeat, 0);
  EXPECT_TRUE(OGR_F_IsFieldNull(hFeat, 0));
  OGR_F_UnsetField(hFeat, 0);
  EXPECT_FALSE(OGR_F_IsFieldSet(hFeat, 0));

  // Clone and Equal
  OGRFeatureH hClone = OGR_F_Clone(hFeat);
  ASSERT_THAT(hClone, testing::NotNull());
  EXPECT_TRUE(OGR_F_Equal(hFeat, hClone));

  // SetFrom and SetFromWithMap (SetFrom does not copy FID)
  OGRFeatureH hDst = OGR_F_Create(hDefn);
  EXPECT_EQ(OGRERR_NONE, OGR_F_SetFrom(hDst, hFeat, TRUE));
  OGR_F_SetFID(hDst, OGR_F_GetFID(hFeat));
  EXPECT_TRUE(OGR_F_Equal(hFeat, hDst));

  const int map[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  EXPECT_EQ(OGRERR_NONE, OGR_F_SetFromWithMap(hDst, hFeat, TRUE, map));
  OGR_F_Destroy(hDst);
  OGR_F_Destroy(hClone);

  // Style
  OGR_F_SetStyleString(hFeat, "@pen(c:#FFFFFF)");
  EXPECT_STREQ("@pen(c:#FFFFFF)", OGR_F_GetStyleString(hFeat));
  char* pszDirectStyle = CPLStrdup("@brush(c:#000000)");
  OGR_F_SetStyleStringDirectly(hFeat, pszDirectStyle);
  EXPECT_STREQ("@brush(c:#000000)", OGR_F_GetStyleString(hFeat));

  OGRStyleTableH hST = OGR_STBL_Create();
  OGR_F_SetStyleTable(hFeat, hST);
  EXPECT_NE(nullptr, OGR_F_GetStyleTable(hFeat));
  OGR_STBL_Destroy(hST);

  OGRStyleTableH hST2 = OGR_STBL_Create();
  OGR_F_SetStyleTableDirectly(hFeat, hST2);
  EXPECT_EQ(hST2, OGR_F_GetStyleTable(hFeat));
  OGR_F_SetStyleTableDirectly(hFeat, nullptr);

  // Native Data
  OGR_F_SetNativeData(hFeat, "c_native_data");
  EXPECT_STREQ("c_native_data", OGR_F_GetNativeData(hFeat));
  OGR_F_SetNativeMediaType(hFeat, "application/geo+json");
  EXPECT_STREQ("application/geo+json", OGR_F_GetNativeMediaType(hFeat));

  // FillUnsetWithDefault & Validate
  OGR_F_FillUnsetWithDefault(hFeat, FALSE, nullptr);
  EXPECT_TRUE(OGR_F_Validate(hFeat, OGR_F_VAL_ALL, FALSE));

  // DumpReadable
  char* pszDump = OGR_F_DumpReadableAsString(hFeat, nullptr);
  ASSERT_THAT(pszDump, testing::NotNull());
  CPLFree(pszDump);

  FILE* fp = tmpfile();
  ASSERT_THAT(fp, testing::NotNull());
  OGR_F_DumpReadable(hFeat, fp);
  fclose(fp);
}

TEST(FeatureExtendedTest, SpecialFields) {
  OGRFeatureDefn* poDefn = new OGRFeatureDefn("special_schema");
  poDefn->Reference();
  auto defn_cleaner = absl::MakeCleanup([poDefn] { poDefn->Release(); });

  OGRFieldDefn fInt("id", OFTInteger);
  poDefn->AddFieldDefn(&fInt);

  auto f = std::make_unique<OGRFeature>(poDefn);
  f->SetFID(42);
  f->SetField(0, 100);

  auto poly = new OGRPolygon();
  auto ring = new OGRLinearRing();
  ring->addPoint(0, 0);
  ring->addPoint(10, 0);
  ring->addPoint(10, 10);
  ring->addPoint(0, 10);
  ring->closeRings();
  poly->addRingDirectly(ring);
  f->SetGeometryDirectly(poly);
  f->SetStyleString("@pen(c:#FF0000)");

  const int nFields = poDefn->GetFieldCount();

  // SPF_FID
  EXPECT_TRUE(f->IsFieldSet(nFields + SPF_FID));
  EXPECT_EQ(42, f->GetFieldAsInteger(nFields + SPF_FID));
  EXPECT_EQ(42LL, f->GetFieldAsInteger64(nFields + SPF_FID));
  EXPECT_DOUBLE_EQ(42.0, f->GetFieldAsDouble(nFields + SPF_FID));
  EXPECT_STREQ("42", f->GetFieldAsString(nFields + SPF_FID));

  // Overflow FID
  f->SetFID(5000000000LL);
  {
    CPLErrorReset();
    EXPECT_EQ(INT_MAX, f->GetFieldAsInteger(nFields + SPF_FID));
  }
  f->SetFID(-5000000000LL);
  {
    CPLErrorReset();
    EXPECT_EQ(INT_MIN, f->GetFieldAsInteger(nFields + SPF_FID));
  }
  f->SetFID(42);

  // SPF_OGR_GEOMETRY
  EXPECT_TRUE(f->IsFieldSet(nFields + SPF_OGR_GEOMETRY));
  EXPECT_STREQ("POLYGON", f->GetFieldAsString(nFields + SPF_OGR_GEOMETRY));

  // SPF_OGR_STYLE
  EXPECT_TRUE(f->IsFieldSet(nFields + SPF_OGR_STYLE));
  EXPECT_STREQ("@pen(c:#FF0000)", f->GetFieldAsString(nFields + SPF_OGR_STYLE));

  // SPF_OGR_GEOM_WKT
  EXPECT_TRUE(f->IsFieldSet(nFields + SPF_OGR_GEOM_WKT));
  EXPECT_THAT(f->GetFieldAsString(nFields + SPF_OGR_GEOM_WKT),
              testing::HasSubstr("POLYGON"));

  // SPF_OGR_GEOM_AREA
  EXPECT_TRUE(f->IsFieldSet(nFields + SPF_OGR_GEOM_AREA));
  EXPECT_EQ(100, f->GetFieldAsInteger(nFields + SPF_OGR_GEOM_AREA));
  EXPECT_EQ(100LL, f->GetFieldAsInteger64(nFields + SPF_OGR_GEOM_AREA));
  EXPECT_DOUBLE_EQ(100.0, f->GetFieldAsDouble(nFields + SPF_OGR_GEOM_AREA));
  EXPECT_STREQ("100", f->GetFieldAsString(nFields + SPF_OGR_GEOM_AREA));

  // Out of bounds / default special field
  EXPECT_FALSE(f->IsFieldSet(nFields + SPECIAL_FIELD_COUNT));
  EXPECT_EQ(0, f->GetFieldAsInteger(nFields + SPECIAL_FIELD_COUNT));
  EXPECT_EQ(0LL, f->GetFieldAsInteger64(nFields + SPECIAL_FIELD_COUNT));
  EXPECT_DOUBLE_EQ(0.0, f->GetFieldAsDouble(nFields + SPECIAL_FIELD_COUNT));
  EXPECT_STREQ("", f->GetFieldAsString(nFields + SPECIAL_FIELD_COUNT));

  // Feature without geometry and without style
  auto fEmpty = std::make_unique<OGRFeature>(poDefn);
  fEmpty->SetFID(OGRNullFID);
  EXPECT_FALSE(fEmpty->IsFieldSet(nFields + SPF_FID));
  EXPECT_FALSE(fEmpty->IsFieldSet(nFields + SPF_OGR_GEOMETRY));
  EXPECT_STREQ("", fEmpty->GetFieldAsString(nFields + SPF_OGR_GEOMETRY));
  EXPECT_FALSE(fEmpty->IsFieldSet(nFields + SPF_OGR_STYLE));
  EXPECT_STREQ("", fEmpty->GetFieldAsString(nFields + SPF_OGR_STYLE));
  EXPECT_FALSE(fEmpty->IsFieldSet(nFields + SPF_OGR_GEOM_WKT));
  EXPECT_STREQ("", fEmpty->GetFieldAsString(nFields + SPF_OGR_GEOM_WKT));
  EXPECT_FALSE(fEmpty->IsFieldSet(nFields + SPF_OGR_GEOM_AREA));
  EXPECT_EQ(0, fEmpty->GetFieldAsInteger(nFields + SPF_OGR_GEOM_AREA));
  EXPECT_EQ(0LL, fEmpty->GetFieldAsInteger64(nFields + SPF_OGR_GEOM_AREA));
  EXPECT_DOUBLE_EQ(0.0, fEmpty->GetFieldAsDouble(nFields + SPF_OGR_GEOM_AREA));
  EXPECT_STREQ("", fEmpty->GetFieldAsString(nFields + SPF_OGR_GEOM_AREA));
}

TEST(FeatureExtendedTest, SetFieldComprehensiveConversions) {
  OGRFeatureDefn* poDefn = new OGRFeatureDefn("all_types");
  poDefn->SetGeomType(wkbNone);
  poDefn->Reference();
  auto defn_cleaner = absl::MakeCleanup([poDefn] { poDefn->Release(); });

  OGRFieldDefn f0("int", OFTInteger);
  OGRFieldDefn f1("int64", OFTInteger64);
  OGRFieldDefn f2("real", OFTReal);
  OGRFieldDefn f3("str", OFTString);
  OGRFieldDefn f4("date", OFTDate);
  OGRFieldDefn f5("time", OFTTime);
  OGRFieldDefn f6("datetime", OFTDateTime);
  OGRFieldDefn f7("binary", OFTBinary);
  OGRFieldDefn f8("int_list", OFTIntegerList);
  OGRFieldDefn f9("int64_list", OFTInteger64List);
  OGRFieldDefn f10("real_list", OFTRealList);
  OGRFieldDefn f11("str_list", OFTStringList);

  poDefn->AddFieldDefn(&f0);
  poDefn->AddFieldDefn(&f1);
  poDefn->AddFieldDefn(&f2);
  poDefn->AddFieldDefn(&f3);
  poDefn->AddFieldDefn(&f4);
  poDefn->AddFieldDefn(&f5);
  poDefn->AddFieldDefn(&f6);
  poDefn->AddFieldDefn(&f7);
  poDefn->AddFieldDefn(&f8);
  poDefn->AddFieldDefn(&f9);
  poDefn->AddFieldDefn(&f10);
  poDefn->AddFieldDefn(&f11);

  auto f = std::make_unique<OGRFeature>(poDefn);

  // 1. SetField(int, int) into various types
  for (int i = 0; i <= 11; ++i) {
    f->SetField(i, 42);
  }
  EXPECT_EQ(42, f->GetFieldAsInteger(0));
  EXPECT_EQ(42LL, f->GetFieldAsInteger64(1));
  EXPECT_DOUBLE_EQ(42.0, f->GetFieldAsDouble(2));
  EXPECT_STREQ("42", f->GetFieldAsString(3));

  // 2. SetField(int, GIntBig) into various types
  for (int i = 0; i <= 11; ++i) {
    f->SetField(i, static_cast<GIntBig>(10000000000LL));
  }
  EXPECT_EQ(10000000000LL, f->GetFieldAsInteger64(1));
  EXPECT_DOUBLE_EQ(10000000000.0, f->GetFieldAsDouble(2));
  EXPECT_STREQ("10000000000", f->GetFieldAsString(3));
  // Overflow warning when setting large GIntBig into OFTInteger
  {
    CPLErrorReset();
    f->SetField(0, static_cast<GIntBig>(10000000000LL));
  }

  // 3. SetField(int, double) into various types
  for (int i = 0; i <= 11; ++i) {
    f->SetField(i, 3.1415);
  }
  EXPECT_EQ(3, f->GetFieldAsInteger(0));
  EXPECT_EQ(3LL, f->GetFieldAsInteger64(1));
  EXPECT_DOUBLE_EQ(3.1415, f->GetFieldAsDouble(2));

  // 4. SetField(int, const char*) into all types
  f->SetField(0, "123");
  EXPECT_EQ(123, f->GetFieldAsInteger(0));
  f->SetField(0, "not_an_int");
  EXPECT_EQ(0, f->GetFieldAsInteger(0));

  f->SetField(1, "999999999999");
  EXPECT_EQ(999999999999LL, f->GetFieldAsInteger64(1));

  f->SetField(2, "2.71828");
  EXPECT_DOUBLE_EQ(2.71828, f->GetFieldAsDouble(2));

  f->SetField(3, "test_string");
  EXPECT_STREQ("test_string", f->GetFieldAsString(3));

  f->SetField(4, "2024-05-10");
  int y = 0, m = 0, d = 0, hr = 0, min = 0, tz = 0;
  float sec = 0.0f;
  EXPECT_TRUE(f->GetFieldAsDateTime(4, &y, &m, &d, &hr, &min, &sec, &tz));
  EXPECT_EQ(2024, y);
  EXPECT_EQ(5, m);
  EXPECT_EQ(10, d);

  f->SetField(5, "14:30:15");
  EXPECT_TRUE(f->GetFieldAsDateTime(5, &y, &m, &d, &hr, &min, &sec, &tz));
  EXPECT_EQ(14, hr);
  EXPECT_EQ(30, min);

  f->SetField(6, "2024-05-10T14:30:15+02:00");
  EXPECT_TRUE(f->GetFieldAsDateTime(6, &y, &m, &d, &hr, &min, &sec, &tz));

  // SetField(const char*) on OFTBinary is unsupported and does nothing
  f->SetField(7, "01020304");
  int nBytes = 0;
  GByte* paby = f->GetFieldAsBinary(7, &nBytes);
  EXPECT_EQ(0, nBytes);
  EXPECT_EQ(nullptr, paby);

  f->SetField(8, "(3:10,20,30)");
  int count = 0;
  const int* pan = f->GetFieldAsIntegerList(8, &count);
  EXPECT_EQ(3, count);
  if (pan) EXPECT_EQ(10, pan[0]);

  f->SetField(9, "(2:10000000000,20000000000)");
  const GIntBig* pan64 = f->GetFieldAsInteger64List(9, &count);
  EXPECT_EQ(2, count);
  if (pan64) EXPECT_EQ(10000000000LL, pan64[0]);

  f->SetField(10, "(2:1.5,2.5)");
  const double* padf = f->GetFieldAsDoubleList(10, &count);
  EXPECT_EQ(2, count);
  if (padf) EXPECT_DOUBLE_EQ(1.5, padf[0]);

  f->SetField(11, "(2:hello,world)");
  char** papsz = f->GetFieldAsStringList(11);
  EXPECT_EQ(2, CSLCount(papsz));

  // Set nullptr string on OFTString sets empty string
  f->SetField(3, static_cast<const char*>(nullptr));
  EXPECT_STREQ("", f->GetFieldAsString(3));

  // 5. SetField(int, int nCount, const int *panValues)
  const int anVals[] = {1, 2, 3};
  for (int i = 0; i <= 11; ++i) {
    f->SetField(i, 3, anVals);
    f->SetField(i, 1, anVals);
    f->SetField(i, 0, static_cast<const int*>(nullptr));
  }

  // 6. SetField(int, int nCount, const GIntBig *panValues)
  const GIntBig anVals64[] = {10LL, 20LL, 30LL};
  for (int i = 0; i <= 11; ++i) {
    f->SetField(i, 3, anVals64);
    f->SetField(i, 1, anVals64);
    f->SetField(i, 0, static_cast<const GIntBig*>(nullptr));
  }

  // 7. SetField(int, int nCount, const double *padfValues)
  const double adfVals[] = {1.1, 2.2, 3.3};
  for (int i = 0; i <= 11; ++i) {
    f->SetField(i, 3, adfVals);
    f->SetField(i, 1, adfVals);
    f->SetField(i, 0, static_cast<const double*>(nullptr));
  }

  // 8. SetField(int, const char *const *papszValues)
  const char* aszVals[] = {"100", "200", nullptr};
  for (int i = 0; i <= 11; ++i) {
    f->SetField(i, const_cast<char* const*>(aszVals));
  }

  // 9. SetField(int, int nBytes, const void *pabyData)
  const GByte abyData[] = {0xAA, 0xBB, 0xCC};
  f->SetField(7, 3, abyData);
  paby = f->GetFieldAsBinary(7, &nBytes);
  EXPECT_EQ(3, nBytes);
  f->SetField(3, 3, abyData);
  EXPECT_STREQ("\xAA\xBB\xCC", f->GetFieldAsString(3));

  // 10. SetField(datetime)
  f->SetField(4, 2024, 6, 15, 0, 0, 0.0f, 0);
  f->SetField(5, 0, 0, 0, 18, 45, 30.0f, 0);
  f->SetField(6, 2024, 6, 15, 18, 45, 30.0f, 100);
  f->SetField(3, 2024, 6, 15, 18, 45, 30.0f, 100);
  EXPECT_STREQ("2024/06/15 18:45:30+00", f->GetFieldAsString(3));

  // 11. SetField(int, const OGRField *puValue)
  auto fSrc = std::make_unique<OGRFeature>(poDefn);
  fSrc->SetField(0, 777);
  fSrc->SetFieldNull(1);
  f->SetField(0, fSrc->GetRawFieldRef(0));
  EXPECT_EQ(777, f->GetFieldAsInteger(0));
  f->SetField(1, fSrc->GetRawFieldRef(1));
  EXPECT_TRUE(f->IsFieldNull(1));
  f->SetField(2, fSrc->GetRawFieldRef(2));  // unset
  EXPECT_FALSE(f->IsFieldSet(2));

  // SetFieldNull and UnsetField
  for (int i = 0; i <= 11; ++i) {
    f->SetFieldNull(i);
    EXPECT_TRUE(f->IsFieldNull(i));
    f->UnsetField(i);
    EXPECT_FALSE(f->IsFieldSet(i));
  }
}

TEST(FeatureExtendedTest, FieldValueOperations) {
  OGRFeatureDefn* poDefn = new OGRFeatureDefn("fv_schema");
  poDefn->SetGeomType(wkbNone);
  poDefn->Reference();
  auto defn_cleaner = absl::MakeCleanup([poDefn] { poDefn->Release(); });

  OGRFieldDefn f0("f_int", OFTInteger);
  OGRFieldDefn f1("f_int64", OFTInteger64);
  OGRFieldDefn f2("f_real", OFTReal);
  OGRFieldDefn f3("f_str", OFTString);
  OGRFieldDefn f4("f_dt", OFTDateTime);
  OGRFieldDefn f5("f_int_list", OFTIntegerList);
  OGRFieldDefn f6("f_int64_list", OFTInteger64List);
  OGRFieldDefn f7("f_real_list", OFTRealList);
  OGRFieldDefn f8("f_str_list", OFTStringList);

  poDefn->AddFieldDefn(&f0);
  poDefn->AddFieldDefn(&f1);
  poDefn->AddFieldDefn(&f2);
  poDefn->AddFieldDefn(&f3);
  poDefn->AddFieldDefn(&f4);
  poDefn->AddFieldDefn(&f5);
  poDefn->AddFieldDefn(&f6);
  poDefn->AddFieldDefn(&f7);
  poDefn->AddFieldDefn(&f8);

  auto f = std::make_unique<OGRFeature>(poDefn);

  // FieldValue assignment operators
  (*f)[0] = 123;
  EXPECT_EQ(123, (*f)[0].GetAsInteger());
  EXPECT_EQ(0, (*f)[0].GetIndex());
  EXPECT_EQ(poDefn->GetFieldDefn(0), (*f)[0].GetDefn());
  EXPECT_FALSE((*f)[0].IsUnset());
  EXPECT_FALSE((*f)[0].IsNull());

  (*f)[1] = static_cast<GIntBig>(9876543210LL);
  EXPECT_EQ(9876543210LL, (*f)[1].GetAsInteger64());

  (*f)[2] = 12.34;
  EXPECT_DOUBLE_EQ(12.34, (*f)[2].GetAsDouble());

  (*f)[3] = "char_ptr";
  EXPECT_STREQ("char_ptr", (*f)[3].GetAsString());

  (*f)[3] = std::string("std_string");
  EXPECT_STREQ("std_string", (*f)[3].GetAsString());

  (*f)[4].SetDateTime(2024, 7, 20, 11, 22, 33.0f, 100);
  int y = 0, m = 0, d = 0, hr = 0, min = 0, tz = 0;
  float sec = 0.0f;
  EXPECT_TRUE((*f)[4].GetDateTime(&y, &m, &d, &hr, &min, &sec, &tz));
  EXPECT_EQ(2024, y);

  (*f)[5] = std::vector<int>{10, 20, 30};
  EXPECT_EQ(3, (*f)[5].GetAsIntegerList().size());

  (*f)[6] = std::vector<GIntBig>{100LL, 200LL};
  EXPECT_EQ(2, (*f)[6].GetAsInteger64List().size());

  (*f)[7] = std::vector<double>{1.5, 2.5};
  EXPECT_EQ(2, (*f)[7].GetAsDoubleList().size());

  (*f)[8] = std::vector<std::string>{"foo", "bar"};
  EXPECT_EQ(2, (*f)[8].GetAsStringList().size());

  const char* csl[] = {"alpha", "beta", nullptr};
  (*f)[8] = const_cast<CSLConstList>(csl);
  EXPECT_EQ(2, CSLCount(static_cast<CSLConstList>((*f)[8])));

  // Range-based for loop (non-const)
  int idx = 0;
  for (auto&& field : *f) {
    EXPECT_EQ(idx, field.GetIndex());
    ++idx;
  }
  EXPECT_EQ(9, idx);

  // Range-based for loop (const)
  const OGRFeature* cf = f.get();
  idx = 0;
  for (const auto& field : *cf) {
    EXPECT_EQ(idx, field.GetIndex());
    ++idx;
  }
  EXPECT_EQ(9, idx);

  // FieldValue::operator=(const FieldValue &oOther) cross assignment
  auto fDst = std::make_unique<OGRFeature>(poDefn);
  for (int i = 0; i < 9; ++i) {
    (*fDst)[i] = (*f)[i];
  }
  EXPECT_EQ(123, (*fDst)[0].GetAsInteger());
  EXPECT_EQ(9876543210LL, (*fDst)[1].GetAsInteger64());

  // SetNull and clear (Unset)
  (*f)[0].SetNull();
  EXPECT_TRUE((*f)[0].IsNull());
  (*fDst)[0] = (*f)[0];  // Copy null
  EXPECT_TRUE((*fDst)[0].IsNull());

  (*f)[0].clear();
  EXPECT_TRUE((*f)[0].IsUnset());
  (*fDst)[0] = (*f)[0];  // Copy unset
  EXPECT_TRUE((*fDst)[0].IsUnset());

  // Self assignment
  (*f)[1] = (*f)[1];
  EXPECT_EQ(9876543210LL, (*f)[1].GetAsInteger64());
}

TEST(FeatureExtendedTest, BinarySerializationFull) {
  OGRFeatureDefn* poDefn = new OGRFeatureDefn("bin_full_schema");
  poDefn->Reference();
  auto defn_cleaner = absl::MakeCleanup([poDefn] { poDefn->Release(); });

  OGRFieldDefn f0("int", OFTInteger);
  OGRFieldDefn f1("int64", OFTInteger64);
  OGRFieldDefn f2("real", OFTReal);
  OGRFieldDefn f3("str", OFTString);
  OGRFieldDefn f4("date", OFTDate);
  OGRFieldDefn f5("time", OFTTime);
  OGRFieldDefn f6("datetime", OFTDateTime);
  OGRFieldDefn f7("binary", OFTBinary);
  OGRFieldDefn f8("int_list", OFTIntegerList);
  OGRFieldDefn f9("int64_list", OFTInteger64List);
  OGRFieldDefn f10("real_list", OFTRealList);
  OGRFieldDefn f11("str_list", OFTStringList);
  OGRFieldDefn f12("null_fld", OFTString);
  OGRFieldDefn f13("unset_fld", OFTString);

  poDefn->AddFieldDefn(&f0);
  poDefn->AddFieldDefn(&f1);
  poDefn->AddFieldDefn(&f2);
  poDefn->AddFieldDefn(&f3);
  poDefn->AddFieldDefn(&f4);
  poDefn->AddFieldDefn(&f5);
  poDefn->AddFieldDefn(&f6);
  poDefn->AddFieldDefn(&f7);
  poDefn->AddFieldDefn(&f8);
  poDefn->AddFieldDefn(&f9);
  poDefn->AddFieldDefn(&f10);
  poDefn->AddFieldDefn(&f11);
  poDefn->AddFieldDefn(&f12);
  poDefn->AddFieldDefn(&f13);

  auto f = std::make_unique<OGRFeature>(poDefn);
  f->SetFID(999);
  f->SetField(0, 42);
  f->SetField(1, static_cast<GIntBig>(123456789012345LL));
  f->SetField(2, 3.14159);
  f->SetField(3, "hello binary");
  f->SetField(4, 2024, 11, 5, 0, 0, 0.0f, 0);
  f->SetField(5, 0, 0, 0, 15, 30, 45.5f, 0);
  f->SetField(6, 2024, 11, 5, 15, 30, 45.5f, 100);
  const GByte binData[] = {0xDE, 0xAD, 0xBE, 0xEF};
  f->SetField(7, 4, binData);
  const int anVals[] = {1, 2, 3};
  f->SetField(8, 3, anVals);
  const GIntBig anVals64[] = {10000000000LL, 20000000000LL};
  f->SetField(9, 2, anVals64);
  const double adfVals[] = {1.5, 2.5, 3.5};
  f->SetField(10, 3, adfVals);
  const char* aszVals[] = {"apple", "banana", nullptr};
  f->SetField(11, const_cast<char* const*>(aszVals));
  f->SetFieldNull(12);
  // 13 is unset

  auto pt = new OGRPoint(12.34, 56.78);
  auto srs = new OGRSpatialReference();
  srs->importFromEPSG(4326);
  pt->assignSpatialReference(srs);
  srs->Release();
  f->SetGeometryDirectly(pt);

  auto fDst = std::make_unique<OGRFeature>(poDefn);

  std::vector<GByte> byteBuffer;
  EXPECT_TRUE(f->SerializeToBinary(byteBuffer));
  EXPECT_TRUE(
      fDst->DeserializeFromBinary(byteBuffer.data(), byteBuffer.size()));
  EXPECT_EQ(f->GetFID(), fDst->GetFID());
  EXPECT_TRUE(f->Equal(fDst.get()));

  // Error handling in DeserializeFromBinary
  EXPECT_FALSE(fDst->DeserializeFromBinary(nullptr, 0));
  EXPECT_FALSE(fDst->DeserializeFromBinary(byteBuffer.data(), 1));  // too small
  const GByte badHeader[] = {0xFF, 0xFF, 0xFF, 0xFF};
  EXPECT_FALSE(fDst->DeserializeFromBinary(badHeader, sizeof(badHeader)));

  // Truncated buffer test
  if (byteBuffer.size() > 10) {
    EXPECT_FALSE(
        fDst->DeserializeFromBinary(byteBuffer.data(), byteBuffer.size() - 5));
  }
}

TEST(FeatureExtendedTest, ValidateFull) {
  OGRFeatureDefn* poDefn = new OGRFeatureDefn("val_schema");
  poDefn->Reference();
  auto defn_cleaner = absl::MakeCleanup([poDefn] { poDefn->Release(); });

  OGRFieldDefn fStr("str_val", OFTString);
  fStr.SetNullable(FALSE);
  fStr.SetWidth(5);
  poDefn->AddFieldDefn(&fStr);

  poDefn->GetGeomFieldDefn(0)->SetType(wkbPoint);
  poDefn->GetGeomFieldDefn(0)->SetNullable(FALSE);

  auto f = std::make_unique<OGRFeature>(poDefn);

  // Non-nullable field is unset (and default is null) -> invalid
  EXPECT_FALSE(f->Validate(OGR_F_VAL_NULL, FALSE));
  EXPECT_FALSE(f->Validate(OGR_F_VAL_NULL, TRUE));

  // Non-nullable field is null -> invalid
  f->SetFieldNull(0);
  EXPECT_FALSE(f->Validate(OGR_F_VAL_NULL, FALSE));
  EXPECT_FALSE(f->Validate(OGR_F_VAL_NULL, TRUE));

  // String width exceeded -> invalid
  f->SetField(0, "toolongstring");
  EXPECT_FALSE(f->Validate(OGR_F_VAL_WIDTH, FALSE));
  EXPECT_FALSE(f->Validate(OGR_F_VAL_WIDTH, TRUE));

  // Valid string
  f->SetField(0, "short");
  EXPECT_TRUE(f->Validate(OGR_F_VAL_WIDTH, FALSE));

  // Non-nullable geometry is null -> invalid
  EXPECT_FALSE(f->Validate(OGR_F_VAL_NULL, FALSE));
  EXPECT_FALSE(f->Validate(OGR_F_VAL_NULL, TRUE));

  // Geometry type mismatch -> invalid
  auto poly = new OGRPolygon();
  f->SetGeometryDirectly(poly);
  EXPECT_FALSE(f->Validate(OGR_F_VAL_GEOM_TYPE, FALSE));
  EXPECT_FALSE(f->Validate(OGR_F_VAL_GEOM_TYPE, TRUE));

  // Correct geometry type -> valid
  f->SetGeometryDirectly(new OGRPoint(1, 2));
  EXPECT_TRUE(f->Validate(OGR_F_VAL_ALL, TRUE));
}

TEST(FeatureExtendedTest, FillUnsetWithDefaultFull) {
  OGRFeatureDefn* poDefn = new OGRFeatureDefn("default_schema");
  poDefn->SetGeomType(wkbNone);
  poDefn->Reference();
  auto defn_cleaner = absl::MakeCleanup([poDefn] { poDefn->Release(); });

  OGRFieldDefn f0("cur_ts", OFTDateTime);
  f0.SetDefault("CURRENT_TIMESTAMP");
  OGRFieldDefn f1("dt_lit", OFTDateTime);
  f1.SetDefault("'2024/05/10 12:30:00'");
  OGRFieldDefn f2("str_lit", OFTString);
  f2.SetDefault("'literal_value'");
  OGRFieldDefn f3("int_lit", OFTInteger);
  f3.SetDefault("123");

  poDefn->AddFieldDefn(&f0);
  poDefn->AddFieldDefn(&f1);
  poDefn->AddFieldDefn(&f2);
  poDefn->AddFieldDefn(&f3);

  auto f = std::make_unique<OGRFeature>(poDefn);
  // With bNotNullableOnly=TRUE, nullable fields are skipped
  f->FillUnsetWithDefault(TRUE, nullptr);
  EXPECT_FALSE(f->IsFieldSet(0));

  // With bNotNullableOnly=FALSE, all unset fields with defaults are filled
  f->FillUnsetWithDefault(FALSE, nullptr);

  EXPECT_TRUE(f->IsFieldSet(0));
  EXPECT_TRUE(f->IsFieldSet(1));
  int y = 0, m = 0, d = 0, hr = 0, min = 0, tz = 0;
  float sec = 0.0f;
  EXPECT_TRUE(f->GetFieldAsDateTime(1, &y, &m, &d, &hr, &min, &sec, &tz));
  EXPECT_EQ(2024, y);
  EXPECT_EQ(5, m);
  EXPECT_EQ(10, d);
  EXPECT_EQ(12, hr);
  EXPECT_EQ(30, min);

  EXPECT_STREQ("literal_value", f->GetFieldAsString(2));
  EXPECT_EQ(123, f->GetFieldAsInteger(3));
}

TEST(FeatureExtendedTest, SetFieldsFromConversions) {
  OGRFeatureDefn* poSrcDefn = new OGRFeatureDefn("src_schema");
  poSrcDefn->SetGeomType(wkbNone);
  poSrcDefn->Reference();
  auto src_cleaner = absl::MakeCleanup([poSrcDefn] { poSrcDefn->Release(); });

  OGRFieldDefn fDate("date_f", OFTDate);
  OGRFieldDefn fTime("time_f", OFTTime);
  OGRFieldDefn fDateTime("dt_f", OFTDateTime);
  OGRFieldDefn fBin("bin_f", OFTBinary);
  poSrcDefn->AddFieldDefn(&fDate);
  poSrcDefn->AddFieldDefn(&fTime);
  poSrcDefn->AddFieldDefn(&fDateTime);
  poSrcDefn->AddFieldDefn(&fBin);

  auto fSrc = std::make_unique<OGRFeature>(poSrcDefn);
  fSrc->SetField(0, 2024, 11, 5, 0, 0, 0.0f, 0);
  fSrc->SetField(1, 0, 0, 0, 10, 20, 30.0f, 0);
  fSrc->SetField(2, 2024, 11, 5, 10, 20, 30.0f, 100);
  const GByte binData[] = {0x01, 0x02};
  fSrc->SetField(3, 2, binData);

  // Dst with String fields: copying Date/Time/DateTime/Binary to String
  OGRFeatureDefn* poDstDefn = new OGRFeatureDefn("dst_schema");
  poDstDefn->SetGeomType(wkbNone);
  poDstDefn->Reference();
  auto dst_cleaner = absl::MakeCleanup([poDstDefn] { poDstDefn->Release(); });

  OGRFieldDefn fStr0("date_f", OFTString);
  OGRFieldDefn fStr1("time_f", OFTString);
  OGRFieldDefn fStr2("dt_f", OFTString);
  OGRFieldDefn fStr3("bin_f", OFTString);
  poDstDefn->AddFieldDefn(&fStr0);
  poDstDefn->AddFieldDefn(&fStr1);
  poDstDefn->AddFieldDefn(&fStr2);
  poDstDefn->AddFieldDefn(&fStr3);

  auto fDst = std::make_unique<OGRFeature>(poDstDefn);
  const int map[] = {0, 1, 2, 3};
  EXPECT_EQ(OGRERR_NONE, fDst->SetFieldsFrom(fSrc.get(), map, TRUE));
  EXPECT_TRUE(fDst->IsFieldSet(0));
  EXPECT_TRUE(fDst->IsFieldSet(1));
  EXPECT_TRUE(fDst->IsFieldSet(2));
  EXPECT_TRUE(fDst->IsFieldSet(3));

  // Incompatible types with bForgiving = FALSE returns OGRERR_FAILURE
  OGRFeatureDefn* poIncompatDefn = new OGRFeatureDefn("incompat_schema");
  poIncompatDefn->SetGeomType(wkbNone);
  poIncompatDefn->Reference();
  auto incomp_cleaner =
      absl::MakeCleanup([poIncompatDefn] { poIncompatDefn->Release(); });

  OGRFieldDefn fInt0("date_f", OFTInteger);
  poIncompatDefn->AddFieldDefn(&fInt0);
  auto fIncompat = std::make_unique<OGRFeature>(poIncompatDefn);
  const int mapIncompat[] = {0, -1, -1, -1};
  EXPECT_EQ(OGRERR_FAILURE,
            fIncompat->SetFieldsFrom(fSrc.get(), mapIncompat, FALSE));

  // OGR_STYLE special field check in GetStyleString()
  OGRFeatureDefn* poStyleDefn = new OGRFeatureDefn("style_schema");
  poStyleDefn->SetGeomType(wkbNone);
  poStyleDefn->Reference();
  auto style_cleaner =
      absl::MakeCleanup([poStyleDefn] { poStyleDefn->Release(); });

  OGRFieldDefn fStyle("OGR_STYLE", OFTString);
  poStyleDefn->AddFieldDefn(&fStyle);
  auto fStyleFeat = std::make_unique<OGRFeature>(poStyleDefn);
  fStyleFeat->SetField("OGR_STYLE", "@pen(c:#123456)");
  EXPECT_STREQ("@pen(c:#123456)", fStyleFeat->GetStyleString());
}

TEST(FeatureExtendedTest, FieldDomainsClone) {
  OGRField sMin;
  sMin.Integer = 0;
  OGRField sMax;
  sMax.Integer = 100;
  auto range_domain = std::make_unique<OGRRangeFieldDomain>(
      "rdom", "range domain", OFTInteger, OFSTNone, sMin, true, sMax, true);
  range_domain->SetMergePolicy(OFDMP_DEFAULT_VALUE);
  range_domain->SetSplitPolicy(OFDSP_DEFAULT_VALUE);
  std::unique_ptr<OGRRangeFieldDomain> cloned_range(range_domain->Clone());
  ASSERT_THAT(cloned_range, testing::NotNull());
  EXPECT_STREQ("rdom", cloned_range->GetName().c_str());
  EXPECT_EQ(OFDMP_DEFAULT_VALUE, cloned_range->GetMergePolicy());
  EXPECT_EQ(OFDSP_DEFAULT_VALUE, cloned_range->GetSplitPolicy());

  auto glob_domain = std::make_unique<OGRGlobFieldDomain>(
      "gdom", "glob domain", OFTString, OFSTNone, "*glob*");
  glob_domain->SetMergePolicy(OFDMP_DEFAULT_VALUE);
  glob_domain->SetSplitPolicy(OFDSP_DEFAULT_VALUE);
  std::unique_ptr<OGRGlobFieldDomain> cloned_glob(glob_domain->Clone());
  ASSERT_THAT(cloned_glob, testing::NotNull());
  EXPECT_STREQ("gdom", cloned_glob->GetName().c_str());
  EXPECT_EQ(OFDMP_DEFAULT_VALUE, cloned_glob->GetMergePolicy());
  EXPECT_EQ(OFDSP_DEFAULT_VALUE, cloned_glob->GetSplitPolicy());
}

TEST(FeatureExtendedTest, EqualMismatches) {
  OGRFeatureDefn* poDefn = new OGRFeatureDefn("equal_schema");
  poDefn->Reference();
  auto defn_cleaner = absl::MakeCleanup([poDefn] { poDefn->Release(); });

  OGRFieldDefn f0("int", OFTInteger);
  OGRFieldDefn f1("int64", OFTInteger64);
  OGRFieldDefn f2("real", OFTReal);
  OGRFieldDefn f3("str", OFTString);
  OGRFieldDefn f4("date", OFTDate);
  OGRFieldDefn f5("binary", OFTBinary);
  OGRFieldDefn f6("int_list", OFTIntegerList);
  OGRFieldDefn f7("int64_list", OFTInteger64List);
  OGRFieldDefn f8("real_list", OFTRealList);
  OGRFieldDefn f9("str_list", OFTStringList);

  poDefn->AddFieldDefn(&f0);
  poDefn->AddFieldDefn(&f1);
  poDefn->AddFieldDefn(&f2);
  poDefn->AddFieldDefn(&f3);
  poDefn->AddFieldDefn(&f4);
  poDefn->AddFieldDefn(&f5);
  poDefn->AddFieldDefn(&f6);
  poDefn->AddFieldDefn(&f7);
  poDefn->AddFieldDefn(&f8);
  poDefn->AddFieldDefn(&f9);

  auto populate = [](OGRFeature* f) {
    f->SetFID(10);
    f->SetField(0, 100);
    f->SetField(1, static_cast<GIntBig>(200LL));
    f->SetField(2, 300.5);
    f->SetField(3, "str");
    f->SetField(4, 2024, 1, 1, 0, 0, 0.0f, 0);
    const GByte bData[] = {1, 2, 3};
    f->SetField(5, 3, bData);
    const int iList[] = {1, 2};
    f->SetField(6, 2, iList);
    const GIntBig i64List[] = {10LL, 20LL};
    f->SetField(7, 2, i64List);
    const double rList[] = {1.1, 2.2};
    f->SetField(8, 2, rList);
    const char* sList[] = {"a", "b", nullptr};
    f->SetField(9, const_cast<char* const*>(sList));
    f->SetGeometryDirectly(new OGRPoint(1, 2));
  };

  auto fA = std::make_unique<OGRFeature>(poDefn);
  auto fB = std::make_unique<OGRFeature>(poDefn);
  populate(fA.get());
  populate(fB.get());
  EXPECT_TRUE(fA->Equal(fB.get()));

  // FID mismatch
  fB->SetFID(20);
  EXPECT_FALSE(fA->Equal(fB.get()));
  fB->SetFID(10);

  // Field set vs unset mismatch
  fB->UnsetField(0);
  EXPECT_FALSE(fA->Equal(fB.get()));
  fB->SetField(0, 100);

  // Field null vs not null mismatch
  fB->SetFieldNull(0);
  EXPECT_FALSE(fA->Equal(fB.get()));
  fB->SetField(0, 100);

  // Integer mismatch
  fB->SetField(0, 999);
  EXPECT_FALSE(fA->Equal(fB.get()));
  fB->SetField(0, 100);

  // Integer64 mismatch
  fB->SetField(1, static_cast<GIntBig>(999LL));
  EXPECT_FALSE(fA->Equal(fB.get()));
  fB->SetField(1, static_cast<GIntBig>(200LL));

  // Real mismatch
  fB->SetField(2, 999.0);
  EXPECT_FALSE(fA->Equal(fB.get()));
  fB->SetField(2, std::numeric_limits<double>::quiet_NaN());
  EXPECT_FALSE(fA->Equal(fB.get()));
  fA->SetField(2, std::numeric_limits<double>::quiet_NaN());
  EXPECT_TRUE(fA->Equal(fB.get()));  // Both NaN are equal
  fA->SetField(2, 300.5);
  fB->SetField(2, 300.5);

  // String mismatch
  fB->SetField(3, "different");
  EXPECT_FALSE(fA->Equal(fB.get()));
  fB->SetField(3, "str");

  // Date mismatch
  fB->SetField(4, 2025, 1, 1, 0, 0, 0.0f, 0);
  EXPECT_FALSE(fA->Equal(fB.get()));
  fB->SetField(4, 2024, 1, 1, 0, 0, 0.0f, 0);

  // Binary mismatch (size & content)
  const GByte bDataDiffSize[] = {1, 2};
  fB->SetField(5, 2, bDataDiffSize);
  EXPECT_FALSE(fA->Equal(fB.get()));
  const GByte bDataDiffContent[] = {1, 2, 9};
  fB->SetField(5, 3, bDataDiffContent);
  EXPECT_FALSE(fA->Equal(fB.get()));
  const GByte bDataOrig[] = {1, 2, 3};
  fB->SetField(5, 3, bDataOrig);

  // IntegerList mismatch (size & content)
  const int iListDiffSize[] = {1};
  fB->SetField(6, 1, iListDiffSize);
  EXPECT_FALSE(fA->Equal(fB.get()));
  const int iListDiffVal[] = {1, 9};
  fB->SetField(6, 2, iListDiffVal);
  EXPECT_FALSE(fA->Equal(fB.get()));
  const int iListOrig[] = {1, 2};
  fB->SetField(6, 2, iListOrig);

  // Integer64List mismatch
  const GIntBig i64ListDiffVal[] = {10LL, 99LL};
  fB->SetField(7, 2, i64ListDiffVal);
  EXPECT_FALSE(fA->Equal(fB.get()));
  const GIntBig i64ListOrig[] = {10LL, 20LL};
  fB->SetField(7, 2, i64ListOrig);

  // RealList mismatch (and NaNs)
  const double rListDiff[] = {1.1, 9.9};
  fB->SetField(8, 2, rListDiff);
  EXPECT_FALSE(fA->Equal(fB.get()));
  const double rListNaN[] = {1.1, std::numeric_limits<double>::quiet_NaN()};
  fB->SetField(8, 2, rListNaN);
  EXPECT_FALSE(fA->Equal(fB.get()));
  fA->SetField(8, 2, rListNaN);
  EXPECT_TRUE(fA->Equal(fB.get()));
  const double rListOrig[] = {1.1, 2.2};
  fA->SetField(8, 2, rListOrig);
  fB->SetField(8, 2, rListOrig);

  // StringList mismatch
  const char* sListDiff[] = {"a", "z", nullptr};
  fB->SetField(9, const_cast<char* const*>(sListDiff));
  EXPECT_FALSE(fA->Equal(fB.get()));
  const char* sListOrig[] = {"a", "b", nullptr};
  fB->SetField(9, const_cast<char* const*>(sListOrig));

  // Geometry mismatch & null geometry
  fB->SetGeometryDirectly(new OGRPoint(99, 99));
  EXPECT_FALSE(fA->Equal(fB.get()));
  fB->SetGeometryDirectly(nullptr);
  EXPECT_FALSE(fA->Equal(fB.get()));
  fA->SetGeometryDirectly(nullptr);
  EXPECT_TRUE(fA->Equal(fB.get()));

  // Defn mismatch
  OGRFeatureDefn* poOtherDefn = new OGRFeatureDefn("other_schema");
  poOtherDefn->Reference();
  auto other_cleaner =
      absl::MakeCleanup([poOtherDefn] { poOtherDefn->Release(); });
  auto fOther = std::make_unique<OGRFeature>(poOtherDefn);
  EXPECT_FALSE(fA->Equal(fOther.get()));
}

TEST(FeatureExtendedTest, SetFieldInternalAllTypes) {
  OGRFeatureDefn* poDefn = new OGRFeatureDefn("internal_schema");
  poDefn->SetGeomType(wkbNone);
  poDefn->Reference();
  auto defn_cleaner = absl::MakeCleanup([poDefn] { poDefn->Release(); });

  OGRFieldDefn f0("str", OFTString);
  OGRFieldDefn f1("int_list", OFTIntegerList);
  OGRFieldDefn f2("int64_list", OFTInteger64List);
  OGRFieldDefn f3("real_list", OFTRealList);
  OGRFieldDefn f4("str_list", OFTStringList);
  OGRFieldDefn f5("bin", OFTBinary);

  poDefn->AddFieldDefn(&f0);
  poDefn->AddFieldDefn(&f1);
  poDefn->AddFieldDefn(&f2);
  poDefn->AddFieldDefn(&f3);
  poDefn->AddFieldDefn(&f4);
  poDefn->AddFieldDefn(&f5);

  auto fSrc = std::make_unique<OGRFeature>(poDefn);
  fSrc->SetField(0, "initial_str");
  const int iList[] = {1, 2};
  fSrc->SetField(1, 2, iList);
  const GIntBig i64List[] = {10LL, 20LL};
  fSrc->SetField(2, 2, i64List);
  const double rList[] = {1.5, 2.5};
  fSrc->SetField(3, 2, rList);
  const char* sList[] = {"x", "y", nullptr};
  fSrc->SetField(4, const_cast<char* const*>(sList));
  const GByte bData[] = {0x11, 0x22};
  fSrc->SetField(5, 2, bData);

  auto fDst = std::make_unique<OGRFeature>(poDefn);
  // Populate from fSrc using SetField(i, const OGRField*)
  for (int i = 0; i < 6; ++i) {
    fDst->SetField(i, fSrc->GetRawFieldRef(i));
  }
  EXPECT_STREQ("initial_str", fDst->GetFieldAsString(0));

  // Overwrite existing populated fields with new values
  fSrc->SetField(0, "updated_str");
  fDst->SetField(0, fSrc->GetRawFieldRef(0));
  EXPECT_STREQ("updated_str", fDst->GetFieldAsString(0));

  for (int i = 1; i < 6; ++i) {
    fDst->SetField(i, fSrc->GetRawFieldRef(i));
  }

  // Overwrite with null raw fields
  for (int i = 0; i < 6; ++i) {
    fSrc->SetFieldNull(i);
    fDst->SetField(i, fSrc->GetRawFieldRef(i));
    EXPECT_TRUE(fDst->IsFieldNull(i));
  }

  // Overwrite with unset raw fields
  for (int i = 0; i < 6; ++i) {
    fSrc->UnsetField(i);
    fDst->SetField(i, fSrc->GetRawFieldRef(i));
    EXPECT_FALSE(fDst->IsFieldSet(i));
  }
}

TEST(FeatureExtendedTest, GetFieldAsSerializedJSonAllTypes) {
  OGRFeatureDefn* poDefn = new OGRFeatureDefn("json_schema");
  poDefn->SetGeomType(wkbNone);
  poDefn->Reference();
  auto defn_cleaner = absl::MakeCleanup([poDefn] { poDefn->Release(); });

  OGRFieldDefn fStr("str", OFTString);
  fStr.SetSubType(OFSTJSON);
  OGRFieldDefn fIntList("int_list", OFTIntegerList);
  OGRFieldDefn fBoolList("bool_list", OFTIntegerList);
  fBoolList.SetSubType(OFSTBoolean);
  OGRFieldDefn fInt64List("int64_list", OFTInteger64List);
  OGRFieldDefn fRealList("real_list", OFTRealList);
  OGRFieldDefn fStrList("str_list", OFTStringList);

  poDefn->AddFieldDefn(&fStr);
  poDefn->AddFieldDefn(&fIntList);
  poDefn->AddFieldDefn(&fBoolList);
  poDefn->AddFieldDefn(&fInt64List);
  poDefn->AddFieldDefn(&fRealList);
  poDefn->AddFieldDefn(&fStrList);

  auto f = std::make_unique<OGRFeature>(poDefn);

  // Unset returns nullptr
  EXPECT_EQ(nullptr, f->GetFieldAsSerializedJSon(0));

  // Plain string
  f->SetField(0, "plain text");
  char* json = f->GetFieldAsSerializedJSon(0);
  ASSERT_THAT(json, testing::NotNull());
  EXPECT_STREQ("\"plain text\"", json);
  CPLFree(json);

  // String containing json object, array, boolean
  f->SetField(0, "{\"key\": \"val\"}");
  json = f->GetFieldAsSerializedJSon(0);
  EXPECT_STREQ("{\"key\": \"val\"}", json);
  CPLFree(json);

  f->SetField(0, "[1, 2, 3]");
  json = f->GetFieldAsSerializedJSon(0);
  EXPECT_STREQ("[1, 2, 3]", json);
  CPLFree(json);

  f->SetField(0, "true");
  json = f->GetFieldAsSerializedJSon(0);
  EXPECT_STREQ("true", json);
  CPLFree(json);

  // IntegerList
  const int iVals[] = {10, 20};
  f->SetField(1, 2, iVals);
  json = f->GetFieldAsSerializedJSon(1);
  EXPECT_THAT(json, testing::HasSubstr("10"));
  CPLFree(json);

  // BooleanList
  const int bVals[] = {1, 0, 1};
  f->SetField(2, 3, bVals);
  json = f->GetFieldAsSerializedJSon(2);
  EXPECT_THAT(json, testing::HasSubstr("true"));
  CPLFree(json);

  // Integer64List
  const GIntBig i64Vals[] = {10000000000LL, 20000000000LL};
  f->SetField(3, 2, i64Vals);
  json = f->GetFieldAsSerializedJSon(3);
  EXPECT_THAT(json, testing::HasSubstr("10000000000"));
  CPLFree(json);

  // RealList
  const double rVals[] = {1.5, 2.5};
  f->SetField(4, 2, rVals);
  json = f->GetFieldAsSerializedJSon(4);
  EXPECT_THAT(json, testing::HasSubstr("1.5"));
  CPLFree(json);

  // StringList
  const char* sVals[] = {"apple", "banana", nullptr};
  f->SetField(5, const_cast<char* const*>(sVals));
  json = f->GetFieldAsSerializedJSon(5);
  EXPECT_THAT(json, testing::HasSubstr("apple"));
  CPLFree(json);
}

TEST(FeatureExtendedTest, DumpReadableOptionsAndPrecision) {
  OGRFeatureDefn* poDefn = new OGRFeatureDefn("dump_schema");
  poDefn->Reference();
  auto defn_cleaner = absl::MakeCleanup([poDefn] { poDefn->Release(); });

  OGRFieldDefn fBool("flag", OFTInteger);
  fBool.SetSubType(OFSTBoolean);
  poDefn->AddFieldDefn(&fBool);

  OGRFieldDefn fNull("null_f", OFTString);
  poDefn->AddFieldDefn(&fNull);

  OGRGeomCoordinatePrecision oCoordPrec;
  oCoordPrec.dfXYResolution = 0.01;
  oCoordPrec.dfZResolution = 0.1;
  poDefn->GetGeomFieldDefn(0)->SetCoordinatePrecision(oCoordPrec);

  auto f = std::make_unique<OGRFeature>(poDefn);
  f->SetField(0, 1);
  f->SetFieldNull(1);
  f->SetGeometryDirectly(new OGRPoint(10.12345, 20.67891, 5.555));

  const std::string dump = f->DumpReadableAsString();
  EXPECT_THAT(dump, testing::HasSubstr("Integer(Boolean)"));
  EXPECT_THAT(dump, testing::HasSubstr("(null)"));
}

TEST(FeatureExtendedTest, SetFromNamedGeometries) {
  OGRFeatureDefn* poSrcDefn = new OGRFeatureDefn("src_geom_defn");
  poSrcDefn->SetGeomType(wkbNone);
  poSrcDefn->Reference();
  auto src_cleaner = absl::MakeCleanup([poSrcDefn] { poSrcDefn->Release(); });

  OGRGeomFieldDefn gfSrc0("geom_a", wkbPoint);
  OGRGeomFieldDefn gfSrc1("geom_b", wkbPoint);
  poSrcDefn->AddGeomFieldDefn(&gfSrc0);
  poSrcDefn->AddGeomFieldDefn(&gfSrc1);

  OGRFeatureDefn* poDstDefn = new OGRFeatureDefn("dst_geom_defn");
  poDstDefn->SetGeomType(wkbNone);
  poDstDefn->Reference();
  auto dst_cleaner = absl::MakeCleanup([poDstDefn] { poDstDefn->Release(); });

  OGRGeomFieldDefn gfDst0("geom_b", wkbPoint);
  OGRGeomFieldDefn gfDst1("geom_c", wkbPoint);
  poDstDefn->AddGeomFieldDefn(&gfDst0);
  poDstDefn->AddGeomFieldDefn(&gfDst1);

  auto fSrc = std::make_unique<OGRFeature>(poSrcDefn);
  fSrc->SetGeomFieldDirectly(0, new OGRPoint(1, 1));
  fSrc->SetGeomFieldDirectly(1, new OGRPoint(2, 2));

  auto fDst = std::make_unique<OGRFeature>(poDstDefn);
  EXPECT_EQ(OGRERR_NONE, fDst->SetFrom(fSrc.get(), FALSE));
  // geom_b should be copied to field 0 of dst
  ASSERT_THAT(fDst->GetGeomFieldRef(0), testing::NotNull());
  EXPECT_DOUBLE_EQ(2.0, fDst->GetGeomFieldRef(0)->toPoint()->getX());
  // geom_c was not in src, so should be null
  EXPECT_EQ(nullptr, fDst->GetGeomFieldRef(1));
}

TEST(FeatureExtendedTest, CAPINullAndEdgeCases) {
  EXPECT_EQ(nullptr, OGR_F_GetGeometryRef(nullptr));
  EXPECT_EQ(nullptr, OGR_F_GetGeomFieldRef(nullptr, 0));
  EXPECT_EQ(0, OGR_F_GetGeomFieldCount(nullptr));
  EXPECT_EQ(nullptr, OGR_F_GetGeomFieldDefnRef(nullptr, 0));
  EXPECT_EQ(0, OGR_F_GetGeomFieldIndex(nullptr, "geom"));
  EXPECT_EQ(nullptr, OGR_F_StealGeometry(nullptr));
  EXPECT_EQ(nullptr, OGR_F_StealGeometryEx(nullptr, 0));
  EXPECT_EQ(0, OGR_F_GetFieldAsInteger(nullptr, 0));
  EXPECT_EQ(0LL, OGR_F_GetFieldAsInteger64(nullptr, 0));
  EXPECT_DOUBLE_EQ(0.0, OGR_F_GetFieldAsDouble(nullptr, 0));
  EXPECT_EQ(nullptr, OGR_F_GetFieldAsString(nullptr, 0));
  EXPECT_EQ(nullptr, OGR_F_GetFieldAsStringList(nullptr, 0));
  EXPECT_EQ(nullptr, OGR_F_GetFieldAsBinary(nullptr, 0, nullptr));
  EXPECT_FALSE(OGR_F_GetFieldAsDateTime(nullptr, 0, nullptr, nullptr, nullptr,
                                        nullptr, nullptr, nullptr, nullptr));
  int y = 0, m = 0, d = 0, hr = 0, min = 0, tz = 0;
  float sec = 0.0f;
  EXPECT_FALSE(
      OGR_F_GetFieldAsDateTimeEx(nullptr, 0, &y, &m, &d, &hr, &min, &sec, &tz));
  OGR_F_SetFieldNull(nullptr, 0);
  OGR_F_UnsetField(nullptr, 0);
  OGR_F_Destroy(nullptr);
}

TEST(FeatureExtendedTest, FinalPushToCoverage) {
  OGRFeatureDefn* poDefn = new OGRFeatureDefn("push_schema");
  poDefn->Reference();
  auto defn_cleaner = absl::MakeCleanup([poDefn] { poDefn->Release(); });

  OGRFieldDefn fBool("b", OFTInteger);
  fBool.SetSubType(OFSTBoolean);
  OGRFieldDefn fBoolList("bl", OFTIntegerList);
  fBoolList.SetSubType(OFSTBoolean);
  OGRFieldDefn fBoolList64("bl64", OFTInteger64List);
  fBoolList64.SetSubType(OFSTBoolean);
  OGRFieldDefn fRealPrec("flt", OFTReal);
  fRealPrec.SetWidth(10);
  fRealPrec.SetPrecision(4);
  OGRFieldDefn fDT("dt", OFTDateTime);
  OGRFieldDefn fIntList("il", OFTIntegerList);
  OGRFieldDefn fInt64List("i64l", OFTInteger64List);
  OGRFieldDefn fRealList("rl", OFTRealList);
  OGRFieldDefn fStrList("sl", OFTStringList);

  poDefn->AddFieldDefn(&fBool);
  poDefn->AddFieldDefn(&fBoolList);
  poDefn->AddFieldDefn(&fBoolList64);
  poDefn->AddFieldDefn(&fRealPrec);
  poDefn->AddFieldDefn(&fDT);
  poDefn->AddFieldDefn(&fIntList);
  poDefn->AddFieldDefn(&fInt64List);
  poDefn->AddFieldDefn(&fRealList);
  poDefn->AddFieldDefn(&fStrList);

  auto f = std::make_unique<OGRFeature>(poDefn);

  // 1. Operator[] by name and throw on not found
  EXPECT_NO_THROW((*f)["b"] = 1);
  EXPECT_EQ(1, (*f)["b"].GetAsInteger());
  const OGRFeature* cf = f.get();
  EXPECT_EQ(1, (*cf)["b"].GetAsInteger());

  // 2. SetField(const char*) boolean values
  f->SetField("b", "1");
  EXPECT_EQ(1, f->GetFieldAsInteger("b"));
  f->SetField("b", "true");
  EXPECT_EQ(1, f->GetFieldAsInteger("b"));
  f->SetField("b", "on");
  EXPECT_EQ(1, f->GetFieldAsInteger("b"));
  f->SetField("b", "yes");
  EXPECT_EQ(1, f->GetFieldAsInteger("b"));
  f->SetField("b", "0");
  EXPECT_EQ(0, f->GetFieldAsInteger("b"));
  f->SetField("b", "false");
  EXPECT_EQ(0, f->GetFieldAsInteger("b"));
  f->SetField("b", "off");
  EXPECT_EQ(0, f->GetFieldAsInteger("b"));
  f->SetField("b", "no");
  EXPECT_EQ(0, f->GetFieldAsInteger("b"));
  {
    CPLErrorReset();
    f->SetField("b", "invalid_text");
    EXPECT_EQ(0, f->GetFieldAsInteger("b"));
  }
  {
    CPLErrorReset();
    f->SetField("b", "2");
    EXPECT_EQ(1, f->GetFieldAsInteger("b"));
  }

  // 3. SetField integer/int64 lists with OFSTBoolean normalization
  const int nonBoolInts[] = {5, 0, 10};
  f->SetField("bl", 3, nonBoolInts);
  int count = 0;
  const int* pan = f->GetFieldAsIntegerList("bl", &count);
  ASSERT_EQ(3, count);
  EXPECT_EQ(1, pan[0]);
  EXPECT_EQ(0, pan[1]);
  EXPECT_EQ(1, pan[2]);

  const GIntBig nonBoolInts64[] = {5LL, 0LL, 10LL};
  f->SetField("bl64", 3, nonBoolInts64);
  const GIntBig* pan64 = f->GetFieldAsInteger64List("bl64", &count);
  ASSERT_EQ(3, count);
  EXPECT_EQ(5LL, pan64[0]);
  EXPECT_EQ(0LL, pan64[1]);
  EXPECT_EQ(10LL, pan64[2]);

  // 4. Float formatting with width and precision
  f->SetField("flt", 12.3456);
  EXPECT_THAT(f->GetFieldAsString("flt"), testing::HasSubstr("12.3456"));

  // 5. DateTime formatting with negative and fractional timezone, and leap
  // second
  f->SetField("dt", 2024, 1, 1, 10, 0, 0.0f, 78);  // UTC-5:30
  EXPECT_THAT(f->GetFieldAsString("dt"), testing::HasSubstr("-0530"));
  f->SetField("dt", 2024, 1, 1, 10, 0, 61.0f, 100);  // second > 60
  EXPECT_TRUE(f->IsFieldSet(f->GetFieldIndex("dt")));

  // 6. JSON array parsing in SetField(const char*)
  f->SetField("il", "[10, 20, 30]");
  pan = f->GetFieldAsIntegerList("il", &count);
  EXPECT_EQ(3, count);

  f->SetField("i64l", "[10000000000, 20000000000]");
  pan64 = f->GetFieldAsInteger64List("i64l", &count);
  EXPECT_EQ(2, count);

  f->SetField("rl", "[1.5, 2.5]");
  const double* padf = f->GetFieldAsDoubleList("rl", &count);
  EXPECT_EQ(2, count);
  ASSERT_THAT(padf, testing::NotNull());
  EXPECT_DOUBLE_EQ(1.5, padf[0]);

  f->SetField("sl", "[\"item1\", \"item2\"]");
  char** retSl = f->GetFieldAsStringList("sl");
  EXPECT_EQ(2, CSLCount(retSl));

  f->SetField("sl", "single_item");
  retSl = f->GetFieldAsStringList("sl");
  EXPECT_EQ(1, CSLCount(retSl));

  // 7. Non-linear geometry conversion in C API
  OGRSetNonLinearGeometriesEnabledFlag(FALSE);
  auto auto_flag_reset =
      absl::MakeCleanup([] { OGRSetNonLinearGeometriesEnabledFlag(TRUE); });

  OGRFeatureH hFeat = OGR_F_Create(OGRFeatureDefnH(poDefn));
  auto feat_cleaner = absl::MakeCleanup([hFeat] { OGR_F_Destroy(hFeat); });

  auto cs = new OGRCircularString();
  cs->addPoint(0, 0);
  cs->addPoint(1, 1);
  cs->addPoint(2, 0);
  OGR_F_SetGeometryDirectly(hFeat, OGRGeometry::ToHandle(cs));

  OGRGeometryH hGeom = OGR_F_GetGeometryRef(hFeat);
  ASSERT_THAT(hGeom, testing::NotNull());
  EXPECT_EQ(wkbLineString, OGR_G_GetGeometryType(hGeom));

  OGRGeometryH hGeom0 = OGR_F_GetGeomFieldRef(hFeat, 0);
  ASSERT_THAT(hGeom0, testing::NotNull());
  EXPECT_EQ(wkbLineString, OGR_G_GetGeometryType(hGeom0));

  // 8. VarInt truncated buffer test for ReadVarUInt
  const GByte truncatedVarInt[] = {0x80};  // MSB set but buffer ends
  auto fTrunc = std::make_unique<OGRFeature>(poDefn);
  EXPECT_FALSE(
      fTrunc->DeserializeFromBinary(truncatedVarInt, sizeof(truncatedVarInt)));
}
}  // namespace autotest2
