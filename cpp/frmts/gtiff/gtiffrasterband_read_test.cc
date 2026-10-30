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

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/frmts/gtiff/gtiffdataset.h"
#include "third_party/gdal/frmts/gtiff/gtiffrasterband.h"
#include "third_party/gdal/frmts/gtiff/tifvsi.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_virtualmem.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/port/cpl_vsi_virtual.h"

namespace {

// ---------------------------------------------------------------------------
// 100% RAM memfd-backed VSI filesystem ("/vsimem_fd/") so
// VSIFGetNativeFileDescriptorL and CPLVirtualMemFileMapNew succeed in RAM.
// ---------------------------------------------------------------------------
class MemFdHandler final : public VSIFilesystemHandler {
 public:
  ~MemFdHandler() override {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& kv : fds_) {
      close(kv.second);
    }
  }

  static std::string ProcPath(int fd) {
    return CPLSPrintf("/proc/self/fd/%d", fd);
  }

  VSIVirtualHandleUniquePtr Open(const char* pszFilename, const char* pszAccess,
                                 bool bSetError,
                                 CSLConstList papszOptions) override {
    int fd = -1;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = fds_.find(pszFilename);
      const bool create = strchr(pszAccess, 'w') != nullptr ||
                          strchr(pszAccess, 'a') != nullptr;
      if (it == fds_.end()) {
        if (!create) return nullptr;
        fd = memfd_create("gtiff_rb_read_vmem", 0);
        if (fd < 0) return nullptr;
        fds_[pszFilename] = fd;
      } else {
        fd = it->second;
        if (strchr(pszAccess, 'w') != nullptr &&
            strchr(pszAccess, '+') == nullptr) {
          (void)ftruncate(fd, 0);
        }
      }
    }
    const char* delegated_access =
        (strcmp(pszAccess, "wb+") == 0 || strcmp(pszAccess, "w+b") == 0)
            ? "r+b"
            : pszAccess;
    if (strcmp(pszAccess, "wb+") == 0 || strcmp(pszAccess, "w+b") == 0) {
      (void)ftruncate(fd, 0);
    }
    return VSIFileManager::GetHandler("")->Open(
        ProcPath(fd).c_str(), delegated_access, bSetError, papszOptions);
  }

  int Stat(const char* pszFilename, VSIStatBufL* pStatBuf,
           int nFlags) override {
    int fd = -1;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = fds_.find(pszFilename);
      if (it == fds_.end()) return -1;
      fd = it->second;
    }
    return VSIFileManager::GetHandler("")->Stat(ProcPath(fd).c_str(), pStatBuf,
                                                nFlags);
  }

  int Unlink(const char* pszFilename) override {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = fds_.find(pszFilename);
    if (it == fds_.end()) return -1;
    close(it->second);
    fds_.erase(it);
    return 0;
  }

 private:
  std::mutex mu_;
  std::map<std::string, int> fds_;
};

// ---------------------------------------------------------------------------
// 100% RAM wrapper around /vsimem/ ("/vsimem_mr/") with configurable
// HasOptimizedReadMultiRange, HasPRead, GetRangeStatus, and SiblingFiles.
// ---------------------------------------------------------------------------
class MultiRangeVsiMemHandler;

class MultiRangeHandle final : public VSIProxyFileHandle {
 public:
  MultiRangeHandle(VSIVirtualHandleUniquePtr&& inner,
                   const MultiRangeVsiMemHandler* handler)
      : VSIProxyFileHandle(std::move(inner)), handler_(handler) {}

  VSIRangeStatus GetRangeStatus(vsi_l_offset, vsi_l_offset) override;
  bool HasPRead() const override;
  int ReadMultiRange(int nRanges, void** ppData, const vsi_l_offset* panOffsets,
                     const size_t* panSizes) override;

 private:
  const MultiRangeVsiMemHandler* handler_;
};

class MultiRangeVsiMemHandler final : public VSIFilesystemHandler {
 public:
  static constexpr const char* kPrefix = "/vsimem_mr/";

  static std::string ToMem(const char* path) {
    if (strncmp(path, kPrefix, strlen(kPrefix)) == 0) {
      return std::string("/vsimem/mr_backing_") + (path + strlen(kPrefix));
    }
    return path;
  }

  VSIVirtualHandleUniquePtr Open(const char* pszFilename, const char* pszAccess,
                                 bool bSetError,
                                 CSLConstList papszOptions) override {
    const std::string mem = ToMem(pszFilename);
    auto inner = VSIFileManager::GetHandler("/vsimem/")
                     ->Open(mem.c_str(), pszAccess, bSetError, papszOptions);
    if (!inner) return nullptr;
    return VSIVirtualHandleUniquePtr(
        new MultiRangeHandle(std::move(inner), this));
  }

  int Stat(const char* pszFilename, VSIStatBufL* pStatBuf,
           int nFlags) override {
    const std::string mem = ToMem(pszFilename);
    return VSIFileManager::GetHandler("/vsimem/")
        ->Stat(mem.c_str(), pStatBuf, nFlags);
  }

  int Unlink(const char* pszFilename) override {
    const std::string mem = ToMem(pszFilename);
    return VSIFileManager::GetHandler("/vsimem/")->Unlink(mem.c_str());
  }

  int HasOptimizedReadMultiRange(const char*) override {
    return opt_multi_range ? TRUE : FALSE;
  }

  char** SiblingFiles(const char*) override {
    return return_siblings ? CSLDuplicate(siblings.List()) : nullptr;
  }

  VSIRangeStatus range_status = VSI_RANGE_STATUS_DATA;
  bool has_pread = false;
  bool fail_multi_range = false;
  bool opt_multi_range = true;
  bool return_siblings = false;
  CPLStringList siblings;
};

VSIRangeStatus MultiRangeHandle::GetRangeStatus(vsi_l_offset, vsi_l_offset) {
  return handler_->range_status;
}

bool MultiRangeHandle::HasPRead() const { return handler_->has_pread; }

int MultiRangeHandle::ReadMultiRange(int nRanges, void** ppData,
                                     const vsi_l_offset* panOffsets,
                                     const size_t* panSizes) {
  if (handler_->fail_multi_range) return -1;
  return VSIProxyFileHandle::ReadMultiRange(nRanges, ppData, panOffsets,
                                            panSizes);
}

MemFdHandler* g_memfd_handler = nullptr;
MultiRangeVsiMemHandler* g_mr_handler = nullptr;

class GTiffRasterBandReadTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    GDALAllRegister();
    g_memfd_handler = new MemFdHandler();
    VSIFileManager::InstallHandler("/vsimem_fd/", g_memfd_handler);
    g_mr_handler = new MultiRangeVsiMemHandler();
    VSIFileManager::InstallHandler("/vsimem_mr/", g_mr_handler);
  }

  static void TearDownTestSuite() {
    VSIFileManager::RemoveHandler("/vsimem_fd/");
    delete g_memfd_handler;
    g_memfd_handler = nullptr;
    VSIFileManager::RemoveHandler("/vsimem_mr/");
    delete g_mr_handler;
    g_mr_handler = nullptr;
    // Do not call CPLVirtualMemManagerTerminate() here: it writes a
    // CPLVirtualMemMsgToWorkerThread with uninitialized padding bytes to the
    // worker pipe, which MSan flags. The helper thread is torn down at exit.
  }

  void SetUp() override {
    g_mr_handler->range_status = VSI_RANGE_STATUS_DATA;
    g_mr_handler->has_pread = false;
    g_mr_handler->fail_multi_range = false;
    g_mr_handler->opt_multi_range = true;
    g_mr_handler->return_siblings = false;
    g_mr_handler->siblings.Clear();
  }
};

