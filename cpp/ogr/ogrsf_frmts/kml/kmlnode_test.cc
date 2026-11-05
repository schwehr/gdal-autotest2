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
// Tests KMLNode (kmlnode.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/kml/kmlnode.cpp

#include "third_party/gdal/ogr/ogrsf_frmts/kml/kmlnode.h"

#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/kml/kml.h"
#include "third_party/gdal/ogr/ogrsf_frmts/kml/kmlutility.h"
#include "third_party/gdal/ogr/ogrsf_frmts/kml/kmlvector.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

TEST(KmlNodeTest, Nodetype2String) {
  EXPECT_EQ("Empty", Nodetype2String(Empty));
  EXPECT_EQ("Rest", Nodetype2String(Rest));
  EXPECT_EQ("Mixed", Nodetype2String(Mixed));
  EXPECT_EQ("Point", Nodetype2String(Point));
  EXPECT_EQ("LineString", Nodetype2String(LineString));
  EXPECT_EQ("Polygon", Nodetype2String(Polygon));
  EXPECT_EQ("MultiGeometry", Nodetype2String(MultiGeometry));
  EXPECT_EQ("MultiPoint", Nodetype2String(MultiPoint));
  EXPECT_EQ("MultiLineString", Nodetype2String(MultiLineString));
  EXPECT_EQ("MultiPolygon", Nodetype2String(MultiPolygon));
  EXPECT_EQ("Unknown", Nodetype2String(Unknown));
}

TEST(KmlNodeTest, AccessorsAndPrint) {
  KMLNode root;
  root.setName("Root");
  root.setLevel(0);
  root.setType(Empty);
  EXPECT_EQ("Root", root.getName());
  EXPECT_EQ(0, root.getLevel());
  EXPECT_EQ(Empty, root.getType());
  EXPECT_EQ(nullptr, root.getParent());

  auto attr = std::make_unique<Attribute>();
  attr->sName = "attr1";
  attr->sValue = "val1";
  root.addAttribute(attr.release());

  root.addContent("first");
  root.appendContent(" line");
  EXPECT_EQ(1, root.numContent());
  EXPECT_EQ("first line", root.getContent(0));
  root.deleteContent(999);  // Out of bounds check
  EXPECT_EQ(1, root.numContent());
  root.deleteContent(0);
  EXPECT_EQ(0, root.numContent());

  auto child = std::make_unique<KMLNode>();
  child->setName("ChildLayer");
  child->setLevel(1);
  child->setParent(&root);
  child->setType(Point);
  child->setLayerNumber(0);
  EXPECT_EQ(0, child->getLayerNumber());
  EXPECT_EQ(&root, child->getParent());

  auto child_attr = std::make_unique<Attribute>();
  child_attr->sName = "k";
  child_attr->sValue = "v";
  child->addAttribute(child_attr.release());
  child->addContent("child_text");

  auto name_node = std::make_unique<KMLNode>();
  name_node->setName("name");
  name_node->addContent("MyName");
  child->addChildren(name_node.release());

  auto desc_node = std::make_unique<KMLNode>();
  desc_node->setName("description");
  desc_node->addContent("MyDescription");
  child->addChildren(desc_node.release());

  EXPECT_EQ("MyName", child->getNameElement());
  EXPECT_EQ("MyDescription", child->getDescriptionElement());

  auto child2 = std::make_unique<KMLNode>();
  child2->setName("ChildNoLayer");
  child2->setLevel(1);
  child2->setParent(&root);
  child2->setType(LineString);
  child2->setLayerNumber(-1);

  root.addChildren(child.release());
  root.addChildren(child2.release());
  EXPECT_EQ(2, root.countChildren());
  EXPECT_NE(nullptr, root.getChild(0));
  EXPECT_NE(nullptr, root.getChild(1));

  // Exercise all print paths: 0, 1, 2, 3
  root.print(0);
  root.print(1);
  root.print(2);
  root.print(3);

  // getNameElement and getDescriptionElement when empty
  KMLNode empty_info;
  EXPECT_EQ("", empty_info.getNameElement());
  EXPECT_EQ("", empty_info.getDescriptionElement());
  auto empty_name = std::make_unique<KMLNode>();
  empty_name->setName("name");
  empty_info.addChildren(empty_name.release());
  EXPECT_EQ("", empty_info.getNameElement());

  auto empty_desc = std::make_unique<KMLNode>();
  empty_desc->setName("description");
  empty_info.addChildren(empty_desc.release());
  EXPECT_EQ("", empty_info.getDescriptionElement());
}

