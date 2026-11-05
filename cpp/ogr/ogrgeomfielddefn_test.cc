// Copyright 2017 Google Inc. All Rights Reserved.
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

#include <memory>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geomcoordinateprecision.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogr_srs_api.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::NotNull;

TEST(OgrGeomFieldDefnTest, NullptrAndDefaults) {
  const auto fd = std::make_unique<OGRGeomFieldDefn>(nullptr, wkbUnknown);
  ASSERT_THAT(fd, NotNull());
  EXPECT_STREQ("", fd->GetNameRef());
  EXPECT_EQ(wkbUnknown, fd->GetType());
  EXPECT_EQ(nullptr, fd->GetSpatialRef());
  EXPECT_FALSE(fd->IsIgnored());
  EXPECT_TRUE(fd->IsNullable());
}

TEST(OgrGeomFieldDefnTest, Name) {
  const auto fd = std::make_unique<OGRGeomFieldDefn>(nullptr, wkbUnknown);
  ASSERT_THAT(fd, NotNull());
  fd->SetName("a\xFA");
  EXPECT_STREQ("a\xFA", fd->GetNameRef());
  // Self assignment pointer
  fd->SetName(fd->GetNameRef());
  EXPECT_STREQ("a\xFA", fd->GetNameRef());
  fd->SetName(nullptr);
  EXPECT_STREQ("", fd->GetNameRef());
}

TEST(OgrGeomFieldDefnTest, Type) {
  const auto fd = std::make_unique<OGRGeomFieldDefn>("a", wkbUnknown);
  ASSERT_THAT(fd, NotNull());
  fd->SetType(wkbTriangleZM);
  EXPECT_EQ(wkbTriangleZM, fd->GetType());
}

TEST(OgrGeomFieldDefnTest, Srs) {
  const auto fd = std::make_unique<OGRGeomFieldDefn>("a", wkbUnknown);
  ASSERT_THAT(fd, NotNull());
  auto* srs = new OGRSpatialReference();
  ASSERT_EQ(OGRERR_NONE, srs->SetWellKnownGeogCS("WGS84"));

  fd->SetSpatialRef(srs);
  EXPECT_EQ(srs, fd->GetSpatialRef());

  // Re-setting the exact same pointer is a no-op
  fd->SetSpatialRef(srs);
  EXPECT_EQ(srs, fd->GetSpatialRef());

  fd->SetSpatialRef(nullptr);
  EXPECT_EQ(nullptr, fd->GetSpatialRef());

  srs->Release();
  OSRCleanup();
}

TEST(OgrGeomFieldDefnTest, Ignored) {
  const auto fd = std::make_unique<OGRGeomFieldDefn>("a", wkbUnknown);
  EXPECT_FALSE(fd->IsIgnored());
  fd->SetIgnored(true);
  EXPECT_TRUE(fd->IsIgnored());
  fd->SetIgnored(false);
  EXPECT_FALSE(fd->IsIgnored());
}

TEST(OgrGeomFieldDefnTest, Nullable) {
  const auto fd = std::make_unique<OGRGeomFieldDefn>("a", wkbUnknown);
  EXPECT_TRUE(fd->IsNullable());
  fd->SetNullable(false);
  EXPECT_FALSE(fd->IsNullable());
  fd->SetNullable(true);
  EXPECT_TRUE(fd->IsNullable());
}

TEST(OgrGeomFieldDefnTest, CoordinatePrecision) {
  auto fd = std::make_unique<OGRGeomFieldDefn>("geom", wkbPoint);
  OGRGeomCoordinatePrecision prec;
  prec.dfXYResolution = 0.01;
  prec.dfZResolution = 0.05;
  prec.dfMResolution = 0.1;
  fd->SetCoordinatePrecision(prec);

  const auto& got = fd->GetCoordinatePrecision();
  EXPECT_DOUBLE_EQ(0.01, got.dfXYResolution);
  EXPECT_DOUBLE_EQ(0.05, got.dfZResolution);
  EXPECT_DOUBLE_EQ(0.1, got.dfMResolution);
}

