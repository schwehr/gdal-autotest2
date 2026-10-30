// Copyright 2017 Google Inc. All Rights Reserved.
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
// Tests netCDF raster driver.
//
// See also:
//   https://gdal.org/drivers/raster/netcdf.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/netcdf.py
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/netcdf_cf.py
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/netcdf_cfchecks.py
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/netcdf_multidim.py

#include "third_party/gdal/frmts/netcdf/netcdfdataset.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "netcdf.h"
#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/log/check.h"
#include "third_party/absl/memory/memory.h"
#include "third_party/absl/strings/str_split.h"
#include "third_party/gdal/frmts/netcdf/netcdfdrivercore.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_pam.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_json.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::IsSupersetOf;
using ::testing::Pair;

const std::filesystem::path kTestDataPath(
    "google3/third_party/gdal/autotest2/cpp/frmts/netcdf/testdata");

std::filesystem::path GetTestFilePath(const std::string_view filename) {
  return std::filesystem::path(::testing::SrcDir()) / kTestDataPath / filename;
}

TEST(IdentifyTest, IdentifyDoesNotExist) {
  auto open_info = std::make_unique<GDALOpenInfo>("/does_not_exist",
                                                  GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(FALSE, netCDFIdentifyFormat(open_info.get(), /*bCheckExt*/ false));
  EXPECT_EQ(FALSE, netCDFIdentifyFormat(open_info.get(), /*bCheckExt*/ true));
}

// Convert a C string list of "key=value" to a std::map<k, v>.
std::map<std::string, std::string> ParseMetadata(CSLConstList csl) {
  std::map<std::string, std::string> result;
  if (csl == nullptr) return result;
  for (; *csl != nullptr; csl++) {
    std::vector<std::string> v = absl::StrSplit(*csl, '=');
    CHECK_EQ(v.size(), 2);
    result[v[0]] = v[1];
  }
  return result;
}

// Writes a text attribute whose length is derived from the value, so fixture
// code cannot store a wrong length (e.g. a trailing NUL or a truncation).
int PutTextAtt(int ncid, int varid, const char* name, std::string_view value) {
  return nc_put_att_text(ncid, varid, name, value.size(), value.data());
}

// Error handler that records whether a CPLE_NotSupported error was emitted
// (several may be emitted in sequence, so "last error" is not sufficient).
bool g_saw_not_supported_error = false;
void RecordNotSupportedHandler(CPLErr /*eErr*/, CPLErrorNum nErrNo,
                               const char* /*pszMsg*/) {
  if (nErrNo == CPLE_NotSupported) g_saw_not_supported_error = true;
}

TEST(OpenTest, Int16) {
  const auto filepath = GetTestFilePath("int16-nogeo.nc");
  auto open_info = std::make_unique<GDALOpenInfo>(filepath.c_str(),
                                                  GDAL_OF_READONLY, nullptr);
  auto src = absl::WrapUnique(netCDFDataset::Open(open_info.get()));
  ASSERT_NE(nullptr, src);

  double geo_transform[6] = {};
  src->GetGeoTransform(geo_transform);
  // No geotransform, so it will be return the default.
  EXPECT_THAT(geo_transform,
              testing::ElementsAre(0.0, 1.0, 0.0, 0.0, 0.0, 1.0));

  EXPECT_EQ(0, src->GetGCPCount());
  EXPECT_EQ(512, src->GetRasterXSize());
  EXPECT_EQ(512, src->GetRasterYSize());
  // TODO: b/430052067 - This should become 1 in future version of GDAL.
  EXPECT_EQ(0, src->GetRasterCount());

  // Dataset owns metadata.
  CSLConstList metadata_list = src->GetMetadata();
  EXPECT_NE(nullptr, metadata_list);

  auto metadata = ParseMetadata(metadata_list);
  EXPECT_EQ(1, metadata.size());
  EXPECT_THAT(metadata, testing::UnorderedElementsAre(
                            Pair("NC_GLOBAL#Conventions", "CF-1.5")));

  // TODO: b/430052067 - Future versions of GDAL can read this band.
  GDALRasterBand* band = src->GetRasterBand(1);
  ASSERT_EQ(nullptr, band);
}

TEST(OpenTest, Goes_RadC_Dqf) {
  // 8-bit data that caused:
  //   Warning 1:
  //   NetCDF driver detected file type=5, but libnetcdf detected type=3
  const char kFilename[] =
      "OR_ABI-L1b-RadC-M3C06_G16_s20180402242198_e20180402244576_"
      "c20180402245020.nc";
  const auto filepath = GetTestFilePath(kFilename);
  const auto gdalpath = "NETCDF:" + std::string(filepath) + ":DQF";
  auto open_info = std::make_unique<GDALOpenInfo>(gdalpath.c_str(),
                                                  GDAL_OF_READONLY, nullptr);
  auto src = absl::WrapUnique(netCDFDataset::Open(open_info.get()));
  ASSERT_NE(nullptr, src);

  double geo_transform[6] = {};
  src->GetGeoTransform(geo_transform);
  EXPECT_THAT(geo_transform,
              testing::ElementsAre(-3627271.3409673548, 2004.0173154875411, 0,
                                   1583173.7916531805, 0, 2004.0173154875411));

  EXPECT_EQ(0, src->GetGCPCount());
  EXPECT_EQ(2500, src->GetRasterXSize());
  EXPECT_EQ(1500, src->GetRasterYSize());
  EXPECT_EQ(1, src->GetRasterCount());

  // Dataset owns metadata.
  CSLConstList metadata_list = src->GetMetadata();
  EXPECT_NE(nullptr, metadata_list);

  auto metadata = ParseMetadata(metadata_list);
  EXPECT_EQ(66, metadata.size());
  EXPECT_THAT(
      metadata,
      IsSupersetOf({Pair("DQF#valid_range", "{0,3}"),
                    Pair("DQF#_Unsigned", "true"),
                    Pair("goes_imager_projection#sweep_angle_axis", "x"),
                    Pair("NC_GLOBAL#Conventions", "CF-1.7")}));

  GDALRasterBand* band = src->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  int block_xsize = 0;
  int block_ysize = 0;
  band->GetBlockSize(&block_xsize, &block_ysize);
  EXPECT_EQ(250, block_xsize);
  EXPECT_EQ(250, block_ysize);
  EXPECT_EQ(GDT_Int8, band->GetRasterDataType());
  EXPECT_EQ(GCI_Undefined, band->GetColorInterpretation());

  double minmax[2] = {-1.0, -1.0};
  ASSERT_EQ(CE_None, band->ComputeRasterMinMax(false, minmax));
  EXPECT_THAT(minmax, testing::ElementsAre(0.0, 0.0));
}

TEST(NCDFGetAttrTest, ReadLongAttribute) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "long_attr_test.nc")
          .string();
  int cdfid = -1;
  ASSERT_EQ(NC_NOERR, nc_create(filename.c_str(), NC_CLOBBER, &cdfid));
  const std::string long_value(2048, 'x');
  ASSERT_EQ(NC_NOERR, nc_put_att_text(cdfid, NC_GLOBAL, "long_attribute",
                                      long_value.size(), long_value.data()));
  ASSERT_EQ(NC_NOERR, nc_close(cdfid));

  ASSERT_EQ(NC_NOERR, nc_open(filename.c_str(), NC_NOWRITE, &cdfid));
  std::string read_value;
  EXPECT_EQ(CE_None,
            NCDFGetAttr(cdfid, NC_GLOBAL, "long_attribute", read_value));
  EXPECT_EQ(long_value, read_value);
  EXPECT_EQ(NC_NOERR, nc_close(cdfid));
}

class NetCDFDatasetTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALAllRegister(); }
};

TEST_F(NetCDFDatasetTest, CreateAndWriteRastersAllTypes) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  // 1. Classic NetCDF (FORMAT=NC) with various data types
  {
    const std::vector<GDALDataType> types = {GDT_Byte, GDT_Int16, GDT_Int32,
                                             GDT_Float32, GDT_Float64};
    for (GDALDataType eType : types) {
      const std::string filename =
          (std::filesystem::path(::testing::TempDir()) /
           ("test_create_nc_" + std::string(GDALGetDataTypeName(eType)) +
            ".nc"))
              .string();

      char** papszOptions = nullptr;
      papszOptions = CSLSetNameValue(papszOptions, "FORMAT", "NC");
      papszOptions = CSLSetNameValue(papszOptions, "WRITE_BOTTOMUP", "YES");
      papszOptions = CSLSetNameValue(papszOptions, "WRITE_GDAL_TAGS", "YES");
      papszOptions = CSLSetNameValue(papszOptions, "WRITE_LONLAT", "YES");

      auto poDS = std::unique_ptr<GDALDataset>(
          poDriver->Create(filename.c_str(), 4, 4, 1, eType, papszOptions));
      CSLDestroy(papszOptions);
      ASSERT_NE(nullptr, poDS);

      double adfGT[6] = {10.0, 0.5, 0.0, 50.0, 0.0, -0.5};
      EXPECT_EQ(CE_None, poDS->SetGeoTransform(adfGT));

      OGRSpatialReference oSRS;
      oSRS.SetWellKnownGeogCS("WGS84");
      EXPECT_EQ(CE_None, poDS->SetSpatialRef(&oSRS));

      // Dataset SetMetadataItem exercises lines 2998-3028
      poDS->SetMetadataItem("TIFFTAG_DATETIME", "2026:10:29 12:00:00");

      auto poBand = poDS->GetRasterBand(1);
      ASSERT_NE(nullptr, poBand);

      double dfNoDataVal = -9999.0;
      if (eType == GDT_Byte) {
        dfNoDataVal = 255.0;
      } else if (eType == GDT_Int32) {
        dfNoDataVal = -99999.0;
      }
      EXPECT_EQ(CE_None, poBand->SetNoDataValue(dfNoDataVal));
      EXPECT_EQ(CE_None, poBand->SetOffset(10.5));
      EXPECT_EQ(CE_None, poBand->SetScale(2.5));
      EXPECT_EQ(CE_None, poBand->SetUnitType("degC"));
      poBand->SetMetadataItem("long_name", "surface_temperature");

      // Write data
      std::vector<double> dfData(16);
      for (int i = 0; i < 16; ++i) {
        dfData[i] = i * 2.0;
      }
      EXPECT_EQ(CE_None, poBand->RasterIO(GF_Write, 0, 0, 4, 4, dfData.data(),
                                          4, 4, GDT_Float64, 0, 0));
      poBand->FlushCache();
      poDS.reset();

      // Reopen and verify
      auto poOpenDS =
          std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
              filename.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
      ASSERT_NE(nullptr, poOpenDS);
      EXPECT_EQ(4, poOpenDS->GetRasterXSize());
      EXPECT_EQ(4, poOpenDS->GetRasterYSize());
      EXPECT_EQ(1, poOpenDS->GetRasterCount());

      double adfReadGT[6] = {};
      EXPECT_EQ(CE_None, poOpenDS->GetGeoTransform(adfReadGT));
      for (int i = 0; i < 6; ++i) {
        EXPECT_NEAR(adfGT[i], adfReadGT[i], 1e-4);
      }

      auto poOpenBand = poOpenDS->GetRasterBand(1);
      ASSERT_NE(nullptr, poOpenBand);

      int bSuccess = 0;
      double dfNoData = poOpenBand->GetNoDataValue(&bSuccess);
      EXPECT_TRUE(bSuccess);
      EXPECT_NEAR(dfNoDataVal, dfNoData, 1e-3);

      double dfOffset = poOpenBand->GetOffset(&bSuccess);
      EXPECT_TRUE(bSuccess);
      EXPECT_DOUBLE_EQ(10.5, dfOffset);

      double dfScale = poOpenBand->GetScale(&bSuccess);
      EXPECT_TRUE(bSuccess);
      EXPECT_DOUBLE_EQ(2.5, dfScale);

      EXPECT_STREQ("degC", poOpenBand->GetUnitType());

      std::vector<double> dfReadData(16);
      EXPECT_EQ(CE_None,
                poOpenBand->RasterIO(GF_Read, 0, 0, 4, 4, dfReadData.data(), 4,
                                     4, GDT_Float64, 0, 0));
      for (int i = 0; i < 16; ++i) {
        EXPECT_NEAR(dfData[i], dfReadData[i], 1e-3);
      }

      poOpenDS.reset();
      VSIUnlink(filename.c_str());
    }
  }

  // 2. NetCDF-4 (FORMAT=NC4) with advanced types, chunking, deflate, signed
  // bytes
  {
    const std::vector<GDALDataType> nc4Types = {
        GDT_Byte, GDT_Int8, GDT_UInt16, GDT_UInt32, GDT_Int64, GDT_UInt64};
    for (GDALDataType eType : nc4Types) {
      const std::string filename =
          (std::filesystem::path(::testing::TempDir()) /
           ("test_create_nc4_" + std::string(GDALGetDataTypeName(eType)) +
            ".nc"))
              .string();

      char** papszOptions = nullptr;
      papszOptions = CSLSetNameValue(papszOptions, "FORMAT", "NC4");
      papszOptions = CSLSetNameValue(papszOptions, "COMPRESS", "DEFLATE");
      papszOptions = CSLSetNameValue(papszOptions, "ZLEVEL", "4");
      papszOptions = CSLSetNameValue(papszOptions, "CHUNKING", "YES");
      papszOptions = CSLSetNameValue(papszOptions, "BLOCKXSIZE", "2");
      papszOptions = CSLSetNameValue(papszOptions, "BLOCKYSIZE", "2");
      papszOptions = CSLSetNameValue(papszOptions, "CHECKSUM", "YES");
      papszOptions = CSLSetNameValue(papszOptions, "WRITE_BOTTOMUP", "NO");
      if (eType == GDT_Int8) {
        papszOptions = CSLSetNameValue(papszOptions, "PIXELTYPE", "SIGNEDBYTE");
      }

      auto poDS = std::unique_ptr<GDALDataset>(
          poDriver->Create(filename.c_str(), 4, 4, 1, eType, papszOptions));
      CSLDestroy(papszOptions);
      ASSERT_NE(nullptr, poDS);

      auto poBand = poDS->GetRasterBand(1);
      ASSERT_NE(nullptr, poBand);

      if (eType == GDT_Int64) {
        EXPECT_EQ(CE_None, poBand->SetNoDataValueAsInt64(-999999999LL));
        int bSuccess = 0;
        EXPECT_EQ(-999999999LL, poBand->GetNoDataValueAsInt64(&bSuccess));
        EXPECT_TRUE(bSuccess);
      } else if (eType == GDT_UInt64) {
        EXPECT_EQ(CE_None, poBand->SetNoDataValueAsUInt64(999999999ULL));
        int bSuccess = 0;
        EXPECT_EQ(999999999ULL, poBand->GetNoDataValueAsUInt64(&bSuccess));
        EXPECT_TRUE(bSuccess);
      } else {
        EXPECT_EQ(CE_None, poBand->SetNoDataValue(100.0));
      }

      // Color table on Byte band
      if (eType == GDT_Byte) {
        GDALColorTable oCT;
        GDALColorEntry oEntry = {255, 128, 64, 255};
        oCT.SetColorEntry(0, &oEntry);
        EXPECT_EQ(CE_None, poBand->SetColorTable(&oCT));
        EXPECT_EQ(CE_None, poBand->SetColorInterpretation(GCI_PaletteIndex));
      }

      std::vector<double> dfData(16);
      for (int i = 0; i < 16; ++i) {
        dfData[i] = (i + 1) * 3;
      }
      EXPECT_EQ(CE_None, poBand->RasterIO(GF_Write, 0, 0, 4, 4, dfData.data(),
                                          4, 4, GDT_Float64, 0, 0));
      poDS.reset();

      // Reopen and verify
      auto poOpenDS =
          std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
              filename.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
      ASSERT_NE(nullptr, poOpenDS);
      auto poOpenBand = poOpenDS->GetRasterBand(1);
      ASSERT_NE(nullptr, poOpenBand);

      if (eType == GDT_Int64) {
        int bSuccess = 0;
        EXPECT_EQ(-999999999LL, poOpenBand->GetNoDataValueAsInt64(&bSuccess));
        EXPECT_TRUE(bSuccess);
      } else if (eType == GDT_UInt64) {
        int bSuccess = 0;
        EXPECT_EQ(999999999ULL, poOpenBand->GetNoDataValueAsUInt64(&bSuccess));
        EXPECT_TRUE(bSuccess);
      }

      std::vector<double> dfReadData(16);
      EXPECT_EQ(CE_None,
                poOpenBand->RasterIO(GF_Read, 0, 0, 4, 4, dfReadData.data(), 4,
                                     4, GDT_Float64, 0, 0));
      for (int i = 0; i < 16; ++i) {
        EXPECT_NEAR(dfData[i], dfReadData[i], 1e-3);
      }

      poOpenDS.reset();
      VSIUnlink(filename.c_str());
    }
  }

  // 3. Error and edge paths in Create
  {
    // Invalid dimensions
    EXPECT_EQ(nullptr, poDriver->Create("/vsimem/invalid_dims.nc", 0, 0, 1,
                                        GDT_Byte, nullptr));
    // Negative bands
    EXPECT_EQ(nullptr, poDriver->Create("/vsimem/invalid_bands.nc", 5, 5, -1,
                                        GDT_Byte, nullptr));
    // Unsupported type in classic NetCDF (Int64 not supported in NC)
    char** papszNCOpt = CSLSetNameValue(nullptr, "FORMAT", "NC");
    EXPECT_EQ(nullptr, poDriver->Create("/vsimem/unsupported_nc.nc", 5, 5, 1,
                                        GDT_Int64, papszNCOpt));
    CSLDestroy(papszNCOpt);
  }
}

TEST_F(NetCDFDatasetTest, CreateCopyAndOptions) {
  GDALDriver* poNetCDFDriver =
      GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poNetCDFDriver);
  GDALDriver* poMemDriver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, poMemDriver);

  // 1. Single-band dataset CreateCopy
  {
    auto poMemDS = std::unique_ptr<GDALDataset>(
        poMemDriver->Create("", 8, 8, 1, GDT_Int16, nullptr));
    ASSERT_NE(nullptr, poMemDS);

    double adfGT[6] = {10.0, 1.0, 0.0, 50.0, 0.0, -1.0};
    EXPECT_EQ(CE_None, poMemDS->SetGeoTransform(adfGT));

    OGRSpatialReference oSRS;
    oSRS.SetWellKnownGeogCS("WGS84");
    EXPECT_EQ(CE_None, poMemDS->SetSpatialRef(&oSRS));

    auto poBand1 = poMemDS->GetRasterBand(1);
    EXPECT_EQ(CE_None, poBand1->SetNoDataValue(-999.0));
    EXPECT_EQ(CE_None, poBand1->SetOffset(5.0));
    EXPECT_EQ(CE_None, poBand1->SetScale(0.5));
    EXPECT_EQ(CE_None, poBand1->SetUnitType("m"));

    std::vector<int16_t> anData1(64, 42);
    EXPECT_EQ(CE_None, poBand1->RasterIO(GF_Write, 0, 0, 8, 8, anData1.data(),
                                         8, 8, GDT_Int16, 0, 0));

    const std::string filename =
        (std::filesystem::path(::testing::TempDir()) / "test_createcopy_1b.nc")
            .string();
    char** papszOptions = nullptr;
    papszOptions = CSLSetNameValue(papszOptions, "FORMAT", "NC4");
    papszOptions = CSLSetNameValue(papszOptions, "COMPRESS", "DEFLATE");
    papszOptions = CSLSetNameValue(papszOptions, "ZLEVEL", "5");
    papszOptions = CSLSetNameValue(papszOptions, "BLOCKXSIZE", "4");
    papszOptions = CSLSetNameValue(papszOptions, "BLOCKYSIZE", "4");
    papszOptions = CSLSetNameValue(papszOptions, "WRITE_LONLAT", "YES");
    papszOptions = CSLSetNameValue(papszOptions, "TYPE_LONLAT", "float");

    auto poCopiedDS = std::unique_ptr<GDALDataset>(
        poNetCDFDriver->CreateCopy(filename.c_str(), poMemDS.get(), false,
                                   papszOptions, GDALDummyProgress, nullptr));
    CSLDestroy(papszOptions);
    ASSERT_NE(nullptr, poCopiedDS);
    poCopiedDS.reset();

    // Reopen and verify
    auto poOpenDS =
        std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
            filename.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poOpenDS);
    EXPECT_EQ(8, poOpenDS->GetRasterXSize());
    EXPECT_EQ(8, poOpenDS->GetRasterYSize());
    EXPECT_EQ(1, poOpenDS->GetRasterCount());

    double adfReadGT[6] = {};
    EXPECT_EQ(CE_None, poOpenDS->GetGeoTransform(adfReadGT));
    for (int i = 0; i < 6; ++i) {
      EXPECT_NEAR(adfGT[i], adfReadGT[i], 1e-4);
    }

    auto poOpenBand1 = poOpenDS->GetRasterBand(1);
    ASSERT_NE(nullptr, poOpenBand1);
    int bSuccess = 0;
    EXPECT_DOUBLE_EQ(-999.0, poOpenBand1->GetNoDataValue(&bSuccess));
    EXPECT_TRUE(bSuccess);
    EXPECT_DOUBLE_EQ(5.0, poOpenBand1->GetOffset(&bSuccess));
    EXPECT_TRUE(bSuccess);
    EXPECT_DOUBLE_EQ(0.5, poOpenBand1->GetScale(&bSuccess));
    EXPECT_TRUE(bSuccess);

    std::vector<int16_t> anReadData1(64);
    EXPECT_EQ(CE_None,
              poOpenBand1->RasterIO(GF_Read, 0, 0, 8, 8, anReadData1.data(), 8,
                                    8, GDT_Int16, 0, 0));
    for (int i = 0; i < 64; ++i) {
      EXPECT_EQ(42, anReadData1[i]);
    }

    poOpenDS.reset();
    VSIUnlink(filename.c_str());
  }

  // 2. Multi-band dataset CreateCopy (triggers Subdataset generation)
  {
    auto poMemDS = std::unique_ptr<GDALDataset>(
        poMemDriver->Create("", 8, 8, 2, GDT_Int16, nullptr));
    ASSERT_NE(nullptr, poMemDS);

    auto poBand1 = poMemDS->GetRasterBand(1);
    std::vector<int16_t> anData1(64, 11);
    EXPECT_EQ(CE_None, poBand1->RasterIO(GF_Write, 0, 0, 8, 8, anData1.data(),
                                         8, 8, GDT_Int16, 0, 0));

    auto poBand2 = poMemDS->GetRasterBand(2);
    std::vector<int16_t> anData2(64, 22);
    EXPECT_EQ(CE_None, poBand2->RasterIO(GF_Write, 0, 0, 8, 8, anData2.data(),
                                         8, 8, GDT_Int16, 0, 0));

    const std::string filename =
        (std::filesystem::path(::testing::TempDir()) / "test_createcopy_2b.nc")
            .string();
    auto poCopiedDS = std::unique_ptr<GDALDataset>(poNetCDFDriver->CreateCopy(
        filename.c_str(), poMemDS.get(), false, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, poCopiedDS);
    poCopiedDS.reset();

    // Reopen root dataset: should expose subdatasets for each band
    auto poOpenDS =
        std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
            filename.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poOpenDS);
    EXPECT_EQ(0, poOpenDS->GetRasterCount());

    CSLConstList papszSub = poOpenDS->GetMetadata("SUBDATASETS");
    ASSERT_NE(nullptr, papszSub);
    const char* pszSub1 = CSLFetchNameValue(papszSub, "SUBDATASET_1_NAME");
    ASSERT_NE(nullptr, pszSub1);

    auto poSubDS1 = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(pszSub1, GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poSubDS1);
    EXPECT_EQ(1, poSubDS1->GetRasterCount());

    auto poSubBand1 = poSubDS1->GetRasterBand(1);
    ASSERT_NE(nullptr, poSubBand1);
    std::vector<int16_t> anReadSub1(64);
    EXPECT_EQ(CE_None,
              poSubBand1->RasterIO(GF_Read, 0, 0, 8, 8, anReadSub1.data(), 8, 8,
                                   GDT_Int16, 0, 0));
    EXPECT_EQ(11, anReadSub1[0]);

    poSubDS1.reset();
    poOpenDS.reset();
    VSIUnlink(filename.c_str());
  }
}

