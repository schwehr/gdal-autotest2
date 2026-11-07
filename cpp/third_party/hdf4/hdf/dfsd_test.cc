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

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf4/hdf/src/dfsd_priv.h"
#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/hdf_priv.h"

namespace hdf4 {
namespace {

class DfsdTest : public ::testing::Test {
 protected:
  void SetUp() override { ResetGlobalState(); }

  void TearDown() override { ResetGlobalState(); }

 private:
  static void ResetGlobalState() {
    DFSDrestart();
    DFSDclear();
    const std::string dummy =
        std::filesystem::path(::testing::TempDir()) / "dfsd_reset.hdf";
    int32_t fid = DFSDIopen(dummy.c_str(), DFACC_CREATE);
    if (fid != FAIL) {
      Hclose(fid);
      (void)remove(dummy.c_str());
    }
    HEclear();
  }
};

TEST_F(DfsdTest, InvalidArguments) {
  int rank = 0;
  int32_t dims[3] = {0, 0, 0};
  float data[10] = {};

  // DFSDgetdims
  EXPECT_EQ(DFSDgetdims("", &rank, dims, 3), FAIL);
  EXPECT_EQ(DFSDgetdims("nonexistent.hdf", nullptr, dims, 3), FAIL);
  EXPECT_EQ(DFSDgetdims("nonexistent.hdf", &rank, nullptr, 3), FAIL);
  EXPECT_EQ(DFSDgetdims("nonexistent.hdf", &rank, dims, 0), FAIL);
  EXPECT_EQ(DFSDgetdims("nonexistent.hdf", &rank, dims, -1), FAIL);

  // DFSDgetdata
  EXPECT_EQ(DFSDgetdata("nonexistent.hdf", 0, dims, data), FAIL);
  EXPECT_EQ(DFSDgetdata("nonexistent.hdf", 2, nullptr, data), FAIL);
  EXPECT_EQ(DFSDgetdata("nonexistent.hdf", 2, dims, nullptr), FAIL);

  // DFSDputdata
  EXPECT_EQ(DFSDputdata("file.hdf", 0, dims, data), FAIL);
  EXPECT_EQ(DFSDputdata("file.hdf", 2, nullptr, data), FAIL);
  EXPECT_EQ(DFSDputdata("file.hdf", 2, dims, nullptr), FAIL);
  int32_t bad_dims[2] = {0, 5};
  EXPECT_EQ(DFSDputdata("file.hdf", 2, bad_dims, data), FAIL);

  // DFSDadddata
  EXPECT_EQ(DFSDadddata("file.hdf", 0, dims, data), FAIL);

  // DFSDreadref
  EXPECT_EQ(DFSDreadref(const_cast<char*>("nonexistent.hdf"), 1), FAIL);

  // DFSDndatasets
  EXPECT_EQ(DFSDndatasets(const_cast<char*>("nonexistent.hdf")), FAIL);

  // DFSDsetdims with valid pointers
  int32_t valid_dims[2] = {3, 4};
  EXPECT_EQ(DFSDsetdims(2, valid_dims), SUCCEED);

  // DFSDsetdimscale & DFSDgetdimscale
  EXPECT_EQ(DFSDsetdimscale(0, 3, data), FAIL);
  EXPECT_EQ(DFSDsetdimscale(100, 3, data), FAIL);
  EXPECT_EQ(DFSDsetdimscale(1, 0, data), FAIL);
  EXPECT_EQ(DFSDsetdimscale(1, 5, data), FAIL);
  EXPECT_EQ(DFSDgetdimscale(0, 3, data), FAIL);
  EXPECT_EQ(DFSDgetdimscale(100, 3, data), FAIL);
  EXPECT_EQ(DFSDgetdimscale(1, 0, data), FAIL);
  EXPECT_EQ(DFSDgetdimscale(1, 5, nullptr), FAIL);

  // DFSDsetdimstrs & DFSDgetdimstrs & DFSDgetdimlen
  EXPECT_EQ(DFSDsetdimstrs(0, "label", "unit", "format"), FAIL);
  EXPECT_EQ(DFSDsetdimstrs(100, "label", "unit", "format"), FAIL);
  char str_buf[64] = {};
  EXPECT_EQ(DFSDgetdimstrs(0, str_buf, str_buf, str_buf), FAIL);
  EXPECT_EQ(DFSDgetdimstrs(100, str_buf, str_buf, str_buf), FAIL);
  int len1 = 0, len2 = 0, len3 = 0;
  EXPECT_EQ(DFSDgetdimlen(100, &len1, &len2, &len3), FAIL);

  // DFSDgetdatalen
  int dl1 = 0, dl2 = 0, dl3 = 0, dl4 = 0;
  EXPECT_EQ(DFSDgetdatalen(&dl1, &dl2, &dl3, &dl4), FAIL);

  // DFSDsetNT
  EXPECT_EQ(DFSDsetNT(0), FAIL);
  EXPECT_EQ(DFSDsetNT(-1), FAIL);

  // DFSDsetcal & DFSDgetcal
  float64 cal_val = 0, cal_err = 0, ioff = 0, ioff_err = 0;
  int32_t cal_nt = 0;
  EXPECT_EQ(DFSDgetcal(&cal_val, &cal_err, &ioff, &ioff_err, &cal_nt), FAIL);

  // DFSDgetfillvalue without fresh data
  float dummy_fill = 0.0f;
  EXPECT_EQ(DFSDgetfillvalue(&dummy_fill), FAIL);

  // Non-existent file reads
  const std::string non_existent = "/non_existent_dir/missing.hdf";
  EXPECT_EQ(DFSDgetdims(non_existent.c_str(), &rank, dims, 3), FAIL);
  EXPECT_EQ(DFSDgetdata(non_existent.c_str(), 2, dims, data), FAIL);
}

TEST_F(DfsdTest, BasicPutAndGetData) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_basic.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  constexpr int rank = 2;
  int32_t dims[2] = {3, 4};
  std::vector<float> data(dims[0] * dims[1]);
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<float>(i * 1.5f);
  }

  // Write dataset
  EXPECT_EQ(DFSDputdata(path.c_str(), rank, dims, data.data()), SUCCEED);
  const uint16_t last_ref = DFSDlastref();
  EXPECT_NE(last_ref, 0);

  // Check dataset count
  EXPECT_EQ(DFSDndatasets(const_cast<char*>(path.c_str())), 1);

  // Read dimensions back
  int read_rank = 0;
  int32_t read_dims[3] = {};
  EXPECT_EQ(DFSDgetdims(path.c_str(), &read_rank, read_dims, 3), SUCCEED);
  EXPECT_EQ(read_rank, rank);
  EXPECT_EQ(read_dims[0], dims[0]);
  EXPECT_EQ(read_dims[1], dims[1]);

  // Read data back
  std::vector<float> read_data(dims[0] * dims[1], 0.0f);
  EXPECT_EQ(DFSDgetdata(path.c_str(), rank, dims, read_data.data()), SUCCEED);
  EXPECT_EQ(read_data, data);
}

TEST_F(DfsdTest, MultipleDatasetsAndNavigation) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_multi.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  // Dataset 1: int32 [2, 3]
  constexpr int rank1 = 2;
  int32_t dims1[2] = {2, 3};
  std::vector<int32_t> data1 = {10, 20, 30, 40, 50, 60};
  EXPECT_EQ(DFSDsetNT(DFNT_INT32), SUCCEED);
  EXPECT_EQ(DFSDputdata(path.c_str(), rank1, dims1, data1.data()), SUCCEED);
  const uint16_t ref1 = DFSDlastref();
  EXPECT_NE(ref1, 0);

  // Dataset 2: float64 [4] via DFSDadddata
  DFSDclear();
  constexpr int rank2 = 1;
  int32_t dims2[1] = {4};
  std::vector<double> data2 = {1.1, 2.2, 3.3, 4.4};
  EXPECT_EQ(DFSDsetNT(DFNT_FLOAT64), SUCCEED);
  EXPECT_EQ(DFSDadddata(path.c_str(), rank2, dims2, data2.data()), SUCCEED);
  const uint16_t ref2 = DFSDlastref();
  EXPECT_NE(ref2, 0);
  EXPECT_NE(ref1, ref2);

  // Dataset 3: uint8 [2, 2, 2]
  DFSDclear();
  constexpr int rank3 = 3;
  int32_t dims3[3] = {2, 2, 2};
  std::vector<uint8_t> data3 = {1, 2, 3, 4, 5, 6, 7, 8};
  EXPECT_EQ(DFSDsetNT(DFNT_UINT8), SUCCEED);
  EXPECT_EQ(DFSDadddata(path.c_str(), rank3, dims3, data3.data()), SUCCEED);
  const uint16_t ref3 = DFSDlastref();
  EXPECT_NE(ref3, 0);

  // Test DFSDwriteref to set next write target to existing ref1
  EXPECT_EQ(DFSDwriteref(path.c_str(), ref1), SUCCEED);
  EXPECT_EQ(DFSDlastref(), ref1);

  EXPECT_EQ(DFSDndatasets(const_cast<char*>(path.c_str())), 3);

  // Sequential reading: Dataset 1
  int read_rank = 0;
  int32_t read_dims[3] = {};
  EXPECT_EQ(DFSDgetdims(path.c_str(), &read_rank, read_dims, 3), SUCCEED);
  EXPECT_EQ(read_rank, rank1);
  EXPECT_EQ(read_dims[0], dims1[0]);
  EXPECT_EQ(read_dims[1], dims1[1]);
  std::vector<int32_t> rdata1(data1.size());
  EXPECT_EQ(DFSDsetNT(DFNT_INT32), SUCCEED);
  EXPECT_EQ(DFSDgetdata(path.c_str(), rank1, dims1, rdata1.data()), SUCCEED);
  EXPECT_EQ(rdata1, data1);

  // Sequential reading: Dataset 2
  EXPECT_EQ(DFSDgetdims(path.c_str(), &read_rank, read_dims, 3), SUCCEED);
  EXPECT_EQ(read_rank, rank2);
  EXPECT_EQ(read_dims[0], dims2[0]);
  std::vector<double> rdata2(data2.size());
  EXPECT_EQ(DFSDsetNT(DFNT_FLOAT64), SUCCEED);
  EXPECT_EQ(DFSDgetdata(path.c_str(), rank2, dims2, rdata2.data()), SUCCEED);
  EXPECT_EQ(rdata2, data2);

  // Sequential reading: Dataset 3
  EXPECT_EQ(DFSDgetdims(path.c_str(), &read_rank, read_dims, 3), SUCCEED);
  EXPECT_EQ(read_rank, rank3);
  std::vector<uint8_t> rdata3(data3.size());
  EXPECT_EQ(DFSDsetNT(DFNT_UINT8), SUCCEED);
  EXPECT_EQ(DFSDgetdata(path.c_str(), rank3, dims3, rdata3.data()), SUCCEED);
  EXPECT_EQ(rdata3, data3);

  // Sequential reading: EOF check (no 4th dataset)
  EXPECT_EQ(DFSDgetdims(path.c_str(), &read_rank, read_dims, 3), FAIL);

  // Restart to read from beginning
  EXPECT_EQ(DFSDrestart(), SUCCEED);
  EXPECT_EQ(DFSDgetdims(path.c_str(), &read_rank, read_dims, 3), SUCCEED);
  EXPECT_EQ(read_rank, rank1);

  // Seek directly to Dataset 2 using DFSDreadref
  EXPECT_EQ(DFSDreadref(const_cast<char*>(path.c_str()), ref2), SUCCEED);
  EXPECT_EQ(DFSDgetdims(path.c_str(), &read_rank, read_dims, 3), SUCCEED);
  EXPECT_EQ(read_rank, rank2);
  std::fill(rdata2.begin(), rdata2.end(), 0.0);
  EXPECT_EQ(DFSDsetNT(DFNT_FLOAT64), SUCCEED);
  EXPECT_EQ(DFSDgetdata(path.c_str(), rank2, dims2, rdata2.data()), SUCCEED);
  EXPECT_EQ(rdata2, data2);

  // Seek to non-existent ref
  EXPECT_EQ(DFSDreadref(const_cast<char*>(path.c_str()), 9999), FAIL);
}

TEST_F(DfsdTest, LabelsUnitsFormatsAndCoordsys) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_strings.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  constexpr int rank = 2;
  int32_t dims[2] = {2, 3};
  EXPECT_EQ(DFSDsetdims(rank, dims), SUCCEED);

  // Set maximum string lengths
  EXPECT_EQ(DFSDsetlengths(64, 32, 16, 48), SUCCEED);

  const char* datalabel = "Temperature";
  const char* dataunit = "Kelvin";
  const char* datafmt = "%.2f";
  const char* coordsys = "Cartesian";

  EXPECT_EQ(DFSDsetdatastrs(datalabel, dataunit, datafmt, coordsys), SUCCEED);

  // Set dimension 1 strings
  EXPECT_EQ(DFSDsetdimstrs(1, "Latitude", "degrees_north", "%.4f"), SUCCEED);
  // Set dimension 2 strings
  EXPECT_EQ(DFSDsetdimstrs(2, "Longitude", "degrees_east", "%.4f"), SUCCEED);

  std::vector<float> data = {10.0f, 20.0f, 30.0f, 40.0f, 50.0f, 60.0f};
  EXPECT_EQ(DFSDputdata(path.c_str(), rank, dims, data.data()), SUCCEED);

  int read_rank = 0;
  int32_t read_dims[2] = {};
  EXPECT_EQ(DFSDgetdims(path.c_str(), &read_rank, read_dims, 2), SUCCEED);

  // Read back and check data string lengths
  int l_label = 0, l_unit = 0, l_fmt = 0, l_coordsys = 0;
  EXPECT_EQ(DFSDgetdatalen(&l_label, &l_unit, &l_fmt, &l_coordsys), SUCCEED);
  EXPECT_GE(l_label, static_cast<int>(strlen(datalabel)));
  EXPECT_GE(l_unit, static_cast<int>(strlen(dataunit)));
  EXPECT_GE(l_fmt, static_cast<int>(strlen(datafmt)));
  EXPECT_GE(l_coordsys, static_cast<int>(strlen(coordsys)));

  char r_label[64] = {}, r_unit[64] = {}, r_fmt[64] = {}, r_coordsys[64] = {};
  EXPECT_EQ(DFSDgetdatastrs(r_label, r_unit, r_fmt, r_coordsys), SUCCEED);
  EXPECT_STREQ(r_label, datalabel);
  EXPECT_STREQ(r_unit, dataunit);
  EXPECT_STREQ(r_fmt, datafmt);
  EXPECT_STREQ(r_coordsys, coordsys);

  // Read back with some null string pointers
  char partial_label[64] = {};
  EXPECT_EQ(DFSDgetdatastrs(partial_label, nullptr, nullptr, nullptr), SUCCEED);
  EXPECT_STREQ(partial_label, datalabel);

  // Check dimension 1 strings and lengths
  int dl_label = 0, dl_unit = 0, dl_fmt = 0;
  EXPECT_EQ(DFSDgetdimlen(1, &dl_label, &dl_unit, &dl_fmt), SUCCEED);
  EXPECT_GE(dl_label, static_cast<int>(strlen("Latitude")));

  char rd_label[64] = {}, rd_unit[64] = {}, rd_fmt[64] = {};
  EXPECT_EQ(DFSDgetdimstrs(1, rd_label, rd_unit, rd_fmt), SUCCEED);
  EXPECT_STREQ(rd_label, "Latitude");
  EXPECT_STREQ(rd_unit, "degrees_north");
  EXPECT_STREQ(rd_fmt, "%.4f");

  // Dimension 2 strings
  EXPECT_EQ(DFSDgetdimstrs(2, rd_label, rd_unit, rd_fmt), SUCCEED);
  EXPECT_STREQ(rd_label, "Longitude");
  EXPECT_STREQ(rd_unit, "degrees_east");
}

