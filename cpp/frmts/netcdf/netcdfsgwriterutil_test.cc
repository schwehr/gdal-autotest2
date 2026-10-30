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

#include "third_party/gdal/frmts/netcdf/netcdfsgwriterutil.h"

#include <cstddef>
#include <filesystem>  // NOLINT
#include <memory>
#include <string>
#include <vector>

#include "netcdf.h"
#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/frmts/netcdf/netcdf_cf_constants.h"
#include "third_party/gdal/frmts/netcdf/netcdfdataset.h"
#include "third_party/gdal/frmts/netcdf/netcdfsg.h"
#include "third_party/gdal/frmts/netcdf/netcdfvirtual.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::nccfdriver::LINE;
using ::nccfdriver::MULTILINE;
using ::nccfdriver::MULTIPOINT;
using ::nccfdriver::MULTIPOLYGON;
using ::nccfdriver::ncLayer_SG_Metadata;
using ::nccfdriver::netCDFVID;
using ::nccfdriver::OGR_NCScribe;
using ::nccfdriver::OGR_SGFS_NC_Byte_Transaction;
using ::nccfdriver::OGR_SGFS_NC_Char_Transaction;
using ::nccfdriver::OGR_SGFS_NC_CharA_Transaction;
using ::nccfdriver::OGR_SGFS_NC_Double_Transaction;
using ::nccfdriver::OGR_SGFS_NC_Float_Transaction;
using ::nccfdriver::OGR_SGFS_NC_Int64_Transaction;
using ::nccfdriver::OGR_SGFS_NC_Int_Transaction;
using ::nccfdriver::OGR_SGFS_NC_Short_Transaction;
using ::nccfdriver::OGR_SGFS_NC_String_Transaction;
using ::nccfdriver::OGR_SGFS_NC_UByte_Transaction;
using ::nccfdriver::OGR_SGFS_NC_UInt64_Transaction;
using ::nccfdriver::OGR_SGFS_NC_UInt_Transaction;
using ::nccfdriver::OGR_SGFS_NC_UShort_Transaction;
using ::nccfdriver::POINT;
using ::nccfdriver::POLYGON;
using ::nccfdriver::SG_Exception_BadFeature;
using ::nccfdriver::SGeometry_Feature;
using ::nccfdriver::SGWriter_Exception;
using ::nccfdriver::SGWriter_Exception_NCDefFailure;
using ::nccfdriver::SGWriter_Exception_NCDelFailure;
using ::nccfdriver::SGWriter_Exception_NCInqFailure;
using ::nccfdriver::SGWriter_Exception_NCWriteFailure;
using ::nccfdriver::SGWriter_Exception_NullGeometry;
using ::nccfdriver::WBuffer;
using ::nccfdriver::WBufferManager;
using ::nccfdriver::write_Geometry_Container;
using ::nccfdriver::WTransactionLog;
using ::testing::HasSubstr;

class NetCDFSGWriterUtilTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALAllRegister(); }
};

TEST_F(NetCDFSGWriterUtilTest, WBufferAndBufferManager) {
  WBuffer buf1;
  EXPECT_EQ(0, buf1.getUsage());
  buf1.getUsage() = 50;
  EXPECT_EQ(50, buf1.getUsage());
  buf1.getUsage() -= 20;
  EXPECT_EQ(30, buf1.getUsage());

  WBuffer buf2;
  buf2.getUsage() = 40;

  WBufferManager mgr(100);
  mgr.addBuffer(&buf1);
  mgr.addBuffer(&buf2);
  EXPECT_FALSE(mgr.isOverQuota());  // 30 + 40 = 70 <= 100

  buf1.getUsage() += 40;  // now 70 + 40 = 110 > 100
  EXPECT_TRUE(mgr.isOverQuota());

  mgr.adjustLimit(200);
  EXPECT_FALSE(mgr.isOverQuota());

  buf1.reset();
  EXPECT_EQ(0, buf1.getUsage());
}

