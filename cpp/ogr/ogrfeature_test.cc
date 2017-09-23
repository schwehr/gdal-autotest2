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

// Test OGRFeature.

// clang-format off
#include "port/cpl_port.h"
#include "ogr/ogr_api.h"
#include "ogr/ogr_feature.h"
// clang-format on

#include <initializer_list>  // NOLINT(build/include_order)
#include <limits>
#include <memory>

#include "gmock.h"
#include "gunit.h"
#include "third_party/absl/memory/memory.h"
#include "autotest2/cpp/util/ogr/ogrfeaturedefn.h"
#include "autotest2/cpp/util/error_handler.h"
#include "ogr/ogr_core.h"
#include "util/gtl/cleanup.h"
#include "util/task/statusor.h"

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

  void CreateFeature(OGRFeatureDefnBuilder *builder) {
    fd_ = builder->Build();
    f_ = absl::MakeUnique<OGRFeature>(fd_);

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

  OGRFeatureDefn *fd_ = nullptr;
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
  for (int32 v : {-1, 1, std::numeric_limits<int32>::min(),
                  std::numeric_limits<int32>::max()}) {
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

  std::initializer_list<int64> values = {
      0,
      -1,
      1,
      std::numeric_limits<int16>::min(),
      std::numeric_limits<int16>::max(),
  };
  for (int16 v : values) {
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
  f_->SetField(0, std::numeric_limits<int32>::min());
  EXPECT_EQ(std::numeric_limits<int16>::min(), f_->GetFieldAsInteger(0));
  EXPECT_EQ(std::numeric_limits<int16>::min(), f_->GetFieldAsInteger64(0));
  f_->SetField(0, std::numeric_limits<int32>::max());
  EXPECT_EQ(std::numeric_limits<int16>::max(), f_->GetFieldAsInteger(0));
  EXPECT_EQ(std::numeric_limits<int16>::max(), f_->GetFieldAsInteger64(0));
}

TEST_F(OneFeatureTest, FieldInt32) {
  OGRFeatureDefnBuilder builder("schema");
  builder.AddField("i", OFTInteger, OFSTNone);
  CreateFeature(&builder);

  for (int32 v : {0, -1, 1, std::numeric_limits<int32>::min(),
                  std::numeric_limits<int32>::max()}) {
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
  f_->SetField(0, std::numeric_limits<int64>::min());
  EXPECT_EQ(std::numeric_limits<int32>::min(), f_->GetFieldAsInteger(0));
  EXPECT_EQ(std::numeric_limits<int32>::min(), f_->GetFieldAsInteger64(0));
  f_->SetField(0, std::numeric_limits<int64>::max());
  EXPECT_EQ(std::numeric_limits<int32>::max(), f_->GetFieldAsInteger(0));
  EXPECT_EQ(std::numeric_limits<int32>::max(), f_->GetFieldAsInteger64(0));

  f_->SetFieldNull(0);
  EXPECT_TRUE(f_->IsFieldSet(0));
  EXPECT_TRUE(f_->IsFieldNull(0));
  EXPECT_FALSE(f_->IsFieldSetAndNotNull(0));
  EXPECT_EQ(0, f_->GetFieldAsInteger(0));
}

namespace {

int32 ClampToInt32(int64 v64) {
  if (v64 < std::numeric_limits<int32>::min())
    return std::numeric_limits<int32>::min();

  if (v64 > std::numeric_limits<int32>::max())
    return std::numeric_limits<int32>::max();

  return static_cast<int32>(v64);
}

}  // namespace

TEST_F(OneFeatureTest, FieldInt64) {
  OGRFeatureDefnBuilder builder("schema");
  builder.AddField("i", OFTInteger64, OFSTNone);
  CreateFeature(&builder);

  std::initializer_list<int64> values = {
      0,
      -1,
      1,
      std::numeric_limits<int32>::min(),
      std::numeric_limits<int32>::max(),
      std::numeric_limits<int64>::min(),
      std::numeric_limits<int64>::max(),
  };
  for (int64 v : values) {
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
    if (v < std::numeric_limits<int32>::min()) {
      int64 v2 = static_cast<int64>(std::numeric_limits<int32>::min()) * 10;
      f_->SetField(0, static_cast<double>(v2));
      EXPECT_EQ(v2, f_->GetFieldAsInteger64(0));
    } else if (v > std::numeric_limits<int32>::min()) {
      int64 v2 = static_cast<int64>(std::numeric_limits<int32>::max()) * 10;
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

// TODO(schwehr): Test IntergerList
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
  auto fd_cleaner = gtl::MakeCleanup([fd] { fd->Release(); });
  auto f = OGRFeature::CreateFeature(fd);
  ASSERT_NE(nullptr, f);
  EXPECT_EQ(0, f->GetFieldCount());
  EXPECT_EQ(0, f->GetGeomFieldCount());
  OGRFeature::DestroyFeature(f);
  // Success if msan does not find trouble.
}

}  // namespace
}  // namespace autotest2
