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
// Tests for NetCDF-2 / HDF file lifecycle and open-file table management
// (mfhdf/src/file.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/mfhdf/src/file.c

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf4/hdf/src/hdf.h"

// clang-format off
#include "third_party/hdf4/mfhdf/src/nc_priv.h"
#include "third_party/hdf4/mfhdf/src/hdf_xdr_priv.h"
// clang-format on

namespace hdf4 {
namespace {

using ::testing::Eq;
using ::testing::Ge;
using ::testing::Gt;
using ::testing::IsNull;
using ::testing::Ne;
using ::testing::NotNull;

class FileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_ncopts_ = ncopts;
    saved_ncerr_ = ncerr;
    ncopts = 0;
    ncerr = NC_NOERR;
  }

  void TearDown() override {
    SDPfreebuf();
    ResetCdflistToDefault();
    ncopts = saved_ncopts_;
    ncerr = saved_ncerr_;
    HEclear();
  }

  int saved_ncopts_ = 0;
  int saved_ncerr_ = 0;

  std::string GetTempFilePath(const std::string& name) {
    return (std::filesystem::path(::testing::TempDir()) / name).string();
  }

  void ResetCdflistToDefault() {
    (void)NC_reset_maxopenfiles(H4_MAX_NC_OPEN);
    const std::string dummy = GetTempFilePath("reset_cdflist_dummy.hdf");
    const int id = nccreate(dummy.c_str(), NC_CLOBBER);
    if (id >= 0) {
      (void)ncclose(id);
    }
    std::filesystem::remove(dummy);
  }

  static void CreateEmptyNetCdf(const std::string& path) {
    std::vector<uint8_t> data(32, 0);
    data[0] = 'C';
    data[1] = 'D';
    data[2] = 'F';
    data[3] = 0x01;
    const int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd != -1) {
      (void)write(fd, data.data(), data.size());
      close(fd);
    }
  }

  static void CreateNetCdfWithVars(const std::string& path) {
    std::vector<uint8_t> data;
    auto write_u32 = [&data](uint32_t val) {
      data.push_back((val >> 24) & 0xff);
      data.push_back((val >> 16) & 0xff);
      data.push_back((val >> 8) & 0xff);
      data.push_back(val & 0xff);
    };
    auto write_str = [&](std::string_view s) {
      const uint32_t len = static_cast<uint32_t>(s.size() + 1);
      const uint32_t padded = (len + 3) & ~3u;
      write_u32(padded);
      for (char c : s) {
        data.push_back(static_cast<uint8_t>(c));
      }
      data.push_back(0);
      while (data.size() % 4 != 0) {
        data.push_back(0);
      }
    };

    // 1. Magic
    write_u32(0x43444601);
    // 2. numrecs
    write_u32(2);
    // 3. dims: type = NC_DIMENSION (10), count = 2
    write_u32(10);
    write_u32(2);
    // dim 0: fixed size 4
    write_str("x");
    write_u32(4);
    // dim 1: unlimited (size = 0)
    write_str("rec");
    write_u32(0);

    // 4. attrs: empty (NC_UNSPECIFIED = 0, count = 0)
    write_u32(0);
    write_u32(0);

    // 5. vars: type = NC_VARIABLE (11), count = 2
    write_u32(11);
    write_u32(2);

    // var 0: "v0", assoc = {0}, attrs = empty, type = NC_LONG (4), len = 16,
    // begin = 128
    write_str("v0");
    write_u32(1);    // assoc count
    write_u32(0);    // assoc[0] = dim 0
    write_u32(0);    // attrs type
    write_u32(0);    // attrs count
    write_u32(4);    // type = NC_LONG
    write_u32(16);   // len = 16
    write_u32(128);  // begin = 128

    // var 1: "v1", assoc = {1, 0}, attrs = empty, type = NC_LONG (4), len = 16,
    // begin = 144
    write_str("v1");
    write_u32(2);    // assoc count
    write_u32(1);    // assoc[0] = dim 1 (unlimited)
    write_u32(0);    // assoc[1] = dim 0 (fixed)
    write_u32(0);    // attrs type
    write_u32(0);    // attrs count
    write_u32(4);    // type = NC_LONG
    write_u32(16);   // len = 16
    write_u32(144);  // begin = 144

    // Pad to 256 bytes
    if (data.size() < 256) {
      data.resize(256, 0);
    }

    const int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd != -1) {
      (void)write(fd, data.data(), data.size());
      close(fd);
    }
  }
};

