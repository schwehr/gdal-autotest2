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
// White-box tests for HFA file/dataset C API, georeferencing, transforms, and
// metadata (hfa_p.h / hfaopen.cpp).
//
// Note: White-box tests depend on hfa_p.h internals and assume little-endian
// host byte order (HFAStandard is a no-op on CPL_LSB). CaptureMemStream uses
// POSIX open_memstream (Linux/glibc).
//
// See also:
//   https://gdal.org/drivers/raster/hfa.html
//   https://github.com/OSGeo/gdal/blob/master/frmts/hfa/hfaopen.cpp

#include <array>
#include <cstdio>
#include <cstring>
#include <format>  // NOLINT
#include <memory>
#include <span>  // NOLINT
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/hfa/hfa.h"
#include "third_party/gdal/frmts/hfa/hfa_p.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

constexpr char kTestDir[] = "/vsimem/hfaopen_test";

constexpr std::array<EPTType, 13> kAllEptTypes = {
    EPT_u1,  EPT_u2,  EPT_u4,  EPT_u8,  EPT_s8,  EPT_u16, EPT_s16,
    EPT_u32, EPT_s32, EPT_f32, EPT_f64, EPT_c64, EPT_c128};
static_assert(kAllEptTypes.size() == EPT_MAX + 1);

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

template <typename Func>
std::string CaptureMemStream(Func&& dump_fn) {
  char* buf = nullptr;
  size_t size = 0;
  std::unique_ptr<char, FreeDeleter> owned_buf;
  FILE* fp = open_memstream(&buf, &size);
  if (fp == nullptr) {
    ADD_FAILURE() << "open_memstream failed";
    return "";
  }
  dump_fn(fp);
  EXPECT_EQ(0, std::fclose(fp));
  owned_buf.reset(buf);
  return owned_buf == nullptr ? "" : std::string(owned_buf.get(), size);
}

void WriteVsiMemBytes(const char* path, std::span<const GByte> bytes) {
  std::unique_ptr<GByte, decltype(&VSIFree)> owned(
      static_cast<GByte*>(VSIMalloc(bytes.size())), &VSIFree);
  ASSERT_NE(nullptr, owned);
  std::memcpy(owned.get(), bytes.data(), bytes.size());
  EXPECT_EQ(0, VSIFCloseL(VSIFileFromMemBuffer(path, owned.release(),
                                               bytes.size(), TRUE)));
}

void SetLayerDims(HFAEntry* layer, int width, int height, int block_width,
                  int block_height) {
  ASSERT_NE(nullptr, layer);
  EXPECT_EQ(CE_None, layer->SetIntField("width", width));
  EXPECT_EQ(CE_None, layer->SetIntField("height", height));
  EXPECT_EQ(CE_None, layer->SetIntField("blockWidth", block_width));
  EXPECT_EQ(CE_None, layer->SetIntField("blockHeight", block_height));
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
  if (hfa->poDictionary->osDictionaryText.back() != '.') {
    hfa->poDictionary->osDictionaryText += ".";
  }
  hfa->poDictionary->bDictionaryTextDirty = true;
}

void PopulatePolynomial(HFAEntry* node, std::string_view prefix, int order,
                        int termcount, int mtx_cols) {
  auto field_name = [prefix](std::string_view leaf) {
    return prefix.empty() ? std::string(leaf)
                          : std::format("{}[0].{}", prefix, leaf);
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
  EXPECT_EQ(CE_None,
            node->SetDoubleField(field_name("polycoefmtx[0]").c_str(), 1.0));
  EXPECT_EQ(CE_None,
            node->SetDoubleField(field_name("polycoefmtx[3]").c_str(), 1.0));
  EXPECT_EQ(CE_None, node->SetIntField(field_name("polycoefvector[-3]").c_str(),
                                       EPT_f64));
  EXPECT_EQ(CE_None,
            node->SetIntField(field_name("polycoefvector[-2]").c_str(), 1));
  EXPECT_EQ(CE_None,
            node->SetIntField(field_name("polycoefvector[-1]").c_str(), 2));
}

TEST(HfaOpenTest, OpenCloseDeleteAndBandMismatch) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };

  // 1. Nonexistent, too-short (< 16 bytes), wrong magic tag, and truncated
  //    header offset past EOF.
  constexpr char kShortPath[] = "/vsimem/hfaopen_test/short.img";
  constexpr GByte kShortMagic[] = {'E', 'H', 'F', 'A'};
  WriteVsiMemBytes(kShortPath, kShortMagic);

  constexpr char kBadTagPath[] = "/vsimem/hfaopen_test/badtag.img";
  WriteVsiMemBytes(kBadTagPath, std::vector<GByte>(32, 'X'));

  constexpr char kTruncHdrPath[] = "/vsimem/hfaopen_test/trunchdr.img";
  std::vector<GByte> trunc_hdr(20, 0);
  std::memcpy(trunc_hdr.data(), "EHFA_HEADER_TAG", 15);
  WriteHfaLe(trunc_hdr.data(), 16, GUInt32{9999});
  WriteVsiMemBytes(kTruncHdrPath, trunc_hdr);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, HFAOpen("/vsimem/hfaopen_test/nonexistent.img", "rb"));
    EXPECT_EQ(nullptr, HFAOpen(kShortPath, "rb"));
    EXPECT_EQ(nullptr, HFAOpen(kBadTagPath, "rb"));
    EXPECT_EQ(nullptr, HFAOpen(kTruncHdrPath, "rb"));
    EXPECT_EQ(CE_Failure, HFADelete("/vsimem/hfaopen_test/missing_del.img"));
    CPLErrorReset();
  }

  // 2. HFADelete on spill file deletes .img + .ige; path traversal in
  //    ExternalRasterDMS skips unlinking external file.
  constexpr char kSpillImg[] = "/vsimem/hfaopen_test/del_spill.img";
  constexpr char kSpillIge[] = "/vsimem/hfaopen_test/del_spill.ige";
  CPLStringList spill_opts({"USE_SPILL=YES", "BLOCKSIZE=32"});
  HfaPtr hfa(HFACreate(kSpillImg, 32, 32, 1, EPT_u8, spill_opts.List()));
  ASSERT_NE(nullptr, hfa);
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));
  VSIStatBufL stat_buf;
  EXPECT_EQ(0, VSIStatL(kSpillIge, &stat_buf));
  EXPECT_EQ(CE_None, HFADelete(kSpillImg));
  EXPECT_NE(0, VSIStatL(kSpillImg, &stat_buf));
  EXPECT_NE(0, VSIStatL(kSpillIge, &stat_buf));

  hfa.reset(HFACreate(kSpillImg, 32, 32, 1, EPT_u8, spill_opts.List()));
  ASSERT_NE(nullptr, hfa);
  HFAEntry* ext_dms =
      hfa->papoBand[0]->poNode->GetNamedChild("ExternalRasterDMS");
  ASSERT_NE(nullptr, ext_dms);
  ext_dms->SetStringField("fileName.string", "../evil.ige");
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));
  EXPECT_EQ(CE_None, HFADelete(kSpillImg));

  // 3. HFAParseBandInfo rejects a second Eimg_Layer whose dimensions mismatch.
  constexpr char kMismatchPath[] = "/vsimem/hfaopen_test/mismatch.img";
  hfa.reset(HFACreate(kMismatchPath, 32, 32, 1, EPT_u8, nullptr));
  ASSERT_NE(nullptr, hfa);
  HFAEntry* extra_layer =
      HFAEntry::New(hfa.get(), "Layer_2", "Eimg_Layer", hfa->poRoot);
  ASSERT_NO_FATAL_FAILURE(SetLayerDims(extra_layer, 64, 32, 32, 32));
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    hfa.reset(HFAOpen(kMismatchPath, "rb"));
    CPLErrorReset();
  }
  ASSERT_NE(nullptr, hfa);
  EXPECT_EQ(1, hfa->nBands);
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));
}