TEST_F(NetCDFDatasetTest, ProjectionsAndAddProjectionVars) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  struct ProjEntry {
    std::string name;
    std::string definition;
    bool isEpsg;
    bool writeLonLat;
    double gt[6];
  };

  std::vector<ProjEntry> entries = {
      {"utm31n",
       "EPSG:32631",
       true,
       true,
       {500000.0, 10.0, 0.0, 4500000.0, 0.0, -10.0}},
      {"lambert93",
       "EPSG:2154",
       true,
       false,
       {600000.0, 10.0, 0.0, 6800000.0, 0.0, -10.0}},
      {"albers",
       "EPSG:5070",
       true,
       false,
       {-100000.0, 10.0, 0.0, 100000.0, 0.0, -10.0}},
      {"polar_stereo",
       "EPSG:3413",
       true,
       false,
       {-100000.0, 10.0, 0.0, 100000.0, 0.0, -10.0}},
      {"mercator",
       "EPSG:3857",
       true,
       false,
       {-2000000.0, 10.0, 0.0, 2000000.0, 0.0, -10.0}},
      {"rotated_pole",
       "+proj=ob_tran +o_proj=longlat +o_lon_p=0 +o_lat_p=30 +lon_0=180",
       false,
       false,
       {0.0, 0.5, 0.0, 10.0, 0.0, -0.5}},
      {"geos",
       "+proj=geos +h=35786000 +lon_0=0 +sweep=x +datum=WGS84",
       false,
       false,
       {-1000000.0, 1000.0, 0.0, 1000000.0, 0.0, -1000.0}},
      {"ortho",
       "+proj=ortho +lat_0=45 +lon_0=10 +datum=WGS84",
       false,
       false,
       {-100000.0, 100.0, 0.0, 100000.0, 0.0, -100.0}},
      {"sinu",
       "+proj=sinu +lon_0=0 +datum=WGS84",
       false,
       false,
       {-100000.0, 100.0, 0.0, 100000.0, 0.0, -100.0}},
      {"stere",
       "+proj=stere +lat_0=90 +lon_0=0 +datum=WGS84",
       false,
       false,
       {-100000.0, 100.0, 0.0, 100000.0, 0.0, -100.0}},
  };

  for (const auto& entry : entries) {
    const std::string filename = (std::filesystem::path(::testing::TempDir()) /
                                  ("proj_test_" + entry.name + ".nc"))
                                     .string();

    char** papszOptions = nullptr;
    papszOptions = CSLSetNameValue(papszOptions, "FORMAT", "NC4");
    if (entry.writeLonLat) {
      papszOptions = CSLSetNameValue(papszOptions, "WRITE_LONLAT", "YES");
      papszOptions = CSLSetNameValue(papszOptions, "TYPE_LONLAT", "float");
    }

    auto poDS = std::unique_ptr<GDALDataset>(
        poDriver->Create(filename.c_str(), 4, 4, 1, GDT_Float32, papszOptions));
    CSLDestroy(papszOptions);
    ASSERT_NE(nullptr, poDS);

    EXPECT_EQ(CE_None, poDS->SetGeoTransform(const_cast<double*>(entry.gt)));

    OGRSpatialReference oSRS;
    if (entry.isEpsg) {
      EXPECT_EQ(OGRERR_NONE, oSRS.SetFromUserInput(entry.definition.c_str()));
    } else {
      EXPECT_EQ(OGRERR_NONE, oSRS.importFromProj4(entry.definition.c_str()));
    }
    EXPECT_EQ(CE_None, poDS->SetSpatialRef(&oSRS));

    auto poBand = poDS->GetRasterBand(1);
    ASSERT_NE(nullptr, poBand);
    std::vector<float> data(16, 1.0f);
    EXPECT_EQ(CE_None, poBand->RasterIO(GF_Write, 0, 0, 4, 4, data.data(), 4, 4,
                                        GDT_Float32, 0, 0));
    poDS.reset();

    // Reopen and verify
    auto poOpenDS =
        std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
            filename.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poOpenDS);
    EXPECT_NE(nullptr, poOpenDS->GetSpatialRef());

    double adfReadGT[6] = {};
    EXPECT_EQ(CE_None, poOpenDS->GetGeoTransform(adfReadGT));
    EXPECT_NEAR(entry.gt[1], adfReadGT[1], 1e-3);
    EXPECT_NEAR(entry.gt[5], adfReadGT[5], 1e-3);

    poOpenDS.reset();
    VSIUnlink(filename.c_str());
  }

  // 11. Swath data with 2D coordinates -> GCPs
  {
    const std::string filename =
        (std::filesystem::path(::testing::TempDir()) / "swath_2d_coords.nc")
            .string();
    int ncid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_create(filename.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));
    int dimids[2];
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "y", 4, &dimids[0]));
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "x", 4, &dimids[1]));
    int varid_lat = -1, varid_lon = -1, varid_data = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "lat", NC_FLOAT, 2, dimids, &varid_lat));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "lon", NC_FLOAT, 2, dimids, &varid_lon));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "data", NC_FLOAT, 2, dimids, &varid_data));
    ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, varid_data, "coordinates", "lat lon"));
    ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, varid_lat, "units", "degrees_north"));
    ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, varid_lon, "units", "degrees_east"));
    ASSERT_EQ(NC_NOERR, nc_enddef(ncid));
    float afLat[16] = {10, 10, 10, 10, 11, 11, 11, 11,
                       12, 12, 12, 12, 13, 13, 13, 13};
    float afLon[16] = {20, 21, 22, 23, 20, 21, 22, 23,
                       20, 21, 22, 23, 20, 21, 22, 23};
    ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, varid_lat, afLat));
    ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, varid_lon, afLon));
    ASSERT_EQ(NC_NOERR, nc_close(ncid));

    auto poOpenDS =
        std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
            filename.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poOpenDS);
    EXPECT_NE(nullptr, poOpenDS->GetMetadata("GEOLOCATION"));
    poOpenDS.reset();
    VSIUnlink(filename.c_str());
  }

  // 12. 1D coordinate variables
  {
    const std::string filename =
        (std::filesystem::path(::testing::TempDir()) / "geo_1d_coords.nc")
            .string();
    int ncid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_create(filename.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));
    int dimids[2];
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "lat", 4, &dimids[0]));
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "lon", 4, &dimids[1]));
    int varid_lat = -1, varid_lon = -1, varid_data = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "lat", NC_DOUBLE, 1, &dimids[0], &varid_lat));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "lon", NC_DOUBLE, 1, &dimids[1], &varid_lon));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "var1", NC_FLOAT, 2, dimids, &varid_data));
    ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, varid_lat, "units", "degrees_north"));
    ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, varid_lon, "units", "degrees_east"));
    ASSERT_EQ(NC_NOERR,
              PutTextAtt(ncid, varid_lat, "standard_name", "latitude"));
    ASSERT_EQ(NC_NOERR,
              PutTextAtt(ncid, varid_lon, "standard_name", "longitude"));
    ASSERT_EQ(NC_NOERR, nc_enddef(ncid));
    double adfLat[4] = {40.0, 41.0, 42.02, 43.05};
    double adfLon[4] = {10.0, 11.0, 12.02, 13.05};
    ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, varid_lat, adfLat));
    ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, varid_lon, adfLon));
    ASSERT_EQ(NC_NOERR, nc_close(ncid));

    auto poOpenDS =
        std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
            filename.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poOpenDS);
    EXPECT_NE(nullptr, poOpenDS->GetMetadata("GEOLOCATION2"));
    EXPECT_NE(nullptr, poOpenDS->GetMetadataItem("Y_VALUES", "GEOLOCATION2"));
    poOpenDS.reset();
    VSIUnlink(filename.c_str());
  }
}

TEST_F(NetCDFDatasetTest, VectorLayersAndNCGroups) {
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  // 1. Classic NetCDF vector dataset with layer and feature creation
  {
    const std::string filename =
        (std::filesystem::path(::testing::TempDir()) / "vector_classic.nc")
            .string();
    auto poDS = std::unique_ptr<GDALDataset>(
        poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    ASSERT_NE(nullptr, poDS);
    EXPECT_TRUE(poDS->TestCapability(ODsCCreateLayer));
    EXPECT_TRUE(poDS->TestCapability(ODsCZGeometries));

    OGRSpatialReference oSRS;
    oSRS.SetWellKnownGeogCS("WGS84");
    auto poLayer = poDS->CreateLayer("points_layer", &oSRS, wkbPoint, nullptr);
    ASSERT_NE(nullptr, poLayer);

    OGRFieldDefn oFieldStr("name", OFTString);
    EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&oFieldStr));
    OGRFieldDefn oFieldInt("val", OFTInteger);
    EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&oFieldInt));

    // Create 25 features to trigger dimension growth (GrowDim)
    for (int i = 0; i < 25; ++i) {
      auto poFeature = std::make_unique<OGRFeature>(poLayer->GetLayerDefn());
      poFeature->SetField("name", CPLSPrintf("pt_%d", i));
      poFeature->SetField("val", i * 10);
      auto poPt = std::make_unique<OGRPoint>(10.0 + i * 0.1, 50.0 + i * 0.1);
      poFeature->SetGeometry(poPt.get());
      EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(poFeature.get()));
    }
    poDS.reset();

    // Reopen in vector mode
    auto poOpenDS =
        std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
            filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poOpenDS);
    EXPECT_EQ(1, poOpenDS->GetLayerCount());
    auto poOpenLayer = poOpenDS->GetLayer(0);
    ASSERT_NE(nullptr, poOpenLayer);
    EXPECT_EQ(25, poOpenLayer->GetFeatureCount());
    auto poReadFeat =
        std::unique_ptr<OGRFeature>(poOpenLayer->GetNextFeature());
    ASSERT_NE(nullptr, poReadFeat);
    EXPECT_STREQ("pt_0", poReadFeat->GetFieldAsString("name"));
    EXPECT_EQ(0, poReadFeat->GetFieldAsInteger("val"));
    poOpenDS.reset();
    VSIUnlink(filename.c_str());
  }

  // 2. NetCDF-4 vector dataset with MULTIPLE_LAYERS=SEPARATE_GROUPS
  {
    const std::string filename =
        (std::filesystem::path(::testing::TempDir()) / "vector_groups.nc")
            .string();
    char** papszOptions = nullptr;
    papszOptions = CSLSetNameValue(papszOptions, "FORMAT", "NC4");
    papszOptions =
        CSLSetNameValue(papszOptions, "MULTIPLE_LAYERS", "SEPARATE_GROUPS");
    auto poDS = std::unique_ptr<GDALDataset>(
        poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, papszOptions));
    CSLDestroy(papszOptions);
    ASSERT_NE(nullptr, poDS);

    auto poLayerA = poDS->CreateLayer("layer_pts", nullptr, wkbPoint, nullptr);
    ASSERT_NE(nullptr, poLayerA);
    OGRFieldDefn oFieldId("id", OFTInteger);
    EXPECT_EQ(OGRERR_NONE, poLayerA->CreateField(&oFieldId));
    auto featA = std::make_unique<OGRFeature>(poLayerA->GetLayerDefn());
    featA->SetField("id", 101);
    auto pt = std::make_unique<OGRPoint>(1.0, 2.0);
    featA->SetGeometry(pt.get());
    EXPECT_EQ(OGRERR_NONE, poLayerA->CreateFeature(featA.get()));

    auto poLayerB =
        poDS->CreateLayer("layer_lines", nullptr, wkbLineString, nullptr);
    ASSERT_NE(nullptr, poLayerB);
    EXPECT_EQ(OGRERR_NONE, poLayerB->CreateField(&oFieldId));
    auto featB = std::make_unique<OGRFeature>(poLayerB->GetLayerDefn());
    featB->SetField("id", 202);
    auto line = std::make_unique<OGRLineString>();
    line->addPoint(1.0, 2.0);
    line->addPoint(3.0, 4.0);
    featB->SetGeometry(line.get());
    EXPECT_EQ(OGRERR_NONE, poLayerB->CreateFeature(featB.get()));

    poDS.reset();

    // Reopen and verify
    auto poOpenDS =
        std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
            filename.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poOpenDS);
    EXPECT_EQ(2, poOpenDS->GetLayerCount());
    poOpenDS.reset();
    VSIUnlink(filename.c_str());
  }

  // 3. MULTIPLE_LAYERS=SEPARATE_FILES.  The driver only honours this when
  // GEOMETRY_ENCODING is not CF_1.8 (the default), see
  // netCDFDataset::ProcessCreationOptions.
  {
    const std::string dir =
        (std::filesystem::path(::testing::TempDir()) / "nc_sep_files").string();
    ASSERT_EQ(0, VSIMkdir(dir.c_str(), 0755));
    const std::string filename =
        (std::filesystem::path(dir) / "master.nc").string();
    char** papszOptions = nullptr;
    papszOptions = CSLSetNameValue(papszOptions, "GEOMETRY_ENCODING", "WKT");
    papszOptions =
        CSLSetNameValue(papszOptions, "MULTIPLE_LAYERS", "SEPARATE_FILES");
    auto poDS = std::unique_ptr<GDALDataset>(
        poDriver->Create(filename.c_str(), 0, 0, 0, GDT_Unknown, papszOptions));
    CSLDestroy(papszOptions);
    ASSERT_NE(nullptr, poDS);

    auto l1 = poDS->CreateLayer("sub_layer_1", nullptr, wkbPoint, nullptr);
    ASSERT_NE(nullptr, l1);
    auto l2 = poDS->CreateLayer("sub_layer_2", nullptr, wkbPoint, nullptr);
    ASSERT_NE(nullptr, l2);
    poDS.reset();

    // SEPARATE_FILES makes `filename` a directory holding one .nc per layer.
    VSIStatBufL sStat;
    EXPECT_EQ(0, VSIStatL(filename.c_str(), &sStat));
    EXPECT_TRUE(VSI_ISDIR(sStat.st_mode));
    EXPECT_EQ(0, VSIStatL((std::filesystem::path(filename) / "sub_layer_1.nc")
                              .string()
                              .c_str(),
                          &sStat));
    EXPECT_EQ(0, VSIStatL((std::filesystem::path(filename) / "sub_layer_2.nc")
                              .string()
                              .c_str(),
                          &sStat));
    EXPECT_EQ(0, VSIRmdirRecursive(dir.c_str()));
  }
}

TEST_F(NetCDFDatasetTest, GrowDimAndCloning) {
  auto poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  // 1. Vector GrowDim in Classic NetCDF with multiple field types
  {
    const std::string fn =
        (std::filesystem::path(::testing::TempDir()) / "classic_grow.nc")
            .string();
    char** papszOptions = nullptr;
    papszOptions = CSLSetNameValue(papszOptions, "FORMAT", "NC");
    papszOptions = CSLSetNameValue(papszOptions, "GEOMETRY_ENCODING", "WKT");
    auto poDS = std::unique_ptr<GDALDataset>(
        poDriver->Create(fn.c_str(), 0, 0, 0, GDT_Unknown, papszOptions));
    CSLDestroy(papszOptions);
    ASSERT_NE(nullptr, poDS);

    auto poLayer =
        poDS->CreateLayer("grow_layer", nullptr, wkbLineString, nullptr);
    ASSERT_NE(nullptr, poLayer);

    OGRFieldDefn oFieldStr("str_val", OFTString);
    // Width 0 enables m_bAutoGrowStrings to call GrowDim!
    EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&oFieldStr));

    OGRFieldDefn oFieldInt("int_val", OFTInteger);
    EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&oFieldInt));

    OGRFieldDefn oFieldReal("real_val", OFTReal);
    EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&oFieldReal));

    OGRFieldDefn oFieldDate("date_val", OFTDate);
    EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&oFieldDate));

    OGRFieldDefn oFieldDateTime("dt_val", OFTDateTime);
    EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&oFieldDateTime));

    // Feature 1: fits within width
    auto feat1 = std::make_unique<OGRFeature>(poLayer->GetLayerDefn());
    feat1->SetField("str_val", "abc");
    feat1->SetField("int_val", 10);
    feat1->SetField("real_val", 10.5);
    feat1->SetField("date_val", "2023/10/25");
    feat1->SetField("dt_val", "2023/10/25 12:34:56");
    auto line1 = std::make_unique<OGRLineString>();
    line1->addPoint(1.0, 2.0);
    line1->addPoint(3.0, 4.0);
    feat1->SetGeometry(line1.get());
    EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(feat1.get()));

    // Feature 2: exceeds string width, forces GrowDim!
    auto feat2 = std::make_unique<OGRFeature>(poLayer->GetLayerDefn());
    feat2->SetField("str_val", "very_long_string_exceeding_initial_width");
    feat2->SetField("int_val", 20);
    feat2->SetField("real_val", 20.5);
    feat2->SetField("date_val", "2023/10/26");
    feat2->SetField("dt_val", "2023/10/26 12:34:56");
    auto line2 = std::make_unique<OGRLineString>();
    line2->addPoint(5.0, 6.0);
    line2->addPoint(7.0, 8.0);
    feat2->SetGeometry(line2.get());
    EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(feat2.get()));

    // Feature 3: long geometry WKT forces GrowDim on WKT dimension!
    auto feat3 = std::make_unique<OGRFeature>(poLayer->GetLayerDefn());
    feat3->SetField("str_val", "normal");
    feat3->SetField("int_val", 30);
    auto line3 = std::make_unique<OGRLineString>();
    for (int pt_idx = 0; pt_idx < 150; ++pt_idx) {
      line3->addPoint(pt_idx * 1.5, pt_idx * 2.5);
    }
    feat3->SetGeometry(line3.get());
    EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(feat3.get()));

    poDS.reset();

    // Verify reopening
    auto poOpenDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(fn.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poOpenDS);
    auto poReadLayer = poOpenDS->GetLayer(0);
    ASSERT_NE(nullptr, poReadLayer);
    EXPECT_EQ(3, poReadLayer->GetFeatureCount(true));
    poOpenDS.reset();
    VSIUnlink(fn.c_str());
  }

  // 3. Vector GrowDim in NC4 with Groups
  {
    const std::string fn =
        (std::filesystem::path(::testing::TempDir()) / "nc4_grp_grow.nc")
            .string();
    char** papszOptions = nullptr;
    papszOptions = CSLSetNameValue(papszOptions, "FORMAT", "NC4");
    papszOptions =
        CSLSetNameValue(papszOptions, "MULTIPLE_LAYERS", "SEPARATE_GROUPS");
    papszOptions = CSLSetNameValue(papszOptions, "GEOMETRY_ENCODING", "WKT");
    auto poDS = std::unique_ptr<GDALDataset>(
        poDriver->Create(fn.c_str(), 0, 0, 0, GDT_Unknown, papszOptions));
    CSLDestroy(papszOptions);
    ASSERT_NE(nullptr, poDS);

    auto poLayer1 =
        poDS->CreateLayer("grp_layer1", nullptr, wkbLineString, nullptr);
    ASSERT_NE(nullptr, poLayer1);
    auto poLayer2 = poDS->CreateLayer("grp_layer2", nullptr, wkbPoint, nullptr);
    ASSERT_NE(nullptr, poLayer2);

    OGRFieldDefn oFieldStr("str_val", OFTString);
    EXPECT_EQ(OGRERR_NONE, poLayer1->CreateField(&oFieldStr));

    OGRFieldDefn oFieldInt("int_val", OFTInteger);
    EXPECT_EQ(OGRERR_NONE, poLayer1->CreateField(&oFieldInt));

    OGRFieldDefn oFieldReal("real_val", OFTReal);
    EXPECT_EQ(OGRERR_NONE, poLayer2->CreateField(&oFieldReal));

    auto featL2 = std::make_unique<OGRFeature>(poLayer2->GetLayerDefn());
    featL2->SetField("real_val", 123.456);
    auto pt = std::make_unique<OGRPoint>(10.0, 20.0);
    featL2->SetGeometry(pt.get());
    EXPECT_EQ(OGRERR_NONE, poLayer2->CreateFeature(featL2.get()));

    auto feat1 = std::make_unique<OGRFeature>(poLayer1->GetLayerDefn());
    feat1->SetField("str_val", "abc");
    feat1->SetField("int_val", 42);
    auto line1 = std::make_unique<OGRLineString>();
    line1->addPoint(1.0, 2.0);
    line1->addPoint(3.0, 4.0);
    feat1->SetGeometry(line1.get());
    EXPECT_EQ(OGRERR_NONE, poLayer1->CreateFeature(feat1.get()));

    // Exceed string width to trigger GrowDim on NC4 with groups!
    auto feat2 = std::make_unique<OGRFeature>(poLayer1->GetLayerDefn());
    feat2->SetField(
        "str_val",
        "a_much_longer_string_for_nc4_groups_that_exceeds_initial_width");
    feat2->SetField("int_val", 84);
    feat2->SetGeometry(line1.get());
    EXPECT_EQ(OGRERR_NONE, poLayer1->CreateFeature(feat2.get()));

    poDS.reset();

    // Verify reopening NC4 groups after GrowDim
    auto poOpenDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(fn.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poOpenDS);
    EXPECT_EQ(2, poOpenDS->GetLayerCount());
    auto poReadL1 = poOpenDS->GetLayer(0);
    ASSERT_NE(nullptr, poReadL1);
    EXPECT_GE(poReadL1->GetFeatureCount(true), 1);
    auto poReadL2 = poOpenDS->GetLayer(1);
    ASSERT_NE(nullptr, poReadL2);
    EXPECT_GE(poReadL2->GetFeatureCount(true), 1);
    poOpenDS.reset();

    VSIUnlink(fn.c_str());
  }
}

TEST_F(NetCDFDatasetTest, ExtraDimensionsAnd1DVariables) {
  auto poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);
  auto poMemDriver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, poMemDriver);

  // Test various extra dimension types.
  struct ExtraDimCase {
    const char* dim_name;
    nc_type nc_type_val;
    const char* values_str;
  } test_cases[] = {
      {"time_int", NC_INT, "{10, 20}"},
      {"depth_dbl", NC_DOUBLE, "{100.5, 200.5}"},
      {"lev_short", NC_SHORT, "{1, 2}"},
      {"f_flt", NC_FLOAT, "{1.5, 2.5}"},
      {"b_byte", NC_BYTE, "{1, 2}"},
      {"ub_ubyte", NC_UBYTE, "{10, 20}"},
      {"us_ushort", NC_USHORT, "{100, 200}"},
      {"ui_uint", NC_UINT, "{1000, 2000}"},
      {"i64_int64", NC_INT64, "{10000000000, 20000000000}"},
      {"ui64_uint64", NC_UINT64, "{10000000000, 20000000000}"},
  };

  for (const auto& tc : test_cases) {
    const std::string fn = (std::filesystem::path(::testing::TempDir()) /
                            (std::string("extra_dim_") + tc.dim_name + ".nc"))
                               .string();

    auto poSrcDS = std::unique_ptr<GDALDataset>(
        poMemDriver->Create("", 4, 4, 2, GDT_Float32, nullptr));
    ASSERT_NE(nullptr, poSrcDS);

    poSrcDS->SetMetadataItem("NETCDF_DIM_EXTRA",
                             (std::string("{") + tc.dim_name + "}").c_str());
    poSrcDS->SetMetadataItem(
        (std::string("NETCDF_DIM_") + tc.dim_name + "_DEF").c_str(),
        (std::string("{2,") + std::to_string(static_cast<int>(tc.nc_type_val)) +
         "}")
            .c_str());
    poSrcDS->SetMetadataItem(
        (std::string("NETCDF_DIM_") + tc.dim_name + "_VALUES").c_str(),
        tc.values_str);

    char** papszOptions = nullptr;
    papszOptions = CSLSetNameValue(papszOptions, "FORMAT", "NC4");
    auto poDstDS = std::unique_ptr<GDALDataset>(
        poDriver->CreateCopy(fn.c_str(), poSrcDS.get(), false, papszOptions,
                             GDALDummyProgress, nullptr));
    CSLDestroy(papszOptions);
    ASSERT_NE(nullptr, poDstDS);
    poDstDS.reset();

    // Reopen with GDAL_NETCDF_REPORT_EXTRA_DIM_VALUES=YES
    std::unique_ptr<GDALDataset> poOpenDS;
    {
      CPLConfigOptionSetter oSetter("GDAL_NETCDF_REPORT_EXTRA_DIM_VALUES",
                                    "YES", /*bSetOnlyIfUndefined=*/false);
      poOpenDS.reset(static_cast<GDALDataset*>(
          GDALOpenEx(fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    }
    ASSERT_NE(nullptr, poOpenDS);

    // Verify extra dim values read back via NCDFGet1DVar
    const char* pszVal = poOpenDS->GetMetadataItem(
        (std::string("NETCDF_DIM_") + tc.dim_name + "_VALUES").c_str());
    EXPECT_NE(nullptr, pszVal);

    // Trigger CreateMetadataFromOtherVars
    auto poBand1 = poOpenDS->GetRasterBand(1);
    ASSERT_NE(nullptr, poBand1);
    EXPECT_NE(nullptr, poBand1->GetMetadata());
    EXPECT_NE(nullptr, poBand1->GetMetadataItem(
                           (std::string("NETCDF_DIM_") + tc.dim_name).c_str()));

    auto poBand2 = poOpenDS->GetRasterBand(2);
    ASSERT_NE(nullptr, poBand2);
    EXPECT_NE(nullptr, poBand2->GetMetadata());

    poOpenDS.reset();
    VSIUnlink(fn.c_str());
  }
}

TEST_F(NetCDFDatasetTest, ComplexDataTypesAndBlocks) {
  const std::string fn =
      (std::filesystem::path(::testing::TempDir()) / "complex_types.nc")
          .string();

  int cdfid = -1;
  ASSERT_EQ(NC_NOERR, nc_create(fn.c_str(), NC_CLOBBER | NC_NETCDF4, &cdfid));

  int dim_y = -1, dim_x = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "y", 4, &dim_y));
  ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "x", 4, &dim_x));
  int dims[2] = {dim_y, dim_x};

  // 1. Compound Short (GDT_CInt16)
  struct CpxShort {
    short r, i;
  };
  nc_type type_cshort;
  ASSERT_EQ(NC_NOERR,
            nc_def_compound(cdfid, sizeof(CpxShort), "cshort", &type_cshort));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(cdfid, type_cshort, "r",
                                         offsetof(CpxShort, r), NC_SHORT));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(cdfid, type_cshort, "i",
                                         offsetof(CpxShort, i), NC_SHORT));
  int vid_cshort = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(cdfid, "cint16_var", type_cshort, 2, dims, &vid_cshort));

  // 2. Compound Int (GDT_CInt32)
  struct CpxInt {
    int r, i;
  };
  nc_type type_cint;
  ASSERT_EQ(NC_NOERR,
            nc_def_compound(cdfid, sizeof(CpxInt), "cint", &type_cint));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(cdfid, type_cint, "r",
                                         offsetof(CpxInt, r), NC_INT));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(cdfid, type_cint, "i",
                                         offsetof(CpxInt, i), NC_INT));
  int vid_cint = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(cdfid, "cint32_var", type_cint, 2, dims, &vid_cint));

  // 3. Compound Float (GDT_CFloat32)
  struct CpxFloat {
    float r, i;
  };
  nc_type type_cfloat;
  ASSERT_EQ(NC_NOERR,
            nc_def_compound(cdfid, sizeof(CpxFloat), "cfloat", &type_cfloat));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(cdfid, type_cfloat, "r",
                                         offsetof(CpxFloat, r), NC_FLOAT));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(cdfid, type_cfloat, "i",
                                         offsetof(CpxFloat, i), NC_FLOAT));
  int vid_cfloat = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(cdfid, "cfloat32_var", type_cfloat, 2, dims,
                                 &vid_cfloat));

  // 4. Compound Double (GDT_CFloat64)
  struct CpxDouble {
    double r, i;
  };
  nc_type type_cdouble;
  ASSERT_EQ(NC_NOERR, nc_def_compound(cdfid, sizeof(CpxDouble), "cdouble",
                                      &type_cdouble));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(cdfid, type_cdouble, "r",
                                         offsetof(CpxDouble, r), NC_DOUBLE));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(cdfid, type_cdouble, "i",
                                         offsetof(CpxDouble, i), NC_DOUBLE));
  int vid_cdouble = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(cdfid, "cfloat64_var", type_cdouble, 2, dims,
                                 &vid_cdouble));

  ASSERT_EQ(NC_NOERR, nc_enddef(cdfid));

  // Fill sample data
  CpxShort cs_data[16];
  CpxInt ci_data[16];
  CpxFloat cf_data[16];
  CpxDouble cd_data[16];
  for (int idx = 0; idx < 16; ++idx) {
    cs_data[idx] = {static_cast<short>(idx), static_cast<short>(idx * 2)};
    ci_data[idx] = {idx * 10, idx * 20};
    cf_data[idx] = {idx * 1.5f, idx * 2.5f};
    cd_data[idx] = {idx * 10.5, idx * 20.5};
  }
  ASSERT_EQ(NC_NOERR, nc_put_var(cdfid, vid_cshort, cs_data));
  ASSERT_EQ(NC_NOERR, nc_put_var(cdfid, vid_cint, ci_data));
  ASSERT_EQ(NC_NOERR, nc_put_var(cdfid, vid_cfloat, cf_data));
  ASSERT_EQ(NC_NOERR, nc_put_var(cdfid, vid_cdouble, cd_data));

  ASSERT_EQ(NC_NOERR, nc_close(cdfid));

  // Open subdataset for each complex variable and test RasterIO
  const struct {
    const char* var_name;
    GDALDataType expected_dt;
  } cpx_tests[] = {
      {"cint16_var", GDT_CInt16},
      {"cint32_var", GDT_CInt32},
      {"cfloat32_var", GDT_CFloat32},
      {"cfloat64_var", GDT_CFloat64},
  };

  for (const auto& ct : cpx_tests) {
    const std::string sds_name =
        std::string("NETCDF:\"") + fn + "\":" + ct.var_name;
    auto poSubDS =
        std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
            sds_name.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poSubDS);
    auto poBand = poSubDS->GetRasterBand(1);
    ASSERT_NE(nullptr, poBand);
    EXPECT_EQ(ct.expected_dt, poBand->GetRasterDataType());

    // Read full raster to trigger CheckDataCpx
    std::vector<double> buf(4 * 4 * 2);
    EXPECT_EQ(CE_None, poBand->RasterIO(GF_Read, 0, 0, 4, 4, buf.data(), 4, 4,
                                        GDT_CFloat64, 0, 0, nullptr));

    // CreateCopy rejects complex source bands with CPLE_NotSupported.
    auto poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
    const std::string copy_fn = (std::filesystem::path(::testing::TempDir()) /
                                 (std::string("copy_") + ct.var_name + ".nc"))
                                    .string();
    char** papszOptions = nullptr;
    papszOptions = CSLSetNameValue(papszOptions, "FORMAT", "NC4");
    std::unique_ptr<GDALDataset> poCopyDS;
    {
      CPLErrorHandlerPusher oQuiet(CPLQuietErrorHandler);
      CPLErrorReset();
      poCopyDS.reset(poDriver->CreateCopy(copy_fn.c_str(), poSubDS.get(), false,
                                          papszOptions, GDALDummyProgress,
                                          nullptr));
      EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo()) << ct.var_name;
    }
    CSLDestroy(papszOptions);
    EXPECT_EQ(nullptr, poCopyDS) << ct.var_name;
    VSIStatBufL sStat;
    EXPECT_NE(0, VSIStatL(copy_fn.c_str(), &sStat)) << ct.var_name;
  }

  VSIUnlink(fn.c_str());
}