TEST_F(NetCDFSGWriterUtilTest, SGeometryFeatureVariants) {
  OGRFeatureDefn* poDefn = new OGRFeatureDefn("test");
  poDefn->Reference();

  // 1. Null geometry
  {
    std::unique_ptr<OGRFeature> feat(new OGRFeature(poDefn));
    EXPECT_THROW(SGeometry_Feature sgf(*feat), SGWriter_Exception_NullGeometry);
  }

  // 2. Point
  {
    std::unique_ptr<OGRFeature> feat(new OGRFeature(poDefn));
    auto* pt = new OGRPoint(10.0, 20.0);
    feat->SetGeometryDirectly(pt);
    SGeometry_Feature sgf(*feat);
    EXPECT_EQ(POINT, sgf.getType());
    EXPECT_EQ(1, sgf.getTotalNodeCount());
    EXPECT_EQ(1, sgf.getTotalPartCount());
    EXPECT_FALSE(sgf.getHasInteriorRing());
    const OGRPoint& p = sgf.getPoint(0, 0);
    EXPECT_DOUBLE_EQ(10.0, p.getX());
    EXPECT_DOUBLE_EQ(20.0, p.getY());
  }

  // 3. MultiPoint
  {
    std::unique_ptr<OGRFeature> feat(new OGRFeature(poDefn));
    auto* mp = new OGRMultiPoint();
    mp->addGeometryDirectly(new OGRPoint(1.0, 2.0));
    mp->addGeometryDirectly(new OGRPoint(3.0, 4.0));
    feat->SetGeometryDirectly(mp);
    SGeometry_Feature sgf(*feat);
    EXPECT_EQ(MULTIPOINT, sgf.getType());
    EXPECT_EQ(2, sgf.getTotalNodeCount());
    EXPECT_EQ(2, sgf.getTotalPartCount());
    const OGRPoint& p1 = sgf.getPoint(1, 0);
    EXPECT_DOUBLE_EQ(3.0, p1.getX());
  }

  // 4. LineString
  {
    std::unique_ptr<OGRFeature> feat(new OGRFeature(poDefn));
    auto* ls = new OGRLineString();
    ls->addPoint(0, 0);
    ls->addPoint(1, 1);
    ls->addPoint(2, 2);
    feat->SetGeometryDirectly(ls);
    SGeometry_Feature sgf(*feat);
    EXPECT_EQ(LINE, sgf.getType());
    EXPECT_EQ(3, sgf.getTotalNodeCount());
    EXPECT_EQ(1, sgf.getTotalPartCount());
    const OGRPoint& p2 = sgf.getPoint(0, 2);
    EXPECT_DOUBLE_EQ(2.0, p2.getX());
  }

  // 5. MultiLineString
  {
    std::unique_ptr<OGRFeature> feat(new OGRFeature(poDefn));
    auto* mls = new OGRMultiLineString();
    auto* l1 = new OGRLineString();
    l1->addPoint(0, 0);
    l1->addPoint(1, 1);
    auto* l2 = new OGRLineString();
    l2->addPoint(2, 2);
    l2->addPoint(3, 3);
    mls->addGeometryDirectly(l1);
    mls->addGeometryDirectly(l2);
    feat->SetGeometryDirectly(mls);
    SGeometry_Feature sgf(*feat);
    EXPECT_EQ(MULTILINE, sgf.getType());
    EXPECT_EQ(4, sgf.getTotalNodeCount());
    EXPECT_EQ(2, sgf.getTotalPartCount());
    const OGRPoint& p = sgf.getPoint(1, 1);
    EXPECT_DOUBLE_EQ(3.0, p.getX());
  }

  // 6. Polygon with interior ring
  {
    std::unique_ptr<OGRFeature> feat(new OGRFeature(poDefn));
    auto* poly = new OGRPolygon();
    auto* ext = new OGRLinearRing();
    ext->addPoint(0, 0);
    ext->addPoint(10, 0);
    ext->addPoint(10, 10);
    ext->addPoint(0, 10);
    ext->addPoint(0, 0);
    poly->addRingDirectly(ext);

    auto* in = new OGRLinearRing();
    in->addPoint(2, 2);
    in->addPoint(8, 2);
    in->addPoint(8, 8);
    in->addPoint(2, 8);
    in->addPoint(2, 2);
    poly->addRingDirectly(in);

    feat->SetGeometryDirectly(poly);
    SGeometry_Feature sgf(*feat);
    EXPECT_EQ(POLYGON, sgf.getType());
    EXPECT_EQ(10, sgf.getTotalNodeCount());
    EXPECT_EQ(2, sgf.getTotalPartCount());
    EXPECT_TRUE(sgf.getHasInteriorRing());
    const OGRPoint& p_ext = sgf.getPoint(0, 1);
    EXPECT_DOUBLE_EQ(10.0, p_ext.getX());
    const OGRPoint& p_in = sgf.getPoint(1, 2);
    EXPECT_DOUBLE_EQ(8.0, p_in.getX());
  }

  // 7. MultiPolygon with interior ring
  {
    std::unique_ptr<OGRFeature> feat(new OGRFeature(poDefn));
    auto* mpoly = new OGRMultiPolygon();
    auto* poly = new OGRPolygon();
    auto* ext = new OGRLinearRing();
    ext->addPoint(0, 0);
    ext->addPoint(5, 0);
    ext->addPoint(5, 5);
    ext->addPoint(0, 5);
    ext->addPoint(0, 0);
    poly->addRingDirectly(ext);

    auto* in = new OGRLinearRing();
    in->addPoint(1, 1);
    in->addPoint(2, 1);
    in->addPoint(2, 2);
    in->addPoint(1, 2);
    in->addPoint(1, 1);
    poly->addRingDirectly(in);

    mpoly->addGeometryDirectly(poly);
    feat->SetGeometryDirectly(mpoly);
    SGeometry_Feature sgf(*feat);
    EXPECT_EQ(MULTIPOLYGON, sgf.getType());
    EXPECT_EQ(10, sgf.getTotalNodeCount());
    EXPECT_EQ(2, sgf.getTotalPartCount());
    EXPECT_TRUE(sgf.getHasInteriorRing());
    EXPECT_FALSE(sgf.IsPartAtIndInteriorRing(0));
    EXPECT_TRUE(sgf.IsPartAtIndInteriorRing(1));
    const OGRPoint& p = sgf.getPoint(1, 0);
    EXPECT_DOUBLE_EQ(1.0, p.getX());
  }

  // 8. Unsupported geometry
  {
    std::unique_ptr<OGRFeature> feat(new OGRFeature(poDefn));
    feat->SetGeometryDirectly(new OGRGeometryCollection());
    EXPECT_THROW(SGeometry_Feature sgf(*feat), SG_Exception_BadFeature);
  }

  // 9. Null geometry
  {
    std::unique_ptr<OGRFeature> feat(new OGRFeature(poDefn));
    feat->SetGeometryDirectly(nullptr);
    EXPECT_THROW(SGeometry_Feature sgf(*feat), SGWriter_Exception_NullGeometry);
  }

  poDefn->Release();
}

