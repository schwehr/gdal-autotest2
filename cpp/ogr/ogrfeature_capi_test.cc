// Copyright 2017 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Test OGRFeature.

// clang-format off
#include "port/cpl_port.h"
#include "ogr/ogr_api.h"
#include "ogr/ogr_feature.h"
// clang-format on

#include <memory>  // NOLINT(build/include_order)

#include "gunit.h"
#include "third_party/absl/memory/memory.h"
#include "autotest2/cpp/util/ogr/ogrfeaturedefn.h"
#include "autotest2/cpp/util/error_handler.h"
#include "ogr/ogr_core.h"
#include "util/gtl/cleanup.h"
#include "util/task/statusor.h"

namespace autotest2 {
namespace {

class CApiFeatureTest : public ::testing::Test {
 private:
  // Prevent GDAL error messages from going to stderr.
  WithQuietHandler handler_;
};

TEST_F(CApiFeatureTest, CreateDestroy) {
  OGRFeatureDefnBuilder builder("schema");
  auto fd = builder.Build();
  auto fd_cleaner = gtl::MakeCleanup([fd] { fd->Release(); });
  OGRFeatureH f = OGR_F_Create(fd);
  ASSERT_NE(nullptr, f);

  OGRFeatureDefnH fd_ref = OGR_F_GetDefnRef(f);
  EXPECT_EQ(fd_ref, fd);
  OGR_F_Destroy(f);
}

// TODO(schwehr): OGR_F_GetDefnRef()
// TODO(schwehr): OGR_F_SetGeometryDirectly()
// TODO(schwehr): OGR_F_SetGeometry()
// TODO(schwehr): OGR_F_StealGeometry()
// TODO(schwehr): OGR_F_GetGeometryRef()
// TODO(schwehr): OGR_F_GetGeomFieldRef()
// TODO(schwehr): OGR_F_SetGeomFieldDirectly()
// TODO(schwehr): OGR_F_SetGeomField()
// TODO(schwehr): OGR_F_Clone()
// TODO(schwehr): OGR_F_GetFieldCount()
// TODO(schwehr): OGR_F_GetFieldDefnRef()
// TODO(schwehr): OGR_F_GetFieldIndex()
// TODO(schwehr): OGR_F_GetGeomFieldCount()
// TODO(schwehr): OGR_F_GetGeomFieldDefnRef()
// TODO(schwehr): OGR_F_GetGeomFieldIndex()
// TODO(schwehr): OGR_F_IsFieldSet()
// TODO(schwehr): OGR_F_UnsetField()
// TODO(schwehr): OGR_F_IsFieldNull()
// TODO(schwehr): OGR_F_IsFieldSetAndNotNull()
// TODO(schwehr): OGR_F_SetFieldNull()
// TODO(schwehr): OGR_F_GetRawFieldRef()
// TODO(schwehr): OGR_F_GetFieldAsInteger()
// TODO(schwehr): OGR_F_GetFieldAsInteger64()
// TODO(schwehr): OGR_F_GetFieldAsDouble()
// TODO(schwehr): OGR_F_GetFieldAsString()
// TODO(schwehr): OGR_F_GetFieldAsIntegerList()
// TODO(schwehr): OGR_F_GetFieldAsInteger64List()
// TODO(schwehr): OGR_F_GetFieldAsDoubleList()
// TODO(schwehr): OGR_F_GetFieldAsStringList()
// TODO(schwehr): OGR_F_GetFieldAsBinary()
// TODO(schwehr): OGR_F_GetFieldAsDateTime()
// TODO(schwehr): OGR_F_GetFieldAsDateTimeEx()
// TODO(schwehr): OGR_F_SetFieldInteger()
// TODO(schwehr): OGR_F_SetFieldInteger64()
// TODO(schwehr): OGR_F_SetFieldDouble()
// TODO(schwehr): OGR_F_SetFieldString()
// TODO(schwehr): OGR_F_SetFieldIntegerList()
// TODO(schwehr): OGR_F_SetFieldInteger64List()
// TODO(schwehr): OGR_F_SetFieldDoubleList()
// TODO(schwehr): OGR_F_SetFieldStringList()
// TODO(schwehr): OGR_F_SetFieldBinary()
// TODO(schwehr): OGR_F_SetFieldDateTime()
// TODO(schwehr): OGR_F_SetFieldDateTimeEx()
// TODO(schwehr): OGR_F_SetFieldRaw()
// TODO(schwehr): OGR_F_DumpReadable()
// TODO(schwehr): OGR_F_GetFID()
// TODO(schwehr): OGR_F_SetFID()
// TODO(schwehr): OGR_F_Equal()
// TODO(schwehr): OGR_F_SetFrom()
// TODO(schwehr): OGR_F_SetFromWithMap()
// TODO(schwehr): OGR_F_GetStyleString()
// TODO(schwehr): OGR_F_SetStyleString()
// TODO(schwehr): OGR_F_SetStyleStringDirectly()
// TODO(schwehr): OGR_F_GetStyleTable()
// TODO(schwehr): OGR_F_SetStyleTableDirectly()
// TODO(schwehr): OGR_F_SetStyleTable()
// TODO(schwehr): OGR_F_FillUnsetWithDefault()
// TODO(schwehr): OGR_F_Validate()
// TODO(schwehr): OGR_F_GetNativeData()
// TODO(schwehr): OGR_F_GetNativeMediaType()
// TODO(schwehr): OGR_F_SetNativeData()
// TODO(schwehr): OGR_F_SetNativeMediaType()

}  // namespace
}  // namespace autotest2