TEST_F(FileTest, OpenFilesTableManagement) {
  // Query limits when _cdfs is NULL
  EXPECT_THAT(NC_get_maxopenfiles(), Eq(H4_MAX_NC_OPEN));
  EXPECT_THAT(NC_get_systemlimit(), Gt(0));
  EXPECT_THAT(NC_get_numopencdfs(), Eq(0));

  // Invalid request
  EXPECT_THAT(NC_reset_maxopenfiles(-1), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EINVAL));

  // Request 0 allocates default max
  EXPECT_THAT(NC_reset_maxopenfiles(0), Eq(H4_MAX_NC_OPEN));

  // Reset by open and close
  const std::string file0_path = GetTempFilePath("open_mgmt_0.hdf");
  absl::Cleanup remove0 = [&file0_path] {
    std::filesystem::remove(file0_path);
  };
  const int id0 = nccreate(file0_path.c_str(), NC_CLOBBER);
  ASSERT_THAT(id0, Ge(0));
  EXPECT_THAT(ncclose(id0), Eq(0));

  // Now set custom size 4
  EXPECT_THAT(NC_reset_maxopenfiles(4), Eq(4));
  EXPECT_THAT(NC_get_maxopenfiles(), Eq(4));

  const std::string file1_path = GetTempFilePath("open_mgmt_1.hdf");
  absl::Cleanup remove1 = [&file1_path] {
    std::filesystem::remove(file1_path);
  };
  const int id_a = nccreate(file0_path.c_str(), NC_CLOBBER);
  ASSERT_THAT(id_a, Eq(0));
  const int id_b = nccreate(file1_path.c_str(), NC_CLOBBER);
  ASSERT_THAT(id_b, Eq(1));

  // Close id_a so slot 0 is NULL and slot 1 is occupied
  EXPECT_THAT(ncclose(id_a), Eq(0));
  EXPECT_THAT(NC_get_numopencdfs(), Eq(1));
  EXPECT_THAT(NC_get_maxopenfiles(), Eq(4));

  // Slot reuse: slot 0 is NULL, slot 1 is occupied (file.c:281)
  const std::string reuse_path = GetTempFilePath("open_mgmt_reuse.hdf");
  absl::Cleanup rem_reuse = [&reuse_path] {
    std::filesystem::remove(reuse_path);
  };
  const int id_reused = nccreate(reuse_path.c_str(), NC_CLOBBER);
  EXPECT_THAT(id_reused, Eq(0));
  EXPECT_THAT(ncclose(id_reused), Eq(0));

  // Request max <= current opened (1) fails and preserves size
  EXPECT_THAT(NC_reset_maxopenfiles(1), Eq(4));
  EXPECT_THAT(ncerr, Eq(NC_EINVAL));

  // Request max exceeding system limit caps to system limit and compacts slot 1
  // into slot 0
  const int sys_limit = NC_get_systemlimit();
  EXPECT_THAT(NC_reset_maxopenfiles(sys_limit + 1000), Eq(sys_limit));
  EXPECT_THAT(NC_get_maxopenfiles(), Eq(sys_limit));

  // Shrink back to H4_MAX_NC_OPEN
  EXPECT_THAT(NC_reset_maxopenfiles(H4_MAX_NC_OPEN), Eq(H4_MAX_NC_OPEN));

  // Close remaining file: triggers ncreset_cdflist()
  EXPECT_THAT(ncclose(0), Eq(0));
  EXPECT_THAT(NC_get_numopencdfs(), Eq(0));

  // Auto-grow branch in NC_open when cdfid reaches max_NC_open
  EXPECT_THAT(NC_reset_maxopenfiles(2), Eq(2));
  const std::string grow_a = GetTempFilePath("grow_a.hdf");
  const std::string grow_b = GetTempFilePath("grow_b.hdf");
  const std::string grow_c = GetTempFilePath("grow_c.hdf");
  absl::Cleanup rem_g = [&grow_a, &grow_b, &grow_c] {
    std::filesystem::remove(grow_a);
    std::filesystem::remove(grow_b);
    std::filesystem::remove(grow_c);
  };

  const int g0 = nccreate(grow_a.c_str(), NC_CLOBBER);
  ASSERT_THAT(g0, Eq(0));
  const int g1 = nccreate(grow_b.c_str(), NC_CLOBBER);
  ASSERT_THAT(g1, Eq(1));
  // Third file triggers auto-grow to sys_limit
  const int g2 = nccreate(grow_c.c_str(), NC_CLOBBER);
  ASSERT_THAT(g2, Eq(2));
  EXPECT_THAT(NC_get_maxopenfiles(), Eq(sys_limit));

  EXPECT_THAT(ncclose(g2), Eq(0));
  EXPECT_THAT(ncclose(g1), Eq(0));
  EXPECT_THAT(NC_reset_maxopenfiles(H4_MAX_NC_OPEN), Eq(H4_MAX_NC_OPEN));
  EXPECT_THAT(ncclose(g0), Eq(0));
}

TEST_F(FileTest, CheckIdAndIndefineValidation) {
  EXPECT_THAT(NC_check_id(-1), IsNull());
  EXPECT_THAT(ncerr, Eq(NC_EBADID));

  EXPECT_THAT(NC_check_id(999), IsNull());
  EXPECT_THAT(ncerr, Eq(NC_EBADID));

  EXPECT_THAT(ncendef(-1), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EBADID));

  EXPECT_THAT(NC_indefine(-1, TRUE), Eq(FALSE));
  EXPECT_THAT(ncerr, Eq(NC_EBADID));

  EXPECT_THAT(NC_indefine(-1, FALSE), Eq(FALSE));

  const std::string file_path = GetTempFilePath("check_id.hdf");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  const int cdfid = nccreate(file_path.c_str(), NC_CLOBBER);
  ASSERT_THAT(cdfid, Ge(0));
  absl::Cleanup close_cdf = [cdfid] { (void)ncclose(cdfid); };

  // While in define mode
  EXPECT_THAT(NC_indefine(cdfid, TRUE), Ne(FALSE));
  EXPECT_THAT(NC_check_id(cdfid), NotNull());

  // End define mode
  ASSERT_THAT(ncendef(cdfid), Eq(0));
  EXPECT_THAT(NC_indefine(cdfid, FALSE), Eq(FALSE));
  EXPECT_THAT(NC_indefine(cdfid, TRUE), Eq(FALSE));
  EXPECT_THAT(ncerr, Eq(NC_ENOTINDEFINE));

  // Calling ncendef while not in define mode fails (file.c:706)
  EXPECT_THAT(ncendef(cdfid), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_ENOTINDEFINE));
}

