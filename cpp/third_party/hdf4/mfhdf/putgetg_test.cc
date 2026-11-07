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
// Tests generalized strided/mapped NetCDF-2 hyperslab I/O
// (mfhdf/src/putgetg.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/mfhdf/src/putgetg.c

#include <cstdint>
#include <filesystem>  // NOLINT
#include <numeric>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf4/hdf/src/hdf.h"

// clang-format off
#include "third_party/hdf4/mfhdf/src/nc_priv.h"
// clang-format on

namespace hdf4 {
namespace {

using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::Ge;
using ::testing::Pointwise;

class PutgetgTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_ncopts_ = ncopts;
    saved_ncerr_ = ncerr;
    ncopts = 0;
    ncerr = NC_NOERR;
  }

  void TearDown() override {
    SDPfreebuf();
    HEclear();
    ncopts = saved_ncopts_;
    ncerr = saved_ncerr_;
  }

  std::string GetTempFilePath(const std::string& name) {
    return (std::filesystem::path(::testing::TempDir()) / name).string();
  }

 private:
  int saved_ncopts_ = 0;
  int saved_ncerr_ = 0;
};

TEST_F(PutgetgTest, InvalidCdfidAndVaridReturnsError) {
  int32_t val = 42;
  const long start[] = {0};
  const long count[] = {1};
  const long stride[] = {1};
  const long imap[] = {sizeof(int32_t)};

  // Invalid cdfid
  EXPECT_THAT(ncvarputg(-1, 0, start, count, stride, imap, &val), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EBADID));

  EXPECT_THAT(ncvargetg(-1, 0, start, count, stride, imap, &val), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EBADID));

  EXPECT_THAT(ncvarputs(-1, 0, start, count, stride, &val), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EBADID));

  EXPECT_THAT(ncvargets(-1, 0, start, count, stride, &val), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EBADID));

  // Valid cdfid with invalid varid
  const std::string file_path = GetTempFilePath("putgetg_inval.hdf");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  const int cdfid = nccreate(file_path.c_str(), NC_CLOBBER);
  ASSERT_THAT(cdfid, Ge(0));
  absl::Cleanup close_cdf = [cdfid] { (void)ncclose(cdfid); };

  EXPECT_THAT(ncvarputg(cdfid, 999, start, count, stride, imap, &val), Eq(-1));
}

TEST_F(PutgetgTest, ScalarVariableIo) {
  const std::string file_path = GetTempFilePath("putgetg_scalar.hdf");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  const int cdfid = nccreate(file_path.c_str(), NC_CLOBBER);
  ASSERT_THAT(cdfid, Ge(0));
  absl::Cleanup close_cdf = [cdfid] { (void)ncclose(cdfid); };

  // ndims = 0 creates a scalar variable
  const int varid = ncvardef(cdfid, "scalar_val", NC_LONG, 0, nullptr);
  ASSERT_THAT(varid, Ge(0));
  ASSERT_THAT(ncendef(cdfid), Eq(0));

  int32_t write_val = 12345;
  EXPECT_THAT(
      ncvarputg(cdfid, varid, nullptr, nullptr, nullptr, nullptr, &write_val),
      Eq(0));

  int32_t read_val = 0;
  EXPECT_THAT(
      ncvargetg(cdfid, varid, nullptr, nullptr, nullptr, nullptr, &read_val),
      Eq(0));
  EXPECT_THAT(read_val, Eq(write_val));

  // Also verify ncvarputs and ncvargets on scalar
  write_val = 67890;
  EXPECT_THAT(ncvarputs(cdfid, varid, nullptr, nullptr, nullptr, &write_val),
              Eq(0));
  read_val = 0;
  EXPECT_THAT(ncvargets(cdfid, varid, nullptr, nullptr, nullptr, &read_val),
              Eq(0));
  EXPECT_THAT(read_val, Eq(write_val));
}

TEST_F(PutgetgTest, NonPositiveStrideReturnsInvalid) {
  const std::string file_path = GetTempFilePath("putgetg_bad_stride.hdf");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  const int cdfid = nccreate(file_path.c_str(), NC_CLOBBER);
  ASSERT_THAT(cdfid, Ge(0));
  absl::Cleanup close_cdf = [cdfid] { (void)ncclose(cdfid); };

  const int dim0 = ncdimdef(cdfid, "y", 3);
  ASSERT_THAT(dim0, Ge(0));
  const int dim1 = ncdimdef(cdfid, "x", 4);
  ASSERT_THAT(dim1, Ge(0));
  const int dims[] = {dim0, dim1};

  const int varid = ncvardef(cdfid, "data", NC_LONG, 2, dims);
  ASSERT_THAT(varid, Ge(0));
  ASSERT_THAT(ncendef(cdfid), Eq(0));

  const long start[] = {0, 0};
  const long count[] = {2, 2};
  int32_t buf[4] = {1, 2, 3, 4};

  // Zero stride in second dimension
  const long bad_stride0[] = {1, 0};
  EXPECT_THAT(ncvarputg(cdfid, varid, start, count, bad_stride0, nullptr, buf),
              Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EINVAL));

  // Negative stride in first dimension
  const long bad_stride1[] = {-1, 1};
  EXPECT_THAT(ncvarputs(cdfid, varid, start, count, bad_stride1, buf), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EINVAL));
}

