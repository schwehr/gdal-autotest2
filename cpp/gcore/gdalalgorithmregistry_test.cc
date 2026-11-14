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
// Tests GDALAlgorithmRegistry and GDALGlobalAlgorithmRegistry
// (gcore/gdalalgorithmregistry.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdalalgorithmregistry.cpp

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdalalgorithm.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;

class DummySubAlg final : public GDALAlgorithm {
 public:
  static constexpr const char* NAME = "subalg";
  static constexpr const char* DESCRIPTION = "A dummy sub-algorithm";
  static constexpr const char* HELP_URL = "";
  static std::vector<std::string> GetAliasesStatic() { return {}; }

  DummySubAlg() : GDALAlgorithm(NAME, DESCRIPTION, HELP_URL) {}

 protected:
  bool RunImpl(GDALProgressFunc, void*) override { return true; }
};

class DummyTopAlg final : public GDALAlgorithm {
 public:
  static constexpr const char* NAME = "dummy_top";
  static constexpr const char* DESCRIPTION = "A dummy top algorithm";
  static constexpr const char* HELP_URL = "";
  static std::vector<std::string> GetAliasesStatic() {
    return {"alias1", GDALAlgorithmRegistry::HIDDEN_ALIAS_SEPARATOR,
            "hidden_alias1"};
  }

  DummyTopAlg() : GDALAlgorithm(NAME, DESCRIPTION, HELP_URL) {
    RegisterSubAlgorithm<DummySubAlg>();
  }

 protected:
  bool RunImpl(GDALProgressFunc, void*) override { return true; }
};

TEST(GdalalgorithmregistryTest, CustomAndGlobalRegistry) {
  GDALAlgorithmRegistry reg;
  EXPECT_TRUE(reg.Register<DummyTopAlg>());

  // Duplicate name and duplicate alias registration should fail.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(reg.Register<DummyTopAlg>());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    CPLErrorReset();
    GDALAlgorithmRegistry::AlgInfo dup_alias_info;
    dup_alias_info.m_name = "other_name";
    dup_alias_info.m_aliases = {"alias1"};
    dup_alias_info.m_creationFunc = []() -> std::unique_ptr<GDALAlgorithm> {
      return std::make_unique<DummyTopAlg>();
    };
    EXPECT_FALSE(reg.Register(dup_alias_info));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  EXPECT_THAT(reg.GetNames(), ElementsAre("dummy_top"));

  // Instantiate by primary name, visible alias, hidden alias, and missing name.
  EXPECT_NE(nullptr, reg.Instantiate("dummy_top"));
  EXPECT_NE(nullptr, reg.Instantiate("alias1"));
  EXPECT_NE(nullptr, reg.Instantiate("hidden_alias1"));
  EXPECT_EQ(nullptr, reg.Instantiate("does_not_exist"));

  // Instantiate by path.
  EXPECT_EQ(nullptr, reg.Instantiate(std::vector<std::string>{}));
  EXPECT_NE(nullptr,
            reg.Instantiate(std::vector<std::string>{"dummy_top", "subalg"}));
  EXPECT_EQ(nullptr,
            reg.Instantiate(std::vector<std::string>{"dummy_top", "unknown"}));

  // Global algorithm registry.
  auto& global_reg = GDALGlobalAlgorithmRegistry::GetSingleton();
  global_reg.DeclareAlgorithm({"custom_group", "custom_leaf"},
                              []() -> std::unique_ptr<GDALAlgorithm> {
                                return std::make_unique<DummySubAlg>();
                              });
  global_reg.DeclareAlgorithm({"custom_group", "custom_leaf2"},
                              []() -> std::unique_ptr<GDALAlgorithm> {
                                return std::make_unique<DummySubAlg>();
                              });

  EXPECT_TRUE(global_reg.HasDeclaredSubAlgorithm({"custom_group"}));
  EXPECT_TRUE(global_reg.HasDeclaredSubAlgorithm(
      {"gdal", "custom_group", "custom_leaf"}));
  EXPECT_FALSE(global_reg.HasDeclaredSubAlgorithm({}));
  EXPECT_FALSE(global_reg.HasDeclaredSubAlgorithm({"nonexistent_group"}));

  const auto sub_names =
      global_reg.GetDeclaredSubAlgorithmNames({"custom_group"});
  EXPECT_GE(sub_names.size(), 2u);

  EXPECT_NE(nullptr,
            global_reg.InstantiateDeclaredSubAlgorithm({"custom_group"}));
  EXPECT_NE(nullptr, global_reg.InstantiateDeclaredSubAlgorithm(
                         {"custom_group", "custom_leaf"}));
  EXPECT_EQ(nullptr,
            global_reg.InstantiateDeclaredSubAlgorithm({"nonexistent_group"}));

  EXPECT_NE(nullptr,
            global_reg.Instantiate(GDALGlobalAlgorithmRegistry::ROOT_ALG_NAME));
  EXPECT_NE(nullptr, global_reg.Instantiate("custom_group"));

  // C API.
  std::unique_ptr<GDALAlgorithmRegistryHS,
                  decltype(&GDALAlgorithmRegistryRelease)>
      h_reg(GDALGetGlobalAlgorithmRegistry(), GDALAlgorithmRegistryRelease);
  ASSERT_NE(nullptr, h_reg);

  StringListPtr names(GDALAlgorithmRegistryGetAlgNames(h_reg.get()),
                      CSLDestroy);
  ASSERT_NE(nullptr, names);
  EXPECT_THAT(CslToVector(names.get()),
              ::testing::IsSupersetOf({"convert", "dataset", "info", "mdim",
                                       "pipeline", "raster", "vector", "vsi"}));

  std::unique_ptr<GDALAlgorithmHS, decltype(&GDALAlgorithmRelease)> h_alg(
      GDALAlgorithmRegistryInstantiateAlg(h_reg.get(), "custom_group"),
      GDALAlgorithmRelease);
  ASSERT_NE(nullptr, h_alg);

  const char* path[] = {"custom_group", "custom_leaf", nullptr};
  std::unique_ptr<GDALAlgorithmHS, decltype(&GDALAlgorithmRelease)> h_alg_path(
      GDALAlgorithmRegistryInstantiateAlgFromPath(h_reg.get(), path),
      GDALAlgorithmRelease);
  ASSERT_NE(nullptr, h_alg_path);

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALAlgorithmRegistryGetAlgNames(nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALAlgorithmRegistryInstantiateAlg(nullptr, "raster"));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GDALAlgorithmRegistryInstantiateAlg(h_reg.get(), nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GDALAlgorithmRegistryInstantiateAlgFromPath(nullptr, path));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALAlgorithmRegistryInstantiateAlgFromPath(h_reg.get(),
                                                                   nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