TEST_F(NetCDFSGWriterUtilTest, Exceptions) {
  SGWriter_Exception g;
  EXPECT_THAT(g.get_err_msg(), HasSubstr("general error"));

  SGWriter_Exception_NCWriteFailure wf("layer", "fn", "ft");
  EXPECT_THAT(wf.get_err_msg(), HasSubstr("could not be written"));

  SGWriter_Exception_NCInqFailure inf("layer", "fn", "ft");
  EXPECT_THAT(inf.get_err_msg(), HasSubstr("could not be read"));

  SGWriter_Exception_NCDefFailure df("layer", "fn", "ft");
  EXPECT_THAT(df.get_err_msg(), HasSubstr("could not be defined"));

  SGWriter_Exception_NullGeometry ng;
  EXPECT_THAT(ng.get_err_msg(), HasSubstr("null"));

  SGWriter_Exception_NCDelFailure delf("layer", "what");
  EXPECT_THAT(delf.get_err_msg(), HasSubstr("Failed to delete"));
}

TEST_F(NetCDFSGWriterUtilTest, WriteGeometryContainerVariants) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "write_geom_cont.nc")
          .string();

  int ncid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_create(filename.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  std::vector<std::string> coords2d = {"x", "y"};

  // Invalid ncid
  EXPECT_THROW(write_Geometry_Container(-999, "bad", POINT, coords2d),
               nccfdriver::SGWriter_Exception_NCDefFailure);

  // Unsupported type
  EXPECT_THROW(write_Geometry_Container(ncid, "bad_type",
                                        nccfdriver::UNSUPPORTED, coords2d),
               SG_Exception_BadFeature);

  // 1. POINT
  int cid_pt = write_Geometry_Container(ncid, "gc_point", POINT, coords2d);
  EXPECT_GE(cid_pt, 0);

  // 2. MULTIPOINT
  int cid_mp =
      write_Geometry_Container(ncid, "gc_mpoint", MULTIPOINT, coords2d);
  EXPECT_GE(cid_mp, 0);

  // 3. LINE
  int cid_line = write_Geometry_Container(ncid, "gc_line", LINE, coords2d);
  EXPECT_GE(cid_line, 0);

  // 4. MULTILINE
  int cid_mline =
      write_Geometry_Container(ncid, "gc_mline", MULTILINE, coords2d);
  EXPECT_GE(cid_mline, 0);

  // 5. POLYGON
  int cid_poly = write_Geometry_Container(ncid, "gc_poly", POLYGON, coords2d);
  EXPECT_GE(cid_poly, 0);

  // 6. MULTIPOLYGON
  int cid_mpoly =
      write_Geometry_Container(ncid, "gc_mpoly", MULTIPOLYGON, coords2d);
  EXPECT_GE(cid_mpoly, 0);

  ASSERT_EQ(NC_NOERR, nc_close(ncid));
  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFSGWriterUtilTest, TransactionLogAllTypes) {
  const std::string log_file =
      (std::filesystem::path(::testing::TempDir()) / "tx_log.bin").string();

  {
    WTransactionLog log(log_file);
    EXPECT_TRUE(log.logIsNull());
    log.startLog();
    EXPECT_FALSE(log.logIsNull());

    log.push(std::make_unique<OGR_SGFS_NC_Char_Transaction>(1, "char_data"));
    log.push(std::make_unique<OGR_SGFS_NC_CharA_Transaction>(2, "chara_data"));
    log.push(
        std::make_unique<OGR_SGFS_NC_String_Transaction>(3, "string_data"));
    log.push(std::make_unique<OGR_SGFS_NC_Byte_Transaction>(4, -12));
    log.push(std::make_unique<OGR_SGFS_NC_Short_Transaction>(5, 1234));
    log.push(std::make_unique<OGR_SGFS_NC_Int_Transaction>(6, 56789));
    log.push(std::make_unique<OGR_SGFS_NC_Float_Transaction>(7, 3.14f));
    log.push(std::make_unique<OGR_SGFS_NC_Double_Transaction>(8, 2.71828));
    log.push(std::make_unique<OGR_SGFS_NC_UByte_Transaction>(9, 250));
    log.push(std::make_unique<OGR_SGFS_NC_UShort_Transaction>(10, 65000));
    log.push(std::make_unique<OGR_SGFS_NC_UInt_Transaction>(11, 4000000000U));
    log.push(
        std::make_unique<OGR_SGFS_NC_Int64_Transaction>(12, -9000000000000LL));
    log.push(std::make_unique<OGR_SGFS_NC_UInt64_Transaction>(
        13, 18000000000000000000ULL));

    log.startRead();

    auto t1 = log.pop();
    ASSERT_NE(nullptr, t1);
    EXPECT_EQ(1, t1->getVarId());
    EXPECT_EQ(NC_CHAR, t1->getType());
    EXPECT_GT(t1->count(), 0);

    auto t2 = log.pop();
    ASSERT_NE(nullptr, t2);
    EXPECT_EQ(2, t2->getVarId());
    EXPECT_EQ(NC_CHAR, t2->getType());

    auto t3 = log.pop();
    ASSERT_NE(nullptr, t3);
    EXPECT_EQ(3, t3->getVarId());
    EXPECT_EQ(NC_STRING, t3->getType());

    auto t4 = log.pop();
    ASSERT_NE(nullptr, t4);
    EXPECT_EQ(4, t4->getVarId());
    EXPECT_EQ(NC_BYTE, t4->getType());

    auto t5 = log.pop();
    ASSERT_NE(nullptr, t5);
    EXPECT_EQ(5, t5->getVarId());
    EXPECT_EQ(NC_SHORT, t5->getType());

    auto t6 = log.pop();
    ASSERT_NE(nullptr, t6);
    EXPECT_EQ(6, t6->getVarId());
    EXPECT_EQ(NC_INT, t6->getType());

    auto t7 = log.pop();
    ASSERT_NE(nullptr, t7);
    EXPECT_EQ(7, t7->getVarId());
    EXPECT_EQ(NC_FLOAT, t7->getType());

    auto t8 = log.pop();
    ASSERT_NE(nullptr, t8);
    EXPECT_EQ(8, t8->getVarId());
    EXPECT_EQ(NC_DOUBLE, t8->getType());

    auto t9 = log.pop();
    ASSERT_NE(nullptr, t9);
    EXPECT_EQ(9, t9->getVarId());
    EXPECT_EQ(NC_UBYTE, t9->getType());

    auto t10 = log.pop();
    ASSERT_NE(nullptr, t10);
    EXPECT_EQ(10, t10->getVarId());
    EXPECT_EQ(NC_USHORT, t10->getType());

    auto t11 = log.pop();
    ASSERT_NE(nullptr, t11);
    EXPECT_EQ(11, t11->getVarId());
    EXPECT_EQ(NC_UINT, t11->getType());

    auto t12 = log.pop();
    ASSERT_NE(nullptr, t12);
    EXPECT_EQ(12, t12->getVarId());
    EXPECT_EQ(NC_INT64, t12->getType());

    auto t13 = log.pop();
    ASSERT_NE(nullptr, t13);
    EXPECT_EQ(13, t13->getVarId());
    EXPECT_EQ(NC_UINT64, t13->getType());

    auto t_end = log.pop();
    EXPECT_EQ(nullptr, t_end);
  }

  VSIUnlink(log_file.c_str());
}

