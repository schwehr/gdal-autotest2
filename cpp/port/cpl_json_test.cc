// Test CPLJson related code
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_json.cpp
//
// Copyright 2024 Google Inc. All Rights Reserved.
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

#include "/port/cpl_json.h"

#include <features.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "gmock.h"
#include "gunit.h"

#include "absl/cleanup/cleanup.h"
#include "absl/log/check.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_replace.h"

#include "autotest2/cpp/util/error_handler.h"
#include "port/cpl_error.h"
#include "port/cpl_json_streaming_parser.h"
#include "port/cpl_json_streaming_writer.h"
#include "port/cpl_port.h"
#include "port/cpl_string.h"
#include "port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::SrcDir;

struct CplJsonObjectFormattingTestCase {
  std::string test_name;
  CPLJSONObject::PrettyFormat format;
  const char* expected;
};

using CplJsonObjectFormattingTest =
    testing::TestWithParam<CplJsonObjectFormattingTestCase>;

TEST_P(CplJsonObjectFormattingTest, TestCplJsonObjectFormatting) {
  const CplJsonObjectFormattingTestCase& test_case = GetParam();

  CPLJSONObject object;
  object.Set("foo", "bar");

  EXPECT_STREQ(object.Format(test_case.format).c_str(), test_case.expected);
}

INSTANTIATE_TEST_SUITE_P(
    CplJsonObjectFormattingTestSuiteInstantiation, CplJsonObjectFormattingTest,
    testing::ValuesIn<CplJsonObjectFormattingTestCase>({
        {"Spaced", CPLJSONObject::PrettyFormat::Spaced,
         R"json({ "foo": "bar" })json"},
        {"Pretty", CPLJSONObject::PrettyFormat::Pretty,
         R"json({
  "foo":"bar"
})json"},
        {"Plain", CPLJSONObject::PrettyFormat::Plain,
         R"json({"foo":"bar"})json"},
    }),
    [](const testing::TestParamInfo<CplJsonObjectFormattingTest::ParamType>&
           info) { return info.param.test_name; });

// TODO: Test CPLJSONObject constructors like CPLJSONObject object(nullptr) -
// available upstream but not in google3's version.

TEST(CplJsonObjectAddersAndGetters, CanAddAndGetString) {
  CPLJSONObject object;

  object.Add("key", "value");

  EXPECT_EQ(object.GetString("key", "default"), "value");
  EXPECT_EQ(object.GetString("nonexistent", "default"), "default");
  EXPECT_EQ(object.GetString("nonexistent"), "");
}

TEST(CplJsonObjectAddersAndGetters, CanAddAndGetDouble) {
  CPLJSONObject object;

  object.Add("key", 1.23);

  EXPECT_DOUBLE_EQ(object.GetDouble("key", 0.1), 1.23);
  EXPECT_DOUBLE_EQ(object.GetDouble("nonexistent", 0.1), 0.1);
  EXPECT_DOUBLE_EQ(object.GetDouble("nonexistent"), 0);
}

TEST(CplJsonObjectAddersAndGetters, CanAddAndGetInteger) {
  CPLJSONObject object;

  object.Add("key", 99);

  EXPECT_DOUBLE_EQ(object.GetInteger("key", 205), 99);
  EXPECT_DOUBLE_EQ(object.GetInteger("nonexistent", 205), 205);
  EXPECT_DOUBLE_EQ(object.GetInteger("nonexistent"), 0);
}

TEST(CplJsonObjectAddersAndGetters, CanAddAndGetLong) {
  const GInt64 kBigValue = 12ULL << 30;
  const GInt64 kTestDefaultBigValue = 99ULL << 20;
  CPLJSONObject object;

  object.Add("key", kBigValue);

  EXPECT_EQ(object.GetLong("key", kTestDefaultBigValue), kBigValue);
  EXPECT_EQ(object.GetLong("nonexistent", kTestDefaultBigValue),
            kTestDefaultBigValue);
  EXPECT_EQ(object.GetLong("nonexistent"), 0);
}

TEST(CplJsonObjectAddersAndGetters, CanAddAndGetBool) {
  CPLJSONObject object;

  object.Add("key", true);

  EXPECT_EQ(object.GetBool("key", false), true);
  EXPECT_EQ(object.GetBool("nonexistent", true), true);
  EXPECT_EQ(object.GetBool("nonexistent", false), false);
  EXPECT_EQ(object.GetBool("nonexistent"), false);
}

TEST(CplJsonObjectConversions, ToStringWithNonexistentKeyReturnsDefault) {
  CPLJSONObject object;

  EXPECT_EQ(object.GetObj("nonexistent").ToString("default"), "default");
}

TEST(CplJsonObjectConversions, DoubleToString) {
  CPLJSONObject object;

  object.Add("key", 0.123);
  EXPECT_EQ(object.GetObj("key").ToString("default"), "0.123");
}

TEST(CplJsonObjectConversions, IntegerToString) {
  CPLJSONObject object;

  object.Add("key", 324);
  EXPECT_EQ(object.GetObj("key").ToString("default"), "324");
}

TEST(CplJsonObjectConversions, LongToString) {
  CPLJSONObject object;

  object.Add("key", static_cast<GInt64>(23486));
  EXPECT_EQ(object.GetObj("key").ToString("default"), "23486");
}

TEST(CplJsonObjectConversions, BoolTrueToString) {
  CPLJSONObject object;

  object.Add("key", true);
  EXPECT_EQ(object.GetObj("key").ToString("default"), "true");
}

TEST(CplJsonObjectConversions, BoolFalseToString) {
  CPLJSONObject object;

  object.Add("key", false);
  EXPECT_EQ(object.GetObj("key").ToString("default"), "false");
}

TEST(CplJsonObjectConversions, ToDoubleWithNonexistentKeyReturnsDefault) {
  constexpr double kTestDefaultDouble = 3.452;
  CPLJSONObject object;

  EXPECT_DOUBLE_EQ(object.GetObj("nonexistent").ToDouble(kTestDefaultDouble),
                   kTestDefaultDouble);
}

TEST(CplJsonObjectConversions, StringToDouble) {
  CPLJSONObject object;

  object.Add("key", "1234.33");
  EXPECT_DOUBLE_EQ(object.GetObj("key").ToDouble(3.452), 1234.33);
}

TEST(CplJsonObjectConversions, IntegerToDouble) {
  CPLJSONObject object;

  object.Add("key", 2356);
  EXPECT_DOUBLE_EQ(object.GetObj("key").ToDouble(3.452), 2356.0);
}

