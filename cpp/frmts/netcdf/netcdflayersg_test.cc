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

#include "third_party/gdal/frmts/netcdf/netcdflayersg.h"

#include <filesystem>  // NOLINT
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "netcdf.h"
#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdal_utils.h"
#include "third_party/gdal/frmts/netcdf/netcdfdataset.h"
#include "third_party/gdal/frmts/netcdf/netcdfsg.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::nccfdriver::LINE;
using ::nccfdriver::MULTILINE;
using ::nccfdriver::MULTIPOINT;
using ::nccfdriver::MULTIPOLYGON;
using ::nccfdriver::NONE;
using ::nccfdriver::OGRHasZandSupported;
using ::nccfdriver::OGRtoRaw;
using ::nccfdriver::POINT;
using ::nccfdriver::POLYGON;
using ::nccfdriver::RawToOGR;
using ::nccfdriver::UNSUPPORTED;
using ::testing::Contains;
using ::testing::HasSubstr;
using ::testing::Not;
using ::testing::Pair;

// CPLErrorHandler that appends every reported error to the vector passed as
// user data to CPLPushErrorHandlerEx().
void CollectErrors(CPLErr err_class, CPLErrorNum /*err_no*/, const char* msg) {
  auto* errors = static_cast<std::vector<std::pair<CPLErr, std::string>>*>(
      CPLGetErrorHandlerUserData());
  errors->emplace_back(err_class, msg);
}

class NetCDFLayerSGTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALAllRegister(); }
};

TEST_F(NetCDFLayerSGTest, RawToOGR) {
  EXPECT_EQ(wkbNone, RawToOGR(NONE, 2));
  EXPECT_EQ(wkbNone, RawToOGR(UNSUPPORTED, 2));

  // LINE
  EXPECT_EQ(wkbLineString, RawToOGR(LINE, 2));
  EXPECT_EQ(wkbLineString25D, RawToOGR(LINE, 3));
  EXPECT_EQ(wkbNone, RawToOGR(LINE, 1));

  // MULTILINE
  EXPECT_EQ(wkbMultiLineString, RawToOGR(MULTILINE, 2));
  EXPECT_EQ(wkbMultiLineString25D, RawToOGR(MULTILINE, 3));
  EXPECT_EQ(wkbNone, RawToOGR(MULTILINE, 4));

  // POLYGON
  EXPECT_EQ(wkbPolygon, RawToOGR(POLYGON, 2));
  EXPECT_EQ(wkbPolygon25D, RawToOGR(POLYGON, 3));
  EXPECT_EQ(wkbNone, RawToOGR(POLYGON, 1));

  // MULTIPOLYGON
  EXPECT_EQ(wkbMultiPolygon, RawToOGR(MULTIPOLYGON, 2));
  EXPECT_EQ(wkbMultiPolygon25D, RawToOGR(MULTIPOLYGON, 3));
  EXPECT_EQ(wkbNone, RawToOGR(MULTIPOLYGON, 4));

  // POINT
  EXPECT_EQ(wkbPoint, RawToOGR(POINT, 2));
  EXPECT_EQ(wkbPoint25D, RawToOGR(POINT, 3));
  EXPECT_EQ(wkbNone, RawToOGR(POINT, 1));

  // MULTIPOINT
  EXPECT_EQ(wkbMultiPoint, RawToOGR(MULTIPOINT, 2));
  EXPECT_EQ(wkbMultiPoint25D, RawToOGR(MULTIPOINT, 3));
  EXPECT_EQ(wkbNone, RawToOGR(MULTIPOINT, 1));
}

