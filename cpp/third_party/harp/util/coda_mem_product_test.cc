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

#include "third_party/stcorp_harp/google/util/coda_mem_product.h"

#include <array>
#include <cstdint>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/stcorp_coda/libcoda/coda.h"

namespace stcorp_harp {
namespace {

using ::testing::ElementsAre;

TEST(CodaMemProductTest, ScalarsArraysAttributesUnionAndMetadata) {
  CodaMemProduct prod(coda_format_xml, "S5P_TEST.nc");
  ASSERT_TRUE(prod.ok());
  ASSERT_TRUE(prod.SetProductInfo("Sentinel5P", "L2__NO2___", 2));
  prod.SetFileSize(4096);

  const char* pclass = nullptr;
  const char* ptype = nullptr;
  int pver = -1;
  ASSERT_EQ(0, coda_get_product_class(prod.product(), &pclass));
  ASSERT_EQ(0, coda_get_product_type(prod.product(), &ptype));
  ASSERT_EQ(0, coda_get_product_version(prod.product(), &pver));
  EXPECT_STREQ("Sentinel5P", pclass);
  EXPECT_STREQ("L2__NO2___", ptype);
  EXPECT_EQ(2, pver);

  ASSERT_TRUE(prod.AddAttribute(prod.root(), "title", prod.MakeString("demo")));
  ASSERT_TRUE(prod.AddField(prod.root(), "count", prod.MakeInt32(42)));
  ASSERT_TRUE(prod.AddUnavailableField(prod.root(), "missing"));

  constexpr long kDims[] = {2, 2};
  constexpr double kVals[] = {1.5, 2.5, 3.5, 4.5};
  CodaMemNode* arr = prod.MakeDoubleArray(kDims, kVals);
  ASSERT_TRUE(prod.AddAttribute(arr, "FillValue", prod.MakeDouble(-999.0)));
  ASSERT_TRUE(prod.AddField(prod.root(), "values", arr));

  constexpr std::string_view kUnionFields[] = {"a", "b"};
  ASSERT_TRUE(
      prod.AddField(prod.root(), "choice",
                    prod.MakeUnion(kUnionFields, 1, prod.MakeInt16(7))));

  ASSERT_TRUE(prod.AddField(prod.root(), "timestamp", prod.MakeTime(86400.0)));

  constexpr long kStrDims[] = {2};
  constexpr std::string_view kStrs[] = {"alpha", "beta"};
  ASSERT_TRUE(prod.AddField(prod.root(), "labels",
                            prod.MakeStringArray(kStrDims, kStrs)));

  coda_cursor cur;
  ASSERT_EQ(0, coda_cursor_set_product(&cur, prod.product()));
  ASSERT_EQ(0, coda_cursor_goto(&cur, "/@title"));
  char title[16] = {};
  ASSERT_EQ(0, coda_cursor_read_string(&cur, title, sizeof(title)));
  EXPECT_STREQ("demo", title);

  ASSERT_EQ(0, coda_cursor_goto(&cur, "/count"));
  int32_t count = 0;
  ASSERT_EQ(0, coda_cursor_read_int32(&cur, &count));
  EXPECT_EQ(42, count);

  ASSERT_EQ(0, coda_cursor_goto(&cur, "/values"));
  std::array<double, 4> got = {};
  ASSERT_EQ(0, coda_cursor_read_double_array(&cur, got.data(),
                                             coda_array_ordering_c));
  EXPECT_THAT(got, ElementsAre(1.5, 2.5, 3.5, 4.5));

  ASSERT_EQ(0, coda_cursor_goto(&cur, "/choice"));
  long u_idx = -1;
  ASSERT_EQ(0, coda_cursor_get_available_union_field_index(&cur, &u_idx));
  EXPECT_EQ(1, u_idx);

  ASSERT_EQ(0, coda_cursor_goto(&cur, "/timestamp"));
  double tval = 0.0;
  ASSERT_EQ(0, coda_cursor_read_double(&cur, &tval));
  EXPECT_DOUBLE_EQ(86400.0, tval);
}

TEST(CodaMemProductTest, BinaryMultiDimArraySupportsGotoArrayElement) {
  CodaMemProduct prod(coda_format_binary, "sci.dat");
  ASSERT_TRUE(prod.ok());
  constexpr long kDims[] = {2, 3};
  constexpr double kVals[] = {10.0, 20.0, 30.0, 40.0, 50.0, 60.0};
  ASSERT_TRUE(prod.AddField(prod.root(), "grid",
                            prod.MakeBinaryDoubleArray(kDims, kVals)));

  constexpr long kRecDims[] = {2, 2};
  constexpr std::string_view kRecFields[] = {"vmr", "err"};
  constexpr double kRecVals[] = {1.0, 0.1, 2.0, 0.2, 3.0, 0.3, 4.0, 0.4};
  ASSERT_TRUE(prod.AddField(
      prod.root(), "main_species",
      prod.MakeBinaryDoubleRecordArray(kRecDims, kRecFields, kRecVals)));

  coda_cursor cur;
  ASSERT_EQ(0, coda_cursor_set_product(&cur, prod.product()));
  ASSERT_EQ(0, coda_cursor_goto(&cur, "/grid"));
  long subs[] = {1, 2};
  ASSERT_EQ(0, coda_cursor_goto_array_element(&cur, 2, subs));
  double v = 0.0;
  ASSERT_EQ(0, coda_cursor_read_double(&cur, &v));
  EXPECT_DOUBLE_EQ(60.0, v);

  ASSERT_EQ(0, coda_cursor_set_product(&cur, prod.product()));
  ASSERT_EQ(0, coda_cursor_goto(&cur, "/main_species"));
  long rsubs[] = {1, 0};
  ASSERT_EQ(0, coda_cursor_goto_array_element(&cur, 2, rsubs));
  ASSERT_EQ(0, coda_cursor_goto_record_field_by_name(&cur, "err"));
  double err = 0.0;
  ASSERT_EQ(0, coda_cursor_read_double(&cur, &err));
  EXPECT_DOUBLE_EQ(0.3, err);
}

TEST(CodaMemProductTest, SetRootNodeReplacesRootWithNodeArray) {
  CodaMemProduct prod(coda_format_grib, "ecmwf.grib");
  ASSERT_TRUE(prod.ok());
  CodaMemNode* msg0 = prod.MakeRecord();
  ASSERT_TRUE(prod.AddField(msg0, "param", prod.MakeInt32(130)));
  CodaMemNode* msg1 = prod.MakeRecord();
  ASSERT_TRUE(prod.AddField(msg1, "param", prod.MakeInt32(131)));
  CodaMemNode* elems[] = {msg0, msg1};
  constexpr long kDims[] = {2};
  ASSERT_TRUE(prod.SetRootNode(prod.MakeNodeArray(kDims, elems)));

  coda_cursor cur;
  ASSERT_EQ(0, coda_cursor_set_product(&cur, prod.product()));
  ASSERT_EQ(0, coda_cursor_goto_array_element_by_index(&cur, 1));
  ASSERT_EQ(0, coda_cursor_goto_record_field_by_name(&cur, "param"));
  int32_t param = 0;
  ASSERT_EQ(0, coda_cursor_read_int32(&cur, &param));
  EXPECT_EQ(131, param);
}

TEST(CodaMemProductTest, SetProductInfoPreservesHyphensInProductType) {
  CodaMemProduct prod(coda_format_xml, "S4_TEST.nc");
  ASSERT_TRUE(prod.ok());
  ASSERT_TRUE(prod.SetProductInfo("Sentinel4", "UVN-2-ALH", 0));

  const char* ptype = nullptr;
  ASSERT_EQ(0, coda_get_product_type(prod.product(), &ptype));
  EXPECT_STREQ("UVN-2-ALH", ptype);
}

}  // namespace
}  // namespace stcorp_harp