TEST(CplJsonObjectConversions, LongToDouble) {
  CPLJSONObject object;

  object.Add("key", static_cast<GInt64>(89754));
  EXPECT_DOUBLE_EQ(object.GetObj("key").ToDouble(3.452), 89754.0);
}

TEST(CplJsonObjectConversions, BoolFalseToDouble) {
  CPLJSONObject object;

  object.Add("key", false);
  EXPECT_DOUBLE_EQ(object.GetObj("key").ToDouble(3.452), 0.0);
}

TEST(CplJsonObjectConversions, BoolTrueToDouble) {
  CPLJSONObject object;

  object.Add("key", true);
  EXPECT_DOUBLE_EQ(object.GetObj("key").ToDouble(3.452), 1.0);
}

TEST(CplJsonObjectConversions, ToIntegerWithNonexistentKeyReturnsDefault) {
  constexpr int kDefault = 3245;
  CPLJSONObject object;

  EXPECT_EQ(object.GetObj("nonexistent").ToInteger(kDefault), kDefault);
  EXPECT_EQ(object.GetObj("nonexistent").ToInteger(), 0);
}

TEST(CplJsonObjectConversions, MoreThanPoint5DoubleToIntegerTruncates) {
  CPLJSONObject object;

  object.Add("key", 245.88);
  EXPECT_EQ(object.GetObj("key").ToInteger(22), 245);
}

TEST(CplJsonObjectConversions, LessThanPoint5DoubleToIntegerTruncates) {
  CPLJSONObject object;

  object.Add("key", 33.4);
  EXPECT_EQ(object.GetObj("key").ToInteger(22), 33);
}

TEST(CplJsonObjectConversions,
     MoreThanPoint5NegativeDoubleToIntegerTruncatesTowardsZero) {
  CPLJSONObject object;

  object.Add("key", -33.4);
  EXPECT_EQ(object.GetObj("key").ToInteger(22), -33);
}

TEST(CplJsonObjectConversions,
     LessThanPoint5NegativeDoubleToIntegerTruncatesTowardsZero) {
  CPLJSONObject object;

  object.Add("key", -33.8);
  EXPECT_EQ(object.GetObj("key").ToInteger(22), -33);
}

TEST(CplJsonObjectConversions, LongToInteger) {
  CPLJSONObject object;

  object.Add("key", static_cast<GInt64>(1238473));
  EXPECT_EQ(object.GetObj("key").ToInteger(22), 1238473);
}

TEST(CplJsonObjectConversions, BoolFalseToInteger) {
  CPLJSONObject object;

  object.Add("key", false);
  EXPECT_EQ(object.GetObj("key").ToInteger(22), 0);
}

TEST(CplJsonObjectConversions, BoolTrueToInteger) {
  CPLJSONObject object;

  object.Add("key", true);
  EXPECT_EQ(object.GetObj("key").ToInteger(22), 1);
}

TEST(CplJsonObjectConversions, ToLongWithNonexistentKeyReturnsDefault) {
  CPLJSONObject object;

  EXPECT_EQ(object.GetObj("nonexistent").ToLong(9817254), 9817254);
  EXPECT_EQ(object.GetObj("nonexistent").ToLong(), static_cast<GInt64>(0));
}

TEST(CplJsonObjectConversions, MoreThanPoint5DoubleToLongTruncatesTowards0) {
  CPLJSONObject object;

  object.Add("key", 1234.94235);
  EXPECT_EQ(object.GetObj("key").ToLong(22), 1234);
}

TEST(CplJsonObjectConversions, LessThanPoint5DoubleToLongTruncatesTowards0) {
  CPLJSONObject object;

  object.Add("key", 1234.335);
  EXPECT_EQ(object.GetObj("key").ToLong(22), 1234);
}

TEST(CplJsonObjectConversions,
     MoreThanPoint5NegativeDoubleToLongTruncatesTowards0) {
  CPLJSONObject object;

  object.Add("key", -53456.1234);
  EXPECT_EQ(object.GetObj("key").ToLong(22), -53456);
}

TEST(CplJsonObjectConversions,
     LessThanPoint5NegativeDoubleToLongTruncatesTowards0) {
  CPLJSONObject object;

  object.Add("key", -53456.6554);
  EXPECT_EQ(object.GetObj("key").ToLong(22), -53456);
}

TEST(CplJsonObjectConversions, IntegerToLong) {
  CPLJSONObject object;

  object.Add("key", 846735);
  EXPECT_EQ(object.GetObj("key").ToLong(22), static_cast<GInt64>(846735));
}

TEST(CplJsonObjectConversions, StringToLong) {
  CPLJSONObject object;

  object.Add("key", "3452345");
  EXPECT_EQ(object.GetObj("key").ToLong(22), static_cast<GInt64>(3452345));
}

TEST(CplJsonObjectConversions, BoolFalseToLong) {
  CPLJSONObject object;

  object.Add("key", false);
  EXPECT_EQ(object.GetObj("key").ToLong(22), 0);
}

TEST(CplJsonObjectConversions, BoolTrueToLong) {
  CPLJSONObject object;

  object.Add("key", true);
  EXPECT_EQ(object.GetObj("key").ToLong(22), 1);
}

TEST(CplJsonObjectConversions, StringToInteger) {
  CPLJSONObject object;

  object.Add("key", "14323");
  EXPECT_EQ(object.GetObj("key").ToInteger(22), 14323);
}

TEST(CplJsonObjectConversions, ToBoolWithNonexistentKeyReturnsDefault) {
  CPLJSONObject object;

  EXPECT_EQ(object.GetObj("nonexistent").ToBool(true), true);
  EXPECT_EQ(object.GetObj("nonexistent").ToBool(false), false);
}

TEST(CplJsonObjectConversions, StringToBool) {
  CPLJSONObject object;

  object.Add("key", "foo");
  EXPECT_EQ(object.GetObj("key").ToBool(false), true);
}

TEST(CplJsonObjectConversions, EmptyStringToBoolIsFalse) {
  CPLJSONObject object;

  object.Add("key", "");
  EXPECT_EQ(object.GetObj("key").ToBool(false), false);
}

TEST(CplJsonObjectConversions, DoubleToBool) {
  CPLJSONObject object;

  object.Add("key", 0.1);
  EXPECT_EQ(object.GetObj("key").ToBool(false), true);
}

TEST(CplJsonObjectConversions, ZeroDoubleToBoolIsFalse) {
  CPLJSONObject object;

  object.Add("key", 0.0);
  EXPECT_EQ(object.GetObj("key").ToBool(false), false);
}

TEST(CplJsonObjectConversions, IntegerToBool) {
  CPLJSONObject object;

  object.Add("key", 1);
  EXPECT_EQ(object.GetObj("key").ToBool(false), true);
}