TEST_F(NetCDFSGWriterUtilTest, ScribeAndMetadataWriteFeature) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "scribe_meta.nc").string();
  const std::string log_file =
      (std::filesystem::path(::testing::TempDir()) / "scribe_log.bin").string();

  int ncid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_create(filename.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  std::vector<std::string> coords2d = {"node_x", "node_y"};
  int cid_poly =
      write_Geometry_Container(ncid, "poly_container", POLYGON, coords2d);
  ASSERT_GE(cid_poly, 0);

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

  netCDFDataset ds;
  netCDFVID vcdf(&ds, ncid);
  vcdf.enableFullVirtualMode();

  OGR_NCScribe scribe(vcdf, log_file);
  ncLayer_SG_Metadata meta(ncid, POLYGON, vcdf, scribe);
  meta.initializeNewContainer(cid_poly);

  EXPECT_EQ(POLYGON, meta.getWritableType());
  EXPECT_EQ("poly_container", meta.get_containerName());
  EXPECT_EQ(cid_poly, meta.get_containerRealID());
  EXPECT_NE(nccfdriver::INVALID_DIM_ID, meta.get_node_coord_dimID());
  EXPECT_NE(nccfdriver::INVALID_DIM_ID, meta.get_node_count_dimID());
  EXPECT_NE(nccfdriver::INVALID_DIM_ID, meta.get_pnc_dimID());
  EXPECT_NE(nccfdriver::INVALID_VAR_ID, meta.get_pnc_varID());
  EXPECT_NE(nccfdriver::INVALID_VAR_ID, meta.get_intring_varID());
  EXPECT_EQ(2, meta.get_nodeCoordVarIDs().size());

  // Write a polygon feature
  OGRFeatureDefn* poDefn = new OGRFeatureDefn("poly_layer");
  poDefn->Reference();
  std::unique_ptr<OGRFeature> feat(new OGRFeature(poDefn));
  auto* poly = new OGRPolygon();
  auto* ext = new OGRLinearRing();
  ext->addPoint(0, 0);
  ext->addPoint(5, 0);
  ext->addPoint(5, 5);
  ext->addPoint(0, 5);
  ext->addPoint(0, 0);
  poly->addRingDirectly(ext);

  auto* in = new OGRLinearRing();
  in->addPoint(1, 1);
  in->addPoint(2, 1);
  in->addPoint(2, 2);
  in->addPoint(1, 2);
  in->addPoint(1, 1);
  poly->addRingDirectly(in);

  feat->SetGeometryDirectly(poly);
  SGeometry_Feature sgf(*feat);

  meta.writeSGeometryFeature(sgf);
  EXPECT_TRUE(meta.getInteriorRingDetected());
  EXPECT_EQ(10, meta.get_next_write_pos_node_coord());
  EXPECT_EQ(1, meta.get_next_write_pos_node_count());
  EXPECT_EQ(2, meta.get_next_write_pos_pnc());

  // Scribe commit
  scribe.commit_transaction();

  // Test Scribe logging and singleDatumMode
  scribe.enqueue_transaction(
      std::make_unique<OGR_SGFS_NC_Int_Transaction>(meta.get_pnc_varID(), 42));
  scribe.setSingleDatumMode(true);
  scribe.log_transaction();
  scribe.commit_transaction();

  feat.reset();
  poDefn->Release();

  ASSERT_EQ(NC_NOERR, nc_close(ncid));
  VSIUnlink(filename.c_str());
  VSIUnlink(log_file.c_str());
}

