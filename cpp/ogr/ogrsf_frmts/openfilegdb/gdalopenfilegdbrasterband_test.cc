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
// Tests Open FileGDB raster driver (gdalopenfilegdbrasterband.cpp).

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/filegdbtable.h"
#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/filegdbtable_priv.h"
#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/ogr_openfilegdb.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

extern "C" void RegisterOGROpenFileGDB();

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/gdalopenfilegdbrasterband_test";

int MakeBandTypes(int nBitWidth, int nBitType, int nCompression) {
  return (nBitWidth << 19) | (nBitType << 16) | (nCompression << 8);
}

void AppendUTF16String(std::vector<GByte>& buf, const std::string& str) {
  uint32_t byte_len = static_cast<uint32_t>((str.size() + 1) * 2);
  buf.push_back(static_cast<GByte>(byte_len & 0xFF));
  buf.push_back(static_cast<GByte>((byte_len >> 8) & 0xFF));
  buf.push_back(static_cast<GByte>((byte_len >> 16) & 0xFF));
  buf.push_back(static_cast<GByte>((byte_len >> 24) & 0xFF));
  for (char ch : str) {
    buf.push_back(static_cast<GByte>(ch));
    buf.push_back(0);
  }
  buf.push_back(0);
  buf.push_back(0);
}

class GdalOpenFileGdbRasterBandTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALAllRegister();
    RegisterOGROpenFileGDB();
    VSIMkdir(kTestDir, 0755);
  }

  void TearDown() override { VSIRmdirRecursive(kTestDir); }

  static void TearDownTestSuite() { OSRCleanup(); }
};

bool CreateBaselineGDB(const std::string& dir,
                       const std::string& raster_name = "ras",
                       bool bRegisterAux = true, bool bRegisterVAT = true,
                       const char* pszXmlDef = nullptr,
                       const char* pszXmlDoc = nullptr) {
  OGROpenFileGDBDataSource ds;
  if (!ds.Create(dir.c_str())) return false;

  // Record 9 -> a00000009.gdbtable
  if (!ds.RegisterLayerInSystemCatalog("fras_bnd_" + raster_name)) return false;
  // Record 10 -> a0000000a.gdbtable
  if (!ds.RegisterLayerInSystemCatalog("fras_blk_" + raster_name)) return false;
  // Record 11 -> a0000000b.gdbtable
  if (bRegisterAux &&
      !ds.RegisterLayerInSystemCatalog("fras_aux_" + raster_name))
    return false;
  // Record 12 -> a0000000c.gdbtable
  if (bRegisterVAT && !ds.RegisterLayerInSystemCatalog("VAT_" + raster_name))
    return false;

  std::string osDef =
      pszXmlDef ? pszXmlDef
                : "<DERasterDataset><SpatialReference><WKT>GEOGCS[\"WGS "
                  "84\",DATUM[\"WGS_1984\","
                  "SPHEROID[\"WGS "
                  "84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],"
                  "UNIT[\"degree\",0.0174532925199433]]</WKT></"
                  "SpatialReference></DERasterDataset>";
  std::string osDoc = pszXmlDoc ? pszXmlDoc
                                : "<metadata><spdoinfo><rastinfo><rowcount>4</"
                                  "rowcount><colcount>4</colcount>"
                                  "<rastband>1</rastband><rastbpp>8</rastbpp></"
                                  "rastinfo></spdoinfo></metadata>";

  if (!ds.RegisterASpatialTableInItems(OFGDBGenerateUUID(), raster_name,
                                       "\\" + raster_name, osDef.c_str(),
                                       osDoc.c_str())) {
    return false;
  }
  return true;
}

bool CreateBndTable(const std::string& filename, int nWidth = 4,
                    int nHeight = 4, int nBandTypes = (8 << 19),
                    int nBlockWidth = 4, int nBlockHeight = 4,
                    double dfOriginX = 0.0, double dfOriginY = 4.0,
                    double dfMinX = 0.0, double dfMinY = 0.0,
                    double dfMaxX = 4.0, double dfMaxY = 4.0, int nSRID = 1,
                    int nBands = 1, int nRasterId = 1) {
  OpenFileGDB::FileGDBTable table;
  if (!table.Create(filename.c_str(), 4, OpenFileGDB::FGTGT_NONE, false, false))
    return false;
  table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "rasterband_id", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "sequence_nbr", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "raster_id", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "band_width", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "band_height", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "band_types", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "block_width", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "block_height", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "block_origin_x", "", OpenFileGDB::FGFT_FLOAT64, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "block_origin_y", "", OpenFileGDB::FGFT_FLOAT64, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "eminx", "", OpenFileGDB::FGFT_FLOAT64, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "eminy", "", OpenFileGDB::FGFT_FLOAT64, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "emaxx", "", OpenFileGDB::FGFT_FLOAT64, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "emaxy", "", OpenFileGDB::FGFT_FLOAT64, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "srid", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD));

  for (int band = 1; band <= nBands; ++band) {
    std::vector<OGRField> fields(15, OpenFileGDB::FileGDBField::UNSET_FIELD);
    fields[1].Integer = band;
    fields[2].Integer = nRasterId;
    fields[3].Integer = nWidth;
    fields[4].Integer = nHeight;
    fields[5].Integer = nBandTypes;
    fields[6].Integer = nBlockWidth;
    fields[7].Integer = nBlockHeight;
    fields[8].Real = dfOriginX;
    fields[9].Real = dfOriginY;
    fields[10].Real = dfMinX;
    fields[11].Real = dfMinY;
    fields[12].Real = dfMaxX;
    fields[13].Real = dfMaxY;
    fields[14].Integer = nSRID;
    if (!table.CreateFeature(fields, nullptr)) return false;
  }
  return table.Sync();
}

bool CreateBlkTable(const std::string& filename) {
  OpenFileGDB::FileGDBTable table;
  if (!table.Create(filename.c_str(), 4, OpenFileGDB::FGTGT_NONE, false, false))
    return false;
  table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "OBJECTID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "rasterband_id", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "rrd_factor", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "row_nbr", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "col_nbr", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "block_data", "", OpenFileGDB::FGFT_BINARY, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "block_key", "", OpenFileGDB::FGFT_STRING, true, false, true, 255,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  return table.Sync();
}

bool AddBlkRow(OpenFileGDB::FileGDBTable& table, int band_id, int rrd_factor,
               int row_nbr, int col_nbr, const GByte* pabyData, int nDataLen,
               bool bPadKey = false) {
  std::vector<OGRField> fields(7, OpenFileGDB::FileGDBField::UNSET_FIELD);
  fields[1].Integer = band_id;
  fields[2].Integer = rrd_factor;
  fields[3].Integer = row_nbr;
  fields[4].Integer = col_nbr;
  if (pabyData && nDataLen >= 0) {
    fields[5].Binary.nCount = nDataLen;
    fields[5].Binary.paData = const_cast<GByte*>(pabyData);
  }
  std::string key;
  if (row_nbr >= 0 && col_nbr >= 0) {
    key = CPLSPrintf("0000%04X%02X%04X%04X", band_id, rrd_factor, row_nbr,
                     col_nbr);
  } else if (row_nbr < 0 && col_nbr >= 0) {
    key = CPLSPrintf("0000%04X%02X-%04X%04X", band_id, rrd_factor, -row_nbr,
                     col_nbr);
  } else if (row_nbr >= 0 && col_nbr < 0) {
    key = CPLSPrintf("0000%04X%02X%04X-%04X", band_id, rrd_factor, row_nbr,
                     -col_nbr);
  } else {
    key = CPLSPrintf("0000%04X%02X-%04X-%04X", band_id, rrd_factor, -row_nbr,
                     -col_nbr);
  }
  if (bPadKey) {
    key += "    ";
  }
  fields[6].String = const_cast<char*>(key.c_str());
  return table.CreateFeature(fields, nullptr);
}

std::unique_ptr<OGROpenFileGDBDataSource> OpenRasterDS(
    const std::string& gdb_dir, const std::string& layer_name = "ras",
    CSLConstList open_options = nullptr) {
  std::string name =
      CPLSPrintf("OpenFileGDB:\"%s\":%s", gdb_dir.c_str(), layer_name.c_str());
  GDALOpenInfo oOpenInfo(name.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY);
  oOpenInfo.papszOpenOptions = const_cast<char**>(open_options);
  auto ds = std::make_unique<OGROpenFileGDBDataSource>();
  bool bRetry = false;
  if (!ds->Open(&oOpenInfo, bRetry)) return nullptr;
  return ds;
}