TEST_F(FileTest, CreateAndOpenErrorHandling) {
  const std::string file_path = GetTempFilePath("create_open_err.hdf");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  // nccreate without NC_CREAT mode returns -1
  EXPECT_THAT(nccreate(file_path.c_str(), NC_WRITE), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EINVAL));

  // ncopen with NC_CREAT mode returns -1
  EXPECT_THAT(ncopen(file_path.c_str(), NC_CLOBBER), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EINVAL));

  // nccreate to invalid directory path fails and covers remove() error branch
  EXPECT_THAT(nccreate("/nonexistent_dir_xyz/impossible.hdf", NC_CLOBBER),
              Eq(-1));

  // Create valid file
  const int cdfid = nccreate(file_path.c_str(), NC_CLOBBER);
  ASSERT_THAT(cdfid, Ge(0));
  ASSERT_THAT(ncclose(cdfid), Eq(0));

  // nccreate with NC_NOCLOBBER on existing HDF file fails
  EXPECT_THAT(nccreate(file_path.c_str(), NC_NOCLOBBER), Eq(-1));
}

TEST_F(FileTest, DcpyDirectBufferCopy) {
  const std::string src_path = GetTempFilePath("dcpy_src.dat");
  const std::string dst_path = GetTempFilePath("dcpy_dst.dat");
  absl::Cleanup remove_files = [&src_path, &dst_path] {
    std::filesystem::remove(src_path);
    std::filesystem::remove(dst_path);
  };

  // Create 20,000 bytes source file (> 8192 bytes so loop runs multiple times)
  constexpr size_t kDataSize = 20000;
  std::vector<uint8_t> test_bytes(kDataSize);
  for (size_t i = 0; i < kDataSize; ++i) {
    test_bytes[i] = static_cast<uint8_t>(i & 0xff);
  }

  {
    XDR xdr_write{};
    const int fd_w = NCxdrfile_create(&xdr_write, src_path.c_str(), NC_CLOBBER);
    ASSERT_THAT(fd_w, Ge(0));
    EXPECT_THAT(
        hdf_xdr_putbytes(&xdr_write, reinterpret_cast<char*>(test_bytes.data()),
                         kDataSize),
        Eq(TRUE));
    hdf_xdr_destroy(&xdr_write);
  }

  // Copy via NC_dcpy
  {
    XDR src_xdr{};
    const int fd_src = NCxdrfile_create(&src_xdr, src_path.c_str(), NC_NOWRITE);
    ASSERT_THAT(fd_src, Ge(0));
    absl::Cleanup dest_src = [&src_xdr] { hdf_xdr_destroy(&src_xdr); };

    XDR dst_xdr{};
    const int fd_dst = NCxdrfile_create(&dst_xdr, dst_path.c_str(), NC_CLOBBER);
    ASSERT_THAT(fd_dst, Ge(0));
    absl::Cleanup dest_dst = [&dst_xdr] { hdf_xdr_destroy(&dst_xdr); };

    EXPECT_THAT(NC_dcpy(&dst_xdr, &src_xdr, kDataSize), Eq(TRUE));
  }

  // Verify copied bytes match
  std::vector<uint8_t> read_bytes(kDataSize, 0);
  {
    XDR dst_verify{};
    const int fd_v =
        NCxdrfile_create(&dst_verify, dst_path.c_str(), NC_NOWRITE);
    ASSERT_THAT(fd_v, Ge(0));
    EXPECT_THAT(
        hdf_xdr_getbytes(&dst_verify,
                         reinterpret_cast<char*>(read_bytes.data()), kDataSize),
        Eq(TRUE));
    hdf_xdr_destroy(&dst_verify);
  }
  EXPECT_THAT(read_bytes, Eq(test_bytes));

  // Error paths in NC_dcpy: read past EOF and write to read-only stream
  {
    XDR src_xdr{};
    const int fd_src = NCxdrfile_create(&src_xdr, src_path.c_str(), NC_NOWRITE);
    ASSERT_THAT(fd_src, Ge(0));
    absl::Cleanup dest_src = [&src_xdr] { hdf_xdr_destroy(&src_xdr); };

    XDR dst_xdr{};
    const int fd_dst = NCxdrfile_create(&dst_xdr, dst_path.c_str(), NC_CLOBBER);
    ASSERT_THAT(fd_dst, Ge(0));
    absl::Cleanup dest_dst = [&dst_xdr] { hdf_xdr_destroy(&dst_xdr); };

    // Requesting 50,000 bytes from a 20,000 byte file fails on getbytes
    EXPECT_THAT(NC_dcpy(&dst_xdr, &src_xdr, 50000), Eq(FALSE));
    EXPECT_THAT(ncerr, Eq(NC_EXDR));

    XDR data_src{};
    const int fd_ds = NCxdrfile_create(&data_src, src_path.c_str(), NC_NOWRITE);
    ASSERT_THAT(fd_ds, Ge(0));
    absl::Cleanup dest_ds = [&data_src] { hdf_xdr_destroy(&data_src); };

    // Writing to read-only destination fails on putbytes (large chunk > 8192
    // covers file.c:515)
    EXPECT_THAT(NC_dcpy(&src_xdr, &data_src, 10000), Eq(FALSE));
    EXPECT_THAT(ncerr, Eq(NC_EXDR));

    // Reset data_src position to beginning
    EXPECT_THAT(hdf_xdr_setpos(&data_src, 0), Eq(TRUE));

    // Writing to read-only destination fails on putbytes (small remainder <=
    // 8192 covers file.c:522)
    EXPECT_THAT(NC_dcpy(&src_xdr, &data_src, 10), Eq(FALSE));
    EXPECT_THAT(ncerr, Eq(NC_EXDR));
  }
}

