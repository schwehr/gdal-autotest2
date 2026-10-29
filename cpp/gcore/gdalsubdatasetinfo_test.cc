// Copyright 2026 Google Inc. All Rights Reserved.
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
// Tests gcore/gdalsubdatasetinfo.cpp.

#include "third_party/gdal/gcore/gdalsubdatasetinfo.h"

#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_string.h"

namespace {

class TestSubdatasetInfo : public GDALSubdatasetInfo {
 public:
  TestSubdatasetInfo(const std::string& file_name, const std::string& prefix,
                     const std::string& path, const std::string& subdataset)
      : GDALSubdatasetInfo(file_name),
        prefix_(prefix),
        path_(path),
        subdataset_(subdataset) {}

  static std::string CallQuote(const std::string& path) { return quote(path); }
  static std::string CallUnquote(const std::string& path) {
    return unquote(path);
  }

 protected:
  void parseFileName() override {
    m_driverPrefixComponent = prefix_;
    m_pathComponent = path_;
    m_subdatasetComponent = subdataset_;
  }

 private:
  std::string prefix_;
  std::string path_;
  std::string subdataset_;
};

GDALSubdatasetInfo* MockSubdatasetInfoFunc(const char* file_name) {
  if (STARTS_WITH(file_name, "MOCKSDS:")) {
    return new TestSubdatasetInfo(file_name, "MOCKSDS", "/path/to/mock.dat",
                                  "layer1");
  }
  return nullptr;
}

TEST(GdalsubdatasetinfoTest, QuoteUnquoteAndModifyPath) {
  // Unquoted path.
  TestSubdatasetInfo info1("DRV:/dir/file.dat:sub1", "DRV", "/dir/file.dat",
                           "sub1");
  EXPECT_EQ("/dir/file.dat", info1.GetPathComponent());
  EXPECT_EQ("sub1", info1.GetSubdatasetComponent());
  EXPECT_EQ("DRV:/new/file.dat:sub1",
            info1.ModifyPathComponent("/new/file.dat"));

  // Quoted path with escaped quotes inside.
  TestSubdatasetInfo info2(R"(DRV:"/dir/with\"quote/file.dat":sub2)", "DRV",
                           R"("/dir/with\"quote/file.dat")", "sub2");
  EXPECT_EQ(R"(/dir/with"quote/file.dat)", info2.GetPathComponent());
  EXPECT_EQ("sub2", info2.GetSubdatasetComponent());
  EXPECT_EQ(R"(DRV:"/other/\"path\".dat":sub2)",
            info2.ModifyPathComponent(R"(/other/"path".dat)"));

  // Path component not found in m_fileName triggers std::out_of_range catch.
  TestSubdatasetInfo info_missing("DRV:/actual/file.dat:sub3", "DRV",
                                  "not_in_filename", "sub3");
  EXPECT_EQ("", info_missing.ModifyPathComponent("/replacement.dat"));

  // Direct quote/unquote edge cases.
  EXPECT_EQ("a", TestSubdatasetInfo::CallUnquote("a"));
  EXPECT_EQ("unquoted", TestSubdatasetInfo::CallUnquote("unquoted"));
  EXPECT_EQ(R"("hello")", TestSubdatasetInfo::CallUnquote(R"("\"hello\"")"));
  EXPECT_EQ(R"("a\"b")", TestSubdatasetInfo::CallQuote(R"(a"b)"));
}

TEST(GdalsubdatasetinfoTest, CApiAndDriverLookup) {
  // Register two mock drivers: one without GDAL_DMD_SUBDATASETS, and one with.
  GDALDriver drv_no_meta;
  drv_no_meta.SetDescription("MOCK_NO_SDS_META");
  drv_no_meta.pfnGetSubdatasetInfoFunc = &MockSubdatasetInfoFunc;
  GetGDALDriverManager()->RegisterDriver(&drv_no_meta);

  GDALDriver drv_with_meta;
  drv_with_meta.SetDescription("MOCK_WITH_SDS_META");
  drv_with_meta.SetMetadataItem(GDAL_DMD_SUBDATASETS, "YES");
  drv_with_meta.pfnGetSubdatasetInfoFunc = &MockSubdatasetInfoFunc;
  GetGDALDriverManager()->RegisterDriver(&drv_with_meta);

  // Non-matching filename returns nullptr.
  EXPECT_EQ(nullptr, GDALGetSubdatasetInfo("NONEXISTENT_PREFIX:foo:bar"));

  // Matching filename returns valid handle.
  GDALSubdatasetInfoH h_info =
      GDALGetSubdatasetInfo("MOCKSDS:/path/to/mock.dat:layer1");
  ASSERT_NE(nullptr, h_info);

  char* path = GDALSubdatasetInfoGetPathComponent(h_info);
  EXPECT_STREQ("/path/to/mock.dat", path);
  CPLFree(path);

  char* subds = GDALSubdatasetInfoGetSubdatasetComponent(h_info);
  EXPECT_STREQ("layer1", subds);
  CPLFree(subds);

  char* modified =
      GDALSubdatasetInfoModifyPathComponent(h_info, "/new/mock.dat");
  EXPECT_STREQ("MOCKSDS:/new/mock.dat:layer1", modified);
  CPLFree(modified);

  GDALDestroySubdatasetInfo(h_info);

  GetGDALDriverManager()->DeregisterDriver(&drv_with_meta);
  GetGDALDriverManager()->DeregisterDriver(&drv_no_meta);
}

}  // namespace