TEST_F(PutgetgTest, DefaultParametersWithUnityStrideOptimization) {
  const std::string file_path = GetTempFilePath("putgetg_unity.hdf");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  const int cdfid = nccreate(file_path.c_str(), NC_CLOBBER);
  ASSERT_THAT(cdfid, Ge(0));
  absl::Cleanup close_cdf = [cdfid] { (void)ncclose(cdfid); };

  const int dim_y = ncdimdef(cdfid, "y", 3);
  ASSERT_THAT(dim_y, Ge(0));
  const int dim_x = ncdimdef(cdfid, "x", 4);
  ASSERT_THAT(dim_x, Ge(0));
  const int dims[] = {dim_y, dim_x};

  const int varid = ncvardef(cdfid, "grid", NC_LONG, 2, dims);
  ASSERT_THAT(varid, Ge(0));
  ASSERT_THAT(ncendef(cdfid), Eq(0));

  std::vector<int32_t> write_data(12);
  std::iota(write_data.begin(), write_data.end(), 100);

  // count == nullptr, stride == nullptr, imap == nullptr uses full shape and
  // unity stride
  const long start[] = {0, 0};
  EXPECT_THAT(ncvarputg(cdfid, varid, start, nullptr, nullptr, nullptr,
                        write_data.data()),
              Eq(0));

  std::vector<int32_t> read_data(12, 0);
  EXPECT_THAT(ncvargetg(cdfid, varid, start, nullptr, nullptr, nullptr,
                        read_data.data()),
              Eq(0));
  EXPECT_THAT(read_data, Pointwise(Eq(), write_data));
}

TEST_F(PutgetgTest, UnlimitedRecordVariableDefaultCount) {
  const std::string file_path = GetTempFilePath("putgetg_unlim.hdf");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  const int cdfid = nccreate(file_path.c_str(), NC_CLOBBER);
  ASSERT_THAT(cdfid, Ge(0));
  absl::Cleanup close_cdf = [cdfid] { (void)ncclose(cdfid); };

  const int dim_rec = ncdimdef(cdfid, "rec", NC_UNLIMITED);
  ASSERT_THAT(dim_rec, Ge(0));
  const int dim_x = ncdimdef(cdfid, "x", 4);
  ASSERT_THAT(dim_x, Ge(0));
  const int dims[] = {dim_rec, dim_x};

  const int varid = ncvardef(cdfid, "timeseries", NC_LONG, 2, dims);
  ASSERT_THAT(varid, Ge(0));
  ASSERT_THAT(ncendef(cdfid), Eq(0));

  // Write 3 records of 4 elements each (12 elements)
  std::vector<int32_t> write_data(12);
  std::iota(write_data.begin(), write_data.end(), 1);

  const long start0[] = {0, 0};
  const long count0[] = {3, 4};
  EXPECT_THAT(ncvarputg(cdfid, varid, start0, count0, nullptr, nullptr,
                        write_data.data()),
              Eq(0));

  // Read starting at record 1 with count == nullptr: should read 2 records (8
  // elements)
  const long start1[] = {1, 0};
  std::vector<int32_t> read_data(8, 0);
  EXPECT_THAT(ncvargetg(cdfid, varid, start1, nullptr, nullptr, nullptr,
                        read_data.data()),
              Eq(0));

  std::vector<int32_t> expected_records(write_data.begin() + 4,
                                        write_data.end());
  EXPECT_THAT(read_data, Pointwise(Eq(), expected_records));
}

TEST_F(PutgetgTest, NonUnityStrideAndOdometerLoop) {
  const std::string file_path = GetTempFilePath("putgetg_stride.hdf");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  const int cdfid = nccreate(file_path.c_str(), NC_CLOBBER);
  ASSERT_THAT(cdfid, Ge(0));
  absl::Cleanup close_cdf = [cdfid] { (void)ncclose(cdfid); };

  const int dim_y = ncdimdef(cdfid, "y", 4);
  ASSERT_THAT(dim_y, Ge(0));
  const int dim_x = ncdimdef(cdfid, "x", 4);
  ASSERT_THAT(dim_x, Ge(0));
  const int dims[] = {dim_y, dim_x};

  const int varid = ncvardef(cdfid, "matrix", NC_LONG, 2, dims);
  ASSERT_THAT(varid, Ge(0));
  ASSERT_THAT(ncendef(cdfid), Eq(0));

  // Write 2x2 elements with stride 2 in both dimensions (hits (0,0), (0,2),
  // (2,0), (2,2))
  const long start[] = {0, 0};
  const long count[] = {2, 2};
  const long stride[] = {2, 2};
  int32_t write_vals[4] = {10, 20, 30, 40};

  EXPECT_THAT(ncvarputs(cdfid, varid, start, count, stride, write_vals), Eq(0));

  int32_t read_vals[4] = {0, 0, 0, 0};
  EXPECT_THAT(ncvargets(cdfid, varid, start, count, stride, read_vals), Eq(0));
  EXPECT_THAT(read_vals, ElementsAre(10, 20, 30, 40));
}