TEST_F(FileTest, XdrFileCreateModesAndErrors) {
  const std::string path = GetTempFilePath("xdrfile_test.dat");
  absl::Cleanup remove_file = [&path] { std::filesystem::remove(path); };

  XDR xdrs{};
  // NC_CLOBBER
  int fd = NCxdrfile_create(&xdrs, path.c_str(), NC_CLOBBER);
  ASSERT_THAT(fd, Ge(0));
  hdf_xdr_destroy(&xdrs);

  // NC_WRITE
  fd = NCxdrfile_create(&xdrs, path.c_str(), NC_WRITE);
  ASSERT_THAT(fd, Ge(0));
  hdf_xdr_destroy(&xdrs);

  // NC_NOWRITE
  fd = NCxdrfile_create(&xdrs, path.c_str(), NC_NOWRITE);
  ASSERT_THAT(fd, Ge(0));
  hdf_xdr_destroy(&xdrs);

  // NC_NOCLOBBER on existing file fails
  fd = NCxdrfile_create(&xdrs, path.c_str(), NC_NOCLOBBER);
  EXPECT_THAT(fd, Eq(-1));

  // Invalid mode (0x02 is not NC_NOCLOBBER(0x0f), NC_CLOBBER(0x0b),
  // NC_WRITE(0x01), or NC_NOWRITE(0x00))
  fd = NCxdrfile_create(&xdrs, path.c_str(), 0x02);
  EXPECT_THAT(fd, Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EINVAL));

  // Missing file with NC_NOWRITE fails
  fd = NCxdrfile_create(&xdrs, "/nonexistent_dir_xyz/missing.dat", NC_NOWRITE);
  EXPECT_THAT(fd, Eq(-1));
}

TEST_F(FileTest, SetFillMode) {
  EXPECT_THAT(ncsetfill(-1, NC_FILL), Eq(-1));

  const std::string file_path = GetTempFilePath("setfill_test.hdf");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  {
    const int cdfid = nccreate(file_path.c_str(), NC_CLOBBER);
    ASSERT_THAT(cdfid, Ge(0));
    ASSERT_THAT(ncclose(cdfid), Eq(0));
  }

  // Read-only file rejects ncsetfill
  const int ro_id = ncopen(file_path.c_str(), NC_NOWRITE);
  ASSERT_THAT(ro_id, Ge(0));
  EXPECT_THAT(ncsetfill(ro_id, NC_NOFILL), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EPERM));
  EXPECT_THAT(ncclose(ro_id), Eq(0));

  // Writable file
  const int rw_id = ncopen(file_path.c_str(), NC_WRITE);
  ASSERT_THAT(rw_id, Ge(0));
  absl::Cleanup close_rw = [rw_id] { (void)ncclose(rw_id); };

  // Invalid fillmode
  EXPECT_THAT(ncsetfill(rw_id, 999), Eq(-1));
  EXPECT_THAT(ncerr, Eq(NC_EINVAL));

  // Initial state is NC_FILL; setting NC_NOFILL returns NC_FILL
  EXPECT_THAT(ncsetfill(rw_id, NC_NOFILL), Eq(NC_FILL));

  // Setting back to NC_FILL while NC_HDIRTY is set triggers xdr_cdf sync
  NC* handle = NC_check_id(rw_id);
  ASSERT_THAT(handle, NotNull());
  handle->flags |= NC_HDIRTY;
  EXPECT_THAT(ncsetfill(rw_id, NC_FILL), Eq(NC_NOFILL));
  EXPECT_THAT(handle->flags & NC_HDIRTY, Eq(0u));

  // Setting back to NC_FILL while NC_NDIRTY is set triggers xdr_numrecs sync
  EXPECT_THAT(ncsetfill(rw_id, NC_NOFILL), Eq(NC_FILL));
  handle->flags |= NC_NDIRTY;
  EXPECT_THAT(ncsetfill(rw_id, NC_FILL), Eq(NC_NOFILL));

  // Test netCDF file fillmode toggle clearing NC_NDIRTY (covers file.c:803)
  const std::string nc_fill_path = GetTempFilePath("setfill_netcdf.nc");
  absl::Cleanup remove_nc_fill = [&nc_fill_path] {
    std::filesystem::remove(nc_fill_path);
  };
  CreateEmptyNetCdf(nc_fill_path);

  const int nc_id = ncopen(nc_fill_path.c_str(), NC_WRITE);
  ASSERT_THAT(nc_id, Ge(0));
  NC* nc_h = NC_check_id(nc_id);
  ASSERT_THAT(nc_h, NotNull());

  nc_h->flags |= (NC_NOFILL | NC_NDIRTY);
  EXPECT_THAT(ncsetfill(nc_id, NC_FILL), Eq(NC_NOFILL));
  EXPECT_THAT(nc_h->flags & NC_NDIRTY, Eq(0u));
  EXPECT_THAT(ncclose(nc_id), Eq(0));
}

