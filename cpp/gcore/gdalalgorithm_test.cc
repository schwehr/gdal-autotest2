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
// Tests gcore/gdalalgorithm.cpp.

#include "third_party/gdal/gcore/gdalalgorithm.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_json.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace test_gdal_algorithm {

struct test_gdal_algorithm {
  static void SetMapDataset(GDALAlgorithm& alg, const std::string& name,
                            GDALDataset* ds) {
    alg.m_oMapDatasetNameToDataset[name] = ds;
  }

  static void SetSupportsStreamedOutput(GDALAlgorithm& alg, bool b) {
    alg.m_supportsStreamedOutput = b;
  }

  static void SetLongDescription(GDALAlgorithm& alg, const std::string& desc) {
    alg.m_longDescription = desc;
  }

  static void SetHidden(GDALAlgorithm& alg, bool b) { alg.m_hidden = b; }

  static void SetAliases(GDALAlgorithm& alg,
                         const std::vector<std::string>& aliases) {
    alg.m_aliases = aliases;
  }
};

}  // namespace test_gdal_algorithm

namespace {

using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;

class QuietErrorGuard {
 public:
  QuietErrorGuard() { CPLPushErrorHandler(CPLQuietErrorHandler); }
  ~QuietErrorGuard() { CPLPopErrorHandler(); }
};

std::unique_ptr<GDALDataset> CreateMemRaster(int width = 4, int height = 4,
                                             int bands = 2) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!drv) return nullptr;
  return std::unique_ptr<GDALDataset>(
      drv->Create("mem_ds", width, height, bands, GDT_Byte, nullptr));
}

bool CreateGTiffFile(const std::string& path, int bands = 2) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  if (!drv) return false;
  std::unique_ptr<GDALDataset> ds(
      drv->Create(path.c_str(), 4, 4, bands, GDT_Byte, nullptr));
  if (!ds) return false;
  OGRSpatialReference srs;
  srs.SetWellKnownGeogCS("WGS84");
  ds->SetSpatialRef(&srs);
  double gt[6] = {2.0, 0.1, 0.0, 49.0, 0.0, -0.1};
  ds->SetGeoTransform(gt);
  return true;
}

bool CreateGeoJSONFile(const std::string& path) {
  const char* geojson = R"json({
    "type": "FeatureCollection",
    "name": "test_layer",
    "features": [
      {
        "type": "Feature",
        "properties": { "name_field": "val1", "int_field": 10 },
        "geometry": { "type": "Point", "coordinates": [2.0, 49.0] }
      }
    ]
  })json";
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb");
  if (!fp) return false;
  VSIFWriteL(geojson, 1, strlen(geojson), fp);
  VSIFCloseL(fp);
  return true;
}

class CustomSubAlg1 : public GDALAlgorithm {
 public:
  static constexpr const char* NAME = "sub_alpha";
  static constexpr const char* DESCRIPTION = "Sub algorithm alpha";
  static constexpr const char* HELP_URL = "/programs/sub_alpha.html";
  static std::vector<std::string> GetAliasesStatic() {
    return {"sa", GDALAlgorithmRegistry::HIDDEN_ALIAS_SEPARATOR, "hidden_sa"};
  }

  CustomSubAlg1() : GDALAlgorithm(NAME, DESCRIPTION, HELP_URL) {
    AddArg("flag", 'f', "A boolean flag", &flag_);
    AddArg("text", 't', "A text option", &text_).SetDefault("def_text");
    AddProgressArg();
  }

  bool flag_ = false;
  std::string text_;
  bool ran_ = false;

 protected:
  bool RunImpl(GDALProgressFunc pfnProgress, void* pProgressData) override {
    ran_ = true;
    if (pfnProgress) pfnProgress(1.0, "done", pProgressData);
    return true;
  }
};

class CustomSubAlg2 : public GDALAlgorithm {
 public:
  static constexpr const char* NAME = "sub_beta";
  static constexpr const char* DESCRIPTION = "Sub algorithm beta";
  static constexpr const char* HELP_URL = "https://example.com/beta";
  static std::vector<std::string> GetAliasesStatic() { return {}; }

  CustomSubAlg2() : GDALAlgorithm(NAME, DESCRIPTION, HELP_URL) {
    AddArg("val", 'v', "An int value", &val_);
  }

  int val_ = 0;

 protected:
  bool RunImpl(GDALProgressFunc, void*) override { return true; }
};

class ConfigurableAlg : public GDALAlgorithm {
 public:
  ConfigurableAlg(const std::string& name = "test_alg",
                  const std::string& desc = "Test algorithm description",
                  const std::string& help_url = "/programs/test_alg.html")
      : GDALAlgorithm(name, desc, help_url) {}

  using GDALAlgorithm::AddAbsolutePathArg;
  using GDALAlgorithm::AddActiveLayerArg;
  using GDALAlgorithm::AddAliasFor;
  using GDALAlgorithm::AddAppendLayerArg;
  using GDALAlgorithm::AddArrayNameArg;

  template <typename... Args>
  GDALInConstructionAlgorithmArg& AddArg(Args&&... args) {
    return GDALAlgorithm::AddArg(std::forward<Args>(args)...);
  }
  using GDALAlgorithm::AddBandArg;
  using GDALAlgorithm::AddBBOXArg;
  using GDALAlgorithm::AddCreationOptionsArg;
  using GDALAlgorithm::AddFieldNameArg;
  using GDALAlgorithm::AddFieldTypeSubtypeArg;
  using GDALAlgorithm::AddGeometryTypeArg;
  using GDALAlgorithm::AddInputDatasetArg;
  using GDALAlgorithm::AddInputFormatsArg;
  using GDALAlgorithm::AddLayerCreationOptionsArg;
  using GDALAlgorithm::AddLayerNameArg;
  using GDALAlgorithm::AddMemorySizeArg;
  using GDALAlgorithm::AddNodataArg;
  using GDALAlgorithm::AddNumThreadsArg;
  using GDALAlgorithm::AddOpenOptionsArg;
  using GDALAlgorithm::AddOptionsSuggestions;
  using GDALAlgorithm::AddOutputDatasetArg;
  using GDALAlgorithm::AddOutputDataTypeArg;
  using GDALAlgorithm::AddOutputFormatArg;
  using GDALAlgorithm::AddOutputLayerNameArg;
  using GDALAlgorithm::AddOutputOpenOptionsArg;
  using GDALAlgorithm::AddOutputStringArg;
  using GDALAlgorithm::AddOverwriteArg;
  using GDALAlgorithm::AddOverwriteLayerArg;
  using GDALAlgorithm::AddPixelFunctionArgsArg;
  using GDALAlgorithm::AddPixelFunctionNameArg;
  using GDALAlgorithm::AddProgressArg;
  using GDALAlgorithm::AddShortNameAliasFor;
  using GDALAlgorithm::AddStdoutArg;
  using GDALAlgorithm::AddUpdateArg;
  using GDALAlgorithm::AddValidationAction;
  using GDALAlgorithm::AllowArbitraryLongNameArgs;
  using GDALAlgorithm::CheckSafeForStreamOutput;
  using GDALAlgorithm::FormatAutoCompleteFunction;
  using GDALAlgorithm::GetArgNamesForCLI;
  using GDALAlgorithm::GetUsageForCLIEnd;
  using GDALAlgorithm::IsGDALGOutput;
  using GDALAlgorithm::IsKnownOutputRelatedBooleanArgName;
  using GDALAlgorithm::OpenOptionCompleteFunction;
  using GDALAlgorithm::ParseAndValidateKeyValue;
  using GDALAlgorithm::ProcessDatasetArg;
  using GDALAlgorithm::ProcessGDALGOutput;
  using GDALAlgorithm::ProcessGDALGOutputRet;
  using GDALAlgorithm::RegisterSubAlgorithm;
  using GDALAlgorithm::RunPreStepPipelineValidations;
  using GDALAlgorithm::SetAutoCompleteFunctionForFieldName;
  using GDALAlgorithm::SetAutoCompleteFunctionForFilename;
  using GDALAlgorithm::SetAutoCompleteFunctionForLayerName;
  using GDALAlgorithm::SetDisplayInJSONUsage;
  using GDALAlgorithm::ValidateFormat;

  bool run_result_ = true;
  bool run_called_ = false;
  bool deprecated_warned_ = false;

 protected:
  void WarnIfDeprecated() override { deprecated_warned_ = true; }

  bool RunImpl(GDALProgressFunc pfnProgress, void* pProgressData) override {
    run_called_ = true;
    if (pfnProgress) pfnProgress(0.5, "running", pProgressData);
    return run_result_;
  }
};

TEST(GdalAlgorithmTest, FreeFunctionsAndTypes) {
  EXPECT_FALSE(GDALAlgorithmArgTypeIsList(GAAT_BOOLEAN));
  EXPECT_FALSE(GDALAlgorithmArgTypeIsList(GAAT_STRING));
  EXPECT_FALSE(GDALAlgorithmArgTypeIsList(GAAT_INTEGER));
  EXPECT_FALSE(GDALAlgorithmArgTypeIsList(GAAT_REAL));
  EXPECT_FALSE(GDALAlgorithmArgTypeIsList(GAAT_DATASET));
  EXPECT_TRUE(GDALAlgorithmArgTypeIsList(GAAT_STRING_LIST));
  EXPECT_TRUE(GDALAlgorithmArgTypeIsList(GAAT_INTEGER_LIST));
  EXPECT_TRUE(GDALAlgorithmArgTypeIsList(GAAT_REAL_LIST));
  EXPECT_TRUE(GDALAlgorithmArgTypeIsList(GAAT_DATASET_LIST));

  EXPECT_STREQ("boolean", GDALAlgorithmArgTypeName(GAAT_BOOLEAN));
  EXPECT_STREQ("string", GDALAlgorithmArgTypeName(GAAT_STRING));
  EXPECT_STREQ("integer", GDALAlgorithmArgTypeName(GAAT_INTEGER));
  EXPECT_STREQ("real", GDALAlgorithmArgTypeName(GAAT_REAL));
  EXPECT_STREQ("dataset", GDALAlgorithmArgTypeName(GAAT_DATASET));
  EXPECT_STREQ("string_list", GDALAlgorithmArgTypeName(GAAT_STRING_LIST));
  EXPECT_STREQ("integer_list", GDALAlgorithmArgTypeName(GAAT_INTEGER_LIST));
  EXPECT_STREQ("real_list", GDALAlgorithmArgTypeName(GAAT_REAL_LIST));
  EXPECT_STREQ("dataset_list", GDALAlgorithmArgTypeName(GAAT_DATASET_LIST));

  EXPECT_EQ("raster", GDALAlgorithmArgDatasetTypeName(GDAL_OF_RASTER));
  EXPECT_EQ("vector", GDALAlgorithmArgDatasetTypeName(GDAL_OF_VECTOR));
  EXPECT_EQ("multidimensional raster",
            GDALAlgorithmArgDatasetTypeName(GDAL_OF_MULTIDIM_RASTER));
  EXPECT_EQ("raster or vector",
            GDALAlgorithmArgDatasetTypeName(GDAL_OF_RASTER | GDAL_OF_VECTOR));
  EXPECT_EQ("raster or multidimensional raster",
            GDALAlgorithmArgDatasetTypeName(GDAL_OF_RASTER |
                                            GDAL_OF_MULTIDIM_RASTER));
  EXPECT_EQ("vector or multidimensional raster",
            GDALAlgorithmArgDatasetTypeName(GDAL_OF_VECTOR |
                                            GDAL_OF_MULTIDIM_RASTER));
  EXPECT_EQ("raster, vector or multidimensional raster",
            GDALAlgorithmArgDatasetTypeName(GDAL_OF_RASTER | GDAL_OF_VECTOR |
                                            GDAL_OF_MULTIDIM_RASTER));
}

