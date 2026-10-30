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

#include <cstring>
#include <filesystem>  // NOLINT
#include <memory>
#include <string>
#include <vector>

#include "netcdf.h"
#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

class NetCDFSentinel3Test : public ::testing::Test {
 protected:
  void SetUp() override { GDALAllRegister(); }
};

TEST_F(NetCDFSentinel3Test, StandardSRALMWRProduct) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "s3_sral_mwr.nc").string();

  int ncid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_create(filename.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  // Global attributes identifying Sentinel 3 SRAL/MWR product
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, NC_GLOBAL, "mission_name",
                                      strlen("Sentinel 3"), "Sentinel 3"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, NC_GLOBAL, "altimeter_sensor_name",
                                      strlen("SRAL"), "SRAL"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, NC_GLOBAL, "radiometer_sensor_name",
                                      strlen("MWR"), "MWR"));
  // Ellipsoid parameters matching EPSG:4326 WGS 84
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, NC_GLOBAL, "semi_major_ellipsoid_axis",
                            strlen("6378137"), "6378137"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, NC_GLOBAL, "ellipsoid_flattening",
                                      strlen("0.00335281066474748"),
                                      "0.00335281066474748"));

  // Dimensions
  int dim_time = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "time", 3, &dim_time));
  int dim_other = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "other_dim", 2, &dim_other));

  // 1. Longitude (NC_INT)
  int var_lon = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "lon", NC_INT, 1, &dim_time, &var_lon));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, var_lon, "standard_name",
                                      strlen("longitude"), "longitude"));
  double scale_deg = 1e-6;
  double offset_deg = 0.0;
  ASSERT_EQ(NC_NOERR, nc_put_att_double(ncid, var_lon, "scale_factor",
                                        NC_DOUBLE, 1, &scale_deg));
  ASSERT_EQ(NC_NOERR, nc_put_att_double(ncid, var_lon, "add_offset", NC_DOUBLE,
                                        1, &offset_deg));

  // 2. Latitude (NC_INT)
  int var_lat = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "lat", NC_INT, 1, &dim_time, &var_lat));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, var_lat, "standard_name",
                                      strlen("latitude"), "latitude"));
  ASSERT_EQ(NC_NOERR, nc_put_att_double(ncid, var_lat, "scale_factor",
                                        NC_DOUBLE, 1, &scale_deg));
  ASSERT_EQ(NC_NOERR, nc_put_att_double(ncid, var_lat, "add_offset", NC_DOUBLE,
                                        1, &offset_deg));

  // 3. NC_BYTE variable
  int var_byte = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "var_byte", NC_BYTE, 1, &dim_time, &var_byte));
  double scale_byte = 0.1;
  double offset_byte = 1.0;
  signed char fill_byte = -128;
  ASSERT_EQ(NC_NOERR, nc_put_att_double(ncid, var_byte, "scale_factor",
                                        NC_DOUBLE, 1, &scale_byte));
  ASSERT_EQ(NC_NOERR, nc_put_att_double(ncid, var_byte, "add_offset", NC_DOUBLE,
                                        1, &offset_byte));
  ASSERT_EQ(NC_NOERR, nc_put_att_schar(ncid, var_byte, "_FillValue", NC_BYTE, 1,
                                       &fill_byte));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, var_byte, "comment",
                                      strlen("byte test"), "byte test"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, var_byte, "coordinates",
                                      strlen("lon lat"), "lon lat"));

  // 4. NC_SHORT variable
  int var_short = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "var_short", NC_SHORT, 1, &dim_time, &var_short));
  double scale_short = 0.5;
  double offset_short = 2.0;
  short fill_short = -32768;
  ASSERT_EQ(NC_NOERR, nc_put_att_double(ncid, var_short, "scale_factor",
                                        NC_DOUBLE, 1, &scale_short));
  ASSERT_EQ(NC_NOERR, nc_put_att_double(ncid, var_short, "add_offset",
                                        NC_DOUBLE, 1, &offset_short));
  ASSERT_EQ(NC_NOERR, nc_put_att_short(ncid, var_short, "_FillValue", NC_SHORT,
                                       1, &fill_short));

  // 5. NC_USHORT variable
  int var_ushort = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "var_ushort", NC_USHORT, 1, &dim_time,
                                 &var_ushort));
  double scale_ushort = 1.0;
  double offset_ushort = 0.0;
  unsigned short fill_ushort = 65535;
  ASSERT_EQ(NC_NOERR, nc_put_att_double(ncid, var_ushort, "scale_factor",
                                        NC_DOUBLE, 1, &scale_ushort));
  ASSERT_EQ(NC_NOERR, nc_put_att_double(ncid, var_ushort, "add_offset",
                                        NC_DOUBLE, 1, &offset_ushort));
  ASSERT_EQ(NC_NOERR, nc_put_att_ushort(ncid, var_ushort, "_FillValue",
                                        NC_USHORT, 1, &fill_ushort));

  // 6. NC_INT variable (no scale/offset -> OFTInteger)
  int var_int = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "var_int", NC_INT, 1, &dim_time, &var_int));
  int fill_int = -2147483647 - 1;
  ASSERT_EQ(NC_NOERR,
            nc_put_att_int(ncid, var_int, "_FillValue", NC_INT, 1, &fill_int));

  // 7. NC_UINT variable (no scale/offset -> OFTInteger)
  int var_uint = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "var_uint", NC_UINT, 1, &dim_time, &var_uint));
  unsigned int fill_uint = 4294967295U;
  ASSERT_EQ(NC_NOERR, nc_put_att_uint(ncid, var_uint, "_FillValue", NC_UINT, 1,
                                      &fill_uint));

  // 8. NC_DOUBLE variable
  int var_double = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "var_double", NC_DOUBLE, 1, &dim_time,
                                 &var_double));
  double scale_double = 2.0;
  double offset_double = 1.5;
  double fill_double = -9999.0;
  ASSERT_EQ(NC_NOERR, nc_put_att_double(ncid, var_double, "scale_factor",
                                        NC_DOUBLE, 1, &scale_double));
  ASSERT_EQ(NC_NOERR, nc_put_att_double(ncid, var_double, "add_offset",
                                        NC_DOUBLE, 1, &offset_double));
  ASSERT_EQ(NC_NOERR, nc_put_att_double(ncid, var_double, "_FillValue",
                                        NC_DOUBLE, 1, &fill_double));

  // 9. NC_FLOAT variable (unhandled data type)
  int var_float = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "var_float", NC_FLOAT, 1, &dim_time, &var_float));

  // 10. 2D variable (should be skipped by Layer constructor)
  int dims_2d[2] = {dim_other, dim_time};
  int var_2d = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "var_2d", NC_INT, 2, dims_2d, &var_2d));

  // 11. 1D variable on other_dim
  int var_other = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "var_other", NC_INT, 1, &dim_other, &var_other));

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

  // Populate data
  int lons[3] = {10000000, 20000000, 30000000};  // 10.0, 20.0, 30.0 deg
  int lats[3] = {40000000, 50000000, 60000000};  // 40.0, 50.0, 60.0 deg
  ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, var_lon, lons));
  ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, var_lat, lats));

  signed char byte_vals[3] = {10, -128, 20};  // index 1 is nodata
  ASSERT_EQ(NC_NOERR, nc_put_var_schar(ncid, var_byte, byte_vals));

  short short_vals[3] = {100, -32768, 200};  // index 1 is nodata
  ASSERT_EQ(NC_NOERR, nc_put_var_short(ncid, var_short, short_vals));

  unsigned short ushort_vals[3] = {1000, 65535, 2000};  // index 1 is nodata
  ASSERT_EQ(NC_NOERR, nc_put_var_ushort(ncid, var_ushort, ushort_vals));

  int int_vals[3] = {12345, -2147483648, 67890};  // index 1 is nodata
  ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, var_int, int_vals));

  unsigned int uint_vals[3] = {123456, 4294967295U,
                               789012};  // index 1 is nodata
  ASSERT_EQ(NC_NOERR, nc_put_var_uint(ncid, var_uint, uint_vals));

  double double_vals[3] = {12.34, -9999.0, 56.78};  // index 1 is nodata
  ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, var_double, double_vals));

  float float_vals[3] = {1.0f, 2.0f, 3.0f};
  ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, var_float, float_vals));

  int other_vals[2] = {99, 100};
  ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, var_other, other_vals));

  ASSERT_EQ(NC_NOERR, nc_close(ncid));

  // Now open with GDAL
  std::unique_ptr<GDALDataset> poDS(static_cast<GDALDataset*>(
      GDALOpenEx(filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, poDS);

  // Expect 2 layers (one for time, one for other_dim)
  EXPECT_EQ(2, poDS->GetLayerCount());

  OGRLayer* poLayer = poDS->GetLayerByName("s3_sral_mwr_time");
  ASSERT_NE(nullptr, poLayer);

  EXPECT_EQ(wkbPoint, poLayer->GetGeomType());
  EXPECT_NE(nullptr, poLayer->GetSpatialRef());

  // Capabilities
  EXPECT_TRUE(poLayer->TestCapability(OLCFastFeatureCount));
  EXPECT_TRUE(poLayer->TestCapability(OLCRandomRead));
  EXPECT_FALSE(poLayer->TestCapability(OLCFastSpatialFilter));

  // Metadata
  char** papszMD = poLayer->GetMetadata("");
  ASSERT_NE(nullptr, papszMD);
  EXPECT_STREQ("byte test", poLayer->GetMetadataItem("var_byte_comment", ""));
  EXPECT_STREQ("byte test",
               poLayer->GetMetadataItem("var_byte_comment", nullptr));
  EXPECT_EQ(nullptr, poLayer->GetMetadataItem("non_existent_key", ""));
  EXPECT_EQ(nullptr, poLayer->GetMetadata("NON_EXISTING_DOMAIN"));
  EXPECT_EQ(nullptr, poLayer->GetMetadataItem("key", "NON_EXISTING_DOMAIN"));

  // Layer definition
  OGRFeatureDefn* poFDefn = poLayer->GetLayerDefn();
  ASSERT_NE(nullptr, poFDefn);
  EXPECT_EQ(
      OFTReal,
      poFDefn->GetFieldDefn(poFDefn->GetFieldIndex("var_byte"))->GetType());
  EXPECT_EQ(
      OFTReal,
      poFDefn->GetFieldDefn(poFDefn->GetFieldIndex("var_short"))->GetType());
  EXPECT_EQ(
      OFTReal,
      poFDefn->GetFieldDefn(poFDefn->GetFieldIndex("var_ushort"))->GetType());
  EXPECT_EQ(
      OFTInteger,
      poFDefn->GetFieldDefn(poFDefn->GetFieldIndex("var_int"))->GetType());
  EXPECT_EQ(
      OFTInteger,
      poFDefn->GetFieldDefn(poFDefn->GetFieldIndex("var_uint"))->GetType());
  EXPECT_EQ(
      OFTReal,
      poFDefn->GetFieldDefn(poFDefn->GetFieldIndex("var_double"))->GetType());

  // Feature count
  EXPECT_EQ(3, poLayer->GetFeatureCount(FALSE));
  EXPECT_EQ(3, poLayer->GetFeatureCount(TRUE));

  // Read Feature 1
  std::unique_ptr<OGRFeature> poFeat1(poLayer->GetNextFeature());
  ASSERT_NE(nullptr, poFeat1);
  EXPECT_EQ(1, poFeat1->GetFID());
  OGRGeometry* poGeom1 = poFeat1->GetGeometryRef();
  ASSERT_NE(nullptr, poGeom1);
  OGRPoint* poPt1 = poGeom1->toPoint();
  EXPECT_DOUBLE_EQ(10.0, poPt1->getX());
  EXPECT_DOUBLE_EQ(40.0, poPt1->getY());
  // Check scaled field values
  // var_byte: 10 * 0.1 + 1.0 = 2.0
  EXPECT_DOUBLE_EQ(2.0, poFeat1->GetFieldAsDouble("var_byte"));
  // var_short: 100 * 0.5 + 2.0 = 52.0
  EXPECT_DOUBLE_EQ(52.0, poFeat1->GetFieldAsDouble("var_short"));
  // var_ushort: 1000 * 1.0 + 0.0 = 1000.0
  EXPECT_DOUBLE_EQ(1000.0, poFeat1->GetFieldAsDouble("var_ushort"));
  // var_int: 12345
  EXPECT_EQ(12345, poFeat1->GetFieldAsInteger("var_int"));
  // var_uint: 123456
  EXPECT_EQ(123456, poFeat1->GetFieldAsInteger("var_uint"));
  // var_double: 12.34 * 2.0 + 1.5 = 26.18
  EXPECT_DOUBLE_EQ(26.18, poFeat1->GetFieldAsDouble("var_double"));

  // Read Feature 2 (nodata values)
  std::unique_ptr<OGRFeature> poFeat2(poLayer->GetNextFeature());
  ASSERT_NE(nullptr, poFeat2);
  EXPECT_EQ(2, poFeat2->GetFID());
  EXPECT_FALSE(poFeat2->IsFieldSet(poFeat2->GetFieldIndex("var_byte")));
  EXPECT_FALSE(poFeat2->IsFieldSet(poFeat2->GetFieldIndex("var_short")));
  EXPECT_FALSE(poFeat2->IsFieldSet(poFeat2->GetFieldIndex("var_ushort")));
  EXPECT_FALSE(poFeat2->IsFieldSet(poFeat2->GetFieldIndex("var_int")));
  EXPECT_FALSE(poFeat2->IsFieldSet(poFeat2->GetFieldIndex("var_uint")));
  EXPECT_FALSE(poFeat2->IsFieldSet(poFeat2->GetFieldIndex("var_double")));

  // Read Feature 3
  std::unique_ptr<OGRFeature> poFeat3(poLayer->GetNextFeature());
  ASSERT_NE(nullptr, poFeat3);
  EXPECT_EQ(3, poFeat3->GetFID());

  // Past the end
  EXPECT_EQ(nullptr, poLayer->GetNextFeature());

  // ResetReading
  poLayer->ResetReading();
  std::unique_ptr<OGRFeature> poFeat1Again(poLayer->GetNextFeature());
  ASSERT_NE(nullptr, poFeat1Again);
  EXPECT_EQ(1, poFeat1Again->GetFID());

  // GetFeature random access
  std::unique_ptr<OGRFeature> poFeat2Direct(poLayer->GetFeature(2));
  ASSERT_NE(nullptr, poFeat2Direct);
  EXPECT_EQ(2, poFeat2Direct->GetFID());
  EXPECT_EQ(nullptr, poLayer->GetFeature(0));
  EXPECT_EQ(nullptr, poLayer->GetFeature(-1));
  EXPECT_EQ(nullptr, poLayer->GetFeature(4));

  // Test Spatial Filter
  poLayer->SetSpatialFilterRect(9.0, 39.0, 11.0, 41.0);
  EXPECT_FALSE(poLayer->TestCapability(OLCFastFeatureCount));
  EXPECT_EQ(1, poLayer->GetFeatureCount(TRUE));
  poLayer->ResetReading();
  std::unique_ptr<OGRFeature> poFeatFiltered(poLayer->GetNextFeature());
  ASSERT_NE(nullptr, poFeatFiltered);
  EXPECT_EQ(1, poFeatFiltered->GetFID());
  EXPECT_EQ(nullptr, poLayer->GetNextFeature());
  poLayer->SetSpatialFilter(nullptr);

  // Test Attribute Query Filter
  EXPECT_EQ(OGRERR_NONE, poLayer->SetAttributeFilter("var_int = 12345"));
  EXPECT_FALSE(poLayer->TestCapability(OLCFastFeatureCount));
  poLayer->ResetReading();
  std::unique_ptr<OGRFeature> poFeatAttrFiltered(poLayer->GetNextFeature());
  ASSERT_NE(nullptr, poFeatAttrFiltered);
  EXPECT_EQ(1, poFeatAttrFiltered->GetFID());
  EXPECT_EQ(nullptr, poLayer->GetNextFeature());
  poLayer->SetAttributeFilter(nullptr);

  // Test layer 2 (other_dim)
  OGRLayer* poLayerOther = poDS->GetLayerByName("s3_sral_mwr_other_dim");
  ASSERT_NE(nullptr, poLayerOther);
  EXPECT_EQ(2, poLayerOther->GetFeatureCount(FALSE));
  std::unique_ptr<OGRFeature> poFeatOther(poLayerOther->GetNextFeature());
  ASSERT_NE(nullptr, poFeatOther);
  EXPECT_EQ(99, poFeatOther->GetFieldAsInteger("var_other"));
  // Layer other_dim has no lon/lat, so geometry should be null
  EXPECT_EQ(nullptr, poFeatOther->GetGeometryRef());

  poDS.reset();
  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFSentinel3Test, ProductWithoutEllipsoidMetadata) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "s3_no_ellipsoid.nc")
          .string();

  int ncid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_create(filename.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, NC_GLOBAL, "mission_name",
                                      strlen("Sentinel 3"), "Sentinel 3"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, NC_GLOBAL, "altimeter_sensor_name",
                                      strlen("SRAL"), "SRAL"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, NC_GLOBAL, "radiometer_sensor_name",
                                      strlen("MWR"), "MWR"));

  int dim_time = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "time", 1, &dim_time));

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));
  ASSERT_EQ(NC_NOERR, nc_close(ncid));

  // Open with GDAL - SRS should be null because ellipsoid metadata is absent
  std::unique_ptr<GDALDataset> poDS(static_cast<GDALDataset*>(
      GDALOpenEx(filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, poDS);
  EXPECT_EQ(1, poDS->GetLayerCount());
  OGRLayer* poLayer = poDS->GetLayer(0);
  ASSERT_NE(nullptr, poLayer);
  EXPECT_EQ(nullptr, poLayer->GetSpatialRef());

  poDS.reset();
  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFSentinel3Test, ZeroDimensionsReturnEarly) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "s3_zero_dims.nc")
          .string();

  int ncid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_create(filename.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, NC_GLOBAL, "mission_name",
                                      strlen("Sentinel 3"), "Sentinel 3"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, NC_GLOBAL, "altimeter_sensor_name",
                                      strlen("SRAL"), "SRAL"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, NC_GLOBAL, "radiometer_sensor_name",
                                      strlen("MWR"), "MWR"));

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));
  ASSERT_EQ(NC_NOERR, nc_close(ncid));

  CPLPushErrorHandler(CPLQuietErrorHandler);
  std::unique_ptr<GDALDataset> poDS(static_cast<GDALDataset*>(
      GDALOpenEx(filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
  CPLPopErrorHandler();
  // With 0 dimensions, ProcessSentinel3_SRAL_MWR creates no layers, so GDAL
  // returns nullptr
  EXPECT_EQ(nullptr, poDS);

  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFSentinel3Test, OpenUpdateModeFails) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "s3_update.nc").string();

  int ncid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_create(filename.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, NC_GLOBAL, "mission_name",
                                      strlen("Sentinel 3"), "Sentinel 3"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, NC_GLOBAL, "altimeter_sensor_name",
                                      strlen("SRAL"), "SRAL"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, NC_GLOBAL, "radiometer_sensor_name",
                                      strlen("MWR"), "MWR"));
  int dim_time = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "time", 1, &dim_time));
  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));
  ASSERT_EQ(NC_NOERR, nc_close(ncid));

  CPLPushErrorHandler(CPLQuietErrorHandler);
  std::unique_ptr<GDALDataset> poDS(static_cast<GDALDataset*>(
      GDALOpenEx(filename.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE, nullptr,
                 nullptr, nullptr)));
  CPLPopErrorHandler();
  // Sentinel 3 SRAL/MWR driver rejects GA_Update mode
  EXPECT_EQ(nullptr, poDS);

  VSIUnlink(filename.c_str());
}

}  // namespace
}  // namespace autotest2