TEST(KmlNodeTest, ClassifyAndRecursionLimit) {
  KMLVector kml;
  KMLNode node;
  node.setName("Point");

  // Recursion limit check
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(FALSE, node.classify(&kml, 32));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Too many recursion levels"));
    CPLErrorReset();
  }

  // 1. Point
  KMLNode pt_node;
  pt_node.setName("Point");
  EXPECT_EQ(TRUE, pt_node.classify(&kml));
  EXPECT_EQ(Point, pt_node.getType());

  // 2. LineString
  KMLNode ls_node;
  ls_node.setName("LineString");
  EXPECT_EQ(TRUE, ls_node.classify(&kml));
  EXPECT_EQ(LineString, ls_node.getType());

  // 3. Polygon
  KMLNode poly_node;
  poly_node.setName("Polygon");
  EXPECT_EQ(TRUE, poly_node.classify(&kml));
  EXPECT_EQ(Polygon, poly_node.getType());

  // 4. Rest (LinearRing)
  KMLNode ring_node;
  ring_node.setName("LinearRing");
  EXPECT_EQ(TRUE, ring_node.classify(&kml));
  EXPECT_EQ(Empty, ring_node.getType());

  // 5. Coordinates 2D vs 2.5D
  KMLNode coords_2d;
  coords_2d.setName("coordinates");
  coords_2d.addContent("1,2");
  EXPECT_EQ(TRUE, coords_2d.classify(&kml));
  EXPECT_FALSE(coords_2d.is25D());

  KMLNode coords_3d;
  coords_3d.setName("coordinates");
  coords_3d.addContent("1,2,3");
  EXPECT_EQ(TRUE, coords_3d.classify(&kml));
  EXPECT_TRUE(coords_3d.is25D());

  // 6. MultiGeometry classification
  // Homogeneous MultiPoint
  KMLNode mg_mp;
  mg_mp.setName("MultiPoint");
  auto pt1 = std::make_unique<KMLNode>();
  pt1->setName("Point");
  mg_mp.addChildren(pt1.release());
  EXPECT_EQ(TRUE, mg_mp.classify(&kml));
  EXPECT_EQ(MultiPoint, mg_mp.getType());

  // Homogeneous MultiLineString
  KMLNode mg_mls;
  mg_mls.setName("MultiLineString");
  auto ls1 = std::make_unique<KMLNode>();
  ls1->setName("LineString");
  mg_mls.addChildren(ls1.release());
  EXPECT_EQ(TRUE, mg_mls.classify(&kml));
  EXPECT_EQ(MultiLineString, mg_mls.getType());

  // Homogeneous MultiPolygon
  KMLNode mg_mpoly;
  mg_mpoly.setName("MultiPolygon");
  auto py1 = std::make_unique<KMLNode>();
  py1->setName("Polygon");
  mg_mpoly.addChildren(py1.release());
  EXPECT_EQ(TRUE, mg_mpoly.classify(&kml));
  EXPECT_EQ(MultiPolygon, mg_mpoly.getType());

  // Empty MultiGeometry -> MultiGeometry
  KMLNode mg_empty;
  mg_empty.setName("MultiGeometry");
  EXPECT_EQ(TRUE, mg_empty.classify(&kml));
  EXPECT_EQ(MultiGeometry, mg_empty.getType());

  // Mixed children in MultiGeometry -> MultiGeometry
  KMLNode mg_mixed;
  mg_mixed.setName("MultiGeometry");
  auto pt2 = std::make_unique<KMLNode>();
  pt2->setName("Point");
  auto ls2 = std::make_unique<KMLNode>();
  ls2->setName("LineString");
  mg_mixed.addChildren(pt2.release());
  mg_mixed.addChildren(ls2.release());
  EXPECT_EQ(TRUE, mg_mixed.classify(&kml));
  EXPECT_EQ(MultiGeometry, mg_mixed.getType());

  // Mixed children in Folder -> Mixed
  KMLNode folder_mixed;
  folder_mixed.setName("Folder");
  auto pt3 = std::make_unique<KMLNode>();
  pt3->setName("Point");
  auto ls3 = std::make_unique<KMLNode>();
  ls3->setName("LineString");
  folder_mixed.addChildren(pt3.release());
  folder_mixed.addChildren(ls3.release());
  EXPECT_EQ(TRUE, folder_mixed.classify(&kml));
  EXPECT_EQ(Mixed, folder_mixed.getType());

  // Recursion failure in child propagation
  KMLNode parent_fail;
  parent_fail.setName("Folder");
  auto child_fail = std::make_unique<KMLNode>();
  child_fail->setName("Folder");
  parent_fail.addChildren(child_fail.release());
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(FALSE, parent_fail.classify(&kml, 31));
  }
}

