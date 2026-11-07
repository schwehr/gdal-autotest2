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
// Tests for the HDF4 / NetCDF-2 Multi-file Scientific Data Set (SD) interface
// (mfhdf/src/mfsd.c).
//
// Build caveats:
// - SZIP compression is disabled in google3 (#undef H4_HAVE_LIBSZ).
// - comp_info.deflate_setting.level is used instead of .deflate.level.
// - FillValueAttr ("_FillValue") is used.
// - mfhdf/src/mfdatainfo.c is excluded from the build.
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/mfhdf/src/mfsd.c

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

// clang-format off
#ifdef SUCCEED
#undef SUCCEED
#endif
#ifdef FAIL
#undef FAIL
#endif

#include "third_party/hdf4/hdf/src/df.h"
#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/hcomp.h"
#include "third_party/hdf4/hdf/src/hfile.h"
#include "third_party/hdf4/mfhdf/src/mfhdf.h"
#include "third_party/hdf4/mfhdf/src/nc_priv.h"

extern "C" {
#include "third_party/hdf4/mfhdf/src/mf_priv.h"
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

class MfsdTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_ncopts_ = H4_ncopts;
    saved_ncerr_ = H4_ncerr;
    H4_ncopts = 0;
    H4_ncerr = NC_NOERR;

    int cur_max = 0, sys_limit = 0;
    if (SDget_maxopenfiles(&cur_max, &sys_limit) == SUCCEED) {
      saved_max_open_files_ = cur_max;
    }

    HEclear();
  }

  void TearDown() override {
    H4_ncopts = saved_ncopts_;
    H4_ncerr = saved_ncerr_;

    if (saved_max_open_files_ > 0) {
      (void)SDreset_maxopenfiles(saved_max_open_files_);
    }

    (void)DFSDclear();
    (void)HXsetcreatedir(nullptr);
    (void)HXsetdir(nullptr);
    HEclear();
  }

 private:
  int saved_ncopts_ = 0;
  int saved_ncerr_ = 0;
  int saved_max_open_files_ = 0;
};

// ============================================================================
// Suite 1: File Lifecycle, Open Limits, ID Types, Name, and Modes
// ============================================================================

TEST_F(MfsdTest, FileLifecycleOpenLimitsIdTypesAndAccessModes) {
  const std::string path = GetTempFilePath("mfsd_lifecycle.hdf");
  absl::Cleanup rm = [&path] { std::filesystem::remove(path); };

  // 1. Missing file with DFACC_READ fails
  EXPECT_EQ(FAIL, SDstart(GetTempFilePath("nonexistent_mfsd.hdf").c_str(),
                          DFACC_READ));

  // 2. Corrupted truncated 2-byte file fails
  {
    const std::string trunc_path = GetTempFilePath("mfsd_trunc.hdf");
    absl::Cleanup rm_trunc = [&trunc_path] {
      std::filesystem::remove(trunc_path);
    };
    FILE* fp = fopen(trunc_path.c_str(), "wb");
    ASSERT_THAT(fp, NotNull());
    const uint8_t bad_hdr[2] = {0x0E, 0x03};
    fwrite(bad_hdr, 1, sizeof(bad_hdr), fp);
    fclose(fp);
    EXPECT_EQ(FAIL, SDstart(trunc_path.c_str(), DFACC_READ));
  }

  // 3. Create new file with DFACC_CREATE
  {
    const int32_t sd_id = SDstart(path.c_str(), DFACC_CREATE);
    ASSERT_NE(FAIL, sd_id);
    absl::Cleanup close_sd = [sd_id] { EXPECT_EQ(SUCCEED, SDend(sd_id)); };

    // File info on empty file
    int32_t n_datasets = -1, n_gattrs = -1;
    EXPECT_EQ(SUCCEED, SDfileinfo(sd_id, &n_datasets, &n_gattrs));
    EXPECT_EQ(0, n_datasets);
    EXPECT_EQ(0, n_gattrs);

    // ID types
    EXPECT_EQ(SD_ID, SDidtype(sd_id));
    EXPECT_EQ(NOT_SDAPI_ID, SDidtype(FAIL));
    EXPECT_EQ(NOT_SDAPI_ID, SDidtype(0x12345678));

    // Filename queries
    uint16_t name_len = 0;
    EXPECT_EQ(SUCCEED, SDgetnamelen(sd_id, &name_len));
    EXPECT_EQ(path.length(), name_len);
    EXPECT_EQ(FAIL, SDgetnamelen(FAIL, &name_len));

    std::vector<char> fname_buf(name_len + 1, '\0');
    EXPECT_EQ(static_cast<int>(path.length()),
              SDgetfilename(sd_id, fname_buf.data()));
    EXPECT_EQ(path, std::string(fname_buf.data()));
    EXPECT_EQ(static_cast<int>(path.length()), SDgetfilename(sd_id, nullptr));
    EXPECT_EQ(FAIL, SDgetfilename(FAIL, fname_buf.data()));

    // Fill mode
    const int old_fill_mode = SDsetfillmode(sd_id, SD_NOFILL);
    EXPECT_NE(FAIL, old_fill_mode);
    EXPECT_EQ(SD_NOFILL, SDsetfillmode(sd_id, SD_FILL));
    EXPECT_EQ(FAIL, SDsetfillmode(sd_id, 9999));
    EXPECT_EQ(FAIL, SDsetfillmode(FAIL, SD_FILL));

    // Access type (requires SDS_ID, fails on SD_ID)
    EXPECT_EQ(FAIL, SDsetaccesstype(sd_id, DFACC_DEFAULT));
    EXPECT_EQ(FAIL, SDsetaccesstype(sd_id, 9999));
    EXPECT_EQ(FAIL, SDsetaccesstype(FAIL, DFACC_DEFAULT));

    // Open file counts and limits
    EXPECT_GE(SDget_numopenfiles(), 1);
    int cur_max = 0, sys_limit = 0;
    EXPECT_EQ(SUCCEED, SDget_maxopenfiles(&cur_max, &sys_limit));
    EXPECT_GT(cur_max, 0);
    EXPECT_GT(sys_limit, 0);
    EXPECT_EQ(SUCCEED, SDget_maxopenfiles(&cur_max, nullptr));
    EXPECT_EQ(SUCCEED, SDget_maxopenfiles(nullptr, &sys_limit));

    // Test SDreset_maxopenfiles bounds
    EXPECT_EQ(FAIL, SDreset_maxopenfiles(-5));
    EXPECT_EQ(sys_limit, SDreset_maxopenfiles(sys_limit + 100));
    EXPECT_EQ(sys_limit, SDreset_maxopenfiles(0));
    EXPECT_EQ(H4_MAX_NC_OPEN + 1, SDreset_maxopenfiles(H4_MAX_NC_OPEN + 1));
    EXPECT_EQ(cur_max, SDreset_maxopenfiles(cur_max));

    // Create a dataset to query SDS_ID and DIM_ID types
    {
      int32_t dims[2] = {3, 4};
      const int32_t sds_id = SDcreate(sd_id, "test_var", DFNT_INT32, 2, dims);
      ASSERT_NE(FAIL, sds_id);
      absl::Cleanup close_sds = [&sds_id] {
        EXPECT_EQ(SUCCEED, SDendaccess(sds_id));
      };

      int32_t s_start[2] = {0, 0};
      int32_t s_edges[2] = {3, 4};
      int32_t s_data[12] = {0};
      EXPECT_EQ(SUCCEED,
                SDwritedata(sds_id, s_start, nullptr, s_edges, s_data));
      EXPECT_EQ(SUCCEED, SDsetaccesstype(sds_id, DFACC_DEFAULT));
      EXPECT_EQ(FAIL, SDsetaccesstype(sds_id, DFACC_SERIAL));
      EXPECT_EQ(FAIL, SDsetaccesstype(sds_id, 9999));

      EXPECT_EQ(SDS_ID, SDidtype(sds_id));
      const int32_t dim0 = SDgetdimid(sds_id, 0);
      ASSERT_NE(FAIL, dim0);
      EXPECT_EQ(DIM_ID, SDidtype(dim0));

      uint16_t sds_namelen = 0;
      EXPECT_EQ(SUCCEED, SDgetnamelen(sds_id, &sds_namelen));
      EXPECT_EQ(std::string("test_var").length(), sds_namelen);

      uint16_t dim_namelen = 0;
      EXPECT_EQ(SUCCEED, SDgetnamelen(dim0, &dim_namelen));
      EXPECT_GT(dim_namelen, 0);

      // Direct internal SDI* helpers in mf_priv.h
      NC* handle = SDIhandle_from_id(sd_id, CDFTYPE);
      ASSERT_THAT(handle, NotNull());
      EXPECT_THAT(SDIhandle_from_id(FAIL, CDFTYPE), IsNull());
      EXPECT_THAT(SDIhandle_from_id(sd_id, SDSTYPE), IsNull());

      NC_var* var_ptr = SDIget_var(handle, sds_id);
      ASSERT_THAT(var_ptr, NotNull());
      EXPECT_THAT(SDIget_var(handle, FAIL), IsNull());

      NC_dim* dim_ptr = SDIget_dim(handle, dim0);
      ASSERT_THAT(dim_ptr, NotNull());
      EXPECT_THAT(SDIget_dim(handle, FAIL), IsNull());

      EXPECT_EQ(SUCCEED, SDIfreevarAID(handle, 0));
      EXPECT_EQ(FAIL, SDIfreevarAID(handle, -1));

      NC* ap_handle = nullptr;
      NC_array** ap_ptr = nullptr;
      EXPECT_EQ(SUCCEED, SDIapfromid(sd_id, &ap_handle, &ap_ptr));
      EXPECT_EQ(SUCCEED, SDIapfromid(sds_id, &ap_handle, &ap_ptr));
      EXPECT_EQ(SUCCEED, SDIapfromid(dim0, &ap_handle, &ap_ptr));
      EXPECT_EQ(FAIL, SDIapfromid(FAIL, &ap_handle, &ap_ptr));

      // SDIputattr direct
      int32_t attr_val = 123;
      EXPECT_EQ(SUCCEED,
                SDIputattr(ap_ptr, "sdi_attr", DFNT_INT32, 1, &attr_val));
    }
  }

  // 4. Test SDI_can_clobber on read-only file
  if (geteuid() != 0) {
    absl::Cleanup restore_perms = [&path] { (void)chmod(path.c_str(), 0600); };
    ASSERT_EQ(0, chmod(path.c_str(), 0444));
    EXPECT_EQ(0, SDI_can_clobber(path.c_str()));
    const int32_t denied_sd = SDstart(path.c_str(), DFACC_CREATE);
    EXPECT_EQ(FAIL, denied_sd);
    ASSERT_EQ(0, chmod(path.c_str(), 0600));
  }

  // 5. Open with DFACC_WRITE and DFACC_READ
  {
    const int32_t write_sd = SDstart(path.c_str(), DFACC_WRITE);
    ASSERT_NE(FAIL, write_sd);
    absl::Cleanup close_write = [write_sd] {
      EXPECT_EQ(SUCCEED, SDend(write_sd));
    };
  }

  {
    const int32_t read_sd = SDstart(path.c_str(), DFACC_READ);
    ASSERT_NE(FAIL, read_sd);
    absl::Cleanup close_read = [read_sd] {
      EXPECT_EQ(SUCCEED, SDend(read_sd));
    };
  }

  // Error checks on invalid ID
  int32_t n_datasets = -1, n_gattrs = -1;
  EXPECT_EQ(FAIL, SDend(FAIL));
  EXPECT_EQ(FAIL, SDfileinfo(FAIL, &n_datasets, &n_gattrs));
}

// ============================================================================
// Suite 2: Dataset Creation, Lookup, Coord Var Interop, and Check Empty
// ============================================================================

