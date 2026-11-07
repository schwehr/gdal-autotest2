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
// Tests for NetCDF-2 / HDF SDS data I/O (mfhdf/src/putget.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/mfhdf/src/putget.c

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/hfile_priv.h"
#include "third_party/hdf4/hdf/src/vg.h"
#include "third_party/hdf4/mfhdf/src/mfhdf.h"

// clang-format off
#include "third_party/hdf4/mfhdf/src/nc_priv.h"
#include "third_party/hdf4/mfhdf/src/hdf_xdr_priv.h"
extern "C" {
#include "third_party/hdf4/mfhdf/src/mf_priv.h"
int NC_fill_buffer(NC* handle, int varid, const long* edges, void* values);
}
// clang-format on

namespace hdf4 {
namespace {

using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::Ge;
using ::testing::Gt;
using ::testing::Ne;
using ::testing::NotNull;

void CleanupCdfHandle(NC* handle) {
  if (handle == nullptr) return;
  if (handle->vars != nullptr && handle->vars->values != nullptr) {
    NC_var** vpp = reinterpret_cast<NC_var**>(handle->vars->values);
    for (unsigned i = 0; i < handle->vars->count; ++i) {
      if (vpp[i] != nullptr) {
        vix_t* cur = vpp[i]->vixHead;
        while (cur != nullptr) {
          vix_t* next = cur->next;
          std::free(cur);
          cur = next;
        }
        vpp[i]->vixHead = nullptr;
      }
    }
  }
  if (handle->cdf_fp != nullptr) {
    HI_CLOSE(handle->cdf_fp);
    handle->cdf_fp = nullptr;
  }
}

class PutgetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_ncopts_ = ncopts;
    saved_ncerr_ = ncerr;
    ncopts = 0;
    ncerr = NC_NOERR;
  }

  void TearDown() override {
    SDPfreebuf();
    ncopts = saved_ncopts_;
    ncerr = saved_ncerr_;
    HEclear();
  }

  std::string GetTempFilePath(const std::string& name) {
    return (std::filesystem::path(::testing::TempDir()) / name).string();
  }

  int saved_ncopts_ = 0;
  int saved_ncerr_ = 0;
};

class NetCdfBuilder {
 public:
  void WriteI32(int32_t val) {
    buf_.push_back(static_cast<uint8_t>((val >> 24) & 0xff));
    buf_.push_back(static_cast<uint8_t>((val >> 16) & 0xff));
    buf_.push_back(static_cast<uint8_t>((val >> 8) & 0xff));
    buf_.push_back(static_cast<uint8_t>(val & 0xff));
  }

  void WriteStr(std::string_view s) {
    const uint32_t len = static_cast<uint32_t>(s.size());
    WriteI32(static_cast<int32_t>(len));
    for (char c : s) {
      buf_.push_back(static_cast<uint8_t>(c));
    }
    while (buf_.size() % 4 != 0) {
      buf_.push_back(0);
    }
  }

  void PadTo(size_t total_size) {
    if (buf_.size() < total_size) {
      buf_.resize(total_size, 0);
    }
  }

  void WriteBytes(const void* data, size_t len) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    buf_.insert(buf_.end(), bytes, bytes + len);
  }

  size_t Tell() const { return buf_.size(); }

  void WriteToFile(const std::string& path) const {
    const int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd != -1) {
      (void)write(fd, buf_.data(), buf_.size());
      close(fd);
    }
  }

  const std::vector<uint8_t>& Buffer() const { return buf_; }

 private:
  std::vector<uint8_t> buf_;
};

TEST_F(PutgetTest, InvalidArgsAndErrorReturns) {
  int32_t val = 42;
  const long start[] = {0};
  const long count[] = {1};

  // Invalid cdfid
  EXPECT_THAT(ncvarput1(-1, 0, start, &val), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EBADID));
  EXPECT_THAT(ncvarget1(-1, 0, start, &val), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EBADID));
  EXPECT_THAT(ncvarput(-1, 0, start, count, &val), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EBADID));
  EXPECT_THAT(ncvarget(-1, 0, start, count, &val), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EBADID));

  const std::string p = GetTempFilePath("inval_test.hdf");
  absl::Cleanup rem = [&p] { std::filesystem::remove(p); };

  const int cdfid = nccreate(p.c_str(), NC_CLOBBER);
  ASSERT_THAT(cdfid, Ge(0));
  absl::Cleanup close_c = [cdfid] { (void)ncclose(cdfid); };

  // In define mode -> returns -1
  EXPECT_THAT(ncvarput(cdfid, 0, start, count, &val), Eq(-1));
  EXPECT_THAT(ncvarget(cdfid, 0, start, count, &val), Eq(-1));

  // Invalid varid
  ASSERT_THAT(ncendef(cdfid), Eq(0));
  EXPECT_THAT(ncvarput(cdfid, 999, start, count, &val), Eq(-1));
  EXPECT_THAT(ncvarget(cdfid, 999, start, count, &val), Eq(-1));

  // Reopen read-only
  std::move(close_c).Cancel();
  ASSERT_THAT(ncclose(cdfid), Eq(0));
  const int ro_id = ncopen(p.c_str(), NC_NOWRITE);
  ASSERT_THAT(ro_id, Ge(0));
  absl::Cleanup close_ro = [ro_id] { (void)ncclose(ro_id); };
  EXPECT_THAT(ncvarput(ro_id, 0, start, count, &val), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EPERM));
}

TEST_F(PutgetTest, ClassicNetCdfScalarIo) {
  const std::string p = GetTempFilePath("classic_scalar.nc");
  absl::Cleanup rem = [&p] { std::filesystem::remove(p); };

  NetCdfBuilder b;
  b.WriteI32(0x43444601);  // Magic
  b.WriteI32(0);           // numrecs
  b.WriteI32(0);
  b.WriteI32(0);  // dims: empty
  b.WriteI32(0);
  b.WriteI32(0);  // attrs: empty
  // vars: 1 var
  b.WriteI32(11);
  b.WriteI32(1);
  b.WriteStr("scalar_var");
  b.WriteI32(0);  // assoc count = 0 (scalar)
  b.WriteI32(0);
  b.WriteI32(0);    // attrs empty
  b.WriteI32(4);    // type = NC_LONG
  b.WriteI32(4);    // len = 4
  b.WriteI32(256);  // begin = 256
  b.PadTo(512);
  b.WriteToFile(p);

  const int cdfid = ncopen(p.c_str(), NC_WRITE);
  ASSERT_THAT(cdfid, Ge(0));
  absl::Cleanup close_cdf = [cdfid] { (void)ncclose(cdfid); };

  // Write via ncvarput1
  int32_t val = 12345678;
  EXPECT_THAT(ncvarput1(cdfid, 0, nullptr, &val), Eq(0));

  // Read via ncvarget1
  int32_t read_val = 0;
  EXPECT_THAT(ncvarget1(cdfid, 0, nullptr, &read_val), Eq(0));
  EXPECT_THAT(read_val, Eq(val));

  // Write via ncvarput
  val = 87654321;
  EXPECT_THAT(ncvarput(cdfid, 0, nullptr, nullptr, &val), Eq(0));

  // Read via ncvarget
  read_val = 0;
  EXPECT_THAT(ncvarget(cdfid, 0, nullptr, nullptr, &read_val), Eq(0));
  EXPECT_THAT(read_val, Eq(val));
}

TEST_F(PutgetTest, ClassicNetCdfAllTypesIoAndUnalignedBytesShorts) {
  const std::string p = GetTempFilePath("classic_alltypes.nc");
  absl::Cleanup rem = [&p] { std::filesystem::remove(p); };

  NetCdfBuilder b;
  b.WriteI32(0x43444601);
  b.WriteI32(0);  // numrecs
  // 1 fixed dim of size 8
  b.WriteI32(10);
  b.WriteI32(1);
  b.WriteStr("d8");
  b.WriteI32(8);
  b.WriteI32(0);
  b.WriteI32(0);  // attrs empty
  // 6 vars: BYTE, CHAR, SHORT, LONG, FLOAT, DOUBLE
  b.WriteI32(11);
  b.WriteI32(6);

  // Var 0: NC_BYTE, len = 8, begin = 512
  b.WriteStr("v_byte");
  b.WriteI32(1);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(1);
  b.WriteI32(8);
  b.WriteI32(512);

  // Var 1: NC_CHAR, len = 8, begin = 520
  b.WriteStr("v_char");
  b.WriteI32(1);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(2);
  b.WriteI32(8);
  b.WriteI32(520);

  // Var 2: NC_SHORT, len = 16, begin = 528
  b.WriteStr("v_short");
  b.WriteI32(1);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(3);
  b.WriteI32(16);
  b.WriteI32(528);

  // Var 3: NC_LONG, len = 32, begin = 544
  b.WriteStr("v_long");
  b.WriteI32(1);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(4);
  b.WriteI32(32);
  b.WriteI32(544);

  // Var 4: NC_FLOAT, len = 32, begin = 576
  b.WriteStr("v_float");
  b.WriteI32(1);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(5);
  b.WriteI32(32);
  b.WriteI32(576);

  // Var 5: NC_DOUBLE, len = 64, begin = 608
  b.WriteStr("v_double");
  b.WriteI32(1);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(6);
  b.WriteI32(64);
  b.WriteI32(608);

  b.PadTo(1024);
  b.WriteToFile(p);

  const int cdfid = ncopen(p.c_str(), NC_WRITE);
  ASSERT_THAT(cdfid, Ge(0));
  absl::Cleanup close_cdf = [cdfid] { (void)ncclose(cdfid); };

  // 1. Single element puts and gets at various unaligned word offsets
  // BYTE at indices 0, 1, 2, 3 (hits rem = 0, 1, 2, 3 in xdr_NCv1data)
  for (int i = 0; i < 4; ++i) {
    const long coord[] = {i};
    const uint8_t w = static_cast<uint8_t>(10 + i);
    EXPECT_THAT(ncvarput1(cdfid, 0, coord, &w), Eq(0));
    uint8_t r = 0;
    EXPECT_THAT(ncvarget1(cdfid, 0, coord, &r), Eq(0));
    EXPECT_THAT(r, Eq(w));
  }

  // CHAR at index 1
  {
    const long coord[] = {1};
    const char c = 'Z';
    EXPECT_THAT(ncvarput1(cdfid, 1, coord, &c), Eq(0));
    char r = 0;
    EXPECT_THAT(ncvarget1(cdfid, 1, coord, &r), Eq(0));
    EXPECT_THAT(r, Eq(c));
  }

  // SHORT at index 0 and 1 (hits which = 0 and which = 1/2 in xdr_NCvshort)
  for (int i = 0; i < 2; ++i) {
    const long coord[] = {i};
    const int16_t w = static_cast<int16_t>(500 + i);
    EXPECT_THAT(ncvarput1(cdfid, 2, coord, &w), Eq(0));
    int16_t r = 0;
    EXPECT_THAT(ncvarget1(cdfid, 2, coord, &r), Eq(0));
    EXPECT_THAT(r, Eq(w));
  }

  // Negative short
  {
    const long coord[] = {1};
    const int16_t w = -1234;
    EXPECT_THAT(ncvarput1(cdfid, 2, coord, &w), Eq(0));
    int16_t r = 0;
    EXPECT_THAT(ncvarget1(cdfid, 2, coord, &r), Eq(0));
    EXPECT_THAT(r, Eq(w));
  }

  // LONG, FLOAT, DOUBLE
  {
    const long coord[] = {0};
    const int32_t w_l = 123456;
    EXPECT_THAT(ncvarput1(cdfid, 3, coord, &w_l), Eq(0));
    int32_t r_l = 0;
    EXPECT_THAT(ncvarget1(cdfid, 3, coord, &r_l), Eq(0));
    EXPECT_THAT(r_l, Eq(w_l));

    const float w_f = 3.14f;
    EXPECT_THAT(ncvarput1(cdfid, 4, coord, &w_f), Eq(0));
    float r_f = 0.0f;
    EXPECT_THAT(ncvarget1(cdfid, 4, coord, &r_f), Eq(0));
    EXPECT_THAT(r_f, Eq(w_f));

    const double w_d = 2.718281828;
    EXPECT_THAT(ncvarput1(cdfid, 5, coord, &w_d), Eq(0));
    double r_d = 0.0;
    EXPECT_THAT(ncvarget1(cdfid, 5, coord, &r_d), Eq(0));
    EXPECT_THAT(r_d, Eq(w_d));
  }

  // 2. Unaligned block put/get on BYTE:
  // Starts at rem=1, count=5 (covers xdr_NCvbyte head, opaque body, tail)
  {
    const long start[] = {1};
    const long count[] = {5};
    std::vector<uint8_t> w_bytes = {11, 22, 33, 44, 55};
    EXPECT_THAT(ncvarput(cdfid, 0, start, count, w_bytes.data()), Eq(0));
    std::vector<uint8_t> r_bytes(5, 0);
    EXPECT_THAT(ncvarget(cdfid, 0, start, count, r_bytes.data()), Eq(0));
    EXPECT_THAT(r_bytes, Eq(w_bytes));
  }

  // Small slice on BYTE within a single word: start=1, count=2 (count <= 4 -
  // rem)
  {
    const long start[] = {1};
    const long count[] = {2};
    std::vector<uint8_t> w_small = {77, 88};
    EXPECT_THAT(ncvarput(cdfid, 0, start, count, w_small.data()), Eq(0));
    std::vector<uint8_t> r_small(2, 0);
    EXPECT_THAT(ncvarget(cdfid, 0, start, count, r_small.data()), Eq(0));
    EXPECT_THAT(r_small, Eq(w_small));
  }

  // Unaligned block put/get on SHORT: start=1, count=3
  {
    const long start[] = {1};
    const long count[] = {3};
    std::vector<int16_t> w_shorts = {101, 102, 103};
    EXPECT_THAT(ncvarput(cdfid, 2, start, count, w_shorts.data()), Eq(0));
    std::vector<int16_t> r_shorts(3, 0);
    EXPECT_THAT(ncvarget(cdfid, 2, start, count, r_shorts.data()), Eq(0));
    EXPECT_THAT(r_shorts, Eq(w_shorts));
  }
}