TEST(CplJsonObjectConversions, ZeroIntegerToBoolIsFalse) {
  CPLJSONObject object;

  object.Add("key", 0);
  EXPECT_EQ(object.GetObj("key").ToBool(false), false);
}

TEST(CplJsonObjectConversions, LongToBool) {
  CPLJSONObject object;

  object.Add("key", static_cast<GInt64>(12ULL << 30));
  EXPECT_EQ(object.GetObj("key").ToBool(false), true);
}

TEST(CplJsonObjectConversions, ZeroLongToBoolIsFalse) {
  CPLJSONObject object;

  object.Add("key", static_cast<GInt64>(0));
  EXPECT_EQ(object.GetObj("key").ToBool(false), false);
}

TEST(CplJsonObjectDeletion, DeleteRemovesKeyValue) {
  CPLJSONObject object;

  object.Add("key", 112434);
  ASSERT_EQ(object.GetInteger("key"), 112434);

  object.Delete("key");
  EXPECT_EQ(object.GetInteger("key"), 0);
}

TEST(CplJsonObjectDeletion, DeleteReadsSlashesInPath) {
  CPLJSONObject object1;
  CPLJSONObject object2;

  object1.Add("key1", object2);
  object2.Add("key2", 14236);
  ASSERT_EQ(object1.GetInteger("key1/key2"), 14236);

  object1.Delete("key1/key2");
  EXPECT_EQ(object1.GetInteger("key1/key2"), 0);

  EXPECT_STREQ(
      object1.GetObj("key1").Format(CPLJSONObject::PrettyFormat::Plain).c_str(),
      "{}");
}

TEST(CplJsonObjectDeletion, DeleteNoSplitName) {
  CPLJSONObject object1;
  CPLJSONObject object2;

  object1.Add("key1", object2);
  object2.Add("key2", 48354);

  ASSERT_EQ(object1.GetInteger("key1/key2"), 48354);

  // Should treat "key1/key2" as a single key and not delete the integer
  object1.DeleteNoSplitName("key1/key2");
  EXPECT_EQ(object1.GetInteger("key1/key2"), 48354);
}

TEST(CplJsonObjectDeletion, DeleteNonexistentNoError) {
  CPLJSONObject object;

  object.Delete("key");
  EXPECT_STREQ(object.Format(CPLJSONObject::PrettyFormat::Plain).c_str(), "{}");
}

TEST(CplJsonObjectGetChildren, GetChildren) {
  CPLJSONObject object;

  object.Add("key1", 1);
  object.Add("key2", 2);

  EXPECT_EQ(object.GetChildren().size(), 2);

  std::vector<int> values;
  for (auto c : object.GetChildren()) {
    values.push_back(c.ToInteger());
  }

  EXPECT_THAT(values, ::testing::ElementsAre(1, 2));
}

TEST(CplJsonObjectAdd, AddNestedObject) {
  CPLJSONObject object;

  object.Add("key1/key2", 12354);
  EXPECT_THAT(object.Format(CPLJSONObject::PrettyFormat::Plain),
              ::testing::StrEq(R"json({"key1":{"key2":12354}})json"));
}

TEST(CplJsonObjectAdd, AddNoSplitNameShouldNotCreateNestedObject) {
  CPLJSONObject object;
  CPLJSONObject empty;

  object.AddNoSplitName("key1/key2", empty);
  EXPECT_THAT(object.Format(CPLJSONObject::PrettyFormat::Plain),
              ::testing::StrEq(R"json({"key1\/key2":{}})json"));
}

TEST(CplJsonObjectAdd, AddNull) {
  CPLJSONObject object;

  object.AddNull("key");
  EXPECT_THAT(object.Format(CPLJSONObject::PrettyFormat::Plain),
              ::testing::StrEq(R"json({"key":null})json"));
}

TEST(CplJsonObjectAdd, AddDuplicateKeyShouldReplace) {
  CPLJSONObject object;

  object.Add("key", 1);
  object.Add("key", 2);
  EXPECT_EQ(object.GetInteger("key"), 2);
}

TEST(CplJsonDocumentLoading, LoadMemoryCanLoadJsonFromStrings) {
  CPLJSONDocument document;

  EXPECT_TRUE(document.LoadMemory("true"));
  EXPECT_EQ(document.GetRoot().GetType(), CPLJSONObject::Type::Boolean);
  EXPECT_EQ(document.GetRoot().ToBool(), true);

  EXPECT_TRUE(document.LoadMemory("false"));
  EXPECT_EQ(document.GetRoot().GetType(), CPLJSONObject::Type::Boolean);
  EXPECT_EQ(document.GetRoot().ToBool(), false);
}

TEST(CplJsonDocumentLoading, LoadMemoryCannotLoadJsonFromEmptyOrNullStrings) {
  CPLJSONDocument document;

  EXPECT_FALSE(document.LoadMemory(CPLString()));
  EXPECT_FALSE(document.LoadMemory(std::string("")));
  EXPECT_FALSE(document.LoadMemory(nullptr, 0));
}

TEST(CplJsonDocumentLoading, LoadChunksErrorHandlingWhenFileMissing) {
  WithQuietHandler suppress_error_messages;

  CPLErrorReset();

  CPLJSONDocument document;
  EXPECT_FALSE(document.LoadChunks("/file/does/not/exist"));

  EXPECT_STREQ(CPLGetLastErrorMsg(), "Cannot open /file/does/not/exist");
  EXPECT_EQ(CPLGetLastErrorType(), CE_Failure);
  EXPECT_EQ(CPLGetLastErrorNo(), CPLE_FileIO);

  // Call again to cleanup
  CPLErrorReset();
}

void WriteFile(const char* contents, const char* path) {
  VSILFILE* f = VSIFOpenL(path, "wb");
  absl::Cleanup file_closer = [f] { VSIFCloseL(f); };

  size_t size = std::strlen(contents);
  ASSERT_EQ(VSIFWriteL(contents, 1, size, f), size);
}

constexpr char kInvalidJson[] = "some invalid json";

std::string GetValidJson() {
  const auto path =
      std::filesystem::path(SrcDir()) /
      "google3/third_party/gdal/autotest2/cpp/port/testdata/json/valid.json";

  absl::StatusOr<std::string> contents =
      file::GetContents(std::string(path), file::Defaults());
  CHECK_OK(contents);

  return *contents;
}

