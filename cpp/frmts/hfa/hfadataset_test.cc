// Copyright 2017 Google LLC
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
// Tests HFA raster driver (hfadataset.h / hfadataset.cpp).
//
// Note: Includes white-box tests using hfa_p.h internals (assuming
// little-endian host byte order where HFAStandard is a no-op), assumes GDAL
// PAM (.aux.xml) is compiled out, and resets process-global driver/OSR state.
//
// See also:
//   https://gdal.org/drivers/raster/hfa.html
//   https://github.com/OSGeo/gdal/blob/master/frmts/hfa/hfadataset.cpp
//   https://github.com/OSGeo/gdal/blob/master/frmts/hfa/hfadataset.h
//   https://github.com/OSGeo/gdal/blob/master/autotest/gcore/hfa_read.py
//   https://github.com/OSGeo/gdal/blob/master/autotest/gcore/hfa_rfc40.py
//   https://github.com/OSGeo/gdal/blob/master/autotest/gcore/hfa_srs.py
//   https://github.com/OSGeo/gdal/blob/master/autotest/gcore/hfa_write.py
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/hfa.py

#include "third_party/gdal/frmts/hfa/hfadataset.h"

#include <array>
#include <cstring>
#include <filesystem>  // NOLINT
#include <format>      // NOLINT
#include <iterator>
#include <memory>
#include <span>  // NOLINT
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/matchers.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/frmts/hfa/hfa.h"
#include "third_party/gdal/frmts/hfa/hfa_p.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_colortable.h"
#include "third_party/gdal/gcore/gdal_cpp_functions.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_geotransform.h"
#include "third_party/gdal/gcore/gdal_openinfo.h"
#include "third_party/gdal/gcore/gdal_rasterband.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;
using ::testing::UnorderedElementsAre;

constexpr std::string_view kTestDataPath =
    "google3/third_party/gdal/autotest2/cpp/frmts/hfa/testdata";
constexpr char kTestDir[] = "/vsimem/hfadataset_test";

std::filesystem::path GetTestFilePath(std::string_view filename) {
  return std::filesystem::path(::testing::SrcDir()) / kTestDataPath / filename;
}

TEST(IdentifyTest, DoesNotExist) {
  auto open_info = std::make_unique<GDALOpenInfo>("/does_not_exist",
                                                  GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(FALSE, HFADataset::Identify(open_info.get()));
}

TEST(IdentifyTest, MinimumSuccess) {
  constexpr char kFilename[] = "/vsimem/hfadataset_test/identify-minimum.img";
  constexpr char kData[] = "EHFA_HEADER_TAG";
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };
  VsiMemTempWrapper wrapper(kFilename, kData);
  auto open_info =
      std::make_unique<GDALOpenInfo>(kFilename, GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(TRUE, HFADataset::Identify(open_info.get()));
}

TEST(OpenTest, Byte) {
  const auto filepath = GetTestFilePath("byte.img");
  auto open_info = std::make_unique<GDALOpenInfo>(filepath.c_str(),
                                                  GDAL_OF_READONLY, nullptr);
  GDALDatasetUniquePtr src(HFADataset::Open(open_info.get()));
  ASSERT_NE(nullptr, src);
  GDALGeoTransform geo_transform;
  EXPECT_EQ(CE_None, src->GetGeoTransform(geo_transform));
  EXPECT_EQ(GDALGeoTransform(440720.0, 60.0, 0.0, 3751320.0, 0.0, -60.0),
            geo_transform);

  EXPECT_EQ(0, src->GetGCPCount());
  EXPECT_EQ(20, src->GetRasterXSize());
  EXPECT_EQ(20, src->GetRasterYSize());
  EXPECT_EQ(1, src->GetRasterCount());

  // No file level metadata.
  EXPECT_EQ(nullptr, src->GetMetadata());

  GDALRasterBand* band = src->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  int block_xsize = 0;
  int block_ysize = 0;
  band->GetBlockSize(&block_xsize, &block_ysize);
  EXPECT_EQ(20, block_xsize);
  EXPECT_EQ(20, block_ysize);
  EXPECT_EQ(GDT_Byte, band->GetRasterDataType());
  EXPECT_EQ(GCI_Undefined, band->GetColorInterpretation());

  std::array<double, 2> minmax = {};
  ASSERT_EQ(CE_None, band->ComputeRasterMinMax(false, minmax.data()));
  EXPECT_THAT(minmax, ElementsAre(74.0, 255.0));

  EXPECT_THAT(
      CslToVector(band->GetMetadata()),
      UnorderedElementsAre(
          "LAYER_TYPE=athematic", "STATISTICS_EXCLUDEDVALUES=0",
          "STATISTICS_HISTOBINVALUES="
          "0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|"
          "0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|"
          "0|0|0|0|0|0|0|0|0|0|0|0|1|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|6|0|0|"
          "0|0|0|0|0|0|37|0|0|0|0|0|0|0|57|0|0|0|0|0|0|0|62|0|0|0|0|0|0|0|"
          "66|0|0|0|0|0|0|0|0|72|0|0|0|0|0|0|0|31|0|0|0|0|0|0|0|24|0|0|0|"
          "0|0|0|0|12|0|0|0|0|0|0|0|0|7|0|0|0|0|0|0|0|12|0|0|0|0|0|0|0|5|"
          "0|0|0|0|0|0|0|3|0|0|0|0|0|0|0|1|0|0|0|0|0|0|0|0|2|0|0|0|0|0|0|"
          "0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|"
          "0|0|0|1|0|0|0|0|0|0|0|1|",
          "STATISTICS_HISTOMAX=255", "STATISTICS_HISTOMIN=0",
          "STATISTICS_HISTONUMBINS=256", "STATISTICS_MAXIMUM=255",
          "STATISTICS_MEAN=126.765", "STATISTICS_MEDIAN=123",
          "STATISTICS_MINIMUM=74", "STATISTICS_MODE=132",
          "STATISTICS_SKIPFACTORX=1", "STATISTICS_SKIPFACTORY=1",
          "STATISTICS_STDDEV=22.957185278028"));
}

TEST(OpenTest, Compresses1x1) {
  const auto filepath = GetTestFilePath("compressed-1x1.img");
  auto open_info = std::make_unique<GDALOpenInfo>(filepath.c_str(),
                                                  GDAL_OF_READONLY, nullptr);
  GDALDatasetUniquePtr src(HFADataset::Open(open_info.get()));
  ASSERT_NE(nullptr, src);
  GDALGeoTransform geo_transform;
  EXPECT_EQ(CE_Failure, src->GetGeoTransform(geo_transform));
  EXPECT_EQ(GDALGeoTransform(0.0, 1.0, 0.0, 0.0, 0.0, 1.0), geo_transform);

  EXPECT_EQ(0, src->GetGCPCount());
  EXPECT_EQ(1, src->GetRasterXSize());
  EXPECT_EQ(1, src->GetRasterYSize());
  EXPECT_EQ(1, src->GetRasterCount());

  // No file level metadata.
  EXPECT_EQ(nullptr, src->GetMetadata());

  GDALRasterBand* band = src->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  int block_xsize = 0;
  int block_ysize = 0;
  band->GetBlockSize(&block_xsize, &block_ysize);
  EXPECT_EQ(64, block_xsize);
  EXPECT_EQ(64, block_ysize);
  EXPECT_EQ(GDT_Byte, band->GetRasterDataType());
  EXPECT_EQ(GCI_Undefined, band->GetColorInterpretation());

  std::array<double, 2> minmax = {};
  ASSERT_EQ(CE_None, band->ComputeRasterMinMax(false, minmax.data()));
  EXPECT_THAT(minmax, ElementsAre(0.0, 0.0));

  EXPECT_THAT(CslToVector(band->GetMetadata()),
              UnorderedElementsAre("LAYER_TYPE=athematic"));
}

struct HfaCloser {
  void operator()(HFAHandle hfa) const {
    if (hfa != nullptr) {
      EXPECT_EQ(CE_None, HFAClose(hfa));
    }
  }
};

using HfaPtr = std::unique_ptr<hfainfo, HfaCloser>;

template <typename T>
void WriteHfaLe(GByte* dst, size_t offset, T val) {
  HFAStandard(static_cast<int>(sizeof(T)), &val);
  std::memcpy(dst + offset, &val, sizeof(T));
}

void WriteVsiMemBytes(const char* path, std::span<const GByte> bytes) {
  std::unique_ptr<GByte, decltype(&VSIFree)> owned(
      static_cast<GByte*>(VSIMalloc(bytes.size())), &VSIFree);
  ASSERT_NE(nullptr, owned);
  std::memcpy(owned.get(), bytes.data(), bytes.size());
  EXPECT_EQ(0, VSIFCloseL(VSIFileFromMemBuffer(path, owned.release(),
                                               bytes.size(), TRUE)));
}

void RegisterCustomTypes(HFAHandle hfa, const char* defn_text) {
  const char* psz = defn_text;
  while (psz != nullptr && *psz != '\0' && *psz != '.') {
    auto t = std::make_unique<HFAType>();
    const char* next = t->Initialize(psz);
    ASSERT_NE(nullptr, next);
    ASSERT_TRUE(t->CompleteDefn(hfa->poDictionary));
    hfa->poDictionary->AddType(t.release());
    psz = next;
  }
  if (!hfa->poDictionary->osDictionaryText.empty() &&
      hfa->poDictionary->osDictionaryText.back() == '.') {
    hfa->poDictionary->osDictionaryText.pop_back();
  }
  hfa->poDictionary->osDictionaryText += defn_text;
  if (!hfa->poDictionary->osDictionaryText.ends_with('.')) {
    hfa->poDictionary->osDictionaryText += '.';
  }
  hfa->poDictionary->bDictionaryTextDirty = true;
}

void PopulateBFUniqueEntry(HFAEntry* bin_func, std::span<const double> bins) {
  // Custom Edsc_BinFunction840 layout: 16B type + 64B MIFDictionary = 80B,
  // followed by 8B (size, offset=88) pointer header at 80..87 and BASEDATA at
  // offset 88 (24B header + N*8B doubles).
  constexpr size_t kMifHeaderOffset = 80;
  constexpr size_t kBasedataOffset = 88;
  const int payload_size = 24 + static_cast<int>(bins.size() * sizeof(double));
  GByte* data = bin_func->MakeData(kBasedataOffset + payload_size + 8);
  ASSERT_NE(nullptr, data);
  bin_func->SetStringField("binFunction.type.string", "BFUnique");
  bin_func->SetStringField("binFunction.MIFDictionary",
                           "{1:*bbinValues,}BFUnique,.");
  WriteHfaLe(data, kMifHeaderOffset, GInt32{payload_size});
  WriteHfaLe(data, kMifHeaderOffset + 4, GInt32{kBasedataOffset});
  // Upstream HFAReadBFUniqueBins (hfaband.cpp) checks BASEDATA header bytes
  // 20..21 for LE int16 EPT_f64.
  WriteHfaLe(data, kBasedataOffset + 20, GInt16{EPT_f64});
  for (size_t i = 0; i < bins.size(); ++i) {
    WriteHfaLe(data, kBasedataOffset + 24 + i * sizeof(double), bins[i]);
  }
}

void PopulatePolynomial(HFAEntry* node, std::string_view prefix, int order,
                        int termcount, int mtx_cols,
                        std::span<const double> mtx_diag,
                        std::span<const double> vec) {
  auto field_name = [prefix](std::string_view leaf) {
    return prefix.empty() ? std::string(leaf)
                          : std::format("{}.{}", prefix, leaf);
  };
  EXPECT_EQ(CE_None, node->SetIntField(field_name("order").c_str(), order));
  EXPECT_EQ(CE_None,
            node->SetIntField(field_name("numdimtransform").c_str(), 2));
  EXPECT_EQ(CE_None,
            node->SetIntField(field_name("numdimpolynomial").c_str(), 2));
  EXPECT_EQ(CE_None,
            node->SetIntField(field_name("termcount").c_str(), termcount));
  EXPECT_EQ(CE_None,
            node->SetIntField(field_name("polycoefmtx[-3]").c_str(), EPT_f64));
  EXPECT_EQ(CE_None,
            node->SetIntField(field_name("polycoefmtx[-2]").c_str(), 2));
  EXPECT_EQ(CE_None,
            node->SetIntField(field_name("polycoefmtx[-1]").c_str(), mtx_cols));
  EXPECT_EQ(CE_None, node->SetDoubleField(field_name("polycoefmtx[0]").c_str(),
                                          mtx_diag[0]));
  EXPECT_EQ(CE_None, node->SetDoubleField(field_name("polycoefmtx[3]").c_str(),
                                          mtx_diag[1]));
  EXPECT_EQ(CE_None, node->SetIntField(field_name("polycoefvector[-3]").c_str(),
                                       EPT_f64));
  EXPECT_EQ(CE_None,
            node->SetIntField(field_name("polycoefvector[-2]").c_str(), 1));
  EXPECT_EQ(CE_None,
            node->SetIntField(field_name("polycoefvector[-1]").c_str(), 2));
  for (size_t i = 0; i < vec.size(); ++i) {
    const std::string item = std::format("polycoefvector[{}]", i);
    EXPECT_EQ(CE_None, node->SetDoubleField(field_name(item).c_str(), vec[i]));
  }
}

int CPL_STDCALL AbortAtZeroProgress(double, const char*, void*) {
  return FALSE;
}

int CPL_STDCALL AbortAtOneProgress(double dfComplete, const char*, void*) {
  return dfComplete < 1.0 ? TRUE : FALSE;
}

class HfaDatasetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Deliberately registers HFA & MEM and tears down global driver/OSR state
    // per test; relies on single-threaded execution within a test process.
    GDALRegister_HFA();
    GDALRegister_HFA();  // Idempotent registration branch.
    GDALRegister_MEM();
  }

  void TearDown() override {
    GDALDestroyDriverManager();
    OSRCleanup();
    CPLErrorReset();
  }
};