TEST(KmlNodeTest, HasOnlyEmptyAndEliminateEmpty) {
  KMLVector kml;

  // Tree with only Empty nodes
  KMLNode empty_root;
  empty_root.setType(Empty);
  auto empty_child = std::make_unique<KMLNode>();
  empty_child->setType(Empty);
  empty_root.addChildren(empty_child.release());
  EXPECT_TRUE(empty_root.hasOnlyEmpty());

  // Tree with non-empty child
  KMLNode non_empty_root;
  non_empty_root.setType(Empty);
  auto non_empty_child = std::make_unique<KMLNode>();
  non_empty_child->setType(Point);
  non_empty_root.addChildren(non_empty_child.release());
  EXPECT_FALSE(non_empty_root.hasOnlyEmpty());

  // Tree with empty child containing non-empty grandchild
  KMLNode grandchild_root;
  grandchild_root.setType(Empty);
  auto empty_mid = std::make_unique<KMLNode>();
  empty_mid->setType(Empty);
  auto non_empty_leaf = std::make_unique<KMLNode>();
  non_empty_leaf->setType(Point);
  empty_mid->addChildren(non_empty_leaf.release());
  grandchild_root.addChildren(empty_mid.release());
  EXPECT_FALSE(grandchild_root.hasOnlyEmpty());

  // EliminateEmpty
  KMLNode container_root;
  container_root.setName("Document");
  container_root.setType(Empty);

  auto empty_folder = std::make_unique<KMLNode>();
  empty_folder->setName("Folder");
  empty_folder->setType(Empty);

  auto keep_folder = std::make_unique<KMLNode>();
  keep_folder->setName("Folder");
  keep_folder->setType(Point);

  container_root.addChildren(empty_folder.release());
  container_root.addChildren(keep_folder.release());
  EXPECT_EQ(2, container_root.countChildren());

  container_root.eliminateEmpty(&kml);
  EXPECT_EQ(1, container_root.countChildren());
  EXPECT_EQ("Folder", container_root.getChild(0)->getName());
  EXPECT_EQ(Point, container_root.getChild(0)->getType());
}