TEST(HfaOpenTest, CreateOptionsAndAllDataTypes) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };
  absl::Cleanup reset_tls_opts = [] {
    CPLSetThreadLocalConfigOption("FORCE_BLOCKSIZE", nullptr);
  };

  // 1. Invalid HFACreate dimensions and block-size overflow return nullptr.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, HFACreateLL("/"));
    EXPECT_EQ(nullptr, HFACreate("/vsimem/hfaopen_test/zero.img", 0, 32, 1,
                                 EPT_u8, nullptr));
    EXPECT_EQ(nullptr, HFACreate("/vsimem/hfaopen_test/zero.img", 32, 0, 1,
                                 EPT_u8, nullptr));
    EXPECT_EQ(nullptr, HFACreate("/vsimem/hfaopen_test/huge_blocks.img",
                                 3000000, 3000000, 1, EPT_u8, nullptr));

    CPLSetThreadLocalConfigOption("FORCE_BLOCKSIZE", "YES");
    CPLStringList huge_bs({"BLOCKSIZE=20000"});
    EXPECT_EQ(nullptr, HFACreate("/vsimem/hfaopen_test/huge_bytes.img", 32, 32,
                                 1, EPT_c128, huge_bs.List()));
    CPLSetThreadLocalConfigOption("FORCE_BLOCKSIZE", nullptr);
    CPLErrorReset();
  }

  // 2. HFACreateLL removes stale .rrd and .ige files; invalid HFACreateLayer
  //    returns FALSE.
  constexpr char kBaseImg[] = "/vsimem/hfaopen_test/stale.img";
  constexpr char kBaseRrd[] = "/vsimem/hfaopen_test/stale.rrd";
  constexpr char kBaseIge[] = "/vsimem/hfaopen_test/stale.ige";
  constexpr GByte kDummyBytes[] = {1, 2, 3};
  WriteVsiMemBytes(kBaseRrd, kDummyBytes);
  WriteVsiMemBytes(kBaseIge, kDummyBytes);
  HfaPtr hfa(HFACreateLL(kBaseImg));
  ASSERT_NE(nullptr, hfa);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(FALSE, HFACreateLayer(hfa.get(), hfa->poRoot, "BadLayer", FALSE,
                                    0, FALSE, FALSE, FALSE, 32, 32, EPT_u8,
                                    nullptr, 0, 0, 1, 0));
    CPLErrorReset();
  }
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));
  VSIStatBufL stat_buf;
  EXPECT_NE(0, VSIStatL(kBaseRrd, &stat_buf));
  EXPECT_NE(0, VSIStatL(kBaseIge, &stat_buf));

  // 3. BLOCKSIZE validation, FORCE_BLOCKSIZE=YES, COMPRESSED=YES, and nBands=0.
  constexpr char kOptImg[] = "/vsimem/hfaopen_test/opts.img";
  CPLStringList bad_bs(
      {"BLOCKSIZE=17", "AUX=YES", "DEPENDENT_FILE=parent.img"});
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    hfa.reset(HFACreate(kOptImg, 64, 64, 1, EPT_u8, bad_bs.List()));
    CPLErrorReset();
  }
  ASSERT_NE(nullptr, hfa);
  int bx = 0;
  int by = 0;
  EXPECT_EQ(CE_None, HFAGetBandInfo(hfa.get(), 1, nullptr, &bx, &by, nullptr));
  EXPECT_EQ(64, bx);
  EXPECT_EQ(64, by);
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  CPLSetThreadLocalConfigOption("FORCE_BLOCKSIZE", "YES");
  CPLStringList forced_bs({"BLOCKSIZE=16", "COMPRESSED=YES"});
  hfa.reset(HFACreate(kOptImg, 32, 32, 1, EPT_u8, forced_bs.List()));
  ASSERT_NE(nullptr, hfa);
  int comp = 0;
  EXPECT_EQ(CE_None, HFAGetBandInfo(hfa.get(), 1, nullptr, &bx, &by, &comp));
  EXPECT_EQ(16, bx);
  EXPECT_EQ(1, comp);
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));
  CPLSetThreadLocalConfigOption("FORCE_BLOCKSIZE", nullptr);

  hfa.reset(HFACreate(kOptImg, 32, 32, 0, EPT_u8, nullptr));
  ASSERT_NE(nullptr, hfa);
  int nx = -1;
  int ny = -1;
  int nb = -1;
  EXPECT_EQ(CE_None, HFAGetRasterInfo(hfa.get(), &nx, &ny, &nb));
  EXPECT_EQ(0, nb);
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  // 4. Create and inspect 1-band dataset across all supported EPT data types.
  for (EPTType dt : kAllEptTypes) {
    EXPECT_GT(HFAGetDataTypeBits(dt), 0);
    EXPECT_STRNE("unknown", HFAGetDataTypeName(dt));
    hfa.reset(HFACreate(kOptImg, 32, 32, 1, dt, nullptr));
    ASSERT_NE(nullptr, hfa);
    EPTType out_dt = EPT_u8;
    EXPECT_EQ(CE_None,
              HFAGetBandInfo(hfa.get(), 1, &out_dt, nullptr, nullptr, nullptr));
    EXPECT_EQ(dt, out_dt);
    ASSERT_EQ(CE_None, HFAClose(hfa.release()));
  }
}

TEST(HfaOpenTest, BandBlocksOverviewsPctAndRange) {
  constexpr char kPath[] = "/vsimem/hfaopen_test/band_api.img";
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };

  // 1. Band names, overviews, and nodata C API.
  CPLStringList opts({"BLOCKSIZE=32"});
  HfaPtr hfa(HFACreate(kPath, 64, 64, 1, EPT_u8, opts.List()));
  ASSERT_NE(nullptr, hfa);

  EXPECT_STREQ("", HFAGetBandName(hfa.get(), 0));
  EXPECT_STREQ("", HFAGetBandName(hfa.get(), 2));
  HFASetBandName(hfa.get(), 0, "Ignored");
  HFASetBandName(hfa.get(), 1, "BandOne");
  EXPECT_STREQ("BandOne", HFAGetBandName(hfa.get(), 1));

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(-1, HFACreateOverview(hfa.get(), 0, 2, "NEAREST"));
    CPLErrorReset();
  }
  EXPECT_EQ(0, HFACreateOverview(hfa.get(), 1, 2, "NEAREST"));
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  hfa.reset(HFAOpen(kPath, "r+b"));
  ASSERT_NE(nullptr, hfa);
  EXPECT_EQ(1, HFAGetOverviewCount(hfa.get(), 1));
  ASSERT_EQ(1, hfa->papoBand[0]->nOverviews);
  EXPECT_EQ(CE_None, hfa->papoBand[0]->papoOverviews[0]->SetNoDataValue(42.0));
  double nd = 0.0;
  EXPECT_EQ(TRUE, HFAGetBandNoData(hfa.get(), 1, &nd));
  EXPECT_DOUBLE_EQ(42.0, nd);

  EXPECT_EQ(CE_None, HFASetBandNoData(hfa.get(), 1, 99.0));
  EXPECT_EQ(TRUE, HFAGetBandNoData(hfa.get(), 1, &nd));
  EXPECT_DOUBLE_EQ(99.0, nd);

  // 2. Band and overview block read/write and PCT C API.
  int ox = 0;
  int oy = 0;
  int obx = 0;
  int oby = 0;
  EPTType odt = EPT_u1;
  EXPECT_EQ(CE_None,
            HFAGetOverviewInfo(hfa.get(), 1, 0, &ox, &oy, &obx, &oby, &odt));
  EXPECT_EQ(32, ox);
  EXPECT_EQ(32, oy);

  std::vector<GByte> blk(1024, 77);
  std::vector<GByte> out(1024, 0);
  EXPECT_EQ(CE_None, HFASetRasterBlock(hfa.get(), 1, 0, 0, blk.data()));
  EXPECT_EQ(CE_None, HFAGetRasterBlock(hfa.get(), 1, 0, 0, out.data()));
  EXPECT_EQ(blk, out);

  std::vector<GByte> ov_blk(obx * oby, 55);
  std::vector<GByte> ov_out(obx * oby, 0);
  EXPECT_EQ(CE_None,
            HFASetOverviewRasterBlock(hfa.get(), 1, 0, 0, 0, ov_blk.data()));
  EXPECT_EQ(CE_None,
            HFAGetOverviewRasterBlock(hfa.get(), 1, 0, 0, 0, ov_out.data()));
  EXPECT_EQ(ov_blk, ov_out);

  // Note: HFASetPCT takes non-const double* pointers in GDAL's C API.
  std::array<double, 2> red = {0.1, 0.9};
  std::array<double, 2> green = {0.2, 0.8};
  std::array<double, 2> blue = {0.3, 0.7};
  std::array<double, 2> alpha = {1.0, 1.0};
  EXPECT_EQ(CE_None, HFASetPCT(hfa.get(), 1, 2, red.data(), green.data(),
                               blue.data(), alpha.data()));
  int nColors = 0;
  double* pr = nullptr;
  double* pg = nullptr;
  double* pb = nullptr;
  double* pa = nullptr;
  double* pbins = nullptr;
  EXPECT_EQ(CE_None,
            HFAGetPCT(hfa.get(), 1, &nColors, &pr, &pg, &pb, &pa, &pbins));
  EXPECT_EQ(2, nColors);

  // 3. Invalid band/overview/PCT indices and Statistics data range checks.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, HFASetRasterBlock(hfa.get(), 0, 0, 0, blk.data()));
    EXPECT_EQ(CE_Failure, HFAGetRasterBlock(hfa.get(), 0, 0, 0, out.data()));
    EXPECT_EQ(CE_Failure,
              HFASetOverviewRasterBlock(hfa.get(), 0, 0, 0, 0, ov_blk.data()));
    EXPECT_EQ(CE_Failure,
              HFASetOverviewRasterBlock(hfa.get(), 1, 5, 0, 0, ov_blk.data()));
    EXPECT_EQ(CE_Failure,
              HFAGetOverviewRasterBlock(hfa.get(), 0, 0, 0, 0, ov_out.data()));
    EXPECT_EQ(CE_Failure,
              HFAGetOverviewRasterBlock(hfa.get(), 1, 5, 0, 0, ov_out.data()));
    EXPECT_EQ(CE_Failure, HFASetPCT(hfa.get(), 0, 2, red.data(), green.data(),
                                    blue.data(), alpha.data()));
    EXPECT_EQ(CE_Failure,
              HFAGetPCT(hfa.get(), 0, &nColors, &pr, &pg, &pb, &pa, &pbins));
    CPLErrorReset();
  }

  double dmin = 0.0;
  double dmax = 0.0;
  EXPECT_EQ(CE_Failure, HFAGetDataRange(hfa.get(), 0, &dmin, &dmax));
  EXPECT_EQ(CE_Failure, HFAGetDataRange(hfa.get(), 1, &dmin, &dmax));

  HFAEntry* stats = HFAEntry::New(hfa.get(), "Statistics", "Esta_Statistics",
                                  hfa->papoBand[0]->poNode);
  ASSERT_NE(nullptr, stats);
  stats->SetDoubleField("minimum", 10.0);
  stats->SetDoubleField("maximum", 10.0);
  EXPECT_EQ(CE_Failure, HFAGetDataRange(hfa.get(), 1, &dmin, &dmax));
  stats->SetDoubleField("maximum", 250.0);
  EXPECT_EQ(CE_None, HFAGetDataRange(hfa.get(), 1, &dmin, &dmax));
  EXPECT_DOUBLE_EQ(10.0, dmin);
  EXPECT_DOUBLE_EQ(250.0, dmax);

  ASSERT_EQ(CE_None, HFAClose(hfa.release()));
}

