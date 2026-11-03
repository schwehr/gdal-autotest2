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
// Tests GDALPamDataset XML/AUX persistence, georeferencing, cloning, and
// overview metadata (gcore/gdalpamdataset.cpp).
//
// Build caveat in google3: //third_party/gdal/BUILD compiles with PAM_ENABLED
// undefined, so GDAL_PAM_ENABLED defaults to "NO" unless enabled via
// CPLConfigOptionSetter in tests that verify .aux.xml sidecar persistence.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdalpamdataset.cpp

#include <array>
#include <cstring>
#include <filesystem>  // NOLINT
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_pam.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

class TestPamBand : public GDALPamRasterBand {
 public:
  TestPamBand(GDALDataset* ds, int band_num) {
    poDS = ds;
    nBand = band_num;
    eDataType = GDT_Byte;
    nRasterXSize = ds->GetRasterXSize();
    nRasterYSize = ds->GetRasterYSize();
    nBlockXSize = nRasterXSize;
    nBlockYSize = 1;
  }

  CPLErr IReadBlock(int /*x*/, int /*y*/, void* buf) override {
    std::memset(buf, 10, nBlockXSize);
    return CE_None;
  }
};

class TestPamDataset : public GDALPamDataset {
 public:
  TestPamDataset(const char* desc, int xsize = 8, int ysize = 8,
                 int bands = 1) {
    nRasterXSize = xsize;
    nRasterYSize = ysize;
    SetDescription(desc);
    for (int i = 1; i <= bands; ++i) {
      SetBand(i, new TestPamBand(this, i));
    }
  }
  ~TestPamDataset() override { FlushCache(true); }

  using GDALDataset::oOvManager;
  using GDALPamDataset::BuildPamFilename;
  using GDALPamDataset::GetPhysicalFilename;
  using GDALPamDataset::GetSubdatasetName;
  using GDALPamDataset::IBuildOverviews;
  using GDALPamDataset::PamClear;
  using GDALPamDataset::PamInitialize;
  using GDALPamDataset::SerializeToXML;
  using GDALPamDataset::SetDerivedDatasetName;
  using GDALPamDataset::SetPhysicalFilename;
  using GDALPamDataset::SetSubdatasetName;
  using GDALPamDataset::TryLoadAux;
  using GDALPamDataset::TryLoadXML;
  using GDALPamDataset::TrySaveXML;
  using GDALPamDataset::XMLInit;
};

void WriteDummyFile(const char* path) {
  VSILFILE* fp = VSIFOpenL(path, "wb");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup close_fp = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };
  EXPECT_EQ(1u, VSIFWriteL("x", 1, 1, fp));
}