TEST_F(MfsdTest, DatasetCreationLookupCoordVarInteropAndCheckEmpty) {
  const std::string path = GetTempFilePath("mfsd_datasets.hdf");
  absl::Cleanup rm = [&path] { std::filesystem::remove(path); };

  const int32_t sd_id = SDstart(path.c_str(), DFACC_CREATE);
  ASSERT_NE(FAIL, sd_id);
  absl::Cleanup close_sd = [&sd_id] { EXPECT_EQ(SUCCEED, SDend(sd_id)); };

  // 1. SDcreate with nullptr, empty "", and blank "   " names (defaults to
  // "DataSet")
  int32_t d1[1] = {5};
  const int32_t sds_null = SDcreate(sd_id, nullptr, DFNT_INT16, 1, d1);
  ASSERT_NE(FAIL, sds_null);
  std::array<char, H4_MAX_NC_NAME + 1> qname{};
  int32_t qrank = 0, qdims[H4_MAX_VAR_DIMS] = {0}, qnt = 0, qnattrs = 0;
  EXPECT_EQ(SUCCEED,
            SDgetinfo(sds_null, qname.data(), &qrank, qdims, &qnt, &qnattrs));
  EXPECT_EQ(std::string("DataSet"), std::string(qname.data()));
  EXPECT_EQ(SUCCEED, SDendaccess(sds_null));

  const int32_t sds_empty = SDcreate(sd_id, "", DFNT_INT16, 1, d1);
  ASSERT_NE(FAIL, sds_empty);
  EXPECT_EQ(SUCCEED,
            SDgetinfo(sds_empty, qname.data(), &qrank, qdims, &qnt, &qnattrs));
  EXPECT_EQ(std::string("DataSet"), std::string(qname.data()));
  EXPECT_EQ(SUCCEED, SDendaccess(sds_empty));

  const int32_t sds_blank = SDcreate(sd_id, "   ", DFNT_INT16, 1, d1);
  ASSERT_NE(FAIL, sds_blank);
  EXPECT_EQ(SUCCEED,
            SDgetinfo(sds_blank, qname.data(), &qrank, qdims, &qnt, &qnattrs));
  EXPECT_EQ(std::string("DataSet"), std::string(qname.data()));
  EXPECT_EQ(SUCCEED, SDendaccess(sds_blank));

  // 2. All 10 number types + LITEND + NATIVE
  const int32_t kTypes[] = {
      DFNT_CHAR,
      DFNT_UCHAR,
      DFNT_INT8,
      DFNT_UINT8,
      DFNT_INT16,
      DFNT_UINT16,
      DFNT_INT32,
      DFNT_UINT32,
      DFNT_FLOAT32,
      DFNT_FLOAT64,
      DFNT_INT32 | DFNT_LITEND,
      DFNT_INT32 | DFNT_NATIVE,
  };
  for (int i = 0; i < 12; i++) {
    std::string vname = "var_type_" + std::to_string(i);
    int32_t vd[1] = {4};
    const int32_t vid = SDcreate(sd_id, vname.c_str(), kTypes[i], 1, vd);
    ASSERT_NE(FAIL, vid);
    EXPECT_EQ(SUCCEED, SDendaccess(vid));
  }

  // 3. Rank 0 scalar dataset (var->shape == NULL)
  const int32_t sds_scalar =
      SDcreate(sd_id, "scalar_sds", DFNT_INT32, 0, nullptr);
  ASSERT_NE(FAIL, sds_scalar);
  EXPECT_EQ(TRUE, SDisrecord(sds_scalar));
  EXPECT_EQ(SUCCEED, SDendaccess(sds_scalar));

  // 4. Ragged array
  int32_t ragged_dims[2] = {4, SD_RAGGED};
  const int32_t sds_ragged =
      SDcreate(sd_id, "ragged_sds", DFNT_FLOAT32, 2, ragged_dims);
  ASSERT_NE(FAIL, sds_ragged);
  EXPECT_EQ(SUCCEED, SDendaccess(sds_ragged));

  // 5. Unlimited dimension dataset
  int32_t unlim_dims[2] = {SD_UNLIMITED, 3};
  const int32_t sds_unlim =
      SDcreate(sd_id, "unlim_sds", DFNT_INT32, 2, unlim_dims);
  ASSERT_NE(FAIL, sds_unlim);
  EXPECT_EQ(1, SDisrecord(sds_unlim));
  int empty_flag = -1;
  EXPECT_EQ(SUCCEED, SDcheckempty(sds_unlim, &empty_flag));
  EXPECT_EQ(1, empty_flag);
  EXPECT_EQ(SUCCEED, SDendaccess(sds_unlim));

  // 6. Invalid SDcreate calls
  EXPECT_EQ(FAIL, SDcreate(sd_id, "bad_rank", DFNT_INT32, -1, d1));
  std::array<int32_t, H4_MAX_VAR_DIMS + 1> big_dims{};
  EXPECT_EQ(FAIL, SDcreate(sd_id, "bad_rank", DFNT_INT32, H4_MAX_VAR_DIMS + 1,
                           big_dims.data()));
  EXPECT_EQ(FAIL, SDcreate(sd_id, "null_dims", DFNT_INT32, 2, nullptr));
  EXPECT_EQ(FAIL, SDcreate(sd_id, "bad_nt", 9999, 1, d1));
  EXPECT_EQ(FAIL, SDcreate(FAIL, "bad_id", DFNT_INT32, 1, d1));

  // 7. Duplicate variable names and SDnametoindex / SDgetnumvars_byname /
  // SDnametoindices
  const int32_t sds_dup1 = SDcreate(sd_id, "dup_name", DFNT_INT32, 1, d1);
  ASSERT_NE(FAIL, sds_dup1);
  EXPECT_EQ(SUCCEED, SDendaccess(sds_dup1));

  const int32_t sds_dup2 = SDcreate(sd_id, "dup_name", DFNT_FLOAT32, 1, d1);
  ASSERT_NE(FAIL, sds_dup2);
  EXPECT_EQ(SUCCEED, SDendaccess(sds_dup2));

  int32_t n_vars = 0;
  EXPECT_EQ(SUCCEED, SDgetnumvars_byname(sd_id, "dup_name", &n_vars));
  EXPECT_EQ(2, n_vars);
  EXPECT_EQ(SUCCEED, SDgetnumvars_byname(sd_id, "nonexistent_var", &n_vars));
  EXPECT_EQ(0, n_vars);

  const int32_t idx_first = SDnametoindex(sd_id, "dup_name");
  EXPECT_GE(idx_first, 0);

  hdf_varlist_t var_list[10] = {};
  EXPECT_EQ(SUCCEED, SDnametoindices(sd_id, "dup_name", var_list));
  EXPECT_EQ(idx_first, var_list[0].var_index);
  EXPECT_EQ(IS_SDSVAR, var_list[0].var_type);

  // 8. SDselect, SDgetinfo with partial nullptr outputs
  const int32_t sel_sds = SDselect(sd_id, idx_first);
  ASSERT_NE(FAIL, sel_sds);
  EXPECT_EQ(SUCCEED,
            SDgetinfo(sel_sds, nullptr, nullptr, nullptr, nullptr, nullptr));
  EXPECT_EQ(SUCCEED, SDgetinfo(sel_sds, qname.data(), nullptr, nullptr, nullptr,
                               nullptr));

  // SDidtoref & SDreftoindex
  const int32_t ref = SDidtoref(sel_sds);
  EXPECT_GT(ref, 0);
  const int32_t reftoidx = SDreftoindex(sd_id, ref);
  EXPECT_EQ(idx_first, reftoidx);
  // TODO(schwehr): Upstream SDreftoindex quirk: even when ref is found,
  // probing the NDG/SDS tag records pushes an internal error onto the stack.
  HEclear();

  EXPECT_EQ(FAIL, SDreftoindex(sd_id, 65535));
  EXPECT_EQ(FAIL, SDidtoref(FAIL));

  // SDiscoordvar and SDisrecord
  EXPECT_EQ(0, SDiscoordvar(sel_sds));
  EXPECT_EQ(0, SDisrecord(sel_sds));

  // SDcheckempty on empty fixed-size dataset
  empty_flag = -1;
  EXPECT_EQ(SUCCEED, SDcheckempty(sel_sds, &empty_flag));
  EXPECT_EQ(1, empty_flag);

  // Write data to sel_sds and check SDcheckempty is 0
  int32_t write_vals[5] = {1, 2, 3, 4, 5};
  int32_t start[1] = {0};
  int32_t edges[1] = {5};
  EXPECT_EQ(SUCCEED, SDwritedata(sel_sds, start, nullptr, edges, write_vals));

  empty_flag = -1;
  EXPECT_EQ(SUCCEED, SDcheckempty(sel_sds, &empty_flag));
  EXPECT_EQ(0, empty_flag);

  EXPECT_EQ(SUCCEED, SDendaccess(sel_sds));
  EXPECT_EQ(FAIL, SDselect(sd_id, 9999));
  EXPECT_EQ(FAIL, SDselect(sd_id, -1));
  EXPECT_EQ(FAIL, SDendaccess(FAIL));
}

// ============================================================================
// Suite 3: Hyperslab Read/Write Data and Stride Validation
// ============================================================================

TEST_F(MfsdTest, HyperslabReadWriteDataAndStrideValidation) {
  const std::string path = GetTempFilePath("mfsd_hyperslab.hdf");
  absl::Cleanup rm = [&path] { std::filesystem::remove(path); };

  const int32_t sd_id = SDstart(path.c_str(), DFACC_CREATE);
  ASSERT_NE(FAIL, sd_id);
  absl::Cleanup close_sd = [&sd_id] { EXPECT_EQ(SUCCEED, SDend(sd_id)); };

  // 1. 2D dataset with unit strides and non-unit strides
  int32_t dims[2] = {4, 6};
  const int32_t sds_id = SDcreate(sd_id, "matrix", DFNT_INT32, 2, dims);
  ASSERT_NE(FAIL, sds_id);
  absl::Cleanup close_sds = [&sds_id] {
    EXPECT_EQ(SUCCEED, SDendaccess(sds_id));
  };

  std::vector<int32_t> write_data(4 * 6);
  for (int i = 0; i < 24; i++) {
    write_data[i] = i * 10;
  }

  int32_t start[2] = {0, 0};
  int32_t edges[2] = {4, 6};
  int32_t unit_stride[2] = {1, 1};

  // Write full array with unit stride (hits no_strides == 1 L1912)
  EXPECT_EQ(SUCCEED,
            SDwritedata(sds_id, start, unit_stride, edges, write_data.data()));

  // Read full array with stride == nullptr
  std::vector<int32_t> read_data(24, -1);
  EXPECT_EQ(SUCCEED,
            SDreaddata(sds_id, start, nullptr, edges, read_data.data()));
  EXPECT_EQ(write_data, read_data);

  // Strided write with stride {2, 3} (2x2 subgrid)
  int32_t sub_start[2] = {1, 1};
  int32_t sub_edges[2] = {2, 2};
  int32_t strided[2] = {2, 2};
  int32_t sub_vals[4] = {991, 992, 993, 994};
  EXPECT_EQ(SUCCEED,
            SDwritedata(sds_id, sub_start, strided, sub_edges, sub_vals));

  // Strided read
  int32_t read_sub[4] = {0};
  EXPECT_EQ(SUCCEED,
            SDreaddata(sds_id, sub_start, strided, sub_edges, read_sub));
  EXPECT_EQ(991, read_sub[0]);
  EXPECT_EQ(992, read_sub[1]);
  EXPECT_EQ(993, read_sub[2]);
  EXPECT_EQ(994, read_sub[3]);

  // 2. Hyperslab I/O on DIM_ID (coordinate variable)
  const int32_t dim0 = SDgetdimid(sds_id, 0);
  ASSERT_NE(FAIL, dim0);
  float scale_vals[4] = {10.5f, 20.5f, 30.5f, 40.5f};
  int32_t dim_start[H4_MAX_VAR_DIMS] = {0};
  int32_t dim_edges[H4_MAX_VAR_DIMS] = {4, 1};
  EXPECT_EQ(SUCCEED,
            SDwritedata(dim0, dim_start, nullptr, dim_edges, scale_vals));

  float read_scale[4] = {0};
  EXPECT_EQ(SUCCEED,
            SDreaddata(dim0, dim_start, nullptr, dim_edges, read_scale));
  EXPECT_EQ(10.5f, read_scale[0]);
  EXPECT_EQ(40.5f, read_scale[3]);

  // 3. Error branches for SDwritedata and SDreaddata
  EXPECT_EQ(FAIL,
            SDwritedata(sds_id, nullptr, nullptr, edges, write_data.data()));
  EXPECT_EQ(FAIL,
            SDwritedata(sds_id, start, nullptr, nullptr, write_data.data()));
  EXPECT_EQ(FAIL, SDwritedata(sds_id, start, nullptr, edges, nullptr));

  int32_t neg_start[2] = {-1, 0};
  EXPECT_EQ(FAIL,
            SDwritedata(sds_id, neg_start, nullptr, edges, write_data.data()));
  int32_t neg_edges[2] = {4, -1};
  EXPECT_EQ(FAIL,
            SDwritedata(sds_id, start, nullptr, neg_edges, write_data.data()));
  int32_t oob_edges[2] = {5, 6};
  EXPECT_EQ(FAIL,
            SDwritedata(sds_id, start, nullptr, oob_edges, write_data.data()));

  int32_t bad_stride[2] = {0, 1};
  EXPECT_EQ(FAIL,
            SDwritedata(sds_id, start, bad_stride, edges, write_data.data()));
  int32_t oob_stride[2] = {2, 4};
  EXPECT_EQ(FAIL,
            SDwritedata(sds_id, start, oob_stride, edges, write_data.data()));

  EXPECT_EQ(FAIL,
            SDreaddata(sds_id, nullptr, nullptr, edges, read_data.data()));
  EXPECT_EQ(FAIL,
            SDreaddata(sds_id, start, nullptr, nullptr, read_data.data()));
  EXPECT_EQ(FAIL, SDreaddata(sds_id, start, nullptr, edges, nullptr));
  EXPECT_EQ(FAIL,
            SDreaddata(sds_id, neg_start, nullptr, edges, read_data.data()));
  EXPECT_EQ(FAIL,
            SDreaddata(sds_id, start, nullptr, neg_edges, read_data.data()));
  EXPECT_EQ(FAIL,
            SDreaddata(sds_id, start, nullptr, oob_edges, read_data.data()));
  EXPECT_EQ(FAIL,
            SDreaddata(sds_id, start, bad_stride, edges, read_data.data()));
  EXPECT_EQ(FAIL,
            SDreaddata(sds_id, start, oob_stride, edges, read_data.data()));
}

// ============================================================================
// Suite 4: Dimensions, Scales, and Backward Compatibility
// ============================================================================

