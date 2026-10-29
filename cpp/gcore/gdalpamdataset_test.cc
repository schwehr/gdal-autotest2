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
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal_pam.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

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
    memset(buf, 10, nBlockXSize);
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
  ASSERT_NE(fp, nullptr);
  VSIFWriteL("x", 1, 1, fp);
  VSIFCloseL(fp);
}

TEST(GdalPamDatasetTest, SerializeXMLInitSaveLoadSubdatasetAndClearStats) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);
  CPLConfigOptionSetter pam_enabled("GDAL_PAM_ENABLED", "YES", false);

  // 1. Basic dataset with SRS (mapping + coordinateEpoch), GeoTransform,
  //    GCPs, Metadata, Band metadata/stats, Save & Load XML, ClearStatistics:
  const char* phys_file = "/vsimem/pam_ds_main.tif";
  WriteDummyFile(phys_file);
  {
    TestPamDataset ds(phys_file, 8, 8, 2);
    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    srs.SetDataAxisToSRSAxisMapping({2, 1});
    srs.SetCoordinateEpoch(2025.5);
    EXPECT_EQ(ds.SetSpatialRef(&srs), CE_None);
    EXPECT_NE(ds.GetSpatialRef(), nullptr);
    EXPECT_NE(ds.GetSpatialRefRasterOnly(), nullptr);

    GDALGeoTransform gt(100.0, 1.0, 0.0, 200.0, 0.0, -1.0);
    EXPECT_EQ(ds.SetGeoTransform(gt), CE_None);
    GDALGeoTransform gt_out;
    EXPECT_EQ(ds.GetGeoTransform(gt_out), CE_None);
    EXPECT_DOUBLE_EQ(gt_out[0], 100.0);

    GDAL_GCP gcp;
    GDALInitGCPs(1, &gcp);
    gcp.dfGCPPixel = 1.0;
    gcp.dfGCPLine = 2.0;
    gcp.dfGCPX = 10.0;
    gcp.dfGCPY = 20.0;
    EXPECT_EQ(ds.SetGCPs(1, &gcp, &srs), CE_None);
    GDALDeinitGCPs(1, &gcp);
    EXPECT_EQ(ds.GetGCPCount(), 1);
    EXPECT_NE(ds.GetGCPSpatialRef(), nullptr);
    EXPECT_NE(ds.GetGCPs(), nullptr);

    EXPECT_EQ(ds.SetMetadataItem("KEY1", "VAL1"), CE_None);
    CPLStringList md;
    md.SetNameValue("KEY2", "VAL2");
    EXPECT_EQ(ds.SetMetadata(md.List()), CE_None);
    EXPECT_NE(ds.GetMetadata(), nullptr);

    ds.GetRasterBand(1)->SetMetadataItem("STATISTICS_MINIMUM", "0");
    ds.GetRasterBand(1)->SetMetadataItem("OTHER_ITEM", "KEEP");

    // FlushCache saves dirty PAM XML:
    EXPECT_EQ(ds.FlushCache(false), CE_None);

    // GetFileList includes .aux.xml:
    char** files = ds.GetFileList();
    EXPECT_GE(CSLCount(files), 1);
    CSLDestroy(files);

    // ClearStatistics removes STATISTICS_* and marks dirty:
    ds.ClearStatistics();
    EXPECT_EQ(ds.GetRasterBand(1)->GetMetadataItem("STATISTICS_MINIMUM"),
              nullptr);
    EXPECT_STREQ(ds.GetRasterBand(1)->GetMetadataItem("OTHER_ITEM"), "KEEP");

    // DeleteGeoTransform:
    ds.DeleteGeoTransform();
    EXPECT_NE(ds.GetGeoTransform(gt_out), CE_None);
    // Re-save .aux.xml after ClearStatistics and DeleteGeoTransform:
    EXPECT_EQ(ds.FlushCache(false), CE_None);
  }

  // Reload from saved .aux.xml (including sibling files path):
  {
    TestPamDataset ds(phys_file, 8, 8, 2);
    const char* siblings[] = {"pam_ds_main.tif.aux.xml", nullptr};
    EXPECT_EQ(ds.TryLoadXML(siblings), CE_None);
    ASSERT_NE(ds.GetSpatialRef(), nullptr);
    EXPECT_DOUBLE_EQ(ds.GetSpatialRef()->GetCoordinateEpoch(), 2025.5);
    EXPECT_EQ(ds.GetGCPCount(), 1);

    // Test SuppressOnClose unlinking .aux.xml on Close():
    ds.MarkSuppressOnClose();
    EXPECT_EQ(ds.Close(), CE_None);
    VSIStatBufL stat_buf;
    EXPECT_NE(VSIStatL("/vsimem/pam_ds_main.tif.aux.xml", &stat_buf), 0);
  }
  VSIUnlink(phys_file);

  // 2. Subdataset and DerivedDataset XML save & load + Array node preservation:
  const char* sub_phys = "/vsimem/pam_sub.tif";
  WriteDummyFile(sub_phys);
  {
    TestPamDataset sub1("SUBDS:1", 8, 8, 1);
    sub1.SetPhysicalFilename(sub_phys);
    sub1.SetSubdatasetName("1");
    EXPECT_STREQ(sub1.GetPhysicalFilename(), sub_phys);
    EXPECT_STREQ(sub1.GetSubdatasetName(), "1");
    sub1.SetMetadataItem("SUB_K", "SUB_V1");
    EXPECT_EQ(sub1.TrySaveXML(), CE_None);
    // Update existing Subdataset node in .aux.xml:
    sub1.SetMetadataItem("SUB_K", "SUB_V2");
    EXPECT_EQ(sub1.TrySaveXML(), CE_None);

    TestPamDataset sub1_load("SUBDS:1", 8, 8, 1);
    sub1_load.SetPhysicalFilename(sub_phys);
    sub1_load.SetSubdatasetName("1");
    EXPECT_EQ(sub1_load.TryLoadXML(), CE_None);
    EXPECT_STREQ(sub1_load.GetMetadataItem("SUB_K"), "SUB_V2");

    TestPamDataset der1("DERIVED:1", 8, 8, 1);
    der1.SetPhysicalFilename(sub_phys);
    der1.SetDerivedDatasetName("DER1");
    der1.SetMetadataItem("DER_K", "DER_V");
    EXPECT_EQ(der1.TrySaveXML(), CE_None);

    TestPamDataset der1_load("DERIVED:1", 8, 8, 1);
    der1_load.SetPhysicalFilename(sub_phys);
    der1_load.SetDerivedDatasetName("DER1");
    EXPECT_EQ(der1_load.TryLoadXML(), CE_None);
    EXPECT_STREQ(der1_load.GetMetadataItem("DER_K"), "DER_V");
  }
  VSIUnlink(sub_phys);
  VSIUnlink("/vsimem/pam_sub.tif.aux.xml");

  CPLPopErrorHandler();
}