TEST(GdalAlgorithmTest, ArgDeclAndSetConversions) {
  GDALAllRegister();

  // GDALAlgorithmArgDecl scalar vs list SetMinCount/SetMaxCount and SetDefault
  {
    QuietErrorGuard quiet;
    GDALAlgorithmArgDecl scalar_decl("scalar", 's', "desc", GAAT_INTEGER);
    scalar_decl.SetMinCount(2);
    scalar_decl.SetMaxCount(5);
    EXPECT_EQ(0, scalar_decl.GetMinCount());
    EXPECT_EQ(1, scalar_decl.GetMaxCount());
    // Wrong type for SetDefault
    scalar_decl.SetDefault(std::string("wrong"));

    GDALAlgorithmArgDecl ds_decl("ds", 0, "desc", GAAT_DATASET);
    ds_decl.SetDefault(123);
  }

  GDALAlgorithmArgDecl str_list_decl("str-list", 0, "desc", GAAT_STRING_LIST);
  str_list_decl.SetDefault(std::string("single_str"));
  EXPECT_THAT(str_list_decl.GetDefault<std::vector<std::string>>(),
              ElementsAre("single_str"));

  GDALAlgorithmArgDecl int_list_decl("int-list", 0, "desc", GAAT_INTEGER_LIST);
  int_list_decl.SetDefault(7);
  EXPECT_THAT(int_list_decl.GetDefault<std::vector<int>>(), ElementsAre(7));

  GDALAlgorithmArgDecl real_list_decl("real-list", 0, "desc", GAAT_REAL_LIST);
  real_list_decl.SetDefault(2.5);
  EXPECT_THAT(real_list_decl.GetDefault<std::vector<double>>(),
              ElementsAre(2.5));

  // Test all Set() overloads across all 9 argument types
  ConfigurableAlg alg;
  bool b_val = false;
  std::string s_val;
  int i_val = 0;
  double d_val = 0.0;
  GDALArgDatasetValue ds_val;
  std::vector<std::string> sl_val;
  std::vector<int> il_val;
  std::vector<double> dl_val;
  std::vector<GDALArgDatasetValue> dsl_val;

  GDALAlgorithmArg& b_arg = alg.AddArg("bool-arg", 'b', "Bool", &b_val);
  GDALAlgorithmArg& s_arg = alg.AddArg("str-arg", 's', "Str", &s_val);
  GDALAlgorithmArg& i_arg = alg.AddArg("int-arg", 'i', "Int", &i_val);
  GDALAlgorithmArg& d_arg = alg.AddArg("real-arg", 'r', "Real", &d_val);
  GDALAlgorithmArg& ds_arg = alg.AddArg("ds-arg", 'd', "DS", &ds_val);
  GDALAlgorithmArg& sl_arg = alg.AddArg("sl-arg", 0, "StrList", &sl_val);
  GDALAlgorithmArg& il_arg = alg.AddArg("il-arg", 0, "IntList", &il_val);
  GDALAlgorithmArg& dl_arg = alg.AddArg("dl-arg", 0, "RealList", &dl_val);
  GDALAlgorithmArg& dsl_arg = alg.AddArg("dsl-arg", 0, "DSList", &dsl_val);

  // GAAT_BOOLEAN Set() variants
  EXPECT_TRUE(b_arg.Set(true));
  EXPECT_TRUE(b_val);
  EXPECT_TRUE(b_arg.Set(std::string("0")));
  EXPECT_FALSE(b_val);
  EXPECT_TRUE(b_arg.Set(std::string("1")));
  EXPECT_TRUE(b_val);
  EXPECT_TRUE(b_arg.Set(std::string("FALSE")));
  EXPECT_FALSE(b_val);
  EXPECT_TRUE(b_arg.Set(std::string("TRUE")));
  EXPECT_TRUE(b_val);
  EXPECT_TRUE(b_arg.Set(std::string("NO")));
  EXPECT_FALSE(b_val);
  EXPECT_TRUE(b_arg.Set(std::string("YES")));
  EXPECT_TRUE(b_val);
  EXPECT_TRUE(b_arg.Set(std::string("OFF")));
  EXPECT_FALSE(b_val);
  EXPECT_TRUE(b_arg.Set(std::string("ON")));
  EXPECT_TRUE(b_val);
  EXPECT_TRUE(b_arg.Set(0));
  EXPECT_FALSE(b_val);
  EXPECT_TRUE(b_arg.Set(1));
  EXPECT_TRUE(b_val);
  b_arg = false;
  EXPECT_FALSE(b_val);

  {
    QuietErrorGuard quiet;
    EXPECT_FALSE(b_arg.Set(std::string("invalid_bool")));
    EXPECT_FALSE(b_arg.Set(2));
    EXPECT_FALSE(b_arg.Set(1.5));
    EXPECT_FALSE(b_arg.Set(std::vector<std::string>{"true"}));
    EXPECT_FALSE(b_arg.Set(std::vector<int>{1}));
    EXPECT_FALSE(b_arg.Set(std::vector<double>{1.0}));
    EXPECT_FALSE(s_arg.Set(true));
  }

  // GAAT_INTEGER Set() variants
  EXPECT_TRUE(i_arg.Set(42));
  EXPECT_EQ(42, i_val);
  EXPECT_TRUE(i_arg.Set(std::string("-15")));
  EXPECT_EQ(-15, i_val);
  EXPECT_TRUE(i_arg.Set(10.0));
  EXPECT_EQ(10, i_val);
  EXPECT_TRUE(i_arg.Set(std::vector<std::string>{"99"}));
  EXPECT_EQ(99, i_val);
  EXPECT_TRUE(i_arg.Set(std::vector<int>{77}));
  EXPECT_EQ(77, i_val);
  EXPECT_TRUE(i_arg.Set(std::vector<double>{88.0}));
  EXPECT_EQ(88, i_val);
  i_arg = 55;
  EXPECT_EQ(55, i_val);

  {
    QuietErrorGuard quiet;
    EXPECT_FALSE(i_arg.Set(std::string("not_an_int")));
    EXPECT_FALSE(i_arg.Set(10.5));
    EXPECT_FALSE(i_arg.Set(std::vector<int>{1, 2}));
    EXPECT_FALSE(i_arg.Set(std::vector<double>{1.0, 2.0}));
    EXPECT_FALSE(i_arg.Set(std::vector<std::string>{"1", "2"}));
  }

  // GAAT_REAL Set() variants
  EXPECT_TRUE(d_arg.Set(3.25));
  EXPECT_DOUBLE_EQ(3.25, d_val);
  EXPECT_TRUE(d_arg.Set(5));
  EXPECT_DOUBLE_EQ(5.0, d_val);
  EXPECT_TRUE(d_arg.Set(std::string("6.5")));
  EXPECT_DOUBLE_EQ(6.5, d_val);
  EXPECT_TRUE(d_arg.Set(std::vector<std::string>{"7.5"}));
  EXPECT_DOUBLE_EQ(7.5, d_val);
  EXPECT_TRUE(d_arg.Set(std::vector<int>{8}));
  EXPECT_DOUBLE_EQ(8.0, d_val);
  EXPECT_TRUE(d_arg.Set(std::vector<double>{9.5}));
  EXPECT_DOUBLE_EQ(9.5, d_val);
  d_arg = 11.25;
  EXPECT_DOUBLE_EQ(11.25, d_val);

  {
    QuietErrorGuard quiet;
    EXPECT_FALSE(d_arg.Set(std::string("not_a_real")));
  }

  // GAAT_STRING Set() variants
  EXPECT_TRUE(s_arg.Set("hello"));
  EXPECT_EQ("hello", s_val);
  EXPECT_TRUE(s_arg.Set(123));
  EXPECT_EQ("123", s_val);
  EXPECT_TRUE(s_arg.Set(4.5));
  EXPECT_TRUE(s_arg.Set(GDT_Byte));
  EXPECT_EQ("Byte", s_val);
  OGRSpatialReference srs;
  srs.SetWellKnownGeogCS("WGS84");
  EXPECT_TRUE(s_arg.Set(srs));
  EXPECT_THAT(s_val, HasSubstr("WGS 84"));
  EXPECT_TRUE(s_arg.Set(std::vector<std::string>{"from_vec"}));
  EXPECT_EQ("from_vec", s_val);
  EXPECT_TRUE(s_arg.Set(std::vector<int>{321}));
  EXPECT_EQ("321", s_val);
  EXPECT_TRUE(s_arg.Set(std::vector<double>{2.5}));
  s_arg = "via_op";
  EXPECT_EQ("via_op", s_val);
  s_arg = std::string("via_str_op");
  EXPECT_EQ("via_str_op", s_val);
  s_arg = GDT_UInt16;
  EXPECT_EQ("UInt16", s_val);
  s_arg = srs;
  EXPECT_THAT(s_val, HasSubstr("WGS 84"));

  // GAAT_INTEGER_LIST Set() variants
  EXPECT_TRUE(il_arg.Set(std::string("12")));
  EXPECT_THAT(il_val, ElementsAre(12));
  EXPECT_TRUE(il_arg.Set(34));
  EXPECT_THAT(il_val, ElementsAre(34));
  EXPECT_TRUE(il_arg.Set(56.0));
  EXPECT_THAT(il_val, ElementsAre(56));
  EXPECT_TRUE(il_arg.Set(std::vector<std::string>{"10", "20"}));
  EXPECT_THAT(il_val, ElementsAre(10, 20));
  EXPECT_TRUE(il_arg.Set(std::vector<double>{30.0, 40.0}));
  EXPECT_THAT(il_val, ElementsAre(30, 40));
  il_arg = std::vector<int>{50, 60};
  EXPECT_THAT(il_val, ElementsAre(50, 60));

  {
    QuietErrorGuard quiet;
    EXPECT_FALSE(il_arg.Set(std::string("bad")));
    EXPECT_FALSE(il_arg.Set(12.34));
    EXPECT_FALSE(il_arg.Set(std::vector<std::string>{"10", "bad"}));
    EXPECT_FALSE(il_arg.Set(std::vector<double>{10.0, 12.5}));
  }

  // GAAT_REAL_LIST Set() variants
  EXPECT_TRUE(dl_arg.Set(std::string("1.25")));
  EXPECT_THAT(dl_val, ElementsAre(1.25));
  EXPECT_TRUE(dl_arg.Set(5));
  EXPECT_THAT(dl_val, ElementsAre(5.0));
  EXPECT_TRUE(dl_arg.Set(6.5));
  EXPECT_THAT(dl_val, ElementsAre(6.5));
  EXPECT_TRUE(dl_arg.Set(std::vector<std::string>{"1.5", "2.5"}));
  EXPECT_THAT(dl_val, ElementsAre(1.5, 2.5));
  EXPECT_TRUE(dl_arg.Set(std::vector<int>{3, 4}));
  EXPECT_THAT(dl_val, ElementsAre(3.0, 4.0));
  dl_arg = std::vector<double>{7.5, 8.5};
  EXPECT_THAT(dl_val, ElementsAre(7.5, 8.5));

  {
    QuietErrorGuard quiet;
    EXPECT_FALSE(dl_arg.Set(std::string("bad")));
    EXPECT_FALSE(dl_arg.Set(std::vector<std::string>{"1.5", "bad"}));
  }

  // GAAT_STRING_LIST Set() variants
  EXPECT_TRUE(sl_arg.Set(std::string("one")));
  EXPECT_THAT(sl_val, ElementsAre("one"));
  EXPECT_TRUE(sl_arg.Set(2));
  EXPECT_THAT(sl_val, ElementsAre("2"));
  EXPECT_TRUE(sl_arg.Set(3.5));
  EXPECT_EQ(1u, sl_val.size());
  EXPECT_TRUE(sl_arg.Set(std::vector<int>{4, 5}));
  EXPECT_THAT(sl_val, ElementsAre("4", "5"));
  EXPECT_TRUE(sl_arg.Set(std::vector<double>{6.0, 7.0}));
  EXPECT_EQ(2u, sl_val.size());
  sl_arg = std::vector<std::string>{"a", "b"};
  EXPECT_THAT(sl_val, ElementsAre("a", "b"));

  // GAAT_DATASET and GAAT_DATASET_LIST Set() variants
  EXPECT_TRUE(ds_arg.Set(std::string("/vsimem/ds1.tif")));
  EXPECT_EQ("/vsimem/ds1.tif", ds_val.GetName());
  auto mem_ds1 = CreateMemRaster();
  EXPECT_TRUE(ds_arg.Set(mem_ds1.get()));
  EXPECT_EQ(mem_ds1.get(), ds_val.GetDatasetRef());
  auto mem_ds2 = CreateMemRaster();
  GDALDataset* mem_ds2_raw = mem_ds2.get();
  ds_arg = std::move(mem_ds2);
  EXPECT_EQ(mem_ds2_raw, ds_val.GetDatasetRef());
  auto mem_ds3 = CreateMemRaster();
  ds_arg = mem_ds3.get();
  EXPECT_EQ(mem_ds3.get(), ds_val.GetDatasetRef());

  GDALArgDatasetValue other_ds_val("/vsimem/other.tif");
  EXPECT_TRUE(ds_arg.SetFrom(other_ds_val));
  EXPECT_EQ("/vsimem/other.tif", ds_val.GetName());

  EXPECT_TRUE(dsl_arg.Set(std::string("/vsimem/dsl1.tif")));
  ASSERT_EQ(1u, dsl_val.size());
  EXPECT_EQ("/vsimem/dsl1.tif", dsl_val[0].GetName());
  EXPECT_TRUE(
      dsl_arg.Set(std::vector<std::string>{"/vsimem/a.tif", "/vsimem/b.tif"}));
  ASSERT_EQ(2u, dsl_val.size());
  EXPECT_EQ("/vsimem/b.tif", dsl_val[1].GetName());

  // Error paths on dataset setters for non-dataset args
  {
    QuietErrorGuard quiet;
    EXPECT_FALSE(s_arg.Set(mem_ds1.get()));
    EXPECT_FALSE(s_arg.Set(CreateMemRaster()));
    EXPECT_FALSE(s_arg.SetDatasetName("foo"));
    EXPECT_FALSE(s_arg.SetFrom(other_ds_val));
    EXPECT_FALSE(s_arg.Set(std::vector<GDALArgDatasetValue>{}));
    EXPECT_FALSE(ds_arg.Set(123));
  }

  // CheckCanSetDatasetObject restrictions
  GDALArgDatasetValue out_only_val;
  auto& out_only_arg = alg.AddArg("out-only-ds", 0, "Out DS", &out_only_val)
                           .SetDatasetInputFlags(GADV_NAME)
                           .SetDatasetOutputFlags(GADV_OBJECT);
  GDALArgDatasetValue name_only_val;
  auto& name_only_arg = alg.AddArg("name-only-ds", 0, "Name DS", &name_only_val)
                            .SetDatasetInputFlags(GADV_NAME)
                            .SetDatasetOutputFlags(0);
  {
    QuietErrorGuard quiet;
    EXPECT_FALSE(out_only_arg.Set(mem_ds1.get()));
    EXPECT_FALSE(out_only_arg.Set(CreateMemRaster()));
    EXPECT_FALSE(out_only_arg.SetFrom(other_ds_val));
    EXPECT_FALSE(name_only_arg.Set(mem_ds1.get()));
  }

  // SetFrom(const GDALAlgorithmArg&) for all types
  ConfigurableAlg alg2;
  bool b_val2 = false;
  std::string s_val2;
  int i_val2 = 0;
  double d_val2 = 0.0;
  GDALArgDatasetValue ds_val2;
  std::vector<std::string> sl_val2;
  std::vector<int> il_val2;
  std::vector<double> dl_val2;
  std::vector<GDALArgDatasetValue> dsl_val2;

  auto& b_arg2 = alg2.AddArg("bool-arg", 0, "Bool", &b_val2);
  auto& s_arg2 = alg2.AddArg("str-arg", 0, "Str", &s_val2);
  auto& i_arg2 = alg2.AddArg("int-arg", 0, "Int", &i_val2);
  auto& d_arg2 = alg2.AddArg("real-arg", 0, "Real", &d_val2);
  auto& ds_arg2 = alg2.AddArg("ds-arg", 0, "DS", &ds_val2);
  auto& sl_arg2 = alg2.AddArg("sl-arg", 0, "StrList", &sl_val2);
  auto& il_arg2 = alg2.AddArg("il-arg", 0, "IntList", &il_val2);
  auto& dl_arg2 = alg2.AddArg("dl-arg", 0, "RealList", &dl_val2);
  auto& dsl_arg2 = alg2.AddArg("dsl-arg", 0, "DSList", &dsl_val2);

  EXPECT_TRUE(b_arg2.SetFrom(b_arg));
  EXPECT_TRUE(s_arg2.SetFrom(s_arg));
  EXPECT_TRUE(i_arg2.SetFrom(i_arg));
  EXPECT_TRUE(d_arg2.SetFrom(d_arg));
  EXPECT_TRUE(ds_arg2.SetFrom(ds_arg));
  EXPECT_TRUE(sl_arg2.SetFrom(sl_arg));
  EXPECT_TRUE(il_arg2.SetFrom(il_arg));
  EXPECT_TRUE(dl_arg2.SetFrom(dl_arg));
  EXPECT_TRUE(dsl_arg2.SetFrom(dsl_arg));
  EXPECT_EQ(2u, dsl_val2.size());

  {
    QuietErrorGuard quiet;
    EXPECT_FALSE(b_arg2.SetFrom(s_arg));
  }
}