TEST_F(PutgetgTest, TransposedImapMapping) {
  const std::string file_path = GetTempFilePath("putgetg_imap.hdf");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  const int cdfid = nccreate(file_path.c_str(), NC_CLOBBER);
  ASSERT_THAT(cdfid, Ge(0));
  absl::Cleanup close_cdf = [cdfid] { (void)ncclose(cdfid); };

  const int dim_r = ncdimdef(cdfid, "row", 2);
  ASSERT_THAT(dim_r, Ge(0));
  const int dim_c = ncdimdef(cdfid, "col", 3);
  ASSERT_THAT(dim_c, Ge(0));
  const int dims[] = {dim_r, dim_c};

  const int varid = ncvardef(cdfid, "t_grid", NC_LONG, 2, dims);
  ASSERT_THAT(varid, Ge(0));
  ASSERT_THAT(ncendef(cdfid), Eq(0));

  // Memory buffer is stored column-major (3 rows, 2 columns):
  // valp + row * imap[0] + col * imap[1]
  // With imap = {sizeof(int32_t), 2 * sizeof(int32_t)}:
  // (r=0, c=0) -> 0; (r=1, c=0) -> 1; (r=0, c=1) -> 2; (r=1, c=1) -> 3; (r=0,
  // c=2) -> 4; (r=1, c=2) -> 5
  int32_t col_major[6] = {11, 21, 12, 22, 13, 23};

  const long start[] = {0, 0};
  const long count[] = {2, 3};
  const long stride[] = {1, 1};
  const long imap[] = {static_cast<long>(sizeof(int32_t)),
                       static_cast<long>(2 * sizeof(int32_t))};

  EXPECT_THAT(ncvarputg(cdfid, varid, start, count, stride, imap, col_major),
              Eq(0));

  int32_t read_col_major[6] = {0};
  EXPECT_THAT(
      ncvargetg(cdfid, varid, start, count, stride, imap, read_col_major),
      Eq(0));
  EXPECT_THAT(read_col_major, ElementsAre(11, 21, 12, 22, 13, 23));
}

TEST_F(PutgetgTest, OutOfBoundsStartReturnsError) {
  const std::string file_path = GetTempFilePath("putgetg_oob.hdf");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  const int cdfid = nccreate(file_path.c_str(), NC_CLOBBER);
  ASSERT_THAT(cdfid, Ge(0));
  absl::Cleanup close_cdf = [cdfid] { (void)ncclose(cdfid); };

  const int dim0 = ncdimdef(cdfid, "y", 2);
  ASSERT_THAT(dim0, Ge(0));
  const int dim1 = ncdimdef(cdfid, "x", 2);
  ASSERT_THAT(dim1, Ge(0));
  const int dims[] = {dim0, dim1};

  const int varid = ncvardef(cdfid, "small", NC_LONG, 2, dims);
  ASSERT_THAT(varid, Ge(0));
  ASSERT_THAT(ncendef(cdfid), Eq(0));

  const long oob_start[] = {100, 0};
  const long count[] = {1, 1};
  int32_t val = 99;
  EXPECT_THAT(ncvarputg(cdfid, varid, oob_start, count, nullptr, nullptr, &val),
              Eq(-1));
}

TEST_F(PutgetgTest, ReadOnlyFileRejectsWrites) {
  const std::string file_path = GetTempFilePath("putgetg_ro.hdf");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  {
    const int cdfid = nccreate(file_path.c_str(), NC_CLOBBER);
    ASSERT_THAT(cdfid, Ge(0));
    const int dim0 = ncdimdef(cdfid, "x", 4);
    ASSERT_THAT(dim0, Ge(0));
    const int dims[] = {dim0};
    const int varid = ncvardef(cdfid, "v", NC_LONG, 1, dims);
    ASSERT_THAT(varid, Ge(0));
    ASSERT_THAT(ncclose(cdfid), Eq(0));
  }

  const int ro_cdfid = ncopen(file_path.c_str(), NC_NOWRITE);
  ASSERT_THAT(ro_cdfid, Ge(0));
  absl::Cleanup close_ro = [ro_cdfid] { (void)ncclose(ro_cdfid); };

  const long start[] = {0};
  const long count[] = {1};
  int32_t val = 77;
  EXPECT_THAT(ncvarputg(ro_cdfid, 0, start, count, nullptr, nullptr, &val),
              Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EPERM));

  EXPECT_THAT(ncvarputs(ro_cdfid, 0, start, count, nullptr, &val), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EPERM));
}

// TODO(schwehr): putgetg.c has 100% isolated line coverage (98/98 lines).

}  // namespace
}  // namespace hdf4