TEST_F(NetCDFLayerSGTest, OGRtoRaw) {
  EXPECT_EQ(POINT, OGRtoRaw(wkbPoint));
  EXPECT_EQ(POINT, OGRtoRaw(wkbPoint25D));
  EXPECT_EQ(LINE, OGRtoRaw(wkbLineString));
  EXPECT_EQ(LINE, OGRtoRaw(wkbLineString25D));
  EXPECT_EQ(POLYGON, OGRtoRaw(wkbPolygon));
  EXPECT_EQ(POLYGON, OGRtoRaw(wkbPolygon25D));
  EXPECT_EQ(MULTIPOINT, OGRtoRaw(wkbMultiPoint));
  EXPECT_EQ(MULTIPOINT, OGRtoRaw(wkbMultiPoint25D));
  EXPECT_EQ(MULTILINE, OGRtoRaw(wkbMultiLineString));
  EXPECT_EQ(MULTILINE, OGRtoRaw(wkbMultiLineString25D));
  EXPECT_EQ(MULTIPOLYGON, OGRtoRaw(wkbMultiPolygon));
  EXPECT_EQ(MULTIPOLYGON, OGRtoRaw(wkbMultiPolygon25D));
  EXPECT_EQ(NONE, OGRtoRaw(wkbNone));

  // Measured geometry warning
  CPLErrorReset();
  EXPECT_EQ(POINT, OGRtoRaw(wkbPointM));
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
}

TEST_F(NetCDFLayerSGTest, OGRHasZandSupported) {
  EXPECT_TRUE(OGRHasZandSupported(wkbPoint25D));
  EXPECT_TRUE(OGRHasZandSupported(wkbLineString25D));
  EXPECT_TRUE(OGRHasZandSupported(wkbPolygon25D));
  EXPECT_TRUE(OGRHasZandSupported(wkbMultiPoint25D));
  EXPECT_TRUE(OGRHasZandSupported(wkbMultiLineString25D));
  EXPECT_TRUE(OGRHasZandSupported(wkbMultiPolygon25D));

  EXPECT_FALSE(OGRHasZandSupported(wkbPoint));
  EXPECT_FALSE(OGRHasZandSupported(wkbLineString));
  EXPECT_FALSE(OGRHasZandSupported(wkbPolygon));
  EXPECT_FALSE(OGRHasZandSupported(wkbMultiPoint));
  EXPECT_FALSE(OGRHasZandSupported(wkbMultiLineString));
  EXPECT_FALSE(OGRHasZandSupported(wkbMultiPolygon));
  EXPECT_FALSE(OGRHasZandSupported(wkbNone));
}

TEST_F(NetCDFLayerSGTest, GenerateLogNameAndLogPendingTransaction) {
  EXPECT_FALSE(netCDFDataset::generateLogName().empty());

  netCDFDataset ds;
  ds.SGLogPendingTransaction();
  EXPECT_TRUE(ds.SGCommitPendingTransaction());
}

