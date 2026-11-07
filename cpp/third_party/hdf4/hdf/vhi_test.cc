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

#include <array>
#include <cstdint>
#include <filesystem>  // NOLINT
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/vg.h"

namespace hdf4 {
namespace {

std::string GetTempFilePath(const std::string& name) {
  return (std::filesystem::path(::testing::TempDir()) / name).string();
}

TEST(VhiTest, StoreDataBasic) {
  const std::string path = GetTempFilePath("vhi_storedata.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  ASSERT_EQ(SUCCEED, Vstart(fid));
  absl::Cleanup file_cleanup = [&]() {
    Vend(fid);
    Hclose(fid);
    std::filesystem::remove(path);
  };

  constexpr float kValues[] = {1.5f, 2.5f, 3.5f, 4.5f, 5.5f};
  const int32_t vs_ref =
      VHstoredata(fid, "temperature", reinterpret_cast<const uint8_t*>(kValues),
                  5, DFNT_FLOAT32, "TempData", "TempClass");
  ASSERT_GT(vs_ref, 0);

  // Read back and verify via VS API.
  const int32_t vs = VSattach(fid, vs_ref, "r");
  ASSERT_NE(vs, FAIL);
  absl::Cleanup vs_cleanup = [&]() { VSdetach(vs); };

  char name[VSNAMELENMAX + 1] = {};
  char class_name[VSNAMELENMAX + 1] = {};
  EXPECT_EQ(SUCCEED, VSgetname(vs, name));
  EXPECT_EQ(SUCCEED, VSgetclass(vs, class_name));
  EXPECT_STREQ(name, "TempData");
  EXPECT_STREQ(class_name, "TempClass");

  std::array<float, 5> read_values{};
  ASSERT_EQ(SUCCEED, VSsetfields(vs, "temperature"));
  EXPECT_EQ(5, VSread(vs, reinterpret_cast<uint8_t*>(read_values.data()), 5,
                      FULL_INTERLACE));
  for (size_t i = 0; i < 5; ++i) {
    EXPECT_FLOAT_EQ(read_values[i], kValues[i]);
  }
}

TEST(VhiTest, StoredatamAggregate) {
  const std::string path = GetTempFilePath("vhi_storedatam.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  ASSERT_EQ(SUCCEED, Vstart(fid));
  absl::Cleanup file_cleanup = [&]() {
    Vend(fid);
    Hclose(fid);
    std::filesystem::remove(path);
  };

  // 4 records of 3 int16_t values each (order = 3).
  constexpr int16_t kValues[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  const int32_t vs_ref = VHstoredatam(fid, "vector_field",
                                      reinterpret_cast<const uint8_t*>(kValues),
                                      4, DFNT_INT16, "VecData", "VecClass", 3);
  ASSERT_GT(vs_ref, 0);

  const int32_t vs = VSattach(fid, vs_ref, "r");
  ASSERT_NE(vs, FAIL);
  absl::Cleanup vs_cleanup = [&]() { VSdetach(vs); };

  std::array<int16_t, 12> read_values{};
  ASSERT_EQ(SUCCEED, VSsetfields(vs, "vector_field"));
  EXPECT_EQ(4, VSread(vs, reinterpret_cast<uint8_t*>(read_values.data()), 4,
                      FULL_INTERLACE));
  EXPECT_EQ(read_values,
            (std::array<int16_t, 12>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}));
}

TEST(VhiTest, StoredataErrorCases) {
  const std::string path = GetTempFilePath("vhi_store_err.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  ASSERT_EQ(SUCCEED, Vstart(fid));
  absl::Cleanup file_cleanup = [&]() {
    Vend(fid);
    Hclose(fid);
    std::filesystem::remove(path);
  };

  constexpr int32_t kVal = 42;

  // Invalid file ID: VSattach fails, error preserved because vs == FAIL.
  HEclear();
  EXPECT_EQ(FAIL,
            VHstoredata(FAIL, "f", reinterpret_cast<const uint8_t*>(&kVal), 1,
                        DFNT_INT32, "n", "c"));
  EXPECT_EQ(DFE_CANTATTACH, HEvalue(1));

  // Invalid order (0) causes VSfdefine to fail. VSdetach at done clears error
  // stack.
  EXPECT_EQ(FAIL,
            VHstoredatam(fid, "f", reinterpret_cast<const uint8_t*>(&kVal), 1,
                         DFNT_INT32, "n", "c", 0));

  // Invalid datatype causes VSfdefine to fail.
  EXPECT_EQ(FAIL,
            VHstoredatam(fid, "f", reinterpret_cast<const uint8_t*>(&kVal), 1,
                         99999, "n", "c", 1));

  // Field name "a,b" is defined as a single field "a,b", but VSsetfields
  // parses it as two separate fields "a" and "b", causing VSsetfields to fail.
  EXPECT_EQ(FAIL,
            VHstoredatam(fid, "a,b", reinterpret_cast<const uint8_t*>(&kVal), 1,
                         DFNT_INT32, "n", "c", 1));

  // vsname == nullptr causes VSsetname to fail.
  EXPECT_EQ(FAIL,
            VHstoredatam(fid, "f", reinterpret_cast<const uint8_t*>(&kVal), 1,
                         DFNT_INT32, nullptr, "c", 1));

  // vsclass == nullptr causes VSsetclass to fail.
  EXPECT_EQ(FAIL,
            VHstoredatam(fid, "f", reinterpret_cast<const uint8_t*>(&kVal), 1,
                         DFNT_INT32, "n", nullptr, 1));

  // Count n == 0 causes VSwrite to fail (returns FAIL != n).
  EXPECT_EQ(FAIL,
            VHstoredatam(fid, "f", reinterpret_cast<const uint8_t*>(&kVal), 0,
                         DFNT_INT32, "n", "c", 1));
}

TEST(VhiTest, MakegroupBasic) {
  const std::string path = GetTempFilePath("vhi_makegroup.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  ASSERT_EQ(SUCCEED, Vstart(fid));
  absl::Cleanup file_cleanup = [&]() {
    Vend(fid);
    Hclose(fid);
    std::filesystem::remove(path);
  };

  int32_t tags[] = {DFTAG_RI, DFTAG_CI};
  int32_t refs[] = {100, 200};
  const int32_t vg_ref = VHmakegroup(fid, tags, refs, 2, "MyGroup", "MyClass");
  ASSERT_GT(vg_ref, 0);

  // Read back via Vgroup API.
  const int32_t vg = Vattach(fid, vg_ref, "r");
  ASSERT_NE(vg, FAIL);
  absl::Cleanup vg_cleanup = [&]() { Vdetach(vg); };

  char name[VGNAMELENMAX + 1] = {};
  char class_name[VGNAMELENMAX + 1] = {};
  EXPECT_EQ(SUCCEED, Vgetname(vg, name));
  EXPECT_EQ(SUCCEED, Vgetclass(vg, class_name));
  EXPECT_STREQ(name, "MyGroup");
  EXPECT_STREQ(class_name, "MyClass");
  EXPECT_EQ(2, Vntagrefs(vg));
}

TEST(VhiTest, MakegroupNullNameAndClass) {
  const std::string path = GetTempFilePath("vhi_makegroup_null.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  ASSERT_EQ(SUCCEED, Vstart(fid));
  absl::Cleanup file_cleanup = [&]() {
    Vend(fid);
    Hclose(fid);
    std::filesystem::remove(path);
  };

  int32_t tags[] = {DFTAG_RI};
  int32_t refs[] = {101};
  const int32_t vg_ref = VHmakegroup(fid, tags, refs, 1, nullptr, nullptr);
  ASSERT_GT(vg_ref, 0);

  const int32_t vg = Vattach(fid, vg_ref, "r");
  ASSERT_NE(vg, FAIL);
  EXPECT_EQ(1, Vntagrefs(vg));
  EXPECT_EQ(SUCCEED, Vdetach(vg));
}

TEST(VhiTest, MakegroupErrorCases) {
  int32_t tags[] = {DFTAG_RI};
  int32_t refs[] = {102};

  // Invalid fid: Vattach fails, error preserved because vg == FAIL.
  HEclear();
  EXPECT_EQ(FAIL, VHmakegroup(FAIL, tags, refs, 1, "n", "c"));
  EXPECT_EQ(DFE_CANTATTACH, HEvalue(1));
}

}  // namespace
}  // namespace hdf4