TEST_F(DfsdTest, ScalesAndRangeAndCalibrationAndFill) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_scales_cal.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  constexpr int rank = 2;
  int32_t dims[2] = {3, 4};
  EXPECT_EQ(DFSDsetdims(rank, dims), SUCCEED);

  // Set dimension scales
  float scale1[3] = {10.0f, 20.0f, 30.0f};
  float scale2[4] = {100.0f, 200.0f, 300.0f, 400.0f};
  EXPECT_EQ(DFSDsetdimscale(1, dims[0], scale1), SUCCEED);
  EXPECT_EQ(DFSDsetdimscale(2, dims[1], scale2), SUCCEED);

  // Set range
  float max_val = 99.5f;
  float min_val = 1.5f;
  EXPECT_EQ(DFSDsetrange(&max_val, &min_val), SUCCEED);

  // Set calibration
  constexpr float64 cal = 2.0;
  constexpr float64 cal_err = 0.05;
  constexpr float64 ioff = 10.0;
  constexpr float64 ioff_err = 0.2;
  EXPECT_EQ(DFSDsetcal(cal, cal_err, ioff, ioff_err, DFNT_FLOAT32), SUCCEED);

  // Set fill value
  float fill_val = -999.0f;
  EXPECT_EQ(DFSDsetfillvalue(&fill_val), SUCCEED);

  std::vector<float> data(dims[0] * dims[1], 42.0f);
  EXPECT_EQ(DFSDputdata(path.c_str(), rank, dims, data.data()), SUCCEED);

  int read_rank = 0;
  int32_t read_dims[2] = {};
  EXPECT_EQ(DFSDgetdims(path.c_str(), &read_rank, read_dims, 2), SUCCEED);

  // Read dimension scales back
  float r_scale1[3] = {};
  float r_scale2[4] = {};
  EXPECT_EQ(DFSDgetdimscale(1, dims[0], r_scale1), SUCCEED);
  EXPECT_EQ(DFSDgetdimscale(2, dims[1], r_scale2), SUCCEED);
  for (int i = 0; i < dims[0]; ++i) EXPECT_FLOAT_EQ(r_scale1[i], scale1[i]);
  for (int i = 0; i < dims[1]; ++i) EXPECT_FLOAT_EQ(r_scale2[i], scale2[i]);

  // Read range back
  float r_max = 0.0f, r_min = 0.0f;
  EXPECT_EQ(DFSDgetrange(&r_max, &r_min), SUCCEED);
  EXPECT_FLOAT_EQ(r_max, max_val);
  EXPECT_FLOAT_EQ(r_min, min_val);

  // Read calibration back
  float64 r_cal = 0, r_cal_err = 0, r_ioff = 0, r_ioff_err = 0;
  int32_t r_cal_nt = 0;
  EXPECT_EQ(DFSDgetcal(&r_cal, &r_cal_err, &r_ioff, &r_ioff_err, &r_cal_nt),
            SUCCEED);
  EXPECT_DOUBLE_EQ(r_cal, cal);
  EXPECT_DOUBLE_EQ(r_cal_err, cal_err);
  EXPECT_DOUBLE_EQ(r_ioff, ioff);
  EXPECT_DOUBLE_EQ(r_ioff_err, ioff_err);
  EXPECT_EQ(r_cal_nt, DFNT_FLOAT32);

  // Read fill value back
  float r_fill = 0.0f;
  EXPECT_EQ(DFSDgetfillvalue(&r_fill), SUCCEED);
  EXPECT_FLOAT_EQ(r_fill, fill_val);
}

TEST_F(DfsdTest, NumberTypesAndConversions) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_types.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  constexpr int rank = 2;
  int32_t dims[2] = {2, 2};

  // 1. Int16 dataset
  std::vector<int16_t> i16_data = {100, 200, 300, 400};
  EXPECT_EQ(DFSDsetNT(DFNT_INT16), SUCCEED);
  EXPECT_EQ(DFSDputdata(path.c_str(), rank, dims, i16_data.data()), SUCCEED);

  // Read back as Int16
  int read_rank = 0;
  int32_t read_dims[2] = {};
  EXPECT_EQ(DFSDgetdims(path.c_str(), &read_rank, read_dims, 2), SUCCEED);
  std::vector<int16_t> read_i16(4, 0);
  int32_t file_nt = 0;
  EXPECT_EQ(DFSDgetNT(&file_nt), SUCCEED);
  EXPECT_EQ(file_nt, DFNT_INT16);
  EXPECT_EQ(DFSDgetdata(path.c_str(), rank, dims, read_i16.data()), SUCCEED);
  EXPECT_EQ(read_i16, i16_data);
}

TEST_F(DfsdTest, SliceIO) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_slice.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  constexpr int rank = 2;
  int32_t dims[2] = {4, 4};

  EXPECT_EQ(DFSDsetdims(rank, dims), SUCCEED);
  EXPECT_EQ(DFSDsetNT(DFNT_FLOAT32), SUCCEED);
  EXPECT_EQ(DFSDstartslice(path.c_str()), SUCCEED);

  // Write two slices (2 rows each)
  int32_t slice_dims1[2] = {2, 4};
  std::vector<float> slice1 = {1, 2, 3, 4, 5, 6, 7, 8};
  EXPECT_EQ(DFSDputslice(slice_dims1, slice1.data(), dims), SUCCEED);

  int32_t slice_dims2[2] = {2, 4};
  std::vector<float> slice2 = {9, 10, 11, 12, 13, 14, 15, 16};
  EXPECT_EQ(DFSDputslice(slice_dims2, slice2.data(), dims), SUCCEED);

  EXPECT_EQ(DFSDendslice(), SUCCEED);

  // Read whole data back
  std::vector<float> full_data(16, 0.0f);
  EXPECT_EQ(DFSDgetdata(path.c_str(), rank, dims, full_data.data()), SUCCEED);
  for (int i = 0; i < 8; ++i) EXPECT_FLOAT_EQ(full_data[i], slice1[i]);
  for (int i = 0; i < 8; ++i) EXPECT_FLOAT_EQ(full_data[8 + i], slice2[i]);

  // Read a sub-slice: 1-based indexing for winst.
  // winst = {2, 2} means row 2, col 2 (0-based: index 1, 1).
  // windims = {2, 2} means 2 rows, 2 columns.
  int32_t winst[2] = {2, 2};
  int32_t windims[2] = {2, 2};
  int32_t rslice_dims[2] = {2, 2};
  std::vector<float> rslice(4, 0.0f);
  EXPECT_EQ(
      DFSDgetslice(path.c_str(), winst, windims, rslice.data(), rslice_dims),
      SUCCEED);
  EXPECT_FLOAT_EQ(rslice[0], full_data[1 * 4 + 1]);  // 6.0f
  EXPECT_FLOAT_EQ(rslice[1], full_data[1 * 4 + 2]);  // 7.0f
  EXPECT_FLOAT_EQ(rslice[2], full_data[2 * 4 + 1]);  // 10.0f
  EXPECT_FLOAT_EQ(rslice[3], full_data[2 * 4 + 2]);  // 11.0f
}

TEST_F(DfsdTest, SlabIO) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_slab.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  constexpr int rank = 2;
  int32_t dims[2] = {4, 4};

  EXPECT_EQ(DFSDsetdims(rank, dims), SUCCEED);
  EXPECT_EQ(DFSDsetNT(DFNT_FLOAT32), SUCCEED);
  EXPECT_EQ(DFSDstartslab(path.c_str()), SUCCEED);

  // Write top-left 2x2 corner (1-based indexing for slabs in HDF4)
  int32_t start1[2] = {1, 1};
  int32_t stride1[2] = {1, 1};
  int32_t count1[2] = {2, 2};
  std::vector<float> slab1 = {11.0f, 12.0f, 21.0f, 22.0f};
  EXPECT_EQ(DFSDwriteslab(start1, stride1, count1, slab1.data()), SUCCEED);

  // Write bottom-right 2x2 corner (1-based indexing)
  int32_t start2[2] = {3, 3};
  int32_t stride2[2] = {1, 1};
  int32_t count2[2] = {2, 2};
  std::vector<float> slab2 = {33.0f, 34.0f, 43.0f, 44.0f};
  EXPECT_EQ(DFSDwriteslab(start2, stride2, count2, slab2.data()), SUCCEED);

  EXPECT_EQ(DFSDendslab(), SUCCEED);

  // Read slab back (1-based indexing)
  int32_t r_start[2] = {1, 1};
  int32_t r_slab_size[2] = {2, 2};
  int32_t r_stride[2] = {1, 1};
  int32_t r_buf_size[2] = {2, 2};
  std::vector<float> r_slab(4, 0.0f);
  EXPECT_EQ(DFSDreadslab(path.c_str(), r_start, r_slab_size, r_stride,
                         r_slab.data(), r_buf_size),
            SUCCEED);
  EXPECT_FLOAT_EQ(r_slab[0], 11.0f);
  EXPECT_FLOAT_EQ(r_slab[1], 12.0f);
  EXPECT_FLOAT_EQ(r_slab[2], 21.0f);
  EXPECT_FLOAT_EQ(r_slab[3], 22.0f);
}

TEST_F(DfsdTest, Pre32SdgCompatibility) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_pre32.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  constexpr int rank = 2;
  int32_t dims[2] = {2, 2};
  std::vector<float> data = {1, 2, 3, 4};
  EXPECT_EQ(DFSDputdata(path.c_str(), rank, dims, data.data()), SUCCEED);
  const uint16_t ref = DFSDlastref();
  EXPECT_NE(ref, 0);

  int ispre32 = -1;
  EXPECT_EQ(DFSDpre32sdg(const_cast<char*>(path.c_str()), ref, &ispre32),
            SUCCEED);
  // Written by current library, so ispre32 should be 0 (modern NDG)
  EXPECT_EQ(ispre32, 0);

  // Invalid arguments for DFSDpre32sdg
  EXPECT_EQ(DFSDpre32sdg(const_cast<char*>("nonexistent.hdf"), ref, &ispre32),
            FAIL);
  EXPECT_EQ(DFSDpre32sdg(const_cast<char*>(path.c_str()), 9999, &ispre32),
            FAIL);
}

TEST_F(DfsdTest, PrivateHelperRoutines) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_private.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  constexpr int rank = 2;
  int32_t dims[2] = {2, 2};
  std::vector<float> data = {10, 20, 30, 40};
  EXPECT_EQ(DFSDputdata(path.c_str(), rank, dims, data.data()), SUCCEED);

  // Test DFSDIisndg
  int isndg = -1;
  EXPECT_EQ(DFSDIisndg(&isndg), SUCCEED);

  // Test DFSDIgetrrank and DFSDIgetwrank
  int rrank = 0, wrank = 0;
  EXPECT_EQ(DFSDIgetrrank(&rrank), SUCCEED);
  EXPECT_EQ(DFSDIgetwrank(&wrank), SUCCEED);

  // Test DFSDIrefresh
  EXPECT_EQ(DFSDIrefresh(const_cast<char*>(path.c_str())), SUCCEED);
  EXPECT_EQ(DFSDrestart(), SUCCEED);

  // Test DFSDclear
  EXPECT_EQ(DFSDclear(), SUCCEED);
}

TEST_F(DfsdTest, NativeNumberTypesAndFillAndCalibration) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_native.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  constexpr int rank = 2;
  int32_t dims[2] = {2, 2};
  EXPECT_EQ(DFSDsetdims(rank, dims), SUCCEED);
  EXPECT_EQ(DFSDsetNT(DFNT_NFLOAT32), SUCCEED);

  constexpr float64 cal = 3.5;
  constexpr float64 cal_err = 0.1;
  constexpr float64 ioff = 5.0;
  constexpr float64 ioff_err = 0.2;
  EXPECT_EQ(DFSDsetcal(cal, cal_err, ioff, ioff_err, DFNT_NFLOAT32), SUCCEED);

  float fill_val = -123.45f;
  EXPECT_EQ(DFSDsetfillvalue(&fill_val), SUCCEED);

  std::vector<float> data = {1.5f, 2.5f, 3.5f, 4.5f};
  EXPECT_EQ(DFSDputdata(path.c_str(), rank, dims, data.data()), SUCCEED);

  int read_rank = 0;
  int32_t read_dims[2] = {};
  EXPECT_EQ(DFSDgetdims(path.c_str(), &read_rank, read_dims, 2), SUCCEED);

  float64 r_cal = 0, r_cal_err = 0, r_ioff = 0, r_ioff_err = 0;
  int32_t r_cal_nt = 0;
  EXPECT_EQ(DFSDgetcal(&r_cal, &r_cal_err, &r_ioff, &r_ioff_err, &r_cal_nt),
            SUCCEED);
  EXPECT_DOUBLE_EQ(r_cal, cal);
  EXPECT_DOUBLE_EQ(r_cal_err, cal_err);
  EXPECT_DOUBLE_EQ(r_ioff, ioff);
  EXPECT_DOUBLE_EQ(r_ioff_err, ioff_err);

  float r_fill = 0.0f;
  EXPECT_EQ(DFSDgetfillvalue(&r_fill), SUCCEED);
  EXPECT_FLOAT_EQ(r_fill, fill_val);

  std::vector<float> rdata(4, 0.0f);
  EXPECT_EQ(DFSDgetdata(path.c_str(), rank, dims, rdata.data()), SUCCEED);
  EXPECT_EQ(rdata, data);
}

TEST_F(DfsdTest, LargeSlabWithFillValue) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_large_slab.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  // 100 x 100 = 10,000 float elements = 40,000 bytes (> 16,384 fill_bufsize)
  constexpr int rank = 2;
  int32_t dims[2] = {100, 100};
  EXPECT_EQ(DFSDsetdims(rank, dims), SUCCEED);
  EXPECT_EQ(DFSDsetNT(DFNT_FLOAT32), SUCCEED);

  float fill_val = -999.0f;
  EXPECT_EQ(DFSDsetfillvalue(&fill_val), SUCCEED);

  EXPECT_EQ(DFSDstartslab(path.c_str()), SUCCEED);

  // Write a 2x2 slab at (10, 10) (1-based)
  int32_t start[2] = {10, 10};
  int32_t stride[2] = {1, 1};
  int32_t count[2] = {2, 2};
  std::vector<float> slab_data = {100.0f, 200.0f, 300.0f, 400.0f};
  EXPECT_EQ(DFSDwriteslab(start, stride, count, slab_data.data()), SUCCEED);

  EXPECT_EQ(DFSDendslab(), SUCCEED);

  // Read back the written 2x2 slab
  std::vector<float> read_slab(4, 0.0f);
  int32_t r_buf_size[2] = {2, 2};
  EXPECT_EQ(DFSDreadslab(path.c_str(), start, count, stride, read_slab.data(),
                         r_buf_size),
            SUCCEED);
  EXPECT_EQ(read_slab, slab_data);

  // Read an unwritten 2x2 slab at (1, 1) and verify it has the fill value
  int32_t unwritten_start[2] = {1, 1};
  std::vector<float> unwritten_slab(4, 0.0f);
  EXPECT_EQ(DFSDreadslab(path.c_str(), unwritten_start, count, stride,
                         unwritten_slab.data(), r_buf_size),
            SUCCEED);
  for (int i = 0; i < 4; ++i) {
    EXPECT_FLOAT_EQ(unwritten_slab[i], fill_val);
  }
}