TEST_F(MfsdTest, DimensionsScalesAndBackwardCompatibility) {
  const std::string path = GetTempFilePath("mfsd_dims.hdf");
  absl::Cleanup rm = [&path] { std::filesystem::remove(path); };

  const int32_t sd_id = SDstart(path.c_str(), DFACC_CREATE);
  ASSERT_NE(FAIL, sd_id);
  absl::Cleanup close_sd = [&sd_id] { EXPECT_EQ(SUCCEED, SDend(sd_id)); };

  int32_t dims[2] = {5, 8};
  const int32_t sds_id = SDcreate(sd_id, "grid", DFNT_INT32, 2, dims);
  ASSERT_NE(FAIL, sds_id);
  absl::Cleanup close_sds = [&sds_id] {
    EXPECT_EQ(SUCCEED, SDendaccess(sds_id));
  };

  const int32_t dim0 = SDgetdimid(sds_id, 0);
  const int32_t dim1 = SDgetdimid(sds_id, 1);
  ASSERT_NE(FAIL, dim0);
  ASSERT_NE(FAIL, dim1);
  EXPECT_EQ(FAIL, SDgetdimid(sds_id, 2));
  EXPECT_EQ(FAIL, SDgetdimid(sds_id, -1));

  // 1. SDsetdimname and SDdiminfo
  EXPECT_EQ(SUCCEED, SDsetdimname(dim0, "rows"));
  EXPECT_EQ(SUCCEED, SDsetdimname(dim1, "cols"));

  char dname[64] = "";
  int32_t dcount = 0, dnt = 0, dnattrs = 0;
  EXPECT_EQ(SUCCEED, SDdiminfo(dim0, dname, &dcount, &dnt, &dnattrs));
  EXPECT_EQ(std::string("rows"), std::string(dname));
  EXPECT_EQ(5, dcount);
  EXPECT_EQ(SUCCEED, SDdiminfo(dim0, nullptr, &dcount, &dnt, &dnattrs));

  // Create second dataset with dim size 5 and share "rows" name (compatible
  // merge)
  int32_t dims2[1] = {5};
  const int32_t sds2 = SDcreate(sd_id, "vec", DFNT_FLOAT32, 1, dims2);
  ASSERT_NE(FAIL, sds2);
  absl::Cleanup close_sds2 = [&sds2] { EXPECT_EQ(SUCCEED, SDendaccess(sds2)); };
  const int32_t dim2_0 = SDgetdimid(sds2, 0);
  EXPECT_EQ(SUCCEED, SDsetdimname(dim2_0, "rows"));

  // Try to rename dim with size 8 to "rows" (mismatched size -> DFE_BADDIMNAME
  // FAIL)
  EXPECT_EQ(FAIL, SDsetdimname(dim1, "rows"));

  // 2. SDsetdimstrs and SDgetdimstrs
  EXPECT_EQ(SUCCEED, SDsetdimstrs(dim0, "Row Dimension", "meters", "F5.2"));
  char label[64] = "", unit[64] = "", format[64] = "";
  EXPECT_EQ(SUCCEED, SDgetdimstrs(dim0, label, unit, format, 64));
  EXPECT_EQ(std::string("Row Dimension"), std::string(label));
  EXPECT_EQ(std::string("meters"), std::string(unit));
  EXPECT_EQ(std::string("F5.2"), std::string(format));

  // Partial null and short length truncation
  EXPECT_EQ(SUCCEED, SDgetdimstrs(dim0, label, nullptr, nullptr, 64));
  EXPECT_EQ(SUCCEED, SDgetdimstrs(dim0, label, unit, format, 4));  // Truncated
  EXPECT_EQ(SUCCEED, SDgetdimstrs(dim0, label, unit, format, 0));
  EXPECT_EQ(FAIL, SDgetdimstrs(FAIL, label, unit, format, 64));

  // 3. SDsetdimscale and SDgetdimscale
  float scale_vals[5] = {1.1f, 2.2f, 3.3f, 4.4f, 5.5f};
  EXPECT_EQ(SUCCEED, SDsetdimscale(dim0, 5, DFNT_FLOAT32, scale_vals));

  float read_scale[5] = {0};
  EXPECT_EQ(SUCCEED, SDgetdimscale(dim0, read_scale));
  EXPECT_EQ(1.1f, read_scale[0]);
  EXPECT_EQ(5.5f, read_scale[4]);

  // Recompute coordvar with different number type (DFNT_INT32)
  int32_t int_scale[5] = {10, 20, 30, 40, 50};
  EXPECT_EQ(SUCCEED, SDsetdimscale(dim0, 5, DFNT_INT32, int_scale));
  int32_t read_int_scale[5] = {0};
  EXPECT_EQ(SUCCEED, SDgetdimscale(dim0, read_int_scale));
  EXPECT_EQ(10, read_int_scale[0]);

  // Dimension with no scale values
  EXPECT_EQ(FAIL, SDgetdimscale(dim1, read_scale));
  EXPECT_EQ(FAIL,
            SDsetdimscale(dim0, 4, DFNT_INT32, int_scale));  // Mismatched count

  // 4. Backward compatibility: SDsetdimval_comp & SDisdimval_bwcomp
  EXPECT_EQ(SUCCEED, SDsetdimval_comp(dim0, SD_DIMVAL_BW_COMP));
  EXPECT_EQ(SD_DIMVAL_BW_COMP, SDisdimval_bwcomp(dim0));
  EXPECT_EQ(SUCCEED, SDsetdimval_comp(dim0, SD_DIMVAL_BW_INCOMP));
  EXPECT_EQ(SD_DIMVAL_BW_INCOMP, SDisdimval_bwcomp(dim0));
  EXPECT_EQ(FAIL, SDsetdimval_comp(FAIL, SD_DIMVAL_BW_COMP));
}

// ============================================================================
// Suite 5: Attributes, Predefined Strings, Range, Calibration, and Fill Value
// ============================================================================

TEST_F(MfsdTest, AttributesPredefinedStringsRangeCalAndFillValue) {
  const std::string path = GetTempFilePath("mfsd_attrs.hdf");
  absl::Cleanup rm = [&path] { std::filesystem::remove(path); };

  const int32_t sd_id = SDstart(path.c_str(), DFACC_CREATE);
  ASSERT_NE(FAIL, sd_id);
  absl::Cleanup close_sd = [&sd_id] { EXPECT_EQ(SUCCEED, SDend(sd_id)); };

  int32_t dims[2] = {4, 4};
  const int32_t sds_id = SDcreate(sd_id, "data_grid", DFNT_FLOAT32, 2, dims);
  ASSERT_NE(FAIL, sds_id);
  absl::Cleanup close_sds = [&sds_id] {
    EXPECT_EQ(SUCCEED, SDendaccess(sds_id));
  };

  const int32_t dim0 = SDgetdimid(sds_id, 0);
  ASSERT_NE(FAIL, dim0);

  // 1. SDsetattr on all 3 ID types: file, dataset, and dimension
  const char file_ver[] = "1.0.0";
  EXPECT_EQ(SUCCEED,
            SDsetattr(sd_id, "version", DFNT_CHAR, sizeof(file_ver), file_ver));

  int32_t sds_flags[2] = {10, 20};
  EXPECT_EQ(SUCCEED, SDsetattr(sds_id, "flags", DFNT_INT32, 2, sds_flags));

  float dim_weights[4] = {0.1f, 0.2f, 0.3f, 0.4f};
  EXPECT_EQ(SUCCEED, SDsetattr(dim0, "weights", DFNT_FLOAT32, 4, dim_weights));

  // Attribute overwrite
  int32_t new_flags[2] = {100, 200};
  EXPECT_EQ(SUCCEED, SDsetattr(sds_id, "flags", DFNT_INT32, 2, new_flags));

  // 2. SDfindattr, SDattrinfo, and SDreadattr
  const int32_t f_idx = SDfindattr(sd_id, "version");
  EXPECT_GE(f_idx, 0);
  char a_name[64] = "";
  int32_t a_type = 0, a_count = 0;
  EXPECT_EQ(SUCCEED, SDattrinfo(sd_id, f_idx, a_name, &a_type, &a_count));
  EXPECT_EQ(std::string("version"), std::string(a_name));
  EXPECT_EQ(DFNT_CHAR, a_type);
  EXPECT_EQ(sizeof(file_ver), a_count);
  char read_ver[64] = "";
  EXPECT_EQ(SUCCEED, SDreadattr(sd_id, f_idx, read_ver));
  EXPECT_EQ(std::string(file_ver), std::string(read_ver));

  // SDattrinfo with nullptr name buffer fails
  EXPECT_EQ(FAIL, SDattrinfo(sd_id, f_idx, nullptr, &a_type, &a_count));

  const int32_t s_idx = SDfindattr(sds_id, "flags");
  EXPECT_GE(s_idx, 0);
  int32_t read_flags[2] = {0};
  EXPECT_EQ(SUCCEED, SDreadattr(sds_id, s_idx, read_flags));
  EXPECT_EQ(100, read_flags[0]);
  EXPECT_EQ(200, read_flags[1]);

  const int32_t d_idx = SDfindattr(dim0, "weights");
  EXPECT_GE(d_idx, 0);
  float read_weights[4] = {0};
  EXPECT_EQ(SUCCEED, SDreadattr(dim0, d_idx, read_weights));
  EXPECT_EQ(0.1f, read_weights[0]);
  EXPECT_EQ(0.4f, read_weights[3]);

  // SDfindattr non-existent
  EXPECT_EQ(FAIL, SDfindattr(sds_id, "nonexistent"));

  // SDsetattr error checks
  EXPECT_EQ(FAIL, SDsetattr(sds_id, nullptr, DFNT_INT32, 1, sds_flags));
  EXPECT_EQ(FAIL, SDsetattr(sds_id, "bad_nt", DFNT_INT32 | DFNT_NATIVE, 1,
                            sds_flags));
  EXPECT_EQ(FAIL, SDsetattr(sds_id, "bad_count", DFNT_INT32, 0, sds_flags));
  EXPECT_EQ(FAIL, SDsetattr(sds_id, "bad_count", DFNT_INT32, -1, sds_flags));
  EXPECT_EQ(FAIL, SDsetattr(sds_id, "bad_order", DFNT_INT32, MAX_ORDER + 1,
                            sds_flags));
  EXPECT_EQ(FAIL, SDattrinfo(sds_id, 9999, a_name, &a_type, &a_count));
  EXPECT_EQ(FAIL, SDreadattr(sds_id, 9999, read_flags));

  // 3. Predefined strings: SDsetdatastrs and SDgetdatastrs
  EXPECT_EQ(SUCCEED, SDsetdatastrs(sds_id, "Surface Temp", "Celsius", "F4.1",
                                   "Cartesian"));
  char s_label[64] = "", s_unit[64] = "", s_format[64] = "",
       s_coordsys[64] = "";
  EXPECT_EQ(SUCCEED,
            SDgetdatastrs(sds_id, s_label, s_unit, s_format, s_coordsys, 64));
  EXPECT_EQ(std::string("Surface Temp"), std::string(s_label));
  EXPECT_EQ(std::string("Celsius"), std::string(s_unit));
  EXPECT_EQ(std::string("F4.1"), std::string(s_format));
  EXPECT_EQ(std::string("Cartesian"), std::string(s_coordsys));

  // Partial null and short buffer truncation
  EXPECT_EQ(SUCCEED,
            SDgetdatastrs(sds_id, s_label, nullptr, nullptr, nullptr, 64));
  EXPECT_EQ(SUCCEED,
            SDgetdatastrs(sds_id, s_label, s_unit, s_format, s_coordsys, 4));
  EXPECT_EQ(SUCCEED,
            SDgetdatastrs(sds_id, s_label, s_unit, s_format, s_coordsys, 0));
  EXPECT_EQ(FAIL,
            SDgetdatastrs(FAIL, s_label, s_unit, s_format, s_coordsys, 64));

  // 4. Range: SDsetrange and SDgetrange
  float max_v = 100.0f, min_v = -20.0f;
  EXPECT_EQ(SUCCEED, SDsetrange(sds_id, &max_v, &min_v));
  float r_max = 0.0f, r_min = 0.0f;
  EXPECT_EQ(SUCCEED, SDgetrange(sds_id, &r_max, &r_min));
  EXPECT_EQ(100.0f, r_max);
  EXPECT_EQ(-20.0f, r_min);
  EXPECT_EQ(FAIL, SDsetrange(sds_id, nullptr, &min_v));
  EXPECT_EQ(FAIL, SDsetrange(sds_id, &max_v, nullptr));

  // 5. Calibration: SDsetcal and SDgetcal
  EXPECT_EQ(SUCCEED, SDsetcal(sds_id, 1.5, 0.05, 10.0, 0.2, DFNT_FLOAT32));
  double cal = 0, cale = 0, off = 0, offe = 0;
  int32_t cal_nt = 0;
  EXPECT_EQ(SUCCEED, SDgetcal(sds_id, &cal, &cale, &off, &offe, &cal_nt));
  EXPECT_EQ(1.5, cal);
  EXPECT_EQ(0.05, cale);
  EXPECT_EQ(10.0, off);
  EXPECT_EQ(0.2, offe);
  EXPECT_EQ(DFNT_FLOAT32, cal_nt);
  EXPECT_EQ(FAIL, SDsetcal(FAIL, 1.0, 0.0, 0.0, 0.0, DFNT_FLOAT32));

  // 6. Fill value: SDsetfillvalue and SDgetfillvalue
  float fill_val = -999.0f;
  EXPECT_EQ(SUCCEED, SDsetfillvalue(sds_id, &fill_val));
  float read_fill = 0.0f;
  EXPECT_EQ(SUCCEED, SDgetfillvalue(sds_id, &read_fill));
  EXPECT_EQ(-999.0f, read_fill);
  float dummy_fv = -1.0f;
  EXPECT_EQ(FAIL, SDsetfillvalue(FAIL, &dummy_fv));
  EXPECT_EQ(FAIL, SDgetfillvalue(sds_id, nullptr));
}

// ============================================================================
// Suite 6: External Files, Linked Blocks, N-bit, and Compression
// ============================================================================

