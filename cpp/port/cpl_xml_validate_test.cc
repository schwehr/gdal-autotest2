// Tests for cpl_xml_validate:
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_xml_validate.cpp
//
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

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"

namespace autotest2 {
namespace {

// //third_party/gdal:port_lib is built without HAVE_LIBXML2, so CPLValidateXML
// is the stub that always fails with CPLE_NotSupported.  This test must be
// rewritten (with real schema files) if libxml2 support is ever enabled.
TEST(CplXmlValidateTest, ValidateXmlWithoutLibxml2) {
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(FALSE, CPLValidateXML("<root/>", "schema.xsd", nullptr));
  EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
}

}  // namespace
}  // namespace autotest2
