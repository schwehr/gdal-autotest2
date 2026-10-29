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
// Tests gcore/geoheif.cpp.

#include "third_party/gdal/gcore/geoheif.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/port/cpl_port.h"

namespace {

void AppendMSB32(std::vector<uint8_t>& buf, uint32_t val) {
  CPL_MSBPTR32(&val);
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&val);
  buf.insert(buf.end(), p, p + sizeof(uint32_t));
}

void AppendMSB64(std::vector<uint8_t>& buf, double val) {
  CPL_MSBPTR64(&val);
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&val);
  buf.insert(buf.end(), p, p + sizeof(double));
}

std::vector<uint8_t> MakeCRSPayload(const std::string& encoding,
                                    const std::string& body) {
  std::vector<uint8_t> buf(4, 0);
  buf.insert(buf.end(), encoding.begin(), encoding.end());
  buf.insert(buf.end(), body.begin(), body.end());
  return buf;
}

TEST(GeoheifTest, ModelTransformationAndSRS) {
  gdal::GeoHEIF gh;
  EXPECT_FALSE(gh.has_SRS());
  EXPECT_FALSE(gh.has_GCPs());
  EXPECT_EQ(nullptr, gh.GetSpatialRef());
  EXPECT_EQ(nullptr, gh.GetGCPSpatialRef());
  EXPECT_EQ(0, gh.GetGCPCount());

  // Short payload is ignored.
  uint8_t short_buf[4] = {0, 0, 0, 1};
  gh.setModelTransformation(short_buf, sizeof(short_buf));

  // Unsupported version (payload[0] != 0) is ignored.
  std::vector<uint8_t> bad_ver(6 * 8 + 4, 0);
  bad_ver[0] = 1;
  gh.setModelTransformation(bad_ver.data(), bad_ver.size());

  // Valid 2D transformation payload (version 0, flags 0x01).
  std::vector<uint8_t> mtxf = {0, 0, 0, 1};
  AppendMSB64(mtxf, 10.0);   // modelTransform[1]
  AppendMSB64(mtxf, 0.0);    // modelTransform[2]
  AppendMSB64(mtxf, 100.0);  // modelTransform[0]
  AppendMSB64(mtxf, 0.0);    // modelTransform[4]
  AppendMSB64(mtxf, -10.0);  // modelTransform[5]
  AppendMSB64(mtxf, 200.0);  // modelTransform[3]
  gh.setModelTransformation(mtxf.data(), mtxf.size());

  GDALGeoTransform gt;
  EXPECT_EQ(CE_None, gh.GetGeoTransform(gt));
  EXPECT_DOUBLE_EQ(100.0, gt[0]);
  EXPECT_DOUBLE_EQ(10.0, gt[1]);
  EXPECT_DOUBLE_EQ(0.0, gt[2]);
  EXPECT_DOUBLE_EQ(200.0, gt[3]);
  EXPECT_DOUBLE_EQ(0.0, gt[4]);
  EXPECT_DOUBLE_EQ(-10.0, gt[5]);

  // extractSRS: short payload.
  gh.extractSRS(short_buf, sizeof(short_buf));
  EXPECT_FALSE(gh.has_SRS());

  // extractSRS: unsupported encoding.
  auto bad_enc = MakeCRSPayload("xxxx", "something");
  gh.extractSRS(bad_enc.data(), bad_enc.size());
  EXPECT_FALSE(gh.has_SRS());

  // extractSRS: curi invalid format (missing brackets or null terminator).
  auto bad_curi1 = MakeCRSPayload("curi", "EPSG:4326");
  gh.extractSRS(bad_curi1.data(), bad_curi1.size());
  EXPECT_FALSE(gh.has_SRS());

  // extractSRS: curi missing ':' separator.
  std::string no_colon("[EPSG4326]\0", 11);
  auto bad_curi2 = MakeCRSPayload("curi", no_colon);
  gh.extractSRS(bad_curi2.data(), bad_curi2.size());
  EXPECT_FALSE(gh.has_SRS());

  // extractSRS: valid curi (may or may not resolve network URL, but exercises
  // path).
  std::string valid_curi("[EPSG:4326]\0", 12);
  auto curi_payload = MakeCRSPayload("curi", valid_curi);
  gh.extractSRS(curi_payload.data(), curi_payload.size());

  // extractSRS: crsu encoding.
  std::string crsu_str("http://www.opengis.net/def/crs/EPSG/0/4326\0", 43);
  auto crsu_payload = MakeCRSPayload("crsu", crsu_str);
  gh.extractSRS(crsu_payload.data(), crsu_payload.size());

  // extractSRS: wkt2 encoding.
  OGRSpatialReference wgs84;
  wgs84.SetWellKnownGeogCS("WGS84");
  char* wkt = nullptr;
  wgs84.exportToWkt(&wkt);
  std::string wkt_str(wkt, strlen(wkt) + 1);
  CPLFree(wkt);
  auto wkt_payload = MakeCRSPayload("wkt2", wkt_str);
  gh.extractSRS(wkt_payload.data(), wkt_payload.size());
  EXPECT_TRUE(gh.has_SRS());
  EXPECT_NE(nullptr, gh.GetSpatialRef());
  EXPECT_NE(nullptr, gh.GetGCPSpatialRef());
  EXPECT_EQ(CE_None, gh.GetGeoTransform(gt));
}