GDALDatasetUniquePtr OpenHfaDataset(const char* path, GDALAccess access) {
  const unsigned int flags =
      (access == GA_Update ? GDAL_OF_UPDATE : GDAL_OF_READONLY) |
      GDAL_OF_RASTER;
  return GDALDatasetUniquePtr(GDALDataset::Open(path, flags));
}

TEST_F(HfaDatasetTest, CreateAllTypesSubByteCompressedAndSpillIO) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };
  constexpr char kPath[] = "/vsimem/hfadataset_test/create_types.img";

  // 1. Reject unsupported data types and mutually exclusive creation options.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              HFADataset::Create(kPath, 16, 16, 1, GDT_CInt16, nullptr));

    CPLStringList mut_excl({"FORCETOPESTRING=YES", "DISABLEPESTRING=YES"});
    EXPECT_EQ(nullptr,
              HFADataset::Create(kPath, 16, 16, 1, GDT_Byte, mut_excl.List()));
    CPLErrorReset();
  }

  // 2. Sub-byte NBITS=1, 2, 4 block write and read.
  for (int nbits : {1, 2, 4}) {
    const std::string nbits_opt = std::format("NBITS={}", nbits);
    CPLStringList opts({nbits_opt.c_str(), "BLOCKSIZE=32"});
    GDALDatasetUniquePtr ds(
        HFADataset::Create(kPath, 32, 32, 1, GDT_Byte, opts.List()));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    std::vector<GByte> write_buf(32 * 32, 1);
    EXPECT_EQ(CE_None, band->WriteBlock(0, 0, write_buf.data()));
    ds.reset();

    GDALDatasetUniquePtr reopened = OpenHfaDataset(kPath, GA_ReadOnly);
    ASSERT_NE(nullptr, reopened);
    std::vector<GByte> read_buf(32 * 32, 0);
    EXPECT_EQ(CE_None,
              reopened->GetRasterBand(1)->ReadBlock(0, 0, read_buf.data()));
    EXPECT_EQ(1, read_buf[0]);
  }

  // 3. PIXELTYPE=SIGNEDBYTE and all standard GDALDataTypes.
  {
    CPLStringList signed_opts({"PIXELTYPE=SIGNEDBYTE"});
    GDALDatasetUniquePtr sb_ds(
        HFADataset::Create(kPath, 16, 16, 1, GDT_Byte, signed_opts.List()));
    ASSERT_NE(nullptr, sb_ds);
    EXPECT_EQ(GDT_Int8, sb_ds->GetRasterBand(1)->GetRasterDataType());
  }

  constexpr GDALDataType kTypes[] = {
      GDT_Byte,  GDT_Int8,    GDT_UInt16,  GDT_Int16,    GDT_UInt32,
      GDT_Int32, GDT_Float32, GDT_Float64, GDT_CFloat32, GDT_CFloat64};
  for (GDALDataType dt : kTypes) {
    GDALDatasetUniquePtr ds(HFADataset::Create(kPath, 16, 16, 1, dt, nullptr));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(dt, ds->GetRasterBand(1)->GetRasterDataType());
  }

  // 4. Compressed dataset block write and multi-band spill file IRasterIO.
  {
    CPLStringList comp_opts({"COMPRESSED=YES", "BLOCKSIZE=32"});
    GDALDatasetUniquePtr comp_ds(
        HFADataset::Create(kPath, 32, 32, 1, GDT_Byte, comp_opts.List()));
    ASSERT_NE(nullptr, comp_ds);
    std::vector<GByte> comp_buf(32 * 32, 42);
    EXPECT_EQ(CE_None,
              comp_ds->GetRasterBand(1)->WriteBlock(0, 0, comp_buf.data()));
  }

  CPLStringList spill_opts({"USE_SPILL=YES", "BLOCKSIZE=32"});
  GDALDatasetUniquePtr spill_ds(
      HFADataset::Create(kPath, 32, 32, 2, GDT_Byte, spill_opts.List()));
  ASSERT_NE(nullptr, spill_ds);
  std::vector<GByte> io_buf(32 * 32 * 2, 7);
  constexpr std::array<int, 2> kBandMap = {1, 2};
  EXPECT_EQ(CE_None,
            spill_ds->RasterIO(GF_Write, 0, 0, 32, 32, io_buf.data(), 32, 32,
                               GDT_Byte, 2, const_cast<int*>(kBandMap.data()),
                               0, 0, 0, nullptr));
  EXPECT_EQ(CE_None,
            spill_ds->RasterIO(GF_Read, 0, 0, 32, 32, io_buf.data(), 32, 32,
                               GDT_Byte, 2, const_cast<int*>(kBandMap.data()),
                               0, 0, 0, nullptr));
  EXPECT_EQ(CE_None,
            spill_ds->RasterIO(GF_Read, 0, 0, 32, 32, io_buf.data(), 32, 32,
                               GDT_Byte, 1, const_cast<int*>(kBandMap.data()),
                               0, 0, 0, nullptr));
  EXPECT_EQ(CE_None,
            spill_ds->RasterIO(GF_Read, 0, 0, 32, 32, io_buf.data(), 32, 32,
                               GDT_Byte, 2, const_cast<int*>(kBandMap.data()),
                               2, 64, 1, nullptr));
}

TEST_F(HfaDatasetTest, OpenRejectsInvalidAndZeroSizedDatasets) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };
  constexpr char kPath[] = "/vsimem/hfadataset_test/open_invalid.img";

  {
    WithQuietHandler quiet;
    CPLErrorReset();

    constexpr GByte kNotHfaMagic[] = {'N', 'O', 'T', '_', 'H', 'F', 'A', '_',
                                      'F', 'I', 'L', 'E', '_', '1', '2', '3'};
    WriteVsiMemBytes(kPath, kNotHfaMagic);
    GDALOpenInfo non_hfa_oi(kPath, GA_ReadOnly);
    EXPECT_EQ(nullptr, HFADataset::Open(&non_hfa_oi));

    HfaPtr zero_band_hfa(HFACreate(kPath, 16, 16, 0, EPT_u8, nullptr));
    ASSERT_NE(nullptr, zero_band_hfa);
    ASSERT_EQ(CE_None, HFAClose(zero_band_hfa.release()));
    GDALOpenInfo zero_band_oi(kPath, GA_ReadOnly);
    EXPECT_EQ(nullptr, HFADataset::Open(&zero_band_oi));

    HfaPtr zero_px_hfa(HFACreate(kPath, 16, 16, 1, EPT_u8, nullptr));
    ASSERT_NE(nullptr, zero_px_hfa);
    zero_px_hfa->papoBand[0]->poNode->SetIntField("width", 0);
    ASSERT_EQ(CE_None, HFAClose(zero_px_hfa.release()));
    GDALOpenInfo zero_px_oi(kPath, GA_ReadOnly);
    EXPECT_EQ(nullptr, HFADataset::Open(&zero_px_oi));
    CPLErrorReset();
  }
}