TEST_F(PutgetTest, ClassicNetCdfSimpleRecIo) {
  const std::string p = GetTempFilePath("classic_simplerec.nc");
  absl::Cleanup rem = [&p] { std::filesystem::remove(p); };

  NetCdfBuilder b;
  b.WriteI32(0x43444601);
  b.WriteI32(2);  // numrecs = 2
  b.WriteI32(10);
  b.WriteI32(1);
  b.WriteStr("rec");
  b.WriteI32(0);  // unlimited dim
  b.WriteI32(0);
  b.WriteI32(0);
  // 1 var: 1D record var (assoc.count = 1, handle->recsize == vp->len)
  b.WriteI32(11);
  b.WriteI32(1);
  b.WriteStr("v_rec1d");
  b.WriteI32(1);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(4);
  b.WriteI32(4);
  b.WriteI32(256);
  b.PadTo(512);
  b.WriteToFile(p);

  const int cdfid = ncopen(p.c_str(), NC_WRITE);
  ASSERT_THAT(cdfid, Ge(0));
  absl::Cleanup close_cdf = [cdfid] { (void)ncclose(cdfid); };

  NC* handle = NC_check_id(cdfid);
  ASSERT_THAT(handle, NotNull());
  handle->flags |= NC_NSYNC;  // Test NC_NSYNC sync branch in NCsimplerecio

  // Invalid edges <= 0
  const long start0[] = {0};
  const long edges_bad[] = {0};
  int32_t val = 99;
  EXPECT_THAT(ncvarput(cdfid, 0, start0, edges_bad, &val), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EINVALCOORDS));

  // Read past EOF on XDR_DECODE (start 5, edges 1)
  const long start_past[] = {5};
  const long edges1[] = {1};
  EXPECT_THAT(ncvarget(cdfid, 0, start_past, edges1, &val), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EINVALCOORDS));

  // Write record 2 and record 3
  const long start2[] = {2};
  const long edges2[] = {2};
  int32_t w_data[] = {111, 222};
  EXPECT_THAT(ncvarput(cdfid, 0, start2, edges2, w_data), Eq(0));

  // Read back records 2 and 3
  int32_t r_data[2] = {0, 0};
  EXPECT_THAT(ncvarget(cdfid, 0, start2, edges2, r_data), Eq(0));
  EXPECT_THAT(r_data[0], Eq(111));
  EXPECT_THAT(r_data[1], Eq(222));
}

TEST_F(PutgetTest, ClassicNetCdfHyperslabMultiDimAndGapFill) {
  const std::string p = GetTempFilePath("classic_hyper.nc");
  absl::Cleanup rem = [&p] { std::filesystem::remove(p); };

  NetCdfBuilder b;
  b.WriteI32(0x43444601);
  b.WriteI32(1);  // numrecs = 1
  b.WriteI32(10);
  b.WriteI32(2);
  b.WriteStr("rec");
  b.WriteI32(0);  // dim 0: unlimited
  b.WriteStr("x");
  b.WriteI32(4);  // dim 1: fixed 4
  b.WriteI32(0);
  b.WriteI32(0);  // attrs empty

  // 2 vars: fixed 4-element long, and 2D record variable
  b.WriteI32(11);
  b.WriteI32(2);
  b.WriteStr("f_var");
  b.WriteI32(1);
  b.WriteI32(1);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(4);
  b.WriteI32(16);
  b.WriteI32(256);

  b.WriteStr("r_var");
  b.WriteI32(2);
  b.WriteI32(0);
  b.WriteI32(1);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(4);
  b.WriteI32(16);
  b.WriteI32(272);
  b.PadTo(1024);
  b.WriteToFile(p);

  const int cdfid = ncopen(p.c_str(), NC_WRITE);
  ASSERT_THAT(cdfid, Ge(0));
  absl::Cleanup close_cdf = [cdfid] { (void)ncclose(cdfid); };

  // 1. Partial slab on f_var (start = 1, edges = 2) -> covers NCvcmaxcontig
  // partial edge
  const long start_f[] = {1};
  const long edges_f[] = {2};
  int32_t w_f[] = {33, 44};
  EXPECT_THAT(ncvarput(cdfid, 0, start_f, edges_f, w_f), Eq(0));
  int32_t r_f[2] = {0, 0};
  EXPECT_THAT(ncvarget(cdfid, 0, start_f, edges_f, r_f), Eq(0));
  EXPECT_THAT(r_f[0], Eq(33));
  EXPECT_THAT(r_f[1], Eq(44));

  // Invalid edge on f_var (edges > shape - origin)
  const long edges_bad[] = {5};
  EXPECT_THAT(ncvarput(cdfid, 0, start_f, edges_bad, w_f), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EINVAL));

  // Negative edge
  const long edges_neg[] = {-1};
  EXPECT_THAT(ncvarput(cdfid, 0, start_f, edges_neg, w_f), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EINVAL));

  // 2. Write record 0 of r_var
  const long start_r0[] = {0, 0};
  const long edges_r[] = {1, 4};
  int32_t w_r0[] = {1, 2, 3, 4};
  EXPECT_THAT(ncvarput(cdfid, 1, start_r0, edges_r, w_r0), Eq(0));

  // 3. Write record 3 of r_var (skips records 1 and 2, triggering NCfillrecord
  // gap fill)
  const long start_r3[] = {3, 0};
  int32_t w_r3[] = {30, 31, 32, 33};
  EXPECT_THAT(ncvarput(cdfid, 1, start_r3, edges_r, w_r3), Eq(0));

  // Verify records 0, 1, 2, 3
  int32_t r_all[4 * 4];
  const long start_all[] = {0, 0};
  const long edges_all[] = {4, 4};
  EXPECT_THAT(ncvarget(cdfid, 1, start_all, edges_all, r_all), Eq(0));

  // Record 0 has written data
  EXPECT_THAT(r_all[0], Eq(1));
  EXPECT_THAT(r_all[1], Eq(2));
  EXPECT_THAT(r_all[2], Eq(3));
  EXPECT_THAT(r_all[3], Eq(4));

  // Records 1 and 2 have default fill values (-2147483647)
  EXPECT_THAT(r_all[4], Eq(FILL_LONG));
  EXPECT_THAT(r_all[8], Eq(FILL_LONG));

  // Record 3 has written data
  EXPECT_THAT(r_all[12], Eq(30));
  EXPECT_THAT(r_all[13], Eq(31));
  EXPECT_THAT(r_all[14], Eq(32));
  EXPECT_THAT(r_all[15], Eq(33));
}