TEST_F(NetCDFDatasetTest, UtilityFunctionsAndAttributes) {
  const std::string fn =
      (std::filesystem::path(::testing::TempDir()) / "utils_attrs.nc").string();

  int cdfid = -1;
  ASSERT_EQ(NC_NOERR, nc_create(fn.c_str(), NC_CLOBBER | NC_NETCDF4, &cdfid));

  int dim_id = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "dim_coord", 3, &dim_id));

  // Define coordinate variables for dimension check functions
  int lon_vid = -1, lat_vid = -1, x_vid = -1, y_vid = -1, time_vid = -1,
      depth_vid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(cdfid, "lon", NC_DOUBLE, 1, &dim_id, &lon_vid));
  ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, lon_vid, "standard_name", "longitude"));
  ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, lon_vid, "units", "degrees_east"));

  ASSERT_EQ(NC_NOERR,
            nc_def_var(cdfid, "lat", NC_DOUBLE, 1, &dim_id, &lat_vid));
  ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, lat_vid, "standard_name", "latitude"));
  ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, lat_vid, "units", "degrees_north"));

  ASSERT_EQ(NC_NOERR, nc_def_var(cdfid, "x", NC_DOUBLE, 1, &dim_id, &x_vid));
  ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, x_vid, "standard_name",
                                 "projection_x_coordinate"));

  ASSERT_EQ(NC_NOERR, nc_def_var(cdfid, "y", NC_DOUBLE, 1, &dim_id, &y_vid));
  ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, y_vid, "standard_name",
                                 "projection_y_coordinate"));

  ASSERT_EQ(NC_NOERR,
            nc_def_var(cdfid, "time", NC_DOUBLE, 1, &dim_id, &time_vid));
  ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, time_vid, "standard_name", "time"));

  ASSERT_EQ(NC_NOERR,
            nc_def_var(cdfid, "depth", NC_DOUBLE, 1, &dim_id, &depth_vid));
  ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, depth_vid, "axis", "Z"));
  ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, depth_vid, "units", "m"));

  // Global attributes of all types: scalar and array
  const char* str_val = "Sample String";
  ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, NC_GLOBAL, "att_text", str_val));
  signed char schar_arr[2] = {1, 2};
  ASSERT_EQ(NC_NOERR, nc_put_att_schar(cdfid, NC_GLOBAL, "att_schar", NC_BYTE,
                                       2, schar_arr));
  unsigned char uchar_arr[2] = {10, 20};
  ASSERT_EQ(NC_NOERR, nc_put_att_uchar(cdfid, NC_GLOBAL, "att_uchar", NC_UBYTE,
                                       2, uchar_arr));
  short short_arr[2] = {100, 200};
  ASSERT_EQ(NC_NOERR, nc_put_att_short(cdfid, NC_GLOBAL, "att_short", NC_SHORT,
                                       2, short_arr));
  unsigned short ushort_arr[2] = {1000, 2000};
  ASSERT_EQ(NC_NOERR, nc_put_att_ushort(cdfid, NC_GLOBAL, "att_ushort",
                                        NC_USHORT, 2, ushort_arr));
  int int_arr[2] = {10000, 20000};
  ASSERT_EQ(NC_NOERR,
            nc_put_att_int(cdfid, NC_GLOBAL, "att_int", NC_INT, 2, int_arr));
  unsigned int uint_arr[2] = {100000, 200000};
  ASSERT_EQ(NC_NOERR, nc_put_att_uint(cdfid, NC_GLOBAL, "att_uint", NC_UINT, 2,
                                      uint_arr));
  long long i64_arr[2] = {1000000000LL, 2000000000LL};
  ASSERT_EQ(NC_NOERR, nc_put_att_longlong(cdfid, NC_GLOBAL, "att_i64", NC_INT64,
                                          2, i64_arr));
  unsigned long long ui64_arr[2] = {1000000000ULL, 2000000000ULL};
  ASSERT_EQ(NC_NOERR, nc_put_att_ulonglong(cdfid, NC_GLOBAL, "att_ui64",
                                           NC_UINT64, 2, ui64_arr));
  float flt_arr[2] = {1.5f, 2.5f};
  ASSERT_EQ(NC_NOERR, nc_put_att_float(cdfid, NC_GLOBAL, "att_flt", NC_FLOAT, 2,
                                       flt_arr));
  double dbl_arr[2] = {10.5, 20.5};
  ASSERT_EQ(NC_NOERR, nc_put_att_double(cdfid, NC_GLOBAL, "att_dbl", NC_DOUBLE,
                                        2, dbl_arr));
  const char* str_arr[2] = {"first", "second"};
  ASSERT_EQ(NC_NOERR,
            nc_put_att_string(cdfid, NC_GLOBAL, "att_str_arr", 2, str_arr));

  // Subgroup for NCDFGetGroupFullName and NCDFResolveVar
  int subgrp_id = -1;
  ASSERT_EQ(NC_NOERR, nc_def_grp(cdfid, "subgrp", &subgrp_id));
  int subvar_id = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(subgrp_id, "subvar", NC_INT, 0, nullptr, &subvar_id));

  // Attributes with # for NCDFReadMetadataAsJson
  const char* v1 = "val1";
  const char* v2 = "val2";
  ASSERT_EQ(NC_NOERR, PutTextAtt(subgrp_id, NC_GLOBAL, "key#1", v1));
  ASSERT_EQ(NC_NOERR, PutTextAtt(subgrp_id, NC_GLOBAL, "key#2", v2));

  ASSERT_EQ(NC_NOERR, nc_enddef(cdfid));

  // Test NCDFGetAttr overloads on all types
  const char* att_names[] = {
      "att_text",   "att_schar", "att_uchar", "att_short",
      "att_ushort", "att_int",   "att_uint",  "att_i64",
      "att_ui64",   "att_flt",   "att_dbl",   "att_str_arr",
  };
  for (const char* att : att_names) {
    double dfVal = 0.0;
    NCDFGetAttr(cdfid, NC_GLOBAL, att, &dfVal);

    char* pszVal = nullptr;
    if (NCDFGetAttr(cdfid, NC_GLOBAL, att, &pszVal) == CE_None && pszVal) {
      CPLFree(pszVal);
    }

    std::string osVal;
    NCDFGetAttr(cdfid, NC_GLOBAL, att, osVal);
  }

  // Dimension check functions
  EXPECT_TRUE(NCDFIsVarLongitude(cdfid, lon_vid, "lon"));
  EXPECT_TRUE(NCDFIsVarLatitude(cdfid, lat_vid, "lat"));
  EXPECT_TRUE(NCDFIsVarProjectionX(cdfid, x_vid, "x"));
  EXPECT_TRUE(NCDFIsVarProjectionY(cdfid, y_vid, "y"));
  EXPECT_TRUE(NCDFIsVarTimeCoord(cdfid, time_vid, "time"));
  EXPECT_TRUE(NCDFIsVarVerticalCoord(cdfid, depth_vid, "depth"));

  EXPECT_FALSE(NCDFIsVarLongitude(cdfid, x_vid, "x"));
  EXPECT_FALSE(NCDFIsVarLatitude(cdfid, y_vid, "y"));
  EXPECT_FALSE(NCDFIsVarProjectionX(cdfid, lon_vid, "lon"));
  EXPECT_FALSE(NCDFIsVarProjectionY(cdfid, lat_vid, "lat"));

  // NCDFIsUserDefinedType
  EXPECT_FALSE(NCDFIsUserDefinedType(cdfid, NC_INT));
  EXPECT_TRUE(NCDFIsUserDefinedType(cdfid, NC_FIRSTUSERTYPEID));

  // NCDFGetGroupFullName
  EXPECT_EQ("/", NCDFGetGroupFullName(cdfid));
  EXPECT_EQ("/subgrp", NCDFGetGroupFullName(subgrp_id));

  // NCDFResolveVar
  int res_gid = -1, res_vid = -1;
  EXPECT_EQ(CE_None,
            NCDFResolveVar(cdfid, "/subgrp/subvar", &res_gid, &res_vid));
  EXPECT_EQ(subgrp_id, res_gid);
  EXPECT_EQ(subvar_id, res_vid);
  EXPECT_EQ(CE_None, NCDFResolveVar(cdfid, "subvar", &res_gid, &res_vid));
  EXPECT_EQ(subgrp_id, res_gid);
  EXPECT_EQ(subvar_id, res_vid);
  EXPECT_EQ(CE_Failure,
            NCDFResolveVar(cdfid, "nonexistent_var", &res_gid, &res_vid));

  // NCDFTokenizeCoordinatesAttribute
  char** papszTokens = NCDFTokenizeCoordinatesAttribute("lat lon");
  ASSERT_NE(nullptr, papszTokens);
  EXPECT_EQ(2, CSLCount(papszTokens));
  CSLDestroy(papszTokens);

  papszTokens = NCDFTokenizeCoordinatesAttribute(nullptr);
  ASSERT_NE(nullptr, papszTokens);
  EXPECT_EQ(0, CSLCount(papszTokens));
  CSLDestroy(papszTokens);

  // NCDFReadMetadataAsJson
  std::string json = NCDFReadMetadataAsJson(cdfid);
  EXPECT_FALSE(json.empty());

  ASSERT_EQ(NC_NOERR, nc_close(cdfid));

  VSIUnlink(fn.c_str());
}

TEST_F(NetCDFDatasetTest, SubdatasetsAndMetadataDomains) {
  const std::string fn =
      (std::filesystem::path(::testing::TempDir()) / "subdatasets_iso.nc")
          .string();

  int cdfid = -1;
  ASSERT_EQ(NC_NOERR, nc_create(fn.c_str(), NC_CLOBBER | NC_NETCDF4, &cdfid));

  int dim_y = -1, dim_x = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "y", 3, &dim_y));
  ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "x", 3, &dim_x));
  int dims[2] = {dim_y, dim_x};

  // Add 2D variables with spaces and colons in names
  int v1 = -1, v2 = -1, v3 = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(cdfid, "var with space", NC_FLOAT, 2, dims, &v1));
  ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, v1, "standard_name", "space_std_name"));

  ASSERT_EQ(NC_NOERR,
            nc_def_var(cdfid, "var:with:colon", NC_FLOAT, 2, dims, &v2));
  ASSERT_EQ(NC_NOERR, nc_def_var(cdfid, "var_third", NC_FLOAT, 2, dims, &v3));

  // Add ISO metadata and SUPPORT_DATA groups
  int meta_grp = -1, iso_grp = -1;
  ASSERT_EQ(NC_NOERR, nc_def_grp(cdfid, "METADATA", &meta_grp));
  ASSERT_EQ(NC_NOERR, nc_def_grp(meta_grp, "ISO_METADATA", &iso_grp));
  const char* iso_val = "1.0";
  ASSERT_EQ(NC_NOERR, PutTextAtt(iso_grp, NC_GLOBAL, "iso_ver", iso_val));

  int prod_grp = -1, supp_grp = -1;
  ASSERT_EQ(NC_NOERR, nc_def_grp(cdfid, "PRODUCT", &prod_grp));
  ASSERT_EQ(NC_NOERR, nc_def_grp(prod_grp, "SUPPORT_DATA", &supp_grp));
  const char* supp_val = "support";
  ASSERT_EQ(NC_NOERR, PutTextAtt(supp_grp, NC_GLOBAL, "supp_att", supp_val));

  ASSERT_EQ(NC_NOERR, nc_enddef(cdfid));
  ASSERT_EQ(NC_NOERR, nc_close(cdfid));

  // 1. Open and inspect metadata domains
  {
    auto poOpenDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poOpenDS);

    char** papszDomains = poOpenDS->GetMetadataDomainList();
    ASSERT_NE(nullptr, papszDomains);
    EXPECT_NE(-1, CSLFindString(papszDomains, "json:ISO_METADATA"));
    EXPECT_NE(-1, CSLFindString(papszDomains, "json:SUPPORT_DATA"));
    CSLDestroy(papszDomains);

    EXPECT_NE(nullptr, poOpenDS->GetMetadata("json:ISO_METADATA"));
    EXPECT_NE(nullptr, poOpenDS->GetMetadata("json:SUPPORT_DATA"));

    // Check SUBDATASETS list
    char** papszSub = poOpenDS->GetMetadata("SUBDATASETS");
    ASSERT_NE(nullptr, papszSub);
    EXPECT_GE(CSLCount(papszSub), 4);
  }

  // 2. Open with LIST_ALL_ARRAYS=YES
  {
    const char* papszOpenOptions[] = {"LIST_ALL_ARRAYS=YES", nullptr};
    auto poOpenDS =
        std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
            fn.c_str(), GDAL_OF_RASTER, nullptr, papszOpenOptions, nullptr)));
    ASSERT_NE(nullptr, poOpenDS);
  }

  // 3. Open with VARIABLES_AS_BANDS=YES
  {
    const char* papszOpenOptions[] = {"VARIABLES_AS_BANDS=YES", nullptr};
    auto poOpenDS =
        std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
            fn.c_str(), GDAL_OF_RASTER, nullptr, papszOpenOptions, nullptr)));
    ASSERT_NE(nullptr, poOpenDS);
    EXPECT_EQ(3, poOpenDS->GetRasterCount());
  }

  VSIUnlink(fn.c_str());
}

TEST_F(NetCDFDatasetTest, NASAEMITAndL2OceanGeolocation) {
  // 1. NASA L2 Ocean Geolocation
  {
    const std::string fn =
        (std::filesystem::path(::testing::TempDir()) / "nasa_l2_ocean.nc")
            .string();
    int cdfid = -1;
    ASSERT_EQ(NC_NOERR, nc_create(fn.c_str(), NC_CLOBBER | NC_NETCDF4, &cdfid));

    int dim_lines = -1, dim_pixels = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "number_of_lines", 4, &dim_lines));
    ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "pixels_per_line", 5, &dim_pixels));
    int dims[2] = {dim_lines, dim_pixels};

    int grp_geo = -1, grp_nav = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(cdfid, "geophysical_data", &grp_geo));
    ASSERT_EQ(NC_NOERR, nc_def_grp(cdfid, "navigation_data", &grp_nav));

    int vid_data = -1, vid_lon = -1, vid_lat = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(grp_geo, "aot_862", NC_SHORT, 2, dims, &vid_data));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(grp_nav, "longitude", NC_FLOAT, 2, dims, &vid_lon));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(grp_nav, "latitude", NC_FLOAT, 2, dims, &vid_lat));

    ASSERT_EQ(NC_NOERR, nc_enddef(cdfid));
    ASSERT_EQ(NC_NOERR, nc_close(cdfid));

    const std::string sds_name =
        std::string("NETCDF:\"") + fn + "\":/geophysical_data/aot_862";
    auto poDS =
        std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
            sds_name.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    EXPECT_NE(nullptr, poDS->GetMetadata("GEOLOCATION"));

    poDS.reset();
    VSIUnlink(fn.c_str());
  }

  // 2. NASA EMIT Geolocation
  {
    const std::string fn =
        (std::filesystem::path(::testing::TempDir()) / "nasa_emit.nc").string();
    int cdfid = -1;
    ASSERT_EQ(NC_NOERR, nc_create(fn.c_str(), NC_CLOBBER | NC_NETCDF4, &cdfid));

    int dim_down = -1, dim_cross = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "downtrack", 4, &dim_down));
    ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "crosstrack", 5, &dim_cross));
    int dims[2] = {dim_down, dim_cross};

    int vid_refl = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(cdfid, "reflectance", NC_FLOAT, 2, dims, &vid_refl));

    int grp_loc = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(cdfid, "location", &grp_loc));
    int vid_lon = -1, vid_lat = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(grp_loc, "lon", NC_DOUBLE, 2, dims, &vid_lon));
    ASSERT_EQ(NC_NOERR,
              PutTextAtt(grp_loc, vid_lon, "long_name", "Longitude (WGS-84)"));
    ASSERT_EQ(NC_NOERR, PutTextAtt(grp_loc, vid_lon, "units", "degrees east"));

    ASSERT_EQ(NC_NOERR,
              nc_def_var(grp_loc, "lat", NC_DOUBLE, 2, dims, &vid_lat));
    ASSERT_EQ(NC_NOERR,
              PutTextAtt(grp_loc, vid_lat, "long_name", "Latitude (WGS-84)"));
    ASSERT_EQ(NC_NOERR, PutTextAtt(grp_loc, vid_lat, "units", "degrees north"));

    ASSERT_EQ(NC_NOERR, nc_enddef(cdfid));
    ASSERT_EQ(NC_NOERR, nc_close(cdfid));

    const std::string sds_name =
        std::string("NETCDF:\"") + fn + "\":reflectance";
    auto poDS =
        std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
            sds_name.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    EXPECT_NE(nullptr, poDS->GetMetadata("GEOLOCATION"));

    poDS.reset();
    VSIUnlink(fn.c_str());
  }
}

TEST_F(NetCDFDatasetTest, VectorLayersFromConventions) {
  // 1. featureType = "point"
  {
    const std::string fn =
        (std::filesystem::path(::testing::TempDir()) / "point_feat.nc")
            .string();
    int cdfid = -1;
    ASSERT_EQ(NC_NOERR, nc_create(fn.c_str(), NC_CLOBBER | NC_NETCDF4, &cdfid));

    const char* ft = "point";
    ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, NC_GLOBAL, "featureType", ft));

    int dim_rec = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "record", 3, &dim_rec));

    int lon_vid = -1, lat_vid = -1, val_vid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(cdfid, "lon", NC_DOUBLE, 1, &dim_rec, &lon_vid));
    ASSERT_EQ(NC_NOERR,
              PutTextAtt(cdfid, lon_vid, "standard_name", "longitude"));
    ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, lon_vid, "units", "degrees_east"));

    ASSERT_EQ(NC_NOERR,
              nc_def_var(cdfid, "lat", NC_DOUBLE, 1, &dim_rec, &lat_vid));
    ASSERT_EQ(NC_NOERR,
              PutTextAtt(cdfid, lat_vid, "standard_name", "latitude"));
    ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, lat_vid, "units", "degrees_north"));

    ASSERT_EQ(NC_NOERR,
              nc_def_var(cdfid, "temp", NC_FLOAT, 1, &dim_rec, &val_vid));

    ASSERT_EQ(NC_NOERR, nc_enddef(cdfid));

    double lons[3] = {10.0, 20.0, 30.0};
    double lats[3] = {40.0, 50.0, 60.0};
    float temps[3] = {15.5f, 25.5f, 35.5f};
    ASSERT_EQ(NC_NOERR, nc_put_var_double(cdfid, lon_vid, lons));
    ASSERT_EQ(NC_NOERR, nc_put_var_double(cdfid, lat_vid, lats));
    ASSERT_EQ(NC_NOERR, nc_put_var_float(cdfid, val_vid, temps));

    ASSERT_EQ(NC_NOERR, nc_close(cdfid));

    auto poDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(fn.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    EXPECT_EQ(1, poDS->GetLayerCount());
    auto poLayer = poDS->GetLayer(0);
    ASSERT_NE(nullptr, poLayer);
    EXPECT_EQ(3, poLayer->GetFeatureCount(true));

    auto feat = std::unique_ptr<OGRFeature>(poLayer->GetNextFeature());
    ASSERT_NE(nullptr, feat);
    EXPECT_NE(nullptr, feat->GetGeometryRef());
    EXPECT_EQ(wkbPoint, feat->GetGeometryRef()->getGeometryType());

    poDS.reset();
    VSIUnlink(fn.c_str());
  }

  // 2. featureType = "profile"
  {
    const std::string fn =
        (std::filesystem::path(::testing::TempDir()) / "profile_feat.nc")
            .string();
    int cdfid = -1;
    ASSERT_EQ(NC_NOERR, nc_create(fn.c_str(), NC_CLOBBER | NC_NETCDF4, &cdfid));

    const char* ft = "profile";
    ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, NC_GLOBAL, "featureType", ft));

    int dim_prof = -1, dim_obs = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "profile", 2, &dim_prof));
    ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "obs", 4, &dim_obs));

    int lon_vid = -1, lat_vid = -1, parent_vid = -1, temp_vid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(cdfid, "lon", NC_DOUBLE, 1, &dim_prof, &lon_vid));
    ASSERT_EQ(NC_NOERR,
              PutTextAtt(cdfid, lon_vid, "standard_name", "longitude"));

    ASSERT_EQ(NC_NOERR,
              nc_def_var(cdfid, "lat", NC_DOUBLE, 1, &dim_prof, &lat_vid));
    ASSERT_EQ(NC_NOERR,
              PutTextAtt(cdfid, lat_vid, "standard_name", "latitude"));

    ASSERT_EQ(NC_NOERR, nc_def_var(cdfid, "parentIndex", NC_INT, 1, &dim_obs,
                                   &parent_vid));
    ASSERT_EQ(NC_NOERR,
              PutTextAtt(cdfid, parent_vid, "instance_dimension", "profile"));

    ASSERT_EQ(NC_NOERR,
              nc_def_var(cdfid, "temp", NC_FLOAT, 1, &dim_obs, &temp_vid));

    ASSERT_EQ(NC_NOERR, nc_enddef(cdfid));

    double lons[2] = {10.0, 20.0};
    double lats[2] = {40.0, 50.0};
    int parents[4] = {0, 0, 1, 1};
    float temps[4] = {15.5f, 16.5f, 25.5f, 26.5f};
    ASSERT_EQ(NC_NOERR, nc_put_var_double(cdfid, lon_vid, lons));
    ASSERT_EQ(NC_NOERR, nc_put_var_double(cdfid, lat_vid, lats));
    ASSERT_EQ(NC_NOERR, nc_put_var_int(cdfid, parent_vid, parents));
    ASSERT_EQ(NC_NOERR, nc_put_var_float(cdfid, temp_vid, temps));

    ASSERT_EQ(NC_NOERR, nc_close(cdfid));

    auto poDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(fn.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    EXPECT_GE(poDS->GetLayerCount(), 1);

    poDS.reset();
    VSIUnlink(fn.c_str());
  }
}

TEST_F(NetCDFDatasetTest, RasterBandPropertiesAndSerialize) {
  auto poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  const std::string fn =
      (std::filesystem::path(::testing::TempDir()) / "band_props.nc").string();

  char** papszOptions = nullptr;
  papszOptions = CSLSetNameValue(papszOptions, "FORMAT", "NC4");
  auto poDS = std::unique_ptr<GDALDataset>(
      poDriver->Create(fn.c_str(), 10, 10, 2, GDT_Int64, papszOptions));
  CSLDestroy(papszOptions);
  ASSERT_NE(nullptr, poDS);

  auto poBand = poDS->GetRasterBand(1);
  ASSERT_NE(nullptr, poBand);

  // UnitType, Offset, Scale
  EXPECT_EQ(CE_None, poBand->SetUnitType("meters"));
  EXPECT_STREQ("meters", poBand->GetUnitType());

  EXPECT_EQ(CE_None, poBand->SetOffset(10.5));
  int bSuccess = 0;
  EXPECT_DOUBLE_EQ(10.5, poBand->GetOffset(&bSuccess));
  EXPECT_TRUE(bSuccess);

  EXPECT_EQ(CE_None, poBand->SetScale(2.5));
  EXPECT_DOUBLE_EQ(2.5, poBand->GetScale(&bSuccess));
  EXPECT_TRUE(bSuccess);

  // SetMetadataItem and SetMetadata
  poBand->SetMetadataItem("BAND_TAG", "VAL");
  char** papszMD = nullptr;
  papszMD = CSLAddString(papszMD, "KEY1=VAL1");
  papszMD = CSLAddString(papszMD, "KEY2=VAL2");
  poBand->SetMetadata(papszMD);
  CSLDestroy(papszMD);

  // NoData values: Double, Int64, UInt64
  EXPECT_EQ(CE_None, poBand->SetNoDataValue(999.0));
  int bNoDataSuccess = 0;
  EXPECT_DOUBLE_EQ(999.0, poBand->GetNoDataValue(&bNoDataSuccess));
  EXPECT_TRUE(bNoDataSuccess);

  EXPECT_EQ(CE_None, poBand->SetNoDataValueAsInt64(1234567890123LL));
  int64_t nd64 = poBand->GetNoDataValueAsInt64(&bSuccess);
  EXPECT_TRUE(bSuccess);
  EXPECT_EQ(1234567890123LL, nd64);

  EXPECT_EQ(CE_None, poBand->SetNoDataValueAsUInt64(1234567890123ULL));
  uint64_t und64 = poBand->GetNoDataValueAsUInt64(&bSuccess);
  EXPECT_TRUE(bSuccess);
  EXPECT_EQ(1234567890123ULL, und64);

  // Statistics and Histograms -> SerializeToXML
  poBand->SetMetadataItem("STATISTICS_MINIMUM", "1.0");
  poBand->SetMetadataItem("STATISTICS_MAXIMUM", "10.0");
  poBand->SetMetadataItem("STATISTICS_MEAN", "5.5");
  poBand->SetMetadataItem("STATISTICS_STDDEV", "2.5");
  GUIntBig panHist[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  EXPECT_EQ(CE_None, poBand->SetDefaultHistogram(1.0, 10.0, 10, panHist));

  EXPECT_EQ(CE_None, poDS->FlushCache());

  poDS.reset();
  VSIUnlink(fn.c_str());

  // Test valid_range, valid_min, valid_max, _Unsigned on NC_SHORT and NC_BYTE
  {
    const std::string range_fn =
        (std::filesystem::path(::testing::TempDir()) / "range_test.nc")
            .string();
    int cdfid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_create(range_fn.c_str(), NC_CLOBBER | NC_NETCDF4, &cdfid));
    int dy = -1, dx = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "y", 2, &dy));
    ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "x", 2, &dx));
    int d[2] = {dy, dx};

    int v_short = -1, v_byte = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(cdfid, "s_var", NC_SHORT, 2, d, &v_short));
    const char* tru = "true";
    ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, v_short, "_Unsigned", tru));
    short vrange[2] = {0, 1000};
    ASSERT_EQ(NC_NOERR, nc_put_att_short(cdfid, v_short, "valid_range",
                                         NC_SHORT, 2, vrange));

    ASSERT_EQ(NC_NOERR, nc_def_var(cdfid, "b_var", NC_BYTE, 2, d, &v_byte));
    signed char brange[2] = {0, 127};
    ASSERT_EQ(NC_NOERR, nc_put_att_schar(cdfid, v_byte, "valid_range", NC_BYTE,
                                         2, brange));

    ASSERT_EQ(NC_NOERR, nc_enddef(cdfid));
    ASSERT_EQ(NC_NOERR, nc_close(cdfid));

    auto poRangeDS =
        std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
            range_fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poRangeDS);

    // Open subdataset s_var
    const std::string sds_short =
        std::string("NETCDF:\"") + range_fn + "\":s_var";
    auto poShortDS =
        std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
            sds_short.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poShortDS);
    EXPECT_EQ(GDT_UInt16, poShortDS->GetRasterBand(1)->GetRasterDataType());

    poRangeDS.reset();
    poShortDS.reset();
    VSIUnlink(range_fn.c_str());
  }
}