TEST(GdalAlgorithmTest, ProcessStringAndDatasetValueMethods) {
  GDALAllRegister();

  // UTF-8 BOM file and SQL comments removal
  const char* bom_file = "/vsimem/bom_sql.txt";
  const unsigned char bom_content[] = {
      0xEF, 0xBB, 0xBF, '-', '-', ' ', 'c', 'o', 'm', 'm', 'e', 'n',
      't',  '\n', 'S',  'E', 'L', 'E', 'C', 'T', ' ', '1', 0x00};
  VSILFILE* fp = VSIFOpenL(bom_file, "wb");
  ASSERT_NE(nullptr, fp);
  VSIFWriteL(bom_content, 1, sizeof(bom_content) - 1, fp);
  VSIFCloseL(fp);

  ConfigurableAlg alg;
  std::string sql_str;
  std::vector<std::string> sql_list;
  auto& s_arg = alg.AddArg("sql", 0, "SQL", &sql_str)
                    .SetReadFromFileAtSyntaxAllowed()
                    .SetRemoveSQLCommentsEnabled();
  auto& sl_arg = alg.AddArg("sql-list", 0, "SQL list", &sql_list)
                     .SetReadFromFileAtSyntaxAllowed()
                     .SetRemoveSQLCommentsEnabled();

  EXPECT_TRUE(s_arg.IsReadFromFileAtSyntaxAllowed());
  EXPECT_TRUE(s_arg.IsRemoveSQLCommentsEnabled());
  EXPECT_TRUE(s_arg.Set(std::string("@") + bom_file));
  EXPECT_EQ("SELECT 1", sql_str);

  EXPECT_TRUE(
      sl_arg.Set(std::vector<std::string>{std::string("@") + bom_file}));
  EXPECT_THAT(sql_list, ElementsAre("SELECT 1"));

  {
    QuietErrorGuard quiet;
    EXPECT_FALSE(s_arg.Set("@/vsimem/does_not_exist_at_all.txt"));
    EXPECT_FALSE(sl_arg.Set(
        std::vector<std::string>{"@/vsimem/does_not_exist_at_all.txt"}));
  }
  VSIUnlink(bom_file);

  // GDALArgDatasetValue move constructor, move assignment, BorrowDataset, etc.
  auto ds1 = CreateMemRaster();
  GDALDataset* ds1_raw = ds1.get();
  GDALArgDatasetValue v1(ds1_raw);
  EXPECT_EQ(ds1_raw, v1.GetDatasetRef());
  EXPECT_TRUE(v1.IsNameSet());

  GDALDataset* inc_ds = v1.GetDatasetIncreaseRefCount();
  EXPECT_EQ(ds1_raw, inc_ds);
  inc_ds->ReleaseRef();

  GDALArgDatasetValue v2(std::move(v1));
  EXPECT_EQ(nullptr, v1.GetDatasetRef());
  EXPECT_EQ(ds1_raw, v2.GetDatasetRef());

  GDALArgDatasetValue v3;
  v3 = std::move(v2);
  EXPECT_EQ(nullptr, v2.GetDatasetRef());
  EXPECT_EQ(ds1_raw, v3.GetDatasetRef());

  GDALArgDatasetValue v4;
  v4.BorrowDatasetFrom(v3);
  EXPECT_EQ(nullptr, v3.GetDatasetRef());
  EXPECT_EQ(ds1_raw, v4.GetDatasetRef());
  v4.SetDatasetOpenedByAlgorithm();
  EXPECT_TRUE(v4.HasDatasetBeenOpenedByAlgorithm());
  EXPECT_TRUE(v4.Close());

  GDALArgDatasetValue v_null(static_cast<GDALDataset*>(nullptr));
  EXPECT_EQ(nullptr, v_null.GetDatasetRef());
}

TEST(GdalAlgorithmTest, ValidationChoicesRangesCharCountAndListCounts) {
  ConfigurableAlg alg;
  std::string choice_str;
  std::vector<std::string> choice_list;
  std::string minchar_str;
  std::vector<std::string> minchar_list;
  int range_inc_int = 0;
  int range_exc_int = 0;
  std::vector<int> range_int_list;
  double range_inc_real = 0.0;
  double range_exc_real = 0.0;
  std::vector<double> range_real_list;
  std::vector<std::string> exact1_list;
  std::vector<int> exact2_list;
  std::vector<double> minmax_list;
  std::vector<GDALArgDatasetValue> ds_minmax_list;

  auto& c_s = alg.AddArg("choice-str", 0, "Choice str", &choice_str)
                  .SetChoices("Alpha", "Beta")
                  .SetHiddenChoices("Secret");
  const std::vector<std::string> choice_list_choices{"One", "Two"};
  auto& c_l = alg.AddArg("choice-list", 0, "Choice list", &choice_list)
                  .SetChoices(choice_list_choices)
                  .SetHiddenChoices("HiddenThree");

  auto& mc_s = alg.AddArg("minchar-str", 0, "MinChar str", &minchar_str)
                   .SetMinCharCount(3);
  auto& mc_l = alg.AddArg("minchar-list", 0, "MinChar list", &minchar_list)
                   .SetMinCharCount(3);

  auto& ri_inc = alg.AddArg("ri-inc", 0, "Int inc", &range_inc_int)
                     .SetMinValueIncluded(10)
                     .SetMaxValueIncluded(20);
  auto& ri_exc = alg.AddArg("ri-exc", 0, "Int exc", &range_exc_int)
                     .SetMinValueExcluded(10)
                     .SetMaxValueExcluded(20);
  auto& ri_list = alg.AddArg("ri-list", 0, "Int list", &range_int_list)
                      .SetMinValueIncluded(0)
                      .SetMaxValueIncluded(5);

  auto& rr_inc = alg.AddArg("rr-inc", 0, "Real inc", &range_inc_real)
                     .SetMinValueIncluded(1.0)
                     .SetMaxValueIncluded(2.0);
  auto& rr_exc = alg.AddArg("rr-exc", 0, "Real exc", &range_exc_real)
                     .SetMinValueExcluded(1.0)
                     .SetMaxValueExcluded(2.0);
  auto& rr_list = alg.AddArg("rr-list", 0, "Real list", &range_real_list)
                      .SetMinValueIncluded(0.0)
                      .SetMaxValueIncluded(5.0);

  auto& ex1 = alg.AddArg("exact1", 0, "Exact 1", &exact1_list)
                  .SetMinCount(1)
                  .SetMaxCount(1);
  auto& ex2 = alg.AddArg("exact2", 0, "Exact 2", &exact2_list)
                  .SetMinCount(2)
                  .SetMaxCount(2);
  auto& mm = alg.AddArg("minmax", 0, "MinMax", &minmax_list)
                 .SetMinCount(2)
                 .SetMaxCount(3);
  auto& dsmm = alg.AddArg("dsmm", 0, "DS MinMax", &ds_minmax_list)
                   .SetMinCount(2)
                   .SetMaxCount(3);

  // Choices
  EXPECT_TRUE(c_s.Set("alpha"));
  EXPECT_EQ("Alpha", choice_str);
  EXPECT_TRUE(c_s.Set("secret"));
  EXPECT_EQ("Secret", choice_str);
  EXPECT_TRUE(c_l.Set(std::vector<std::string>{"one", "hiddenthree"}));
  EXPECT_THAT(choice_list, ElementsAre("One", "HiddenThree"));

  {
    QuietErrorGuard quiet;
    EXPECT_FALSE(c_s.Set("Invalid"));
    EXPECT_FALSE(c_l.Set(std::vector<std::string>{"One", "Invalid"}));
  }

  // MinCharCount
  EXPECT_TRUE(mc_s.Set("abc"));
  EXPECT_TRUE(mc_l.Set(std::vector<std::string>{"abc", "defg"}));
  {
    QuietErrorGuard quiet;
    EXPECT_FALSE(mc_s.Set("ab"));
    EXPECT_FALSE(mc_l.Set(std::vector<std::string>{"abc", "xy"}));
  }

  // Int ranges
  EXPECT_TRUE(ri_inc.Set(10));
  EXPECT_TRUE(ri_inc.Set(20));
  EXPECT_TRUE(ri_exc.Set(11));
  EXPECT_TRUE(ri_exc.Set(19));
  EXPECT_TRUE(ri_list.Set(std::vector<int>{0, 5}));
  {
    QuietErrorGuard quiet;
    EXPECT_FALSE(ri_inc.Set(9));
    EXPECT_FALSE(ri_inc.Set(21));
    EXPECT_FALSE(ri_exc.Set(10));
    EXPECT_FALSE(ri_exc.Set(20));
    EXPECT_FALSE(ri_list.Set(std::vector<int>{1, -1}));
  }

  // Real ranges
  EXPECT_TRUE(rr_inc.Set(1.0));
  EXPECT_TRUE(rr_inc.Set(2.0));
  EXPECT_TRUE(rr_exc.Set(1.1));
  EXPECT_TRUE(rr_exc.Set(1.9));
  EXPECT_TRUE(rr_list.Set(std::vector<double>{0.0, 5.0}));
  {
    QuietErrorGuard quiet;
    EXPECT_FALSE(rr_inc.Set(0.9));
    EXPECT_FALSE(rr_inc.Set(2.1));
    EXPECT_FALSE(rr_exc.Set(1.0));
    EXPECT_FALSE(rr_exc.Set(2.0));
    EXPECT_FALSE(rr_list.Set(std::vector<double>{1.0, 6.0}));
  }

  // List counts
  EXPECT_TRUE(ex1.Set(std::vector<std::string>{"a"}));
  EXPECT_TRUE(ex2.Set(std::vector<int>{1, 2}));
  EXPECT_TRUE(mm.Set(std::vector<double>{1.0, 2.0}));
  {
    QuietErrorGuard quiet;
    EXPECT_FALSE(ex1.Set(std::vector<std::string>{"a", "b"}));
    EXPECT_FALSE(ex2.Set(std::vector<int>{1}));
    EXPECT_FALSE(mm.Set(std::vector<double>{1.0}));
    EXPECT_FALSE(mm.Set(std::vector<double>{1.0, 2.0, 3.0, 4.0}));
    std::vector<GDALArgDatasetValue> one_ds;
    one_ds.emplace_back("a.tif");
    EXPECT_FALSE(dsmm.Set(std::move(one_ds)));
  }

  // Standalone GDALAlgorithmArg ReportError (m_owner == nullptr)
  {
    QuietErrorGuard quiet;
    std::vector<int> standalone_vec;
    GDALAlgorithmArgDecl standalone_decl("standalone", 0, "desc",
                                         GAAT_INTEGER_LIST);
    standalone_decl.SetMinCount(2);
    GDALAlgorithmArg standalone_arg(standalone_decl, &standalone_vec);
    EXPECT_FALSE(standalone_arg.Set(std::vector<int>{1}));
  }
}

TEST(GdalAlgorithmTest, EscapeAndSerializeArgs) {
  EXPECT_EQ("simple", GDALAlgorithmArg::GetEscapedString("simple"));
  EXPECT_EQ("\"\\\"[ nested pipeline ]\\\"\"",
            GDALAlgorithmArg::GetEscapedString("\"[ nested pipeline ]\""));
  EXPECT_EQ("[ nested pipeline ]",
            GDALAlgorithmArg::GetEscapedString("[ nested pipeline ]"));
  EXPECT_EQ("\"a \\\"b\\\" \\\\c,d\"",
            GDALAlgorithmArg::GetEscapedString("a \"b\" \\c,d"));

  ConfigurableAlg alg;
  bool b_val = false;
  std::string s_val;
  int i_val = 0;
  double d_val = 0.0;
  GDALArgDatasetValue ds_val;
  std::vector<std::string> sl_packed;
  std::vector<std::string> sl_unpacked;
  std::vector<int> il_val;
  std::vector<double> dl_val;
  std::vector<GDALArgDatasetValue> dsl_val;

  auto& b_arg = alg.AddArg("flag", 0, "Flag", &b_val);
  auto& s_arg = alg.AddArg("str", 0, "Str", &s_val);
  auto& i_arg = alg.AddArg("int", 0, "Int", &i_val);
  auto& d_arg = alg.AddArg("real", 0, "Real", &d_val);
  auto& ds_arg = alg.AddArg("ds", 0, "DS", &ds_val);
  auto& slp_arg = alg.AddArg("slp", 0, "SLP", &sl_packed);
  auto& slu_arg =
      alg.AddArg("slu", 0, "SLU", &sl_unpacked).SetPackedValuesAllowed(false);
  auto& il_arg = alg.AddArg("il", 0, "IL", &il_val);
  auto& dl_arg = alg.AddArg("dl", 0, "DL", &dl_val);
  auto& dsl_arg = alg.AddArg("dsl", 0, "DSL", &dsl_val);

  std::string out;
  EXPECT_FALSE(b_arg.Serialize(out));

  EXPECT_TRUE(b_arg.Set(true));
  EXPECT_TRUE(b_arg.Serialize(out));
  EXPECT_EQ("--flag", out);

  EXPECT_TRUE(s_arg.Set("hello world"));
  EXPECT_TRUE(s_arg.Serialize(out));
  EXPECT_EQ("--str \"hello world\"", out);

  EXPECT_TRUE(i_arg.Set(42));
  EXPECT_TRUE(i_arg.Serialize(out));
  EXPECT_EQ("--int 42", out);

  EXPECT_TRUE(d_arg.Set(3.5));
  EXPECT_TRUE(d_arg.Serialize(out));
  EXPECT_EQ("--real 3.5", out);

  // Dataset with empty name fails to serialize
  EXPECT_TRUE(ds_arg.SetDatasetName(""));
  EXPECT_FALSE(ds_arg.Serialize(out));

  EXPECT_TRUE(ds_arg.SetDatasetName("/vsimem/file.tif"));
  EXPECT_TRUE(ds_arg.Serialize(out, /*absolutePath=*/true));
  EXPECT_EQ("--ds /vsimem/file.tif", out);

  // Relative path serialization with existing directory "."
  EXPECT_TRUE(ds_arg.SetDatasetName("."));
  EXPECT_TRUE(ds_arg.Serialize(out, /*absolutePath=*/true));
  EXPECT_THAT(out, HasSubstr("--ds "));

  EXPECT_TRUE(slp_arg.Set(std::vector<std::string>{"a", "b c"}));
  EXPECT_TRUE(slp_arg.Serialize(out));
  EXPECT_EQ("--slp a,\"b c\"", out);

  EXPECT_TRUE(slu_arg.Set(std::vector<std::string>{"a", "b"}));
  EXPECT_TRUE(slu_arg.Serialize(out));
  EXPECT_EQ("--slu a --slu b", out);

  EXPECT_TRUE(il_arg.Set(std::vector<int>{1, 2}));
  EXPECT_TRUE(il_arg.Serialize(out));
  EXPECT_EQ("--il 1,2", out);

  EXPECT_TRUE(dl_arg.Set(std::vector<double>{1.5, 2.5}));
  EXPECT_TRUE(dl_arg.Serialize(out));
  EXPECT_EQ("--dl 1.5,2.5", out);

  EXPECT_TRUE(
      dsl_arg.Set(std::vector<std::string>{"/vsimem/1.tif", "/vsimem/2.tif"}));
  EXPECT_TRUE(dsl_arg.Serialize(out, /*absolutePath=*/true));
  EXPECT_EQ("--dsl /vsimem/1.tif,/vsimem/2.tif", out);

  EXPECT_TRUE(dsl_arg.Set(std::vector<std::string>{"/vsimem/1.tif", ""}));
  EXPECT_FALSE(dsl_arg.Serialize(out));
}

