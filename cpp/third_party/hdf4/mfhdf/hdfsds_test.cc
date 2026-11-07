// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests reading legacy HDF4 SDS (NDG and SDG) objects and converting them
// into netCDF / multi-file SDS representations (hdfsds.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/mfhdf/src/hdfsds.c

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

// GoogleTest defines SUCCEED() and FAIL() as macros; undefine them before
// including HDF4 headers to avoid macro collision under --config=no_modules.
// clang-format off
#ifdef SUCCEED
#undef SUCCEED
#endif
#ifdef FAIL
#undef FAIL
#endif

#include "third_party/hdf4/hdf/src/df.h"
#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/mfan.h"
#include "third_party/hdf4/mfhdf/src/mfhdf.h"
#include "third_party/hdf4/mfhdf/src/nc_priv.h"

extern "C" {
uint8 *hdf_get_pred_str_attr(NC *handle, uint16 stratt_tag, uint16 satt_ref,
                             int null_count);
}
// clang-format on

namespace hdf4 {
namespace {

using ::testing::Eq;
using ::testing::Ge;
using ::testing::IsNull;
using ::testing::Ne;
using ::testing::NotNull;
using ::testing::StrEq;

std::string GetTempFilePath(std::string_view filename) {
  return (std::filesystem::path(::testing::TempDir()) / filename).string();
}

class HdfsdsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_ncopts_ = H4_ncopts;
    saved_ncerr_ = H4_ncerr;
    H4_ncopts = 0;
    H4_ncerr = NC_NOERR;
    HEclear();
  }

  void TearDown() override {
    DFSDrestart();
    (void)DFSDclear();
    H4_ncopts = saved_ncopts_;
    H4_ncerr = saved_ncerr_;
    HEclear();
  }

 private:
  int saved_ncopts_ = 0;
  int saved_ncerr_ = 0;
};