// Helper to create an in-memory .vat.dbf file with OGR ESRI Shapefile driver.
void CreateInMemVatDbf(const std::string& dbf_path) {
  GDALDriver* shp_drv =
      GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
  ASSERT_NE(shp_drv, nullptr);
  std::unique_ptr<GDALDataset> ds(
      shp_drv->Create(dbf_path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(ds, nullptr);
  OGRLayer* lyr = ds->CreateLayer("vat", nullptr, wkbNone, nullptr);
  ASSERT_NE(lyr, nullptr);
  OGRFieldDefn f_val("VALUE", OFTInteger);
  OGRFieldDefn f_cnt("COUNT", OFTInteger);
  OGRFieldDefn f_cls("CLASS_NAME", OFTString);
  f_cls.SetWidth(32);
  ASSERT_EQ(lyr->CreateField(&f_val), OGRERR_NONE);
  ASSERT_EQ(lyr->CreateField(&f_cnt), OGRERR_NONE);
  ASSERT_EQ(lyr->CreateField(&f_cls), OGRERR_NONE);

  OGRFeature feat(lyr->GetLayerDefn());
  feat.SetField("VALUE", 1);
  feat.SetField("COUNT", 10);
  feat.SetField("CLASS_NAME", "Water");
  ASSERT_EQ(lyr->CreateFeature(&feat), OGRERR_NONE);
  ds.reset();

  // Remove any .shp/.shx created alongside .dbf if needed, keeping .dbf intact.
  const std::string base = dbf_path.substr(0, dbf_path.size() - 4);
  VSIUnlink((base + ".shp").c_str());
  VSIUnlink((base + ".shx").c_str());
}

TEST_F(GTiffRasterBandReadTest, GetDefaultRATAndHistograms) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(drv, nullptr);

  // 1. Overview band (m_poBaseDS != nullptr) returns m_poRAT directly (null).
  {
    const char* path = "/vsimem/rb_rat_ovr.tif";
    absl::Cleanup clean = [&] { VSIUnlink(path); };
    std::unique_ptr<GDALDataset> ds(
        drv->Create(path, 16, 16, 1, GDT_Byte, nullptr));
    ASSERT_NE(ds, nullptr);
    EXPECT_EQ(ds->GetRasterBand(1)->Fill(5), CE_None);
    int ovr = 2;
    EXPECT_EQ(
        ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr, nullptr, nullptr),
        CE_None);
    GDALRasterBand* ovr_band = ds->GetRasterBand(1)->GetOverview(0);
    ASSERT_NE(ovr_band, nullptr);
    EXPECT_EQ(ovr_band->GetDefaultRAT(), nullptr);
  }

  // 2. RAT from PAM takes priority, and second call hits m_bRATSet cache.
  {
    const char* path = "/vsimem/rb_rat_pam.tif";
    absl::Cleanup clean = [&] {
      VSIUnlink(path);
      VSIUnlink("/vsimem/rb_rat_pam.tif.aux.xml");
    };
    CPLConfigOptionSetter pam_on("GDAL_PAM_ENABLED", "YES", false);
    CPLConfigOptionSetter rat_pam("GTIFF_WRITE_RAT_TO_PAM", "YES", false);
    {
      CPLStringList co;
      co.SetNameValue("PROFILE", "BASELINE");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(path, 8, 8, 1, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
      GDALDefaultRasterAttributeTable rat;
      rat.CreateColumn("VALUE", GFT_Integer, GFU_MinMax);
      rat.SetValue(0, 0, 42);
      EXPECT_EQ(ds->GetRasterBand(1)->SetDefaultRAT(&rat), CE_None);
      GUIntBig hist[4] = {0};
      EXPECT_EQ(ds->GetRasterBand(1)->GetHistogram(-0.5, 255.5, 4, hist, TRUE,
                                                   FALSE, nullptr, nullptr),
                CE_None);
      EXPECT_EQ(ds->GetRasterBand(1)->SetDefaultHistogram(0.0, 255.0, 4, hist),
                CE_None);
    }
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(path, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    GDALRasterAttributeTable* rat1 = ds->GetRasterBand(1)->GetDefaultRAT();
    ASSERT_NE(rat1, nullptr);
    EXPECT_EQ(rat1->GetValueAsInt(0, 0), 42);
    // Call again after clearing PAM flag path: hits `if (m_bRATSet)`.
    EXPECT_EQ(ds->GetRasterBand(1)->GetDefaultRAT(), rat1);

    // GetHistogram & GetDefaultHistogram.
    GUIntBig hist_out[4] = {0};
    EXPECT_EQ(ds->GetRasterBand(1)->GetHistogram(-0.5, 255.5, 4, hist_out, TRUE,
                                                 FALSE, nullptr, nullptr),
              CE_None);
    double dfMin = 0.0, dfMax = 0.0;
    int nBuckets = 0;
    GUIntBig* panHist = nullptr;
    EXPECT_EQ(ds->GetRasterBand(1)->GetDefaultHistogram(
                  &dfMin, &dfMax, &nBuckets, &panHist, FALSE, nullptr, nullptr),
              CE_None);
    VSIFree(panHist);
  }

  // 3. /vsisubfile/ rejects sidecar file (!GDALCanFileAcceptSidecarFile).
  {
    const char* path = "/vsimem/rb_rat_sub.tif";
    absl::Cleanup clean = [&] { VSIUnlink(path); };
    {
      std::unique_ptr<GDALDataset> ds(
          drv->Create(path, 8, 8, 1, GDT_Byte, nullptr));
      ASSERT_NE(ds, nullptr);
    }
    VSIStatBufL st{};
    ASSERT_EQ(VSIStatL(path, &st), 0);
    std::string sub =
        CPLSPrintf("/vsisubfile/0_%llu,%s",
                   static_cast<unsigned long long>(st.st_size), path);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(sub.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    EXPECT_EQ(ds->GetRasterBand(1)->GetDefaultRAT(), nullptr);
    // Second call hits m_bRATSet == true.
    EXPECT_EQ(ds->GetRasterBand(1)->GetDefaultRAT(), nullptr);
  }

  // 4. Sidecar .vat.dbf via SiblingFiles list (both present & absent) and
  // via VSIStatL fallback (GDAL_DISABLE_READDIR_ON_OPEN=YES).
  {
    const char* mr_path = "/vsimem_mr/vat_sib.tif";
    const std::string backing_tif = MultiRangeVsiMemHandler::ToMem(mr_path);
    const std::string backing_dbf = backing_tif + ".vat.dbf";
    absl::Cleanup clean = [&] {
      VSIUnlink(backing_tif.c_str());
      VSIUnlink(backing_dbf.c_str());
    };
    {
      std::unique_ptr<GDALDataset> ds(
          drv->Create(mr_path, 8, 8, 1, GDT_Byte, nullptr));
      ASSERT_NE(ds, nullptr);
    }
    CreateInMemVatDbf(backing_dbf);

    // 4a. SiblingFiles returns matching "vat_sib.tif.vat.dbf".
    g_mr_handler->return_siblings = true;
    g_mr_handler->siblings.AddString("vat_sib.tif.vat.dbf");
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(mr_path, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      GDALRasterAttributeTable* rat = ds->GetRasterBand(1)->GetDefaultRAT();
      ASSERT_NE(rat, nullptr);
      EXPECT_EQ(rat->GetRowCount(), 1);
    }

    // 4b. SiblingFiles returns list WITHOUT .vat.dbf -> returns nullptr.
    g_mr_handler->siblings.Clear();
    g_mr_handler->siblings.AddString("other_file.txt");
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(mr_path, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetRasterBand(1)->GetDefaultRAT(), nullptr);
    }

    // 4c. VSIStatL fallback when GDAL_DISABLE_READDIR_ON_OPEN=YES.
    g_mr_handler->return_siblings = false;
    {
      CPLConfigOptionSetter no_readdir("GDAL_DISABLE_READDIR_ON_OPEN", "YES",
                                       false);
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(mr_path, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      GDALRasterAttributeTable* rat = ds->GetRasterBand(1)->GetDefaultRAT();
      ASSERT_NE(rat, nullptr);
      EXPECT_EQ(rat->GetRowCount(), 1);
    }
  }
}

TEST_F(GTiffRasterBandReadTest, DirectIOAllBranches) {
  CPLConfigOptionSetter direct_io("GTIFF_DIRECT_IO", "YES", false);
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(drv, nullptr);

  // 1. Tiled uncompressed CONTIG in GA_Update -> exercises FlushCache +
  // CommonDirectIOClassic via single-band RasterIO!
  {
    const char* path = "/vsimem/rb_dio_tiled.tif";
    absl::Cleanup clean = [&] { VSIUnlink(path); };
    CPLStringList co;
    co.SetNameValue("TILED", "YES");
    co.SetNameValue("BLOCKXSIZE", "16");
    co.SetNameValue("BLOCKYSIZE", "16");
    std::unique_ptr<GDALDataset> ds(
        drv->Create(path, 32, 32, 3, GDT_Byte, co.List()));
    ASSERT_NE(ds, nullptr);
    EXPECT_EQ(ds->GetRasterBand(1)->Fill(11), CE_None);
    EXPECT_EQ(ds->GetRasterBand(2)->Fill(22), CE_None);
    EXPECT_EQ(ds->GetRasterBand(3)->Fill(33), CE_None);

    // Single-band RasterIO calls GTiffRasterBand::IRasterIO -> DirectIO!
    std::vector<GByte> buf(16 * 16, 0);
    EXPECT_EQ(ds->GetRasterBand(2)->RasterIO(GF_Read, 0, 0, 16, 16, buf.data(),
                                             16, 16, GDT_Byte, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(buf[0], 22);

    // Non-nearest resampling falls back (DirectIO returns -1 -> block cache).
    GDALRasterIOExtraArg extra;
    INIT_RASTERIO_EXTRA_ARG(extra);
    extra.eResampleAlg = GRIORA_Bilinear;
    std::vector<GByte> sub(8 * 8, 0);
    EXPECT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 16, 16, sub.data(),
                                             8, 8, GDT_Byte, 0, 0, &extra),
              CE_None);
    EXPECT_EQ(sub[0], 11);
  }

  // 2. Compressed TIFF -> DirectIO returns -1 (unsupported) and falls back.
  {
    const char* path = "/vsimem/rb_dio_lzw.tif";
    absl::Cleanup clean = [&] { VSIUnlink(path); };
    CPLStringList co;
    co.SetNameValue("COMPRESS", "LZW");
    std::unique_ptr<GDALDataset> ds(
        drv->Create(path, 16, 16, 1, GDT_Byte, co.List()));
    ASSERT_NE(ds, nullptr);
    EXPECT_EQ(ds->GetRasterBand(1)->Fill(77), CE_None);
    ds->FlushCache(false);
    GByte v = 0;
    EXPECT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &v, 1, 1,
                                             GDT_Byte, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(v, 77);
  }

  // 3. Stripped uncompressed single-band Byte:
  // - exact read without pTmpBuffer (ppData points directly into pData)
  // - Y subsampling + X subsampling (bOneByteCopy == true)
  // - Y oversampling + X oversampling + type conversion (bOneByteCopy == false)
  {
    const char* path = "/vsimem/rb_dio_strip1.tif";
    absl::Cleanup clean = [&] { VSIUnlink(path); };
    CPLStringList co;
    co.SetNameValue("BLOCKYSIZE", "4");
    std::unique_ptr<GDALDataset> ds(
        drv->Create(path, 16, 16, 1, GDT_Byte, co.List()));
    ASSERT_NE(ds, nullptr);
    std::vector<GByte> src(16 * 16);
    for (int i = 0; i < 256; ++i) src[i] = static_cast<GByte>(i);
    EXPECT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 16, 16, src.data(),
                                             16, 16, GDT_Byte, 0, 0, nullptr),
              CE_None);
    ds.reset();

    ds.reset(GDALDataset::Open(path, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    // Exact 1-band Byte read -> pTmpBuffer == nullptr!
    std::vector<GByte> exact(8 * 8, 0);
    EXPECT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Read, 2, 2, 8, 8, exact.data(),
                                             8, 8, GDT_Byte, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(exact[0], src[2 * 16 + 2]);

    // Subsampling in X & Y with GDT_Byte (bOneByteCopy == true).
    std::vector<GByte> sub(4 * 4, 0);
    EXPECT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 16, 16, sub.data(),
                                             4, 4, GDT_Byte, 0, 0, nullptr),
              CE_None);
    // Nearest neighbour picks source pixel floor((i + 0.5) * 4), i.e. 2 for
    // i == 0.
    EXPECT_EQ(sub[0], src[2 * 16 + 2]);

    // Oversampling in X & Y with GDT_UInt16 (bOneByteCopy == false).
    std::vector<uint16_t> over(8 * 8, 0);
    EXPECT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 4, 4, over.data(),
                                             8, 8, GDT_UInt16, 0, 0, nullptr),
              CE_None);
    // Output pixels 0 and 1 both map to source pixel 0, pixel 2 maps to 1.
    EXPECT_EQ(over[0], src[0]);
    EXPECT_EQ(over[1], src[0]);
    EXPECT_EQ(over[2], src[1]);

    // Same XSize, type conversion (nBufXSize == nXSize in pTmpBuffer loop).
    std::vector<uint16_t> conv(16 * 4, 0);
    EXPECT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 16, 4, conv.data(),
                                             16, 4, GDT_UInt16, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(conv[5], 5);
  }

  // 4. Stripped multi-band CONTIG + big-endian (ENDIANNESS=INVERTED):
  // both non-complex (UInt16) and complex (CInt16) byte-swapping!
  {
    const char* path_u16 = "/vsimem/rb_dio_be_u16.tif";
    const char* path_c16 = "/vsimem/rb_dio_be_c16.tif";
    absl::Cleanup clean = [&] {
      VSIUnlink(path_u16);
      VSIUnlink(path_c16);
    };
    CPLStringList co;
    co.SetNameValue("ENDIANNESS", "INVERTED");
    co.SetNameValue("BLOCKYSIZE", "4");
    {
      std::unique_ptr<GDALDataset> ds(
          drv->Create(path_u16, 8, 8, 2, GDT_UInt16, co.List()));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetRasterBand(1)->Fill(1000), CE_None);
      EXPECT_EQ(ds->GetRasterBand(2)->Fill(2000), CE_None);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(path_u16, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      std::vector<uint16_t> out(8 * 8, 0);
      EXPECT_EQ(ds->GetRasterBand(2)->RasterIO(GF_Read, 0, 0, 8, 8, out.data(),
                                               8, 8, GDT_UInt16, 0, 0, nullptr),
                CE_None);
      EXPECT_EQ(out[0], 2000);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          drv->Create(path_c16, 8, 8, 1, GDT_CInt16, co.List()));
      ASSERT_NE(ds, nullptr);
      std::vector<int16_t> cdata(8 * 8 * 2, 0);
      for (int i = 0; i < 64; ++i) {
        cdata[2 * i] = 300;
        cdata[2 * i + 1] = -400;
      }
      EXPECT_EQ(
          ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 8, 8, cdata.data(), 8,
                                         8, GDT_CInt16, 0, 0, nullptr),
          CE_None);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(path_c16, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      std::vector<int16_t> cout(8 * 8 * 2, 0);
      EXPECT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 8, 8, cout.data(),
                                               8, 8, GDT_CInt16, 0, 0, nullptr),
                CE_None);
      EXPECT_EQ(cout[0], 300);
      EXPECT_EQ(cout[1], -400);
    }
  }

  // 5. Sparse stripped file (panOffsets[iLine] == 0 -> DirectIO returns -1) and
  // ReadMultiRange failure (returns CE_Failure).
  {
    const char* sparse_path = "/vsimem/rb_dio_sparse.tif";
    const char* fail_mr_path = "/vsimem_mr/rb_dio_failmr.tif";
    const std::string backing = MultiRangeVsiMemHandler::ToMem(fail_mr_path);
    absl::Cleanup clean = [&] {
      VSIUnlink(sparse_path);
      VSIUnlink(backing.c_str());
    };
    {
      CPLStringList co;
      co.SetNameValue("SPARSE_OK", "YES");
      co.SetNameValue("BLOCKYSIZE", "4");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(sparse_path, 8, 8, 1, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(sparse_path, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      GByte v = 99;
      EXPECT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &v, 1, 1,
                                               GDT_Byte, 0, 0, nullptr),
                CE_None);
      EXPECT_EQ(v, 0);
    }

    {
      CPLStringList co;
      co.SetNameValue("BLOCKYSIZE", "4");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(fail_mr_path, 8, 8, 1, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetRasterBand(1)->Fill(55), CE_None);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(fail_mr_path, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      g_mr_handler->fail_multi_range = true;
      CPLPushErrorHandler(CPLQuietErrorHandler);
      GByte v = 0;
      EXPECT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, &v, 1, 1,
                                               GDT_Byte, 0, 0, nullptr),
                CE_Failure);
      CPLPopErrorHandler();
    }
  }
}