TEST_F(DfsdTest, SlabContiguousNativeRank1) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_rank1_slab.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  constexpr int rank = 1;
  int32_t dims[1] = {50};
  EXPECT_EQ(DFSDsetdims(rank, dims), SUCCEED);
  EXPECT_EQ(DFSDsetNT(DFNT_NFLOAT32), SUCCEED);

  EXPECT_EQ(DFSDstartslab(path.c_str()), SUCCEED);

  int32_t start[1] = {10};
  int32_t stride[1] = {1};
  int32_t count[1] = {5};
  std::vector<float> slab_data = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
  EXPECT_EQ(DFSDwriteslab(start, stride, count, slab_data.data()), SUCCEED);

  EXPECT_EQ(DFSDendslab(), SUCCEED);

  std::vector<float> read_slab(5, 0.0f);
  int32_t r_buf_size[1] = {5};
  EXPECT_EQ(DFSDreadslab(path.c_str(), start, count, stride, read_slab.data(),
                         r_buf_size),
            SUCCEED);
  EXPECT_EQ(read_slab, slab_data);
}

TEST_F(DfsdTest, FortranOrderAndAppendMode) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_fortran.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  constexpr int rank = 2;
  int32_t dims[2] = {2, 3};
  std::vector<float> data = {1, 2, 3, 4, 5, 6};

  // Write with accmode = 0 (new file), isfortran = 1
  EXPECT_EQ(DFSDIputdata(path.c_str(), rank, dims, data.data(), 0, 1), SUCCEED);

  // Read back with isfortran = 1
  std::vector<float> rdata(6, 0.0f);
  EXPECT_EQ(DFSDIgetdata(path.c_str(), rank, dims, rdata.data(), 1), SUCCEED);
  EXPECT_EQ(rdata, data);

  // Append a second dataset with accmode = 1 (append), isfortran = 0
  std::vector<float> data2 = {10, 20, 30, 40, 50, 60};
  EXPECT_EQ(DFSDIputdata(path.c_str(), rank, dims, data2.data(), 1, 0),
            SUCCEED);

  EXPECT_EQ(DFSDndatasets(const_cast<char*>(path.c_str())), 2);
}

TEST_F(DfsdTest, SliceAndSlabErrorHandling) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_err_slab.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  constexpr int rank = 2;
  int32_t dims[2] = {4, 4};
  float buf[16] = {};

  // Calling slice/slab ops before start must fail
  EXPECT_EQ(DFSDputslice(dims, buf, dims), FAIL);
  EXPECT_EQ(DFSDendslice(), FAIL);
  int32_t start[2] = {1, 1};
  int32_t stride[2] = {1, 1};
  int32_t count[2] = {2, 2};
  EXPECT_EQ(DFSDendslab(), FAIL);

  // Out of bounds write
  EXPECT_EQ(DFSDsetdims(rank, dims), SUCCEED);
  EXPECT_EQ(DFSDstartslab(path.c_str()), SUCCEED);

  // Cannot change dims while active
  EXPECT_EQ(DFSDsetdims(rank, dims), FAIL);

  int32_t bad_start[2] = {0, 1};  // 1-based indexing required!
  int32_t oob_start[2] = {4, 4};
  EXPECT_EQ(DFSDwriteslab(start, stride, count, nullptr), FAIL);
  EXPECT_EQ(DFSDwriteslab(start, stride, count, buf), SUCCEED);
  EXPECT_EQ(DFSDendslab(), SUCCEED);

  // Out of bounds read
  EXPECT_EQ(DFSDreadslab(path.c_str(), bad_start, count, stride, buf, count),
            FAIL);
  EXPECT_EQ(DFSDreadslab(path.c_str(), oob_start, count, stride, buf, count),
            FAIL);
}

TEST_F(DfsdTest, ScaleResetAndPartialScales) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_partial_scale.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  constexpr int rank = 2;
  int32_t dims[2] = {3, 4};
  EXPECT_EQ(DFSDsetdims(rank, dims), SUCCEED);
  EXPECT_EQ(DFSDsetNT(DFNT_NFLOAT32), SUCCEED);

  // Set scale for dim 1
  float scale1[3] = {1.0f, 2.0f, 3.0f};
  EXPECT_EQ(DFSDsetdimscale(1, dims[0], scale1), SUCCEED);

  // Set scale for dim 2, then clear it by passing nullptr
  float scale2[4] = {10.0f, 20.0f, 30.0f, 40.0f};
  EXPECT_EQ(DFSDsetdimscale(2, dims[1], scale2), SUCCEED);
  EXPECT_EQ(DFSDsetdimscale(2, dims[1], nullptr), SUCCEED);

  // Set range with native type (no conversion)
  float max_val = 100.0f, min_val = 0.0f;
  EXPECT_EQ(DFSDsetrange(&max_val, &min_val), SUCCEED);

  std::vector<float> data(12, 1.0f);
  EXPECT_EQ(DFSDputdata(path.c_str(), rank, dims, data.data()), SUCCEED);

  int read_rank = 0;
  int32_t read_dims[2] = {};
  EXPECT_EQ(DFSDgetdims(path.c_str(), &read_rank, read_dims, 2), SUCCEED);

  float r_scale1[3] = {};
  EXPECT_EQ(DFSDgetdimscale(1, dims[0], r_scale1), SUCCEED);
  EXPECT_FLOAT_EQ(r_scale1[0], scale1[0]);

  float r_scale2[4] = {};
  // Dim 2 scale was not set, so getdimscale returns FAIL
  EXPECT_EQ(DFSDgetdimscale(2, dims[1], r_scale2), FAIL);

  float r_max = 0, r_min = 0;
  EXPECT_EQ(DFSDgetrange(&r_max, &r_min), SUCCEED);
  EXPECT_FLOAT_EQ(r_max, max_val);
  EXPECT_FLOAT_EQ(r_min, min_val);
}

TEST_F(DfsdTest, NonContiguousSliceAndFortranSlice) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_slice_noncontig.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  constexpr int rank = 2;
  int32_t dims[2] = {4, 4};
  EXPECT_EQ(DFSDsetdims(rank, dims), SUCCEED);
  EXPECT_EQ(DFSDsetNT(DFNT_NFLOAT32), SUCCEED);

  EXPECT_EQ(DFSDstartslice(path.c_str()), SUCCEED);

  // Non-contiguous slice: 2 rows of 4 columns stored in a 2x6 buffer
  int32_t windims[2] = {2, 4};
  int32_t stride_dims[2] = {2, 6};
  std::vector<float> sdata(12, 1.0f);
  // Write first 2 rows
  EXPECT_EQ(DFSDIputslice(windims, sdata.data(), stride_dims, 0), SUCCEED);
  sdata.assign(12, 2.0f);
  // Write next 2 rows (completing 4 rows)
  EXPECT_EQ(DFSDIputslice(windims, sdata.data(), stride_dims, 0), SUCCEED);
  EXPECT_EQ(DFSDendslice(), SUCCEED);

  // Read back slice using isfortran = 1 (transposed = 1, scatterbuf hit)
  int32_t winst[2] = {1, 1};
  int32_t rwindims[2] = {2, 2};
  int32_t rdims[2] = {2, 4};
  std::vector<float> rslice(8, 0.0f);
  // isfortran = 0 to avoid crash if transposed pure SDG
  EXPECT_EQ(
      DFSDIgetslice(path.c_str(), winst, rwindims, rslice.data(), rdims, 0),
      SUCCEED);
}

TEST_F(DfsdTest, ReadAndModifySlabTable) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_slab_table.hdf";
  const std::string path2 = dir + "/dfsd_slab_table2.hdf";
  (void)remove(path.c_str());
  (void)remove(path2.c_str());
  const absl::Cleanup cleanup = [&] {
    (void)remove(path.c_str());
    (void)remove(path2.c_str());
  };

  constexpr int rank = 2;
  int32_t dims[2] = {4, 4};
  std::vector<float> data(16, 42.0f);

  // 1. Create file 1
  EXPECT_EQ(DFSDputdata(path.c_str(), rank, dims, data.data()), SUCCEED);

  // 2. Read dimensions from file 1 to populate nsdghdr->nsdg_t table
  int rrank = 0;
  int32_t rdims[2] = {};
  EXPECT_EQ(DFSDgetdims(path.c_str(), &rrank, rdims, 2), SUCCEED);

  // 3. Start and end slab on file 1: DFSDendslab frees the existing nsdghdr
  // table
  EXPECT_EQ(DFSDsetdims(rank, dims), SUCCEED);
  EXPECT_EQ(DFSDstartslab(path.c_str()), SUCCEED);
  int32_t start[2] = {1, 1};
  int32_t stride[2] = {1, 1};
  int32_t count[2] = {2, 2};
  std::vector<float> slab_data = {1, 2, 3, 4};
  EXPECT_EQ(DFSDwriteslab(start, stride, count, slab_data.data()), SUCCEED);
  EXPECT_EQ(DFSDendslab(), SUCCEED);

  // 4. Read from file 1 again to repopulate nsdghdr->nsdg_t
  EXPECT_EQ(DFSDrestart(), SUCCEED);
  EXPECT_EQ(DFSDgetdims(path.c_str(), &rrank, rdims, 2), SUCCEED);

  // 5. Open file 2 for reading: DFSDIopen frees existing nsdghdr table from
  // file 1
  EXPECT_EQ(DFSDputdata(path2.c_str(), rank, dims, data.data()), SUCCEED);
  EXPECT_EQ(DFSDgetdims(path2.c_str(), &rrank, rdims, 2), SUCCEED);
}

TEST_F(DfsdTest, NonNativeInt32AttributesAndScales) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_nonnative_int32.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  constexpr int rank = 2;
  int32_t dims[2] = {3, 3};
  std::vector<int32_t> data = {1, 2, 3, 4, 5, 6, 7, 8, 9};

  EXPECT_EQ(DFSDsetdims(rank, dims), SUCCEED);
  EXPECT_EQ(DFSDsetNT(DFNT_INT32), SUCCEED);

  int32_t scale1[3] = {10, 20, 30};
  int32_t scale2[3] = {100, 200, 300};
  EXPECT_EQ(DFSDsetdimscale(1, 3, scale1), SUCCEED);
  EXPECT_EQ(DFSDsetdimscale(2, 3, scale2), SUCCEED);

  int32_t max_val = 9;
  int32_t min_val = 1;
  EXPECT_EQ(DFSDsetrange(&max_val, &min_val), SUCCEED);

  int32_t fill_val = -1;
  EXPECT_EQ(DFSDsetfillvalue(&fill_val), SUCCEED);

  EXPECT_EQ(DFSDsetcal(2.0, 0.1, 5.0, 0.2, DFNT_INT32), SUCCEED);
  EXPECT_EQ(DFSDsetdatastrs("data_lbl", "data_unit", "data_fmt", "coordsys"),
            SUCCEED);
  EXPECT_EQ(DFSDsetdimstrs(1, "x_lbl", "x_unit", "x_fmt"), SUCCEED);
  EXPECT_EQ(DFSDsetdimstrs(2, "y_lbl", "y_unit", "y_fmt"), SUCCEED);

  EXPECT_EQ(DFSDputdata(path.c_str(), rank, dims, data.data()), SUCCEED);

  int rrank = 0;
  int32_t rdims[2] = {};
  EXPECT_EQ(DFSDgetdims(path.c_str(), &rrank, rdims, 2), SUCCEED);
  EXPECT_EQ(rrank, 2);
  EXPECT_EQ(rdims[0], 3);
  EXPECT_EQ(rdims[1], 3);

  int32_t nt = 0;
  EXPECT_EQ(DFSDgetNT(&nt), SUCCEED);
  EXPECT_EQ(nt, DFNT_INT32);

  int32_t rscale1[3] = {};
  int32_t rscale2[3] = {};
  EXPECT_EQ(DFSDgetdimscale(1, 3, rscale1), SUCCEED);
  EXPECT_EQ(DFSDgetdimscale(2, 3, rscale2), SUCCEED);
  EXPECT_EQ(rscale1[0], 10);
  EXPECT_EQ(rscale2[2], 300);

  int32_t rmax = 0, rmin = 0;
  EXPECT_EQ(DFSDgetrange(&rmax, &rmin), SUCCEED);
  EXPECT_EQ(rmax, 9);
  EXPECT_EQ(rmin, 1);

  int32_t rfill = 0;
  EXPECT_EQ(DFSDgetfillvalue(&rfill), SUCCEED);
  EXPECT_EQ(rfill, -1);

  double cal = 0, cal_err = 0, ioff = 0, ioff_err = 0;
  int32_t cal_nt = 0;
  EXPECT_EQ(DFSDgetcal(&cal, &cal_err, &ioff, &ioff_err, &cal_nt), SUCCEED);
  EXPECT_DOUBLE_EQ(cal, 2.0);
  EXPECT_EQ(cal_nt, DFNT_INT32);

  std::vector<int32_t> rdata(9, 0);
  EXPECT_EQ(DFSDgetdata(path.c_str(), rank, rdims, rdata.data()), SUCCEED);
  EXPECT_EQ(rdata, data);

  int32_t start[2] = {1, 1};
  int32_t stride[2] = {1, 1};
  int32_t count[2] = {2, 2};
  int32_t out_dims[2] = {2, 2};
  int32_t slab_out[4] = {};
  EXPECT_EQ(
      DFSDreadslab(path.c_str(), start, count, stride, slab_out, out_dims),
      SUCCEED);
  EXPECT_EQ(slab_out[0], 1);
  EXPECT_EQ(slab_out[1], 2);
  EXPECT_EQ(slab_out[2], 4);
  EXPECT_EQ(slab_out[3], 5);
}

TEST_F(DfsdTest, FortranOrderPutAndGet) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_fortran.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  constexpr int rank = 2;
  int32_t dims[2] = {3, 2};
  std::vector<float> data = {1, 2, 3, 4, 5, 6};

  EXPECT_EQ(DFSDsetdims(rank, dims), SUCCEED);
  EXPECT_EQ(DFSDsetNT(DFNT_NFLOAT32), SUCCEED);

  EXPECT_EQ(
      DFSDIputdata(path.c_str(), rank, dims, data.data(), 0, /*isfortran=*/1),
      SUCCEED);

  int rrank = 0;
  int32_t rdims[2] = {};
  EXPECT_EQ(DFSDgetdims(path.c_str(), &rrank, rdims, 2), SUCCEED);

  std::vector<float> rdata(6, 0.0f);
  EXPECT_EQ(
      DFSDIgetdata(path.c_str(), rank, rdims, rdata.data(), /*isfortran=*/1),
      SUCCEED);

  int32_t start[2] = {1, 1};
  int32_t stride[2] = {1, 1};
  int32_t count[2] = {2, 2};
  int32_t f_out_dims[2] = {2, 2};
  float slab_out[4] = {};
  EXPECT_EQ(
      DFSDreadslab(path.c_str(), start, count, stride, slab_out, f_out_dims),
      SUCCEED);
}

TEST_F(DfsdTest, PrivateHelpersDirectCoverage) {
  DFSsdg dummy_sdg = {};
  EXPECT_EQ(DFSDIclear(&dummy_sdg), SUCCEED);

  dummy_sdg.numbertype = DFNT_FLOAT32;
  EXPECT_EQ(DFSDIclearNT(&dummy_sdg), SUCCEED);

  int32_t pdims[2] = {2, 2};
  EXPECT_EQ(DFSDsetdims(2, pdims), SUCCEED);
  EXPECT_EQ(DFSDIsetdatastrs("lbl", "u", "f", "c"), SUCCEED);
  EXPECT_EQ(DFSDIsetdimstrs(1, "dlbl", "du", "df"), SUCCEED);
  EXPECT_EQ(DFSDIsetdimstrs(0, "dlbl", "du", "df"), FAIL);

  EXPECT_EQ(DFSDIsdginfo(FAIL), FAIL);
  EXPECT_EQ(DFSDIendslice(0), FAIL);
  EXPECT_EQ(DFSDIendslice(1), FAIL);
}