TEST(KmlNodeTest, GetGeometryPointAndLineString) {
  // 1. Point 2D
  KMLNode pt_node;
  pt_node.setName("Point");
  auto pt_coords = std::make_unique<KMLNode>();
  pt_coords->setName("coordinates");
  pt_coords->addContent("-122.08,37.42");
  pt_node.addChildren(pt_coords.release());

  std::unique_ptr<OGRGeometry> geom(pt_node.getGeometry());
  ASSERT_NE(nullptr, geom);
  ASSERT_EQ(wkbPoint, geom->getGeometryType());
  OGRPoint* pt = geom->toPoint();
  EXPECT_DOUBLE_EQ(-122.08, pt->getX());
  EXPECT_DOUBLE_EQ(37.42, pt->getY());

  // 2. Point 3D with scientific notation
  KMLNode pt_node_3d;
  pt_node_3d.setName("Point");
  auto pt_coords_3d = std::make_unique<KMLNode>();
  pt_coords_3d->setName("coordinates");
  pt_coords_3d->addContent("+1.0e1,-2.0E-1,50.5");
  pt_node_3d.addChildren(pt_coords_3d.release());

  std::unique_ptr<OGRGeometry> geom_3d(pt_node_3d.getGeometry());
  ASSERT_NE(nullptr, geom_3d);
  ASSERT_EQ(wkbPoint25D, geom_3d->getGeometryType());
  OGRPoint* pt3 = geom_3d->toPoint();
  EXPECT_DOUBLE_EQ(10.0, pt3->getX());
  EXPECT_DOUBLE_EQ(-0.2, pt3->getY());
  EXPECT_DOUBLE_EQ(50.5, pt3->getZ());

  // 3. Point with no coordinates (or invalid coordinate format)
  KMLNode pt_node_empty;
  pt_node_empty.setName("Point");
  auto pt_coords_bad = std::make_unique<KMLNode>();
  pt_coords_bad->setName("coordinates");
  pt_coords_bad->addContent("not_a_valid_coord");
  pt_node_empty.addChildren(pt_coords_bad.release());

  std::unique_ptr<OGRGeometry> geom_empty(pt_node_empty.getGeometry());
  ASSERT_NE(nullptr, geom_empty);
  EXPECT_EQ(wkbPoint, geom_empty->getGeometryType());
  EXPECT_TRUE(geom_empty->IsEmpty());

  // 4. LineString 2D & 3D
  KMLNode ls_node;
  ls_node.setName("LineString");
  auto ls_coords = std::make_unique<KMLNode>();
  ls_coords->setName("coordinates");
  ls_coords->addContent("1,2");
  ls_coords->addContent("3,4,5");
  ls_coords->addContent("invalid");
  ls_node.addChildren(ls_coords.release());

  std::unique_ptr<OGRGeometry> ls_geom(ls_node.getGeometry());
  ASSERT_NE(nullptr, ls_geom);
  ASSERT_EQ(wkbLineString25D, ls_geom->getGeometryType());
  OGRLineString* ls = ls_geom->toLineString();
  EXPECT_EQ(2, ls->getNumPoints());
  EXPECT_DOUBLE_EQ(1.0, ls->getX(0));
  EXPECT_DOUBLE_EQ(2.0, ls->getY(0));
  EXPECT_DOUBLE_EQ(3.0, ls->getX(1));
  EXPECT_DOUBLE_EQ(4.0, ls->getY(1));
  EXPECT_DOUBLE_EQ(5.0, ls->getZ(1));
}

