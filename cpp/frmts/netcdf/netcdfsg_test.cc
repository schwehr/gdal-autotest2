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

#include "third_party/gdal/frmts/netcdf/netcdfsg.h"

#include <cstring>
#include <filesystem>  // NOLINT
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "netcdf.h"
#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/frmts/netcdf/netcdf_cf_constants.h"
#include "third_party/gdal/frmts/netcdf/netcdfdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::nccfdriver::getCFVersion;
using ::nccfdriver::getGeometryType;
using ::nccfdriver::LINE;
using ::nccfdriver::MULTILINE;
using ::nccfdriver::MULTIPOINT;
using ::nccfdriver::MULTIPOLYGON;
using ::nccfdriver::NONE;
using ::nccfdriver::POINT;
using ::nccfdriver::Point;
using ::nccfdriver::POLYGON;
using ::nccfdriver::scanForGeometryContainers;
using ::nccfdriver::SG_Exception_BadFeature;
using ::nccfdriver::SG_Exception_BadPoint;
using ::nccfdriver::SG_Exception_BadSum;
using ::nccfdriver::SG_Exception_Dep;
using ::nccfdriver::SG_Exception_Dim_MM;
using ::nccfdriver::SG_Exception_EmptyDim;
using ::nccfdriver::SG_Exception_Existential;
using ::nccfdriver::SG_Exception_General_Malformed;
using ::nccfdriver::SG_Exception_Not1D;
using ::nccfdriver::SG_Exception_Value_Required;
using ::nccfdriver::SG_Exception_Value_Violation;
using ::nccfdriver::SGeometry_PropertyScanner;
using ::nccfdriver::SGeometry_Reader;
using ::nccfdriver::UNSUPPORTED;
using ::testing::HasSubstr;

class NetCDFSGTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALAllRegister(); }
};

TEST_F(NetCDFSGTest, PointClass) {
  Point pt2(2);
  EXPECT_EQ(2, pt2.getOrder());
  pt2[0] = 12.5;
  pt2[1] = 45.75;
  EXPECT_DOUBLE_EQ(12.5, pt2[0]);
  EXPECT_DOUBLE_EQ(45.75, pt2[1]);

  Point pt3(3);
  EXPECT_EQ(3, pt3.getOrder());
  pt3[0] = 1.0;
  pt3[1] = 2.0;
  pt3[2] = 3.0;
  EXPECT_DOUBLE_EQ(1.0, pt3[0]);
  EXPECT_DOUBLE_EQ(2.0, pt3[1]);
  EXPECT_DOUBLE_EQ(3.0, pt3[2]);
}

TEST_F(NetCDFSGTest, Exceptions) {
  SG_Exception_Dim_MM mm("cont", "f1", "f2");
  EXPECT_THAT(mm.get_err_msg(), HasSubstr("do not match"));

  SG_Exception_Existential ex("cont", "prop");
  EXPECT_THAT(ex.get_err_msg(), HasSubstr("is missing"));

  SG_Exception_Dep dep("cont", "a1", "a2");
  EXPECT_THAT(dep.get_err_msg(), HasSubstr("may not exist without"));

  SG_Exception_BadSum bs("cont", "s1", "s2");
  EXPECT_THAT(bs.get_err_msg(), HasSubstr("do not match"));

  SG_Exception_BadFeature bf;
  EXPECT_THAT(bf.get_err_msg(), HasSubstr("Unsupported or unrecognized"));

  SG_Exception_BadPoint bp;
  EXPECT_THAT(bp.get_err_msg(), HasSubstr("invalid point"));

  SG_Exception_Not1D n1d;
  EXPECT_THAT(n1d.get_err_msg(), HasSubstr("not one dimensional"));

  SG_Exception_EmptyDim ed;
  EXPECT_THAT(ed.get_err_msg(), HasSubstr("must have length > 0"));

  SG_Exception_General_Malformed gm("something");
  EXPECT_THAT(gm.get_err_msg(), HasSubstr("Corruption or malformed"));

  SG_Exception_Value_Violation vv("cont", "type", "bad");
  EXPECT_THAT(vv.get_err_msg(), HasSubstr("may not be bad"));

  SG_Exception_Value_Required vr("cont", "type", "exp");
  EXPECT_THAT(vr.get_err_msg(), HasSubstr("must be exp"));
}