TEST_F(DfsdTest, LowLevelSpecialTagsAndCorruptNdg) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_lowlevel_tags.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  int32_t dims[2] = {2, 2};
  std::vector<float> data = {1, 2, 3, 4};
  EXPECT_EQ(DFSDputdata(path.c_str(), 2, dims, data.data()), SUCCEED);
  const uint16_t last_ref = DFSDlastref();

  int32_t fid = Hopen(path.c_str(), DFACC_WRITE, 0);
  ASSERT_NE(fid, FAIL);

  const uint16_t cal_ref = Hnewref(fid);
  float cal16[4] = {1.5f, 0.05f, 10.0f, 0.5f};
  EXPECT_NE(Hputelement(fid, DFTAG_CAL, cal_ref,
                        reinterpret_cast<const uint8_t*>(cal16), sizeof(cal16)),
            FAIL);

  const uint16_t sdt_ref = Hnewref(fid);
  uint8_t dummy_byte = 1;
  EXPECT_NE(Hputelement(fid, DFTAG_SDT, sdt_ref, &dummy_byte, 1), FAIL);

  const uint16_t bad_ref = Hnewref(fid);
  uint16_t bad_tag = 785;  // DFTAG_EREQ <= 785 <= DFTAG_BREQ
  EXPECT_NE(Hputelement(fid, bad_tag, bad_ref, &dummy_byte, 1), FAIL);

  int32_t group = DFdisetup(6);
  ASSERT_NE(group, FAIL);
  EXPECT_GE(DFdiput(group, DFTAG_SD, last_ref), 0);
  EXPECT_GE(DFdiput(group, DFTAG_CAL, cal_ref), 0);
  EXPECT_GE(DFdiput(group, DFTAG_SDT, sdt_ref), 0);
  EXPECT_GE(DFdiput(group, bad_tag, bad_ref), 0);

  const uint16_t ndg_ref = Hnewref(fid);
  EXPECT_GT(DFdiwrite(fid, group, DFTAG_NDG, ndg_ref), 0);
  EXPECT_EQ(Hclose(fid), SUCCEED);

  EXPECT_EQ(DFSDrestart(), SUCCEED);
  int rrank = 0;
  int32_t rdims[2] = {};
  EXPECT_EQ(DFSDgetdims(path.c_str(), &rrank, rdims, 2), SUCCEED);
  EXPECT_EQ(DFSDgetdims(path.c_str(), &rrank, rdims, 2), FAIL);
}

TEST_F(DfsdTest, LowLevel16ByteCalibrationSuccess) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_cal16.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  int32_t dims[2] = {2, 2};
  std::vector<float> data = {1, 2, 3, 4};
  EXPECT_EQ(DFSDsetNT(DFNT_NFLOAT32), SUCCEED);
  EXPECT_EQ(DFSDputdata(path.c_str(), 2, dims, data.data()), SUCCEED);
  const uint16_t data_ref = DFSDlastref();

  int32_t fid = Hopen(path.c_str(), DFACC_WRITE, 0);
  ASSERT_NE(fid, FAIL);

  const uint16_t cal_ref = Hnewref(fid);
  // Note: in 16-byte calibration, order is: ioff, ioff_err, cal, cal_err
  float cal16[4] = {15.0f, 0.2f, 2.5f, 0.1f};
  EXPECT_NE(Hputelement(fid, DFTAG_CAL, cal_ref,
                        reinterpret_cast<const uint8_t*>(cal16), sizeof(cal16)),
            FAIL);

  const uint16_t sdt_ref = Hnewref(fid);
  uint8_t dummy_byte = 1;
  EXPECT_NE(Hputelement(fid, DFTAG_SDT, sdt_ref, &dummy_byte, 1), FAIL);

  uint16_t sdd_ref = DFfindnextref(fid, DFTAG_SDD, DFREF_WILDCARD);
  ASSERT_NE(sdd_ref, 0);

  int32_t group2 = DFdisetup(5);
  ASSERT_NE(group2, FAIL);
  EXPECT_GE(DFdiput(group2, DFTAG_SD, data_ref), 0);
  EXPECT_GE(DFdiput(group2, DFTAG_SDD, sdd_ref), 0);
  EXPECT_GE(DFdiput(group2, DFTAG_CAL, cal_ref), 0);
  EXPECT_GE(DFdiput(group2, DFTAG_SDT, sdt_ref), 0);

  const uint16_t ndg_ref = Hnewref(fid);
  EXPECT_GT(DFdiwrite(fid, group2, DFTAG_NDG, ndg_ref), 0);
  EXPECT_EQ(Hclose(fid), SUCCEED);

  EXPECT_EQ(DFSDrestart(), SUCCEED);
  int rrank = 0;
  int32_t rdims[2] = {};
  EXPECT_EQ(DFSDgetdims(path.c_str(), &rrank, rdims, 2), SUCCEED);
  EXPECT_EQ(DFSDgetdims(path.c_str(), &rrank, rdims, 2), SUCCEED);

  double cal = 0, cal_err = 0, ioff = 0, ioff_err = 0;
  int32_t cal_nt = 0;
  EXPECT_EQ(DFSDgetcal(&cal, &cal_err, &ioff, &ioff_err, &cal_nt), SUCCEED);
  EXPECT_FLOAT_EQ(static_cast<float>(cal), 2.5f);
  EXPECT_FLOAT_EQ(static_cast<float>(ioff), 15.0f);
  EXPECT_EQ(cal_nt, DFNT_INT16);
}

TEST_F(DfsdTest, PureSdgDatasetAndTransposedSlice) {
  const std::string dir = ::testing::TempDir();
  std::string path = dir + "/dfsd_pure_sdg.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);

  // 1. Write DFTAG_NT (float32 IEEE)
  uint8_t nt_desc[4] = {DFNT_VERSION, DFNT_FLOAT32, 32, DFNTF_HDFDEFAULT};
  uint16_t nt_ref = Hnewref(fid);
  EXPECT_NE(Hputelement(fid, DFTAG_NT, nt_ref, nt_desc, sizeof(nt_desc)), FAIL);

  // 2. Write DFTAG_SDD (rank=2, dims={2, 3}, data NT, scale NTs)
  uint8_t sdd_buf[64] = {};
  uint8_t* p = sdd_buf;
  INT16ENCODE(p, 2);  // rank = 2
  INT32ENCODE(p, 2);  // dim[0] = 2
  INT32ENCODE(p, 3);  // dim[1] = 3
  UINT16ENCODE(p, DFTAG_NT);
  UINT16ENCODE(p, nt_ref);
  UINT16ENCODE(p, DFTAG_NT);
  UINT16ENCODE(p, nt_ref);
  UINT16ENCODE(p, DFTAG_NT);
  UINT16ENCODE(p, nt_ref);
  int32_t sdd_len = p - sdd_buf;
  uint16_t sdd_ref = Hnewref(fid);
  EXPECT_NE(Hputelement(fid, DFTAG_SDD, sdd_ref, sdd_buf, sdd_len), FAIL);

  // 3. Write DFTAG_SD (6 float32s in IEEE big-endian)
  float data_vals[6] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
  uint8_t sd_buf[24] = {};
  DFKconvert(data_vals, sd_buf, DFNT_FLOAT32, 6, DFACC_WRITE, 0, 0);
  uint16_t sd_ref = Hnewref(fid);
  EXPECT_NE(Hputelement(fid, DFTAG_SD, sd_ref, sd_buf, sizeof(sd_buf)), FAIL);

  // 4. Write DFTAG_CAL (16 bytes legacy float32)
  float cal16[4] = {10.0f, 0.1f, 2.0f, 0.05f};
  uint8_t cal_buf[16] = {};
  DFKconvert(cal16, cal_buf, DFNT_FLOAT32, 4, DFACC_WRITE, 0, 0);
  uint16_t cal_ref = Hnewref(fid);
  EXPECT_NE(Hputelement(fid, DFTAG_CAL, cal_ref, cal_buf, sizeof(cal_buf)),
            FAIL);

  // 5. Write DFTAG_SDL, DFTAG_SDU, DFTAG_SDF, DFTAG_SDC
  // In HDF4, SDL/SDU/SDF contains: data string + rank dimension strings
  const char lbl_buf[] = "sdg_label\0dim0_lbl\0dim1_lbl";
  const char unit_buf[] = "sdg_unit\0dim0_unit\0dim1_unit";
  const char fmt_buf[] = "sdg_fmt\0dim0_fmt\0dim1_fmt";
  const char coordsys[] = "sdg_coordsys";
  uint16_t lbl_ref = Hnewref(fid);
  uint16_t unit_ref = Hnewref(fid);
  uint16_t fmt_ref = Hnewref(fid);
  uint16_t coord_ref = Hnewref(fid);
  EXPECT_NE(
      Hputelement(fid, DFTAG_SDL, lbl_ref,
                  reinterpret_cast<const uint8_t*>(lbl_buf), sizeof(lbl_buf)),
      FAIL);
  EXPECT_NE(
      Hputelement(fid, DFTAG_SDU, unit_ref,
                  reinterpret_cast<const uint8_t*>(unit_buf), sizeof(unit_buf)),
      FAIL);
  EXPECT_NE(
      Hputelement(fid, DFTAG_SDF, fmt_ref,
                  reinterpret_cast<const uint8_t*>(fmt_buf), sizeof(fmt_buf)),
      FAIL);
  EXPECT_NE(Hputelement(fid, DFTAG_SDC, coord_ref,
                        reinterpret_cast<const uint8_t*>(coordsys),
                        strlen(coordsys) + 1),
            FAIL);

  // 6. Write DFTAG_SDG group
  int32_t group = DFdisetup(7);
  ASSERT_NE(group, FAIL);
  EXPECT_GE(DFdiput(group, DFTAG_SDD, sdd_ref), 0);
  EXPECT_GE(DFdiput(group, DFTAG_SD, sd_ref), 0);
  EXPECT_GE(DFdiput(group, DFTAG_CAL, cal_ref), 0);
  EXPECT_GE(DFdiput(group, DFTAG_SDL, lbl_ref), 0);
  EXPECT_GE(DFdiput(group, DFTAG_SDU, unit_ref), 0);
  EXPECT_GE(DFdiput(group, DFTAG_SDF, fmt_ref), 0);
  EXPECT_GE(DFdiput(group, DFTAG_SDC, coord_ref), 0);
  uint16_t sdg_ref = Hnewref(fid);
  EXPECT_GT(DFdiwrite(fid, group, DFTAG_SDG, sdg_ref), 0);
  EXPECT_EQ(Hclose(fid), SUCCEED);

  // Verify DFSDpre32sdg
  int is_pre32 = 0;
  EXPECT_EQ(DFSDpre32sdg(path.data(), sdg_ref, &is_pre32), SUCCEED);
  EXPECT_EQ(is_pre32, 1);

  // Read via DFSD
  EXPECT_EQ(DFSDrestart(), SUCCEED);
  EXPECT_EQ(DFSDreadref(path.data(), sdg_ref), SUCCEED);
  int rrank = 0;
  int32_t rdims[2] = {};
  EXPECT_EQ(DFSDgetdims(path.c_str(), &rrank, rdims, 2), SUCCEED);
  EXPECT_EQ(rrank, 2);
  EXPECT_EQ(rdims[0], 2);
  EXPECT_EQ(rdims[1], 3);

  char rlbl[32] = {}, runit[32] = {}, rfmt[32] = {}, rcoord[32] = {};
  EXPECT_EQ(DFSDgetdatastrs(rlbl, runit, rfmt, rcoord), SUCCEED);
  EXPECT_STREQ(rlbl, "sdg_label");
  EXPECT_STREQ(runit, "sdg_unit");
  EXPECT_STREQ(rfmt, "sdg_fmt");
  EXPECT_STREQ(rcoord, coordsys);

  char d0_lbl[32] = {}, d0_unit[32] = {}, d0_fmt[32] = {};
  EXPECT_EQ(DFSDgetdimstrs(1, d0_lbl, d0_unit, d0_fmt), SUCCEED);
  EXPECT_STREQ(d0_lbl, "dim0_lbl");
  EXPECT_STREQ(d0_unit, "dim0_unit");
  EXPECT_STREQ(d0_fmt, "dim0_fmt");

  double cal2 = 0, cal_err2 = 0, ioff2 = 0, ioff_err2 = 0;
  int32_t cal_nt2 = 0;
  EXPECT_EQ(DFSDgetcal(&cal2, &cal_err2, &ioff2, &ioff_err2, &cal_nt2),
            SUCCEED);
  EXPECT_FLOAT_EQ(static_cast<float>(cal2), 2.0f);
  EXPECT_FLOAT_EQ(static_cast<float>(ioff2), 10.0f);

  // Read transposed slice with isfortran = 1
  int32_t winst[2] = {1, 1};
  int32_t windims[2] = {2, 3};
  float out_data[6] = {};
  int32_t dims_buf[2] = {2, 3};
  EXPECT_EQ(DFSDIgetslice(path.c_str(), winst, windims, out_data, dims_buf, 1),
            SUCCEED);

  // Also read non-transposed slice with isfortran = 0
  float out_data2[6] = {};
  EXPECT_EQ(DFSDIgetslice(path.c_str(), winst, windims, out_data2, dims_buf, 0),
            SUCCEED);

  // Test DFSDwriteref with SDG
  EXPECT_EQ(DFSDwriteref(path.c_str(), sdg_ref), SUCCEED);

  // Also test DFSDndatasets on file with pure SDG
  EXPECT_EQ(DFSDndatasets(path.data()), 1);
}

TEST_F(DfsdTest, DatasetsEmptyAndMixedSdgNdg) {
  const std::string dir = ::testing::TempDir();
  std::string empty_path = dir + "/dfsd_empty.hdf";
  (void)remove(empty_path.c_str());
  const absl::Cleanup cleanup1 = [&empty_path] {
    (void)remove(empty_path.c_str());
  };

  int32_t fid = Hopen(empty_path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  EXPECT_EQ(Hclose(fid), SUCCEED);

  EXPECT_EQ(DFSDrestart(), SUCCEED);
  EXPECT_EQ(DFSDndatasets(empty_path.data()), 0);
}

TEST_F(DfsdTest, MultiDimSlabWithFillValueAndConversion) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_multidim_slab.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  // First create an existing file and call DFSDndatasets so nsdghdr is
  // populated
  std::string seed_path = dir + "/dfsd_seed.hdf";
  (void)remove(seed_path.c_str());
  const absl::Cleanup cleanup_seed = [&seed_path] {
    (void)remove(seed_path.c_str());
  };
  {
    int32_t seed_dims[2] = {2, 2};
    std::vector<float> seed_data = {1, 2, 3, 4};
    EXPECT_EQ(DFSDputdata(seed_path.c_str(), 2, seed_dims, seed_data.data()),
              SUCCEED);
    EXPECT_EQ(DFSDndatasets(seed_path.data()), 1);
  }

  constexpr int rank = 3;
  int32_t dims[3] = {
      5, 5, 50};  // 5 * 5 * 50 = 1250 elements = 5000 bytes > 4096 fill_bufsize
  EXPECT_EQ(DFSDsetdims(rank, dims), SUCCEED);
  EXPECT_EQ(DFSDsetNT(DFNT_INT32), SUCCEED);

  int32_t fill_val = 0x12345678;
  EXPECT_EQ(DFSDsetfillvalue(&fill_val), SUCCEED);

  EXPECT_EQ(DFSDstartslab(path.c_str()), SUCCEED);

  // Slab write error branches
  int32_t bad_start[3] = {0, 1, 1};
  int32_t stride[3] = {1, 1, 1};
  int32_t count1[3] = {2, 2, 10};
  std::vector<int32_t> data1(2 * 2 * 10, 42);
  EXPECT_EQ(DFSDwriteslab(bad_start, stride, count1, data1.data()), FAIL);
  int32_t bad_count[3] = {2, 0, 10};
  int32_t start1[3] = {1, 1, 1};
  EXPECT_EQ(DFSDwriteslab(start1, stride, bad_count, data1.data()), FAIL);
  int32_t overflow_start[3] = {5, 5, 45};
  int32_t overflow_count[3] = {1, 1, 10};
  EXPECT_EQ(DFSDwriteslab(overflow_start, stride, overflow_count, data1.data()),
            FAIL);

  // Sub-slab with no dimension collapse (count[2] = 10 < dims[2] = 50), with
  // conversion
  EXPECT_EQ(DFSDwriteslab(start1, stride, count1, data1.data()), SUCCEED);

  // Sub-slab with dimension collapse down to rank 1 (count[2]=50, count[1]=5)
  int32_t start2[3] = {3, 1, 1};
  int32_t count2[3] = {2, 5, 50};
  std::vector<int32_t> data2(2 * 5 * 50, 99);
  EXPECT_EQ(DFSDwriteslab(start2, stride, count2, data2.data()), SUCCEED);

  // DFSDendslab cleans up nsdghdr
  EXPECT_EQ(DFSDendslab(), SUCCEED);
}