TEST(GdalAlgorithmTest, CRSArgValidationAndAutoComplete) {
  GDALAllRegister();

  const std::string tif_earth = "/vsimem/crs_earth.tif";
  ASSERT_TRUE(CreateGTiffFile(tif_earth));

  const std::string tif_moon = "/vsimem/crs_moon.tif";
  {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, drv);
    std::unique_ptr<GDALDataset> ds(
        drv->Create(tif_moon.c_str(), 4, 4, 1, GDT_Byte, nullptr));
    OGRSpatialReference moon_srs;
    moon_srs.importFromWkt(
        R"(GEOGCRS["Moon 2000",DATUM["D_Moon_2000",ELLIPSOID["Moon_2000_IAU_IAG",1737400,0]],PRIMEM["Reference_Meridian",0],CS[ellipsoidal,2],AXIS["lat",north],AXIS["lon",east]])");
    ds->SetSpatialRef(&moon_srs);
    double gt[6] = {0.0, 1.0, 0.0, 10.0, 0.0, -1.0};
    ds->SetGeoTransform(gt);
  }

  ConfigurableAlg alg;
  std::vector<GDALArgDatasetValue> input_ds;
  std::string dst_crs;
  int non_str = 0;

  alg.AddInputDatasetArg(&input_ds);
  auto& crs_arg = alg.AddArg("dst-crs", 0, "Destination CRS", &dst_crs)
                      .SetIsCRSArg(/*noneAllowed=*/true, {"SPECIAL_CRS"});

  {
    QuietErrorGuard quiet;
    alg.AddArg("non-str-crs", 0, "Invalid", &non_str).SetIsCRSArg();
  }

  EXPECT_TRUE(crs_arg.Set("none"));
  EXPECT_TRUE(crs_arg.Set("null"));
  EXPECT_TRUE(crs_arg.Set("SPECIAL_CRS"));
  EXPECT_TRUE(crs_arg.Set("EPSG:4326"));
  {
    QuietErrorGuard quiet;
    EXPECT_FALSE(crs_arg.Set("TOTALLY_INVALID_CRS_XYZ"));
  }

  alg.SetCalledFromCommandLine();
  EXPECT_TRUE(crs_arg.Set("?"));

  // Autocomplete with Earth dataset
  ASSERT_TRUE(alg.GetArg("input")->Set(std::vector<std::string>{tif_earth}));
  auto choices_empty = crs_arg.GetAutoCompleteChoices("");
  EXPECT_THAT(choices_empty, Contains("none"));
  EXPECT_THAT(choices_empty, Contains("SPECIAL_CRS"));

  auto choices_4326 = crs_arg.GetAutoCompleteChoices("EPSG:4326");
  EXPECT_FALSE(choices_4326.empty());

  auto choices_multi = crs_arg.GetAutoCompleteChoices("EPSG:326");
  EXPECT_FALSE(choices_multi.empty());

  // Autocomplete with Moon dataset
  ASSERT_TRUE(alg.GetArg("input")->Set(std::vector<std::string>{tif_moon}));
  auto choices_moon = crs_arg.GetAutoCompleteChoices("IAU_2015:");
  EXPECT_FALSE(choices_moon.empty());

  VSIUnlink(tif_earth.c_str());
  VSIUnlink(tif_moon.c_str());
}