TEST(OgrGeomFieldDefnTest, IsSame) {
  auto fd = std::make_unique<OGRGeomFieldDefn>("a", wkbPoint);
  EXPECT_TRUE(fd->IsSame(fd.get()));

  {
    const auto other = std::make_unique<OGRGeomFieldDefn>("b", wkbPoint);
    EXPECT_FALSE(fd->IsSame(other.get()));
  }
  {
    const auto other = std::make_unique<OGRGeomFieldDefn>("a", wkbMultiPoint);
    EXPECT_FALSE(fd->IsSame(other.get()));
  }
  {
    const auto other = std::make_unique<OGRGeomFieldDefn>("a", wkbPoint);
    other->SetNullable(false);
    EXPECT_FALSE(fd->IsSame(other.get()));
  }

  // Coordinate precision differences in IsSame
  {
    const auto other = std::make_unique<OGRGeomFieldDefn>("a", wkbPoint);
    OGRGeomCoordinatePrecision prec;
    prec.dfXYResolution = 0.001;
    other->SetCoordinatePrecision(prec);
    EXPECT_FALSE(fd->IsSame(other.get()));
  }
  {
    const auto other = std::make_unique<OGRGeomFieldDefn>("a", wkbPoint);
    OGRGeomCoordinatePrecision prec;
    prec.dfZResolution = 0.001;
    other->SetCoordinatePrecision(prec);
    EXPECT_FALSE(fd->IsSame(other.get()));
  }
  {
    const auto other = std::make_unique<OGRGeomFieldDefn>("a", wkbPoint);
    OGRGeomCoordinatePrecision prec;
    prec.dfMResolution = 0.001;
    other->SetCoordinatePrecision(prec);
    EXPECT_FALSE(fd->IsSame(other.get()));
  }

  // Spatial reference comparisons
  {
    auto* srs4326 = new OGRSpatialReference();
    ASSERT_EQ(OGRERR_NONE, srs4326->SetWellKnownGeogCS("WGS84"));

    auto* srs32611 = new OGRSpatialReference();
    ASSERT_EQ(OGRERR_NONE, srs32611->SetWellKnownGeogCS("NAD27"));

    const auto other = std::make_unique<OGRGeomFieldDefn>("a", wkbPoint);
    other->SetSpatialRef(srs4326);
    EXPECT_FALSE(fd->IsSame(other.get()));

    const auto another = std::make_unique<OGRGeomFieldDefn>("a", wkbPoint);
    another->SetSpatialRef(srs4326);
    EXPECT_TRUE(other->IsSame(another.get()));

    another->SetSpatialRef(srs32611);
    EXPECT_FALSE(other->IsSame(another.get()));

    srs4326->Release();
    srs32611->Release();
  }

  // Ignored is ignored in IsSame
  {
    const auto other = std::make_unique<OGRGeomFieldDefn>("a", wkbPoint);
    other->SetIgnored(false);
    EXPECT_TRUE(fd->IsSame(other.get()));
    other->SetIgnored(true);
    EXPECT_TRUE(fd->IsSame(other.get()));
  }

  OSRCleanup();
}

TEST(OgrGeomFieldDefnTest, PrototypeConstructorWithSrs) {
  auto orig = std::make_unique<OGRGeomFieldDefn>("geom", wkbPolygon);
  auto* srs = new OGRSpatialReference();
  ASSERT_EQ(OGRERR_NONE, srs->SetWellKnownGeogCS("WGS84"));
  orig->SetSpatialRef(srs);
  srs->Release();
  orig->SetNullable(false);
  OGRGeomCoordinatePrecision prec;
  prec.dfXYResolution = 0.01;
  orig->SetCoordinatePrecision(prec);

  auto cloned = std::make_unique<OGRGeomFieldDefn>(orig.get());
  EXPECT_TRUE(orig->IsSame(cloned.get()));
  EXPECT_STREQ("geom", cloned->GetNameRef());
  EXPECT_EQ(wkbPolygon, cloned->GetType());
  EXPECT_THAT(cloned->GetSpatialRef(), NotNull());
  EXPECT_FALSE(cloned->IsNullable());
  EXPECT_DOUBLE_EQ(0.01, cloned->GetCoordinatePrecision().dfXYResolution);

  OSRCleanup();
}

TEST(OgrGeomFieldDefnTest, CopyConstructorAndAssignment) {
  auto fd1 = std::make_unique<OGRGeomFieldDefn>("geom1", wkbLineString);
  auto* srs1 = new OGRSpatialReference();
  ASSERT_EQ(OGRERR_NONE, srs1->SetWellKnownGeogCS("WGS84"));
  fd1->SetSpatialRef(srs1);
  srs1->Release();
  fd1->SetNullable(false);

  // Copy constructor
  OGRGeomFieldDefn fd2(*fd1);
  EXPECT_TRUE(fd1->IsSame(&fd2));
  EXPECT_THAT(fd2.GetSpatialRef(), NotNull());

  // Copy assignment
  OGRGeomFieldDefn fd3("geom3", wkbPoint);
  auto* srs2 = new OGRSpatialReference();
  ASSERT_EQ(OGRERR_NONE, srs2->SetWellKnownGeogCS("NAD27"));
  fd3.SetSpatialRef(srs2);
  // fd3 = fd2 calls srs2->Dereference(), decrementing refcount to 1
  fd3 = fd2;
  EXPECT_TRUE(fd2.IsSame(&fd3));
  // Release srs2 to decrement refcount from 1 to 0 and delete it
  srs2->Release();

  // Self assignment
  fd3 = fd3;
  EXPECT_TRUE(fd2.IsSame(&fd3));

  OSRCleanup();
}

