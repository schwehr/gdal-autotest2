// Copyright 2024 Google LLC
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
// Test CPLJson related code
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_json.cpp

#include "third_party/gdal/port/cpl_json.h"

#include <features.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::SrcDir;

struct CplJsonObjectFormattingTestCase {
  std::string_view test_name;
  CPLJSONObject::PrettyFormat format;
  std::string_view expected;
};

using CplJsonObjectFormattingTest =
    testing::TestWithParam<CplJsonObjectFormattingTestCase>;

TEST_P(CplJsonObjectFormattingTest, TestCplJsonObjectFormatting) {
  const CplJsonObjectFormattingTestCase& test_case = GetParam();

  CPLJSONObject object;
  object.Set("foo", "bar");

  EXPECT_EQ(object.Format(test_case.format), test_case.expected);
}

INSTANTIATE_TEST_SUITE_P(
    CplJsonObjectFormattingTestSuiteInstantiation, CplJsonObjectFormattingTest,
    testing::ValuesIn<CplJsonObjectFormattingTestCase>({
        {.test_name = "Spaced",
         .format = CPLJSONObject::PrettyFormat::Spaced,
         .expected = R"json({ "foo": "bar" })json"},
        {.test_name = "Pretty",
         .format = CPLJSONObject::PrettyFormat::Pretty,
         .expected = R"json({
  "foo":"bar"
})json"},
        {.test_name = "Plain",
         .format = CPLJSONObject::PrettyFormat::Plain,
         .expected = R"json({"foo":"bar"})json"},
    }),
    [](const testing::TestParamInfo<CplJsonObjectFormattingTest::ParamType>&
           info) { return std::string(info.param.test_name); });

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
  constexpr GInt64 kBigValue = 12ULL << 30;
  constexpr GInt64 kTestDefaultBigValue = 99ULL << 20;
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

TEST(CplJsonObjectConversions, ToString) {
  CPLJSONObject object;
  object.Add("dbl", 0.123);
  object.Add("int", 324);
  object.Add("long", static_cast<GInt64>(23486));
  object.Add("bool_true", true);
  object.Add("bool_false", false);

  EXPECT_EQ("default", object.GetObj("nonexistent").ToString("default"));
  EXPECT_EQ("0.123", object.GetObj("dbl").ToString("default"));
  EXPECT_EQ("324", object.GetObj("int").ToString("default"));
  EXPECT_EQ("23486", object.GetObj("long").ToString("default"));
  EXPECT_EQ("true", object.GetObj("bool_true").ToString("default"));
  EXPECT_EQ("false", object.GetObj("bool_false").ToString("default"));
}

TEST(CplJsonObjectConversions, ToDouble) {
  constexpr double kDefault = 3.452;
  CPLJSONObject object;
  object.Add("str", "1234.33");
  object.Add("int", 2356);
  object.Add("long", static_cast<GInt64>(89754));
  object.Add("bool_false", false);
  object.Add("bool_true", true);

  EXPECT_DOUBLE_EQ(kDefault, object.GetObj("nonexistent").ToDouble(kDefault));
  EXPECT_DOUBLE_EQ(1234.33, object.GetObj("str").ToDouble(kDefault));
  EXPECT_DOUBLE_EQ(2356.0, object.GetObj("int").ToDouble(kDefault));
  EXPECT_DOUBLE_EQ(89754.0, object.GetObj("long").ToDouble(kDefault));
  EXPECT_DOUBLE_EQ(0.0, object.GetObj("bool_false").ToDouble(kDefault));
  EXPECT_DOUBLE_EQ(1.0, object.GetObj("bool_true").ToDouble(kDefault));
}

