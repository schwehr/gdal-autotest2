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
//
// Tests gcore/gdalgeorefpamdataset.cpp.

#include "third_party/gdal/gcore/gdalgeorefpamdataset.h"

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_string.h"

namespace {

class TestGeorefPamDataset : public GDALGeorefPamDataset {
 public:
  TestGeorefPamDataset() = default;

  void ConfigureInternalGeoref() {
    bGeoTransformValid = true;
    m_gt = GDALGeoTransform(100.0, 1.0, 0.0, 200.0, 0.0, -1.0);
    m_oSRS.SetWellKnownGeogCS("WGS84");
    m_bPixelIsPoint = true;
    m_papszRPC = CSLSetNameValue(m_papszRPC, "LINE_OFF", "10");

    nGCPCount = 1;
    pasGCPList = static_cast<GDAL_GCP*>(CPLCalloc(1, sizeof(GDAL_GCP)));
    GDALInitGCPs(1, pasGCPList);
    pasGCPList[0].dfGCPX = 10.0;
    pasGCPList[0].dfGCPY = 20.0;
  }

  void ClearGCPs() {
    if (nGCPCount > 0) {
      GDALDeinitGCPs(nGCPCount, pasGCPList);
      CPLFree(pasGCPList);
      pasGCPList = nullptr;
      nGCPCount = 0;
    }
  }
};

TEST(GdalgeorefpamdatasetTest, GeorefAndMetadataPrecedence) {
  TestGeorefPamDataset ds;

  // Empty state before configuring internal georef.
  GDALGeoTransform gt;
  EXPECT_EQ(CE_Failure, ds.GetGeoTransform(gt));
  EXPECT_EQ(nullptr, ds.GetSpatialRef());
  EXPECT_EQ(nullptr, ds.GetGCPSpatialRef());
  EXPECT_EQ(0, ds.GetGCPCount());
  EXPECT_EQ(nullptr, ds.GetGCPs());
  EXPECT_EQ(nullptr, ds.GetMetadata("RPC"));
  EXPECT_EQ(nullptr, ds.GetMetadataItem(GDALMD_AREA_OR_POINT));

  // Configure internal georef.
  ds.ConfigureInternalGeoref();
  EXPECT_EQ(CE_None, ds.TryLoadXML(nullptr));

  EXPECT_EQ(CE_None, ds.GetGeoTransform(gt));
  EXPECT_DOUBLE_EQ(100.0, gt[0]);

  // When GCPCount > 0, GetSpatialRef() returns nullptr while GetGCPSpatialRef()
  // returns &m_oSRS.
  EXPECT_EQ(1, ds.GetGCPCount());
  EXPECT_NE(nullptr, ds.GetGCPs());
  EXPECT_EQ(nullptr, ds.GetSpatialRef());
  EXPECT_NE(nullptr, ds.GetGCPSpatialRef());

  // Clear GCPs so GetSpatialRef() returns &m_oSRS.
  ds.ClearGCPs();
  EXPECT_NE(nullptr, ds.GetSpatialRef());

  // Metadata on default, RPC, and custom domains.
  EXPECT_STREQ("10", ds.GetMetadataItem("LINE_OFF", "RPC"));
  EXPECT_STREQ(GDALMD_AOP_POINT, ds.GetMetadataItem(GDALMD_AREA_OR_POINT, ""));
  EXPECT_EQ(nullptr, ds.GetMetadataItem("OTHER_KEY", "SUBDOMAIN"));
  EXPECT_EQ(nullptr, ds.GetMetadata("SUBDOMAIN"));

  // SetMetadata and SetMetadataItem when m_bPAMLoaded is true.
  CPLStringList md;
  md.SetNameValue("FOO", "BAR");
  EXPECT_EQ(CE_None, ds.SetMetadata(md.List(), ""));
  EXPECT_STREQ("BAR", ds.GetMetadataItem("FOO", ""));

  EXPECT_EQ(CE_None, ds.SetMetadataItem("BAZ", "QUX", ""));
  EXPECT_STREQ("QUX", ds.GetMetadataItem("BAZ", ""));
  EXPECT_EQ(CE_None, ds.SetMetadataItem("SUBK", "SUBV", "SUBDOMAIN"));
  EXPECT_STREQ("SUBV", ds.GetMetadataItem("SUBK", "SUBDOMAIN"));
}

}  // namespace