// TODO: b/335317901 - Investigate and re-enable.
TEST_F(GdalOpenFileGdbRasterBandTest, OpenRasterValidationAndErrors) {
  const std::string gdb = std::string(kTestDir) + "/validation.gdb";
  ASSERT_TRUE(CreateBaselineGDB(gdb));
#if 0

  // 1. Missing fras_bnd table file (a00000009.gdbtable does not exist yet)
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterDS(gdb));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Cannot open table fras_bnd_ras"));
  }

  // 2. fras_bnd with wrong structure (missing fields)
  std::string bnd_file = gdb + "/a00000009.gdbtable";
  {
    OpenFileGDB::FileGDBTable bad_bnd;
    ASSERT_TRUE(bad_bnd.Create(bnd_file.c_str(), 4, OpenFileGDB::FGTGT_NONE,
                               false, false));
    bad_bnd.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "wrong_field", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    bad_bnd.Sync();
    bad_bnd.Close();

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterDS(gdb));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Wrong structure for fras_bnd_ras table"));
  }

  // 3. Empty fras_bnd table (0 records)
  {
    ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(8, 0, 0), 4, 4, 0,
                               4, 0, 0, 4, 4, 1, /*nBands=*/0));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterDS(gdb));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Cannot read record in fras_bnd_ras table"));
  }

  // 4. Record with raster_id = 2 ignored with warning
  {
    ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(8, 0, 0), 4, 4, 0,
                               4, 0, 0, 4, 4, 1, 1, /*nRasterId=*/2));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterDS(gdb));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Cannot read record in fras_bnd_ras table"));
  }

  // 5. Missing fras_blk table (fras_bnd valid, but fras_blk not created)
  {
    ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(8, 0, 0)));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterDS(gdb));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Cannot find table fras_blk_ras"));
  }

  // 6. Wrong structure for fras_blk table
  std::string blk_file = gdb + "/a0000000a.gdbtable";
  {
    OpenFileGDB::FileGDBTable bad_blk;
    ASSERT_TRUE(bad_blk.Create(blk_file.c_str(), 4, OpenFileGDB::FGTGT_NONE,
                               false, false));
    bad_blk.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "OBJECTID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    bad_blk.Sync();
    bad_blk.Close();

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterDS(gdb));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Wrong structure for fras_blk_ras table"));
  }

  // Now create valid blk table
  ASSERT_TRUE(CreateBlkTable(blk_file));

  // 7. Invalid dimensions (band_width <= 0)
  {
    ASSERT_TRUE(CreateBndTable(bnd_file, 0, 4));
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, OpenRasterDS(gdb));
  }

  // 8. Invalid block dimensions (block_width <= 0 or > 32768)
  {
    ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(8, 0, 0),
                               /*nBlockWidth=*/0, 4));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterDS(gdb));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid block_width"));

    ASSERT_TRUE(
        CreateBndTable(bnd_file, 4, 4, MakeBandTypes(8, 0, 0), 40000, 4));
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterDS(gdb));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid block_width"));

    ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(8, 0, 0), 4, 0));
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterDS(gdb));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid block_height"));

    ASSERT_TRUE(
        CreateBndTable(bnd_file, 4, 4, MakeBandTypes(8, 0, 0), 4, 40000));
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterDS(gdb));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid block_height"));
  }

  // 9. Unhandled nBitWidth / nBitType
  {
    ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(12, 0, 0)));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterDS(gdb));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Unhandled nBitWidth=12"));
  }

  // 10. Block dimensions overflow: nBlockWidth * nBlockHeight > INT_MAX /
  // nBitWidth
  {
    ASSERT_TRUE(
        CreateBndTable(bnd_file, 4, 4, MakeBandTypes(64, 0, 0), 30000, 30000));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterDS(gdb));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Too large block"));
  }

  // 11. Unhandled compression
  {
    ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(8, 0, 99)));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterDS(gdb));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Unhandled compression 99"));
  }

  // 12. Invalid bounds: !(dfMaxX > dfMinX && dfMaxY > dfMinY)
  {
    ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(8, 0, 0), 4, 4, 0,
                               4, 10, 0, 5, 4));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterDS(gdb));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("!(dfMaxX > dfMinX && dfMaxY > dfMinY)"));
  }

  // 13. nWidth == 1 || nHeight == 1 (warning about geotransform)
  {
    ASSERT_TRUE(CreateBndTable(bnd_file, 1, 4, MakeBandTypes(8, 0, 0), 1, 4, 0,
                               4, 0, 0, 4, 4));
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = OpenRasterDS(gdb);
    ASSERT_NE(nullptr, ds);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("cannot determine geotransform"));
    GDALGeoTransform gt;
    EXPECT_EQ(CE_Failure, ds->GetGeoTransform(gt));
  }

  // 14. Origin shift with overflow
  {
    // Inconsistent eminx and block_origin_x
    ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(8, 0, 0), 4, 4,
                               0.0, 4.0, 1000.0, 0.0, 1000.0 + 1e-8, 4.0));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterDS(gdb));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Inconsistent eminx="));

    // Inconsistent emaxy and block_origin_y
    ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(8, 0, 0), 4, 4,
                               0.0, 0.0, 0.0, 1000.0 - 1e-8, 4.0, 1000.0));
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterDS(gdb));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Inconsistent emaxy="));
  }

  // 15. Valid origin shifts (dfMinX != dfBlockOriginX, dfMaxY !=
  // dfBlockOriginY)
  {
    // Block origin (4, 8), bounds (0, 0, 8, 8), res = 8/3, blockGeoref = 32/3
    ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(8, 0, 0), 4, 4,
                               4.0, 8.0, 0.0, 0.0, 8.0, 8.0));
    auto ds = OpenRasterDS(gdb);
    ASSERT_NE(nullptr, ds);
    GDALGeoTransform gt;
    EXPECT_EQ(CE_None, ds->GetGeoTransform(gt));
  }

  // 16. Test setting individual fields in fras_bnd to null
  for (int iField = 1; iField <= 14; ++iField) {
    ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(8, 0, 0), 4, 4, 0,
                               4, 0, 0, 4, 4, 1, /*nBands=*/0));
    OpenFileGDB::FileGDBTable bnd;
    ASSERT_TRUE(bnd.Open(bnd_file.c_str(), true));
    std::vector<OGRField> fields(15, OpenFileGDB::FileGDBField::UNSET_FIELD);
    fields[1].Integer = 1;
    fields[2].Integer = 1;
    fields[3].Integer = 4;
    fields[4].Integer = 4;
    fields[5].Integer = MakeBandTypes(8, 0, 0);
    fields[6].Integer = 4;
    fields[7].Integer = 4;
    fields[8].Real = 0.0;
    fields[9].Real = 4.0;
    fields[10].Real = 0.0;
    fields[11].Real = 0.0;
    fields[12].Real = 4.0;
    fields[13].Real = 4.0;
    fields[14].Integer = 1;
    OGR_RawField_SetNull(&fields[iField]);

    bnd.CreateFeature(fields, nullptr);
    bnd.Sync();
    bnd.Close();

    WithQuietHandler quiet;
    OpenRasterDS(gdb);
  }
#endif
}