TEST_F(HdfsdsTest, EmptyHdfFileAndSafeDirectHelperEdgeCases) {
  // 1. Empty HDF4 file (no _HDF_CDF, no NDGs/SDGs) opened via SD API
  const std::string empty_path = GetTempFilePath("hdfsds_empty.hdf");
  absl::Cleanup remove_empty = [&empty_path] {
    std::filesystem::remove(empty_path);
  };

  const int32_t fid = Hopen(empty_path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  ASSERT_THAT(Hclose(fid), Eq(SUCCEED));

  const int32_t sd_id = SDstart(empty_path.c_str(), DFACC_READ);
  ASSERT_THAT(sd_id, Ne(FAIL));
  int32_t n_datasets = -1;
  int32_t n_gattrs = -1;
  EXPECT_THAT(SDfileinfo(sd_id, &n_datasets, &n_gattrs), Eq(SUCCEED));
  EXPECT_THAT(n_datasets, Eq(0));
  EXPECT_THAT(n_gattrs, Eq(0));
  EXPECT_THAT(SDend(sd_id), Eq(SUCCEED));

  // 2. Non-existent file and truncated corrupt file
  EXPECT_THAT(SDstart("/nonexistent_hdfsds_file.hdf", DFACC_READ), Eq(FAIL));

  const std::string trunc_path = GetTempFilePath("hdfsds_trunc.hdf");
  absl::Cleanup remove_trunc = [&trunc_path] {
    std::filesystem::remove(trunc_path);
  };
  FILE* fp = fopen(trunc_path.c_str(), "wb");
  ASSERT_THAT(fp, NotNull());
  const uint8_t bad_magic[2] = {0x0E, 0x03};
  fwrite(bad_magic, 1, sizeof(bad_magic), fp);
  fclose(fp);
  EXPECT_THAT(SDstart(trunc_path.c_str(), DFACC_READ), Eq(FAIL));

  // 3. Direct hdf_read_sds_cdf with null handle pointer (L1607-1609)
  NC* null_handle = nullptr;
  EXPECT_THAT(hdf_read_sds_cdf(nullptr, &null_handle), Eq(FAIL));

  // 4. Direct hdf_get_pred_str_attr edge cases
  NC valid_nc{};
  valid_nc.hdf_file = Hopen(empty_path.c_str(), DFACC_READ, 0);
  ASSERT_THAT(valid_nc.hdf_file, Ne(FAIL));
  absl::Cleanup close_vfid = [&valid_nc] {
    ASSERT_THAT(Hclose(valid_nc.hdf_file), Eq(SUCCEED));
  };

  // satt_ref == 0 -> returns nullptr (L421)
  EXPECT_THAT(hdf_get_pred_str_attr(&valid_nc, DFTAG_SDL, 0, 0), IsNull());

  // Non-existent satt_ref = 9999 -> returns nullptr (L428-431)
  EXPECT_THAT(hdf_get_pred_str_attr(&valid_nc, DFTAG_SDL, 9999, 0), IsNull());
}

TEST_F(HdfsdsTest, RichMultiTypeDfsdDatasetsReadViaSdApi) {
  const std::string multi_path = GetTempFilePath("hdfsds_rich_dfsd.hdf");
  absl::Cleanup remove_multi = [&multi_path] {
    std::filesystem::remove(multi_path);
  };

  // Dataset 1: 3D DFNT_FLOAT32 with LUF, dim scales, range, and calibration
  {
    int32_t dims[3] = {2, 3, 4};
    ASSERT_THAT(DFSDsetdims(3, dims), Eq(SUCCEED));
    ASSERT_THAT(DFSDsetNT(DFNT_FLOAT32), Eq(SUCCEED));
    ASSERT_THAT(
        DFSDsetdatastrs("Temperature", "Kelvin", "F6.2", "Polar Coordinates"),
        Eq(SUCCEED));

    // Dim 0: full LUF + scale data
    float scale0[2] = {100.0f, 200.0f};
    ASSERT_THAT(DFSDsetdimstrs(1, "alt", "km", "F5.1"), Eq(SUCCEED));
    ASSERT_THAT(DFSDsetdimscale(1, 2, scale0), Eq(SUCCEED));

    // Dim 1: partial LUF ("lat", no units, no format) without scale
    ASSERT_THAT(DFSDsetdimstrs(2, "lat", nullptr, nullptr), Eq(SUCCEED));

    // Dim 2: scale data without dimstrs (exercise scalebuf && scalebuf[dim])
    float scale2[4] = {10.0f, 20.0f, 30.0f, 40.0f};
    ASSERT_THAT(DFSDsetdimscale(3, 4, scale2), Eq(SUCCEED));

    float max_val = 500.0f;
    float min_val = -50.0f;
    ASSERT_THAT(DFSDsetrange(&max_val, &min_val), Eq(SUCCEED));

    // Calibration: writes 36-byte DFNT_FLOAT64 DFTAG_CAL
    ASSERT_THAT(DFSDsetcal(2.0, 0.1, 10.0, 0.5, DFNT_INT16), Eq(SUCCEED));

    std::vector<float> data1(2 * 3 * 4, 1.5f);
    ASSERT_THAT(DFSDadddata(multi_path.c_str(), 3, dims, data1.data()),
                Eq(SUCCEED));
  }

  // Dataset 2: 2D DFNT_INT32 | DFNT_LITEND without dim scales/LUF
  {
    DFSDrestart();
    ASSERT_THAT(DFSDclear(), Eq(SUCCEED));
    int32_t dims[2] = {2, 2};
    ASSERT_THAT(DFSDsetdims(2, dims), Eq(SUCCEED));
    ASSERT_THAT(DFSDsetNT(DFNT_INT32 | DFNT_LITEND), Eq(SUCCEED));
    std::vector<int32_t> data2(2 * 2, 42);
    ASSERT_THAT(DFSDadddata(multi_path.c_str(), 2, dims, data2.data()),
                Eq(SUCCEED));
  }

  // Dataset 3: 1D DFNT_INT16 | DFNT_NATIVE
  {
    DFSDrestart();
    ASSERT_THAT(DFSDclear(), Eq(SUCCEED));
    int32_t dims[1] = {3};
    ASSERT_THAT(DFSDsetdims(1, dims), Eq(SUCCEED));
    ASSERT_THAT(DFSDsetNT(DFNT_INT16 | DFNT_NATIVE), Eq(SUCCEED));
    int16_t data3[3] = {1, 2, 3};
    ASSERT_THAT(DFSDadddata(multi_path.c_str(), 1, dims, data3), Eq(SUCCEED));
  }

  // Verify all datasets when opened via the SD API
  const int32_t sd_id = SDstart(multi_path.c_str(), DFACC_READ);
  ASSERT_THAT(sd_id, Ne(FAIL));
  absl::Cleanup close_sd = [&sd_id] { EXPECT_THAT(SDend(sd_id), Eq(SUCCEED)); };

  int32_t n_datasets = 0;
  int32_t n_gattrs = 0;
  ASSERT_THAT(SDfileinfo(sd_id, &n_datasets, &n_gattrs), Eq(SUCCEED));
  EXPECT_THAT(n_datasets, Ge(3));

  // Inspect Dataset 1 (3D dataset, at variable index 2)
  const int32_t sds0 = SDselect(sd_id, 2);
  ASSERT_THAT(sds0, Ne(FAIL));
  absl::Cleanup close_sds0 = [&sds0] {
    EXPECT_THAT(SDendaccess(sds0), Eq(SUCCEED));
  };
  EXPECT_THAT(SDiscoordvar(sds0), Eq(FALSE));

  char label[64] = "";
  char unit[64] = "";
  char format[64] = "";
  char coordsys[64] = "";
  EXPECT_THAT(SDgetdatastrs(sds0, label, unit, format, coordsys, 64),
              Eq(SUCCEED));
  EXPECT_THAT(label, StrEq("Temperature"));
  EXPECT_THAT(unit, StrEq("Kelvin"));
  EXPECT_THAT(format, StrEq("F6.2"));
  EXPECT_THAT(coordsys, StrEq("Polar Coordinates"));

  float r_max = 0.0f;
  float r_min = 0.0f;
  EXPECT_THAT(SDgetrange(sds0, &r_max, &r_min), Eq(SUCCEED));
  EXPECT_THAT(r_max, Eq(500.0f));
  EXPECT_THAT(r_min, Eq(-50.0f));

  double cal = 0.0;
  double cal_err = 0.0;
  double offset = 0.0;
  double offset_err = 0.0;
  int32_t cal_nt = 0;
  EXPECT_THAT(SDgetcal(sds0, &cal, &cal_err, &offset, &offset_err, &cal_nt),
              Eq(SUCCEED));
  EXPECT_THAT(cal, Eq(2.0));
  EXPECT_THAT(cal_nt, Eq(DFNT_INT16));

  // Dim 0 of SDS 0: scale values and dimstrs
  const int32_t dim0 = SDgetdimid(sds0, 0);
  ASSERT_THAT(dim0, Ne(FAIL));
  float read_scale0[2] = {0.0f, 0.0f};
  EXPECT_THAT(SDgetdimscale(dim0, read_scale0), Eq(SUCCEED));
  EXPECT_THAT(read_scale0[0], Eq(100.0f));
  EXPECT_THAT(read_scale0[1], Eq(200.0f));
  char dlabel[64] = "";
  char dunit[64] = "";
  char dformat[64] = "";
  EXPECT_THAT(SDgetdimstrs(dim0, dlabel, dunit, dformat, 64), Eq(SUCCEED));
  EXPECT_THAT(dlabel, StrEq("alt"));

  // Dim 1 of SDS 0: dim with no scale
  const int32_t dim1 = SDgetdimid(sds0, 1);
  ASSERT_THAT(dim1, Ne(FAIL));

  // Dim 2 of SDS 0: scale without dimstrs
  const int32_t dim2 = SDgetdimid(sds0, 2);
  ASSERT_THAT(dim2, Ne(FAIL));
  float read_scale2[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  EXPECT_THAT(SDgetdimscale(dim2, read_scale2), Eq(SUCCEED));
  EXPECT_THAT(read_scale2[0], Eq(10.0f));
  EXPECT_THAT(read_scale2[3], Eq(40.0f));

  // Inspect Dataset 2 (variable index 5)
  const int32_t sds1 = SDselect(sd_id, 5);
  ASSERT_THAT(sds1, Ne(FAIL));
  absl::Cleanup close_sds1 = [&sds1] {
    EXPECT_THAT(SDendaccess(sds1), Eq(SUCCEED));
  };
  int32_t sds1_type = 0;
  int32_t sds1_rank = 0;
  int32_t sds1_dims[H4_MAX_VAR_DIMS] = {0};
  int32_t sds1_nattrs = 0;
  char sds1_name[H4_MAX_NC_NAME + 1] = "";
  EXPECT_THAT(SDgetinfo(sds1, sds1_name, &sds1_rank, sds1_dims, &sds1_type,
                        &sds1_nattrs),
              Eq(SUCCEED));
  EXPECT_THAT(sds1_type, Eq(DFNT_INT32 | DFNT_LITEND));

  // Inspect Dataset 3 (variable index 7)
  const int32_t sds2 = SDselect(sd_id, 7);
  ASSERT_THAT(sds2, Ne(FAIL));
  absl::Cleanup close_sds2 = [&sds2] {
    EXPECT_THAT(SDendaccess(sds2), Eq(SUCCEED));
  };
  EXPECT_THAT(SDgetinfo(sds2, sds1_name, &sds1_rank, sds1_dims, &sds1_type,
                        &sds1_nattrs),
              Eq(SUCCEED));
  EXPECT_THAT(sds1_type, Eq(DFNT_INT16 | DFNT_LITEND));
}

TEST_F(HdfsdsTest, SdgTableAndDynamicArrayReallocOver100Datasets) {
  const std::string over100_path = GetTempFilePath("hdfsds_over100.hdf");
  absl::Cleanup remove_over100 = [&over100_path] {
    std::filesystem::remove(over100_path);
  };

  int32_t dim[1] = {1};
  float val = 7.0f;

  // Dataset 0: 1D DFNT_FLOAT32 without dimstrs
  // After Dataset 0: current_dim = 1 (max_thangs grew 1 -> 2 at L1284),
  // current_var = 1
  ASSERT_EQ(SUCCEED, DFSDclear());
  ASSERT_EQ(SUCCEED, DFSDsetdims(1, dim));
  ASSERT_EQ(SUCCEED, DFSDsetNT(DFNT_FLOAT32));
  ASSERT_EQ(SUCCEED, DFSDadddata(over100_path.c_str(), 1, dim, &val));

  // Dataset 1: 2D DFNT_FLOAT32 with lRef == 0 -> creates 2 coordvars + 1 SDS
  // dim 0: current_dim = 2 (2 < 4)
  // coordvar 0: current_var = 3 (3 < 4)
  // dim 1: current_dim = 3 (3 < 4)
  // coordvar 1: current_var becomes 4 == max_thangs at L1386 -> hits L1388-1397
  // max_thangs *= 2! SDS: current_var = 5 (5 < 8)
  int32_t dims2[2] = {2, 2};
  float vals2[4] = {1.0f, 2.0f, 3.0f, 4.0f};
  ASSERT_EQ(SUCCEED, DFSDclear());
  ASSERT_EQ(SUCCEED, DFSDsetdims(2, dims2));
  ASSERT_EQ(SUCCEED, DFSDsetNT(DFNT_FLOAT32));
  ASSERT_EQ(SUCCEED, DFSDadddata(over100_path.c_str(), 2, dims2, vals2));

  // Dataset 2: 1D DFNT_FLOAT32 with dimstrs
  ASSERT_EQ(SUCCEED, DFSDclear());
  ASSERT_EQ(SUCCEED, DFSDsetdims(1, dim));
  ASSERT_EQ(SUCCEED, DFSDsetNT(DFNT_FLOAT32));
  ASSERT_EQ(SUCCEED, DFSDsetdimstrs(1, "dim2", nullptr, nullptr));
  ASSERT_EQ(SUCCEED, DFSDadddata(over100_path.c_str(), 1, dim, &val));

  // Write 100 more small datasets with DFNT_FLOAT32 (total 103 NDGs with
  // DFTAG_SDLNK) Exceeds SDG_MAX_INITIAL = 100 -> hits sdgTable realloc at
  // L156-162!
  for (int i = 0; i < 100; i++) {
    ASSERT_EQ(SUCCEED, DFSDclear());
    ASSERT_EQ(SUCCEED, DFSDsetdims(1, dim));
    ASSERT_EQ(SUCCEED, DFSDsetNT(DFNT_FLOAT32));
    ASSERT_EQ(SUCCEED, DFSDadddata(over100_path.c_str(), 1, dim, &val));
  }

  // Opening via SD API runs hdf_register_seen_sdg reallocating sdgTable
  const int32_t sd_id = SDstart(over100_path.c_str(), DFACC_READ);
  ASSERT_NE(FAIL, sd_id);
  absl::Cleanup close_sd = [&sd_id] { EXPECT_EQ(SUCCEED, SDend(sd_id)); };

  int32_t n_datasets = 0;
  int32_t n_gattrs = 0;
  ASSERT_EQ(SUCCEED, SDfileinfo(sd_id, &n_datasets, &n_gattrs));
  EXPECT_GE(n_datasets, 103);

  const int32_t first_sds = SDselect(sd_id, 0);
  ASSERT_NE(FAIL, first_sds);
  EXPECT_EQ(SUCCEED, SDendaccess(first_sds));

  const int32_t last_sds = SDselect(sd_id, n_datasets - 1);
  ASSERT_NE(FAIL, last_sds);
  EXPECT_EQ(SUCCEED, SDendaccess(last_sds));
}

TEST_F(HdfsdsTest, AnnotationsLegacyFloat32CalEmptyCoordsysAndStandaloneSdg) {
  // Test File A: NDG with annotations, 18-byte DFTAG_CAL, empty DFTAG_SDC,
  // and an extra standalone DFTAG_SDG.
  const std::string file_a = GetTempFilePath("hdfsds_file_a.hdf");
  absl::Cleanup remove_a = [&file_a] { std::filesystem::remove(file_a); };

  {
    ASSERT_EQ(SUCCEED, DFSDclear());
    int32_t dim[1] = {3};
    ASSERT_EQ(SUCCEED, DFSDsetdims(1, dim));
    ASSERT_EQ(SUCCEED, DFSDsetNT(DFNT_FLOAT32));
    float vals[3] = {11.0f, 22.0f, 33.0f};
    ASSERT_EQ(SUCCEED, DFSDadddata(file_a.c_str(), 1, dim, vals));
    ASSERT_EQ(SUCCEED, DFSDclear());

    const int32_t fid = Hopen(file_a.c_str(), DFACC_RDWR, 0);
    ASSERT_NE(FAIL, fid);

    // Find the NDG ref
    const int32_t aid = Hstartread(fid, DFTAG_NDG, DFREF_WILDCARD);
    ASSERT_NE(FAIL, aid);
    uint16_t ndg_tag = 0;
    uint16_t ndg_ref = 0;
    ASSERT_EQ(SUCCEED, HQuerytagref(aid, &ndg_tag, &ndg_ref));
    ASSERT_EQ(SUCCEED, Hendaccess(aid));

    // Add 2 label and 2 desc annotations to the NDG
    const int32_t an_id = ANstart(fid);
    ASSERT_NE(FAIL, an_id);
    const int32_t ann_l1 = ANcreate(an_id, DFTAG_NDG, ndg_ref, AN_DATA_LABEL);
    ASSERT_NE(FAIL, ann_l1);
    ASSERT_EQ(SUCCEED, ANwriteann(ann_l1, "label1", 6));
    ASSERT_EQ(SUCCEED, ANendaccess(ann_l1));

    const int32_t ann_l2 = ANcreate(an_id, DFTAG_NDG, ndg_ref, AN_DATA_LABEL);
    ASSERT_NE(FAIL, ann_l2);
    ASSERT_EQ(SUCCEED, ANwriteann(ann_l2, "label2", 6));
    ASSERT_EQ(SUCCEED, ANendaccess(ann_l2));

    const int32_t ann_d1 = ANcreate(an_id, DFTAG_NDG, ndg_ref, AN_DATA_DESC);
    ASSERT_NE(FAIL, ann_d1);
    ASSERT_EQ(SUCCEED, ANwriteann(ann_d1, "desc1", 5));
    ASSERT_EQ(SUCCEED, ANendaccess(ann_d1));

    const int32_t ann_d2 = ANcreate(an_id, DFTAG_NDG, ndg_ref, AN_DATA_DESC);
    ASSERT_NE(FAIL, ann_d2);
    ASSERT_EQ(SUCCEED, ANwriteann(ann_d2, "desc2", 5));
    ASSERT_EQ(SUCCEED, ANendaccess(ann_d2));
    ASSERT_EQ(SUCCEED, ANend(an_id));

    // Read existing NDG members
    const int read_gid = DFdiread(fid, DFTAG_NDG, ndg_ref);
    ASSERT_GE(read_gid, 0);
    std::vector<std::pair<uint16_t, uint16_t>> di_pairs;
    uint16_t t = 0;
    uint16_t r = 0;
    while (DFdiget(read_gid, &t, &r) == SUCCEED) {
      di_pairs.emplace_back(t, r);
    }

    // Create 18-byte DFTAG_CAL: 4 big-endian float32s + 1 big-endian int16
    const uint16_t cal18_ref = Hnewref(fid);
    uint8_t cal18_bytes[18] = {0};
    uint8_t* pcal = cal18_bytes;
    float cal_vals[4] = {2.0f, 0.1f, 5.0f, 0.2f};
    for (int c = 0; c < 4; c++) {
      (void)DFKconvert(reinterpret_cast<void*>(&cal_vals[c]), pcal,
                       DFNT_FLOAT32, 1, DFACC_WRITE, 0, 0);
      pcal += 4;
    }
    int16_t cal_type = DFNT_INT16;
    (void)DFKconvert(reinterpret_cast<void*>(&cal_type), pcal, DFNT_INT16, 1,
                     DFACC_WRITE, 0, 0);

    ASSERT_EQ(sizeof(cal18_bytes),
              Hputelement(fid, DFTAG_CAL, cal18_ref, cal18_bytes,
                          sizeof(cal18_bytes)));

    // Create 1-byte empty DFTAG_SDC ("\0")
    const uint16_t sdc_ref = Hnewref(fid);
    const char empty_sdc[1] = {'\0'};
    ASSERT_EQ(1, Hputelement(fid, DFTAG_SDC, sdc_ref,
                             reinterpret_cast<const uint8_t*>(empty_sdc), 1));

    // Delete existing NDG DD so a larger one can be written at same ref
    ASSERT_EQ(SUCCEED, Hdeldd(fid, DFTAG_NDG, ndg_ref));

    // Rebuild the NDG with CAL and SDC added
    const int write_gid = DFdisetup(static_cast<int>(di_pairs.size()) + 2);
    ASSERT_GE(write_gid, 0);
    for (const auto& [tag, ref] : di_pairs) {
      ASSERT_EQ(SUCCEED, DFdiput(write_gid, tag, ref));
    }
    ASSERT_EQ(SUCCEED, DFdiput(write_gid, DFTAG_CAL, cal18_ref));
    ASSERT_EQ(SUCCEED, DFdiput(write_gid, DFTAG_SDC, sdc_ref));
    ASSERT_GE(DFdiwrite(fid, write_gid, DFTAG_NDG, ndg_ref), 0);

    // Now write a standalone legacy DFTAG_SDG into the file
    const uint16_t sdg_ref = Hnewref(fid);
    const int sdg_gid = DFdisetup(10);
    ASSERT_GE(sdg_gid, 0);

    const uint16_t nt_ref = Hnewref(fid);
    const uint8_t nt_bytes[4] = {DFNT_VERSION, DFNT_FLOAT32, 32, DFNTF_IEEE};
    ASSERT_EQ(sizeof(nt_bytes),
              Hputelement(fid, DFTAG_NT, nt_ref, nt_bytes, sizeof(nt_bytes)));
    ASSERT_EQ(SUCCEED, DFdiput(sdg_gid, DFTAG_NT, nt_ref));

    const uint16_t sdd_ref = Hnewref(fid);
    uint8_t sdd_bytes[14] = {0};
    uint8_t* psdd = sdd_bytes;
    UINT16ENCODE(psdd, 1);  // rank = 1
    INT32ENCODE(psdd, 3);   // dim = 3
    UINT16ENCODE(psdd, DFTAG_NT);
    UINT16ENCODE(psdd, nt_ref);
    UINT16ENCODE(psdd, DFTAG_NT);
    UINT16ENCODE(psdd, nt_ref);
    ASSERT_EQ(sizeof(sdd_bytes), Hputelement(fid, DFTAG_SDD, sdd_ref, sdd_bytes,
                                             sizeof(sdd_bytes)));
    ASSERT_EQ(SUCCEED, DFdiput(sdg_gid, DFTAG_SDD, sdd_ref));

    const uint16_t sd_ref = Hnewref(fid);
    float float_data[3] = {1.1f, 2.2f, 3.3f};
    uint8_t sd_bytes[12] = {0};
    (void)DFKconvert(float_data, sd_bytes, DFNT_FLOAT32, 3, DFACC_WRITE, 0, 0);
    ASSERT_EQ(sizeof(sd_bytes),
              Hputelement(fid, DFTAG_SD, sd_ref, sd_bytes, sizeof(sd_bytes)));
    ASSERT_EQ(SUCCEED, DFdiput(sdg_gid, DFTAG_SD, sd_ref));

    // Dummy SDLNK and MT tags
    ASSERT_EQ(SUCCEED, DFdiput(sdg_gid, DFTAG_SDLNK, sdg_ref));
    ASSERT_EQ(SUCCEED, DFdiput(sdg_gid, DFTAG_MT, sdg_ref));

    ASSERT_GE(DFdiwrite(fid, sdg_gid, DFTAG_SDG, sdg_ref), 0);
    ASSERT_EQ(SUCCEED, Hclose(fid));
  }

  // Open File A with SD API and verify
  {
    const int32_t sd_id = SDstart(file_a.c_str(), DFACC_READ);
    ASSERT_NE(FAIL, sd_id);
    absl::Cleanup close_sd = [&sd_id] { EXPECT_EQ(SUCCEED, SDend(sd_id)); };

    int32_t n_datasets = 0;
    int32_t n_gattrs = 0;
    ASSERT_EQ(SUCCEED, SDfileinfo(sd_id, &n_datasets, &n_gattrs));
    // hdfsds.c:1258 sets new_dim = FAIL (-1, truthy); with lRef == 0,
    // coordinate variables fakeDim0 (index 0) and fakeDim1 (index 2) are
    // created, yielding 4 total datasets in File A.
    EXPECT_EQ(4, n_datasets);

    // NDG dataset is at index 1 (fakeDim0 is at index 0)
    const int32_t sds0 = SDselect(sd_id, 1);
    ASSERT_NE(FAIL, sds0);
    absl::Cleanup close_sds0 = [&sds0] {
      EXPECT_EQ(SUCCEED, SDendaccess(sds0));
    };
    EXPECT_EQ(FALSE, SDiscoordvar(sds0));

    // Check annotations
    const int32_t idx_rem1 = SDfindattr(sds0, "remarks-1");
    EXPECT_GE(idx_rem1, 0);
    const int32_t idx_lbl1 = SDfindattr(sds0, "anno_label-1");
    EXPECT_GE(idx_lbl1, 0);

    // Check 18-byte calibration attributes
    const int32_t idx_scale = SDfindattr(sds0, "scale_factor");
    ASSERT_GE(idx_scale, 0);
    int32_t a_type = 0, a_count = 0;
    std::array<char, H4_MAX_NC_NAME + 1> a_name{};
    EXPECT_EQ(SUCCEED,
              SDattrinfo(sds0, idx_scale, a_name.data(), &a_type, &a_count));
    EXPECT_EQ(DFNT_FLOAT32, a_type);
    float s_factor = 0.0f;
    EXPECT_EQ(SUCCEED, SDreadattr(sds0, idx_scale, &s_factor));
    EXPECT_EQ(2.0f, s_factor);

    const int32_t idx_cnt = SDfindattr(sds0, "calibrated_nt");
    ASSERT_GE(idx_cnt, 0);
    int16_t cnt_val = 0;
    EXPECT_EQ(SUCCEED, SDreadattr(sds0, idx_cnt, &cnt_val));
    EXPECT_EQ(DFNT_INT16, cnt_val);

    // Standalone SDG dataset is at index 3 (fakeDim1 is at index 2)
    const int32_t sds1 = SDselect(sd_id, 3);
    ASSERT_NE(FAIL, sds1);
    absl::Cleanup close_sds1 = [&sds1] {
      EXPECT_EQ(SUCCEED, SDendaccess(sds1));
    };
    EXPECT_EQ(FALSE, SDiscoordvar(sds1));
    float read_floats[3] = {0.0f, 0.0f, 0.0f};
    int32_t start[1] = {0};
    int32_t edges[1] = {3};
    EXPECT_EQ(SUCCEED, SDreaddata(sds1, start, nullptr, edges, read_floats));
    EXPECT_EQ(1.1f, read_floats[0]);
  }

  // Test File B: Standalone DFTAG_SDG in a file with ZERO NDGs (sdgTable ==
  // NULL) Use ntstring[0] != DFNT_VERSION and DFKgetPNSC to exercise
  // hdf_check_nt branch
  const std::string file_b = GetTempFilePath("hdfsds_file_b.hdf");
  absl::Cleanup remove_b = [&file_b] { std::filesystem::remove(file_b); };

  {
    const int32_t fid = Hopen(file_b.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));

    const uint16_t sdg_ref = Hnewref(fid);
    const int sdg_gid = DFdisetup(10);
    ASSERT_THAT(sdg_gid, Ge(0));

    const uint16_t nt_ref = Hnewref(fid);
    const uint8_t nt_bytes[4] = {0, DFNT_CHAR8, 8, DFNTF_IEEE};
    ASSERT_THAT(Hputelement(fid, DFTAG_NT, nt_ref, nt_bytes, sizeof(nt_bytes)),
                Eq(sizeof(nt_bytes)));
    ASSERT_THAT(DFdiput(sdg_gid, DFTAG_NT, nt_ref), Eq(SUCCEED));

    const uint16_t sdd_ref = Hnewref(fid);
    uint8_t sdd_bytes[14] = {0};
    uint8_t* psdd = sdd_bytes;
    UINT16ENCODE(psdd, 1);  // rank = 1
    INT32ENCODE(psdd, 2);   // dim = 2
    UINT16ENCODE(psdd, DFTAG_NT);
    UINT16ENCODE(psdd, nt_ref);
    UINT16ENCODE(psdd, DFTAG_NT);
    UINT16ENCODE(psdd, nt_ref);
    ASSERT_THAT(
        Hputelement(fid, DFTAG_SDD, sdd_ref, sdd_bytes, sizeof(sdd_bytes)),
        Eq(sizeof(sdd_bytes)));
    ASSERT_THAT(DFdiput(sdg_gid, DFTAG_SDD, sdd_ref), Eq(SUCCEED));

    const uint16_t sd_ref = Hnewref(fid);
    char char_data[2] = {'A', 'B'};
    ASSERT_THAT(Hputelement(fid, DFTAG_SD, sd_ref,
                            reinterpret_cast<const uint8_t*>(char_data),
                            sizeof(char_data)),
                Eq(sizeof(char_data)));
    ASSERT_THAT(DFdiput(sdg_gid, DFTAG_SD, sd_ref), Eq(SUCCEED));

    ASSERT_THAT(DFdiwrite(fid, sdg_gid, DFTAG_SDG, sdg_ref), Ge(0));
    ASSERT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  {
    const int32_t sd_id = SDstart(file_b.c_str(), DFACC_READ);
    ASSERT_THAT(sd_id, Ne(FAIL));
    absl::Cleanup close_sd = [&sd_id] {
      EXPECT_THAT(SDend(sd_id), Eq(SUCCEED));
    };
    int32_t n_datasets = 0;
    int32_t n_gattrs = 0;
    ASSERT_THAT(SDfileinfo(sd_id, &n_datasets, &n_gattrs), Eq(SUCCEED));
    EXPECT_THAT(n_datasets, Eq(2));
    const int32_t sds = SDselect(sd_id, 1);
    ASSERT_NE(FAIL, sds);
    int32_t sds_type = 0, sds_rank = 0, sds_dims[1] = {0}, sds_nattrs = 0;
    std::array<char, H4_MAX_NC_NAME + 1> sds_name{};
    EXPECT_EQ(SUCCEED, SDgetinfo(sds, sds_name.data(), &sds_rank, sds_dims,
                                 &sds_type, &sds_nattrs));
    EXPECT_EQ(DFNT_CHAR8 | DFNT_NATIVE, sds_type);
    EXPECT_EQ(SUCCEED, SDendaccess(sds));
  }
}

TEST_F(HdfsdsTest, MalformedNdgAndSddErrorBranchesFailCleanly) {
  auto write_ndg_file =
      [](std::string_view filename,
         const std::vector<std::pair<uint16_t, uint16_t>>& di_pairs) {
        const std::string path = GetTempFilePath(filename);
        const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
        EXPECT_NE(FAIL, fid);
        if (fid != FAIL) {
          const uint16_t ndg_ref = Hnewref(fid);
          const int gid = DFdisetup(static_cast<int>(di_pairs.size()));
          EXPECT_GE(gid, 0);
          for (const auto& [tag, ref] : di_pairs) {
            EXPECT_EQ(SUCCEED, DFdiput(gid, tag, ref));
          }
          EXPECT_GE(DFdiwrite(fid, gid, DFTAG_NDG, ndg_ref), 0);
          EXPECT_EQ(SUCCEED, Hclose(fid));
        }
        return path;
      };

  // 1. Missing DFTAG_SDD ref 999 (L1039, L1555-1597, L1619)
  {
    const std::string p =
        write_ndg_file("err_sdd_missing.hdf", {{DFTAG_SDD, 999}});
    absl::Cleanup rm = [&p] { std::filesystem::remove(p); };
    EXPECT_EQ(FAIL, SDstart(p.c_str(), DFACC_READ));
  }

  // 2. 1-byte truncated DFTAG_SDD (hdf_read_rank Hread fail L227, 244, 1045)
  {
    const std::string p = GetTempFilePath("err_sdd_trunc1.hdf");
    absl::Cleanup rm = [&p] { std::filesystem::remove(p); };
    const int32_t fid = Hopen(p.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    const uint16_t sdd_ref = Hnewref(fid);
    const uint8_t one_byte = 0;
    ASSERT_EQ(1, Hputelement(fid, DFTAG_SDD, sdd_ref, &one_byte, 1));
    const uint16_t ndg_ref = Hnewref(fid);
    const int gid = DFdisetup(1);
    ASSERT_GE(gid, 0);
    ASSERT_EQ(SUCCEED, DFdiput(gid, DFTAG_SDD, sdd_ref));
    ASSERT_GE(DFdiwrite(fid, gid, DFTAG_NDG, ndg_ref), 0);
    ASSERT_EQ(SUCCEED, Hclose(fid));
    EXPECT_EQ(FAIL, SDstart(p.c_str(), DFACC_READ));
  }

  // 3. DFTAG_SDD with rank = 0 (hdf_read_rank DFE_RANGE L239)
  {
    const std::string p = GetTempFilePath("err_sdd_rank0.hdf");
    absl::Cleanup rm = [&p] { std::filesystem::remove(p); };
    const int32_t fid = Hopen(p.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    const uint16_t sdd_ref = Hnewref(fid);
    const uint8_t sdd_bytes[2] = {0, 0};
    ASSERT_EQ(2, Hputelement(fid, DFTAG_SDD, sdd_ref, sdd_bytes, 2));
    const uint16_t ndg_ref = Hnewref(fid);
    const int gid = DFdisetup(1);
    ASSERT_GE(gid, 0);
    ASSERT_EQ(SUCCEED, DFdiput(gid, DFTAG_SDD, sdd_ref));
    ASSERT_GE(DFdiwrite(fid, gid, DFTAG_NDG, ndg_ref), 0);
    ASSERT_EQ(SUCCEED, Hclose(fid));
    EXPECT_EQ(FAIL, SDstart(p.c_str(), DFACC_READ));
  }

  // 4. DFTAG_SDD with rank = 2 and only 4 dim bytes (hdf_read_dimsizes Hread
  // fail L276, 293, 1066)
  {
    const std::string p = GetTempFilePath("err_sdd_dimtrunc.hdf");
    absl::Cleanup rm = [&p] { std::filesystem::remove(p); };
    const int32_t fid = Hopen(p.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    const uint16_t sdd_ref = Hnewref(fid);
    uint8_t sdd_bytes[6] = {0};
    uint8_t* psdd = sdd_bytes;
    UINT16ENCODE(psdd, 2);  // rank = 2
    INT32ENCODE(psdd, 4);   // only 1 dim provided
    ASSERT_EQ(6, Hputelement(fid, DFTAG_SDD, sdd_ref, sdd_bytes, 6));
    const uint16_t ndg_ref = Hnewref(fid);
    const int gid = DFdisetup(1);
    ASSERT_GE(gid, 0);
    ASSERT_EQ(SUCCEED, DFdiput(gid, DFTAG_SDD, sdd_ref));
    ASSERT_GE(DFdiwrite(fid, gid, DFTAG_NDG, ndg_ref), 0);
    ASSERT_EQ(SUCCEED, Hclose(fid));
    EXPECT_EQ(FAIL, SDstart(p.c_str(), DFACC_READ));
  }

  // 5. DFTAG_SDD with rank = 1, dim_size = -5 (hdf_read_dimsizes DFE_RANGE
  // L284)
  {
    const std::string p = GetTempFilePath("err_sdd_negdim.hdf");
    absl::Cleanup rm = [&p] { std::filesystem::remove(p); };
    const int32_t fid = Hopen(p.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    const uint16_t sdd_ref = Hnewref(fid);
    uint8_t sdd_bytes[6] = {0};
    uint8_t* psdd = sdd_bytes;
    UINT16ENCODE(psdd, 1);  // rank = 1
    INT32ENCODE(psdd, -5);  // negative dim
    ASSERT_EQ(6, Hputelement(fid, DFTAG_SDD, sdd_ref, sdd_bytes, 6));
    const uint16_t ndg_ref = Hnewref(fid);
    const int gid = DFdisetup(1);
    ASSERT_GE(gid, 0);
    ASSERT_EQ(SUCCEED, DFdiput(gid, DFTAG_SDD, sdd_ref));
    ASSERT_GE(DFdiwrite(fid, gid, DFTAG_NDG, ndg_ref), 0);
    ASSERT_EQ(SUCCEED, Hclose(fid));
    EXPECT_EQ(FAIL, SDstart(p.c_str(), DFACC_READ));
  }

  // 6. DFTAG_SDD with rank = 1, dim_size = 2 and truncated NT (hdf_read_NT
  // Hread fail L325, 340, 1074)
  {
    const std::string p = GetTempFilePath("err_sdd_truncnt.hdf");
    absl::Cleanup rm = [&p] { std::filesystem::remove(p); };
    const int32_t fid = Hopen(p.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    const uint16_t sdd_ref = Hnewref(fid);
    uint8_t sdd_bytes[8] = {0};
    uint8_t* psdd = sdd_bytes;
    UINT16ENCODE(psdd, 1);         // rank = 1
    INT32ENCODE(psdd, 2);          // dim = 2
    UINT16ENCODE(psdd, DFTAG_NT);  // only 2 bytes of tag/ref
    ASSERT_EQ(8, Hputelement(fid, DFTAG_SDD, sdd_ref, sdd_bytes, 8));
    const uint16_t ndg_ref = Hnewref(fid);
    const int gid = DFdisetup(1);
    ASSERT_GE(gid, 0);
    ASSERT_EQ(SUCCEED, DFdiput(gid, DFTAG_SDD, sdd_ref));
    ASSERT_GE(DFdiwrite(fid, gid, DFTAG_NDG, ndg_ref), 0);
    ASSERT_EQ(SUCCEED, Hclose(fid));
    EXPECT_EQ(FAIL, SDstart(p.c_str(), DFACC_READ));
  }

  // 7. DFTAG_SDD with rank = 1, dim_size = 2 and missing DFTAG_NT ref 999
  // (hdf_read_NT Hgetelement fail L334)
  {
    const std::string p = GetTempFilePath("err_sdd_ntmissing.hdf");
    absl::Cleanup rm = [&p] { std::filesystem::remove(p); };
    const int32_t fid = Hopen(p.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    const uint16_t sdd_ref = Hnewref(fid);
    uint8_t sdd_bytes[10] = {0};
    uint8_t* psdd = sdd_bytes;
    UINT16ENCODE(psdd, 1);  // rank = 1
    INT32ENCODE(psdd, 2);   // dim = 2
    UINT16ENCODE(psdd, DFTAG_NT);
    UINT16ENCODE(psdd, 999);  // non-existent NT ref
    ASSERT_EQ(10, Hputelement(fid, DFTAG_SDD, sdd_ref, sdd_bytes, 10));
    const uint16_t ndg_ref = Hnewref(fid);
    const int gid = DFdisetup(1);
    ASSERT_GE(gid, 0);
    ASSERT_EQ(SUCCEED, DFdiput(gid, DFTAG_SDD, sdd_ref));
    ASSERT_GE(DFdiwrite(fid, gid, DFTAG_NDG, ndg_ref), 0);
    ASSERT_EQ(SUCCEED, Hclose(fid));
    EXPECT_EQ(FAIL, SDstart(p.c_str(), DFACC_READ));
  }

  // 8. DFTAG_SDD with dataset DFTAG_NT having invalid type 250 (hdf_unmap_type
  // FAIL L1078)
  {
    const std::string p = GetTempFilePath("err_sdd_badnt.hdf");
    absl::Cleanup rm = [&p] { std::filesystem::remove(p); };
    const int32_t fid = Hopen(p.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    const uint16_t nt_ref = Hnewref(fid);
    const uint8_t nt_bytes[4] = {DFNT_VERSION, 250, 32, DFNTF_IEEE};
    ASSERT_EQ(4, Hputelement(fid, DFTAG_NT, nt_ref, nt_bytes, 4));
    const uint16_t sdd_ref = Hnewref(fid);
    uint8_t sdd_bytes[10] = {0};
    uint8_t* psdd = sdd_bytes;
    UINT16ENCODE(psdd, 1);
    INT32ENCODE(psdd, 2);
    UINT16ENCODE(psdd, DFTAG_NT);
    UINT16ENCODE(psdd, nt_ref);
    ASSERT_EQ(10, Hputelement(fid, DFTAG_SDD, sdd_ref, sdd_bytes, 10));
    const uint16_t ndg_ref = Hnewref(fid);
    const int gid = DFdisetup(2);
    ASSERT_GE(gid, 0);
    ASSERT_EQ(SUCCEED, DFdiput(gid, DFTAG_NT, nt_ref));
    ASSERT_EQ(SUCCEED, DFdiput(gid, DFTAG_SDD, sdd_ref));
    ASSERT_GE(DFdiwrite(fid, gid, DFTAG_NDG, ndg_ref), 0);
    ASSERT_EQ(SUCCEED, Hclose(fid));
    EXPECT_EQ(FAIL, SDstart(p.c_str(), DFACC_READ));
  }

  // 9. DFTAG_SDD with dataset DFTAG_NT foreign machine type (hdf_check_nt FAIL
  // L194, 1083)
  {
    const std::string p = GetTempFilePath("err_sdd_foreign_nt.hdf");
    absl::Cleanup rm = [&p] { std::filesystem::remove(p); };
    const int32_t fid = Hopen(p.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    const uint16_t nt_ref = Hnewref(fid);
    const uint8_t nt_bytes[4] = {DFNT_VERSION, DFNT_FLOAT32, 32, 0x77};
    ASSERT_EQ(4, Hputelement(fid, DFTAG_NT, nt_ref, nt_bytes, 4));
    const uint16_t sdd_ref = Hnewref(fid);
    uint8_t sdd_bytes[10] = {0};
    uint8_t* psdd = sdd_bytes;
    UINT16ENCODE(psdd, 1);
    INT32ENCODE(psdd, 2);
    UINT16ENCODE(psdd, DFTAG_NT);
    UINT16ENCODE(psdd, nt_ref);
    ASSERT_EQ(10, Hputelement(fid, DFTAG_SDD, sdd_ref, sdd_bytes, 10));
    const uint16_t ndg_ref = Hnewref(fid);
    const int gid = DFdisetup(2);
    ASSERT_GE(gid, 0);
    ASSERT_EQ(SUCCEED, DFdiput(gid, DFTAG_NT, nt_ref));
    ASSERT_EQ(SUCCEED, DFdiput(gid, DFTAG_SDD, sdd_ref));
    ASSERT_GE(DFdiwrite(fid, gid, DFTAG_NDG, ndg_ref), 0);
    ASSERT_EQ(SUCCEED, Hclose(fid));
    EXPECT_EQ(FAIL, SDstart(p.c_str(), DFACC_READ));
  }

  // 10. Valid dataset DFTAG_NT, missing scale DFTAG_NT ref 999 (scale
  // hdf_read_NT fail L1089)
  {
    const std::string p = GetTempFilePath("err_sdd_scalent_missing.hdf");
    absl::Cleanup rm = [&p] { std::filesystem::remove(p); };
    const int32_t fid = Hopen(p.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    const uint16_t nt_ref = Hnewref(fid);
    const uint8_t nt_bytes[4] = {DFNT_VERSION, DFNT_FLOAT32, 32, DFNTF_IEEE};
    ASSERT_EQ(4, Hputelement(fid, DFTAG_NT, nt_ref, nt_bytes, 4));
    const uint16_t sdd_ref = Hnewref(fid);
    uint8_t sdd_bytes[14] = {0};
    uint8_t* psdd = sdd_bytes;
    UINT16ENCODE(psdd, 1);
    INT32ENCODE(psdd, 2);
    UINT16ENCODE(psdd, DFTAG_NT);
    UINT16ENCODE(psdd, nt_ref);
    UINT16ENCODE(psdd, DFTAG_NT);
    UINT16ENCODE(psdd, 999);  // missing scale NT
    ASSERT_EQ(14, Hputelement(fid, DFTAG_SDD, sdd_ref, sdd_bytes, 14));
    const uint16_t ndg_ref = Hnewref(fid);
    const int gid = DFdisetup(2);
    ASSERT_GE(gid, 0);
    ASSERT_EQ(SUCCEED, DFdiput(gid, DFTAG_NT, nt_ref));
    ASSERT_EQ(SUCCEED, DFdiput(gid, DFTAG_SDD, sdd_ref));
    ASSERT_GE(DFdiwrite(fid, gid, DFTAG_NDG, ndg_ref), 0);
    ASSERT_EQ(SUCCEED, Hclose(fid));
    EXPECT_EQ(FAIL, SDstart(p.c_str(), DFACC_READ));
  }

  // 11. Valid dataset DFTAG_NT, foreign scale DFTAG_NT (scale hdf_check_nt fail
  // L1098)
  {
    const std::string p = GetTempFilePath("err_sdd_scalent_foreign.hdf");
    absl::Cleanup rm = [&p] { std::filesystem::remove(p); };
    const int32_t fid = Hopen(p.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    const uint16_t nt_ref = Hnewref(fid);
    const uint8_t nt_bytes[4] = {DFNT_VERSION, DFNT_FLOAT32, 32, DFNTF_IEEE};
    ASSERT_EQ(4, Hputelement(fid, DFTAG_NT, nt_ref, nt_bytes, 4));
    const uint16_t scale_nt_ref = Hnewref(fid);
    const uint8_t scale_nt_bytes[4] = {DFNT_VERSION, DFNT_FLOAT32, 32, 0x77};
    ASSERT_EQ(4, Hputelement(fid, DFTAG_NT, scale_nt_ref, scale_nt_bytes, 4));
    const uint16_t sdd_ref = Hnewref(fid);
    uint8_t sdd_bytes[14] = {0};
    uint8_t* psdd = sdd_bytes;
    UINT16ENCODE(psdd, 1);
    INT32ENCODE(psdd, 2);
    UINT16ENCODE(psdd, DFTAG_NT);
    UINT16ENCODE(psdd, nt_ref);
    UINT16ENCODE(psdd, DFTAG_NT);
    UINT16ENCODE(psdd, scale_nt_ref);
    ASSERT_EQ(14, Hputelement(fid, DFTAG_SDD, sdd_ref, sdd_bytes, 14));
    const uint16_t ndg_ref = Hnewref(fid);
    const int gid = DFdisetup(3);
    ASSERT_GE(gid, 0);
    ASSERT_EQ(SUCCEED, DFdiput(gid, DFTAG_NT, nt_ref));
    ASSERT_EQ(SUCCEED, DFdiput(gid, DFTAG_NT, scale_nt_ref));
    ASSERT_EQ(SUCCEED, DFdiput(gid, DFTAG_SDD, sdd_ref));
    ASSERT_GE(DFdiwrite(fid, gid, DFTAG_NDG, ndg_ref), 0);
    ASSERT_EQ(SUCCEED, Hclose(fid));
    EXPECT_EQ(FAIL, SDstart(p.c_str(), DFACC_READ));
  }

  // 12. Valid dataset DFTAG_NT, unknown scale DFTAG_NT type 250 without
  // DFTAG_SDL (L1326)
  {
    const std::string p = GetTempFilePath("err_sdd_bad_scaletype.hdf");
    absl::Cleanup rm = [&p] { std::filesystem::remove(p); };
    const int32_t fid = Hopen(p.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    const uint16_t nt_ref = Hnewref(fid);
    const uint8_t nt_bytes[4] = {DFNT_VERSION, DFNT_FLOAT32, 32, DFNTF_IEEE};
    ASSERT_EQ(4, Hputelement(fid, DFTAG_NT, nt_ref, nt_bytes, 4));
    const uint16_t scale_nt_ref = Hnewref(fid);
    const uint8_t scale_nt_bytes[4] = {DFNT_VERSION, 250, 32, DFNTF_IEEE};
    ASSERT_EQ(4, Hputelement(fid, DFTAG_NT, scale_nt_ref, scale_nt_bytes, 4));
    const uint16_t sdd_ref = Hnewref(fid);
    uint8_t sdd_bytes[14] = {0};
    uint8_t* psdd = sdd_bytes;
    UINT16ENCODE(psdd, 1);
    INT32ENCODE(psdd, 2);
    UINT16ENCODE(psdd, DFTAG_NT);
    UINT16ENCODE(psdd, nt_ref);
    UINT16ENCODE(psdd, DFTAG_NT);
    UINT16ENCODE(psdd, scale_nt_ref);
    ASSERT_EQ(14, Hputelement(fid, DFTAG_SDD, sdd_ref, sdd_bytes, 14));
    const uint16_t ndg_ref = Hnewref(fid);
    const int gid = DFdisetup(3);
    ASSERT_GE(gid, 0);
    ASSERT_EQ(SUCCEED, DFdiput(gid, DFTAG_NT, nt_ref));
    ASSERT_EQ(SUCCEED, DFdiput(gid, DFTAG_NT, scale_nt_ref));
    ASSERT_EQ(SUCCEED, DFdiput(gid, DFTAG_SDD, sdd_ref));
    ASSERT_GE(DFdiwrite(fid, gid, DFTAG_NDG, ndg_ref), 0);
    ASSERT_EQ(SUCCEED, Hclose(fid));
    const int32_t bad_sd = SDstart(p.c_str(), DFACC_READ);
    EXPECT_EQ(FAIL, bad_sd);
    if (bad_sd != FAIL) {
      EXPECT_EQ(SUCCEED, SDend(bad_sd));
    }
  }

  // 13. Missing DFTAG_SDC ref 999 (L367, 1130)
  {
    const std::string p =
        write_ndg_file("err_sdc_missing.hdf", {{DFTAG_SDC, 999}});
    absl::Cleanup rm = [&p] { std::filesystem::remove(p); };
    EXPECT_EQ(FAIL, SDstart(p.c_str(), DFACC_READ));
  }

  // 14. Missing DFTAG_CAL ref 999 (L1150)
  {
    const std::string p =
        write_ndg_file("err_cal_missing.hdf", {{DFTAG_CAL, 999}});
    absl::Cleanup rm = [&p] { std::filesystem::remove(p); };
    EXPECT_EQ(FAIL, SDstart(p.c_str(), DFACC_READ));
  }

  // 15. Missing DFTAG_SDM ref 999 (L1176)
  {
    const std::string p =
        write_ndg_file("err_sdm_missing.hdf", {{DFTAG_SDM, 999}});
    absl::Cleanup rm = [&p] { std::filesystem::remove(p); };
    EXPECT_EQ(FAIL, SDstart(p.c_str(), DFACC_READ));
  }

  // 16. Missing DFTAG_SDLNK ref 999 (L1191)
  {
    const std::string p =
        write_ndg_file("err_sdlnk_missing.hdf", {{DFTAG_SDLNK, 999}});
    absl::Cleanup rm = [&p] { std::filesystem::remove(p); };
    EXPECT_EQ(FAIL, SDstart(p.c_str(), DFACC_READ));
  }

  // 17. Error cleanup in done: with current_var > 0 and current_attr > 0
  // (L1584-1597)
  {
    const std::string p = GetTempFilePath("err_cleanup_attrs_and_vars.hdf");
    absl::Cleanup rm = [&p] { std::filesystem::remove(p); };
    int32_t d1[1] = {2};
    float v1[2] = {1.0f, 2.0f};
    ASSERT_EQ(SUCCEED, DFSDclear());
    ASSERT_EQ(SUCCEED, DFSDsetdims(1, d1));
    ASSERT_EQ(SUCCEED, DFSDsetNT(DFNT_FLOAT32));
    ASSERT_EQ(SUCCEED, DFSDputdata(p.c_str(), 1, d1, v1));

    const int32_t fid = Hopen(p.c_str(), DFACC_WRITE, 0);
    ASSERT_NE(FAIL, fid);
    const uint16_t sdc_ref = Hnewref(fid);
    const char coordsys[] = "polar";
    ASSERT_EQ(sizeof(coordsys),
              Hputelement(fid, DFTAG_SDC, sdc_ref,
                          reinterpret_cast<const uint8_t*>(coordsys),
                          sizeof(coordsys)));
    const uint16_t ndg_ref = Hnewref(fid);
    const int gid = DFdisetup(2);
    ASSERT_GE(gid, 0);
    ASSERT_EQ(SUCCEED, DFdiput(gid, DFTAG_SDC, sdc_ref));
    ASSERT_EQ(SUCCEED, DFdiput(gid, DFTAG_CAL, 999));
    ASSERT_GE(DFdiwrite(fid, gid, DFTAG_NDG, ndg_ref), 0);
    ASSERT_EQ(SUCCEED, Hclose(fid));

    const int32_t bad_sd = SDstart(p.c_str(), DFACC_READ);
    EXPECT_EQ(FAIL, bad_sd);
    if (bad_sd != FAIL) {
      EXPECT_EQ(SUCCEED, SDend(bad_sd));
    }
  }
}

// TODO(schwehr): Uncovered lines in hdfsds.c (~90.6% covered) are OOM error
// branches, legacy 3D/irregular-coordinate conversion cases, and corrupted
// DFTAG_NT/SDD record paths.

}  // namespace
}  // namespace hdf4
