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
// Tests GDALMajorObject C++ and C APIs (gcore/gdalmajorobject.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdalmajorobject.cpp

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

using ::testing::Contains;
using ::testing::Not;

class TestMajorObject : public GDALMajorObject {
 public:
  using GDALMajorObject::BuildMetadataDomainList;
};

TEST(GdalMajorObjectTest, CppAndCApi) {
  TestMajorObject obj;
  GDALMajorObjectH handle = GDALMajorObject::ToHandle(&obj);
  EXPECT_EQ(&obj, GDALMajorObject::FromHandle(handle));

  // Description.
  EXPECT_STREQ("", obj.GetDescription());
  obj.SetDescription("desc1");
  EXPECT_STREQ("desc1", obj.GetDescription());
  EXPECT_STREQ("desc1", GDALGetDescription(handle));
  GDALSetDescription(handle, "desc2");
  EXPECT_STREQ("desc2", GDALGetDescription(handle));

  // Flags.
  EXPECT_EQ(GMO_VALID, obj.GetMOFlags());
  obj.SetMOFlags(GMO_VALID | GMO_SUPPORT_MD);
  EXPECT_EQ(GMO_VALID | GMO_SUPPORT_MD, obj.GetMOFlags());

  // Metadata items & domains.
  EXPECT_EQ(nullptr, obj.GetMetadata());
  EXPECT_EQ(nullptr, obj.GetMetadataItem("KEY"));

  EXPECT_EQ(CE_None, obj.SetMetadataItem("KEY1", "VAL1"));
  EXPECT_NE(0, obj.GetMOFlags() & GMO_MD_DIRTY);
  EXPECT_STREQ("VAL1", obj.GetMetadataItem("KEY1"));
  EXPECT_STREQ("VAL1", GDALGetMetadataItem(handle, "KEY1", nullptr));

  EXPECT_EQ(CE_None, GDALSetMetadataItem(handle, "KEY2", "VAL2", "SUBDOMAIN"));
  EXPECT_STREQ("VAL2", GDALGetMetadataItem(handle, "KEY2", "SUBDOMAIN"));

  CPLStringList md;
  md.SetNameValue("A", "1");
  md.SetNameValue("B", "2");
  EXPECT_EQ(CE_None, GDALSetMetadata(handle, md.List(), "DOM2"));
  char** fetched_md = GDALGetMetadata(handle, "DOM2");
  ASSERT_NE(nullptr, fetched_md);
  EXPECT_STREQ("1", CSLFetchNameValue(fetched_md, "A"));
  EXPECT_STREQ("2", CSLFetchNameValue(fetched_md, "B"));

  // Domain list.
  StringListPtr domains(GDALGetMetadataDomainList(handle), CSLDestroy);
  ASSERT_NE(nullptr, domains);
  EXPECT_THAT(CslToVector(domains.get()), Contains(""));
  EXPECT_THAT(CslToVector(domains.get()), Contains("SUBDOMAIN"));
  EXPECT_THAT(CslToVector(domains.get()), Contains("DOM2"));

  // BuildMetadataDomainList with bCheckNonEmpty = TRUE and FALSE.
  domains.reset(obj.BuildMetadataDomainList(
      domains.release(), TRUE, "SUBDOMAIN", "DOM2", "EMPTY_DOM", nullptr));
  EXPECT_THAT(CslToVector(domains.get()), Not(Contains("EMPTY_DOM")));
  domains.reset(obj.BuildMetadataDomainList(domains.release(), FALSE,
                                            "EMPTY_DOM", nullptr));
  EXPECT_THAT(CslToVector(domains.get()), Contains("EMPTY_DOM"));

  // Null handle checks for C API.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALGetDescription(nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();

    GDALSetDescription(nullptr, "ignored");
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();

    EXPECT_EQ(nullptr, GDALGetMetadataDomainList(nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();

    EXPECT_EQ(nullptr, GDALGetMetadata(nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();

    EXPECT_EQ(CE_Failure, GDALSetMetadata(nullptr, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();

    EXPECT_EQ(nullptr, GDALGetMetadataItem(nullptr, "K", nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();

    EXPECT_EQ(CE_Failure, GDALSetMetadataItem(nullptr, "K", "V", nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Exercise destructor when GMO_VALID is cleared (CPLDebug branch inside
  // ~GDALMajorObject is compiled out in google3 via WITHOUT_CPLDEBUG).
  {
    TestMajorObject invalid_obj;
    invalid_obj.SetMOFlags(0);
  }
}

}  // namespace
}  // namespace autotest2
