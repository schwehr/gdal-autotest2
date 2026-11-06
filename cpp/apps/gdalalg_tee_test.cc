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
// Tests for GDALTee algorithms (apps/gdalalg_tee.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalalg_tee.cpp

#include "third_party/gdal/apps/gdalalg_tee.h"

#include <memory>

#include "testing/base/public/gunit.h"

namespace autotest2 {
namespace {

TEST(GdalalgTeeTest, ConstructAndDestroyRaster) {
  auto alg = std::make_unique<GDALTeeRasterAlgorithm>();
  ASSERT_NE(nullptr, alg);
  EXPECT_EQ("tee", alg->GetName());
  EXPECT_FALSE(alg->IsNativelyStreamingCompatible());
  EXPECT_TRUE(alg->CanBeMiddleStep());
  EXPECT_TRUE(alg->CanBeLastStep());
  EXPECT_TRUE(alg->GeneratesFilesFromUserInput());
}

TEST(GdalalgTeeTest, ConstructAndDestroyVector) {
  auto alg = std::make_unique<GDALTeeVectorAlgorithm>();
  ASSERT_NE(nullptr, alg);
  EXPECT_EQ("tee", alg->GetName());
  EXPECT_FALSE(alg->IsNativelyStreamingCompatible());
  EXPECT_TRUE(alg->CanBeMiddleStep());
  EXPECT_TRUE(alg->CanBeLastStep());
  EXPECT_TRUE(alg->GeneratesFilesFromUserInput());
}

}  // namespace
}  // namespace autotest2