TEST(GdalAlgorithmTest, ParseCommandLineArgumentsComprehensive) {
  GDALAllRegister();

  // 1. Subalgorithms, "help" AWS syntax, duplicate call error
  {
    ConfigurableAlg top_alg("gdal");
    top_alg.SetCallPath({"gdal"});
    top_alg.RegisterSubAlgorithm<CustomSubAlg1>();
    top_alg.RegisterSubAlgorithm<CustomSubAlg2>();

    EXPECT_TRUE(top_alg.ParseCommandLineArguments({"help"}));
    EXPECT_TRUE(top_alg.IsHelpRequested());

    {
      QuietErrorGuard quiet;
      // Second call on same instance fails
      EXPECT_FALSE(top_alg.ParseCommandLineArguments({"help"}));
    }

    // Missing command name
    {
      QuietErrorGuard quiet;
      ConfigurableAlg empty_alg("gdal");
      empty_alg.SetCallPath({"gdal"});
      empty_alg.RegisterSubAlgorithm<CustomSubAlg1>();
      EXPECT_FALSE(empty_alg.ParseCommandLineArguments({}));

      // Unknown subcommand with suggestion and without suggestion
      ConfigurableAlg unk_alg1("gdal");
      unk_alg1.RegisterSubAlgorithm<CustomSubAlg1>();
      EXPECT_FALSE(unk_alg1.ParseCommandLineArguments({"sub_alph"}));

      ConfigurableAlg unk_alg2("gdal");
      unk_alg2.RegisterSubAlgorithm<CustomSubAlg1>();
      EXPECT_FALSE(
          unk_alg2.ParseCommandLineArguments({"completely_unknown_xyz"}));
    }

    // Valid subcommand invocation and execution
    ConfigurableAlg run_top("gdal");
    run_top.SetCallPath({"gdal"});
    run_top.SetReferencePathForRelativePaths("/vsimem");
    run_top.RegisterSubAlgorithm<CustomSubAlg1>();
    EXPECT_TRUE(run_top.ParseCommandLineArguments(
        {"sub_alpha", "-f", "--text=abc", "-q"}));
    EXPECT_EQ("sub_alpha", run_top.GetActualAlgorithm().GetName());
    EXPECT_FALSE(run_top.IsProgressBarRequested());
    EXPECT_TRUE(run_top.Run());
    EXPECT_TRUE(run_top.Finalize());
  }

  // 2. Options parsing: long, short, combined short, errors, suggestions
  {
    ConfigurableAlg alg;
    bool b1 = false;
    bool b2 = false;
    int ival = 0;
    double dval = 0.0;
    std::string sval;
    GDALArgDatasetValue dsval;
    std::vector<std::string> slval;
    std::vector<int> ilval;
    std::vector<double> dlval;
    std::vector<GDALArgDatasetValue> dslval;

    alg.AddArg("bool-one", 'a', "Bool 1", &b1).AddShortNameAlias('x');
    alg.AddArg("bool-two", 'b', "Bool 2", &b2);
    alg.AddArg("int-opt", 'i', "Int", &ival).AddAlias("integer-opt");
    alg.AddArg("real-opt", 'r', "Real", &dval).AddHiddenAlias("hidden-real");
    alg.AddArg("str-opt", 's', "Str", &sval);
    alg.AddArg("ds-opt", 0, "DS", &dsval).SetAutoOpenDataset(false);
    alg.AddArg("sl-opt", 0, "SL", &slval);
    alg.AddArg("il-opt", 0, "IL", &ilval);
    alg.AddArg("dl-opt", 0, "DL", &dlval);
    alg.AddArg("dsl-opt", 0, "DSL", &dslval).SetAutoOpenDataset(false);

    EXPECT_TRUE(alg.ParseCommandLineArguments(
        {"-xb", "--integer-opt=10", "--hidden-real", "2.5", "--str-opt", "hi",
         "--ds-opt=/vsimem/foo.tif", "--sl-opt=s1,s2", "--il-opt=1,2",
         "--dl-opt=1.5,2.5", "--dsl-opt={\"json\":1}",
         "--dsl-opt=d1.tif,d2.tif", "--config", "FOO=BAR"}));
    EXPECT_TRUE(b1);
    EXPECT_TRUE(b2);
    EXPECT_EQ(10, ival);
    EXPECT_DOUBLE_EQ(2.5, dval);
    EXPECT_EQ("hi", sval);
    EXPECT_EQ("/vsimem/foo.tif", dsval.GetName());
    EXPECT_THAT(slval, ElementsAre("s1", "s2"));
    EXPECT_THAT(ilval, ElementsAre(1, 2));
    EXPECT_THAT(dlval, ElementsAre(1.5, 2.5));
    EXPECT_EQ(3u, dslval.size());
  }

  // 3. Option parsing errors & edge cases
  {
    QuietErrorGuard quiet;

    // Unknown long option with suggestion
    {
      ConfigurableAlg alg;
      int v = 0;
      alg.AddArg("my-option", 0, "desc", &v);
      EXPECT_FALSE(alg.ParseCommandLineArguments({"--my-optio=1"}));
    }
    // Unknown long option without suggestion
    {
      ConfigurableAlg alg;
      EXPECT_FALSE(alg.ParseCommandLineArguments({"--zzzzzzzzzz"}));
    }
    // Unknown short option matching long option name (-help instead of --help)
    {
      ConfigurableAlg alg;
      EXPECT_FALSE(alg.ParseCommandLineArguments({"-help"}));
    }
    // Unknown short option close to long option name (-helx instead of --help)
    {
      ConfigurableAlg alg;
      EXPECT_FALSE(alg.ParseCommandLineArguments({"-helx"}));
    }
    // Unknown single-char short option
    {
      ConfigurableAlg alg;
      EXPECT_FALSE(alg.ParseCommandLineArguments({"-z"}));
    }
    // Combined short option where one is non-boolean
    {
      ConfigurableAlg alg;
      bool b = false;
      int i = 0;
      alg.AddArg("bool-a", 'a', "desc", &b);
      alg.AddArg("int-i", 'i', "desc", &i);
      EXPECT_FALSE(alg.ParseCommandLineArguments({"-ai"}));
    }
    // Missing value at end of tokens
    {
      ConfigurableAlg alg;
      int i = 0;
      alg.AddArg("int-i", 'i', "desc", &i);
      EXPECT_FALSE(alg.ParseCommandLineArguments({"--int-i"}));
    }
    // Missing value at end of tokens when SetParseForAutoCompletion() is set
    {
      ConfigurableAlg alg;
      int i = 0;
      alg.AddArg("int-i", 'i', "desc", &i);
      alg.SetParseForAutoCompletion();
      EXPECT_TRUE(alg.ParseCommandLineArguments({"--int-i"}));
    }
    // Scalar specified twice (with and without SkipIfAlreadySet)
    {
      ConfigurableAlg alg;
      int i = 0;
      auto& arg = alg.AddArg("int-i", 'i', "desc", &i);
      arg.Set(1);
      arg.SetSkipIfAlreadySet(true);
      EXPECT_TRUE(alg.ParseCommandLineArguments({"--int-i=2"}));
      EXPECT_EQ(1, i);
      EXPECT_FALSE(alg.ParseCommandLineArguments({"--int-i=2", "--int-i=3"}));
    }
    // List arg with RepeatedArgAllowed(false) specified twice
    {
      ConfigurableAlg alg;
      std::vector<int> il;
      alg.AddArg("il", 0, "desc", &il).SetRepeatedArgAllowed(false);
      EXPECT_FALSE(alg.ParseCommandLineArguments({"--il=1", "--il=2"}));
    }
    // Invalid boolean value, int value, real value, int list value, real list
    // value
    {
      ConfigurableAlg alg;
      bool b = false;
      alg.AddArg("bool-a", 0, "desc", &b);
      EXPECT_FALSE(alg.ParseCommandLineArguments({"--bool-a=maybe"}));
    }
    {
      ConfigurableAlg alg;
      bool b = false;
      alg.AddArg("bool-a", 0, "desc", &b);
      EXPECT_TRUE(alg.ParseCommandLineArguments({"--bool-a=false"}));
      EXPECT_FALSE(b);
    }
    {
      ConfigurableAlg alg;
      int i = 0;
      alg.AddArg("int-a", 0, "desc", &i);
      EXPECT_FALSE(alg.ParseCommandLineArguments({"--int-a=xyz"}));
    }
    {
      ConfigurableAlg alg;
      double d = 0;
      alg.AddArg("real-a", 0, "desc", &d);
      EXPECT_FALSE(alg.ParseCommandLineArguments({"--real-a=xyz"}));
    }
    {
      ConfigurableAlg alg;
      std::vector<int> il;
      alg.AddArg("il", 0, "desc", &il);
      EXPECT_FALSE(alg.ParseCommandLineArguments({"--il=1,xyz"}));
    }
    {
      ConfigurableAlg alg;
      std::vector<double> dl;
      alg.AddArg("dl", 0, "desc", &dl);
      EXPECT_FALSE(alg.ParseCommandLineArguments({"--dl=1.0,xyz"}));
    }
  }

  // 4. Positional arguments: 3-arg special case (<INPUT> <AUX>... <OUTPUT>)
  {
    ConfigurableAlg alg;
    std::string in, out;
    std::vector<std::string> aux;
    alg.AddArg("in", 0, "In", &in).SetPositional().SetRequired();
    alg.AddArg("aux", 0, "Aux", &aux).SetPositional().SetMinCount(1);
    alg.AddArg("out", 0, "Out", &out).SetPositional().SetRequired();

    EXPECT_TRUE(alg.ParseCommandLineArguments(
        {"in.tif", "a1.tif", "a2.tif", "out.tif"}));
    EXPECT_EQ("in.tif", in);
    EXPECT_THAT(aux, ElementsAre("a1.tif", "a2.tif"));
    EXPECT_EQ("out.tif", out);

    QuietErrorGuard quiet;
    ConfigurableAlg alg_short;
    alg_short.AddArg("in", 0, "In", &in).SetPositional().SetRequired();
    alg_short.AddArg("aux", 0, "Aux", &aux).SetPositional().SetMinCount(1);
    alg_short.AddArg("out", 0, "Out", &out).SetPositional().SetRequired();
    EXPECT_FALSE(alg_short.ParseCommandLineArguments({"in.tif", "out.tif"}));
  }

  // 5. Positional arguments: variable-count list first or last, and ambiguity
  // errors
  {
    // Variable list first, followed by fixed required scalar and fixed list
    ConfigurableAlg alg;
    std::vector<std::string> first_list;
    std::vector<int> mid_fixed_list;
    std::string last_req;
    alg.AddArg("first", 0, "First", &first_list).SetPositional().SetMinCount(1);
    alg.AddArg("mid", 0, "Mid", &mid_fixed_list)
        .SetPositional()
        .SetMinCount(2)
        .SetMaxCount(2);
    alg.AddArg("last", 0, "Last", &last_req).SetPositional().SetRequired();

    EXPECT_TRUE(
        alg.ParseCommandLineArguments({"f1", "f2", "10", "20", "end_val"}));
    EXPECT_THAT(first_list, ElementsAre("f1", "f2"));
    EXPECT_THAT(mid_fixed_list, ElementsAre(10, 20));
    EXPECT_EQ("end_val", last_req);

    // Variable list at the end, with first positional already set by option
    ConfigurableAlg alg_last;
    std::string p1;
    std::vector<std::string> p2;
    alg_last.AddArg("p1", 0, "P1", &p1).SetPositional().SetRequired();
    alg_last.AddArg("p2", 0, "P2", &p2).SetPositional().SetMinCount(1);
    EXPECT_TRUE(
        alg_last.ParseCommandLineArguments({"--p1=set_by_opt", "v1", "v2"}));
    EXPECT_EQ("set_by_opt", p1);
    EXPECT_THAT(p2, ElementsAre("v1", "v2"));

    QuietErrorGuard quiet;
    // Ambiguity 1: two variable-count list positional args
    {
      ConfigurableAlg amb1;
      std::vector<std::string> l1, l2;
      amb1.AddArg("l1", 0, "L1", &l1).SetPositional();
      amb1.AddArg("l2", 0, "L2", &l2).SetPositional();
      EXPECT_FALSE(amb1.ParseCommandLineArguments({"a", "b"}));
    }
    // Ambiguity 2: variable-count list followed by optional scalar
    {
      ConfigurableAlg amb2;
      std::vector<std::string> l1;
      std::string opt_s;
      amb2.AddArg("l1", 0, "L1", &l1).SetPositional();
      amb2.AddArg("opt-s", 0, "OptS", &opt_s).SetPositional();
      EXPECT_FALSE(amb2.ParseCommandLineArguments({"a", "b"}));
    }
    // Ambiguity 3: variable-count list in the middle (when first is already
    // set)
    {
      ConfigurableAlg amb3;
      std::string s1, s3;
      std::vector<std::string> l2;
      amb3.AddArg("s1", 0, "S1", &s1).SetPositional().SetRequired();
      amb3.AddArg("l2", 0, "L2", &l2).SetPositional();
      amb3.AddArg("s3", 0, "S3", &s3).SetPositional().SetRequired();
      EXPECT_FALSE(amb3.ParseCommandLineArguments({"--s1=val", "a", "b"}));
    }
    // Unexpected extra positional values
    {
      ConfigurableAlg extra;
      std::string s1;
      extra.AddArg("s1", 0, "S1", &s1).SetPositional();
      EXPECT_FALSE(extra.ParseCommandLineArguments({"a", "unexpected"}));
    }
    // Missing required positional value
    {
      ConfigurableAlg miss;
      std::string s1;
      miss.AddArg("s1", 0, "S1", &s1).SetPositional().SetRequired();
      EXPECT_FALSE(miss.ParseCommandLineArguments({}));
    }
  }

  // 6. Help value "?": single choice vs multiple choices
  {
    QuietErrorGuard quiet;
    ConfigurableAlg alg1;
    std::string c1;
    alg1.AddArg("c1", 0, "C1", &c1).SetChoices("only_one");
    alg1.SetCalledFromCommandLine();
    EXPECT_FALSE(alg1.ParseCommandLineArguments({"--c1=?"}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Single potential value"));

    ConfigurableAlg alg2;
    std::vector<std::string> c2;
    alg2.AddArg("c2", 0, "C2", &c2).SetChoices("opt_a", "opt_b");
    alg2.SetCalledFromCommandLine();
    EXPECT_FALSE(alg2.ParseCommandLineArguments({"--c2=?"}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Potential values"));
  }
}

TEST(GdalAlgorithmTest, ProcessDatasetArgAndValidateArguments) {
  GDALAllRegister();

  const std::string in_tif = "/vsimem/proc_in.tif";
  const std::string out_tif = "/vsimem/proc_out.tif";
  ASSERT_TRUE(CreateGTiffFile(in_tif));
  ASSERT_TRUE(CreateGTiffFile(out_tif));

  // 1. Auto-opening input dataset, overview delete special case, pre-mapped
  // dataset
  {
    ConfigurableAlg alg("delete");
    alg.SetCallPath({"gdal", "raster", "overview", "delete"});
    GDALArgDatasetValue in_ds;
    bool update = true;
    std::vector<std::string> oo;
    std::vector<std::string> ifmts;
    alg.AddInputDatasetArg(&in_ds, GDAL_OF_RASTER);
    alg.AddUpdateArg(&update);
    alg.AddOpenOptionsArg(&oo);
    alg.AddInputFormatsArg(&ifmts);

    EXPECT_TRUE(alg.ParseCommandLineArguments({in_tif, "--update"}));
    EXPECT_NE(nullptr, in_ds.GetDatasetRef());
    EXPECT_TRUE(alg.Finalize());
  }

  // 2. Pre-mapped dataset in m_oMapDatasetNameToDataset
  {
    ConfigurableAlg alg;
    GDALArgDatasetValue in_ds;
    alg.AddInputDatasetArg(&in_ds, GDAL_OF_RASTER);
    auto mem_ds = CreateMemRaster();
    test_gdal_algorithm::test_gdal_algorithm::SetMapDataset(alg, "mapped_ds",
                                                            mem_ds.get());
    EXPECT_TRUE(alg.ParseCommandLineArguments({"mapped_ds"}));
    EXPECT_EQ(mem_ds.get(), in_ds.GetDatasetRef());
    EXPECT_TRUE(alg.Finalize());
  }

  // 3. Input and output same dataset in update mode +
  // onlyInputSpecifiedInUpdateAndOutputNotRequired
  {
    ConfigurableAlg alg;
    GDALArgDatasetValue in_ds, out_ds;
    bool update = false;
    bool overwrite = false;
    std::vector<std::string> out_oo;
    alg.AddInputDatasetArg(&in_ds, GDAL_OF_RASTER, false);
    alg.AddOutputDatasetArg(&out_ds, GDAL_OF_RASTER, false)
        .SetDatasetInputFlags(GADV_NAME | GADV_OBJECT);
    alg.AddUpdateArg(&update);
    alg.AddOverwriteArg(&overwrite);
    alg.AddOutputOpenOptionsArg(&out_oo);

    EXPECT_TRUE(alg.ParseCommandLineArguments({"--input", in_tif, "--update"}));
    EXPECT_NE(nullptr, in_ds.GetDatasetRef());
    EXPECT_EQ(in_ds.GetDatasetRef(), out_ds.GetDatasetRef());
    EXPECT_TRUE(alg.Finalize());
  }

  // 4. Overwriting existing file vs error when --overwrite not set vs directory
  // error
  {
    QuietErrorGuard quiet;
    ConfigurableAlg alg1;
    GDALArgDatasetValue out_ds;
    bool overwrite = false;
    bool overwrite_layer = false;
    bool append = false;
    bool update = false;
    alg1.AddOutputDatasetArg(&out_ds, GDAL_OF_RASTER);
    alg1.AddOverwriteArg(&overwrite);
    alg1.AddUpdateArg(&update);
    alg1.AddOverwriteLayerArg(&overwrite_layer);
    alg1.AddAppendLayerArg(&append);

    // Fails because out_tif exists and --overwrite is not set
    EXPECT_FALSE(alg1.ParseCommandLineArguments({out_tif}));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("already exists"));

    // Succeeds when --overwrite is set (deletes existing file)
    ConfigurableAlg alg2;
    GDALArgDatasetValue out_ds2;
    bool overwrite2 = false;
    alg2.AddOutputDatasetArg(&out_ds2, GDAL_OF_RASTER);
    alg2.AddOverwriteArg(&overwrite2);
    EXPECT_TRUE(alg2.ParseCommandLineArguments({out_tif, "--overwrite"}));

    // Directory that is not a GDAL dataset cannot be overwritten
    VSIMkdir("/vsimem/non_gdal_dir", 0755);
    VSILFILE* fp = VSIFOpenL("/vsimem/non_gdal_dir/random.bin", "wb");
    VSIFWriteL("xyz", 1, 3, fp);
    VSIFCloseL(fp);

    ConfigurableAlg alg3;
    GDALArgDatasetValue out_ds3;
    bool overwrite3 = false;
    alg3.AddOutputDatasetArg(&out_ds3, GDAL_OF_RASTER);
    alg3.AddOverwriteArg(&overwrite3);
    EXPECT_FALSE(alg3.ParseCommandLineArguments(
        {"/vsimem/non_gdal_dir", "--overwrite"}));
    VSIUnlink("/vsimem/non_gdal_dir/random.bin");
    VSIRmdir("/vsimem/non_gdal_dir");
  }

  // 5. Output to "-" (/vsistdout/) turns on quiet; outputFormat=MEM sets empty
  // output name
  {
    ConfigurableAlg alg;
    GDALArgDatasetValue out_ds;
    std::string out_fmt;
    alg.AddOutputDatasetArg(&out_ds, GDAL_OF_RASTER, false);
    alg.AddOutputFormatArg(&out_fmt, /*bStreamAllowed=*/true);
    alg.AddProgressArg();

    EXPECT_TRUE(alg.ParseCommandLineArguments({"--output-format=MEM"}));
    EXPECT_TRUE(out_ds.IsNameSet());
    EXPECT_EQ("", out_ds.GetName());

    ConfigurableAlg alg_stdout;
    GDALArgDatasetValue out_stdout;
    alg_stdout.AddOutputDatasetArg(&out_stdout, GDAL_OF_RASTER);
    alg_stdout.AddProgressArg();
    EXPECT_TRUE(alg_stdout.ParseCommandLineArguments({"-"}));
    EXPECT_EQ("/vsistdout/", out_stdout.GetName());
    EXPECT_TRUE(alg_stdout.ValidateArguments());
    EXPECT_TRUE(alg_stdout.GetArg(GDAL_ARG_NAME_QUIET)->Get<bool>());
  }

  // 6. Mutual exclusion groups and required args in mutual exclusion group
  {
    QuietErrorGuard quiet;
    ConfigurableAlg alg;
    std::string a, b;
    alg.AddArg("opt-a", 0, "A", &a)
        .SetMutualExclusionGroup("grp1")
        .SetRequired();
    alg.AddArg("opt-b", 0, "B", &b).SetMutualExclusionGroup("grp1");

    // Setting opt-b satisfies the required constraint of opt-a in the same
    // group
    EXPECT_TRUE(alg.ParseCommandLineArguments({"--opt-b=val"}));

    // Setting both fails
    ConfigurableAlg alg_both;
    alg_both.AddArg("opt-a", 0, "A", &a).SetMutualExclusionGroup("grp1");
    alg_both.AddArg("opt-b", 0, "B", &b).SetMutualExclusionGroup("grp1");
    EXPECT_FALSE(
        alg_both.ParseCommandLineArguments({"--opt-a=1", "--opt-b=2"}));

    // Setting neither fails when opt-a is required
    ConfigurableAlg alg_none;
    alg_none.AddArg("opt-a", 0, "A", &a)
        .SetMutualExclusionGroup("grp1")
        .SetRequired();
    alg_none.AddArg("opt-b", 0, "B", &b).SetMutualExclusionGroup("grp1");
    EXPECT_FALSE(alg_none.ParseCommandLineArguments({}));
  }

  // 7. GAAT_DATASET_LIST auto-open with multiple datasets
  {
    const std::string in_tif2 = "/vsimem/proc_in2.tif";
    ASSERT_TRUE(CreateGTiffFile(in_tif2));

    ConfigurableAlg alg;
    std::vector<GDALArgDatasetValue> in_list;
    std::vector<std::string> oo, ifmts;
    bool update = false;
    alg.AddInputDatasetArg(&in_list, GDAL_OF_RASTER);
    alg.AddOpenOptionsArg(&oo);
    alg.AddInputFormatsArg(&ifmts);
    alg.AddUpdateArg(&update);

    EXPECT_TRUE(alg.ParseCommandLineArguments(
        {in_tif, in_tif2, "--input-format=GTiff", "--update"}));
    ASSERT_EQ(2u, in_list.size());
    EXPECT_NE(nullptr, in_list[0].GetDatasetRef());
    EXPECT_NE(nullptr, in_list[1].GetDatasetRef());
    EXPECT_TRUE(alg.Finalize());

    VSIUnlink(in_tif2.c_str());
  }

  VSIUnlink(in_tif.c_str());
  VSIUnlink(out_tif.c_str());
}

TEST(GdalAlgorithmTest, BuiltinHelperAddArgsAndValidations) {
  GDALAllRegister();

  const std::string tif_path = "/vsimem/helper_test.tif";
  const std::string json_path = "/vsimem/helper_test.geojson";
  ASSERT_TRUE(CreateGTiffFile(tif_path, /*bands=*/2));
  ASSERT_TRUE(CreateGeoJSONFile(json_path));

  // 1. AddFormats, ValidateFormat, FormatAutoCompleteFunction
  {
    ConfigurableAlg alg;
    std::string out_fmt;
    std::vector<std::string> in_fmts;
    bool update = false;
    alg.AddUpdateArg(&update);
    auto& of_arg = alg.AddOutputFormatArg(&out_fmt, /*bStreamAllowed=*/true,
                                          /*bGDALGAllowed=*/true);
    of_arg.AddMetadataItem(GAAMDI_VRT_COMPATIBLE, {"false"});
    of_arg.AddMetadataItem(GAAMDI_ALLOWED_FORMATS, {"CustomAllowed", "MEM"});
    of_arg.AddMetadataItem(GAAMDI_EXCLUDED_FORMATS, {"JPEG"});
    of_arg.AddMetadataItem(
        GAAMDI_REQUIRED_CAPABILITIES,
        {GDAL_DCAP_RASTER, GDAL_DCAP_CREATE, GDAL_DCAP_CREATECOPY,
         GDAL_DMD_EXTENSIONS, GDAL_ALG_DCAP_RASTER_OR_MULTIDIM_RASTER});

    alg.AddInputFormatsArg(&in_fmts);

    EXPECT_TRUE(of_arg.Set("stream"));
    EXPECT_TRUE(of_arg.Set("GDALG"));
    EXPECT_TRUE(of_arg.Set("CustomAllowed"));

    auto fmt_choices = of_arg.GetAutoCompleteChoices("");
    EXPECT_THAT(fmt_choices, Contains("GDALG"));
    EXPECT_THAT(fmt_choices, Contains("MEM"));
    EXPECT_THAT(fmt_choices, Not(Contains("VRT")));
    EXPECT_THAT(fmt_choices, Not(Contains("JPEG")));

    QuietErrorGuard quiet;
    EXPECT_FALSE(of_arg.Set("VRT"));
    EXPECT_FALSE(of_arg.Set("JPEG"));
    EXPECT_FALSE(of_arg.Set("NON_EXISTENT_DRIVER_XYZ"));
    // VRTSourcedRasterBand / other non-allowed format without extensions
    EXPECT_FALSE(of_arg.Set("VRT"));

    // Output dataset with .vrt when VRT_COMPATIBLE=false, and .gdalg.json when
    // GDALG not allowed
    ConfigurableAlg alg_out_vrt;
    GDALArgDatasetValue out_ds;
    std::string out_fmt2;
    alg_out_vrt.AddOutputDatasetArg(&out_ds, GDAL_OF_RASTER);
    alg_out_vrt
        .AddOutputFormatArg(&out_fmt2, /*bStreamAllowed=*/false,
                            /*bGDALGAllowed=*/false)
        .AddMetadataItem(GAAMDI_VRT_COMPATIBLE, {"false"});
    EXPECT_FALSE(alg_out_vrt.ParseCommandLineArguments({"/vsimem/test.vrt"}));
    EXPECT_FALSE(
        alg_out_vrt.ParseCommandLineArguments({"/vsimem/test.gdalg.json"}));
    EXPECT_FALSE(alg_out_vrt.GetArg(GDAL_ARG_NAME_OUTPUT_FORMAT)->Set("GDALG"));
  }

  // 2. AddOutputDataTypeArg, AddNodataArg, AddMemorySizeArg,
  // AddGeometryTypeArg, AddBBOXArg, AddBandArg, AddNumThreadsArg
  {
    ConfigurableAlg alg;
    std::string dt;
    std::string nodata_with_none, nodata_no_none;
    size_t mem_bytes = 0;
    std::string mem_str = "10MB";
    std::string geom_type;
    std::vector<double> bbox;
    int band = 1;
    std::vector<int> bands;
    int num_threads = 1;
    std::string num_threads_str = "ALL_CPUS";
    std::string out_str;
    bool stdout_flag = false;
    std::string active_layer;
    bool abs_path = false;
    std::string out_layer;
    std::vector<std::string> in_layers;
    std::string arr_name;
    std::vector<std::string> arr_names;

    alg.AddOutputDataTypeArg(&dt);
    auto& nd1 =
        alg.AddNodataArg(&nodata_with_none, /*noneAllowed=*/true, "nd1");
    auto& nd2 = alg.AddNodataArg(&nodata_no_none, /*noneAllowed=*/false, "nd2");
    auto& mem_arg =
        alg.AddMemorySizeArg(&mem_bytes, &mem_str, "mem", "Memory size");
    auto& geom_arg = alg.AddGeometryTypeArg(&geom_type);
    auto& bbox_arg = alg.AddBBOXArg(&bbox);
    ConfigurableAlg alg2;
    auto& band_arg = alg.AddBandArg(&band);
    auto& bands_arg = alg2.AddBandArg(&bands);
    auto& threads_arg = alg.AddNumThreadsArg(&num_threads, &num_threads_str);
    alg.AddOutputStringArg(&out_str);
    alg.AddStdoutArg(&stdout_flag);
    alg.AddActiveLayerArg(&active_layer);
    alg.AddAbsolutePathArg(&abs_path);
    alg.AddOutputLayerNameArg(&out_layer);
    alg.AddLayerNameArg(&in_layers);
    alg.AddArrayNameArg(&arr_name);
    alg2.AddArrayNameArg(&arr_names);

    EXPECT_TRUE(alg.GetArg("ot")->Set("Float32"));
    EXPECT_EQ("Float32", dt);

    EXPECT_TRUE(nd1.Set("none"));
    EXPECT_TRUE(nd1.Set("-9999.5"));
    EXPECT_TRUE(nd1.Set("nan"));
    EXPECT_TRUE(nd2.Set("inf"));
    {
      QuietErrorGuard quiet;
      EXPECT_FALSE(nd2.Set("none"));
      EXPECT_FALSE(nd1.Set("not_a_number"));
    }

    EXPECT_TRUE(mem_arg.Set("20MB"));
    EXPECT_EQ(20u * 1024u * 1024u, mem_bytes);
    {
      QuietErrorGuard quiet;
      EXPECT_FALSE(mem_arg.Set("1024"));  // missing unit
      EXPECT_FALSE(mem_arg.Set("invalid_mem"));
    }

    EXPECT_FALSE(geom_arg.GetAutoCompleteChoices("POINT").empty());
    EXPECT_TRUE(geom_arg.Set("POINTZ"));
    EXPECT_TRUE(geom_arg.Set("GEOMETRY"));
    {
      QuietErrorGuard quiet;
      EXPECT_FALSE(geom_arg.Set("NOT_A_GEOM_TYPE"));
    }

    EXPECT_TRUE(bbox_arg.Set(std::vector<double>{1.0, 2.0, 3.0, 4.0}));
    {
      QuietErrorGuard quiet;
      EXPECT_FALSE(bbox_arg.Set(std::vector<double>{5.0, 2.0, 3.0, 4.0}));
    }

    EXPECT_TRUE(band_arg.Set(1));
    EXPECT_TRUE(bands_arg.Set(std::vector<int>{1, 2}));
    {
      QuietErrorGuard quiet;
      EXPECT_FALSE(band_arg.Set(0));
      EXPECT_FALSE(bands_arg.Set(std::vector<int>{1, 0}));
    }

    CPLSetThreadLocalConfigOption("GDAL_NUM_THREADS", "4");
    EXPECT_TRUE(threads_arg.Set("ALL_CPUS"));
    EXPECT_GE(num_threads, 1);
    EXPECT_TRUE(threads_arg.Set("2"));
    EXPECT_LE(num_threads, 2);
    {
      QuietErrorGuard quiet;
      EXPECT_FALSE(threads_arg.Set("invalid_threads"));
    }
    CPLSetThreadLocalConfigOption("GDAL_NUM_THREADS", nullptr);
  }

  // 3. ValidateBandArg against actual dataset raster count
  {
    QuietErrorGuard quiet;
    ConfigurableAlg alg;
    GDALArgDatasetValue in_ds;
    int band = 0;
    alg.AddInputDatasetArg(&in_ds, GDAL_OF_RASTER);
    alg.AddBandArg(&band);
    EXPECT_FALSE(alg.ParseCommandLineArguments({tif_path, "--band=5"}));

    ConfigurableAlg alg_ok;
    GDALArgDatasetValue in_ds_ok;
    std::vector<int> bands_ok;
    alg_ok.AddInputDatasetArg(&in_ds_ok, GDAL_OF_RASTER);
    alg_ok.AddBandArg(&bands_ok);
    EXPECT_TRUE(alg_ok.ParseCommandLineArguments({tif_path, "--band=1,2"}));
    EXPECT_TRUE(alg_ok.RunPreStepPipelineValidations());
    EXPECT_TRUE(alg_ok.Finalize());
  }

  // 4. AddFieldTypeSubtypeArg, SetAutoCompleteFunctionForLayerName,
  // SetAutoCompleteFunctionForFieldName
  {
    ConfigurableAlg alg;
    std::vector<GDALArgDatasetValue> in_ds_list;
    std::string layer_name;
    std::string field_name;
    OGRFieldType ftype = OFTMaxType;
    OGRFieldSubType fsubtype = OFSTNone;
    std::string ftype_str = "Integer";

    auto& ds_arg = alg.AddInputDatasetArg(&in_ds_list, GDAL_OF_VECTOR);
    auto& lyr_arg = alg.AddLayerNameArg(&layer_name);
    auto& fld_arg = alg.AddFieldNameArg(&field_name);
    auto& ft_arg =
        alg.AddFieldTypeSubtypeArg(&ftype, &fsubtype, &ftype_str, "field-type");

    ConfigurableAlg::SetAutoCompleteFunctionForLayerName(lyr_arg, ds_arg);
    ConfigurableAlg::SetAutoCompleteFunctionForFieldName(fld_arg, lyr_arg,
                                                         in_ds_list);

    EXPECT_EQ(OFTInteger, ftype);
    EXPECT_FALSE(ft_arg.GetAutoCompleteChoices("Int").empty());
    EXPECT_TRUE(ft_arg.Set("Boolean"));
    EXPECT_EQ(OFTInteger, ftype);
    EXPECT_EQ(OFSTBoolean, fsubtype);
    EXPECT_TRUE(ft_arg.Set("Float32"));
    EXPECT_EQ(OFTReal, ftype);
    EXPECT_EQ(OFSTFloat32, fsubtype);
    EXPECT_TRUE(ft_arg.Set("JSON"));
    EXPECT_EQ(OFTString, ftype);
    EXPECT_EQ(OFSTJSON, fsubtype);
    {
      QuietErrorGuard quiet;
      EXPECT_FALSE(ft_arg.Set("InvalidFieldType"));
    }

    ASSERT_TRUE(ds_arg.Set(std::vector<std::string>{json_path}));
    auto layers = lyr_arg.GetAutoCompleteChoices("");
    EXPECT_THAT(layers, Contains("test_layer"));
    auto layers_exact = lyr_arg.GetAutoCompleteChoices("test_layer");
    EXPECT_THAT(layers_exact, ElementsAre("test_layer"));

    // Field autocomplete with empty layer name (scans all layers) and specific
    // layer name
    auto fields_all = fld_arg.GetAutoCompleteChoices("");
    EXPECT_THAT(fields_all, Contains("name_field"));
    EXPECT_THAT(fields_all, Contains("int_field"));

    ASSERT_TRUE(lyr_arg.Set("test_layer"));
    auto fields_exact = fld_arg.GetAutoCompleteChoices("name_field");
    EXPECT_THAT(fields_exact, ElementsAre("name_field"));
  }

  // 5. AddOptionsSuggestions, OpenOptions, CreationOptions,
  // LayerCreationOptions, PixelFunction
  {
    const char* test_xml = R"xml(
      <OptionList>
        <Option name="SEL_OPT" type="string-select" scope="raster">
          <Value>VAL_A</Value>
          <Value>VAL_B</Value>
        </Option>
        <Option name="BOOL_OPT" type="boolean" scope="vector"/>
        <Option name="SMALL_INT" type="int" min="1" max="3"/>
        <Option name="RANGE_BOTH" type="int" min="1" max="100"/>
        <Option name="RANGE_MIN" type="int" min="0"/>
        <Option name="RANGE_MAX" type="int" max="10"/>
        <Option name="DESC_ONLY" type="string" description="Some text"/>
      </OptionList>
    )xml";

    std::vector<std::string> suggestions;
    EXPECT_FALSE(ConfigurableAlg::AddOptionsSuggestions(nullptr, GDAL_OF_RASTER,
                                                        "", suggestions));
    EXPECT_FALSE(ConfigurableAlg::AddOptionsSuggestions(
        test_xml, GDAL_OF_RASTER | GDAL_OF_VECTOR, "", suggestions));
    EXPECT_THAT(suggestions, Contains("SEL_OPT="));
    EXPECT_THAT(suggestions, Contains("BOOL_OPT="));

    suggestions.clear();
    EXPECT_TRUE(ConfigurableAlg::AddOptionsSuggestions(
        test_xml, GDAL_OF_RASTER, "SEL_OPT=", suggestions));
    EXPECT_THAT(suggestions, ElementsAre("VAL_A", "VAL_B"));

    suggestions.clear();
    EXPECT_TRUE(ConfigurableAlg::AddOptionsSuggestions(
        test_xml, GDAL_OF_VECTOR, "BOOL_OPT=", suggestions));
    EXPECT_THAT(suggestions, ElementsAre("NO", "YES"));

    suggestions.clear();
    EXPECT_TRUE(ConfigurableAlg::AddOptionsSuggestions(
        test_xml, GDAL_OF_VECTOR, "BOOL_OPT=YES", suggestions));
    EXPECT_THAT(suggestions, ElementsAre("BOOL_OPT=YES"));

    suggestions.clear();
    EXPECT_TRUE(ConfigurableAlg::AddOptionsSuggestions(
        test_xml, GDAL_OF_RASTER, "SMALL_INT=", suggestions));
    EXPECT_THAT(suggestions, ElementsAre("1", "2", "3"));

    suggestions.clear();
    EXPECT_TRUE(ConfigurableAlg::AddOptionsSuggestions(
        test_xml, GDAL_OF_RASTER, "RANGE_BOTH=", suggestions));
    EXPECT_EQ(2u, suggestions.size());

    suggestions.clear();
    EXPECT_TRUE(ConfigurableAlg::AddOptionsSuggestions(
        test_xml, GDAL_OF_RASTER, "RANGE_MIN=", suggestions));
    EXPECT_EQ(2u, suggestions.size());

    suggestions.clear();
    EXPECT_TRUE(ConfigurableAlg::AddOptionsSuggestions(
        test_xml, GDAL_OF_RASTER, "RANGE_MAX=", suggestions));
    EXPECT_EQ(2u, suggestions.size());

    suggestions.clear();
    EXPECT_TRUE(ConfigurableAlg::AddOptionsSuggestions(
        test_xml, GDAL_OF_RASTER, "DESC_ONLY=", suggestions));
    EXPECT_EQ(2u, suggestions.size());

    // OpenOptions / CreationOptions / LayerCreationOptions / PixelFunction args
    ConfigurableAlg alg;
    GDALArgDatasetValue in_ds, out_ds;
    std::vector<std::string> in_fmts;
    std::string out_fmt;
    std::vector<std::string> oo, co, lco;
    std::string pf_name;
    std::vector<std::string> pf_args;

    alg.AddInputDatasetArg(&in_ds);
    alg.AddInputFormatsArg(&in_fmts);
    alg.AddOutputDatasetArg(&out_ds);
    alg.AddOutputFormatArg(&out_fmt);
    auto& oo_arg = alg.AddOpenOptionsArg(&oo);
    auto& co_arg = alg.AddCreationOptionsArg(&co);
    auto& lco_arg = alg.AddLayerCreationOptionsArg(&lco);
    auto& pf_arg = alg.AddPixelFunctionNameArg(&pf_name);
    auto& pfa_arg = alg.AddPixelFunctionArgsArg(&pf_args);

    // Key=Value splitting on comma when unambiguous
    EXPECT_TRUE(oo_arg.Set(std::vector<std::string>{"A=B,C=\"D,E\""}));
    EXPECT_THAT(oo, ElementsAre("A=B", "C=\"D,E\""));
    {
      QuietErrorGuard quiet;
      EXPECT_FALSE(oo_arg.Set(std::vector<std::string>{"NO_EQUAL_SIGN"}));
    }

    // Autocomplete open/creation/layer-creation options by filename extension
    // first
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT)->SetDatasetName(tif_path));
    EXPECT_FALSE(oo_arg.GetAutoCompleteChoices("").empty());
    ASSERT_TRUE(
        alg.GetArg(GDAL_ARG_NAME_OUTPUT)->SetDatasetName("/vsimem/o.tif"));
    EXPECT_FALSE(co_arg.GetAutoCompleteChoices("").empty());
    ASSERT_TRUE(
        alg.GetArg(GDAL_ARG_NAME_OUTPUT)->SetDatasetName("/vsimem/o.geojson"));
    EXPECT_FALSE(lco_arg.GetAutoCompleteChoices("").empty());

    // Now with explicit format
    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_INPUT_FORMAT)
                    ->Set(std::vector<std::string>{"GTiff"}));
    EXPECT_FALSE(oo_arg.GetAutoCompleteChoices("").empty());

    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_OUTPUT_FORMAT)->Set("GTiff"));
    EXPECT_FALSE(co_arg.GetAutoCompleteChoices("").empty());

    ASSERT_TRUE(alg.GetArg(GDAL_ARG_NAME_OUTPUT_FORMAT)->Set("GeoJSON"));
    EXPECT_FALSE(lco_arg.GetAutoCompleteChoices("").empty());

    // Pixel function args autocomplete
    EXPECT_TRUE(pf_arg.Set("min"));
    EXPECT_FALSE(pfa_arg.GetAutoCompleteChoices("").empty());
    EXPECT_TRUE(pf_arg.Set("scale"));
    EXPECT_FALSE(pfa_arg.GetAutoCompleteChoices("").empty());
    pf_name = "nonexistent_pf";
    EXPECT_FALSE(pfa_arg.GetAutoCompleteChoices("").empty());
  }

  // 6. AutoCompleteArrayName with multidimensional VRT dataset
  {
    const char* mdim_vrt = "/vsimem/test_mdim.vrt";
    const char* mdim_xml = R"xml(<VRTDataset>
      <Group name="/">
        <Array name="temperature">
          <DataType>Float32</DataType>
          <Dimension name="x" size="2"/>
        </Array>
      </Group>
    </VRTDataset>)xml";
    VSILFILE* fp = VSIFOpenL(mdim_vrt, "wb");
    ASSERT_NE(nullptr, fp);
    VSIFWriteL(mdim_xml, 1, strlen(mdim_xml), fp);
    VSIFCloseL(fp);

    ConfigurableAlg alg1;
    GDALArgDatasetValue in_ds;
    std::vector<std::string> in_fmts, oo;
    std::string arr;
    alg1.AddInputDatasetArg(&in_ds, GDAL_OF_MULTIDIM_RASTER);
    alg1.AddInputFormatsArg(&in_fmts);
    alg1.AddOpenOptionsArg(&oo);
    auto& arr_arg1 = alg1.AddArrayNameArg(&arr);
    ASSERT_TRUE(alg1.GetArg(GDAL_ARG_NAME_INPUT)->SetDatasetName(mdim_vrt));
    EXPECT_THAT(arr_arg1.GetAutoCompleteChoices(""), Contains("/temperature"));

    ConfigurableAlg alg2;
    std::vector<GDALArgDatasetValue> in_ds_list;
    std::vector<std::string> arr_list;
    alg2.AddInputDatasetArg(&in_ds_list, GDAL_OF_MULTIDIM_RASTER);
    auto& arr_arg2 = alg2.AddArrayNameArg(&arr_list);
    ASSERT_TRUE(alg2.GetArg(GDAL_ARG_NAME_INPUT)
                    ->Set(std::vector<std::string>{mdim_vrt}));
    EXPECT_THAT(arr_arg2.GetAutoCompleteChoices(""), Contains("/temperature"));

    VSIUnlink(mdim_vrt);
  }

  // 7. AddOverwriteLayerArg and AddAppendLayerArg actions and missing --update
  // error
  {
    ConfigurableAlg alg_ok;
    bool update = false, ov_lyr = false, app_lyr = false;
    alg_ok.AddUpdateArg(&update);
    auto& ov_arg = alg_ok.AddOverwriteLayerArg(&ov_lyr);
    auto& app_arg = alg_ok.AddAppendLayerArg(&app_lyr);
    EXPECT_TRUE(ov_arg.Set(true));
    EXPECT_TRUE(update);
    update = false;
    EXPECT_TRUE(app_arg.Set(true));
    EXPECT_TRUE(update);

    QuietErrorGuard quiet;
    ConfigurableAlg alg_no_upd1;
    alg_no_upd1.AddOverwriteLayerArg(&ov_lyr);
    EXPECT_FALSE(alg_no_upd1.ValidateArguments());

    ConfigurableAlg alg_no_upd2;
    alg_no_upd2.AddAppendLayerArg(&app_lyr);
    EXPECT_FALSE(alg_no_upd2.ValidateArguments());
  }

  VSIUnlink(tif_path.c_str());
  VSIUnlink(json_path.c_str());
}