TEST(CplJsonDocumentLoading, LoadChunksErrorHandlingWhenInvalidJson) {
  WithQuietHandler suppress_error_messages;

  static constexpr char path[] = "/vsimem/invalid.json";
  WriteFile(kInvalidJson, path);
  absl::Cleanup remove_file = [] { VSIUnlink(path); };

  CPLErrorReset();

  CPLJSONDocument document;
  EXPECT_FALSE(document.LoadChunks(path));

  EXPECT_STREQ(CPLGetLastErrorMsg(), "JSON error: unexpected character");
  EXPECT_EQ(CPLGetLastErrorType(), CE_Failure);
  EXPECT_EQ(CPLGetLastErrorNo(), CPLE_AppDefined);

  // Call again to cleanup
  CPLErrorReset();
}

int load_chunks_test_sentinel = 0xabcd;
int load_chunks_test_progress_reporter_call_count = 0;

int LoadChunksTestProgressReporter(double fraction_complete,
                                   const char* message, void* arg) {
  EXPECT_STREQ(message, "Loading ...");
  EXPECT_EQ(arg, &load_chunks_test_sentinel);

  ++load_chunks_test_progress_reporter_call_count;

  // kFraction computed based on chunk / file size
  const double kFraction = 0.23094271538114569;
  EXPECT_DOUBLE_EQ(
      fraction_complete,
      std::min(kFraction * load_chunks_test_progress_reporter_call_count, 1.0));

  return 0;
}

TEST(CplJsonDocumentLoading, LoadChunksWorking) {
  static constexpr char path[] = "/vsimem/valid.json";
  WriteFile(GetValidJson().c_str(), path);
  absl::Cleanup remove_file = [] { VSIUnlink(path); };

  CPLErrorReset();

  CPLJSONDocument document;
  EXPECT_TRUE(
      document.LoadChunks(path, 512, LoadChunksTestProgressReporter,
                          static_cast<void*>(&load_chunks_test_sentinel)));
  EXPECT_EQ(CPLGetLastErrorType(), CE_None);
  EXPECT_EQ(load_chunks_test_progress_reporter_call_count, 5);

  // Test a few things to check that json was loaded correctly.
  CPLJSONObject root = document.GetRoot();
  EXPECT_TRUE(root.IsValid());
  EXPECT_EQ(root.GetInteger("resource/id", 10), 0);

  CPLJSONObject resource = root.GetObj("resource");
  EXPECT_TRUE(resource.IsValid());
  std::vector resource_children = resource.GetChildren();
  EXPECT_EQ(resource_children.size(), 11);

  CPLJSONArray scopes = root.GetArray("resource/scopes");
  EXPECT_TRUE(scopes.IsValid());
  EXPECT_EQ(scopes.Size(), 2);

  CPLJSONObject children = root.GetObj("resource/children");
  EXPECT_TRUE(children.IsValid());
  EXPECT_EQ(children.ToBool(), true);

  EXPECT_EQ(resource.GetBool("children", false), true);

  CPLJSONObject id = root["resource/owner_user/id"];
  EXPECT_TRUE(id.IsValid());

  // Call again to cleanup
  CPLErrorReset();
}

TEST(CplJsonDocumentLoading, LoadWhenFileMissing) {
  WithQuietHandler suppress_error_messages;

  CPLErrorReset();

  CPLJSONDocument document;
  EXPECT_FALSE(document.Load("/file/does/not/exist"));

  EXPECT_STREQ(CPLGetLastErrorMsg(),
               "Load json file /file/does/not/exist failed");
  EXPECT_EQ(CPLGetLastErrorType(), CE_Failure);
  EXPECT_EQ(CPLGetLastErrorNo(), CPLE_FileIO);

  // Call again to cleanup
  CPLErrorReset();
}

TEST(CplJsonDocumentLoading, LoadWhenInvalidJson) {
  WithQuietHandler suppress_error_messages;

  static constexpr char path[] = "/vsimem/invalid.json";
  WriteFile(kInvalidJson, path);
  absl::Cleanup remove_file = [] { VSIUnlink(path); };

  CPLErrorReset();

  CPLJSONDocument document;
  EXPECT_FALSE(document.Load(path));

  EXPECT_STREQ(CPLGetLastErrorMsg(),
               "JSON parsing error: unexpected character (at offset 0)");
  EXPECT_EQ(CPLGetLastErrorType(), CE_Failure);
  EXPECT_EQ(CPLGetLastErrorNo(), CPLE_AppDefined);

  // Call again to cleanup
  CPLErrorReset();
}

TEST(CplJsonDocumentLoading, LoadWorking) {
  static constexpr char path[] = "/vsimem/valid.json";
  WriteFile(GetValidJson().c_str(), path);
  absl::Cleanup remove_file = [] { VSIUnlink(path); };

  CPLErrorReset();

  CPLJSONDocument document;
  EXPECT_TRUE(document.Load(path));

  EXPECT_EQ(CPLGetLastErrorType(), CE_None);

  CPLJSONObject root = document.GetRoot();
  EXPECT_TRUE(root.IsValid());

  // Call again to cleanup
  CPLErrorReset();
}

TEST(CplJsonDocumentGetSetRoot, GetSetRootWorking) {
  CPLJSONObject object;
  object.Add("key", "value");

  CPLJSONDocument document;
  document.SetRoot(object);

  EXPECT_EQ(document.GetRoot().GetString("key", "default"), "value");
}

TEST(CplJsonDocumentConstructors, CopyConstructorWorking) {
  CPLJSONObject object;
  object.Add("key", "value");

  CPLJSONDocument document;
  document.SetRoot(object);
  ASSERT_EQ(document.GetRoot().GetString("key", "default"), "value");

  CPLJSONDocument document_copy(document);
  EXPECT_EQ(document_copy.GetRoot().GetString("key", "default"), "value");
}

TEST(CplJsonDocumentConstructors, CopyAssignmentWorking) {
  CPLJSONObject object;
  object.Add("key", "value");

  CPLJSONDocument document;
  document.SetRoot(object);

  ASSERT_EQ(document.GetRoot().GetString("key", "default"), "value");

  CPLJSONDocument document_copy;
  document_copy = document;
  EXPECT_EQ(document_copy.GetRoot().GetString("key", "default"), "value");
}

TEST(CplJsonDocumentConstructors, MoveConstructorWorking) {
  CPLJSONObject object;
  object.Add("key", "value");

  CPLJSONDocument document;
  document.SetRoot(object);
  ASSERT_EQ(document.GetRoot().GetString("key", "default"), "value");

  CPLJSONDocument document_moved(std::move(document));
  EXPECT_EQ(document_moved.GetRoot().GetString("key", "default"), "value");
}