TEST_F(NetCDFLayerSGTest, VectorTranslatePointLayer) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "sg_translate_pt.nc")
          .string();

  // Create in-memory dataset with points
  GDALDriver* poMemDriver = GetGDALDriverManager()->GetDriverByName("Memory");
  ASSERT_NE(nullptr, poMemDriver);
  std::unique_ptr<GDALDataset> poMemDS(
      poMemDriver->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, poMemDS);

  OGRLayer* poLayer =
      poMemDS->CreateLayer("points", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, poLayer);
  OGRFieldDefn field_defn("name", OFTString);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&field_defn));

  OGRFeature* feat = new OGRFeature(poLayer->GetLayerDefn());
  feat->SetField("name", "point1");
  feat->SetGeometryDirectly(new OGRPoint(10.0, 20.0));
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(feat));
  OGRFeature::DestroyFeature(feat);

  // Vector translate to netCDF
  char* argv[] = {
      const_cast<char*>("-f"),
      const_cast<char*>("netCDF"),
      const_cast<char*>("-dsco"),
      const_cast<char*>("FORMAT=NC4"),
      nullptr,
  };
  auto* options = GDALVectorTranslateOptionsNew(argv, nullptr);
  GDALDatasetH hSrc = poMemDS.get();
  GDALDatasetH hDst = GDALVectorTranslate(filename.c_str(), nullptr, 1, &hSrc,
                                          options, nullptr);
  GDALVectorTranslateOptionsFree(options);
  ASSERT_NE(nullptr, hDst);
  GDALClose(hDst);

  // Reopen and inspect with netCDF driver
  {
    std::unique_ptr<GDALDataset> poDS(static_cast<GDALDataset*>(GDALOpenEx(
        filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS) << CPLGetLastErrorMsg();
    EXPECT_EQ(1, poDS->GetLayerCount());
    OGRLayer* l = poDS->GetLayer(0);
    ASSERT_NE(nullptr, l);
    std::unique_ptr<OGRFeature> f(l->GetNextFeature());
    ASSERT_NE(nullptr, f);
    EXPECT_STREQ("point1", f->GetFieldAsString("name"));
    OGRGeometry* geom = f->GetGeometryRef();
    ASSERT_NE(nullptr, geom);
    EXPECT_EQ(wkbPoint, wkbFlatten(geom->getGeometryType()));
  }

  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFLayerSGTest, VectorTranslatePolygonLayer) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "sg_translate_poly.nc")
          .string();

  GDALDriver* poMemDriver = GetGDALDriverManager()->GetDriverByName("Memory");
  ASSERT_NE(nullptr, poMemDriver);
  std::unique_ptr<GDALDataset> poMemDS(
      poMemDriver->Create("", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, poMemDS);

  OGRLayer* poLayer =
      poMemDS->CreateLayer("polys", nullptr, wkbPolygon, nullptr);
  ASSERT_NE(nullptr, poLayer);
  OGRFieldDefn field_defn("id", OFTInteger);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&field_defn));

  OGRFeature* feat = new OGRFeature(poLayer->GetLayerDefn());
  feat->SetField("id", 1);
  auto* poly = new OGRPolygon();
  auto* ring = new OGRLinearRing();
  ring->addPoint(0.0, 0.0);
  ring->addPoint(1.0, 0.0);
  ring->addPoint(1.0, 1.0);
  ring->addPoint(0.0, 1.0);
  ring->addPoint(0.0, 0.0);
  poly->addRingDirectly(ring);
  feat->SetGeometryDirectly(poly);
  EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(feat));
  OGRFeature::DestroyFeature(feat);

  char* argv[] = {
      const_cast<char*>("-f"),
      const_cast<char*>("netCDF"),
      const_cast<char*>("-dsco"),
      const_cast<char*>("FORMAT=NC4"),
      nullptr,
  };
  auto* options = GDALVectorTranslateOptionsNew(argv, nullptr);
  GDALDatasetH hSrc = poMemDS.get();
  GDALDatasetH hDst = GDALVectorTranslate(filename.c_str(), nullptr, 1, &hSrc,
                                          options, nullptr);
  GDALVectorTranslateOptionsFree(options);
  ASSERT_NE(nullptr, hDst);
  GDALClose(hDst);

  {
    std::unique_ptr<GDALDataset> poDS(static_cast<GDALDataset*>(GDALOpenEx(
        filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS) << CPLGetLastErrorMsg();
    EXPECT_EQ(1, poDS->GetLayerCount());
    OGRLayer* l = poDS->GetLayer(0);
    ASSERT_NE(nullptr, l);
    std::unique_ptr<OGRFeature> f(l->GetNextFeature());
    ASSERT_NE(nullptr, f);
    EXPECT_EQ(1, f->GetFieldAsInteger("id"));
    OGRGeometry* geom = f->GetGeometryRef();
    ASSERT_NE(nullptr, geom);
    EXPECT_EQ(wkbPolygon, wkbFlatten(geom->getGeometryType()));
  }

  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFLayerSGTest, VectorTranslateMultiPolygonAndMultiLineLayer) {
  // Test MultiLineString
  {
    const std::string filename =
        (std::filesystem::path(::testing::TempDir()) / "sg_translate_mls.nc")
            .string();

    GDALDriver* poMemDriver = GetGDALDriverManager()->GetDriverByName("Memory");
    std::unique_ptr<GDALDataset> poMemDS(
        poMemDriver->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* poLayer =
        poMemDS->CreateLayer("lines", nullptr, wkbMultiLineString, nullptr);
    OGRFieldDefn field_defn("name", OFTString);
    EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&field_defn));

    OGRFeature* feat = new OGRFeature(poLayer->GetLayerDefn());
    feat->SetField("name", "mls1");
    auto* mls = new OGRMultiLineString();
    auto* ls = new OGRLineString();
    ls->addPoint(0.0, 0.0);
    ls->addPoint(1.0, 1.0);
    mls->addGeometryDirectly(ls);
    feat->SetGeometryDirectly(mls);
    EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);

    char* argv[] = {
        const_cast<char*>("-f"),
        const_cast<char*>("netCDF"),
        const_cast<char*>("-dsco"),
        const_cast<char*>("FORMAT=NC4"),
        nullptr,
    };
    auto* options = GDALVectorTranslateOptionsNew(argv, nullptr);
    GDALDatasetH hSrc = poMemDS.get();
    GDALDatasetH hDst = GDALVectorTranslate(filename.c_str(), nullptr, 1, &hSrc,
                                            options, nullptr);
    GDALVectorTranslateOptionsFree(options);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);

    std::unique_ptr<GDALDataset> poDS(static_cast<GDALDataset*>(GDALOpenEx(
        filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS) << CPLGetLastErrorMsg();
    OGRLayer* l = poDS->GetLayer(0);
    ASSERT_NE(nullptr, l);
    std::unique_ptr<OGRFeature> f(l->GetNextFeature());
    ASSERT_NE(nullptr, f);
    ASSERT_NE(nullptr, f->GetGeometryRef());
    EXPECT_EQ(wkbMultiLineString,
              wkbFlatten(f->GetGeometryRef()->getGeometryType()));

    VSIUnlink(filename.c_str());
  }

  // Test MultiPolygon with interior ring
  {
    const std::string filename =
        (std::filesystem::path(::testing::TempDir()) / "sg_translate_mpoly.nc")
            .string();

    GDALDriver* poMemDriver = GetGDALDriverManager()->GetDriverByName("Memory");
    std::unique_ptr<GDALDataset> poMemDS(
        poMemDriver->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* poLayer =
        poMemDS->CreateLayer("mpolys", nullptr, wkbMultiPolygon, nullptr);
    OGRFieldDefn field_defn("name", OFTString);
    EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&field_defn));

    OGRFeature* feat = new OGRFeature(poLayer->GetLayerDefn());
    feat->SetField("name", "mpoly1");
    auto* mpoly = new OGRMultiPolygon();
    auto* poly = new OGRPolygon();
    auto* ext_ring = new OGRLinearRing();
    ext_ring->addPoint(0.0, 0.0);
    ext_ring->addPoint(10.0, 0.0);
    ext_ring->addPoint(10.0, 10.0);
    ext_ring->addPoint(0.0, 10.0);
    ext_ring->addPoint(0.0, 0.0);
    poly->addRingDirectly(ext_ring);
    auto* int_ring = new OGRLinearRing();
    int_ring->addPoint(2.0, 2.0);
    int_ring->addPoint(8.0, 2.0);
    int_ring->addPoint(8.0, 8.0);
    int_ring->addPoint(2.0, 8.0);
    int_ring->addPoint(2.0, 2.0);
    poly->addRingDirectly(int_ring);
    mpoly->addGeometryDirectly(poly);
    feat->SetGeometryDirectly(mpoly);
    EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);

    char* argv[] = {
        const_cast<char*>("-f"),
        const_cast<char*>("netCDF"),
        const_cast<char*>("-dsco"),
        const_cast<char*>("FORMAT=NC4"),
        nullptr,
    };
    auto* options = GDALVectorTranslateOptionsNew(argv, nullptr);
    GDALDatasetH hSrc = poMemDS.get();
    GDALDatasetH hDst = GDALVectorTranslate(filename.c_str(), nullptr, 1, &hSrc,
                                            options, nullptr);
    GDALVectorTranslateOptionsFree(options);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);

    std::unique_ptr<GDALDataset> poDS(static_cast<GDALDataset*>(GDALOpenEx(
        filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS) << CPLGetLastErrorMsg();
    OGRLayer* l = poDS->GetLayer(0);
    ASSERT_NE(nullptr, l);
    std::unique_ptr<OGRFeature> f(l->GetNextFeature());
    ASSERT_NE(nullptr, f);
    ASSERT_NE(nullptr, f->GetGeometryRef());
    EXPECT_EQ(wkbMultiPolygon,
              wkbFlatten(f->GetGeometryRef()->getGeometryType()));

    VSIUnlink(filename.c_str());
  }
}