TEST_F(FileTest, AbortAndCloseHdfFile) {
  EXPECT_THAT(ncabort(-1), Eq(-1));
  EXPECT_THAT(ncclose(-1), Eq(-1));

  const std::string file_path = GetTempFilePath("abort_hdf.hdf");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  // nccreate in define mode: ncabort removes the file
  const int cdfid0 = nccreate(file_path.c_str(), NC_CLOBBER);
  ASSERT_THAT(cdfid0, Ge(0));
  EXPECT_THAT(ncabort(cdfid0), Eq(0));
  EXPECT_FALSE(std::filesystem::exists(file_path));

  // nccreate in define mode: ncclose automatically calls NC_endef and saves
  // file
  const int cdfid1 = nccreate(file_path.c_str(), NC_CLOBBER);
  ASSERT_THAT(cdfid1, Ge(0));
  EXPECT_THAT(ncclose(cdfid1), Eq(0));
  EXPECT_TRUE(std::filesystem::exists(file_path));

  // Reopen in data mode: test dirty flags during ncabort and ncclose
  const int rw_id0 = ncopen(file_path.c_str(), NC_WRITE);
  ASSERT_THAT(rw_id0, Ge(0));
  NC* handle0 = NC_check_id(rw_id0);
  ASSERT_THAT(handle0, NotNull());
  handle0->flags |= NC_HDIRTY;
  EXPECT_THAT(ncabort(rw_id0), Eq(0));

  const int rw_id1 = ncopen(file_path.c_str(), NC_WRITE);
  ASSERT_THAT(rw_id1, Ge(0));
  NC* handle1 = NC_check_id(rw_id1);
  ASSERT_THAT(handle1, NotNull());
  handle1->flags |= NC_NDIRTY;
  EXPECT_THAT(ncclose(rw_id1), Eq(0));

  // Test ncabort with NC_NDIRTY on HDF file (file.c:408-411)
  const int rw_id2 = ncopen(file_path.c_str(), NC_WRITE);
  ASSERT_THAT(rw_id2, Ge(0));
  NC* handle2 = NC_check_id(rw_id2);
  ASSERT_THAT(handle2, NotNull());
  handle2->flags |= NC_NDIRTY;
  EXPECT_THAT(ncabort(rw_id2), Eq(0));

  // Test ncclose with NC_HDIRTY on HDF file (file.c:732-734)
  const int rw_id3 = ncopen(file_path.c_str(), NC_WRITE);
  ASSERT_THAT(rw_id3, Ge(0));
  NC* handle3 = NC_check_id(rw_id3);
  ASSERT_THAT(handle3, NotNull());
  handle3->flags |= NC_HDIRTY;
  EXPECT_THAT(ncclose(rw_id3), Eq(0));

  // Test remove(path) != 0 failure in ncabort on HDF file (file.c:428)
  const std::string bad_rem = GetTempFilePath("bad_rem.hdf");
  absl::Cleanup rem_bad = [&bad_rem] { std::filesystem::remove(bad_rem); };
  const int id_bad = nccreate(bad_rem.c_str(), NC_CLOBBER);
  ASSERT_THAT(id_bad, Ge(0));
  NC* h_bad = NC_check_id(id_bad);
  ASSERT_THAT(h_bad, NotNull());
  (void)strncpy(h_bad->path, "/nonexistent_dir_xyz/no.hdf", FILENAME_MAX);
  EXPECT_THAT(ncabort(id_bad), Eq(0));

  // Test ncabort and ncclose on 32-byte netCDF file with NC_NDIRTY (file.c:408,
  // 411, 736)
  const std::string nc_abort_path = GetTempFilePath("abort_nc_dirty.nc");
  absl::Cleanup rem_nc_ab = [&nc_abort_path] {
    std::filesystem::remove(nc_abort_path);
  };
  CreateEmptyNetCdf(nc_abort_path);

  // ncabort with NC_NDIRTY
  const int nc_ab_id = ncopen(nc_abort_path.c_str(), NC_WRITE);
  ASSERT_THAT(nc_ab_id, Ge(0));
  NC* nc_ab_h = NC_check_id(nc_ab_id);
  ASSERT_THAT(nc_ab_h, NotNull());
  nc_ab_h->flags |= NC_NDIRTY;
  EXPECT_THAT(ncabort(nc_ab_id), Eq(0));

  // ncclose with NC_NDIRTY
  const int nc_cl_id = ncopen(nc_abort_path.c_str(), NC_WRITE);
  ASSERT_THAT(nc_cl_id, Ge(0));
  NC* nc_cl_h = NC_check_id(nc_cl_id);
  ASSERT_THAT(nc_cl_h, NotNull());
  nc_cl_h->flags |= NC_NDIRTY;
  EXPECT_THAT(ncclose(nc_cl_id), Eq(0));
}