TEST_F(NetCDFSGWriterUtilTest, InitializeNewContainerErrors) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "init_err.nc").string();
  int ncid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_create(filename.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  // 1. Container with fewer than 2 coordinates
  std::vector<std::string> coords1d = {"only_x"};
  int cid_bad_coords =
      write_Geometry_Container(ncid, "bad_coords_cont", POINT, coords1d);
  ASSERT_GE(cid_bad_coords, 0);

  // 2. Container missing node_coordinates attribute
  int cid_missing_att = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "missing_att_cont", NC_FLOAT, 0, nullptr,
                                 &cid_missing_att));

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

  netCDFDataset ds;
  netCDFVID vcdf(&ds, ncid);
  vcdf.enableFullVirtualMode();
  std::string log_file =
      (std::filesystem::path(::testing::TempDir()) / "err_log.bin").string();
  OGR_NCScribe scribe(vcdf, log_file);
  ncLayer_SG_Metadata meta(ncid, POINT, vcdf, scribe);

  // Invalid container ID
  EXPECT_THROW(meta.initializeNewContainer(-999),
               nccfdriver::SGWriter_Exception_NCInqFailure);

  // Missing node_coordinates attribute
  EXPECT_THROW(meta.initializeNewContainer(cid_missing_att),
               nccfdriver::SGWriter_Exception_NCInqFailure);

  // Less than 2 coordinates
  EXPECT_THROW(meta.initializeNewContainer(cid_bad_coords),
               nccfdriver::SGWriter_Exception);

  ASSERT_EQ(NC_NOERR, nc_close(ncid));
  VSIUnlink(filename.c_str());
  VSIUnlink(log_file.c_str());
}