TEST_F(NetCDFDatasetTest, ProjectionsWritingAndCreationOptions) {
  auto poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  // 1. Creation options testing.  Unsupported FORMAT / COMPRESS values are
  // reported as CPLE_NotSupported but the driver falls back to defaults, so
  // creation still succeeds in every case.
  struct OptTest {
    const char* format;
    const char* compress;
    const char* zlevel;
    const char* chunking;
    const char* write_tags;
    const char* write_lonlat;
    const char* write_bottomup;
    bool expect_not_supported_error;
    // Without lon/lat variables and GDAL tags, the geographic CRS cannot be
    // recovered when reopening.
    bool expect_srs_on_reopen;
  } opt_tests[] = {
      {"NC4C", "NONE", nullptr, "YES", "YES", "YES", "YES", false, true},
      {"NC", "DEFLATE", "5", "NO", "NO", "NO", "NO", false, false},
      {"NC2", "NONE", nullptr, "YES", "YES", "IF_NEEDED", "YES", false, true},
      {"INVALID_FMT", "NONE", "99", "YES", "YES", "YES", "YES", true, true},
      {"NC4", "INVALID_COMP", nullptr, "YES", "YES", "YES", "YES", true, true},
  };

  for (size_t idx = 0; idx < sizeof(opt_tests) / sizeof(opt_tests[0]); ++idx) {
    const auto& ot = opt_tests[idx];
    SCOPED_TRACE(std::string(ot.format) + "/" + ot.compress);
    const std::string fn =
        (std::filesystem::path(::testing::TempDir()) /
         (std::string("opt_test_") + std::to_string(idx) + ".nc"))
            .string();

    char** papszOptions = nullptr;
    if (ot.format)
      papszOptions = CSLSetNameValue(papszOptions, "FORMAT", ot.format);
    if (ot.compress)
      papszOptions = CSLSetNameValue(papszOptions, "COMPRESS", ot.compress);
    if (ot.zlevel)
      papszOptions = CSLSetNameValue(papszOptions, "ZLEVEL", ot.zlevel);
    if (ot.chunking)
      papszOptions = CSLSetNameValue(papszOptions, "CHUNKING", ot.chunking);
    if (ot.write_tags)
      papszOptions =
          CSLSetNameValue(papszOptions, "WRITE_GDAL_TAGS", ot.write_tags);
    if (ot.write_lonlat)
      papszOptions =
          CSLSetNameValue(papszOptions, "WRITE_LONLAT", ot.write_lonlat);
    if (ot.write_bottomup)
      papszOptions =
          CSLSetNameValue(papszOptions, "WRITE_BOTTOMUP", ot.write_bottomup);

    std::unique_ptr<GDALDataset> poDS;
    {
      CPLErrorHandlerPusher oRecorder(RecordNotSupportedHandler);
      g_saw_not_supported_error = false;
      poDS.reset(
          poDriver->Create(fn.c_str(), 4, 4, 1, GDT_Float32, papszOptions));
      EXPECT_EQ(ot.expect_not_supported_error, g_saw_not_supported_error);
    }
    CSLDestroy(papszOptions);
    ASSERT_NE(nullptr, poDS);

    OGRSpatialReference oSRS;
    oSRS.importFromEPSG(4326);
    EXPECT_EQ(CE_None, poDS->SetSpatialRef(&oSRS));
    GDALGeoTransform gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    EXPECT_EQ(CE_None, poDS->SetGeoTransform(gt));
    poDS.reset();

    // Reopen and verify the SRS was written.
    auto poReadDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poReadDS);
    if (ot.expect_srs_on_reopen) {
      ASSERT_NE(nullptr, poReadDS->GetSpatialRef());
      EXPECT_TRUE(poReadDS->GetSpatialRef()->IsGeographic());
    } else {
      EXPECT_EQ(nullptr, poReadDS->GetSpatialRef());
    }
    poReadDS.reset();
    EXPECT_EQ(0, VSIUnlink(fn.c_str()));
  }

  // 2. NCDFGetProjectedCFUnit
  OGRSpatialReference oSRSUtm;
  oSRSUtm.importFromEPSG(32631);
  EXPECT_EQ("m", NCDFGetProjectedCFUnit(&oSRSUtm));

  OGRSpatialReference oSRSFt;
  oSRSFt.importFromEPSG(2230);
  EXPECT_FALSE(NCDFGetProjectedCFUnit(&oSRSFt).empty());

  // 3. NCDFWriteSRSVariable
  {
    const std::string srs_fn =
        (std::filesystem::path(::testing::TempDir()) / "srs_var_test.nc")
            .string();
    int test_cdfid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_create(srs_fn.c_str(), NC_CLOBBER | NC_NETCDF4, &test_cdfid));

    char* pszProjName = nullptr;
    int v1 = NCDFWriteSRSVariable(test_cdfid, &oSRSUtm, &pszProjName, true);
    EXPECT_GE(v1, 0);
    CPLFree(pszProjName);
    pszProjName = nullptr;

    // Same SRS -> returns same variable
    int v2 = NCDFWriteSRSVariable(test_cdfid, &oSRSUtm, &pszProjName, true);
    EXPECT_EQ(v1, v2);
    CPLFree(pszProjName);
    pszProjName = nullptr;

    // Different SRS -> returns new variable
    int v3 = NCDFWriteSRSVariable(test_cdfid, &oSRSFt, &pszProjName, true);
    EXPECT_NE(v1, v3);
    CPLFree(pszProjName);

    ASSERT_EQ(NC_NOERR, nc_close(test_cdfid));
    VSIUnlink(srs_fn.c_str());
  }

  // 4. ASSUME_LONGLAT open option
  {
    const std::string fn =
        (std::filesystem::path(::testing::TempDir()) / "assume_ll.nc").string();
    int cdfid = -1;
    ASSERT_EQ(NC_NOERR, nc_create(fn.c_str(), NC_CLOBBER | NC_NETCDF4, &cdfid));
    int dy = -1, dx = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "lat", 3, &dy));
    ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "lon", 3, &dx));
    int d[2] = {dy, dx};
    int vid_data = -1, vid_lat = -1, vid_lon = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(cdfid, "lat", NC_DOUBLE, 1, &dy, &vid_lat));
    ASSERT_EQ(NC_NOERR, nc_def_var(cdfid, "lon", NC_DOUBLE, 1, &dx, &vid_lon));
    ASSERT_EQ(NC_NOERR, nc_def_var(cdfid, "data", NC_FLOAT, 2, d, &vid_data));
    ASSERT_EQ(NC_NOERR, nc_enddef(cdfid));

    double lats[3] = {10.0, 11.0, 12.0};
    double lons[3] = {20.0, 21.0, 22.0};
    ASSERT_EQ(NC_NOERR, nc_put_var_double(cdfid, vid_lat, lats));
    ASSERT_EQ(NC_NOERR, nc_put_var_double(cdfid, vid_lon, lons));
    ASSERT_EQ(NC_NOERR, nc_close(cdfid));

    const char* papszOpenOptions[] = {"ASSUME_LONGLAT=YES", nullptr};
    auto poOpenDS =
        std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
            fn.c_str(), GDAL_OF_RASTER, nullptr, papszOpenOptions, nullptr)));
    ASSERT_NE(nullptr, poOpenDS);
    EXPECT_NE(nullptr, poOpenDS->GetSpatialRef());

    poOpenDS.reset();
    VSIUnlink(fn.c_str());
  }

  // 5. GDAL_NETCDF_CENTERLONG_180=YES
  {
    const std::string fn =
        (std::filesystem::path(::testing::TempDir()) / "center_long.nc")
            .string();
    int cdfid = -1;
    ASSERT_EQ(NC_NOERR, nc_create(fn.c_str(), NC_CLOBBER | NC_NETCDF4, &cdfid));
    int dy = -1, dx = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "lat", 3, &dy));
    ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "lon", 3, &dx));
    int d[2] = {dy, dx};
    int vid_data = -1, vid_lat = -1, vid_lon = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(cdfid, "lat", NC_DOUBLE, 1, &dy, &vid_lat));
    ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, vid_lat, "units", "degrees_north"));
    ASSERT_EQ(NC_NOERR, nc_def_var(cdfid, "lon", NC_DOUBLE, 1, &dx, &vid_lon));
    ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, vid_lon, "units", "degrees_east"));
    ASSERT_EQ(NC_NOERR, nc_def_var(cdfid, "data", NC_FLOAT, 2, d, &vid_data));
    ASSERT_EQ(NC_NOERR, nc_enddef(cdfid));

    double lats[3] = {10.0, 11.0, 12.0};
    double lons[3] = {190.0, 200.0, 210.0};
    ASSERT_EQ(NC_NOERR, nc_put_var_double(cdfid, vid_lat, lats));
    ASSERT_EQ(NC_NOERR, nc_put_var_double(cdfid, vid_lon, lons));
    ASSERT_EQ(NC_NOERR, nc_close(cdfid));

    std::unique_ptr<GDALDataset> poOpenDS;
    {
      CPLConfigOptionSetter oSetter("GDAL_NETCDF_CENTERLONG_180", "YES",
                                    /*bSetOnlyIfUndefined=*/false);
      poOpenDS.reset(static_cast<GDALDataset*>(
          GDALOpenEx(fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    }
    ASSERT_NE(nullptr, poOpenDS);
    GDALGeoTransform gt;
    EXPECT_EQ(CE_None, poOpenDS->GetGeoTransform(gt));
    EXPECT_LT(gt[0], 0.0);  // Shifted by -360

    poOpenDS.reset();
    VSIUnlink(fn.c_str());
  }
}

TEST_F(NetCDFDatasetTest, GeolocationMetadataAndCreateCopy) {
  auto poMemDriver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, poMemDriver);
  auto poTiffDriver = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, poTiffDriver);
  auto poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  // Create X and Y geolocation datasets in memory
  const std::string x_fn = "/vsimem/geoloc_x.tif";
  const std::string y_fn = "/vsimem/geoloc_y.tif";
  auto poXDS = std::unique_ptr<GDALDataset>(
      poTiffDriver->Create(x_fn.c_str(), 4, 4, 1, GDT_Float64, nullptr));
  auto poYDS = std::unique_ptr<GDALDataset>(
      poTiffDriver->Create(y_fn.c_str(), 4, 4, 1, GDT_Float64, nullptr));
  ASSERT_NE(nullptr, poXDS);
  ASSERT_NE(nullptr, poYDS);

  std::vector<double> x_vals(16), y_vals(16);
  for (int i = 0; i < 16; ++i) {
    x_vals[i] = 10.0 + (i % 4) * 0.5;
    y_vals[i] = 20.0 + (i / 4) * 0.5;
  }
  EXPECT_EQ(CE_None, poXDS->GetRasterBand(1)->RasterIO(
                         GF_Write, 0, 0, 4, 4, x_vals.data(), 4, 4, GDT_Float64,
                         0, 0, nullptr));
  EXPECT_EQ(CE_None, poYDS->GetRasterBand(1)->RasterIO(
                         GF_Write, 0, 0, 4, 4, y_vals.data(), 4, 4, GDT_Float64,
                         0, 0, nullptr));
  // Flush TIFF files so they are readable via GDALDataset::Open
  poXDS.reset();
  poYDS.reset();

  // Create source dataset with GEOLOCATION domain
  const std::string src_fn = "/vsimem/geoloc_src.tif";
  auto poSrcDS = std::unique_ptr<GDALDataset>(
      poMemDriver->Create(src_fn.c_str(), 4, 4, 1, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, poSrcDS);

  char** papszGeoloc = nullptr;
  papszGeoloc = CSLSetNameValue(papszGeoloc, "X_DATASET", x_fn.c_str());
  papszGeoloc = CSLSetNameValue(papszGeoloc, "X_BAND", "1");
  papszGeoloc = CSLSetNameValue(papszGeoloc, "Y_DATASET", y_fn.c_str());
  papszGeoloc = CSLSetNameValue(papszGeoloc, "Y_BAND", "1");
  papszGeoloc = CSLSetNameValue(papszGeoloc, "PIXEL_OFFSET", "0");
  papszGeoloc = CSLSetNameValue(papszGeoloc, "LINE_OFFSET", "0");
  papszGeoloc = CSLSetNameValue(papszGeoloc, "PIXEL_STEP", "1");
  papszGeoloc = CSLSetNameValue(papszGeoloc, "LINE_STEP", "1");
  poSrcDS->SetMetadata(papszGeoloc, "GEOLOCATION");
  CSLDestroy(papszGeoloc);

  // CreateCopy to NetCDF
  const std::string out_fn =
      (std::filesystem::path(::testing::TempDir()) / "geoloc_out.nc").string();
  char** papszOptions = nullptr;
  papszOptions = CSLSetNameValue(papszOptions, "FORMAT", "NC4");
  papszOptions = CSLSetNameValue(papszOptions, "WRITE_LONLAT", "YES");
  auto poCopyDS = std::unique_ptr<GDALDataset>(
      poDriver->CreateCopy(out_fn.c_str(), poSrcDS.get(), FALSE, papszOptions,
                           GDALDummyProgress, nullptr));
  CSLDestroy(papszOptions);
  ASSERT_NE(nullptr, poCopyDS);
  poCopyDS.reset();

  // Reopen and check geolocation metadata
  auto poOpenDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
      GDALOpenEx(out_fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, poOpenDS);
  EXPECT_EQ(4, poOpenDS->GetRasterXSize());
  EXPECT_EQ(4, poOpenDS->GetRasterYSize());
  EXPECT_NE(nullptr, poOpenDS->GetMetadata("GEOLOCATION"));
  poOpenDS.reset();

  VSIUnlink(out_fn.c_str());
  VSIUnlink(x_fn.c_str());
  VSIUnlink(y_fn.c_str());
  VSIUnlink(x_fn.c_str());
  VSIUnlink(y_fn.c_str());
  VSIUnlink(src_fn.c_str());
}

TEST_F(NetCDFDatasetTest, NCDFPutAttrAndMetadataItems) {
  auto poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  const std::string fn =
      (std::filesystem::path(::testing::TempDir()) / "metadata_attrs.nc")
          .string();
  char** papszOptions = nullptr;
  papszOptions = CSLSetNameValue(papszOptions, "FORMAT", "NC4");
  auto poDS = std::unique_ptr<GDALDataset>(
      poDriver->Create(fn.c_str(), 4, 4, 1, GDT_Int32, papszOptions));
  CSLDestroy(papszOptions);
  ASSERT_NE(nullptr, poDS);

  // Exercise NCDFPutAttr via SetMetadataItem with various array types
  poDS->SetMetadataItem("NC_GLOBAL#int_arr", "{1, 2, 3}");
  poDS->SetMetadataItem("NC_GLOBAL#uint_arr", "{100, 200}");
  poDS->SetMetadataItem("NC_GLOBAL#flt_arr", "{1.5, 2.5}");
  poDS->SetMetadataItem("NC_GLOBAL#dbl_arr", "{1.123456789, 2.123456789}");
  poDS->SetMetadataItem("NC_GLOBAL#str_arr", "{\"hello\", \"world\"}");
  poDS->SetMetadataItem("NC_GLOBAL#single_str", "test");
  poDS->GetRasterBand(1)->SetMetadataItem("band_int_arr", "{10, 20}");

  char** papszMD = nullptr;
  papszMD = CSLSetNameValue(papszMD, "KEY_A", "VAL_A");
  papszMD = CSLSetNameValue(papszMD, "KEY_B", "VAL_B");
  poDS->SetMetadata(papszMD);
  CSLDestroy(papszMD);

  poDS.reset();

  auto poOpenDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
      GDALOpenEx(fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, poOpenDS);
  EXPECT_NE(nullptr, poOpenDS->GetMetadataItem("NC_GLOBAL#single_str"));
  poOpenDS.reset();

  VSIUnlink(fn.c_str());
}

TEST_F(NetCDFDatasetTest, LinearUnitsAndGeospatialBoundsCRS) {
  const std::string fn =
      (std::filesystem::path(::testing::TempDir()) / "linear_units.nc")
          .string();
  int cdfid = -1;
  ASSERT_EQ(NC_NOERR, nc_create(fn.c_str(), NC_CLOBBER | NC_NETCDF4, &cdfid));

  int dy = -1, dx = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "y", 4, &dy));
  ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "x", 5, &dx));
  int d[2] = {dy, dx};

  int vid_data = -1, vid_x = -1, vid_y = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(cdfid, "x", NC_DOUBLE, 1, &dx, &vid_x));
  ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, vid_x, "units", "km"));
  ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, vid_x, "standard_name",
                                 "projection_x_coordinate"));

  ASSERT_EQ(NC_NOERR, nc_def_var(cdfid, "y", NC_DOUBLE, 1, &dy, &vid_y));
  ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, vid_y, "units", "km"));
  ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, vid_y, "standard_name",
                                 "projection_y_coordinate"));

  ASSERT_EQ(NC_NOERR, nc_def_var(cdfid, "data", NC_FLOAT, 2, d, &vid_data));

  // Grid mapping variable
  int vid_map = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(cdfid, "transverse_mercator", NC_CHAR, 0,
                                 nullptr, &vid_map));
  ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, vid_map, "grid_mapping_name",
                                 "transverse_mercator"));
  double val = 0.0;
  ASSERT_EQ(NC_NOERR,
            nc_put_att_double(cdfid, vid_map, "longitude_of_central_meridian",
                              NC_DOUBLE, 1, &val));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_double(cdfid, vid_map, "latitude_of_projection_origin",
                              NC_DOUBLE, 1, &val));
  double scale = 0.9996;
  ASSERT_EQ(NC_NOERR, nc_put_att_double(cdfid, vid_map,
                                        "scale_factor_at_central_meridian",
                                        NC_DOUBLE, 1, &scale));
  ASSERT_EQ(NC_NOERR, nc_put_att_double(cdfid, vid_map, "false_easting",
                                        NC_DOUBLE, 1, &val));
  ASSERT_EQ(NC_NOERR, nc_put_att_double(cdfid, vid_map, "false_northing",
                                        NC_DOUBLE, 1, &val));

  ASSERT_EQ(NC_NOERR,
            PutTextAtt(cdfid, vid_data, "grid_mapping", "transverse_mercator"));

  // Global attribute geospatial_bounds_crs
  const char* gbcrs = "EPSG:32631";
  ASSERT_EQ(NC_NOERR,
            PutTextAtt(cdfid, NC_GLOBAL, "geospatial_bounds_crs", gbcrs));

  ASSERT_EQ(NC_NOERR, nc_enddef(cdfid));

  double x_coords[5] = {1.0, 2.0, 3.0, 4.0, 5.0};
  double y_coords[4] = {10.0, 9.0, 8.0, 7.0};
  ASSERT_EQ(NC_NOERR, nc_put_var_double(cdfid, vid_x, x_coords));
  ASSERT_EQ(NC_NOERR, nc_put_var_double(cdfid, vid_y, y_coords));

  ASSERT_EQ(NC_NOERR, nc_close(cdfid));

  auto poOpenDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
      GDALOpenEx(fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, poOpenDS);
  GDALGeoTransform gt;
  EXPECT_EQ(CE_None, poOpenDS->GetGeoTransform(gt));
  // dx should be 1.0 km * 1000.0 m/km = 1000.0
  EXPECT_DOUBLE_EQ(1000.0, gt[1]);

  poOpenDS.reset();
  VSIUnlink(fn.c_str());
}

TEST_F(NetCDFDatasetTest, YXBandOrderAnd3DCoordinates) {
  const std::string fn =
      (std::filesystem::path(::testing::TempDir()) / "yx_band_3d.nc").string();
  int cdfid = -1;
  ASSERT_EQ(NC_NOERR, nc_create(fn.c_str(), NC_CLOBBER | NC_NETCDF4, &cdfid));

  int dim_y = -1, dim_x = -1, dim_b = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "y", 3, &dim_y));
  ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "x", 4, &dim_x));
  ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "band", 2, &dim_b));

  int dims_2d[2] = {dim_y, dim_x};
  int dims_3d[3] = {dim_y, dim_x, dim_b};

  int vid_lat = -1, vid_lon = -1, vid_3d = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(cdfid, "lat", NC_DOUBLE, 2, dims_2d, &vid_lat));
  ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, vid_lat, "units", "degrees_north"));
  ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, vid_lat, "standard_name", "latitude"));

  ASSERT_EQ(NC_NOERR,
            nc_def_var(cdfid, "lon", NC_DOUBLE, 2, dims_2d, &vid_lon));
  ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, vid_lon, "units", "degrees_east"));
  ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, vid_lon, "standard_name", "longitude"));

  ASSERT_EQ(NC_NOERR, nc_def_var(cdfid, "cube", NC_FLOAT, 3, dims_3d, &vid_3d));
  const char* coords = "lat lon";
  ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, vid_3d, "coordinates", coords));

  ASSERT_EQ(NC_NOERR, nc_enddef(cdfid));
  ASSERT_EQ(NC_NOERR, nc_close(cdfid));

  const std::string sds_name = std::string("NETCDF:\"") + fn + "\":cube";
  auto poOpenDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
      GDALOpenEx(sds_name.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, poOpenDS);
  EXPECT_EQ(4, poOpenDS->GetRasterXSize());
  EXPECT_EQ(3, poOpenDS->GetRasterYSize());
  EXPECT_EQ(2, poOpenDS->GetRasterCount());

  poOpenDS.reset();
  VSIUnlink(fn.c_str());
}

TEST_F(NetCDFDatasetTest, WriterConfigFileAndLayerOptions) {
  auto poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  // netCDFWriterConfiguration::Parse() expects a <Configuration> root and reads
  // the file through VSIFOpenL, so write a real file in TempDir.
  const std::string cfg_fn =
      (std::filesystem::path(::testing::TempDir()) / "netcdf_writer_cfg.xml")
          .string();
  const char* xml =
      "<Configuration>\n"
      "  <Layer name=\"my_layer\" netcdf_name=\"renamed_layer\"/>\n"
      "</Configuration>\n";
  {
    VSILFILE* fp = VSIFOpenL(cfg_fn.c_str(), "wb");
    ASSERT_NE(nullptr, fp);
    ASSERT_EQ(strlen(xml), VSIFWriteL(xml, 1, strlen(xml), fp));
    ASSERT_EQ(0, VSIFCloseL(fp));
  }

  const std::string dir =
      (std::filesystem::path(::testing::TempDir()) / "cfg_sep_files").string();
  ASSERT_EQ(0, VSIMkdir(dir.c_str(), 0755));
  const std::string master_fn =
      (std::filesystem::path(dir) / "master.nc").string();

  char** papszOptions = nullptr;
  papszOptions = CSLSetNameValue(papszOptions, "CONFIG_FILE", cfg_fn.c_str());
  papszOptions =
      CSLSetNameValue(papszOptions, "MULTIPLE_LAYERS", "SEPARATE_FILES");
  papszOptions = CSLSetNameValue(papszOptions, "GEOMETRY_ENCODING", "WKT");
  auto poDS = std::unique_ptr<GDALDataset>(
      poDriver->Create(master_fn.c_str(), 0, 0, 0, GDT_Unknown, papszOptions));
  CSLDestroy(papszOptions);
  ASSERT_NE(nullptr, poDS);

  // Layer with renaming from config
  auto poLayer1 = poDS->CreateLayer("my_layer", nullptr, wkbPoint, nullptr);
  ASSERT_NE(nullptr, poLayer1);

  // Layer with illegal characters for filename
  {
    CPLErrorHandlerPusher oQuiet(CPLQuietErrorHandler);
    auto poLayerBad =
        poDS->CreateLayer("illegal:char*layer", nullptr, wkbPoint, nullptr);
    EXPECT_EQ(nullptr, poLayerBad);
  }

  poDS.reset();

  // SEPARATE_FILES makes `master_fn` a directory; the config renamed the
  // layer file from my_layer.nc to renamed_layer.nc.
  VSIStatBufL sStat;
  EXPECT_EQ(0, VSIStatL((std::filesystem::path(master_fn) / "renamed_layer.nc")
                            .string()
                            .c_str(),
                        &sStat));
  EXPECT_NE(
      0,
      VSIStatL(
          (std::filesystem::path(master_fn) / "my_layer.nc").string().c_str(),
          &sStat));
  EXPECT_EQ(0, VSIRmdirRecursive(dir.c_str()));
  EXPECT_EQ(0, VSIUnlink(cfg_fn.c_str()));
}

TEST_F(NetCDFDatasetTest, ReadOnlyPamSerialization) {
  auto poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  const std::string fn =
      (std::filesystem::path(::testing::TempDir()) / "ro_pam.nc").string();
  char** papszOptions = nullptr;
  papszOptions = CSLSetNameValue(papszOptions, "FORMAT", "NC4");
  auto poDS = std::unique_ptr<GDALDataset>(
      poDriver->Create(fn.c_str(), 4, 4, 1, GDT_Int16, papszOptions));
  CSLDestroy(papszOptions);
  ASSERT_NE(nullptr, poDS);
  poDS.reset();

  const std::string pam_fn = fn + ".aux.xml";
  {
    CPLConfigOptionSetter oSetter("GDAL_PAM_ENABLED", "YES",
                                  /*bSetOnlyIfUndefined=*/false);

    // Open in update mode, set statistics and histograms in PAM
    auto poRODS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(fn.c_str(), GDAL_OF_UPDATE | GDAL_OF_RASTER, nullptr,
                   nullptr, nullptr)));
    ASSERT_NE(nullptr, poRODS);

    auto poBand = poRODS->GetRasterBand(1);
    ASSERT_NE(nullptr, poBand);

    poBand->SetMetadataItem("STATISTICS_MINIMUM", "1.0");
    poBand->SetMetadataItem("STATISTICS_MAXIMUM", "10.0");
    poBand->SetMetadataItem("STATISTICS_MEAN", "5.5");
    poBand->SetMetadataItem("STATISTICS_STDDEV", "2.5");
    GUIntBig panHist[5] = {1, 2, 3, 4, 5};
    EXPECT_EQ(CE_None, poBand->SetDefaultHistogram(1.0, 10.0, 5, panHist));

    // Closing dataset with PAM dirty flushes to .aux.xml via SerializeToXML
    poRODS.reset();

    VSIStatBufL sStat;
    EXPECT_EQ(0, VSIStatL(pam_fn.c_str(), &sStat));

    // Reopen and verify PAM statistics were loaded
    auto poReopenDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poReopenDS);
    EXPECT_STREQ("1.0", poReopenDS->GetRasterBand(1)->GetMetadataItem(
                            "STATISTICS_MINIMUM"));
    poReopenDS.reset();
  }

  VSIUnlink(pam_fn.c_str());
  VSIUnlink(fn.c_str());
}