TEST(HfaOpenTest, SpillStackDependentAndRenameReferences) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };

  constexpr char kImgPath[] = "/vsimem/hfaopen_test/dep.img";
  constexpr char kRrdPath[] = "/vsimem/hfaopen_test/dep.rrd";
  constexpr char kAuxPath[] = "/vsimem/hfaopen_test/dep.aux";

  // 1. Dependent .rrd/.aux creation and lookup.
  HfaPtr hfa(HFACreate(kImgPath, 64, 64, 1, EPT_u8, nullptr));
  ASSERT_NE(nullptr, hfa);
  HFAInfo_t* dep = HFACreateDependent(hfa.get());
  ASSERT_NE(nullptr, dep);
  EXPECT_EQ(dep, HFACreateDependent(hfa.get()));
  EXPECT_EQ(hfa.get(), HFAGetDependent(hfa.get(), hfa->pszFilename));
  EXPECT_EQ(dep, HFAGetDependent(hfa.get(), dep->pszFilename));
  EXPECT_EQ(nullptr, HFAGetDependent(hfa.get(), "other_unknown.rrd"));
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  hfa.reset(HFAOpen(kImgPath, "rb"));
  ASSERT_NE(nullptr, hfa);
  EXPECT_NE(nullptr, HFAGetDependent(hfa.get(), "dep.rrd"));
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  // 2. HFACreateSpillStack across zero block size, standard size, large
  //    >2GB file offset, and .rrd/.aux files.
  GIntBig vflags_off = 0;
  GIntBig data_off = 0;
  hfa.reset(HFACreateLL(kImgPath));
  ASSERT_NE(nullptr, hfa);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(HFACreateSpillStack(hfa.get(), 64, 64, 1, 0, EPT_u8,
                                     &vflags_off, &data_off));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
  EXPECT_TRUE(HFACreateSpillStack(hfa.get(), 96, 32, 1, 32, EPT_u8, &vflags_off,
                                  &data_off));
  EXPECT_TRUE(HFACreateSpillStack(hfa.get(), 256, 32, 1, 32, EPT_u8,
                                  &vflags_off, &data_off));
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  HfaPtr rrd_hfa(HFACreateLL(kRrdPath));
  ASSERT_NE(nullptr, rrd_hfa);
  EXPECT_TRUE(HFACreateSpillStack(rrd_hfa.get(), 32, 32, 1, 32, EPT_u8,
                                  &vflags_off, &data_off));
  ASSERT_EQ(CE_None, HFAClose(rrd_hfa.release()));

  HfaPtr aux_hfa(HFACreateLL(kAuxPath));
  ASSERT_NE(nullptr, aux_hfa);
  EXPECT_TRUE(HFACreateSpillStack(aux_hfa.get(), 32, 32, 1, 32, EPT_u8,
                                  &vflags_off, &data_off));
  ASSERT_EQ(CE_None, HFAClose(aux_hfa.release()));

  // 3. HFARenameReferences and HFAGetIGEFilename fallback + traversal check.
  CPLStringList spill_opts(
      {"USE_SPILL=YES", "BLOCKSIZE=32", "DEPENDENT_FILE=dep.img"});
  hfa.reset(HFACreate(kImgPath, 64, 64, 1, EPT_u8, spill_opts.List()));
  ASSERT_NE(nullptr, hfa);
  EXPECT_EQ(0, HFACreateOverview(hfa.get(), 1, 2, "NEAREST"));
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  hfa.reset(HFAOpen(kImgPath, "r+b"));
  ASSERT_NE(nullptr, hfa);
  EXPECT_EQ(CE_None, HFARenameReferences(hfa.get(), "dep_longer_name", "dep"));
  EXPECT_EQ(CE_None,
            HFARenameReferences(hfa.get(), "short_nm", "dep_longer_name"));
  HFAEntry* ext_dms =
      hfa->papoBand[0]->poNode->GetNamedChild("ExternalRasterDMS");
  ASSERT_NE(nullptr, ext_dms);
  ext_dms->SetStringField("fileName.string", "missing_name.ige");
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  hfa.reset(HFAOpen(kImgPath, "r+b"));
  ASSERT_NE(nullptr, hfa);
  EXPECT_TRUE(HFAGetIGEFilename(hfa.get()).ends_with("dep.ige"));
  CPLFree(hfa->pszIGEFilename);
  hfa->pszIGEFilename = nullptr;
  ext_dms = hfa->papoBand[0]->poNode->GetNamedChild("ExternalRasterDMS");
  ASSERT_NE(nullptr, ext_dms);
  ext_dms->SetStringField("fileName.string", "../traversal.ige");
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ("", HFAGetIGEFilename(hfa.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));
}