TEST(CplJsonObjectConversions, ToInteger) {
  // Doubles are truncated toward zero (never rounded) when converted to int.
  constexpr int kDefault = 3245;
  CPLJSONObject object;
  object.Add("pos_high", 245.88);
  object.Add("pos_low", 33.4);
  object.Add("neg_low", -33.4);
  object.Add("neg_high", -33.8);
  object.Add("long", static_cast<GInt64>(1238473));
  object.Add("str", "14323");
  object.Add("bool_false", false);
  object.Add("bool_true", true);

  EXPECT_EQ(kDefault, object.GetObj("nonexistent").ToInteger(kDefault));
  EXPECT_EQ(0, object.GetObj("nonexistent").ToInteger());
  EXPECT_EQ(245, object.GetObj("pos_high").ToInteger(22));
  EXPECT_EQ(33, object.GetObj("pos_low").ToInteger(22));
  EXPECT_EQ(-33, object.GetObj("neg_low").ToInteger(22));
  EXPECT_EQ(-33, object.GetObj("neg_high").ToInteger(22));
  EXPECT_EQ(1238473, object.GetObj("long").ToInteger(22));
  EXPECT_EQ(14323, object.GetObj("str").ToInteger(22));
  EXPECT_EQ(0, object.GetObj("bool_false").ToInteger(22));
  EXPECT_EQ(1, object.GetObj("bool_true").ToInteger(22));
}

TEST(CplJsonObjectConversions, ToLong) {
  // Doubles are truncated toward zero (never rounded) when converted to GInt64.
  CPLJSONObject object;
  object.Add("pos_high", 1234.94235);
  object.Add("pos_low", 1234.335);
  object.Add("neg_low", -53456.1234);
  object.Add("neg_high", -53456.6554);
  object.Add("int", 846735);
  object.Add("str", "3452345");
  object.Add("bool_false", false);
  object.Add("bool_true", true);

  EXPECT_EQ(9817254, object.GetObj("nonexistent").ToLong(9817254));
  EXPECT_EQ(static_cast<GInt64>(0), object.GetObj("nonexistent").ToLong());
  EXPECT_EQ(1234, object.GetObj("pos_high").ToLong(22));
  EXPECT_EQ(1234, object.GetObj("pos_low").ToLong(22));
  EXPECT_EQ(-53456, object.GetObj("neg_low").ToLong(22));
  EXPECT_EQ(-53456, object.GetObj("neg_high").ToLong(22));
  EXPECT_EQ(static_cast<GInt64>(846735), object.GetObj("int").ToLong(22));
  EXPECT_EQ(static_cast<GInt64>(3452345), object.GetObj("str").ToLong(22));
  EXPECT_EQ(0, object.GetObj("bool_false").ToLong(22));
  EXPECT_EQ(1, object.GetObj("bool_true").ToLong(22));
}