TEST_F(NetCDFDatasetTest, CloneAllTypesAndGroupsInGrowDim) {
  auto poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  const std::string fn =
      (std::filesystem::path(::testing::TempDir()) / "clone_types_grow.nc")
          .string();
  char** papszOptions = nullptr;
  papszOptions = CSLSetNameValue(papszOptions, "FORMAT", "NC4");
  papszOptions = CSLSetNameValue(papszOptions, "GEOMETRY_ENCODING", "WKT");
  auto poDS = std::unique_ptr<GDALDataset>(
      poDriver->Create(fn.c_str(), 0, 0, 0, GDT_Unknown, papszOptions));
  CSLDestroy(papszOptions);
  ASSERT_NE(nullptr, poDS);

  char** papszLayerOptions = nullptr;
  papszLayerOptions =
      CSLSetNameValue(papszLayerOptions, "USE_STRING_IN_NC4", "NO");
  papszLayerOptions =
      CSLSetNameValue(papszLayerOptions, "WKT_DEFAULT_WIDTH", "50");
  auto poLayer1 =
      poDS->CreateLayer("layer1", nullptr, wkbLineString, papszLayerOptions);
  CSLDestroy(papszLayerOptions);
  ASSERT_NE(nullptr, poLayer1);

  OGRFieldDefn oFieldStr("str_field", OFTString);
  EXPECT_EQ(OGRERR_NONE, poLayer1->CreateField(&oFieldStr));
  OGRFieldDefn oFieldInt("int_field", OFTInteger);
  EXPECT_EQ(OGRERR_NONE, poLayer1->CreateField(&oFieldInt));
  OGRFieldDefn oFieldReal("real_field", OFTReal);
  EXPECT_EQ(OGRERR_NONE, poLayer1->CreateField(&oFieldReal));
  OGRFieldDefn oFieldInt64("int64_field", OFTInteger64);
  EXPECT_EQ(OGRERR_NONE, poLayer1->CreateField(&oFieldInt64));

  // Add raw variables of remaining NC types in root cdfid
  auto poNCDFLayer1 = dynamic_cast<netCDFLayer*>(poLayer1);
  ASSERT_NE(nullptr, poNCDFLayer1);
  const int root_cdfid = poNCDFLayer1->GetCDFID();

  int dim_extra = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(root_cdfid, "dim_extra", 3, &dim_extra));
  int vf = -1, vs = -1, vus = -1, vui = -1, vui64 = -1, vub = -1, vb = -1,
      vstr = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(root_cdfid, "vf", NC_FLOAT, 1, &dim_extra, &vf));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(root_cdfid, "vs", NC_SHORT, 1, &dim_extra, &vs));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(root_cdfid, "vus", NC_USHORT, 1, &dim_extra, &vus));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(root_cdfid, "vui", NC_UINT, 1, &dim_extra, &vui));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(root_cdfid, "vui64", NC_UINT64, 1, &dim_extra, &vui64));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(root_cdfid, "vub", NC_UBYTE, 1, &dim_extra, &vub));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(root_cdfid, "vb", NC_BYTE, 1, &dim_extra, &vb));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(root_cdfid, "vstr", NC_STRING, 1, &dim_extra, &vstr));

  ASSERT_EQ(NC_NOERR, nc_enddef(root_cdfid));
  float f_vals[3] = {1.1f, 2.2f, 3.3f};
  short s_vals[3] = {10, 20, 30};
  unsigned short us_vals[3] = {100, 200, 300};
  unsigned int ui_vals[3] = {1000, 2000, 3000};
  unsigned long long ui64_vals[3] = {10000, 20000, 30000};
  unsigned char ub_vals[3] = {1, 2, 3};
  signed char b_vals[3] = {4, 5, 6};
  const char* str_vals[3] = {"one", "two", "three"};
  ASSERT_EQ(NC_NOERR, nc_put_var_float(root_cdfid, vf, f_vals));
  ASSERT_EQ(NC_NOERR, nc_put_var_short(root_cdfid, vs, s_vals));
  ASSERT_EQ(NC_NOERR, nc_put_var_ushort(root_cdfid, vus, us_vals));
  ASSERT_EQ(NC_NOERR, nc_put_var_uint(root_cdfid, vui, ui_vals));
  ASSERT_EQ(NC_NOERR, nc_put_var_ulonglong(root_cdfid, vui64, ui64_vals));
  ASSERT_EQ(NC_NOERR, nc_put_var_uchar(root_cdfid, vub, ub_vals));
  ASSERT_EQ(NC_NOERR, nc_put_var_schar(root_cdfid, vb, b_vals));
  ASSERT_EQ(NC_NOERR, nc_put_var_string(root_cdfid, vstr, str_vals));

  // Feature 1: initial short geometry
  auto poFeature1 = std::make_unique<OGRFeature>(poLayer1->GetLayerDefn());
  poFeature1->SetField("str_field", "short");
  OGRLineString shortLine;
  shortLine.addPoint(0, 0);
  shortLine.addPoint(1, 1);
  poFeature1->SetGeometry(&shortLine);
  EXPECT_EQ(OGRERR_NONE, poLayer1->CreateFeature(poFeature1.get()));

  // Feature 2: long geometry > 50 chars forces GrowDim, which clones all NC
  // types
  auto poFeature2 = std::make_unique<OGRFeature>(poLayer1->GetLayerDefn());
  poFeature2->SetField("str_field", "short");
  OGRLineString longLine;
  for (int i = 0; i < 250; ++i) {
    longLine.addPoint(i * 0.1, i * 0.2);
  }
  poFeature2->SetGeometry(&longLine);
  EXPECT_EQ(OGRERR_NONE, poLayer1->CreateFeature(poFeature2.get()));

  poDS.reset();
  VSIUnlink(fn.c_str());
}

TEST_F(NetCDFDatasetTest, CreateCopyEdgeCasesAndDataTypes) {
  auto poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);
  auto poMemDriver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, poMemDriver);

  // 1. Zero bands rejected
  const std::string dummy_out =
      (std::filesystem::path(::testing::TempDir()) / "cc_dummy.nc").string();
  auto poZeroDS = std::unique_ptr<GDALDataset>(
      poMemDriver->Create("/vsimem/zero.tif", 2, 2, 0, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, poZeroDS);
  CPLErrorReset();
  {
    CPLErrorHandlerPusher oQuiet(CPLQuietErrorHandler);
    std::unique_ptr<GDALDataset> poBad(poDriver->CreateCopy(
        dummy_out.c_str(), poZeroDS.get(), FALSE, nullptr, nullptr, nullptr));
    EXPECT_EQ(nullptr, poBad);
  }

  // 2. Complex data type rejected
  auto poCpxDS = std::unique_ptr<GDALDataset>(
      poMemDriver->Create("/vsimem/cpx.tif", 2, 2, 1, GDT_CFloat32, nullptr));
  ASSERT_NE(nullptr, poCpxDS);
  {
    CPLErrorHandlerPusher oQuiet(CPLQuietErrorHandler);
    std::unique_ptr<GDALDataset> poBad(poDriver->CreateCopy(
        dummy_out.c_str(), poCpxDS.get(), FALSE, nullptr, nullptr, nullptr));
    EXPECT_EQ(nullptr, poBad);
  }

  // 3. Mismatched BAND_NAMES count rejected
  auto poOneDS = std::unique_ptr<GDALDataset>(
      poMemDriver->Create("/vsimem/one.tif", 2, 2, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, poOneDS);
  char** papszBadNames = nullptr;
  papszBadNames = CSLSetNameValue(papszBadNames, "BAND_NAMES", "b1,b2");
  {
    CPLErrorHandlerPusher oQuiet(CPLQuietErrorHandler);
    std::unique_ptr<GDALDataset> poBad(
        poDriver->CreateCopy(dummy_out.c_str(), poOneDS.get(), FALSE,
                             papszBadNames, nullptr, nullptr));
    EXPECT_EQ(nullptr, poBad);
  }
  CSLDestroy(papszBadNames);

  // 4. Multiple data types in CreateCopy: Byte, Int8, UInt16, Int16, UInt32,
  // Int32, UInt64, Int64, Float32, Float64
  const std::string multi_out =
      (std::filesystem::path(::testing::TempDir()) / "cc_multitypes.nc")
          .string();
  auto poMultiDS = std::unique_ptr<GDALDataset>(
      poMemDriver->Create("/vsimem/multi.tif", 4, 4, 0, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, poMultiDS);

  const GDALDataType types[] = {GDT_Byte,    GDT_Int8,   GDT_UInt16, GDT_Int16,
                                GDT_UInt32,  GDT_Int32,  GDT_UInt64, GDT_Int64,
                                GDT_Float32, GDT_Float64};
  for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
    ASSERT_EQ(CE_None, poMultiDS->AddBand(types[i], nullptr));
    auto poBand = poMultiDS->GetRasterBand(i + 1);
    ASSERT_NE(nullptr, poBand);
    poBand->SetOffset(1.5);
    poBand->SetScale(2.5);
  }
  poMultiDS->GetRasterBand(1)->SetMetadataItem("NETCDF_VARNAME", "band_one");
  poMultiDS->SetMetadataItem("band_one#long_name", "First Band Long Name");
  poMultiDS->SetMetadataItem("NC_GLOBAL#title", "Custom Title");
  poMultiDS->SetMetadataItem("custom_gdal_meta", "custom_val");

  char** papszMultiOpt = nullptr;
  papszMultiOpt = CSLSetNameValue(papszMultiOpt, "FORMAT", "NC4");
  papszMultiOpt = CSLSetNameValue(papszMultiOpt, "WRITE_GDAL_VERSION", "NO");
  papszMultiOpt = CSLSetNameValue(papszMultiOpt, "WRITE_GDAL_HISTORY", "NO");
  auto poCopyMultiDS = std::unique_ptr<GDALDataset>(
      poDriver->CreateCopy(multi_out.c_str(), poMultiDS.get(), FALSE,
                           papszMultiOpt, GDALDummyProgress, nullptr));
  CSLDestroy(papszMultiOpt);
  ASSERT_NE(nullptr, poCopyMultiDS);
  poCopyMultiDS.reset();

  auto poOpenMulti =
      std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
          multi_out.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, poOpenMulti);
  EXPECT_NE(nullptr, poOpenMulti->GetMetadata("SUBDATASETS"));
  EXPECT_STREQ("Custom Title", poOpenMulti->GetMetadataItem("NC_GLOBAL#title"));
  poOpenMulti.reset();
  VSIUnlink(multi_out.c_str());

  // 5. 3D cube with NETCDF_DIM_EXTRA in CreateCopy
  const std::string cube_out =
      (std::filesystem::path(::testing::TempDir()) / "cc_cube3d.nc").string();
  auto poCubeSrc = std::unique_ptr<GDALDataset>(
      poMemDriver->Create("/vsimem/cube.tif", 4, 4, 2, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, poCubeSrc);
  poCubeSrc->SetMetadataItem("NETCDF_DIM_EXTRA", "{time}");
  poCubeSrc->SetMetadataItem("NETCDF_DIM_time_DEF", "{2, 4}");
  poCubeSrc->SetMetadataItem("NETCDF_DIM_time_VALUES", "{10, 20}");

  char** papszCubeOpt = nullptr;
  papszCubeOpt = CSLSetNameValue(papszCubeOpt, "FORMAT", "NC4");
  auto poCopyCube = std::unique_ptr<GDALDataset>(
      poDriver->CreateCopy(cube_out.c_str(), poCubeSrc.get(), FALSE,
                           papszCubeOpt, GDALDummyProgress, nullptr));
  CSLDestroy(papszCubeOpt);
  ASSERT_NE(nullptr, poCopyCube);
  poCopyCube.reset();

  auto poOpenCube = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
      GDALOpenEx(cube_out.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, poOpenCube);
  EXPECT_EQ(2, poOpenCube->GetRasterCount());
  poOpenCube.reset();
  VSIUnlink(cube_out.c_str());
}

TEST_F(NetCDFDatasetTest, ComplexDataTypesAndChunkCrossing) {
  const std::string fn =
      (std::filesystem::path(::testing::TempDir()) / "cpx_chunk_cross.nc")
          .string();
  int ncid = -1;
  ASSERT_EQ(NC_NOERR, nc_create(fn.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  // 5x5 raster with 3x3 chunks
  int dim_x = -1, dim_y = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "x", 5, &dim_x));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "y", 5, &dim_y));

  // Define lat and lon coordinate variables with increasing latitude
  // (bottom-up)
  int var_x = -1, var_y = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "x", NC_DOUBLE, 1, &dim_x, &var_x));
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "y", NC_DOUBLE, 1, &dim_y, &var_y));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_x, "standard_name", "longitude"));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_y, "standard_name", "latitude"));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_x, "units", "degrees_east"));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_y, "units", "degrees_north"));

  // Define complex compound type: two floats {r, i}
  struct CpxFloat {
    float r;
    float i;
  };
  nc_type cpx_type = NC_NAT;
  ASSERT_EQ(NC_NOERR,
            nc_def_compound(ncid, sizeof(CpxFloat), "cpx_float", &cpx_type));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(ncid, cpx_type, "r",
                                         offsetof(CpxFloat, r), NC_FLOAT));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(ncid, cpx_type, "i",
                                         offsetof(CpxFloat, i), NC_FLOAT));

  int dims[2] = {dim_y, dim_x};
  int var_z = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "z", cpx_type, 2, dims, &var_z));

  size_t chunks[2] = {3, 3};
  ASSERT_EQ(NC_NOERR, nc_def_var_chunking(ncid, var_z, NC_CHUNKED, chunks));

  // Set valid_range and _FillValue using compound type
  CpxFloat cpx_fill = {-999.0f, -999.0f};
  ASSERT_EQ(NC_NOERR,
            nc_put_att(ncid, var_z, "_FillValue", cpx_type, 1, &cpx_fill));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_z, "valid_range", "{-2,10}"));

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

  double x_coords[5] = {0.0, 1.0, 2.0, 3.0, 4.0};
  double y_coords[5] = {10.0, 11.0, 12.0, 13.0, 14.0};
  ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, var_x, x_coords));
  ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, var_y, y_coords));

  // Fill 5x5 data with normal, out-of-range, NaN, and nodata values
  std::vector<CpxFloat> z_data(25);
  for (int i = 0; i < 25; ++i) {
    z_data[i].r = static_cast<float>(i - 5);
    z_data[i].i = static_cast<float>(i);
  }
  z_data[0].r = std::numeric_limits<float>::quiet_NaN();
  z_data[0].i = 0.0f;
  z_data[1].r = -999.0f;
  z_data[1].i = 0.0f;
  z_data[24].r = 50.0f;

  ASSERT_EQ(NC_NOERR, nc_put_var(ncid, var_z, z_data.data()));
  ASSERT_EQ(NC_NOERR, nc_close(ncid));

  auto poDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
      GDALOpenEx(fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, poDS);
  auto poBand = poDS->GetRasterBand(1);
  ASSERT_NE(nullptr, poBand);
  EXPECT_EQ(GDT_CFloat32, poBand->GetRasterDataType());

  std::vector<float> block0(3 * 3 * 2);
  EXPECT_EQ(CE_None, poBand->ReadBlock(0, 0, block0.data()));

  std::vector<float> block1(3 * 3 * 2);
  EXPECT_EQ(CE_None, poBand->ReadBlock(1, 0, block1.data()));

  std::vector<float> block01(3 * 3 * 2);
  EXPECT_EQ(CE_None, poBand->ReadBlock(0, 1, block01.data()));

  std::vector<float> block11(3 * 3 * 2);
  EXPECT_EQ(CE_None, poBand->ReadBlock(1, 1, block11.data()));

  poDS.reset();
  VSIUnlink(fn.c_str());
}

TEST_F(NetCDFDatasetTest, OneDVariablesAndSubdatasetTypes) {
  const std::string fn =
      (std::filesystem::path(::testing::TempDir()) / "oned_types.nc").string();
  int ncid = -1;
  ASSERT_EQ(NC_NOERR, nc_create(fn.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  int d_byte = -1, d_short = -1, d_int = -1, d_float = -1, d_double = -1;
  int d_ubyte = -1, d_ushort = -1, d_uint = -1, d_int64 = -1, d_uint64 = -1,
      d_str = -1;

  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_byte", 2, &d_byte));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_short", 2, &d_short));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_int", 2, &d_int));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_float", 2, &d_float));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_double", 2, &d_double));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_ubyte", 2, &d_ubyte));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_ushort", 2, &d_ushort));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_uint", 2, &d_uint));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_int64", 2, &d_int64));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_uint64", 2, &d_uint64));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_str", 2, &d_str));

  int v_byte = -1, v_short = -1, v_int = -1, v_float = -1, v_double = -1;
  int v_ubyte = -1, v_ushort = -1, v_uint = -1, v_int64 = -1, v_uint64 = -1,
      v_str = -1;

  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "dim_byte", NC_BYTE, 1, &d_byte, &v_byte));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "dim_short", NC_SHORT, 1, &d_short, &v_short));
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "dim_int", NC_INT, 1, &d_int, &v_int));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "dim_float", NC_FLOAT, 1, &d_float, &v_float));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "dim_double", NC_DOUBLE, 1, &d_double, &v_double));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "dim_ubyte", NC_UBYTE, 1, &d_ubyte, &v_ubyte));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "dim_ushort", NC_USHORT, 1, &d_ushort, &v_ushort));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "dim_uint", NC_UINT, 1, &d_uint, &v_uint));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "dim_int64", NC_INT64, 1, &d_int64, &v_int64));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "dim_uint64", NC_UINT64, 1, &d_uint64, &v_uint64));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "dim_str", NC_STRING, 1, &d_str, &v_str));

  int dims2[2] = {d_byte, d_short};
  int v2_byte = -1, v2_char = -1, v2_short = -1, v2_int = -1, v2_float = -1;
  int v2_double = -1, v2_ubyte = -1, v2_ushort = -1, v2_uint = -1,
      v2_int64 = -1, v2_uint64 = -1;

  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "sub_byte", NC_BYTE, 2, dims2, &v2_byte));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "sub_char", NC_CHAR, 2, dims2, &v2_char));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "sub_short", NC_SHORT, 2, dims2, &v2_short));
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "sub_int", NC_INT, 2, dims2, &v2_int));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "sub_float", NC_FLOAT, 2, dims2, &v2_float));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "sub_double", NC_DOUBLE, 2, dims2, &v2_double));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "sub_ubyte", NC_UBYTE, 2, dims2, &v2_ubyte));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "sub_ushort", NC_USHORT, 2, dims2, &v2_ushort));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "sub_uint", NC_UINT, 2, dims2, &v2_uint));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "sub_int64", NC_INT64, 2, dims2, &v2_int64));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "sub_uint64", NC_UINT64, 2, dims2, &v2_uint64));

  // Add 3D variable to test extra dimensions reporting (nd > 2)
  int dims3[3] = {d_byte, d_short, d_int};
  int v_cube = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "cube3d", NC_FLOAT, 3, dims3, &v_cube));

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

  signed char b2[2] = {1, 2};
  short s2[2] = {10, 20};
  int i2[2] = {100, 200};
  float f2[2] = {1.5f, 2.5f};
  double d2[2] = {10.5, 20.5};
  unsigned char ub2[2] = {250, 251};
  unsigned short us2[2] = {1000, 2000};
  unsigned int ui2[2] = {100000, 200000};
  long long i642[2] = {10000000000LL, 20000000000LL};
  unsigned long long ui642[2] = {30000000000ULL, 40000000000ULL};
  const char* str2[2] = {"alpha", "beta"};
  float f_cube[8] = {0};

  ASSERT_EQ(NC_NOERR, nc_put_var_schar(ncid, v_byte, b2));
  ASSERT_EQ(NC_NOERR, nc_put_var_short(ncid, v_short, s2));
  ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, v_int, i2));
  ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, v_float, f2));
  ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, v_double, d2));
  ASSERT_EQ(NC_NOERR, nc_put_var_uchar(ncid, v_ubyte, ub2));
  ASSERT_EQ(NC_NOERR, nc_put_var_ushort(ncid, v_ushort, us2));
  ASSERT_EQ(NC_NOERR, nc_put_var_uint(ncid, v_uint, ui2));
  ASSERT_EQ(NC_NOERR, nc_put_var_longlong(ncid, v_int64, i642));
  ASSERT_EQ(NC_NOERR, nc_put_var_ulonglong(ncid, v_uint64, ui642));
  ASSERT_EQ(NC_NOERR, nc_put_var_string(ncid, v_str, str2));
  ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, v_cube, f_cube));

  ASSERT_EQ(NC_NOERR, nc_close(ncid));

  {
    CPLConfigOptionSetter oSetter("GDAL_NETCDF_REPORT_EXTRA_DIM_VALUES", "YES",
                                  /*bSetOnlyIfUndefined=*/false);

    auto poDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);

    char** papszSubDS = poDS->GetMetadata("SUBDATASETS");
    ASSERT_NE(nullptr, papszSubDS);
    EXPECT_NE(nullptr, CSLFetchNameValue(papszSubDS, "SUBDATASET_1_DESC"));
    poDS.reset();

    // Open 3D cube subdataset to exercise reporting extra dim values (nd > 2)
    const std::string cube_sub = std::string("NETCDF:\"") + fn + "\":cube3d";
    auto poCubeDS =
        std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
            cube_sub.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poCubeDS);
    EXPECT_NE(nullptr, poCubeDS->GetMetadataItem("NETCDF_DIM_dim_byte_VALUES"));
    poCubeDS.reset();
  }
  VSIUnlink(fn.c_str());
}

TEST_F(NetCDFDatasetTest, FloatCoordinateRoundingAndLatitudeSpacing) {
  const std::string fn =
      (std::filesystem::path(::testing::TempDir()) / "float_coords.nc")
          .string();
  int ncid = -1;
  ASSERT_EQ(NC_NOERR, nc_create(fn.c_str(), NC_CLOBBER, &ncid));

  int dim_x = -1, dim_y = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "lon", 5, &dim_x));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "lat", 5, &dim_y));

  int var_x = -1, var_y = -1, var_z = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "lon", NC_FLOAT, 1, &dim_x, &var_x));
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "lat", NC_FLOAT, 1, &dim_y, &var_y));
  int dims[2] = {dim_y, dim_x};
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "data", NC_FLOAT, 2, dims, &var_z));

  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_x, "standard_name", "longitude"));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_y, "standard_name", "latitude"));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_x, "units", "degrees_east"));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_y, "units", "degrees_north"));

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

  float x_vals[5] = {10.0f, 10.5f, 11.0f, 11.5f, 12.0f};
  float y_vals[5] = {0.0f, 1.0f, 2.01f, 3.02f, 4.05f};
  float z_vals[25] = {0};

  ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, var_x, x_vals));
  ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, var_y, y_vals));
  ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, var_z, z_vals));
  ASSERT_EQ(NC_NOERR, nc_close(ncid));

  auto poDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
      GDALOpenEx(fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, poDS);
  poDS.reset();

  {
    CPLConfigOptionSetter oSetter("GDAL_NETCDF_IGNORE_EQUALLY_SPACED_XY_CHECK",
                                  "YES",
                                  /*bSetOnlyIfUndefined=*/false);
    auto poDS2 = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS2);
    poDS2.reset();
  }

  VSIUnlink(fn.c_str());
}

TEST_F(NetCDFDatasetTest, GridMappingExpandedSyntaxAndStandardParallels) {
  const std::string fn =
      (std::filesystem::path(::testing::TempDir()) / "grid_mapping_exp.nc")
          .string();
  int ncid = -1;
  ASSERT_EQ(NC_NOERR, nc_create(fn.c_str(), NC_CLOBBER, &ncid));

  int dim_x = -1, dim_y = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "x", 4, &dim_x));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "y", 4, &dim_y));

  int var_x = -1, var_y = -1, var_crs = -1, var_data = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "x", NC_DOUBLE, 1, &dim_x, &var_x));
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "y", NC_DOUBLE, 1, &dim_y, &var_y));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "crsLCC", NC_CHAR, 0, nullptr, &var_crs));
  int dims[2] = {dim_y, dim_x};
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "data", NC_FLOAT, 2, dims, &var_data));

  const char* gm = "crsLCC: x y crsWGS84: lat lon";
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_data, "grid_mapping", gm));

  const char* gmn = "lambert_conformal_conic";
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_crs, "grid_mapping_name", gmn));
  const char* sp = "30.0 60.0";
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_crs, "standard_parallel", sp));
  double origin_lat = 40.0, central_meridian = -100.0;
  ASSERT_EQ(NC_NOERR,
            nc_put_att_double(ncid, var_crs, "latitude_of_projection_origin",
                              NC_DOUBLE, 1, &origin_lat));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_double(ncid, var_crs, "longitude_of_central_meridian",
                              NC_DOUBLE, 1, &central_meridian));

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

  double x_v[4] = {1000.0, 2000.0, 3000.0, 4000.0};
  double y_v[4] = {4000.0, 3000.0, 2000.0, 1000.0};
  float d_v[16] = {0};
  ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, var_x, x_v));
  ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, var_y, y_v));
  ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, var_data, d_v));
  ASSERT_EQ(NC_NOERR, nc_close(ncid));

  auto poDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
      GDALOpenEx(fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, poDS);
  const auto poSRS = poDS->GetSpatialRef();
  ASSERT_NE(nullptr, poSRS);
  EXPECT_TRUE(poSRS->IsProjected());
  poDS.reset();

  VSIUnlink(fn.c_str());
}

TEST_F(NetCDFDatasetTest, OpenSpecialPathsAndSeparateFilesErrors) {
  auto poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  // 1. Subdataset string with URL or vsicurl: exercises CSLTokenize / memmove.
  // Hold results in unique_ptr so an unexpected success does not leak.
  {
    CPLErrorHandlerPusher oQuiet(CPLQuietErrorHandler);
    for (const char* path :
         {"NETCDF:\"http://invalid.domain/test.nc\":var",
          "NETCDF:\"/vsicurl/http://invalid.domain/test.nc\":var",
          "NETCDF:\"/vsicurl_streaming/http://invalid.domain/test.nc\":var",
          "NETCDF:\"D:\\invalid\\file.nc\":var"}) {
      std::unique_ptr<GDALDataset> poBad(static_cast<GDALDataset*>(
          GDALOpenEx(path, GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
      EXPECT_EQ(nullptr, poBad) << path;
    }
  }

  // 2. Open with GDAL_OF_MULTIDIM_RASTER
  const std::string fn =
      (std::filesystem::path(::testing::TempDir()) / "multidim_open.nc")
          .string();
  int ncid = -1;
  ASSERT_EQ(NC_NOERR, nc_create(fn.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));
  ASSERT_EQ(NC_NOERR, nc_close(ncid));

  auto poMultiDimDS =
      std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
          fn.c_str(), GDAL_OF_MULTIDIM_RASTER, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, poMultiDimDS);
  EXPECT_NE(nullptr, poMultiDimDS->GetRootGroup());
  poMultiDimDS.reset();
  VSIUnlink(fn.c_str());

  // 3. SEPARATE_FILES where output path is an existing non-directory file
  const std::string reg_file =
      (std::filesystem::path(::testing::TempDir()) / "reg_file.txt").string();
  FILE* fp = fopen(reg_file.c_str(), "w");
  ASSERT_NE(nullptr, fp);
  fputs("hello", fp);
  fclose(fp);

  auto poMemDriver = GetGDALDriverManager()->GetDriverByName("MEM");
  auto poSrc = std::unique_ptr<GDALDataset>(
      poMemDriver->Create("/vsimem/sep_src.tif", 2, 2, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, poSrc);

  char** papszOptions = nullptr;
  papszOptions = CSLSetNameValue(papszOptions, "GEOMETRY_ENCODING", "WKT");
  papszOptions =
      CSLSetNameValue(papszOptions, "MULTIPLE_LAYERS", "SEPARATE_FILES");
  {
    CPLErrorHandlerPusher oQuiet(CPLQuietErrorHandler);
    std::unique_ptr<GDALDataset> poBad(poDriver->CreateCopy(
        reg_file.c_str(), poSrc.get(), FALSE, papszOptions, nullptr, nullptr));
    EXPECT_EQ(nullptr, poBad);
  }
  CSLDestroy(papszOptions);
  VSIUnlink(reg_file.c_str());
}

TEST_F(NetCDFDatasetTest, CheckGeolocationInferenceWithoutAttribute) {
  const std::string fn =
      (std::filesystem::path(::testing::TempDir()) / "geoloc_infer.nc")
          .string();
  int ncid = -1;
  ASSERT_EQ(NC_NOERR, nc_create(fn.c_str(), NC_CLOBBER, &ncid));

  int dim_y = -1, dim_x = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_y", 4, &dim_y));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_x", 4, &dim_x));

  int dims[2] = {dim_y, dim_x};
  int var_data = -1, var_lon = -1, var_lat = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "data", NC_FLOAT, 2, dims, &var_data));
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "lon", NC_FLOAT, 2, dims, &var_lon));
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "lat", NC_FLOAT, 2, dims, &var_lat));

  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_lon, "standard_name", "longitude"));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_lat, "standard_name", "latitude"));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_lon, "units", "degrees_east"));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_lat, "units", "degrees_north"));

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

  std::vector<float> lons(16), lats(16), vals(16, 1.0f);
  for (int i = 0; i < 16; ++i) {
    lons[i] = 10.0f + (i % 4) * 0.5f;
    lats[i] = 20.0f + (i / 4) * 0.5f;
  }
  ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, var_lon, lons.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, var_lat, lats.data()));
  ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, var_data, vals.data()));
  ASSERT_EQ(NC_NOERR, nc_close(ncid));

  const std::string sub_fn = std::string("NETCDF:\"") + fn + "\":data";
  auto poDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
      GDALOpenEx(sub_fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, poDS);
  EXPECT_NE(nullptr, poDS->GetMetadata("GEOLOCATION"));
  poDS.reset();

  VSIUnlink(fn.c_str());
}