TEST_F(GdalOpenFileGdbRasterBandTest, SRSAndSubdatasetsAndMetadata) {
  const std::string gdb = std::string(kTestDir) + "/srs_meta.gdb";
  ASSERT_TRUE(CreateBaselineGDB(gdb));

  std::string bnd_file = gdb + "/a00000009.gdbtable";
  std::string blk_file = gdb + "/a0000000a.gdbtable";
  ASSERT_TRUE(CreateBlkTable(blk_file));

  // 1. Valid SRID = 1 (WGS84 from GDB_SpatialRefs)
  {
    ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(8, 0, 0), 4, 4, 0,
                               4, 0, 0, 4, 4, 1));
    auto ds = OpenRasterDS(gdb);
    ASSERT_NE(nullptr, ds);
    const auto* srs = ds->GetSpatialRef();
    ASSERT_NE(nullptr, srs);
    EXPECT_TRUE(srs->IsGeographic());
  }

  // 2. Missing record for SRID = 999
  {
    ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(8, 0, 0), 4, 4, 0,
                               4, 0, 0, 4, 4, 999));
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = OpenRasterDS(gdb);
    ASSERT_NE(nullptr, ds);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Cannot find record corresponding to SRID = 999"));
  }

  // 3. SRID = 0 (SRS parsed from XML definition)
  {
    ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(8, 0, 0), 4, 4, 0,
                               4, 0, 0, 4, 4, 0));
    auto ds = OpenRasterDS(gdb);
    ASSERT_NE(nullptr, ds);
    const auto* srs = ds->GetSpatialRef();
    ASSERT_NE(nullptr, srs);
    EXPECT_TRUE(srs->IsGeographic());
  }

  // 4. SRID = 0 with invalid / empty XML definition
  {
    const std::string bad_xml_gdb = std::string(kTestDir) + "/bad_xml.gdb";
    ASSERT_TRUE(CreateBaselineGDB(bad_xml_gdb, "ras", false, false,
                                  "<DERasterDataset", "<metadata/>"));
    ASSERT_TRUE(CreateBndTable(bad_xml_gdb + "/a00000009.gdbtable", 4, 4,
                               MakeBandTypes(8, 0, 0), 4, 4, 0, 4, 0, 0, 4, 4,
                               0));
    ASSERT_TRUE(CreateBlkTable(bad_xml_gdb + "/a0000000a.gdbtable"));
    WithQuietHandler quiet;
    CPLErrorReset();
    auto ds = OpenRasterDS(bad_xml_gdb);
    ASSERT_NE(nullptr, ds);
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot parse XML definition"));

    // Missing SRS in XML
    const std::string no_srs_xml_gdb =
        std::string(kTestDir) + "/no_srs_xml.gdb";
    ASSERT_TRUE(CreateBaselineGDB(no_srs_xml_gdb, "ras", false, false,
                                  "<DERasterDataset/>", "<metadata/>"));
    ASSERT_TRUE(CreateBndTable(no_srs_xml_gdb + "/a00000009.gdbtable", 4, 4,
                               MakeBandTypes(8, 0, 0), 4, 4, 0, 4, 0, 0, 4, 4,
                               0));
    ASSERT_TRUE(CreateBlkTable(no_srs_xml_gdb + "/a0000000a.gdbtable"));
    CPLErrorReset();
    ds = OpenRasterDS(no_srs_xml_gdb);
    ASSERT_NE(nullptr, ds);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Cannot get SRS from XML definition"));
  }

  // 5. GDB_SpatialRefs table missing or invalid
  {
    const std::string no_srs_gdb = std::string(kTestDir) + "/no_srs_table.gdb";
    ASSERT_TRUE(CreateBaselineGDB(no_srs_gdb));
    VSIUnlink((no_srs_gdb + "/a00000003.gdbtable").c_str());
    ASSERT_TRUE(CreateBndTable(no_srs_gdb + "/a00000009.gdbtable", 4, 4,
                               MakeBandTypes(8, 0, 0), 4, 4, 0, 4, 0, 0, 4, 4,
                               1));
    ASSERT_TRUE(CreateBlkTable(no_srs_gdb + "/a0000000a.gdbtable"));
    WithQuietHandler quiet;
    auto ds = OpenRasterDS(no_srs_gdb);
    ASSERT_NE(nullptr, ds);

    // GDB_SpatialRefs table missing SRTEXT column
    const std::string bad_srs_col_gdb =
        std::string(kTestDir) + "/bad_srs_col.gdb";
    ASSERT_TRUE(CreateBaselineGDB(bad_srs_col_gdb));
    VSIUnlink((bad_srs_col_gdb + "/a00000003.gdbtable").c_str());
    OpenFileGDB::FileGDBTable bad_srs;
    ASSERT_TRUE(
        bad_srs.Create((bad_srs_col_gdb + "/a00000003.gdbtable").c_str(), 4,
                       OpenFileGDB::FGTGT_NONE, false, false));
    bad_srs.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "dummy", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    bad_srs.Sync();
    bad_srs.Close();
    ASSERT_TRUE(CreateBndTable(bad_srs_col_gdb + "/a00000009.gdbtable", 4, 4,
                               MakeBandTypes(8, 0, 0), 4, 4, 0, 4, 0, 0, 4, 4,
                               1));
    ASSERT_TRUE(CreateBlkTable(bad_srs_col_gdb + "/a0000000a.gdbtable"));
    CPLErrorReset();
    ds = OpenRasterDS(bad_srs_col_gdb);
    ASSERT_NE(nullptr, ds);
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Could not find field SRTEXT"));
  }

  // 6. Subdatasets list and XML metadata
  {
    ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(8, 0, 0)));
    GDALOpenInfo oOpenInfo(gdb.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY);
    OGROpenFileGDBDataSource ds;
    bool bRetry = false;
    ASSERT_TRUE(ds.Open(&oOpenInfo, bRetry));
    const auto& subds = ds.GetSubdatasets();
    EXPECT_GE(subds.size(), 2);
    EXPECT_STREQ(subds.FetchNameValue("SUBDATASET_1_NAME"),
                 CPLSPrintf("OpenFileGDB:\"%s\":ras", gdb.c_str()));
    EXPECT_THAT(subds.FetchNameValue("SUBDATASET_1_DESC"),
                HasSubstr("Raster ras [4x4x1], 8 bits"));

    // Check xml:definition and xml:documentation on opened subdataset
    auto rds = OpenRasterDS(gdb);
    ASSERT_NE(nullptr, rds);
    char** papszDef = rds->GetMetadata("xml:definition");
    ASSERT_NE(nullptr, papszDef);
    EXPECT_THAT(papszDef[0], HasSubstr("<DERasterDataset>"));
    char** papszDoc = rds->GetMetadata("xml:documentation");
    ASSERT_NE(nullptr, papszDoc);
    EXPECT_THAT(papszDoc[0], HasSubstr("<rastinfo>"));
    EXPECT_STREQ(rds->GetMetadataItem("RASTER_DATASET"), "ras");
  }

  // 7. ReadAuxTable with PropertySet statistics
  {
    std::string aux_file = gdb + "/a0000000b.gdbtable";
    OpenFileGDB::FileGDBTable aux_table;
    ASSERT_TRUE(aux_table.Create(aux_file.c_str(), 4, OpenFileGDB::FGTGT_NONE,
                                 false, false));
    aux_table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "OBJECTID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    aux_table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "type", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD));
    aux_table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
        "object", "", OpenFileGDB::FGFT_BINARY, true, false, true, 0,
        OpenFileGDB::FileGDBField::UNSET_FIELD));

    // Construct PropertySet blob
    std::vector<GByte> blob;
    blob.push_back(0);  // padding

    // abyNeedle: KIND=BAND
    constexpr GByte abyNeedle[] = {'K', 0, 'I', 0,  'N', 0, 'D', 0,   0,
                                   0,   8, 0,   10, 0,   0, 0,   'B', 0,
                                   'A', 0, 'N', 0,  'D', 0, 0,   0};
    blob.insert(blob.end(), abyNeedle, abyNeedle + sizeof(abyNeedle));

    // key 1: type 0 (null)
    AppendUTF16String(blob, "K0");
    blob.push_back(0);
    blob.push_back(0);  // type 0

    // key 2: type 2 (short)
    AppendUTF16String(blob, "K2");
    blob.push_back(2);
    blob.push_back(0);  // type 2
    blob.push_back(0x12);
    blob.push_back(0x34);

    // key 3: type 3 (int)
    AppendUTF16String(blob, "K3");
    blob.push_back(3);
    blob.push_back(0);  // type 3
    blob.push_back(1);
    blob.push_back(2);
    blob.push_back(3);
    blob.push_back(4);

    // key 4: type 5 (double)
    AppendUTF16String(blob, "K5");
    blob.push_back(5);
    blob.push_back(0);  // type 5
    for (int i = 0; i < 8; ++i) blob.push_back(static_cast<GByte>(i));

    // key 5: type 8 (string STATISTICS_MINIMUM = 10.5)
    AppendUTF16String(blob, "STATISTICS_MINIMUM");
    blob.push_back(8);
    blob.push_back(0);  // type 8
    AppendUTF16String(blob, "10.5");

    // key 6: type 99 (unknown value type to break loop)
    AppendUTF16String(blob, "K99");
    blob.push_back(99);
    blob.push_back(0);

    std::vector<OGRField> fields(3, OpenFileGDB::FileGDBField::UNSET_FIELD);
    fields[1].Integer = 9;  // type = 9
    fields[2].Binary.nCount = static_cast<int>(blob.size());
    fields[2].Binary.paData = blob.data();
    ASSERT_TRUE(aux_table.CreateFeature(fields, nullptr));
    aux_table.Sync();
    aux_table.Close();

    auto ds = OpenRasterDS(gdb);
    ASSERT_NE(nullptr, ds);
    auto* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    EXPECT_STREQ(band->GetMetadataItem("STATISTICS_MINIMUM"), "10.5");

    // Helper to write a single row to aux_table and open raster DS
    auto test_aux_blob = [&](const std::vector<GByte>* p_blob,
                             bool is_null = false) {
      OpenFileGDB::FileGDBTable t;
      ASSERT_TRUE(
          t.Create(aux_file.c_str(), 4, OpenFileGDB::FGTGT_NONE, false, false));
      t.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "OBJECTID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
          OpenFileGDB::FileGDBField::UNSET_FIELD));
      t.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "type", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
          OpenFileGDB::FileGDBField::UNSET_FIELD));
      t.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "object", "", OpenFileGDB::FGFT_BINARY, true, false, true, 0,
          OpenFileGDB::FileGDBField::UNSET_FIELD));
      if (p_blob || is_null) {
        std::vector<OGRField> f(3, OpenFileGDB::FileGDBField::UNSET_FIELD);
        f[1].Integer = 9;
        if (is_null) {
          OGR_RawField_SetNull(&f[2]);
        } else {
          f[2].Binary.nCount = static_cast<int>(p_blob->size());
          f[2].Binary.paData = const_cast<GByte*>(p_blob->data());
        }
        ASSERT_TRUE(t.CreateFeature(f, nullptr));
      }
      t.Sync();
      t.Close();
      EXPECT_NE(nullptr, OpenRasterDS(gdb));
    };

    // Empty aux table (no type=9 row)
    test_aux_blob(nullptr, false);
    // Null object field
    test_aux_blob(nullptr, true);
    // 0-byte object field
    std::vector<GByte> empty_blob;
    test_aux_blob(&empty_blob, false);
    // No KIND=BAND needle
    std::vector<GByte> no_needle(40, 0);
    test_aux_blob(&no_needle, false);

    auto make_needle_blob = [&]() {
      return std::vector<GByte>(abyNeedle, abyNeedle + sizeof(abyNeedle));
    };
    // Truncated key length (< 4 bytes after needle)
    {
      auto b = make_needle_blob();
      b.push_back(0);
      test_aux_blob(&b);
    }
    // Key length <= 2
    {
      auto b = make_needle_blob();
      b.insert(b.end(), {2, 0, 0, 0});
      test_aux_blob(&b);
    }
    // Odd key length (nStrLength % 2 != 0)
    {
      auto b = make_needle_blob();
      b.insert(b.end(), {3, 0, 0, 0, 0, 0, 0});
      test_aux_blob(&b);
    }
    // Truncated value type (< 2 bytes after key)
    {
      auto b = make_needle_blob();
      AppendUTF16String(b, "K");
      b.push_back(0);
      test_aux_blob(&b);
    }
    // Truncated type 2 (short), type 3 (int), type 5 (double), type 8 (string)
    for (GByte vt : {2, 3, 5, 8}) {
      auto b = make_needle_blob();
      AppendUTF16String(b, "K");
      b.push_back(vt);
      b.push_back(0);
      test_aux_blob(&b);
    }

    // Wrong aux_table structure (missing "object" field)
    {
      OpenFileGDB::FileGDBTable bad_aux;
      ASSERT_TRUE(bad_aux.Create(aux_file.c_str(), 4, OpenFileGDB::FGTGT_NONE,
                                 false, false));
      bad_aux.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
          "OBJECTID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
          OpenFileGDB::FileGDBField::UNSET_FIELD));
      bad_aux.Sync();
      bad_aux.Close();
      EXPECT_NE(nullptr, OpenRasterDS(gdb));
    }
  }
}