TEST(HfaOpenTest, MapInfoDatumProParametersAndPEString) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };
  absl::Cleanup cleanup_osr = [] { OSRCleanup(); };
  absl::Cleanup reset_tls_opts = [] {
    CPLSetThreadLocalConfigOption("HFA_WRITE_PE_STRING", nullptr);
  };

  constexpr char kPath[] = "/vsimem/hfaopen_test/georef.img";

  // 1. Zero-band file returns nullptr/FALSE for georef queries.
  HfaPtr zero_hfa(HFACreate(kPath, 32, 32, 0, EPT_u8, nullptr));
  ASSERT_NE(nullptr, zero_hfa);
  EXPECT_EQ(nullptr, HFAGetMapInfo(zero_hfa.get()));
  std::array<double, 6> gt = {};
  EXPECT_EQ(FALSE, HFAGetGeoTransform(zero_hfa.get(), gt.data()));
  EXPECT_EQ(nullptr, HFAGetDatum(zero_hfa.get()));
  EXPECT_EQ(nullptr, HFAGetProParameters(zero_hfa.get()));
  EXPECT_EQ(nullptr, HFAGetPEString(zero_hfa.get()));
  ASSERT_EQ(CE_None, HFAClose(zero_hfa.release()));

  // 2. Set Datum, ProParameters, MapInfo, and PEString on 1-band dataset.
  HfaPtr hfa(HFACreate(kPath, 32, 32, 1, EPT_u8, nullptr));
  ASSERT_NE(nullptr, hfa);
  EXPECT_EQ(nullptr, HFAGetMapInfo(hfa.get()));
  EXPECT_EQ(FALSE, HFAGetGeoTransform(hfa.get(), gt.data()));

  char datum_name[] = "WGS 84";
  char grid_name[] = "nadcon";
  Eprj_Datum datum = {
      .datumname = datum_name,
      .type = EPRJ_DATUM_PARAMETRIC,
      .params = {1.0},
      .gridname = grid_name,
  };
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    // HFASetDatum requires a Projection child node to already exist.
    EXPECT_EQ(CE_Failure, HFASetDatum(hfa.get(), &datum));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  char exe_name[] = "utm";
  char pro_name[] = "UTM";
  char sphere_name[] = "WGS 84";
  Eprj_ProParameters pro = {
      .proType = EPRJ_INTERNAL,
      .proNumber = EPRJ_UTM,
      .proExeName = exe_name,
      .proName = pro_name,
      .proZone = 11,
      .proParams = {1.0},
      .proSpheroid =
          {
              .sphereName = sphere_name,
              .a = 6378137.0,
              .b = 6356752.314245,
          },
  };
  EXPECT_EQ(CE_None, HFASetProParameters(hfa.get(), &pro));
  EXPECT_EQ(CE_None, HFASetDatum(hfa.get(), &datum));

  char units_meters[] = "meters";
  Eprj_MapInfo mi = {
      .proName = pro_name,
      .upperLeftCenter = {.x = 100.0, .y = 200.0},
      .lowerRightCenter = {.x = 410.0, .y = 100.0},
      .pixelSize = {.width = 10.0, .height = 10.0},
      .units = units_meters,
  };
  EXPECT_EQ(CE_None, HFASetMapInfo(hfa.get(), &mi));

  CPLSetThreadLocalConfigOption("HFA_WRITE_PE_STRING", "NO");
  EXPECT_EQ(CE_None, HFASetPEString(hfa.get(), R"wkt(PROJCS["Test"])wkt"));
  CPLSetThreadLocalConfigOption("HFA_WRITE_PE_STRING", nullptr);
  EXPECT_EQ(CE_None, HFASetPEString(hfa.get(), ""));
  EXPECT_EQ(CE_None, HFASetPEString(hfa.get(), R"wkt(PROJCS["Test_PE"])wkt"));
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  // 3. Reopen and verify cached pointers and pixel-center-to-corner
  //    geotransform: upperLeftCenter is the center of pixel (0, 0), so
  //    top-left corner is (100 - 10/2, 200 + 10/2) = (95.0, 205.0).
  hfa.reset(HFAOpen(kPath, "r+b"));
  ASSERT_NE(nullptr, hfa);
  const Eprj_MapInfo* out_mi = HFAGetMapInfo(hfa.get());
  ASSERT_NE(nullptr, out_mi);
  EXPECT_EQ(out_mi, HFAGetMapInfo(hfa.get()));
  EXPECT_STREQ("UTM", out_mi->proName);
  EXPECT_EQ(TRUE, HFAGetGeoTransform(hfa.get(), gt.data()));
  EXPECT_DOUBLE_EQ(95.0, gt[0]);
  EXPECT_DOUBLE_EQ(10.0, gt[1]);
  EXPECT_DOUBLE_EQ(205.0, gt[3]);
  EXPECT_DOUBLE_EQ(-10.0, gt[5]);

  const Eprj_ProParameters* out_pro = HFAGetProParameters(hfa.get());
  ASSERT_NE(nullptr, out_pro);
  EXPECT_EQ(out_pro, HFAGetProParameters(hfa.get()));
  EXPECT_EQ(11, out_pro->proZone);

  const Eprj_Datum* out_datum = HFAGetDatum(hfa.get());
  ASSERT_NE(nullptr, out_datum);
  EXPECT_EQ(out_datum, HFAGetDatum(hfa.get()));
  EXPECT_STREQ("WGS 84", out_datum->datumname);

  std::unique_ptr<char, CplFreeDeleter> pe_str(HFAGetPEString(hfa.get()));
  ASSERT_NE(nullptr, pe_str);
  EXPECT_STREQ(R"wkt(PROJCS["Test_PE"])wkt", pe_str.get());

  // "ds" (degree-seconds = 1/3600 deg) with zero pixelSize falls back to 1.0
  // unit (1/3600 deg) and positive Y step for south-up (upperY < lowerY).
  char units_ds[] = "ds";
  mi.upperLeftCenter.y = 50.0;
  mi.lowerRightCenter.y = 200.0;
  mi.pixelSize.width = 0.0;
  mi.pixelSize.height = 0.0;
  mi.units = units_ds;
  EXPECT_EQ(CE_None, HFASetMapInfo(hfa.get(), &mi));
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  hfa.reset(HFAOpen(kPath, "r+b"));
  ASSERT_NE(nullptr, hfa);
  EXPECT_EQ(TRUE, HFAGetGeoTransform(hfa.get(), gt.data()));
  EXPECT_DOUBLE_EQ(1.0 / 3600.0, gt[1]);
  EXPECT_DOUBLE_EQ(1.0 / 3600.0, gt[5]);
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));
}

TEST(HfaOpenTest, AffineXFormGeoTransformAndCorruptNodes) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };

  constexpr char kPath[] = "/vsimem/hfaopen_test/xform_gt.img";
  HfaPtr hfa(HFACreate(kPath, 32, 32, 1, EPT_u8, nullptr));
  ASSERT_NE(nullptr, hfa);

  // 1. Write affine geotransform and read back via HFAGetGeoTransform.
  // Note: HFASetGeoTransform takes a non-const double* pointer in GDAL's C API.
  std::array<double, 6> set_gt = {1000.0, 2.0, 0.5, 2000.0, -0.5, -2.0};
  EXPECT_EQ(CE_None,
            HFASetGeoTransform(hfa.get(), "UTM", "meters", set_gt.data()));
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  hfa.reset(HFAOpen(kPath, "r+b"));
  ASSERT_NE(nullptr, hfa);
  std::array<double, 6> read_gt = {};
  EXPECT_EQ(TRUE, HFAGetGeoTransform(hfa.get(), read_gt.data()));
  EXPECT_NEAR(1000.0, read_gt[0], 1e-6);
  EXPECT_NEAR(2.0, read_gt[1], 1e-6);
  EXPECT_NEAR(0.5, read_gt[2], 1e-6);
  EXPECT_NEAR(2000.0, read_gt[3], 1e-6);

  HFAEntry* xform_hdr =
      hfa->papoBand[0]->poNode->GetNamedChild("MapToPixelXForm");
  ASSERT_NE(nullptr, xform_hdr);
  HFAEntry* xform1 =
      HFAEntry::New(hfa.get(), "XForm1", "Efga_Polynomial", xform_hdr);
  ASSERT_NE(nullptr, xform1);
  EXPECT_EQ(FALSE, HFAGetGeoTransform(hfa.get(), read_gt.data()));
  ASSERT_EQ(CE_None, xform1->RemoveAndDestroy());

  HFAEntry* alt_mi = HFAEntry::New(hfa.get(), "AltMapInfo", "Eprj_MapInfo",
                                   hfa->papoBand[0]->poNode);
  ASSERT_NE(nullptr, alt_mi);
  // Eprj_MapInfo contains variable-length proName/units strings plus 6
  // doubles (120B).
  GByte* alt_data = alt_mi->MakeData(120);
  ASSERT_NE(nullptr, alt_data);
  std::memset(alt_data, 0, 120);
  alt_mi->SetPosition();
  alt_mi->SetStringField("proName", "AltProj");
  alt_mi->SetStringField("units", "meters");
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  hfa.reset(HFAOpen(kPath, "r+b"));
  ASSERT_NE(nullptr, hfa);
  const Eprj_MapInfo* out_mi = HFAGetMapInfo(hfa.get());
  ASSERT_NE(nullptr, out_mi);
  EXPECT_STREQ("AltProj", out_mi->proName);

  char pro_name[] = "TM";
  char sphere_name[] = "WGS 84";
  char datum_name[] = "WGS 84";
  Eprj_ProParameters pro = {
      .proType = EPRJ_INTERNAL,
      .proNumber = EPRJ_TRANSVERSE_MERCATOR,
      .proName = pro_name,
      .proParams = {0.0, 0.0, 0.9996},
      .proSpheroid = {.sphereName = sphere_name,
                      .a = 6378137.0,
                      .b = 6356752.314245},
  };
  Eprj_Datum datum = {.datumname = datum_name, .type = EPRJ_DATUM_PARAMETRIC};
  ASSERT_EQ(CE_None, HFASetProParameters(hfa.get(), &pro));
  ASSERT_EQ(CE_None, HFASetDatum(hfa.get(), &datum));

  HFAEntry* proj_node = hfa->papoBand[0]->poNode->GetNamedChild("Projection");
  ASSERT_NE(nullptr, proj_node);
  proj_node->SetIntField("proType", 99);
  HFAEntry* datum_node =
      hfa->papoBand[0]->poNode->GetNamedChild("Projection.Datum");
  ASSERT_NE(nullptr, datum_node);
  datum_node->SetIntField("type", 99);
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  hfa.reset(HFAOpen(kPath, "rb"));
  ASSERT_NE(nullptr, hfa);
  const Eprj_Datum* out_datum = nullptr;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, HFAGetProParameters(hfa.get()));
    out_datum = HFAGetDatum(hfa.get());
    CPLErrorReset();
  }
  ASSERT_NE(nullptr, out_datum);
  EXPECT_EQ(EPRJ_DATUM_NONE, out_datum->type);
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));
}

