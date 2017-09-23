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
//
// See also:
//   https://trac.osgeo.org/gdal/browser/trunk/gdal/ogr/gml2ogrgeometry.cpp
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/ogr/ogr_gml_read.py

#include "gmock.h"
#include "gunit.h"
#include "third_party/absl/memory/memory.h"
#include "autotest2/cpp/util/cpl_cstringlist.h"
#include "autotest2/cpp/util/error_handler.h"
#include "ogr/ogr_api.h"
#include "ogr/ogr_core.h"
#include "ogr/ogr_geometry.h"
#include "ogr/ogr_spatialref.h"
#include "port/cpl_conv.h"
#include "port/cpl_csv.h"
#include "port/cpl_error.h"
#include "port/cpl_string.h"
#include "port/cpl_vsi.h"

namespace autotest2 {
namespace {

TEST(GmlTest, Empty) {
  WithQuietHandler error_handler;
  EXPECT_EQ(nullptr, OGR_G_CreateFromGML(nullptr));
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("empty"));
  EXPECT_EQ(nullptr, OGR_G_CreateFromGML(""));
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("empty"));
}

TEST(GmlTest, Junk) {
  WithQuietHandler error_handler;
  EXPECT_EQ(nullptr, OGR_G_CreateFromGML("abc"));
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("recognized"));
}

TEST(GmlTest, InvalidXmlJunk) {
  WithQuietHandler error_handler;
  EXPECT_EQ(nullptr, OGR_G_CreateFromGML("<a></differs>"));
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("matching"));
}

TEST(GmlTest, ValidXmlJunk) {
  WithQuietHandler error_handler;
  EXPECT_EQ(nullptr, OGR_G_CreateFromGML("<a></a>"));
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("recognized"));
}

TEST(GmlTest, ValidXmlJunkWithNamespacePrefixs) {
  // Trigger the ':' present case in BareGMLElement().
  EXPECT_EQ(nullptr, OGR_G_CreateFromGML("<gml:a></gml:a>"));
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("recognized"));
}

TEST(GmlTest, XmlHeaderAndValidXmlJunk) {
WithQuietHandler error_handler;  EXPECT_EQ(nullptr,
            OGR_G_CreateFromGML(
                R"xml(<?xml version="1.0" encoding="utf-8" ?><a></a>"))xml"));
}

TEST(GmlTest, ValidXmlComment) {
  EXPECT_EQ(nullptr, OGR_G_CreateFromGML("<!-- comments are skipped -->"));
}

TEST(GmlTest, ValidPoint) {
  constexpr char kGml[] =
      "<gml:Point><gml:coordinates>-0.9,0.8,0.7</gml:coordinates></gml:Point>";
  OGRGeometryH geom_handle = OGR_G_CreateFromGML(kGml);
  ASSERT_NE(nullptr, geom_handle);
  auto geom = absl::WrapUnique(static_cast<OGRGeometry *>(geom_handle));
  EXPECT_EQ(wkbPoint25D, geom->getGeometryType());
}

TEST(GmlTest, InvalidPointCoordinate) {
  WithQuietHandler error_handler;
  constexpr char kGml[] =
      "<gml:Point><gml:coordinates>1</gml:coordinates></gml:Point>";
  EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGml));
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("Corrupt"));
}

TEST(GmlTest, ArcByBulge) {
  constexpr char kGml[] =
      "<gml:ArcByBulge><gml:posList>2 0 -2 0</gml:posList>"
      "<gml:bulge>2</gml:bulge><gml:normal>-1</gml:normal></gml:ArcByBulge>";
  OGRGeometryH geom_handle = OGR_G_CreateFromGML(kGml);
  ASSERT_NE(nullptr, geom_handle);
  auto geom = absl::WrapUnique(static_cast<OGRGeometry *>(geom_handle));
  EXPECT_EQ(wkbCircularString, geom->getGeometryType());
}

TEST(GmlTest, ArcByBulgeInvalid) {
  WithQuietHandler error_handler;
  // Bulge is missing a child.
  constexpr char kGml[] = "<ArcByBulge><bulge/></ArcByBulge>";
  EXPECT_EQ(nullptr, OGR_G_CreateFromGML(kGml));
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("bulge element"));
}

// TODO(schwehr): Expand test coverage.

}  // namespace
}  // namespace autotest2
