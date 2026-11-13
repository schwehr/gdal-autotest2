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
// Unit tests for ogrparquetdrivercore.cpp (OGRParquetDriverIdentify and
// OGRParquetDriverSetCommonMetadata).
//
// Build caveat in google3: GDAL_USE_ARROWDATASET and PLUGIN_FILENAME are not
// defined in //third_party/gdal/BUILD, so the directory-identification branch
// and DeclareDeferredOGRParquetPlugin() are compiled out.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/parquet/ogrparquetdrivercore.cpp
//   https://gdal.org/drivers/vector/parquet.html

#include "third_party/gdal/ogr/ogrsf_frmts/parquet/ogrparquetdrivercore.h"

#include <cstdint>
#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/ogrparquetdrivercore_test";

std::string MakeParquetFrame(uint32_t metadata_size_le,
                             const char footer_magic[4], int body_bytes = 4) {
  std::string data;
  data.append("PAR1", 4);
  data.append(body_bytes, '\0');
  data.push_back(static_cast<char>(metadata_size_le & 0xff));
  data.push_back(static_cast<char>((metadata_size_le >> 8) & 0xff));
  data.push_back(static_cast<char>((metadata_size_le >> 16) & 0xff));
  data.push_back(static_cast<char>((metadata_size_le >> 24) & 0xff));
  data.append(footer_magic, 4);
  return data;
}

TEST(OgrParquetDriverCoreTest, IdentifyCases) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kTestDir); };
  VSIMkdirRecursive(kTestDir, 0755);

  // 1. Non-existent path returns FALSE.
  {
    auto info = std::make_unique<GDALOpenInfo>("/does_not_exist.parquet",
                                               GDAL_OF_READONLY);
    EXPECT_EQ(FALSE, OGRParquetDriverIdentify(info.get()));
  }

  // 2. Explicit PARQUET: prefix returns TRUE immediately without I/O.
  {
    auto info = std::make_unique<GDALOpenInfo>("PARQUET:/any/path.parquet",
                                               GDAL_OF_READONLY);
    EXPECT_EQ(TRUE, OGRParquetDriverIdentify(info.get()));
  }

  // 3. Too-short file (11 bytes, < 12) even starting with PAR1 returns FALSE.
  {
    constexpr char kPath[] = "/vsimem/ogrparquetdrivercore_test/short.parquet";
    const std::string short_frame("PAR1\0\0\0\0PAR", 11);
    VsiMemTempWrapper file(kPath, short_frame);
    auto info = std::make_unique<GDALOpenInfo>(kPath, GDAL_OF_READONLY);
    EXPECT_EQ(FALSE, OGRParquetDriverIdentify(info.get()));
  }

  // 4. Wrong header magic returns FALSE.
  {
    constexpr char kPath[] =
        "/vsimem/ogrparquetdrivercore_test/bad_hdr.parquet";
    std::string data = MakeParquetFrame(4, "PAR1");
    data[0] = 'X';
    VsiMemTempWrapper file(kPath, data);
    auto info = std::make_unique<GDALOpenInfo>(kPath, GDAL_OF_READONLY);
    EXPECT_EQ(FALSE, OGRParquetDriverIdentify(info.get()));
  }

  // 5. Wrong footer magic returns FALSE.
  {
    constexpr char kPath[] =
        "/vsimem/ogrparquetdrivercore_test/bad_ftr.parquet";
    VsiMemTempWrapper file(kPath, MakeParquetFrame(4, "PARX"));
    auto info = std::make_unique<GDALOpenInfo>(kPath, GDAL_OF_READONLY);
    EXPECT_EQ(FALSE, OGRParquetDriverIdentify(info.get()));
  }

  // 6. Metadata size >= file size fails bounds check and returns FALSE.
  {
    constexpr char kPath[] = "/vsimem/ogrparquetdrivercore_test/bad_sz.parquet";
    const std::string data = MakeParquetFrame(1000, "PAR1");
    VsiMemTempWrapper file(kPath, data);
    auto info = std::make_unique<GDALOpenInfo>(kPath, GDAL_OF_READONLY);
    EXPECT_EQ(FALSE, OGRParquetDriverIdentify(info.get()));
  }

  // 7. Valid 16-byte header/metadata-len/footer framing returns TRUE and
  // rewinds the file handle to offset 0.
  {
    constexpr char kPath[] = "/vsimem/ogrparquetdrivercore_test/valid.parquet";
    const std::string data = MakeParquetFrame(4, "PAR1");
    VsiMemTempWrapper file(kPath, data);
    auto info = std::make_unique<GDALOpenInfo>(kPath, GDAL_OF_READONLY);
    EXPECT_EQ(TRUE, OGRParquetDriverIdentify(info.get()));
    ASSERT_NE(nullptr, info->fpL);
    EXPECT_EQ(0, VSIFTellL(info->fpL));
  }
}