TEST_F(HfaDatasetTest, OpenLoadsXFormStackElevationAndDependentFile) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };
  constexpr char kPath[] = "/vsimem/hfadataset_test/open_xform.img";

  CPLStringList dep_opt({"DEPENDENT_FILE=parent_dep.img"});
  HfaPtr hfa(HFACreate(kPath, 32, 32, 1, EPT_u8, dep_opt.List()));
  ASSERT_NE(nullptr, hfa);
  ASSERT_EQ(
      CE_None,
      HFASetPEString(
          hfa.get(),
          R"wkt(GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]])wkt"));

  HFAEntry* xform_hdr =
      HFAEntry::New(hfa.get(), "MapToPixelXForm", "Exfr_GenericXFormHeader",
                    hfa->papoBand[0]->poNode);
  ASSERT_NE(nullptr, xform_hdr);

  // Order 2, 1, and 3 polynomials to exercise XForm GCP extraction.
  HFAEntry* xf0 =
      HFAEntry::New(hfa.get(), "XForm0", "Efga_Polynomial", xform_hdr);
  ASSERT_NE(nullptr, xf0);
  ASSERT_NE(nullptr, xf0->MakeData(180));
  xf0->SetPosition();
  ASSERT_NO_FATAL_FAILURE(
      PopulatePolynomial(xf0, "", 2, 6, 5, std::to_array({1.0, 1.0}), {}));

  HFAEntry* xf1 =
      HFAEntry::New(hfa.get(), "XForm1", "Efga_Polynomial", xform_hdr);
  ASSERT_NE(nullptr, xf1);
  ASSERT_NE(nullptr, xf1->MakeData(120));
  xf1->SetPosition();
  ASSERT_NO_FATAL_FAILURE(PopulatePolynomial(xf1, "", 1, 3, 2,
                                             std::to_array({2.0, -2.0}),
                                             std::to_array({100.0, 200.0})));

  HFAEntry* xf2 =
      HFAEntry::New(hfa.get(), "XForm2", "Efga_Polynomial", xform_hdr);
  ASSERT_NE(nullptr, xf2);
  ASSERT_NE(nullptr, xf2->MakeData(260));
  xf2->SetPosition();
  ASSERT_NO_FATAL_FAILURE(
      PopulatePolynomial(xf2, "", 3, 10, 9, std::to_array({1.0, 1.0}), {}));

  ASSERT_NO_FATAL_FAILURE(
      RegisterCustomTypes(hfa.get(), "{16:celevationUnit,}TestElevInfo,."));
  HFAEntry* elev_node = HFAEntry::New(hfa.get(), "Elevation_Info",
                                      "TestElevInfo", hfa->papoBand[0]->poNode);
  ASSERT_NE(nullptr, elev_node);
  ASSERT_NE(nullptr, elev_node->MakeData());
  elev_node->SetStringField("elevationUnit", "feet");

  HFAEntry* map_info_old =
      HFAEntry::New(hfa.get(), "MapInformation", "Eimg_MapInformation",
                    hfa->papoBand[0]->poNode);
  ASSERT_NE(nullptr, map_info_old);
  ASSERT_NE(nullptr, map_info_old->MakeData(64));
  map_info_old->SetPosition();
  map_info_old->SetStringField("projection.string", "UTM Zone 11");
  map_info_old->SetStringField("units.string", "meters");
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr ds = OpenHfaDataset(kPath, GA_ReadOnly);
    ASSERT_NE(nullptr, ds);
    EXPECT_GT(ds->GetGCPCount(), 0);
    EXPECT_NE(nullptr, ds->GetGCPs());
    EXPECT_NE(nullptr, ds->GetGCPSpatialRef());
    EXPECT_STREQ("3", ds->GetMetadataItem("XFORM_STEPS", "XFORMS"));
    EXPECT_STREQ("feet", ds->GetRasterBand(1)->GetUnitType());
    EXPECT_STREQ("feet", ds->GetMetadataItem("ELEVATION_UNITS"));
    EXPECT_STREQ("parent_dep.img",
                 ds->GetMetadataItem("HFA_DEPENDENT_FILE", "HFA"));
    CPLErrorReset();
  }
}

TEST_F(HfaDatasetTest, OpenLoadsCameraModelPeStringWarningAndTowgs84) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };
  absl::Cleanup reset_tls_opts = [] {
    CPLSetThreadLocalConfigOption("HFA_SHOW_ESRI_PE_STRING_ONLY_WARNING",
                                  nullptr);
  };
  constexpr char kPath[] = "/vsimem/hfadataset_test/open_cam_towgs.img";

  // 1. Camera_ModelX metadata loading on Open.
  HfaPtr hfa(HFACreate(kPath, 16, 16, 1, EPT_u8, nullptr));
  ASSERT_NE(nullptr, hfa);
  ASSERT_NO_FATAL_FAILURE(RegisterCustomTypes(
      hfa.get(), "{16:cdirection,16:crefType,}Camera_ModelX,."));
  HFAEntry* cam_hdr =
      HFAEntry::New(hfa.get(), "MapToPixelXForm", "Exfr_GenericXFormHeader",
                    hfa->papoBand[0]->poNode);
  ASSERT_NE(nullptr, cam_hdr);
  HFAEntry* cam = HFAEntry::New(hfa.get(), "XForm0", "Camera_ModelX", cam_hdr);
  ASSERT_NE(nullptr, cam);
  ASSERT_NE(nullptr, cam->MakeData());
  cam->SetStringField("direction", "forward");
  cam->SetStringField("refType", "ground");
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  {
    GDALDatasetUniquePtr ds = OpenHfaDataset(kPath, GA_ReadOnly);
    ASSERT_NE(nullptr, ds);
    EXPECT_STREQ("forward", ds->GetMetadataItem("direction", "CAMERA_MODEL"));
  }

  // 2. PE_STRING-only Vertical Perspective warning and TOWGS84 copy.
  hfa.reset(HFACreate(kPath, 16, 16, 1, EPT_u8, nullptr));
  ASSERT_NE(nullptr, hfa);
  char pro_unknown[] = "Unknown";
  char units_meters[] = "meters";
  Eprj_MapInfo mi = {
      .proName = pro_unknown,
      .upperLeftCenter = {.x = 0.0, .y = 0.0},
      .lowerRightCenter = {.x = 15.0, .y = -15.0},
      .pixelSize = {.width = 1.0, .height = 1.0},
      .units = units_meters,
  };
  ASSERT_EQ(CE_None, HFASetMapInfo(hfa.get(), &mi));
  ASSERT_EQ(
      CE_None,
      HFASetPEString(
          hfa.get(),
          R"wkt(PROJCS["VP",GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]],PROJECTION["Vertical Perspective"],PARAMETER["Viewpoint height",35785831],PARAMETER["Latitude of topocentric origin",0],PARAMETER["Longitude of topocentric origin",0],PARAMETER["False easting",0],PARAMETER["False northing",0],UNIT["metre",1]])wkt"));
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    CPLSetThreadLocalConfigOption("HFA_SHOW_ESRI_PE_STRING_ONLY_WARNING",
                                  "YES");
    GDALDatasetUniquePtr ds = OpenHfaDataset(kPath, GA_ReadOnly);
    ASSERT_NE(nullptr, ds);
    EXPECT_NE(nullptr, ds->GetSpatialRef());
    CPLSetThreadLocalConfigOption("HFA_SHOW_ESRI_PE_STRING_ONLY_WARNING",
                                  nullptr);
    CPLErrorReset();
  }

  hfa.reset(HFAOpen(kPath, "r+b"));
  ASSERT_NE(nullptr, hfa);
  char datum_custom[] = "Custom_Towgs";
  char pro_tm[] = "Transverse Mercator";
  char sphere_wgs84[] = "WGS 84";
  char pro_tm_custom[] = "TM_Custom";

  Eprj_Datum d = {
      .datumname = datum_custom,
      .type = EPRJ_DATUM_PARAMETRIC,
      .params = {10.0, 20.0, 30.0},
  };
  Eprj_ProParameters p = {
      .proType = EPRJ_INTERNAL,
      .proNumber = EPRJ_TRANSVERSE_MERCATOR,
      .proName = pro_tm,
      .proParams = {0.0, 0.0, 0.9996},
      .proSpheroid = {.sphereName = sphere_wgs84,
                      .a = 6378137.0,
                      .b = 6356752.314245},
  };
  ASSERT_EQ(CE_None, HFASetProParameters(hfa.get(), &p));
  ASSERT_EQ(CE_None, HFASetDatum(hfa.get(), &d));
  mi.proName = pro_tm_custom;
  ASSERT_EQ(CE_None, HFASetMapInfo(hfa.get(), &mi));
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  {
    GDALDatasetUniquePtr ds = OpenHfaDataset(kPath, GA_ReadOnly);
    ASSERT_NE(nullptr, ds);
    const OGRSpatialReference* srs = ds->GetSpatialRef();
    ASSERT_NE(nullptr, srs);
    std::array<double, 7> towgs = {};
    EXPECT_EQ(OGRERR_NONE, srs->GetTOWGS84(towgs.data()));
    EXPECT_DOUBLE_EQ(10.0, towgs[0]);
  }
}