TEST_F(NetCDFSGTest, GetCFVersion) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "cf_ver.nc").string();

  int ncid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_create(filename.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));
  // No conventions attribute
  EXPECT_DOUBLE_EQ(-1.0, getCFVersion(ncid));

  // Non-matching conventions attribute
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, NC_GLOBAL, "Conventions",
                                      strlen("COARDS"), "COARDS"));
  EXPECT_DOUBLE_EQ(-1.0, getCFVersion(ncid));

  // Matching CF conventions
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, NC_GLOBAL, "Conventions",
                                      strlen("CF-1.8"), "CF-1.8"));
  EXPECT_DOUBLE_EQ(1.8, getCFVersion(ncid));

  ASSERT_EQ(NC_NOERR, nc_close(ncid));
  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFSGTest, GetGeometryType) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "geom_type.nc").string();

  int ncid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_create(filename.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  int dim_dummy = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim", 1, &dim_dummy));

  // 1. Missing attribute -> NONE
  int v_none = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_none", NC_INT, 0, nullptr, &v_none));
  EXPECT_EQ(NONE, getGeometryType(ncid, v_none));

  // 2. Unknown attribute -> UNSUPPORTED
  int v_unsupp = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_unsupp", NC_INT, 0, nullptr, &v_unsupp));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_unsupp, CF_SG_GEOMETRY_TYPE,
                                      strlen("unknown"), "unknown"));
  EXPECT_EQ(UNSUPPORTED, getGeometryType(ncid, v_unsupp));

  // 3. Point without node_count -> POINT
  int v_point = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_point", NC_INT, 0, nullptr, &v_point));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, v_point, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_POINT), CF_SG_TYPE_POINT));
  EXPECT_EQ(POINT, getGeometryType(ncid, v_point));

  // 4. Point with node_count -> MULTIPOINT
  int v_mpoint = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_mpoint", NC_INT, 0, nullptr, &v_mpoint));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, v_mpoint, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_POINT), CF_SG_TYPE_POINT));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_mpoint, CF_SG_NODE_COUNT,
                                      strlen("dummy"), "dummy"));
  EXPECT_EQ(MULTIPOINT, getGeometryType(ncid, v_mpoint));

  // 5. Line without part_node_count -> LINE
  int v_line = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_line", NC_INT, 0, nullptr, &v_line));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, v_line, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_LINE), CF_SG_TYPE_LINE));
  EXPECT_EQ(LINE, getGeometryType(ncid, v_line));

  // 6. Line with part_node_count -> MULTILINE
  int v_mline = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_mline", NC_INT, 0, nullptr, &v_mline));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, v_mline, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_LINE), CF_SG_TYPE_LINE));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_mline, CF_SG_PART_NODE_COUNT,
                                      strlen("dummy"), "dummy"));
  EXPECT_EQ(MULTILINE, getGeometryType(ncid, v_mline));

  // 7. Poly without part_node_count and without interior_ring -> POLYGON
  int v_poly = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_poly", NC_INT, 0, nullptr, &v_poly));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, v_poly, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_POLY), CF_SG_TYPE_POLY));
  EXPECT_EQ(POLYGON, getGeometryType(ncid, v_poly));

  // 8. Poly with part_node_count -> MULTIPOLYGON
  int v_mpoly1 = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_mpoly1", NC_INT, 0, nullptr, &v_mpoly1));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, v_mpoly1, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_POLY), CF_SG_TYPE_POLY));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_mpoly1, CF_SG_PART_NODE_COUNT,
                                      strlen("dummy"), "dummy"));
  EXPECT_EQ(MULTIPOLYGON, getGeometryType(ncid, v_mpoly1));

  // 9. Poly with interior_ring -> MULTIPOLYGON
  int v_mpoly2 = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_mpoly2", NC_INT, 0, nullptr, &v_mpoly2));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, v_mpoly2, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_POLY), CF_SG_TYPE_POLY));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_mpoly2, CF_SG_INTERIOR_RING,
                                      strlen("dummy"), "dummy"));
  EXPECT_EQ(MULTIPOLYGON, getGeometryType(ncid, v_mpoly2));

  ASSERT_EQ(NC_NOERR, nc_close(ncid));
  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFSGTest, ScanForGeometryContainersAndPropertyScanner) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "scan_prop.nc").string();

  // Test scanForGeometryContainers with invalid ncid
  std::set<int> ids;
  EXPECT_EQ(-1, scanForGeometryContainers(-999, ids));

  int ncid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_create(filename.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  // Create container variable
  int geom_cid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "geom_container", NC_CHAR, 0, nullptr, &geom_cid));

  // Create data variable 1 referencing geom_container with
  // OGR_SG_ORIGINAL_LAYERNAME
  int data1 = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "prop1", NC_INT, 0, nullptr, &data1));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, data1, CF_SG_GEOMETRY,
                            strlen("geom_container"), "geom_container"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, data1, OGR_SG_ORIGINAL_LAYERNAME,
                                      strlen("custom_prop1"), "custom_prop1"));

  // Create data variable 2 referencing geom_container without
  // OGR_SG_ORIGINAL_LAYERNAME
  int data2 = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "prop2", NC_INT, 0, nullptr, &data2));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, data2, CF_SG_GEOMETRY,
                            strlen("geom_container"), "geom_container"));

  // Create data variable 3 referencing a non-existent container
  int data3 = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "prop3", NC_INT, 0, nullptr, &data3));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, data3, CF_SG_GEOMETRY,
                                      strlen("no_such_var"), "no_such_var"));

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

  // Scan geometry containers
  EXPECT_EQ(0, scanForGeometryContainers(ncid, ids));
  EXPECT_EQ(1, ids.size());
  EXPECT_TRUE(ids.find(geom_cid) != ids.end());

  // Test SGeometry_PropertyScanner
  SGeometry_PropertyScanner scanner(ncid, geom_cid);
  EXPECT_EQ(2, scanner.ids().size());
  EXPECT_EQ(2, scanner.headers().size());
  EXPECT_STREQ("custom_prop1", scanner.headers()[0].c_str());
  EXPECT_STREQ("prop2", scanner.headers()[1].c_str());

  // Invalid container id
  SGeometry_PropertyScanner invalid_scanner(ncid, -999);
  EXPECT_TRUE(invalid_scanner.ids().empty());

  ASSERT_EQ(NC_NOERR, nc_close(ncid));
  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFSGTest, ReaderPoint2DAnd3D) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "reader_pt.nc").string();

  int ncid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_create(filename.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  int dim_nodes = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "nodes", 2, &dim_nodes));

  // Coordinates X, Y, Z
  int x_var = -1;
  int y_var = -1;
  int z_var = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "x", NC_DOUBLE, 1, &dim_nodes, &x_var));
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "y", NC_DOUBLE, 1, &dim_nodes, &y_var));
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "z", NC_DOUBLE, 1, &dim_nodes, &z_var));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, x_var, CF_AXIS, 1, "X"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, y_var, CF_AXIS, 1, "Y"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, z_var, CF_AXIS, 1, "Z"));

  // 2D Point Container
  int pt2d_var = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "pt2d", NC_CHAR, 0, nullptr, &pt2d_var));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, pt2d_var, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_POINT), CF_SG_TYPE_POINT));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, pt2d_var, CF_SG_NODE_COORDINATES,
                                      strlen("x y"), "x y"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, pt2d_var, CF_GRD_MAPPING,
                                      strlen("crs"), "crs"));

  // 3D Point Container
  int pt3d_var = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "pt3d", NC_CHAR, 0, nullptr, &pt3d_var));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, pt3d_var, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_POINT), CF_SG_TYPE_POINT));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, pt3d_var, CF_SG_NODE_COORDINATES,
                                      strlen("x y z"), "x y z"));

  // CRS var
  int crs_var = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "crs", NC_CHAR, 0, nullptr, &crs_var));

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

  double x_data[2] = {1.5, 3.5};
  double y_data[2] = {2.5, 4.5};
  double z_data[2] = {10.0, 20.0};
  ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, x_var, x_data));
  ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, y_var, y_data));
  ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, z_var, z_data));

  // Test 2D Point reader
  {
    SGeometry_Reader reader(ncid, pt2d_var);
    EXPECT_EQ(ncid, reader.get_ncID());
    EXPECT_EQ(2, reader.get_axisCount());
    EXPECT_EQ(POINT, reader.getGeometryType());
    EXPECT_EQ(2, reader.get_geometry_count());
    EXPECT_EQ("pt2d", reader.getContainerName());
    EXPECT_EQ(pt2d_var, reader.getContainerId());
    EXPECT_EQ(dim_nodes, reader.getInstDim());
    EXPECT_EQ(2, reader.getInstDimLen());
    EXPECT_EQ("crs", reader.getGridMappingName());
    EXPECT_EQ(crs_var, reader.getGridMappingVarID());
    EXPECT_EQ(2, reader.getNodeCoordVars().size());

    Point& p0 = reader[0];
    EXPECT_DOUBLE_EQ(1.5, p0[0]);
    EXPECT_DOUBLE_EQ(2.5, p0[1]);

    std::vector<unsigned char> wkb0 = reader.serializeToWKB(0);
    EXPECT_FALSE(wkb0.empty());

    // Out of bounds read
    EXPECT_THROW(reader[999], SG_Exception_BadPoint);
  }

  // Test 3D Point reader
  {
    SGeometry_Reader reader3d(ncid, pt3d_var);
    EXPECT_EQ(3, reader3d.get_axisCount());
    Point& p1 = reader3d[1];
    EXPECT_DOUBLE_EQ(3.5, p1[0]);
    EXPECT_DOUBLE_EQ(4.5, p1[1]);
    EXPECT_DOUBLE_EQ(20.0, p1[2]);

    std::vector<unsigned char> wkb1 = reader3d.serializeToWKB(1);
    EXPECT_FALSE(wkb1.empty());
  }

  ASSERT_EQ(NC_NOERR, nc_close(ncid));
  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFSGTest, ReaderLineAndMultiLine) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "reader_line.nc").string();

  int ncid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_create(filename.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  int dim_nodes = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "nodes", 5, &dim_nodes));
  int dim_lines = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "lines", 2, &dim_lines));
  int dim_parts = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "parts", 3, &dim_parts));

  int x_var = -1;
  int y_var = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "x", NC_DOUBLE, 1, &dim_nodes, &x_var));
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "y", NC_DOUBLE, 1, &dim_nodes, &y_var));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, x_var, CF_AXIS, 1, "X"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, y_var, CF_AXIS, 1, "Y"));

  // Line: node_count = [2, 3] (total 5 nodes)
  int nc_var = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "node_counts", NC_INT, 1, &dim_lines, &nc_var));

  int line_var = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "line_geom", NC_CHAR, 0, nullptr, &line_var));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, line_var, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_LINE), CF_SG_TYPE_LINE));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, line_var, CF_SG_NODE_COORDINATES,
                                      strlen("x y"), "x y"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, line_var, CF_SG_NODE_COUNT,
                                      strlen("node_counts"), "node_counts"));

  // MultiLine: node_count = [2, 3], part_node_count = [2, 1, 2] (total 5 nodes)
  int pnc_var = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "pnode_counts", NC_INT, 1, &dim_parts, &pnc_var));

  int mline_var = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "mline_geom", NC_CHAR, 0, nullptr, &mline_var));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, mline_var, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_LINE), CF_SG_TYPE_LINE));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, mline_var, CF_SG_NODE_COORDINATES,
                                      strlen("x y"), "x y"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, mline_var, CF_SG_NODE_COUNT,
                                      strlen("node_counts"), "node_counts"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, mline_var, CF_SG_PART_NODE_COUNT,
                                      strlen("pnode_counts"), "pnode_counts"));

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

  double x_data[5] = {0, 1, 2, 3, 4};
  double y_data[5] = {0, 1, 2, 3, 4};
  int nc_data[2] = {2, 3};
  int pnc_data[3] = {2, 1, 2};
  ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, x_var, x_data));
  ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, y_var, y_data));
  ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, nc_var, nc_data));
  ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, pnc_var, pnc_data));

  // Test Line
  {
    SGeometry_Reader line_reader(ncid, line_var);
    EXPECT_EQ(LINE, line_reader.getGeometryType());
    EXPECT_EQ(2, line_reader.get_geometry_count());
    auto wkb0 = line_reader.serializeToWKB(0);
    EXPECT_FALSE(wkb0.empty());
    auto wkb1 = line_reader.serializeToWKB(1);
    EXPECT_FALSE(wkb1.empty());
  }

  // Test MultiLine
  {
    SGeometry_Reader mline_reader(ncid, mline_var);
    EXPECT_EQ(MULTILINE, mline_reader.getGeometryType());
    EXPECT_EQ(2, mline_reader.get_geometry_count());
    auto wkb0 = mline_reader.serializeToWKB(0);
    EXPECT_FALSE(wkb0.empty());
    auto wkb1 = mline_reader.serializeToWKB(1);
    EXPECT_FALSE(wkb1.empty());
  }

  ASSERT_EQ(NC_NOERR, nc_close(ncid));
  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFSGTest, ReaderPolygonAndMultiPolygonWithInteriors) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "reader_poly.nc").string();

  int ncid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_create(filename.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  // Polygon exterior only: 5 nodes
  // MultiPolygon with interior ring: 5 exterior + 5 interior = 10 nodes
  int dim_nodes = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "nodes", 10, &dim_nodes));
  int dim_polys = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "polys", 1, &dim_polys));
  int dim_parts = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "parts", 2, &dim_parts));

  int x_var = -1;
  int y_var = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "x", NC_DOUBLE, 1, &dim_nodes, &x_var));
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "y", NC_DOUBLE, 1, &dim_nodes, &y_var));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, x_var, CF_AXIS, 1, "X"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, y_var, CF_AXIS, 1, "Y"));

  // Node count = [10]
  int nc_var = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "nc", NC_INT, 1, &dim_polys, &nc_var));
  // Part node count = [5, 5]
  int pnc_var = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "pnc", NC_INT, 1, &dim_parts, &pnc_var));
  // Interior ring = [0, 1]
  int ir_var = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "ir", NC_INT, 1, &dim_parts, &ir_var));

  // Polygon Container (exterior only using 5 nodes)
  int dim_ext = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "ext_nodes", 5, &dim_ext));
  int x_ext = -1;
  int y_ext = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "x_ext", NC_DOUBLE, 1, &dim_ext, &x_ext));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "y_ext", NC_DOUBLE, 1, &dim_ext, &y_ext));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, x_ext, CF_AXIS, 1, "X"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, y_ext, CF_AXIS, 1, "Y"));
  int nc_ext = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "nc_ext", NC_INT, 1, &dim_polys, &nc_ext));

  int poly_var = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "poly_geom", NC_CHAR, 0, nullptr, &poly_var));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, poly_var, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_POLY), CF_SG_TYPE_POLY));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, poly_var, CF_SG_NODE_COORDINATES,
                                      strlen("x_ext y_ext"), "x_ext y_ext"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, poly_var, CF_SG_NODE_COUNT,
                                      strlen("nc_ext"), "nc_ext"));

  // MultiPolygon with interior ring
  int mpoly_var = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "mpoly_geom", NC_CHAR, 0, nullptr, &mpoly_var));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, mpoly_var, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_POLY), CF_SG_TYPE_POLY));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, mpoly_var, CF_SG_NODE_COORDINATES,
                                      strlen("x y"), "x y"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, mpoly_var, CF_SG_NODE_COUNT,
                                      strlen("nc"), "nc"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, mpoly_var, CF_SG_PART_NODE_COUNT,
                                      strlen("pnc"), "pnc"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, mpoly_var, CF_SG_INTERIOR_RING,
                                      strlen("ir"), "ir"));

  // MultiPolygon without interior rings
  int mpoly_no_ir = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "mpoly_no_ir", NC_CHAR, 0, nullptr, &mpoly_no_ir));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, mpoly_no_ir, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_POLY), CF_SG_TYPE_POLY));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, mpoly_no_ir, CF_SG_NODE_COORDINATES,
                                      strlen("x y"), "x y"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, mpoly_no_ir, CF_SG_NODE_COUNT,
                                      strlen("nc"), "nc"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, mpoly_no_ir, CF_SG_PART_NODE_COUNT,
                                      strlen("pnc"), "pnc"));

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

  double x_data[10] = {0, 10, 10, 0, 0, 2, 8, 8, 2, 2};
  double y_data[10] = {0, 0, 10, 10, 0, 2, 2, 8, 8, 2};
  int nc_data[1] = {10};
  int pnc_data[2] = {5, 5};
  int ir_data[2] = {0, 1};

  ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, x_var, x_data));
  ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, y_var, y_data));
  ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, nc_var, nc_data));
  ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, pnc_var, pnc_data));
  ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, ir_var, ir_data));

  int nc_ext_data[1] = {5};
  ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, x_ext, x_data));
  ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, y_ext, y_data));
  ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, nc_ext, nc_ext_data));

  // Polygon
  {
    SGeometry_Reader poly_reader(ncid, poly_var);
    EXPECT_EQ(POLYGON, poly_reader.getGeometryType());
    EXPECT_EQ(1, poly_reader.get_geometry_count());
    auto wkb = poly_reader.serializeToWKB(0);
    EXPECT_FALSE(wkb.empty());
  }

  // MultiPolygon with interior ring
  {
    SGeometry_Reader mpoly_reader(ncid, mpoly_var);
    EXPECT_EQ(MULTIPOLYGON, mpoly_reader.getGeometryType());
    EXPECT_EQ(1, mpoly_reader.get_geometry_count());
    auto wkb = mpoly_reader.serializeToWKB(0);
    EXPECT_FALSE(wkb.empty());
  }

  // MultiPolygon without interior rings
  {
    SGeometry_Reader mpoly_no_ir_reader(ncid, mpoly_no_ir);
    EXPECT_EQ(MULTIPOLYGON, mpoly_no_ir_reader.getGeometryType());
    EXPECT_EQ(1, mpoly_no_ir_reader.get_geometry_count());
    auto wkb = mpoly_no_ir_reader.serializeToWKB(0);
    EXPECT_FALSE(wkb.empty());
  }

  ASSERT_EQ(NC_NOERR, nc_close(ncid));
  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFSGTest, ReaderMultiPoint) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "reader_mpoint.nc")
          .string();

  int ncid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_create(filename.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  int dim_nodes = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "nodes", 3, &dim_nodes));
  int dim_features = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "features", 1, &dim_features));

  int x_var = -1;
  int y_var = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "x", NC_DOUBLE, 1, &dim_nodes, &x_var));
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "y", NC_DOUBLE, 1, &dim_nodes, &y_var));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, x_var, CF_AXIS, 1, "X"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, y_var, CF_AXIS, 1, "Y"));

  int nc_var = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "nc", NC_INT, 1, &dim_features, &nc_var));

  int mp_var = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "mp_geom", NC_CHAR, 0, nullptr, &mp_var));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, mp_var, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_POINT), CF_SG_TYPE_POINT));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, mp_var, CF_SG_NODE_COORDINATES,
                                      strlen("x y"), "x y"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, mp_var, CF_SG_NODE_COUNT,
                                      strlen("nc"), "nc"));

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

  double x_data[3] = {1, 2, 3};
  double y_data[3] = {4, 5, 6};
  int nc_data[1] = {3};
  ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, x_var, x_data));
  ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, y_var, y_data));
  ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, nc_var, nc_data));

  SGeometry_Reader reader(ncid, mp_var);
  EXPECT_EQ(MULTIPOINT, reader.getGeometryType());
  EXPECT_EQ(1, reader.get_geometry_count());
  auto wkb = reader.serializeToWKB(0);
  EXPECT_FALSE(wkb.empty());

  ASSERT_EQ(NC_NOERR, nc_close(ncid));
  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFSGTest, ReaderValidationFailures) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "reader_val.nc").string();

  int ncid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_create(filename.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  int dim_nodes = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "nodes", 4, &dim_nodes));
  int dim_feats = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "feats", 2, &dim_feats));

  // Invalid container ID
  EXPECT_THROW(SGeometry_Reader(ncid, -999), SG_Exception_Existential);

  // Var without geometry_type
  int v_nogt = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "nogt", NC_CHAR, 0, nullptr, &v_nogt));
  EXPECT_THROW(SGeometry_Reader(ncid, v_nogt), SG_Exception_Existential);

  // Negative node_count
  int v_neg_nc = -1;
  int nc_neg = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "nc_neg", NC_INT, 1, &dim_feats, &nc_neg));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_neg_nc", NC_CHAR, 0, nullptr, &v_neg_nc));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, v_neg_nc, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_LINE), CF_SG_TYPE_LINE));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_neg_nc, CF_SG_NODE_COUNT,
                                      strlen("nc_neg"), "nc_neg"));

  // Negative part_node_count
  int v_neg_pnc = -1;
  int pnc_neg = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "pnc_neg", NC_INT, 1, &dim_feats, &pnc_neg));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_neg_pnc", NC_CHAR, 0, nullptr, &v_neg_pnc));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, v_neg_pnc, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_LINE), CF_SG_TYPE_LINE));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_neg_pnc, CF_SG_PART_NODE_COUNT,
                                      strlen("pnc_neg"), "pnc_neg"));

  // Invalid interior_ring (value 2 instead of 0 or 1)
  int v_bad_ir = -1;
  int ir_bad = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "ir_bad", NC_INT, 1, &dim_feats, &ir_bad));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_bad_ir", NC_CHAR, 0, nullptr, &v_bad_ir));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, v_bad_ir, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_POLY), CF_SG_TYPE_POLY));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_bad_ir, CF_SG_INTERIOR_RING,
                                      strlen("ir_bad"), "ir_bad"));

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

  int neg_val[2] = {-1, 5};
  ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, nc_neg, neg_val));
  ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, pnc_neg, neg_val));
  int bad_ir_val[2] = {0, 2};
  ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, ir_bad, bad_ir_val));

  EXPECT_THROW(SGeometry_Reader(ncid, v_neg_nc), SG_Exception_Value_Violation);
  EXPECT_THROW(SGeometry_Reader(ncid, v_neg_pnc), SG_Exception_Value_Violation);
  EXPECT_THROW(SGeometry_Reader(ncid, v_bad_ir), SG_Exception_Value_Required);

  ASSERT_EQ(NC_NOERR, nc_close(ncid));
  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFSGTest, ReaderSumAndDepFailures) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "reader_sum.nc").string();

  int ncid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_create(filename.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  int dim_feats = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "feats", 1, &dim_feats));
  int dim_parts = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "parts", 2, &dim_parts));

  // Case 1: pnode_counts exists but node_counts missing
  int pnc_var = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "pnc", NC_INT, 1, &dim_parts, &pnc_var));
  int v_pnc_no_nc = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_pnc_no_nc", NC_CHAR, 0, nullptr, &v_pnc_no_nc));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, v_pnc_no_nc, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_LINE), CF_SG_TYPE_LINE));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_pnc_no_nc, CF_SG_PART_NODE_COUNT,
                                      strlen("pnc"), "pnc"));

  // Case 2: node_counts and part_node_counts sum mismatch
  int nc_var = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "nc", NC_INT, 1, &dim_feats, &nc_var));
  int v_sum_mm = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_sum_mm", NC_CHAR, 0, nullptr, &v_sum_mm));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, v_sum_mm, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_LINE), CF_SG_TYPE_LINE));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_sum_mm, CF_SG_NODE_COUNT,
                                      strlen("nc"), "nc"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_sum_mm, CF_SG_PART_NODE_COUNT,
                                      strlen("pnc"), "pnc"));

  // Case 3: interior_ring without part_node_count
  int ir_var = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "ir", NC_INT, 1, &dim_parts, &ir_var));
  int v_ir_no_pnc = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_ir_no_pnc", NC_CHAR, 0, nullptr, &v_ir_no_pnc));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, v_ir_no_pnc, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_POLY), CF_SG_TYPE_POLY));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_ir_no_pnc, CF_SG_INTERIOR_RING,
                                      strlen("ir"), "ir"));

  // Case 4: interior_ring size != part_node_count size
  int dim_parts3 = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "parts3", 3, &dim_parts3));
  int ir_var3 = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "ir3", NC_INT, 1, &dim_parts3, &ir_var3));
  int nc_match = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "nc_match", NC_INT, 1, &dim_feats, &nc_match));
  int v_ir_size_mm = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_ir_size_mm", NC_CHAR, 0, nullptr,
                                 &v_ir_size_mm));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, v_ir_size_mm, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_POLY), CF_SG_TYPE_POLY));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_ir_size_mm, CF_SG_NODE_COUNT,
                                      strlen("nc_match"), "nc_match"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_ir_size_mm, CF_SG_PART_NODE_COUNT,
                                      strlen("pnc"), "pnc"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_ir_size_mm, CF_SG_INTERIOR_RING,
                                      strlen("ir3"), "ir3"));

  // Case 5: node_coordinates attribute missing
  int v_no_nc = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_no_nc", NC_CHAR, 0, nullptr, &v_no_nc));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, v_no_nc, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_POINT), CF_SG_TYPE_POINT));

  // Case 6: node coordinates missing axis attribute
  int dim_c = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_c", 2, &dim_c));
  int c_var = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "c_no_axis", NC_DOUBLE, 1, &dim_c, &c_var));
  int v_bad_axis = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_bad_axis", NC_CHAR, 0, nullptr, &v_bad_axis));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, v_bad_axis, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_POINT), CF_SG_TYPE_POINT));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_bad_axis, CF_SG_NODE_COORDINATES,
                                      strlen("c_no_axis"), "c_no_axis"));

  // Case 7: only X coordinate (insufficient axis, touple_order < 2)
  int x_only = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "x_only", NC_DOUBLE, 1, &dim_c, &x_only));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, x_only, CF_AXIS, 1, "X"));
  int v_x_only = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_x_only", NC_CHAR, 0, nullptr, &v_x_only));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, v_x_only, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_POINT), CF_SG_TYPE_POINT));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_x_only, CF_SG_NODE_COORDINATES,
                                      strlen("x_only"), "x_only"));

  // Case 8: coordinate variable not 1D
  int dim_2d_coords[2] = {dim_c, dim_c};
  int x_2d = -1;
  int y_1d = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "x_2d", NC_DOUBLE, 2, dim_2d_coords, &x_2d));
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "y_1d", NC_DOUBLE, 1, &dim_c, &y_1d));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, x_2d, CF_AXIS, 1, "X"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, y_1d, CF_AXIS, 1, "Y"));
  int v_2d_coord = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_2d_coord", NC_CHAR, 0, nullptr, &v_2d_coord));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, v_2d_coord, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_POINT), CF_SG_TYPE_POINT));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, v_2d_coord, CF_SG_NODE_COORDINATES,
                                      strlen("x_2d y_1d"), "x_2d y_1d"));

  // Case 9: X and Y have different dimensions
  int dim_d = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_d", 3, &dim_d));
  int y_dim_d = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "y_dim_d", NC_DOUBLE, 1, &dim_d, &y_dim_d));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, y_dim_d, CF_AXIS, 1, "Y"));
  int v_coord_dim_mm = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_coord_dim_mm", NC_CHAR, 0, nullptr,
                                 &v_coord_dim_mm));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, v_coord_dim_mm, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_POINT), CF_SG_TYPE_POINT));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, v_coord_dim_mm, CF_SG_NODE_COORDINATES,
                            strlen("x_only y_dim_d"), "x_only y_dim_d"));

  // Case 10: dimension length 0 -> empty dimension exception
  int dim_zero = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_zero", 0,
                                 &dim_zero));  // unlimited dim with 0 length
  int x_zero = -1;
  int y_zero = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "x_zero", NC_DOUBLE, 1, &dim_zero, &x_zero));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "y_zero", NC_DOUBLE, 1, &dim_zero, &y_zero));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, x_zero, CF_AXIS, 1, "X"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, y_zero, CF_AXIS, 1, "Y"));
  int v_zero_dim = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_zero_dim", NC_CHAR, 0, nullptr, &v_zero_dim));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, v_zero_dim, CF_SG_GEOMETRY_TYPE,
                            strlen(CF_SG_TYPE_POINT), CF_SG_TYPE_POINT));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, v_zero_dim, CF_SG_NODE_COORDINATES,
                            strlen("x_zero y_zero"), "x_zero y_zero"));

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

  int pnc_val[2] = {2, 3};  // sum = 5
  int nc_val[1] = {10};     // sum = 10 -> mismatch
  int nc_match_val[1] = {5};
  ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, pnc_var, pnc_val));
  ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, nc_var, nc_val));
  ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, nc_match, nc_match_val));
  int ir_val[2] = {0, 1};
  ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, ir_var, ir_val));
  int ir_val3[3] = {0, 1, 1};
  ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, ir_var3, ir_val3));

  EXPECT_THROW(SGeometry_Reader(ncid, v_pnc_no_nc), SG_Exception_Dep);
  EXPECT_THROW(SGeometry_Reader(ncid, v_sum_mm), SG_Exception_BadSum);
  EXPECT_THROW(SGeometry_Reader(ncid, v_ir_no_pnc), SG_Exception_Dep);
  EXPECT_THROW(SGeometry_Reader(ncid, v_ir_size_mm), SG_Exception_Dim_MM);
  EXPECT_THROW(SGeometry_Reader(ncid, v_no_nc), SG_Exception_Existential);
  EXPECT_THROW(SGeometry_Reader(ncid, v_bad_axis), SG_Exception_Dep);
  EXPECT_THROW(SGeometry_Reader(ncid, v_x_only), SG_Exception_Existential);
  EXPECT_THROW(SGeometry_Reader(ncid, v_2d_coord), SG_Exception_Not1D);
  EXPECT_THROW(SGeometry_Reader(ncid, v_coord_dim_mm), SG_Exception_Dim_MM);
  EXPECT_THROW(SGeometry_Reader(ncid, v_zero_dim), SG_Exception_EmptyDim);

  ASSERT_EQ(NC_NOERR, nc_close(ncid));
  VSIUnlink(filename.c_str());
}

}  // namespace
}  // namespace autotest2