TEST_F(NetCDFSGWriterUtilTest, ScribeAndMetadataMultiPolygon3D) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "scribe_mpoly3d.nc")
          .string();
  const std::string log_file =
      (std::filesystem::path(::testing::TempDir()) / "scribe_mpoly3d.bin")
          .string();

  int ncid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_create(filename.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  std::vector<std::string> coords3d = {"node_x", "node_y", "node_z"};
  int cid_mpoly =
      write_Geometry_Container(ncid, "mpoly3d_cont", MULTIPOLYGON, coords3d);
  ASSERT_GE(cid_mpoly, 0);

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

  netCDFDataset ds;
  netCDFVID vcdf(&ds, ncid);
  vcdf.enableFullVirtualMode();

  OGR_NCScribe scribe(vcdf, log_file);
  ncLayer_SG_Metadata meta(ncid, MULTIPOLYGON, vcdf, scribe);
  meta.initializeNewContainer(cid_mpoly);

  EXPECT_EQ(MULTIPOLYGON, meta.getWritableType());
  EXPECT_EQ(3, meta.get_nodeCoordVarIDs().size());

  // Create a 3D MultiPolygon feature
  OGRFeatureDefn* poDefn = new OGRFeatureDefn("mpoly3d_layer");
  poDefn->Reference();
  std::unique_ptr<OGRFeature> feat(new OGRFeature(poDefn));

  auto* mpoly = new OGRMultiPolygon();

  // First polygon: simple triangle
  auto* poly1 = new OGRPolygon();
  auto* ring1 = new OGRLinearRing();
  ring1->addPoint(0, 0, 10);
  ring1->addPoint(1, 0, 11);
  ring1->addPoint(0, 1, 12);
  ring1->addPoint(0, 0, 10);
  poly1->addRingDirectly(ring1);
  mpoly->addGeometryDirectly(poly1);

  // Second polygon: exterior and interior ring
  auto* poly2 = new OGRPolygon();
  auto* ext2 = new OGRLinearRing();
  ext2->addPoint(10, 10, 20);
  ext2->addPoint(20, 10, 21);
  ext2->addPoint(20, 20, 22);
  ext2->addPoint(10, 20, 23);
  ext2->addPoint(10, 10, 20);
  poly2->addRingDirectly(ext2);

  auto* in2 = new OGRLinearRing();
  in2->addPoint(12, 12, 25);
  in2->addPoint(14, 12, 26);
  in2->addPoint(14, 14, 27);
  in2->addPoint(12, 14, 28);
  in2->addPoint(12, 12, 25);
  poly2->addRingDirectly(in2);
  mpoly->addGeometryDirectly(poly2);

  feat->SetGeometryDirectly(mpoly);
  SGeometry_Feature sgf(*feat);

  meta.writeSGeometryFeature(sgf);
  EXPECT_TRUE(meta.getInteriorRingDetected());
  // 4 + 5 + 5 nodes, 3 rings, 1 feature.
  EXPECT_EQ(14, meta.get_next_write_pos_node_coord());
  EXPECT_EQ(3, meta.get_next_write_pos_pnc());
  EXPECT_EQ(1, meta.get_next_write_pos_node_count());

  // An empty MultiPolygon has no nodes, so the node coordinate write position
  // must not advance. It still emits one node_count entry (0) and, because
  // multipolygon part counts are per ring, one explicit part_node_count of 0.
  const size_t node_pos = meta.get_next_write_pos_node_coord();
  const size_t pnc_pos = meta.get_next_write_pos_pnc();
  const size_t node_count_pos = meta.get_next_write_pos_node_count();
  std::unique_ptr<OGRFeature> featEmpty(new OGRFeature(poDefn));
  featEmpty->SetGeometryDirectly(new OGRMultiPolygon());
  SGeometry_Feature sgfEmpty(*featEmpty);
  meta.writeSGeometryFeature(sgfEmpty);
  EXPECT_EQ(node_pos, meta.get_next_write_pos_node_coord());
  EXPECT_EQ(pnc_pos + 1, meta.get_next_write_pos_pnc());
  EXPECT_EQ(node_count_pos + 1, meta.get_next_write_pos_node_count());

  // Enqueueing a null transaction is a no-op: nothing is buffered.
  const unsigned long long usage_before = scribe.getMemBuffer().getUsage();
  scribe.enqueue_transaction(nullptr);
  EXPECT_EQ(usage_before, scribe.getMemBuffer().getUsage());

  scribe.commit_transaction();

  feat.reset();
  featEmpty.reset();
  poDefn->Release();

  ASSERT_EQ(NC_NOERR, nc_close(ncid));
  VSIUnlink(filename.c_str());
  VSIUnlink(log_file.c_str());
}