TEST(GdalPamDatasetTest, EsriGeodataXformCloneInfoOverviewMetadataAndAux) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);
  CPLConfigOptionSetter pam_enabled("GDAL_PAM_ENABLED", "YES", false);

  // 1. ArcGIS 9.3 GeodataXform with SourceGCPs and TargetGCPs:
  {
    const char* xml_str =
        "<GeodataXform>"
        "  <SpatialReference><WKT>GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\","
        "SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],"
        "UNIT[\"degree\",0.0174532925199433]]</WKT></SpatialReference>"
        "  <SourceGCPs><Double>10</Double><Double>-20</Double></SourceGCPs>"
        "  <TargetGCPs><Double>100</Double><Double>200</Double></TargetGCPs>"
        "</GeodataXform>";
    CPLXMLNode* tree = CPLParseXMLString(xml_str);
    ASSERT_NE(tree, nullptr);
    TestPamDataset ds("/vsimem/esri_gcp.tif", 8, 8, 1);
    ds.SetPamFlags(GPF_NOSAVE);
    ds.PamInitialize();
    EXPECT_EQ(ds.XMLInit(tree, nullptr), CE_None);
    CPLDestroyXMLNode(tree);
    ASSERT_EQ(ds.GetGCPCount(), 1);
    EXPECT_DOUBLE_EQ(ds.GetGCPs()[0].dfGCPLine, 20.0);  // negated
  }

  // 2. ArcGIS 10 xml:ESRI GeodataXform with PolynomialOrder=1, CoeffX, CoeffY:
  {
    const char* xml_str =
        "<PAMDataset>"
        "  <SRS>EPSG:4326</SRS>"
        "  <GeoTransform>1,2,3</GeoTransform>"  // invalid (3 values -> warning)
        "  <Array name=\"arr\"/>"
        "  <DerivedDataset name=\"d\"/>"
        "  <Metadata domain=\"xml:ESRI\" format=\"xml\">"
        "    <GeodataXform>"
        "      <PolynomialOrder>1</PolynomialOrder>"
        "      "
        "<CoeffX><Double>10</Double><Double>2</Double><Double>0</Double></"
        "CoeffX>"
        "      "
        "<CoeffY><Double>20</Double><Double>0</Double><Double>2</Double></"
        "CoeffY>"
        "    </GeodataXform>"
        "  </Metadata>"
        "</PAMDataset>";
    CPLXMLNode* tree = CPLParseXMLString(xml_str);
    ASSERT_NE(tree, nullptr);
    TestPamDataset ds("/vsimem/esri_poly.tif", 8, 8, 1);
    ds.SetPamFlags(GPF_NOSAVE);
    ds.PamInitialize();
    EXPECT_EQ(ds.XMLInit(tree, nullptr), CE_None);
    CPLDestroyXMLNode(tree);
    GDALGeoTransform gt;
    EXPECT_EQ(ds.GetGeoTransform(gt), CE_None);
    EXPECT_DOUBLE_EQ(gt[1], 2.0);
    EXPECT_DOUBLE_EQ(gt[5], -2.0);
  }

  // 3. CloneInfo and OVERVIEW_FILE / ProxyOverviewRequest metadata:
  {
    TestPamDataset src("/vsimem/clone_src.tif", 8, 8, 1);
    src.SetPamFlags(GPF_NOSAVE);
    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    src.SetSpatialRef(&srs);
    GDALGeoTransform gt(1.0, 1.0, 0.0, 2.0, 0.0, -1.0);
    src.SetGeoTransform(gt);
    GDAL_GCP gcp;
    GDALInitGCPs(1, &gcp);
    src.SetGCPs(1, &gcp, &srs);
    GDALDeinitGCPs(1, &gcp);
    src.SetMetadataItem("MK", "MV");
    src.SetMetadataItem("RPC_K", "RPC_V", "RPC");

    TestPamDataset dst("/vsimem/clone_dst.tif", 8, 8, 2);
    dst.SetPamFlags(GPF_NOSAVE);
    EXPECT_EQ(dst.CloneInfo(&src, GCIF_PAM_DEFAULT), CE_None);
    EXPECT_EQ(dst.CloneInfo(&src, GCIF_PAM_DEFAULT & ~GCIF_ONLY_IF_MISSING),
              CE_None);

    // OVERVIEW_FILE with :::BASE:::
    dst.SetPhysicalFilename("/vsimem/sub/phys.tif");
    dst.SetMetadataItem("OVERVIEW_FILE", ":::BASE:::my.ovr", "OVERVIEWS");
    EXPECT_STREQ(dst.GetMetadataItem("OVERVIEW_FILE", "OVERVIEWS"),
                 "/vsimem/sub/my.ovr");

    // ProxyOverviewRequest metadata domain and IBuildOverviews:
    CPLConfigOptionSetter pam_proxy("GDAL_PAM_PROXY_DIR", "/vsimem/pam_proxy",
                                    false);
    VSIMkdir("/vsimem/pam_proxy", 0755);
    PamCleanProxyDB();
    EXPECT_NE(dst.GetMetadataItem("FILENAME", "ProxyOverviewRequest"), nullptr);

    int ovr_levels[] = {2};
    int ovr_bands[] = {1};
    dst.oOvManager.Initialize(&dst, "/vsimem/sub/phys.tif");
    dst.IBuildOverviews("NEAREST", 1, ovr_levels, 1, ovr_bands, nullptr,
                        nullptr, nullptr);

    // TrySaveXML fallback to PamAllocateProxy when primary pam path is
    // unwritable:
    TestPamDataset unwritable("/vsizip//vsimem/nonexistent.zip/file.tif", 4, 4,
                              1);
    unwritable.SetMetadataItem("K", "V");
    EXPECT_EQ(unwritable.TrySaveXML(), CE_None);
    PamCleanProxyDB();
  }

  // 4. TryLoadAux with a real HFA .aux file:
  GDALDriver* hfa_drv = GetGDALDriverManager()->GetDriverByName("HFA");
  if (hfa_drv != nullptr) {
    std::string raw_path = testing::TempDir() + "/with_aux.raw";
    std::string aux_path = testing::TempDir() + "/with_aux.aux";
    WriteDummyFile(raw_path.c_str());
    const char* aux_opts[] = {"AUX=YES", "DEPENDENT_FILE=with_aux.raw",
                              nullptr};
    GDALDataset* aux_ds = hfa_drv->Create(aux_path.c_str(), 8, 8, 1, GDT_Byte,
                                          const_cast<char**>(aux_opts));
    if (aux_ds != nullptr) {
      OGRSpatialReference srs;
      srs.SetWellKnownGeogCS("WGS84");
      aux_ds->SetSpatialRef(&srs);
      GDALGeoTransform gt(0.0, 1.0, 0.0, 0.0, 0.0, -1.0);
      aux_ds->SetGeoTransform(gt);
      GDAL_GCP aux_gcp;
      GDALInitGCPs(1, &aux_gcp);
      aux_ds->SetGCPs(1, &aux_gcp, &srs);
      GDALDeinitGCPs(1, &aux_gcp);
      aux_ds->SetMetadataItem("AUX_MD", "VAL");
      aux_ds->SetMetadataItem("XFORM_K", "XFORM_V", "XFORMS");
      GDALRasterBand* b = aux_ds->GetRasterBand(1);
      b->SetDescription("AuxBand");
      b->SetNoDataValue(99.0);
      b->SetMetadataItem("BAND_MD", "BVAL");
      const char* cats[] = {"Cat0", "Cat1", nullptr};
      b->SetCategoryNames(const_cast<char**>(cats));
      GDALColorTable ct;
      GDALColorEntry ce = {10, 20, 30, 255};
      ct.SetColorEntry(0, &ce);
      b->SetColorTable(&ct);
      GUIntBig hist[2] = {32, 32};
      b->SetDefaultHistogram(0.0, 255.0, 2, hist);
      GDALDefaultRasterAttributeTable rat;
      rat.CreateColumn("Name", GFT_String, GFU_Name);
      rat.SetRowCount(1);
      b->SetDefaultRAT(&rat);
      GDALClose(aux_ds);

      TestPamDataset ds(raw_path.c_str(), 8, 8, 1);
      ds.SetPamFlags(GPF_NOSAVE);
      ds.SetPhysicalFilename(raw_path.c_str());
      const char* miss_siblings[] = {"other.file", nullptr};
      ds.TryLoadAux(miss_siblings);
      const char* hit_siblings[] = {"with_aux.aux", nullptr};
      ds.TryLoadAux(hit_siblings);
      EXPECT_STREQ(ds.GetRasterBand(1)->GetDescription(), "AuxBand");
      char** fl = ds.GetFileList();
      CSLDestroy(fl);
    }
    VSIUnlink(raw_path.c_str());
    VSIUnlink(aux_path.c_str());
  }

  // 5. Additional PAM edge cases:
  // - Unknown XML child node preserved in m_apoOtherNodes across XMLInit &
  // SerializeToXML
  // - TrySaveXML on unwritable path without proxy DB
  // - GDAL_PAM_ENABLED=NO and GDAL_PAM_MODE=AUX in PamInitialize /
  // IBuildOverviews
  // - GetFileList with oOvManager sibling matching pam file when !GPF_DIRTY
  {
    TestPamDataset other_ds("/vsimem/other_nodes.tif", 4, 4, 1);
    other_ds.PamInitialize();
    CPLXMLNode* tree = CPLParseXMLString(
        "<PAMDataset><Array name=\"arr\">hello</Array>"
        "<Metadata><MDI key=\"K\">V</MDI></Metadata></PAMDataset>");
    ASSERT_NE(tree, nullptr);
    EXPECT_EQ(other_ds.XMLInit(tree, ""), CE_None);
    CPLDestroyXMLNode(tree);
    other_ds.SetMetadataItem("K2", "V2");
    EXPECT_EQ(other_ds.TrySaveXML(), CE_None);
    CPLXMLNode* saved_tree = CPLParseXMLFile("/vsimem/other_nodes.tif.aux.xml");
    ASSERT_NE(saved_tree, nullptr);
    EXPECT_NE(CPLGetXMLNode(saved_tree, "=PAMDataset.Array"), nullptr);
    CPLDestroyXMLNode(saved_tree);
    VSIUnlink("/vsimem/other_nodes.tif.aux.xml");

    // Unwritable non-vsicurl path without proxy DB:
    TestPamDataset fail_save("/nonexistent_root_dir_xyz_999/unwritable.tif", 4,
                             4, 1);
    fail_save.SetMetadataItem("K", "V");
    EXPECT_EQ(fail_save.TrySaveXML(), CE_Warning);

    // GetFileList when oOvManager has sibling files and !GPF_DIRTY:
    WriteDummyFile("/vsimem/fl_clean.tif");
    WriteDummyFile("/vsimem/fl_clean.tif.aux.xml");
    TestPamDataset clean_ds("/vsimem/fl_clean.tif", 4, 4, 1);
    clean_ds.PamInitialize();
    clean_ds.SetPhysicalFilename("/vsimem/fl_clean.tif");
    clean_ds.SetPamFlags(clean_ds.GetPamFlags() & ~GPF_DIRTY);
    const char* ov_siblings[] = {"fl_clean.tif.aux.xml", nullptr};
    clean_ds.oOvManager.Initialize(&clean_ds, "/vsimem/fl_clean.tif",
                                   const_cast<char**>(ov_siblings));
    char** fl = clean_ds.GetFileList();
    CSLDestroy(fl);
    VSIUnlink("/vsimem/fl_clean.tif");
    VSIUnlink("/vsimem/fl_clean.tif.aux.xml");

    // GDAL_PAM_MODE=AUX:
    {
      CPLConfigOptionSetter pam_mode("GDAL_PAM_MODE", "AUX", false);
      TestPamDataset aux_mode_ds("/vsimem/aux_mode.tif", 4, 4, 1);
      aux_mode_ds.PamInitialize();
      EXPECT_EQ(aux_mode_ds.GetPamFlags() & GPF_AUXMODE, GPF_AUXMODE);
    }

    // GDAL_PAM_ENABLED=NO:
    {
      CPLConfigOptionSetter pam_off("GDAL_PAM_ENABLED", "NO", false);
      TestPamDataset off_ds("/vsimem/pam_off.tif", 4, 4, 1);
      off_ds.PamInitialize();
      int ovr_level = 2;
      int ovr_band = 1;
      off_ds.IBuildOverviews("NEAREST", 1, &ovr_level, 1, &ovr_band, nullptr,
                             nullptr, nullptr);
    }
  }

  CPLPopErrorHandler();
}

}  // namespace
