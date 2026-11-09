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
// Unit tests for third_party/hdf5/c++/src/H5Exception.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5Exception.cpp

#include <cstdio>
#include <filesystem>  // NOLINT
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;

struct WalkRecord {
  unsigned count = 0;
  std::vector<hid_t> major_ids;
  std::vector<hid_t> minor_ids;
};

herr_t DummyAutoPrint(hid_t /*err_stack*/, void* /*client_data*/) { return 0; }

herr_t RecordErrorWalk(unsigned /*n*/, const H5E_error2_t* err_desc,
                       void* client_data) {
  auto* record = static_cast<WalkRecord*>(client_data);
  if (record == nullptr || err_desc == nullptr) {
    return -1;
  }
  ++record->count;
  record->major_ids.push_back(err_desc->maj_num);
  record->minor_ids.push_back(err_desc->min_num);
  return 0;
}

TEST(H5ExceptionTest, DefaultOverloadedAndCopyConstructors) {
  const H5::Exception empty;
  EXPECT_EQ("", empty.getDetailMsg());
  EXPECT_STREQ("", empty.getCDetailMsg());
  EXPECT_EQ("", empty.getFuncName());
  EXPECT_STREQ("", empty.getCFuncName());

  const H5::Exception with_default_msg("TestFunc");
  EXPECT_EQ("TestFunc", with_default_msg.getFuncName());
  EXPECT_STREQ("TestFunc", with_default_msg.getCFuncName());
  EXPECT_EQ("No detailed information provided",
            with_default_msg.getDetailMsg());

  const H5::Exception detailed("TestFunc2", "custom failure");
  EXPECT_EQ("TestFunc2", detailed.getFuncName());
  EXPECT_STREQ("TestFunc2", detailed.getCFuncName());
  EXPECT_EQ("custom failure", detailed.getDetailMsg());
  EXPECT_STREQ("custom failure", detailed.getCDetailMsg());

  const H5::Exception copied(detailed);
  EXPECT_EQ("TestFunc2", copied.getFuncName());
  EXPECT_EQ("custom failure", copied.getDetailMsg());
}

TEST(H5ExceptionTest, DerivedExceptionSubclassesConstructCleanly) {
  const auto check_pair = [](const H5::Exception& def_ex,
                             const H5::Exception& msg_ex) {
    EXPECT_EQ("", def_ex.getFuncName());
    EXPECT_EQ("op", msg_ex.getFuncName());
    EXPECT_EQ("msg", msg_ex.getDetailMsg());
  };

  check_pair(H5::FileIException(), H5::FileIException("op", "msg"));
  check_pair(H5::GroupIException(), H5::GroupIException("op", "msg"));
  check_pair(H5::DataSpaceIException(), H5::DataSpaceIException("op", "msg"));
  check_pair(H5::DataTypeIException(), H5::DataTypeIException("op", "msg"));
  check_pair(H5::ObjHeaderIException(), H5::ObjHeaderIException("op", "msg"));
  check_pair(H5::PropListIException(), H5::PropListIException("op", "msg"));
  check_pair(H5::DataSetIException(), H5::DataSetIException("op", "msg"));
  check_pair(H5::AttributeIException(), H5::AttributeIException("op", "msg"));
  check_pair(H5::ReferenceException(), H5::ReferenceException("op", "msg"));
  check_pair(H5::LibraryIException(), H5::LibraryIException("op", "msg"));
  check_pair(H5::IdComponentException(), H5::IdComponentException("op", "msg"));
  check_pair(H5::LocationException(), H5::LocationException("op", "msg"));
}

TEST(H5ExceptionTest, WalkErrorStackAndResolveMajorMinorStrings) {
  ScopedQuietHdf5Errors quiet;

  H5::Exception::clearErrorStack();
  // Trigger a C-library error frame so H5E_DEFAULT contains valid major/minor
  // message IDs to walk and stringify.
  EXPECT_THROW(H5::DataSpace(static_cast<H5S_class_t>(-1)),
               H5::DataSpaceIException);

  WalkRecord downward;
  H5::Exception::walkErrorStack(H5E_WALK_DOWNWARD, RecordErrorWalk, &downward);
  ASSERT_GT(downward.count, 0u);
  ASSERT_FALSE(downward.major_ids.empty());
  ASSERT_FALSE(downward.minor_ids.empty());

  WalkRecord upward;
  H5::Exception::walkErrorStack(H5E_WALK_UPWARD, RecordErrorWalk, &upward);
  EXPECT_EQ(downward.count, upward.count);

  const H5::Exception helper;
  EXPECT_THAT(helper.getMajorString(downward.major_ids.front()),
              Not(IsEmpty()));
  EXPECT_THAT(helper.getMinorString(downward.minor_ids.front()),
              Not(IsEmpty()));

  EXPECT_THROW(helper.getMajorString(H5I_INVALID_HID),
               H5::IdComponentException);
  EXPECT_THROW(helper.getMinorString(H5I_INVALID_HID),
               H5::IdComponentException);
  // Returning negative from the H5E_walk2_t callback causes H5Ewalk2 to fail
  // cleanly without triggering UBSan enum checks on H5E_direction_t.
  EXPECT_THROW(H5::Exception::walkErrorStack(H5E_WALK_DOWNWARD, RecordErrorWalk,
                                             nullptr),
               H5::Exception);

  H5::Exception::clearErrorStack();
  WalkRecord after_clear;
  H5::Exception::walkErrorStack(H5E_WALK_DOWNWARD, RecordErrorWalk,
                                &after_clear);
  EXPECT_EQ(0u, after_clear.count);
}

TEST(H5ExceptionTest, PrintErrorStackAndAutoPrintRoundTrip) {
  ScopedQuietHdf5Errors quiet;

  H5E_auto2_t saved_func = nullptr;
  void* saved_data = nullptr;
  H5::Exception::getAutoPrint(saved_func, &saved_data);
  EXPECT_EQ(nullptr, saved_func);

  H5E_auto2_t custom_func = &DummyAutoPrint;
  H5::Exception::setAutoPrint(custom_func, nullptr);
  H5E_auto2_t round_trip_func = nullptr;
  void* round_trip_data = &round_trip_func;
  H5::Exception::getAutoPrint(round_trip_func, &round_trip_data);
  EXPECT_EQ(&DummyAutoPrint, round_trip_func);

  H5::Exception::dontPrint();
  round_trip_func = &DummyAutoPrint;
  H5::Exception::getAutoPrint(round_trip_func, &round_trip_data);
  EXPECT_EQ(nullptr, round_trip_func);

  // Populate the error stack and write it to a private temp file stream.
  EXPECT_THROW(H5::DataSpace(static_cast<H5S_class_t>(-1)),
               H5::DataSpaceIException);
  TempFile stack_out(".txt");
  std::FILE* fp = std::fopen(stack_out.c_str(), "w+");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup close_fp = [&fp]() {
    if (fp != nullptr) {
      std::fclose(fp);
    }
  };
  H5::Exception::printErrorStack(fp, H5E_DEFAULT);
  std::fclose(fp);
  fp = nullptr;

  std::ifstream in(stack_out.path());
  const std::string contents((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
  EXPECT_THAT(contents, HasSubstr("H5Screate"));

  // Invalid error stack ID causes H5Eprint2 to fail and throw Exception.
  EXPECT_THROW(H5::Exception::printErrorStack(stderr, H5I_INVALID_HID),
               H5::Exception);
}

}  // namespace
}  // namespace hdf5