TEST(OgrGeomFieldDefnTest, Sealing) {
  auto fd = std::make_unique<OGRGeomFieldDefn>("geom", wkbPoint);
  fd->Seal();

  CPLErrorReset();
  {
    WithQuietHandler quiet;

    // Attempting mutations on sealed object should fail and log errors
    fd->SetName("new_name");
    EXPECT_STREQ("geom", fd->GetNameRef());
    EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());

    CPLErrorReset();
    fd->SetType(wkbPolygon);
    EXPECT_EQ(wkbPoint, fd->GetType());
    EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());

    CPLErrorReset();
    auto* srs = new OGRSpatialReference();
    ASSERT_EQ(OGRERR_NONE, srs->SetWellKnownGeogCS("WGS84"));
    fd->SetSpatialRef(srs);
    EXPECT_EQ(nullptr, fd->GetSpatialRef());
    EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
    srs->Release();

    CPLErrorReset();
    fd->SetNullable(false);
    EXPECT_TRUE(fd->IsNullable());
    EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());

    CPLErrorReset();
    OGRGeomCoordinatePrecision prec;
    prec.dfXYResolution = 0.05;
    fd->SetCoordinatePrecision(prec);
    EXPECT_NE(0.05, fd->GetCoordinatePrecision().dfXYResolution);
    EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
  }

  // Unseal
  fd->Unseal();
  fd->SetName("unsealed_name");
  EXPECT_STREQ("unsealed_name", fd->GetNameRef());

  // TemporaryUnsealer
  fd->Seal();
  {
    auto unsealer = fd->GetTemporaryUnsealer();
    fd->SetName("temp_unsealed");
  }
  // After TemporaryUnsealer destruction, object is sealed again
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    fd->SetName("should_fail");
    EXPECT_STREQ("temp_unsealed", fd->GetNameRef());
    EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
  }

  OSRCleanup();
}

TEST(OgrGeomFieldDefnTest, CApi) {
  OGRGeomFieldDefnH hDefn = OGR_GFld_Create("test_c", wkbLineString);
  ASSERT_THAT(hDefn, NotNull());

  EXPECT_STREQ("test_c", OGR_GFld_GetNameRef(hDefn));
  OGR_GFld_SetName(hDefn, "new_c_name");
  EXPECT_STREQ("new_c_name", OGR_GFld_GetNameRef(hDefn));

  EXPECT_EQ(wkbLineString, OGR_GFld_GetType(hDefn));
  OGR_GFld_SetType(hDefn, wkbCircularString);
  EXPECT_EQ(wkbCircularString, OGR_GFld_GetType(hDefn));

  // Non-linear disabled conversion in OGR_GFld_GetType
  OGRSetNonLinearGeometriesEnabledFlag(FALSE);
  EXPECT_EQ(wkbLineString, OGR_GFld_GetType(hDefn));
  OGRSetNonLinearGeometriesEnabledFlag(TRUE);
  EXPECT_EQ(wkbCircularString, OGR_GFld_GetType(hDefn));

  EXPECT_FALSE(OGR_GFld_IsIgnored(hDefn));
  OGR_GFld_SetIgnored(hDefn, TRUE);
  EXPECT_TRUE(OGR_GFld_IsIgnored(hDefn));
  OGR_GFld_SetIgnored(hDefn, FALSE);
  EXPECT_FALSE(OGR_GFld_IsIgnored(hDefn));

  EXPECT_TRUE(OGR_GFld_IsNullable(hDefn));
  OGR_GFld_SetNullable(hDefn, FALSE);
  EXPECT_FALSE(OGR_GFld_IsNullable(hDefn));

  EXPECT_EQ(nullptr, OGR_GFld_GetSpatialRef(hDefn));
  OGRSpatialReferenceH hSRS = OSRNewSpatialReference(nullptr);
  ASSERT_EQ(OGRERR_NONE, OSRSetWellKnownGeogCS(hSRS, "WGS84"));
  OGR_GFld_SetSpatialRef(hDefn, hSRS);
  EXPECT_THAT(OGR_GFld_GetSpatialRef(hDefn), NotNull());
  OSRRelease(hSRS);

  OGRGeomCoordinatePrecision prec;
  prec.dfXYResolution = 0.02;
  OGR_GFld_SetCoordinatePrecision(
      hDefn, reinterpret_cast<OGRGeomCoordinatePrecisionH>(&prec));
  OGRGeomCoordinatePrecisionH hPrec = OGR_GFld_GetCoordinatePrecision(hDefn);
  ASSERT_THAT(hPrec, NotNull());
  auto* pPrec = reinterpret_cast<OGRGeomCoordinatePrecision*>(hPrec);
  EXPECT_DOUBLE_EQ(0.02, pPrec->dfXYResolution);

  OGR_GFld_Destroy(hDefn);

  // Null safety check
  {
    WithQuietHandler quiet;
    OGR_GFld_Destroy(nullptr);
    EXPECT_STREQ("", OGR_GFld_GetNameRef(nullptr));
    EXPECT_EQ(wkbUnknown, OGR_GFld_GetType(nullptr));
    EXPECT_FALSE(OGR_GFld_IsIgnored(nullptr));
    EXPECT_EQ(nullptr, OGR_GFld_GetSpatialRef(nullptr));
  }

  OSRCleanup();
}

}  // namespace
}  // namespace autotest2
