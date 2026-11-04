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
// Tests for GML utils (ogr/ogrsf_frmts/gmlutils/gmlutils.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/gmlutils/gmlutils.cpp
//   https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_gml.py

#include "third_party/gdal/ogr/ogrsf_frmts/gmlutils/gmlutils.h"

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_minixml.h"

namespace autotest2 {
namespace {

TEST(GmlUtilsTest, ExtractSrsNameFromGeometry) {
  std::string work;

  // 1. Empty or multiple geometry elements.
  const CPLXMLNode* empty_list[1] = {nullptr};
  EXPECT_EQ(nullptr, GML_ExtractSrsNameFromGeometry(empty_list, work, false));

  CPLXMLNode* dummy1 = CPLCreateXMLNode(nullptr, CXT_Element, "Point");
  CPLXMLNode* dummy2 = CPLCreateXMLNode(nullptr, CXT_Element, "Point");
  absl::Cleanup cleanup_dummies = [dummy1, dummy2] {
    CPLDestroyXMLNode(dummy1);
    CPLDestroyXMLNode(dummy2);
  };
  const CPLXMLNode* multi_list[3] = {dummy1, dummy2, nullptr};
  EXPECT_EQ(nullptr, GML_ExtractSrsNameFromGeometry(multi_list, work, false));

  // 2. Node without srsName.
  const CPLXMLNode* single_no_srs[2] = {dummy1, nullptr};
  EXPECT_EQ(nullptr,
            GML_ExtractSrsNameFromGeometry(single_no_srs, work, false));

  // 3. Node with EPSG:4326.
  CPLXMLNode* pt_srs = CPLCreateXMLNode(nullptr, CXT_Element, "Point");
  CPLAddXMLAttributeAndValue(pt_srs, "srsName", "EPSG:4326");
  absl::Cleanup cleanup_pt_srs = [pt_srs] { CPLDestroyXMLNode(pt_srs); };
  const CPLXMLNode* list_srs[2] = {pt_srs, nullptr};

  // With bConsiderEPSGAsURN = false.
  EXPECT_STREQ("EPSG:4326",
               GML_ExtractSrsNameFromGeometry(list_srs, work, false));

  // With bConsiderEPSGAsURN = true.
  EXPECT_STREQ("urn:ogc:def:crs:EPSG::4326",
               GML_ExtractSrsNameFromGeometry(list_srs, work, true));

  // 4. URL format.
  CPLXMLNode* pt_url = CPLCreateXMLNode(nullptr, CXT_Element, "Point");
  CPLAddXMLAttributeAndValue(pt_url, "srsName",
                             "http://www.opengis.net/gml/srs/epsg.xml#4326");
  absl::Cleanup cleanup_pt_url = [pt_url] { CPLDestroyXMLNode(pt_url); };
  const CPLXMLNode* list_url[2] = {pt_url, nullptr};
  EXPECT_STREQ("EPSG:4326",
               GML_ExtractSrsNameFromGeometry(list_url, work, false));

  // 5. Standard URN format.
  CPLXMLNode* pt_urn = CPLCreateXMLNode(nullptr, CXT_Element, "Point");
  CPLAddXMLAttributeAndValue(pt_urn, "srsName",
                             "urn:ogc:def:crs:OGC:1.3:CRS84");
  absl::Cleanup cleanup_pt_urn = [pt_urn] { CPLDestroyXMLNode(pt_urn); };
  const CPLXMLNode* list_urn[2] = {pt_urn, nullptr};
  EXPECT_STREQ("urn:ogc:def:crs:OGC:1.3:CRS84",
               GML_ExtractSrsNameFromGeometry(list_urn, work, false));
}

TEST(GmlUtilsTest, IsSRSLatLongOrder) {
  EXPECT_FALSE(GML_IsSRSLatLongOrder(nullptr));
  // Note: urn:...:4326 matches the internal strstr(":4326") shortcut.
  EXPECT_TRUE(GML_IsSRSLatLongOrder("urn:ogc:def:crs:EPSG::4326"));
  EXPECT_FALSE(GML_IsSRSLatLongOrder("EPSG:4326"));
  EXPECT_TRUE(GML_IsSRSLatLongOrder("urn:ogc:def:crs:EPSG::4269"));
  EXPECT_FALSE(GML_IsSRSLatLongOrder("urn:ogc:def:crs:EPSG::32631"));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(GML_IsSRSLatLongOrder("invalid_srs_name"));
    CPLErrorReset();
  }
}

TEST(GmlUtilsTest, IsLegitSRSName) {
  EXPECT_TRUE(GML_IsLegitSRSName("EPSG:4326"));
  EXPECT_TRUE(GML_IsLegitSRSName("urn:ogc:def:crs:EPSG::4326"));
  EXPECT_TRUE(GML_IsLegitSRSName("http://opengis.net/def/crs/EPSG/0/4326"));
  EXPECT_TRUE(GML_IsLegitSRSName("http://www.opengis.net/def/crs/EPSG/0/4326"));
  EXPECT_FALSE(GML_IsLegitSRSName("http://example.com/unsupported/crs"));
}

TEST(GmlUtilsTest, GetSRSName) {
  bool coord_swap = false;

  // 1. Null SRS.
  {
    char* srs = GML_GetSRSName(nullptr, SRSNAME_SHORT, &coord_swap);
    std::unique_ptr<char, CplFreeDeleter> srs_ptr(srs);
    EXPECT_STREQ("", srs);
    EXPECT_FALSE(coord_swap);
  }

  // 2. SRS with authority (EPSG:4326).
  OGRSpatialReference srs_4326;
  ASSERT_EQ(OGRERR_NONE, srs_4326.importFromEPSG(4326));
  srs_4326.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  {
    char* srs = GML_GetSRSName(&srs_4326, SRSNAME_SHORT, &coord_swap);
    std::unique_ptr<char, CplFreeDeleter> srs_ptr(srs);
    EXPECT_STREQ(" srsName=\"EPSG:4326\"", srs);
    EXPECT_FALSE(coord_swap);
  }

  {
    char* srs = GML_GetSRSName(&srs_4326, SRSNAME_OGC_URN, &coord_swap);
    std::unique_ptr<char, CplFreeDeleter> srs_ptr(srs);
    EXPECT_STREQ(" srsName=\"urn:ogc:def:crs:EPSG::4326\"", srs);
    // EPSG:4326 is lat/long; with OAMS_TRADITIONAL_GIS_ORDER, GML_GetSRSName
    // reports a coordinate swap for URN/URL forms.
    EXPECT_TRUE(coord_swap);
  }

  {
    char* srs = GML_GetSRSName(&srs_4326, SRSNAME_OGC_URL, &coord_swap);
    std::unique_ptr<char, CplFreeDeleter> srs_ptr(srs);
    EXPECT_STREQ(" srsName=\"http://www.opengis.net/def/crs/EPSG/0/4326\"",
                 srs);
    EXPECT_TRUE(coord_swap);
  }

  // 3. Custom SRS without authority.
  OGRSpatialReference srs_custom;
  ASSERT_EQ(OGRERR_NONE,
            srs_custom.importFromProj4(
                "+proj=merc +lon_0=0 +k=1 +x_0=0 +y_0=0 +datum=WGS84 +units=m "
                "+no_defs"));
  {
    char* srs = GML_GetSRSName(&srs_custom, SRSNAME_SHORT, &coord_swap);
    std::unique_ptr<char, CplFreeDeleter> srs_ptr(srs);
    EXPECT_STREQ("", srs);
    EXPECT_FALSE(coord_swap);
  }
}

TEST(GmlUtilsTest, BuildOGRGeometryFromListEmpty) {
  void* cache = GML_BuildOGRGeometryFromList_CreateCache();
  absl::Cleanup cache_cleaner = [cache] {
    GML_BuildOGRGeometryFromList_DestroyCache(cache);
  };

  const CPLXMLNode* empty_list[1] = {nullptr};
  EXPECT_EQ(nullptr,
            GML_BuildOGRGeometryFromList(empty_list, true, false, nullptr,
                                         false, GML_SWAP_AUTO, 0, cache));
}

TEST(GmlUtilsTest, BuildOGRGeometryFromListSinglePointAndCache) {
  void* cache = GML_BuildOGRGeometryFromList_CreateCache();
  absl::Cleanup cache_cleaner = [cache] {
    GML_BuildOGRGeometryFromList_DestroyCache(cache);
  };

  constexpr const char kGmlPoint[] = R"xml(
<gml:Point srsName="urn:ogc:def:crs:EPSG::4326">
  <gml:pos>50 10</gml:pos>
</gml:Point>
)xml";
  CPLXMLNode* pt_node = CPLParseXMLString(kGmlPoint);
  ASSERT_NE(nullptr, pt_node);
  absl::Cleanup pt_cleaner = [pt_node] { CPLDestroyXMLNode(pt_node); };