TEST(CplJsonDocumentConstructors, MoveAssignmentWorking) {
  CPLJSONObject object;
  object.Add("key", "value");

  CPLJSONDocument document;
  document.SetRoot(object);

  ASSERT_EQ(document.GetRoot().GetString("key", "default"), "value");

  CPLJSONDocument document_moved;
  document_moved = std::move(document);
  EXPECT_EQ(document_moved.GetRoot().GetString("key", "default"), "value");
}

TEST(CplJsonObjectConstructors, CopyConstructorWorking) {
  CPLJSONObject object;
  object.Add("key", "value");
  ASSERT_EQ(object.GetString("key", "default"), "value");

  CPLJSONObject object_copy(object);
  EXPECT_EQ(object_copy.GetString("key", "default"), "value");
}

TEST(CplJsonObjectConstructors, CopyAssignmentWorking) {
  CPLJSONObject object;
  object.Add("key", "value");
  ASSERT_EQ(object.GetString("key", "default"), "value");

  CPLJSONObject object_copy;
  object_copy = object;
  EXPECT_EQ(object_copy.GetString("key", "default"), "value");
}

TEST(CplJsonObjectConstructors, MoveConstructorWorking) {
  CPLJSONObject object;
  object.Add("key", "value");
  ASSERT_EQ(object.GetString("key", "default"), "value");

  CPLJSONObject object_moved(std::move(object));
  EXPECT_EQ(object_moved.GetString("key", "default"), "value");
}

TEST(CplJsonObjectConstructors, MoveAssignmentWorking) {
  CPLJSONObject object;
  object.Add("key", "value");
  ASSERT_EQ(object.GetString("key", "default"), "value");

  CPLJSONObject object_moved;
  object_moved = std::move(object);
  EXPECT_EQ(object_moved.GetString("key", "default"), "value");
}

TEST(CplJsonDocumentSaving, SaveWhenPathMissing) {
  WithQuietHandler suppress_error_messages;

  CPLJSONDocument document;
  ASSERT_TRUE(document.LoadMemory("true"));

  CPLErrorReset();

  EXPECT_FALSE(document.Save("/file/does/not/exist"));

  EXPECT_STREQ(CPLGetLastErrorMsg(),
               "File /file/does/not/exist cannot be opened for writing");
  EXPECT_EQ(CPLGetLastErrorType(), CE_Failure);
  EXPECT_EQ(CPLGetLastErrorNo(), CPLE_NoWriteAccess);

  // Call again to cleanup
  CPLErrorReset();
}

TEST(CplJsonDocumentSaving, SaveWorks) {
  constexpr char kJsonContents[] = "true";

  CPLJSONDocument document;
  ASSERT_TRUE(document.LoadMemory(kJsonContents));

  CPLErrorReset();

  static constexpr char kPath[] = "/vsimem/temporary";
  bool result = document.Save(kPath);
  absl::Cleanup remove_file = [] { VSIUnlink(kPath); };
  EXPECT_TRUE(result);

  EXPECT_EQ(CPLGetLastErrorType(), CE_None);

  VSILFILE* f = VSIFOpenL(kPath, "r");
  absl::Cleanup file_closer = [f] { VSIFCloseL(f); };

  char buffer[5] = {0};
  ASSERT_EQ(VSIFReadL(buffer, 1, sizeof(buffer), f),
            std::strlen(kJsonContents));
  EXPECT_STREQ(buffer, kJsonContents);

  // Call again to cleanup
  CPLErrorReset();
}

TEST(CplJsonDocumentSaving, SaveAsStringWorks) {
  CPLJSONDocument document;
  ASSERT_TRUE(document.LoadMemory("true"));

  EXPECT_EQ(document.SaveAsString(), "true");
}

TEST(CplJsonStreamingWriter, ShouldBeginEmpty) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  EXPECT_EQ(writer.GetString(), "");
}

TEST(CplJsonStreamingWriter, CanWriteBasicString) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  writer.Add(std::string("basic"));
  EXPECT_EQ(writer.GetString(), R"json("basic")json");
}

TEST(CplJsonStreamingWriter, CanWriteBasicCharPtr) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  writer.Add("foobar");
  EXPECT_EQ(writer.GetString(), R"json("foobar")json");
}

TEST(CplJsonStreamingWriter, CanWriteNonSpecialCharacters) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  writer.Add("foo\\bar\"baz\b\f\n\r\t\x01voo");
  EXPECT_EQ(writer.GetString(),
            R"json("foo\\bar\"baz\b\f\n\r\t\u0001voo")json");
}

TEST(CplJsonStreamingWriter, CanWriteBoolTrue) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  writer.Add(true);
  EXPECT_EQ(writer.GetString(), "true");
}

TEST(CplJsonStreamingWriter, CanWriteBoolFalse) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  writer.Add(false);
  EXPECT_EQ(writer.GetString(), "false");
}

TEST(CplJsonStreamingWriter, CanWriteInt) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  writer.Add(1);
  EXPECT_EQ(writer.GetString(), "1");
}

TEST(CplJsonStreamingWriter, CanWriteUnsignedInt) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  unsigned int value = 4200000000;
  writer.Add(value);
  EXPECT_EQ(writer.GetString(), "4200000000");
}

TEST(CplJsonStreamingWriter, CanWriteInt64) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  std::int64_t value = -10000;
  writer.Add(value * 1000000);
  EXPECT_EQ(writer.GetString(), "-10000000000");
}

TEST(CplJsonStreamingWriter, CanWriteUnsignedInt64) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  std::uint64_t value = 10000;
  writer.Add(value * 1000000);
  EXPECT_EQ(writer.GetString(), "10000000000");
}

TEST(CplJsonStreamingWriter, CanWriteFloatDefaultPrecision) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  float value = 1.12345678901234567890;
  writer.Add(value);
  EXPECT_EQ(writer.GetString(), "1.12345684");
}

TEST(CplJsonStreamingWriter, CanWriteFloatCustomPrecision) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  float value = 1.12345678901234567890;
  writer.Add(value, 2);
  EXPECT_EQ(writer.GetString(), "1.1");
}

TEST(CplJsonStreamingWriter, CanWriteFloatNan) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  writer.Add(std::numeric_limits<float>::quiet_NaN());
  EXPECT_EQ(writer.GetString(), R"json("NaN")json");
}

TEST(CplJsonStreamingWriter, CanWriteFloatInfinity) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  writer.Add(std::numeric_limits<float>::infinity());
  EXPECT_EQ(writer.GetString(), R"json("Infinity")json");
}

TEST(CplJsonStreamingWriter, CanWriteFloatNegativeInfinity) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  writer.Add(-std::numeric_limits<float>::infinity());
  EXPECT_EQ(writer.GetString(), R"json("-Infinity")json");
}