TEST_F(PutgetTest, HdfFileAllTypesAndNativeLittleEndian) {
  const std::string p = GetTempFilePath("hdf_alltypes.hdf");
  absl::Cleanup rem = [&p] { std::filesystem::remove(p); };

  const int32 sd_id = SDstart(p.c_str(), DFACC_CREATE);
  ASSERT_THAT(sd_id, Ge(0));
  absl::Cleanup close_sd = [sd_id] { EXPECT_EQ(SUCCEED, SDend(sd_id)); };

  int32 dims[] = {4};

  // Test all HDF types: INT8, UINT8, INT16, UINT16, INT32, UINT32, FLOAT32,
  // FLOAT64, CHAR8, UCHAR8
  const int32 types[] = {DFNT_INT8,   DFNT_UINT8,  DFNT_INT16,   DFNT_UINT16,
                         DFNT_INT32,  DFNT_UINT32, DFNT_FLOAT32, DFNT_FLOAT64,
                         DFNT_CHAR,   DFNT_UCHAR,  DFNT_NINT16,  DFNT_NINT32,
                         DFNT_LINT16, DFNT_LINT32};

  for (size_t i = 0; i < std::size(types); ++i) {
    const std::string name = "var_" + std::to_string(i);
    const int32 sds = SDcreate(sd_id, name.c_str(), types[i], 1, dims);
    ASSERT_THAT(sds, Ge(0));

    int32 start[] = {0};
    int32 edges[] = {4};

    switch (types[i]) {
      case DFNT_INT8:
      case DFNT_UINT8:
      case DFNT_CHAR:
      case DFNT_UCHAR: {
        uint8_t w[4] = {10, 20, 30, 40};
        EXPECT_THAT(SDwritedata(sds, start, nullptr, edges, w), Eq(0));
        uint8_t r[4] = {0, 0, 0, 0};
        EXPECT_THAT(SDreaddata(sds, start, nullptr, edges, r), Eq(0));
        EXPECT_THAT(r[0], Eq(10));
        EXPECT_THAT(r[1], Eq(20));
        break;
      }
      case DFNT_INT16:
      case DFNT_UINT16:
      case DFNT_NINT16:
      case DFNT_LINT16: {
        int16_t w[4] = {10, 20, 30, 40};
        EXPECT_THAT(SDwritedata(sds, start, nullptr, edges, w), Eq(0));
        int16_t r[4] = {0, 0, 0, 0};
        EXPECT_THAT(SDreaddata(sds, start, nullptr, edges, r), Eq(0));
        EXPECT_THAT(r[0], Eq(10));
        EXPECT_THAT(r[1], Eq(20));
        break;
      }
      case DFNT_FLOAT32: {
        float w[4] = {10.0f, 20.0f, 30.0f, 40.0f};
        EXPECT_THAT(SDwritedata(sds, start, nullptr, edges, w), Eq(0));
        float r[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        EXPECT_THAT(SDreaddata(sds, start, nullptr, edges, r), Eq(0));
        EXPECT_THAT(r[0], Eq(10.0f));
        EXPECT_THAT(r[1], Eq(20.0f));
        break;
      }
      case DFNT_FLOAT64: {
        double w[4] = {10.0, 20.0, 30.0, 40.0};
        EXPECT_THAT(SDwritedata(sds, start, nullptr, edges, w), Eq(0));
        double r[4] = {0.0, 0.0, 0.0, 0.0};
        EXPECT_THAT(SDreaddata(sds, start, nullptr, edges, r), Eq(0));
        EXPECT_THAT(r[0], Eq(10.0));
        EXPECT_THAT(r[1], Eq(20.0));
        break;
      }
      default: {
        int32_t w[4] = {10, 20, 30, 40};
        EXPECT_THAT(SDwritedata(sds, start, nullptr, edges, w), Eq(0));
        int32_t r[4] = {0, 0, 0, 0};
        EXPECT_THAT(SDreaddata(sds, start, nullptr, edges, r), Eq(0));
        EXPECT_THAT(r[0], Eq(10));
        EXPECT_THAT(r[1], Eq(20));
        break;
      }
    }

    EXPECT_THAT(SDendaccess(sds), Eq(0));
  }

  // Scalar variable on HDF file via ncvarput1 / ncvarget1 (hits
  // hdf_xdr_NCv1data)
  const int cdfid = sd_id & 0xffff;
  NC* handle = NC_check_id(cdfid);
  ASSERT_THAT(handle, NotNull());
  handle->flags |= NC_INDEF;
  const int v_scalar = ncvardef(cdfid, "hdf_scalar", NC_LONG, 0, nullptr);
  ASSERT_THAT(v_scalar, Ge(0));
  ASSERT_THAT(ncendef(cdfid), Eq(0));

  int32_t s_val = 987654;
  EXPECT_THAT(ncvarput1(cdfid, v_scalar, nullptr, &s_val), Eq(0));
  int32_t s_read = 0;
  EXPECT_THAT(ncvarget1(cdfid, v_scalar, nullptr, &s_read), Eq(0));
  EXPECT_THAT(s_read, Eq(s_val));
}

TEST_F(PutgetTest, HdfFileFillModesAndStorageAllocation) {
  const std::string p = GetTempFilePath("hdf_fillmodes.hdf");
  absl::Cleanup rem = [&p] { std::filesystem::remove(p); };

  const int32 sd_id = SDstart(p.c_str(), DFACC_CREATE);
  ASSERT_THAT(sd_id, Ge(0));
  absl::Cleanup close_sd = [sd_id] { EXPECT_EQ(SUCCEED, SDend(sd_id)); };

  // 1. Partial write at offset where = 8 (2 elements in) with default fill
  int32 dims4[] = {4};
  const int32 sds1 = SDcreate(sd_id, "sds_lead_fill", DFNT_INT32, 1, dims4);
  ASSERT_THAT(sds1, Ge(0));

  int32 start2[] = {2};
  int32 edges2[] = {2};
  int32 w_part[] = {77, 88};
  EXPECT_THAT(SDwritedata(sds1, start2, nullptr, edges2, w_part), Eq(0));

  int32 r_part[4] = {0, 0, 0, 0};
  int32 start0[] = {0};
  int32 edges4[] = {4};
  EXPECT_THAT(SDreaddata(sds1, start0, nullptr, edges4, r_part), Eq(0));
  EXPECT_THAT(r_part[0], Eq(FILL_LONG));
  EXPECT_THAT(r_part[1], Eq(FILL_LONG));
  EXPECT_THAT(r_part[2], Eq(77));
  EXPECT_THAT(r_part[3], Eq(88));
  EXPECT_THAT(SDendaccess(sds1), Eq(0));

  // 2. Partial write with custom _FillValue
  const int32 sds2 = SDcreate(sd_id, "sds_custom_fill", DFNT_INT32, 1, dims4);
  ASSERT_THAT(sds2, Ge(0));
  int32 custom_fill = -9999;
  EXPECT_THAT(SDsetfillvalue(sds2, &custom_fill), Eq(0));
  EXPECT_THAT(SDwritedata(sds2, start2, nullptr, edges2, w_part), Eq(0));

  std::fill(std::begin(r_part), std::end(r_part), 0);
  EXPECT_THAT(SDreaddata(sds2, start0, nullptr, edges4, r_part), Eq(0));
  EXPECT_THAT(r_part[0], Eq(custom_fill));
  EXPECT_THAT(r_part[1], Eq(custom_fill));
  EXPECT_THAT(r_part[2], Eq(77));
  EXPECT_THAT(r_part[3], Eq(88));
  EXPECT_THAT(SDendaccess(sds2), Eq(0));

  // 3. Partial write with SD_NOFILL mode (hits Hseek at line 1077)
  const int32 sds3 = SDcreate(sd_id, "sds_nofill", DFNT_INT32, 1, dims4);
  ASSERT_THAT(sds3, Ge(0));
  EXPECT_THAT(SDsetfillmode(sd_id, SD_NOFILL), Eq(0));
  EXPECT_THAT(SDwritedata(sds3, start2, nullptr, edges2, w_part), Eq(0));
  EXPECT_THAT(SDsetfillmode(sd_id, SD_FILL), Eq(SD_NOFILL));
  EXPECT_THAT(SDendaccess(sds3), Eq(0));

  // 4. Reading unwritten SDS (vp->data_ref == 0):
  const int cdfid = sd_id & 0xffff;
  NC* handle = NC_check_id(cdfid);
  ASSERT_THAT(handle, NotNull());

  const int32 sds_unwritten =
      SDcreate(sd_id, "sds_unwritten", DFNT_INT32, 1, dims4);
  ASSERT_THAT(sds_unwritten, Ge(0));
  NC_var* vp_unw = SDIget_var(handle, sds_unwritten);
  ASSERT_THAT(vp_unw, NotNull());

  // 4a. In DFACC_RDONLY while vp_unw->data_ref == 0, reading fills with
  // FILL_LONG (putget.c:843-854).
  const int saved_mode = handle->hdf_mode;
  handle->hdf_mode = DFACC_RDONLY;
  std::fill(std::begin(r_part), std::end(r_part), 0);
  EXPECT_THAT(SDreaddata(sds_unwritten, start0, nullptr, edges4, r_part),
              Eq(0));
  EXPECT_THAT(r_part[0], Eq(FILL_LONG));
  EXPECT_THAT(r_part[1], Eq(FILL_LONG));
  handle->hdf_mode = saved_mode;

  // 4b. In DFACC_CREATE with invalid vgid (forces hdf_get_data -> DFREF_NONE ==
  // 0), hits putget.c:855-858 -> FAIL.
  const int32 saved_vgid = vp_unw->vgid;
  vp_unw->vgid = 999999;
  EXPECT_THAT(SDreaddata(sds_unwritten, start0, nullptr, edges4, r_part),
              Eq(FAIL));
  vp_unw->vgid = saved_vgid;

  // 4c. Normal DFACC_CREATE read on unwritten SDS -> FAIL.
  EXPECT_THAT(SDreaddata(sds_unwritten, start0, nullptr, edges4, r_part),
              Eq(FAIL));
  EXPECT_THAT(SDendaccess(sds_unwritten), Eq(0));

  // 5. Compressed SDS read (COMP_CODE_DEFLATE)
  const int32 sds_comp =
      SDcreate(sd_id, "sds_compressed", DFNT_INT32, 1, dims4);
  ASSERT_THAT(sds_comp, Ge(0));
  comp_info c_info{};
  c_info.deflate_setting.level = 6;
  EXPECT_THAT(SDsetcompress(sds_comp, COMP_CODE_DEFLATE, &c_info), Eq(0));
  int32 w_comp[] = {10, 20, 30, 40};
  EXPECT_THAT(SDwritedata(sds_comp, start0, nullptr, edges4, w_comp), Eq(0));
  std::fill(std::begin(r_part), std::end(r_part), 0);
  EXPECT_THAT(SDreaddata(sds_comp, start0, nullptr, edges4, r_part), Eq(0));
  EXPECT_THAT(r_part[0], Eq(10));
  EXPECT_THAT(r_part[3], Eq(40));
  EXPECT_THAT(SDendaccess(sds_comp), Eq(0));

  // 6. Unlimited SDS with block sizes (16, 2048, 70000) and gap records
  int32 u_dims[] = {SD_UNLIMITED, 4};
  const int32 sds_u1 = SDcreate(sd_id, "sds_u1", DFNT_INT32, 2, u_dims);
  ASSERT_THAT(sds_u1, Ge(0));
  EXPECT_THAT(SDsetblocksize(sds_u1, 16), Eq(0));
  int32 u_start0[] = {0, 0};
  int32 u_edges[] = {1, 4};
  int32 u_data[] = {1, 2, 3, 4};
  EXPECT_THAT(SDwritedata(sds_u1, u_start0, nullptr, u_edges, u_data), Eq(0));

  // Write record 3 on sds_u2 so handle->numrecs becomes 4
  const int32 sds_u2 = SDcreate(sd_id, "sds_u2", DFNT_INT32, 2, u_dims);
  ASSERT_THAT(sds_u2, Ge(0));
  EXPECT_THAT(SDsetblocksize(sds_u2, 2048), Eq(0));
  int32 u_start3[] = {3, 0};
  EXPECT_THAT(SDwritedata(sds_u2, u_start3, nullptr, u_edges, u_data), Eq(0));

  // Reading gap record via SDreaddata returns FAIL
  int32 u_start2[] = {2, 0};
  int32 u_read[4] = {0, 0, 0, 0};
  EXPECT_THAT(SDreaddata(sds_u1, u_start2, nullptr, u_edges, u_read), Eq(FAIL));

  // Reading gap record via ncvarget succeeds and fills
  const long nc_start2[] = {2, 0};
  const long nc_edges[] = {1, 4};
  EXPECT_THAT(ncvarget(cdfid, SDnametoindex(sd_id, "sds_u1"), nc_start2,
                       nc_edges, u_read),
              Eq(0));
  EXPECT_THAT(u_read[0], Eq(FILL_LONG));

  EXPECT_THAT(SDendaccess(sds_u1), Eq(0));
  EXPECT_THAT(SDendaccess(sds_u2), Eq(0));

  const int32 sds_u3 = SDcreate(sd_id, "sds_u3", DFNT_INT32, 2, u_dims);
  ASSERT_THAT(sds_u3, Ge(0));
  EXPECT_THAT(SDsetblocksize(sds_u3, 70000), Eq(0));
  EXPECT_THAT(SDwritedata(sds_u3, u_start0, nullptr, u_edges, u_data), Eq(0));
  EXPECT_THAT(SDendaccess(sds_u3), Eq(0));
}

TEST_F(PutgetTest, HdfScaleDataOffset) {
  const std::string p = GetTempFilePath("hdf_scale_offset.hdf");
  absl::Cleanup rem = [&p] { std::filesystem::remove(p); };

  const int32 sd_id = SDstart(p.c_str(), DFACC_CREATE);
  ASSERT_THAT(sd_id, Ge(0));
  absl::Cleanup close_sd = [sd_id] { EXPECT_EQ(SUCCEED, SDend(sd_id)); };

  int32 dims[] = {4};
  const int32 sds = SDcreate(sd_id, "scale_var", DFNT_INT32, 1, dims);
  ASSERT_THAT(sds, Ge(0));

  const int cdfid = sd_id & 0xffff;
  NC* handle = NC_check_id(cdfid);
  ASSERT_THAT(handle, NotNull());
  NC_var* vp = SDIget_var(handle, sds);
  ASSERT_THAT(vp, NotNull());

  // Manually configure data_offset > 0 to test scale offset path in
  // hdf_xdr_NCvdata (lines 921-991)
  int32 fv = -123;
  EXPECT_THAT(SDsetfillvalue(sds, &fv), Eq(0));
  vp->data_offset = 16;
  int32 start[] = {0};
  int32 edges[] = {4};
  int32 vals[] = {100, 200, 300, 400};
  // 1. Due to upstream bug in putget.c:972, with SD_FILL the first write when
  // elem_length <= 0 triggers inverted status check (status == data_size ->
  // FAIL).
  EXPECT_THAT(SDwritedata(sds, start, nullptr, edges, vals), Eq(FAIL));
  EXPECT_THAT(SDendaccess(sds), Eq(0));

  // 1b. Test with NO explicit fill value set (attr == NULL) to hit putget.c:960
  const int32 sds_nofv = SDcreate(sd_id, "scale_nofv", DFNT_INT32, 1, dims);
  ASSERT_THAT(sds_nofv, Ge(0));
  NC_var* vp_nofv = SDIget_var(handle, sds_nofv);
  ASSERT_THAT(vp_nofv, NotNull());
  vp_nofv->data_offset = 16;
  EXPECT_THAT(SDwritedata(sds_nofv, start, nullptr, edges, vals), Eq(FAIL));
  EXPECT_THAT(SDendaccess(sds_nofv), Eq(0));

  // 2. With SD_NOFILL on a fresh SDS, line 924 is skipped, data_offset is
  // applied, and Hseek succeeds.
  EXPECT_THAT(SDsetfillmode(sd_id, SD_NOFILL), Eq(SD_FILL));
  const int32 sds_nf = SDcreate(sd_id, "scale_nofill", DFNT_INT32, 1, dims);
  ASSERT_THAT(sds_nf, Ge(0));
  NC_var* vp_nf = SDIget_var(handle, sds_nf);
  ASSERT_THAT(vp_nf, NotNull());
  vp_nf->data_offset = 16;
  vp_nf->len += 16;
  EXPECT_THAT(SDwritedata(sds_nf, start, nullptr, edges, vals), Eq(0));

  int32 r_vals[4] = {0, 0, 0, 0};
  EXPECT_THAT(SDreaddata(sds_nf, start, nullptr, edges, r_vals), Eq(0));
  EXPECT_THAT(r_vals[0], Eq(100));
  EXPECT_THAT(r_vals[3], Eq(400));

  EXPECT_THAT(SDendaccess(sds_nf), Eq(0));
}

TEST_F(PutgetTest, CdfFileReadAndGapFill) {
  const std::string file_path = GetTempFilePath("cdf_putget.cdf");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  NetCdfBuilder b;
  // 1. Magic
  b.WriteI32(0x0000FFFF);
  b.WriteI32(0x0000FFFF);

  // 2. CDR (48 bytes, offset 8..55)
  b.WriteI32(48);
  b.WriteI32(1);
  b.WriteI32(56);
  b.WriteI32(2);
  b.WriteI32(5);
  b.WriteI32(1);
  b.WriteI32(2);
  for (int i = 0; i < 5; ++i) b.WriteI32(0);

  // 3. GDR (64 bytes, offset 56..119)
  b.WriteI32(64);
  b.WriteI32(2);
  b.WriteI32(120);  // varNext = 120
  b.WriteI32(0);    // zVarNext = 0
  b.WriteI32(0);    // adrNext = 0
  b.WriteI32(0);    // eof
  b.WriteI32(1);    // numRVars = 1
  b.WriteI32(0);    // numAttrs = 0
  b.WriteI32(4);    // maxRec = 4 (fixed size 5)
  b.WriteI32(1);    // numDims = 1
  b.WriteI32(0);    // numZVars = 0
  for (int i = 0; i < 4; ++i) b.WriteI32(0);
  b.WriteI32(3);  // dim 0 size = 3

  // 4. VXR for rvar0: 2 chunks
  const size_t vxr_offset = 120 + 260;  // 380
  // rVar at 120 (size 260)
  b.WriteI32(260);
  b.WriteI32(3);
  b.WriteI32(0);                                 // varNext = 0
  b.WriteI32(4);                                 // nt = CDF_INT4 (NC_LONG)
  b.WriteI32(4);                                 // vMaxRec = 4
  b.WriteI32(static_cast<int32_t>(vxr_offset));  // vxrNext
  b.WriteI32(0);
  b.WriteI32(0);  // vFlags = 0 (non-record var, exercises NC_varoffset vixHead)
  for (int i = 0; i < 4; ++i) b.WriteI32(0);
  const std::vector<char> rfu128(128, 0);
  b.WriteBytes(rfu128.data(), 128);
  b.WriteI32(1);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(0);
  // Pad name to 64 bytes
  std::vector<char> name64(64, '\0');
  std::memcpy(name64.data(), "rvar_data", 9);
  b.WriteBytes(name64.data(), 64);
  b.WriteI32(1);  // variance 1

  // VXR at 380
  ASSERT_THAT(b.Tell(), Eq(vxr_offset));
  b.WriteI32(44);
  b.WriteI32(6);
  b.WriteI32(0);  // next = 0
  b.WriteI32(2);
  b.WriteI32(2);  // nEntries=2, nUsed=2
  b.WriteI32(0);
  b.WriteI32(3);  // firstRec = {0, 3}
  b.WriteI32(1);
  b.WriteI32(3);  // lastRec = {1, 3}
  b.WriteI32(1024);
  b.WriteI32(2048);  // offset = {1024, 2048}

  // Data at offset 1024: record 0 and 1 (3 ints each = 12 bytes each, +8 CDF
  // record header)
  b.PadTo(1024);
  // Chunk 0 record 0: 8-byte header [size, type=2] + 3 ints {10, 20, 30}
  b.WriteI32(20);
  b.WriteI32(2);
  b.WriteI32(10);
  b.WriteI32(20);
  b.WriteI32(30);
  // Chunk 0 record 1: 8-byte header + 3 ints {40, 50, 60}
  b.WriteI32(20);
  b.WriteI32(2);
  b.WriteI32(40);
  b.WriteI32(50);
  b.WriteI32(60);

  // Data at offset 2048: record 3 (3 ints {70, 80, 90})
  b.PadTo(2048);
  b.WriteI32(20);
  b.WriteI32(2);
  b.WriteI32(70);
  b.WriteI32(80);
  b.WriteI32(90);

  b.PadTo(2200);
  b.WriteToFile(file_path);

  const int cdfid = ncopen(file_path.c_str(), NC_NOWRITE);
  ASSERT_THAT(cdfid, Ge(0));
  NC* handle = NC_check_id(cdfid);
  ASSERT_THAT(handle, NotNull());

  // 1. Read record 0 via ncvarget: upstream nssdc_xdr_NCvdata seeks and resizes
  // tBuf without copying data into values ((void)values;), leaving r_buf
  // untouched.
  const long start0[] = {0, 0};
  const long edges[] = {1, 3};
  int32_t r_buf[3] = {777, 777, 777};
  EXPECT_THAT(ncvarget(cdfid, 0, start0, edges, r_buf), Eq(0));
  EXPECT_THAT(r_buf, ElementsAre(777, 777, 777));

  // 2. Read record 1 via ncvarget1 (NCvar1io has no CDF_FILE case; r_val stays
  // untouched).
  const long coord1_2[] = {1, 2};
  int32_t r_val = 777;
  EXPECT_THAT(ncvarget1(cdfid, 0, coord1_2, &r_val), Eq(0));
  EXPECT_THAT(r_val, Eq(777));

  // 3. Read record 2 (missing gap record) -> NC_varoffset returns 0
  const long start2[] = {2, 0};
  EXPECT_THAT(ncvarget(cdfid, 0, start2, edges, r_buf), Eq(0));
  EXPECT_THAT(r_buf, ElementsAre(777, 777, 777));

  // 4. Test NC_fill_buffer directly with and without _FillValue
  int32_t fill_buf[3] = {0, 0, 0};
  EXPECT_THAT(NC_fill_buffer(handle, 0, edges, fill_buf), Eq(0));
  // Untouched because attr == NULL
  EXPECT_THAT(fill_buf[0], Eq(0));

  // Add _FillValue attribute temporarily, then restore vp->attrs so steps 5-7
  // exercise nssdc_xdr_NCvdata / NC_fill_buffer with attr == NULL.
  NC_var* vp = reinterpret_cast<NC_var**>(handle->vars->values)[0];
  {
    int32_t custom_fill = -5555;
    NC_attr* fv_attr = reinterpret_cast<NC_attr*>(
        NC_new_attr(FillValueAttr, NC_LONG, 1, &custom_fill));
    ASSERT_THAT(fv_attr, NotNull());
    absl::Cleanup free_fv = [fv_attr] { NC_free_attr(fv_attr); };
    NC_array* const saved_attrs = vp->attrs;
    NC_attr* attrs[] = {fv_attr};
    NC_array* const added_attrs = NC_new_array(NC_ATTRIBUTE, 1, attrs);
    ASSERT_THAT(added_attrs, NotNull());
    std::move(free_fv).Cancel();
    vp->attrs = added_attrs;
    absl::Cleanup restore_attrs = [vp, saved_attrs, added_attrs] {
      vp->attrs = saved_attrs;
      EXPECT_THAT(NC_free_array(added_attrs), Eq(SUCCEED));
    };
    EXPECT_THAT(NC_fill_buffer(handle, 0, edges, fill_buf), Eq(0));
    // TODO(schwehr): Upstream bug in putget.c NC_fill_buffer (also exercises
    // GOOGLE MODIFICATION FillValueAttr): when attr != NULL, HDmemfill succeeds
    // and falls through to the trailing NC_arrayfill(FILL_LONG) branch.
    EXPECT_THAT(fill_buf[0], Eq(FILL_LONG));
  }

  // 5. Test CDF_FILE in NCsimplerecio & nssdc_xdr_NCvdata
  {
    NC_dim* d0 = reinterpret_cast<NC_dim**>(handle->dims->values)[0];
    const long saved_d0_size = d0->size;
    const int saved_assoc_count = vp->assoc->count;
    const long saved_recsize = handle->recsize;
    const unsigned long saved_shape0 = vp->shape ? vp->shape[0] : 0;
    const long saved_h_numrecs = handle->numrecs;
    const long saved_vp_numrecs = vp->numrecs;
    vix_t* const saved_vix = vp->vixHead;
    auto* const saved_fp = handle->cdf_fp;
    absl::Cleanup restore_var = [&] {
      handle->numrecs = saved_h_numrecs;
      vp->numrecs = saved_vp_numrecs;
      d0->size = saved_d0_size;
      if (vp->shape != nullptr) {
        vp->shape[0] = saved_shape0;
      }
      vp->assoc->count = saved_assoc_count;
      handle->recsize = saved_recsize;
      handle->cdf_fp = saved_fp;
      vp->vixHead = saved_vix;
    };

    handle->numrecs = 2;
    vp->numrecs = 2;
    d0->size = NC_UNLIMITED;
    if (vp->shape != nullptr) {
      vp->shape[0] = NC_UNLIMITED;  // Makes IS_RECVAR(vp) == TRUE
    }
    vp->assoc->count = 1;
    handle->recsize = vp->len;
    const long cdf_s0[] = {0};
    const long cdf_e1[] = {1};
    int32_t cdf_val = -888;
    EXPECT_THAT(ncvarget(cdfid, 0, cdf_s0, cdf_e1, &cdf_val), Eq(0));
    EXPECT_THAT(cdf_val, Eq(-888));

    // Test nssdc_xdr_NCvdata HI_SEEK failure via a non-seekable pipe
    int pfd[2] = {-1, -1};
    ASSERT_EQ(0, pipe(pfd));
    FILE* pipe_fp = fdopen(pfd[0], "rb");
    {
      absl::Cleanup close_pipe = [pfd, pipe_fp] {
        if (pipe_fp != nullptr) {
          std::fclose(pipe_fp);
        } else if (pfd[0] >= 0) {
          close(pfd[0]);
        }
        if (pfd[1] >= 0) {
          close(pfd[1]);
        }
      };
      ASSERT_THAT(pipe_fp, NotNull());
      handle->cdf_fp = pipe_fp;
      EXPECT_THAT(ncvarget(cdfid, 0, cdf_s0, cdf_e1, &cdf_val), Eq(-1));
      handle->cdf_fp = saved_fp;
    }

    // 6. Test IS_RECVAR and vixHead == NULL branches on CDF_FILE
    vp->assoc->count = saved_assoc_count;
    handle->recsize = saved_recsize;
    const long cdf_2d_start[] = {0, 0};
    const long cdf_2d_edges[] = {1, 3};
    EXPECT_THAT(ncvarget(cdfid, 0, cdf_2d_start, cdf_2d_edges, r_buf), Eq(0));
    EXPECT_THAT(r_buf, ElementsAre(777, 777, 777));

    handle->numrecs = saved_h_numrecs;
    vp->numrecs = saved_vp_numrecs;
    d0->size = saved_d0_size;
    if (vp->shape != nullptr) {
      vp->shape[0] = saved_shape0;
    }

    // vixHead == NULL on CDF_FILE (NC_varoffset returns -1)
    vp->vixHead = nullptr;
    EXPECT_THAT(ncvarget(cdfid, 0, start0, edges, r_buf), Eq(-1));
    vp->vixHead = saved_vix;

    // 7. Test 0D scalar varoffset branch on CDF_FILE
    vp->assoc->count = 0;
    const long sc_coord0[] = {0};
    int32_t sc_cdf_val = -777;
    EXPECT_THAT(ncvarget1(cdfid, 0, sc_coord0, &sc_cdf_val), Eq(0));
    EXPECT_THAT(sc_cdf_val, Eq(-777));
  }

  CleanupCdfHandle(handle);
  EXPECT_THAT(ncclose(cdfid), Eq(0));
}

TEST_F(PutgetTest, PartialSdsWriteTailFill) {
  const std::string p = GetTempFilePath("partial_tail_fill.hdf");
  absl::Cleanup rem = [&p] { std::filesystem::remove(p); };

  const int32 sd_id = SDstart(p.c_str(), DFACC_CREATE);
  ASSERT_THAT(sd_id, Ge(0));
  absl::Cleanup close_sd = [sd_id] { EXPECT_EQ(SUCCEED, SDend(sd_id)); };

  // 1. Partial write on newly created SDS without explicit fill value (lines
  // 1240-1325 default fill)
  int32 dims[] = {8};
  const int32 sds1 = SDcreate(sd_id, "sds_partial1", DFNT_INT32, 1, dims);
  ASSERT_THAT(sds1, Ge(0));

  int32 start[] = {0};
  int32 edges[] = {4};
  int32 vals[] = {10, 20, 30, 40};
  // Write only the first 4 of 8 elements; remaining 4 must be filled with
  // FILL_LONG
  EXPECT_THAT(SDwritedata(sds1, start, nullptr, edges, vals), Eq(0));

  int32 edges8[] = {8};
  int32 r_vals[8] = {0};
  EXPECT_THAT(SDreaddata(sds1, start, nullptr, edges8, r_vals), Eq(0));
  EXPECT_THAT(r_vals[0], Eq(10));
  EXPECT_THAT(r_vals[3], Eq(40));
  EXPECT_THAT(r_vals[4], Eq(FILL_LONG));
  EXPECT_THAT(r_vals[7], Eq(FILL_LONG));
  EXPECT_THAT(SDendaccess(sds1), Eq(0));

  // 2. Partial write with custom fill value attribute and type conversion
  // (FLOAT32)
  const int32 sds2 = SDcreate(sd_id, "sds_partial2", DFNT_FLOAT32, 1, dims);
  ASSERT_THAT(sds2, Ge(0));
  float custom_fill = -999.0f;
  EXPECT_THAT(SDsetfillvalue(sds2, &custom_fill), Eq(0));

  float f_vals[] = {1.5f, 2.5f, 3.5f, 4.5f};
  EXPECT_THAT(SDwritedata(sds2, start, nullptr, edges, f_vals), Eq(0));

  float r_fvals[8] = {0.0f};
  EXPECT_THAT(SDreaddata(sds2, start, nullptr, edges8, r_fvals), Eq(0));
  EXPECT_THAT(r_fvals[0], Eq(1.5f));
  EXPECT_THAT(r_fvals[3], Eq(4.5f));
  EXPECT_THAT(r_fvals[4], Eq(-999.0f));
  EXPECT_THAT(r_fvals[7], Eq(-999.0f));
  EXPECT_THAT(SDendaccess(sds2), Eq(0));
}

TEST_F(PutgetTest, MultiDimensionalVarioRippleCounter) {
  const std::string p = GetTempFilePath("vario_ripple.hdf");
  absl::Cleanup rem = [&p] { std::filesystem::remove(p); };

  const int32 sd_id = SDstart(p.c_str(), DFACC_CREATE);
  ASSERT_THAT(sd_id, Ge(0));
  absl::Cleanup close_sd = [sd_id] { EXPECT_EQ(SUCCEED, SDend(sd_id)); };

  // Create 4x4 2D SDS
  int32 dims[] = {4, 4};
  const int32 sds = SDcreate(sd_id, "grid", DFNT_INT32, 2, dims);
  ASSERT_THAT(sds, Ge(0));

  // Write full 4x4 array with initial values
  int32 init_grid[16];
  std::iota(std::begin(init_grid), std::end(init_grid), 0);
  int32 start0[] = {0, 0};
  int32 edges4x4[] = {4, 4};
  EXPECT_THAT(SDwritedata(sds, start0, nullptr, edges4x4, init_grid), Eq(0));

  // Overwrite a 2x2 sub-block starting at {1, 1} via ncvarput (triggers ripple
  // counter in NCvario)
  const int cdfid = sd_id & 0xffff;
  const long sub_start[] = {1, 1};
  const long sub_edges[] = {2, 2};
  int32 sub_data[4] = {100, 101, 200, 201};
  EXPECT_THAT(ncvarput(cdfid, SDnametoindex(sd_id, "grid"), sub_start,
                       sub_edges, sub_data),
              Eq(0));

  // Read back the 2x2 sub-block via ncvarget
  int32 r_sub[4] = {0, 0, 0, 0};
  EXPECT_THAT(ncvarget(cdfid, SDnametoindex(sd_id, "grid"), sub_start,
                       sub_edges, r_sub),
              Eq(0));
  EXPECT_THAT(r_sub[0], Eq(100));
  EXPECT_THAT(r_sub[1], Eq(101));
  EXPECT_THAT(r_sub[2], Eq(200));
  EXPECT_THAT(r_sub[3], Eq(201));
  EXPECT_THAT(SDendaccess(sds), Eq(0));

  // Test 1D unlimited NCsimplerecio + NC_NSYNC header sync (putget.c:1604-1664,
  // 1709-1712)
  int32 u_dims1[] = {SD_UNLIMITED};
  const int32 sds_u = SDcreate(sd_id, "rec_sync", DFNT_INT32, 1, u_dims1);
  ASSERT_THAT(sds_u, Ge(0));
  NC* handle = NC_check_id(cdfid);
  ASSERT_THAT(handle, NotNull());
  handle->flags |= NC_NSYNC;
  int32 u_start1[] = {0};
  int32 u_edges1[] = {2};
  int32 u_vals1[] = {42, 43};
  EXPECT_THAT(SDwritedata(sds_u, u_start1, nullptr, u_edges1, u_vals1), Eq(0));
  EXPECT_THAT(handle->flags & NC_NDIRTY, Eq(0));

  // Also cover NCsimplerecio error branches: read past vp->numrecs (1624-1627)
  // and edges <= 0 (1612-1615)
  const long bad_start[] = {0};
  const long bad_edges5[] = {5};
  int32 r_tmp[5] = {0};
  EXPECT_THAT(ncvarget(cdfid, sds_u & 0xffff, bad_start, bad_edges5, r_tmp),
              Eq(-1));
  const long zero_edges[] = {0};
  EXPECT_THAT(ncvarget(cdfid, sds_u & 0xffff, bad_start, zero_edges, r_tmp),
              Eq(-1));
  EXPECT_THAT(SDendaccess(sds_u), Eq(0));

  // Also cover 0D scalar variable in NCvar1io & NCvario (putget.c:1402-1411,
  // 1693-1701)
  const int32 sds_0d = SDcreate(sd_id, "scalar0d", DFNT_INT32, 0, nullptr);
  ASSERT_THAT(sds_0d, Ge(0));
  int32 sc_w = 777;
  int32 sc_r = 0;
  EXPECT_THAT(ncvarput1(cdfid, sds_0d & 0xffff, nullptr, &sc_w), Eq(0));
  EXPECT_THAT(ncvarget1(cdfid, sds_0d & 0xffff, nullptr, &sc_r), Eq(0));
  EXPECT_THAT(sc_r, Eq(777));
  sc_w = 888;
  EXPECT_THAT(ncvarput(cdfid, sds_0d & 0xffff, nullptr, nullptr, &sc_w), Eq(0));
  EXPECT_THAT(ncvarget(cdfid, sds_0d & 0xffff, nullptr, nullptr, &sc_r), Eq(0));
  EXPECT_THAT(sc_r, Eq(888));
  EXPECT_THAT(SDendaccess(sds_0d), Eq(0));

  // 3D array ripple counter across multiple dimensions (putget.c:1783-1786)
  int32 dims3d[] = {3, 3, 3};
  const int32 sds3d = SDcreate(sd_id, "cube", DFNT_INT32, 3, dims3d);
  ASSERT_THAT(sds3d, Ge(0));
  int32 cube_init[27] = {0};
  int32 c_start0[] = {0, 0, 0};
  int32 c_edges27[] = {3, 3, 3};
  EXPECT_THAT(SDwritedata(sds3d, c_start0, nullptr, c_edges27, cube_init),
              Eq(0));

  const long c_sub_start[] = {0, 0, 0};
  const long c_sub_edges[] = {2, 2, 2};
  int32 c_sub_data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  EXPECT_THAT(ncvarput(cdfid, SDnametoindex(sd_id, "cube"), c_sub_start,
                       c_sub_edges, c_sub_data),
              Eq(0));
  int32 c_r_sub[8] = {0};
  EXPECT_THAT(ncvarget(cdfid, SDnametoindex(sd_id, "cube"), c_sub_start,
                       c_sub_edges, c_r_sub),
              Eq(0));
  EXPECT_THAT(c_r_sub[0], Eq(1));
  EXPECT_THAT(c_r_sub[7], Eq(8));

  // Single-element I/O on HDF multidimensional SDS (putget.c:1338-1352,
  // 1422-1425)
  const long coord3d[] = {1, 1, 1};
  int32 elem_val = 9999;
  EXPECT_THAT(
      ncvarput1(cdfid, SDnametoindex(sd_id, "cube"), coord3d, &elem_val),
      Eq(0));
  int32 r_elem = 0;
  EXPECT_THAT(ncvarget1(cdfid, SDnametoindex(sd_id, "cube"), coord3d, &r_elem),
              Eq(0));
  EXPECT_THAT(r_elem, Eq(9999));
  EXPECT_THAT(SDendaccess(sds3d), Eq(0));

  // Close file and reopen in DFACC_RDWR to test read-then-write AID promotion
  // (putget.c:756-768)
  std::move(close_sd).Cancel();
  EXPECT_THAT(SDend(sd_id), Eq(0));

  const int32 sd_rw = SDstart(p.c_str(), DFACC_RDWR);
  ASSERT_THAT(sd_rw, Ge(0));
  absl::Cleanup close_rw = [sd_rw] { (void)SDend(sd_rw); };

  const int32 sds_rw = SDselect(sd_rw, SDnametoindex(sd_rw, "grid"));
  ASSERT_THAT(sds_rw, Ge(0));
  int32 r_grid[4] = {0};
  int32 rw_start[] = {0, 0};
  int32 rw_edges[] = {1, 4};
  // 1. SDreaddata attaches vp->aid with DFACC_READ
  EXPECT_THAT(SDreaddata(sds_rw, rw_start, nullptr, rw_edges, r_grid), Eq(0));
  // 2. SDwritedata sees vp->aid != FAIL but read-only, calls Hendaccess and
  // reopens DFACC_WRITE
  int32 w_grid[] = {55, 66, 77, 88};
  EXPECT_THAT(SDwritedata(sds_rw, rw_start, nullptr, rw_edges, w_grid), Eq(0));
  EXPECT_THAT(SDendaccess(sds_rw), Eq(0));
}

TEST_F(PutgetTest, ClassicNetCdfTypesAndIo) {
  const std::string file_path = GetTempFilePath("classic_types.nc");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  // Build classic "CDF\x01" NetCDF file
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
  // numrecs = 0
  write_u32(0);
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

  // Vars: 6 vars (byte, char, fixed_short, short, float, double)
  // In classic netCDF, fixed variables precede record variables in the data
  // section.
  write_u32(11);  // NC_VARIABLE
  write_u32(6);

  // Var 0: "v_byte", fixed assoc=[1], len=4, begin=512
  write_str("v_byte");
  write_u32(1);
  write_u32(1);
  write_u32(0);
  write_u32(0);
  write_u32(1);  // NC_BYTE
  write_u32(4);
  write_u32(512);

  // Var 1: "v_char", fixed assoc=[1], len=4, begin=516
  write_str("v_char");
  write_u32(1);
  write_u32(1);
  write_u32(0);
  write_u32(0);
  write_u32(2);  // NC_CHAR
  write_u32(4);
  write_u32(516);

  // Var 2: "v_fix", fixed assoc=[1], len=8, begin=520
  write_str("v_fix");
  write_u32(1);
  write_u32(1);
  write_u32(0);
  write_u32(0);
  write_u32(3);  // NC_SHORT
  write_u32(8);
  write_u32(520);

  // Record variables (begin_rec = 528, recsize = 8 + 12 + 24 = 44)
  // Var 3: "v_short", record assoc=[0, 1], len=8, begin=528
  write_str("v_short");
  write_u32(2);
  write_u32(0);
  write_u32(1);
  write_u32(0);
  write_u32(0);
  write_u32(3);  // NC_SHORT
  write_u32(8);
  write_u32(528);

  // Var 4: "v_float", record assoc=[0, 1], len=12, begin=536
  write_str("v_float");
  write_u32(2);
  write_u32(0);
  write_u32(1);
  write_u32(0);
  write_u32(0);
  write_u32(5);  // NC_FLOAT
  write_u32(12);
  write_u32(536);

  // Var 5: "v_double", record assoc=[0, 1], len=24, begin=548
  write_str("v_double");
  write_u32(2);
  write_u32(0);
  write_u32(1);
  write_u32(0);
  write_u32(0);
  write_u32(6);  // NC_DOUBLE
  write_u32(24);
  write_u32(548);

  // Pad file to 4096 bytes
  if (data.size() < 4096) {
    data.resize(4096, 0);
  }

  const int fd = open(file_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
  ASSERT_THAT(fd, Ne(-1));
  (void)write(fd, data.data(), data.size());
  close(fd);

  const int cdfid = ncopen(file_path.c_str(), NC_WRITE);
  ASSERT_THAT(cdfid, Ge(0));
  NC* handle = NC_check_id(cdfid);
  ASSERT_THAT(handle, NotNull());
  EXPECT_THAT(handle->file_type, Eq(netCDF_FILE));

  // 1. Single-element I/O via ncvarput1 / ncvarget1 across types
  // (putget.c:340-470)
  const long fix_coord0[] = {0};
  const long fix_coord1[] = {1};

  int8_t b_val = 42;
  EXPECT_THAT(ncvarput1(cdfid, 0, fix_coord0, &b_val), Eq(0));
  int8_t r_bval = 0;
  EXPECT_THAT(ncvarget1(cdfid, 0, fix_coord0, &r_bval), Eq(0));
  EXPECT_THAT(r_bval, Eq(42));

  char c_val = 'Z';
  EXPECT_THAT(ncvarput1(cdfid, 1, fix_coord0, &c_val), Eq(0));
  char r_cval = 0;
  EXPECT_THAT(ncvarget1(cdfid, 1, fix_coord0, &r_cval), Eq(0));
  EXPECT_THAT(r_cval, Eq('Z'));

  int16_t fix_val = 999;
  EXPECT_THAT(ncvarput1(cdfid, 2, fix_coord0, &fix_val), Eq(0));
  int16_t fix_val1 = -888;
  EXPECT_THAT(ncvarput1(cdfid, 2, fix_coord1, &fix_val1), Eq(0));
  int16_t r_fix = 0;
  EXPECT_THAT(ncvarget1(cdfid, 2, fix_coord0, &r_fix), Eq(0));
  EXPECT_THAT(r_fix, Eq(999));
  EXPECT_THAT(ncvarget1(cdfid, 2, fix_coord1, &r_fix), Eq(0));
  EXPECT_THAT(r_fix, Eq(-888));

  // Record variable single-element I/O
  const long rec_coord0[] = {0, 0};
  const long rec_coord1[] = {0, 1};

  int16_t s_val = 1234;
  EXPECT_THAT(ncvarput1(cdfid, 3, rec_coord0, &s_val), Eq(0));
  int16_t s_val1 = -5678;
  EXPECT_THAT(ncvarput1(cdfid, 3, rec_coord1, &s_val1), Eq(0));
  int16_t r_sval = 0;
  EXPECT_THAT(ncvarget1(cdfid, 3, rec_coord0, &r_sval), Eq(0));
  EXPECT_THAT(r_sval, Eq(1234));
  EXPECT_THAT(ncvarget1(cdfid, 3, rec_coord1, &r_sval), Eq(0));
  EXPECT_THAT(r_sval, Eq(-5678));

  float f_val = 3.1415f;
  EXPECT_THAT(ncvarput1(cdfid, 4, rec_coord0, &f_val), Eq(0));
  float r_fval = 0.0f;
  EXPECT_THAT(ncvarget1(cdfid, 4, rec_coord0, &r_fval), Eq(0));
  EXPECT_THAT(r_fval, Eq(3.1415f));

  double d_val = 2.718281828;
  EXPECT_THAT(ncvarput1(cdfid, 5, rec_coord0, &d_val), Eq(0));
  double r_dval = 0.0;
  EXPECT_THAT(ncvarget1(cdfid, 5, rec_coord0, &r_dval), Eq(0));
  EXPECT_THAT(r_dval, Eq(2.718281828));

  // 2. Multi-element hyperslab I/O with odd count (3 shorts -> count=3, rem=1)
  const long rec0_start[] = {0, 0};
  const long rec_edges[] = {1, 3};
  int16_t s_arr[3] = {11, 22, 33};
  EXPECT_THAT(ncvarput(cdfid, 3, rec0_start, rec_edges, s_arr), Eq(0));
  int16_t r_sarr[3] = {0, 0, 0};
  EXPECT_THAT(ncvarget(cdfid, 3, rec0_start, rec_edges, r_sarr), Eq(0));
  EXPECT_THAT(r_sarr[0], Eq(11));
  EXPECT_THAT(r_sarr[1], Eq(22));
  EXPECT_THAT(r_sarr[2], Eq(33));

  // 3. Record gap fill: writing record 2 fills record 1 (putget.c:230-245)
  const long rec2_start[] = {2, 0};
  int16_t s_rec2[3] = {101, 102, 103};
  EXPECT_THAT(ncvarput(cdfid, 3, rec2_start, rec_edges, s_rec2), Eq(0));
  EXPECT_THAT(handle->numrecs, Ge(3));

  // 4. Gap record write with NC_NOFILL (putget.c:231-234)
  handle->flags |= NC_NOFILL;
  const long rec4_start[] = {4, 0};
  EXPECT_THAT(ncvarput(cdfid, 3, rec4_start, rec_edges, s_rec2), Eq(0));
  EXPECT_THAT(handle->numrecs, Ge(5));

  // 5. Multi-element I/O on byte and fixed-short
  const long b_start[] = {1};
  const long b_edges[] = {2};
  int8_t b_arr[2] = {10, 20};
  EXPECT_THAT(ncvarput(cdfid, 0, b_start, b_edges, b_arr), Eq(0));
  int8_t r_barr[2] = {0, 0};
  EXPECT_THAT(ncvarget(cdfid, 0, b_start, b_edges, r_barr), Eq(0));
  EXPECT_THAT(r_barr[0], Eq(10));
  EXPECT_THAT(r_barr[1], Eq(20));

  const long fix_edges[] = {2};
  int16_t fix_arr[2] = {300, 400};
  EXPECT_THAT(ncvarput(cdfid, 2, b_start, fix_edges, fix_arr), Eq(0));
  int16_t r_fixarr[2] = {0, 0};
  EXPECT_THAT(ncvarget(cdfid, 2, b_start, fix_edges, r_fixarr), Eq(0));
  EXPECT_THAT(r_fixarr[0], Eq(300));
  EXPECT_THAT(r_fixarr[1], Eq(400));

  // 6. Multi-element I/O on float and double record variables
  float f_arr[3] = {1.1f, 2.2f, 3.3f};
  EXPECT_THAT(ncvarput(cdfid, 4, rec0_start, rec_edges, f_arr), Eq(0));
  float r_farr[3] = {0};
  EXPECT_THAT(ncvarget(cdfid, 4, rec0_start, rec_edges, r_farr), Eq(0));
  EXPECT_THAT(r_farr[0], Eq(1.1f));

  double d_arr[3] = {10.5, 20.5, 30.5};
  EXPECT_THAT(ncvarput(cdfid, 5, rec0_start, rec_edges, d_arr), Eq(0));
  double r_darr[3] = {0};
  EXPECT_THAT(ncvarget(cdfid, 5, rec0_start, rec_edges, r_darr), Eq(0));
  EXPECT_THAT(r_darr[0], Eq(10.5));

  // 7. Validation checks: out of bounds, invalid varid, NC_INDEF
  const long bad_edges99[] = {1, 99};
  EXPECT_THAT(ncvarput(cdfid, 3, rec0_start, bad_edges99, s_arr), Eq(-1));
  EXPECT_THAT(ncvarput1(cdfid, 99, rec0_start, &s_val), Eq(-1));
  EXPECT_THAT(ncvarget1(cdfid, 99, rec0_start, &s_val), Eq(-1));
  EXPECT_THAT(ncvarput(cdfid, 99, rec0_start, rec_edges, s_arr), Eq(-1));
  EXPECT_THAT(ncvarget(cdfid, 99, rec0_start, rec_edges, s_arr), Eq(-1));

  handle->flags |= NC_INDEF;
  EXPECT_THAT(ncvarput1(cdfid, 3, rec_coord0, &s_val), Eq(-1));
  EXPECT_THAT(ncvarput(cdfid, 3, rec0_start, rec_edges, s_arr), Eq(-1));
  handle->flags &= ~NC_INDEF;

  EXPECT_THAT(ncclose(cdfid), Eq(0));
}

TEST_F(PutgetTest, CoordCheckAndVarIoValidationAndErrorBranches) {
  const std::string p = GetTempFilePath("coordck_errors.hdf");
  absl::Cleanup rem = [&p] { std::filesystem::remove(p); };

  const int32 sd_id = SDstart(p.c_str(), DFACC_CREATE);
  ASSERT_THAT(sd_id, Ge(0));
  absl::Cleanup close_sd = [sd_id] { EXPECT_EQ(SUCCEED, SDend(sd_id)); };

  int32 u_dims[] = {SD_UNLIMITED, 4};
  const int32 sds_u = SDcreate(sd_id, "u_var", DFNT_INT32, 2, u_dims);
  ASSERT_THAT(sds_u, Ge(0));

  int32 f_dims[] = {4};
  const int32 sds_f = SDcreate(sd_id, "f_var", DFNT_INT32, 1, f_dims);
  ASSERT_THAT(sds_f, Ge(0));

  const int cdfid = sd_id & 0xffff;
  NC* handle = NC_check_id(cdfid);
  ASSERT_THAT(handle, NotNull());

  // 1. Negative coordinate on unlimited variable (putget.c:98)
  const long neg_start[] = {-1, 0};
  const long edges[] = {1, 4};
  int32 vals[4] = {1, 2, 3, 4};
  EXPECT_THAT(
      ncvarput(cdfid, SDnametoindex(sd_id, "u_var"), neg_start, edges, vals),
      Eq(-1));

  // 2. Out of bounds coordinate on fixed variable (putget.c:115)
  const long oob_start[] = {99};
  const long f_edges[] = {1};
  int32 val = 42;
  EXPECT_THAT(
      ncvarput(cdfid, SDnametoindex(sd_id, "f_var"), oob_start, f_edges, &val),
      Eq(-1));

  // 3. Read beyond max numrecs in file via ncvarget (putget.c:158)
  const long read_oob_start[] = {99, 0};
  int32 r_vals[4] = {0};
  EXPECT_THAT(ncvarget(cdfid, SDnametoindex(sd_id, "u_var"), read_oob_start,
                       edges, r_vals),
              Eq(-1));

  // 4. Fill values on unlimited variable with custom FillValueAttr
  // (putget.c:179)
  const int32 sds_u_fill = SDcreate(sd_id, "u_fill_var", DFNT_INT32, 2, u_dims);
  ASSERT_THAT(sds_u_fill, Ge(0));
  int32 custom_fill = -7777;
  EXPECT_THAT(SDsetfillvalue(sds_u_fill, &custom_fill), Eq(0));
  // Write record 2 directly so records 0, 1, 2 are filled with custom_fill
  const long rec2_start[] = {2, 0};
  EXPECT_THAT(ncvarput(cdfid, SDnametoindex(sd_id, "u_fill_var"), rec2_start,
                       edges, vals),
              Eq(0));
  int32 r_rec0[4] = {0};
  const long rec0_start[] = {0, 0};
  EXPECT_THAT(ncvarget(cdfid, SDnametoindex(sd_id, "u_fill_var"), rec0_start,
                       edges, r_rec0),
              Eq(0));
  EXPECT_THAT(r_rec0[0], Eq(custom_fill));
  EXPECT_THAT(SDendaccess(sds_u_fill), Eq(0));

  // 5. Unwritten IS_RECVAR in RDONLY mode fails hdf_get_vp_aid(DFACC_WRITE) in
  // NCcoordck (putget.c:165)
  const int32 sds_u_fail = SDcreate(sd_id, "u_fail_var", DFNT_INT32, 2, u_dims);
  ASSERT_THAT(sds_u_fail, Ge(0));
  const int saved_mode = handle->hdf_mode;
  handle->hdf_mode = DFACC_RDONLY;
  const long rec1_start[] = {1, 0};
  EXPECT_THAT(ncvarput(cdfid, SDnametoindex(sd_id, "u_fail_var"), rec1_start,
                       edges, vals),
              Eq(-1));
  handle->hdf_mode = saved_mode;
  EXPECT_THAT(SDendaccess(sds_u_fail), Eq(0));

  // 6. Ripple counter error in NCvario on SDreaddata when reading across gap
  // (putget.c:1754) sds_u has 0 records written. Write record 2 on sds_u so
  // handle->numrecs becomes 3.
  EXPECT_THAT(
      ncvarput(cdfid, SDnametoindex(sd_id, "u_var"), rec2_start, edges, vals),
      Eq(0));
  // Create another unlimited var where only record 0 is written
  const int32 sds_u_part = SDcreate(sd_id, "u_part", DFNT_INT32, 2, u_dims);
  ASSERT_THAT(sds_u_part, Ge(0));
  int32 p_start0[] = {0, 0};
  int32 p_edges1[] = {1, 4};
  EXPECT_THAT(SDwritedata(sds_u_part, p_start0, nullptr, p_edges1, vals),
              Eq(0));
  // Now attempt to SDreaddata 2 records ({0, 0}, {2, 4}): record 0 succeeds,
  // record 1 fails in NCcoordck
  int32 p_edges2[] = {2, 4};
  int32 r_part2[8] = {0};
  EXPECT_THAT(SDreaddata(sds_u_part, p_start0, nullptr, p_edges2, r_part2),
              Eq(FAIL));
  EXPECT_THAT(SDendaccess(sds_u_part), Eq(0));

  // 7. vars == nullptr checks (putget.c:1396)
  NC_array* saved_vars = handle->vars;
  handle->vars = nullptr;
  EXPECT_THAT(ncvarput1(cdfid, 0, rec0_start, &val), Eq(-1));
  EXPECT_THAT(ncvarget1(cdfid, 0, rec0_start, &val), Eq(-1));
  EXPECT_THAT(ncvarput(cdfid, 0, rec0_start, edges, vals), Eq(-1));
  EXPECT_THAT(ncvarget(cdfid, 0, rec0_start, edges, vals), Eq(-1));
  EXPECT_THAT(NC_fill_buffer(handle, 0, edges, vals), Eq(-1));
  handle->vars = saved_vars;

  // 8. ncvarput1 on NC_NOWRITE file (putget.c:1447-1448)
  EXPECT_THAT(SDendaccess(sds_u), Eq(0));
  EXPECT_THAT(SDendaccess(sds_f), Eq(0));
  std::move(close_sd).Cancel();
  EXPECT_THAT(SDend(sd_id), Eq(0));

  const int ro_id = ncopen(p.c_str(), NC_NOWRITE);
  ASSERT_THAT(ro_id, Ge(0));
  absl::Cleanup close_ro = [ro_id] { (void)ncclose(ro_id); };
  EXPECT_THAT(ncvarput1(ro_id, 0, rec0_start, &val), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EPERM));
}

TEST_F(PutgetTest, HdfGetDataAndVpAidBranches) {
  const std::string p = GetTempFilePath("get_data_aid.hdf");
  absl::Cleanup rem = [&p] { std::filesystem::remove(p); };

  const int32 sd_id = SDstart(p.c_str(), DFACC_CREATE);
  ASSERT_THAT(sd_id, Ge(0));
  absl::Cleanup close_sd = [sd_id] { EXPECT_EQ(SUCCEED, SDend(sd_id)); };

  const int cdfid = sd_id & 0xffff;
  NC* handle = NC_check_id(cdfid);
  ASSERT_THAT(handle, NotNull());

  int32 dims[] = {4};
  const int32 sds = SDcreate(sd_id, "var1", DFNT_INT32, 1, dims);
  ASSERT_THAT(sds, Ge(0));
  NC_var* vp = SDIget_var(handle, sds);
  ASSERT_THAT(vp, NotNull());

  // 1. NULL handle / NULL vp (putget.c:569-575)
  EXPECT_THAT(hdf_get_data(nullptr, vp), Eq(DFREF_NONE));
  EXPECT_THAT(hdf_get_data(handle, nullptr), Eq(DFREF_NONE));

  // 2. DFTAG_SDS branch (putget.c:583-584)
  const uint16 saved_tag = vp->data_tag;
  const int32 saved_ref = vp->data_ref;
  vp->data_tag = DFTAG_SDS;
  vp->data_ref = 777;
  EXPECT_THAT(hdf_get_data(handle, vp), Eq(777));
  vp->data_tag = saved_tag;
  vp->data_ref = saved_ref;

  // 3. Existing DATA_TAG inside variable's Vgroup (putget.c:609, 613-614)
  int32 start[] = {0};
  int32 edges[] = {4};
  int32 vals[] = {10, 20, 30, 40};
  EXPECT_THAT(SDwritedata(sds, start, nullptr, edges, vals), Eq(0));
  const int32 written_ref = vp->data_ref;
  ASSERT_THAT(written_ref, Ne(0));
  EXPECT_THAT(SDendaccess(sds), Eq(0));

  // Reopen read-only so Vgroup is loaded from disk
  std::move(close_sd).Cancel();
  EXPECT_THAT(SDend(sd_id), Eq(0));

  const int32 sd_ro = SDstart(p.c_str(), DFACC_READ);
  ASSERT_THAT(sd_ro, Ge(0));
  absl::Cleanup close_ro = [sd_ro] { (void)SDend(sd_ro); };

  const int32 sds_ro = SDselect(sd_ro, 0);
  ASSERT_THAT(sds_ro, Ge(0));
  NC* handle_ro = NC_check_id(sd_ro & 0xffff);
  ASSERT_THAT(handle_ro, NotNull());
  NC_var* vp_ro = SDIget_var(handle_ro, sds_ro);
  ASSERT_THAT(vp_ro, NotNull());

  // Clear vp_ro->data_ref to force hdf_get_data to scan the Vgroup for DATA_TAG
  vp_ro->data_ref = 0;
  EXPECT_THAT(hdf_get_data(handle_ro, vp_ro), Eq(written_ref));
  EXPECT_THAT(SDendaccess(sds_ro), Eq(0));

  // Reopen in DFACC_RDWR to test linked block heuristics and error paths
  std::move(close_ro).Cancel();
  EXPECT_THAT(SDend(sd_ro), Eq(0));

  const int32 sd_rw = SDstart(p.c_str(), DFACC_RDWR);
  ASSERT_THAT(sd_rw, Ge(0));
  absl::Cleanup close_rw = [sd_rw] { (void)SDend(sd_rw); };
  NC* handle_rw = NC_check_id(sd_rw & 0xffff);
  ASSERT_THAT(handle_rw, NotNull());

  // 4. Linked block heuristics (putget.c:665, 669)
  int32 u_dims[] = {SD_UNLIMITED, 4};
  const int32 sds_u1 = SDcreate(sd_rw, "u_heur1", DFNT_INT32, 2, u_dims);
  ASSERT_THAT(sds_u1, Ge(0));
  NC_var* vp_u1 = SDIget_var(handle_rw, sds_u1);
  ASSERT_THAT(vp_u1, NotNull());
  const size_t saved_len1 = vp_u1->len;
  vp_u1->len = MAX_BLOCK_SIZE + 16;
  EXPECT_THAT(hdf_get_data(handle_rw, vp_u1), Ne(DFREF_NONE));
  vp_u1->len = saved_len1;
  EXPECT_THAT(SDendaccess(sds_u1), Eq(0));

  const int32 sds_u2 = SDcreate(sd_rw, "u_heur2", DFNT_INT32, 2, u_dims);
  ASSERT_THAT(sds_u2, Ge(0));
  NC_var* vp_u2 = SDIget_var(handle_rw, sds_u2);
  ASSERT_THAT(vp_u2, NotNull());
  const size_t saved_len2 = vp_u2->len;
  vp_u2->len = (MAX_BLOCK_SIZE / BLOCK_MULT) + 16;
  EXPECT_THAT(hdf_get_data(handle_rw, vp_u2), Ne(DFREF_NONE));
  vp_u2->len = saved_len2;
  EXPECT_THAT(SDendaccess(sds_u2), Eq(0));

  // 5. HLcreate failure on invalid negative block size (putget.c:675-676)
  const int32 sds_u3 = SDcreate(sd_rw, "u_heur3", DFNT_INT32, 2, u_dims);
  ASSERT_THAT(sds_u3, Ge(0));
  NC_var* vp_u3 = SDIget_var(handle_rw, sds_u3);
  ASSERT_THAT(vp_u3, NotNull());
  vp_u3->block_size = -5;
  EXPECT_THAT(hdf_get_data(handle_rw, vp_u3), Eq(DFREF_NONE));
  vp_u3->block_size = -1;
  vp_u3->aid = FAIL;
  EXPECT_THAT(SDendaccess(sds_u3), Eq(0));

  // 6. Invalid cached vp->aid in hdf_get_vp_aid (putget.c:758-759)
  const int32 sds_rw = SDselect(sd_rw, 0);
  ASSERT_THAT(sds_rw, Ge(0));
  NC_var* vp_rw = SDIget_var(handle_rw, sds_rw);
  ASSERT_THAT(vp_rw, NotNull());
  vp_rw->aid = 999999;
  EXPECT_THAT(hdf_get_vp_aid(handle_rw, vp_rw, DFACC_WRITE), Eq(FAIL));
  vp_rw->aid = FAIL;

  // 7. Vattach failure in hdf_get_data (putget.c:689-690, 716-717)
  const int32 sds_badvg = SDcreate(sd_rw, "bad_vg", DFNT_INT32, 1, dims);
  ASSERT_THAT(sds_badvg, Ge(0));
  NC_var* vp_bvg = SDIget_var(handle_rw, sds_badvg);
  ASSERT_THAT(vp_bvg, NotNull());
  const int32 saved_vgid = vp_bvg->vgid;
  vp_bvg->vgid = 999999;
  EXPECT_THAT(hdf_get_data(handle_rw, vp_bvg), Eq(DFREF_NONE));
  vp_bvg->vgid = saved_vgid;
  EXPECT_THAT(SDendaccess(sds_badvg), Eq(0));

  // 8. Hsetlength branches in hdf_get_vp_aid (putget.c:785-793)
  const int32 sds_sl = SDcreate(sd_rw, "setlen_var", DFNT_INT32, 1, dims);
  ASSERT_THAT(sds_sl, Ge(0));
  NC_var* vp_sl = SDIget_var(handle_rw, sds_sl);
  ASSERT_THAT(vp_sl, NotNull());
  // Success path
  vp_sl->set_length = TRUE;
  vp_sl->len = 16;
  EXPECT_THAT(hdf_get_vp_aid(handle_rw, vp_sl, DFACC_WRITE), Ne(FAIL));
  EXPECT_THAT(vp_sl->set_length, Eq(FALSE));
  Hendaccess(vp_sl->aid);
  vp_sl->aid = FAIL;
  // Failure path (negative length)
  vp_sl->set_length = TRUE;
  vp_sl->len = -10;
  EXPECT_THAT(hdf_get_vp_aid(handle_rw, vp_sl, DFACC_WRITE), Eq(FAIL));
  vp_sl->len = 16;
  vp_sl->set_length = FALSE;
  EXPECT_THAT(SDendaccess(sds_sl), Eq(0));

  EXPECT_THAT(SDendaccess(sds_rw), Eq(0));
}

TEST_F(PutgetTest, HdfXdrNCvdataAndV1dataBranches) {
  const std::string p = GetTempFilePath("xdr_ncvdata_branches.hdf");
  absl::Cleanup rem = [&p] { std::filesystem::remove(p); };

  const int32 sd_id = SDstart(p.c_str(), DFACC_CREATE);
  ASSERT_THAT(sd_id, Ge(0));
  absl::Cleanup close_sd = [sd_id] { EXPECT_EQ(SUCCEED, SDend(sd_id)); };

  const int cdfid = sd_id & 0xffff;
  NC* handle = NC_check_id(cdfid);
  ASSERT_THAT(handle, NotNull());

  int32 dims4[] = {4};

  // 1. Template compressed SDS reads with and without _FillValue
  // (putget.c:879-885)
  const int32 sds_comp_nofv =
      SDcreate(sd_id, "comp_nofv", DFNT_INT32, 1, dims4);
  ASSERT_THAT(sds_comp_nofv, Ge(0));
  comp_info c_info{};
  c_info.deflate_setting.level = 6;
  EXPECT_THAT(SDsetcompress(sds_comp_nofv, COMP_CODE_DEFLATE, &c_info), Eq(0));
  // Do not write data -> elem_length <= 0 and isspecial == SPECIAL_COMP
  int32 start0[] = {0};
  int32 edges4[] = {4};
  int32 r_comp[4] = {0};
  EXPECT_THAT(SDreaddata(sds_comp_nofv, start0, nullptr, edges4, r_comp),
              Eq(0));
  EXPECT_THAT(r_comp[0], Eq(FILL_LONG));
  EXPECT_THAT(SDendaccess(sds_comp_nofv), Eq(0));

  // Template compressed with explicit _FillValue
  const int32 sds_comp_fv = SDcreate(sd_id, "comp_fv", DFNT_INT32, 1, dims4);
  ASSERT_THAT(sds_comp_fv, Ge(0));
  int32 cust_fill = -8888;
  EXPECT_THAT(SDsetfillvalue(sds_comp_fv, &cust_fill), Eq(0));
  EXPECT_THAT(SDsetcompress(sds_comp_fv, COMP_CODE_DEFLATE, &c_info), Eq(0));
  std::fill(std::begin(r_comp), std::end(r_comp), 0);
  EXPECT_THAT(SDreaddata(sds_comp_fv, start0, nullptr, edges4, r_comp), Eq(0));
  EXPECT_THAT(r_comp[0], Eq(cust_fill));
  EXPECT_THAT(SDendaccess(sds_comp_fv), Eq(0));

  // 2. Unwritten SDS with FillValueAttr in RDONLY mode (putget.c:847)
  const int32 sds_unw_fv = SDcreate(sd_id, "unw_fv", DFNT_INT32, 1, dims4);
  ASSERT_THAT(sds_unw_fv, Ge(0));
  int32 fv_val = 12345;
  EXPECT_THAT(SDsetfillvalue(sds_unw_fv, &fv_val), Eq(0));
  const int saved_mode = handle->hdf_mode;
  handle->hdf_mode = DFACC_RDONLY;
  int32 r_unw[4] = {0};
  EXPECT_THAT(SDreaddata(sds_unw_fv, start0, nullptr, edges4, r_unw), Eq(0));
  EXPECT_THAT(r_unw[0], Eq(fv_val));
  handle->hdf_mode = saved_mode;
  EXPECT_THAT(SDendaccess(sds_unw_fv), Eq(0));

  // 3. Non-converting (convert == 0) partial lead write and tail write
  // (putget.c:1045, 1059, 1307)
  int32 dims8[] = {8};
  const int32 sds_i8_lead =
      SDcreate(sd_id, "i8_lead", DFNT_INT32 | DFNT_NATIVE, 1, dims8);
  ASSERT_THAT(sds_i8_lead, Ge(0));
  int32 fv_lead = -77;
  EXPECT_THAT(SDsetfillvalue(sds_i8_lead, &fv_lead), Eq(0));
  int32 start4[] = {4};
  int32 edges4_part[] = {4};
  int32 nat_data[] = {11, 22, 33, 44};
  EXPECT_THAT(SDwritedata(sds_i8_lead, start4, nullptr, edges4_part, nat_data),
              Eq(0));
  EXPECT_THAT(SDendaccess(sds_i8_lead), Eq(0));

  const int32 sds_i8_tail =
      SDcreate(sd_id, "i8_tail", DFNT_INT32 | DFNT_NATIVE, 1, dims8);
  ASSERT_THAT(sds_i8_tail, Ge(0));
  EXPECT_THAT(SDwritedata(sds_i8_tail, start0, nullptr, edges4_part, nat_data),
              Eq(0));
  EXPECT_THAT(SDendaccess(sds_i8_tail), Eq(0));

  // 4. SD_NOFILL lead seek when where > 0 and elem_length <= 0 (putget.c:1077)
  int32 u_dims[] = {SD_UNLIMITED, 4};
  EXPECT_THAT(SDsetfillmode(sd_id, SD_NOFILL), Eq(SD_FILL));
  const int32 sds_u_nf =
      SDcreate(sd_id, "u_nofill_seek", DFNT_INT32, 2, u_dims);
  ASSERT_THAT(sds_u_nf, Ge(0));
  int32 u_start_mid[] = {0, 2};
  int32 u_edges_mid[] = {1, 2};
  int32 u_mid_data[] = {55, 66};
  EXPECT_THAT(
      SDwritedata(sds_u_nf, u_start_mid, nullptr, u_edges_mid, u_mid_data),
      Eq(0));
  EXPECT_THAT(SDsetfillmode(sd_id, SD_FILL), Eq(SD_NOFILL));
  EXPECT_THAT(SDendaccess(sds_u_nf), Eq(0));

  // 5. Short/truncated Hread (putget.c:1133-1134, 1163-1164)
  const int32 sds_short_c1 =
      SDcreate(sd_id, "short_read_c1", DFNT_INT32, 1, dims4);
  ASSERT_THAT(sds_short_c1, Ge(0));
  NC_var* vp_s1 = SDIget_var(handle, sds_short_c1);
  ASSERT_THAT(vp_s1, NotNull());
  const uint16 ref1 = Hnewref(handle->hdf_file);
  uint8_t tiny[2] = {0, 0};
  ASSERT_THAT(Hputelement(handle->hdf_file, DATA_TAG, ref1, tiny, 2), Ne(FAIL));
  vp_s1->data_ref = ref1;
  int32 short_r[4] = {0};
  EXPECT_THAT(SDreaddata(sds_short_c1, start0, nullptr, edges4, short_r),
              Eq(FAIL));
  EXPECT_THAT(SDendaccess(sds_short_c1), Eq(0));

  const int32 sds_short_c0 =
      SDcreate(sd_id, "short_read_c0", DFNT_INT8 | DFNT_NATIVE, 1, dims4);
  ASSERT_THAT(sds_short_c0, Ge(0));
  NC_var* vp_s0 = SDIget_var(handle, sds_short_c0);
  ASSERT_THAT(vp_s0, NotNull());
  const uint16 ref0 = Hnewref(handle->hdf_file);
  ASSERT_THAT(Hputelement(handle->hdf_file, DATA_TAG, ref0, tiny, 2), Ne(FAIL));
  vp_s0->data_ref = ref0;
  uint8_t short_r0[4] = {0};
  EXPECT_THAT(SDreaddata(sds_short_c0, start0, nullptr, edges4, short_r0),
              Eq(FAIL));
  EXPECT_THAT(SDendaccess(sds_short_c0), Eq(0));

  // 6. Non-appendable DATA_TAG element write overrun (putget.c:1087-1088,
  // 1211-1212, 1234-1235)
  const int32 sds_trunc_c1 = SDcreate(sd_id, "trunc_c1", DFNT_INT32, 1, dims4);
  ASSERT_THAT(sds_trunc_c1, Ge(0));
  NC_var* vp_tr1 = SDIget_var(handle, sds_trunc_c1);
  ASSERT_THAT(vp_tr1, NotNull());
  const uint16 ref_tr1 = Hnewref(handle->hdf_file);
  ASSERT_THAT(Hputelement(handle->hdf_file, DATA_TAG, ref_tr1, tiny, 2),
              Ne(FAIL));
  vp_tr1->data_ref = ref_tr1;
  int32 tr_data[4] = {1, 2, 3, 4};
  int32 start2[] = {2};
  int32 edges2_tr[] = {2};
  EXPECT_THAT(SDwritedata(sds_trunc_c1, start2, nullptr, edges2_tr, tr_data),
              Eq(FAIL));
  EXPECT_THAT(SDwritedata(sds_trunc_c1, start0, nullptr, edges4, tr_data),
              Eq(FAIL));
  EXPECT_THAT(SDendaccess(sds_trunc_c1), Eq(0));

  const int32 sds_trunc_c0 =
      SDcreate(sd_id, "trunc_c0", DFNT_INT32 | DFNT_NATIVE, 1, dims4);
  ASSERT_THAT(sds_trunc_c0, Ge(0));
  NC_var* vp_tr0 = SDIget_var(handle, sds_trunc_c0);
  ASSERT_THAT(vp_tr0, NotNull());
  const uint16 ref_tr0 = Hnewref(handle->hdf_file);
  ASSERT_THAT(Hputelement(handle->hdf_file, DATA_TAG, ref_tr0, tiny, 2),
              Ne(FAIL));
  vp_tr0->data_ref = ref_tr0;
  EXPECT_THAT(SDwritedata(sds_trunc_c0, start0, nullptr, edges4, tr_data),
              Eq(FAIL));
  EXPECT_THAT(SDendaccess(sds_trunc_c0), Eq(0));

  // 7. NCsimplerecio hdf_xdr_NCvdata failure (putget.c:1638)
  int32 u_dims_gap[] = {SD_UNLIMITED};
  const int32 sds_simp_err =
      SDcreate(sd_id, "simp_err", DFNT_INT32, 1, u_dims_gap);
  ASSERT_THAT(sds_simp_err, Ge(0));
  NC_var* vp_simp = SDIget_var(handle, sds_simp_err);
  ASSERT_THAT(vp_simp, NotNull());
  const uint16 ref_simp = Hnewref(handle->hdf_file);
  ASSERT_THAT(Hputelement(handle->hdf_file, DATA_TAG, ref_simp, tiny, 2),
              Ne(FAIL));
  vp_simp->data_ref = ref_simp;
  vp_simp->numrecs = 1;
  const long simp_start[] = {0};
  const long simp_edges[] = {1};
  int32 simp_rval = 0;
  EXPECT_THAT(ncvarget(cdfid, sds_simp_err & 0xffff, simp_start, simp_edges,
                       &simp_rval),
              Eq(-1));
  EXPECT_THAT(SDendaccess(sds_simp_err), Eq(0));

  // 8. Invalid HDFtype & unwritten 0D scalar errors (putget.c:856, 893-894,
  // 1344-1345, 1408, 1424, 1638, 1690, 1698) 0D scalar
  const int32 sds_sc = SDcreate(sd_id, "bad_sc", DFNT_INT32, 0, nullptr);
  ASSERT_THAT(sds_sc, Ge(0));
  NC_var* vp_sc = SDIget_var(handle, sds_sc);
  ASSERT_THAT(vp_sc, NotNull());
  const int32 saved_type_sc = vp_sc->HDFtype;
  vp_sc->HDFtype = 99999;
  int32 sc_val = 42;
  EXPECT_THAT(ncvarput1(cdfid, sds_sc & 0xffff, nullptr, &sc_val), Eq(-1));
  EXPECT_THAT(ncvarput(cdfid, sds_sc & 0xffff, nullptr, nullptr, &sc_val),
              Eq(-1));
  vp_sc->HDFtype = saved_type_sc;

  // Unwritten 0D scalar read without fill value in normal mode (lines 856,
  // 1408, 1698)
  int32 sc_rval = 0;
  EXPECT_THAT(ncvarget1(cdfid, sds_sc & 0xffff, nullptr, &sc_rval), Eq(-1));
  EXPECT_THAT(ncvarget(cdfid, sds_sc & 0xffff, nullptr, nullptr, &sc_rval),
              Eq(-1));
  EXPECT_THAT(SDendaccess(sds_sc), Eq(0));

  // 9. hdf_get_data record block-size heuristics (putget.c:665, 669)
  int32 dims_med[] = {SD_UNLIMITED, 2000};
  const int32 sds_med = SDcreate(sd_id, "blk_med", DFNT_INT8, 2, dims_med);
  ASSERT_THAT(sds_med, Ge(0));
  NC_var* vp_med = SDIget_var(handle, sds_med);
  ASSERT_THAT(vp_med, NotNull());
  EXPECT_THAT(hdf_get_data(handle, vp_med), Gt(0));
  EXPECT_THAT(SDendaccess(sds_med), Eq(0));

  int32 dims_big[] = {SD_UNLIMITED, 70000};
  const int32 sds_big = SDcreate(sd_id, "blk_big", DFNT_INT8, 2, dims_big);
  ASSERT_THAT(sds_big, Ge(0));
  NC_var* vp_big = SDIget_var(handle, sds_big);
  ASSERT_THAT(vp_big, NotNull());
  EXPECT_THAT(hdf_get_data(handle, vp_big), Gt(0));
  EXPECT_THAT(SDendaccess(sds_big), Eq(0));

  // 1D fixed
  const int32 sds_fx = SDcreate(sd_id, "bad_fx", DFNT_INT32, 1, dims4);
  ASSERT_THAT(sds_fx, Ge(0));
  NC_var* vp_fx = SDIget_var(handle, sds_fx);
  ASSERT_THAT(vp_fx, NotNull());
  const int32 saved_type_fx = vp_fx->HDFtype;
  vp_fx->HDFtype = 99999;
  const long coord0[] = {0};
  EXPECT_THAT(ncvarput1(cdfid, sds_fx & 0xffff, coord0, &sc_val), Eq(-1));
  vp_fx->HDFtype = saved_type_fx;
  EXPECT_THAT(SDendaccess(sds_fx), Eq(0));

  // 1D unlimited simplerecio
  int32 u_dims1[] = {SD_UNLIMITED};
  const int32 sds_u1d = SDcreate(sd_id, "bad_u1d", DFNT_INT32, 1, u_dims1);
  ASSERT_THAT(sds_u1d, Ge(0));
  int32 u_start1[] = {0};
  int32 u_edges1[] = {1};
  int32 u_val = 123;
  EXPECT_THAT(SDwritedata(sds_u1d, u_start1, nullptr, u_edges1, &u_val), Eq(0));
  NC_var* vp_u1d = SDIget_var(handle, sds_u1d);
  ASSERT_THAT(vp_u1d, NotNull());
  const int32 saved_type_u = vp_u1d->HDFtype;
  vp_u1d->HDFtype = 99999;
  const long start_nc1[] = {0};
  const long edges_nc1[] = {1};
  EXPECT_THAT(ncvarput(cdfid, sds_u1d & 0xffff, start_nc1, edges_nc1, &u_val),
              Eq(-1));
  vp_u1d->HDFtype = saved_type_u;
  EXPECT_THAT(SDendaccess(sds_u1d), Eq(0));
}

TEST_F(PutgetTest, ClassicNetCdfXdrBoundariesAndErrors) {
  const std::string p = GetTempFilePath("classic_boundaries.nc");
  absl::Cleanup rem = [&p] { std::filesystem::remove(p); };

  NetCdfBuilder b;
  b.WriteI32(0x43444601);  // Magic
  b.WriteI32(0);           // numrecs = 0
  // Dims: 1 rec dim, 1 fixed dim of size 4
  b.WriteI32(10);
  b.WriteI32(2);
  b.WriteStr("rec");
  b.WriteI32(0);
  b.WriteStr("dim4");
  b.WriteI32(4);
  b.WriteI32(0);
  b.WriteI32(0);  // attrs empty

  // 3 vars:
  // Var 0: NC_BYTE fixed, len = 4, begin = 512 (at EOF!)
  // Var 1: NC_SHORT fixed, len = 4, begin = 512 (at EOF!)
  // Var 2: NC_LONG record, len = 4, begin = 256
  b.WriteI32(11);
  b.WriteI32(3);
  b.WriteStr("v_b");
  b.WriteI32(1);
  b.WriteI32(1);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(1);
  b.WriteI32(4);
  b.WriteI32(512);

  b.WriteStr("v_s");
  b.WriteI32(1);
  b.WriteI32(1);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(3);
  b.WriteI32(4);
  b.WriteI32(512);

  b.WriteStr("v_rec");
  b.WriteI32(1);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(0);
  b.WriteI32(4);
  b.WriteI32(4);
  b.WriteI32(256);

  b.PadTo(512);
  b.WriteToFile(p);

  const int ro_id = ncopen(p.c_str(), NC_NOWRITE);
  ASSERT_THAT(ro_id, Ge(0));
  absl::Cleanup close_ro = [ro_id] { (void)ncclose(ro_id); };

  const int rw_id = ncopen(p.c_str(), NC_WRITE);
  ASSERT_THAT(rw_id, Ge(0));
  absl::Cleanup close_rw = [rw_id] { (void)ncclose(rw_id); };

  NC* ro_h = NC_check_id(ro_id);
  NC* rw_h = NC_check_id(rw_id);
  ASSERT_THAT(ro_h, NotNull());
  ASSERT_THAT(rw_h, NotNull());

  // 1. Read at EOF triggers hdf_xdr_opaque failure -> memset(buf, 0, 4)
  // (putget.c:360-361, 406-407)
  const long coord0[] = {0};
  uint8_t r_byte = 99;
  EXPECT_THAT(ncvarget1(ro_id, 0, coord0, &r_byte), Eq(0));
  EXPECT_THAT(r_byte, Eq(0));

  int16_t r_short = 99;
  EXPECT_THAT(ncvarget1(ro_id, 1, coord0, &r_short), Eq(0));
  EXPECT_THAT(r_short, Eq(0));

  // 2. Swap rw_h->xdrs with ro_h->xdrs (O_RDONLY biobuf, write fails)
  // (putget.c:61, 243-244, 380, 422, 1428, 1501, 1509, 1521, 1528, 1769)
  XDR* orig_xdrs = rw_h->xdrs;
  rw_h->xdrs = ro_h->xdrs;

  uint8_t w_b = 55;
  EXPECT_THAT(ncvarput1(rw_id, 0, coord0, &w_b), Eq(-1));
  int16_t w_s = 555;
  EXPECT_THAT(ncvarput1(rw_id, 1, coord0, &w_s), Eq(-1));

  const long start1[] = {1};
  const long edges2[] = {2};
  uint8_t w_barr[] = {1, 2};
  EXPECT_THAT(ncvarput(rw_id, 0, start1, edges2, w_barr), Eq(-1));
  const long start0[] = {0};
  const long edges4[] = {4};
  uint8_t w_b4[] = {1, 2, 3, 4};
  EXPECT_THAT(ncvarput(rw_id, 0, start0, edges4, w_b4), Eq(-1));
  const long edges3[] = {3};
  uint8_t w_b3[] = {1, 2, 3};
  EXPECT_THAT(ncvarput(rw_id, 0, start0, edges3, w_b3), Eq(-1));

  int16_t w_sarr[] = {10, 20};
  EXPECT_THAT(ncvarput(rw_id, 1, start1, edges2, w_sarr), Eq(-1));
  int16_t w_s4[] = {10, 20, 30, 40};
  EXPECT_THAT(ncvarput(rw_id, 1, start0, edges4, w_s4), Eq(-1));
  const long edges1_s[] = {1};
  int16_t w_s1 = 10;
  EXPECT_THAT(ncvarput(rw_id, 1, start0, edges1_s, &w_s1), Eq(-1));

  // Gap write on record variable: NCfillrecord fails on O_RDONLY write
  const long rec1_start[] = {1};
  const long edges1[] = {1};
  int32_t rec_val = 999;
  EXPECT_THAT(ncvarput(rw_id, 2, rec1_start, edges1, &rec_val), Eq(-1));

  rw_h->xdrs = orig_xdrs;

  // 3. Clear xdrs->x_private to simulate hdf_xdr_setpos failure
  // (putget.c:238-239, 251, 453, 1493, 1647)
  auto* orig_priv = rw_h->xdrs->x_private;
  rw_h->xdrs->x_private = nullptr;

  EXPECT_THAT(ncvarput1(rw_id, 0, coord0, &w_b), Eq(-1));
  EXPECT_THAT(ncvarput1(rw_id, 2, rec1_start, &rec_val), Eq(-1));
  EXPECT_THAT(ncvarput(rw_id, 0, start0, edges4, w_b4), Eq(-1));
  EXPECT_THAT(ncvarput(rw_id, 2, rec1_start, edges1, &rec_val), Eq(-1));

  const long saved_numrecs = rw_h->numrecs;
  rw_h->numrecs = 1;
  EXPECT_THAT(ncvarput(rw_id, 2, coord0, edges1, &rec_val), Eq(-1));
  rw_h->numrecs = saved_numrecs;

  // With NC_NOFILL | NC_NSYNC
  rw_h->flags |= (NC_NOFILL | NC_NSYNC);
  const long rec2_start[] = {2};
  EXPECT_THAT(ncvarput(rw_id, 2, rec2_start, edges1, &rec_val), Eq(-1));
  rw_h->flags &= ~(NC_NOFILL | NC_NSYNC);

  rw_h->xdrs->x_private = orig_priv;

  // 4. Invalid NC_UNSPECIFIED type (putget.c:468, 470, 1547)
  NC_var* vp_b = reinterpret_cast<NC_var**>(ro_h->vars->values)[0];
  const nc_type saved_type = vp_b->type;
  vp_b->type = NC_UNSPECIFIED;
  EXPECT_THAT(ncvarget1(ro_id, 0, coord0, &r_byte), Eq(-1));
  EXPECT_THAT(ncvarget(ro_id, 0, start0, edges4, w_b4), Eq(-1));
  vp_b->type = saved_type;
}

// TODO(schwehr): Uncovered lines in putget.c (~90.1% covered) are defensive
// OOM paths, external/compressed write allocation failures, and XDR errors.

}  // namespace
}  // namespace hdf4