TEST(GeoheifTest, AddGCPs2DAnd3D) {
  gdal::GeoHEIF gh;

  // Too short payload.
  uint8_t short_gcp[10] = {0};
  gh.addGCPs(short_gcp, sizeof(short_gcp));
  EXPECT_FALSE(gh.has_GCPs());

  // Unsupported version (data[0] != 0).
  std::vector<uint8_t> bad_ver(32, 0);
  bad_ver[0] = 1;
  gh.addGCPs(bad_ver.data(), bad_ver.size());
  EXPECT_FALSE(gh.has_GCPs());

  // 3D flag (data[3] == 0x00) with length too short for 3D.
  std::vector<uint8_t> short_3d(30, 0);
  gh.addGCPs(short_3d.data(), short_3d.size());
  EXPECT_FALSE(gh.has_GCPs());

  // Valid 2D GCP (data[3] == 0x01, count = 1).
  std::vector<uint8_t> gcp_2d = {0, 0, 0, 1, 0, 1};
  AppendMSB32(gcp_2d, 10);
  AppendMSB32(gcp_2d, 20);
  AppendMSB64(gcp_2d, 100.5);
  AppendMSB64(gcp_2d, 200.5);
  gh.addGCPs(gcp_2d.data(), gcp_2d.size());
  EXPECT_TRUE(gh.has_GCPs());
  EXPECT_EQ(1, gh.GetGCPCount());
  const GDAL_GCP* gcps = gh.GetGCPs();
  ASSERT_NE(nullptr, gcps);
  EXPECT_DOUBLE_EQ(10.0, gcps[0].dfGCPPixel);
  EXPECT_DOUBLE_EQ(20.0, gcps[0].dfGCPLine);
  EXPECT_DOUBLE_EQ(100.5, gcps[0].dfGCPX);
  EXPECT_DOUBLE_EQ(200.5, gcps[0].dfGCPY);
  EXPECT_DOUBLE_EQ(0.0, gcps[0].dfGCPZ);

  // Valid 3D GCP (data[3] == 0x00, count = 1).
  std::vector<uint8_t> gcp_3d = {0, 0, 0, 0, 0, 1};
  AppendMSB32(gcp_3d, 30);
  AppendMSB32(gcp_3d, 40);
  AppendMSB64(gcp_3d, 300.5);
  AppendMSB64(gcp_3d, 400.5);
  AppendMSB64(gcp_3d, 50.25);
  gh.addGCPs(gcp_3d.data(), gcp_3d.size());
  EXPECT_EQ(2, gh.GetGCPCount());
  gcps = gh.GetGCPs();
  EXPECT_DOUBLE_EQ(50.25, gcps[1].dfGCPZ);
}

}  // namespace