TEST(CplJsonStreamingWriter, CanWriteDoubleDefaultPrecision) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  double value = 1.12345678901234567890;
  writer.Add(value);
  EXPECT_EQ(writer.GetString(), "1.1234567890123457");
}

TEST(CplJsonStreamingWriter, CanWriteDoubleCustomPrecision) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  double value = 1.12345678901234567890;
  writer.Add(value, 5);
  EXPECT_EQ(writer.GetString(), "1.1235");
}

TEST(CplJsonStreamingWriter, CanWriteDoubleNan) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  writer.Add(std::numeric_limits<double>::quiet_NaN());
  EXPECT_EQ(writer.GetString(), R"json("NaN")json");
}

TEST(CplJsonStreamingWriter, CanWriteDoubleInfinity) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  writer.Add(std::numeric_limits<double>::infinity());
  EXPECT_EQ(writer.GetString(), R"json("Infinity")json");
}

TEST(CplJsonStreamingWriter, CanWriteDoubleNegativeInfinity) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  writer.Add(-std::numeric_limits<double>::infinity());
  EXPECT_EQ(writer.GetString(), R"json("-Infinity")json");
}

TEST(CplJsonStreamingWriter, CanWriteNull) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  writer.AddNull();
  EXPECT_EQ(writer.GetString(), "null");
}

TEST(CplJsonStreamingWriter, CanWriteObjectStart) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  writer.StartObj();
  EXPECT_EQ(writer.GetString(), "{");
}

TEST(CplJsonStreamingWriter, CanWriteObjectStartEnd) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  writer.StartObj();
  writer.EndObj();
  EXPECT_EQ(writer.GetString(), "{}");
}

TEST(CplJsonStreamingWriter, CanWriteObject) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  writer.StartObj();
  writer.AddObjKey("key");
  writer.Add("value");
  writer.EndObj();
  EXPECT_EQ(writer.GetString(), R"json({
  "key": "value"
})json");
}

TEST(CplJsonStreamingWriter, CanWriteEmptyObjectWithContext) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  {
    CPLJSonStreamingWriter::ObjectContext context = writer.MakeObjectContext();
  }
  EXPECT_EQ(writer.GetString(), "{}");
}

TEST(CplJsonStreamingWriter, CanWriteWithoutPrettyFormatting) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  writer.SetPrettyFormatting(false);
  {
    CPLJSonStreamingWriter::ObjectContext context = writer.MakeObjectContext();
    writer.AddObjKey("key");
    writer.Add("value");
  }
  EXPECT_EQ(writer.GetString(), R"json({"key":"value"})json");
}

TEST(CplJsonStreamingWriter, CanWriteObjectWithContext) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  {
    CPLJSonStreamingWriter::ObjectContext context = writer.MakeObjectContext();
    writer.AddObjKey("key");
    writer.Add("value");
    writer.AddObjKey("key2");
    writer.Add("value2");
  }
  EXPECT_EQ(writer.GetString(), R"json({
  "key": "value",
  "key2": "value2"
})json");
}

TEST(CplJsonStreamingWriter, CanWriteNestedObjectWithContext) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  {
    CPLJSonStreamingWriter::ObjectContext context = writer.MakeObjectContext();
    writer.AddObjKey("key");
    writer.Add("value");
    writer.AddObjKey("nested");
    {
      CPLJSonStreamingWriter::ObjectContext context =
          writer.MakeObjectContext();
      writer.AddObjKey("key2");
      writer.Add("value2");
    }
  }
  EXPECT_EQ(writer.GetString(), R"json({
  "key": "value",
  "nested": {
    "key2": "value2"
  }
})json");
}

TEST(CplJsonStreamingWriter, CanWriteArrayStart) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  writer.StartArray();
  EXPECT_EQ(writer.GetString(), "[");
}

TEST(CplJsonStreamingWriter, CanWriteArrayStartEnd) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  writer.StartArray();
  writer.EndArray();
  EXPECT_EQ(writer.GetString(), "[]");
}

TEST(CplJsonStreamingWriter, CanWriteArray) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  writer.StartArray();
  writer.Add(1);
  writer.EndArray();
  EXPECT_EQ(writer.GetString(), R"json([
  1
])json");
}

TEST(CplJsonStreamingWriter, CanWriteArrayDisableNewline) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  writer.SetNewline(false);
  writer.StartArray();
  writer.Add(1);
  writer.EndArray();
  EXPECT_EQ(writer.GetString(), "[1]");
}

TEST(CplJsonStreamingWriter, CanWriteEmptyArrayWithContext) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  {
    CPLJSonStreamingWriter::ArrayContext context = writer.MakeArrayContext();
  }
  EXPECT_EQ(writer.GetString(), "[]");
}

TEST(CplJsonStreamingWriter, CanWriteArrayWithContext) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  {
    CPLJSonStreamingWriter::ArrayContext context = writer.MakeArrayContext();
    writer.Add(1);
    writer.Add(2);
  }
  EXPECT_EQ(writer.GetString(), R"json([
  1,
  2
])json");
}

TEST(CplJsonStreamingWriter, CanWriteArrayWithDifferentTypesUsingContext) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  {
    CPLJSonStreamingWriter::ArrayContext context = writer.MakeArrayContext();
    writer.Add(1);
    writer.Add("foo");
    writer.Add(true);
    writer.Add(3.5);
  }
  EXPECT_EQ(writer.GetString(), R"json([
  1,
  "foo",
  true,
  3.5
])json");
}

TEST(CplJsonStreamingWriter, CanWriteNestedArrayWithContext) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  {
    CPLJSonStreamingWriter::ArrayContext context = writer.MakeArrayContext();
    writer.Add(1);
    {
      CPLJSonStreamingWriter::ArrayContext context = writer.MakeArrayContext();
      writer.Add(true);
    }
  }
  EXPECT_EQ(writer.GetString(), R"json([
  1,
  [
    true
  ]
])json");
}

TEST(CplJsonStreamingWriter, CanWriteArraySingleLine) {
  CPLJSonStreamingWriter writer(nullptr, nullptr);
  {
    CPLJSonStreamingWriter::ArrayContext context =
        writer.MakeArrayContext(true);
    writer.Add(1);
    writer.Add(2);
  }
  EXPECT_EQ(writer.GetString(), "[1, 2]");
}