  const CPLXMLNode* list[2] = {pt_node, nullptr};

  // 1. Build geometry with coordinate swap (LatLong order with swap_auto).
  OGRGeometry* geom1 = GML_BuildOGRGeometryFromList(
      list, true, /*bInvertAxisOrderIfLatLong=*/true, nullptr, false,
      GML_SWAP_AUTO, 0, cache);
  ASSERT_NE(nullptr, geom1);
  std::unique_ptr<OGRGeometry> g1_ptr(geom1);
  EXPECT_EQ(wkbPoint, wkbFlatten(geom1->getGeometryType()));
  auto pt1 = dynamic_cast<OGRPoint*>(geom1);
  ASSERT_NE(nullptr, pt1);
  EXPECT_DOUBLE_EQ(10.0, pt1->getX());
  EXPECT_DOUBLE_EQ(50.0, pt1->getY());

  // 2. Build again with same SRS: exercises SRSCache poLastDesc fast-path.
  OGRGeometry* geom2 = GML_BuildOGRGeometryFromList(
      list, true, /*bInvertAxisOrderIfLatLong=*/false, nullptr, false,
      GML_SWAP_AUTO, 0, cache);
  ASSERT_NE(nullptr, geom2);
  std::unique_ptr<OGRGeometry> g2_ptr(geom2);
  auto pt2 = dynamic_cast<OGRPoint*>(geom2);
  ASSERT_NE(nullptr, pt2);
  EXPECT_DOUBLE_EQ(50.0, pt2->getX());
  EXPECT_DOUBLE_EQ(10.0, pt2->getY());