TEST(HfaOpenTest, XFormStackWriteReadAndEvaluate) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };

  constexpr char kPath[] = "/vsimem/hfaopen_test/xform.img";
  HfaPtr hfa(HFACreate(kPath, 32, 32, 1, EPT_u8, nullptr));
  ASSERT_NE(nullptr, hfa);

  // 1. Write polynomial XForm stack and reject unsupported orders/counts.
  Efga_Polynomial poly = {};
  poly.order = 2;
  Efga_Polynomial* ppoly = &poly;
  EXPECT_EQ(CE_None, HFAWriteXFormStack(hfa.get(), 1, 0, &ppoly, &ppoly));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, HFAWriteXFormStack(hfa.get(), 1, 1, &ppoly, &ppoly));
    poly.order = 1;
    EXPECT_EQ(CE_Failure, HFAWriteXFormStack(hfa.get(), 5, 1, &ppoly, &ppoly));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 2. Build order-2, order-3, and GM_PolyPair nodes in MapToPixelXForm.
  poly.polycoefvector[0] = 10.0;
  poly.polycoefvector[1] = 20.0;
  poly.polycoefmtx[0] = 2.0;
  poly.polycoefmtx[1] = 0.0;
  poly.polycoefmtx[2] = 0.0;
  poly.polycoefmtx[3] = 3.0;
  EXPECT_EQ(CE_None, HFAWriteXFormStack(hfa.get(), 1, 1, &ppoly, &ppoly));

  ASSERT_NO_FATAL_FAILURE(RegisterCustomTypes(
      hfa.get(),
      "{1:oEfga_Polynomial,forward,1:oEfga_Polynomial,reverse,}GM_PolyPair,."));
  HFAEntry* xform_hdr =
      hfa->papoBand[0]->poNode->GetNamedChild("MapToPixelXForm");
  ASSERT_NE(nullptr, xform_hdr);

  // Order-2 polynomial: 4 ints (16B) + polycoefmtx (24B hdr + 10*8B = 104B) +
  // polycoefvector (24B hdr + 2*8B = 40B) = 160B -> 180B with alignment.
  HFAEntry* xf2 =
      HFAEntry::New(hfa.get(), "XForm2", "Efga_Polynomial", xform_hdr);
  ASSERT_NE(nullptr, xf2);
  GByte* xf2_data = xf2->MakeData(180);
  ASSERT_NE(nullptr, xf2_data);
  std::memset(xf2_data, 0, 180);
  xf2->SetPosition();
  ASSERT_NO_FATAL_FAILURE(PopulatePolynomial(xf2, "", 2, 6, 5));

  // Order-3 polynomial: 16B + polycoefmtx (24B hdr + 18*8B = 168B) + 40B =
  // 224B -> 260B with alignment.
  HFAEntry* xf3 =
      HFAEntry::New(hfa.get(), "XForm3", "Efga_Polynomial", xform_hdr);
  ASSERT_NE(nullptr, xf3);
  GByte* xf3_data = xf3->MakeData(260);
  ASSERT_NE(nullptr, xf3_data);
  std::memset(xf3_data, 0, 260);
  xf3->SetPosition();
  ASSERT_NO_FATAL_FAILURE(PopulatePolynomial(xf3, "", 3, 10, 9));

  // Pair of two order-1 polynomials: 2 * (16B + 56B + 40B) = 224B -> 320B.
  HFAEntry* pair =
      HFAEntry::New(hfa.get(), "XFormPair", "GM_PolyPair", xform_hdr);
  ASSERT_NE(nullptr, pair);
  GByte* pair_data = pair->MakeData(320);
  ASSERT_NE(nullptr, pair_data);
  std::memset(pair_data, 0, 320);
  pair->SetPosition();
  ASSERT_NO_FATAL_FAILURE(PopulatePolynomial(pair, "forward", 1, 3, 2));
  ASSERT_NO_FATAL_FAILURE(PopulatePolynomial(pair, "reverse", 1, 3, 2));

  // 3. Read XForm stack, evaluate forward/reverse transforms, and reject
  // invalid
  //    order.
  Efga_Polynomial* fwd_raw = nullptr;
  Efga_Polynomial* rev_raw = nullptr;
  const int nSteps = HFAReadXFormStack(hfa.get(), &fwd_raw, &rev_raw);
  std::unique_ptr<Efga_Polynomial[], CplFreeDeleter> fwd(fwd_raw);
  std::unique_ptr<Efga_Polynomial[], CplFreeDeleter> rev(rev_raw);
  EXPECT_EQ(4, nSteps);
  double x = 5.0;
  double y = 7.0;
  EXPECT_EQ(TRUE, HFAEvaluateXFormStack(nSteps, 1, fwd.get(), &x, &y));
  EXPECT_EQ(TRUE, HFAEvaluateXFormStack(nSteps, 0, rev.get(), &x, &y));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    fwd[0].order = 99;
    EXPECT_EQ(FALSE, HFAEvaluateXFormStack(1, 1, fwd.get(), &x, &y));
    CPLErrorReset();
  }
}