TEST(CplJsonStreamingWriter,
     SerializationFuncWritesToUserDataAndNotInternalString) {
  // This tests how CPLJSonStreamingWriter uses the provided serialization
  // function.  The test sets up a Serialize function, which should be used in
  // place of the default serialization function. The mock Serialize function
  // writes to a string serialization_destination, which should contain the
  // added string, instead of the default string managed by
  // CPLJSonStreamingWriter.
  std::string serialization_destination;

  struct Local {
    static void Serialize(const char *text, void *user_data) {
      *static_cast<std::string*>(user_data) += text;
    }
  };

  CPLJSonStreamingWriter writer(Local::Serialize,
                                static_cast<void*>(&serialization_destination));
  writer.Add(true);

  EXPECT_EQ(serialization_destination, "true");
  EXPECT_EQ(writer.GetString(), "");
}

class MockStreamingParser : public CPLJSonStreamingParser {
 public:
  MOCK_METHOD(void, String, (std::string_view), (override));
  MOCK_METHOD(void, Number, (std::string_view), (override));
  MOCK_METHOD(void, Boolean, (bool), (override));
  MOCK_METHOD(void, Null, (), (override));
  MOCK_METHOD(void, StartObject, (), (override));
  MOCK_METHOD(void, EndObject, (), (override));
  MOCK_METHOD(void, StartObjectMember, (std::string_view), (override));
  MOCK_METHOD(void, StartArray, (), (override));
  MOCK_METHOD(void, EndArray, (), (override));
  MOCK_METHOD(void, StartArrayMember, (), (override));
  MOCK_METHOD(void, Exception, (const char*), (override));
};

TEST(CplJsonStreamingParser, CanParseString) {
  MockStreamingParser parser;

  EXPECT_CALL(parser, String(::testing::StrEq("foo")));
  EXPECT_TRUE(parser.Parse(std::string_view(R"json("foo")json", 5), true));
  EXPECT_FALSE(parser.ExceptionOccurred());
}

TEST(CplJsonStreamingParser, CanParseNumber) {
  MockStreamingParser parser;

  EXPECT_CALL(parser, Number(::testing::StrEq("12345")));
  EXPECT_TRUE(parser.Parse(std::string_view("12345", 5), true));
  EXPECT_FALSE(parser.ExceptionOccurred());
}

TEST(CplJsonStreamingParser, CanParseInfinity) {
  MockStreamingParser parser;

  EXPECT_CALL(parser, Number(::testing::StrEq("infinity")));
  EXPECT_TRUE(parser.Parse(std::string_view("infinity", 8), true));
  EXPECT_FALSE(parser.ExceptionOccurred());
}

TEST(CplJsonStreamingParser, CanParseNegativeInfinity) {
  MockStreamingParser parser;

  EXPECT_CALL(parser, Number(::testing::StrEq("-infinity")));
  EXPECT_TRUE(parser.Parse(std::string_view("-infinity", 9), true));
  EXPECT_FALSE(parser.ExceptionOccurred());
}

TEST(CplJsonStreamingParser, CanParseNan) {
  MockStreamingParser parser;

  EXPECT_CALL(parser, Number(::testing::StrEq("nan")));
  EXPECT_TRUE(parser.Parse(std::string_view("nan", 3), true));
  EXPECT_FALSE(parser.ExceptionOccurred());
}

TEST(CplJsonStreamingParser, CanParseBool) {
  MockStreamingParser parser;

  EXPECT_CALL(parser, Boolean(true));
  EXPECT_TRUE(parser.Parse(std::string_view("true", 4), true));
  EXPECT_FALSE(parser.ExceptionOccurred());
}

TEST(CplJsonStreamingParser, CanParseNull) {
  MockStreamingParser parser;

  EXPECT_CALL(parser, Null());
  EXPECT_TRUE(parser.Parse(std::string_view("null", 4), true));
  EXPECT_FALSE(parser.ExceptionOccurred());
}

TEST(CplJsonStreamingParser, CanParseEmptyObject) {
  MockStreamingParser parser;

  {
    ::testing::InSequence s;

    EXPECT_CALL(parser, StartObject()).Times(1);
    EXPECT_CALL(parser, EndObject()).Times(1);
  }

  EXPECT_TRUE(parser.Parse(std::string_view("{}", 2), true));
  EXPECT_FALSE(parser.ExceptionOccurred());
}

TEST(CplJsonStreamingParser, CanParseObject) {
  MockStreamingParser parser;

  {
    ::testing::InSequence s;

    EXPECT_CALL(parser, StartObject()).Times(1);
    EXPECT_CALL(parser, StartObjectMember(::testing::StrEq("key"))).Times(1);
    EXPECT_CALL(parser, Number(::testing::StrEq("2"))).Times(1);
    EXPECT_CALL(parser, EndObject()).Times(1);
  }

  EXPECT_TRUE(parser.Parse(std::string_view(R"json({"key":2})json", 9), true));
  EXPECT_FALSE(parser.ExceptionOccurred());
}

TEST(CplJsonStreamingParser, CanParseNestedObject) {
  MockStreamingParser parser;

  {
    ::testing::InSequence s;

    EXPECT_CALL(parser, StartObject()).Times(1);
    EXPECT_CALL(parser, StartObjectMember(::testing::StrEq("key"))).Times(1);
    EXPECT_CALL(parser, StartObject()).Times(1);
    EXPECT_CALL(parser, StartObjectMember(::testing::StrEq("key2")))
        .Times(1);
    EXPECT_CALL(parser, Boolean(false)).Times(1);
    EXPECT_CALL(parser, EndObject()).Times(1);
    EXPECT_CALL(parser, EndObject()).Times(1);
  }

  EXPECT_TRUE(parser.Parse(
      std::string_view(R"json({"key":{"key2": false}})json", 23), true));
  EXPECT_FALSE(parser.ExceptionOccurred());
}

TEST(CplJsonStreamingParser, CanParseEmptyArray) {
  MockStreamingParser parser;

  {
    ::testing::InSequence s;

    EXPECT_CALL(parser, StartArray()).Times(1);
    EXPECT_CALL(parser, EndArray()).Times(1);
  }

  EXPECT_TRUE(parser.Parse(std::string_view("[]", 2), true));
  EXPECT_FALSE(parser.ExceptionOccurred());
}

TEST(CplJsonStreamingParser, CanParseArray) {
  MockStreamingParser parser;

  {
    ::testing::InSequence s;

    EXPECT_CALL(parser, StartArray()).Times(1);
    EXPECT_CALL(parser, StartArrayMember()).Times(1);
    EXPECT_CALL(parser, Boolean(true)).Times(1);
    EXPECT_CALL(parser, StartArrayMember()).Times(1);
    EXPECT_CALL(parser, Null()).Times(1);
    EXPECT_CALL(parser, EndArray()).Times(1);
  }

  EXPECT_TRUE(parser.Parse(std::string_view("[true, null]", 12), true));
  EXPECT_FALSE(parser.ExceptionOccurred());
}