TEST_F(MfsdTest, ExternalFilesLinkedBlocksNbitAndCompression) {
  const std::string dir = GetTempFilePath("mfsd_ext_dir");
  std::filesystem::create_directories(dir);
  absl::Cleanup rm_dir = [&dir] { std::filesystem::remove_all(dir); };

  const std::string path = (std::filesystem::path(dir) / "main.hdf").string();
  const std::string ext_file =
      (std::filesystem::path(dir) / "ext_data.bin").string();

  const int32_t sd_id = SDstart(path.c_str(), DFACC_CREATE);
  ASSERT_NE(FAIL, sd_id);
  absl::Cleanup close_sd = [&sd_id] { EXPECT_EQ(SUCCEED, SDend(sd_id)); };

  // 1. SDsetexternalfile on empty unwritten SDS (data_ref == 0)
  int32_t dims[1] = {10};
  const int32_t sds_ext = SDcreate(sd_id, "ext_sds", DFNT_INT32, 1, dims);
  ASSERT_NE(FAIL, sds_ext);
  absl::Cleanup close_ext = [&sds_ext] {
    EXPECT_EQ(SUCCEED, SDendaccess(sds_ext));
  };

  EXPECT_EQ(FAIL, SDsetexternalfile(sds_ext, ext_file.c_str(), -1));
  EXPECT_EQ(SUCCEED, SDsetexternalfile(sds_ext, ext_file.c_str(), 0));
  // 2nd call returns 0 no-op
  EXPECT_EQ(0, SDsetexternalfile(sds_ext, ext_file.c_str(), 0));

  // SDgetexternalinfo and deprecated SDgetexternalfile
  EXPECT_EQ(static_cast<int>(ext_file.length()),
            SDgetexternalinfo(sds_ext, 0, nullptr, nullptr, nullptr));
  std::vector<char> read_extname(ext_file.length() + 16, '\0');
  int32_t offset = -1, length = -1;
  EXPECT_GT(SDgetexternalinfo(sds_ext, read_extname.size(), read_extname.data(),
                              &offset, &length),
            0);
  EXPECT_EQ(ext_file, std::string(read_extname.data()));

  EXPECT_EQ(static_cast<int>(ext_file.length()),
            SDgetexternalfile(sds_ext, 0, nullptr, nullptr));
  EXPECT_EQ(static_cast<int>(ext_file.length()),
            SDgetexternalfile(sds_ext, read_extname.size(), read_extname.data(),
                              &offset));

  // Write data to external file through SDS
  std::vector<int32_t> ext_data = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  int32_t start[1] = {0};
  int32_t edges[1] = {10};
  EXPECT_EQ(SUCCEED,
            SDwritedata(sds_ext, start, nullptr, edges, ext_data.data()));

  // 2. SDsetblocksize and SDgetblocksize on unlimited dataset
  int32_t unlim_dims[2] = {SD_UNLIMITED, 5};
  const int32_t sds_unlim =
      SDcreate(sd_id, "unlim_block", DFNT_INT32, 2, unlim_dims);
  ASSERT_NE(FAIL, sds_unlim);
  absl::Cleanup close_unlim = [&sds_unlim] {
    EXPECT_EQ(SUCCEED, SDendaccess(sds_unlim));
  };

  EXPECT_EQ(FAIL, SDsetblocksize(FAIL, 1024));
  EXPECT_EQ(SUCCEED, SDsetblocksize(sds_unlim, 1024));

  int32_t block_size = 0;
  EXPECT_EQ(FAIL, SDgetblocksize(sds_unlim, &block_size));

  // Write record data to unlimited dataset
  int32_t row_vals[5] = {10, 20, 30, 40, 50};
  int32_t u_start[2] = {0, 0};
  int32_t u_edges[2] = {1, 5};
  EXPECT_EQ(SUCCEED,
            SDwritedata(sds_unlim, u_start, nullptr, u_edges, row_vals));

  EXPECT_EQ(SUCCEED, SDgetblocksize(sds_unlim, &block_size));
  EXPECT_EQ(1024, block_size);

  // 3. SDsetnbitdataset
  int32_t nb_dims[1] = {8};
  const int32_t sds_nbit = SDcreate(sd_id, "nbit_sds", DFNT_INT32, 1, nb_dims);
  ASSERT_NE(FAIL, sds_nbit);
  absl::Cleanup close_nbit = [&sds_nbit] {
    EXPECT_EQ(SUCCEED, SDendaccess(sds_nbit));
  };

  EXPECT_EQ(FAIL, SDsetnbitdataset(sds_nbit, -1, 10, FALSE, FALSE));
  EXPECT_EQ(FAIL, SDsetnbitdataset(sds_nbit, 0, 0, FALSE, FALSE));
  EXPECT_NE(FAIL, SDsetnbitdataset(sds_nbit, 0, 16, TRUE, FALSE));

  int32_t nb_vals[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  int32_t nb_start[1] = {0};
  int32_t nb_edges[1] = {8};
  EXPECT_EQ(SUCCEED,
            SDwritedata(sds_nbit, nb_start, nullptr, nb_edges, nb_vals));

  // 4. Compression: SDsetcompress across NONE, RLE, SKPHUFF, DEFLATE
  int32_t c_dims[2] = {6, 6};
  std::vector<int32_t> c_data(36, 42);
  int32_t c_start[2] = {0, 0};
  int32_t c_edges[2] = {6, 6};

  // 4a. COMP_CODE_NONE
  {
    const int32_t sds_none =
        SDcreate(sd_id, "comp_none", DFNT_INT32, 2, c_dims);
    ASSERT_NE(FAIL, sds_none);
    comp_info c_info{};
    EXPECT_EQ(SUCCEED, SDsetcompress(sds_none, COMP_CODE_NONE, &c_info));
    EXPECT_EQ(SUCCEED,
              SDwritedata(sds_none, c_start, nullptr, c_edges, c_data.data()));
    comp_coder_t comp_type = COMP_CODE_INVALID;
    EXPECT_EQ(SUCCEED, SDgetcomptype(sds_none, &comp_type));
    EXPECT_EQ(COMP_CODE_NONE, comp_type);
    EXPECT_EQ(SUCCEED, SDendaccess(sds_none));
  }

  // 4b. COMP_CODE_RLE
  {
    const int32_t sds_rle = SDcreate(sd_id, "comp_rle", DFNT_INT32, 2, c_dims);
    ASSERT_NE(FAIL, sds_rle);
    comp_info c_info{};
    EXPECT_EQ(SUCCEED, SDsetcompress(sds_rle, COMP_CODE_RLE, &c_info));
    EXPECT_EQ(SUCCEED,
              SDwritedata(sds_rle, c_start, nullptr, c_edges, c_data.data()));
    comp_info read_info{};
    comp_coder_t comp_type = COMP_CODE_INVALID;
    EXPECT_EQ(SUCCEED, SDgetcompinfo(sds_rle, &comp_type, &read_info));
    EXPECT_EQ(COMP_CODE_RLE, comp_type);
    EXPECT_EQ(SUCCEED, SDendaccess(sds_rle));
  }

  // 4c. COMP_CODE_SKPHUFF
  {
    const int32_t sds_skp = SDcreate(sd_id, "comp_skp", DFNT_INT32, 2, c_dims);
    ASSERT_NE(FAIL, sds_skp);
    comp_info c_info{};
    c_info.skphuff.skp_size = 4;
    EXPECT_EQ(SUCCEED, SDsetcompress(sds_skp, COMP_CODE_SKPHUFF, &c_info));
    EXPECT_EQ(SUCCEED,
              SDwritedata(sds_skp, c_start, nullptr, c_edges, c_data.data()));
    EXPECT_EQ(SUCCEED, SDendaccess(sds_skp));
  }

  // 4d. COMP_CODE_DEFLATE
  {
    const int32_t sds_def = SDcreate(sd_id, "comp_def", DFNT_INT32, 2, c_dims);
    ASSERT_NE(FAIL, sds_def);
    comp_info c_info{};
    c_info.deflate_setting.level = 6;
    EXPECT_EQ(SUCCEED, SDsetcompress(sds_def, COMP_CODE_DEFLATE, &c_info));
    EXPECT_EQ(SUCCEED,
              SDwritedata(sds_def, c_start, nullptr, c_edges, c_data.data()));
    EXPECT_EQ(SUCCEED, SDendaccess(sds_def));

    const int32_t sds_def_r = SDselect(sd_id, SDnametoindex(sd_id, "comp_def"));
    ASSERT_NE(FAIL, sds_def_r);
    absl::Cleanup close_def_r = [&sds_def_r] {
      EXPECT_EQ(SUCCEED, SDendaccess(sds_def_r));
    };

    int32_t comp_size = 0, orig_size = 0;
    EXPECT_EQ(SUCCEED, SDgetdatasize(sds_def_r, &comp_size, &orig_size));
    EXPECT_GT(orig_size, 0);
    EXPECT_GT(comp_size, 0);
    EXPECT_EQ(FAIL, SDgetdatasize(sds_def_r, nullptr, nullptr));

    comp_coder_t comp_type = COMP_CODE_INVALID;
    comp_info read_info{};
    EXPECT_EQ(SUCCEED, SDgetcompress(sds_def_r, &comp_type, &read_info));
    EXPECT_EQ(COMP_CODE_DEFLATE, comp_type);
  }

  // 4e. SZIP fails with DFE_NOENCODER
  {
    const int32_t sds_sz = SDcreate(sd_id, "comp_szip", DFNT_INT32, 2, c_dims);
    ASSERT_NE(FAIL, sds_sz);
    absl::Cleanup close_sz = [sds_sz] {
      EXPECT_EQ(SUCCEED, SDendaccess(sds_sz));
    };
    comp_info c_info{};
    HEclear();
    EXPECT_EQ(FAIL, SDsetcompress(sds_sz, COMP_CODE_SZIP, &c_info));
    EXPECT_EQ(DFE_NOENCODER, HEvalue(1));
  }
}

// ============================================================================
// Suite 7: Chunked Datasets, Cache, and All Types
// ============================================================================

TEST_F(MfsdTest, ChunkedDatasetsCacheAndAllTypes) {
  const std::string path = GetTempFilePath("mfsd_chunked.hdf");
  absl::Cleanup rm = [&path] { std::filesystem::remove(path); };

  const int32_t sd_id = SDstart(path.c_str(), DFACC_CREATE);
  ASSERT_NE(FAIL, sd_id);
  absl::Cleanup close_sd = [&sd_id] { EXPECT_EQ(SUCCEED, SDend(sd_id)); };

  // 1. SDsetchunk default fill-value switch across all 10 types
  const int32_t kChunkTypes[] = {
      DFNT_CHAR,   DFNT_UCHAR, DFNT_INT8,   DFNT_UINT8,   DFNT_INT16,
      DFNT_UINT16, DFNT_INT32, DFNT_UINT32, DFNT_FLOAT32, DFNT_FLOAT64,
  };
  int32_t full_dims[2] = {4, 4};

  for (int i = 0; i < 10; i++) {
    std::string sname = "chunk_type_" + std::to_string(i);
    const int32_t sds =
        SDcreate(sd_id, sname.c_str(), kChunkTypes[i], 2, full_dims);
    ASSERT_NE(FAIL, sds);

    HDF_CHUNK_DEF chunk_def{};
    chunk_def.chunk_lengths[0] = 2;
    chunk_def.chunk_lengths[1] = 2;
    EXPECT_EQ(SUCCEED, SDsetchunk(sds, chunk_def, HDF_CHUNK));

    int32_t flg = 0;
    HDF_CHUNK_DEF read_def{};
    EXPECT_EQ(SUCCEED, SDgetchunkinfo(sds, &read_def, &flg));
    EXPECT_EQ(HDF_CHUNK, flg);
    EXPECT_EQ(2, read_def.chunk_lengths[0]);
    EXPECT_EQ(2, read_def.chunk_lengths[1]);

    EXPECT_EQ(SUCCEED, SDendaccess(sds));
  }

  // 2. Both convert == 1 and convert == 0 paths for SDwritechunk / SDreadchunk
  // 2a. convert == 1: DFNT_INT32 (big-endian HDF default on LE host)
  {
    const int32_t sds =
        SDcreate(sd_id, "chunk_conv1", DFNT_INT32, 2, full_dims);
    ASSERT_NE(FAIL, sds);
    HDF_CHUNK_DEF chunk_def{};
    chunk_def.chunk_lengths[0] = 2;
    chunk_def.chunk_lengths[1] = 2;
    EXPECT_EQ(SUCCEED, SDsetchunk(sds, chunk_def, HDF_CHUNK));

    int32_t origin[2] = {0, 0};
    int32_t chunk_data[4] = {11, 12, 21, 22};
    EXPECT_EQ(SUCCEED, SDwritechunk(sds, origin, chunk_data));

    int32_t read_chunk[4] = {0};
    EXPECT_EQ(SUCCEED, SDreadchunk(sds, origin, read_chunk));
    EXPECT_EQ(11, read_chunk[0]);
    EXPECT_EQ(22, read_chunk[3]);

    // Test SDgetdatasize on written chunked dataset
    int32_t comp_sz = 0, orig_sz = 0;
    EXPECT_EQ(SUCCEED, SDgetdatasize(sds, &comp_sz, &orig_sz));
    EXPECT_GT(orig_sz, 0);
    // Single-null allowed on chunked dataset
    EXPECT_EQ(SUCCEED, SDgetdatasize(sds, &comp_sz, nullptr));
    EXPECT_EQ(SUCCEED, SDgetdatasize(sds, nullptr, &orig_sz));

    EXPECT_EQ(SUCCEED, SDendaccess(sds));
  }

  // 2b. convert == 0: DFNT_INT8
  {
    const int32_t sds = SDcreate(sd_id, "chunk_conv0", DFNT_INT8, 2, full_dims);
    ASSERT_NE(FAIL, sds);
    HDF_CHUNK_DEF chunk_def{};
    chunk_def.chunk_lengths[0] = 2;
    chunk_def.chunk_lengths[1] = 2;
    EXPECT_EQ(SUCCEED, SDsetchunk(sds, chunk_def, HDF_CHUNK));

    int32_t origin[2] = {1, 1};
    int8_t chunk_data[4] = {1, 2, 3, 4};
    EXPECT_EQ(SUCCEED, SDwritechunk(sds, origin, chunk_data));

    int8_t read_chunk[4] = {0};
    EXPECT_EQ(SUCCEED, SDreadchunk(sds, origin, read_chunk));
    EXPECT_EQ(1, read_chunk[0]);
    EXPECT_EQ(4, read_chunk[3]);

    EXPECT_EQ(SUCCEED, SDendaccess(sds));
  }

  // 3. Modes: HDF_CHUNK | HDF_COMP (Deflate, RLE, SkpHuff) and HDF_CHUNK |
  // HDF_NBIT
  {
    const int32_t sds =
        SDcreate(sd_id, "chunk_deflate", DFNT_INT32, 2, full_dims);
    ASSERT_NE(FAIL, sds);
    HDF_CHUNK_DEF chunk_def{};
    chunk_def.comp.chunk_lengths[0] = 2;
    chunk_def.comp.chunk_lengths[1] = 2;
    chunk_def.comp.comp_type = COMP_CODE_DEFLATE;
    chunk_def.comp.cinfo.deflate_setting.level = 6;
    EXPECT_EQ(SUCCEED, SDsetchunk(sds, chunk_def, HDF_CHUNK | HDF_COMP));

    int32_t flg = 0;
    HDF_CHUNK_DEF read_def{};
    EXPECT_EQ(SUCCEED, SDgetchunkinfo(sds, &read_def, &flg));
    EXPECT_EQ(HDF_CHUNK | HDF_COMP, flg);
    EXPECT_EQ(COMP_CODE_DEFLATE, read_def.comp.comp_type);

    EXPECT_EQ(SUCCEED, SDendaccess(sds));
  }

  // SZIP chunking fails
  {
    const int32_t sds = SDcreate(sd_id, "chunk_szip", DFNT_INT32, 2, full_dims);
    ASSERT_NE(FAIL, sds);
    HDF_CHUNK_DEF chunk_def{};
    chunk_def.comp.chunk_lengths[0] = 2;
    chunk_def.comp.chunk_lengths[1] = 2;
    chunk_def.comp.comp_type = COMP_CODE_SZIP;
    EXPECT_EQ(FAIL, SDsetchunk(sds, chunk_def, HDF_CHUNK | HDF_COMP));
    EXPECT_EQ(SUCCEED, SDendaccess(sds));
  }

  // HDF_CHUNK | HDF_NBIT
  {
    const int32_t sds = SDcreate(sd_id, "chunk_nbit", DFNT_INT32, 2, full_dims);
    ASSERT_NE(FAIL, sds);
    HDF_CHUNK_DEF chunk_def{};
    chunk_def.nbit.chunk_lengths[0] = 2;
    chunk_def.nbit.chunk_lengths[1] = 2;
    chunk_def.nbit.start_bit = 0;
    chunk_def.nbit.bit_len = 16;
    chunk_def.nbit.sign_ext = TRUE;
    chunk_def.nbit.fill_one = FALSE;
    EXPECT_EQ(SUCCEED, SDsetchunk(sds, chunk_def, HDF_CHUNK | HDF_NBIT));

    int32_t flg = 0;
    HDF_CHUNK_DEF read_def{};
    EXPECT_EQ(SUCCEED, SDgetchunkinfo(sds, &read_def, &flg));
    EXPECT_EQ(HDF_CHUNK | HDF_NBIT, flg);
    EXPECT_EQ(16, read_def.nbit.bit_len);

    EXPECT_EQ(SUCCEED, SDendaccess(sds));
  }

  // 4. SDsetchunkcache and errors
  {
    const int32_t sds =
        SDcreate(sd_id, "chunk_cache_test", DFNT_INT32, 2, full_dims);
    ASSERT_NE(FAIL, sds);
    HDF_CHUNK_DEF chunk_def{};
    chunk_def.chunk_lengths[0] = 2;
    chunk_def.chunk_lengths[1] = 2;
    EXPECT_EQ(SUCCEED, SDsetchunk(sds, chunk_def, HDF_CHUNK));

    EXPECT_EQ(10, SDsetchunkcache(sds, 10, 0));
    EXPECT_EQ(FAIL, SDsetchunkcache(sds, 0, 0));
    EXPECT_EQ(FAIL, SDsetchunkcache(sds, -1, 0));
    EXPECT_EQ(FAIL, SDsetchunkcache(sds, 10, 9999));  // Invalid flags

    EXPECT_EQ(SUCCEED, SDendaccess(sds));
  }

  // Non-chunked dataset getchunkinfo returns HDF_NONE
  {
    const int32_t sds_reg =
        SDcreate(sd_id, "regular_sds", DFNT_INT32, 2, full_dims);
    ASSERT_NE(FAIL, sds_reg);
    int32_t flg = -1;
    EXPECT_EQ(SUCCEED, SDgetchunkinfo(sds_reg, nullptr, &flg));
    EXPECT_EQ(HDF_NONE, flg);
    EXPECT_EQ(FAIL, SDsetchunkcache(sds_reg, 5, 0));  // Non-chunked error
    EXPECT_EQ(SUCCEED, SDendaccess(sds_reg));
  }
}

// ============================================================================
// Suite 8: Classic NetCDF-3 Programmatically Synthesized File Branches
// ============================================================================

TEST_F(MfsdTest, ClassicNetCdf3SynthesizedFileBranches) {
  const std::string file_path = GetTempFilePath("mfsd_classic_netcdf.nc");
  absl::Cleanup rm = [&file_path] { std::filesystem::remove(file_path); };

  // Build classic "CDF\x01" NetCDF-3 file in big-endian XDR
  std::vector<uint8_t> data;
  auto write_u32 = [&data](uint32_t val) {
    data.push_back((val >> 24) & 0xff);
    data.push_back((val >> 16) & 0xff);
    data.push_back((val >> 8) & 0xff);
    data.push_back(val & 0xff);
  };
  auto write_str = [&](std::string_view s) {
    const uint32_t len = static_cast<uint32_t>(s.size());
    write_u32(len);
    for (char c : s) {
      data.push_back(static_cast<uint8_t>(c));
    }
    while (data.size() % 4 != 0) {
      data.push_back(0);
    }
  };

  // Magic: "CDF\x01"
  write_u32(0x43444601);
  // numrecs = 2
  write_u32(2);
  // Dims: 2 dims (0: unlimited "rec", 1: fixed "len"=3)
  write_u32(10);  // NC_DIMENSION
  write_u32(2);
  write_str("rec");
  write_u32(0);  // unlimited
  write_str("len");
  write_u32(3);  // fixed 3

  // Attrs: empty
  write_u32(0);
  write_u32(0);

  // Vars: 3 vars (fixed "len" float, record "rec" float, record "v_long" long)
  write_u32(11);  // NC_VARIABLE
  write_u32(3);

  // Var 0: "len", fixed coord var assoc=[1], len=12, begin=512
  write_str("len");
  write_u32(1);
  write_u32(1);
  write_u32(0);
  write_u32(0);
  write_u32(5);  // NC_FLOAT
  write_u32(12);
  write_u32(512);

  // Var 1: "rec", record coord var assoc=[0], len=4, begin=524
  write_str("rec");
  write_u32(1);
  write_u32(0);
  write_u32(0);
  write_u32(0);
  write_u32(5);  // NC_FLOAT
  write_u32(4);
  write_u32(524);

  // Var 2: "v_long", record var assoc=[0, 1], len=12, begin=528
  // Record size = 4 ("rec") + 12 ("v_long") = 16 bytes per record
  write_str("v_long");
  write_u32(2);
  write_u32(0);
  write_u32(1);
  write_u32(0);
  write_u32(0);
  write_u32(4);  // NC_LONG (32-bit int)
  write_u32(12);
  write_u32(528);

  // Pad to 4096 bytes and initialize data
  data.resize(4096, 0);

  // Populate fixed "len" coord var at offset 512 with 3 float values
  // {10.0, 20.0, 30.0}
  float f_scales[3] = {10.0f, 20.0f, 30.0f};
  for (int i = 0; i < 3; i++) {
    uint32_t bits = 0;
    memcpy(&bits, &f_scales[i], 4);
    // big-endian
    data[512 + i * 4 + 0] = (bits >> 24) & 0xff;
    data[512 + i * 4 + 1] = (bits >> 16) & 0xff;
    data[512 + i * 4 + 2] = (bits >> 8) & 0xff;
    data[512 + i * 4 + 3] = bits & 0xff;
  }

  // Populate 2 records:
  // Record 0 at offset 524:
  //   rec[0] = 1.0f at 524
  //   v_long[0] = {100, 200, 300} at 528
  float r0 = 1.0f;
  uint32_t r0_bits = 0;
  memcpy(&r0_bits, &r0, 4);
  data[524] = (r0_bits >> 24) & 0xff;
  data[525] = (r0_bits >> 16) & 0xff;
  data[526] = (r0_bits >> 8) & 0xff;
  data[527] = r0_bits & 0xff;

  int32_t v0[3] = {100, 200, 300};
  for (int i = 0; i < 3; i++) {
    data[528 + i * 4 + 0] = (v0[i] >> 24) & 0xff;
    data[528 + i * 4 + 1] = (v0[i] >> 16) & 0xff;
    data[528 + i * 4 + 2] = (v0[i] >> 8) & 0xff;
    data[528 + i * 4 + 3] = v0[i] & 0xff;
  }

  // Record 1 at offset 524 + 16 = 540:
  //   rec[1] = 2.0f at 540
  //   v_long[1] = {400, 500, 600} at 544
  float r1 = 2.0f;
  uint32_t r1_bits = 0;
  memcpy(&r1_bits, &r1, 4);
  data[540] = (r1_bits >> 24) & 0xff;
  data[541] = (r1_bits >> 16) & 0xff;
  data[542] = (r1_bits >> 8) & 0xff;
  data[543] = r1_bits & 0xff;

  int32_t v1[3] = {400, 500, 600};
  for (int i = 0; i < 3; i++) {
    data[544 + i * 4 + 0] = (v1[i] >> 24) & 0xff;
    data[544 + i * 4 + 1] = (v1[i] >> 16) & 0xff;
    data[544 + i * 4 + 2] = (v1[i] >> 8) & 0xff;
    data[544 + i * 4 + 3] = v1[i] & 0xff;
  }

  const int fd = open(file_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
  ASSERT_THAT(fd, Ne(-1));
  (void)write(fd, data.data(), data.size());
  close(fd);

  // 1. Open with SDstart in DFACC_READ mode and verify netCDF_FILE branches
  {
    const int32_t sd_id = SDstart(file_path.c_str(), DFACC_READ);
    ASSERT_NE(FAIL, sd_id);
    absl::Cleanup close_sd = [&sd_id] { EXPECT_EQ(SUCCEED, SDend(sd_id)); };

    NC* handle = SDIhandle_from_id(sd_id, CDFTYPE);
    ASSERT_THAT(handle, NotNull());
    EXPECT_EQ(netCDF_FILE, handle->file_type);

    int32_t n_datasets = 0, n_gattrs = 0;
    EXPECT_EQ(SUCCEED, SDfileinfo(sd_id, &n_datasets, &n_gattrs));
    EXPECT_EQ(3, n_datasets);

    // Select dataset 2 ("v_long")
    const int32_t sds_id = SDselect(sd_id, 2);
    ASSERT_NE(FAIL, sds_id);
    absl::Cleanup close_sds = [&sds_id] {
      EXPECT_EQ(SUCCEED, SDendaccess(sds_id));
    };

    char s_name[64] = "";
    int32_t s_rank = 0, s_dims[H4_MAX_VAR_DIMS] = {0}, s_nt = 0, s_natts = 0;
    // Hits mfsd.c:588 (file_type != HDF_FILE branch in SDgetinfo)
    EXPECT_EQ(SUCCEED,
              SDgetinfo(sds_id, s_name, &s_rank, s_dims, &s_nt, &s_natts));
    EXPECT_EQ(std::string("v_long"), std::string(s_name));
    EXPECT_EQ(2, s_rank);
    EXPECT_EQ(2, s_dims[0]);
    EXPECT_EQ(3, s_dims[1]);
    EXPECT_EQ(DFNT_INT32, s_nt);

    EXPECT_EQ(1, SDisrecord(sds_id));
    EXPECT_EQ(0, SDiscoordvar(sds_id));

    // Read record 0 of v_long: hits mfsd.c:731 (file_type != HDF_FILE branch in
    // SDreaddata)
    int32_t r_start[2] = {0, 0};
    int32_t r_edges[2] = {1, 3};
    int32_t read_vals[3] = {0};
    EXPECT_EQ(SUCCEED,
              SDreaddata(sds_id, r_start, nullptr, r_edges, read_vals));
    EXPECT_EQ(100, read_vals[0]);
    EXPECT_EQ(200, read_vals[1]);
    EXPECT_EQ(300, read_vals[2]);

    // Test dimensions and scales on netCDF file
    const int32_t dim_unlim = SDgetdimid(sds_id, 0);
    const int32_t dim_fixed = SDgetdimid(sds_id, 1);
    ASSERT_NE(FAIL, dim_unlim);
    ASSERT_NE(FAIL, dim_fixed);

    // Hits mfsd.c:2892-2897 in SDdiminfo
    char d_name[64] = "";
    int32_t d_count = 0, d_type = 0, d_natts = 0;
    EXPECT_EQ(SUCCEED,
              SDdiminfo(dim_unlim, d_name, &d_count, &d_type, &d_natts));
    EXPECT_EQ(std::string("rec"), std::string(d_name));
    EXPECT_EQ(SD_UNLIMITED, d_count);

    EXPECT_EQ(SUCCEED,
              SDdiminfo(dim_fixed, d_name, &d_count, &d_type, &d_natts));
    EXPECT_EQ(std::string("len"), std::string(d_name));
    EXPECT_EQ(3, d_count);

    // Hits mfsd.c:2776 in SDgetdimscale
    float read_fscales[3] = {0};
    EXPECT_EQ(SUCCEED, SDgetdimscale(dim_fixed, read_fscales));
    EXPECT_EQ(10.0f, read_fscales[0]);
    EXPECT_EQ(20.0f, read_fscales[1]);
    EXPECT_EQ(30.0f, read_fscales[2]);

    // Hits mfsd.c:2778-2779 in SDgetdimscale on unlimited dimension in NetCDF
    // file
    float read_rec_scales[2] = {0};
    EXPECT_EQ(SUCCEED, SDgetdimscale(dim_unlim, read_rec_scales));
    EXPECT_FLOAT_EQ(1.0f, read_rec_scales[0]);
    EXPECT_FLOAT_EQ(2.0f, read_rec_scales[1]);

    // Read with stride on unlimited dimension in NetCDF file: hits mfsd.c:732
    int32_t strided_start[2] = {0, 0};
    int32_t strided_stride[2] = {1, 1};
    int32_t strided_edges[2] = {2, 3};
    int32_t strided_vals[6] = {0};
    EXPECT_EQ(SUCCEED, SDreaddata(sds_id, strided_start, strided_stride,
                                  strided_edges, strided_vals));
    EXPECT_EQ(100, strided_vals[0]);
    EXPECT_EQ(600, strided_vals[5]);

    // 2. Verify HDF-only vs common APIs on netCDF files
    EXPECT_EQ(FAIL, SDidtoref(sds_id));
    EXPECT_EQ(FAIL, SDreftoindex(sd_id, 1));
    EXPECT_EQ(FAIL, SDsetaccesstype(sds_id, DFACC_SERIAL));
    EXPECT_EQ(SUCCEED, SDsetblocksize(sds_id, 512));
    int32_t blk = 0;
    EXPECT_EQ(FAIL, SDgetblocksize(sds_id, &blk));
    EXPECT_EQ(SUCCEED, SDsetdimval_comp(dim_fixed, SD_DIMVAL_BW_COMP));
    EXPECT_EQ(SD_DIMVAL_BW_COMP, SDisdimval_bwcomp(dim_fixed));
    EXPECT_EQ(FAIL, SDsetexternalfile(sds_id, "foo.bin", 0));
    char ext_b[16] = "";
    int32_t e_off = 0, e_len = 0;
    EXPECT_EQ(FAIL, SDgetexternalinfo(sds_id, 16, ext_b, &e_off, &e_len));
    EXPECT_EQ(FAIL, SDgetexternalfile(sds_id, 16, ext_b, &e_off));
    EXPECT_EQ(FAIL, SDsetnbitdataset(sds_id, 0, 16, FALSE, FALSE));
    comp_info ci{};
    EXPECT_EQ(FAIL, SDsetcompress(sds_id, COMP_CODE_RLE, &ci));
    comp_coder_t ct = COMP_CODE_INVALID;
    EXPECT_EQ(FAIL, SDgetcompinfo(sds_id, &ct, &ci));
    EXPECT_EQ(FAIL, SDgetcomptype(sds_id, &ct));
    int32_t cs = 0, os = 0;
    EXPECT_EQ(FAIL, SDgetdatasize(sds_id, &cs, &os));
    HDF_CHUNK_DEF chk{};
    chk.chunk_lengths[0] = 1;
    chk.chunk_lengths[1] = 1;
    EXPECT_EQ(FAIL, SDsetchunk(sds_id, chk, HDF_CHUNK));
    int32_t chk_flg = 0;
    EXPECT_EQ(FAIL, SDgetchunkinfo(sds_id, &chk, &chk_flg));
    int32_t o_coords[2] = {0, 0};
    int32_t chk_val[1] = {0};
    EXPECT_EQ(FAIL, SDwritechunk(sds_id, o_coords, chk_val));
    EXPECT_EQ(FAIL, SDreadchunk(sds_id, o_coords, chk_val));
    EXPECT_EQ(FAIL, SDsetchunkcache(sds_id, 10, 0));
    int empty_flag = -1;
    EXPECT_EQ(SUCCEED, SDcheckempty(sds_id, &empty_flag));
    EXPECT_EQ(TRUE, empty_flag);
  }

  // 3. Open with DFACC_WRITE and append record 2 via SDwritedata (NC_NDIRTY
  // only) This exercises mfsd.c:394-401 (xdr_numrecs update on SDend) without
  // touching NC_HDIRTY
  {
    const int32_t write_sd = SDstart(file_path.c_str(), DFACC_WRITE);
    ASSERT_NE(FAIL, write_sd);
    absl::Cleanup close_wsd = [&write_sd] {
      EXPECT_EQ(SUCCEED, SDend(write_sd));
    };

    const int32_t write_sds = SDselect(write_sd, 2);
    ASSERT_NE(FAIL, write_sds);
    absl::Cleanup close_wsds = [&write_sds] {
      EXPECT_EQ(SUCCEED, SDendaccess(write_sds));
    };

    int32_t rec2_start[2] = {2, 0};
    int32_t rec2_edges[2] = {1, 3};
    int32_t rec2_vals[3] = {700, 800, 900};
    EXPECT_EQ(SUCCEED, SDwritedata(write_sds, rec2_start, nullptr, rec2_edges,
                                   rec2_vals));
  }

  // Reopen and verify record 2 was written and numrecs increased
  {
    const int32_t verify_sd = SDstart(file_path.c_str(), DFACC_READ);
    ASSERT_NE(FAIL, verify_sd);
    absl::Cleanup close_vsd = [&verify_sd] {
      EXPECT_EQ(SUCCEED, SDend(verify_sd));
    };

    const int32_t verify_sds = SDselect(verify_sd, 2);
    ASSERT_NE(FAIL, verify_sds);
    absl::Cleanup close_vsds = [&verify_sds] {
      EXPECT_EQ(SUCCEED, SDendaccess(verify_sds));
    };

    int32_t rec2_start[2] = {2, 0};
    int32_t rec2_edges[2] = {1, 3};
    int32_t rec2_read[3] = {0};
    EXPECT_EQ(SUCCEED, SDreaddata(verify_sds, rec2_start, nullptr, rec2_edges,
                                  rec2_read));
    EXPECT_EQ(700, rec2_read[0]);
    EXPECT_EQ(800, rec2_read[1]);
    EXPECT_EQ(900, rec2_read[2]);
  }
}

// ============================================================================
// Suite 9: Error Validation, Empty-file & Out-of-Bounds Sweep, and Edge
// Branches
// ============================================================================

TEST_F(MfsdTest, ErrorValidationAndRemainingBranches) {
  // 1. Empty-file and out-of-bounds ID sweep
  const std::string path_empty = GetTempFilePath("mfsd_empty_sweep.hdf");
  absl::Cleanup rm_empty = [&path_empty] {
    std::filesystem::remove(path_empty);
  };
  const int32_t empty_sd = SDstart(path_empty.c_str(), DFACC_CREATE);
  ASSERT_NE(FAIL, empty_sd);
  absl::Cleanup close_empty_sd = [empty_sd] {
    EXPECT_EQ(SUCCEED, SDend(empty_sd));
  };

  const int32_t empty_sds_id = (empty_sd & ~0xfffff) | (SDSTYPE << 16) | 0;
  const int32_t empty_dim_id = (empty_sd & ~0xfffff) | (DIMTYPE << 16) | 0;

  const std::string path_pop = GetTempFilePath("mfsd_pop_sweep.hdf");
  absl::Cleanup rm_pop = [&path_pop] { std::filesystem::remove(path_pop); };
  const int32_t pop_sd = SDstart(path_pop.c_str(), DFACC_CREATE);
  ASSERT_NE(FAIL, pop_sd);
  absl::Cleanup close_pop_sd = [pop_sd] { EXPECT_EQ(SUCCEED, SDend(pop_sd)); };

  int32_t dims[2] = {4, 4};
  const int32_t sds_id = SDcreate(pop_sd, "pop_var", DFNT_INT32, 2, dims);
  ASSERT_NE(FAIL, sds_id);
  absl::Cleanup close_sds = [sds_id] {
    EXPECT_EQ(SUCCEED, SDendaccess(sds_id));
  };

  const int32_t dim0 = SDgetdimid(sds_id, 0);
  ASSERT_NE(FAIL, dim0);

  const int32_t oob_sds_id = (pop_sd & ~0xfffff) | (SDSTYPE << 16) | 999;
  const int32_t oob_dim_id = (pop_sd & ~0xfffff) | (DIMTYPE << 16) | 999;

  // Selection and queries on empty file / invalid IDs
  EXPECT_EQ(FAIL, SDselect(empty_sd, 0));
  EXPECT_EQ(FAIL, SDselect(pop_sd, 999));

  char qname[64] = "";
  int32_t qrank = 0, qdims[H4_MAX_VAR_DIMS] = {0}, qnt = 0, qnattrs = 0;
  EXPECT_EQ(FAIL, SDgetinfo(FAIL, qname, &qrank, qdims, &qnt, &qnattrs));
  EXPECT_EQ(FAIL,
            SDgetinfo(empty_sds_id, qname, &qrank, qdims, &qnt, &qnattrs));
  EXPECT_EQ(FAIL, SDgetinfo(oob_sds_id, qname, &qrank, qdims, &qnt, &qnattrs));

  int32_t s_start[2] = {0, 0};
  int32_t s_edges[2] = {4, 4};
  int32_t s_buf[16] = {1};
  EXPECT_EQ(FAIL, SDreaddata(FAIL, s_start, nullptr, s_edges, s_buf));
  EXPECT_EQ(FAIL, SDreaddata(empty_sds_id, s_start, nullptr, s_edges, s_buf));
  EXPECT_EQ(FAIL, SDreaddata(oob_sds_id, s_start, nullptr, s_edges, s_buf));

  EXPECT_EQ(FAIL, SDwritedata(FAIL, s_start, nullptr, s_edges, s_buf));
  EXPECT_EQ(FAIL, SDwritedata(empty_sds_id, s_start, nullptr, s_edges, s_buf));
  EXPECT_EQ(FAIL, SDwritedata(oob_sds_id, s_start, nullptr, s_edges, s_buf));

  EXPECT_EQ(FAIL, SDnametoindex(FAIL, "pop_var"));
  EXPECT_EQ(FAIL, SDnametoindex(empty_sd, "pop_var"));
  EXPECT_EQ(FAIL, SDnametoindex(pop_sd, "nonexistent_var"));

  int32_t num_vars = 0;
  EXPECT_EQ(FAIL, SDgetnumvars_byname(FAIL, "pop_var", &num_vars));
  EXPECT_EQ(FAIL, SDgetnumvars_byname(empty_sd, "pop_var", &num_vars));

  hdf_varlist_t var_list[1] = {};
  EXPECT_EQ(FAIL, SDnametoindices(FAIL, "pop_var", var_list));
  EXPECT_EQ(FAIL, SDnametoindices(empty_sd, "pop_var", var_list));

  EXPECT_EQ(FAIL, SDgetdimid(FAIL, 0));
  EXPECT_EQ(FAIL, SDgetdimid(empty_sds_id, 0));
  EXPECT_EQ(FAIL, SDgetdimid(oob_sds_id, 0));

  EXPECT_EQ(FAIL, SDsetdimname(FAIL, "new_name"));
  EXPECT_EQ(FAIL, SDsetdimname(oob_dim_id, "new_name"));

  int32_t rmin = 0, rmax = 100;
  EXPECT_EQ(FAIL, SDsetrange(FAIL, &rmax, &rmin));
  EXPECT_EQ(FAIL, SDsetrange(oob_sds_id, &rmax, &rmin));

  EXPECT_EQ(FAIL, SDgetrange(FAIL, &rmax, &rmin));
  EXPECT_EQ(FAIL, SDgetrange(oob_sds_id, &rmax, &rmin));

  int32_t dummy_val = 42;
  EXPECT_EQ(FAIL, SDsetattr(FAIL, "dummy", DFNT_INT32, 1, &dummy_val));
  EXPECT_EQ(FAIL, SDsetattr(pop_sd, "dummy", 99999, 1, &dummy_val));

  EXPECT_EQ(FAIL, SDattrinfo(FAIL, 0, qname, &qnt, &qnattrs));
  EXPECT_EQ(FAIL, SDreadattr(FAIL, 0, &dummy_val));

  EXPECT_EQ(FAIL, SDsetdatastrs(FAIL, "l", "u", "f", "c"));
  EXPECT_EQ(FAIL, SDsetdatastrs(empty_sds_id, "l", "u", "f", "c"));
  EXPECT_EQ(FAIL, SDsetdatastrs(oob_sds_id, "l", "u", "f", "c"));

  char sl[64] = "", su[64] = "", sf[64] = "", sc[64] = "";
  EXPECT_EQ(FAIL, SDgetdatastrs(empty_sds_id, sl, su, sf, sc, 64));
  EXPECT_EQ(FAIL, SDgetdatastrs(oob_sds_id, sl, su, sf, sc, 64));

  double cal_d = 1.0;
  EXPECT_EQ(FAIL,
            SDsetcal(empty_sds_id, cal_d, cal_d, cal_d, cal_d, DFNT_INT32));
  EXPECT_EQ(FAIL, SDsetcal(oob_sds_id, cal_d, cal_d, cal_d, cal_d, DFNT_INT32));

  int32_t cal_nt = 0;
  EXPECT_EQ(FAIL, SDgetcal(FAIL, &cal_d, &cal_d, &cal_d, &cal_d, &cal_nt));
  EXPECT_EQ(FAIL,
            SDgetcal(empty_sds_id, &cal_d, &cal_d, &cal_d, &cal_d, &cal_nt));
  EXPECT_EQ(FAIL,
            SDgetcal(oob_sds_id, &cal_d, &cal_d, &cal_d, &cal_d, &cal_nt));

  EXPECT_EQ(FAIL, SDsetfillvalue(FAIL, &dummy_val));
  EXPECT_EQ(FAIL, SDsetfillvalue(oob_sds_id, &dummy_val));

  EXPECT_EQ(FAIL, SDgetfillvalue(FAIL, &dummy_val));
  EXPECT_EQ(FAIL, SDgetfillvalue(empty_sds_id, &dummy_val));
  EXPECT_EQ(FAIL, SDgetfillvalue(oob_sds_id, &dummy_val));
  EXPECT_EQ(FAIL,
            SDgetfillvalue(sds_id, &dummy_val));  // uninitialized fill value

  EXPECT_EQ(FAIL, SDsetdimstrs(FAIL, "l", "u", "f"));
  EXPECT_EQ(FAIL, SDsetdimstrs(oob_dim_id, "l", "u", "f"));

  EXPECT_EQ(FAIL, SDgetdimstrs(FAIL, sl, su, sf, 64));
  EXPECT_EQ(FAIL, SDgetdimstrs(dim0, sl, su, sf, -1));
  EXPECT_EQ(FAIL, SDgetdimstrs(empty_dim_id, sl, su, sf, 64));
  EXPECT_EQ(FAIL, SDgetdimstrs(oob_dim_id, sl, su, sf, 64));

  EXPECT_EQ(FAIL, SDsetdimscale(FAIL, 4, DFNT_INT32, s_buf));
  EXPECT_EQ(FAIL, SDsetdimscale(oob_dim_id, 4, DFNT_INT32, s_buf));

  EXPECT_EQ(FAIL, SDgetdimscale(oob_dim_id, s_buf));
  EXPECT_EQ(FAIL, SDgetdimscale(dim0, nullptr));
  EXPECT_EQ(FAIL, SDgetdimscale(dim0, s_buf));  // uninitialized scale

  EXPECT_EQ(FAIL, SDdiminfo(FAIL, qname, &qrank, &qnt, &qnattrs));
  EXPECT_EQ(FAIL, SDdiminfo(empty_dim_id, qname, &qrank, &qnt, &qnattrs));
  EXPECT_EQ(FAIL, SDdiminfo(oob_dim_id, qname, &qrank, &qnt, &qnattrs));

  EXPECT_EQ(FAIL, SDsetexternalfile(empty_sds_id, "ext.bin", 0));
  EXPECT_EQ(FAIL, SDsetexternalfile(oob_sds_id, "ext.bin", 0));

  int32_t ext_off = 0, ext_len = 0;
  char ext_fn[64] = "";
  EXPECT_EQ(FAIL,
            SDgetexternalinfo(empty_sds_id, 64, ext_fn, &ext_off, &ext_len));
  EXPECT_EQ(FAIL,
            SDgetexternalinfo(oob_sds_id, 64, ext_fn, &ext_off, &ext_len));

  EXPECT_EQ(FAIL, SDgetexternalfile(empty_sds_id, 64, ext_fn, &ext_off));
  EXPECT_EQ(FAIL, SDgetexternalfile(oob_sds_id, 64, ext_fn, &ext_off));

  EXPECT_EQ(FAIL, SDsetnbitdataset(empty_sds_id, 0, 16, TRUE, FALSE));
  EXPECT_EQ(FAIL, SDsetnbitdataset(oob_sds_id, 0, 16, TRUE, FALSE));

  comp_info comp_in{};
  EXPECT_EQ(FAIL, SDsetcompress(pop_sd, COMP_CODE_INVALID, &comp_in));
  EXPECT_EQ(FAIL, SDsetcompress(empty_sds_id, COMP_CODE_RLE, &comp_in));
  EXPECT_EQ(FAIL, SDsetcompress(oob_sds_id, COMP_CODE_RLE, &comp_in));

  const int32_t sds_scalar =
      SDcreate(pop_sd, "scalar_test", DFNT_INT32, 0, nullptr);
  ASSERT_NE(FAIL, sds_scalar);
  EXPECT_EQ(FAIL, SDsetcompress(sds_scalar, COMP_CODE_RLE, &comp_in));
  HDF_CHUNK_DEF chunk_def_scalar{};
  EXPECT_EQ(FAIL, SDsetchunk(sds_scalar, chunk_def_scalar, HDF_CHUNK));
  EXPECT_EQ(SUCCEED, SDendaccess(sds_scalar));

  int32_t u_dims[2] = {SD_UNLIMITED, 4};
  const int32_t sds_unl =
      SDcreate(pop_sd, "unl_compress_test", DFNT_INT32, 2, u_dims);
  ASSERT_NE(FAIL, sds_unl);
  EXPECT_EQ(FAIL, SDsetcompress(sds_unl, COMP_CODE_RLE, &comp_in));
  EXPECT_EQ(SUCCEED, SDendaccess(sds_unl));

  comp_coder_t comp_coder = COMP_CODE_INVALID;
  EXPECT_EQ(FAIL, SDgetcompinfo(sds_id, nullptr, &comp_in));
  EXPECT_EQ(FAIL, SDgetcompinfo(sds_id, &comp_coder, nullptr));
  EXPECT_EQ(FAIL, SDgetcompinfo(empty_sds_id, &comp_coder, &comp_in));
  EXPECT_EQ(FAIL, SDgetcompinfo(oob_sds_id, &comp_coder, &comp_in));

  EXPECT_EQ(FAIL, SDgetcomptype(sds_id, nullptr));
  EXPECT_EQ(FAIL, SDgetcomptype(empty_sds_id, &comp_coder));
  EXPECT_EQ(FAIL, SDgetcomptype(oob_sds_id, &comp_coder));

  int32_t csize = -1, osize = -1;
  EXPECT_EQ(FAIL, SDgetdatasize(empty_sds_id, &csize, &osize));
  EXPECT_EQ(FAIL, SDgetdatasize(oob_sds_id, &csize, &osize));

  EXPECT_EQ(FAIL, SDfindattr(FAIL, "dummy"));
  EXPECT_EQ(FAIL, SDidtoref(FAIL));
  EXPECT_EQ(FAIL, SDidtoref(oob_sds_id));

  EXPECT_EQ(FALSE, SDisrecord(FAIL));
  EXPECT_EQ(FALSE, SDisrecord(oob_sds_id));

  EXPECT_EQ(FAIL, SDiscoordvar(FAIL));
  EXPECT_EQ(FAIL, SDiscoordvar(empty_sds_id));
  EXPECT_EQ(FAIL, SDiscoordvar(oob_sds_id));

  int32_t blk_sz = 0;
  EXPECT_EQ(FAIL, SDgetblocksize(FAIL, &blk_sz));
  EXPECT_EQ(FAIL, SDgetblocksize(oob_sds_id, &blk_sz));

  {
    const int32_t sds_bad_chunk =
        SDcreate(pop_sd, "bad_chunk_var", DFNT_INT32, 2, dims);
    ASSERT_NE(FAIL, sds_bad_chunk);
    absl::Cleanup close_bad_chunk = [sds_bad_chunk] {
      EXPECT_EQ(SUCCEED, SDendaccess(sds_bad_chunk));
    };
    HDF_CHUNK_DEF chunk_def_sweep{};
    EXPECT_EQ(FAIL, SDsetchunk(oob_sds_id, chunk_def_sweep, HDF_CHUNK));
    EXPECT_EQ(FAIL, SDsetchunk(sds_bad_chunk, chunk_def_sweep, 99));
    chunk_def_sweep.chunk_lengths[0] = 0;
    EXPECT_EQ(FAIL, SDsetchunk(sds_bad_chunk, chunk_def_sweep, HDF_CHUNK));
  }

  HDF_CHUNK_DEF chunk_def_sweep{};
  int32_t chk_flg = 0;
  EXPECT_EQ(FAIL, SDgetchunkinfo(oob_sds_id, &chunk_def_sweep, &chk_flg));

  int32_t chk_origin[2] = {0, 0};
  EXPECT_EQ(FAIL, SDwritechunk(sds_id, nullptr, s_buf));
  EXPECT_EQ(FAIL, SDwritechunk(sds_id, chk_origin, nullptr));
  EXPECT_EQ(FAIL, SDwritechunk(oob_sds_id, chk_origin, s_buf));

  EXPECT_EQ(FAIL, SDreadchunk(sds_id, nullptr, s_buf));
  EXPECT_EQ(FAIL, SDreadchunk(sds_id, chk_origin, nullptr));
  EXPECT_EQ(FAIL, SDreadchunk(oob_sds_id, chk_origin, s_buf));

  int empty_flag = -1;
  EXPECT_EQ(FAIL, SDcheckempty(FAIL, &empty_flag));
  EXPECT_EQ(FAIL, SDcheckempty(oob_sds_id, &empty_flag));

  // 2. Chunking, Native Conversion (convert == 0), and Non-Chunked Edge Cases
  {
    const int32_t sds_native =
        SDcreate(pop_sd, "chunk_native", DFNT_INT32 | DFNT_NATIVE, 2, dims);
    ASSERT_NE(FAIL, sds_native);
    absl::Cleanup close_native = [sds_native] {
      EXPECT_EQ(SUCCEED, SDendaccess(sds_native));
    };

    int32_t fill_val = -1;
    EXPECT_EQ(SUCCEED, SDsetfillvalue(sds_native, &fill_val));

    HDF_CHUNK_DEF native_cdef{};
    native_cdef.chunk_lengths[0] = 2;
    native_cdef.chunk_lengths[1] = 2;
    EXPECT_EQ(SUCCEED, SDsetchunk(sds_native, native_cdef, HDF_CHUNK));

    int32_t origin[2] = {0, 0};
    int32_t chunk_data[4] = {10, 20, 30, 40};
    EXPECT_EQ(SUCCEED, SDwritechunk(sds_native, origin, chunk_data));

    int32_t read_chunk[4] = {0};
    EXPECT_EQ(SUCCEED, SDreadchunk(sds_native, origin, read_chunk));
    EXPECT_EQ(10, read_chunk[0]);
    EXPECT_EQ(40, read_chunk[3]);
  }

  // Regular non-chunked SDS error checks
  {
    const int32_t sds_reg =
        SDcreate(pop_sd, "reg_for_chunk_err", DFNT_INT32, 2, dims);
    ASSERT_NE(FAIL, sds_reg);
    absl::Cleanup close_reg = [sds_reg] {
      EXPECT_EQ(SUCCEED, SDendaccess(sds_reg));
    };

    EXPECT_EQ(SUCCEED, SDwritedata(sds_reg, s_start, nullptr, s_edges, s_buf));

    HDF_CHUNK_DEF cdef{};
    cdef.chunk_lengths[0] = 2;
    cdef.chunk_lengths[1] = 2;
    EXPECT_EQ(FAIL, SDsetchunk(sds_reg, cdef, HDF_CHUNK));

    HDF_CHUNK_DEF reg_cinfo{};
    int32_t reg_flg = -1;
    EXPECT_EQ(SUCCEED, SDgetchunkinfo(sds_reg, &reg_cinfo, &reg_flg));
    EXPECT_EQ(HDF_NONE, reg_flg);

    int32_t origin[2] = {0, 0};
    int32_t chunk_buf[4] = {0};
    EXPECT_EQ(FAIL, SDwritechunk(sds_reg, origin, chunk_buf));
    EXPECT_EQ(FAIL, SDreadchunk(sds_reg, origin, chunk_buf));

    // External file on existing regular dataset and queries
    EXPECT_EQ(0, SDgetexternalinfo(sds_reg, sizeof(ext_fn), ext_fn, &ext_off,
                                   &ext_len));
    EXPECT_EQ(FAIL,
              SDgetexternalfile(sds_reg, sizeof(ext_fn), ext_fn, &ext_off));
    const std::string ext_exist_path = GetTempFilePath("mfsd_ext_exist.bin");
    absl::Cleanup rm_ext_exist = [&ext_exist_path] {
      std::filesystem::remove(ext_exist_path);
    };
    EXPECT_EQ(SUCCEED, SDsetexternalfile(sds_reg, ext_exist_path.c_str(), 0));
  }

  // External element sidecar
  {
    const int32_t sds_ext = SDcreate(pop_sd, "ext_var", DFNT_INT32, 2, dims);
    ASSERT_NE(FAIL, sds_ext);
    absl::Cleanup close_ext = [sds_ext] {
      EXPECT_EQ(SUCCEED, SDendaccess(sds_ext));
    };

    const std::string ext_path = GetTempFilePath("mfsd_sidecar.bin");
    absl::Cleanup rm_ext = [&ext_path] { std::filesystem::remove(ext_path); };

    EXPECT_EQ(SUCCEED, SDsetexternalfile(sds_ext, ext_path.c_str(), 0));
    EXPECT_EQ(SUCCEED, SDwritedata(sds_ext, s_start, nullptr, s_edges, s_buf));

    EXPECT_EQ(FAIL,
              SDgetexternalinfo(sds_ext, 16, nullptr, &ext_off, &ext_len));
    EXPECT_EQ(FAIL, SDgetexternalfile(sds_ext, 16, nullptr, &ext_off));
  }

  // 3. Unwritten SDS queries
  {
    comp_coder_t coder = COMP_CODE_INVALID;
    comp_info cinfo{};
    EXPECT_EQ(SUCCEED, SDgetcompinfo(sds_id, &coder, &cinfo));
    EXPECT_EQ(COMP_CODE_NONE, coder);

    EXPECT_EQ(SUCCEED, SDgetcomptype(sds_id, &coder));
    EXPECT_EQ(COMP_CODE_NONE, coder);

    int32_t csize = -1, osize = -1;
    EXPECT_EQ(SUCCEED, SDgetdatasize(sds_id, &csize, &osize));
    EXPECT_EQ(0, csize);
    EXPECT_EQ(0, osize);
  }

  // Linked-block reopen and existing Vgroup compression
  {
    const std::string reopen_path = GetTempFilePath("mfsd_reopen_blk.hdf");
    absl::Cleanup rm_reopen = [&reopen_path] {
      std::filesystem::remove(reopen_path);
    };

    {
      const int32_t reopen_sd = SDstart(reopen_path.c_str(), DFACC_CREATE);
      ASSERT_NE(FAIL, reopen_sd);
      absl::Cleanup close_r1 = [reopen_sd] {
        EXPECT_EQ(SUCCEED, SDend(reopen_sd));
      };

      const int32_t sds_blk =
          SDcreate(reopen_sd, "blk_var", DFNT_INT32, 2, dims);
      ASSERT_NE(FAIL, sds_blk);
      EXPECT_EQ(SUCCEED, SDsetblocksize(sds_blk, 4096));
      EXPECT_EQ(SUCCEED,
                SDwritedata(sds_blk, s_start, nullptr, s_edges, s_buf));
      EXPECT_EQ(SUCCEED, SDendaccess(sds_blk));

      const int32_t sds_vgrp =
          SDcreate(reopen_sd, "vgrp_var", DFNT_INT32, 2, dims);
      ASSERT_NE(FAIL, sds_vgrp);
      int32_t a_val = 1;
      EXPECT_EQ(SUCCEED, SDsetattr(sds_vgrp, "a", DFNT_INT32, 1, &a_val));
      EXPECT_EQ(SUCCEED, SDendaccess(sds_vgrp));
    }

    {
      const int32_t r_sd = SDstart(reopen_path.c_str(), DFACC_WRITE);
      ASSERT_NE(FAIL, r_sd);
      absl::Cleanup close_r2 = [r_sd] { EXPECT_EQ(SUCCEED, SDend(r_sd)); };

      const int32_t r_sds_blk = SDselect(r_sd, 0);
      ASSERT_NE(FAIL, r_sds_blk);
      absl::Cleanup close_rblk = [r_sds_blk] {
        EXPECT_EQ(SUCCEED, SDendaccess(r_sds_blk));
      };
      int32_t blk_len = 0;
      EXPECT_EQ(SUCCEED, SDgetblocksize(r_sds_blk, &blk_len));
      EXPECT_EQ(4096, blk_len);

      const int32_t r_sds_vgrp = SDselect(r_sd, 1);
      ASSERT_NE(FAIL, r_sds_vgrp);
      absl::Cleanup close_rvgrp = [r_sds_vgrp] {
        EXPECT_EQ(SUCCEED, SDendaccess(r_sds_vgrp));
      };
      comp_info rle_info{};
      EXPECT_EQ(SUCCEED, SDsetcompress(r_sds_vgrp, COMP_CODE_RLE, &rle_info));
    }
  }

  // 4. Coordinate Variables, Dimension Strings/Scales, and valid_max/valid_min
  {
    int32_t d1[1] = {5};
    const int32_t sds_1d = SDcreate(pop_sd, "dim_var_test", DFNT_INT32, 1, d1);
    ASSERT_NE(FAIL, sds_1d);
    absl::Cleanup close_1d = [sds_1d] {
      EXPECT_EQ(SUCCEED, SDendaccess(sds_1d));
    };

    const int32_t d1_dim = SDgetdimid(sds_1d, 0);
    ASSERT_NE(FAIL, d1_dim);
    EXPECT_EQ(SUCCEED, SDsetdimname(d1_dim, "coord_dim"));
    int32_t scale_vals[5] = {1, 2, 3, 4, 5};
    EXPECT_EQ(SUCCEED, SDsetdimscale(d1_dim, 5, DFNT_INT32, scale_vals));

    const int32_t cv_idx = SDnametoindex(pop_sd, "coord_dim");
    ASSERT_NE(FAIL, cv_idx);
    const int32_t cv_sds = SDselect(pop_sd, cv_idx);
    ASSERT_NE(FAIL, cv_sds);
    absl::Cleanup close_cv = [cv_sds] {
      EXPECT_EQ(SUCCEED, SDendaccess(cv_sds));
    };
    EXPECT_EQ(TRUE, SDiscoordvar(cv_sds));

    char dname[64] = "";
    int32_t dcnt = 0, dnt = 0, dnattrs = 0;
    EXPECT_EQ(SUCCEED, SDdiminfo(d1_dim, dname, &dcnt, &dnt, &dnattrs));
    EXPECT_EQ(DFNT_INT32, dnt);

    // UNKNOWN var_type branch in SDiscoordvar
    NC* handle = SDIhandle_from_id(pop_sd, CDFTYPE);
    ASSERT_THAT(handle, NotNull());
    NC_var* vp = SDIget_var(handle, cv_sds);
    ASSERT_THAT(vp, NotNull());
    {
      NC_string* const orig_name = vp->name;
      const hdf_vartype_t orig_type = vp->var_type;
      NC_string* diff_len = NC_new_string(4, "diff");
      NC_string* same_len = NC_new_string(orig_name->len, "coord_xxx");
      ASSERT_THAT(diff_len, NotNull());
      ASSERT_THAT(same_len, NotNull());
      absl::Cleanup restore_vp = [vp, orig_name, orig_type, diff_len,
                                  same_len] {
        vp->name = orig_name;
        vp->var_type = orig_type;
        NC_free_string(diff_len);
        NC_free_string(same_len);
      };

      vp->var_type = UNKNOWN;
      EXPECT_EQ(TRUE, SDiscoordvar(cv_sds));

      vp->name = diff_len;
      EXPECT_EQ(FALSE, SDiscoordvar(cv_sds));

      vp->name = same_len;
      EXPECT_EQ(FALSE, SDiscoordvar(cv_sds));
    }

    // getdimstrs and getdatastrs empty attributes checks
    EXPECT_EQ(SUCCEED, SDgetdimstrs(dim0, sl, su, sf, 64));
    EXPECT_STREQ("", sl);
    EXPECT_STREQ("", su);
    EXPECT_STREQ("", sf);

    EXPECT_EQ(SUCCEED, SDgetdimstrs(d1_dim, sl, su, sf, 64));
    EXPECT_STREQ("", sl);
    EXPECT_STREQ("", su);
    EXPECT_STREQ("", sf);

    EXPECT_EQ(SUCCEED, SDgetdatastrs(sds_id, sl, su, sf, sc, 64));
    EXPECT_STREQ("", sl);
    EXPECT_STREQ("", su);
    EXPECT_STREQ("", sf);
    EXPECT_STREQ("", sc);
  }

  // valid_max and valid_min in SDgetrange
  {
    const int32_t sds_range =
        SDcreate(pop_sd, "range_test_var", DFNT_INT32, 2, dims);
    ASSERT_NE(FAIL, sds_range);
    absl::Cleanup close_rng = [sds_range] {
      EXPECT_EQ(SUCCEED, SDendaccess(sds_range));
    };

    EXPECT_EQ(FAIL, SDgetrange(sds_range, &rmax, &rmin));

    double bad_max = 100.0, bad_min = 0.0;
    EXPECT_EQ(SUCCEED,
              SDsetattr(sds_range, "valid_max", DFNT_FLOAT64, 1, &bad_max));
    EXPECT_EQ(SUCCEED,
              SDsetattr(sds_range, "valid_min", DFNT_FLOAT64, 1, &bad_min));
    EXPECT_EQ(FAIL, SDgetrange(sds_range, &rmax, &rmin));

    int32_t good_max = 500, good_min = 10;
    EXPECT_EQ(SUCCEED,
              SDsetattr(sds_range, "valid_max", DFNT_INT32, 1, &good_max));
    EXPECT_EQ(SUCCEED,
              SDsetattr(sds_range, "valid_min", DFNT_INT32, 1, &good_min));
    int32_t qmin = 0, qmax = 0;
    EXPECT_EQ(SUCCEED, SDgetrange(sds_range, &qmax, &qmin));
    EXPECT_EQ(500, qmax);
    EXPECT_EQ(10, qmin);
  }

  // Progressive missing attrs in SDgetcal
  {
    const int32_t sds_cal =
        SDcreate(pop_sd, "cal_test_var", DFNT_INT32, 2, dims);
    ASSERT_NE(FAIL, sds_cal);
    absl::Cleanup close_cal = [sds_cal] {
      EXPECT_EQ(SUCCEED, SDendaccess(sds_cal));
    };

    double cal = 1.0, cale = 0.0, ioff = 0.0, ioffe = 0.0;
    int32_t nt = 0;
    EXPECT_EQ(FAIL, SDgetcal(sds_cal, &cal, &cale, &ioff, &ioffe, &nt));

    double val_d = 2.0;
    EXPECT_EQ(SUCCEED,
              SDsetattr(sds_cal, "scale_factor", DFNT_FLOAT64, 1, &val_d));
    EXPECT_EQ(FAIL, SDgetcal(sds_cal, &cal, &cale, &ioff, &ioffe, &nt));

    EXPECT_EQ(SUCCEED,
              SDsetattr(sds_cal, "scale_factor_err", DFNT_FLOAT64, 1, &val_d));
    EXPECT_EQ(FAIL, SDgetcal(sds_cal, &cal, &cale, &ioff, &ioffe, &nt));

    EXPECT_EQ(SUCCEED,
              SDsetattr(sds_cal, "add_offset", DFNT_FLOAT64, 1, &val_d));
    EXPECT_EQ(FAIL, SDgetcal(sds_cal, &cal, &cale, &ioff, &ioffe, &nt));

    EXPECT_EQ(SUCCEED,
              SDsetattr(sds_cal, "add_offset_err", DFNT_FLOAT64, 1, &val_d));
    EXPECT_EQ(FAIL, SDgetcal(sds_cal, &cal, &cale, &ioff, &ioffe, &nt));

    int32_t cal_nt = DFNT_INT32;
    EXPECT_EQ(SUCCEED,
              SDsetattr(sds_cal, "calibrated_nt", DFNT_INT32, 1, &cal_nt));
    EXPECT_EQ(SUCCEED, SDgetcal(sds_cal, &cal, &cale, &ioff, &ioffe, &nt));
    EXPECT_EQ(DFNT_INT32, nt);
  }

  // Unlimited HDF SDS scale, getinfo, strided read, and checkempty
  {
    const std::string unl_path = GetTempFilePath("mfsd_unl_rec.hdf");
    absl::Cleanup rm_unl = [&unl_path] { std::filesystem::remove(unl_path); };

    const int32_t unl_sd = SDstart(unl_path.c_str(), DFACC_CREATE);
    ASSERT_NE(FAIL, unl_sd);
    absl::Cleanup close_unl_sd = [unl_sd] {
      EXPECT_EQ(SUCCEED, SDend(unl_sd));
    };

    int32_t udims[2] = {SD_UNLIMITED, 3};
    const int32_t u_sds = SDcreate(unl_sd, "unl_sds", DFNT_INT32, 2, udims);
    ASSERT_NE(FAIL, u_sds);
    absl::Cleanup close_usds = [u_sds] {
      EXPECT_EQ(SUCCEED, SDendaccess(u_sds));
    };

    int eflag = -1;
    EXPECT_EQ(SUCCEED, SDcheckempty(u_sds, &eflag));
    EXPECT_EQ(1, eflag);

    int32_t ustart[2] = {0, 0};
    int32_t uedges[2] = {2, 3};
    int32_t udata[6] = {1, 2, 3, 4, 5, 6};
    EXPECT_EQ(SUCCEED, SDwritedata(u_sds, ustart, nullptr, uedges, udata));

    EXPECT_EQ(SUCCEED, SDcheckempty(u_sds, &eflag));
    EXPECT_EQ(0, eflag);

    int32_t qdims2[H4_MAX_VAR_DIMS] = {0};
    EXPECT_EQ(SUCCEED,
              SDgetinfo(u_sds, nullptr, &qrank, qdims2, &qnt, &qnattrs));
    EXPECT_EQ(2, qdims2[0]);

    const int32_t u_dim0 = SDgetdimid(u_sds, 0);
    ASSERT_NE(FAIL, u_dim0);
    int32_t uscale[2] = {100, 200};
    EXPECT_EQ(SUCCEED, SDsetdimscale(u_dim0, 2, DFNT_INT32, uscale));
    int32_t uscale_read[2] = {0};
    EXPECT_EQ(SUCCEED, SDgetdimscale(u_dim0, uscale_read));
    EXPECT_EQ(100, uscale_read[0]);
    EXPECT_EQ(200, uscale_read[1]);

    int32_t ustride[2] = {1, 1};
    int32_t uread[6] = {0};
    EXPECT_EQ(SUCCEED, SDreaddata(u_sds, ustart, ustride, uedges, uread));
    EXPECT_EQ(1, uread[0]);
    EXPECT_EQ(6, uread[5]);

    int32_t bad_stride[2] = {1, 10};
    int32_t bad_edges[2] = {1, 2};
    EXPECT_EQ(FAIL, SDreaddata(u_sds, ustart, bad_stride, bad_edges, uread));
  }

  // Fixed SDS with SD_NOFILL
  {
    EXPECT_NE(FAIL, SDsetfillmode(pop_sd, SD_NOFILL));
    const int32_t nofill_sds =
        SDcreate(pop_sd, "nofill_fixed", DFNT_INT32, 2, dims);
    ASSERT_NE(FAIL, nofill_sds);
    EXPECT_EQ(SUCCEED,
              SDwritedata(nofill_sds, s_start, nullptr, s_edges, s_buf));
    EXPECT_EQ(SUCCEED, SDendaccess(nofill_sds));
    EXPECT_NE(FAIL, SDsetfillmode(pop_sd, SD_FILL));
  }
}

// TODO(schwehr): Cover remaining internal netCDF-3 and parallel SD entry points
// when H4_HAVE_NETCDF or parallel HDF4 configurations are enabled in google3.

}  // namespace
}  // namespace hdf4