TEST(GdalAlgorithmTest, GDALGOutputAndStreamedExecution) {
  GDALAllRegister();

  const std::string gdalg_file = "/vsimem/test_out.gdalg.json";
  std::string out_json;
  EXPECT_TRUE(
      GDALAlgorithm::SaveGDALG("", out_json, "gdal raster info in.tif"));
  EXPECT_THAT(out_json, HasSubstr("gdal_streamed_alg"));

  ConfigurableAlg alg("convert");
  alg.SetCallPath({"gdal", "raster", "convert"});
  test_gdal_algorithm::test_gdal_algorithm::SetSupportsStreamedOutput(alg,
                                                                      true);

  GDALArgDatasetValue in_ds, out_ds;
  std::string out_fmt;
  bool overwrite = false;
  alg.AddInputDatasetArg(&in_ds, GDAL_OF_RASTER).SetAutoOpenDataset(false);
  alg.AddOutputDatasetArg(&out_ds, GDAL_OF_RASTER);
  alg.AddOutputFormatArg(&out_fmt, /*bStreamAllowed=*/true,
                         /*bGDALGAllowed=*/true);
  alg.AddOverwriteArg(&overwrite);

  EXPECT_TRUE(alg.ParseCommandLineArguments({"/vsimem/in.tif", gdalg_file}));
  EXPECT_TRUE(alg.IsGDALGOutput());
  EXPECT_TRUE(alg.Run());
  EXPECT_FALSE(alg.run_called_);  // Handled by ProcessGDALGOutput!

  // Running again without --overwrite should fail because gdalg_file now exists
  {
    QuietErrorGuard quiet;
    ConfigurableAlg alg2("convert");
    alg2.SetCallPath({"gdal", "raster", "convert"});
    test_gdal_algorithm::test_gdal_algorithm::SetSupportsStreamedOutput(alg2,
                                                                        true);
    GDALArgDatasetValue in_ds2, out_ds2;
    std::string out_fmt2;
    bool overwrite2 = false;
    alg2.AddInputDatasetArg(&in_ds2, GDAL_OF_RASTER).SetAutoOpenDataset(false);
    alg2.AddOutputDatasetArg(&out_ds2, GDAL_OF_RASTER)
        .SetAutoOpenDataset(false);
    alg2.AddOutputFormatArg(&out_fmt2, true, true);
    alg2.AddOverwriteArg(&overwrite2);
    // ProcessGDALGOutput checks VSIStatL when --output-format=GDALG is passed
    // and --overwrite is false (bypassing ProcessDatasetArg
    // ifAutoOpenDataset=false)
    in_ds2.Set("/vsimem/in.tif");
    out_ds2.Set(gdalg_file);
    alg2.GetArg(GDAL_ARG_NAME_OUTPUT_FORMAT)->Set("GDALG");
    EXPECT_EQ(ConfigurableAlg::ProcessGDALGOutputRet::GDALG_ERROR,
              alg2.ProcessGDALGOutput());
  }

  // Streamed output execution safety check
  {
    QuietErrorGuard quiet;
    ConfigurableAlg stream_alg("convert");
    std::string fmt;
    stream_alg.AddOutputFormatArg(&fmt, /*bStreamAllowed=*/true);
    stream_alg.SetExecutionForStreamedOutput();
    ASSERT_TRUE(stream_alg.GetArg(GDAL_ARG_NAME_OUTPUT_FORMAT)->Set("GTiff"));
    EXPECT_FALSE(stream_alg.Run());

    ASSERT_TRUE(stream_alg.GetArg(GDAL_ARG_NAME_OUTPUT_FORMAT)->Set("stream"));
    EXPECT_TRUE(stream_alg.Run());
  }

  VSIUnlink(gdalg_file.c_str());
}