TEST(HfaOpenTest, ElevationUnitDumpTreeAndCameraModel) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };
  absl::Cleanup cleanup_osr = [] { OSRCleanup(); };

  constexpr char kPath[] = "/vsimem/hfaopen_test/cam.img";
  HfaPtr hfa(HFACreate(kPath, 32, 32, 1, EPT_u8, nullptr));
  ASSERT_NE(nullptr, hfa);

  // 1. HFAReadElevationUnit.
  EXPECT_EQ(nullptr, HFAReadElevationUnit(hfa.get(), 5));
  EXPECT_EQ(nullptr, HFAReadElevationUnit(hfa.get(), 0));
  ASSERT_NO_FATAL_FAILURE(
      RegisterCustomTypes(hfa.get(), "{16:celevationUnit,}TestElevInfo,."));
  HFAEntry* elev_node = HFAEntry::New(hfa.get(), "Elevation_Info",
                                      "TestElevInfo", hfa->papoBand[0]->poNode);
  ASSERT_NE(nullptr, elev_node);
  ASSERT_NE(nullptr, elev_node->MakeData());
  elev_node->SetStringField("elevationUnit", "feet");
  EXPECT_STREQ("feet", HFAReadElevationUnit(hfa.get(), 0));

  // 2. HFADumpTree and HFADumpDictionary via open_memstream.
  const std::string dump_str = CaptureMemStream([&](FILE* fp) {
    HFADumpTree(hfa.get(), fp);
    HFADumpDictionary(hfa.get(), fp);
  });
  EXPECT_GT(dump_str.size(), 100u);

  // 3. HFAReadCameraModel with Camera_ModelX, outputProjection, and
  //    outputElevationInfo.
  EXPECT_EQ(nullptr, HFAReadCameraModel(hfa.get()));

  constexpr char kCamTypes[] =
      "{16:cstring,}TestCamStr,"
      "{16:cdatumname,1:ltype,}TestCamDatum,"
      "{16:csphereName,1:da,1:db,}TestCamSph,"
      "{1:oTestCamDatum,datum,1:oTestCamSph,proSpheroid,}TestCamEM,"
      "{1:lproType,1:lproNumber,16:cproName,1:lproZone,}TestCamPO,"
      "{1:oTestCamEM,earthModel,1:oTestCamPO,projectionObject,}OutProj,"
      "{16:cdatumname,16:ctype,}TestVertDatum,"
      "{1:oTestVertDatum,verticalDatum,16:celevationUnit,16:celevationType,}"
      "OutElev,"
      "{1:oTestCamStr,type,300:cMIFDictionary,0:pCMIFObject,}TestCamMIF,"
      "{16:cdirection,16:crefType,1:oTestCamStr,outputHorizontalUnits,"
      "1:oTestCamMIF,outputProjection,1:oTestCamMIF,outputElevationInfo,}"
      "Camera_ModelX,.";
  ASSERT_NO_FATAL_FAILURE(RegisterCustomTypes(hfa.get(), kCamTypes));

  HFAEntry* tmp_proj =
      HFAEntry::New(hfa.get(), "tmp_proj", "OutProj", hfa->poRoot);
  ASSERT_NE(nullptr, tmp_proj);
  ASSERT_NE(nullptr, tmp_proj->MakeData());
  tmp_proj->SetStringField("earthModel.datum.datumname", "WGS 84");
  tmp_proj->SetIntField("earthModel.datum.type", 0);
  tmp_proj->SetStringField("earthModel.proSpheroid.sphereName", "WGS 84");
  tmp_proj->SetDoubleField("earthModel.proSpheroid.a", 6378137.0);
  tmp_proj->SetDoubleField("earthModel.proSpheroid.b", 6356752.314245);
  tmp_proj->SetIntField("projectionObject.proType", EPRJ_INTERNAL);
  tmp_proj->SetIntField("projectionObject.proNumber", EPRJ_UTM);
  tmp_proj->SetStringField("projectionObject.proName", "UTM");
  tmp_proj->SetIntField("projectionObject.proZone", 11);

  HFAEntry* tmp_elev =
      HFAEntry::New(hfa.get(), "tmp_elev", "OutElev", hfa->poRoot);
  ASSERT_NE(nullptr, tmp_elev);
  ASSERT_NE(nullptr, tmp_elev->MakeData());
  tmp_elev->SetStringField("verticalDatum.datumname", "NAVD88");
  tmp_elev->SetStringField("verticalDatum.type", "orthometric");
  tmp_elev->SetStringField("elevationUnit", "meters");
  tmp_elev->SetStringField("elevationType", "height");

  // Camera_ModelX fixed prefix before outputProjection.MIFObject:
  //   16 direction + 16 refType + 16 outputHorizontalUnits = 48 bytes;
  //   outputProjection (16 type + 300 MIFDictionary) = 316 bytes;
  //   outputProjection.MIFObject 8B (size, offset) header is at 364..371,
  //   payload (OutProj = 80B) starts at 372..451.
  // Next, outputElevationInfo (16 type + 300 MIFDictionary) = 316 bytes ->
  //   452 + 316 = 768; MIFObject 8B header at 768..775, payload (OutElev =
  //   64B) at 776..839. Total = 840 bytes -> 860B buffer.
  HFAEntry* cam_hdr =
      HFAEntry::New(hfa.get(), "MapToPixelXForm", "Exfr_GenericXFormHeader",
                    hfa->papoBand[0]->poNode);
  ASSERT_NE(nullptr, cam_hdr);
  HFAEntry* cam = HFAEntry::New(hfa.get(), "XForm0", "Camera_ModelX", cam_hdr);
  ASSERT_NE(nullptr, cam);
  GByte* cam_data = cam->MakeData(860);
  ASSERT_NE(nullptr, cam_data);
  std::memset(cam_data, 0, 860);
  cam->SetStringField("direction", "forward");
  cam->SetStringField("refType", "ground");
  cam->SetStringField("outputHorizontalUnits.string", "meters");

  cam->SetStringField("outputProjection.type.string", "OutProj");
  cam->SetStringField(
      "outputProjection.MIFDictionary",
      "{16:cdatumname,1:ltype,}TestCamDatum,"
      "{16:csphereName,1:da,1:db,}TestCamSph,"
      "{1:oTestCamDatum,datum,1:oTestCamSph,proSpheroid,}TestCamEM,"
      "{1:lproType,1:lproNumber,16:cproName,1:lproZone,}TestCamPO,"
      "{1:oTestCamEM,earthModel,1:oTestCamPO,projectionObject,}OutProj,.");
  WriteHfaLe(cam_data, 364, GInt32{80});
  WriteHfaLe(cam_data, 368, GInt32{372});
  std::memcpy(cam_data + 372, tmp_proj->GetData(), 80);

  cam->SetStringField("outputElevationInfo.type.string", "OutElev");
  cam->SetStringField("outputElevationInfo.MIFDictionary",
                      "{16:cdatumname,16:ctype,}TestVertDatum,"
                      "{1:oTestVertDatum,verticalDatum,16:celevationUnit,16:"
                      "celevationType,}OutElev,.");
  WriteHfaLe(cam_data, 768, GInt32{64});
  WriteHfaLe(cam_data, 772, GInt32{776});
  std::memcpy(cam_data + 776, tmp_elev->GetData(), 64);

  ASSERT_EQ(CE_None, tmp_proj->RemoveAndDestroy());
  ASSERT_EQ(CE_None, tmp_elev->RemoveAndDestroy());

  StringListPtr cam_md(HFAReadCameraModel(hfa.get()), CSLDestroy);
  ASSERT_NE(nullptr, cam_md);
  EXPECT_STREQ("forward", CSLFetchNameValue(cam_md.get(), "direction"));
  EXPECT_STREQ("meters",
               CSLFetchNameValue(cam_md.get(), "outputHorizontalUnits"));
  EXPECT_STREQ("NAVD88",
               CSLFetchNameValue(cam_md.get(), "verticalDatum.datumname"));
  EXPECT_NE(nullptr, CSLFetchNameValue(cam_md.get(), "outputProjection"));
}

TEST(HfaOpenTest, MetadataAndHistogram) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };

  constexpr char kPath[] = "/vsimem/hfaopen_test/md.img";
  HfaPtr hfa(HFACreate(kPath, 32, 32, 1, EPT_u8, nullptr));
  ASSERT_NE(nullptr, hfa);

  // 1. Out-of-range band indices and null metadata no-op path.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, HFAGetMetadata(hfa.get(), -1));
    EXPECT_EQ(nullptr, HFAGetMetadata(hfa.get(), 5));
    CPLStringList dummy_md({"K=V"});
    EXPECT_EQ(CE_Failure, HFASetMetadata(hfa.get(), -1, dummy_md.List()));
    CPLErrorReset();
  }
  EXPECT_EQ(CE_None, HFASetMetadata(hfa.get(), 1, nullptr));

  // 2. Dataset-level (band 0) metadata round-trip.
  CPLStringList ds_md({"DS_KEY=DS_VAL"});
  EXPECT_EQ(CE_None, HFASetMetadata(hfa.get(), 0, ds_md.List()));
  StringListPtr read_ds_md(HFAGetMetadata(hfa.get(), 0), CSLDestroy);
  ASSERT_NE(nullptr, read_ds_md);
  EXPECT_STREQ("DS_VAL", CSLFetchNameValue(read_ds_md.get(), "DS_KEY"));

  // 3. Band-level statistics, histogram, layer type, and custom key metadata.
  CPLStringList band_md({
      "STATISTICS_MINIMUM=1.5",
      "STATISTICS_MAXIMUM=250.5",
      "STATISTICS_MEAN=100.0",
      "STATISTICS_MEDIAN=99.0",
      "STATISTICS_MODE=98.0",
      "STATISTICS_STDDEV=12.5",
      "STATISTICS_HISTONUMBINS=4",
      "STATISTICS_HISTOMIN=0.0",
      "STATISTICS_HISTOMAX=3.0",
      "STATISTICS_HISTOBINVALUES=10|20|30|40|",
      "STATISTICS_SKIPFACTORX=1",
      "STATISTICS_SKIPFACTORY=1",
      "STATISTICS_EXCLUDEDVALUES=0",
      "LAYER_TYPE=thematic",
      "CUSTOM_BAND_KEY=BAND_VAL",
  });
  EXPECT_EQ(CE_None, HFASetMetadata(hfa.get(), 1, band_md.List()));

  // 4. Reject overwriting existing Histogram bin column (real and integer).
  HFAEntry* histo_col =
      hfa->papoBand[0]->poNode->GetNamedChild("Descriptor_Table.Histogram");
  ASSERT_NE(nullptr, histo_col);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    CPLStringList upd_real_hist({"STATISTICS_HISTOBINVALUES=11|21|31|41|"});
    EXPECT_EQ(CE_Failure, HFASetMetadata(hfa.get(), 1, upd_real_hist.List()));

    histo_col->SetStringField("dataType", "integer");
    CPLStringList upd_int_hist({"STATISTICS_HISTOBINVALUES=5|6|7|8|"});
    EXPECT_EQ(CE_Failure, HFASetMetadata(hfa.get(), 1, upd_int_hist.List()));
    CPLErrorReset();
  }

  HFAEntry* rrd_info = HFAEntry::New(
      hfa.get(), "RRDInfoList", "Eimg_RRDNamesList", hfa->papoBand[0]->poNode);
  ASSERT_NE(nullptr, rrd_info);
  ASSERT_NE(nullptr, rrd_info->MakeData(64));
  rrd_info->SetPosition();
  rrd_info->SetStringField("algorithm.string", "NEAREST");

  StringListPtr read_band_md(HFAGetMetadata(hfa.get(), 1), CSLDestroy);
  ASSERT_NE(nullptr, read_band_md);
  EXPECT_STREQ("BAND_VAL",
               CSLFetchNameValue(read_band_md.get(), "CUSTOM_BAND_KEY"));

  // 5. Zero-width GDAL_MetaData column returns "" and numRows != 1 fails.

  HFAEntry* gdal_tbl = hfa->papoBand[0]->poNode->GetNamedChild("GDAL_MetaData");
  ASSERT_NE(nullptr, gdal_tbl);
  HFAEntry* empty_col =
      HFAEntry::New(hfa.get(), "EMPTY_KEY", "Edsc_Column", gdal_tbl);
  ASSERT_NE(nullptr, empty_col);
  empty_col->SetIntField("numRows", 1);
  empty_col->SetStringField("dataType", "string");
  empty_col->SetIntField("columnDataPtr", 10);
  empty_col->SetIntField("maxNumChars", 0);

  read_band_md.reset(HFAGetMetadata(hfa.get(), 1));
  ASSERT_NE(nullptr, read_band_md);
  EXPECT_STREQ("", CSLFetchNameValue(read_band_md.get(), "EMPTY_KEY"));

  // GDAL_MetaData tables require numRows == 1; otherwise HFAGetMetadata logs
  // via CPLDebug (compiled out) and returns nullptr without setting CPLError.
  gdal_tbl->SetIntField("numRows", 2);
  EXPECT_EQ(nullptr, HFAGetMetadata(hfa.get(), 1));
}