TEST_F(HfaDatasetTest, WriteGeoTransformDatumsAndPeStringOptions) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };

  constexpr char kPath[] = "/vsimem/hfadataset_test/write_gt_datum.img";
  constexpr char kForcePePath[] = "/vsimem/hfadataset_test/force_pe.img";
  constexpr char kDisablePePath[] = "/vsimem/hfadataset_test/disable_pe.img";
  constexpr char kIgnoreUtmPath[] = "/vsimem/hfadataset_test/ignore_utm.img";

  GDALDatasetUniquePtr ds(
      HFADataset::Create(kPath, 16, 16, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);

  // 1. Rotated and north-up GeoTransform.
  GDALGeoTransform rot_gt(100.0, 2.0, 0.5, 200.0, -0.5, -2.0);
  EXPECT_EQ(CE_None, ds->SetGeoTransform(rot_gt));
  GDALGeoTransform out_gt;
  EXPECT_EQ(CE_None, ds->GetGeoTransform(out_gt));
  EXPECT_DOUBLE_EQ(100.0, out_gt[0]);
  EXPECT_EQ(CE_None, ds->FlushCache(false));

  GDALGeoTransform std_gt(440720.0, 60.0, 0.0, 3751320.0, 0.0, -60.0);
  EXPECT_EQ(CE_None, ds->SetGeoTransform(std_gt));

  // 2. Datums: EPSG 4326, 4322, 4267, 4269, 4283, 4284, 4272, and custom
  //    datum with TOWGS84 + US survey feet + unknown unit.
  constexpr int kDatumEpsgs[] = {4326, 4322, 4267, 4269, 4283, 4284, 4272};
  for (int epsg : kDatumEpsgs) {
    OGRSpatialReference srs;
    ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(epsg));
    EXPECT_EQ(CE_None, ds->SetSpatialRef(&srs));
    EXPECT_EQ(CE_None, ds->FlushCache(false));
  }

  OGRSpatialReference srs;
  srs.SetProjCS("CustomTM");
  srs.SetGeogCS("CustomGeog", "CustomDatum", "WGS 84", 6378137.0,
                298.257223563);
  srs.SetTOWGS84(1.0, 2.0, 3.0, 0.1, 0.2, 0.3, 1.5);
  srs.SetTM(0.0, -117.0, 0.9996, 500000.0, 0.0);
  srs.SetLinearUnits("US survey foot", 0.3048006096012192);
  EXPECT_EQ(CE_None, ds->SetSpatialRef(&srs));
  EXPECT_EQ(CE_None, ds->FlushCache(false));

  srs.SetLinearUnits("weird_unit", 0.12345);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_None, ds->SetSpatialRef(&srs));
    EXPECT_EQ(CE_None, ds->FlushCache(false));
    CPLErrorReset();
  }
  ds.reset();

  // 3. FORCETOPESTRING=YES, DISABLEPESTRING=YES (plus ClearSR), and
  // IGNOREUTM=YES.
  CPLStringList force_opts({"FORCETOPESTRING=YES"});
  GDALDatasetUniquePtr force_ds(
      HFADataset::Create(kForcePePath, 16, 16, 1, GDT_Byte, force_opts.List()));
  ASSERT_NE(nullptr, force_ds);
  OGRSpatialReference utm_srs;
  ASSERT_EQ(OGRERR_NONE, utm_srs.importFromEPSG(32611));
  EXPECT_EQ(CE_None, force_ds->SetSpatialRef(&utm_srs));
  EXPECT_EQ(CE_None, force_ds->FlushCache(false));

  OGRSpatialReference matching_tm_srs;
  ASSERT_EQ(
      OGRERR_NONE,
      matching_tm_srs.importFromWkt(
          R"wkt(PROJCS["Test",GEOGCS["MyDatum",DATUM["MyDatum",SPHEROID["WGS 84",6378137,298.257223563]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]],PROJECTION["Transverse_Mercator"],PARAMETER["latitude_of_origin",0],PARAMETER["central_meridian",0],PARAMETER["scale_factor",0.9996],PARAMETER["false_easting",0],PARAMETER["false_northing",0],UNIT["metre",1]])wkt"));
  EXPECT_EQ(CE_None, force_ds->SetSpatialRef(&matching_tm_srs));
  EXPECT_EQ(CE_None, force_ds->FlushCache(false));
  force_ds.reset();

  CPLStringList disable_opts({"DISABLEPESTRING=YES"});
  GDALDatasetUniquePtr disable_ds(HFADataset::Create(
      kDisablePePath, 16, 16, 1, GDT_Byte, disable_opts.List()));
  ASSERT_NE(nullptr, disable_ds);
  EXPECT_EQ(CE_None, disable_ds->SetSpatialRef(&utm_srs));
  EXPECT_EQ(CE_None, disable_ds->FlushCache(false));
  OGRSpatialReference local_srs;
  ASSERT_EQ(OGRERR_NONE,
            local_srs.importFromWkt(
                R"wkt(LOCAL_CS["CustomLocal",UNIT["metre",1]])wkt"));
  EXPECT_EQ(CE_None, disable_ds->SetSpatialRef(&local_srs));
  EXPECT_EQ(CE_None, disable_ds->FlushCache(false));
  EXPECT_EQ(CE_None, disable_ds->SetSpatialRef(&utm_srs));
  EXPECT_EQ(CE_None, disable_ds->FlushCache(false));
  EXPECT_EQ(CE_None, disable_ds->SetSpatialRef(nullptr));
  EXPECT_EQ(CE_None, disable_ds->FlushCache(false));
  disable_ds.reset();

  CPLStringList ignore_utm_opts({"IGNOREUTM=YES"});
  GDALDatasetUniquePtr ignore_ds(HFADataset::Create(
      kIgnoreUtmPath, 16, 16, 1, GDT_Byte, ignore_utm_opts.List()));
  ASSERT_NE(nullptr, ignore_ds);
  EXPECT_EQ(CE_None, ignore_ds->SetSpatialRef(&utm_srs));
  EXPECT_EQ(CE_None, ignore_ds->FlushCache(false));
}

TEST_F(HfaDatasetTest, WriteProjectionRoundTripsAllProjections) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };
  constexpr char kPath[] = "/vsimem/hfadataset_test/write_all_projs.img";

  // 1. Build suite of OGRSpatialReference test cases from OSR setters, WKT
  //    projection bodies, and standalone WKT strings.
  struct ProjTestEntry {
    OGRSpatialReference srs;
    bool verify_exact_roundtrip = true;
  };
  std::vector<ProjTestEntry> proj_list;
  auto add_fn = [&](auto fn, bool verify_exact = true) {
    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    fn(srs);
    proj_list.push_back(
        {.srs = std::move(srs), .verify_exact_roundtrip = verify_exact});
  };

  add_fn([](OGRSpatialReference& s) { s.SetUTM(11, TRUE); });
  add_fn([](OGRSpatialReference& s) { s.SetUTM(11, FALSE); });
  add_fn([](OGRSpatialReference& s) {
    s.SetACEA(20.0, 60.0, 40.0, -96.0, 0.0, 0.0);
  });
  add_fn([](OGRSpatialReference& s) {
    s.SetLCC1SP(35.0, -90.0, 0.9996, 0.0, 0.0);
  });
  add_fn([](OGRSpatialReference& s) {
    s.SetLCC(33.0, 45.0, 39.0, -96.0, 0.0, 0.0);
  });
  add_fn(
      [](OGRSpatialReference& s) { s.SetMercator(0.0, -90.0, 1.0, 0.0, 0.0); });
  add_fn([](OGRSpatialReference& s) {
    s.SetMercator(0.0, -90.0, 0.9996, 0.0, 0.0);
  });
  add_fn([](OGRSpatialReference& s) {
    s.SetMercator2SP(30.0, 0.0, -90.0, 0.0, 0.0);
  });
  add_fn([](OGRSpatialReference& s) {
    s.SetKrovak(49.5, 24.8333, 30.2881, 78.5, 0.9999, 0.0, 0.0);
  });
  add_fn([](OGRSpatialReference& s) { s.SetPS(70.0, -45.0, 1.0, 0.0, 0.0); });
  add_fn([](OGRSpatialReference& s) { s.SetPolyconic(0.0, -90.0, 0.0, 0.0); });
  add_fn([](OGRSpatialReference& s) {
    s.SetEC(20.0, 60.0, 40.0, -96.0, 0.0, 0.0);
  });
  add_fn([](OGRSpatialReference& s) {
    s.SetTM(0.0, -117.0, 0.9996, 500000.0, 0.0);
  });
  add_fn([](OGRSpatialReference& s) {
    s.SetStereographic(45.0, -90.0, 0.9996, 0.0, 0.0);
  });
  add_fn([](OGRSpatialReference& s) { s.SetLAEA(45.0, -100.0, 0.0, 0.0); });
  add_fn([](OGRSpatialReference& s) { s.SetAE(45.0, -100.0, 0.0, 0.0); });
  add_fn([](OGRSpatialReference& s) { s.SetGnomonic(45.0, -100.0, 0.0, 0.0); });
  add_fn([](OGRSpatialReference& s) {
    s.SetOrthographic(45.0, -100.0, 0.0, 0.0);
  });
  add_fn([](OGRSpatialReference& s) { s.SetSinusoidal(-90.0, 0.0, 0.0); });
  add_fn([](OGRSpatialReference& s) {
    s.SetEquirectangular2(0.0, -90.0, 30.0, 0.0, 0.0);
  });
  add_fn([](OGRSpatialReference& s) { s.SetMC(0.0, -90.0, 0.0, 0.0); });
  add_fn([](OGRSpatialReference& s) { s.SetVDG(-90.0, 0.0, 0.0); });
  add_fn([](OGRSpatialReference& s) {
    s.SetHOM(45.0, -90.0, 30.0, 0.0, 0.9996, 0.0, 0.0);
  });
  add_fn([](OGRSpatialReference& s) {
    s.SetHOM(45.0, -90.0, 30.0, 15.0, 0.9996, 0.0, 0.0);
  });
  add_fn([](OGRSpatialReference& s) {
    s.SetHOMAC(45.0, -90.0, 30.0, 30.0, 0.9996, 0.0, 0.0);
  });
  add_fn([](OGRSpatialReference& s) { s.SetRobinson(-90.0, 0.0, 0.0); });
  add_fn([](OGRSpatialReference& s) { s.SetMollweide(-90.0, 0.0, 0.0); });
  for (int i = 1; i <= 6; ++i) {
    add_fn([i](OGRSpatialReference& s) { s.SetEckert(i, -90.0, 0.0, 0.0); });
  }
  add_fn([](OGRSpatialReference& s) { s.SetGS(-90.0, 0.0, 0.0); });
  add_fn([](OGRSpatialReference& s) { s.SetCS(0.0, -90.0, 0.0, 0.0); });
  add_fn([](OGRSpatialReference& s) {
    s.SetTPED(10.0, -80.0, 50.0, -100.0, 0.0, 0.0);
  });
  add_fn([](OGRSpatialReference& s) { s.SetBonne(40.0, -90.0, 0.0, 0.0); });
  add_fn([](OGRSpatialReference& s) { s.SetCEA(30.0, -90.0, 0.0, 0.0); });
  add_fn([](OGRSpatialReference& s) {
    s.SetHOM2PNO(45.0, 10.0, -80.0, 50.0, -100.0, 0.9996, 0.0, 0.0);
  });
  add_fn(
      [](OGRSpatialReference& s) {
        s.SetNZMG(-41.0, 173.0, 2510000.0, 6023150.0);
      },
      /*verify_exact=*/false);
  add_fn([](OGRSpatialReference& s) { s.SetTMSO(0.0, 19.0, 1.0, 0.0, 0.0); });
  add_fn([](OGRSpatialReference& s) {
    s.SetVerticalPerspective(0.0, 0.0, 0.0, 35785831.0, 0.0, 0.0);
  });

  constexpr std::string_view kWgs84WktPrefix =
      R"wkt(PROJCS["Test",GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]],)wkt";
  constexpr std::string_view kWgs84WktSuffix = R"wkt(,UNIT["metre",1]])wkt";

  constexpr std::string_view kWgs84ProjBodies[] = {
      R"wkt(PROJECTION["Loximuthal"],PARAMETER["central_meridian",0],PARAMETER["latitude_of_origin",40],PARAMETER["false_easting",0],PARAMETER["false_northing",0])wkt",
      R"wkt(PROJECTION["Quartic_Authalic"],PARAMETER["central_meridian",0],PARAMETER["false_easting",0],PARAMETER["false_northing",0])wkt",
      R"wkt(PROJECTION["Winkel_I"],PARAMETER["central_meridian",0],PARAMETER["standard_parallel_1",50],PARAMETER["false_easting",0],PARAMETER["false_northing",0])wkt",
      R"wkt(PROJECTION["Winkel_II"],PARAMETER["central_meridian",0],PARAMETER["standard_parallel_1",50],PARAMETER["false_easting",0],PARAMETER["false_northing",0])wkt",
      R"wkt(PROJECTION["Behrmann"],PARAMETER["central_meridian",0],PARAMETER["false_easting",0],PARAMETER["false_northing",0])wkt",
      R"wkt(PROJECTION["Double_Stereographic"],PARAMETER["latitude_of_origin",46.5],PARAMETER["central_meridian",-66.5],PARAMETER["scale_factor",0.999912],PARAMETER["false_easting",0],PARAMETER["false_northing",0])wkt",
      R"wkt(PROJECTION["Aitoff"],PARAMETER["central_meridian",0],PARAMETER["false_easting",0],PARAMETER["false_northing",0])wkt",
      R"wkt(PROJECTION["Craster_Parabolic"],PARAMETER["central_meridian",0],PARAMETER["false_easting",0],PARAMETER["false_northing",0])wkt",
      R"wkt(PROJECTION["Flat_Polar_Quartic"],PARAMETER["central_meridian",0],PARAMETER["false_easting",0],PARAMETER["false_northing",0])wkt",
      R"wkt(PROJECTION["Times"],PARAMETER["central_meridian",0],PARAMETER["false_easting",0],PARAMETER["false_northing",0])wkt",
      R"wkt(PROJECTION["Winkel_Tripel"],PARAMETER["central_meridian",0],PARAMETER["standard_parallel_1",50],PARAMETER["false_easting",0],PARAMETER["false_northing",0])wkt",
      R"wkt(PROJECTION["Hammer_Aitoff"],PARAMETER["central_meridian",0],PARAMETER["false_easting",0],PARAMETER["false_northing",0])wkt",
      R"wkt(PROJECTION["Vertical_Near_Side_Perspective"],PARAMETER["Height",35785831],PARAMETER["longitude_of_center",0],PARAMETER["latitude_of_center",0],PARAMETER["false_easting",0],PARAMETER["false_northing",0])wkt",
      R"wkt(PROJECTION["Hotine_Oblique_Mercator_Two_Point_Center"],PARAMETER["latitude_of_center",45],PARAMETER["longitude_of_point_1",-80],PARAMETER["latitude_of_point_1",10],PARAMETER["longitude_of_point_2",-100],PARAMETER["latitude_of_point_2",50],PARAMETER["scale_factor",0.9996],PARAMETER["false_easting",0],PARAMETER["false_northing",0])wkt",
      R"wkt(PROJECTION["Goode_Homolosine"],PARAMETER["central_meridian",0],PARAMETER["false_easting",0],PARAMETER["false_northing",0])wkt",
  };

  for (size_t i = 0; i < std::size(kWgs84ProjBodies); ++i) {
    const std::string wkt = std::format("{}{}{}", kWgs84WktPrefix,
                                        kWgs84ProjBodies[i], kWgs84WktSuffix);
    OGRSpatialReference srs;
    ASSERT_EQ(OGRERR_NONE, srs.importFromWkt(wkt.c_str())) << wkt;
    // Indices 12..14 use PE_STRING/alias encodings that differ structurally
    // from ESRI PE_STRING when parsed back without authority metadata.
    const bool exact = (i < 12);
    proj_list.push_back(
        {.srs = std::move(srs), .verify_exact_roundtrip = exact});
  }

  constexpr const char* kStandaloneWkts[] = {
      R"wkt(PROJCS["S-JTSK / Krovak",GEOGCS["S-JTSK",DATUM["System_Jednotne_Trigonometricke_Site_Katastralni",SPHEROID["Bessel 1841",6377397.155,299.1528128]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]],PROJECTION["Krovak"],PARAMETER["latitude_of_center",49.5],PARAMETER["longitude_of_center",24.83333333333333],PARAMETER["azimuth",30.28813972222222],PARAMETER["pseudo_standard_parallel_1",78.5],PARAMETER["scale_factor",0.9999],PARAMETER["false_easting",0],PARAMETER["false_northing",0],UNIT["metre",1]])wkt",
      R"wkt(PROJCS["Test",GEOGCS["MyDatum",DATUM["MyDatum",SPHEROID["WGS 84",6378137,298.257223563]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]],PROJECTION["Goode_Homolosine"],PARAMETER["central_meridian",0],PARAMETER["false_easting",0],PARAMETER["false_northing",0],UNIT["metre",1]])wkt",
      R"wkt(GEOGCS["MyDatum",DATUM["MyDatum",SPHEROID["Clarke 1880 (IGN)",6378249.2,293.4660212936269]],PRIMEM["Paris",2.33722917],UNIT["grad",0.01570796326794897]])wkt",
      R"wkt(GEOGCS["MyDatum",DATUM["MyDatum",SPHEROID["Clarke 1880 (IGN)",6378249.2,293.4660212936269]],PRIMEM["Greenwich",0],UNIT["grad",0.01570796326794897]])wkt",
      R"wkt(PROJCS["Test",GEOGCS["MyDatum",DATUM["MyDatum",SPHEROID["WGS 84",6378137,298.257223563]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]],PROJECTION["Transverse_Mercator"],PARAMETER["latitude_of_origin",0],PARAMETER["central_meridian",0],PARAMETER["scale_factor",0.9996],PARAMETER["false_easting",0],PARAMETER["false_northing",0],UNIT["kilometre",1000]])wkt",
      R"wkt(GEOGCS["WGS 84",DATUM["Custom_WGS84",SPHEROID["WGS 84",6378137,298.257223563]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433],AUTHORITY["EPSG","4326"]])wkt",
      R"wkt(GEOGCS["WGS 72",DATUM["Custom_WGS72",SPHEROID["WGS 72",6378135,298.26]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433],AUTHORITY["EPSG","4322"]])wkt",
      R"wkt(GEOGCS["NAD83",DATUM["Custom_NAD83",SPHEROID["GRS 1980",6378137,298.257222101]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433],AUTHORITY["EPSG","4269"]])wkt",
  };

  for (const char* wkt : kStandaloneWkts) {
    OGRSpatialReference srs;
    ASSERT_EQ(OGRERR_NONE, srs.importFromWkt(wkt)) << wkt;
    proj_list.push_back(
        {.srs = std::move(srs), .verify_exact_roundtrip = false});
  }

  // 2. Write each spatial reference to HFA and verify round-trip where exact.
  for (size_t i = 0; i < proj_list.size(); ++i) {
    GDALDatasetUniquePtr ds(
        HFADataset::Create(kPath, 16, 16, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_None, ds->SetSpatialRef(&proj_list[i].srs)) << "index=" << i;
      EXPECT_EQ(CE_None, ds->FlushCache(false)) << "index=" << i;
      CPLErrorReset();
    }
    // HFADataset writes HFA metadata nodes on dataset close (HFAClose).
    ds.reset();
    if (proj_list[i].verify_exact_roundtrip) {
      GDALDatasetUniquePtr reopened = OpenHfaDataset(kPath, GA_ReadOnly);
      ASSERT_NE(nullptr, reopened);
      const OGRSpatialReference* got = reopened->GetSpatialRef();
      ASSERT_NE(nullptr, got) << "index=" << i;
      EXPECT_THAT(*got, IsSameAs(proj_list[i].srs)) << "index=" << i;
    }
  }
}

