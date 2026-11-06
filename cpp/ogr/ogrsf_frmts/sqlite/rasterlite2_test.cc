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
// Tests for RasterLite2 support in SQLite driver (rasterlite2.cpp).
//
// Note: In google3 (//third_party/gdal/BUILD), HAVE_RASTERLITE2 is not
// enabled, so OpenRaster and OpenRasterSubDataset return false.
//
// See also:
//   https://gdal.org/drivers/raster/rasterlite2.html
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/sqlite/rasterlite2.cpp

#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal_geotransform.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/sqlite/ogr_sqlite.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

template <typename Tag>
struct Stowed {
  inline static typename Tag::type value;
};

template <typename Tag, typename Tag::type Ptr>
struct Rob {
  struct Init {
    Init() { Stowed<Tag>::value = Ptr; }
  };
  inline static Init init;
};

struct OpenRasterTag {
  using type = bool (OGRSQLiteDataSource::*)();
};
template struct Rob<OpenRasterTag, &OGRSQLiteDataSource::OpenRaster>;

struct OpenRasterSubDatasetTag {
  using type = bool (OGRSQLiteDataSource::*)(const char*);
};
template struct Rob<OpenRasterSubDatasetTag,
                    &OGRSQLiteDataSource::OpenRasterSubDataset>;

struct SubDatasetsTag {
  using type = CPLStringList OGRSQLiteDataSource::*;
};
template struct Rob<SubDatasetsTag, &OGRSQLiteDataSource::m_aosSubDatasets>;

struct GeoTransformValidTag {
  using type = bool OGRSQLiteDataSource::*;
};
template struct Rob<GeoTransformValidTag,
                    &OGRSQLiteDataSource::m_bGeoTransformValid>;

struct GtTag {
  using type = GDALGeoTransform OGRSQLiteDataSource::*;
};
template struct Rob<GtTag, &OGRSQLiteDataSource::m_gt>;

struct SrsTag {
  using type = OGRSpatialReference OGRSQLiteDataSource::*;
};
template struct Rob<SrsTag, &OGRSQLiteDataSource::m_oSRS>;

TEST(Rasterlite2Test, DefaultStateWithoutRasterlite2) {
  auto ds = std::make_unique<OGRSQLiteDataSource>();

  EXPECT_FALSE((ds.get()->*Stowed<OpenRasterTag>::value)());
  EXPECT_FALSE(
      (ds.get()->*Stowed<OpenRasterSubDatasetTag>::value)("RASTERLITE2:test"));

  EXPECT_EQ(nullptr, ds->GetMetadata("SUBDATASETS"));
  EXPECT_EQ(nullptr, ds->GetMetadata(""));

  GDALGeoTransform gt;
  EXPECT_EQ(CE_Failure, ds->GetGeoTransform(gt));
  EXPECT_EQ(nullptr, ds->GetSpatialRef());
}

TEST(Rasterlite2Test, PopulatedStateMethods) {
  auto ds = std::make_unique<OGRSQLiteDataSource>();
  auto& subdatasets = ds.get()->*Stowed<SubDatasetsTag>::value;
  subdatasets.AddNameValue("SUBDATASET_1_NAME", "RASTERLITE2:a:b");
  subdatasets.AddNameValue("SUBDATASET_1_DESC", "Desc 1");
  subdatasets.AddNameValue("SUBDATASET_2_NAME", "RASTERLITE2:a:c");
  subdatasets.AddNameValue("SUBDATASET_2_DESC", "Desc 2");

  ds.get()->*Stowed<GeoTransformValidTag>::value = true;
  ds.get()->*Stowed<GtTag>::value =
      GDALGeoTransform(10.0, 1.0, 0.0, 20.0, 0.0, -1.0);
  (ds.get()->*Stowed<SrsTag>::value).SetWellKnownGeogCS("WGS84");

  char** papszSub = ds->GetMetadata("SUBDATASETS");
  ASSERT_NE(nullptr, papszSub);
  EXPECT_STREQ("SUBDATASET_1_NAME=RASTERLITE2:a:b", papszSub[0]);

  GDALGeoTransform gt;
  EXPECT_EQ(CE_None, ds->GetGeoTransform(gt));
  EXPECT_DOUBLE_EQ(10.0, gt[0]);
  EXPECT_DOUBLE_EQ(1.0, gt[1]);
  EXPECT_DOUBLE_EQ(0.0, gt[2]);
  EXPECT_DOUBLE_EQ(20.0, gt[3]);
  EXPECT_DOUBLE_EQ(0.0, gt[4]);
  EXPECT_DOUBLE_EQ(-1.0, gt[5]);

  const OGRSpatialReference* srs = ds->GetSpatialRef();
  ASSERT_NE(nullptr, srs);
  EXPECT_TRUE(srs->IsGeographic());
}

// TODO: b/335317901 - Cover librasterlite2 band, overview, color table, and
// CreateCopy paths if HAVE_RASTERLITE2 is enabled.

}  // namespace
}  // namespace autotest2