  // 3. Swap coordinates with GML_SWAP_YES and no SRS.
  constexpr const char kGmlPointNoSrs[] = R"xml(
<gml:Point>
  <gml:pos>50 10</gml:pos>
</gml:Point>
)xml";
  CPLXMLNode* pt_nosrs = CPLParseXMLString(kGmlPointNoSrs);
  ASSERT_NE(nullptr, pt_nosrs);
  absl::Cleanup nosrs_cleaner = [pt_nosrs] { CPLDestroyXMLNode(pt_nosrs); };
  const CPLXMLNode* list_nosrs[2] = {pt_nosrs, nullptr};

  OGRGeometry* geom3 = GML_BuildOGRGeometryFromList(
      list_nosrs, true, false, nullptr, false, GML_SWAP_YES, 0, cache);
  ASSERT_NE(nullptr, geom3);
  std::unique_ptr<OGRGeometry> g3_ptr(geom3);
  auto pt3 = dynamic_cast<OGRPoint*>(geom3);
  ASSERT_NE(nullptr, pt3);
  EXPECT_DOUBLE_EQ(10.0, pt3->getX());
  EXPECT_DOUBLE_EQ(50.0, pt3->getY());

  // 4. Default SRS lookup with a different SRS and invalid SRS in cache.
  OGRGeometry* geom4 = GML_BuildOGRGeometryFromList(
      list_nosrs, true, false, "EPSG:32631", false, GML_SWAP_NO, 0, cache);
  ASSERT_NE(nullptr, geom4);
  std::unique_ptr<OGRGeometry> g4_ptr(geom4);
  EXPECT_NE(nullptr, geom4->getSpatialReference());
  EXPECT_STREQ("32631",
               geom4->getSpatialReference()->GetAuthorityCode(nullptr));

  // Call again with first SRS to exercise oMap.find hit in SRSCache.
  OGRGeometry* geom5 = GML_BuildOGRGeometryFromList(
      list, true, false, nullptr, false, GML_SWAP_NO, 0, cache);
  ASSERT_NE(nullptr, geom5);
  std::unique_ptr<OGRGeometry> g5_ptr(geom5);
  EXPECT_NE(nullptr, geom5->getSpatialReference());
  EXPECT_STREQ("4326", geom5->getSpatialReference()->GetAuthorityCode(nullptr));

  // Call with invalid SRS in cache.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    OGRGeometry* geom6 = GML_BuildOGRGeometryFromList(
        list_nosrs, true, false, "invalid_crs_string", false, GML_SWAP_NO, 0,
        cache);
    ASSERT_NE(nullptr, geom6);
    std::unique_ptr<OGRGeometry> g6_ptr(geom6);
    EXPECT_EQ(nullptr, geom6->getSpatialReference());
    CPLErrorReset();
  }
}