TEST_F(HfaDatasetTest, BandDescriptionStatsHistogramColorTableAndFileOps) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };
  absl::Cleanup reset_tls_opts = [] {
    CPLSetThreadLocalConfigOption("HFA_USE_RRD", nullptr);
  };

  constexpr char kPath[] = "/vsimem/hfadataset_test/band_ops.img";
  constexpr char kRenamedPath[] = "/vsimem/hfadataset_test/renamed.img";
  constexpr char kCopiedPath[] = "/vsimem/hfadataset_test/copied.img";

  CPLStringList opts({"BLOCKSIZE=32"});
  GDALDatasetUniquePtr ds(
      HFADataset::Create(kPath, 64, 64, 1, GDT_Byte, opts.List()));
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  // 1. PAM is compiled out, so SetDescription/SetMetadataItem persist
  //    directly in the HFA band nodes rather than a sidecar .aux.xml.
  band->SetDescription("");
  band->SetDescription("CustomBandDesc");
  EXPECT_STREQ("CustomBandDesc", band->GetDescription());

  int bSuccess = FALSE;
  band->GetNoDataValue(&bSuccess);
  EXPECT_EQ(FALSE, bSuccess);
  EXPECT_EQ(CE_None, band->SetNoDataValue(255.0));
  EXPECT_DOUBLE_EQ(255.0, band->GetNoDataValue(&bSuccess));
  EXPECT_EQ(TRUE, bSuccess);

  band->GetMinimum(&bSuccess);
  EXPECT_EQ(FALSE, bSuccess);
  band->GetMaximum(&bSuccess);
  EXPECT_EQ(FALSE, bSuccess);

  // 2. Statistics, default histogram, and RAT persistence.
  double hmin = 0.0;
  double hmax = 0.0;
  int nbuckets = 0;
  GUIntBig* hist_raw = nullptr;
  EXPECT_EQ(CE_None,
            band->GetDefaultHistogram(&hmin, &hmax, &nbuckets, &hist_raw, TRUE,
                                      GDALDummyProgress, nullptr));
  std::unique_ptr<GUIntBig, CplFreeDeleter> hist(hist_raw);
  hist.reset();

  EXPECT_EQ(CE_None, ds->SetMetadataItem("DS_META", "DS_VAL"));
  CPLStringList ds_md({"DS_META2=VAL2"});
  EXPECT_EQ(CE_None, ds->SetMetadata(ds_md.List()));
  EXPECT_EQ(CE_None, band->SetMetadataItem("STATISTICS_MINIMUM", "5"));
  EXPECT_EQ(CE_None, band->SetMetadataItem("STATISTICS_MAXIMUM", "200"));
  EXPECT_EQ(CE_None, band->SetMetadataItem("STATISTICS_HISTOMIN", "0"));
  EXPECT_EQ(CE_None, band->SetMetadataItem("STATISTICS_HISTOMAX", "3"));
  EXPECT_EQ(CE_None,
            band->SetMetadataItem("STATISTICS_HISTOBINVALUES", "1|2|3|4|"));
  EXPECT_EQ(CE_None, band->SetMetadataItem("BAND_CUSTOM", "BVAL"));
  EXPECT_DOUBLE_EQ(5.0, band->GetMinimum(&bSuccess));
  EXPECT_EQ(TRUE, bSuccess);
  EXPECT_DOUBLE_EQ(200.0, band->GetMaximum(&bSuccess));
  EXPECT_EQ(TRUE, bSuccess);
  EXPECT_EQ(CE_None,
            band->GetDefaultHistogram(&hmin, &hmax, &nbuckets, &hist_raw, FALSE,
                                      GDALDummyProgress, nullptr));
  hist.reset(hist_raw);
  EXPECT_EQ(4, nbuckets);
  hist.reset();

  GDALDefaultRasterAttributeTable small_rat;
  small_rat.CreateColumn("Val", GFT_Integer, GFU_Generic);
  small_rat.SetRowCount(2);
  small_rat.SetValue(0, 0, 10);
  small_rat.SetValue(1, 0, 20);
  EXPECT_EQ(CE_None, band->SetDefaultRAT(&small_rat));

  // 3. Color table persistence, clearing, and re-assignment.
  GDALColorTable ct;
  constexpr GDALColorEntry kC0 = {255, 0, 0, 255};
  constexpr GDALColorEntry kC1 = {0, 255, 0, 255};
  constexpr GDALColorEntry kCTail = {128, 128, 128, 255};
  ct.SetColorEntry(0, &kC0);
  ct.SetColorEntry(1, &kC1);
  ct.SetColorEntry(2, &kCTail);
  ct.SetColorEntry(3, &kCTail);
  EXPECT_EQ(CE_None, band->SetColorTable(&ct));
  EXPECT_EQ(GCI_PaletteIndex, band->GetColorInterpretation());
  ASSERT_NE(nullptr, band->GetColorTable());
  EXPECT_EQ(4, band->GetColorTable()->GetColorEntryCount());

  EXPECT_EQ(CE_None, band->SetColorTable(nullptr));
  EXPECT_EQ(GCI_Undefined, band->GetColorInterpretation());
  GDALColorTable ct2;
  ct2.SetColorEntry(0, &kC0);
  ct2.SetColorEntry(1, &kC1);
  EXPECT_EQ(CE_None, band->SetColorTable(&ct2));

  // 4. Overview building, @REGENERATE=NO, clearing, HFA_USE_RRD, and
  //    read-only/file operations.
  EXPECT_EQ(0, band->GetOverviewCount());
  EXPECT_EQ(nullptr, band->GetOverview(0));
  constexpr std::array<int, 1> kOvList = {2};
  constexpr std::array<int, 1> kBadBand = {99};
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, ds->BuildOverviews("NEAREST", 1, kOvList.data(), 1,
                                             kBadBand.data(), GDALDummyProgress,
                                             nullptr, nullptr));
    CPLErrorReset();
  }
  EXPECT_EQ(CE_None,
            ds->BuildOverviews("NEAREST", 1, kOvList.data(), 0, nullptr,
                               GDALDummyProgress, nullptr, nullptr));
  EXPECT_EQ(1, band->GetOverviewCount());
  EXPECT_EQ(nullptr, band->GetOverview(-1));
  EXPECT_EQ(nullptr, band->GetOverview(5));
  GDALRasterBand* ov_band = band->GetOverview(0);
  ASSERT_NE(nullptr, ov_band);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              ov_band->BuildOverviews("NEAREST", 1, kOvList.data(),
                                      GDALDummyProgress, nullptr, nullptr));
    CPLErrorReset();
  }
  CPLStringList no_regen({"@REGENERATE=NO"});
  EXPECT_EQ(CE_None,
            ds->BuildOverviews("NEAREST", 1, kOvList.data(), 0, nullptr,
                               GDALDummyProgress, nullptr, no_regen.List()));
  EXPECT_EQ(CE_None, ds->BuildOverviews("NEAREST", 0, nullptr, 0, nullptr,
                                        GDALDummyProgress, nullptr, nullptr));
  EXPECT_EQ(0, band->GetOverviewCount());

  CPLSetThreadLocalConfigOption("HFA_USE_RRD", "YES");
  EXPECT_EQ(CE_None,
            ds->BuildOverviews("NEAREST", 1, kOvList.data(), 0, nullptr,
                               GDALDummyProgress, nullptr, nullptr));
  CPLSetThreadLocalConfigOption("HFA_USE_RRD", nullptr);
  StringListPtr file_list(ds->GetFileList(), CSLDestroy);
  // With PAM compiled out, >= 2 entries reflects .img + .rrd.
  EXPECT_GE(CSLCount(file_list.get()), 2);
  file_list.reset();
  ds.reset();

  GDALDatasetUniquePtr ro_ds = OpenHfaDataset(kPath, GA_ReadOnly);
  ASSERT_NE(nullptr, ro_ds);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, ro_ds->GetRasterBand(1)->SetColorTable(&ct2));
    EXPECT_EQ(CE_Failure,
              ro_ds->BuildOverviews("NEAREST", 1, kOvList.data(), 0, nullptr,
                                    GDALDummyProgress, nullptr, nullptr));

    EXPECT_EQ(CE_Failure,
              HFADataset::Rename("/vsimem/hfadataset_test/no_such_new.img",
                                 "/vsimem/hfadataset_test/no_such_old.img"));
    CPLErrorReset();
  }

  EXPECT_EQ(CE_None, HFADataset::Rename(kRenamedPath, kPath));
  EXPECT_EQ(CE_None, HFADataset::CopyFiles(kCopiedPath, kRenamedPath));

  {
    GDALDatasetUniquePtr upd_ds = OpenHfaDataset(kRenamedPath, GA_Update);
    ASSERT_NE(nullptr, upd_ds);
    EXPECT_EQ(CE_None,
              upd_ds->BuildOverviews("NEAREST", 0, nullptr, 0, nullptr,
                                     GDALDummyProgress, nullptr, nullptr));
  }
}