TEST(OgrParquetDriverCoreTest, SetCommonMetadata) {
  auto drv = std::make_unique<GDALDriver>();
  OGRParquetDriverSetCommonMetadata(drv.get());

  EXPECT_STREQ("Parquet", drv->GetDescription());
  EXPECT_STREQ("YES", drv->GetMetadataItem(GDAL_DCAP_VECTOR));
  EXPECT_STREQ("YES", drv->GetMetadataItem(GDAL_DCAP_CREATE_LAYER));
  EXPECT_STREQ("(Geo)Parquet", drv->GetMetadataItem(GDAL_DMD_LONGNAME));
  EXPECT_STREQ("parquet", drv->GetMetadataItem(GDAL_DMD_EXTENSION));
  EXPECT_STREQ("drivers/vector/parquet.html",
               drv->GetMetadataItem(GDAL_DMD_HELPTOPIC));
  EXPECT_STREQ("YES", drv->GetMetadataItem(GDAL_DCAP_VIRTUALIO));
  EXPECT_STREQ("YES", drv->GetMetadataItem(GDAL_DCAP_MEASURED_GEOMETRIES));
  EXPECT_STREQ("YES", drv->GetMetadataItem(GDAL_DCAP_Z_GEOMETRIES));
  EXPECT_STREQ("YES",
               drv->GetMetadataItem(GDAL_DCAP_REOPEN_AFTER_WRITE_REQUIRED));
  EXPECT_STREQ("YES", drv->GetMetadataItem(GDAL_DCAP_CAN_READ_AFTER_DELETE));
  EXPECT_STREQ("YES", drv->GetMetadataItem(GDAL_DCAP_CREATE_FIELD));
  EXPECT_STREQ("YES", drv->GetMetadataItem(GDAL_DCAP_OPEN));
  EXPECT_STREQ("YES", drv->GetMetadataItem(GDAL_DCAP_CREATE));
  EXPECT_STREQ("YES", drv->GetMetadataItem(GDAL_DCAP_UPDATE));
  EXPECT_STREQ("Features", drv->GetMetadataItem(GDAL_DMD_UPDATE_ITEMS));
  EXPECT_STREQ("YES", drv->GetMetadataItem(GDAL_DCAP_DELETE_FIELD));
  EXPECT_STREQ("YES", drv->GetMetadataItem(GDAL_DCAP_REORDER_FIELDS));
  EXPECT_STREQ("OGRSQL SQLITE",
               drv->GetMetadataItem(GDAL_DMD_SUPPORTED_SQL_DIALECTS));
  EXPECT_THAT(drv->GetMetadataItem(GDAL_DMD_OPENOPTIONLIST),
              HasSubstr("GEOM_POSSIBLE_NAMES"));
  EXPECT_THAT(drv->GetMetadataItem(GDAL_DMD_CREATIONFIELDDATATYPES),
              HasSubstr("Integer64"));
  EXPECT_THAT(drv->GetMetadataItem(GDAL_DMD_CREATIONFIELDDATASUBTYPES),
              HasSubstr("UUID"));
  EXPECT_THAT(drv->GetMetadataItem(GDAL_DMD_CREATION_FIELD_DEFN_FLAGS),
              HasSubstr("AlternativeName"));
  EXPECT_THAT(drv->GetMetadataItem(GDAL_DMD_ALTER_FIELD_DEFN_FLAGS),
              HasSubstr("WidthPrecision"));
  EXPECT_EQ(&OGRParquetDriverIdentify, drv->pfnIdentify);
}

}  // namespace
}  // namespace autotest2