TEST(KmlNodeTest, GetGeometryPolygonAndMultiGeometry) {
  // 1. Polygon without outerBoundaryIs
  KMLNode poly_no_outer;
  poly_no_outer.setName("Polygon");
  std::unique_ptr<OGRGeometry> p_no_outer(poly_no_outer.getGeometry());
  ASSERT_NE(nullptr, p_no_outer);
  EXPECT_TRUE(p_no_outer->IsEmpty());

  // 2. Polygon with empty outerBoundaryIs
  KMLNode poly_empty_outer;
  poly_empty_outer.setName("Polygon");
  auto outer_empty = std::make_unique<KMLNode>();
  outer_empty->setName("outerBoundaryIs");
  poly_empty_outer.addChildren(outer_empty.release());
  std::unique_ptr<OGRGeometry> p_empty_outer(poly_empty_outer.getGeometry());
  ASSERT_NE(nullptr, p_empty_outer);
  EXPECT_TRUE(p_empty_outer->IsEmpty());

  // 3. Polygon with outerBoundaryIs having no valid coordinates
  KMLNode poly_bad_coords;
  poly_bad_coords.setName("Polygon");
  auto outer_bad = std::make_unique<KMLNode>();
  outer_bad->setName("outerBoundaryIs");
  auto ring_bad = std::make_unique<KMLNode>();
  ring_bad->setName("LinearRing");
  auto coords_bad = std::make_unique<KMLNode>();
  coords_bad->setName("coordinates");
  coords_bad->addContent("bad_coord");
  ring_bad->addChildren(coords_bad.release());
  outer_bad->addChildren(ring_bad.release());
  poly_bad_coords.addChildren(outer_bad.release());
  std::unique_ptr<OGRGeometry> p_bad_coords(poly_bad_coords.getGeometry());
  ASSERT_NE(nullptr, p_bad_coords);
  EXPECT_TRUE(p_bad_coords->IsEmpty());

  // 4. Polygon with outer boundary (2D and 3D) and inner boundaries
  KMLNode poly_full;
  poly_full.setName("Polygon");

  // Outer ring
  auto outer = std::make_unique<KMLNode>();
  outer->setName("outerBoundaryIs");
  auto outer_ring = std::make_unique<KMLNode>();
  outer_ring->setName("LinearRing");
  auto outer_coords = std::make_unique<KMLNode>();
  outer_coords->setName("coordinates");
  outer_coords->addContent("0,0,1");
  outer_coords->addContent("10,0");
  outer_coords->addContent("10,10");
  outer_coords->addContent("0,10");
  outer_coords->addContent("0,0,1");
  outer_ring->addChildren(outer_coords.release());
  outer->addChildren(outer_ring.release());
  poly_full.addChildren(outer.release());

  // Empty innerBoundaryIs
  auto empty_inner = std::make_unique<KMLNode>();
  empty_inner->setName("innerBoundaryIs");
  poly_full.addChildren(empty_inner.release());

  // First valid inner ring (2D and 3D points)
  auto inner1 = std::make_unique<KMLNode>();
  inner1->setName("innerBoundaryIs");
  auto inner_ring1 = std::make_unique<KMLNode>();
  inner_ring1->setName("LinearRing");
  auto inner_coords1 = std::make_unique<KMLNode>();
  inner_coords1->setName("coordinates");
  inner_coords1->addContent("1,1,2");
  inner_coords1->addContent("2,1");
  inner_coords1->addContent("2,2");
  inner_coords1->addContent("1,2");
  inner_coords1->addContent("1,1,2");
  inner_ring1->addChildren(inner_coords1.release());
  inner1->addChildren(inner_ring1.release());
  poly_full.addChildren(inner1.release());

  // Second valid inner ring
  auto inner2 = std::make_unique<KMLNode>();
  inner2->setName("innerBoundaryIs");
  auto inner_ring2 = std::make_unique<KMLNode>();
  inner_ring2->setName("LinearRing");
  auto inner_coords2 = std::make_unique<KMLNode>();
  inner_coords2->setName("coordinates");
  inner_coords2->addContent("5,5");
  inner_coords2->addContent("6,5");
  inner_coords2->addContent("6,6");
  inner_coords2->addContent("5,6");
  inner_coords2->addContent("5,5");
  inner_ring2->addChildren(inner_coords2.release());
  inner2->addChildren(inner_ring2.release());
  poly_full.addChildren(inner2.release());

  std::unique_ptr<OGRGeometry> full_poly(poly_full.getGeometry());
  ASSERT_NE(nullptr, full_poly);
  ASSERT_EQ(wkbPolygon25D, full_poly->getGeometryType());
  OGRPolygon* poPoly = full_poly->toPolygon();
  EXPECT_EQ(2, poPoly->getNumInteriorRings());
  EXPECT_NE(nullptr, poPoly->getExteriorRing());

  // 5. MultiGeometry: MultiPoint, MultiLineString, MultiPolygon,
  // GeometryCollection MultiPoint
  KMLNode mg_mp_node;
  mg_mp_node.setName("MultiGeometry");
  auto sub_pt = std::make_unique<KMLNode>();
  sub_pt->setName("Point");
  auto sub_pt_coord = std::make_unique<KMLNode>();
  sub_pt_coord->setName("coordinates");
  sub_pt_coord->addContent("1,1");
  sub_pt->addChildren(sub_pt_coord.release());
  mg_mp_node.addChildren(sub_pt.release());
  std::unique_ptr<OGRGeometry> g_mp(mg_mp_node.getGeometry(MultiPoint));
  ASSERT_NE(nullptr, g_mp);
  EXPECT_EQ(wkbMultiPoint, g_mp->getGeometryType());

  // MultiLineString
  KMLNode mg_mls_node;
  mg_mls_node.setName("MultiGeometry");
  auto sub_ls = std::make_unique<KMLNode>();
  sub_ls->setName("LineString");
  auto sub_ls_coord = std::make_unique<KMLNode>();
  sub_ls_coord->setName("coordinates");
  sub_ls_coord->addContent("1,1 2,2");
  sub_ls->addChildren(sub_ls_coord.release());
  mg_mls_node.addChildren(sub_ls.release());
  std::unique_ptr<OGRGeometry> g_mls(mg_mls_node.getGeometry(MultiLineString));
  ASSERT_NE(nullptr, g_mls);
  EXPECT_EQ(wkbMultiLineString, g_mls->getGeometryType());

  // MultiPolygon
  KMLNode mg_mpoly_node;
  mg_mpoly_node.setName("MultiPolygon");
  auto sub_poly = std::make_unique<KMLNode>();
  sub_poly->setName("Polygon");
  auto sub_poly_outer = std::make_unique<KMLNode>();
  sub_poly_outer->setName("outerBoundaryIs");
  auto sub_poly_ring = std::make_unique<KMLNode>();
  sub_poly_ring->setName("LinearRing");
  auto sub_poly_coord = std::make_unique<KMLNode>();
  sub_poly_coord->setName("coordinates");
  sub_poly_coord->addContent("0,0 1,0 1,1 0,1 0,0");
  sub_poly_ring->addChildren(sub_poly_coord.release());
  sub_poly_outer->addChildren(sub_poly_ring.release());
  sub_poly->addChildren(sub_poly_outer.release());
  mg_mpoly_node.addChildren(sub_poly.release());
  std::unique_ptr<OGRGeometry> g_mpoly(mg_mpoly_node.getGeometry(MultiPolygon));
  ASSERT_NE(nullptr, g_mpoly);
  EXPECT_EQ(wkbMultiPolygon, g_mpoly->getGeometryType());

  // GeometryCollection
  KMLNode mg_gc_node;
  mg_gc_node.setName("MultiGeometry");
  auto sub_pt2 = std::make_unique<KMLNode>();
  sub_pt2->setName("Point");
  auto sub_pt2_coord = std::make_unique<KMLNode>();
  sub_pt2_coord->setName("coordinates");
  sub_pt2_coord->addContent("2,2");
  sub_pt2->addChildren(sub_pt2_coord.release());
  mg_gc_node.addChildren(sub_pt2.release());
  std::unique_ptr<OGRGeometry> g_gc(mg_gc_node.getGeometry(MultiGeometry));
  ASSERT_NE(nullptr, g_gc);
  EXPECT_EQ(wkbGeometryCollection, g_gc->getGeometryType());
}