TEST(GmlUtilsTest, BuildOGRGeometryFromListPolygonsAndMultipolygons) {
  void* cache = GML_BuildOGRGeometryFromList_CreateCache();
  absl::Cleanup cache_cleaner = [cache] {
    GML_BuildOGRGeometryFromList_DestroyCache(cache);
  };

  constexpr const char kGmlPoly1[] = R"xml(
<gml:Polygon>
  <gml:exterior>
    <gml:LinearRing>
      <gml:posList>0 0 0 1 1 1 1 0 0 0</gml:posList>
    </gml:LinearRing>
  </gml:exterior>
</gml:Polygon>
)xml";
  constexpr const char kGmlPoly2[] = R"xml(
<gml:Polygon>
  <gml:exterior>
    <gml:LinearRing>
      <gml:posList>2 2 2 3 3 3 3 2 2 2</gml:posList>
    </gml:LinearRing>
  </gml:exterior>
</gml:Polygon>
)xml";
  constexpr const char kGmlPoly3[] = R"xml(
<gml:Polygon>
  <gml:exterior>
    <gml:LinearRing>
      <gml:posList>4 4 4 5 5 5 5 4 4 4</gml:posList>
    </gml:LinearRing>
  </gml:exterior>
</gml:Polygon>
)xml";
  constexpr const char kGmlPt[] = R"xml(
<gml:Point><gml:pos>10 10</gml:pos></gml:Point>
)xml";

  CPLXMLNode* p1 = CPLParseXMLString(kGmlPoly1);
  CPLXMLNode* p2 = CPLParseXMLString(kGmlPoly2);
  CPLXMLNode* p3 = CPLParseXMLString(kGmlPoly3);
  CPLXMLNode* pt = CPLParseXMLString(kGmlPt);
  ASSERT_NE(nullptr, p1);
  ASSERT_NE(nullptr, p2);
  ASSERT_NE(nullptr, p3);
  ASSERT_NE(nullptr, pt);
  absl::Cleanup cleaner = [p1, p2, p3, pt] {
    CPLDestroyXMLNode(p1);
    CPLDestroyXMLNode(p2);
    CPLDestroyXMLNode(p3);
    CPLDestroyXMLNode(pt);
  };

  // 1. Two Polygons -> MultiPolygon.
  {
    const CPLXMLNode* list[3] = {p1, p2, nullptr};
    OGRGeometry* geom = GML_BuildOGRGeometryFromList(
        list, /*bTryToMakeMultipolygons=*/true, false, nullptr, false,
        GML_SWAP_NO, 0, cache);
    ASSERT_NE(nullptr, geom);
    std::unique_ptr<OGRGeometry> g_ptr(geom);
    EXPECT_EQ(wkbMultiPolygon, wkbFlatten(geom->getGeometryType()));
    EXPECT_EQ(2, geom->toMultiPolygon()->getNumGeometries());
  }

  // 2. Three Polygons -> MultiPolygon with 3 parts.
  {
    const CPLXMLNode* list[4] = {p1, p2, p3, nullptr};
    OGRGeometry* geom = GML_BuildOGRGeometryFromList(
        list, /*bTryToMakeMultipolygons=*/true, false, nullptr, false,
        GML_SWAP_NO, 0, cache);
    ASSERT_NE(nullptr, geom);
    std::unique_ptr<OGRGeometry> g_ptr(geom);
    EXPECT_EQ(wkbMultiPolygon, wkbFlatten(geom->getGeometryType()));
    EXPECT_EQ(3, geom->toMultiPolygon()->getNumGeometries());
  }

  // 3. MultiPolygon + MultiPolygon merging.
  constexpr const char kGmlMultiPoly1[] = R"xml(