TEST_F(GTiffRasterBandReadTest, GetVirtualMemAutoAllBranches) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(drv, nullptr);

  // 1. CONTIG stripped file on /vsimem_fd/:
  // - Created with SPARSE_OK=YES (all zero offsets) and opened in GA_Update ->
  //   exercises zero-strip initialization + ftruncate + base mapping + derived
  //   mapping for band 1 & band 2 + DropReferenceVirtualMem (both before and
  //   after dataset destruction!).
  {
    const char* path = "/vsimem_fd/vmem_contig_sparse.tif";
    absl::Cleanup clean = [&] { VSIUnlink(path); };
    {
      CPLStringList co;
      co.SetNameValue("SPARSE_OK", "YES");
      co.SetNameValue("BLOCKYSIZE", "4");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(path, 16, 16, 2, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
    }

    // First open RO on sparse file -> "Sparse files not supported in file
    // mapping" + USE_DEFAULT_IMPLEMENTATION=NO returns nullptr.
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(path, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      int px = 0;
      GIntBig ln = 0;
      CPLStringList opts;
      opts.SetNameValue("USE_DEFAULT_IMPLEMENTATION", "NO");
      EXPECT_EQ(ds->GetRasterBand(1)->GetVirtualMemAuto(GF_Read, &px, &ln,
                                                        opts.List()),
                nullptr);
    }

    // Now open GA_Update -> initializes all strips via ftruncate and maps RW!
    CPLVirtualMem* vmem_outlive = nullptr;
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(path, GDAL_OF_RASTER | GDAL_OF_UPDATE));
      ASSERT_NE(ds, nullptr);
      int px1 = 0, px2 = 0;
      GIntBig ln1 = 0, ln2 = 0;
      CPLVirtualMem* vm1 = ds->GetRasterBand(1)->GetVirtualMemAuto(
          GF_Write, &px1, &ln1, nullptr);
      ASSERT_NE(vm1, nullptr);
      EXPECT_EQ(px1, 2);
      EXPECT_EQ(ln1, 32);

      // Band 2 reuses m_pBaseMapping!
      vmem_outlive = ds->GetRasterBand(2)->GetVirtualMemAuto(GF_Write, &px2,
                                                             &ln2, nullptr);
      ASSERT_NE(vmem_outlive, nullptr);
      EXPECT_EQ(px2, 2);

      auto* ptr1 = static_cast<GByte*>(CPLVirtualMemGetAddr(vm1));
      auto* ptr2 = static_cast<GByte*>(CPLVirtualMemGetAddr(vmem_outlive));
      ptr1[0] = 123;
      ptr2[0] = 234;

      // Free vm1 while dataset is STILL ALIVE -> DropReferenceVirtualMem
      // with *ppoSelf != nullptr!
      CPLVirtualMemFree(vm1);
    }
    // Free vmem_outlive AFTER dataset was destroyed -> DropReferenceVirtualMem
    // with *ppoSelf == nullptr!
    CPLVirtualMemFree(vmem_outlive);
  }

  // 2. SEPARATE stripped file on /vsimem_fd/ with multiple strips per band,
  // plus USE_DEFAULT_IMPLEMENTATION=YES and fallback on /vsimem/ (no fd).
  {
    const char* path = "/vsimem_fd/vmem_separate.tif";
    absl::Cleanup clean = [&] { VSIUnlink(path); };
    {
      CPLStringList co;
      co.SetNameValue("INTERLEAVE", "BAND");
      co.SetNameValue("BLOCKYSIZE", "4");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(path, 16, 16, 2, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetRasterBand(1)->Fill(42), CE_None);
      EXPECT_EQ(ds->GetRasterBand(2)->Fill(84), CE_None);
    }
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(path, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    int px = 0;
    GIntBig ln = 0;
    CPLVirtualMem* vm =
        ds->GetRasterBand(2)->GetVirtualMemAuto(GF_Read, &px, &ln, nullptr);
    ASSERT_NE(vm, nullptr);
    EXPECT_EQ(px, 1);
    EXPECT_EQ(ln, 16);
    EXPECT_EQ(*static_cast<GByte*>(CPLVirtualMemGetAddr(vm)), 84);
    CPLVirtualMemFree(vm);

    // USE_DEFAULT_IMPLEMENTATION=YES.
    CPLStringList opts_yes;
    opts_yes.SetNameValue("USE_DEFAULT_IMPLEMENTATION", "YES");
    CPLVirtualMem* vm_def = ds->GetRasterBand(1)->GetVirtualMemAuto(
        GF_Read, &px, &ln, opts_yes.List());
    ASSERT_NE(vm_def, nullptr);
    CPLVirtualMemFree(vm_def);
  }

  // 3. Incompatible spacing on /vsimem_fd/: strip 0 written, strip 1 missing
  // (panTIFFOffsets[1] == 0), and reversed strip offsets (strip 0 written after
  // strip 1) -> bCompatibleSpacing == false -> falls back to base impl!
  {
    const char* path_hole = "/vsimem_fd/vmem_hole.tif";
    const char* path_rev = "/vsimem_fd/vmem_rev.tif";
    absl::Cleanup clean = [&] {
      VSIUnlink(path_hole);
      VSIUnlink(path_rev);
    };
    {
      CPLStringList co;
      co.SetNameValue("SPARSE_OK", "YES");
      co.SetNameValue("BLOCKYSIZE", "4");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(path_hole, 16, 16, 1, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
      std::vector<GByte> blk(16 * 4, 7);
      EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, blk.data()), CE_None);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(path_hole, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      int px = 0;
      GIntBig ln = 0;
      CPLStringList opts_no;
      opts_no.SetNameValue("USE_DEFAULT_IMPLEMENTATION", "NO");
      EXPECT_EQ(ds->GetRasterBand(1)->GetVirtualMemAuto(GF_Read, &px, &ln,
                                                        opts_no.List()),
                nullptr);
      // AUTO falls back to GDALRasterBand::GetVirtualMemAuto.
      CPLVirtualMem* vm_fb =
          ds->GetRasterBand(1)->GetVirtualMemAuto(GF_Read, &px, &ln, nullptr);
      ASSERT_NE(vm_fb, nullptr);
      CPLVirtualMemFree(vm_fb);
    }

    // Out-of-order strip offsets: write strip 2 first, then strip 0, then 1, 3.
    {
      CPLStringList co;
      co.SetNameValue("SPARSE_OK", "YES");
      co.SetNameValue("BLOCKYSIZE", "4");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(path_rev, 16, 16, 1, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
      std::vector<GByte> blk(16 * 4, 9);
      EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 2, blk.data()), CE_None);
      ds->FlushCache(false);
      EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, blk.data()), CE_None);
      ds->FlushCache(false);
      EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 1, blk.data()), CE_None);
      ds->FlushCache(false);
      EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 3, blk.data()), CE_None);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(path_rev, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      int px = 0;
      GIntBig ln = 0;
      CPLStringList opts_no;
      opts_no.SetNameValue("USE_DEFAULT_IMPLEMENTATION", "OFF");
      EXPECT_EQ(ds->GetRasterBand(1)->GetVirtualMemAuto(GF_Read, &px, &ln,
                                                        opts_no.List()),
                nullptr);
    }
  }
}