TEST_F(GdalOpenFileGdbRasterBandTest, DataTypesNoDataAndMaskOptions) {
  const std::string gdb = std::string(kTestDir) + "/datatypes.gdb";
  ASSERT_TRUE(CreateBaselineGDB(gdb));

  std::string bnd_file = gdb + "/a00000009.gdbtable";
  std::string blk_file = gdb + "/a0000000a.gdbtable";
  ASSERT_TRUE(CreateBlkTable(blk_file));

  // Test data type mappings
  struct TypeTestCase {
    int bit_width;
    int bit_type;
    GDALDataType expected_dt;
  };
  TypeTestCase cases[] = {
      {4, 0, GDT_Byte},    {8, 0, GDT_Byte},     {8, 1, GDT_Int8},
      {16, 0, GDT_UInt16}, {16, 1, GDT_Int16},   {32, 0, GDT_UInt32},
      {32, 1, GDT_Int32},  {32, 2, GDT_Float32}, {64, 0, GDT_Float64},
  };

  for (const auto& tc : cases) {
    ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4,
                               MakeBandTypes(tc.bit_width, tc.bit_type, 0)));
    auto ds = OpenRasterDS(gdb);
    ASSERT_NE(nullptr, ds);
    auto* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    EXPECT_EQ(tc.expected_dt, band->GetRasterDataType());
    if (tc.bit_width < 8) {
      EXPECT_STREQ(band->GetMetadataItem("NBITS", "IMAGE_STRUCTURE"), "4");
    }
  }

  // Test NODATA_OR_MASK options
  // 1. AUTO mode: Float32 gets nodata, Byte gets mask band
  {
    ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(32, 2, 0)));
    auto ds = OpenRasterDS(gdb, "ras");
    ASSERT_NE(nullptr, ds);
    auto* band = ds->GetRasterBand(1);
    int bHasNoData = 0;
    band->GetNoDataValue(&bHasNoData);
    EXPECT_TRUE(bHasNoData);

    ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(8, 0, 0)));
    ds = OpenRasterDS(gdb, "ras");
    ASSERT_NE(nullptr, ds);
    band = ds->GetRasterBand(1);
    EXPECT_EQ(GMF_PER_DATASET, band->GetMaskFlags());
    EXPECT_NE(nullptr, band->GetMaskBand());
  }

  // 2. MASK mode: even Float32 gets mask band
  {
    ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(32, 2, 0)));
    const char* const options[] = {"NODATA_OR_MASK=MASK", nullptr};
    auto ds = OpenRasterDS(gdb, "ras", options);
    ASSERT_NE(nullptr, ds);
    auto* band = ds->GetRasterBand(1);
    EXPECT_EQ(GMF_PER_DATASET, band->GetMaskFlags());
  }

  // 3. NONE mode: no mask band, no nodata
  {
    ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(8, 0, 0)));
    const char* const options[] = {"NODATA_OR_MASK=NONE", nullptr};
    auto ds = OpenRasterDS(gdb, "ras", options);
    ASSERT_NE(nullptr, ds);
    auto* band = ds->GetRasterBand(1);
    int bHasNoData = 0;
    band->GetNoDataValue(&bHasNoData);
    EXPECT_FALSE(bHasNoData);
    EXPECT_EQ(GMF_ALL_VALID, band->GetMaskFlags());
  }

  // 4. Invalid explicit nodata values
  {
    ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(8, 0, 0)));
    const char* const bad_opt1[] = {"NODATA_OR_MASK=300", nullptr};
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterDS(gdb, "ras", bad_opt1));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Invalid nodata value 300 for Byte"));

    const char* const bad_opt2[] = {"NODATA_OR_MASK=1.5", nullptr};
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterDS(gdb, "ras", bad_opt2));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Invalid nodata value 1.5 for Byte"));

    ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(32, 2, 0)));
    const char* const bad_opt3[] = {"NODATA_OR_MASK=1e40", nullptr};
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenRasterDS(gdb, "ras", bad_opt3));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Invalid nodata value 1e+40 for Float32"));
  }

  // 5. Multi-band dataset with overviews
  {
    ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(8, 0, 0), 4, 4, 0,
                               4, 0, 0, 4, 4, 1, /*nBands=*/2));
    OpenFileGDB::FileGDBTable blk;
    ASSERT_TRUE(blk.Open(blk_file.c_str(), true));
    // Add row with padded block_key for overview detection (rrd_factor = 1)
    std::vector<GByte> dummy_data(16, 42);
    ASSERT_TRUE(
        AddBlkRow(blk, 1, 1, 0, 0, dummy_data.data(), 16, /*bPadKey=*/true));
    blk.Sync();
    blk.Close();

    auto ds = OpenRasterDS(gdb);
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(2, ds->GetRasterCount());
    EXPECT_STREQ(ds->GetMetadataItem("INTERLEAVE", "IMAGE_STRUCTURE"), "BAND");

    auto* band1 = ds->GetRasterBand(1);
    EXPECT_EQ(1, band1->GetOverviewCount());
    EXPECT_NE(nullptr, band1->GetOverview(0));
    EXPECT_EQ(nullptr, band1->GetOverview(-1));
    EXPECT_EQ(nullptr, band1->GetOverview(2));
    EXPECT_NE(nullptr, band1->GetOverview(0)->GetMaskBand());

    // Also open overview dataset with explicit nodata (covers overview nodata &
    // missing block nodata)
    const char* const nd_opts[] = {"NODATA_OR_MASK=5", nullptr};
    auto ds_nd = OpenRasterDS(gdb, "ras", nd_opts);
    ASSERT_NE(nullptr, ds_nd);
    int has_nd = 0;
    EXPECT_DOUBLE_EQ(
        5.0, ds_nd->GetRasterBand(1)->GetOverview(0)->GetNoDataValue(&has_nd));
    EXPECT_TRUE(has_nd);
    GByte missing_px = 0;
    EXPECT_EQ(CE_None, ds_nd->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 1, 1, &missing_px, 1, 1, GDT_Byte, 0,
                           0, nullptr));
    EXPECT_EQ(5, missing_px);

    // Ignore overviews if dfBlockOriginX != dfMinX
    ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(8, 0, 0), 4, 4,
                               2.0, 4.0, 0.0, 0.0, 4.0, 4.0, 1, 2));
    ds = OpenRasterDS(gdb);
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(0, ds->GetRasterBand(1)->GetOverviewCount());
  }

  // 6. SetNoDataFromMask across data types
  {
    // Test uint8_t, int8_t, uint16_t, int16_t, uint32_t, int32_t, float, double
    struct NoDataTest {
      int bit_width;
      int bit_type;
      const char* nodata_str;
      double expected_nodata;
    };
    NoDataTest nd_tests[] = {
        {8, 0, "5", 5.0},          {8, 1, "-5", -5.0},
        {16, 0, "258", 258.0},     {16, 1, "-258", -258.0},
        {32, 0, "70000", 70000.0}, {32, 1, "-70000", -70000.0},
        {32, 2, "99.5", 99.5},     {64, 0, "12345.6", 12345.6},
    };

    for (const auto& ndt : nd_tests) {
      ASSERT_TRUE(CreateBndTable(
          bnd_file, 4, 4, MakeBandTypes(ndt.bit_width, ndt.bit_type, 0)));
      // Reset blk table
      ASSERT_TRUE(CreateBlkTable(blk_file));
      OpenFileGDB::FileGDBTable blk;
      ASSERT_TRUE(blk.Open(blk_file.c_str(), true));

      int bytes_per_pixel = ndt.bit_width / 8;
      int img_bytes = 16 * bytes_per_pixel;
      int mask_bytes = 2;  // (16 + 7) / 8
      std::vector<GByte> block_payload(img_bytes + mask_bytes, 0);

      // Populate pixel 1 with value equal to nodata (triggers substitution
      // warning!)
      if (ndt.bit_width == 8) {
        block_payload[0] = 10;
        block_payload[1] =
            static_cast<GByte>(static_cast<int>(ndt.expected_nodata));
      } else if (ndt.bit_width == 16) {
        // MSB order in raw block!
        uint16_t val =
            static_cast<uint16_t>(static_cast<int>(ndt.expected_nodata));
        block_payload[2] = static_cast<GByte>((val >> 8) & 0xFF);
        block_payload[3] = static_cast<GByte>(val & 0xFF);
      }

      // Mask: pixel 0 masked out (0), pixel 1 valid (1), remaining valid
      block_payload[img_bytes] =
          0x7F;  // 01111111b -> pixel 0 is 0, pixel 1..7 are 1
      block_payload[img_bytes + 1] = 0xFF;

      ASSERT_TRUE(AddBlkRow(blk, 1, 0, 0, 0, block_payload.data(),
                            static_cast<int>(block_payload.size())));
      blk.Sync();
      blk.Close();

      std::string opt_str = std::string("NODATA_OR_MASK=") + ndt.nodata_str;
      const char* const options[] = {opt_str.c_str(), nullptr};
      WithQuietHandler quiet;
      auto ds = OpenRasterDS(gdb, "ras", options);
      ASSERT_NE(nullptr, ds);
      std::vector<double> out_pixels(16, 0.0);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                             GF_Read, 0, 0, 4, 4, out_pixels.data(), 4, 4,
                             GDT_Float64, 0, 0, nullptr));
      EXPECT_DOUBLE_EQ(ndt.expected_nodata,
                       out_pixels[0]);  // masked out pixel got nodata
    }
  }
}