// Reads back the single value at index 0 of each scribe-written variable and
// checks it against the expected value. Variable ids are the *real* netCDF ids.
struct ScribeVarIds {
  int v_byte, v_short, v_int, v_float, v_double, v_uint, v_uint64, v_int64,
      v_ubyte, v_ushort, v_char, v_string;
};

void ExpectScribeValues(int ncid, const ScribeVarIds& ids, int base) {
  const size_t index[1] = {0};
  signed char b = 0;
  ASSERT_EQ(NC_NOERR, nc_get_var1_schar(ncid, ids.v_byte, index, &b));
  EXPECT_EQ(base + 1, b);
  short s = 0;
  ASSERT_EQ(NC_NOERR, nc_get_var1_short(ncid, ids.v_short, index, &s));
  EXPECT_EQ(base + 2, s);
  int i = 0;
  ASSERT_EQ(NC_NOERR, nc_get_var1_int(ncid, ids.v_int, index, &i));
  EXPECT_EQ(base + 3, i);
  float f = 0;
  ASSERT_EQ(NC_NOERR, nc_get_var1_float(ncid, ids.v_float, index, &f));
  EXPECT_FLOAT_EQ(base + 4.0f, f);
  double d = 0;
  ASSERT_EQ(NC_NOERR, nc_get_var1_double(ncid, ids.v_double, index, &d));
  EXPECT_DOUBLE_EQ(base + 5.0, d);
  unsigned int ui = 0;
  ASSERT_EQ(NC_NOERR, nc_get_var1_uint(ncid, ids.v_uint, index, &ui));
  EXPECT_EQ(base + 6, ui);
  unsigned long long ull = 0;
  ASSERT_EQ(NC_NOERR, nc_get_var1_ulonglong(ncid, ids.v_uint64, index, &ull));
  EXPECT_EQ(base + 7, ull);
  long long ll = 0;
  ASSERT_EQ(NC_NOERR, nc_get_var1_longlong(ncid, ids.v_int64, index, &ll));
  EXPECT_EQ(base + 8, ll);
  unsigned char ub = 0;
  ASSERT_EQ(NC_NOERR, nc_get_var1_uchar(ncid, ids.v_ubyte, index, &ub));
  EXPECT_EQ(base + 9, ub);
  unsigned short us = 0;
  ASSERT_EQ(NC_NOERR, nc_get_var1_ushort(ncid, ids.v_ushort, index, &us));
  EXPECT_EQ(base + 10, us);
  char c = 0;
  ASSERT_EQ(NC_NOERR, nc_get_var1_text(ncid, ids.v_char, index, &c));
  EXPECT_EQ(base == 0 ? 'x' : 'y', c);
  char* str = nullptr;
  ASSERT_EQ(NC_NOERR, nc_get_var1_string(ncid, ids.v_string, index, &str));
  ASSERT_NE(nullptr, str);
  EXPECT_STREQ(base == 0 ? "hello" : "world", str);
  ASSERT_EQ(NC_NOERR, nc_free_string(1, &str));
}

void EnqueueAllTypes(OGR_NCScribe& scribe, const ScribeVarIds& ids, int base,
                     const char* char_value, const char* string_value) {
  scribe.enqueue_transaction(std::make_unique<OGR_SGFS_NC_Byte_Transaction>(
      ids.v_byte, static_cast<signed char>(base + 1)));
  scribe.enqueue_transaction(std::make_unique<OGR_SGFS_NC_Short_Transaction>(
      ids.v_short, static_cast<short>(base + 2)));
  scribe.enqueue_transaction(
      std::make_unique<OGR_SGFS_NC_Int_Transaction>(ids.v_int, base + 3));
  scribe.enqueue_transaction(std::make_unique<OGR_SGFS_NC_Float_Transaction>(
      ids.v_float, base + 4.0f));
  scribe.enqueue_transaction(std::make_unique<OGR_SGFS_NC_Double_Transaction>(
      ids.v_double, base + 5.0));
  scribe.enqueue_transaction(
      std::make_unique<OGR_SGFS_NC_UInt_Transaction>(ids.v_uint, base + 6));
  scribe.enqueue_transaction(
      std::make_unique<OGR_SGFS_NC_UInt64_Transaction>(ids.v_uint64, base + 7));
  scribe.enqueue_transaction(
      std::make_unique<OGR_SGFS_NC_Int64_Transaction>(ids.v_int64, base + 8));
  scribe.enqueue_transaction(std::make_unique<OGR_SGFS_NC_UByte_Transaction>(
      ids.v_ubyte, static_cast<unsigned char>(base + 9)));
  scribe.enqueue_transaction(std::make_unique<OGR_SGFS_NC_UShort_Transaction>(
      ids.v_ushort, static_cast<unsigned short>(base + 10)));
  scribe.enqueue_transaction(
      std::make_unique<OGR_SGFS_NC_Char_Transaction>(ids.v_char, char_value));
  scribe.enqueue_transaction(std::make_unique<OGR_SGFS_NC_String_Transaction>(
      ids.v_string, string_value));
}