TEST_F(DfsdTest, SliceWrappingAndEndSliceError) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_slice_wrap.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  // First populate nsdghdr by calling DFSDndatasets on a seed file
  std::string seed_path = dir + "/dfsd_seed2.hdf";
  (void)remove(seed_path.c_str());
  const absl::Cleanup cleanup_seed = [&seed_path] {
    (void)remove(seed_path.c_str());
  };
  {
    int32_t seed_dims[2] = {2, 2};
    std::vector<float> seed_data = {1, 2, 3, 4};
    EXPECT_EQ(DFSDputdata(seed_path.c_str(), 2, seed_dims, seed_data.data()),
              SUCCEED);
    EXPECT_EQ(DFSDndatasets(seed_path.data()), 1);
  }

  constexpr int rank = 2;
  int32_t dims[2] = {3, 3};
  EXPECT_EQ(DFSDsetdims(rank, dims), SUCCEED);
  EXPECT_EQ(DFSDsetNT(DFNT_NFLOAT32), SUCCEED);

  EXPECT_EQ(DFSDstartslice(path.c_str()), SUCCEED);

  // Partial write: size {1, 3}
  int32_t windims1[2] = {1, 3};
  float data1[3] = {1, 2, 3};
  EXPECT_EQ(DFSDputslice(windims1, data1, dims), SUCCEED);

  // Calling DFSDendslice prematurely fails
  EXPECT_EQ(DFSDendslice(), FAIL);

  // Write remaining slice of size {2, 3} to complete the dataset cleanly
  int32_t windims2[2] = {2, 3};
  float data2[6] = {4, 5, 6, 7, 8, 9};
  EXPECT_EQ(DFSDputslice(windims2, data2, dims), SUCCEED);

  // End slice cleans up nsdghdr, Sfile_id, and Sddims
  EXPECT_EQ(DFSDendslice(), SUCCEED);
}

TEST_F(DfsdTest, SetDimScaleWithoutNTAndDFSDreadrefErrors) {
  const std::string dir = ::testing::TempDir();
  std::string path = dir + "/dfsd_dimscale_nont.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  EXPECT_EQ(DFSDrestart(), SUCCEED);
  constexpr int rank = 2;
  int32_t dims[2] = {3, 3};
  EXPECT_EQ(DFSDsetdims(rank, dims), SUCCEED);
  // Do NOT call DFSDsetNT -> numbertype is DFNT_NONE, DFSDsetdimscale defaults
  // to float32
  float scale1[3] = {1.0f, 2.0f, 3.0f};
  EXPECT_EQ(DFSDsetdimscale(1, 3, scale1), SUCCEED);

  std::vector<float> data(9, 1.0f);
  EXPECT_EQ(DFSDputdata(path.c_str(), rank, dims, data.data()), SUCCEED);

  // DFSDreadref on non-existent ref
  EXPECT_EQ(DFSDreadref(path.data(), 9999), FAIL);

  // DFSDgetcal when dataset has no calibration -> FAIL (DFE_NOVALS)
  int rrank = 0;
  int32_t rdims[2] = {};
  EXPECT_EQ(DFSDgetdims(path.c_str(), &rrank, rdims, 2), SUCCEED);
  double cal = 0, cal_err = 0, ioff = 0, ioff_err = 0;
  int32_t cal_nt = 0;
  EXPECT_EQ(DFSDgetcal(&cal, &cal_err, &ioff, &ioff_err, &cal_nt), FAIL);

  // DFSDgetrange when dataset has no range -> FAIL (DFE_NOVALS)
  float rmax = 0, rmin = 0;
  EXPECT_EQ(DFSDgetrange(&rmax, &rmin), FAIL);

  // Call getters after DFSDreadref when Newdata < 0 -> FAIL (DFE_BADCALL)
  const uint16_t valid_ref = DFSDlastref();
  EXPECT_EQ(DFSDreadref(path.data(), valid_ref), SUCCEED);
  EXPECT_EQ(DFSDgetcal(&cal, &cal_err, &ioff, &ioff_err, &cal_nt), FAIL);
  EXPECT_EQ(DFSDgetrange(&rmax, &rmin), FAIL);
  int32_t fill = 0;
  EXPECT_EQ(DFSDgetfillvalue(&fill), FAIL);
  char lbl[16] = {}, unit[16] = {}, fmt[16] = {}, coord[16] = {};
  EXPECT_EQ(DFSDgetdatastrs(lbl, unit, fmt, coord), FAIL);
  EXPECT_EQ(DFSDgetdimstrs(1, lbl, unit, fmt), FAIL);
  float rscale[3] = {};
  EXPECT_EQ(DFSDgetdimscale(1, 3, rscale), FAIL);
}

TEST_F(DfsdTest, SetnsdgOrderAndDuplicateChecks) {
  const std::string dir = ::testing::TempDir();
  std::string path = dir + "/dfsd_nsdg_order.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);

  // Write two NDG groups: first with ref 50, second with ref 20 (out of order!)
  int32_t g1 = DFdisetup(1);
  EXPECT_NE(g1, FAIL);
  EXPECT_GE(DFdiput(g1, DFTAG_SD, 1), 0);
  EXPECT_GT(DFdiwrite(fid, g1, DFTAG_NDG, 50), 0);

  int32_t g2 = DFdisetup(1);
  EXPECT_NE(g2, FAIL);
  EXPECT_GE(DFdiput(g2, DFTAG_SD, 1), 0);
  EXPECT_GT(DFdiwrite(fid, g2, DFTAG_NDG, 20), 0);

  // Write two SDG groups: first with ref 40, second with ref 10 (out of order!)
  int32_t g3 = DFdisetup(1);
  EXPECT_NE(g3, FAIL);
  EXPECT_GE(DFdiput(g3, DFTAG_SD, 1), 0);
  EXPECT_GT(DFdiwrite(fid, g3, DFTAG_SDG, 40), 0);

  int32_t g4 = DFdisetup(1);
  EXPECT_NE(g4, FAIL);
  EXPECT_GE(DFdiput(g4, DFTAG_SD, 1), 0);
  EXPECT_GT(DFdiwrite(fid, g4, DFTAG_SDG, 10), 0);

  EXPECT_EQ(Hclose(fid), SUCCEED);

  EXPECT_EQ(DFSDrestart(), SUCCEED);
  // DFSDndatasets will sort NDGs (ref 20 before ref 50) and SDGs (ref 10 before
  // ref 40)!
  EXPECT_EQ(DFSDndatasets(path.data()), 4);
}

TEST_F(DfsdTest, GetSliceValidationErrors) {
  const std::string dir = ::testing::TempDir();
  std::string path = dir + "/dfsd_slice_errs.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  int32_t dims[2] = {4, 4};
  std::vector<float> data(16, 1.0f);
  EXPECT_EQ(DFSDputdata(path.c_str(), 2, dims, data.data()), SUCCEED);

  float out[16] = {};
  int32_t out_dims[2] = {4, 4};

  // winst < 1
  int32_t bad_winst[2] = {0, 1};
  int32_t windims[2] = {2, 2};
  EXPECT_EQ(DFSDIgetslice(path.c_str(), bad_winst, windims, out, out_dims, 0),
            FAIL);

  // windims < 1
  int32_t bad_windims[2] = {2, 0};
  int32_t winst[2] = {1, 1};
  EXPECT_EQ(DFSDIgetslice(path.c_str(), winst, bad_windims, out, out_dims, 0),
            FAIL);

  // winst + windims - 1 > dimsize
  int32_t ovf_winst[2] = {3, 1};
  int32_t ovf_windims[2] = {3, 2};  // 3 + 3 - 1 = 5 > 4
  EXPECT_EQ(
      DFSDIgetslice(path.c_str(), ovf_winst, ovf_windims, out, out_dims, 0),
      FAIL);

  // out_dims < windims
  int32_t small_out_dims[2] = {1, 4};
  EXPECT_EQ(DFSDIgetslice(path.c_str(), winst, windims, out, small_out_dims, 0),
            FAIL);
}

TEST_F(DfsdTest, SlabValidationErrorsAndNativeFill) {
  const std::string dir = ::testing::TempDir();
  std::string path = dir + "/dfsd_slab_native.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  EXPECT_EQ(DFSDrestart(), SUCCEED);

  // DFSDstartslab with rank == 0 -> FAIL
  EXPECT_EQ(DFSDstartslab(path.c_str()), FAIL);

  // Now set dims and native fill value
  int32_t dims[2] = {2, 2};
  EXPECT_EQ(DFSDsetdims(2, dims), SUCCEED);
  EXPECT_EQ(DFSDsetNT(DFNT_NFLOAT32), SUCCEED);
  float fill = -1.0f;
  EXPECT_EQ(DFSDsetfillvalue(&fill), SUCCEED);

  // Calling DFSDstartslab with valid path (covers lines 4667-4668 native fill
  // memcpy)
  EXPECT_EQ(DFSDstartslab(path.c_str()), SUCCEED);

  // DFSDwriteslab with nullptr data -> FAIL
  int32_t st[2] = {1, 1}, str[2] = {1, 1}, cnt[2] = {1, 1};
  float d[1] = {1.0f};
  EXPECT_EQ(DFSDwriteslab(st, str, cnt, nullptr), FAIL);

  // Write valid slab
  EXPECT_EQ(DFSDwriteslab(st, str, cnt, d), SUCCEED);
  EXPECT_EQ(DFSDendslab(), SUCCEED);
}

TEST_F(DfsdTest, StringsNullBranchesAndDefaults) {
  const std::string dir = ::testing::TempDir();
  std::string path = dir + "/dfsd_str_null.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  EXPECT_EQ(DFSDrestart(), SUCCEED);
  int32_t dims[2] = {2, 2};
  // DFSDsetrange without prior DFSDsetNT -> defaults to float32 (line 1158)
  float max_v = 10.0f, min_v = 1.0f;
  EXPECT_EQ(DFSDsetrange(&max_v, &min_v), SUCCEED);

  // Set data strings without coordsys (coordsys = nullptr)
  EXPECT_EQ(DFSDsetdatastrs("lbl", "unit", "fmt", nullptr), SUCCEED);

  std::vector<float> data(4, 5.0f);
  EXPECT_EQ(DFSDputdata(path.c_str(), 2, dims, data.data()), SUCCEED);

  int rrank = 0;
  int32_t rdims[2] = {};
  EXPECT_EQ(DFSDgetdims(path.c_str(), &rrank, rdims, 2), SUCCEED);

  // Query datastrs with non-null coordsys buffer -> receives empty string (line
  // 331)
  char rlbl[16] = {}, runit[16] = {}, rfmt[16] = {}, rcoord[16] = "initial";
  EXPECT_EQ(DFSDgetdatastrs(rlbl, runit, rfmt, rcoord), SUCCEED);
  EXPECT_STREQ(rcoord, "");

  // Query dimstrs when dim labels were not set -> receives empty strings (line
  // 391)
  char dlbl[16] = "init", dunit[16] = "init", dfmt[16] = "init";
  EXPECT_EQ(DFSDgetdimstrs(1, dlbl, dunit, dfmt), SUCCEED);
  EXPECT_STREQ(dlbl, "");
  EXPECT_STREQ(dunit, "");
  EXPECT_STREQ(dfmt, "");
}