TEST_F(GdalOpenFileGdbRasterBandTest, ReadBlockCompressionsAndCache) {
  const std::string gdb = std::string(kTestDir) + "/compressions.gdb";
  ASSERT_TRUE(CreateBaselineGDB(gdb));

  std::string bnd_file = gdb + "/a00000009.gdbtable";
  std::string blk_file = gdb + "/a0000000a.gdbtable";

  // 1. Compression::NONE error and corner cases
  {
    ASSERT_TRUE(CreateBndTable(bnd_file, 32, 4, MakeBandTypes(8, 0, 0)));
    ASSERT_TRUE(CreateBlkTable(blk_file));
    OpenFileGDB::FileGDBTable blk;
    ASSERT_TRUE(blk.Open(blk_file.c_str(), true));

    // Block (0, 0) missing -> FillMissingBlock
    // Block (1, 0) NULL data -> FillMissingBlock
    ASSERT_TRUE(AddBlkRow(blk, 1, 0, 0, 1, nullptr, -1));
    // Block (2, 0) 0-byte data -> error
    GByte empty_b = 0;
    ASSERT_TRUE(AddBlkRow(blk, 1, 0, 0, 2, &empty_b, 0));
    // Block (3, 0) wrong size -> error
    std::vector<GByte> wrong_size(5, 1);
    ASSERT_TRUE(AddBlkRow(blk, 1, 0, 0, 3, wrong_size.data(), 5));

    blk.Sync();
    blk.Close();

    auto ds = OpenRasterDS(gdb);
    ASSERT_NE(nullptr, ds);
    auto* band = ds->GetRasterBand(1);
    GByte pixel = 0;

    // Missing block (0, 0)
    EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 1, 1, &pixel, 1, 1,
                                      GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(0, pixel);

    // NULL data block (1, 0)
    EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 4, 0, 1, 1, &pixel, 1, 1,
                                      GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(0, pixel);

    // 0-byte data block (2, 0)
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, band->RasterIO(GF_Read, 8, 0, 1, 1, &pixel, 1, 1,
                                           GDT_Byte, 0, 0, nullptr));
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Image block is empty"));
    }

    // Wrong size data block (3, 0)
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, band->RasterIO(GF_Read, 12, 0, 1, 1, &pixel, 1, 1,
                                           GDT_Byte, 0, 0, nullptr));
      EXPECT_THAT(CPLGetLastErrorMsg(),
                  HasSubstr("Not expected number of input bytes"));
    }
  }

  // 2. Cache lock testing: read mask first, then main band, and vice versa
  {
    ASSERT_TRUE(CreateBndTable(bnd_file, 32, 4, MakeBandTypes(8, 0, 0)));
    ASSERT_TRUE(CreateBlkTable(blk_file));
    OpenFileGDB::FileGDBTable blk;
    ASSERT_TRUE(blk.Open(blk_file.c_str(), true));

    std::vector<GByte> payload(18, 0);  // 16 pixels + 2 mask bytes
    for (int i = 0; i < 16; ++i) payload[i] = static_cast<GByte>(i + 1);
    payload[16] = 0xAA;  // alternating mask bits
    payload[17] = 0x55;
    ASSERT_TRUE(AddBlkRow(blk, 1, 0, 0, 0, payload.data(), 18));
    ASSERT_TRUE(AddBlkRow(blk, 1, 0, 0, 1, payload.data(), 18));
    blk.Sync();
    blk.Close();

    auto ds = OpenRasterDS(gdb);
    ASSERT_NE(nullptr, ds);
    auto* band = ds->GetRasterBand(1);
    auto* mask = band->GetMaskBand();
    ASSERT_NE(nullptr, mask);

    // Block (0, 0): read main first, then mask
    GByte p_main = 0, p_mask = 0;
    EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 1, 1, &p_main, 1, 1,
                                      GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(1, p_main);
    EXPECT_EQ(CE_None, mask->RasterIO(GF_Read, 0, 0, 1, 1, &p_mask, 1, 1,
                                      GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(255, p_mask);

    // Block (1, 0): read mask first, then main
    EXPECT_EQ(CE_None, mask->RasterIO(GF_Read, 4, 0, 1, 1, &p_mask, 1, 1,
                                      GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(255, p_mask);
    EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 4, 0, 1, 1, &p_main, 1, 1,
                                      GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(1, p_main);
  }

  // 3. Sub-byte 4-bit unpacking (ExpandSubByteData)
  {
    ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(4, 0, 0)));
    ASSERT_TRUE(CreateBlkTable(blk_file));
    OpenFileGDB::FileGDBTable blk;
    ASSERT_TRUE(blk.Open(blk_file.c_str(), true));

    // 16 4-bit pixels = 8 bytes, plus 2 bytes binary mask = 10 bytes
    std::vector<GByte> payload(10, 0);
    payload[0] = 0xAB;  // pixel 0 = 10, pixel 1 = 11
    payload[8] = 0xC0;  // pixel 0 & 1 valid
    ASSERT_TRUE(AddBlkRow(blk, 1, 0, 0, 0, payload.data(), 10));
    blk.Sync();
    blk.Close();

    auto ds = OpenRasterDS(gdb);
    ASSERT_NE(nullptr, ds);
    GByte pixels[2] = {0, 0};
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 1, pixels, 2, 1,
                                             GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(10, pixels[0]);
    EXPECT_EQ(11, pixels[1]);
  }

  // 4. Negative block coordinates in block_key
  {
    // Origin (4, 4) with bounds (0, 0, 4, 4), res=1.33, blockGeoref=5.33 ->
    // shift -1
    ASSERT_TRUE(CreateBndTable(bnd_file, 8, 8, MakeBandTypes(8, 0, 0), 4, 4,
                               4.0, 0.0, 0.0, 0.0, 4.0, 4.0));
    ASSERT_TRUE(CreateBlkTable(blk_file));
    OpenFileGDB::FileGDBTable blk;
    ASSERT_TRUE(blk.Open(blk_file.c_str(), true));

    std::vector<GByte> payload(16, 77);
    // col -1, row 0 -> 00000001000000-0001
    ASSERT_TRUE(AddBlkRow(blk, 1, 0, 0, -1, payload.data(), 16));
    // col -1, row -1 -> 0000000100-0001-0001
    ASSERT_TRUE(AddBlkRow(blk, 1, 0, -1, -1, payload.data(), 16));
    // col 0, row -1 -> 0000000100-00010000
    ASSERT_TRUE(AddBlkRow(blk, 1, 0, -1, 0, payload.data(), 16));
    blk.Sync();
    blk.Close();

    auto ds = OpenRasterDS(gdb);
    ASSERT_NE(nullptr, ds);
    GByte p = 0;
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &p, 1, 1,
                                             GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 4, 0, 1, 1, &p, 1, 1,
                                             GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 4, 1, 1, &p, 1, 1,
                                             GDT_Byte, 0, 0, nullptr));
  }

  // 5. Compression::LZ77
  {
    ASSERT_TRUE(
        CreateBndTable(bnd_file, 32, 4, MakeBandTypes(8, 0, 4)));  // 4 = LZ77
    ASSERT_TRUE(CreateBlkTable(blk_file));
    OpenFileGDB::FileGDBTable blk;
    ASSERT_TRUE(blk.Open(blk_file.c_str(), true));

    // Valid deflate block (16 pixels + 2 mask bytes = 18 bytes)
    std::vector<GByte> uncompressed(18, 55);
    size_t comp_size = 0;
    void* comp_data = CPLZLibDeflate(uncompressed.data(), uncompressed.size(),
                                     6, nullptr, 0, &comp_size);
    ASSERT_NE(nullptr, comp_data);
    ASSERT_TRUE(AddBlkRow(blk, 1, 0, 0, 0, static_cast<const GByte*>(comp_data),
                          static_cast<int>(comp_size)));
    CPLFree(comp_data);

    // Corrupt deflate block
    std::vector<GByte> corrupt(10, 0xFF);
    ASSERT_TRUE(AddBlkRow(blk, 1, 0, 0, 1, corrupt.data(), 10));
    blk.Sync();
    blk.Close();

    auto ds = OpenRasterDS(gdb);
    ASSERT_NE(nullptr, ds);
    EXPECT_STREQ(ds->GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"),
                 "DEFLATE");
    GByte p = 0;
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &p, 1, 1,
                                             GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(55, p);

    // Corrupt deflate
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure,
                ds->GetRasterBand(1)->RasterIO(GF_Read, 4, 0, 1, 1, &p, 1, 1,
                                               GDT_Byte, 0, 0, nullptr));
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("CPLZLibInflate() failed"));
    }

    // LZ77 16-bit (covers GDALSwapWordsEx in LZ77)
    {
      ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(16, 0, 4)));
      ASSERT_TRUE(CreateBlkTable(blk_file));
      OpenFileGDB::FileGDBTable blk16;
      ASSERT_TRUE(blk16.Open(blk_file.c_str(), true));
      std::vector<GByte> uncomp16(32, 1);
      size_t csize = 0;
      void* cdata = CPLZLibDeflate(uncomp16.data(), uncomp16.size(), 6, nullptr,
                                   0, &csize);
      ASSERT_TRUE(AddBlkRow(blk16, 1, 0, 0, 0, static_cast<const GByte*>(cdata),
                            static_cast<int>(csize)));
      CPLFree(cdata);
      blk16.Sync();
      blk16.Close();
      auto ds16 = OpenRasterDS(gdb);
      ASSERT_NE(nullptr, ds16);
      uint16_t p16 = 0;
      EXPECT_EQ(CE_None,
                ds16->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &p16, 1,
                                                 1, GDT_UInt16, 0, 0, nullptr));
      EXPECT_EQ(0x0101, p16);
    }

    // LZ77 4-bit and mask-first read (covers ExpandSubByteData &
    // poBlock->DropLock in LZ77)
    {
      ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(4, 0, 4)));
      ASSERT_TRUE(CreateBlkTable(blk_file));
      OpenFileGDB::FileGDBTable blk4;
      ASSERT_TRUE(blk4.Open(blk_file.c_str(), true));
      std::vector<GByte> uncomp4(10, 0xFF);
      size_t csize = 0;
      void* cdata =
          CPLZLibDeflate(uncomp4.data(), uncomp4.size(), 6, nullptr, 0, &csize);
      ASSERT_TRUE(AddBlkRow(blk4, 1, 0, 0, 0, static_cast<const GByte*>(cdata),
                            static_cast<int>(csize)));
      CPLFree(cdata);
      blk4.Sync();
      blk4.Close();
      auto ds4 = OpenRasterDS(gdb);
      ASSERT_NE(nullptr, ds4);
      GByte pm = 0;
      EXPECT_EQ(CE_None,
                ds4->GetRasterBand(1)->GetMaskBand()->RasterIO(
                    GF_Read, 0, 0, 1, 1, &pm, 1, 1, GDT_Byte, 0, 0, nullptr));
    }
  }

  // 6. Compression::JPEG
  {
    ASSERT_TRUE(
        CreateBndTable(bnd_file, 32, 4, MakeBandTypes(8, 0, 8)));  // 8 = JPEG
    ASSERT_TRUE(CreateBlkTable(blk_file));

    // Create a 4x4 JPEG image in /vsimem/
    std::vector<GByte> jpeg_bytes;
    {
      const std::string tmp_jpg = std::string(kTestDir) + "/tile.jpg";
      auto* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
      ASSERT_NE(nullptr, mem_drv);
      auto* mem_ds = mem_drv->Create("", 4, 4, 1, GDT_Byte, nullptr);
      ASSERT_NE(nullptr, mem_ds);
      std::vector<GByte> tile_pixels(16, 128);
      EXPECT_EQ(CE_None, mem_ds->GetRasterBand(1)->RasterIO(
                             GF_Write, 0, 0, 4, 4, tile_pixels.data(), 4, 4,
                             GDT_Byte, 0, 0, nullptr));
      auto* jpeg_drv = GetGDALDriverManager()->GetDriverByName("JPEG");
      ASSERT_NE(nullptr, jpeg_drv);
      auto* tmp_ds = jpeg_drv->CreateCopy(tmp_jpg.c_str(), mem_ds, FALSE,
                                          nullptr, nullptr, nullptr);
      ASSERT_NE(nullptr, tmp_ds);
      GDALClose(tmp_ds);
      GDALClose(mem_ds);

      vsi_l_offset nBlobLen = 0;
      GByte* pBlob = VSIGetMemFileBuffer(tmp_jpg.c_str(), &nBlobLen, true);
      ASSERT_NE(nullptr, pBlob);
      jpeg_bytes.assign(pBlob, pBlob + nBlobLen);
      CPLFree(pBlob);
    }

    OpenFileGDB::FileGDBTable blk;
    ASSERT_TRUE(blk.Open(blk_file.c_str(), true));

    // Block (0, 0): prefix 0x01 + jpeg_bytes
    std::vector<GByte> b0(1 + jpeg_bytes.size());
    b0[0] = 1;
    memcpy(b0.data() + 1, jpeg_bytes.data(), jpeg_bytes.size());
    ASSERT_TRUE(
        AddBlkRow(blk, 1, 0, 0, 0, b0.data(), static_cast<int>(b0.size())));

    // Block (1, 0): prefix 0xFE + 4-byte size + jpeg_bytes + zlib-compressed
    // 2-byte mask
    std::vector<GByte> mask(2, 0xFF);
    size_t mask_comp_size = 0;
    void* mask_comp = CPLZLibDeflate(mask.data(), mask.size(), 6, nullptr, 0,
                                     &mask_comp_size);
    ASSERT_NE(nullptr, mask_comp);
    std::vector<GByte> b1;
    b1.push_back(0xFE);
    uint32_t jpeg_sz = static_cast<uint32_t>(jpeg_bytes.size());
    b1.push_back(static_cast<GByte>(jpeg_sz & 0xFF));
    b1.push_back(static_cast<GByte>((jpeg_sz >> 8) & 0xFF));
    b1.push_back(static_cast<GByte>((jpeg_sz >> 16) & 0xFF));
    b1.push_back(static_cast<GByte>((jpeg_sz >> 24) & 0xFF));
    b1.insert(b1.end(), jpeg_bytes.begin(), jpeg_bytes.end());
    b1.insert(b1.end(), static_cast<const GByte*>(mask_comp),
              static_cast<const GByte*>(mask_comp) + mask_comp_size);
    CPLFree(mask_comp);
    ASSERT_TRUE(
        AddBlkRow(blk, 1, 0, 0, 1, b1.data(), static_cast<int>(b1.size())));

    // Block (2, 0): prefix 0xFE with corrupt mask
    std::vector<GByte> b2 = b1;
    b2.back() ^= 0xFF;  // corrupt last byte
    ASSERT_TRUE(
        AddBlkRow(blk, 1, 0, 0, 2, b2.data(), static_cast<int>(b2.size())));

    // Block (3, 0): prefix 0xFE with invalid nJPEGSize
    std::vector<GByte> b3 = b1;
    b3[1] = 0xFF;
    b3[2] = 0xFF;
    b3[3] = 0xFF;
    b3[4] = 0x7F;  // massive size
    ASSERT_TRUE(
        AddBlkRow(blk, 1, 0, 0, 3, b3.data(), static_cast<int>(b3.size())));

    // Block (4, 0): < 5 bytes
    std::vector<GByte> b4 = {1, 2, 3};
    ASSERT_TRUE(AddBlkRow(blk, 1, 0, 0, 4, b4.data(), 3));

    // Block (5, 0): invalid prefix 0x02
    std::vector<GByte> b5 = {2, 0, 0, 0, 0, 0};
    ASSERT_TRUE(AddBlkRow(blk, 1, 0, 0, 5, b5.data(), 6));

    // Block (6, 0): prefix 0x01 + corrupt JPEG
    std::vector<GByte> b6 = {1, 0xFF, 0xD8, 0, 0, 0};
    ASSERT_TRUE(AddBlkRow(blk, 1, 0, 0, 6, b6.data(), 6));

    blk.Sync();
    blk.Close();

    auto ds = OpenRasterDS(gdb);
    ASSERT_NE(nullptr, ds);
    EXPECT_STREQ(ds->GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"), "JPEG");
    auto* band = ds->GetRasterBand(1);
    GByte p = 0;

    // Block (0, 0)
    EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 1, 1, &p, 1, 1, GDT_Byte,
                                      0, 0, nullptr));

    // Block (1, 0)
    EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 4, 0, 1, 1, &p, 1, 1, GDT_Byte,
                                      0, 0, nullptr));

    // Block (2, 0) corrupt mask
    {
      WithQuietHandler quiet;
      EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 8, 0, 1, 1, &p, 1, 1, GDT_Byte,
                                        0, 0, nullptr));
    }

    // Block (3, 0) invalid nJPEGSize
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, band->RasterIO(GF_Read, 12, 0, 1, 1, &p, 1, 1,
                                           GDT_Byte, 0, 0, nullptr));
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid nJPEGSize"));
    }

    // Block (4, 0) < 5 bytes
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, band->RasterIO(GF_Read, 16, 0, 1, 1, &p, 1, 1,
                                           GDT_Byte, 0, 0, nullptr));
      EXPECT_THAT(CPLGetLastErrorMsg(),
                  HasSubstr("Not expected number of input bytes"));
    }

    // Block (5, 0) invalid prefix
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, band->RasterIO(GF_Read, 20, 0, 1, 1, &p, 1, 1,
                                           GDT_Byte, 0, 0, nullptr));
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid JPEG blob"));
    }

    // Block (6, 0) corrupt JPEG
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, band->RasterIO(GF_Read, 24, 0, 1, 1, &p, 1, 1,
                                           GDT_Byte, 0, 0, nullptr));
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot open JPEG blob"));
    }

    // Test GuessJPEGQuality branches when block (0, 0) has 0xFE (valid &
    // invalid size) or 0x02
    for (const auto* vec : {&b1, &b3, &b5}) {
      ASSERT_TRUE(CreateBlkTable(blk_file));
      OpenFileGDB::FileGDBTable blk_q;
      ASSERT_TRUE(blk_q.Open(blk_file.c_str(), true));
      ASSERT_TRUE(AddBlkRow(blk_q, 1, 0, 0, 0, vec->data(),
                            static_cast<int>(vec->size())));
      blk_q.Sync();
      blk_q.Close();
      EXPECT_NE(nullptr, OpenRasterDS(gdb));
    }

    // Test JPEG block with mismatched dimensions (8x8 block vs 4x4 JPEG)
    {
      ASSERT_TRUE(CreateBndTable(bnd_file, 8, 8, MakeBandTypes(8, 0, 8),
                                 /*nBlockWidth=*/8, /*nBlockHeight=*/8));
      ASSERT_TRUE(CreateBlkTable(blk_file));
      OpenFileGDB::FileGDBTable blk_m;
      ASSERT_TRUE(blk_m.Open(blk_file.c_str(), true));
      ASSERT_TRUE(
          AddBlkRow(blk_m, 1, 0, 0, 0, b0.data(), static_cast<int>(b0.size())));
      blk_m.Sync();
      blk_m.Close();
      auto ds_m = OpenRasterDS(gdb);
      ASSERT_NE(nullptr, ds_m);
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure,
                ds_m->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &p, 1, 1,
                                                 GDT_Byte, 0, 0, nullptr));
      EXPECT_THAT(CPLGetLastErrorMsg(),
                  HasSubstr("Inconsistent characteristics of JPEG blob"));
    }
  }

  // 7. Compression::JPEG2000
  {
    ASSERT_TRUE(CreateBndTable(bnd_file, 32, 4,
                               MakeBandTypes(8, 0, 12)));  // 12 = JPEG2000
    ASSERT_TRUE(CreateBlkTable(blk_file));
    OpenFileGDB::FileGDBTable blk;
    ASSERT_TRUE(blk.Open(blk_file.c_str(), true));

    // Test < 5 bytes
    std::vector<GByte> jp2_b0 = {0, 1, 2};
    ASSERT_TRUE(AddBlkRow(blk, 1, 0, 0, 0, jp2_b0.data(), 3));

    // Test invalid prefix 0x02
    std::vector<GByte> jp2_b1 = {2, 0, 0, 0, 0, 0};
    ASSERT_TRUE(AddBlkRow(blk, 1, 0, 0, 1, jp2_b1.data(), 6));

    // Test 0xFF prefix with invalid nJPEGSize
    std::vector<GByte> jp2_b2 = {0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0, 0};
    ASSERT_TRUE(AddBlkRow(blk, 1, 0, 0, 2, jp2_b2.data(), 7));

    // Test 0xFF prefix with valid compressed mask + corrupt JP2 blob
    std::vector<GByte> mask(2, 0xFF);
    size_t mask_comp_size = 0;
    void* mask_comp = CPLZLibDeflate(mask.data(), mask.size(), 6, nullptr, 0,
                                     &mask_comp_size);
    std::vector<GByte> jp2_b3 = {0xFF, 1, 0, 0, 0, 0xAA};
    jp2_b3.insert(jp2_b3.end(), static_cast<const GByte*>(mask_comp),
                  static_cast<const GByte*>(mask_comp) + mask_comp_size);
    CPLFree(mask_comp);
    ASSERT_TRUE(AddBlkRow(blk, 1, 0, 0, 3, jp2_b3.data(),
                          static_cast<int>(jp2_b3.size())));

    blk.Sync();
    blk.Close();

    auto ds = OpenRasterDS(gdb);
    ASSERT_NE(nullptr, ds);
    EXPECT_STREQ(ds->GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"),
                 "JPEG2000");

    auto* band = ds->GetRasterBand(1);
    GByte p = 0;
    WithQuietHandler quiet;

    // Block (0, 0)
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, band->RasterIO(GF_Read, 0, 0, 1, 1, &p, 1, 1,
                                         GDT_Byte, 0, 0, nullptr));

    // Block (1, 0)
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, band->RasterIO(GF_Read, 4, 0, 1, 1, &p, 1, 1,
                                         GDT_Byte, 0, 0, nullptr));

    // Block (2, 0)
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, band->RasterIO(GF_Read, 8, 0, 1, 1, &p, 1, 1,
                                         GDT_Byte, 0, 0, nullptr));

    // Block (3, 0)
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, band->RasterIO(GF_Read, 12, 0, 1, 1, &p, 1, 1,
                                         GDT_Byte, 0, 0, nullptr));
  }
}