TEST(GdalPamDatasetTest, SerializeXMLInitSaveLoadSubdatasetAndClearStats) {
  GDALRegister_GTiff();
  absl::Cleanup cleanup_dir = [] {
    VSIRmdirRecursive("/vsimem/gdalpamdataset_test");
  };
  CPLConfigOptionSetter pam_enabled("GDAL_PAM_ENABLED", "YES", false);

  // 1. Basic dataset with SRS (mapping + coordinateEpoch), GeoTransform,
  //    GCPs, Metadata, Band metadata/stats, Save & Load XML, ClearStatistics:
  constexpr char kPhysFile[] = "/vsimem/gdalpamdataset_test/pam_ds_main.tif";
  {
    WriteDummyFile(kPhysFile);
    {
      TestPamDataset ds(kPhysFile, 8, 8, 2);
      OGRSpatialReference srs;
      EXPECT_EQ(OGRERR_NONE, srs.SetWellKnownGeogCS("WGS84"));
      EXPECT_EQ(OGRERR_NONE, srs.SetDataAxisToSRSAxisMapping({2, 1}));
      srs.SetCoordinateEpoch(2025.5);
      EXPECT_EQ(CE_None, ds.SetSpatialRef(&srs));
      EXPECT_NE(nullptr, ds.GetSpatialRef());
      EXPECT_NE(nullptr, ds.GetSpatialRefRasterOnly());

      GDALGeoTransform gt(100.0, 1.0, 0.0, 200.0, 0.0, -1.0);
      EXPECT_EQ(CE_None, ds.SetGeoTransform(gt));
      GDALGeoTransform gt_out;
      EXPECT_EQ(CE_None, ds.GetGeoTransform(gt_out));
      EXPECT_DOUBLE_EQ(100.0, gt_out[0]);

      GDAL_GCP gcp;
      GDALInitGCPs(1, &gcp);
      absl::Cleanup deinit_gcp = [&gcp] { GDALDeinitGCPs(1, &gcp); };
      gcp.dfGCPPixel = 1.0;
      gcp.dfGCPLine = 2.0;
      gcp.dfGCPX = 10.0;
      gcp.dfGCPY = 20.0;
      EXPECT_EQ(CE_None, ds.SetGCPs(1, &gcp, &srs));
      EXPECT_EQ(1, ds.GetGCPCount());
      EXPECT_NE(nullptr, ds.GetGCPSpatialRef());
      EXPECT_NE(nullptr, ds.GetGCPs());

      EXPECT_EQ(CE_None, ds.SetMetadataItem("KEY1", "VAL1"));
      CPLStringList md;
      md.SetNameValue("KEY2", "VAL2");
      EXPECT_EQ(CE_None, ds.SetMetadata(md.List()));
      EXPECT_NE(nullptr, ds.GetMetadata());

      EXPECT_EQ(CE_None, ds.GetRasterBand(1)->SetMetadataItem(
                             "STATISTICS_MINIMUM", "0"));
      EXPECT_EQ(CE_None,
                ds.GetRasterBand(1)->SetMetadataItem("OTHER_ITEM", "KEEP"));

      // FlushCache saves dirty PAM XML:
      EXPECT_EQ(CE_None, ds.FlushCache(false));

      // GetFileList includes .aux.xml:
      StringListPtr files(ds.GetFileList(), CSLDestroy);
      EXPECT_GE(CSLCount(files.get()), 1);

      // ClearStatistics removes STATISTICS_* and marks dirty:
      ds.ClearStatistics();
      EXPECT_EQ(nullptr,
                ds.GetRasterBand(1)->GetMetadataItem("STATISTICS_MINIMUM"));
      EXPECT_STREQ("KEEP", ds.GetRasterBand(1)->GetMetadataItem("OTHER_ITEM"));

      // DeleteGeoTransform:
      ds.DeleteGeoTransform();
      EXPECT_NE(CE_None, ds.GetGeoTransform(gt_out));
      // Re-save .aux.xml after ClearStatistics and DeleteGeoTransform:
      EXPECT_EQ(CE_None, ds.FlushCache(false));
    }

    // Reload from saved .aux.xml (including sibling files path):
    {
      TestPamDataset ds(kPhysFile, 8, 8, 2);
      const char* siblings[] = {"pam_ds_main.tif.aux.xml", nullptr};
      EXPECT_EQ(CE_None, ds.TryLoadXML(siblings));
      ASSERT_NE(nullptr, ds.GetSpatialRef());
      EXPECT_DOUBLE_EQ(2025.5, ds.GetSpatialRef()->GetCoordinateEpoch());
      EXPECT_EQ(1, ds.GetGCPCount());

      // Test SuppressOnClose unlinking .aux.xml on Close():
      ds.MarkSuppressOnClose();
      EXPECT_EQ(CE_None, ds.Close());
      VSIStatBufL stat_buf;
      EXPECT_NE(0,
                VSIStatL("/vsimem/gdalpamdataset_test/pam_ds_main.tif.aux.xml",
                         &stat_buf));
    }
  }

  // 2. Subdataset and DerivedDataset XML save & load + Array node preservation:
  constexpr char kSubPhys[] = "/vsimem/gdalpamdataset_test/pam_sub.tif";
  {
    WriteDummyFile(kSubPhys);
    TestPamDataset sub1("SUBDS:1", 8, 8, 1);
    sub1.SetPhysicalFilename(kSubPhys);
    sub1.SetSubdatasetName("1");
    EXPECT_STREQ(kSubPhys, sub1.GetPhysicalFilename());
    EXPECT_STREQ("1", sub1.GetSubdatasetName());
    EXPECT_EQ(CE_None, sub1.SetMetadataItem("SUB_K", "SUB_V1"));
    EXPECT_EQ(CE_None, sub1.TrySaveXML());
    // Update existing Subdataset node in .aux.xml:
    EXPECT_EQ(CE_None, sub1.SetMetadataItem("SUB_K", "SUB_V2"));
    EXPECT_EQ(CE_None, sub1.TrySaveXML());

    TestPamDataset sub1_load("SUBDS:1", 8, 8, 1);
    sub1_load.SetPhysicalFilename(kSubPhys);
    sub1_load.SetSubdatasetName("1");
    EXPECT_EQ(CE_None, sub1_load.TryLoadXML());
    EXPECT_STREQ("SUB_V2", sub1_load.GetMetadataItem("SUB_K"));

    TestPamDataset der1("DERIVED:1", 8, 8, 1);
    der1.SetPhysicalFilename(kSubPhys);
    der1.SetDerivedDatasetName("DER1");
    EXPECT_EQ(CE_None, der1.SetMetadataItem("DER_K", "DER_V"));
    EXPECT_EQ(CE_None, der1.TrySaveXML());

    TestPamDataset der1_load("DERIVED:1", 8, 8, 1);
    der1_load.SetPhysicalFilename(kSubPhys);
    der1_load.SetDerivedDatasetName("DER1");
    EXPECT_EQ(CE_None, der1_load.TryLoadXML());
    EXPECT_STREQ("DER_V", der1_load.GetMetadataItem("DER_K"));
  }
}