TEST(HfaOpenTest, PcsStructToOsrLocalExternalAndAllProjections) {
  absl::Cleanup cleanup_osr = [] { OSRCleanup(); };

  // 1. Verify datum/unit map pointers and local SRS fallback with MapInfo.
  EXPECT_NE(nullptr, HFAGetDatumMap());
  EXPECT_NE(nullptr, HFAGetUnitMap());

  EXPECT_EQ(nullptr, HFAPCSStructToOSR(nullptr, nullptr, nullptr, nullptr));

  char pro_local[] = "LocalGrid";
  char unit_feet[] = "feet";
  char unit_unknown[] = "some_unknown_unit";
  char unit_meters[] = "meters";

  Eprj_MapInfo mi = {.proName = pro_local, .units = unit_feet};
  auto srs_local = HFAPCSStructToOSR(nullptr, nullptr, &mi, nullptr);
  ASSERT_NE(nullptr, srs_local);
  EXPECT_TRUE(srs_local->IsLocal());

  mi.units = unit_unknown;
  auto srs_unknown_unit = HFAPCSStructToOSR(nullptr, nullptr, &mi, nullptr);
  ASSERT_NE(nullptr, srs_unknown_unit);
  mi.units = unit_meters;

  // 2. External projection handling (recognized NZMG vs unrecognized local).

  char exe_nzmg[] = "nzmg";
  char pro_nzmg[] = "NZMG";
  char sphere_intl[] = "International 1924";
  char datum_gd49[] = "Geodetic Datum 1949";
  char grid_nz[] = "nzgd2kgrid0005.gsb";
  char exe_custom[] = "custom_ext";

  Eprj_ProParameters pro = {
      .proType = EPRJ_EXTERNAL,
      .proExeName = exe_nzmg,
      .proName = pro_nzmg,
      .proSpheroid = {.sphereName = sphere_intl,
                      .a = 6378388.0,
                      .b = 6356911.946128},
  };
  Eprj_Datum datum = {
      .datumname = datum_gd49,
      .type = EPRJ_DATUM_GRID,
      .gridname = grid_nz,
  };

  auto srs_nzmg = HFAPCSStructToOSR(&datum, &pro, &mi, nullptr);
  ASSERT_NE(nullptr, srs_nzmg);
  EXPECT_STREQ("New_Zealand_Map_Grid", srs_nzmg->GetAttrValue("PROJECTION"));

  pro.proExeName = exe_custom;
  auto srs_ext_other = HFAPCSStructToOSR(&datum, &pro, nullptr, nullptr);
  ASSERT_NE(nullptr, srs_ext_other);
  EXPECT_TRUE(srs_ext_other->IsLocal());

  // 3. Pin every internal projection method number to its OGR WKT PROJECTION.
  struct PcsProjectionCase {
    int pro_number;
    const char* expected_projection;
    bool is_geographic = false;
    bool is_local = false;
  };
  // Note upstream quirks pinned here (hfaopen.cpp):
  // - EPRJ_HOTINE_OBLIQUE_MERCATOR_TWO_POINT_CENTER has literal typo
  //   "Hotine_Oblique_Mercator_Twp_Point_Center".
  // - EPRJ_PSEUDO_MERCATOR calls SetMercator(..., 1.0, ...) -> "Mercator_2SP".
  // - EPRJ_VERTICAL_NEAR_SIDE_PERSPECTIVE emits WKT2 "Vertical Perspective".
  constexpr PcsProjectionCase kProjectionCases[] = {
      {.pro_number = EPRJ_LATLONG,
       .expected_projection = nullptr,
       .is_geographic = true},
      {.pro_number = EPRJ_UTM, .expected_projection = "Transverse_Mercator"},
      {.pro_number = EPRJ_STATE_PLANE,
       .expected_projection = "Transverse_Mercator"},
      {.pro_number = EPRJ_ALBERS_CONIC_EQUAL_AREA,
       .expected_projection = "Albers_Conic_Equal_Area"},
      {.pro_number = EPRJ_LAMBERT_CONFORMAL_CONIC,
       .expected_projection = "Lambert_Conformal_Conic_2SP"},
      {.pro_number = EPRJ_MERCATOR, .expected_projection = "Mercator_2SP"},
      {.pro_number = EPRJ_POLAR_STEREOGRAPHIC,
       .expected_projection = "Polar_Stereographic"},
      {.pro_number = EPRJ_POLYCONIC, .expected_projection = "Polyconic"},
      {.pro_number = EPRJ_EQUIDISTANT_CONIC,
       .expected_projection = "Equidistant_Conic"},
      {.pro_number = EPRJ_TRANSVERSE_MERCATOR,
       .expected_projection = "Transverse_Mercator"},
      {.pro_number = EPRJ_GAUSS_KRUGER,
       .expected_projection = "Transverse_Mercator"},
      {.pro_number = EPRJ_STEREOGRAPHIC,
       .expected_projection = "Stereographic"},
      {.pro_number = EPRJ_LAMBERT_AZIMUTHAL_EQUAL_AREA,
       .expected_projection = "Lambert_Azimuthal_Equal_Area"},
      {.pro_number = EPRJ_AZIMUTHAL_EQUIDISTANT,
       .expected_projection = "Azimuthal_Equidistant"},
      {.pro_number = EPRJ_GNOMONIC, .expected_projection = "Gnomonic"},
      {.pro_number = EPRJ_ORTHOGRAPHIC, .expected_projection = "Orthographic"},
      {.pro_number = EPRJ_SINUSOIDAL, .expected_projection = "Sinusoidal"},
      {.pro_number = EPRJ_PLATE_CARREE,
       .expected_projection = "Equirectangular"},
      {.pro_number = EPRJ_EQUIRECTANGULAR,
       .expected_projection = "Equirectangular"},
      {.pro_number = EPRJ_EQUIDISTANT_CYLINDRICAL,
       .expected_projection = "Equirectangular"},
      {.pro_number = EPRJ_MILLER_CYLINDRICAL,
       .expected_projection = "Miller_Cylindrical"},
      {.pro_number = EPRJ_VANDERGRINTEN,
       .expected_projection = "VanDerGrinten"},
      {.pro_number = EPRJ_HOTINE_OBLIQUE_MERCATOR,
       .expected_projection = "Hotine_Oblique_Mercator"},
      {.pro_number = EPRJ_HOTINE_OBLIQUE_MERCATOR_AZIMUTH_CENTER,
       .expected_projection = "Hotine_Oblique_Mercator_Azimuth_Center"},
      {.pro_number = EPRJ_ROBINSON, .expected_projection = "Robinson"},
      {.pro_number = EPRJ_MOLLWEIDE, .expected_projection = "Mollweide"},
      {.pro_number = EPRJ_GALL_STEREOGRAPHIC,
       .expected_projection = "Gall_Stereographic"},
      {.pro_number = EPRJ_ECKERT_I, .expected_projection = "Eckert_I"},
      {.pro_number = EPRJ_ECKERT_II, .expected_projection = "Eckert_II"},
      {.pro_number = EPRJ_ECKERT_III, .expected_projection = "Eckert_III"},
      {.pro_number = EPRJ_ECKERT_IV, .expected_projection = "Eckert_IV"},
      {.pro_number = EPRJ_ECKERT_V, .expected_projection = "Eckert_V"},
      {.pro_number = EPRJ_ECKERT_VI, .expected_projection = "Eckert_VI"},
      {.pro_number = EPRJ_CASSINI, .expected_projection = "Cassini_Soldner"},
      {.pro_number = EPRJ_TWO_POINT_EQUIDISTANT,
       .expected_projection = "Two_Point_Equidistant"},
      {.pro_number = EPRJ_STEREOGRAPHIC_EXTENDED,
       .expected_projection = "Stereographic"},
      {.pro_number = EPRJ_BONNE, .expected_projection = "Bonne"},
      {.pro_number = EPRJ_LOXIMUTHAL, .expected_projection = "Loximuthal"},
      {.pro_number = EPRJ_QUARTIC_AUTHALIC,
       .expected_projection = "Quartic_Authalic"},
      {.pro_number = EPRJ_WINKEL_I, .expected_projection = "Winkel_I"},
      {.pro_number = EPRJ_WINKEL_II, .expected_projection = "Winkel_II"},
      {.pro_number = EPRJ_BEHRMANN,
       .expected_projection = "Cylindrical_Equal_Area"},
      {.pro_number = EPRJ_KROVAK, .expected_projection = "Krovak"},
      {.pro_number = EPRJ_DOUBLE_STEREOGRAPHIC,
       .expected_projection = "Oblique_Stereographic"},
      {.pro_number = EPRJ_AITOFF, .expected_projection = "Aitoff"},
      {.pro_number = EPRJ_CRASTER_PARABOLIC,
       .expected_projection = "Craster_Parabolic"},
      {.pro_number = EPRJ_CYLINDRICAL_EQUAL_AREA,
       .expected_projection = "Cylindrical_Equal_Area"},
      {.pro_number = EPRJ_FLAT_POLAR_QUARTIC,
       .expected_projection = "Flat_Polar_Quartic"},
      {.pro_number = EPRJ_TIMES, .expected_projection = "Times"},
      {.pro_number = EPRJ_WINKEL_TRIPEL,
       .expected_projection = "Winkel_Tripel"},
      {.pro_number = EPRJ_HAMMER_AITOFF,
       .expected_projection = "Hammer_Aitoff"},
      {.pro_number = EPRJ_VERTICAL_NEAR_SIDE_PERSPECTIVE,
       .expected_projection = "Vertical Perspective"},
      {.pro_number = EPRJ_HOTINE_OBLIQUE_MERCATOR_TWO_POINT_CENTER,
       .expected_projection = "Hotine_Oblique_Mercator_Twp_Point_Center"},
      {.pro_number = EPRJ_HOTINE_OBLIQUE_MERCATOR_TWO_POINT_NATURAL_ORIGIN,
       .expected_projection =
           "Hotine_Oblique_Mercator_Two_Point_Natural_Origin"},
      {.pro_number = EPRJ_LAMBERT_CONFORMAL_CONIC_1SP,
       .expected_projection = "Lambert_Conformal_Conic_1SP"},
      {.pro_number = EPRJ_MERCATOR_VARIANT_A,
       .expected_projection = "Mercator (variant A)"},
      {.pro_number = EPRJ_PSEUDO_MERCATOR,
       .expected_projection = "Mercator_2SP"},
      {.pro_number = EPRJ_HOTINE_OBLIQUE_MERCATOR_VARIANT_A,
       .expected_projection = "Hotine_Oblique_Mercator"},
      {.pro_number = EPRJ_TRANSVERSE_MERCATOR_SOUTH_ORIENTATED,
       .expected_projection = "Transverse_Mercator_South_Orientated"},
      {.pro_number = 999, .expected_projection = nullptr, .is_local = true},
  };

  char pro_test[] = "TestPro";
  char sphere_wgs84[] = "WGS 84";
  for (const PcsProjectionCase& tc : kProjectionCases) {
    Eprj_ProParameters p = {
        .proType = EPRJ_INTERNAL,
        .proNumber = tc.pro_number,
        .proName = pro_test,
        .proZone = (tc.pro_number == EPRJ_STATE_PLANE) ? 3101 : 11,
        .proParams = {0.0, 0.0, 0.9996, 0.5, -1.5, 0.6, 500000.0, 0.0, 0.2, 0.4,
                      -1.2, 0.7, 1.0},
        .proSpheroid = {.sphereName = sphere_wgs84,
                        .a = 6378137.0,
                        .b = 6356752.314245},
    };
    Eprj_Datum d = {.datumname = sphere_wgs84, .type = EPRJ_DATUM_NONE};

    auto srs = HFAPCSStructToOSR(&d, &p, &mi, nullptr);
    ASSERT_NE(nullptr, srs) << "pro_num=" << tc.pro_number;
    if (tc.is_geographic) {
      EXPECT_TRUE(srs->IsGeographic()) << "pro_num=" << tc.pro_number;
    } else if (tc.is_local) {
      EXPECT_TRUE(srs->IsLocal()) << "pro_num=" << tc.pro_number;
    } else {
      EXPECT_STREQ(tc.expected_projection, srs->GetAttrValue("PROJECTION"))
          << "pro_num=" << tc.pro_number;
    }
  }
}