TEST_F(DfsdTest, DetailedErrorBranchesAndDefaults) {
  const std::string dir = ::testing::TempDir();
  std::string path = dir + "/dfsd_detailed_branches.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  EXPECT_EQ(DFSDrestart(), SUCCEED);

  // Setdims called twice with same dimensions (line 765 shortcut)
  int32_t dims[2] = {3, 4};
  EXPECT_EQ(DFSDsetdims(2, dims), SUCCEED);
  EXPECT_EQ(DFSDsetdims(2, dims), SUCCEED);

  // DFSDsetdimscale without prior DFSDsetNT defaults to DFNT_FLOAT32 (line
  // 1070)
  float scale_data[3] = {1.0f, 2.0f, 3.0f};
  EXPECT_EQ(DFSDsetdimscale(1, 3, scale_data), SUCCEED);
  // DFSDsetdimscale with bad dim
  EXPECT_EQ(DFSDsetdimscale(0, 3, scale_data), FAIL);
  EXPECT_EQ(DFSDsetdimscale(99, 3, scale_data), FAIL);

  std::vector<float> data(12, 1.5f);
  EXPECT_EQ(DFSDputdata(path.c_str(), 2, dims, data.data()), SUCCEED);

  // DFSDgetdims
  int rrank = 0;
  int32_t rdims[4] = {};
  EXPECT_EQ(DFSDrestart(), SUCCEED);
  EXPECT_EQ(DFSDgetdims(path.c_str(), &rrank, rdims, 2), SUCCEED);
  EXPECT_EQ(rrank, 2);

  // DFSDgetdimstrs bad dim
  char dlbl[16] = {}, dunit[16] = {}, dfmt[16] = {};
  EXPECT_EQ(DFSDgetdimstrs(0, dlbl, dunit, dfmt), FAIL);
  EXPECT_EQ(DFSDgetdimstrs(99, dlbl, dunit, dfmt), FAIL);

  // DFSDgetdimlen bad dim
  int len1 = 0, len2 = 0, len3 = 0;
  EXPECT_EQ(DFSDgetdimlen(99, &len1, &len2, &len3), FAIL);

  // DFSDgetdimscale bad dim and buffer too small
  float rscale[4] = {};
  EXPECT_EQ(DFSDgetdimscale(0, 4, rscale), FAIL);
  EXPECT_EQ(DFSDgetdimscale(99, 4, rscale), FAIL);
  EXPECT_EQ(DFSDgetdimscale(1, 1, rscale), FAIL);  // maxsize 1 < dimsize 3
  EXPECT_EQ(DFSDgetdimscale(1, 4, rscale), SUCCEED);

  // DFSDendslab cleanup when nsdghdr != NULL (lines 5078-5092)
  // nsdghdr was loaded by DFSDgetdims above.
  EXPECT_EQ(DFSDrestart(), SUCCEED);
  int32_t sdims[2] = {2, 2};
  EXPECT_EQ(DFSDsetdims(2, sdims), SUCCEED);
  EXPECT_EQ(DFSDstartslab(path.c_str()), SUCCEED);
  int32_t st[2] = {1, 1}, cnt[2] = {2, 2}, str[2] = {1, 1};
  float slab_data[4] = {10.0f, 20.0f, 30.0f, 40.0f};
  EXPECT_EQ(DFSDwriteslab(st, str, cnt, slab_data), SUCCEED);
  EXPECT_EQ(DFSDendslab(), SUCCEED);  // Frees nsdghdr->nsdg_t list!

  // DFSDIgetslice and DFSDIgetdata with isfortran = 1 (lines 3886, 3896)
  std::vector<float> fort_data(12, 0.0f);
  int32_t maxsizes[2] = {3, 4};
  EXPECT_EQ(DFSDrestart(), SUCCEED);
  EXPECT_EQ(DFSDgetdims(path.c_str(), &rrank, rdims, 2), SUCCEED);
  EXPECT_EQ(DFSDIgetdata(path.c_str(), 2, maxsizes, fort_data.data(), 1),
            SUCCEED);
  int32_t winst[2] = {1, 1}, windims[2] = {3, 4};
  EXPECT_EQ(DFSDIgetslice(path.c_str(), winst, windims, fort_data.data(),
                          maxsizes, 1),
            SUCCEED);

  // Slices: DFSDstartslice, DFSDputslice error checks, and 2D slice writing
  std::string slice_path = dir + "/dfsd_slice_2d.hdf";
  (void)remove(slice_path.c_str());
  const absl::Cleanup cleanup2 = [&slice_path] {
    (void)remove(slice_path.c_str());
  };

  EXPECT_EQ(DFSDrestart(), SUCCEED);
  EXPECT_EQ(DFSDsetdims(2, sdims), SUCCEED);
  EXPECT_EQ(DFSDsetNT(DFNT_FLOAT32), SUCCEED);
  EXPECT_EQ(DFSDstartslice(slice_path.c_str()), SUCCEED);

  // DFSDputslice parameter validation (lines 4146-4171)
  int32_t valid_windims[2] = {1, 2};
  int32_t mem_dims[2] = {1, 2};
  EXPECT_EQ(DFSDputslice(valid_windims, nullptr, mem_dims), FAIL);
  int32_t bad_windims_zero[2] = {0, 2};
  EXPECT_EQ(DFSDputslice(bad_windims_zero, slab_data, mem_dims), FAIL);
  int32_t bad_windims_high[2] = {10, 2};
  EXPECT_EQ(DFSDputslice(bad_windims_high, slab_data, mem_dims), FAIL);
  int32_t bad_mem_dims[2] = {0, 2};  // mem_dims[0] < windims[0]
  EXPECT_EQ(DFSDputslice(valid_windims, slab_data, bad_mem_dims), FAIL);
  int32_t bad_mismatch_dim[2] = {
      2, 1};  // windims[0] != 1 and windims[1] != dimsizes[1]
  EXPECT_EQ(DFSDputslice(bad_mismatch_dim, slab_data, mem_dims), FAIL);

  // Write rank > 1 slice (exercises line 4220-4222)
  EXPECT_EQ(DFSDputslice(valid_windims, slab_data, mem_dims), SUCCEED);
  EXPECT_EQ(DFSDputslice(valid_windims, slab_data + 2, mem_dims), SUCCEED);
  EXPECT_EQ(DFSDendslice(), SUCCEED);

  // Multi-dimensional slab writing with conversion (lines 4919-4927, 4981)
  std::string conv_path = dir + "/dfsd_slab_conv.hdf";
  (void)remove(conv_path.c_str());
  const absl::Cleanup cleanup3 = [&conv_path] {
    (void)remove(conv_path.c_str());
  };

  EXPECT_EQ(DFSDrestart(), SUCCEED);
  EXPECT_EQ(DFSDsetdims(2, sdims), SUCCEED);
  EXPECT_EQ(DFSDsetNT(DFNT_INT16 | DFNT_LITEND), SUCCEED);
  EXPECT_EQ(DFSDstartslab(conv_path.c_str()), SUCCEED);
  int16_t conv_data[4] = {100, 200, 300, 400};
  EXPECT_EQ(DFSDwriteslab(st, str, cnt, conv_data), SUCCEED);
  EXPECT_EQ(DFSDendslab(), SUCCEED);
}

extern "C" int DFSDPshutdown(void);

TEST_F(DfsdTest, SlabFillValueChunkingAndOddSize) {
  const std::string dir = ::testing::TempDir();
  const std::string path1 = dir + "/dfsd_slab_fill_conv.hdf";
  const std::string path2 = dir + "/dfsd_slab_fill_noconv.hdf";
  (void)remove(path1.c_str());
  (void)remove(path2.c_str());
  const absl::Cleanup cleanup = [&path1, &path2] {
    (void)remove(path1.c_str());
    (void)remove(path2.c_str());
  };

  // Dimensions: 5000 elements = 20,000 bytes > 16384 (fill_bufsize)
  // Has both chunk write and odd_size write!
  int32_t dims[2] = {50, 100};

  // Case 1: conversion (DFNT_FLOAT32 default big-endian on x86)
  EXPECT_EQ(DFSDrestart(), SUCCEED);
  EXPECT_EQ(DFSDsetdims(2, dims), SUCCEED);
  EXPECT_EQ(DFSDsetNT(DFNT_FLOAT32), SUCCEED);
  float fill1 = -999.5f;
  EXPECT_EQ(DFSDsetfillvalue(&fill1), SUCCEED);
  EXPECT_EQ(DFSDstartslab(path1.c_str()), SUCCEED);
  int32_t start[2] = {1, 1};
  int32_t stride[2] = {1, 1};
  int32_t count[2] = {1, 1};
  float val1 = 42.0f;
  EXPECT_EQ(DFSDwriteslab(start, stride, count, &val1), SUCCEED);
  EXPECT_EQ(DFSDendslab(), SUCCEED);

  // Case 2: no conversion (DFNT_FLOAT32 | DFNT_NATIVE)
  EXPECT_EQ(DFSDclear(), SUCCEED);
  EXPECT_EQ(DFSDrestart(), SUCCEED);
  EXPECT_EQ(DFSDsetdims(2, dims), SUCCEED);
  EXPECT_EQ(DFSDsetNT(DFNT_FLOAT32 | DFNT_NATIVE), SUCCEED);
  float fill2 = 123.4f;
  EXPECT_EQ(DFSDsetfillvalue(&fill2), SUCCEED);
  EXPECT_EQ(DFSDstartslab(path2.c_str()), SUCCEED);
  float val2 = 84.0f;
  EXPECT_EQ(DFSDwriteslab(start, stride, count, &val2), SUCCEED);
  EXPECT_EQ(DFSDendslab(), SUCCEED);
}

TEST_F(DfsdTest, SlabCollapsedRankWriteAndNonContiguousRead) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_slab_collapsed.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  int32_t dims[2] = {4, 4};
  EXPECT_EQ(DFSDsetdims(2, dims), SUCCEED);
  EXPECT_EQ(DFSDsetNT(DFNT_FLOAT32 | DFNT_NATIVE), SUCCEED);
  EXPECT_EQ(DFSDstartslab(path.c_str()), SUCCEED);

  // Write full width 2 rows: start {1, 1}, count {2, 4}
  // Collapses rank to 1 in DFSDwriteslab (lines 4914-4921)
  int32_t start[2] = {1, 1};
  int32_t stride[2] = {1, 1};
  int32_t count[2] = {2, 4};
  std::vector<float> data = {1, 2, 3, 4, 5, 6, 7, 8};
  EXPECT_EQ(DFSDwriteslab(start, stride, count, data.data()), SUCCEED);
  EXPECT_EQ(DFSDendslab(), SUCCEED);

  // Read back non-contiguously (buf_size[1] > slab_size[1])
  int32_t r_start[2] = {1, 1};
  int32_t r_slab[2] = {2, 2};
  int32_t r_buf[2] = {4, 4};
  std::vector<float> out(16, 0.0f);
  EXPECT_EQ(
      DFSDreadslab(path.c_str(), r_start, r_slab, r_start, out.data(), r_buf),
      SUCCEED);
  EXPECT_FLOAT_EQ(out[0], 1.0f);
  EXPECT_FLOAT_EQ(out[1], 2.0f);
}

TEST_F(DfsdTest, NonContiguousAndConversionSlicePutGet) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_slice_noncontig.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  int32_t file_dims[2] = {4, 4};
  EXPECT_EQ(DFSDsetdims(2, file_dims), SUCCEED);
  EXPECT_EQ(DFSDsetNT(DFNT_FLOAT32), SUCCEED);  // triggers convert on x86
  EXPECT_EQ(DFSDstartslice(path.c_str()), SUCCEED);

  // User buffer dimensions are {6, 4} (larger than file dim 4 =>
  // non-contiguous!)
  int32_t mem_dims[2] = {6, 4};
  int32_t windims[2] = {2, 4};
  std::vector<float> mem_buf(24, 0.0f);
  for (int i = 0; i < 24; ++i) mem_buf[i] = static_cast<float>(i);

  // Put two slices (exercises lines 4242-4255 conversion loop in DFSDIputslice)
  EXPECT_EQ(DFSDputslice(windims, mem_buf.data(), mem_dims), SUCCEED);
  EXPECT_EQ(DFSDputslice(windims, mem_buf.data() + 8, mem_dims), SUCCEED);
  EXPECT_EQ(DFSDendslice(), SUCCEED);

  // Read slice back with conversion and non-contiguous memory buffer
  int32_t winst[2] = {1, 1};
  int32_t r_windims[2] = {2, 2};
  int32_t r_mem_dims[2] = {4, 4};
  std::vector<float> r_mem(16, 0.0f);
  EXPECT_EQ(
      DFSDgetslice(path.c_str(), winst, r_windims, r_mem.data(), r_mem_dims),
      SUCCEED);
}

TEST_F(DfsdTest, TransposedSliceReadPureSdg) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_transposed_sdg.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  // Create a pure SDG file
  int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);

  // Write DFTAG_SD
  uint16_t sd_ref = Hnewref(fid);
  int32_t aid = Hstartwrite(fid, DFTAG_SD, sd_ref, 6 * sizeof(float));
  std::vector<float> sd_data = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
  Hwrite(aid, 6 * sizeof(float), reinterpret_cast<uint8_t*>(sd_data.data()));
  Hendaccess(aid);

  // Write DFTAG_NT
  uint16_t nt_ref = Hnewref(fid);
  uint8_t nt_bytes[4] = {DFNT_VERSION, DFNT_FLOAT32, 32, DFNTF_IEEE};
  Hputelement(fid, DFTAG_NT, nt_ref, nt_bytes, 4);

  // Write DFTAG_SDD
  uint16_t sdd_ref = Hnewref(fid);
  aid = Hstartwrite(fid, DFTAG_SDD, sdd_ref, 2 + 2 * 4 + 4 + 2 * 4);
  uint8_t sdd_buf[22];
  uint8_t* p = sdd_buf;
  INT16ENCODE(p, 2);  // rank 2
  INT32ENCODE(p, 3);  // dim 1 = 3
  INT32ENCODE(p, 2);  // dim 2 = 2
  UINT16ENCODE(p, DFTAG_NT);
  UINT16ENCODE(p, nt_ref);
  UINT16ENCODE(p, DFTAG_NT);
  UINT16ENCODE(p, nt_ref);
  UINT16ENCODE(p, DFTAG_NT);
  UINT16ENCODE(p, nt_ref);
  Hwrite(aid, 22, sdd_buf);
  Hendaccess(aid);

  // Write pure DFTAG_SDG group
  int32_t gid = DFdisetup(5);
  DFdiput(gid, DFTAG_SD, sd_ref);
  DFdiput(gid, DFTAG_SDD, sdd_ref);
  uint16_t sdg_ref = Hnewref(fid);
  DFdiwrite(fid, gid, DFTAG_SDG, sdg_ref);
  Hclose(fid);

  // Read with DFSDIgetslice and isfortran = 1
  // issdg = 1, isfortran = 1 => transposed = 1!
  // Exercises lines 3967-3977, 3995-3999, 4047-4054
  int32_t winst[2] = {1, 1};
  int32_t windims[2] = {3, 2};
  int32_t mem_dims[2] = {3, 2};
  std::vector<float> out(6, 0.0f);
  EXPECT_EQ(DFSDIgetslice(path.c_str(), winst, windims, out.data(), mem_dims,
                          /*isfortran=*/1),
            SUCCEED);

  // Test DFSDpre32sdg on pure SDG
  int ispre32 = 0;
  EXPECT_EQ(DFSDpre32sdg(const_cast<char*>(path.c_str()), sdg_ref, &ispre32),
            SUCCEED);
  EXPECT_EQ(ispre32, 1);
}

TEST_F(DfsdTest, SdlnkLinkingAndMerging) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_sdlnk.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);

  // DFTAG_SD
  uint16_t sd_ref = Hnewref(fid);
  int32_t aid = Hstartwrite(fid, DFTAG_SD, sd_ref, 4 * sizeof(float));
  float val = 1.0f;
  for (int i = 0; i < 4; ++i)
    Hwrite(aid, sizeof(float), reinterpret_cast<uint8_t*>(&val));
  Hendaccess(aid);

  // DFTAG_NT
  uint16_t nt_ref = Hnewref(fid);
  uint8_t nt_bytes[4] = {DFNT_VERSION, DFNT_FLOAT32, 32, DFNTF_IEEE};
  Hputelement(fid, DFTAG_NT, nt_ref, nt_bytes, 4);

  // DFTAG_SDD
  uint16_t sdd_ref = Hnewref(fid);
  aid = Hstartwrite(fid, DFTAG_SDD, sdd_ref, 2 + 2 * 4 + 4 + 2 * 4);
  uint8_t sdd_buf[22];
  uint8_t* p = sdd_buf;
  INT16ENCODE(p, 2);
  INT32ENCODE(p, 2);
  INT32ENCODE(p, 2);
  UINT16ENCODE(p, DFTAG_NT);
  UINT16ENCODE(p, nt_ref);
  UINT16ENCODE(p, DFTAG_NT);
  UINT16ENCODE(p, nt_ref);
  UINT16ENCODE(p, DFTAG_NT);
  UINT16ENCODE(p, nt_ref);
  Hwrite(aid, 22, sdd_buf);
  Hendaccess(aid);

  uint16_t ndg_ref = Hnewref(fid);
  uint16_t sdg_ref = Hnewref(fid);
  uint16_t lnk_ref = Hnewref(fid);

  // DFTAG_SDLNK: [NDG tag, ndg_ref, SDG tag, sdg_ref]
  uint8_t lnk_buf[8];
  p = lnk_buf;
  UINT16ENCODE(p, DFTAG_NDG);
  UINT16ENCODE(p, ndg_ref);
  UINT16ENCODE(p, DFTAG_SDG);
  UINT16ENCODE(p, sdg_ref);
  Hputelement(fid, DFTAG_SDLNK, lnk_ref, lnk_buf, 8);

  // NDG group
  int32_t ndg_gid = DFdisetup(5);
  DFdiput(ndg_gid, DFTAG_SD, sd_ref);
  DFdiput(ndg_gid, DFTAG_SDD, sdd_ref);
  DFdiput(ndg_gid, DFTAG_SDLNK, lnk_ref);
  DFdiwrite(fid, ndg_gid, DFTAG_NDG, ndg_ref);

  // SDG group
  int32_t sdg_gid = DFdisetup(5);
  DFdiput(sdg_gid, DFTAG_SD, sd_ref);
  DFdiput(sdg_gid, DFTAG_SDD, sdd_ref);
  DFdiput(sdg_gid, DFTAG_SDLNK, lnk_ref);
  DFdiwrite(fid, sdg_gid, DFTAG_SDG, sdg_ref);

  Hclose(fid);

  // Reading the file exercises SDLNK discovery in NDG and SDG,
  // matching and merging in DFSDIsetnsdg_t (lines 2001-2014, 2054-2067,
  // 2077-2099)
  int rank = 0;
  int32_t dims[2] = {0, 0};
  EXPECT_EQ(DFSDgetdims(path.c_str(), &rank, dims, 2), SUCCEED);
  EXPECT_EQ(rank, 2);

  // Test DFSDpre32sdg on NDG-linked SDG (exercises line 1842-1845)
  int ispre32 = -1;
  EXPECT_EQ(DFSDpre32sdg(const_cast<char*>(path.c_str()), sdg_ref, &ispre32),
            SUCCEED);
  EXPECT_EQ(ispre32, 0);

  // Shutdown while nsdghdr is populated (exercises lines 5181-5196 in
  // DFSDPshutdown)
  EXPECT_EQ(DFSDPshutdown(), SUCCEED);
}