TEST(CplJsonObjectConversions, ToBool) {
  // Only the empty string and zero numeric values convert to false.
  CPLJSONObject object;
  object.Add("str_nonempty", "foo");
  object.Add("str_empty", "");
  object.Add("dbl_nonzero", 0.1);
  object.Add("dbl_zero", 0.0);
  object.Add("int_nonzero", 1);
  object.Add("int_zero", 0);
  object.Add("long_nonzero", static_cast<GInt64>(12ULL << 30));
  object.Add("long_zero", static_cast<GInt64>(0));

  EXPECT_TRUE(object.GetObj("nonexistent").ToBool(true));
  EXPECT_FALSE(object.GetObj("nonexistent").ToBool(false));
  EXPECT_TRUE(object.GetObj("str_nonempty").ToBool(false));
  EXPECT_FALSE(object.GetObj("str_empty").ToBool(false));
  EXPECT_TRUE(object.GetObj("dbl_nonzero").ToBool(false));
  EXPECT_FALSE(object.GetObj("dbl_zero").ToBool(false));
  EXPECT_TRUE(object.GetObj("int_nonzero").ToBool(false));
  EXPECT_FALSE(object.GetObj("int_zero").ToBool(false));
  EXPECT_TRUE(object.GetObj("long_nonzero").ToBool(false));
  EXPECT_FALSE(object.GetObj("long_zero").ToBool(false));
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

constexpr char kInvalidJson[] = "some invalid json";

std::string GetValidJson() {
  const auto path =
      std::filesystem::path(SrcDir()) /
      "google3/third_party/gdal/autotest2/cpp/port/testdata/json/valid.json";

  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

TEST(CplJsonDocumentLoading, LoadChunksErrorHandlingWhenInvalidJson) {
  WithQuietHandler suppress_error_messages;

  constexpr char kPath[] = "/vsimem/invalid.json";
  VsiMemTempWrapper temp_file(kPath, kInvalidJson);

  CPLErrorReset();

  CPLJSONDocument document;
  EXPECT_FALSE(document.LoadChunks(kPath));

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
  constexpr double kFraction = 0.23094271538114569;
  EXPECT_DOUBLE_EQ(
      fraction_complete,
      std::min(kFraction * load_chunks_test_progress_reporter_call_count, 1.0));

  return 0;
}

TEST(CplJsonDocumentLoading, LoadChunksWorking) {
  constexpr char kPath[] = "/vsimem/valid.json";
  const std::string valid_json = GetValidJson();
  ASSERT_FALSE(valid_json.empty());
  VsiMemTempWrapper temp_file(kPath, valid_json);

  load_chunks_test_progress_reporter_call_count = 0;
  CPLErrorReset();

  CPLJSONDocument document;
  EXPECT_TRUE(
      document.LoadChunks(kPath, 512, LoadChunksTestProgressReporter,
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

  constexpr char kPath[] = "/vsimem/invalid.json";
  VsiMemTempWrapper temp_file(kPath, kInvalidJson);

  CPLErrorReset();

  CPLJSONDocument document;
  EXPECT_FALSE(document.Load(kPath));

  EXPECT_STREQ(CPLGetLastErrorMsg(),
               "JSON parsing error: unexpected character (at offset 0)");
  EXPECT_EQ(CPLGetLastErrorType(), CE_Failure);
  EXPECT_EQ(CPLGetLastErrorNo(), CPLE_AppDefined);

  // Call again to cleanup
  CPLErrorReset();
}

TEST(CplJsonDocumentLoading, LoadWorking) {
  constexpr char kPath[] = "/vsimem/valid.json";
  const std::string valid_json = GetValidJson();
  ASSERT_FALSE(valid_json.empty());
  VsiMemTempWrapper temp_file(kPath, valid_json);

  CPLErrorReset();

  CPLJSONDocument document;
  EXPECT_TRUE(document.Load(kPath));

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
  ASSERT_NE(nullptr, f);
  absl::Cleanup file_closer = [f] { EXPECT_EQ(0, VSIFCloseL(f)); };

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

TEST(CplJsonCoverageTest, DocumentEdgeCasesAndLoadUrl) {
  WithQuietHandler quiet;

  // Empty document GetRoot creates new object root
  CPLJSONDocument empty_doc;
  CPLJSONObject root = empty_doc.GetRoot();
  EXPECT_TRUE(root.IsValid());
  root.Add("a", 1);

  // Const GetRoot
  const CPLJSONDocument& const_doc = empty_doc;
  EXPECT_EQ(1, const_doc.GetRoot().GetInteger("a"));

  // Self copy and move assignment, and non-null destination assignment
  CPLJSONDocument doc2;
  ASSERT_TRUE(doc2.LoadMemory("{\"b\":2}"));
  doc2 = empty_doc;
  doc2 = *&doc2;
  EXPECT_EQ(1, doc2.GetRoot().GetInteger("a"));

  CPLJSONDocument doc3;
  ASSERT_TRUE(doc3.LoadMemory("{\"c\":3}"));
  doc3 = std::move(doc2);
  doc3 = std::move(*&doc3);
  EXPECT_EQ(1, doc3.GetRoot().GetInteger("a"));

  // SetRoot when m_poRootJsonObject is already non-null replaces the root.
  doc3.SetRoot(CPLJSONObject("new_root_str"));
  EXPECT_EQ("new_root_str", doc3.GetRoot().ToString());

  // GetRoot when root is an array
  CPLJSONDocument arr_doc;
  ASSERT_TRUE(arr_doc.LoadMemory("[1, 2, 3]"));
  CPLJSONObject arr_root = arr_doc.GetRoot();
  EXPECT_EQ(CPLJSONObject::Type::Array, arr_root.GetType());

  // CPL_JSON_MAX_SIZE <= 0 makes Load() fail early, even for a file that
  // loads fine otherwise.
  {
    constexpr char kPath[] = "/vsimem/document_edge_cases.json";
    VsiMemTempWrapper temp_file(kPath, "{\"d\": 4}");
    CPLJSONDocument max_size_doc;
    EXPECT_TRUE(max_size_doc.Load(kPath));
    EXPECT_EQ(4, max_size_doc.GetRoot().GetInteger("d"));
    CPLConfigOptionSetter max_size("CPL_JSON_MAX_SIZE", "0",
                                   /*bSetOnlyIfUndefined=*/false);
    EXPECT_FALSE(max_size_doc.Load(kPath));
  }

#ifndef HAVE_CURL
  // port_lib is built without curl, so LoadUrl() is a stub that always fails.
  // Guarded so that this never becomes a real network request if curl support
  // is enabled in the build.
  EXPECT_FALSE(arr_doc.LoadUrl("http://example.com/test.json", nullptr));
#endif
}

TEST(CplJsonCoverageTest, ObjectConstructorsCloneTypesSubscriptAndArrays) {
  WithQuietHandler quiet;

  // Value constructors and GetType()
  CPLJSONObject o_null(nullptr);
  EXPECT_EQ(CPLJSONObject::Type::Null, o_null.GetType());

  CPLJSONObject o_str(std::string("hello"));
  EXPECT_EQ(CPLJSONObject::Type::String, o_str.GetType());

  CPLJSONObject o_cstr("world");
  EXPECT_EQ(CPLJSONObject::Type::String, o_cstr.GetType());

  CPLJSONObject o_bool(true);
  EXPECT_EQ(CPLJSONObject::Type::Boolean, o_bool.GetType());

  CPLJSONObject o_int(42);
  EXPECT_EQ(CPLJSONObject::Type::Integer, o_int.GetType());

  CPLJSONObject o_int64(static_cast<int64_t>(1LL << 40));
  EXPECT_EQ(CPLJSONObject::Type::Long, o_int64.GetType());

  CPLJSONObject o_uint64(static_cast<uint64_t>(1ULL << 40));
  EXPECT_EQ(CPLJSONObject::Type::Long, o_uint64.GetType());

  CPLJSONObject o_dbl(3.14);
  EXPECT_EQ(CPLJSONObject::Type::Double, o_dbl.GetType());

  // Clone and Deinit
  CPLJSONObject obj;
  obj.Add("s", std::string("val"));
  obj.Add("null_cstr", static_cast<const char*>(nullptr));
  obj.Add("u64", static_cast<uint64_t>(999));
  obj.Add("", CPLJSONObject("empty_name"));
  obj.SetNull("explicit_null");
  EXPECT_EQ(CPLJSONObject::Type::Object, obj.GetType());

  CPLJSONObject cloned = obj.Clone();
  EXPECT_EQ("val", cloned.GetString("s"));

  // Self-assignment on CPLJSONObject
  cloned = *&cloned;
  cloned = std::move(*&cloned);

  // Const operator[]
  const CPLJSONObject& const_obj = obj;
  EXPECT_EQ("val", const_obj["s"].ToString());

  // Non-const operator[] with non-existing key (m_osKeyForSet path)
  auto unset_proxy = obj["nonexistent_proxy"];
  EXPECT_FALSE(unset_proxy.IsValid());
  EXPECT_EQ("def", unset_proxy.GetString("k", "def"));
  EXPECT_EQ("def", unset_proxy.ToString("def"));
  EXPECT_DOUBLE_EQ(1.5, unset_proxy.GetDouble("k", 1.5));
  EXPECT_DOUBLE_EQ(1.5, unset_proxy.ToDouble(1.5));
  EXPECT_EQ(7, unset_proxy.GetInteger("k", 7));
  EXPECT_EQ(7, unset_proxy.ToInteger(7));
  EXPECT_EQ(8, unset_proxy.GetLong("k", 8));
  EXPECT_EQ(8, unset_proxy.ToLong(8));
  EXPECT_TRUE(unset_proxy.GetBool("k", true));
  EXPECT_TRUE(unset_proxy.ToBool(true));
  EXPECT_TRUE(unset_proxy.GetChildren().empty());
  EXPECT_FALSE(unset_proxy.ToArray().IsValid());
  EXPECT_EQ(CPLJSONObject::Type::Unknown, unset_proxy.GetType());

  // Assigning to operator[] via copy, move, and CPLJSONArray move
  obj["assigned_copy"] = o_str;
  EXPECT_EQ("hello", obj.GetString("assigned_copy"));
  obj["assigned_move"] = CPLJSONObject("moved_str");
  EXPECT_EQ("moved_str", obj.GetString("assigned_move"));
  obj["assigned_arr"] = CPLJSONArray();
  EXPECT_EQ(CPLJSONObject::Type::Array, obj["assigned_arr"].GetType());

  // CPLJSONArray constructors and Add overloads
  CPLJSONArray arr;
  CPLJSONArray named_arr("named");
  arr.AddNull();
  arr.Add(CPLJSONObject("elem_obj"));
  arr.Add(std::string("std_str"));
  arr.Add("c_str");
  arr.Add(static_cast<const char*>(nullptr));
  arr.Add(2.5);
  arr.Add(10);
  arr.Add(static_cast<GInt64>(100));
  arr.Add(static_cast<uint64_t>(200));
  arr.Add(true);
  EXPECT_EQ(9, arr.Size());

  obj.Add("my_arr", arr);
  CPLJSONArray fetched_arr = obj.GetArray("my_arr");
  EXPECT_TRUE(fetched_arr.IsValid());
  EXPECT_EQ(9, fetched_arr.Size());
  EXPECT_EQ("std_str", fetched_arr[2].ToString());
  const CPLJSONArray& const_arr = fetched_arr;
  EXPECT_EQ("c_str", const_arr[3].ToString());

  CPLJSONArray from_obj(obj["my_arr"]);
  EXPECT_EQ(9, from_obj.Size());
  EXPECT_TRUE(obj["my_arr"].ToArray().IsValid());
  EXPECT_FALSE(obj.ToArray().IsValid());
  EXPECT_FALSE(obj.GetArray("s").IsValid());
  EXPECT_FALSE(obj.GetArray("missing_arr").IsValid());

  // GetObjectByPath edge cases: > 100 components, empty path, and a
  // non-object intermediate.
  std::string deep_path;
  for (int i = 0; i < 102; ++i) deep_path += "a/";
  deep_path += 'b';
  EXPECT_FALSE(obj.GetObj(deep_path).IsValid());
  EXPECT_FALSE(obj.GetObj("/").IsValid());
  EXPECT_FALSE(o_int.GetObj("a/b").IsValid());
  EXPECT_TRUE(o_int.GetChildren().empty());

  // Deinit and operations on invalid object (m_osKey == INVALID_OBJ_KEY)
  CPLJSONObject deinit_obj;
  deinit_obj.Deinit();
  EXPECT_FALSE(deinit_obj.IsValid());
  EXPECT_EQ(CPLJSONObject::Type::Unknown, deinit_obj.GetType());
  EXPECT_EQ("", deinit_obj.Format(CPLJSONObject::PrettyFormat::Plain));
  EXPECT_TRUE(deinit_obj.Clone().IsValid());

  CPLJSONArray deinit_arr;
  deinit_arr.Deinit();
  EXPECT_EQ(0, deinit_arr.Size());

  // CPLParseKeyValueJson
  EXPECT_TRUE(CPLParseKeyValueJson(nullptr).empty());
  CPLStringList kv = CPLParseKeyValueJson(
      R"json({"k1":"v1","k2":42,"k3":1.5,"ignored":[1,2]})json");
  EXPECT_STREQ("v1", kv.FetchNameValue("k1"));
  EXPECT_STREQ("42", kv.FetchNameValue("k2"));
}

}  // namespace
}  // namespace autotest2
