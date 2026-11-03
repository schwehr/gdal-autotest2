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
// Tests GMLJP2 metadata generation (gcore/gdaljp2metadatagenerator.cpp).
//
// Note: HAVE_LIBXML2 is not defined in google3 builds, so
// GDALGMLJP2GenerateMetadata() is compiled as a stub returning nullptr.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdaljp2metadatagenerator.cpp

#include "third_party/gdal/gcore/gdaljp2metadatagenerator.h"

#include "testing/base/public/gunit.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"

namespace autotest2 {
namespace {

constexpr char kTemplatePath[] =
    "/vsimem/gdaljp2metadatagenerator_test/nonexistent_template.xml";
constexpr char kSourcePath[] =
    "/vsimem/gdaljp2metadatagenerator_test/nonexistent_source.xml";

TEST(GdalJp2MetadataGeneratorTest, GenerateMetadata) {
  CPLErrorReset();
  CPLXMLTreeCloser node(GDALGMLJP2GenerateMetadata(kTemplatePath, kSourcePath));
  EXPECT_EQ(nullptr, node);
  EXPECT_EQ(CE_None, CPLGetLastErrorType());
}

}  // namespace
}  // namespace autotest2