TEST_F(FileTest, NetCdfRedefineAbort) {
  const std::string stash_file = GetTempFilePath("redef_abort_stash.nc");
  const std::string redef_file = GetTempFilePath("redef_abort_work.nc");
  absl::Cleanup remove_files = [&stash_file, &redef_file] {
    std::filesystem::remove(stash_file);
    std::filesystem::remove(redef_file);
  };

  CreateEmptyNetCdf(stash_file);
  CreateEmptyNetCdf(redef_file);

  // Open redef_file first (slot 0) and stash_file second (slot 1) so
  // redef_handle->redefid == _ncdf - 1 (file.c:389)
  const int redef_id = ncopen(redef_file.c_str(), NC_WRITE);
  ASSERT_THAT(redef_id, Eq(0));

  const int stash_id = ncopen(stash_file.c_str(), NC_WRITE);
  ASSERT_THAT(stash_id, Eq(1));

  NC* redef_handle = NC_check_id(redef_id);
  ASSERT_THAT(redef_handle, NotNull());
  redef_handle->flags |= NC_INDEF;
  redef_handle->redefid = stash_id;

  // Set path to invalid directory so remove(path) != 0 covers file.c:422
  (void)strncpy(redef_handle->path, "/nonexistent_dir_xyz/no.nc", FILENAME_MAX);

  // ncabort on redefinition frees stash, decrements _ncdf, hits remove failure,
  // closes cleanly
  EXPECT_THAT(ncabort(redef_id), Eq(0));
}

TEST_F(FileTest, NetCdfBeginsAndEndefCreat) {
  // ncendef on netCDF file with vars == NULL (covers file.c:461, 584-585, 597)
  const std::string no_vars_file = GetTempFilePath("netcdf_novars.nc");
  absl::Cleanup rem_novars = [&no_vars_file] {
    std::filesystem::remove(no_vars_file);
  };
  CreateEmptyNetCdf(no_vars_file);
  const int id_novars = ncopen(no_vars_file.c_str(), NC_WRITE);
  ASSERT_THAT(id_novars, Ge(0));
  NC* h_novars = NC_check_id(id_novars);
  ASSERT_THAT(h_novars, NotNull());
  h_novars->flags |= (NC_CREAT | NC_INDEF);

  // Success with vars == NULL (file.c:461, 597)
  EXPECT_THAT(ncendef(id_novars), Eq(0));
  EXPECT_THAT(ncclose(id_novars), Eq(0));

  const std::string file_path = GetTempFilePath("netcdf_creat.nc");
  absl::Cleanup remove_file = [&file_path] {
    std::filesystem::remove(file_path);
  };

  CreateEmptyNetCdf(file_path);

  const int cdfid = ncopen(file_path.c_str(), NC_WRITE);
  ASSERT_THAT(cdfid, Ge(0));
  absl::Cleanup close_cdf = [cdfid] { (void)ncclose(cdfid); };

  NC* handle = NC_check_id(cdfid);
  ASSERT_THAT(handle, NotNull());
  handle->flags |= (NC_CREAT | NC_INDEF | NC_NOFILL);

  // Define 1 unlimited dim and 1 fixed dim
  const int dim_unlim = ncdimdef(cdfid, "time", NC_UNLIMITED);
  ASSERT_THAT(dim_unlim, Ge(0));
  const int dim_fix = ncdimdef(cdfid, "lat", 4);
  ASSERT_THAT(dim_fix, Ge(0));

  // Define 1 fixed var and 1 record var (covers last != NULL && handle->recsize
  // == last->len)
  const int fix_dims[] = {dim_fix};
  const int v_fix = ncvardef(cdfid, "lat_var", NC_LONG, 1, fix_dims);
  ASSERT_THAT(v_fix, Ge(0));

  const int rec_dims[] = {dim_unlim, dim_fix};
  const int v_rec = ncvardef(cdfid, "data_rec", NC_LONG, 2, rec_dims);
  ASSERT_THAT(v_rec, Ge(0));

  // ncendef computes begins, packs single record size, encodes header
  EXPECT_THAT(ncendef(cdfid), Eq(0));
  EXPECT_THAT(handle->recsize, Eq(4 * sizeof(int32_t)));

  // Define mode with fixed and record vars without NC_NOFILL: xdr_NC_fill fails
  // on type==0 and calls ncabort (file.c:614, 615, 726)
  const std::string abort_nc = GetTempFilePath("netcdf_abort_creat.nc");
  absl::Cleanup remove_abort = [&abort_nc] {
    std::filesystem::remove(abort_nc);
  };
  CreateEmptyNetCdf(abort_nc);

  const int cdfid_ab = ncopen(abort_nc.c_str(), NC_WRITE);
  ASSERT_THAT(cdfid_ab, Ge(0));
  NC* handle_ab = NC_check_id(cdfid_ab);
  ASSERT_THAT(handle_ab, NotNull());
  handle_ab->flags |= (NC_CREAT | NC_INDEF);  // Without NC_NOFILL

  const int d_u = ncdimdef(cdfid_ab, "rec", NC_UNLIMITED);
  const int d_f = ncdimdef(cdfid_ab, "x", 4);
  const int fix_d[] = {d_f};
  (void)ncvardef(cdfid_ab, "f0", NC_LONG, 1, fix_d);
  (void)ncvardef(cdfid_ab, "f1", NC_LONG, 1, fix_d);
  const int r_dims[] = {d_u, d_f};
  (void)ncvardef(cdfid_ab, "r0", NC_LONG, 2, r_dims);

  // Set f1 to invalid type so f0 succeeds xdr_NC_fill (file.c:614) and f1 fails
  // (file.c:615)
  reinterpret_cast<NC_var**>(handle_ab->vars->values)[1]->type =
      static_cast<nc_type>(99);

  // ncclose in define mode calls NC_endef, which fails at xdr_NC_fill and
  // aborts (file.c:726)
  EXPECT_THAT(ncclose(cdfid_ab), Eq(0));
}