TEST_F(NetCDFSGWriterUtilTest, ScribeCommitAllTypes) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "scribe_types.nc")
          .string();
  const std::string log_file =
      (std::filesystem::path(::testing::TempDir()) / "scribe_types.bin")
          .string();

  int ncid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_create(filename.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  netCDFDataset ds;
  netCDFVID vcdf(&ds, ncid);
  vcdf.enableFullVirtualMode();

  // Virtual ids: these are what the scribe transactions target.
  int dimid = vcdf.nc_def_vdim("vdim", 100);
  const ScribeVarIds vids = {
      vcdf.nc_def_vvar("v_byte", NC_BYTE, 1, &dimid),
      vcdf.nc_def_vvar("v_short", NC_SHORT, 1, &dimid),
      vcdf.nc_def_vvar("v_int", NC_INT, 1, &dimid),
      vcdf.nc_def_vvar("v_float", NC_FLOAT, 1, &dimid),
      vcdf.nc_def_vvar("v_double", NC_DOUBLE, 1, &dimid),
      vcdf.nc_def_vvar("v_uint", NC_UINT, 1, &dimid),
      vcdf.nc_def_vvar("v_uint64", NC_UINT64, 1, &dimid),
      vcdf.nc_def_vvar("v_int64", NC_INT64, 1, &dimid),
      vcdf.nc_def_vvar("v_ubyte", NC_UBYTE, 1, &dimid),
      vcdf.nc_def_vvar("v_ushort", NC_USHORT, 1, &dimid),
      vcdf.nc_def_vvar("v_char", NC_CHAR, 1, &dimid),
      vcdf.nc_def_vvar("v_string", NC_STRING, 1, &dimid),
  };

  // Materialize the virtual dimension/variables in the file. Without this
  // every virtual variable keeps INVALID_VAR_ID and the scribe silently drops
  // all writes.
  vcdf.nc_vmap();
  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

  // Real ids, used to read the committed values back.
  const ScribeVarIds rids = {
      vcdf.virtualVIDToVar(vids.v_byte).getRealID(),
      vcdf.virtualVIDToVar(vids.v_short).getRealID(),
      vcdf.virtualVIDToVar(vids.v_int).getRealID(),
      vcdf.virtualVIDToVar(vids.v_float).getRealID(),
      vcdf.virtualVIDToVar(vids.v_double).getRealID(),
      vcdf.virtualVIDToVar(vids.v_uint).getRealID(),
      vcdf.virtualVIDToVar(vids.v_uint64).getRealID(),
      vcdf.virtualVIDToVar(vids.v_int64).getRealID(),
      vcdf.virtualVIDToVar(vids.v_ubyte).getRealID(),
      vcdf.virtualVIDToVar(vids.v_ushort).getRealID(),
      vcdf.virtualVIDToVar(vids.v_char).getRealID(),
      vcdf.virtualVIDToVar(vids.v_string).getRealID(),
  };
  ASSERT_NE(nccfdriver::INVALID_VAR_ID, rids.v_byte);
  ASSERT_NE(nccfdriver::INVALID_VAR_ID, rids.v_string);

  // 1. Commit directly from the in-memory queue (batched array writes for the
  //    fixed-size types, single datum writes for NC_CHAR / NC_STRING).
  {
    OGR_NCScribe scribe(vcdf, log_file);
    EnqueueAllTypes(scribe, vids, /*base=*/0, "x", "hello");
    EXPECT_EQ(1, scribe.getWriteCount(vids.v_int));
    scribe.commit_transaction();
  }
  ExpectScribeValues(ncid, rids, /*base=*/0);

  // 2. Spill the queue to the on-disk transaction log, then replay it in
  //    singleDatumMode. A fresh scribe restarts at index 0, so this overwrites
  //    the values from step 1.
  {
    OGR_NCScribe scribe(vcdf, log_file);
    EnqueueAllTypes(scribe, vids, /*base=*/10, "y", "world");
    scribe.setSingleDatumMode(true);
    scribe.log_transaction();
    EXPECT_EQ(0, scribe.getMemBuffer().getUsage());
    scribe.commit_transaction();
  }
  ExpectScribeValues(ncid, rids, /*base=*/10);

  ASSERT_EQ(NC_NOERR, nc_close(ncid));
  VSIUnlink(filename.c_str());
  VSIUnlink(log_file.c_str());
}

}  // namespace
}  // namespace autotest2