TEST(GdalAlgorithmTest, UsageForCLIAndJSONAndAutoComplete) {
  GDALAllRegister();

  // 1. Container algorithm with sub-algorithms
  {
    ConfigurableAlg container("gdal", "Main GDAL CLI", "/programs/gdal.html");
    container.SetCallPath({"gdal"});
    container.RegisterSubAlgorithm<CustomSubAlg1>();
    container.RegisterSubAlgorithm<CustomSubAlg2>();

    GDALAlgorithm::UsageOptions uopts;
    uopts.isPipelineStep = true;
    std::string cli_step =
        container.GetUsageForCLI(/*shortUsage=*/false, uopts);
    EXPECT_THAT(cli_step, HasSubstr("<COMMAND>"));
    EXPECT_THAT(cli_step, HasSubstr("sub_alpha"));
    EXPECT_THAT(cli_step, HasSubstr("(alias: sa)"));

    std::string cli_short = container.GetUsageForCLI(/*shortUsage=*/true);
    EXPECT_THAT(cli_short, HasSubstr("Try 'gdal --help' for help."));

    std::string json_usage = container.GetUsageAsJSON();
    EXPECT_THAT(json_usage, HasSubstr("sub_algorithms"));

    // AutoComplete on subalgorithms: prefix matching 2 subalgorithms ("sub_")
    std::vector<std::string> ac_args = {"sub_"};
    auto ac_res =
        container.GetAutoComplete(ac_args, /*lastWordIsComplete=*/false,
                                  /*showAllOptions=*/false);
    EXPECT_THAT(ac_res, Contains("sub_alpha"));
    EXPECT_THAT(ac_res, Contains("sub_beta"));

    // AutoComplete recursing into sub_alpha
    std::vector<std::string> ac_sub_args = {"sub_alpha", "--"};
    auto ac_sub_res =
        container.GetAutoComplete(ac_sub_args, false, /*showAllOptions=*/false);
    EXPECT_THAT(ac_sub_res, Contains("--flag"));
    EXPECT_THAT(ac_sub_res, Contains("--text"));
  }

  // 2. Leaf algorithm with rich argument types for CLI and JSON usage
  {
    ConfigurableAlg alg("rich_alg", "Rich description", "/programs/rich.html");
    alg.SetCallPath({"gdal", "raster", "rich_alg"});
    test_gdal_algorithm::test_gdal_algorithm::SetLongDescription(
        alg, "Detailed long description.");
    test_gdal_algorithm::test_gdal_algorithm::SetSupportsStreamedOutput(alg,
                                                                        true);
    alg.AllowArbitraryLongNameArgs();

    GDALArgDatasetValue in_ds, out_ds;
    std::vector<GDALArgDatasetValue> opt_ds_list;
    bool b_def_true = true;
    std::string s_def = "hello";
    int i_def = 5;
    double d_def = 1.5;
    std::vector<std::string> sl_def1, sl_def2;
    std::vector<int> il_def1, il_def2;
    std::vector<double> dl_def1, dl_def2;
    std::string out_only_str;

    alg.AddInputDatasetArg(&in_ds, GDAL_OF_RASTER);
    alg.AddOutputDatasetArg(&out_ds, GDAL_OF_VECTOR);
    alg.AddArg("aux-ds", 0, "Aux datasets", &opt_ds_list,
               GDAL_OF_MULTIDIM_RASTER)
        .SetPositional();

    alg.AddArg("bool-true", 'b', "Bool default true", &b_def_true)
        .SetDefault(true)
        .AddShortNameAlias('B')
        .SetMutualExclusionGroup("mex");
    alg.AddArg("str-opt", 's', "String opt", &s_def)
        .SetDefault("hello")
        .SetChoices("hello", "world")
        .SetMutualExclusionGroup("mex")
        .SetCategory(GAAC_ADVANCED);
    alg.AddArg("int-opt", 0, "Int opt", &i_def)
        .SetDefault(5)
        .SetMinValueIncluded(0)
        .SetMaxValueIncluded(10)
        .SetCategory(GAAC_ESOTERIC);
    alg.AddArg("real-opt", 0, "Real opt", &d_def)
        .SetDefault(1.5)
        .SetMinValueExcluded(0.0)
        .SetMaxValueExcluded(10.0)
        .SetCategory("CustomCat");

    alg.AddArg("sl1", 0, "SL1", &sl_def1)
        .SetDefault(std::vector<std::string>{"only1"})
        .SetMinCount(2)
        .SetMaxCount(2);
    alg.AddArg("sl2", 0, "SL2", &sl_def2)
        .SetDefault(std::vector<std::string>{"a", "b"})
        .SetMinCount(1)
        .SetMaxCount(3);
    alg.AddArg("il1", 0, "IL1", &il_def1)
        .SetDefault(std::vector<int>{10})
        .SetMinCount(2);
    alg.AddArg("il2", 0, "IL2", &il_def2).SetDefault(std::vector<int>{10, 20});
    alg.AddArg("dl1", 0, "DL1", &dl_def1).SetDefault(std::vector<double>{2.5});
    alg.AddArg("dl2", 0, "DL2", &dl_def2)
        .SetDefault(std::vector<double>{2.5, 3.5});
    alg.AddOutputStringArg(&out_only_str);

    std::string cli_full = alg.GetUsageForCLI(/*shortUsage=*/false);
    EXPECT_THAT(cli_full, HasSubstr("Positional arguments:"));
    EXPECT_THAT(cli_full, HasSubstr("(created by algorithm)"));
    EXPECT_THAT(cli_full, HasSubstr("(default: true)"));
    EXPECT_THAT(cli_full, HasSubstr("Mutually exclusive with"));
    EXPECT_THAT(cli_full, HasSubstr("CustomCat Options:"));
    EXPECT_THAT(cli_full, HasSubstr("Esoteric Options:"));
    EXPECT_THAT(cli_full, HasSubstr("--<user-provided-option>=<value>"));
    EXPECT_THAT(cli_full, HasSubstr("WARNING: the gdal command"));

    std::string cli_short = alg.GetUsageForCLI(/*shortUsage=*/true);
    EXPECT_THAT(cli_short, HasSubstr("Try 'gdal raster rich_alg --help'"));

    std::string json_str = alg.GetUsageAsJSON();
    EXPECT_THAT(json_str, HasSubstr("user_provided_arguments_allowed"));
    EXPECT_THAT(json_str, HasSubstr("supports_streamed_output"));
    EXPECT_THAT(json_str, HasSubstr("output_arguments"));
    EXPECT_THAT(json_str, HasSubstr("input_output_arguments"));

    // Arbitrary user-provided arguments via GetArg
    EXPECT_NE(nullptr, alg.GetArg("--custom.overwrite"));
    EXPECT_NE(nullptr, alg.GetArg("--oo"));
    EXPECT_NE(nullptr, alg.GetArg("--co"));
    EXPECT_NE(nullptr, alg.GetArg("--of"));
    EXPECT_NE(nullptr, alg.GetArg("--if"));
    EXPECT_NE(nullptr, alg.GetArg("--custom-str-arg"));

    // AutoComplete options & values
    std::vector<std::string> ac1 = {"-"};
    EXPECT_FALSE(
        alg.GetAutoComplete(ac1, false, /*showAllOptions=*/true).empty());

    std::vector<std::string> ac2 = {"--str"};
    EXPECT_THAT(alg.GetAutoComplete(ac2, false, false), Contains("--str-opt"));

    std::vector<std::string> ac3 = {"--str-opt="};
    EXPECT_THAT(alg.GetAutoComplete(ac3, false, false),
                ElementsAre("hello", "world"));

    // When value already matches the last choice, returns empty
    std::vector<std::string> ac4 = {"--str-opt=world"};
    EXPECT_TRUE(alg.GetAutoComplete(ac4, false, false).empty());

    // When option has no choices or autocomplete function, returns description
    // hint
    std::vector<std::string> ac5 = {"--int-opt", "5"};
    EXPECT_EQ(2u, alg.GetAutoComplete(ac5, false, false).size());

    // Filename completion via SetAutoCompleteFunctionForFilename
    VSILFILE* fp = VSIFOpenL("/vsimem/ac_file.tif", "wb");
    VSIFCloseL(fp);
    VSIMkdir("/vsimem/ac_subdir", 0755);

    std::vector<std::string> ac_vsi = {"/vsim"};
    EXPECT_THAT(alg.GetAutoComplete(ac_vsi, false, false),
                Contains("/vsimem/"));

    std::vector<std::string> ac_dir = {"/vsimem/"};
    auto dir_res = alg.GetAutoComplete(ac_dir, false, false);
    EXPECT_THAT(dir_res, Contains("/vsimem/ac_file.tif"));
    EXPECT_THAT(dir_res, Contains("/vsimem/ac_subdir/"));

    // Existing file returns empty
    std::vector<std::string> ac_existing = {"/vsimem/ac_file.tif"};
    EXPECT_TRUE(alg.GetAutoComplete(ac_existing, false, false).empty());

    VSIUnlink("/vsimem/ac_file.tif");
    VSIRmdir("/vsimem/ac_subdir");
  }

  // 3. GDALContainerAlgorithm and special action propagation (--help,
  // --json-usage)
  {
    GDALContainerAlgorithm container("cont", "Container desc");
    EXPECT_FALSE(container.Run());

    ConfigurableAlg a1, a2;
    EXPECT_TRUE(a1.ParseCommandLineArguments({"--help"}));
    EXPECT_TRUE(a1.PropagateSpecialActionTo(&a2));
    EXPECT_TRUE(a2.IsHelpRequested());
    EXPECT_TRUE(a2.Run());

    ConfigurableAlg j1;
    EXPECT_TRUE(j1.ParseCommandLineArguments({"--json-usage"}));
    EXPECT_TRUE(j1.IsJSONUsageRequested());
    EXPECT_TRUE(j1.Run());
  }

  // 4. AddArg / AddAliasFor / AddShortNameAliasFor error paths & operator[]
  {
    QuietErrorGuard quiet;
    ConfigurableAlg alg;
    int v1 = 0, v2 = 0;
    alg.AddArg("-bad-start", 0, "desc", &v1);
    alg.AddArg("bad=equal", 0, "desc", &v1);
    alg.AddArg("bad-short", '-', "desc", &v1);
    auto& good = alg.AddArg("good", 'g', "desc", &v1);
    alg.AddArg("good", 'g', "dup", &v2);
    good.AddAlias("good");
    good.AddShortNameAlias('g');

    EXPECT_EQ(nullptr, alg.GetArg("---"));
    EXPECT_EQ(nullptr, alg.GetArg("goo", /*suggestionAllowed=*/true));
    EXPECT_EQ("dummy", alg["nonexistent"].GetName());
    const ConfigurableAlg& calg = alg;
    EXPECT_EQ("dummy", calg["nonexistent"].GetName());
  }
}