TEST_F(NetCDFDatasetTest, NCDFPutAttrArrayTypes) {
  const std::string fn =
      (std::filesystem::path(::testing::TempDir()) / "put_attr_arr.nc")
          .string();
  int ncid = -1;
  ASSERT_EQ(NC_NOERR, nc_create(fn.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  int dim_x = -1, dim_y = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "x", 2, &dim_x));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "y", 2, &dim_y));
  int dims[2] = {dim_y, dim_x};
  int var_z = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "z", NC_INT, 2, dims, &var_z));

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));
  int data[4] = {1, 2, 3, 4};
  ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, var_z, data));
  ASSERT_EQ(NC_NOERR, nc_close(ncid));

  auto poDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
      fn.c_str(), GDAL_OF_UPDATE | GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, poDS);
  auto poBand = poDS->GetRasterBand(1);
  ASSERT_NE(nullptr, poBand);

  // Exercise NCDFPutAttr via SetMetadataItem with various array and scalar
  // types. Note: netCDFRasterBand::SetMetadataItem checks "!NCDFPutAttr(...)",
  // so it returns CE_Failure even though NCDFPutAttr succeeds.
  {
    CPLErrorHandlerPusher oQuiet(CPLQuietErrorHandler);
    poBand->SetMetadataItem("int_arr", "{100,200}");
    poBand->SetMetadataItem("uint_arr", "{3000000000,3000000001}");
    poBand->SetMetadataItem("float_arr", "{1.5,2.5}");
    poBand->SetMetadataItem("double_arr", "{1.1234567890123,2.1234567890123}");
    poBand->SetMetadataItem("single_val", "42");
    poBand->SetMetadataItem("str_val", "some_text");
  }

  // Check valid_range handling in netCDFRasterBand::SetMetadataItem
  EXPECT_EQ(CE_None, poBand->SetMetadataItem("valid_range", "{5,10}"));
  {
    CPLErrorHandlerPusher oQuiet(CPLQuietErrorHandler);
    EXPECT_EQ(CE_None, poBand->SetMetadataItem("valid_range",
                                               "{10,5}"));  // min > max warning
  }

  // Check ignored metadata names
  EXPECT_EQ(CE_None, poBand->SetMetadataItem("NETCDF_VARNAME", "ignored"));
  EXPECT_EQ(CE_None, poBand->SetMetadataItem("missing_value", "-999"));
  EXPECT_EQ(CE_None, poBand->SetMetadataItem("_FillValue", "-999"));

  poDS.reset();
  VSIUnlink(fn.c_str());
}

TEST_F(NetCDFDatasetTest, DimensionOrderingAndYXBandOrder) {
  const std::string fn =
      (std::filesystem::path(::testing::TempDir()) / "dim_order_yx.nc")
          .string();
  int ncid = -1;
  ASSERT_EQ(NC_NOERR, nc_create(fn.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  // 1. PACE OCI: number_of_lines, pixels_per_line, bands
  int dim_lines = -1, dim_pixels = -1, dim_bands = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "number_of_lines", 3, &dim_lines));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "pixels_per_line", 4, &dim_pixels));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "bands", 2, &dim_bands));

  int dims_pace[3] = {dim_lines, dim_pixels, dim_bands};
  int var_pace = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "pace_data", NC_FLOAT, 3, dims_pace, &var_pace));

  // 2. EMIT: downtrack, crosstrack, band
  int dim_down = -1, dim_cross = -1, dim_ebands = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "downtrack", 3, &dim_down));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "crosstrack", 4, &dim_cross));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "band", 2, &dim_ebands));
  int dims_emit[3] = {dim_down, dim_cross, dim_ebands};
  int var_emit = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "emit_data", NC_FLOAT, 3, dims_emit, &var_emit));

  // 3. 4D variable with non-standard CF dimensions and swapped coordinates
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, NC_GLOBAL, "Conventions", "CF-1.6"));
  int dim_d0 = -1, dim_d1 = -1, dim_y = -1, dim_x = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim0_not_time", 2, &dim_d0));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim1_not_vert", 2, &dim_d1));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "y_lat", 2, &dim_y));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "x_lon", 2, &dim_x));

  int var_coord_x = -1, var_coord_y = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "x_lon", NC_FLOAT, 1, &dim_x, &var_coord_x));
  ASSERT_EQ(NC_NOERR,
            PutTextAtt(ncid, var_coord_x, "standard_name", "latitude"));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "y_lat", NC_FLOAT, 1, &dim_y, &var_coord_y));
  ASSERT_EQ(NC_NOERR,
            PutTextAtt(ncid, var_coord_y, "standard_name", "longitude"));

  int dims_4d[4] = {dim_d0, dim_d1, dim_y, dim_x};
  int var_4d = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "data4d", NC_INT, 4, dims_4d, &var_4d));

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));
  float dummy_pace[24] = {0};
  ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, var_pace, dummy_pace));
  ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, var_emit, dummy_pace));
  int data4d[16] = {0};
  ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, var_4d, data4d));
  ASSERT_EQ(NC_NOERR, nc_close(ncid));

  // Open PACE subdataset
  {
    const std::string sub_pace = std::string("NETCDF:\"") + fn + "\":pace_data";
    auto poDS =
        std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
            sub_pace.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    EXPECT_EQ(4, poDS->GetRasterXSize());
    EXPECT_EQ(3, poDS->GetRasterYSize());
    EXPECT_EQ(2, poDS->GetRasterCount());
  }

  // Open EMIT subdataset
  {
    const std::string sub_emit = std::string("NETCDF:\"") + fn + "\":emit_data";
    auto poDS =
        std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
            sub_emit.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    EXPECT_EQ(4, poDS->GetRasterXSize());
    EXPECT_EQ(3, poDS->GetRasterYSize());
    EXPECT_EQ(2, poDS->GetRasterCount());
  }

  // Open 4D subdataset - triggers CF 4D warnings, swapped XY, and multidim band
  // math
  {
    const std::string sub_4d = std::string("NETCDF:\"") + fn + "\":data4d";
    std::unique_ptr<GDALDataset> poDS;
    {
      CPLErrorHandlerPusher oQuiet(CPLQuietErrorHandler);
      poDS.reset(static_cast<GDALDataset*>(
          GDALOpenEx(sub_4d.c_str(), GDAL_OF_UPDATE | GDAL_OF_RASTER, nullptr,
                     nullptr, nullptr)));
    }
    ASSERT_NE(nullptr, poDS);
    EXPECT_EQ(4, poDS->GetRasterCount());
    auto poBand = poDS->GetRasterBand(2);
    ASSERT_NE(nullptr, poBand);
    int block[4] = {1, 2, 3, 4};
    EXPECT_EQ(CE_None, poBand->RasterIO(GF_Read, 0, 0, 2, 2, block, 2, 2,
                                        GDT_Int32, 0, 0));
    EXPECT_EQ(CE_None, poBand->RasterIO(GF_Write, 0, 0, 2, 2, block, 2, 2,
                                        GDT_Int32, 0, 0));
  }

  VSIUnlink(fn.c_str());
}

TEST_F(NetCDFDatasetTest, OpenOptionsAndMemoryFile) {
  const std::string fn =
      (std::filesystem::path(::testing::TempDir()) / "open_mem_test.nc")
          .string();
  int ncid = -1;
  ASSERT_EQ(NC_NOERR, nc_create(fn.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  int dim_x = -1, dim_y = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "x", 3, &dim_x));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "y", 3, &dim_y));
  int dims[2] = {dim_y, dim_x};
  int var1 = -1, var2 = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "var1", NC_FLOAT, 2, dims, &var1));
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "var2", NC_FLOAT, 2, dims, &var2));
  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));
  float data[9] = {1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f, 9.f};
  ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, var1, data));
  ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, var2, data));
  ASSERT_EQ(NC_NOERR, nc_close(ncid));

  // Copy file into /vsimem/
  VSILFILE* fp = VSIFOpenL(fn.c_str(), "rb");
  ASSERT_NE(nullptr, fp);
  VSIFSeekL(fp, 0, SEEK_END);
  vsi_l_offset nLength = VSIFTellL(fp);
  VSIFSeekL(fp, 0, SEEK_SET);
  std::vector<GByte> buf(nLength);
  ASSERT_EQ(nLength, VSIFReadL(buf.data(), 1, nLength, fp));
  VSIFCloseL(fp);

  const std::string mem_fn = "/vsimem/netcdf_mem_test.nc";
  VSILFILE* fpMem =
      VSIFileFromMemBuffer(mem_fn.c_str(), buf.data(), nLength, false);
  ASSERT_NE(nullptr, fpMem);
  VSIFCloseL(fpMem);

  // 1. Exercise nc_open_mem via /vsimem/
  auto poMemDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
      GDALOpenEx(mem_fn.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
                 nullptr, nullptr)));
  ASSERT_NE(nullptr, poMemDS);
  poMemDS.reset();
  VSIUnlink(mem_fn.c_str());

  // 2. Open options: VARIABLES_AS_BANDS=YES
  const char* apszOpenOptions[] = {"VARIABLES_AS_BANDS=YES", nullptr};
  auto poBandsDS =
      std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
          fn.c_str(), GDAL_OF_RASTER, nullptr, apszOpenOptions, nullptr)));
  ASSERT_NE(nullptr, poBandsDS);
  EXPECT_EQ(2, poBandsDS->GetRasterCount());
  poBandsDS.reset();

  // 3. NETCDF: URL parsing paths (exercise lines 8130-8172)
  {
    CPLErrorHandlerPusher oQuiet(CPLQuietErrorHandler);
    GDALClose(GDALOpenEx("NETCDF:http://example.com/file.nc", GDAL_OF_RASTER,
                         nullptr, nullptr, nullptr));
    GDALClose(GDALOpenEx("NETCDF:http://example.com/file.nc:sub",
                         GDAL_OF_RASTER, nullptr, nullptr, nullptr));
    GDALClose(GDALOpenEx("NETCDF:a:b:c:d:e", GDAL_OF_RASTER, nullptr, nullptr,
                         nullptr));
  }

  VSIUnlink(fn.c_str());
}

TEST_F(NetCDFDatasetTest, ProjectionsAndCoordinateVariations) {
  const std::string fn =
      (std::filesystem::path(::testing::TempDir()) / "coord_variations.nc")
          .string();
  int ncid = -1;
  ASSERT_EQ(NC_NOERR, nc_create(fn.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  int dim_x = -1, dim_y = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "lon", 4, &dim_x));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "lat", 3, &dim_y));

  // Longitude coordinate with wrap around: 170, 175, -180, -175
  int var_lon = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "lon", NC_DOUBLE, 1, &dim_x, &var_lon));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_lon, "standard_name", "longitude"));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_lon, "units", "degrees_east"));

  int var_lat = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "lat", NC_DOUBLE, 1, &dim_y, &var_lat));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_lat, "standard_name", "latitude"));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_lat, "units", "degrees_north"));

  // Grid mapping with bounding coordinates: Northernmost_Northing, etc.
  int var_crs = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "crs", NC_CHAR, 0, nullptr, &var_crs));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_crs, "grid_mapping_name",
                                 "latitude_longitude"));
  double dfNN = 50.0, dfSN = 40.0, dfEE = 20.0, dfWE = 10.0;
  ASSERT_EQ(NC_NOERR, nc_put_att_double(ncid, var_crs, "Northernmost_Northing",
                                        NC_DOUBLE, 1, &dfNN));
  ASSERT_EQ(NC_NOERR, nc_put_att_double(ncid, var_crs, "Southernmost_Northing",
                                        NC_DOUBLE, 1, &dfSN));
  ASSERT_EQ(NC_NOERR, nc_put_att_double(ncid, var_crs, "Easternmost_Easting",
                                        NC_DOUBLE, 1, &dfEE));
  ASSERT_EQ(NC_NOERR, nc_put_att_double(ncid, var_crs, "Westernmost_Easting",
                                        NC_DOUBLE, 1, &dfWE));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_crs, "srid", "EPSG:4326"));

  int dims[2] = {dim_y, dim_x};
  int var_data = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "data", NC_FLOAT, 2, dims, &var_data));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_data, "grid_mapping", "crs"));

  // Variable with crs attribute containing PROJ string directly (no
  // grid_mapping)
  int var_data2 = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "data_proj", NC_FLOAT, 2, dims, &var_data2));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_data2, "crs",
                                 "+proj=utm +zone=31 +datum=WGS84"));

  // Variable with grid mapping containing srid attribute directly (no
  // grid_mapping_name)
  int var_crs_srid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "crs_srid", NC_CHAR, 0, nullptr, &var_crs_srid));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_crs_srid, "srid",
                                 "urn:ogc:def:crs:EPSG::6931"));
  int var_data_srid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "data_srid", NC_FLOAT, 2, dims, &var_data_srid));
  ASSERT_EQ(NC_NOERR,
            PutTextAtt(ncid, var_data_srid, "grid_mapping", "crs_srid"));

  // Variable with non-existent grid mapping to trigger mandatory resolution
  // failure
  int var_data_bad = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "data_bad_gm", NC_FLOAT, 2, dims, &var_data_bad));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_data_bad, "grid_mapping",
                                 "non_existent_grid"));

  // Variable with geostationary projection and microradian coordinates
  int var_geo_crs = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "geo_crs", NC_CHAR, 0, nullptr, &var_geo_crs));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_geo_crs, "grid_mapping_name",
                                 "geostationary"));
  double dfHeight = 35785831.0;
  ASSERT_EQ(NC_NOERR,
            nc_put_att_double(ncid, var_geo_crs, "perspective_point_height",
                              NC_DOUBLE, 1, &dfHeight));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_geo_crs, "sweep_angle_axis", "x"));
  double dfSemiMajor = 6378137.0;
  double dfSemiMinor = 6356752.31414;
  ASSERT_EQ(NC_NOERR, nc_put_att_double(ncid, var_geo_crs, "semi_major_axis",
                                        NC_DOUBLE, 1, &dfSemiMajor));
  ASSERT_EQ(NC_NOERR, nc_put_att_double(ncid, var_geo_crs, "semi_minor_axis",
                                        NC_DOUBLE, 1, &dfSemiMinor));
  double dfOriginLat = 0.0, dfOriginLon = -89.5;
  ASSERT_EQ(NC_NOERR, nc_put_att_double(ncid, var_geo_crs,
                                        "latitude_of_projection_origin",
                                        NC_DOUBLE, 1, &dfOriginLat));
  ASSERT_EQ(NC_NOERR, nc_put_att_double(ncid, var_geo_crs,
                                        "longitude_of_projection_origin",
                                        NC_DOUBLE, 1, &dfOriginLon));

  int dim_gx = -1, dim_gy = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "gx", 2, &dim_gx));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "gy", 2, &dim_gy));
  int var_gx = -1, var_gy = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "gx", NC_DOUBLE, 1, &dim_gx, &var_gx));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_gx, "standard_name",
                                 "projection_x_coordinate"));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_gx, "units", "microradian"));
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "gy", NC_DOUBLE, 1, &dim_gy, &var_gy));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_gy, "standard_name",
                                 "projection_y_coordinate"));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_gy, "units", "microradian"));

  int dims_geo[2] = {dim_gy, dim_gx};
  int var_geo_data = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "data_geostationary", NC_FLOAT, 2,
                                 dims_geo, &var_geo_data));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_geo_data, "coordinates", "gy gx"));
  ASSERT_EQ(NC_NOERR,
            PutTextAtt(ncid, var_geo_data, "grid_mapping", "geo_crs"));

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));
  double lon_vals[4] = {170.0, 175.0, -180.0, -175.0};
  double lat_vals[3] = {10.0, 20.0, 30.0};
  ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, var_lon, lon_vals));
  ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, var_lat, lat_vals));
  double gx_vals[2] = {1000.0, 2000.0};
  double gy_vals[2] = {3000.0, 4000.0};
  ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, var_gx, gx_vals));
  ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, var_gy, gy_vals));
  ASSERT_EQ(NC_NOERR, nc_close(ncid));

  // Open subdataset "data": longitude wrap and grid_mapping N/S/E/W and srid
  {
    const std::string sub_fn = std::string("NETCDF:\"") + fn + "\":data";
    auto poDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(sub_fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    double adfGeoTransform[6];
    EXPECT_EQ(CE_None, poDS->GetGeoTransform(adfGeoTransform));
  }

  // Open subdataset "data_proj": crs attribute
  {
    const std::string sub_fn = std::string("NETCDF:\"") + fn + "\":data_proj";
    auto poDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(sub_fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    EXPECT_NE(nullptr, poDS->GetSpatialRef());
  }

  // Open subdataset "data_srid": srid grid_mapping attribute
  {
    const std::string sub_fn = std::string("NETCDF:\"") + fn + "\":data_srid";
    auto poDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(sub_fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    EXPECT_NE(nullptr, poDS->GetSpatialRef());
  }

  // Open subdataset "data_geostationary": geostationary microradian
  {
    const std::string sub_fn =
        std::string("NETCDF:\"") + fn + "\":data_geostationary";
    auto poDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(sub_fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    double adfGeoTransform[6];
    EXPECT_EQ(CE_None, poDS->GetGeoTransform(adfGeoTransform));
  }

  // Open subdataset "data_bad_gm": mandatory variable resolution failure
  {
    const std::string sub_fn = std::string("NETCDF:\"") + fn + "\":data_bad_gm";
    CPLErrorReset();
    std::unique_ptr<GDALDataset> poDS;
    {
      CPLErrorHandlerPusher oQuiet(CPLQuietErrorHandler);
      poDS.reset(static_cast<GDALDataset*>(GDALOpenEx(
          sub_fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    }
  }

  VSIUnlink(fn.c_str());
}

TEST_F(NetCDFDatasetTest, CenterLongAndValidRangeEdgeCases) {
  const std::string fn =
      (std::filesystem::path(::testing::TempDir()) / "center_long_range.nc")
          .string();
  int ncid = -1;
  ASSERT_EQ(NC_NOERR, nc_create(fn.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  int dim_x = -1, dim_y = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "x", 2, &dim_x));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "y", 2, &dim_y));
  int dims[2] = {dim_y, dim_x};

  // 1. Variable "lon" with values > 180 (for bCheckLongitude lines 2184-2204)
  int var_lon = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "lon", NC_FLOAT, 2, dims, &var_lon));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_lon, "units", "degrees_east"));

  // 2. Variable with NC_SHORT, _Unsigned="true", valid_range negative (lines
  // 582-600)
  int var_ushort = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_ushort", NC_SHORT, 2, dims, &var_ushort));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_ushort, "_Unsigned", "true"));
  short neg_range[2] = {-30000, -1000};
  ASSERT_EQ(NC_NOERR, nc_put_att_short(ncid, var_ushort, "valid_range",
                                       NC_SHORT, 2, neg_range));

  // 3. Variable with valid_range min > max (lines 601-610)
  int var_inv_range = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "v_inv", NC_FLOAT, 2, dims, &var_inv_range));
  float inv_range[2] = {100.f, 50.f};
  ASSERT_EQ(NC_NOERR, nc_put_att_float(ncid, var_inv_range, "valid_range",
                                       NC_FLOAT, 2, inv_range));

  // 4. NC_BYTE variables with valid_range={0, 255} and valid_range={-128, 127}
  int var_ub = -1, var_sb = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_ub", NC_BYTE, 2, dims, &var_ub));
  signed char ub_range[2] = {0, -1};
  ASSERT_EQ(NC_NOERR, nc_put_att_schar(ncid, var_ub, "valid_range", NC_BYTE, 2,
                                       ub_range));

  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "v_sb", NC_BYTE, 2, dims, &var_sb));
  signed char sb_range[2] = {-128, 127};
  ASSERT_EQ(NC_NOERR, nc_put_att_schar(ncid, var_sb, "valid_range", NC_BYTE, 2,
                                       sb_range));

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));
  float lon_data[4] = {190.0f, 200.0f, 210.0f, 220.0f};
  ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, var_lon, lon_data));
  ASSERT_EQ(NC_NOERR, nc_close(ncid));

  // Read "lon" with GDAL_NETCDF_CENTERLONG_180=YES
  {
    CPLConfigOptionSetter oSetter("GDAL_NETCDF_CENTERLONG_180", "YES",
                                  /*bSetOnlyIfUndefined=*/false);
    {
      const std::string sub_fn = std::string("NETCDF:\"") + fn + "\":lon";
      auto poDS =
          std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
              sub_fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
      ASSERT_NE(nullptr, poDS);
      auto poBand = poDS->GetRasterBand(1);
      ASSERT_NE(nullptr, poBand);
      float read_lon[4] = {0};
      EXPECT_EQ(CE_None, poBand->RasterIO(GF_Read, 0, 0, 2, 2, read_lon, 2, 2,
                                          GDT_Float32, 0, 0));
      EXPECT_LT(read_lon[0], 0.0f);
    }
  }

  // Open other variables to verify valid_range logic
  {
    const std::string sub_fn = std::string("NETCDF:\"") + fn + "\":v_ushort";
    auto poDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(sub_fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
  }
  {
    const std::string sub_fn = std::string("NETCDF:\"") + fn + "\":v_inv";
    std::unique_ptr<GDALDataset> poDS;
    {
      CPLErrorHandlerPusher oQuiet(CPLQuietErrorHandler);
      poDS.reset(static_cast<GDALDataset*>(GDALOpenEx(
          sub_fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    }
    ASSERT_NE(nullptr, poDS);
  }
  {
    const std::string sub_fn = std::string("NETCDF:\"") + fn + "\":v_ub";
    auto poDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(sub_fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
  }
  {
    const std::string sub_fn = std::string("NETCDF:\"") + fn + "\":v_sb";
    auto poDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(sub_fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
  }

  VSIUnlink(fn.c_str());
}

TEST_F(NetCDFDatasetTest, DirectUtilityFunctionsAndVectorProfiles) {
  // NC3 SetNoDataValue on GDT_Int8 (lines 1065-1089)
  auto poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);
  const std::string fn_nc3 =
      (std::filesystem::path(::testing::TempDir()) / "nc3_nodata.nc").string();
  char** papszOpts = nullptr;
  papszOpts = CSLSetNameValue(papszOpts, "FORMAT", "NC");
  auto poNC3DS = std::unique_ptr<GDALDataset>(
      poDriver->Create(fn_nc3.c_str(), 2, 2, 1, GDT_Int8, papszOpts));
  CSLDestroy(papszOpts);
  ASSERT_NE(nullptr, poNC3DS);
  auto poBand = poNC3DS->GetRasterBand(1);
  ASSERT_NE(nullptr, poBand);
  EXPECT_EQ(CE_None, poBand->SetNoDataValue(-50));
  poNC3DS.reset();
  VSIUnlink(fn_nc3.c_str());

  // Vector profile with 3D coordinates (lon lat z) (lines 12570-12625)
  const std::string fn_vec =
      (std::filesystem::path(::testing::TempDir()) / "vec_profile.nc").string();
  int ncid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_create(fn_vec.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, NC_GLOBAL, "featureType", "profile"));
  int dim_prof = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "obs", 3, &dim_prof));

  int dim_x = -1, dim_y = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "x", 2, &dim_x));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "y", 2, &dim_y));
  int dims2d[2] = {dim_y, dim_x};
  int v_raster = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "raster_var", NC_INT, 2, dims2d, &v_raster));

  int v_lon = -1, v_lat = -1, v_z = -1, v_val = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "lon", NC_DOUBLE, 1, &dim_prof, &v_lon));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, v_lon, "standard_name", "longitude"));

  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "lat", NC_DOUBLE, 1, &dim_prof, &v_lat));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, v_lat, "standard_name", "latitude"));

  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "alt", NC_DOUBLE, 1, &dim_prof, &v_z));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, v_z, "axis", "Z"));

  int dims_prof[1] = {dim_prof};
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "temp", NC_FLOAT, 1, dims_prof, &v_val));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, v_val, "coordinates", "lon lat alt"));

  // 1D variables for extra types in NCDFGet1DVar (lines 11155-11235)
  int v_str = -1, v_ush = -1, v_ui = -1, v_ub = -1, v_i64 = -1, v_ui64 = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "str_var", NC_STRING, 1, dims_prof, &v_str));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "ush_var", NC_USHORT, 1, dims_prof, &v_ush));
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "ui_var", NC_UINT, 1, dims_prof, &v_ui));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "ub_var", NC_UBYTE, 1, dims_prof, &v_ub));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "i64_var", NC_INT64, 1, dims_prof, &v_i64));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "ui64_var", NC_UINT64, 1, dims_prof, &v_ui64));

  // Sentinel-5 JSON metadata with # in sub-groups (lines 6386-6395 and
  // 6418-6435)
  int grp_meta = -1, grp_iso = -1, grp_sub1 = -1, grp_sub2 = -1;
  ASSERT_EQ(NC_NOERR, nc_def_grp(ncid, "METADATA", &grp_meta));
  ASSERT_EQ(NC_NOERR, nc_def_grp(grp_meta, "ISO_METADATA", &grp_iso));
  ASSERT_EQ(NC_NOERR, PutTextAtt(grp_iso, NC_GLOBAL, "title", "test"));
  ASSERT_EQ(NC_NOERR, nc_def_grp(grp_iso, "item#1", &grp_sub1));
  ASSERT_EQ(NC_NOERR, nc_def_grp(grp_iso, "item#2", &grp_sub2));
  ASSERT_EQ(NC_NOERR, PutTextAtt(grp_sub1, NC_GLOBAL, "desc", "sub1"));
  ASSERT_EQ(NC_NOERR, PutTextAtt(grp_sub2, NC_GLOBAL, "desc", "sub2"));

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

  double lons[3] = {10., 11., 12.};
  double lats[3] = {45., 46., 47.};
  double alts[3] = {100., 200., 300.};
  float temps[3] = {20.f, 21.f, 22.f};
  const char* strs[3] = {"first", "second", "third"};
  unsigned short ushs[3] = {10, 20, 30};
  unsigned int uis[3] = {100, 200, 300};
  unsigned char ubs[3] = {1, 2, 3};
  long long i64s[3] = {1000, 2000, 3000};
  unsigned long long ui64s[3] = {10000, 20000, 30000};

  ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, v_lon, lons));
  ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, v_lat, lats));
  ASSERT_EQ(NC_NOERR, nc_put_var_double(ncid, v_z, alts));
  ASSERT_EQ(NC_NOERR, nc_put_var_float(ncid, v_val, temps));
  ASSERT_EQ(NC_NOERR, nc_put_var_string(ncid, v_str, strs));
  ASSERT_EQ(NC_NOERR, nc_put_var_ushort(ncid, v_ush, ushs));
  ASSERT_EQ(NC_NOERR, nc_put_var_uint(ncid, v_ui, uis));
  ASSERT_EQ(NC_NOERR, nc_put_var_uchar(ncid, v_ub, ubs));
  ASSERT_EQ(NC_NOERR, nc_put_var_longlong(ncid, v_i64, i64s));
  ASSERT_EQ(NC_NOERR, nc_put_var_ulonglong(ncid, v_ui64, ui64s));
  ASSERT_EQ(NC_NOERR, nc_close(ncid));

  // Open vector dataset
  auto poVecDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
      GDALOpenEx(fn_vec.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, poVecDS);
  EXPECT_GE(poVecDS->GetLayerCount(), 1);
  poVecDS.reset();

  // Open raster dataset to check Sentinel-5 JSON metadata
  auto poRasDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
      GDALOpenEx(fn_vec.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, poRasDS);
  char** papszMeta = poRasDS->GetMetadata("json:ISO_METADATA");
  EXPECT_NE(nullptr, papszMeta);
  poRasDS.reset();

  VSIUnlink(fn_vec.c_str());
}