TEST(HfaOpenTest, PcsStructToOsrDatumsHarnStatePlaneAndTowgs84) {
  absl::Cleanup cleanup_osr = [] { OSRCleanup(); };
  absl::Cleanup reset_tls_opts = [] {
    CPLSetThreadLocalConfigOption("OSR_STRIP_TOWGS84", nullptr);
  };

  char pro_local[] = "LocalGrid";
  char unit_meters[] = "meters";
  Eprj_MapInfo mi = {.proName = pro_local, .units = unit_meters};

  char pro_tm[] = "TM_Custom";
  char pro_sp_fallback[] = "SP_Fallback";
  char datum_nad27[] = "NAD27";
  char datum_nad83[] = "NAD83";
  char datum_custom[] = "Custom_Parametric_Datum";
  char datum_harn[] = "HARN";

  // 1. Default spheroid fallback, NAD27, and NAD83 parametric datums.
  Eprj_ProParameters p = {
      .proType = EPRJ_INTERNAL,
      .proNumber = EPRJ_TRANSVERSE_MERCATOR,
      .proName = pro_tm,
      .proParams = {0.0, 0.0, 0.9996, 0.0, -1.5, 0.7},
  };
  EXPECT_NE(nullptr, HFAPCSStructToOSR(nullptr, &p, nullptr, nullptr));

  Eprj_Datum d = {.datumname = datum_nad27, .type = EPRJ_DATUM_PARAMETRIC};
  EXPECT_NE(nullptr, HFAPCSStructToOSR(&d, &p, &mi, nullptr));

  d.datumname = datum_nad83;
  EXPECT_NE(nullptr, HFAPCSStructToOSR(&d, &p, &mi, nullptr));

  // 2. Parametric TOWGS84 parameters on a well-known datum ("WGS 84") are
  //    preserved when OSR_STRIP_TOWGS84=NO and stripped under YES/default.
  char datum_wgs84[] = "WGS 84";
  d.datumname = datum_wgs84;
  d.params[0] = 10.0;
  d.params[1] = 20.0;
  d.params[2] = 30.0;
  CPLSetThreadLocalConfigOption("OSR_STRIP_TOWGS84", "NO");
  auto srs_keep_towgs = HFAPCSStructToOSR(&d, &p, &mi, nullptr);
  ASSERT_NE(nullptr, srs_keep_towgs);
  std::array<double, 7> towgs = {};
  EXPECT_EQ(OGRERR_NONE, srs_keep_towgs->GetTOWGS84(towgs.data()));
  EXPECT_DOUBLE_EQ(10.0, towgs[0]);

  CPLSetThreadLocalConfigOption("OSR_STRIP_TOWGS84", "YES");
  auto srs_strip_towgs = HFAPCSStructToOSR(&d, &p, &mi, nullptr);
  ASSERT_NE(nullptr, srs_strip_towgs);
  EXPECT_NE(OGRERR_NONE, srs_strip_towgs->GetTOWGS84(towgs.data()));
  CPLSetThreadLocalConfigOption("OSR_STRIP_TOWGS84", nullptr);

  // 3. Unmapped parametric datum and HARN datum translations.
  d.datumname = datum_custom;
  EXPECT_NE(nullptr, HFAPCSStructToOSR(&d, &p, &mi, nullptr));

  d.datumname = datum_harn;
  EXPECT_NE(nullptr, HFAPCSStructToOSR(&d, &p, &mi, nullptr));
  p.proNumber = EPRJ_LAMBERT_CONFORMAL_CONIC;
  EXPECT_NE(nullptr, HFAPCSStructToOSR(&d, &p, &mi, nullptr));

  // 4. Unknown State Plane zone falls back to local SRS cleanly.
  p.proNumber = EPRJ_STATE_PLANE;
  p.proZone = 9999;
  p.proName = pro_sp_fallback;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_NE(nullptr, HFAPCSStructToOSR(nullptr, &p, &mi, nullptr));
    CPLErrorReset();
  }
}

// TODO: b/335317901 - Cover remaining corrupted dictionary/header error paths
// in hfaopen.cpp.

}  // namespace
}  // namespace autotest2