TEST(GdalAlgorithmTest, CAPI) {
  GDALAllRegister();

  ConfigurableAlg cpp_alg("c_test_alg", "Short desc", "/programs/c_test.html");
  test_gdal_algorithm::test_gdal_algorithm::SetLongDescription(cpp_alg,
                                                               "Long desc");
  cpp_alg.RegisterSubAlgorithm<CustomSubAlg1>();

  bool b_val = false;
  std::string s_val;
  int i_val = 0;
  double d_val = 0.0;
  GDALArgDatasetValue ds_val;
  std::vector<std::string> sl_val;
  std::vector<int> il_val;
  std::vector<double> dl_val;
  std::vector<GDALArgDatasetValue> dsl_val;

  cpp_alg.AddArg("b-arg", 'b', "Bool arg", &b_val)
      .SetDefault(true)
      .AddAlias("bool-alias")
      .SetPositional()
      .SetRequired()
      .SetMutualExclusionGroup("mex_c")
      .AddMetadataItem("meta_key", {"meta_val"});
  cpp_alg.AddArg("s-arg", 's', "Str arg", &s_val)
      .SetDefault("def_s")
      .SetChoices("def_s", "other_s");
  cpp_alg.AddArg("i-arg", 'i', "Int arg", &i_val).SetDefault(42);
  cpp_alg.AddArg("d-arg", 'd', "Real arg", &d_val).SetDefault(3.5);
  cpp_alg.AddArg("ds-arg", 0, "DS arg", &ds_val, GDAL_OF_RASTER);
  cpp_alg.AddArg("sl-arg", 0, "SL arg", &sl_val)
      .SetDefault(std::vector<std::string>{"s1", "s2"});
  cpp_alg.AddArg("il-arg", 0, "IL arg", &il_val)
      .SetDefault(std::vector<int>{1, 2});
  cpp_alg.AddArg("dl-arg", 0, "DL arg", &dl_val)
      .SetDefault(std::vector<double>{1.5, 2.5});
  cpp_alg.AddArg("dsl-arg", 0, "DSL arg", &dsl_val, GDAL_OF_VECTOR);
  cpp_alg.AllowArbitraryLongNameArgs();

  auto h_alg_holder = GDALAlgorithmHS::FromRef(cpp_alg);
  GDALAlgorithmH h_alg = h_alg_holder.get();

  EXPECT_STREQ("c_test_alg", GDALAlgorithmGetName(h_alg));
  EXPECT_STREQ("Short desc", GDALAlgorithmGetDescription(h_alg));
  EXPECT_STREQ("Long desc", GDALAlgorithmGetLongDescription(h_alg));
  EXPECT_STREQ("https://gdal.org/programs/c_test.html",
               GDALAlgorithmGetHelpFullURL(h_alg));
  EXPECT_TRUE(GDALAlgorithmHasSubAlgorithms(h_alg));

  char** sub_names = GDALAlgorithmGetSubAlgorithmNames(h_alg);
  EXPECT_NE(nullptr, sub_names);
  CSLDestroy(sub_names);

  GDALAlgorithmH h_sub =
      GDALAlgorithmInstantiateSubAlgorithm(h_alg, "sub_alpha");
  ASSERT_NE(nullptr, h_sub);
  const char* sub_cli[] = {"-f", nullptr};
  EXPECT_TRUE(GDALAlgorithmParseCommandLineArguments(h_sub, sub_cli));
  GDALAlgorithmH h_actual = GDALAlgorithmGetActualAlgorithm(h_sub);
  ASSERT_NE(nullptr, h_actual);
  GDALAlgorithmRelease(h_actual);
  EXPECT_TRUE(GDALAlgorithmRun(h_sub, nullptr, nullptr));
  EXPECT_TRUE(GDALAlgorithmFinalize(h_sub));
  GDALAlgorithmRelease(h_sub);

  char* json_usage = GDALAlgorithmGetUsageAsJSON(h_alg);
  EXPECT_NE(nullptr, json_usage);
  CPLFree(json_usage);

  char** arg_names = GDALAlgorithmGetArgNames(h_alg);
  EXPECT_NE(nullptr, arg_names);
  CSLDestroy(arg_names);

  // Test GDALAlgorithmGetArgNonConst creating user-provided arg
  GDALAlgorithmArgH h_user_arg =
      GDALAlgorithmGetArgNonConst(h_alg, "user-created");
  ASSERT_NE(nullptr, h_user_arg);
  GDALAlgorithmArgRelease(h_user_arg);

  // Inspect and mutate each argument type via C API
  GDALAlgorithmArgH hb = GDALAlgorithmGetArg(h_alg, "b-arg");
  GDALAlgorithmArgH hs = GDALAlgorithmGetArg(h_alg, "s-arg");
  GDALAlgorithmArgH hi = GDALAlgorithmGetArg(h_alg, "i-arg");
  GDALAlgorithmArgH hd = GDALAlgorithmGetArg(h_alg, "d-arg");
  GDALAlgorithmArgH hds = GDALAlgorithmGetArg(h_alg, "ds-arg");
  GDALAlgorithmArgH hsl = GDALAlgorithmGetArg(h_alg, "sl-arg");
  GDALAlgorithmArgH hil = GDALAlgorithmGetArg(h_alg, "il-arg");
  GDALAlgorithmArgH hdl = GDALAlgorithmGetArg(h_alg, "dl-arg");
  GDALAlgorithmArgH hdsl = GDALAlgorithmGetArg(h_alg, "dsl-arg");

  EXPECT_STREQ("b-arg", GDALAlgorithmArgGetName(hb));
  EXPECT_EQ(GAAT_BOOLEAN, GDALAlgorithmArgGetType(hb));
  EXPECT_STREQ("Bool arg", GDALAlgorithmArgGetDescription(hb));
  EXPECT_STREQ("b", GDALAlgorithmArgGetShortName(hb));
  char** aliases = GDALAlgorithmArgGetAliases(hb);
  CSLDestroy(aliases);
  EXPECT_STREQ("", GDALAlgorithmArgGetMetaVar(hb));
  EXPECT_STREQ("Base", GDALAlgorithmArgGetCategory(hb));
  EXPECT_TRUE(GDALAlgorithmArgIsPositional(hb));
  EXPECT_TRUE(GDALAlgorithmArgIsRequired(hb));
  EXPECT_EQ(0, GDALAlgorithmArgGetMinCount(hb));
  EXPECT_EQ(1, GDALAlgorithmArgGetMaxCount(hb));
  EXPECT_TRUE(GDALAlgorithmArgGetPackedValuesAllowed(hb));
  EXPECT_TRUE(GDALAlgorithmArgGetRepeatedArgAllowed(hb));
  EXPECT_FALSE(GDALAlgorithmArgIsExplicitlySet(hb));
  EXPECT_TRUE(GDALAlgorithmArgHasDefaultValue(hb));
  EXPECT_TRUE(GDALAlgorithmArgGetDefaultAsBoolean(hb));
  EXPECT_FALSE(GDALAlgorithmArgIsHidden(hb));
  EXPECT_FALSE(GDALAlgorithmArgIsHiddenForCLI(hb));
  EXPECT_FALSE(GDALAlgorithmArgIsHiddenForAPI(hb));
  EXPECT_TRUE(GDALAlgorithmArgIsInput(hb));
  EXPECT_FALSE(GDALAlgorithmArgIsOutput(hb));
  EXPECT_STREQ("mex_c", GDALAlgorithmArgGetMutualExclusionGroup(hb));

  char** mitem = GDALAlgorithmArgGetMetadataItem(hb, "meta_key");
  EXPECT_NE(nullptr, mitem);
  CSLDestroy(mitem);
  EXPECT_EQ(nullptr, GDALAlgorithmArgGetMetadataItem(hb, "nonexistent"));

  char** choices = GDALAlgorithmArgGetChoices(hs);
  EXPECT_NE(nullptr, choices);
  CSLDestroy(choices);

  // Defaults & Getters/Setters across types
  EXPECT_STREQ("def_s", GDALAlgorithmArgGetDefaultAsString(hs));
  EXPECT_EQ(42, GDALAlgorithmArgGetDefaultAsInteger(hi));
  EXPECT_DOUBLE_EQ(3.5, GDALAlgorithmArgGetDefaultAsDouble(hd));
  char** def_sl = GDALAlgorithmArgGetDefaultAsStringList(hsl);
  CSLDestroy(def_sl);
  size_t count = 0;
  EXPECT_NE(nullptr, GDALAlgorithmArgGetDefaultAsIntegerList(hil, &count));
  EXPECT_EQ(2u, count);
  EXPECT_NE(nullptr, GDALAlgorithmArgGetDefaultAsDoubleList(hdl, &count));
  EXPECT_EQ(2u, count);

  EXPECT_TRUE(GDALAlgorithmArgSetAsBoolean(hb, false));
  EXPECT_FALSE(GDALAlgorithmArgGetAsBoolean(hb));

  EXPECT_TRUE(GDALAlgorithmArgSetAsString(hs, "other_s"));
  EXPECT_STREQ("other_s", GDALAlgorithmArgGetAsString(hs));

  EXPECT_TRUE(GDALAlgorithmArgSetAsInteger(hi, 99));
  EXPECT_EQ(99, GDALAlgorithmArgGetAsInteger(hi));

  EXPECT_TRUE(GDALAlgorithmArgSetAsDouble(hd, 7.25));
  EXPECT_DOUBLE_EQ(7.25, GDALAlgorithmArgGetAsDouble(hd));

  const char* sl_c[] = {"x", "y", nullptr};
  EXPECT_TRUE(GDALAlgorithmArgSetAsStringList(hsl, sl_c));
  char** got_sl = GDALAlgorithmArgGetAsStringList(hsl);
  CSLDestroy(got_sl);

  const int il_c[] = {10, 20, 30};
  EXPECT_TRUE(GDALAlgorithmArgSetAsIntegerList(hil, 3, il_c));
  EXPECT_NE(nullptr, GDALAlgorithmArgGetAsIntegerList(hil, &count));
  EXPECT_EQ(3u, count);

  const double dl_c[] = {1.1, 2.2};
  EXPECT_TRUE(GDALAlgorithmArgSetAsDoubleList(hdl, 2, dl_c));
  EXPECT_NE(nullptr, GDALAlgorithmArgGetAsDoubleList(hdl, &count));
  EXPECT_EQ(2u, count);

  // Dataset Value C API
  EXPECT_EQ(GDAL_OF_RASTER, GDALAlgorithmArgGetDatasetType(hds));
  EXPECT_EQ(GADV_NAME | GADV_OBJECT, GDALAlgorithmArgGetDatasetInputFlags(hds));
  EXPECT_EQ(GADV_OBJECT, GDALAlgorithmArgGetDatasetOutputFlags(hds));

  GDALArgDatasetValueH h_dsv = GDALArgDatasetValueCreate();
  ASSERT_NE(nullptr, h_dsv);
  GDALArgDatasetValueSetName(h_dsv, "/vsimem/c_api.tif");
  EXPECT_STREQ("/vsimem/c_api.tif", GDALArgDatasetValueGetName(h_dsv));

  auto mem_ds = CreateMemRaster();
  GDALArgDatasetValueSetDataset(h_dsv, GDALDataset::ToHandle(mem_ds.get()));
  EXPECT_EQ(GDALDataset::ToHandle(mem_ds.get()),
            GDALArgDatasetValueGetDatasetRef(h_dsv));
  GDALDatasetH h_inc_ds = GDALArgDatasetValueGetDatasetIncreaseRefCount(h_dsv);
  EXPECT_EQ(GDALDataset::ToHandle(mem_ds.get()), h_inc_ds);
  GDALReleaseDataset(h_inc_ds);

  EXPECT_TRUE(GDALAlgorithmArgSetAsDatasetValue(hds, h_dsv));
  GDALArgDatasetValueH h_got_dsv = GDALAlgorithmArgGetAsDatasetValue(hds);
  ASSERT_NE(nullptr, h_got_dsv);
  EXPECT_EQ(GDALDataset::ToHandle(mem_ds.get()),
            GDALArgDatasetValueGetDatasetRef(h_got_dsv));
  GDALArgDatasetValueRelease(h_got_dsv);
  GDALArgDatasetValueRelease(h_dsv);

  EXPECT_TRUE(
      GDALAlgorithmArgSetDataset(hds, GDALDataset::ToHandle(mem_ds.get())));

  GDALDatasetH ds_arr[] = {GDALDataset::ToHandle(mem_ds.get())};
  EXPECT_TRUE(GDALAlgorithmArgSetDatasets(hdsl, 1, ds_arr));
  const char* ds_names[] = {"/vsimem/n1.tif", "/vsimem/n2.tif", nullptr};
  EXPECT_TRUE(GDALAlgorithmArgSetDatasetNames(hdsl, ds_names));
  EXPECT_TRUE(GDALAlgorithmArgSetDataset(hds, nullptr));
  EXPECT_TRUE(GDALAlgorithmArgSetDatasets(hdsl, 0, nullptr));

  // Wrong-type calls on C API getters (error paths)
  {
    QuietErrorGuard quiet;
    EXPECT_FALSE(GDALAlgorithmArgGetDefaultAsBoolean(hs));
    EXPECT_EQ(nullptr, GDALAlgorithmArgGetDefaultAsString(hb));
    EXPECT_EQ(0, GDALAlgorithmArgGetDefaultAsInteger(hb));
    EXPECT_DOUBLE_EQ(0.0, GDALAlgorithmArgGetDefaultAsDouble(hb));
    EXPECT_EQ(nullptr, GDALAlgorithmArgGetDefaultAsStringList(hb));
    EXPECT_EQ(nullptr, GDALAlgorithmArgGetDefaultAsIntegerList(hb, &count));
    EXPECT_EQ(nullptr, GDALAlgorithmArgGetDefaultAsDoubleList(hb, &count));

    EXPECT_FALSE(GDALAlgorithmArgGetAsBoolean(hs));
    EXPECT_EQ(nullptr, GDALAlgorithmArgGetAsString(hb));
    EXPECT_EQ(nullptr, GDALAlgorithmArgGetAsDatasetValue(hb));
    EXPECT_EQ(0, GDALAlgorithmArgGetAsInteger(hb));
    EXPECT_DOUBLE_EQ(0.0, GDALAlgorithmArgGetAsDouble(hb));
    EXPECT_EQ(nullptr, GDALAlgorithmArgGetAsStringList(hb));
    EXPECT_EQ(nullptr, GDALAlgorithmArgGetAsIntegerList(hb, &count));
    EXPECT_EQ(nullptr, GDALAlgorithmArgGetAsDoubleList(hb, &count));
  }

  GDALAlgorithmArgRelease(hb);
  GDALAlgorithmArgRelease(hs);
  GDALAlgorithmArgRelease(hi);
  GDALAlgorithmArgRelease(hd);
  GDALAlgorithmArgRelease(hds);
  GDALAlgorithmArgRelease(hsl);
  GDALAlgorithmArgRelease(hil);
  GDALAlgorithmArgRelease(hdl);
  GDALAlgorithmArgRelease(hdsl);
}

}  // namespace