TEST_F(GTiffRasterBandReadTest, IGetDataCoverageStatusAllBranches) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(drv, nullptr);

  // 1. SEPARATE tiled sparse TIFF on /vsimem_mr/:
  // - GA_Update FlushCache branch
  // - Empty tile + non-empty tile
  // - GetRangeStatus returning HOLE vs DATA
  // - nMaskFlagStop early exit setting *pdfDataPct = -1.0
  // - Int64 / UInt64 / Double NoData == 0 vs != 0 branches
  {
    const char* mr_path = "/vsimem_mr/cov_status.tif";
    const std::string backing = MultiRangeVsiMemHandler::ToMem(mr_path);
    absl::Cleanup clean = [&] { VSIUnlink(backing.c_str()); };

    CPLStringList co;
    co.SetNameValue("TILED", "YES");
    co.SetNameValue("BLOCKXSIZE", "16");
    co.SetNameValue("BLOCKYSIZE", "16");
    co.SetNameValue("INTERLEAVE", "BAND");
    co.SetNameValue("SPARSE_OK", "YES");
    std::unique_ptr<GDALDataset> ds(
        drv->Create(mr_path, 32, 32, 2, GDT_Byte, co.List()));
    ASSERT_NE(ds, nullptr);
    std::vector<GByte> tile(16 * 16, 42);
    EXPECT_EQ(ds->GetRasterBand(2)->WriteBlock(0, 0, tile.data()), CE_None);

    // Query in GA_Update mode -> flushes cache; 1 of 4 tiles has data (25%).
    double pct = 0.0;
    int st = ds->GetRasterBand(2)->GetDataCoverageStatus(0, 0, 32, 32, 0, &pct);
    EXPECT_EQ(st,
              GDAL_DATA_COVERAGE_STATUS_DATA | GDAL_DATA_COVERAGE_STATUS_EMPTY);
    EXPECT_DOUBLE_EQ(pct, 25.0);

    // Early stop via nMaskFlagStop -> pct = -1.0.
    st = ds->GetRasterBand(2)->GetDataCoverageStatus(
        0, 0, 32, 32, GDAL_DATA_COVERAGE_STATUS_DATA, &pct);
    EXPECT_NE(st & GDAL_DATA_COVERAGE_STATUS_DATA, 0);
    EXPECT_DOUBLE_EQ(pct, -1.0);
    ds.reset();

    // Reopen RO with range_status = HOLE -> allocated uncompressed block
    // without nonzero NoData is reported as EMPTY!
    g_mr_handler->range_status = VSI_RANGE_STATUS_HOLE;
    ds.reset(GDALDataset::Open(mr_path, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    st = ds->GetRasterBand(2)->GetDataCoverageStatus(0, 0, 16, 16, 0, &pct);
    EXPECT_EQ(st, GDAL_DATA_COVERAGE_STATUS_EMPTY);
    EXPECT_DOUBLE_EQ(pct, 0.0);

    // With range_status = DATA -> reported as DATA (100%).
    g_mr_handler->range_status = VSI_RANGE_STATUS_DATA;
    st = ds->GetRasterBand(2)->GetDataCoverageStatus(0, 0, 16, 16, 0, &pct);
    EXPECT_EQ(st, GDAL_DATA_COVERAGE_STATUS_DATA);
    EXPECT_DOUBLE_EQ(pct, 100.0);
  }

  // 2. NoData = 0 in Int64 and UInt64 RO datasets.
  for (GDALDataType dt : {GDT_Int64, GDT_UInt64}) {
    const char* mr_path =
        dt == GDT_Int64 ? "/vsimem_mr/cov_i64.tif" : "/vsimem_mr/cov_u64.tif";
    const std::string backing = MultiRangeVsiMemHandler::ToMem(mr_path);
    absl::Cleanup clean = [&] { VSIUnlink(backing.c_str()); };
    {
      std::unique_ptr<GDALDataset> ds(
          drv->Create(mr_path, 8, 8, 1, dt, nullptr));
      ASSERT_NE(ds, nullptr);
      if (dt == GDT_Int64) {
        EXPECT_EQ(ds->GetRasterBand(1)->SetNoDataValueAsInt64(0), CE_None);
      } else {
        EXPECT_EQ(ds->GetRasterBand(1)->SetNoDataValueAsUInt64(0), CE_None);
      }
      EXPECT_EQ(ds->GetRasterBand(1)->Fill(1), CE_None);
    }
    g_mr_handler->range_status = VSI_RANGE_STATUS_DATA;
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(mr_path, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    double pct = 0.0;
    EXPECT_EQ(ds->GetRasterBand(1)->GetDataCoverageStatus(0, 0, 8, 8, 0, &pct),
              GDAL_DATA_COVERAGE_STATUS_DATA);
    EXPECT_DOUBLE_EQ(pct, 100.0);
  }
}

TEST_F(GTiffRasterBandReadTest, IReadBlockFillCacheAndInterleavedMask) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(drv, nullptr);

  // 1. Partial bottom strip & partial bottom tile, SPARSE_OK missing block,
  // 3-band RO GDALDeinterleave fast path, 2-band CONTIG GDALCopyWords64 +
  // FillCacheForOtherBands, and corrupt strile error paths (both SEPARATE and
  // CONTIG).
  {
    // 1a. 3-band RO CONTIG with partial bottom strip (height 10, rowsperstrip
    // 4)
    const char* p3 = "/vsimem/rb_read_3band.tif";
    absl::Cleanup clean3 = [&] { VSIUnlink(p3); };
    {
      CPLStringList co;
      co.SetNameValue("BLOCKYSIZE", "4");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p3, 8, 10, 3, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetRasterBand(1)->Fill(10), CE_None);
      EXPECT_EQ(ds->GetRasterBand(2)->Fill(20), CE_None);
      EXPECT_EQ(ds->GetRasterBand(3)->Fill(30), CE_None);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p3, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      std::vector<GByte> blk(8 * 4, 0);
      // Reading band 1 first triggers 3-band GDALDeinterleave fast path!
      EXPECT_EQ(ds->GetRasterBand(1)->ReadBlock(0, 2, blk.data()), CE_None);
      EXPECT_EQ(blk[0], 10);
      // Reading band 2 first on block 0 triggers GDALCopyWords64 +
      // FillCacheForOtherBands!
      EXPECT_EQ(ds->GetRasterBand(2)->ReadBlock(0, 0, blk.data()), CE_None);
      EXPECT_EQ(blk[0], 20);
    }
  }

  // 1b. Sparse missing block + corrupt LZW block in SEPARATE and CONTIG.
  for (bool separate : {true, false}) {
    const char* path =
        separate ? "/vsimem/rb_corrupt_sep.tif" : "/vsimem/rb_corrupt_ctg.tif";
    absl::Cleanup clean = [&] { VSIUnlink(path); };
    {
      CPLStringList co;
      co.SetNameValue("TILED", "YES");
      co.SetNameValue("BLOCKXSIZE", "16");
      co.SetNameValue("BLOCKYSIZE", "16");
      co.SetNameValue("COMPRESS", "LZW");
      co.SetNameValue("SPARSE_OK", "YES");
      co.SetNameValue("INTERLEAVE", separate ? "BAND" : "PIXEL");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(path, 32, 20, 2, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
      std::vector<GByte> tile(16 * 16, 77);
      // Write tile (0,0), leave tile (1,0) sparse!
      EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, tile.data()), CE_None);
      EXPECT_EQ(ds->GetRasterBand(2)->WriteBlock(0, 0, tile.data()), CE_None);
    }
    // Corrupt the LZW payload of tile (0,0) in-memory.
    vsi_l_offset off0 = 0;
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(path, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      const char* s_off =
          ds->GetRasterBand(1)->GetMetadataItem("BLOCK_OFFSET_0_0", "TIFF");
      ASSERT_NE(s_off, nullptr);
      off0 = static_cast<vsi_l_offset>(std::strtoull(s_off, nullptr, 10));
    }
    vsi_l_offset len = 0;
    GByte* raw = VSIGetMemFileBuffer(path, &len, FALSE);
    ASSERT_NE(raw, nullptr);
    ASSERT_LT(off0 + 4, len);
    memset(raw + off0, 0xFF, 4);

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(path, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    std::vector<GByte> buf(16 * 16, 5);
    // Sparse tile (1,0) -> NullBlock -> 0, CE_None!
    EXPECT_EQ(ds->GetRasterBand(1)->ReadBlock(1, 0, buf.data()), CE_None);
    EXPECT_EQ(buf[0], 0);
    // Corrupt tile (0,0) -> CE_Failure!
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(ds->GetRasterBand(1)->ReadBlock(0, 0, buf.data()), CE_Failure);
    CPLPopErrorHandler();
  }

  // 2. COG with internal mask on /vsimem_mr/ -> IRasterIO invokes
  // CacheMultiRange -> IReadBlock invokes CacheMaskForBlock and preloads mask!
  {
    const char* src_path = "/vsimem/rb_cog_mask_src.tif";
    const char* cog_path = "/vsimem_mr/rb_cog_mask.tif";
    const std::string backing = MultiRangeVsiMemHandler::ToMem(cog_path);
    absl::Cleanup clean = [&] {
      VSIUnlink(src_path);
      VSIUnlink(backing.c_str());
    };
    {
      CPLConfigOptionSetter int_msk("GDAL_TIFF_INTERNAL_MASK", "YES", false);
      std::unique_ptr<GDALDataset> src(
          drv->Create(src_path, 64, 64, 3, GDT_Byte, nullptr));
      ASSERT_NE(src, nullptr);
      EXPECT_EQ(src->GetRasterBand(1)->Fill(100), CE_None);
      EXPECT_EQ(src->GetRasterBand(2)->Fill(150), CE_None);
      EXPECT_EQ(src->GetRasterBand(3)->Fill(200), CE_None);
      EXPECT_EQ(src->CreateMaskBand(GMF_PER_DATASET), CE_None);
      EXPECT_EQ(src->GetRasterBand(1)->GetMaskBand()->Fill(255), CE_None);

      GDALDriver* cog_drv = GetGDALDriverManager()->GetDriverByName("COG");
      ASSERT_NE(cog_drv, nullptr);
      CPLStringList cog_co;
      cog_co.SetNameValue("BLOCKSIZE", "32");
      cog_co.SetNameValue("COMPRESS", "LZW");
      std::unique_ptr<GDALDataset> cog(cog_drv->CreateCopy(
          cog_path, src.get(), FALSE, cog_co.List(), nullptr, nullptr));
      ASSERT_NE(cog, nullptr);
    }

    g_mr_handler->opt_multi_range = true;
    g_mr_handler->has_pread = false;
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(cog_path, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    std::vector<GByte> out(64 * 64 * 3, 0);
    EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 64, 64, out.data(), 64, 64, GDT_Byte,
                           3, nullptr, 0, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(out[0], 100);
  }

  // 3. Streaming backward read rejection (m_bStreamingIn == true).
  {
    const char* cog_stream = "/vsimem/rb_stream_cog.tif";
    absl::Cleanup clean = [&] { VSIUnlink(cog_stream); };
    {
      CPLStringList co;
      co.SetNameValue("STREAMABLE_OUTPUT", "YES");
      co.SetNameValue("BLOCKYSIZE", "4");
      std::unique_ptr<GDALDataset> src(
          drv->Create(cog_stream, 8, 16, 1, GDT_Byte, co.List()));
      ASSERT_NE(src, nullptr);
      std::vector<GByte> strip(8 * 4, 88);
      for (int y_blk = 0; y_blk < 4; ++y_blk) {
        EXPECT_EQ(src->GetRasterBand(1)->WriteBlock(0, y_blk, strip.data()),
                  CE_None);
      }
      EXPECT_EQ(src->Close(), CE_None);
    }

    CPLConfigOptionSetter stream_on("TIFF_READ_STREAMING", "YES", false);
    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(cog_stream, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    std::vector<GByte> blk(8 * 4, 0);
    // Read later strip (0, 2) first so m_fpL advances past strip 0...
    EXPECT_EQ(ds->GetRasterBand(1)->ReadBlock(0, 2, blk.data()), CE_None);
    EXPECT_EQ(blk[0], 88);
    // Now read earlier strip (0, 0) -> backward read not supported!
    CPLPushErrorHandler(CPLQuietErrorHandler);
    EXPECT_EQ(ds->GetRasterBand(1)->ReadBlock(0, 0, blk.data()), CE_Failure);
    CPLPopErrorHandler();
  }
}

TEST_F(GTiffRasterBandReadTest, MetadataPropertiesNoDataOverviewsAndMasks) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(drv, nullptr);

  // 1. Description, Offset, Scale, UnitType (including 3D vertical SRS unit
  // fallback!), ColorInterp, ColorTable (band 1 vs band 2), Metadata domains,
  // JPEGTABLES, IFD_OFFSET, BLOCK_OFFSET/BLOCK_SIZE, _DEBUG_ HAS_BLOCK_CACHE,
  // and signed GDT_Byte legacy PIXELTYPE.
  {
    const char* jpeg_path = "/vsimem/rb_meta_jpeg.tif";
    absl::Cleanup clean = [&] { VSIUnlink(jpeg_path); };
    {
      CPLStringList co;
      co.SetNameValue("TILED", "YES");
      co.SetNameValue("BLOCKXSIZE", "16");
      co.SetNameValue("BLOCKYSIZE", "16");
      co.SetNameValue("COMPRESS", "JPEG");
      co.SetNameValue("INTERLEAVE", "BAND");
      co.SetNameValue("SPARSE_OK", "YES");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(jpeg_path, 32, 32, 2, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
      ds->GetRasterBand(1)->SetDescription("BandOne");
      EXPECT_EQ(ds->GetRasterBand(1)->SetOffset(1.5), CE_None);
      EXPECT_EQ(ds->GetRasterBand(1)->SetScale(2.5), CE_None);
      EXPECT_EQ(ds->GetRasterBand(2)->SetUnitType("W/m2"), CE_None);

      // Compound SRS with vertical units so band 1 (empty unit) falls back to
      // m_pszVertUnit!
      OGRSpatialReference comp;
      OGRSpatialReference horiz, vert;
      ASSERT_EQ(horiz.importFromEPSG(4326), OGRERR_NONE);
      ASSERT_EQ(vert.importFromEPSG(5703), OGRERR_NONE);  // NAVD88 height (m)
      ASSERT_EQ(comp.SetCompoundCS("WGS84+NAVD88", &horiz, &vert), OGRERR_NONE);
      EXPECT_EQ(ds->SetSpatialRef(&comp), CE_None);

      std::vector<GByte> tile(16 * 16, 128);
      EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, tile.data()), CE_None);
      EXPECT_EQ(ds->GetRasterBand(2)->WriteBlock(0, 0, tile.data()), CE_None);
    }

    std::unique_ptr<GDALDataset> ds(
        GDALDataset::Open(jpeg_path, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    GDALRasterBand* b1 = ds->GetRasterBand(1);
    GDALRasterBand* b2 = ds->GetRasterBand(2);

    EXPECT_STREQ(b1->GetDescription(), "BandOne");
    int ok = 0;
    EXPECT_DOUBLE_EQ(b1->GetOffset(&ok), 1.5);
    EXPECT_EQ(ok, TRUE);
    EXPECT_DOUBLE_EQ(b1->GetScale(&ok), 2.5);
    EXPECT_EQ(ok, TRUE);
    EXPECT_STREQ(b1->GetUnitType(), "metre");
    EXPECT_STREQ(b2->GetUnitType(), "W/m2");
    EXPECT_EQ(b1->GetColorInterpretation(), GCI_GrayIndex);
    EXPECT_EQ(b1->GetColorTable(), nullptr);
    EXPECT_EQ(b2->GetColorTable(), nullptr);

    char** domains = b1->GetMetadataDomainList();
    CSLDestroy(domains);
    (void)b1->GetMetadata("IMAGE_STRUCTURE");
    (void)b1->GetMetadata(nullptr);

    EXPECT_NE(b1->GetMetadataItem("JPEGTABLES", "TIFF"), nullptr);
    EXPECT_NE(b1->GetMetadataItem("IFD_OFFSET", "TIFF"), nullptr);
    EXPECT_NE(b2->GetMetadataItem("BLOCK_OFFSET_0_0", "TIFF"), nullptr);
    EXPECT_NE(b2->GetMetadataItem("BLOCK_SIZE_0_0", "TIFF"), nullptr);
    // Sparse tile (1, 0) -> nullptr.
    EXPECT_EQ(b1->GetMetadataItem("BLOCK_OFFSET_1_0", "TIFF"), nullptr);
    EXPECT_EQ(b1->GetMetadataItem("BLOCK_SIZE_1_0", "TIFF"), nullptr);
    // Out-of-bounds block coordinates -> nullptr.
    EXPECT_EQ(b1->GetMetadataItem("BLOCK_OFFSET_-1_0", "TIFF"), nullptr);
    EXPECT_EQ(b1->GetMetadataItem("BLOCK_OFFSET_99_0", "TIFF"), nullptr);
    EXPECT_EQ(b1->GetMetadataItem("BLOCK_SIZE_0_-1", "TIFF"), nullptr);
    EXPECT_EQ(b1->GetMetadataItem("BLOCK_SIZE_0_99", "TIFF"), nullptr);

    // _DEBUG_ HAS_BLOCK_CACHE before and after reading a block.
    EXPECT_STREQ(b1->GetMetadataItem("HAS_BLOCK_CACHE", "_DEBUG_"), "0");
    std::vector<GByte> tmp(16 * 16);
    EXPECT_EQ(b1->ReadBlock(0, 0, tmp.data()), CE_None);
    EXPECT_STREQ(b1->GetMetadataItem("HAS_BLOCK_CACHE", "_DEBUG_"), "1");

    // PIXELTYPE on GDT_Byte.
    CPLPushErrorHandler(CPLQuietErrorHandler);
    (void)b1->GetMetadataItem("PIXELTYPE", "IMAGE_STRUCTURE");
    CPLPopErrorHandler();

    // Unknown metadata domain -> nullptr.
    EXPECT_EQ(b1->GetMetadataItem("JPEGTABLES", "NONEXISTENT"), nullptr);
  }

  // JPEGTABLES on uncompressed TIFF returns nullptr, and ColorTable on Palette.
  {
    const char* pal_path = "/vsimem/rb_meta_pal.tif";
    absl::Cleanup clean = [&] { VSIUnlink(pal_path); };
    std::unique_ptr<GDALDataset> ds(
        drv->Create(pal_path, 8, 8, 1, GDT_Byte, nullptr));
    ASSERT_NE(ds, nullptr);
    GDALColorTable ct;
    GDALColorEntry ce = {10, 20, 30, 255};
    ct.SetColorEntry(0, &ce);
    EXPECT_EQ(ds->GetRasterBand(1)->SetColorTable(&ct), CE_None);
    EXPECT_NE(ds->GetRasterBand(1)->GetColorTable(), nullptr);
    EXPECT_EQ(ds->GetRasterBand(1)->GetMetadataItem("JPEGTABLES", "TIFF"),
              nullptr);
  }

  // 2. GetNoDataValue, GetNoDataValueAsInt64, GetNoDataValueAsUInt64:
  // - PAM NoData override (BASELINE profile)
  // - Multi-band per-band NODATA_VALUES (m_bNoDataSet) vs dataset GDAL_NODATA
  // - Int64 & UInt64 dataset/band NoData + cross-type error paths!
  {
    CPLConfigOptionSetter pam_on("GDAL_PAM_ENABLED", "YES", false);
    const char* p_pam = "/vsimem/rb_nd_pam.tif";
    const char* p_mb = "/vsimem/rb_nd_mb.tif";
    const char* p_i64 = "/vsimem/rb_nd_i64.tif";
    const char* p_u64 = "/vsimem/rb_nd_u64.tif";
    const char* p_i64_pam = "/vsimem/rb_nd_i64_pam.tif";
    const char* p_u64_pam = "/vsimem/rb_nd_u64_pam.tif";
    absl::Cleanup clean = [&] {
      for (const char* p : {p_pam, p_mb, p_i64, p_u64, p_i64_pam, p_u64_pam}) {
        VSIUnlink(p);
        VSIUnlink((std::string(p) + ".aux.xml").c_str());
      }
    };

    // PAM NoData on Byte, Int64, UInt64 (PROFILE=BASELINE forces PAM).
    CPLStringList base_co;
    base_co.SetNameValue("PROFILE", "BASELINE");
    {
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_pam, 4, 4, 1, GDT_Byte, base_co.List()));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetRasterBand(1)->SetNoDataValue(77.0), CE_None);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_pam, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      int ok = 0;
      EXPECT_DOUBLE_EQ(ds->GetRasterBand(1)->GetNoDataValue(&ok), 77.0);
      EXPECT_EQ(ok, TRUE);
      CPLPushErrorHandler(CPLQuietErrorHandler);
      (void)ds->GetRasterBand(1)->GetNoDataValueAsInt64(&ok);
      EXPECT_EQ(ok, FALSE);
      (void)ds->GetRasterBand(1)->GetNoDataValueAsUInt64(&ok);
      EXPECT_EQ(ok, FALSE);
      CPLPopErrorHandler();
    }

    // Per-band m_bNoDataSet (live on band 1 while band 2 has none) vs
    // dataset-level m_poGDS->m_bNoDataSet on reopen!
    {
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_mb, 4, 4, 2, GDT_Byte, nullptr));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetRasterBand(1)->SetNoDataValue(12.0), CE_None);
      int ok = 0;
      EXPECT_DOUBLE_EQ(ds->GetRasterBand(1)->GetNoDataValue(&ok), 12.0);
      EXPECT_EQ(ok, TRUE);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_mb, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      int ok = 0;
      EXPECT_DOUBLE_EQ(ds->GetRasterBand(1)->GetNoDataValue(&ok), 12.0);
      EXPECT_EQ(ok, TRUE);
    }

    // Int64 TIFF with NoData & without NoData + PAM Int64 NoData.
    {
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_i64, 4, 4, 1, GDT_Int64, nullptr));
      ASSERT_NE(ds, nullptr);
      int ok = 0;
      EXPECT_EQ(ds->GetRasterBand(1)->GetNoDataValueAsInt64(&ok),
                GDAL_PAM_DEFAULT_NODATA_VALUE_INT64);
      EXPECT_EQ(ok, FALSE);
      EXPECT_EQ(ds->GetRasterBand(1)->SetNoDataValueAsInt64(-1234567890123LL),
                CE_None);
      // Band-level m_bNoDataSetAsInt64 cast to double:
      EXPECT_NEAR(ds->GetRasterBand(1)->GetNoDataValue(&ok), -1234567890123.0,
                  1.0);
      EXPECT_EQ(ok, TRUE);
      EXPECT_EQ(ds->GetRasterBand(1)->GetNoDataValueAsInt64(&ok),
                -1234567890123LL);
      EXPECT_EQ(ok, TRUE);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_i64, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      int ok = 0;
      // Dataset-level m_poGDS->m_bNoDataSetAsInt64!
      EXPECT_EQ(ds->GetRasterBand(1)->GetNoDataValueAsInt64(&ok),
                -1234567890123LL);
      EXPECT_EQ(ok, TRUE);
      EXPECT_NEAR(ds->GetRasterBand(1)->GetNoDataValue(&ok), -1234567890123.0,
                  1.0);
      CPLPushErrorHandler(CPLQuietErrorHandler);
      (void)ds->GetRasterBand(1)->GetNoDataValueAsUInt64(&ok);
      EXPECT_EQ(ok, FALSE);
      CPLPopErrorHandler();
    }
    {
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_i64_pam, 4, 4, 1, GDT_Int64, base_co.List()));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetRasterBand(1)->SetNoDataValueAsInt64(-999), CE_None);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_i64_pam, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      int ok = 0;
      EXPECT_EQ(ds->GetRasterBand(1)->GetNoDataValueAsInt64(&ok), -999);
      EXPECT_EQ(ok, TRUE);
    }

    // UInt64 TIFF with NoData & without NoData + PAM UInt64 NoData.
    {
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_u64, 4, 4, 1, GDT_UInt64, nullptr));
      ASSERT_NE(ds, nullptr);
      int ok = 0;
      (void)ds->GetRasterBand(1)->GetNoDataValueAsUInt64(&ok);
      EXPECT_EQ(ok, FALSE);
      EXPECT_EQ(ds->GetRasterBand(1)->SetNoDataValueAsUInt64(9876543210ULL),
                CE_None);
      EXPECT_NEAR(ds->GetRasterBand(1)->GetNoDataValue(&ok), 9876543210.0, 1.0);
      EXPECT_EQ(ds->GetRasterBand(1)->GetNoDataValueAsUInt64(&ok),
                9876543210ULL);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_u64, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      int ok = 0;
      EXPECT_EQ(ds->GetRasterBand(1)->GetNoDataValueAsUInt64(&ok),
                9876543210ULL);
      EXPECT_EQ(ok, TRUE);
      EXPECT_NEAR(ds->GetRasterBand(1)->GetNoDataValue(&ok), 9876543210.0, 1.0);
      CPLPushErrorHandler(CPLQuietErrorHandler);
      (void)ds->GetRasterBand(1)->GetNoDataValueAsInt64(&ok);
      EXPECT_EQ(ok, FALSE);
      CPLPopErrorHandler();
    }
    {
      std::unique_ptr<GDALDataset> ds(
          drv->Create(p_u64_pam, 4, 4, 1, GDT_UInt64, base_co.List()));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetRasterBand(1)->SetNoDataValueAsUInt64(888ULL), CE_None);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(p_u64_pam, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      int ok = 0;
      EXPECT_EQ(ds->GetRasterBand(1)->GetNoDataValueAsUInt64(&ok), 888ULL);
      EXPECT_EQ(ok, TRUE);
    }
  }

  // 3. Overviews (internal, external .ovr, implicit JPEG overviews, disabled
  // overviews) & Masks (internal 1-band & per-band, overview mask, external
  // MASK_OVERVIEW_DATASET, GetMaskValueRange 0_AND_255 vs 0_AND_1 vs UNKNOWN).
  {
    // 3a. Implicit JPEG overviews (256x256 JPEG) + downsampled RasterIO.
    const char* jpg256 = "/vsimem/rb_jpg_ovr.tif";
    absl::Cleanup clean_jpg = [&] { VSIUnlink(jpg256); };
    {
      CPLStringList co;
      co.SetNameValue("COMPRESS", "JPEG");
      std::unique_ptr<GDALDataset> ds(
          drv->Create(jpg256, 256, 256, 1, GDT_Byte, co.List()));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetRasterBand(1)->Fill(100), CE_None);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(jpg256, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetRasterBand(1)->GetOverviewCount(), 0);
      EXPECT_NE(ds->GetRasterBand(1)->GetOverview(0), nullptr);
      EXPECT_EQ(ds->GetRasterBand(1)->GetOverview(99), nullptr);
      // Downsampled RasterIO increments m_nJPEGOverviewVisibilityCounter ->
      // hits m_poGDS->GetJPEGOverviewCount() inside GetOverviewCount()!
      std::vector<GByte> sub(64 * 64, 0);
      EXPECT_EQ(ds->RasterIO(GF_Read, 0, 0, 256, 256, sub.data(), 64, 64,
                             GDT_Byte, 1, nullptr, 0, 0, 0, nullptr),
                CE_None);

      // Disable overviews via open option OVERVIEW_LEVEL=NONE!
      const char* const oo[] = {"OVERVIEW_LEVEL=NONE", nullptr};
      std::unique_ptr<GDALDataset> ds_no_ovr(GDALDataset::Open(
          jpg256, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, oo, nullptr));
      ASSERT_NE(ds_no_ovr, nullptr);
      EXPECT_EQ(ds_no_ovr->GetRasterBand(1)->GetOverviewCount(), 0);
    }

    // 3b. External .ovr + external .msk (created in RO mode).
    const char* ext_path = "/vsimem/rb_ext_ovr.tif";
    absl::Cleanup clean_ext = [&] {
      VSIUnlink(ext_path);
      VSIUnlink("/vsimem/rb_ext_ovr.tif.ovr");
      VSIUnlink("/vsimem/rb_ext_ovr.tif.msk");
    };
    {
      std::unique_ptr<GDALDataset> ds(
          drv->Create(ext_path, 32, 32, 1, GDT_Byte, nullptr));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetRasterBand(1)->Fill(50), CE_None);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(ext_path, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      int ovr = 2;
      EXPECT_EQ(
          ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr, nullptr, nullptr),
          CE_None);
      EXPECT_EQ(ds->GetRasterBand(1)->GetOverviewCount(), 1);
      EXPECT_NE(ds->GetRasterBand(1)->GetOverview(0), nullptr);
      EXPECT_EQ(ds->CreateMaskBand(GMF_PER_DATASET), CE_None);
      EXPECT_EQ(ds->GetRasterBand(1)->GetMaskFlags(), GMF_PER_DATASET);
      GDALRasterBand* ext_msk = ds->GetRasterBand(1)->GetMaskBand();
      ASSERT_NE(ext_msk, nullptr);
      EXPECT_TRUE(ext_msk->IsMaskBand());
      EXPECT_EQ(ext_msk->GetMaskValueRange(), GMVR_UNKNOWN);
    }

    // 3c. Internal per-dataset 1-bit mask + internal overviews -> tests
    // overview mask delegation, 8-bit promoted vs 1-bit GetMaskValueRange,
    // per-band internal mask, and MASK_OVERVIEW_DATASET open option!
    const char* int_msk_path = "/vsimem/rb_int_msk.tif";
    const char* perband_msk_path = "/vsimem/rb_perband_msk.tif";
    const char* ext_mo_path = "/vsimem/rb_ext_mo.tif";
    absl::Cleanup clean_msk = [&] {
      VSIUnlink(int_msk_path);
      VSIUnlink(perband_msk_path);
      VSIUnlink(ext_mo_path);
    };
    {
      CPLConfigOptionSetter int_msk("GDAL_TIFF_INTERNAL_MASK", "YES", false);
      std::unique_ptr<GDALDataset> ds(
          drv->Create(int_msk_path, 32, 32, 2, GDT_Byte, nullptr));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->CreateMaskBand(GMF_PER_DATASET), CE_None);
      EXPECT_EQ(ds->GetRasterBand(1)->GetMaskBand()->Fill(255), CE_None);
      int ovr = 2;
      EXPECT_EQ(
          ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr, nullptr, nullptr),
          CE_None);

      // Per-band internal mask IFD (2-band main IFD + 2-band FILETYPE_MASK IFD)
      // so m_poMaskDS->GetRasterCount() == 2 -> GetMaskFlags() == 0!
      {
        VSILFILE* fp = VSIFOpenL(perband_msk_path, "wb+");
        ASSERT_NE(fp, nullptr);
        TIFF* tif = VSI_TIFFOpen(perband_msk_path, "w", fp);
        ASSERT_NE(tif, nullptr);
        std::vector<GByte> buf2(16 * 16 * 2, 100);
        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 16);
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 16);
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 2);
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
        TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 16);
        TIFFWriteEncodedStrip(tif, 0, buf2.data(), buf2.size());
        TIFFWriteDirectory(tif);

        std::vector<GByte> msk2(16 * 16 / 8 * 2, 0xFF);
        TIFFSetField(tif, TIFFTAG_SUBFILETYPE, FILETYPE_MASK);
        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 16);
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 16);
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 2);
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 1);
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MASK);
        TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 16);
        TIFFWriteEncodedStrip(tif, 0, msk2.data(), msk2.size());
        TIFFWriteDirectory(tif);
        TIFFClose(tif);
        VSIFCloseL(fp);

        std::unique_ptr<GDALDataset> ds_pb(GDALDataset::Open(
            perband_msk_path, GDAL_OF_RASTER | GDAL_OF_READONLY));
        ASSERT_NE(ds_pb, nullptr);
        EXPECT_EQ(ds_pb->GetRasterBand(1)->GetMaskFlags(), 0);
        EXPECT_NE(ds_pb->GetRasterBand(2)->GetMaskBand(), nullptr);
      }

      // Small 16x16 1-band TIFF to serve as MASK_OVERVIEW_DATASET for 16x16 ovr
      std::unique_ptr<GDALDataset> ds_mo(
          drv->Create(ext_mo_path, 16, 16, 1, GDT_Byte, nullptr));
      ASSERT_NE(ds_mo, nullptr);
      EXPECT_EQ(ds_mo->GetRasterBand(1)->Fill(255), CE_None);
    }

    // Open with GDAL_TIFF_INTERNAL_MASK_TO_8BIT=YES (default -> 0_AND_255)
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(int_msk_path, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      GDALRasterBand* b1 = ds->GetRasterBand(1);
      EXPECT_FALSE(b1->IsMaskBand());
      EXPECT_EQ(b1->GetMaskValueRange(), GMVR_UNKNOWN);
      EXPECT_EQ(b1->GetOverview(-1), nullptr);
      EXPECT_EQ(b1->GetOverview(99), nullptr);
      GDALRasterBand* msk = b1->GetMaskBand();
      ASSERT_NE(msk, nullptr);
      EXPECT_TRUE(msk->IsMaskBand());
      EXPECT_EQ(msk->GetMaskValueRange(), GMVR_0_AND_255_ONLY);

      GDALRasterBand* ovr_b1 = b1->GetOverview(0);
      ASSERT_NE(ovr_b1, nullptr);
      EXPECT_EQ(ovr_b1->GetMaskFlags(), GMF_PER_DATASET);
      EXPECT_NE(ovr_b1->GetMaskBand(), nullptr);
    }

    // Open with GDAL_TIFF_INTERNAL_MASK_TO_8BIT=NO -> GMVR_0_AND_1_ONLY!
    {
      CPLConfigOptionSetter msk_1bit("GDAL_TIFF_INTERNAL_MASK_TO_8BIT", "NO",
                                     false);
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(int_msk_path, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetRasterBand(1)->GetMaskBand()->GetMaskValueRange(),
                GMVR_0_AND_1_ONLY);
    }

    // Overview without internal mask delegating to base band's external mask,
    // plus MASK_OVERVIEW_DATASET open option!
    const char* ovr_no_int_msk = "/vsimem/rb_ovr_base_msk.tif";
    absl::Cleanup clean_obm = [&] {
      VSIUnlink(ovr_no_int_msk);
      VSIUnlink("/vsimem/rb_ovr_base_msk.tif.msk");
    };
    {
      std::unique_ptr<GDALDataset> ds(
          drv->Create(ovr_no_int_msk, 32, 32, 1, GDT_Byte, nullptr));
      ASSERT_NE(ds, nullptr);
      int ovr = 2;
      EXPECT_EQ(
          ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr, nullptr, nullptr),
          CE_None);
    }
    {
      // Create external .msk with its own 16x16 overview!
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(ovr_no_int_msk, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->CreateMaskBand(GMF_PER_DATASET), CE_None);
      int ovr = 2;
      EXPECT_EQ(
          ds->GetRasterBand(1)->GetMaskBand()->GetDataset()->BuildOverviews(
              "NEAREST", 1, &ovr, 0, nullptr, nullptr, nullptr),
          CE_None);
    }
    {
      std::unique_ptr<GDALDataset> ds(
          GDALDataset::Open(ovr_no_int_msk, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      GDALRasterBand* ovr_b = ds->GetRasterBand(1)->GetOverview(0);
      ASSERT_NE(ovr_b, nullptr);
      EXPECT_EQ(ovr_b->GetMaskFlags(), GMF_PER_DATASET);
      GDALRasterBand* ovr_m = ovr_b->GetMaskBand();
      ASSERT_NE(ovr_m, nullptr);
      EXPECT_EQ(ovr_m->GetXSize(), 16);

      // Also open with MASK_OVERVIEW_DATASET -> m_poExternalMaskDS != nullptr!
      std::string mo_opt = std::string("MASK_OVERVIEW_DATASET=") + ext_mo_path;
      const char* const oo[] = {mo_opt.c_str(), nullptr};
      std::unique_ptr<GDALDataset> ds_mo(
          GDALDataset::Open(ovr_no_int_msk, GDAL_OF_RASTER | GDAL_OF_READONLY,
                            nullptr, oo, nullptr));
      ASSERT_NE(ds_mo, nullptr);
      GDALRasterBand* ovr_b_mo = ds_mo->GetRasterBand(1)->GetOverview(0);
      ASSERT_NE(ovr_b_mo, nullptr);
      EXPECT_EQ(ovr_b_mo->GetMaskFlags(), GMF_PER_DATASET);
      EXPECT_NE(ovr_b_mo->GetMaskBand(), nullptr);
    }
  }
}

}  // namespace