TEST(GdalPamDatasetTest, EsriGeodataXformCloneInfoOverviewMetadataAndAux) {
  GDALRegister_GTiff();
  GDALRegister_HFA();
  absl::Cleanup cleanup_dir = [] {
    VSIRmdirRecursive("/vsimem/gdalpamdataset_test");
  };
  CPLConfigOptionSetter pam_enabled("GDAL_PAM_ENABLED", "YES", false);

  // 1. ArcGIS 9.3 GeodataXform with SourceGCPs and TargetGCPs:
  {
    constexpr char kXmlStr[] =
        "<GeodataXform>"
        "  <SpatialReference><WKT>GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
        "SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],"
        "UNIT[\"degree\",0.0174532925199433]]</WKT></SpatialReference>"
        "  <SourceGCPs><Double>10</Double><Double>-20</Double></SourceGCPs>"
        "  <TargetGCPs><Double>100</Double><Double>200</Double></TargetGCPs>"
        "</GeodataXform>";
    CPLXMLTreeCloser tree(CPLParseXMLString(kXmlStr));
    ASSERT_NE(nullptr, tree.get());
    TestPamDataset ds("/vsimem/gdalpamdataset_test/esri_gcp.tif", 8, 8, 1);
    ds.SetPamFlags(GPF_NOSAVE);
    ds.PamInitialize();
    EXPECT_EQ(CE_None, ds.XMLInit(tree.get(), nullptr));
    ASSERT_EQ(1, ds.GetGCPCount());
    EXPECT_DOUBLE_EQ(20.0, ds.GetGCPs()[0].dfGCPLine);  // negated
  }

  // 2. ArcGIS 10 xml:ESRI GeodataXform with PolynomialOrder=1, CoeffX, CoeffY:
  {
    constexpr char kXmlStr[] =
        "<PAMDataset>"
        "  <SRS>EPSG:4326</SRS>"
        "  <GeoTransform>1,2,3</GeoTransform>"  // invalid (3 values -> warning)
        "  <Array name=\"arr\"/>"
        "  <DerivedDataset name=\"d\"/>"
        "  <Metadata domain=\"xml:ESRI\" format=\"xml\">"
        "    <GeodataXform>"
        "      <PolynomialOrder>1</PolynomialOrder>"
        "      <CoeffX><Double>10</Double><Double>2</Double><Double>0</Double>"
        "</CoeffX>"
        "      <CoeffY><Double>20</Double><Double>0</Double><Double>2</Double>"
        "</CoeffY>"
        "    </GeodataXform>"
        "  </Metadata>"
        "</PAMDataset>";
    CPLXMLTreeCloser tree(CPLParseXMLString(kXmlStr));
    ASSERT_NE(nullptr, tree.get());
    TestPamDataset ds("/vsimem/gdalpamdataset_test/esri_poly.tif", 8, 8, 1);
    ds.SetPamFlags(GPF_NOSAVE);
    ds.PamInitialize();
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_None, ds.XMLInit(tree.get(), nullptr));
      EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
      CPLErrorReset();
    }
    GDALGeoTransform gt;
    EXPECT_EQ(CE_None, ds.GetGeoTransform(gt));
    EXPECT_DOUBLE_EQ(2.0, gt[1]);
    EXPECT_DOUBLE_EQ(-2.0, gt[5]);
  }

  // 3. CloneInfo and OVERVIEW_FILE / ProxyOverviewRequest metadata:
  {
    TestPamDataset src("/vsimem/gdalpamdataset_test/clone_src.tif", 8, 8, 1);
    src.SetPamFlags(GPF_NOSAVE);
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.SetWellKnownGeogCS("WGS84"));
    EXPECT_EQ(CE_None, src.SetSpatialRef(&srs));
    GDALGeoTransform gt(1.0, 1.0, 0.0, 2.0, 0.0, -1.0);
    EXPECT_EQ(CE_None, src.SetGeoTransform(gt));
    GDAL_GCP gcp;
    GDALInitGCPs(1, &gcp);
    absl::Cleanup deinit_gcp = [&gcp] { GDALDeinitGCPs(1, &gcp); };
    EXPECT_EQ(CE_None, src.SetGCPs(1, &gcp, &srs));
    EXPECT_EQ(CE_None, src.SetMetadataItem("MK", "MV"));
    EXPECT_EQ(CE_None, src.SetMetadataItem("RPC_K", "RPC_V", "RPC"));

    TestPamDataset dst("/vsimem/gdalpamdataset_test/clone_dst.tif", 8, 8, 2);
    dst.SetPamFlags(GPF_NOSAVE);
    EXPECT_EQ(CE_None, dst.CloneInfo(&src, GCIF_PAM_DEFAULT));
    EXPECT_EQ(CE_None,
              dst.CloneInfo(&src, GCIF_PAM_DEFAULT & ~GCIF_ONLY_IF_MISSING));

    // OVERVIEW_FILE with :::BASE:::
    VSIMkdir("/vsimem/gdalpamdataset_test/sub", 0755);
    dst.SetPhysicalFilename("/vsimem/gdalpamdataset_test/sub/phys.tif");
    EXPECT_EQ(CE_None, dst.SetMetadataItem("OVERVIEW_FILE", ":::BASE:::my.ovr",
                                           "OVERVIEWS"));
    EXPECT_STREQ("/vsimem/gdalpamdataset_test/sub/my.ovr",
                 dst.GetMetadataItem("OVERVIEW_FILE", "OVERVIEWS"));

    // ProxyOverviewRequest metadata domain and IBuildOverviews:
    PamCleanProxyDB();
    absl::Cleanup cleanup_proxy_db = [] { PamCleanProxyDB(); };
    constexpr char kProxyDir[] = "/vsimem/gdalpamdataset_test/pam_proxy";
    CPLConfigOptionSetter pam_proxy("GDAL_PAM_PROXY_DIR", kProxyDir, false);
    EXPECT_EQ(0, VSIMkdir(kProxyDir, 0755));
    EXPECT_NE(nullptr, dst.GetMetadataItem("FILENAME", "ProxyOverviewRequest"));

    constexpr std::array<int, 1> kOvrLevels = {2};
    constexpr std::array<int, 2> kOvrBands = {1, 2};
    dst.oOvManager.Initialize(&dst, "/vsimem/gdalpamdataset_test/sub/phys.tif");
    EXPECT_EQ(CE_None,
              dst.IBuildOverviews("NEAREST", kOvrLevels.size(),
                                  kOvrLevels.data(), kOvrBands.size(),
                                  kOvrBands.data(), nullptr, nullptr, nullptr));

    // TrySaveXML fallback to PamAllocateProxy when primary pam path is
    // unwritable:
    TestPamDataset unwritable(
        "/vsizip//vsimem/gdalpamdataset_test/nonexistent.zip/file.tif", 4, 4,
        1);
    EXPECT_EQ(CE_None, unwritable.SetMetadataItem("K", "V"));
    EXPECT_EQ(CE_None, unwritable.TrySaveXML());
  }

  // 4. TryLoadAux with a real HFA .aux file:
  {
    GDALDriver* hfa_drv = GetGDALDriverManager()->GetDriverByName("HFA");
    ASSERT_NE(nullptr, hfa_drv);
    const std::string raw_path =
        (std::filesystem::path(::testing::TempDir()) / "with_aux.raw").string();
    const std::string aux_path =
        (std::filesystem::path(::testing::TempDir()) / "with_aux.aux").string();
    absl::Cleanup cleanup_aux_files = [&raw_path, &aux_path] {
      VSIUnlink(raw_path.c_str());
      VSIUnlink(aux_path.c_str());
    };
    WriteDummyFile(raw_path.c_str());
    const char* aux_opts[] = {"AUX=YES", "DEPENDENT_FILE=with_aux.raw",
                              nullptr};
    GDALDatasetUniquePtr aux_ds(hfa_drv->Create(
        aux_path.c_str(), 8, 8, 1, GDT_Byte, const_cast<char**>(aux_opts)));
    ASSERT_NE(nullptr, aux_ds);

    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.SetWellKnownGeogCS("WGS84"));
    EXPECT_EQ(CE_None, aux_ds->SetSpatialRef(&srs));
    GDALGeoTransform gt(0.0, 1.0, 0.0, 0.0, 0.0, -1.0);
    EXPECT_EQ(CE_None, aux_ds->SetGeoTransform(gt));
    GDAL_GCP aux_gcp;
    GDALInitGCPs(1, &aux_gcp);
    {
      absl::Cleanup deinit_aux_gcp = [&aux_gcp] {
        GDALDeinitGCPs(1, &aux_gcp);
      };
      EXPECT_EQ(CE_None, aux_ds->SetGCPs(1, &aux_gcp, &srs));
    }
    EXPECT_EQ(CE_None, aux_ds->SetMetadataItem("AUX_MD", "VAL"));
    EXPECT_EQ(CE_None, aux_ds->SetMetadataItem("XFORM_K", "XFORM_V", "XFORMS"));
    GDALRasterBand* b = aux_ds->GetRasterBand(1);
    ASSERT_NE(nullptr, b);
    b->SetDescription("AuxBand");
    EXPECT_EQ(CE_None, b->SetNoDataValue(99.0));
    EXPECT_EQ(CE_None, b->SetMetadataItem("BAND_MD", "BVAL"));
    const char* cats[] = {"Cat0", "Cat1", nullptr};
    EXPECT_EQ(CE_None, b->SetCategoryNames(const_cast<char**>(cats)));
    GDALColorTable ct;
    constexpr GDALColorEntry kColor = {10, 20, 30, 255};
    ct.SetColorEntry(0, &kColor);
    EXPECT_EQ(CE_None, b->SetColorTable(&ct));
    std::array<GUIntBig, 2> hist = {32, 32};
    EXPECT_EQ(CE_None, b->SetDefaultHistogram(0.0, 255.0, 2, hist.data()));
    GDALDefaultRasterAttributeTable rat;
    EXPECT_EQ(CE_None, rat.CreateColumn("Name", GFT_String, GFU_Name));
    rat.SetRowCount(1);
    EXPECT_EQ(CE_None, b->SetDefaultRAT(&rat));
    aux_ds.reset();

    TestPamDataset ds(raw_path.c_str(), 8, 8, 1);
    ds.SetPamFlags(GPF_NOSAVE);
    ds.SetPhysicalFilename(raw_path.c_str());
    const char* miss_siblings[] = {"other.file", nullptr};
    ds.TryLoadAux(miss_siblings);
    const char* hit_siblings[] = {"with_aux.aux", nullptr};
    ds.TryLoadAux(hit_siblings);
    EXPECT_STREQ("AuxBand", ds.GetRasterBand(1)->GetDescription());
    StringListPtr fl(ds.GetFileList(), CSLDestroy);
    EXPECT_GE(CSLCount(fl.get()), 1);
  }

  // 5. Additional PAM edge cases:
  // - Unknown XML child node preserved in m_apoOtherNodes across XMLInit &
  //   SerializeToXML
  // - TrySaveXML on unwritable path without proxy DB
  // - GDAL_PAM_ENABLED=NO and GDAL_PAM_MODE=AUX in PamInitialize /
  //   IBuildOverviews
  // - GetFileList with oOvManager sibling matching pam file when !GPF_DIRTY
  {
    {
      TestPamDataset other_ds("/vsimem/gdalpamdataset_test/other_nodes.tif", 4,
                              4, 1);
      other_ds.PamInitialize();
      CPLXMLTreeCloser tree(CPLParseXMLString(
          "<PAMDataset><Array name=\"arr\">hello</Array>"
          "<Metadata><MDI key=\"K\">V</MDI></Metadata></PAMDataset>"));
      ASSERT_NE(nullptr, tree.get());
      EXPECT_EQ(CE_None, other_ds.XMLInit(tree.get(), ""));
      EXPECT_EQ(CE_None, other_ds.SetMetadataItem("K2", "V2"));
      EXPECT_EQ(CE_None, other_ds.TrySaveXML());
      CPLXMLTreeCloser saved_tree(CPLParseXMLFile(
          "/vsimem/gdalpamdataset_test/other_nodes.tif.aux.xml"));
      ASSERT_NE(nullptr, saved_tree.get());
      EXPECT_NE(nullptr, CPLGetXMLNode(saved_tree.get(), "=PAMDataset.Array"));
    }

    // Unwritable non-vsicurl path without proxy DB:
    {
      TestPamDataset fail_save("/nonexistent_root_dir_xyz_999/unwritable.tif",
                               4, 4, 1);
      EXPECT_EQ(CE_None, fail_save.SetMetadataItem("K", "V"));
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Warning, fail_save.TrySaveXML());
      EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
      CPLErrorReset();
    }

    // GetFileList when oOvManager has sibling files and !GPF_DIRTY:
    {
      constexpr char kCleanTif[] = "/vsimem/gdalpamdataset_test/fl_clean.tif";
      constexpr char kCleanAux[] =
          "/vsimem/gdalpamdataset_test/fl_clean.tif.aux.xml";
      WriteDummyFile(kCleanTif);
      WriteDummyFile(kCleanAux);
      TestPamDataset clean_ds(kCleanTif, 4, 4, 1);
      clean_ds.PamInitialize();
      clean_ds.SetPhysicalFilename(kCleanTif);
      EXPECT_NE(nullptr, clean_ds.BuildPamFilename());
      clean_ds.SetPamFlags(clean_ds.GetPamFlags() & ~GPF_DIRTY);
      const char* ov_siblings[] = {"fl_clean.tif.aux.xml", nullptr};
      clean_ds.oOvManager.Initialize(&clean_ds, kCleanTif,
                                     const_cast<char**>(ov_siblings));
      StringListPtr fl(clean_ds.GetFileList(), CSLDestroy);
      EXPECT_GE(CSLCount(fl.get()), 2);
    }

    // GDAL_PAM_MODE=AUX:
    {
      CPLConfigOptionSetter pam_mode("GDAL_PAM_MODE", "AUX", false);
      TestPamDataset aux_mode_ds("/vsimem/gdalpamdataset_test/aux_mode.tif", 4,
                                 4, 1);
      aux_mode_ds.PamInitialize();
      EXPECT_EQ(GPF_AUXMODE, aux_mode_ds.GetPamFlags() & GPF_AUXMODE);
    }

    // GDAL_PAM_ENABLED=NO:
    {
      constexpr char kPamOffTif[] = "/vsimem/gdalpamdataset_test/pam_off.tif";
      WriteDummyFile(kPamOffTif);
      CPLConfigOptionSetter pam_off("GDAL_PAM_ENABLED", "NO", false);
      TestPamDataset off_ds(kPamOffTif, 4, 4, 1);
      off_ds.PamInitialize();
      off_ds.oOvManager.Initialize(&off_ds, kPamOffTif);
      constexpr std::array<int, 1> kOvrLevels = {2};
      constexpr std::array<int, 1> kOvrBands = {1};
      EXPECT_EQ(CE_None, off_ds.IBuildOverviews(
                             "NEAREST", kOvrLevels.size(), kOvrLevels.data(),
                             kOvrBands.size(), kOvrBands.data(), nullptr,
                             nullptr, nullptr));
    }
  }
}

}  // namespace
}  // namespace autotest2