TEST_F(HfaDatasetTest, ReadHistogramMetadataIntegerAndBFUniqueEdgeCases) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };
  constexpr char kPath[] = "/vsimem/hfadataset_test/hist_edge.img";
  constexpr std::array<int, 1> kOvList = {2};

  // 1. Valid integer histogram + BFUnique bins for histogram and PCT.
  HfaPtr hfa(HFACreate(kPath, 16, 16, 1, EPT_u8, nullptr));
  ASSERT_NE(nullptr, hfa);
  ASSERT_NO_FATAL_FAILURE(
      RegisterCustomTypes(hfa.get(),
                          "{16:cstring,}BFStr,{1:oBFStr,type,64:cMIFDictionary,"
                          "0:pCMIFObject,}BFMIF,"
                          "{1:oBFMIF,binFunction,}Edsc_BinFunction840,."));

  HFAEntry* dt_node = HFAEntry::New(hfa.get(), "Descriptor_Table", "Edsc_Table",
                                    hfa->papoBand[0]->poNode);
  ASSERT_NE(nullptr, dt_node);
  dt_node->SetIntField("numRows", 3);
  HFAEntry* hist_col =
      HFAEntry::New(hfa.get(), "Histogram", "Edsc_Column", dt_node);
  ASSERT_NE(nullptr, hist_col);
  hist_col->SetIntField("numRows", 3);
  hist_col->SetStringField("dataType", "integer");
  hist_col->SetIntField("maxNumChars", 0);
  GUInt32 h_off = HFAAllocateSpace(hfa.get(), 12);
  hist_col->SetIntField("columnDataPtr", h_off);
  std::vector<GByte> h_bytes(3 * sizeof(GInt32), 0);
  WriteHfaLe(h_bytes.data(), 0, GInt32{10});
  WriteHfaLe(h_bytes.data(), 4, GInt32{20});
  WriteHfaLe(h_bytes.data(), 8, GInt32{30});
  EXPECT_EQ(0, VSIFSeekL(hfa->fp, h_off, SEEK_SET));
  EXPECT_EQ(h_bytes.size(),
            VSIFWriteL(h_bytes.data(), 1, h_bytes.size(), hfa->fp));

  HFAEntry* bf840 = HFAEntry::New(hfa.get(), "#Bin_Function840#",
                                  "Edsc_BinFunction840", dt_node);
  ASSERT_NE(nullptr, bf840);
  ASSERT_NO_FATAL_FAILURE(
      PopulateBFUniqueEntry(bf840, std::to_array({0.0, 2.0, 5.0})));

  // Note: HFASetPCT takes non-const double* pointers in GDAL's C API.
  std::array<double, 2> red = {0.2, 0.8};
  std::array<double, 2> green = {0.3, 0.7};
  std::array<double, 2> blue = {0.4, 0.6};
  std::array<double, 2> alpha = {1.0, 1.0};
  ASSERT_EQ(CE_None, HFASetPCT(hfa.get(), 1, 2, red.data(), green.data(),
                               blue.data(), alpha.data()));
  HFAEntry* pct_bf = hfa->papoBand[0]->poNode->GetNamedChild(
      "Descriptor_Table.#Bin_Function840#");
  ASSERT_NE(nullptr, pct_bf);
  ASSERT_NO_FATAL_FAILURE(
      PopulateBFUniqueEntry(pct_bf, std::to_array({1.0, 3.0})));
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  GDALDatasetUniquePtr ds_bf = OpenHfaDataset(kPath, GA_ReadOnly);
  ASSERT_NE(nullptr, ds_bf);
  EXPECT_STREQ("0|10|0|20|0|30|", ds_bf->GetRasterBand(1)->GetMetadataItem(
                                      "STATISTICS_HISTOBINVALUES"));
  EXPECT_NE(nullptr, ds_bf->GetRasterBand(1)->GetColorTable());
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              ds_bf->BuildOverviews("NEAREST", 1, kOvList.data(), 0, nullptr,
                                    GDALDummyProgress, nullptr, nullptr));
    CPLErrorReset();
  }

  // 2. Non-integer BFUnique histogram bins fail validation in
  //    ReadHistogramMetadata; STATISTICS_HISTOBINVALUES is omitted cleanly.
  hfa.reset(HFAOpen(kPath, "r+b"));
  ASSERT_NE(nullptr, hfa);
  bf840 = hfa->papoBand[0]->poNode->GetNamedChild(
      "Descriptor_Table.#Bin_Function840#");
  ASSERT_NE(nullptr, bf840);
  ASSERT_NO_FATAL_FAILURE(
      PopulateBFUniqueEntry(bf840, std::to_array({0.5, 2.0, 5.0})));
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  GDALDatasetUniquePtr ds_bad_bf = OpenHfaDataset(kPath, GA_ReadOnly);
  ASSERT_NE(nullptr, ds_bad_bf);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, ds_bad_bf->GetRasterBand(1)->GetMetadataItem(
                           "STATISTICS_HISTOBINVALUES"));
    CPLErrorReset();
  }

  // 3. Out-of-range PCT bin values (> 65535) log CPLE_NotSupported and stop
  //    populating further color table entries rather than failing band open.
  hfa.reset(HFAOpen(kPath, "r+b"));
  ASSERT_NE(nullptr, hfa);
  bf840 = hfa->papoBand[0]->poNode->GetNamedChild(
      "Descriptor_Table.#Bin_Function840#");
  ASSERT_NE(nullptr, bf840);
  ASSERT_NO_FATAL_FAILURE(
      PopulateBFUniqueEntry(bf840, std::to_array({1.0, 70000.0, 2.0})));
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  GDALDatasetUniquePtr ds_bad_pct_bf = OpenHfaDataset(kPath, GA_ReadOnly);
  ASSERT_NE(nullptr, ds_bad_pct_bf);
  const GDALColorTable* fallback_ct = nullptr;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    fallback_ct = ds_bad_pct_bf->GetRasterBand(1)->GetColorTable();
    CPLErrorReset();
  }
  ASSERT_NE(nullptr, fallback_ct);
  EXPECT_EQ(2, fallback_ct->GetColorEntryCount());

  // 4. Large >1024-char real histogram.
  hfa.reset(HFACreate(kPath, 16, 16, 1, EPT_u8, nullptr));
  ASSERT_NE(nullptr, hfa);
  dt_node = HFAEntry::New(hfa.get(), "Descriptor_Table", "Edsc_Table",
                          hfa->papoBand[0]->poNode);
  ASSERT_NE(nullptr, dt_node);
  dt_node->SetIntField("numRows", 300);
  hist_col = HFAEntry::New(hfa.get(), "Histogram", "Edsc_Column", dt_node);
  ASSERT_NE(nullptr, hist_col);
  hist_col->SetIntField("numRows", 300);
  hist_col->SetStringField("dataType", "real");
  h_off = HFAAllocateSpace(hfa.get(), 300 * sizeof(double));
  hist_col->SetIntField("columnDataPtr", h_off);
  std::vector<GByte> big_hist_bytes(300 * sizeof(double), 0);
  for (size_t i = 0; i < 300; ++i) {
    WriteHfaLe(big_hist_bytes.data(), i * sizeof(double), 123456.0);
  }
  EXPECT_EQ(0, VSIFSeekL(hfa->fp, h_off, SEEK_SET));
  EXPECT_EQ(big_hist_bytes.size(), VSIFWriteL(big_hist_bytes.data(), 1,
                                              big_hist_bytes.size(), hfa->fp));
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  {
    GDALDatasetUniquePtr ds_big_h = OpenHfaDataset(kPath, GA_ReadOnly);
    ASSERT_NE(nullptr, ds_big_h);
    const char* hb = ds_big_h->GetRasterBand(1)->GetMetadataItem(
        "STATISTICS_HISTOBINVALUES");
    ASSERT_NE(nullptr, hb);
    EXPECT_GT(std::strlen(hb), 1024u);
  }
}