<gml:MultiPolygon>
  <gml:polygonMember>
    <gml:Polygon><gml:exterior><gml:LinearRing><gml:posList>0 0 0 1 1 1 0 0</gml:posList></gml:LinearRing></gml:exterior></gml:Polygon>
  </gml:polygonMember>
</gml:MultiPolygon>
)xml";
  constexpr const char kGmlMultiPoly2[] = R"xml(
<gml:MultiPolygon>
  <gml:polygonMember>
    <gml:Polygon><gml:exterior><gml:LinearRing><gml:posList>2 2 2 3 3 3 2 2</gml:posList></gml:LinearRing></gml:exterior></gml:Polygon>
  </gml:polygonMember>
</gml:MultiPolygon>
)xml";
  CPLXMLNode* mp1 = CPLParseXMLString(kGmlMultiPoly1);
  CPLXMLNode* mp2 = CPLParseXMLString(kGmlMultiPoly2);
  ASSERT_NE(nullptr, mp1);
  ASSERT_NE(nullptr, mp2);
  absl::Cleanup mp_cleaner = [mp1, mp2] {
    CPLDestroyXMLNode(mp1);
    CPLDestroyXMLNode(mp2);
  };

  {
    const CPLXMLNode* list[3] = {mp1, mp2, nullptr};
    OGRGeometry* geom = GML_BuildOGRGeometryFromList(
        list, /*bTryToMakeMultipolygons=*/true, false, nullptr, false,
        GML_SWAP_NO, 0, cache);
    ASSERT_NE(nullptr, geom);
    std::unique_ptr<OGRGeometry> g_ptr(geom);
    EXPECT_EQ(wkbMultiPolygon, wkbFlatten(geom->getGeometryType()));
    EXPECT_EQ(2, geom->toMultiPolygon()->getNumGeometries());
  }

  // 4. MultiPolygon + Point with bTryToMakeMultipolygons = true restarts with
  // bTryToMakeMultipolygons = false producing GeometryCollection.
  {
    const CPLXMLNode* list[3] = {mp1, pt, nullptr};
    OGRGeometry* geom = GML_BuildOGRGeometryFromList(
        list, /*bTryToMakeMultipolygons=*/true, false, nullptr, false,
        GML_SWAP_NO, 0, cache);
    ASSERT_NE(nullptr, geom);
    std::unique_ptr<OGRGeometry> g_ptr(geom);
    EXPECT_EQ(wkbGeometryCollection, wkbFlatten(geom->getGeometryType()));
  }

  // 5. Two points with bTryToMakeMultipolygons = false -> GeometryCollection.
  {
    const CPLXMLNode* list[3] = {pt, pt, nullptr};
    OGRGeometry* geom = GML_BuildOGRGeometryFromList(
        list, /*bTryToMakeMultipolygons=*/false, false, nullptr, false,
        GML_SWAP_NO, 0, cache);
    ASSERT_NE(nullptr, geom);
    std::unique_ptr<OGRGeometry> g_ptr(geom);
    EXPECT_EQ(wkbGeometryCollection, wkbFlatten(geom->getGeometryType()));
  }
}

// TODO: b/335317901 - GMLUtils achieves 98.98% line coverage. Track
// bFaceHoleNegative and GML_SWAP_AUTO with pszDefaultSRSName.

}  // namespace
}  // namespace autotest2
