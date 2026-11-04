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
// Tests registration of raw format drivers (rawdrivers.cpp).
// Note: In google3, GDALRegister_raw_no_sidecar() is compiled as a no-op
// stub via #if 0; only ENVI and EHdr are registered by
// GDALRegister_raw_with_sidecar().
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/raw/rawdrivers.cpp

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"

extern "C" void GDALRegister_raw();
extern "C" void GDALRegister_raw_no_sidecar();
extern "C" void GDALRegister_raw_with_sidecar();

namespace autotest2 {
namespace {

constexpr char kEnviDriverName[] = "ENVI";
constexpr char kEhdrDriverName[] = "EHdr";

TEST(RawDriversTest, RegisterRawNoSidecarIsNoOpInGoogle3) {
  GDALRegister_raw_no_sidecar();

  GDALDriverManager* mgr = GetGDALDriverManager();
  ASSERT_NE(nullptr, mgr);

  // In google3, drivers in GDALRegister_raw_no_sidecar are #if 0'd out.
  EXPECT_EQ(nullptr, mgr->GetDriverByName("PNM"));
  EXPECT_EQ(nullptr, mgr->GetDriverByName("DOQ1"));
  EXPECT_EQ(nullptr, mgr->GetDriverByName("DOQ2"));
  EXPECT_EQ(nullptr, mgr->GetDriverByName("BT"));
  EXPECT_EQ(nullptr, mgr->GetDriverByName("LAN"));
}

TEST(RawDriversTest, RegisterRawWithSidecarRegistersEnviAndEhdr) {
  GDALRegister_raw_with_sidecar();

  GDALDriverManager* mgr = GetGDALDriverManager();
  ASSERT_NE(nullptr, mgr);

  GDALDriver* envi = mgr->GetDriverByName(kEnviDriverName);
  ASSERT_NE(nullptr, envi);
  EXPECT_STREQ("ENVI .hdr Labelled",
               envi->GetMetadataItem(GDAL_DMD_LONGNAME));

  GDALDriver* ehdr = mgr->GetDriverByName(kEhdrDriverName);
  ASSERT_NE(nullptr, ehdr);
  EXPECT_STREQ("ESRI .hdr Labelled",
               ehdr->GetMetadataItem(GDAL_DMD_LONGNAME));

  // Disabled sidecars should be null.
  EXPECT_EQ(nullptr, mgr->GetDriverByName("PAux"));
  EXPECT_EQ(nullptr, mgr->GetDriverByName("GenBin"));
  EXPECT_EQ(nullptr, mgr->GetDriverByName("ISCE"));
}

TEST(RawDriversTest, RegisterRawRegistersBothGroups) {
  GDALRegister_raw();

  GDALDriverManager* mgr = GetGDALDriverManager();
  ASSERT_NE(nullptr, mgr);

  EXPECT_NE(nullptr, mgr->GetDriverByName(kEnviDriverName));
  EXPECT_NE(nullptr, mgr->GetDriverByName(kEhdrDriverName));
}

// TODO: b/335317901 - Test remaining disabled drivers when upstreamed.

}  // namespace
}  // namespace autotest2