TEST_F(HfaDatasetTest, CreateCopyAllPaths) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };

  constexpr char kOutPath[] = "/vsimem/hfadataset_test/createcopy.img";
  constexpr char kAuxPath[] = "/vsimem/hfadataset_test/createcopy.aux";

  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr mem_ds(
      mem_drv->Create("", 16, 16, 2, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, mem_ds);

  GDALGeoTransform gt(10.0, 1.0, 0.0, 20.0, 0.0, -1.0);
  mem_ds->SetGeoTransform(gt);
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  mem_ds->SetSpatialRef(&srs);
  mem_ds->SetMetadataItem("SRC_DS_KEY", "SRC_DS_VAL");

  GDALRasterBand* b1 = mem_ds->GetRasterBand(1);
  b1->SetMetadataItem("PIXELTYPE", "SIGNEDBYTE", "IMAGE_STRUCTURE");
  b1->SetMetadataItem("SRC_BAND_KEY", "SRC_BAND_VAL");
  b1->SetDescription("SrcBand1");
  b1->SetNoDataValue(0.0);

  // Attach a color table and RAT to the source dataset before CreateCopy.
  GDALColorTable ct;
  constexpr GDALColorEntry kC0 = {10, 20, 30, 255};
  ct.SetColorEntry(0, &kC0);
  b1->SetColorTable(&ct);

  GDALDefaultRasterAttributeTable rat;
  rat.CreateColumn("Class", GFT_String, GFU_Name);
  rat.SetRowCount(1);
  rat.SetValue(0, 0, "Forest");
  b1->SetDefaultRAT(&rat);

  std::vector<GByte> pix(16 * 16, 5);
  pix[0] = 1;
  pix[1] = 250;
  ASSERT_EQ(CE_None, b1->RasterIO(GF_Write, 0, 0, 16, 16, pix.data(), 16, 16,
                                  GDT_Byte, 0, 0, nullptr));
  ASSERT_EQ(CE_None, mem_ds->GetRasterBand(2)->RasterIO(
                         GF_Write, 0, 0, 16, 16, pix.data(), 16, 16, GDT_Byte,
                         0, 0, nullptr));

  // 1. Progress abort at 0.0 and at 1.0.
  EXPECT_EQ(nullptr,
            HFADataset::CreateCopy(kOutPath, mem_ds.get(), FALSE, nullptr,
                                   AbortAtZeroProgress, nullptr));
  EXPECT_EQ(nullptr,
            HFADataset::CreateCopy(kOutPath, mem_ds.get(), FALSE, nullptr,
                                   AbortAtOneProgress, nullptr));

  // 2. Full CreateCopy with STATISTICS=YES.
  CPLStringList cc_opts({"STATISTICS=YES"});
  GDALDatasetUniquePtr copy_ds(
      HFADataset::CreateCopy(kOutPath, mem_ds.get(), FALSE, cc_opts.List(),
                             GDALDummyProgress, nullptr));
  ASSERT_NE(nullptr, copy_ds);
  EXPECT_STREQ("SrcBand1", copy_ds->GetRasterBand(1)->GetDescription());
  copy_ds.reset();

  // 3. CreateCopy with AUX=YES (and abort at 1.0 completion check).
  CPLStringList aux_opts({"AUX=YES"});
  EXPECT_EQ(nullptr, HFADataset::CreateCopy(kAuxPath, mem_ds.get(), FALSE,
                                            aux_opts.List(), AbortAtOneProgress,
                                            nullptr));
  GDALDatasetUniquePtr aux_ds(
      HFADataset::CreateCopy(kAuxPath, mem_ds.get(), FALSE, aux_opts.List(),
                             GDALDummyProgress, nullptr));
  ASSERT_NE(nullptr, aux_ds);
}

TEST_F(HfaDatasetTest, RasterAttributeTableColumnsValuesAndReadOnlyErrors) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };
  constexpr char kPath[] = "/vsimem/hfadataset_test/rat.img";

  GDALDatasetUniquePtr ds(
      HFADataset::Create(kPath, 16, 16, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  HFARasterBand* band = cpl::down_cast<HFARasterBand*>(ds->GetRasterBand(1));
  ASSERT_NE(nullptr, band);

  // 1. Null RAT rejection and multi-type RAT creation/readback.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, band->SetDefaultRAT(nullptr));
    CPLErrorReset();
  }

  GDALDefaultRasterAttributeTable src_rat;
  src_rat.CreateColumn("Red", GFT_Integer, GFU_Red);
  src_rat.CreateColumn("Green", GFT_Integer, GFU_Green);
  src_rat.CreateColumn("Blue", GFT_Integer, GFU_Blue);
  src_rat.CreateColumn("Alpha", GFT_Integer, GFU_Alpha);
  src_rat.CreateColumn("Hist", GFT_Real, GFU_PixelCount);
  src_rat.CreateColumn("Name", GFT_String, GFU_Name);
  src_rat.CreateColumn("GenericInt", GFT_Integer, GFU_Generic);
  src_rat.CreateColumn("GenericReal", GFT_Real, GFU_Generic);
  src_rat.CreateColumn("MinVal", GFT_Real, GFU_Min);
  src_rat.CreateColumn("MaxVal", GFT_Real, GFU_Max);
  src_rat.CreateColumn("DateVal", GFT_DateTime, GFU_Generic);
  src_rat.SetRowCount(3);
  src_rat.SetLinearBinning(10.0, 5.0);
  for (int r = 0; r < 3; ++r) {
    src_rat.SetValue(r, 0, 50 * r);
    src_rat.SetValue(r, 1, 60 * r);
    src_rat.SetValue(r, 2, 70 * r);
    src_rat.SetValue(r, 3, 255);
    src_rat.SetValue(r, 4, 100.0 + r);
    src_rat.SetValue(r, 5, r == 0 ? "A" : "BB");
    src_rat.SetValue(r, 6, 10 + r);
    src_rat.SetValue(r, 7, 1.5 + r);
    src_rat.SetValue(r, 8, 10.0 + r * 5.0);
    src_rat.SetValue(r, 9, 15.0 + r * 5.0);
    src_rat.SetValue(r, 10, "2026-10-30T00:00:00");
  }
  EXPECT_EQ(CE_None, band->SetDefaultRAT(&src_rat));

  GDALRasterAttributeTable* rat = band->GetDefaultRAT();
  ASSERT_NE(nullptr, rat);
  EXPECT_EQ(TRUE, rat->ChangesAreWrittenToFile());
  EXPECT_EQ(11, rat->GetColumnCount());
  EXPECT_EQ(3, rat->GetRowCount());
  EXPECT_STREQ("Red", rat->GetNameOfCol(0));
  EXPECT_EQ(nullptr, rat->GetNameOfCol(-1));
  EXPECT_EQ(nullptr, rat->GetNameOfCol(99));
  EXPECT_EQ(GFU_Red, rat->GetUsageOfCol(0));
  EXPECT_EQ(GFU_Generic, rat->GetUsageOfCol(-1));
  EXPECT_EQ(GFT_Integer, rat->GetTypeOfCol(0));
  EXPECT_EQ(GFT_Integer, rat->GetTypeOfCol(-1));
  EXPECT_EQ(0, rat->GetColOfUsage(GFU_Red));
  EXPECT_EQ(-1, rat->GetColOfUsage(GFU_RedMin));

  // 2. Column metadata, linear binning, and dynamic column creation.
  EXPECT_EQ(CE_None, rat->SetLinearBinning(10.0, 5.0));
  double r0 = 0.0;
  double bsz = 0.0;
  EXPECT_EQ(TRUE, rat->GetLinearBinning(&r0, &bsz));
  EXPECT_DOUBLE_EQ(10.0, r0);
  EXPECT_DOUBLE_EQ(5.0, bsz);
  EXPECT_EQ(1, rat->GetRowOfValue(16.0));
  EXPECT_EQ(1, rat->GetRowOfValue(16));
  EXPECT_EQ(-1, rat->GetRowOfValue(5.0));
  EXPECT_EQ(-1, rat->GetRowOfValue(30.0));

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              rat->CreateColumn("WkbCol", GFT_WKBGeometry, GFU_Generic));
    CPLErrorReset();
  }
  EXPECT_EQ(CE_None, rat->CreateColumn("BoolCol", GFT_Boolean, GFU_Generic));
  EXPECT_EQ(CE_None, rat->CreateColumn("DateCol", GFT_DateTime, GFU_Generic));

  const int bool_col = rat->GetColumnCount() - 2;
  const int date_col = rat->GetColumnCount() - 1;
  EXPECT_EQ(CE_None, rat->SetValue(0, bool_col, true));
  EXPECT_TRUE(rat->GetValueAsBoolean(0, bool_col));
  GDALRATDateTime dt;
  dt.nYear = 2026;
  dt.nMonth = 10;
  dt.nDay = 30;
  dt.bIsValid = true;
  EXPECT_EQ(CE_None, rat->SetValue(0, date_col, dt));
  GDALRATDateTime dt_out = rat->GetValueAsDateTime(0, date_col);
  EXPECT_EQ(2026, dt_out.nYear);

  // 3. Cross-type cell read/write, out-of-range ValuesIO/GetValueAs* errors,
  //    and read-only RAT enforcement.
  constexpr std::array<GByte, 21> kWkbPoint = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                               0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  EXPECT_EQ(CE_None, rat->SetValue(0, 5, kWkbPoint.data(), kWkbPoint.size()));
  size_t wkb_sz = 0;
  EXPECT_NE(nullptr, rat->GetValueAsWKBGeometry(0, 5, wkb_sz));

  EXPECT_EQ(CE_None, rat->SetValue(0, 0, 128.0));
  EXPECT_EQ(CE_None, rat->SetValue(0, 0, "128"));
  EXPECT_STREQ("128", rat->GetValueAsString(0, 0));
  EXPECT_DOUBLE_EQ(128.0, rat->GetValueAsDouble(0, 0));

  EXPECT_EQ(CE_None, rat->SetValue(0, 6, 42.0));
  EXPECT_EQ(CE_None, rat->SetValue(0, 6, "42"));
  EXPECT_STREQ("42", rat->GetValueAsString(0, 6));
  EXPECT_DOUBLE_EQ(42.0, rat->GetValueAsDouble(0, 6));

  EXPECT_EQ(CE_None, rat->SetValue(0, 7, 77));
  EXPECT_EQ(CE_None, rat->SetValue(0, 7, "3.25"));
  EXPECT_EQ(3, rat->GetValueAsInt(0, 7));
  EXPECT_STREQ("3.25", rat->GetValueAsString(0, 7));

  EXPECT_EQ(CE_None, rat->SetValue(0, 5, 99));
  EXPECT_EQ(99, rat->GetValueAsInt(0, 5));
  EXPECT_EQ(CE_None, rat->SetValue(0, 5, 12.5));
  EXPECT_DOUBLE_EQ(12.5, rat->GetValueAsDouble(0, 5));
  EXPECT_EQ(CE_None,
            rat->SetValue(1, 5, "A_much_longer_string_triggering_realloc"));
  EXPECT_STREQ("A_much_longer_string_triggering_realloc",
               rat->GetValueAsString(1, 5));

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    double d_tmp = 0.0;
    int i_tmp = 0;
    char* s_tmp = nullptr;
    EXPECT_EQ(CE_Failure, rat->ValuesIO(GF_Read, -1, 0, 1, &d_tmp));
    EXPECT_EQ(CE_Failure, rat->ValuesIO(GF_Read, 0, -1, 1, &d_tmp));
    EXPECT_EQ(CE_Failure, rat->ValuesIO(GF_Read, -1, 0, 1, &i_tmp));
    EXPECT_EQ(CE_Failure, rat->ValuesIO(GF_Read, 0, -1, 1, &i_tmp));
    EXPECT_EQ(CE_Failure, rat->ValuesIO(GF_Read, -1, 0, 1, &s_tmp));
    EXPECT_EQ(CE_Failure, rat->ValuesIO(GF_Read, 0, -1, 1, &s_tmp));
    EXPECT_STREQ("", rat->GetValueAsString(-1, 5));
    EXPECT_EQ(0, rat->GetValueAsInt(-1, 6));
    EXPECT_DOUBLE_EQ(0.0, rat->GetValueAsDouble(-1, 7));
    EXPECT_FALSE(rat->GetValueAsBoolean(-1, bool_col));
    CPLErrorReset();
  }

  auto cloned = std::unique_ptr<GDALRasterAttributeTable>(rat->Clone());
  ASSERT_NE(nullptr, cloned);
  CPLXMLTreeCloser xml(rat->Serialize());
  ASSERT_NE(nullptr, xml);

  EXPECT_EQ(CE_None, rat->SetTableType(GRTT_ATHEMATIC));
  EXPECT_EQ(GRTT_ATHEMATIC, rat->GetTableType());

  rat->SetRowCount(5);
  EXPECT_EQ(5, rat->GetRowCount());
  rat->SetRowCount(3);
  EXPECT_EQ(3, rat->GetRowCount());

  rat->RemoveStatistics();
  EXPECT_EQ(-1, rat->GetColOfUsage(GFU_PixelCount));
  ds.reset();

  // Remove #Bin_Function# node (created when linear binning is active) so
  // reopening exercises missing linear binning on read-only RAT.
  HfaPtr hfa(HFAOpen(kPath, "r+b"));
  ASSERT_NE(nullptr, hfa);
  HFAEntry* bin_fn = hfa->papoBand[0]->poNode->GetNamedChild(
      "Descriptor_Table.#Bin_Function#");
  ASSERT_NE(nullptr, bin_fn);
  ASSERT_EQ(CE_None, bin_fn->RemoveAndDestroy());
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  GDALDatasetUniquePtr ro_ds = OpenHfaDataset(kPath, GA_ReadOnly);
  ASSERT_NE(nullptr, ro_ds);
  GDALRasterAttributeTable* ro_rat = ro_ds->GetRasterBand(1)->GetDefaultRAT();
  ASSERT_NE(nullptr, ro_rat);
  EXPECT_EQ(FALSE, ro_rat->GetLinearBinning(&r0, &bsz));
  EXPECT_EQ(-1, ro_rat->GetRowOfValue(17.0));

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, ro_rat->SetValue(0, 0, 1.0));
    EXPECT_EQ(CE_Failure, ro_rat->SetValue(0, 0, 1));
    EXPECT_EQ(CE_Failure, ro_rat->SetValue(0, 0, "1"));
    EXPECT_EQ(CE_Failure, ro_rat->CreateColumn("X", GFT_Integer, GFU_Generic));
    EXPECT_EQ(CE_Failure, ro_rat->SetLinearBinning(0.0, 1.0));
    ro_rat->SetRowCount(10);
    CPLErrorReset();
  }
  EXPECT_EQ(3, ro_rat->GetRowCount());
}

