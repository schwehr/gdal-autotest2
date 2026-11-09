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
// Unit tests for third_party/hdf5/c++/src/H5Object.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5Object.cpp

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

void RecordAttrName(H5::H5Object& /*loc*/, const H5std_string attr_name,
                    void* op_data) {
  auto* names = static_cast<std::vector<std::string>*>(op_data);
  names->push_back(attr_name);
}

int VisitRecordNames(H5::H5Object& /*obj*/, const H5std_string name,
                     const H5O_info2_t* /*oinfo*/, void* op_data) {
  auto* names = static_cast<std::vector<std::string>*>(op_data);
  names->push_back(name);
  return 0;
}

int VisitEarlyStop(H5::H5Object& /*obj*/, const H5std_string /*name*/,
                   const H5O_info2_t* /*oinfo*/, void* op_data) {
  auto* count = static_cast<int*>(op_data);
  ++(*count);
  return 1;
}

int VisitFail(H5::H5Object& /*obj*/, const H5std_string /*name*/,
              const H5O_info2_t* /*oinfo*/, void* /*op_data*/) {
  return -1;
}

TEST(H5ObjectTest, AttributesIterationVisitVersionAndObjName) {
  H5::H5File file = CreateInMemoryFile();
  H5::Group grp = file.createGroup("grp");
  H5::DataSpace scalar(H5S_SCALAR);
  H5::DataSet ds =
      grp.createDataSet("dataset", H5::PredType::NATIVE_INT32, scalar);

  const int32_t v1 = 10;
  {
    H5::Attribute a1 =
        ds.createAttribute("alpha", H5::PredType::NATIVE_INT32, scalar);
    a1.write(H5::PredType::NATIVE_INT32, &v1);
  }

  const int32_t v2 = 20;
  {
    H5::Attribute a2 = ds.createAttribute(H5std_string("beta"),
                                          H5::PredType::NATIVE_INT32, scalar);
    a2.write(H5::PredType::NATIVE_INT32, &v2);
  }

  EXPECT_EQ(ds.getNumAttrs(), 2);
  EXPECT_TRUE(ds.attrExists("alpha"));
  EXPECT_TRUE(ds.attrExists(H5std_string("beta")));
  EXPECT_FALSE(ds.attrExists("gamma"));

  EXPECT_EQ(ds.openAttribute("alpha").getName(), "alpha");
  EXPECT_EQ(ds.openAttribute(H5std_string("beta")).getName(), "beta");
  EXPECT_EQ(ds.openAttribute(0u).getName(), "alpha");

  std::vector<std::string> iter_names;
  unsigned start_idx = 0;
  EXPECT_EQ(ds.iterateAttrs(RecordAttrName, &start_idx, &iter_names), 0);
  EXPECT_THAT(iter_names, ElementsAre("alpha", "beta"));
  EXPECT_EQ(start_idx, 2u);

  iter_names.clear();
  EXPECT_EQ(ds.iterateAttrs(RecordAttrName, nullptr, &iter_names), 0);
  EXPECT_THAT(iter_names, ElementsAre("alpha", "beta"));

  ds.renameAttr("alpha", "alpha_new");
  EXPECT_TRUE(ds.attrExists("alpha_new"));
  EXPECT_FALSE(ds.attrExists("alpha"));

  ds.renameAttr(H5std_string("beta"), H5std_string("beta_new"));
  EXPECT_TRUE(ds.attrExists("beta_new"));

  ds.removeAttr("alpha_new");
  EXPECT_EQ(ds.getNumAttrs(), 1);
  ds.removeAttr(H5std_string("beta_new"));
  EXPECT_EQ(ds.getNumAttrs(), 0);

  std::vector<std::string> visited;
  grp.visit(H5_INDEX_NAME, H5_ITER_INC, VisitRecordNames, &visited,
            H5O_INFO_BASIC);
  EXPECT_THAT(visited, ElementsAre(".", "dataset"));

  int stop_calls = 0;
  grp.visit(H5_INDEX_NAME, H5_ITER_INC, VisitEarlyStop, &stop_calls,
            H5O_INFO_BASIC);
  EXPECT_EQ(stop_calls, 1);

  const unsigned ver = ds.objVersion();
  EXPECT_TRUE(ver == H5O_VERSION_1 || ver == H5O_VERSION_2);

  // Exercise all three getObjName overloads.
  EXPECT_EQ(ds.getObjName(), "/grp/dataset");
  H5std_string full_name;
  EXPECT_EQ(ds.getObjName(full_name), 12);
  EXPECT_EQ(full_name, "/grp/dataset");

  H5std_string prefix_name;
  EXPECT_EQ(ds.getObjName(prefix_name, 4), 12);
  EXPECT_EQ(prefix_name, "/grp");

  std::array<char, 32> name_buf{};
  EXPECT_EQ(ds.getObjName(name_buf.data(), name_buf.size()), 12);
  EXPECT_STREQ(name_buf.data(), "/grp/dataset");
}

TEST(H5ObjectTest, ClosedAndUnnamedObjectErrors) {
  H5::H5File file = CreateInMemoryFile();
  H5::Group grp = file.createGroup("err_grp");
  H5::DataSpace scalar(H5S_SCALAR);

  ScopedQuietHdf5Errors quiet;
  H5::DataSet closed;
  EXPECT_THROW(closed.createAttribute("a", H5::PredType::NATIVE_INT32, scalar),
               H5::AttributeIException);
  EXPECT_THROW(closed.openAttribute("a"), H5::AttributeIException);
  EXPECT_THROW(closed.openAttribute(0u), H5::AttributeIException);
  EXPECT_THROW(closed.iterateAttrs(RecordAttrName, nullptr, nullptr),
               H5::AttributeIException);
  EXPECT_THROW(closed.visit(H5_INDEX_NAME, H5_ITER_INC, VisitRecordNames,
                            nullptr, H5O_INFO_BASIC),
               H5::Exception);
  EXPECT_THROW(
      grp.visit(H5_INDEX_NAME, H5_ITER_INC, VisitFail, nullptr, H5O_INFO_BASIC),
      H5::Exception);
  EXPECT_THROW(closed.objVersion(), H5::Exception);
  EXPECT_THROW(closed.getNumAttrs(), H5::AttributeIException);
  EXPECT_THROW(closed.attrExists("a"), H5::AttributeIException);
  EXPECT_THROW(closed.removeAttr("a"), H5::AttributeIException);
  EXPECT_THROW(closed.renameAttr("a", "b"), H5::AttributeIException);

  std::array<char, 16> buf{};
  EXPECT_THROW(closed.getObjName(buf.data(), buf.size()), H5::Exception);
  EXPECT_THROW(closed.getObjName(), H5::Exception);

  // Uncommitted transient datatype has a valid ID whose name length is 0.
  H5::IntType transient(H5::PredType::NATIVE_INT32);
  EXPECT_THROW(transient.getObjName(buf.data(), buf.size()), H5::Exception);
  EXPECT_THROW(transient.getObjName(), H5::Exception);
  H5std_string sname;
  EXPECT_THROW(transient.getObjName(sname), H5::Exception);
}

}  // namespace
}  // namespace hdf5