TEST_F(NetCDFDatasetTest, IWriteBlockAllTypes) {
  auto poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  struct TypeInfo {
    GDALDataType eType;
    const char* pszPixelType;
  };
  const TypeInfo types[] = {
      {GDT_Byte, nullptr},    {GDT_Byte, "SIGNEDBYTE"}, {GDT_Int8, nullptr},
      {GDT_Int16, nullptr},   {GDT_UInt16, nullptr},    {GDT_Int32, nullptr},
      {GDT_UInt32, nullptr},  {GDT_Int64, nullptr},     {GDT_UInt64, nullptr},
      {GDT_Float32, nullptr}, {GDT_Float64, nullptr},
  };

  int idx = 0;
  for (const auto& ti : types) {
    const std::string fn = (std::filesystem::path(::testing::TempDir()) /
                            ("write_block_" + std::to_string(idx++) + ".nc"))
                               .string();
    char** papszOptions = nullptr;
    papszOptions = CSLSetNameValue(papszOptions, "FORMAT", "NC4");
    if (ti.pszPixelType) {
      papszOptions =
          CSLSetNameValue(papszOptions, "PIXELTYPE", ti.pszPixelType);
    }
    auto poDS = std::unique_ptr<GDALDataset>(
        poDriver->Create(fn.c_str(), 2, 2, 1, ti.eType, papszOptions));
    CSLDestroy(papszOptions);
    ASSERT_NE(nullptr, poDS);
    auto poBand = poDS->GetRasterBand(1);
    ASSERT_NE(nullptr, poBand);

    int nBlockX = 0, nBlockY = 0;
    poBand->GetBlockSize(&nBlockX, &nBlockY);
    const int nBytes = GDALGetDataTypeSizeBytes(ti.eType) * nBlockX * nBlockY;

    // Test SetNoDataValue / SetNoDataValueAsInt64 / SetNoDataValueAsUInt64
    // (Must be called before WriteBlock in NetCDF-4)
    if (ti.eType == GDT_Int64) {
      EXPECT_EQ(CE_None, poBand->SetNoDataValueAsInt64(-9999));
      int bSuccess = FALSE;
      EXPECT_EQ(int64_t(-9999), poBand->GetNoDataValueAsInt64(&bSuccess));
      EXPECT_TRUE(bSuccess);
      // Early exit if value already set
      EXPECT_EQ(CE_None, poBand->SetNoDataValueAsInt64(-9999));
    } else if (ti.eType == GDT_UInt64) {
      EXPECT_EQ(CE_None, poBand->SetNoDataValueAsUInt64(9999));
      int bSuccess = FALSE;
      EXPECT_EQ(uint64_t(9999), poBand->GetNoDataValueAsUInt64(&bSuccess));
      EXPECT_TRUE(bSuccess);
      // Early exit if value already set
      EXPECT_EQ(CE_None, poBand->SetNoDataValueAsUInt64(9999));
    } else {
      EXPECT_EQ(CE_None, poBand->SetNoDataValue(42.0));
      int bSuccess = FALSE;
      EXPECT_DOUBLE_EQ(42.0, poBand->GetNoDataValue(&bSuccess));
      EXPECT_TRUE(bSuccess);
      // Early exit if value already set
      EXPECT_EQ(CE_None, poBand->SetNoDataValue(42.0));
    }

    std::vector<GByte> buf(nBytes, 1);
    EXPECT_EQ(CE_None, poBand->WriteBlock(0, 0, buf.data()));

    poDS.reset();

    // Verify reading back
    auto poReadDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poReadDS);
    std::vector<GByte> read_buf(nBytes, 0);
    EXPECT_EQ(CE_None,
              poReadDS->GetRasterBand(1)->ReadBlock(0, 0, read_buf.data()));
    EXPECT_EQ(buf, read_buf);

    if (ti.eType == GDT_Int64) {
      int bSuccess = FALSE;
      EXPECT_EQ(int64_t(-9999),
                poReadDS->GetRasterBand(1)->GetNoDataValueAsInt64(&bSuccess));
      EXPECT_TRUE(bSuccess);
    } else if (ti.eType == GDT_UInt64) {
      int bSuccess = FALSE;
      EXPECT_EQ(uint64_t(9999),
                poReadDS->GetRasterBand(1)->GetNoDataValueAsUInt64(&bSuccess));
      EXPECT_TRUE(bSuccess);
    } else {
      int bSuccess = FALSE;
      EXPECT_DOUBLE_EQ(42.0,
                       poReadDS->GetRasterBand(1)->GetNoDataValue(&bSuccess));
      EXPECT_TRUE(bSuccess);
    }

    poReadDS.reset();

    VSIUnlink(fn.c_str());
  }

  // Bottom-up dataset opened in update mode: the driver forces 1-row blocks
  // (even for a chunked variable) so that IWriteBlock succeeds, rather than
  // hitting the "bottom-up with block Y size > 1" failure path.
  {
    const std::string fn_err =
        (std::filesystem::path(::testing::TempDir()) / "write_err_bu.nc")
            .string();
    int ncid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_create(fn_err.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));
    int dim_x = -1, dim_y = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "x", 4, &dim_x));
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "y", 4, &dim_y));
    int dims[2] = {dim_y, dim_x};
    int var = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "data", NC_BYTE, 2, dims, &var));
    size_t chunksize[2] = {2, 2};
    ASSERT_EQ(NC_NOERR, nc_def_var_chunking(ncid, var, NC_CHUNKED, chunksize));
    ASSERT_EQ(NC_NOERR, nc_enddef(ncid));
    unsigned char dummy[16] = {0};
    ASSERT_EQ(NC_NOERR, nc_put_var_uchar(ncid, var, dummy));
    ASSERT_EQ(NC_NOERR, nc_close(ncid));

    std::unique_ptr<GDALDataset> poDS;
    {
      CPLConfigOptionSetter oSetter("GDAL_NETCDF_BOTTOMUP", "YES",
                                    /*bSetOnlyIfUndefined=*/false);
      poDS.reset(static_cast<GDALDataset*>(
          GDALOpenEx(fn_err.c_str(), GDAL_OF_UPDATE | GDAL_OF_RASTER, nullptr,
                     nullptr, nullptr)));
    }
    ASSERT_NE(nullptr, poDS);
    auto poBand = poDS->GetRasterBand(1);
    ASSERT_NE(nullptr, poBand);
    int bx = 0, by = 0;
    poBand->GetBlockSize(&bx, &by);
    EXPECT_EQ(1, by);
    std::vector<GByte> buf(bx * by, 0);
    EXPECT_EQ(CE_None, poBand->WriteBlock(0, 0, buf.data()));
    poDS.reset();
    VSIUnlink(fn_err.c_str());
  }
}