TEST_F(FileTest, NetCdfRedefineVcpyReccpyAndRollback) {
  const std::string stash_path = GetTempFilePath("redef_vcpy_stash.nc");
  const std::string work_path = GetTempFilePath("redef_vcpy_work.nc");
  absl::Cleanup remove_files = [&stash_path, &work_path] {
    std::filesystem::remove(stash_path);
    std::filesystem::remove(work_path);
  };

  CreateNetCdfWithVars(stash_path);
  CreateEmptyNetCdf(work_path);

  // Open work_path first (slot 0) and stash_path second (slot 1) so
  // work->redefid == _ncdf - 1 (file.c:681)
  const int work_id = ncopen(work_path.c_str(), NC_WRITE);
  ASSERT_THAT(work_id, Eq(0));

  const int stash_id = ncopen(stash_path.c_str(), NC_WRITE);
  ASSERT_THAT(stash_id, Eq(1));

  NC* work = NC_check_id(work_id);
  ASSERT_THAT(work, NotNull());
  work->flags |= (NC_INDEF | NC_NOFILL);  // Redefine mode (no NC_CREAT)
  work->redefid = stash_id;

  // Define matching dims and vars on work, plus extra vars (file.c:613, 632)
  const int d0 = ncdimdef(work_id, "x", 4);
  const int d1 = ncdimdef(work_id, "rec", NC_UNLIMITED);
  const int fix_dims[] = {d0};
  const int rec_dims[] = {d1, d0};
  (void)ncvardef(work_id, "v0", NC_LONG, 1, fix_dims);
  (void)ncvardef(work_id, "v_extra_fix", NC_LONG, 1, fix_dims);
  (void)ncvardef(work_id, "v1", NC_LONG, 2, rec_dims);
  (void)ncvardef(work_id, "v_extra_rec", NC_LONG, 2, rec_dims);

  // ncendef runs NC_vcpy and NC_reccpy, replaces stash with work via rename
  EXPECT_THAT(ncendef(work_id), Eq(0));
  EXPECT_THAT(ncclose(work_id), Eq(0));

  // Rollback branch when rename fails (file.c:660)
  const std::string stash_path2 = GetTempFilePath("redef_roll_stash.nc");
  const std::string work_path2 = GetTempFilePath("redef_roll_work.nc");
  absl::Cleanup remove_files2 = [&stash_path2, &work_path2] {
    std::filesystem::remove(stash_path2);
    std::filesystem::remove(work_path2);
  };

  CreateNetCdfWithVars(stash_path2);
  CreateEmptyNetCdf(work_path2);

  // Open work_path2 first (slot 0) and stash_path2 second (slot 1) so
  // work2->redefid == _ncdf - 1
  const int work_id2 = ncopen(work_path2.c_str(), NC_WRITE);
  ASSERT_THAT(work_id2, Eq(0));

  const int stash_id2 = ncopen(stash_path2.c_str(), NC_WRITE);
  ASSERT_THAT(stash_id2, Eq(1));

  NC* work2 = NC_check_id(work_id2);
  ASSERT_THAT(work2, NotNull());
  work2->flags |= (NC_INDEF | NC_NOFILL);
  work2->redefid = stash_id2;

  const int d0_2 = ncdimdef(work_id2, "x", 4);
  const int d1_2 = ncdimdef(work_id2, "rec", NC_UNLIMITED);
  const int f_dims2[] = {d0_2};
  const int r_dims2[] = {d1_2, d0_2};
  (void)ncvardef(work_id2, "v0", NC_LONG, 1, f_dims2);
  (void)ncvardef(work_id2, "v1", NC_LONG, 2, r_dims2);

  NC* stash2 = NC_check_id(stash_id2);
  ASSERT_THAT(stash2, NotNull());
  {
    const std::string nonexistent_path =
        (std::filesystem::path(::testing::TempDir()) / "nonexistent_dir_xyz" /
         "impossible.nc")
            .string();
    absl::Cleanup restore_path = [stash2, &stash_path2] {
      (void)std::strncpy(stash2->path, stash_path2.c_str(), FILENAME_MAX);
    };
    // Point stash path to nonexistent directory so rename fails.
    (void)std::strncpy(stash2->path, nonexistent_path.c_str(), FILENAME_MAX);

    // ncendef rename fails, restores stash into slot work_id2.
    EXPECT_THAT(ncendef(work_id2), Eq(-1));
  }
  EXPECT_THAT(ncclose(work_id2), Eq(0));
}