TEST_F(NetCDFLayerSGTest, VectorTranslateLineStringAndMultiPointWithSRS) {
  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  // 1. LineString with SRS
  {
    const std::string filename =
        (std::filesystem::path(::testing::TempDir()) / "sg_translate_ls_srs.nc")
            .string();

    GDALDriver* poMemDriver = GetGDALDriverManager()->GetDriverByName("Memory");
    std::unique_ptr<GDALDataset> poMemDS(
        poMemDriver->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* poLayer =
        poMemDS->CreateLayer("linestring_layer", &srs, wkbLineString, nullptr);
    OGRFieldDefn field_defn("id", OFTInteger);
    EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&field_defn));

    OGRFeature* feat = new OGRFeature(poLayer->GetLayerDefn());
    feat->SetField("id", 42);
    auto* ls = new OGRLineString();
    ls->addPoint(10.0, 20.0);
    ls->addPoint(30.0, 40.0);
    feat->SetGeometryDirectly(ls);
    EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);

    char* argv[] = {
        const_cast<char*>("-f"),
        const_cast<char*>("netCDF"),
        const_cast<char*>("-dsco"),
        const_cast<char*>("FORMAT=NC4"),
        nullptr,
    };
    auto* options = GDALVectorTranslateOptionsNew(argv, nullptr);
    GDALDatasetH hSrc = poMemDS.get();
    GDALDatasetH hDst = GDALVectorTranslate(filename.c_str(), nullptr, 1, &hSrc,
                                            options, nullptr);
    GDALVectorTranslateOptionsFree(options);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);

    std::unique_ptr<GDALDataset> poDS(static_cast<GDALDataset*>(GDALOpenEx(
        filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    OGRLayer* l = poDS->GetLayer(0);
    ASSERT_NE(nullptr, l);
    const OGRSpatialReference* srs_out = l->GetSpatialRef();
    ASSERT_NE(nullptr, srs_out);
    EXPECT_TRUE(srs_out->IsGeographic());
    EXPECT_STREQ("4326", srs_out->GetAuthorityCode(nullptr));
    std::unique_ptr<OGRFeature> f(l->GetNextFeature());
    ASSERT_NE(nullptr, f);
    EXPECT_EQ(42, f->GetFieldAsInteger("id"));
    ASSERT_NE(nullptr, f->GetGeometryRef());
    EXPECT_EQ(wkbLineString,
              wkbFlatten(f->GetGeometryRef()->getGeometryType()));

    VSIUnlink(filename.c_str());
  }

  // 2. MultiPoint with SRS
  {
    const std::string filename =
        (std::filesystem::path(::testing::TempDir()) / "sg_translate_mp_srs.nc")
            .string();

    GDALDriver* poMemDriver = GetGDALDriverManager()->GetDriverByName("Memory");
    std::unique_ptr<GDALDataset> poMemDS(
        poMemDriver->Create("", 0, 0, 0, GDT_Unknown, nullptr));
    OGRLayer* poLayer =
        poMemDS->CreateLayer("mp_layer", &srs, wkbMultiPoint, nullptr);
    OGRFieldDefn field_defn("id", OFTInteger);
    EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&field_defn));

    OGRFeature* feat = new OGRFeature(poLayer->GetLayerDefn());
    feat->SetField("id", 100);
    auto* mp = new OGRMultiPoint();
    auto* p1 = new OGRPoint(1.0, 2.0);
    auto* p2 = new OGRPoint(3.0, 4.0);
    mp->addGeometryDirectly(p1);
    mp->addGeometryDirectly(p2);
    feat->SetGeometryDirectly(mp);
    EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(feat));
    OGRFeature::DestroyFeature(feat);

    char* argv[] = {
        const_cast<char*>("-f"),
        const_cast<char*>("netCDF"),
        const_cast<char*>("-dsco"),
        const_cast<char*>("FORMAT=NC4"),
        nullptr,
    };
    auto* options = GDALVectorTranslateOptionsNew(argv, nullptr);
    GDALDatasetH hSrc = poMemDS.get();
    GDALDatasetH hDst = GDALVectorTranslate(filename.c_str(), nullptr, 1, &hSrc,
                                            options, nullptr);
    GDALVectorTranslateOptionsFree(options);
    ASSERT_NE(nullptr, hDst);
    GDALClose(hDst);

    std::unique_ptr<GDALDataset> poDS(static_cast<GDALDataset*>(GDALOpenEx(
        filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    OGRLayer* l = poDS->GetLayer(0);
    ASSERT_NE(nullptr, l);
    const OGRSpatialReference* srs_out = l->GetSpatialRef();
    ASSERT_NE(nullptr, srs_out);
    EXPECT_STREQ("4326", srs_out->GetAuthorityCode(nullptr));
    std::unique_ptr<OGRFeature> f(l->GetNextFeature());
    ASSERT_NE(nullptr, f);
    EXPECT_EQ(100, f->GetFieldAsInteger("id"));
    ASSERT_NE(nullptr, f->GetGeometryRef());
    EXPECT_EQ(wkbMultiPoint,
              wkbFlatten(f->GetGeometryRef()->getGeometryType()));

    VSIUnlink(filename.c_str());
  }
}

TEST_F(NetCDFLayerSGTest, ScanForGeometryContainersException) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "sg_invalid_container.nc")
          .string();

  int ncid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_create(filename.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, NC_GLOBAL, "Conventions", 6, "CF-1.8"));

  // Create a data variable with a "geometry" attribute pointing to a container
  // variable
  int data_varid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "data_var", NC_INT, 0, nullptr, &data_varid));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, data_varid, "geometry", 14,
                                      "geom_container"));

  // Create the container variable, but without the mandatory "geometry_type"
  // attribute
  int geom_varid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "geom_container", NC_CHAR, 0, nullptr,
                                 &geom_varid));

  ASSERT_EQ(NC_NOERR, nc_close(ncid));

  // When opening, scanForGeometryContainers finds geom_container, and
  // LoadSGVarIntoLayer throws SG_Exception_Existential from the
  // SGeometry_Reader constructor. DetectAndFillSGLayers catches it and logs
  // a warning. Because no layer could be built and the file has no raster
  // variables, the read-only vector open then yields no dataset.
  std::vector<std::pair<CPLErr, std::string>> errors;
  CPLErrorReset();
  CPLPushErrorHandlerEx(CollectErrors, &errors);
  std::unique_ptr<GDALDataset> poDS(static_cast<GDALDataset*>(
      GDALOpenEx(filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
  CPLPopErrorHandler();

  EXPECT_EQ(nullptr, poDS);
  EXPECT_THAT(errors,
              Contains(Pair(CE_Warning,
                            HasSubstr("Translation of a simple geometry layer "
                                      "has been terminated prematurely"))));
  // The exception was handled: nothing escalated to a failure.
  EXPECT_THAT(errors, Not(Contains(Pair(CE_Failure, ::testing::_))));

  VSIUnlink(filename.c_str());
}

}  // namespace
}  // namespace autotest2