TEST_F(DfsdTest, ReadrefWriterefAndRestartOpenSlice) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_refs.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  int32_t dims[2] = {2, 2};
  std::vector<float> data = {1, 2, 3, 4};
  EXPECT_EQ(DFSDputdata(path.c_str(), 2, dims, data.data()), SUCCEED);
  uint16_t last_ref = DFSDlastref();
  EXPECT_GT(last_ref, 0);

  // DFSDreadref non-existent ref (line 1426)
  EXPECT_EQ(DFSDreadref(const_cast<char*>(path.c_str()), 9999), FAIL);
  // DFSDreadref existing ref (line 1425)
  EXPECT_EQ(DFSDreadref(const_cast<char*>(path.c_str()), last_ref), SUCCEED);

  // DFSDwriteref non-existent ref (line 4438)
  EXPECT_EQ(DFSDwriteref(path.c_str(), 9999), FAIL);
  // DFSDwriteref existing ref (line 4437 & 4449)
  EXPECT_EQ(DFSDwriteref(path.c_str(), last_ref), SUCCEED);

  // DFSDstartslab with existing Writeref
  EXPECT_EQ(DFSDstartslab(path.c_str()), SUCCEED);

  // DFSDrestart while Sfile_id != DF_NOFILE (lines 1270-1274)
  EXPECT_EQ(DFSDrestart(), SUCCEED);
  EXPECT_EQ(DFSDendslab(), SUCCEED);
}

TEST_F(DfsdTest, EndslabWithoutWriteslab) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_endslab_nowrite.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  // Write initial dataset
  int32_t dims[2] = {2, 2};
  std::vector<float> data = {1, 2, 3, 4};
  EXPECT_EQ(DFSDputdata(path.c_str(), 2, dims, data.data()), SUCCEED);

  // Read ndatasets so nsdghdr is populated
  EXPECT_EQ(DFSDndatasets(const_cast<char*>(path.c_str())), 1);

  // Start slab and end slab without writeslab:
  // Ref.new_ndg == 0, nsdghdr != NULL => executes lines 5084-5111 in
  // DFSDendslab
  EXPECT_EQ(DFSDsetdims(2, dims), SUCCEED);
  EXPECT_EQ(DFSDstartslab(path.c_str()), SUCCEED);
  EXPECT_EQ(DFSDendslab(), SUCCEED);
}

TEST_F(DfsdTest, RemainingEdgeCasesAndCoverageHoles) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_edge_cases.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  // DFSDsetlengths negative / zero (lines 699-706)
  EXPECT_EQ(DFSDsetlengths(-1, 0, -5, 0), SUCCEED);

  int is_ndg = -1, rrank = -1, wrank = -1;
  EXPECT_EQ(DFSDIisndg(&is_ndg), SUCCEED);
  EXPECT_EQ(DFSDIgetrrank(&rrank), SUCCEED);
  EXPECT_EQ(DFSDIgetwrank(&wrank), SUCCEED);

  // DFSDsetfillvalue when fill_fixed is true and Ref.fill_value == -1 (line
  // 4505) DFSDstartslab with fill value sets Writesdg.fill_fixed = TRUE
  int32_t dims[2] = {2, 2};
  std::vector<float> data = {1, 2, 3, 4};
  float init_fv = 99.0f;
  EXPECT_EQ(DFSDsetdims(2, dims), SUCCEED);
  EXPECT_EQ(DFSDsetNT(DFNT_FLOAT32), SUCCEED);
  EXPECT_EQ(DFSDsetfillvalue(&init_fv), SUCCEED);
  EXPECT_EQ(DFSDstartslab(path.c_str()), SUCCEED);
  EXPECT_EQ(DFSDendslab(), SUCCEED);
  // Writesdg.fill_fixed is now TRUE, and Ref.fill_value is -1
  EXPECT_EQ(DFSDsetfillvalue(&init_fv), FAIL);

  // DFSDclear scale cleanup (lines 1728-1730 in DFSDIclearNT)
  EXPECT_EQ(DFSDrestart(), SUCCEED);
  EXPECT_EQ(DFSDsetdims(2, dims), SUCCEED);
  float scale[2] = {1.0f, 2.0f};
  EXPECT_EQ(DFSDsetdimscale(1, 2, scale), SUCCEED);
  EXPECT_EQ(DFSDclear(), SUCCEED);

  // DFSDIputdata with accmode = 1 (append mode) and isfortran = 1 (lines 3742,
  // 3760)
  EXPECT_EQ(DFSDIputdata(path.c_str(), 2, dims, data.data(), /*accmode=*/1,
                         /*isfortran=*/1),
            SUCCEED);

  // DFSDIgetdata without DFSDgetdims called (lines 3680-3687) and isfortran = 1
  EXPECT_EQ(DFSDrestart(), SUCCEED);
  std::vector<float> rdata(4, 0.0f);
  EXPECT_EQ(DFSDIgetdata(path.c_str(), 2, dims, rdata.data(), /*isfortran=*/1),
            SUCCEED);

  // DFSDIrefresh with Newdata != 1 (lines 3490-3497)
  EXPECT_EQ(DFSDrestart(), SUCCEED);
  EXPECT_EQ(DFSDIrefresh(const_cast<char*>(path.c_str())), SUCCEED);
}

TEST_F(DfsdTest, ReadConversionScaleRangeFillCal) {
  const std::string dir = ::testing::TempDir();
  const std::string path = dir + "/dfsd_read_conv_all.hdf";
  (void)remove(path.c_str());
  const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

  // Write dataset with DFNT_FLOAT32 (big-endian IEEE on x86, causing fileNT !=
  // platnumsubclass)
  int32_t dims[2] = {2, 2};
  EXPECT_EQ(DFSDsetdims(2, dims), SUCCEED);
  EXPECT_EQ(DFSDsetNT(DFNT_FLOAT32), SUCCEED);

  float max_val = 10.0f;
  float min_val = 1.0f;
  EXPECT_EQ(DFSDsetrange(&max_val, &min_val), SUCCEED);

  float fv = -999.0f;
  EXPECT_EQ(DFSDsetfillvalue(&fv), SUCCEED);

  EXPECT_EQ(DFSDsetcal(2.0, 0.1, 10.0, 0.5, DFNT_FLOAT32), SUCCEED);

  float scale1[2] = {1.5f, 2.5f};
  float scale2[2] = {3.5f, 4.5f};
  EXPECT_EQ(DFSDsetdimscale(1, 2, scale1), SUCCEED);
  EXPECT_EQ(DFSDsetdimscale(2, 2, scale2), SUCCEED);

  std::vector<float> data = {1.0f, 2.0f, 3.0f, 4.0f};
  EXPECT_EQ(DFSDputdata(path.c_str(), 2, dims, data.data()), SUCCEED);

  // Now read it back. DFSDgetdims calls DFSDIgetndg, which executes:
  // - DFTAG_SDS conversion reading (lines 2574-2601)
  // - DFTAG_SDM conversion reading (lines 2636-2654)
  // - DFTAG_CAL 36-byte conversion reading (lines 2692-2725)
  // - DFTAG_FV conversion reading (lines 2760-2787)
  EXPECT_EQ(DFSDrestart(), SUCCEED);
  int rank = 0;
  int32_t r_dims[2] = {0, 0};
  EXPECT_EQ(DFSDgetdims(path.c_str(), &rank, r_dims, 2), SUCCEED);
  EXPECT_EQ(rank, 2);

  float r_max = 0.0f, r_min = 0.0f;
  EXPECT_EQ(DFSDgetrange(&r_max, &r_min), SUCCEED);
  EXPECT_FLOAT_EQ(r_max, 10.0f);
  EXPECT_FLOAT_EQ(r_min, 1.0f);

  float r_fv = 0.0f;
  EXPECT_EQ(DFSDgetfillvalue(&r_fv), SUCCEED);
  EXPECT_FLOAT_EQ(r_fv, -999.0f);

  float64 cal, cal_err, ioff, ioff_err;
  int32_t cal_nt = 0;
  EXPECT_EQ(DFSDgetcal(&cal, &cal_err, &ioff, &ioff_err, &cal_nt), SUCCEED);
  EXPECT_DOUBLE_EQ(cal, 2.0);

  float r_scale1[2] = {0.0f, 0.0f};
  EXPECT_EQ(DFSDgetdimscale(1, 2, r_scale1), SUCCEED);
  EXPECT_FLOAT_EQ(r_scale1[0], 1.5f);
  EXPECT_FLOAT_EQ(r_scale1[1], 2.5f);

  // Non-contiguous slice read with conversion (lines 4032-4040)
  int32_t winst[2] = {1, 1};
  int32_t windims[2] = {1, 2};
  int32_t mem_dims[2] = {2, 2};
  std::vector<float> slice_buf(4, 0.0f);
  EXPECT_EQ(
      DFSDgetslice(path.c_str(), winst, windims, slice_buf.data(), mem_dims),
      SUCCEED);
  EXPECT_FLOAT_EQ(slice_buf[0], 1.0f);
  EXPECT_FLOAT_EQ(slice_buf[1], 2.0f);
}