TEST_F(GdalOpenFileGdbRasterBandTest, RasterAttributeTable) {
  const std::string gdb = std::string(kTestDir) + "/vat.gdb";
  ASSERT_TRUE(CreateBaselineGDB(gdb));

  std::string bnd_file = gdb + "/a00000009.gdbtable";
  std::string blk_file = gdb + "/a0000000a.gdbtable";
  std::string vat_file = gdb + "/a0000000c.gdbtable";

  ASSERT_TRUE(CreateBndTable(bnd_file, 4, 4, MakeBandTypes(8, 0, 0)));
  ASSERT_TRUE(CreateBlkTable(blk_file));

  // Create VAT_ras table
  OpenFileGDB::FileGDBTable vat_table;
  ASSERT_TRUE(vat_table.Create(vat_file.c_str(), 4, OpenFileGDB::FGTGT_NONE,
                               false, false));
  vat_table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "OBJECTID", "", OpenFileGDB::FGFT_OBJECTID, false, true, false, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  vat_table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "Value", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  vat_table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "Count", "", OpenFileGDB::FGFT_INT32, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  vat_table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "Name", "", OpenFileGDB::FGFT_STRING, true, false, true, 255,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  vat_table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "Elevation", "", OpenFileGDB::FGFT_FLOAT64, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  vat_table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "Active", "", OpenFileGDB::FGFT_INT16, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  vat_table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "ObsDate", "", OpenFileGDB::FGFT_DATETIME, true, false, true, 0,
      OpenFileGDB::FileGDBField::UNSET_FIELD));
  vat_table.CreateField(std::make_unique<OpenFileGDB::FileGDBField>(
      "WktGeom", "", OpenFileGDB::FGFT_STRING, true, false, true, 255,
      OpenFileGDB::FileGDBField::UNSET_FIELD));

  // Add 2 rows
  {
    std::vector<OGRField> r1(8, OpenFileGDB::FileGDBField::UNSET_FIELD);
    r1[1].Integer = 10;
    r1[2].Integer = 500;
    r1[3].String = const_cast<char*>("Forest");
    r1[4].Real = 123.5;
    r1[5].Integer = 1;  // Active = true
    r1[6].Date.Year = 2023;
    r1[6].Date.Month = 5;
    r1[6].Date.Day = 15;
    r1[6].Date.Hour = 12;
    r1[6].Date.Minute = 30;
    r1[6].Date.Second = 0;
    r1[6].Date.TZFlag = 100;
    r1[7].String = const_cast<char*>("POINT (10 20)");
    ASSERT_TRUE(vat_table.CreateFeature(r1, nullptr));

    std::vector<OGRField> r2(8, OpenFileGDB::FileGDBField::UNSET_FIELD);
    r2[1].Integer = 20;
    r2[2].Integer = 1000;
    r2[3].String = const_cast<char*>("Water");
    r2[4].Real = 45.0;
    r2[5].Integer = 0;  // Active = false
    r2[6].Date.Year = 2023;
    r2[6].Date.Month = 5;
    r2[6].Date.Day = 16;
    r2[6].Date.Hour = 14;
    r2[6].Date.Minute = 0;
    r2[6].Date.Second = 0;
    r2[6].Date.TZFlag = 0;
    r2[7].String = const_cast<char*>("POINT (30 40)");
    ASSERT_TRUE(vat_table.CreateFeature(r2, nullptr));
  }
  vat_table.Sync();
  vat_table.Close();

  auto ds = OpenRasterDS(gdb);
  ASSERT_NE(nullptr, ds);
  auto* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  // Mask band RAT returns null
  auto* mask_band = band->GetMaskBand();
  if (mask_band && mask_band != band) {
    EXPECT_EQ(nullptr, mask_band->GetDefaultRAT());
  }

  // GetDefaultRAT() twice (tests cached m_poRAT)
  auto* rat = band->GetDefaultRAT();
  ASSERT_NE(nullptr, rat);
  EXPECT_EQ(rat, band->GetDefaultRAT());

  // Test RAT methods
  EXPECT_EQ(7, rat->GetColumnCount());
  EXPECT_EQ(2, rat->GetRowCount());

  EXPECT_STREQ("Value", rat->GetNameOfCol(0));
  EXPECT_EQ(nullptr, rat->GetNameOfCol(-1));
  EXPECT_EQ(nullptr, rat->GetNameOfCol(100));

  EXPECT_EQ(GFU_MinMax, rat->GetUsageOfCol(0));
  EXPECT_EQ(GFU_PixelCount, rat->GetUsageOfCol(1));
  EXPECT_EQ(GFU_Generic, rat->GetUsageOfCol(2));

  EXPECT_EQ(0, rat->GetColOfUsage(GFU_MinMax));
  EXPECT_EQ(1, rat->GetColOfUsage(GFU_PixelCount));
  EXPECT_EQ(-1, rat->GetColOfUsage(GFU_Red));

  EXPECT_EQ(GFT_Integer, rat->GetTypeOfCol(0));
  EXPECT_EQ(GFT_Real, rat->GetTypeOfCol(3));
  EXPECT_EQ(GFT_String, rat->GetTypeOfCol(2));
  EXPECT_EQ(GFT_DateTime, rat->GetTypeOfCol(5));
  EXPECT_EQ(GFT_Integer, rat->GetTypeOfCol(-1));

  // Get values
  EXPECT_STREQ("Forest", rat->GetValueAsString(0, 2));
  EXPECT_STREQ("", rat->GetValueAsString(99, 2));
  EXPECT_STREQ("", rat->GetValueAsString(0, 99));

  EXPECT_EQ(10, rat->GetValueAsInt(0, 0));
  EXPECT_EQ(0, rat->GetValueAsInt(99, 0));
  EXPECT_EQ(0, rat->GetValueAsInt(0, 99));

  EXPECT_DOUBLE_EQ(123.5, rat->GetValueAsDouble(0, 3));
  EXPECT_DOUBLE_EQ(0.0, rat->GetValueAsDouble(99, 3));
  EXPECT_DOUBLE_EQ(0.0, rat->GetValueAsDouble(0, 99));

  EXPECT_TRUE(rat->GetValueAsBoolean(0, 4));
  EXPECT_FALSE(rat->GetValueAsBoolean(1, 4));
  EXPECT_FALSE(rat->GetValueAsBoolean(99, 4));

  GDALRATDateTime dt = rat->GetValueAsDateTime(0, 5);
  EXPECT_TRUE(dt.bIsValid);
  EXPECT_EQ(2023, dt.nYear);
  EXPECT_EQ(5, dt.nMonth);
  EXPECT_EQ(15, dt.nDay);

  size_t wkb_size = 0;
  const GByte* pWKB = rat->GetValueAsWKBGeometry(0, 6, wkb_size);
  EXPECT_NE(nullptr, pWKB);
  EXPECT_GT(wkb_size, 0);

  // Unsupported mutation methods
  WithQuietHandler quiet;
  EXPECT_EQ(CE_Failure, rat->SetValue(0, 0, "test"));
  EXPECT_EQ(CE_Failure, rat->SetValue(0, 0, 1));
  EXPECT_EQ(CE_Failure, rat->SetValue(0, 0, 1.0));
  EXPECT_EQ(CE_Failure, rat->SetValue(0, 0, true));
  EXPECT_EQ(CE_Failure, rat->SetValue(0, 0, dt));
  EXPECT_EQ(CE_Failure, rat->SetValue(0, 0, nullptr, 0));
  EXPECT_EQ(0, rat->ChangesAreWrittenToFile());
  EXPECT_EQ(CE_Failure, rat->SetTableType(GRTT_ATHEMATIC));
  EXPECT_EQ(GRTT_THEMATIC, rat->GetTableType());
  rat->RemoveStatistics();

  // Test Clone()
  auto* cloned = rat->Clone();
  ASSERT_NE(nullptr, cloned);
  EXPECT_EQ(rat->GetRowCount(), cloned->GetRowCount());
  EXPECT_STREQ(rat->GetValueAsString(0, 2), cloned->GetValueAsString(0, 2));
  delete cloned;
}

}  // namespace
}  // namespace autotest2