TEST_F(FileTest, NetCdfRedefineVcpyAndReccpyErrors) {
  // 1. NC_vcpy failure at hdf_xdr_setpos (file.c:540, 541, 609)
  {
    const std::string stash_path = GetTempFilePath("redef_err1_stash.nc");
    const std::string work_path = GetTempFilePath("redef_err1_work.nc");
    absl::Cleanup rem = [&stash_path, &work_path] {
      std::filesystem::remove(stash_path);
      std::filesystem::remove(work_path);
    };
    CreateNetCdfWithVars(stash_path);
    CreateEmptyNetCdf(work_path);

    const int work_id = ncopen(work_path.c_str(), NC_WRITE);
    ASSERT_THAT(work_id, Ge(0));
    const int stash_id = ncopen(stash_path.c_str(), NC_WRITE);
    ASSERT_THAT(stash_id, Ge(0));

    NC* work = NC_check_id(work_id);
    NC* stash = NC_check_id(stash_id);
    ASSERT_THAT(work, NotNull());
    ASSERT_THAT(stash, NotNull());

    work->flags |= (NC_INDEF | NC_NOFILL);
    work->redefid = stash_id;

    const int d0 = ncdimdef(work_id, "x", 4);
    const int fix_dims[] = {d0};
    (void)ncvardef(work_id, "v0", NC_LONG, 1, fix_dims);

    {
      auto saved_priv = stash->xdrs->x_private;
      absl::Cleanup restore_stash = [stash, saved_priv, work] {
        stash->xdrs->x_private = saved_priv;
        work->flags &= ~NC_INDEF;
      };
      stash->xdrs->x_private = nullptr;

      EXPECT_THAT(ncendef(work_id), Eq(-1));
    }
    EXPECT_THAT(ncclose(work_id), Eq(0));
    EXPECT_THAT(ncclose(stash_id), Eq(0));
  }

  // 2. New record variable fill failure without NC_NOFILL (file.c:632-634)
  {
    const std::string stash_path = GetTempFilePath("redef_err2_stash.nc");
    const std::string work_path = GetTempFilePath("redef_err2_work.nc");
    absl::Cleanup rem = [&stash_path, &work_path] {
      std::filesystem::remove(stash_path);
      std::filesystem::remove(work_path);
    };
    CreateNetCdfWithVars(stash_path);
    CreateEmptyNetCdf(work_path);

    const int work_id = ncopen(work_path.c_str(), NC_WRITE);
    ASSERT_THAT(work_id, Ge(0));
    const int stash_id = ncopen(stash_path.c_str(), NC_WRITE);
    ASSERT_THAT(stash_id, Ge(0));

    NC* work = NC_check_id(work_id);
    ASSERT_THAT(work, NotNull());

    work->flags = (NC_RDWR | NC_INDEF);  // WITHOUT NC_NOFILL!
    work->redefid = stash_id;

    const int d0 = ncdimdef(work_id, "x", 4);
    const int d1 = ncdimdef(work_id, "rec", NC_UNLIMITED);
    const int fix_dims[] = {d0};
    const int rec_dims[] = {d1, d0};
    (void)ncvardef(work_id, "v0", NC_LONG, 1, fix_dims);
    (void)ncvardef(work_id, "v1", NC_LONG, 2, rec_dims);
    // Extra new record variables: v2 succeeds xdr_NC_fill (file.c:632), v3
    // fails xdr_NC_fill (file.c:634)
    (void)ncvardef(work_id, "v2", NC_LONG, 2, rec_dims);
    (void)ncvardef(work_id, "v3", NC_LONG, 2, rec_dims);
    reinterpret_cast<NC_var**>(work->vars->values)[3]->type =
        static_cast<nc_type>(99);

    EXPECT_THAT(ncendef(work_id), Eq(-1));

    work->flags &= ~NC_INDEF;
    EXPECT_THAT(ncclose(work_id), Eq(0));
    EXPECT_THAT(ncclose(stash_id), Eq(0));
  }

  // 3. NC_reccpy failure at hdf_xdr_setpos (file.c:558, 559, 629)
  {
    const std::string stash_path = GetTempFilePath("redef_err3_stash.nc");
    const std::string work_path = GetTempFilePath("redef_err3_work.nc");
    absl::Cleanup rem = [&stash_path, &work_path] {
      std::filesystem::remove(stash_path);
      std::filesystem::remove(work_path);
    };
    CreateNetCdfWithVars(stash_path);
    CreateEmptyNetCdf(work_path);

    const int work_id = ncopen(work_path.c_str(), NC_WRITE);
    ASSERT_THAT(work_id, Ge(0));
    const int stash_id = ncopen(stash_path.c_str(), NC_WRITE);
    ASSERT_THAT(stash_id, Ge(0));

    NC* work = NC_check_id(work_id);
    NC* stash = NC_check_id(stash_id);
    ASSERT_THAT(work, NotNull());
    ASSERT_THAT(stash, NotNull());

    work->flags |= (NC_INDEF | NC_NOFILL);
    work->redefid = stash_id;

    const int d0 = ncdimdef(work_id, "x", 4);
    const int d1 = ncdimdef(work_id, "rec", NC_UNLIMITED);
    // ONLY define record variables on work (no fixed vars), so pass 1 skips
    // NC_vcpy completely!
    const int rec_dims[] = {d1, d0};
    (void)ncvardef(work_id, "v1", NC_LONG, 2, rec_dims);

    {
      auto saved_priv = stash->xdrs->x_private;
      absl::Cleanup restore_stash = [stash, saved_priv, work] {
        stash->xdrs->x_private = saved_priv;
        work->flags &= ~NC_INDEF;
      };
      stash->xdrs->x_private = nullptr;

      EXPECT_THAT(ncendef(work_id), Eq(-1));
    }
    EXPECT_THAT(ncclose(work_id), Eq(0));
    EXPECT_THAT(ncclose(stash_id), Eq(0));
  }
}

// TODO(schwehr): Uncovered lines in file.c (~91.5% covered) are defensive OOM
// branches, unlink/rename error fallbacks, and max-open-file exhaustion paths.

}  // namespace
}  // namespace hdf4