TEST_F(DfsdTest, CalibrationOldFloat16BytesAndSdtAndUnknownReqTag) {
  const std::string dir = ::testing::TempDir();
  const std::string path1 = dir + "/dfsd_old_cal_native.hdf";
  const std::string path2 = dir + "/dfsd_old_cal_conv.hdf";
  const std::string path3 = dir + "/dfsd_unknown_req_tag.hdf";
  (void)remove(path1.c_str());
  (void)remove(path2.c_str());
  (void)remove(path3.c_str());
  const absl::Cleanup cleanup = [&path1, &path2, &path3] {
    (void)remove(path1.c_str());
    (void)remove(path2.c_str());
    (void)remove(path3.c_str());
  };

  // Helper lambda to construct an NDG with custom elements
  auto create_ndg_file = [](const std::string& filepath,
                            uint8_t filenumsubclass, bool old_cal, bool add_sdt,
                            bool add_req_tag) {
    int32_t fid = Hopen(filepath.c_str(), DFACC_CREATE, 0);
    if (fid == FAIL) return;

    // DFTAG_SD (4 floats)
    uint16_t sd_ref = Hnewref(fid);
    int32_t aid = Hstartwrite(fid, DFTAG_SD, sd_ref, 4 * sizeof(float));
    float data[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    Hwrite(aid, 4 * sizeof(float), reinterpret_cast<uint8_t*>(data));
    Hendaccess(aid);

    // DFTAG_NT
    uint16_t nt_ref = Hnewref(fid);
    uint8_t nt_bytes[4] = {DFNT_VERSION, DFNT_FLOAT32, 32, filenumsubclass};
    Hputelement(fid, DFTAG_NT, nt_ref, nt_bytes, 4);

    // DFTAG_SDD
    uint16_t sdd_ref = Hnewref(fid);
    aid = Hstartwrite(fid, DFTAG_SDD, sdd_ref, 2 + 2 * 4 + 4 + 2 * 4);
    uint8_t sdd_buf[22];
    uint8_t* p = sdd_buf;
    INT16ENCODE(p, 2);
    INT32ENCODE(p, 2);
    INT32ENCODE(p, 2);
    UINT16ENCODE(p, DFTAG_NT);
    UINT16ENCODE(p, nt_ref);
    UINT16ENCODE(p, DFTAG_NT);
    UINT16ENCODE(p, nt_ref);
    UINT16ENCODE(p, DFTAG_NT);
    UINT16ENCODE(p, nt_ref);
    Hwrite(aid, 22, sdd_buf);
    Hendaccess(aid);

    int32_t gid = DFdisetup(10);
    DFdiput(gid, DFTAG_SD, sd_ref);
    DFdiput(gid, DFTAG_SDD, sdd_ref);

    if (old_cal) {
      // 16-byte old float calibration: ioff, ioff_err, cal, cal_err
      uint16_t cal_ref = Hnewref(fid);
      float32 cal_vals[4] = {1.0f, 0.1f, 2.0f, 0.05f};
      Hputelement(fid, DFTAG_CAL, cal_ref, reinterpret_cast<uint8_t*>(cal_vals),
                  16);
      DFdiput(gid, DFTAG_CAL, cal_ref);
    }

    if (add_sdt) {
      uint16_t sdt_ref = Hnewref(fid);
      DFdiput(gid, DFTAG_SDT, sdt_ref);
    }

    if (add_req_tag) {
      // Required tag between DFTAG_EREQ (780) and DFTAG_BREQ (799)
      uint16_t bad_tag = 785;
      uint16_t bad_ref = Hnewref(fid);
      uint8_t dummy = 0;
      Hputelement(fid, bad_tag, bad_ref, &dummy, 1);
      DFdiput(gid, bad_tag, bad_ref);
    }

    uint16_t ndg_ref = Hnewref(fid);
    DFdiwrite(fid, gid, DFTAG_NDG, ndg_ref);
    Hclose(fid);
  };

  // Case 1: Native old float CAL (lines 2674-2690) and DFTAG_SDT (lines
  // 2790-2792)
  create_ndg_file(path1, DFNTF_PC, /*old_cal=*/true, /*add_sdt=*/true,
                  /*add_req_tag=*/false);
  EXPECT_EQ(DFSDrestart(), SUCCEED);
  int rank = 0;
  int32_t dims[2] = {0, 0};
  EXPECT_EQ(DFSDgetdims(path1.c_str(), &rank, dims, 2), SUCCEED);
  float64 cal, cal_err, ioff, ioff_err;
  int32_t cal_nt;
  EXPECT_EQ(DFSDgetcal(&cal, &cal_err, &ioff, &ioff_err, &cal_nt), SUCCEED);
  EXPECT_DOUBLE_EQ(ioff, 1.0);
  EXPECT_DOUBLE_EQ(cal, 2.0);

  // Case 2: Conversion old float CAL (lines 2726-2739)
  create_ndg_file(path2, DFNTF_HDFDEFAULT, /*old_cal=*/true, /*add_sdt=*/false,
                  /*add_req_tag=*/false);
  EXPECT_EQ(DFSDrestart(), SUCCEED);
  EXPECT_EQ(DFSDgetdims(path2.c_str(), &rank, dims, 2), SUCCEED);
  EXPECT_EQ(DFSDgetcal(&cal, &cal_err, &ioff, &ioff_err, &cal_nt), SUCCEED);

  // Case 3: Unknown required tag in NDG (lines 2794-2797, DFE_BADNDG)
  create_ndg_file(path3, DFNTF_PC, /*old_cal=*/false, /*add_sdt=*/false,
                  /*add_req_tag=*/true);
  EXPECT_EQ(DFSDrestart(), SUCCEED);
  EXPECT_EQ(DFSDgetdims(path3.c_str(), &rank, dims, 2), FAIL);
}

TEST_F(DfsdTest, ComprehensiveCorruptRecordsAndValidation) {
  const std::string dir = ::testing::TempDir();
  const std::string base = dir + "/dfsd_corrupt_val_";

  // 1. DFSDgetrange & DFSDgetdimscale on dataset with NO values (NOVALS lines
  // 609, 531)
  std::string p_novals = base + "novals.hdf";
  (void)remove(p_novals.c_str());
  int32_t sdims[2] = {4, 4};
  std::vector<float> data(16, 1.0f);
  EXPECT_EQ(DFSDputdata(p_novals.c_str(), 2, sdims, data.data()), SUCCEED);
  EXPECT_EQ(DFSDrestart(), SUCCEED);
  int rank = 0;
  int32_t r_dims[2] = {0, 0};
  EXPECT_EQ(DFSDgetdims(p_novals.c_str(), &rank, r_dims, 2), SUCCEED);
  float pmax, pmin;
  EXPECT_EQ(DFSDgetrange(&pmax, &pmin), FAIL);  // Ismaxmin is 0 -> DFE_NOVALS
  float scale_buf[4];
  EXPECT_EQ(DFSDgetdimscale(1, 4, scale_buf),
            FAIL);  // Readsdg.dimscales is null -> DFE_NOVALS
  // maxsize < dimsize (line 525)
  EXPECT_EQ(DFSDgetdimscale(1, 0, scale_buf), FAIL);  // DFE_NOSPACE
  // dim <= 0 or dim > rank (line 522)
  EXPECT_EQ(DFSDgetdimscale(0, 4, scale_buf), FAIL);
  EXPECT_EQ(DFSDgetdimscale(99, 4, scale_buf), FAIL);

  // DFSDgetdimstrs when Readsdg.dimluf[0] == NULL (lines 384-387)
  char l_buf[32], u_buf[32], f_buf[32];
  EXPECT_EQ(DFSDgetdimstrs(1, l_buf, u_buf, f_buf), SUCCEED);
  EXPECT_STREQ(l_buf, "");

  // DFSDgetdatalen when strings not set (lines 432-435)
  int ll, lu, lf, lc;
  EXPECT_EQ(DFSDgetdatalen(&ll, &lu, &lf, &lc), SUCCEED);
  EXPECT_EQ(ll, 0);
  EXPECT_EQ(lu, 0);
  EXPECT_EQ(lf, 0);
  EXPECT_EQ(lc, 0);
  (void)remove(p_novals.c_str());

  // 2. DFSDIgetndg validation branches:
  auto test_corrupted_ndg = [&](const std::string& name, auto setup_fn) {
    std::string p = base + name + ".hdf";
    (void)remove(p.c_str());
    int32_t fid = Hopen(p.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);
    int32_t gid = DFdisetup(10);
    setup_fn(fid, gid);
    uint16_t ndg_ref = Hnewref(fid);
    DFdiwrite(fid, gid, DFTAG_NDG, ndg_ref);
    Hclose(fid);

    EXPECT_EQ(DFSDrestart(), SUCCEED);
    EXPECT_EQ(DFSDgetdims(p.c_str(), &rank, r_dims, 2), FAIL);
    (void)remove(p.c_str());
  };

  // 4c. NT not DFTAG_NT (line 2262: Hgetelement fails)
  test_corrupted_ndg("bad_nt_tag", [](int32_t fid, int32_t gid) {
    uint16_t sdd_ref = Hnewref(fid);
    uint8_t buf[14];
    uint8_t* p = buf;
    INT16ENCODE(p, 1);
    INT32ENCODE(p, 4);
    UINT16ENCODE(p, DFTAG_SD);  // wrong tag!
    UINT16ENCODE(p, 9999);      // non-existent
    UINT16ENCODE(p, DFTAG_SD);
    UINT16ENCODE(p, 9999);
    Hputelement(fid, DFTAG_SDD, sdd_ref, buf, 14);
    DFdiput(gid, DFTAG_SDD, sdd_ref);
  });

  // 4e. NT DFNT_NONE (line 2355)
  test_corrupted_ndg("bad_nt_none", [](int32_t fid, int32_t gid) {
    uint16_t nt_ref = Hnewref(fid);
    uint8_t nt_bytes[4] = {DFNT_VERSION, DFNT_NONE, 0, DFNTF_NONE};
    Hputelement(fid, DFTAG_NT, nt_ref, nt_bytes, 4);

    uint16_t sdd_ref = Hnewref(fid);
    uint8_t buf[14];
    uint8_t* p = buf;
    INT16ENCODE(p, 1);
    INT32ENCODE(p, 4);
    UINT16ENCODE(p, DFTAG_NT);
    UINT16ENCODE(p, nt_ref);
    UINT16ENCODE(p, DFTAG_NT);
    UINT16ENCODE(p, nt_ref);
    Hputelement(fid, DFTAG_SDD, sdd_ref, buf, 14);
    DFdiput(gid, DFTAG_SDD, sdd_ref);
  });

  // 4f. Invalid fileNT subclass (line 2376)
  test_corrupted_ndg("bad_filent", [](int32_t fid, int32_t gid) {
    uint16_t nt_ref = Hnewref(fid);
    uint8_t nt_bytes[4] = {DFNT_VERSION, DFNT_FLOAT32, 32, 99};  // 99 invalid
    Hputelement(fid, DFTAG_NT, nt_ref, nt_bytes, 4);

    uint16_t sdd_ref = Hnewref(fid);
    uint8_t buf[14];
    uint8_t* p = buf;
    INT16ENCODE(p, 1);
    INT32ENCODE(p, 4);
    UINT16ENCODE(p, DFTAG_NT);
    UINT16ENCODE(p, nt_ref);
    UINT16ENCODE(p, DFTAG_NT);
    UINT16ENCODE(p, nt_ref);
    Hputelement(fid, DFTAG_SDD, sdd_ref, buf, 14);
    DFdiput(gid, DFTAG_SDD, sdd_ref);
  });

  // 4g. DFTAG_SDS before DFTAG_SDD (line 2479: !sdg->dimsizes)
  test_corrupted_ndg("sds_before_sdd", [](int32_t fid, int32_t gid) {
    uint16_t sds_ref = Hnewref(fid);
    uint8_t buf[4] = {0, 0, 0, 0};
    Hputelement(fid, DFTAG_SDS, sds_ref, buf, 4);
    DFdiput(gid, DFTAG_SDS, sds_ref);
  });

  // 4h. DFTAG_SDL before DFTAG_SDD (line 2454: !sdg->dimsizes)
  test_corrupted_ndg("sdl_before_sdd", [](int32_t fid, int32_t gid) {
    uint16_t sdl_ref = Hnewref(fid);
    const char* str = "label\0";
    Hputelement(fid, DFTAG_SDL, sdl_ref, reinterpret_cast<const uint8_t*>(str),
                6);
    DFdiput(gid, DFTAG_SDL, sdl_ref);
  });
}

TEST_F(DfsdTest, TargetedCoverageBoost) {
  const std::string dir = ::testing::TempDir();

  // 1. DimLufNullBranches
  {
    const std::string path = dir + "/dfsd_luf_null_branches.hdf";
    (void)remove(path.c_str());
    const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

    int32_t dims[2] = {2, 3};
    float32 data[6] = {1, 2, 3, 4, 5, 6};
    ASSERT_EQ(DFSDclear(), SUCCEED);
    ASSERT_EQ(DFSDsetdims(2, dims), SUCCEED);
    ASSERT_EQ(DFSDsetdimstrs(1, "dim1_label", nullptr, nullptr), SUCCEED);
    ASSERT_EQ(DFSDputdata(path.c_str(), 2, dims, data), SUCCEED);
  }

  // 2. FortranOrderSlicePutAndEnd
  {
    const std::string path = dir + "/dfsd_fort_slice.hdf";
    (void)remove(path.c_str());
    const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

    int32_t dims[2] = {3, 4};
    ASSERT_EQ(DFSDclear(), SUCCEED);
    ASSERT_EQ(DFSDsetdims(2, dims), SUCCEED);
    ASSERT_EQ(DFSDstartslice(path.c_str()), SUCCEED);

    int32_t rdims[2];
    EXPECT_EQ(DFSDgetdims(path.c_str(), nullptr, rdims, 2), FAIL);

    int32_t windims[2] = {1, 4};
    float32 sdata[4] = {1, 2, 3, 4};
    EXPECT_EQ(DFSDIputslice(windims, sdata, dims, /*isfortran=*/1), SUCCEED);
    EXPECT_EQ(DFSDIputslice(windims, sdata, dims, /*isfortran=*/1), SUCCEED);
    EXPECT_EQ(DFSDIputslice(windims, sdata, dims, /*isfortran=*/1), SUCCEED);
    EXPECT_EQ(DFSDIendslice(/*isfortran=*/1), SUCCEED);
  }

  // 3. GetSliceDimensionTooSmall
  {
    const std::string path = dir + "/dfsd_slice_too_small.hdf";
    (void)remove(path.c_str());
    const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

    int32_t dims[2] = {2, 2};
    float32 data[4] = {1, 2, 3, 4};
    ASSERT_EQ(DFSDclear(), SUCCEED);
    ASSERT_EQ(DFSDputdata(path.c_str(), 2, dims, data), SUCCEED);
    int32_t winst[2] = {1, 1};
    int32_t windims[2] = {2, 2};
    int32_t small_dims[2] = {1, 2};
    float32 out_buf[4];
    EXPECT_EQ(DFSDgetslice(path.c_str(), winst, windims, out_buf, small_dims),
              FAIL);
  }

  // 4. EmptyFileNextNsdg
  {
    const std::string path = dir + "/dfsd_empty_file_nsdg.hdf";
    (void)remove(path.c_str());
    const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

    int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);
    Hclose(fid);
    int32_t rank;
    int32_t dims[2];
    EXPECT_EQ(DFSDgetdims(path.c_str(), &rank, dims, 2), FAIL);
  }

  // 5. PureSdgScaleAndRangeWithoutNT
  {
    const std::string path = dir + "/dfsd_pure_sdg_no_nt.hdf";
    (void)remove(path.c_str());
    const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

    int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);
    int32_t gid = DFdisetup(10);
    ASSERT_NE(gid, FAIL);

    uint16_t nt_ref = Hnewref(fid);
    uint8_t nt_bytes[4] = {DFNT_VERSION, DFNT_FLOAT32, 32, DFNTF_HDFDEFAULT};
    Hputelement(fid, DFTAG_NT, nt_ref, nt_bytes, 4);

    uint16_t sdd_ref = Hnewref(fid);
    uint8_t sdd_buf[14];
    uint8_t* p = sdd_buf;
    INT16ENCODE(p, 1);
    INT32ENCODE(p, 2);
    UINT16ENCODE(p, DFTAG_NT);
    UINT16ENCODE(p, nt_ref);
    UINT16ENCODE(p, DFTAG_NT);
    UINT16ENCODE(p, nt_ref);
    Hputelement(fid, DFTAG_SDD, sdd_ref, sdd_buf, 14);
    DFdiput(gid, DFTAG_SDD, sdd_ref);

    uint16_t sd_ref = Hnewref(fid);
    float32 svals[2] = {1.5f, 2.5f};
    Hputelement(fid, DFTAG_SD, sd_ref, reinterpret_cast<uint8_t*>(svals), 8);
    DFdiput(gid, DFTAG_SD, sd_ref);

    uint16_t sds_ref = Hnewref(fid);
    uint8_t sds_buf[1 + 8] = {1, 0, 0, 0, 0, 0, 0, 0, 0};
    memcpy(sds_buf + 1, svals, 8);
    Hputelement(fid, DFTAG_SDS, sds_ref, sds_buf, 9);
    DFdiput(gid, DFTAG_SDS, sds_ref);

    uint16_t sdm_ref = Hnewref(fid);
    float32 mm[2] = {2.5f, 1.5f};
    Hputelement(fid, DFTAG_SDM, sdm_ref, reinterpret_cast<uint8_t*>(mm), 8);
    DFdiput(gid, DFTAG_SDM, sdm_ref);

    uint16_t sdg_ref = Hnewref(fid);
    DFdiwrite(fid, gid, DFTAG_SDG, sdg_ref);
    DFdifree(gid);
    Hclose(fid);

    DFSDrestart();
    int32_t rank_out;
    int32_t dims_out[1];
    ASSERT_EQ(DFSDgetdims(path.c_str(), &rank_out, dims_out, 1), SUCCEED);
    float32 scale_out[2];
    EXPECT_EQ(DFSDgetdimscale(1, 2, scale_out), SUCCEED);
    float32 max_val, min_val;
    EXPECT_EQ(DFSDgetrange(&max_val, &min_val), SUCCEED);
  }

  // 6. SetDimScaleDefaultsWithDFNTNone
  {
    DFSDclear();
    int32_t dims[1] = {5};
    ASSERT_EQ(DFSDsetdims(1, dims), SUCCEED);
    float32 svals[5] = {1, 2, 3, 4, 5};
    EXPECT_EQ(DFSDsetdimscale(1, 5, svals), SUCCEED);
  }

  // 7. WriterefOnNonSdgGroup
  {
    const std::string path = dir + "/dfsd_writeref_nonsdg.hdf";
    (void)remove(path.c_str());
    const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

    int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(fid, FAIL);
    uint16_t raw_ref = Hnewref(fid);
    uint8_t raw_data[4] = {1, 2, 3, 4};
    Hputelement(fid, DFTAG_SD, raw_ref, raw_data, 4);
    Hclose(fid);

    EXPECT_EQ(DFSDwriteref(path.c_str(), raw_ref), FAIL);
  }

  // 8. MultiDatasetEndSlabAndEndSlice
  {
    const std::string path = dir + "/dfsd_multi_dataset_slab.hdf";
    (void)remove(path.c_str());
    const absl::Cleanup cleanup = [&path] { (void)remove(path.c_str()); };

    int32_t dims[2] = {2, 2};
    float32 data[4] = {1, 2, 3, 4};
    ASSERT_EQ(DFSDclear(), SUCCEED);
    ASSERT_EQ(DFSDputdata(path.c_str(), 2, dims, data), SUCCEED);
    ASSERT_EQ(DFSDputdata(path.c_str(), 2, dims, data), SUCCEED);
    ASSERT_EQ(DFSDputdata(path.c_str(), 2, dims, data), SUCCEED);

    int32_t rrank;
    int32_t rdims[2];
    EXPECT_EQ(DFSDgetdims(path.c_str(), &rrank, rdims, 2), SUCCEED);

    const std::string path2 = dir + "/dfsd_multi_dataset_slab2.hdf";
    (void)remove(path2.c_str());
    const absl::Cleanup cleanup2 = [&path2] { (void)remove(path2.c_str()); };

    ASSERT_EQ(DFSDsetdims(2, dims), SUCCEED);
    ASSERT_EQ(DFSDstartslab(path2.c_str()), SUCCEED);
    int32_t start[2] = {1, 1};
    int32_t stride[2] = {1, 1};
    int32_t count[2] = {2, 2};
    EXPECT_EQ(DFSDwriteslab(start, stride, count, data), SUCCEED);
    EXPECT_EQ(DFSDendslab(), SUCCEED);
  }
}

}  // namespace
}  // namespace hdf4