TEST_F(NetCDFDatasetTest, ExtraDimensionsAndNoDataVariations) {
  const std::string fn =
      (std::filesystem::path(::testing::TempDir()) / "extradim_nodata.nc")
          .string();
  int ncid = -1;
  ASSERT_EQ(NC_NOERR, nc_create(fn.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

  int dim_x = -1, dim_y = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "x", 2, &dim_x));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "y", 2, &dim_y));

  // 1. Extra dimensions of types STRING, UBYTE, USHORT, UINT, INT64, UINT64
  int dim_str = -1, dim_ub = -1, dim_us = -1, dim_ui = -1, dim_i64 = -1,
      dim_ui64 = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_str", 2, &dim_str));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_ub", 2, &dim_ub));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_us", 2, &dim_us));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_ui", 2, &dim_ui));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_i64", 2, &dim_i64));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_ui64", 2, &dim_ui64));

  int var_d_str = -1, var_d_ub = -1, var_d_us = -1, var_d_ui = -1,
      var_d_i64 = -1, var_d_ui64 = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "dim_str", NC_STRING, 1, &dim_str, &var_d_str));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "dim_ub", NC_UBYTE, 1, &dim_ub, &var_d_ub));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "dim_us", NC_USHORT, 1, &dim_us, &var_d_us));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "dim_ui", NC_UINT, 1, &dim_ui, &var_d_ui));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "dim_i64", NC_INT64, 1, &dim_i64, &var_d_i64));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "dim_ui64", NC_UINT64, 1, &dim_ui64, &var_d_ui64));

  int dims_c_str[3] = {dim_str, dim_y, dim_x};
  int dims_c_ub[3] = {dim_ub, dim_y, dim_x};
  int dims_c_us[3] = {dim_us, dim_y, dim_x};
  int dims_c_ui[3] = {dim_ui, dim_y, dim_x};
  int dims_c_i64[3] = {dim_i64, dim_y, dim_x};
  int dims_c_ui64[3] = {dim_ui64, dim_y, dim_x};

  int c_str = -1, c_ub = -1, c_us = -1, c_ui = -1, c_i64 = -1, c_ui64 = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "cube_str", NC_FLOAT, 3, dims_c_str, &c_str));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "cube_ub", NC_FLOAT, 3, dims_c_ub, &c_ub));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "cube_us", NC_FLOAT, 3, dims_c_us, &c_us));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "cube_ui", NC_FLOAT, 3, dims_c_ui, &c_ui));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "cube_i64", NC_FLOAT, 3, dims_c_i64, &c_i64));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "cube_ui64", NC_FLOAT, 3, dims_c_ui64, &c_ui64));

  // 2. 2D variable of type NC_INT64 and NC_UINT64 without nodata attribute
  int dims_2d[2] = {dim_y, dim_x};
  int var_no_nd_i64 = -1, var_no_nd_ui64 = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "var_no_nd_i64", NC_INT64, 2, dims_2d,
                                 &var_no_nd_i64));
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "var_no_nd_ui64", NC_UINT64, 2, dims_2d,
                                 &var_no_nd_ui64));

  // 3. 2D variable with valid_min and valid_max
  int var_valid_min_max = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "var_vm", NC_FLOAT, 2, dims_2d,
                                 &var_valid_min_max));
  double dfVMin = 5.0, dfVMax = 50.0;
  ASSERT_EQ(NC_NOERR, nc_put_att_double(ncid, var_valid_min_max, "valid_min",
                                        NC_DOUBLE, 1, &dfVMin));
  ASSERT_EQ(NC_NOERR, nc_put_att_double(ncid, var_valid_min_max, "valid_max",
                                        NC_DOUBLE, 1, &dfVMax));

  // 4. 2D variable of NC_SHORT with _Unsigned="true" and negative _FillValue
  int var_us_neg_fill = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "var_us_neg", NC_SHORT, 2, dims_2d,
                                 &var_us_neg_fill));
  ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var_us_neg_fill, "_Unsigned", "true"));
  short sFill = -999;
  ASSERT_EQ(NC_NOERR, nc_put_att_short(ncid, var_us_neg_fill, "_FillValue",
                                       NC_SHORT, 1, &sFill));

  // 5. 3D cube with 4 levels for GDAL_MAX_BAND_COUNT
  int dim_lev4 = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_lev4", 4, &dim_lev4));
  int dims_c4[3] = {dim_lev4, dim_y, dim_x};
  int c4 = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "cube_lev4", NC_FLOAT, 3, dims_c4, &c4));

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

  // Populate extra dim coordinate variables
  const char* str_data[2] = {"alpha", "beta"};
  ASSERT_EQ(NC_NOERR, nc_put_var_string(ncid, var_d_str, str_data));
  unsigned char ub_data[2] = {10, 20};
  ASSERT_EQ(NC_NOERR, nc_put_var_uchar(ncid, var_d_ub, ub_data));
  unsigned short us_data[2] = {1000, 2000};
  ASSERT_EQ(NC_NOERR, nc_put_var_ushort(ncid, var_d_us, us_data));
  unsigned int ui_data[2] = {100000, 200000};
  ASSERT_EQ(NC_NOERR, nc_put_var_uint(ncid, var_d_ui, ui_data));
  long long i64_data[2] = {-10000000000LL, 20000000000LL};
  ASSERT_EQ(NC_NOERR, nc_put_var_longlong(ncid, var_d_i64, i64_data));
  unsigned long long ui64_data[2] = {10000000000ULL, 20000000000ULL};
  ASSERT_EQ(NC_NOERR, nc_put_var_ulonglong(ncid, var_d_ui64, ui64_data));

  ASSERT_EQ(NC_NOERR, nc_close(ncid));

  // Test opening extra dimension cubes with
  // GDAL_NETCDF_REPORT_EXTRA_DIM_VALUES=YES
  {
    CPLConfigOptionSetter oSetter("GDAL_NETCDF_REPORT_EXTRA_DIM_VALUES", "YES",
                                  /*bSetOnlyIfUndefined=*/false);
    const char* const cube_names[] = {"cube_str", "cube_ub",  "cube_us",
                                      "cube_ui",  "cube_i64", "cube_ui64"};
    for (const char* name : cube_names) {
      const std::string sub_fn = std::string("NETCDF:\"") + fn + "\":" + name;
      auto poDS =
          std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
              sub_fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
      ASSERT_NE(nullptr, poDS);
      EXPECT_EQ(2, poDS->GetRasterCount());
    }
  }

  // Test default nodata for NC_INT64 and NC_UINT64 without _FillValue
  {
    const std::string sub_fn =
        std::string("NETCDF:\"") + fn + "\":var_no_nd_i64";
    auto poDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(sub_fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    int bSuccess = FALSE;
    EXPECT_EQ(NC_FILL_INT64,
              poDS->GetRasterBand(1)->GetNoDataValueAsInt64(&bSuccess));
    EXPECT_TRUE(bSuccess);
  }
  {
    const std::string sub_fn =
        std::string("NETCDF:\"") + fn + "\":var_no_nd_ui64";
    auto poDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(sub_fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    int bSuccess = FALSE;
    EXPECT_EQ(NC_FILL_UINT64,
              poDS->GetRasterBand(1)->GetNoDataValueAsUInt64(&bSuccess));
    EXPECT_TRUE(bSuccess);
  }

  // Test valid_min and valid_max
  {
    const std::string sub_fn = std::string("NETCDF:\"") + fn + "\":var_vm";
    auto poDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(sub_fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
  }

  // Test NC_SHORT with _Unsigned="true" and negative _FillValue -> +65536
  {
    const std::string sub_fn = std::string("NETCDF:\"") + fn + "\":var_us_neg";
    auto poDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(sub_fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    int bSuccess = FALSE;
    EXPECT_DOUBLE_EQ(65536.0 - 999.0,
                     poDS->GetRasterBand(1)->GetNoDataValue(&bSuccess));
    EXPECT_TRUE(bSuccess);
  }

  // Test GDAL_MAX_BAND_COUNT limiting 4 bands to 2
  {
    std::unique_ptr<GDALDataset> poDS;
    {
      CPLConfigOptionSetter oSetter("GDAL_MAX_BAND_COUNT", "2",
                                    /*bSetOnlyIfUndefined=*/false);
      CPLErrorReset();
      const std::string sub_fn = std::string("NETCDF:\"") + fn + "\":cube_lev4";
      {
        CPLErrorHandlerPusher oQuiet(CPLQuietErrorHandler);
        poDS.reset(static_cast<GDALDataset*>(GDALOpenEx(
            sub_fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
      }
    }
    ASSERT_NE(nullptr, poDS);
    EXPECT_EQ(2, poDS->GetRasterCount());
  }

  VSIUnlink(fn.c_str());
}

TEST_F(NetCDFDatasetTest, CreateCopyAndConfigFileVariations) {
  auto poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  // 1. Config file parsing in Create and CreateLayer
  {
    const std::string xml_fn =
        (std::filesystem::path(::testing::TempDir()) / "writer_config.xml")
            .string();
    const char* pszXML =
        "<Configuration>\n"
        "  <DatasetCreationOption name=\"FORMAT\" value=\"NC4\"/>\n"
        "  <LayerCreationOption name=\"GEOMETRY_ENCODING\" value=\"WKT\"/>\n"
        "  <Layer name=\"custom_layer\">\n"
        "    <LayerCreationOption name=\"STRING_DEFAULT_WIDTH\" "
        "value=\"100\"/>\n"
        "  </Layer>\n"
        "</Configuration>\n";
    auto fp = VSIFOpenL(xml_fn.c_str(), "wb");
    ASSERT_NE(nullptr, fp);
    VSIFWriteL(pszXML, 1, strlen(pszXML), fp);
    VSIFCloseL(fp);

    const std::string ds_fn =
        (std::filesystem::path(::testing::TempDir()) / "ds_with_config.nc")
            .string();
    char** papszOptions = nullptr;
    papszOptions = CSLSetNameValue(papszOptions, "CONFIG_FILE", xml_fn.c_str());
    auto poDS = std::unique_ptr<GDALDataset>(
        poDriver->Create(ds_fn.c_str(), 0, 0, 0, GDT_Unknown, papszOptions));
    CSLDestroy(papszOptions);
    ASSERT_NE(nullptr, poDS);

    auto poLayer =
        poDS->CreateLayer("custom_layer", nullptr, wkbPoint, nullptr);
    ASSERT_NE(nullptr, poLayer);

    poDS.reset();
    VSIUnlink(xml_fn.c_str());
    VSIUnlink(ds_fn.c_str());
  }

  // 2. Create and CreateCopy with GDT_Int64 and no FORMAT option ->
  // auto-selects NC4
  {
    const std::string fn_c =
        (std::filesystem::path(::testing::TempDir()) / "auto_nc4_c.nc")
            .string();
    auto poDS = std::unique_ptr<GDALDataset>(
        poDriver->Create(fn_c.c_str(), 2, 2, 1, GDT_Int64, nullptr));
    ASSERT_NE(nullptr, poDS);
    poDS.reset();
    VSIUnlink(fn_c.c_str());

    // CreateCopy with GDT_Int64
    auto poMemDriver = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_NE(nullptr, poMemDriver);
    auto poMemDS = std::unique_ptr<GDALDataset>(
        poMemDriver->Create("", 2, 2, 1, GDT_Int64, nullptr));
    ASSERT_NE(nullptr, poMemDS);

    const std::string fn_cc =
        (std::filesystem::path(::testing::TempDir()) / "auto_nc4_cc.nc")
            .string();
    auto poCopyDS = std::unique_ptr<GDALDataset>(poDriver->CreateCopy(
        fn_cc.c_str(), poMemDS.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, poCopyDS);
    poCopyDS.reset();
    VSIUnlink(fn_cc.c_str());
  }

  // 3. CreateCopy with multidimensional dataset (GetRootGroup() != nullptr)
  {
    const std::string mdim_src =
        (std::filesystem::path(::testing::TempDir()) / "mdim_src.nc").string();
    auto poMDS = std::unique_ptr<GDALDataset>(
        poDriver->CreateMultiDimensional(mdim_src.c_str(), nullptr, nullptr));
    ASSERT_NE(nullptr, poMDS);

    const std::string mdim_dst =
        (std::filesystem::path(::testing::TempDir()) / "mdim_dst.nc").string();
    auto poCopy = std::unique_ptr<GDALDataset>(poDriver->CreateCopy(
        mdim_dst.c_str(), poMDS.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, poCopy);
    poCopy.reset();
    poMDS.reset();
    VSIUnlink(mdim_dst.c_str());
    VSIUnlink(mdim_src.c_str());
  }

  // 4. CreateCopy with BAND_NAMES, STATISTICS_*, NETCDF_DIM_*, missing_value,
  // _FillValue metadata
  {
    auto poMemDriver = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_NE(nullptr, poMemDriver);
    auto poSrcDS = std::unique_ptr<GDALDataset>(
        poMemDriver->Create("", 2, 2, 2, GDT_Float32, nullptr));
    ASSERT_NE(nullptr, poSrcDS);

    auto poBand1 = poSrcDS->GetRasterBand(1);
    poBand1->SetMetadataItem("STATISTICS_MINIMUM", "1.0");
    poBand1->SetMetadataItem("STATISTICS_MAXIMUM", "10.0");
    poBand1->SetMetadataItem("STATISTICS_MEAN", "5.0");
    poBand1->SetMetadataItem("NETCDF_DIM_EXTRA", "{1,2}");
    poBand1->SetMetadataItem("missing_value", "-999");
    poBand1->SetMetadataItem("_FillValue", "-999");
    poBand1->SetMetadataItem("custom_attr", "hello");

    char** papszCCOptions = nullptr;
    papszCCOptions = CSLSetNameValue(papszCCOptions, "FORMAT", "NC4");
    papszCCOptions =
        CSLSetNameValue(papszCCOptions, "BAND_NAMES", "bandA,bandB");

    const std::string cc_meta_fn =
        (std::filesystem::path(::testing::TempDir()) / "cc_meta.nc").string();
    auto poDst = std::unique_ptr<GDALDataset>(
        poDriver->CreateCopy(cc_meta_fn.c_str(), poSrcDS.get(), FALSE,
                             papszCCOptions, nullptr, nullptr));
    CSLDestroy(papszCCOptions);
    ASSERT_NE(nullptr, poDst);
    poDst.reset();
    VSIUnlink(cc_meta_fn.c_str());
  }

  // 5. Geolocation Y_VALUES with WRITE_BOTTOMUP=YES
  {
    auto poMemDriver = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_NE(nullptr, poMemDriver);
    auto poSrcDS = std::unique_ptr<GDALDataset>(
        poMemDriver->Create("", 2, 2, 1, GDT_Float32, nullptr));
    ASSERT_NE(nullptr, poSrcDS);
    poSrcDS->SetMetadataItem("Y_VALUES", "10.0,20.0", "GEOLOCATION");
    poSrcDS->SetMetadataItem("Y_VALUES", "10.0,20.0", "GEOLOCATION2");
    double adfGT[6] = {0, 1, 0, 20, 0, -1};
    poSrcDS->SetGeoTransform(adfGT);

    char** papszOptions = nullptr;
    papszOptions = CSLSetNameValue(papszOptions, "FORMAT", "NC4");
    papszOptions = CSLSetNameValue(papszOptions, "WRITE_BOTTOMUP", "YES");
    const std::string bu_geo_fn =
        (std::filesystem::path(::testing::TempDir()) / "bu_geo.nc").string();
    auto poDst = std::unique_ptr<GDALDataset>(
        poDriver->CreateCopy(bu_geo_fn.c_str(), poSrcDS.get(), FALSE,
                             papszOptions, nullptr, nullptr));
    CSLDestroy(papszOptions);
    ASSERT_NE(nullptr, poDst);
    poDst.reset();
    VSIUnlink(bu_geo_fn.c_str());
  }
}

TEST_F(NetCDFDatasetTest, OpenDatasetEdgeCases) {
  // 1. Empty NetCDF file with 0 variables
  {
    const std::string fn_empty =
        (std::filesystem::path(::testing::TempDir()) / "empty_dataset.nc")
            .string();
    int ncid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_create(fn_empty.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));
    ASSERT_EQ(NC_NOERR, nc_enddef(ncid));
    ASSERT_EQ(NC_NOERR, nc_close(ncid));

    // Open with GDAL_OF_RASTER -> nullptr
    auto poR =
        std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
            fn_empty.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    EXPECT_EQ(nullptr, poR);

    // Open with GDAL_OF_VECTOR and GA_ReadOnly -> nullptr
    auto poV =
        std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
            fn_empty.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)));
    EXPECT_EQ(nullptr, poV);

    VSIUnlink(fn_empty.c_str());
  }

  // 2. NetCDF file with scalar variable, 1D-only variable, same dimension
  // twice, and 3D/4D dimension names
  {
    const std::string fn_vars =
        (std::filesystem::path(::testing::TempDir()) / "vars_edge_cases.nc")
            .string();
    int ncid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_create(fn_vars.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));

    // Scalar variable (0 dimensions)
    int var_scalar = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "scalar_var", NC_INT, 0, nullptr, &var_scalar));

    // Dimensions
    int dim_x = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_x", 3, &dim_x));

    // 2D variable with the SAME dimension used twice: {dim_x, dim_x}
    int same_dims[2] = {dim_x, dim_x};
    int var_same = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "same_dim", NC_INT, 2, same_dims, &var_same));

    // 3D variable with 3rd dimension not Time or Vertical
    int dim_y = -1, dim_non_vert = -1, dim_non_time = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_y", 2, &dim_y));
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "custom_non_vert", 2, &dim_non_vert));
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "custom_non_time", 2, &dim_non_time));

    int dims_3d[3] = {dim_non_vert, dim_y, dim_x};
    int var_3d = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "var_3d_custom", NC_FLOAT, 3, dims_3d, &var_3d));

    int dims_4d[4] = {dim_non_time, dim_non_vert, dim_y, dim_x};
    int var_4d = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "var_4d_custom", NC_FLOAT, 4, dims_4d, &var_4d));

    ASSERT_EQ(NC_NOERR, nc_enddef(ncid));
    ASSERT_EQ(NC_NOERR, nc_close(ncid));

    // Open scalar variable -> warning "Variable has 0 dimension(s) - not
    // supported."
    {
      const std::string sub_fn =
          std::string("NETCDF:\"") + fn_vars + "\":scalar_var";
      CPLErrorReset();
      std::unique_ptr<GDALDataset> poDS;
      {
        CPLErrorHandlerPusher oQuiet(CPLQuietErrorHandler);
        poDS.reset(static_cast<GDALDataset*>(GDALOpenEx(
            sub_fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
      }
      EXPECT_EQ(nullptr, poDS);
    }

    // Open nonexistent variable -> warning "... is not a variable"
    {
      const std::string sub_fn =
          std::string("NETCDF:\"") + fn_vars + "\":nonexistent_var";
      CPLErrorReset();
      std::unique_ptr<GDALDataset> poDS;
      {
        CPLErrorHandlerPusher oQuiet(CPLQuietErrorHandler);
        poDS.reset(static_cast<GDALDataset*>(GDALOpenEx(
            sub_fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
      }
      EXPECT_EQ(nullptr, poDS);
    }

    // Open variable with same dim twice -> nullptr (X and Y Dimension Ids were
    // not found)
    {
      const std::string sub_fn =
          std::string("NETCDF:\"") + fn_vars + "\":same_dim";
      CPLErrorReset();
      std::unique_ptr<GDALDataset> poDS;
      {
        CPLErrorHandlerPusher oQuiet(CPLQuietErrorHandler);
        poDS.reset(static_cast<GDALDataset*>(GDALOpenEx(
            sub_fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
      }
      EXPECT_EQ(nullptr, poDS);
    }

    // Open 3D variable with custom dimension -> issues warning but succeeds
    {
      const std::string sub_fn =
          std::string("NETCDF:\"") + fn_vars + "\":var_3d_custom";
      CPLErrorReset();
      std::unique_ptr<GDALDataset> poDS;
      {
        CPLErrorHandlerPusher oQuiet(CPLQuietErrorHandler);
        poDS.reset(static_cast<GDALDataset*>(GDALOpenEx(
            sub_fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
      }
      ASSERT_NE(nullptr, poDS);
    }

    // Open 4D variable with custom dimensions -> issues warnings but succeeds
    {
      const std::string sub_fn =
          std::string("NETCDF:\"") + fn_vars + "\":var_4d_custom";
      CPLErrorReset();
      std::unique_ptr<GDALDataset> poDS;
      {
        CPLErrorHandlerPusher oQuiet(CPLQuietErrorHandler);
        poDS.reset(static_cast<GDALDataset*>(GDALOpenEx(
            sub_fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
      }
      ASSERT_NE(nullptr, poDS);
    }

    VSIUnlink(fn_vars.c_str());
  }

  // 3. File with only a 1D variable opened with GDAL_OF_RASTER (nRasterXSize ==
  // 0)
  {
    const std::string fn_1d =
        (std::filesystem::path(::testing::TempDir()) / "only_1d.nc").string();
    int ncid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_create(fn_1d.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));
    int dim = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim1", 5, &dim));
    int var = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "var1", NC_INT, 1, &dim, &var));
    ASSERT_EQ(NC_NOERR, nc_enddef(ncid));
    ASSERT_EQ(NC_NOERR, nc_close(ncid));

    auto poDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(fn_1d.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    EXPECT_EQ(nullptr, poDS);

    VSIUnlink(fn_1d.c_str());
  }
}

TEST_F(NetCDFDatasetTest, Put1DVarAndGeolocationBands) {
  auto poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);
  auto poMemDriver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, poMemDriver);

  // 1. Driver GetMetadata (exercises GDALnetCDFDriver::GetMetadata lines
  // 10274-10280)
  char** papszDriverMeta = poDriver->GetMetadata(nullptr);
  EXPECT_NE(nullptr, papszDriverMeta);

  // 2. Signed byte chunk reading (exercises lines 2360-2367 in
  // FetchNetcdfChunk)
  {
    const std::string fn_sb =
        (std::filesystem::path(::testing::TempDir()) / "signed_chunk.nc")
            .string();
    int ncid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_create(fn_sb.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));
    int dx = -1, dy = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "x", 2, &dx));
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "y", 2, &dy));
    int dims[2] = {dy, dx};
    int var = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "data", NC_BYTE, 2, dims, &var));
    size_t chunks[2] = {2, 2};
    ASSERT_EQ(NC_NOERR, nc_def_var_chunking(ncid, var, NC_CHUNKED, chunks));
    ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var, "_Unsigned", "false"));
    ASSERT_EQ(NC_NOERR, nc_enddef(ncid));
    signed char sb_data[4] = {-1, -2, 3, 4};
    ASSERT_EQ(NC_NOERR, nc_put_var_schar(ncid, var, sb_data));
    ASSERT_EQ(NC_NOERR, nc_close(ncid));

    auto poDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(fn_sb.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    std::vector<GByte> buf(4);
    EXPECT_EQ(CE_None, poDS->GetRasterBand(1)->ReadBlock(0, 0, buf.data()));
    poDS.reset();
    VSIUnlink(fn_sb.c_str());
  }

  // 3. WRITE_GDAL_HISTORY=NO with source history: the previous history is
  // copied verbatim instead of being prefixed with a new GDAL entry.
  {
    auto poSrcDS = std::unique_ptr<GDALDataset>(
        poMemDriver->Create("", 2, 2, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poSrcDS);
    poSrcDS->SetMetadataItem("NC_GLOBAL#history", "previous history");
    char** papszHistOpts = nullptr;
    papszHistOpts = CSLSetNameValue(papszHistOpts, "FORMAT", "NC4");
    papszHistOpts = CSLSetNameValue(papszHistOpts, "WRITE_GDAL_HISTORY", "NO");
    const std::string hist_fn =
        (std::filesystem::path(::testing::TempDir()) / "hist_no.nc").string();
    auto poDst = std::unique_ptr<GDALDataset>(
        poDriver->CreateCopy(hist_fn.c_str(), poSrcDS.get(), FALSE,
                             papszHistOpts, nullptr, nullptr));
    CSLDestroy(papszHistOpts);
    ASSERT_NE(nullptr, poDst);
    poDst.reset();

    auto poRead =
        std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
            hist_fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poRead);
    EXPECT_STREQ("previous history",
                 poRead->GetMetadataItem("NC_GLOBAL#history"));
    poRead.reset();
    VSIUnlink(hist_fn.c_str());
  }

  // 4. Geolocation Y_VALUES with SetGeoTransform and WRITE_BOTTOMUP=YES
  // (exercises lines 4993-4997, 6145-6155)
  {
    const std::string bu_fn =
        (std::filesystem::path(::testing::TempDir()) / "bu_direct.nc").string();
    char** papszBuOpts = nullptr;
    papszBuOpts = CSLSetNameValue(papszBuOpts, "FORMAT", "NC4");
    papszBuOpts = CSLSetNameValue(papszBuOpts, "WRITE_BOTTOMUP", "YES");
    auto poBUDS = std::unique_ptr<GDALDataset>(
        poDriver->Create(bu_fn.c_str(), 2, 2, 1, GDT_Byte, papszBuOpts));
    CSLDestroy(papszBuOpts);
    ASSERT_NE(nullptr, poBUDS);
    poBUDS->SetMetadataItem("Y_VALUES", "10.0,20.0", "GEOLOCATION");
    poBUDS->SetMetadataItem("Y_VALUES", "10.0,20.0", "GEOLOCATION2");
    double adfGT[6] = {0, 1, 0, 20, 0, -1};
    poBUDS->SetGeoTransform(adfGT);
    poBUDS.reset();
    VSIUnlink(bu_fn.c_str());
  }

  // 5. 2D Geolocation bands RasterIO (exercises lines 5968-5986, 6004-6006)
  {
    const std::string fn_gx =
        (std::filesystem::path(::testing::TempDir()) / "gx_band.nc").string();
    const std::string fn_gy =
        (std::filesystem::path(::testing::TempDir()) / "gy_band.nc").string();
    auto poDS_X = std::unique_ptr<GDALDataset>(
        poDriver->Create(fn_gx.c_str(), 2, 2, 1, GDT_Float64, nullptr));
    auto poDS_Y = std::unique_ptr<GDALDataset>(
        poDriver->Create(fn_gy.c_str(), 2, 2, 1, GDT_Float64, nullptr));
    ASSERT_NE(nullptr, poDS_X);
    ASSERT_NE(nullptr, poDS_Y);
    double x_vals[4] = {10.0, 11.0, 10.0, 11.0};
    double y_vals[4] = {20.0, 20.0, 21.0, 21.0};
    EXPECT_EQ(CE_None, poDS_X->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 2, 2, x_vals, 2, 2, GDT_Float64, 0,
                           0, nullptr));
    EXPECT_EQ(CE_None, poDS_Y->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 2, 2, y_vals, 2, 2, GDT_Float64, 0,
                           0, nullptr));
    poDS_X.reset();
    poDS_Y.reset();

    auto poSrcDS = std::unique_ptr<GDALDataset>(
        poMemDriver->Create("", 2, 2, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poSrcDS);
    poSrcDS->SetMetadataItem("X_DATASET", fn_gx.c_str(), "GEOLOCATION");
    poSrcDS->SetMetadataItem("Y_DATASET", fn_gy.c_str(), "GEOLOCATION");
    poSrcDS->SetMetadataItem("X_BAND", "1", "GEOLOCATION");
    poSrcDS->SetMetadataItem("Y_BAND", "1", "GEOLOCATION");

    char** papszGeoOpts = nullptr;
    papszGeoOpts = CSLSetNameValue(papszGeoOpts, "FORMAT", "NC4");
    papszGeoOpts = CSLSetNameValue(papszGeoOpts, "WRITE_LONLAT", "YES");
    const std::string fn_geoloc_out =
        (std::filesystem::path(::testing::TempDir()) / "geoloc_out.nc")
            .string();
    auto poDst = std::unique_ptr<GDALDataset>(
        poDriver->CreateCopy(fn_geoloc_out.c_str(), poSrcDS.get(), FALSE,
                             papszGeoOpts, nullptr, nullptr));
    CSLDestroy(papszGeoOpts);
    ASSERT_NE(nullptr, poDst);
    poDst.reset();
    VSIUnlink(fn_geoloc_out.c_str());
    VSIUnlink(fn_gx.c_str());
    VSIUnlink(fn_gy.c_str());
  }

  // 6. NCDFPut1DVar types: STRING, UBYTE, USHORT, UINT, INT64, UINT64
  // (exercises lines 11408-11490)
  {
    const int type_codes[] = {12, 7, 8, 9, 10, 11};
    const char* const type_vals[] = {"{alpha,beta}",
                                     "{10,20}",
                                     "{1000,2000}",
                                     "{100000,200000}",
                                     "{-10000000000,20000000000}",
                                     "{10000000000,20000000000}"};
    for (int i = 0; i < 6; ++i) {
      auto poSrc = std::unique_ptr<GDALDataset>(
          poMemDriver->Create("", 2, 2, 2, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, poSrc);
      poSrc->SetMetadataItem("NETCDF_DIM_EXTRA", "{my_dim}");
      poSrc->SetMetadataItem("NETCDF_DIM_my_dim_DEF",
                             CPLSPrintf("{2,%d}", type_codes[i]));
      poSrc->SetMetadataItem("NETCDF_DIM_my_dim_VALUES", type_vals[i]);

      char** papszOpts = nullptr;
      papszOpts = CSLSetNameValue(papszOpts, "FORMAT", "NC4");
      const std::string fn_dim = (std::filesystem::path(::testing::TempDir()) /
                                  ("put_dim_" + std::to_string(i) + ".nc"))
                                     .string();
      auto poCopy = std::unique_ptr<GDALDataset>(poDriver->CreateCopy(
          fn_dim.c_str(), poSrc.get(), FALSE, papszOpts, nullptr, nullptr));
      CSLDestroy(papszOpts);
      ASSERT_NE(nullptr, poCopy);
      poCopy.reset();
      VSIUnlink(fn_dim.c_str());
    }
  }
}

TEST_F(NetCDFDatasetTest, FinalCoveragePush) {
  auto poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);
  auto poMemDriver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, poMemDriver);

  // 1. Partial X chunk reading in CheckData (lines 2140-2150)
  {
    const std::string fn_partial =
        (std::filesystem::path(::testing::TempDir()) / "partial_chunk.nc")
            .string();
    int ncid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_create(fn_partial.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));
    int dx = -1, dy = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "x", 5, &dx));
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "y", 3, &dy));
    int dims[2] = {dy, dx};
    int var = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "data", NC_INT, 2, dims, &var));
    size_t chunks[2] = {3, 3};
    ASSERT_EQ(NC_NOERR, nc_def_var_chunking(ncid, var, NC_CHUNKED, chunks));
    ASSERT_EQ(NC_NOERR, nc_enddef(ncid));
    int data[15] = {0};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(ncid, var, data));
    ASSERT_EQ(NC_NOERR, nc_close(ncid));

    auto poDS =
        std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
            fn_partial.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    std::vector<int> buf(3 * 3);
    // Block (1, 0) has nTmpBlockXSize = 2 != nBlockXSize = 3
    EXPECT_EQ(CE_None, poDS->GetRasterBand(1)->ReadBlock(1, 0, buf.data()));
    poDS.reset();
    VSIUnlink(fn_partial.c_str());
  }

  // 2. 4D dataset without coordinate variables (lines 1980-1990, 2004-2006)
  {
    const std::string fn_4d =
        (std::filesystem::path(::testing::TempDir()) / "cube_4d_dims.nc")
            .string();
    int ncid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_create(fn_4d.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));
    int dt = -1, dz = -1, dy = -1, dx = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_t", 2, &dt));
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_z", 2, &dz));
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "y", 2, &dy));
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "x", 2, &dx));
    int dims[4] = {dt, dz, dy, dx};
    int var = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "data4d", NC_FLOAT, 4, dims, &var));
    ASSERT_EQ(NC_NOERR, nc_enddef(ncid));
    ASSERT_EQ(NC_NOERR, nc_close(ncid));

    const std::string sub_fn = std::string("NETCDF:\"") + fn_4d + "\":data4d";
    auto poDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(sub_fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);
    EXPECT_EQ(4, poDS->GetRasterCount());
    EXPECT_NE(nullptr,
              poDS->GetRasterBand(1)->GetMetadataItem("NETCDF_DIM_dim_t"));
    poDS.reset();
    VSIUnlink(fn_4d.c_str());
  }

  // 3. Integer variable with fractional valid_range and scale_factor (lines
  // 789-807)
  {
    const std::string fn_frac =
        (std::filesystem::path(::testing::TempDir()) / "frac_valid_range.nc")
            .string();
    int ncid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_create(fn_frac.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));
    int dx = -1, dy = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "x", 2, &dx));
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "y", 2, &dy));
    int dims[2] = {dy, dx};
    int var = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "int_data", NC_SHORT, 2, dims, &var));
    ASSERT_EQ(NC_NOERR, PutTextAtt(ncid, var, "valid_range", "{1.5, 10.5}"));
    double dfScale = 0.1;
    ASSERT_EQ(NC_NOERR, nc_put_att_double(ncid, var, "scale_factor", NC_DOUBLE,
                                          1, &dfScale));
    ASSERT_EQ(NC_NOERR, nc_enddef(ncid));
    ASSERT_EQ(NC_NOERR, nc_close(ncid));

    CPLErrorReset();
    std::unique_ptr<GDALDataset> poDS;
    {
      CPLErrorHandlerPusher oQuiet(CPLQuietErrorHandler);
      poDS.reset(static_cast<GDALDataset*>(GDALOpenEx(
          fn_frac.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    }
    ASSERT_NE(nullptr, poDS);
    poDS.reset();
    VSIUnlink(fn_frac.c_str());
  }

  // 4. Unsupported GDAL datatype in Classic NetCDF (lines 992-1000)
  {
    const std::string fn_classic =
        (std::filesystem::path(::testing::TempDir()) / "classic_uint32.nc")
            .string();
    char** papszOpts = nullptr;
    papszOpts = CSLSetNameValue(papszOpts, "FORMAT", "NC");
    CPLErrorReset();
    std::unique_ptr<GDALDataset> poDS;
    {
      CPLErrorHandlerPusher oQuiet(CPLQuietErrorHandler);
      poDS.reset(
          poDriver->Create(fn_classic.c_str(), 2, 2, 1, GDT_UInt32, papszOpts));
    }
    CSLDestroy(papszOpts);
    ASSERT_NE(nullptr, poDS);
    EXPECT_EQ(GDT_Float32, poDS->GetRasterBand(1)->GetRasterDataType());
    poDS.reset();
    VSIUnlink(fn_classic.c_str());
  }

  // 5. Multiple 1D candidate variables for a dimension (lines 1873-1883)
  {
    const std::string fn_multi_1d =
        (std::filesystem::path(::testing::TempDir()) / "multi_1d_vars.nc")
            .string();
    int ncid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_create(fn_multi_1d.c_str(), NC_CLOBBER | NC_NETCDF4, &ncid));
    int dz = -1, dy = -1, dx = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_multi", 2, &dz));
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "y", 2, &dy));
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "x", 2, &dx));
    int v1 = -1, v2 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "var_one", NC_FLOAT, 1, &dz, &v1));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "var_two", NC_FLOAT, 1, &dz, &v2));
    int dims_3d[3] = {dz, dy, dx};
    int v3d = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(ncid, "data_multi", NC_FLOAT, 3, dims_3d, &v3d));
    ASSERT_EQ(NC_NOERR, nc_enddef(ncid));
    ASSERT_EQ(NC_NOERR, nc_close(ncid));

    const std::string sub_fn =
        std::string("NETCDF:\"") + fn_multi_1d + "\":data_multi";
    CPLErrorReset();
    std::unique_ptr<GDALDataset> poDS;
    {
      CPLErrorHandlerPusher oQuiet(CPLQuietErrorHandler);
      poDS.reset(static_cast<GDALDataset*>(GDALOpenEx(
          sub_fn.c_str(), GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
    }
    ASSERT_NE(nullptr, poDS);
    poDS.reset();
    VSIUnlink(fn_multi_1d.c_str());
  }

  // 6. 2D Geolocation bands with projected SRS (lines 5968-5986, 6004-6006)
  {
    const std::string fn_gx =
        (std::filesystem::path(::testing::TempDir()) / "gx_proj.nc").string();
    const std::string fn_gy =
        (std::filesystem::path(::testing::TempDir()) / "gy_proj.nc").string();
    auto poDS_X = std::unique_ptr<GDALDataset>(
        poDriver->Create(fn_gx.c_str(), 2, 2, 1, GDT_Float64, nullptr));
    auto poDS_Y = std::unique_ptr<GDALDataset>(
        poDriver->Create(fn_gy.c_str(), 2, 2, 1, GDT_Float64, nullptr));
    ASSERT_NE(nullptr, poDS_X);
    ASSERT_NE(nullptr, poDS_Y);
    double x_vals[4] = {10.0, 11.0, 10.0, 11.0};
    double y_vals[4] = {50.0, 50.0, 51.0, 51.0};
    EXPECT_EQ(CE_None, poDS_X->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 2, 2, x_vals, 2, 2, GDT_Float64, 0,
                           0, nullptr));
    EXPECT_EQ(CE_None, poDS_Y->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 2, 2, y_vals, 2, 2, GDT_Float64, 0,
                           0, nullptr));
    poDS_X.reset();
    poDS_Y.reset();

    auto poSrcDS = std::unique_ptr<GDALDataset>(
        poMemDriver->Create("", 2, 2, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, poSrcDS);
    OGRSpatialReference oSRS;
    oSRS.importFromEPSG(32631);
    poSrcDS->SetSpatialRef(&oSRS);
    double adfGT[6] = {500000, 10, 0, 4000000, 0, -10};
    poSrcDS->SetGeoTransform(adfGT);

    poSrcDS->SetMetadataItem("X_DATASET", fn_gx.c_str(), "GEOLOCATION");
    poSrcDS->SetMetadataItem("Y_DATASET", fn_gy.c_str(), "GEOLOCATION");
    poSrcDS->SetMetadataItem("X_BAND", "1", "GEOLOCATION");
    poSrcDS->SetMetadataItem("Y_BAND", "1", "GEOLOCATION");

    char** papszGeoOpts = nullptr;
    papszGeoOpts = CSLSetNameValue(papszGeoOpts, "FORMAT", "NC4");
    papszGeoOpts = CSLSetNameValue(papszGeoOpts, "WRITE_LONLAT", "YES");
    const std::string fn_geoloc_out =
        (std::filesystem::path(::testing::TempDir()) / "geoloc_proj_out.nc")
            .string();
    auto poDst = std::unique_ptr<GDALDataset>(
        poDriver->CreateCopy(fn_geoloc_out.c_str(), poSrcDS.get(), FALSE,
                             papszGeoOpts, nullptr, nullptr));
    CSLDestroy(papszGeoOpts);
    ASSERT_NE(nullptr, poDst);
    poDst.reset();
    VSIUnlink(fn_geoloc_out.c_str());
    VSIUnlink(fn_gx.c_str());
    VSIUnlink(fn_gy.c_str());
  }

  // Test GrowDim with MULTIPLE_LAYERS=SEPARATE_GROUPS
  {
    const std::string fn_groups =
        (std::filesystem::path(::testing::TempDir()) / "growdim_groups.nc")
            .string();
    char** papszOpts = nullptr;
    papszOpts = CSLSetNameValue(papszOpts, "FORMAT", "NC4");
    papszOpts =
        CSLSetNameValue(papszOpts, "MULTIPLE_LAYERS", "SEPARATE_GROUPS");
    auto poDS = std::unique_ptr<GDALDataset>(
        poDriver->Create(fn_groups.c_str(), 0, 0, 0, GDT_Unknown, papszOpts));
    CSLDestroy(papszOpts);
    ASSERT_NE(nullptr, poDS);
    auto* poLayer = poDS->CreateLayer("layer_grp", nullptr, wkbPoint, nullptr);
    ASSERT_NE(nullptr, poLayer);
    OGRFieldDefn oField("str_fld", OFTString);
    oField.SetWidth(5);
    EXPECT_EQ(OGRERR_NONE, poLayer->CreateField(&oField));
    OGRFeature oFeat(poLayer->GetLayerDefn());
    oFeat.SetField("str_fld", "A very long string exceeding initial width");
    OGRPoint pt(1.0, 2.0);
    oFeat.SetGeometry(&pt);
    EXPECT_EQ(OGRERR_NONE, poLayer->CreateFeature(&oFeat));
    poDS.reset();
    VSIUnlink(fn_groups.c_str());
  }

  // Int64 and UInt64 band GetNoDataValue with pbSuccess
  {
    const std::string fn_nodata64 =
        (std::filesystem::path(::testing::TempDir()) / "nodata64_success.nc")
            .string();
    char** papszOpts = nullptr;
    papszOpts = CSLSetNameValue(papszOpts, "FORMAT", "NC4");
    auto poDS = std::unique_ptr<GDALDataset>(
        poDriver->Create(fn_nodata64.c_str(), 2, 2, 2, GDT_Int64, papszOpts));
    CSLDestroy(papszOpts);
    ASSERT_NE(nullptr, poDS);
    auto* poBand1 = poDS->GetRasterBand(1);
    poBand1->SetNoDataValueAsInt64(-999999999999LL);
    int bSuccess = FALSE;
    double dfVal = poBand1->GetNoDataValue(&bSuccess);
    EXPECT_TRUE(bSuccess);
    EXPECT_EQ(dfVal, static_cast<double>(-999999999999LL));

    poDS.reset();

    char** papszOptsU = nullptr;
    papszOptsU = CSLSetNameValue(papszOptsU, "FORMAT", "NC4");
    poDS.reset(
        poDriver->Create(fn_nodata64.c_str(), 2, 2, 1, GDT_UInt64, papszOptsU));
    CSLDestroy(papszOptsU);
    ASSERT_NE(nullptr, poDS);
    auto* poBandU = poDS->GetRasterBand(1);
    poBandU->SetNoDataValueAsUInt64(999999999999ULL);
    bSuccess = FALSE;
    dfVal = poBandU->GetNoDataValue(&bSuccess);
    EXPECT_TRUE(bSuccess);
    EXPECT_EQ(dfVal, static_cast<double>(999999999999ULL));
    poDS.reset();
    VSIUnlink(fn_nodata64.c_str());
  }

  // standard_parallel with space-separated values without curly braces
  {
    const std::string fn_stdpar =
        (std::filesystem::path(::testing::TempDir()) / "stdpar_spaces.nc")
            .string();
    int cdfid = -1;
    ASSERT_EQ(NC_NOERR, nc_create(fn_stdpar.c_str(), NC_CLOBBER, &cdfid));
    int dimids[2];
    ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "y", 2, &dimids[0]));
    ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "x", 2, &dimids[1]));
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(cdfid, "data", NC_SHORT, 2, dimids, &varid));
    ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, varid, "grid_mapping", "proj"));
    int grid_varid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(cdfid, "proj", NC_CHAR, 0, nullptr, &grid_varid));
    const char* grid_mapping_name = "lambert_conformal_conic";
    ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, grid_varid, "grid_mapping_name",
                                   grid_mapping_name));
    const char* std_parallel = "30.0 60.0";
    ASSERT_EQ(NC_NOERR,
              PutTextAtt(cdfid, grid_varid, "standard_parallel", std_parallel));
    double lat_origin = 45.0;
    ASSERT_EQ(NC_NOERR, nc_put_att_double(cdfid, grid_varid,
                                          "latitude_of_projection_origin",
                                          NC_DOUBLE, 1, &lat_origin));
    double lon_origin = -100.0;
    ASSERT_EQ(NC_NOERR, nc_put_att_double(cdfid, grid_varid,
                                          "longitude_of_central_meridian",
                                          NC_DOUBLE, 1, &lon_origin));
    ASSERT_EQ(NC_NOERR, nc_enddef(cdfid));
    ASSERT_EQ(NC_NOERR, nc_close(cdfid));

    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(fn_stdpar.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(nullptr, poDS);
    const auto* poSRS = poDS->GetSpatialRef();
    EXPECT_NE(nullptr, poSRS);
    poDS.reset();
    VSIUnlink(fn_stdpar.c_str());
  }

  // Vector-only dataset opened in raster-only mode
  {
    const std::string fn_vec =
        (std::filesystem::path(::testing::TempDir()) / "vec_only.nc").string();
    int cdfid = -1;
    ASSERT_EQ(NC_NOERR, nc_create(fn_vec.c_str(), NC_CLOBBER, &cdfid));
    int dimid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "features", 5, &dimid));
    int varid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(cdfid, "feat_attr", NC_INT, 1, &dimid, &varid));
    ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, NC_GLOBAL, "Conventions", "CF-1.6"));
    ASSERT_EQ(NC_NOERR, PutTextAtt(cdfid, NC_GLOBAL, "featureType", "point"));
    ASSERT_EQ(NC_NOERR, nc_enddef(cdfid));
    ASSERT_EQ(NC_NOERR, nc_close(cdfid));

    // With no raster variable and raster-only open flags the driver refuses
    // the file, while a vector open exposes the point layer.
    {
      CPLErrorHandlerPusher oQuiet(CPLQuietErrorHandler);
      auto poDS = std::unique_ptr<GDALDataset>(
          GDALDataset::Open(fn_vec.c_str(), GDAL_OF_RASTER));
      EXPECT_EQ(nullptr, poDS);
    }
    auto poVecDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(fn_vec.c_str(), GDAL_OF_VECTOR));
    ASSERT_NE(nullptr, poVecDS);
    EXPECT_EQ(0, poVecDS->GetRasterCount());
    EXPECT_EQ(1, poVecDS->GetLayerCount());
    poVecDS.reset();
    VSIUnlink(fn_vec.c_str());
  }

  // 3D dataset reading block from band 2 (nd == 3, nLevel > 0)
  {
    const std::string fn_3d =
        (std::filesystem::path(::testing::TempDir()) / "test_3d_band2.nc")
            .string();
    int cdfid = -1;
    ASSERT_EQ(NC_NOERR, nc_create(fn_3d.c_str(), NC_CLOBBER, &cdfid));
    int dimids[3];
    ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "time", 2, &dimids[0]));
    ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "lat", 2, &dimids[1]));
    ASSERT_EQ(NC_NOERR, nc_def_dim(cdfid, "lon", 2, &dimids[2]));
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(cdfid, "var3d", NC_INT, 3, dimids, &varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(cdfid));
    int data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(cdfid, varid, data));
    ASSERT_EQ(NC_NOERR, nc_close(cdfid));

    auto poDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(fn_3d.c_str(), GDAL_OF_RASTER));
    ASSERT_NE(nullptr, poDS);
    EXPECT_GE(poDS->GetRasterCount(), 2);
    auto* poBand2 = poDS->GetRasterBand(2);
    ASSERT_NE(nullptr, poBand2);
    int buf[4] = {0};
    EXPECT_EQ(CE_None, poBand2->ReadBlock(0, 0, buf));
    EXPECT_EQ(buf[0], 7);
    poDS.reset();
    VSIUnlink(fn_3d.c_str());
  }
}

}  // namespace
}  // namespace autotest2