TEST(KmlNodeTest, GetNumFeaturesAndGetFeature) {
  KMLNode folder;
  folder.setName("Folder");
  folder.setType(Mixed);

  // Placemark 1: Point
  auto pm1 = std::make_unique<KMLNode>();
  pm1->setName("Placemark");
  pm1->setType(Point);
  auto name1 = std::make_unique<KMLNode>();
  name1->setName("name");
  name1->addContent("Feat1");
  pm1->addChildren(name1.release());
  auto pt1 = std::make_unique<KMLNode>();
  pt1->setName("Point");
  auto pt1_coord = std::make_unique<KMLNode>();
  pt1_coord->setName("coordinates");
  pt1_coord->addContent("10,20");
  pt1->addChildren(pt1_coord.release());
  pm1->addChildren(pt1.release());
  folder.addChildren(pm1.release());

  // Placemark 2: MultiPoint with MultiGeometry container
  auto pm2 = std::make_unique<KMLNode>();
  pm2->setName("Placemark");
  pm2->setType(MultiPoint);
  auto desc2 = std::make_unique<KMLNode>();
  desc2->setName("description");
  desc2->addContent("Desc2");
  pm2->addChildren(desc2.release());
  auto mg2 = std::make_unique<KMLNode>();
  mg2->setName("MultiGeometry");
  auto sub_pt = std::make_unique<KMLNode>();
  sub_pt->setName("Point");
  auto sub_coord = std::make_unique<KMLNode>();
  sub_coord->setName("coordinates");
  sub_coord->addContent("30,40");
  sub_pt->addChildren(sub_coord.release());
  mg2->addChildren(sub_pt.release());
  pm2->addChildren(mg2.release());
  folder.addChildren(pm2.release());

  // Placemark 3: Unsupported type (e.g. Empty) -> getFeature returns nullptr
  auto pm3 = std::make_unique<KMLNode>();
  pm3->setName("Placemark");
  pm3->setType(Empty);
  folder.addChildren(pm3.release());

  // Placemark 4: Point type but missing geometry child -> getFeature returns
  // nullptr
  auto pm4 = std::make_unique<KMLNode>();
  pm4->setName("Placemark");
  pm4->setType(Point);
  folder.addChildren(pm4.release());

  EXPECT_EQ(4, folder.getNumFeatures());

  int last_asked = -1;
  int last_count = -1;

  // Out of range feature
  EXPECT_EQ(nullptr, folder.getFeature(10, last_asked, last_count));

  // Feature 0
  std::unique_ptr<Feature> f0(folder.getFeature(0, last_asked, last_count));
  ASSERT_NE(nullptr, f0);
  EXPECT_EQ("Feat1", f0->sName);
  EXPECT_EQ(Point, f0->eType);
  ASSERT_NE(nullptr, f0->poGeom);
  EXPECT_EQ(wkbPoint, f0->poGeom->getGeometryType());

  // Feature 1 (sequential)
  std::unique_ptr<Feature> f1(folder.getFeature(1, last_asked, last_count));
  ASSERT_NE(nullptr, f1);
  EXPECT_EQ("Desc2", f1->sDescription);
  EXPECT_EQ(MultiPoint, f1->eType);

  // Non-sequential query back to feature 0
  std::unique_ptr<Feature> f0_again(
      folder.getFeature(0, last_asked, last_count));
  ASSERT_NE(nullptr, f0_again);

  // Feature 2 (Empty type -> returns nullptr)
  EXPECT_EQ(nullptr, folder.getFeature(2, last_asked, last_count));

  // Feature 3 (missing Point child -> returns nullptr)
  EXPECT_EQ(nullptr, folder.getFeature(3, last_asked, last_count));
}

}  // namespace
}  // namespace autotest2