TEST_F(HfaDatasetTest,
       RasterAttributeTableBFUniqueMinMaxAndCreateColumnVariants) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };
  constexpr char kPath[] = "/vsimem/hfadataset_test/rat_variants.img";

  // 1. BFUnique BinValues column + unsupported string/complex column errors.
  HfaPtr hfa(HFACreate(kPath, 16, 16, 1, EPT_u8, nullptr));
  ASSERT_NE(nullptr, hfa);
  ASSERT_NO_FATAL_FAILURE(
      RegisterCustomTypes(hfa.get(),
                          "{16:cstring,}BFStr,{1:oBFStr,type,64:cMIFDictionary,"
                          "0:pCMIFObject,}BFMIF,"
                          "{1:oBFMIF,binFunction,}Edsc_BinFunction840,."));
  HFAEntry* dt_node = HFAEntry::New(hfa.get(), "Descriptor_Table", "Edsc_Table",
                                    hfa->papoBand[0]->poNode);
  ASSERT_NE(nullptr, dt_node);
  dt_node->SetIntField("numRows", 2);
  HFAEntry* bf840 = HFAEntry::New(hfa.get(), "#Bin_Function840#",
                                  "Edsc_BinFunction840", dt_node);
  ASSERT_NE(nullptr, bf840);
  ASSERT_NO_FATAL_FAILURE(
      PopulateBFUniqueEntry(bf840, std::to_array({100.0, 200.0})));

  HFAEntry* bad_str_col =
      HFAEntry::New(hfa.get(), "BadStr", "Edsc_Column", dt_node);
  ASSERT_NE(nullptr, bad_str_col);
  bad_str_col->SetIntField("numRows", 2);
  bad_str_col->SetStringField("dataType", "string");
  bad_str_col->SetIntField("maxNumChars", 0);
  bad_str_col->SetIntField("columnDataPtr", HFAAllocateSpace(hfa.get(), 8));

  HFAEntry* cplx_col =
      HFAEntry::New(hfa.get(), "CplxCol", "Edsc_Column", dt_node);
  ASSERT_NE(nullptr, cplx_col);
  cplx_col->SetIntField("numRows", 2);
  cplx_col->SetStringField("dataType", "complex");
  cplx_col->SetIntField("columnDataPtr", HFAAllocateSpace(hfa.get(), 16));
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  GDALDatasetUniquePtr bf_ds = OpenHfaDataset(kPath, GA_Update);
  ASSERT_NE(nullptr, bf_ds);
  GDALRasterAttributeTable* bf_rat = nullptr;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    bf_rat = bf_ds->GetRasterBand(1)->GetDefaultRAT();
    CPLErrorReset();
  }
  ASSERT_NE(nullptr, bf_rat);
  EXPECT_EQ(2, bf_rat->GetColumnCount());
  EXPECT_DOUBLE_EQ(100.0, bf_rat->GetValueAsDouble(0, 0));
  EXPECT_DOUBLE_EQ(200.0, bf_rat->GetValueAsDouble(1, 0));
  EXPECT_EQ(0, bf_rat->GetRowOfValue(100.0));
  EXPECT_EQ(-1, bf_rat->GetRowOfValue(150.0));

  // 2. GFU_Min / GFU_Max GetRowOfValue lookup.
  {
    GDALDatasetUniquePtr mm_ds(
        HFADataset::Create(kPath, 16, 16, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, mm_ds);
    GDALRasterAttributeTable* mm_rat = mm_ds->GetRasterBand(1)->GetDefaultRAT();
    ASSERT_NE(nullptr, mm_rat);
    mm_rat->SetRowCount(2);
    ASSERT_EQ(CE_None, mm_rat->CreateColumn("MinC", GFT_Real, GFU_Min));
    ASSERT_EQ(CE_None, mm_rat->CreateColumn("MaxC", GFT_Real, GFU_Max));
    mm_rat->SetValue(0, 0, 10.0);
    mm_rat->SetValue(0, 1, 15.0);
    mm_rat->SetValue(1, 0, 15.0);
    mm_rat->SetValue(1, 1, 20.0);
    EXPECT_EQ(1, mm_rat->GetRowOfValue(17.0));
    EXPECT_EQ(-1, mm_rat->GetRowOfValue(5.0));
    EXPECT_EQ(-1, mm_rat->GetRowOfValue(999.0));
  }

  // 3. Direct CreateColumn across all usages and field types.
  {
    GDALDatasetUniquePtr cc_ds(
        HFADataset::Create(kPath, 16, 16, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, cc_ds);
    GDALRasterAttributeTable* cc_rat = cc_ds->GetRasterBand(1)->GetDefaultRAT();
    ASSERT_NE(nullptr, cc_rat);
    cc_rat->SetRowCount(2);
    EXPECT_EQ(CE_None, cc_rat->CreateColumn("R", GFT_Integer, GFU_Red));
    EXPECT_EQ(CE_None, cc_rat->CreateColumn("G", GFT_Integer, GFU_Green));
    EXPECT_EQ(CE_None, cc_rat->CreateColumn("B", GFT_Integer, GFU_Blue));
    EXPECT_EQ(CE_None, cc_rat->CreateColumn("A", GFT_Integer, GFU_Alpha));
    EXPECT_EQ(CE_None, cc_rat->CreateColumn("H", GFT_Real, GFU_PixelCount));
    EXPECT_EQ(CE_None, cc_rat->CreateColumn("N", GFT_String, GFU_Name));
    EXPECT_EQ(CE_None, cc_rat->CreateColumn("I", GFT_Integer, GFU_Generic));
    EXPECT_EQ(CE_None, cc_rat->CreateColumn("D", GFT_Real, GFU_Generic));
    EXPECT_EQ(CE_None, cc_rat->CreateColumn("S", GFT_String, GFU_Generic));
    EXPECT_EQ(9, cc_rat->GetColumnCount());
  }
}

// TODO: b/335317901 - Cover remaining ReadProjection/WriteProjection and
// multi-band overview resampling paths in hfadataset.cpp.

}  // namespace
}  // namespace autotest2