TEST(CplJsonStreamingParser, CanParseNestedArray) {
  MockStreamingParser parser;

  {
    ::testing::InSequence s;

    EXPECT_CALL(parser, StartArray()).Times(1);
    EXPECT_CALL(parser, StartArrayMember()).Times(1);
    EXPECT_CALL(parser, Boolean(true)).Times(1);
    EXPECT_CALL(parser, StartArrayMember()).Times(1);
    EXPECT_CALL(parser, StartArray()).Times(1);
    EXPECT_CALL(parser, StartArrayMember()).Times(1);
    EXPECT_CALL(parser, Number(::testing::StrEq("1"))).Times(1);
    EXPECT_CALL(parser, StartArrayMember()).Times(1);
    EXPECT_CALL(parser, Number(::testing::StrEq("2"))).Times(1);
    EXPECT_CALL(parser, EndArray()).Times(1);
    EXPECT_CALL(parser, EndArray()).Times(1);
  }

  EXPECT_TRUE(parser.Parse(std::string_view("[true, [1, 2]]", 14), true));
  EXPECT_FALSE(parser.ExceptionOccurred());
}

struct CplJsonStreamingParserExceptionTestCase {
  const char* input;
  std::string expected_exception;
};

using CplJsonStreamingParserExceptionTest =
    testing::TestWithParam<CplJsonStreamingParserExceptionTestCase>;

TEST_P(CplJsonStreamingParserExceptionTest, TestCplJsonObjectFormatting) {
  const CplJsonStreamingParserExceptionTestCase& test_case = GetParam();

  MockStreamingParser parser;
  EXPECT_CALL(parser,
              Exception(::testing::HasSubstr(test_case.expected_exception)));
  EXPECT_FALSE(parser.Parse(
      std::string_view(test_case.input, std::strlen(test_case.input)), true));
  EXPECT_TRUE(parser.ExceptionOccurred());
}

INSTANTIATE_TEST_SUITE_P(
    CplJsonStreamingParserExceptionTestInstantiation,
    CplJsonStreamingParserExceptionTest,
    testing::ValuesIn<CplJsonStreamingParserExceptionTestCase>({
        {"tru", "Unexpected character"},
        {"tru1", "Unexpected character (1)"},
        {"truxe", "Unexpected character (x)"},
        {"truex", "Unexpected character (x)"},
        {"fals", "Unexpected character"},
        {"falsex", "Unexpected character (x)"},
        {"falsxe", "Unexpected character (x)"},
        {"nul", "Unexpected character"},
        {"nullx", "Unexpected character (x)"},
        {"nulxl", "Unexpected character (x)"},
        {"na", "Invalid number"},
        {"nanx", "Unexpected character (x)"},
        {"infinit", "Invalid number"},
        {"infinityx", "Unexpected character (x)"},
        {"true false", "Unexpected character (f)"},
        {"x", "Unexpected character (x)"},
        {"{", "Unterminated object"},
        {"}", "Unexpected character (})"},
        {"[", "Unterminated array"},
        {"]", "Unexpected character (])"},
        {"[1", "Unterminated array"},
        {"[,", "Unexpected character (,)"},
        {"[|", "Unexpected character (|)"},
        {"{ :", "Unexpected character (:)"},
        {"{ ,", "Unexpected character (,)"},
        {"{ |", "Unexpected character (|)"},
        {"{ 1", "Unexpected character (1)"},
        {R"json({"x")json", "Unterminated object"},
        {R"json({"x":)json", "Unterminated object"},
        {R"json({"x": 1 2)json", "Unexpected state"},
        {R"json({"x",)json", "Unexpected character (,)"},
        {R"json({"x"})json", "Missing value"},
        {R"json({"a" x})json", "Unexpected character (x)"},
        {"1x", "Unexpected character (x)"},
        {R"json(")json", "Unterminated string"},
        {R"json("\)json", "Unterminated string"},
        {R"json("\x)json", "Illegal escape sequence (\\x)"},
        {R"json("\u)json", "Unterminated string"},
        {R"json("\ux)json", "Illegal character in unicode sequence (\\x)"},
        {R"json("\u000)json", "Unterminated string"},
        {R"json("\uD834\ux)json",
         "Illegal character in unicode sequence (\\x)"},
        {R"json("\")json", "Unterminated string"},
        {"[,]", "Unexpected character (,)"},
        {"[true,]", "Missing value"},
        {"[true,,true]", "Unexpected character (,)"},
        {"[true true]", "Unexpected state"},
    }),
    [](const testing::TestParamInfo<
        CplJsonStreamingParserExceptionTest::ParamType>& info) {
      return absl::StrReplaceAll(info.param.input, {
                                                       {" ", "_"},
                                                       {"{", "open_brace"},
                                                       {"}", "close_brace"},
                                                       {"[", "open_bracket"},
                                                       {"]", "close_bracket"},
                                                       {",", "comma"},
                                                       {"|", "pipe"},
                                                       {":", "colon"},
                                                       {"\"", "double_quote"},
                                                       {"\\", "backslash"},
                                                   });
    });

TEST(CplJsonStreamingParser, ExceptionOnExceedingMaxStringSize) {
  MockStreamingParser parser;

  parser.SetMaxStringSize(2);

  EXPECT_CALL(parser, Exception(::testing::HasSubstr("Too many characters")));
  EXPECT_FALSE(
      parser.Parse(std::string_view(R"json("too long")json", 10), true));
  EXPECT_TRUE(parser.ExceptionOccurred());
}

TEST(CplJsonStreamingParser, ExceptionOnExceedingMaxDepthArray) {
  MockStreamingParser parser;

  parser.SetMaxDepth(1);

  EXPECT_CALL(
      parser,
      Exception(::testing::HasSubstr("Too many nested objects and/or arrays")));
  EXPECT_FALSE(parser.Parse(std::string_view("[[]]", 4), true));
  EXPECT_TRUE(parser.ExceptionOccurred());
}

TEST(CplJsonStreamingParser, ExceptionOnExceedingMaxDepthObject) {
  MockStreamingParser parser;

  parser.SetMaxDepth(1);

  EXPECT_CALL(
      parser,
      Exception(::testing::HasSubstr("Too many nested objects and/or arrays")));
  EXPECT_FALSE(
      parser.Parse(std::string_view(R"json({"key":{}})json", 10), true));
  EXPECT_TRUE(parser.ExceptionOccurred());
}

}  // namespace
}  // namespace autotest2
