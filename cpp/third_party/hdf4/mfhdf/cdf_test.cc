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
// Tests for HDF4/netCDF-2 CDF structure management, XDR translation,
// file format detection, and dimension/variable/attribute conversion (cdf.c).
//
// Note: Google3 compiles NetCDF-2 symbols with the H4_ prefix.
// Tests also pin GOOGLE MODIFICATION in cdf.c (NC_xdr_cdf initializes
// magic = NCMAGIC for XDR_ENCODE so encoding netCDF_FILE headers is defined
// under MSan).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/mfhdf/src/cdf.c

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

// GoogleTest defines SUCCEED() and FAIL() as macros; undefine them before
// including HDF4 headers to avoid macro collision under --config=no_modules.
#ifdef SUCCEED
#undef SUCCEED
#endif
#ifdef FAIL
#undef FAIL
#endif

#include "third_party/hdf4/hdf/src/atom_priv.h"
#include "third_party/hdf4/hdf/src/df.h"
#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/hfile_priv.h"
#include "third_party/hdf4/hdf/src/vg.h"
#include "third_party/hdf4/hdf/src/vg_priv.h"
#include "third_party/hdf4/mfhdf/src/mfhdf.h"

// clang-format off
#include "third_party/hdf4/mfhdf/src/nc_priv.h"
#include "third_party/hdf4/mfhdf/src/hdf_xdr_priv.h"
extern "C" {
#include "third_party/hdf4/mfhdf/src/mf_priv.h"
int32 hdf_get_magicnum(const char* filename);
}
// clang-format on

namespace hdf4 {
namespace {

using ::testing::Eq;
using ::testing::Ge;
using ::testing::Gt;
using ::testing::IsNull;
using ::testing::Ne;
using ::testing::NotNull;

void CleanupCdfHandleResources(NC* handle) {
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

// When cdf.c error paths jump to done: without calling Vdetach/VSdetach,
// Vend(fid) frees the underlying vgtree/vstree nodes, leaving only the
// VGIDGROUP/VSIDGROUP atom entries registered. Detach any live atoms first.
void DetachLeakedVsetAtoms(int32_t max_idx = 2048) {
  for (int32_t i = 0; i < max_idx; ++i) {
    const atom_t vs_cand = (static_cast<atom_t>(VSIDGROUP) << 28) | i;
    if (HAatom_object(vs_cand) != nullptr) {
      (void)VSdetach(vs_cand);
    }
    const atom_t vg_cand = (static_cast<atom_t>(VGIDGROUP) << 28) | i;
    if (HAatom_object(vg_cand) != nullptr) {
      (void)Vdetach(vg_cand);
    }
  }
}

class CdfTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_ncopts_ = ncopts;
    saved_ncerr_ = ncerr;
    ncopts = 0;
    ncerr = NC_NOERR;
    HEclear();
  }

  void TearDown() override {
    DetachLeakedVsetAtoms();
    DFSDrestart();
    DFSDclear();
    SDPfreebuf();
    ncopts = saved_ncopts_;
    ncerr = saved_ncerr_;
    HEclear();
  }

  std::string GetTempFilePath(const std::string& name) {
    return (std::filesystem::path(::testing::TempDir()) / name).string();
  }

  void WriteRawBytes(const std::string& path, const void* data, size_t size) {
    const int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    ASSERT_THAT(fd, Ge(0));
    (void)write(fd, data, size);
    close(fd);
  }

  int saved_ncopts_ = 0;
  int saved_ncerr_ = 0;
};

// -----------------------------------------------------------------------------
// Suite 1: Magic Number & Format Predicates (cdf.c:92-198)
// -----------------------------------------------------------------------------

TEST_F(CdfTest, MagicNumberAndFormatPredicatesNonExistentFile) {
  const std::string path = GetTempFilePath("non_existent_file.cdf");
  (void)std::filesystem::remove(path);

  EXPECT_THAT(hdf_get_magicnum(path.c_str()), Eq(FAIL));
  EXPECT_THAT(HEvalue(1), Eq(DFE_BADNAME));
  HEclear();

  EXPECT_THAT(HDiscdf(path.c_str()), Eq(FALSE));
  EXPECT_THAT(HDisnetcdf(path.c_str()), Eq(FALSE));
  EXPECT_THAT(HDisnetcdf64(path.c_str()), Eq(FALSE));
  HEclear();
}

TEST_F(CdfTest, MagicNumberAndFormatPredicatesTruncatedFile) {
  const std::string path = GetTempFilePath("truncated_2bytes.cdf");
  absl::Cleanup remove = [&path] { std::filesystem::remove(path); };

  constexpr uint8_t kTruncated[2] = {'C', 'D'};
  WriteRawBytes(path, kTruncated, sizeof(kTruncated));

  EXPECT_THAT(hdf_get_magicnum(path.c_str()), Eq(FAIL));
  EXPECT_THAT(HEvalue(1), Eq(DFE_READERROR));
  HEclear();

  EXPECT_THAT(HDiscdf(path.c_str()), Eq(FALSE));
  EXPECT_THAT(HDisnetcdf(path.c_str()), Eq(FALSE));
  EXPECT_THAT(HDisnetcdf64(path.c_str()), Eq(FALSE));
  HEclear();
}

TEST_F(CdfTest, MagicNumberAndFormatPredicatesUnknownMagic) {
  const std::string path = GetTempFilePath("unknown_magic.cdf");
  absl::Cleanup remove = [&path] { std::filesystem::remove(path); };

  constexpr uint8_t kUnknown[4] = {0x89, 'H', 'D', 'F'};
  WriteRawBytes(path, kUnknown, sizeof(kUnknown));

  EXPECT_THAT(hdf_get_magicnum(path.c_str()), Eq(FAIL));
  EXPECT_THAT(HEvalue(1), Eq(DFE_INVFILE));
  HEclear();

  EXPECT_THAT(HDiscdf(path.c_str()), Eq(FALSE));
  EXPECT_THAT(HDisnetcdf(path.c_str()), Eq(FALSE));
  EXPECT_THAT(HDisnetcdf64(path.c_str()), Eq(FALSE));
  HEclear();
}

TEST_F(CdfTest, MagicNumberAndFormatPredicatesValidFormats) {
  const std::string hdf_path = GetTempFilePath("magic_hdf.hdf");
  const std::string cdf_path = GetTempFilePath("magic_cdf.cdf");
  const std::string nc_path = GetTempFilePath("magic_nc.nc");
  const std::string nc64_path = GetTempFilePath("magic_nc64.nc");

  absl::Cleanup remove = [&] {
    std::filesystem::remove(hdf_path);
    std::filesystem::remove(cdf_path);
    std::filesystem::remove(nc_path);
    std::filesystem::remove(nc64_path);
  };

  constexpr uint8_t kHdfMagic[4] = {0x0e, 0x03, 0x13, 0x01};
  constexpr uint8_t kCdfMagic[4] = {0x00, 0x00, 0xff, 0xff};
  constexpr uint8_t kNcMagic[4] = {'C', 'D', 'F', 0x01};
  constexpr uint8_t kNc64Magic[4] = {'C', 'D', 'F', 0x02};

  WriteRawBytes(hdf_path, kHdfMagic, sizeof(kHdfMagic));
  WriteRawBytes(cdf_path, kCdfMagic, sizeof(kCdfMagic));
  WriteRawBytes(nc_path, kNcMagic, sizeof(kNcMagic));
  WriteRawBytes(nc64_path, kNc64Magic, sizeof(kNc64Magic));

  EXPECT_THAT(hdf_get_magicnum(hdf_path.c_str()), Eq(HDFXMAGIC));
  EXPECT_THAT(HDiscdf(hdf_path.c_str()), Eq(FALSE));
  EXPECT_THAT(HDisnetcdf(hdf_path.c_str()), Eq(FALSE));
  EXPECT_THAT(HDisnetcdf64(hdf_path.c_str()), Eq(FALSE));

  EXPECT_THAT(hdf_get_magicnum(cdf_path.c_str()), Eq(CDFMAGIC));
  EXPECT_THAT(HDiscdf(cdf_path.c_str()), Eq(TRUE));
  EXPECT_THAT(HDisnetcdf(cdf_path.c_str()), Eq(FALSE));
  EXPECT_THAT(HDisnetcdf64(cdf_path.c_str()), Eq(FALSE));

  EXPECT_THAT(hdf_get_magicnum(nc_path.c_str()), Eq(NCMAGIC));
  EXPECT_THAT(HDiscdf(nc_path.c_str()), Eq(FALSE));
  EXPECT_THAT(HDisnetcdf(nc_path.c_str()), Eq(TRUE));
  EXPECT_THAT(HDisnetcdf64(nc_path.c_str()), Eq(FALSE));

  EXPECT_THAT(hdf_get_magicnum(nc64_path.c_str()), Eq(NCMAGIC64));
  EXPECT_THAT(HDiscdf(nc64_path.c_str()), Eq(FALSE));
  EXPECT_THAT(HDisnetcdf(nc64_path.c_str()), Eq(FALSE));
  EXPECT_THAT(HDisnetcdf64(nc64_path.c_str()), Eq(TRUE));
}

// -----------------------------------------------------------------------------
// Suite 2: Type Mapping, Ref Lookup & XDR Length (cdf.c:481-566, 2213-2225)
// -----------------------------------------------------------------------------

TEST_F(CdfTest, TypeMappingHdfMapType) {
  EXPECT_THAT(hdf_map_type(NC_BYTE), Eq(DFNT_INT8));
  EXPECT_THAT(hdf_map_type(NC_CHAR), Eq(DFNT_CHAR));
  EXPECT_THAT(hdf_map_type(NC_SHORT), Eq(DFNT_INT16));
  EXPECT_THAT(hdf_map_type(NC_LONG), Eq(DFNT_INT32));
  EXPECT_THAT(hdf_map_type(NC_FLOAT), Eq(DFNT_FLOAT32));
  EXPECT_THAT(hdf_map_type(NC_DOUBLE), Eq(DFNT_FLOAT64));
  EXPECT_THAT(hdf_map_type(NC_UNSPECIFIED), Eq(DFNT_CHAR));
  EXPECT_THAT(hdf_map_type(static_cast<nc_type>(999)), Eq(DFNT_NONE));
}

TEST_F(CdfTest, TypeMappingHdfUnmapType) {
  struct TestCase {
    int32_t input;
    nc_type expected;
  };
  constexpr TestCase kCases[] = {
      {.input = DFNT_CHAR, .expected = NC_CHAR},
      {.input = DFNT_UCHAR, .expected = NC_CHAR},
      {.input = DFNT_INT8, .expected = NC_BYTE},
      {.input = DFNT_UINT8, .expected = NC_BYTE},
      {.input = DFNT_INT16, .expected = NC_SHORT},
      {.input = DFNT_UINT16, .expected = NC_SHORT},
      {.input = DFNT_INT32, .expected = NC_LONG},
      {.input = DFNT_UINT32, .expected = NC_LONG},
      {.input = DFNT_FLOAT32, .expected = NC_FLOAT},
      {.input = DFNT_FLOAT64, .expected = NC_DOUBLE},
  };

  for (const auto& tc : kCases) {
    EXPECT_THAT(hdf_unmap_type(tc.input), Eq(tc.expected));
    EXPECT_THAT(hdf_unmap_type(tc.input | DFNT_NATIVE), Eq(tc.expected));
    EXPECT_THAT(hdf_unmap_type(tc.input | DFNT_LITEND), Eq(tc.expected));
  }
  EXPECT_THAT(hdf_unmap_type(999), Eq(static_cast<nc_type>(FAIL)));
}

TEST_F(CdfTest, XlenCdfAndGetRef) {
  EXPECT_THAT(NC_xlen_cdf(nullptr), Eq(0));

  NC dummy{};
  EXPECT_THAT(NC_xlen_cdf(&dummy), Eq(32));

  const std::string file_path = GetTempFilePath("xlen_test.hdf");
  absl::Cleanup remove = [&file_path] { std::filesystem::remove(file_path); };

  const int32_t sd_id = SDstart(file_path.c_str(), DFACC_CREATE);
  ASSERT_THAT(sd_id, Ne(FAIL));

  int32_t dim_sizes[1] = {10};
  const int32_t sds_id = SDcreate(sd_id, "data", DFNT_INT32, 1, dim_sizes);
  ASSERT_THAT(sds_id, Ne(FAIL));

  const int32_t attr_val = 42;
  EXPECT_THAT(SDsetattr(sd_id, "global_attr", DFNT_INT32, 1, &attr_val),
              Eq(SUCCEED));

  EXPECT_THAT(SDendaccess(sds_id), Eq(SUCCEED));
  EXPECT_THAT(SDend(sd_id), Eq(SUCCEED));

  NC* handle = NC_new_cdf(file_path.c_str(), NC_WRITE);
  ASSERT_THAT(handle, NotNull());
  absl::Cleanup close_handle = [&handle] { (void)NC_free_cdf(handle); };

  EXPECT_THAT(NC_xlen_cdf(handle), Gt(32));
  EXPECT_THAT(hdf_get_ref(handle, 0), Gt(0));
}

// -----------------------------------------------------------------------------
// Suite 3: NC_new_cdf, NC_free_cdf & NSSDC CDF Lifecycle (cdf.c:38-368)
// -----------------------------------------------------------------------------

TEST_F(CdfTest, NcNewCdfFileModesAndErrors) {
  const std::string path = GetTempFilePath("new_cdf_modes.hdf");
  absl::Cleanup remove = [&path] { std::filesystem::remove(path); };

  // 1. NC_CLOBBER creates an HDF file
  NC* cdf1 = NC_new_cdf(path.c_str(), NC_CLOBBER);
  ASSERT_THAT(cdf1, NotNull());
  EXPECT_THAT(cdf1->file_type, Eq(HDF_FILE));
  EXPECT_THAT(NC_free_cdf(cdf1), Eq(SUCCEED));

  // 2. NC_NOCLOBBER on existing HDF file fails (L296-300)
  EXPECT_THAT(NC_new_cdf(path.c_str(), NC_NOCLOBBER), IsNull());

  // 3. NC_WRITE and NC_NOWRITE open existing HDF file
  NC* cdf2 = NC_new_cdf(path.c_str(), NC_WRITE);
  ASSERT_THAT(cdf2, NotNull());
  EXPECT_THAT(NC_free_cdf(cdf2), Eq(SUCCEED));

  NC* cdf3 = NC_new_cdf(path.c_str(), NC_NOWRITE);
  ASSERT_THAT(cdf3, NotNull());
  EXPECT_THAT(NC_free_cdf(cdf3), Eq(SUCCEED));

  // 4. Unknown format file fails (L233)
  const std::string unk_path = GetTempFilePath("unknown_format.bin");
  absl::Cleanup remove_unk = [&unk_path] { std::filesystem::remove(unk_path); };
  constexpr uint8_t kGarbage[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  WriteRawBytes(unk_path, kGarbage, sizeof(kGarbage));
  EXPECT_THAT(NC_new_cdf(unk_path.c_str(), NC_NOWRITE), IsNull());

  // 5. Corrupt NetCDF file failing xdr_cdf (L338-343)
  const std::string corrupt_nc = GetTempFilePath("corrupt.nc");
  absl::Cleanup remove_cnc = [&corrupt_nc] {
    std::filesystem::remove(corrupt_nc);
  };
  constexpr uint8_t kTruncNc[4] = {'C', 'D', 'F', 0x01};
  WriteRawBytes(corrupt_nc, kTruncNc, sizeof(kTruncNc));
  EXPECT_THAT(NC_new_cdf(corrupt_nc.c_str(), NC_NOWRITE), IsNull());
}

TEST_F(CdfTest, NcNewCdfNssdcCdfFileIngestion) {
  const std::string cdf_path = GetTempFilePath("minimal.cdf");
  absl::Cleanup remove_cdf = [&cdf_path] { std::filesystem::remove(cdf_path); };

  // Synthesize minimal valid NSSDC CDF file: 8-byte magic + 48-byte CDR +
  // 68-byte GDR
  std::vector<uint8_t> buf;
  auto write_i32 = [&buf](int32_t val) {
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xff));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xff));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xff));
    buf.push_back(static_cast<uint8_t>(val & 0xff));
  };

  // Magic
  write_i32(0x0000FFFF);
  write_i32(0x0000FFFF);

  // CDR (48 bytes)
  const size_t gdr_offset = 56;
  write_i32(48);
  write_i32(1);  // CDR
  write_i32(static_cast<int32_t>(gdr_offset));
  write_i32(2);  // vers
  write_i32(5);  // release
  write_i32(1);  // NETWORK_ENCODING
  write_i32(2);  // flags
  for (int i = 0; i < 5; ++i) write_i32(0);

  // GDR (68 bytes)
  write_i32(68);
  write_i32(2);  // GDR
  write_i32(0);  // rvarNext
  write_i32(0);  // zvarNext
  write_i32(0);  // adrNext
  write_i32(0);  // eof
  write_i32(0);  // numRVars
  write_i32(0);  // numAttrs
  write_i32(0);  // maxRec
  write_i32(0);  // numDims
  write_i32(0);  // numZVars
  for (int i = 0; i < 4; ++i) write_i32(0);

  WriteRawBytes(cdf_path, buf.data(), buf.size());

  NC* handle = NC_new_cdf(cdf_path.c_str(), NC_NOWRITE);
  ASSERT_THAT(handle, NotNull());
  EXPECT_THAT(handle->file_type, Eq(CDF_FILE));

  CleanupCdfHandleResources(handle);
  EXPECT_THAT(NC_free_cdf(handle), Eq(SUCCEED));
}

// -----------------------------------------------------------------------------
// Suite 4: XDR CDF Dispatch & NC_xdr_cdf Decode/Encode (cdf.c:371-449,
// 1878-1914)
// -----------------------------------------------------------------------------

TEST_F(CdfTest, XdrCdfAndHdfXdrCdfDispatchEdges) {
  XDR xdrs{};
  NC handle{};

  // Unknown file type returns FALSE (L386-388)
  handle.file_type = 99;
  NC* handle_ptr = &handle;
  EXPECT_THAT(xdr_cdf(&xdrs, &handle_ptr), Eq(FALSE));

  // hdf_xdr_cdf unknown x_op returns FAIL
  constexpr enum xdr_op kBogusXdrOp = static_cast<enum xdr_op>(3);
  handle.file_type = HDF_FILE;
  xdrs.x_op = kBogusXdrOp;
  EXPECT_THAT(hdf_xdr_cdf(&xdrs, &handle_ptr), Eq(FAIL));

  // XDR_FREE on netCDF_FILE calls NC_xdr_cdf -> NC_free_xcdf (L399-402)
  NC* nc_free_target = static_cast<NC*>(std::calloc(1, sizeof(NC)));
  ASSERT_THAT(nc_free_target, NotNull());
  nc_free_target->file_type = netCDF_FILE;
  xdrs.x_op = XDR_FREE;
  EXPECT_THAT(xdr_cdf(&xdrs, &nc_free_target), Eq(TRUE));
  std::free(nc_free_target);

  // XDR_FREE on HDF_FILE (hdf_xdr_cdf lines 1902-1907)
  const std::string hdf_free_path = GetTempFilePath("xdr_free.hdf");
  absl::Cleanup remove_hdf = [&hdf_free_path] {
    std::filesystem::remove(hdf_free_path);
  };
  NC* hdf_free_target = NC_new_cdf(hdf_free_path.c_str(), NC_CLOBBER);
  ASSERT_THAT(hdf_free_target, NotNull());
  xdrs.x_op = XDR_FREE;
  EXPECT_THAT(hdf_xdr_cdf(&xdrs, &hdf_free_target), Eq(SUCCEED));
}

TEST_F(CdfTest, NCxdrCdfDecodeErrorBranches) {
  const std::string temp_xdr = GetTempFilePath("decode_err.xdr");
  absl::Cleanup remove_xdr = [&temp_xdr] { std::filesystem::remove(temp_xdr); };

  auto test_decode_bytes = [&](const void* data, size_t size,
                               bool set_pos_non_zero = false) {
    WriteRawBytes(temp_xdr, data, size);
    const int fd = open(temp_xdr.c_str(), O_RDONLY);
    EXPECT_THAT(fd, Ge(0));
    XDR xdrs{};
    EXPECT_THAT(hdf_xdr_create(&xdrs, fd, O_RDONLY, XDR_DECODE), Ge(0));
    absl::Cleanup destroy_xdr = [&xdrs] { hdf_xdr_destroy(&xdrs); };
    if (set_pos_non_zero) {
      (void)hdf_xdr_setpos(&xdrs, 4);
    }
    NC* h = static_cast<NC*>(std::calloc(1, sizeof(NC)));
    if (h == nullptr) return static_cast<bool_t>(FALSE);
    h->file_type = netCDF_FILE;
    h->xdrs = static_cast<XDR*>(std::calloc(1, sizeof(XDR)));
    if (h->xdrs == nullptr) {
      std::free(h);
      return static_cast<bool_t>(FALSE);
    }
    hdf_xdr_setup_nofile(h->xdrs, NC_NOWRITE);
    absl::Cleanup free_h = [&h] {
      if (h != nullptr) {
        (void)NC_free_cdf(h);
      }
    };
    return xdr_cdf(&xdrs, &h);
  };

  // 1. Non-zero initial position (L404-408)
  std::vector<uint8_t> dummy(64, 0);
  EXPECT_THAT(test_decode_bytes(dummy.data(), dummy.size(), true), Eq(FALSE));

  // 2. Truncated EOF magic read (L412-415)
  EXPECT_THAT(test_decode_bytes(dummy.data(), 2), Eq(FALSE));

  // 3. NCLINKMAGIC ("CDL\x01") (L424-426)
  uint8_t cdl_buf[4] = {'C', 'D', 'L', 0x01};
  EXPECT_THAT(test_decode_bytes(cdl_buf, sizeof(cdl_buf)), Eq(FALSE));
  EXPECT_THAT(ncerr, Eq(NC_NOERR));

  // 4. Bad magic (L428-429)
  uint8_t bad_buf[4] = {0x12, 0x34, 0x56, 0x78};
  EXPECT_THAT(test_decode_bytes(bad_buf, sizeof(bad_buf)), Eq(FALSE));

  // 5. Truncated at numrecs (L432-435)
  uint8_t numrecs_buf[4] = {'C', 'D', 'F', 0x01};
  EXPECT_THAT(test_decode_bytes(numrecs_buf, sizeof(numrecs_buf)), Eq(FALSE));

  // 6. Truncated at dims (L436-439)
  uint8_t dims_buf[8] = {'C', 'D', 'F', 0x01, 0, 0, 0, 0};
  EXPECT_THAT(test_decode_bytes(dims_buf, sizeof(dims_buf)), Eq(FALSE));

  // 7. Truncated at attrs (L440-443): dims is NC_DIMENSION (10) with count 0
  uint8_t attrs_buf[16] = {'C', 'D', 'F', 0x01, 0, 0, 0, 0,
                           0,   0,   0,   10,   0, 0, 0, 0};
  EXPECT_THAT(test_decode_bytes(attrs_buf, sizeof(attrs_buf)), Eq(FALSE));

  // 8. Truncated at vars (L444-447): dims count 0, attrs count 0
  uint8_t vars_buf[24] = {'C', 'D', 'F', 0x01, 0, 0, 0, 0,  0, 0, 0, 10,
                          0,   0,   0,   0,    0, 0, 0, 12, 0, 0, 0, 0};
  EXPECT_THAT(test_decode_bytes(vars_buf, sizeof(vars_buf)), Eq(FALSE));
}

TEST_F(CdfTest, NCxdrCdfValidDecodeAndEncode) {
  const std::string temp_xdr = GetTempFilePath("valid_nc.xdr");
  absl::Cleanup remove_xdr = [&temp_xdr] { std::filesystem::remove(temp_xdr); };

  NC* handle = static_cast<NC*>(std::calloc(1, sizeof(NC)));
  ASSERT_THAT(handle, NotNull());
  handle->file_type = netCDF_FILE;
  handle->xdrs = static_cast<XDR*>(std::calloc(1, sizeof(XDR)));
  ASSERT_THAT(handle->xdrs, NotNull());
  hdf_xdr_setup_nofile(handle->xdrs, NC_WRITE);
  absl::Cleanup free_handle = [&handle] {
    if (handle != nullptr) {
      (void)NC_free_cdf(handle);
    }
  };

  // Valid NetCDF header with dims=0, attrs=0, vars=0
  uint8_t valid_buf[32] = {
      'C', 'D', 'F', 0x01,  // magic
      0,   0,   0,   0,     // numrecs = 0
      0,   0,   0,   10,    // NC_DIMENSION
      0,   0,   0,   0,     // 0 dims
      0,   0,   0,   12,    // NC_ATTRIBUTE
      0,   0,   0,   0,     // 0 attrs
      0,   0,   0,   11,    // NC_VARIABLE
      0,   0,   0,   0      // 0 vars
  };
  WriteRawBytes(temp_xdr, valid_buf, sizeof(valid_buf));

  const int fd = open(temp_xdr.c_str(), O_RDONLY);
  ASSERT_THAT(fd, Ge(0));
  XDR xdrs{};
  ASSERT_THAT(hdf_xdr_create(&xdrs, fd, O_RDONLY, XDR_DECODE), Ge(0));
  absl::Cleanup destroy_xdr = [&xdrs] { hdf_xdr_destroy(&xdrs); };
  EXPECT_THAT(xdr_cdf(&xdrs, &handle), Eq(TRUE));

  // XDR_ENCODE path (pins GOOGLE MODIFICATION initializing magic in cdf.c)
  const std::string out_xdr = GetTempFilePath("encode_out.xdr");
  absl::Cleanup remove_out = [&out_xdr] { std::filesystem::remove(out_xdr); };
  const int out_fd = open(out_xdr.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
  ASSERT_THAT(out_fd, Ge(0));
  XDR enc_xdr{};
  const int enc_create = hdf_xdr_create(&enc_xdr, out_fd, O_RDWR, XDR_ENCODE);
  if (enc_xdr.x_private == nullptr) {
    close(out_fd);
  }
  absl::Cleanup destroy_enc = [&enc_xdr] { hdf_xdr_destroy(&enc_xdr); };
  ASSERT_THAT(enc_create, Ge(0));
  EXPECT_THAT(xdr_cdf(&enc_xdr, &handle), Eq(TRUE));

  // Write error branch: using hdf_xdr_setup_nofile (mode=0, no fd)
  XDR err_xdr{};
  hdf_xdr_setup_nofile(&err_xdr, NC_CREAT);
  absl::Cleanup destroy_err = [&err_xdr] { hdf_xdr_destroy(&err_xdr); };
  EXPECT_THAT(xdr_cdf(&err_xdr, &handle), Eq(FALSE));

  // XDR_FREE path
  XDR free_xdr{.x_op = XDR_FREE};
  EXPECT_THAT(xdr_cdf(&free_xdr, &handle), Eq(TRUE));
  handle->dims = nullptr;
  handle->attrs = nullptr;
  handle->vars = nullptr;
}

// -----------------------------------------------------------------------------
// Suite 5: HDF4 Vgroup/Vdata Write, Read, Compat Dims & Clobber
// (cdf.c:568-2204)
// -----------------------------------------------------------------------------

TEST_F(CdfTest, HdfMetadataWriteReadCompatDimsAndCloseFlush) {
  const std::string path = GetTempFilePath("full_hdf_meta.hdf");
  absl::Cleanup remove = [&path] { std::filesystem::remove(path); };

  // 1. Create HDF file with SDstart
  const int32_t sd_id = SDstart(path.c_str(), DFACC_CREATE);
  ASSERT_THAT(sd_id, Ne(FAIL));

  // Unlimited dimension with SD_DIMVAL_BW_COMP (writes DIM_VALS & DIM_VALS01)
  int32_t unlim_sizes[1] = {SD_UNLIMITED};
  const int32_t unlim_sds =
      SDcreate(sd_id, "rec_var", DFNT_INT32, 1, unlim_sizes);
  ASSERT_THAT(unlim_sds, Ne(FAIL));

  const int32_t udim_id = SDgetdimid(unlim_sds, 0);
  ASSERT_THAT(udim_id, Ne(FAIL));
  EXPECT_THAT(SDsetdimval_comp(udim_id, SD_DIMVAL_BW_COMP), Eq(SUCCEED));

  // 2D fixed dataset with auto-generated fakeDim dimensions
  int32_t fixed_sizes[2] = {3, 4};
  const int32_t fixed_sds =
      SDcreate(sd_id, "fixed_var", DFNT_INT32 | DFNT_LITEND, 2, fixed_sizes);
  ASSERT_THAT(fixed_sds, Ne(FAIL));

  // Scale and dimension strings/attributes on dim 1
  const int32_t fdim1 = SDgetdimid(fixed_sds, 1);
  ASSERT_THAT(fdim1, Ne(FAIL));
  EXPECT_THAT(SDsetdimval_comp(fdim1, SD_DIMVAL_BW_COMP), Eq(SUCCEED));
  EXPECT_THAT(SDsetdimname(fdim1, "x_dim"), Eq(SUCCEED));
  float scale_vals[4] = {1.0f, 2.0f, 3.0f, 4.0f};
  EXPECT_THAT(SDsetdimscale(fdim1, 4, DFNT_FLOAT32, scale_vals), Eq(SUCCEED));
  EXPECT_THAT(SDsetdimstrs(fdim1, "Xcoord", "meters", "F4.1"), Eq(SUCCEED));

  // Second dataset sharing "x_dim" to hit duplicate-dimension loop
  int32_t shared_sizes[1] = {4};
  const int32_t shared_sds =
      SDcreate(sd_id, "shared_var", DFNT_INT32, 1, shared_sizes);
  ASSERT_THAT(shared_sds, Ne(FAIL));
  const int32_t sdim0 = SDgetdimid(shared_sds, 0);
  ASSERT_THAT(sdim0, Ne(FAIL));
  EXPECT_THAT(SDsetdimname(sdim0, "x_dim"), Eq(SUCCEED));
  EXPECT_THAT(SDendaccess(shared_sds), Eq(SUCCEED));

  // Write record data to trigger NC_NDIRTY on unlimited dimension
  int32_t start[1] = {0};
  int32_t edges[1] = {2};
  int32_t rec_vals[2] = {100, 200};
  EXPECT_THAT(SDwritedata(unlim_sds, start, nullptr, edges, rec_vals),
              Eq(SUCCEED));

  // Fixed data
  int32_t fstart[2] = {0, 0};
  int32_t fedges[2] = {3, 4};
  std::vector<int32_t> fixed_data(12, 555);
  EXPECT_THAT(
      SDwritedata(fixed_sds, fstart, nullptr, fedges, fixed_data.data()),
      Eq(SUCCEED));

  // Native character dataset (writes DFNTF_ASCII on LE host -> |= DFNT_NATIVE)
  int32_t str_sizes[1] = {5};
  const int32_t str_sds =
      SDcreate(sd_id, "str_var", DFNT_CHAR | DFNT_NATIVE, 1, str_sizes);
  ASSERT_THAT(str_sds, Ne(FAIL));
  int32_t sstart[1] = {0};
  int32_t sedges[1] = {5};
  char str_val[] = "hello";
  EXPECT_THAT(SDwritedata(str_sds, sstart, nullptr, sedges, str_val),
              Eq(SUCCEED));

  // Global & variable attributes
  const double gattr = 3.14159;
  EXPECT_THAT(SDsetattr(sd_id, "pi", DFNT_FLOAT64, 1, &gattr), Eq(SUCCEED));
  char str_test[] = "test";
  EXPECT_THAT(SDsetattr(sd_id, "desc", DFNT_CHAR, 4, str_test), Eq(SUCCEED));

  // Close everything -> flushes HDF close and unlimited dim records
  // (cdf.c:2126-2200)
  EXPECT_THAT(SDendaccess(unlim_sds), Eq(SUCCEED));
  EXPECT_THAT(SDendaccess(fixed_sds), Eq(SUCCEED));
  EXPECT_THAT(SDendaccess(str_sds), Eq(SUCCEED));
  EXPECT_THAT(SDend(sd_id), Eq(SUCCEED));

  // Flush unlimited dimension records when NC_NDIRTY is set and aid is still
  // active
  const int32_t flush_id = SDstart(path.c_str(), DFACC_RDWR);
  ASSERT_THAT(flush_id, Ne(FAIL));
  const int32_t flush_sds = SDselect(flush_id, 0);
  ASSERT_THAT(flush_sds, Ne(FAIL));
  int32_t flush_start[1] = {2};
  int32_t flush_edges[1] = {1};
  int32_t flush_val[1] = {300};
  EXPECT_THAT(
      SDwritedata(flush_sds, flush_start, nullptr, flush_edges, flush_val),
      Eq(SUCCEED));
  // Call SDend without calling SDendaccess(flush_sds) -> flushes aid and
  // UDIMENSION
  EXPECT_THAT(SDend(flush_id), Eq(SUCCEED));

  // 2. Reopen and read back metadata (hdf_read_xdr_cdf -> hdf_read_dims,
  // hdf_read_vars, hdf_read_attrs)
  const int32_t rw_id = SDstart(path.c_str(), DFACC_RDWR);
  ASSERT_THAT(rw_id, Ne(FAIL));

  int32_t n_datasets = 0;
  int32_t n_gattrs = 0;
  EXPECT_THAT(SDfileinfo(rw_id, &n_datasets, &n_gattrs), Eq(SUCCEED));
  EXPECT_THAT(n_datasets, Ge(4));
  EXPECT_THAT(n_gattrs, Ge(2));

  // Modify metadata and close -> triggers hdf_cdf_clobber and hdf_vg_clobber
  // (cdf.c:1923-2077)
  const int32_t extra_val = 999;
  EXPECT_THAT(SDsetattr(rw_id, "clobber_attr", DFNT_INT32, 1, &extra_val),
              Eq(SUCCEED));
  EXPECT_THAT(SDend(rw_id), Eq(SUCCEED));

  // 3. 0-dimension file to cover else handle->dims = NULL and attrs = NULL
  const std::string zpath = GetTempFilePath("zero_dim.hdf");
  absl::Cleanup remove_z = [&zpath] { std::filesystem::remove(zpath); };
  const int zcid = nccreate(zpath.c_str(), NC_CLOBBER);
  ASSERT_THAT(zcid, Ge(0));
  const int zvar = ncvardef(zcid, "scalar", NC_LONG, 0, nullptr);
  EXPECT_THAT(zvar, Ge(0));
  EXPECT_THAT(ncendef(zcid), Eq(SUCCEED));
  EXPECT_THAT(ncclose(zcid), Eq(SUCCEED));

  const int zopen = ncopen(zpath.c_str(), NC_NOWRITE);
  ASSERT_THAT(zopen, Ge(0));
  EXPECT_THAT(ncclose(zopen), Eq(SUCCEED));

  // 3. Direct hdf_create_compat_dim_vdata error checks (cdf.c:612-617)
  NC dummy_nc{};
  NC_dim dummy_dim{};
  dummy_dim.size = -5;
  EXPECT_THAT(hdf_create_compat_dim_vdata(nullptr, &dummy_nc, &dummy_dim,
                                          DIMVAL_VERSION00),
              Eq(FAIL));
  dummy_dim.size = 10;
  EXPECT_THAT(hdf_create_compat_dim_vdata(nullptr, &dummy_nc, &dummy_dim, 999),
              Eq(FAIL));

  // 4. Direct hdf_cdf_clobber on handle with vgid == 0 (cdf.c:2003-2005)
  dummy_nc.vgid = 0;
  EXPECT_THAT(hdf_cdf_clobber(&dummy_nc), Eq(SUCCEED));
}

// -----------------------------------------------------------------------------
// Suite 6: DFTAG_SDRAG & Foreign Machine DFTAG_NT in hdf_read_vars
// (cdf.c:1664-1703)
// -----------------------------------------------------------------------------

TEST_F(CdfTest, HdfReadVarsRaggedArrayBranch) {
  const std::string path = GetTempFilePath("ragged_test.hdf");
  absl::Cleanup remove = [&path] { std::filesystem::remove(path); };

  // 1. Create a 1-dataset HDF file
  const int32_t sd_id = SDstart(path.c_str(), DFACC_CREATE);
  ASSERT_THAT(sd_id, Ne(FAIL));
  int32_t dims[1] = {4};
  const int32_t sds_id = SDcreate(sd_id, "rag_var", DFNT_INT32, 1, dims);
  ASSERT_THAT(sds_id, Ne(FAIL));
  EXPECT_THAT(SDendaccess(sds_id), Eq(SUCCEED));
  EXPECT_THAT(SDend(sd_id), Eq(SUCCEED));

  // 2. Open via low-level Hopen/Vstart and add DFTAG_SDRAG tag/ref to
  // _HDF_VARIABLE Vgroup
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_RDWR, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_h = [fid] { (void)Hclose(fid); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    absl::Cleanup end_v = [fid] { (void)Vend(fid); };

    int32_t vgid = -1;
    while ((vgid = Vgetid(fid, vgid)) != FAIL) {
      const int32_t vkey = Vattach(fid, vgid, "w");
      if (vkey == FAIL) continue;

      char vclass[64] = "";
      (void)Vgetclass(vkey, vclass);
      if (std::strcmp(vclass, _HDF_VARIABLE) == 0) {
        // Add DFTAG_SDRAG tagref
        (void)Vaddtagref(vkey, DFTAG_SDRAG, 1);
        (void)Vdetach(vkey);
        break;
      }
      (void)Vdetach(vkey);
    }
  }

  // 3. Reopen via SDstart -> hits case DFTAG_SDRAG (cdf.c:1664-1666, 1741-1743)
  const int32_t re_id = SDstart(path.c_str(), DFACC_READ);
  ASSERT_THAT(re_id, Ne(FAIL));
  EXPECT_THAT(SDend(re_id), Eq(SUCCEED));
}

TEST_F(CdfTest, HdfReadVarsBadNumberTypeBranch) {
  const std::string path = GetTempFilePath("bad_nt_test.hdf");
  absl::Cleanup remove = [&path] { std::filesystem::remove(path); };

  // 1. Create 1-dataset HDF file
  const int32_t sd_id = SDstart(path.c_str(), DFACC_CREATE);
  ASSERT_THAT(sd_id, Ne(FAIL));
  int32_t dims[1] = {4};
  const int32_t sds_id = SDcreate(sd_id, "nt_var", DFNT_INT32, 1, dims);
  ASSERT_THAT(sds_id, Ne(FAIL));
  EXPECT_THAT(SDendaccess(sds_id), Eq(SUCCEED));
  EXPECT_THAT(SDend(sd_id), Eq(SUCCEED));

  // 2. Open via low-level Hopen and write a foreign DFTAG_NT element
  // (ntstring[3] = 99)
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_RDWR, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_h = [fid] { (void)Hclose(fid); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    absl::Cleanup end_v = [fid] { (void)Vend(fid); };

    const uint16_t bad_ref = Hnewref(fid);
    const uint8_t bad_nt[4] = {DFNT_VERSION + 1, DFNT_INT32, 32, 99};
    EXPECT_THAT(Hputelement(fid, DFTAG_NT, bad_ref, bad_nt, sizeof(bad_nt)),
                Eq(sizeof(bad_nt)));

    int32_t vgid = -1;
    while ((vgid = Vgetid(fid, vgid)) != FAIL) {
      const int32_t vkey = Vattach(fid, vgid, "w");
      if (vkey == FAIL) continue;

      char vclass[64] = "";
      (void)Vgetclass(vkey, vclass);
      if (std::strcmp(vclass, _HDF_VARIABLE) == 0) {
        (void)Vaddtagref(vkey, DFTAG_NT, bad_ref);
        (void)Vdetach(vkey);
        break;
      }
      (void)Vdetach(vkey);
    }
  }

  // 3. Reopen via SDstart -> hits goto bad_number_type (cdf.c:1689-1695)
  // Incompatible variable is skipped; SDstart succeeds and returns 0 datasets.
  const int32_t re_id = SDstart(path.c_str(), DFACC_READ);
  ASSERT_THAT(re_id, Ne(FAIL));
  int32_t n_ds = -1;
  int32_t n_ga = -1;
  EXPECT_THAT(SDfileinfo(re_id, &n_ds, &n_ga), Eq(SUCCEED));
  EXPECT_THAT(n_ds, Eq(0));
  EXPECT_THAT(SDend(re_id), Eq(SUCCEED));
}

TEST_F(CdfTest, HdfReadVarsNativeCharCustomSubobjectsAndClobberBranches) {
  const std::string path = GetTempFilePath("subobjects_clobber.hdf");
  absl::Cleanup remove = [&path] { std::filesystem::remove(path); };

  const int32_t sd_id = SDstart(path.c_str(), DFACC_CREATE);
  ASSERT_THAT(sd_id, Ne(FAIL));
  int32_t dims[1] = {4};
  const int32_t sds_id = SDcreate(sd_id, "v1", DFNT_INT32, 1, dims);
  ASSERT_THAT(sds_id, Ne(FAIL));
  EXPECT_THAT(SDendaccess(sds_id), Eq(SUCCEED));
  EXPECT_THAT(SDend(sd_id), Eq(SUCCEED));

  {
    const int32_t fid = Hopen(path.c_str(), DFACC_RDWR, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_h = [fid] { (void)Hclose(fid); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    absl::Cleanup end_v = [fid] { (void)Vend(fid); };

    // 1. Write DFTAG_NT for native char (cdf.c:1700)
    const uint16_t nt_ref = Hnewref(fid);
    const uint8_t nt_data[4] = {
        DFNT_VERSION + 1, DFNT_CHAR, 8,
        static_cast<uint8_t>(DFKgetPNSC(DFNT_CHAR, DF_MT))};
    EXPECT_THAT(Hputelement(fid, DFTAG_NT, nt_ref, nt_data, sizeof(nt_data)),
                Eq(sizeof(nt_data)));

    // 2. Create custom Vdata of class "CUSTOM_VS" (cdf.c:1647)
    const int32_t vs = VSattach(fid, -1, "w");
    ASSERT_THAT(vs, Ne(FAIL));
    EXPECT_THAT(VSsetclass(vs, "CUSTOM_VS"), Eq(SUCCEED));
    char fld[] = "F";
    EXPECT_THAT(VSfdefine(vs, fld, DFNT_INT32, 1), Eq(SUCCEED));
    EXPECT_THAT(VSsetfields(vs, fld), Eq(SUCCEED));
    const int32_t vs_ref = VSQueryref(vs);
    EXPECT_THAT(VSdetach(vs), Eq(SUCCEED));

    // 3. Create empty child Vgroup "CUSTOM_VG" (cdf.c:1612, 1956-1960)
    const int32_t child_vg = Vattach(fid, -1, "w");
    ASSERT_THAT(child_vg, Ne(FAIL));
    EXPECT_THAT(Vsetclass(child_vg, "CUSTOM_VG"), Eq(SUCCEED));

    // 4. Attach _HDF_VARIABLE and insert NT, VS, and child VG
    int32_t vgid = -1;
    while ((vgid = Vgetid(fid, vgid)) != FAIL) {
      const int32_t vkey = Vattach(fid, vgid, "w");
      if (vkey == FAIL) continue;
      char vclass[64] = "";
      (void)Vgetclass(vkey, vclass);
      if (std::strcmp(vclass, _HDF_VARIABLE) == 0) {
        EXPECT_THAT(Vaddtagref(vkey, DFTAG_NT, nt_ref), Ne(FAIL));
        EXPECT_THAT(Vaddtagref(vkey, DFTAG_VH, vs_ref), Ne(FAIL));
        EXPECT_THAT(Vinsert(vkey, child_vg), Ne(FAIL));
        EXPECT_THAT(Vdetach(vkey), Eq(SUCCEED));
        break;
      }
      (void)Vdetach(vkey);
    }
    EXPECT_THAT(Vdetach(child_vg), Eq(SUCCEED));

    // 5. Attach _HDF_CDF and add DFTAG_VERSION tag/ref (cdf.c:2055-2060)
    const uint16_t ver_ref = Hnewref(fid);
    constexpr uint8_t kVerData[4] = {1, 2, 3, 4};
    EXPECT_THAT(
        Hputelement(fid, DFTAG_VERSION, ver_ref, kVerData, sizeof(kVerData)),
        Eq(sizeof(kVerData)));
    vgid = -1;
    while ((vgid = Vgetid(fid, vgid)) != FAIL) {
      const int32_t vkey = Vattach(fid, vgid, "w");
      if (vkey == FAIL) continue;
      char vclass[64] = "";
      (void)Vgetclass(vkey, vclass);
      if (std::strcmp(vclass, _HDF_CDF) == 0) {
        EXPECT_THAT(Vaddtagref(vkey, DFTAG_VERSION, ver_ref), Ne(FAIL));
        (void)Vdetach(vkey);
        break;
      }
      (void)Vdetach(vkey);
    }
  }

  // Reopen via SDstart RDWR, set global attribute to trigger clobber, and close
  const int32_t rw_id = SDstart(path.c_str(), DFACC_RDWR);
  ASSERT_THAT(rw_id, Ne(FAIL));
  const int32_t attr_val = 123;
  EXPECT_THAT(SDsetattr(rw_id, "clobber_me", DFNT_INT32, 1, &attr_val),
              Eq(SUCCEED));
  EXPECT_THAT(SDend(rw_id), Eq(SUCCEED));
}

// -----------------------------------------------------------------------------
// Suite 7: hdf_conv_scales Conversion & Error Handling (cdf.c:1090-1147)
// -----------------------------------------------------------------------------

TEST_F(CdfTest, HdfConvScalesFromDfsdFile) {
  const std::string path = GetTempFilePath("conv_scales.hdf");
  absl::Cleanup remove = [&path] { std::filesystem::remove(path); };

  // Write a 2D dataset with DFSD APIs: dim 1 has scale, dim 2 has string only
  // (data_offset = -1)
  int32_t dims[2] = {3, 2};
  EXPECT_THAT(DFSDsetdims(2, dims), Eq(SUCCEED));
  float scale1[3] = {10.0f, 20.0f, 30.0f};
  EXPECT_THAT(DFSDsetdimscale(1, 3, scale1), Eq(SUCCEED));
  EXPECT_THAT(DFSDsetdimstrs(2, "dim2_name", "units", "F4.1"), Eq(SUCCEED));

  float data[6] = {1, 2, 3, 4, 5, 6};
  EXPECT_THAT(DFSDputdata(path.c_str(), 2, dims, data), Eq(SUCCEED));

  // Open with SDstart RDWR, set global attribute so NC_HDIRTY is set, and close
  const int32_t sd_id = SDstart(path.c_str(), DFACC_RDWR);
  ASSERT_THAT(sd_id, Ne(FAIL));

  const int32_t dirty_val = 1;
  EXPECT_THAT(SDsetattr(sd_id, "trigger_dirty", DFNT_INT32, 1, &dirty_val),
              Eq(SUCCEED));

  // SDend flushes hdf_write_xdr_cdf -> calls hdf_conv_scales (both scale &
  // data_offset == -1)
  EXPECT_THAT(SDend(sd_id), Eq(SUCCEED));
}

TEST_F(CdfTest, HdfConvScalesMissingRefError) {
  const std::string path = GetTempFilePath("conv_scales_err.hdf");
  absl::Cleanup remove = [&path] { std::filesystem::remove(path); };

  NC* handle = NC_new_cdf(path.c_str(), NC_CLOBBER);
  ASSERT_THAT(handle, NotNull());
  absl::Cleanup free_handle = [&handle] { (void)NC_free_cdf(handle); };

  NC_var dummy_var{};
  dummy_var.data_tag = DFTAG_SDS;
  dummy_var.data_ref = 9999;  // Non-existent ref -> Hlength == FAIL
  dummy_var.ndg_ref = 1000;

  NC_var* var_ptrs[1] = {&dummy_var};
  NC_array var_array{};
  var_array.count = 1;
  var_array.szof = sizeof(NC_var*);
  var_array.values = reinterpret_cast<uint8_t*>(var_ptrs);
  handle->vars = &var_array;
  absl::Cleanup unlink_vars = [handle] { handle->vars = nullptr; };

  EXPECT_THAT(hdf_conv_scales(&handle), Eq(FAIL));
}

TEST_F(CdfTest, HdfConvScalesNoOffsetAndReadOnlyPutFailure) {
  const std::string path = GetTempFilePath("conv_scales_offset.hdf");
  absl::Cleanup remove = [&path] { std::filesystem::remove(path); };

  NC* wh = NC_new_cdf(path.c_str(), NC_CLOBBER);
  ASSERT_THAT(wh, NotNull());

  const uint16_t sref = Hnewref(wh->hdf_file);
  constexpr uint8_t kBuf[4] = {1, 2, 3, 4};
  EXPECT_THAT(Hputelement(wh->hdf_file, DFTAG_SDS, sref, kBuf, sizeof(kBuf)),
              Eq(sizeof(kBuf)));

  // 1. data_offset == -1 (cdf.c:1112-1114)
  NC_var v1{};
  v1.data_tag = DFTAG_SDS;
  v1.data_ref = sref;
  v1.ndg_ref = 1000;
  v1.data_offset = -1;

  NC_var* var_ptrs[1] = {&v1};
  NC_array var_array{};
  var_array.count = 1;
  var_array.szof = sizeof(NC_var*);
  var_array.values = reinterpret_cast<uint8_t*>(var_ptrs);
  wh->vars = &var_array;

  EXPECT_THAT(hdf_conv_scales(&wh), Eq(SUCCEED));
  EXPECT_THAT(v1.data_ref, Eq(0));
  EXPECT_THAT(v1.data_tag, Eq(DATA_TAG));

  wh->vars = nullptr;
  EXPECT_THAT(NC_free_cdf(wh), Eq(SUCCEED));

  // 2. Read-only handle where Hputelement fails (cdf.c:1132-1136)
  NC* roh = NC_new_cdf(path.c_str(), NC_NOWRITE);
  ASSERT_THAT(roh, NotNull());

  NC_var v2{};
  v2.data_tag = DFTAG_SDS;
  v2.data_ref = sref;
  v2.ndg_ref = 1000;
  v2.data_offset = 0;
  v2.len = 4;

  NC_var* ro_var_ptrs[1] = {&v2};
  NC_array ro_var_array{};
  ro_var_array.count = 1;
  ro_var_array.szof = sizeof(NC_var*);
  ro_var_array.values = reinterpret_cast<uint8_t*>(ro_var_ptrs);
  roh->vars = &ro_var_array;

  EXPECT_THAT(hdf_conv_scales(&roh), Eq(FAIL));
  EXPECT_THAT(v2.data_tag, Eq(DFTAG_SDS));
  EXPECT_THAT(v2.data_ref, Eq(sref));

  roh->vars = nullptr;
  EXPECT_THAT(NC_free_cdf(roh), Eq(SUCCEED));
  HEclear();

  // 3. Hgetelement failure: delete DFTAG_SDS DD after creating element
  // (cdf.c:1121-1122)
  const std::string get_err_path = GetTempFilePath("conv_get_err.hdf");
  absl::Cleanup remove_ge = [&get_err_path] {
    std::filesystem::remove(get_err_path);
  };
  NC* gh = NC_new_cdf(get_err_path.c_str(), NC_CLOBBER);
  ASSERT_THAT(gh, NotNull());
  const uint16_t gsref = Hnewref(gh->hdf_file);
  constexpr uint8_t kGBuf[4] = {1, 2, 3, 4};
  EXPECT_THAT(Hputelement(gh->hdf_file, DFTAG_SDS, gsref, kGBuf, sizeof(kGBuf)),
              Eq(sizeof(kGBuf)));
  NC_var gvar{};
  gvar.data_tag = DFTAG_SDS;
  gvar.data_ref = gsref;
  gvar.ndg_ref = 1000;
  gvar.data_offset = 0;
  gvar.len = 4;
  NC_var* gvar_ptrs[1] = {&gvar};
  NC_array gvar_arr{};
  gvar_arr.count = 1;
  gvar_arr.szof = sizeof(NC_var*);
  gvar_arr.values = reinterpret_cast<uint8_t*>(gvar_ptrs);
  gh->vars = &gvar_arr;
  EXPECT_THAT(Hdeldd(gh->hdf_file, DFTAG_SDS, gsref), Eq(SUCCEED));
  EXPECT_THAT(hdf_conv_scales(&gh), Eq(FAIL));
  gh->vars = nullptr;
  EXPECT_THAT(NC_free_cdf(gh), Eq(SUCCEED));
  HEclear();
}

// -----------------------------------------------------------------------------
// Suite 8: xdr_numrecs & xdr_NC_fill (cdf.c:2229-2334)
// -----------------------------------------------------------------------------

TEST_F(CdfTest, XdrNumrecsAllBranches) {
  XDR xdrs{};
  NC handle{};

  // 1. HDF_FILE returns TRUE (L2232-2233)
  handle.file_type = HDF_FILE;
  EXPECT_THAT(xdr_numrecs(&xdrs, &handle), Eq(TRUE));

  // 2. netCDF_FILE with NC_NOFILL, XDR_ENCODE, begin_rec > 0 (L2235-2248)
  const std::string temp_xdr = GetTempFilePath("numrecs.xdr");
  absl::Cleanup remove_xdr = [&temp_xdr] { std::filesystem::remove(temp_xdr); };
  const int fd = open(temp_xdr.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
  ASSERT_THAT(fd, Ge(0));
  ASSERT_THAT(hdf_xdr_create(&xdrs, fd, O_RDWR, XDR_ENCODE), Ge(0));
  absl::Cleanup destroy_xdr = [&xdrs] { hdf_xdr_destroy(&xdrs); };

  handle.file_type = netCDF_FILE;
  handle.flags = NC_NOFILL;
  handle.begin_rec = 16;
  handle.numrecs = 2;
  handle.recsize = 8;
  EXPECT_THAT(xdr_numrecs(&xdrs, &handle), Eq(TRUE));

  // 2b. Write failure on read-only file with XDR_ENCODE (cdf.c:2247)
  const int ro_fd = open(temp_xdr.c_str(), O_RDONLY);
  ASSERT_THAT(ro_fd, Ge(0));
  XDR ro_enc_xdr{};
  ASSERT_THAT(hdf_xdr_create(&ro_enc_xdr, ro_fd, O_RDONLY, XDR_ENCODE), Ge(0));
  EXPECT_THAT(xdr_numrecs(&ro_enc_xdr, &handle), Eq(FALSE));
  hdf_xdr_destroy(&ro_enc_xdr);

  // 3. Seek failure on begin_rec overflow: setup_nofile with fd = -1 forces
  // fail on >= 8192
  XDR err_xdr{};
  hdf_xdr_setup_nofile(&err_xdr, NC_CREAT);
  handle.begin_rec = 16384;
  EXPECT_THAT(xdr_numrecs(&err_xdr, &handle), Eq(FALSE));
  hdf_xdr_destroy(&err_xdr);

  // 4. Seek failure on RECPOS (4L): NULL x_private returns FALSE (L2250-2253)
  XDR empty_xdr{.x_op = XDR_DECODE, .x_private = nullptr};
  handle.flags = 0;
  handle.begin_rec = 0;
  EXPECT_THAT(xdr_numrecs(&empty_xdr, &handle), Eq(FALSE));

  // 5. Decode failure on EOF: truncated 4-byte NetCDF header returns FALSE
  // (L2254-2256)
  const std::string eof_xdr_path = GetTempFilePath("numrecs_eof.xdr");
  absl::Cleanup remove_eof = [&eof_xdr_path] {
    std::filesystem::remove(eof_xdr_path);
  };
  constexpr uint8_t kTruncMagic[4] = {'C', 'D', 'F', 0x01};
  WriteRawBytes(eof_xdr_path, kTruncMagic, sizeof(kTruncMagic));
  const int eof_fd = open(eof_xdr_path.c_str(), O_RDONLY);
  ASSERT_THAT(eof_fd, Ge(0));
  XDR eof_xdr{};
  ASSERT_THAT(hdf_xdr_create(&eof_xdr, eof_fd, O_RDONLY, XDR_DECODE), Ge(0));
  EXPECT_THAT(xdr_numrecs(&eof_xdr, &handle), Eq(FALSE));
  hdf_xdr_destroy(&eof_xdr);
}

TEST_F(CdfTest, XdrNCFillTypesAndCustomFillValues) {
  const std::string fill_xdr = GetTempFilePath("fill.xdr");
  absl::Cleanup remove_xdr = [&fill_xdr] { std::filesystem::remove(fill_xdr); };

  auto create_encode_xdr = [&](XDR* xdrs) {
    const int fd = open(fill_xdr.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
    EXPECT_THAT(fd, Ge(0));
    EXPECT_THAT(hdf_xdr_create(xdrs, fd, O_RDWR, XDR_ENCODE), Ge(0));
  };

  // 1. Types NC_SHORT, NC_LONG, NC_FLOAT, NC_DOUBLE (alen = 8 / 4 or 8 / 8)
  struct TypeCase {
    nc_type type;
    unsigned long len;
  };
  constexpr TypeCase kTypes[] = {
      {.type = NC_SHORT, .len = 8},
      {.type = NC_LONG, .len = 8},
      {.type = NC_FLOAT, .len = 8},
      {.type = NC_DOUBLE, .len = 8},
      // NC_BYTE and NC_CHAR must have alen = len / 4 == 0 (len = 2) to avoid
      // wild-pointer crash in hdf_xdr_bytes (cdf.c:2297-2300, 2324)
      {.type = NC_BYTE, .len = 2},
      {.type = NC_CHAR, .len = 2},
  };

  for (const auto& tc : kTypes) {
    XDR xdrs{};
    create_encode_xdr(&xdrs);
    NC_var vp{};
    vp.type = tc.type;
    vp.len = tc.len;
    EXPECT_THAT(xdr_NC_fill(&xdrs, &vp), Eq(TRUE));
    hdf_xdr_destroy(&xdrs);
  }

  // 2. Custom FillValueAttr matching type (L2286-2293)
  {
    XDR xdrs{};
    create_encode_xdr(&xdrs);
    NC_var vp{};
    vp.type = NC_LONG;
    vp.len = 8;

    const int32_t custom_fill = -9999;
    NC_attr* attr = NC_new_attr(FillValueAttr, NC_LONG, 1, &custom_fill);
    ASSERT_THAT(attr, NotNull());
    NC_attr* attr_ptrs[1] = {attr};
    NC_array attr_array{};
    attr_array.count = 1;
    attr_array.szof = sizeof(NC_attr*);
    attr_array.values = reinterpret_cast<uint8_t*>(attr_ptrs);
    vp.attrs = &attr_array;

    EXPECT_THAT(xdr_NC_fill(&xdrs, &vp), Eq(TRUE));
    NC_free_attr(attr);
    hdf_xdr_destroy(&xdrs);
  }

  // 3. Custom FillValueAttr type mismatch (L2284-2285)
  {
    XDR xdrs{};
    create_encode_xdr(&xdrs);
    NC_var vp{};
    vp.type = NC_LONG;
    vp.len = 8;
    NC_string* vname = NC_new_string(4, "var0");
    vp.name = vname;

    const float bad_fill = 1.0f;
    NC_attr* attr = NC_new_attr(FillValueAttr, NC_FLOAT, 1, &bad_fill);
    ASSERT_THAT(attr, NotNull());
    NC_attr* attr_ptrs[1] = {attr};
    NC_array attr_array{};
    attr_array.count = 1;
    attr_array.szof = sizeof(NC_attr*);
    attr_array.values = reinterpret_cast<uint8_t*>(attr_ptrs);
    vp.attrs = &attr_array;

    EXPECT_THAT(xdr_NC_fill(&xdrs, &vp), Eq(TRUE));
    NC_free_attr(attr);
    NC_free_string(vname);
    hdf_xdr_destroy(&xdrs);
  }

  // 4. Invalid vp->type = NC_UNSPECIFIED -> returns FALSE (L2318-2320)
  {
    XDR xdrs{};
    create_encode_xdr(&xdrs);
    NC_var vp{};
    vp.type = NC_UNSPECIFIED;
    vp.len = 8;
    EXPECT_THAT(xdr_NC_fill(&xdrs, &vp), Eq(FALSE));
    hdf_xdr_destroy(&xdrs);
  }

  // 5. XDR write failure on setup_nofile stream (mode = 0) -> returns FALSE
  // (L2328-2330)
  {
    XDR xdrs{};
    hdf_xdr_setup_nofile(&xdrs, NC_CREAT);
    NC_var vp{};
    vp.type = NC_LONG;
    vp.len = 8;
    EXPECT_THAT(xdr_NC_fill(&xdrs, &vp), Eq(FALSE));
    hdf_xdr_destroy(&xdrs);
  }
}

TEST_F(CdfTest, NcNewCdfAndXdrStreamErrorBranches) {
  // 1. NC_CREAT | 0x100 hits default in switch(mode) (cdf.c:286-287)
  const std::string creat_path = GetTempFilePath("creat_extra_mode.hdf");
  absl::Cleanup remove_creat = [&creat_path] {
    std::filesystem::remove(creat_path);
  };
  NC* cdf_extra = NC_new_cdf(creat_path.c_str(), NC_CREAT | 0x100);
  ASSERT_THAT(cdf_extra, NotNull());
  EXPECT_THAT(NC_free_cdf(cdf_extra), Eq(SUCCEED));

  // 2. NC_NOCLOBBER in non-existent directory hits cdf->hdf_file == FAIL
  // (cdf.c:306-307)
  EXPECT_THAT(
      NC_new_cdf("/nonexistent_dir_hdf4_cdf_test/missing.hdf", NC_NOCLOBBER),
      IsNull());
  HEclear();

  // 3. Read-only NetCDF file opened NC_WRITE (cdf.c:251-252)
  const std::string ro_nc = GetTempFilePath("readonly.nc");
  absl::Cleanup remove_ro = [&ro_nc] {
    (void)chmod(ro_nc.c_str(), 0600);
    std::filesystem::remove(ro_nc);
  };
  constexpr uint8_t kNcHdr[32] = {'C', 'D', 'F', 0x01, 0, 0,  0, 0, 0,  0, 0,
                                  10,  0,   0,   0,    0, 0,  0, 0, 12, 0, 0,
                                  0,   0,   0,   0,    0, 11, 0, 0, 0,  0};
  WriteRawBytes(ro_nc, kNcHdr, sizeof(kNcHdr));
  if (geteuid() != 0) {
    ASSERT_THAT(chmod(ro_nc.c_str(), 0400), Eq(0));
    EXPECT_THAT(NC_new_cdf(ro_nc.c_str(), NC_WRITE), IsNull());
    (void)chmod(ro_nc.c_str(), 0600);
  }

  // 4. xdr_cdf with invalid x_op
  constexpr enum xdr_op kBogusXdrOp = static_cast<enum xdr_op>(3);
  XDR bad_op{};
  bad_op.x_op = kBogusXdrOp;
  NC dummy_h{};
  dummy_h.file_type = HDF_FILE;
  NC* dummy_ptr = &dummy_h;
  EXPECT_THAT(xdr_cdf(&bad_op, &dummy_ptr), Eq(FALSE));

  // 5. NC_xdr_cdf setpos(xdrs, 0) failure
  const std::string pos_path = GetTempFilePath("setpos_fail.xdr");
  absl::Cleanup remove_pos = [&pos_path] { std::filesystem::remove(pos_path); };
  std::vector<uint8_t> zeros(16384, 0);
  WriteRawBytes(pos_path, zeros.data(), zeros.size());
  const int fd = open(pos_path.c_str(), O_RDONLY);
  ASSERT_THAT(fd, Ge(0));
  XDR xdrs{};
  const int create_rc = hdf_xdr_create(&xdrs, fd, O_RDONLY, XDR_DECODE);
  if (xdrs.x_private == nullptr) {
    close(fd);
  }
  absl::Cleanup destroy_xdr = [&xdrs] { hdf_xdr_destroy(&xdrs); };
  ASSERT_THAT(create_rc, Ge(0));
  EXPECT_THAT(hdf_xdr_setpos(&xdrs, 8192), Eq(1));
  // Replace fd with a non-seekable pipe via dup2 so lseek fails with ESPIPE
  // while keeping fd open until hdf_xdr_destroy closes it once.
  int pfd[2] = {-1, -1};
  ASSERT_EQ(0, pipe(pfd));
  absl::Cleanup close_pipe = [&pfd] {
    if (pfd[0] >= 0) close(pfd[0]);
    if (pfd[1] >= 0) close(pfd[1]);
  };
  ASSERT_NE(-1, dup2(pfd[0], fd));

  NC* nc_h = static_cast<NC*>(std::calloc(1, sizeof(NC)));
  ASSERT_THAT(nc_h, NotNull());
  absl::Cleanup free_nc_h = [&nc_h] {
    if (nc_h != nullptr) (void)NC_free_cdf(nc_h);
  };
  nc_h->file_type = netCDF_FILE;
  nc_h->xdrs = static_cast<XDR*>(std::calloc(1, sizeof(XDR)));
  ASSERT_THAT(nc_h->xdrs, NotNull());
  hdf_xdr_setup_nofile(nc_h->xdrs, NC_NOWRITE);

  EXPECT_THAT(xdr_cdf(&xdrs, &nc_h), Eq(FALSE));
}

TEST_F(CdfTest, HdfExportedHelpersInvalidHandleAndDanglingTagRefs) {
  NC bad{};
  bad.file_type = HDF_FILE;
  bad.hdf_file = FAIL;
  NC* bad_ptr = &bad;

  NC_dim* dim = NC_new_dim("d0", 4);
  ASSERT_THAT(dim, NotNull());
  absl::Cleanup free_dim = [dim] { NC_free_dim(dim); };

  const int32_t val = 1;
  NC_attr* attr = NC_new_attr("a0", NC_LONG, 1, &val);
  ASSERT_THAT(attr, NotNull());
  absl::Cleanup free_attr = [attr] { NC_free_attr(attr); };

  NC_var* var = NC_new_var("v0", NC_LONG, 0, nullptr);
  ASSERT_THAT(var, NotNull());
  absl::Cleanup free_var = [var] { NC_free_var(var); };

  // 1. hdf_create_dim_vdata / hdf_create_compat_dim_vdata error (cdf.c:583-584,
  // 638-639)
  EXPECT_THAT(hdf_create_dim_vdata(nullptr, &bad, dim), Eq(FAIL));
  EXPECT_THAT(hdf_create_compat_dim_vdata(nullptr, &bad, dim, DIMVAL_VERSION00),
              Eq(FAIL));

  // 2. hdf_write_dim error (cdf.c:704)
  EXPECT_THAT(hdf_write_dim(nullptr, &bad, &dim, 0), Eq(FAIL));

  // 3. hdf_write_var error branches (cdf.c:782, 805, 855)
  NC_attr* attr_ptrs[1] = {attr};
  NC_array attr_arr{};
  attr_arr.count = 1;
  attr_arr.szof = sizeof(NC_attr*);
  attr_arr.values = reinterpret_cast<uint8_t*>(attr_ptrs);
  var->attrs = &attr_arr;
  EXPECT_THAT(hdf_write_var(nullptr, &bad, &var), Eq(FAIL));

  var->attrs = nullptr;
  var->var_type = IS_SDSVAR;
  EXPECT_THAT(hdf_write_var(nullptr, &bad, &var), Eq(FAIL));

  var->var_type = UNKNOWN;
  EXPECT_THAT(hdf_write_var(nullptr, &bad, &var), Eq(FAIL));

  // 4. hdf_write_xdr_cdf & hdf_xdr_cdf XDR_ENCODE error paths (cdf.c:963, 1029,
  // 1046, 1062, 1887, 1891)
  NC_var bad_scale_var{};
  bad_scale_var.data_tag = DFTAG_SDS;
  bad_scale_var.data_ref = 9999;
  bad_scale_var.ndg_ref = 1000;
  NC_var* bsv_ptrs[1] = {&bad_scale_var};
  NC_array bsv_arr{};
  bsv_arr.count = 1;
  bsv_arr.szof = sizeof(NC_var*);
  bsv_arr.values = reinterpret_cast<uint8_t*>(bsv_ptrs);
  bad.vars = &bsv_arr;
  EXPECT_THAT(hdf_write_xdr_cdf(nullptr, &bad_ptr), Eq(FAIL));

  NC_dim* dim_ptrs[1] = {dim};
  NC_array dim_arr{};
  dim_arr.count = 1;
  dim_arr.szof = sizeof(NC_dim*);
  dim_arr.values = reinterpret_cast<uint8_t*>(dim_ptrs);
  bad.vars = nullptr;
  bad.dims = &dim_arr;
  EXPECT_THAT(hdf_write_xdr_cdf(nullptr, &bad_ptr), Eq(FAIL));

  NC_var* var_ptrs[1] = {var};
  NC_array var_arr{};
  var_arr.count = 1;
  var_arr.szof = sizeof(NC_var*);
  var_arr.values = reinterpret_cast<uint8_t*>(var_ptrs);
  bad.dims = nullptr;
  bad.vars = &var_arr;
  EXPECT_THAT(hdf_write_xdr_cdf(nullptr, &bad_ptr), Eq(FAIL));

  bad.vars = nullptr;
  bad.attrs = &attr_arr;
  EXPECT_THAT(hdf_write_xdr_cdf(nullptr, &bad_ptr), Eq(FAIL));

  XDR enc_xdr{};
  enc_xdr.x_op = XDR_ENCODE;
  bad.vgid = 0;
  bad.attrs = &attr_arr;
  EXPECT_THAT(hdf_xdr_cdf(&enc_xdr, &bad_ptr), Eq(FAIL));

  bad.vgid = 999;
  bad.attrs = nullptr;
  EXPECT_THAT(hdf_xdr_cdf(&enc_xdr, &bad_ptr), Eq(FAIL));
  bad.vgid = 0;

  // 5. hdf_read_attrs / hdf_vg_clobber / hdf_close error paths (cdf.c:1405,
  // 1935, 2010, 2115, 2130)
  EXPECT_THAT(hdf_read_attrs(nullptr, &bad, -1), IsNull());
  EXPECT_THAT(hdf_vg_clobber(&bad, 999), Eq(FAIL));

  bad.flags = NC_NDIRTY;
  bad.vgid = 999;
  bad.vars = nullptr;
  EXPECT_THAT(hdf_close(&bad), Eq(FAIL));

  var->aid = 999999;
  bad.vars = &var_arr;
  bad.vgid = 999;
  EXPECT_THAT(hdf_cdf_clobber(&bad), Eq(FAIL));
  var->aid = FAIL;
  bad.vars = nullptr;
  bad.vgid = 0;
  bad.flags = 0;

  // 6. Test on real file with dangling tag refs in Vgroup
  const std::string dpath = GetTempFilePath("dangling_vg.hdf");
  absl::Cleanup remove_d = [&dpath] { std::filesystem::remove(dpath); };

  NC* dh = NC_new_cdf(dpath.c_str(), NC_CLOBBER);
  ASSERT_THAT(dh, NotNull());

  // hdf_write_var else val = handle->numrecs (cdf.c:888)
  NC_dim* udim = NC_new_dim("udim", NC_UNLIMITED);
  ASSERT_THAT(udim, NotNull());
  EXPECT_THAT(hdf_write_dim(nullptr, dh, &udim, 0), Ge(0));
  NC_dim* udim_ptrs[1] = {udim};
  NC_array udim_arr{};
  udim_arr.count = 1;
  udim_arr.szof = sizeof(NC_dim*);
  udim_arr.values = reinterpret_cast<uint8_t*>(udim_ptrs);
  dh->dims = &udim_arr;

  int zero_dim = 0;
  NC_var* uvar = NC_new_var("uvar", NC_LONG, 1, &zero_dim);
  ASSERT_THAT(uvar, NotNull());
  uvar->cdf = dh;
  ASSERT_THAT(NC_var_shape(uvar, dh->dims), Ne(-1));
  uvar->ndg_ref = Hnewref(dh->hdf_file);
  dh->file_type = netCDF_FILE;
  dh->numrecs = 7;
  uvar->data_ref = 100;
  EXPECT_THAT(hdf_write_var(nullptr, dh, &uvar), Ge(0));
  uvar->data_ref = 0;
  dh->file_type = HDF_FILE;
  NC_free_var(uvar);
  NC_free_dim(udim);
  dh->dims = nullptr;

  // Attach _HDF_CDF with dangling (DFTAG_VG, 9998) and (DFTAG_VH, 9999)
  const int32_t vg = Vattach(dh->hdf_file, -1, "w");
  ASSERT_THAT(vg, Ne(FAIL));
  EXPECT_THAT(Vsetclass(vg, _HDF_CDF), Eq(SUCCEED));
  EXPECT_THAT(Vaddtagref(vg, DFTAG_VG, 9998), Ne(FAIL));
  EXPECT_THAT(Vaddtagref(vg, DFTAG_VH, 9999), Ne(FAIL));
  EXPECT_THAT(hdf_read_dims(nullptr, dh, vg), Eq(SUCCEED));
  EXPECT_THAT(hdf_read_vars(nullptr, dh, vg), Eq(SUCCEED));
  EXPECT_THAT(hdf_read_attrs(nullptr, dh, vg), IsNull());
  EXPECT_THAT(Vdetach(vg), Eq(SUCCEED));

  EXPECT_THAT(hdf_read_xdr_cdf(nullptr, &dh), Eq(SUCCEED));
  EXPECT_THAT(NC_free_cdf(dh), Eq(SUCCEED));
  HEclear();
}

TEST_F(CdfTest, NcFreeCdfErrorPathsAndBadComputeshapes) {
  // 1. Vend(FAIL) failure in NC_free_cdf and hdf_xdr_cdf (cdf.c:71, 1904)
  NC* h1 = static_cast<NC*>(std::calloc(1, sizeof(NC)));
  ASSERT_THAT(h1, NotNull());
  h1->file_type = HDF_FILE;
  h1->hdf_file = FAIL;
  h1->xdrs = static_cast<XDR*>(std::calloc(1, sizeof(XDR)));
  ASSERT_THAT(h1->xdrs, NotNull());
  hdf_xdr_setup_nofile(h1->xdrs, NC_NOWRITE);

  XDR fx{};
  fx.x_op = XDR_FREE;
  EXPECT_THAT(hdf_xdr_cdf(&fx, &h1), Eq(FAIL));
  std::free(h1);

  // 2. Hclose failure due to active aid (cdf.c:74)
  const std::string open_aid_path = GetTempFilePath("open_aid.hdf");
  absl::Cleanup remove_oa = [&open_aid_path] {
    std::filesystem::remove(open_aid_path);
  };
  const int32_t fid = Hopen(open_aid_path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
  const int32_t open_aid = Hstartwrite(fid, DFTAG_SD, 1, 4);
  ASSERT_THAT(open_aid, Ne(FAIL));

  NC* h2 = static_cast<NC*>(std::calloc(1, sizeof(NC)));
  ASSERT_THAT(h2, NotNull());
  h2->file_type = HDF_FILE;
  h2->hdf_file = fid;
  h2->xdrs = static_cast<XDR*>(std::calloc(1, sizeof(XDR)));
  ASSERT_THAT(h2->xdrs, NotNull());
  hdf_xdr_setup_nofile(h2->xdrs, NC_NOWRITE);

  EXPECT_THAT(NC_free_cdf(h2), Eq(FAIL));
  EXPECT_THAT(Hendaccess(open_aid), Eq(SUCCEED));
  EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  std::free(h2);
  HEclear();

  // 3. NC_computeshapes failure in NC_new_cdf (cdf.c:346)
  const std::string bad_shape_path = GetTempFilePath("bad_shape.nc");
  absl::Cleanup remove_bs = [&bad_shape_path] {
    std::filesystem::remove(bad_shape_path);
  };
  constexpr uint8_t kBadShapeNc[68] = {
      'C', 'D', 'F', 0x01, 0,   0, 0, 0,  // magic + numrecs=0
      0,   0,   0,   10,   0,   0, 0, 0,  // NC_DIMENSION, 0
      0,   0,   0,   12,   0,   0, 0, 0,  // NC_ATTRIBUTE, 0
      0,   0,   0,   11,   0,   0, 0, 1,  // NC_VARIABLE, 1
      0,   0,   0,   1,    'v', 0, 0, 0,  // name "v"
      0,   0,   0,   1,    0,   0, 0, 0,  // assoc count=1, dimid=0
      0,   0,   0,   0,    0,   0, 0, 0,  // attrs ABSENT
      0,   0,   0,   4,    0,   0, 0, 4,
      0,   0,   0,   68,  // NC_LONG, len=4, begin=68
  };
  WriteRawBytes(bad_shape_path, kBadShapeNc, sizeof(kBadShapeNc));
  EXPECT_THAT(NC_new_cdf(bad_shape_path.c_str(), NC_NOWRITE), IsNull());
}

TEST_F(CdfTest, HdfReadDimsVarsAttrsAndXdrCdfSubobjectErrorBranches) {
  const std::string p1 = GetTempFilePath("subobj_errs.hdf");
  absl::Cleanup remove_p1 = [&p1] { std::filesystem::remove(p1); };

  NC* dh = NC_new_cdf(p1.c_str(), NC_CLOBBER);
  ASSERT_THAT(dh, NotNull());
  absl::Cleanup free_dh = [&dh] {
    if (dh != nullptr) {
      (void)NC_free_cdf(dh);
    }
  };

  // 1. hdf_write_dim compat error (cdf.c:712)
  NC_dim* cdim = NC_new_dim("cd", 4);
  ASSERT_THAT(cdim, NotNull());
  cdim->dim00_compat = 1;
  cdim->size = -5;
  EXPECT_THAT(hdf_write_dim(nullptr, dh, &cdim, 0), Eq(FAIL));
  cdim->size = 4;
  NC_free_dim(cdim);

  // 2. hdf_read_dims dangling VH & NC_free_array cleanup (cdf.c:1214,
  // 1312-1316)
  dh->dims = NC_new_array(NC_DIMENSION, 0, nullptr);
  ASSERT_THAT(dh->dims, NotNull());
  const int32_t cdf_vg1 = Vattach(dh->hdf_file, -1, "w");
  ASSERT_THAT(cdf_vg1, Ne(FAIL));
  EXPECT_THAT(Vsetclass(cdf_vg1, _HDF_CDF), Eq(SUCCEED));
  const int32_t dim_vg1 = Vattach(dh->hdf_file, -1, "w");
  ASSERT_THAT(dim_vg1, Ne(FAIL));
  EXPECT_THAT(Vsetclass(dim_vg1, _HDF_DIMENSION), Eq(SUCCEED));
  EXPECT_THAT(Vsetname(dim_vg1, "d_bad_vh"), Eq(SUCCEED));
  EXPECT_THAT(Vaddtagref(dim_vg1, DFTAG_VH, 9901), Ne(FAIL));
  EXPECT_THAT(Vinsert(cdf_vg1, dim_vg1), Ne(FAIL));
  EXPECT_THAT(hdf_read_dims(nullptr, dh, cdf_vg1), Eq(FAIL));
  EXPECT_THAT(dh->dims, IsNull());
  EXPECT_THAT(Vdetach(dim_vg1), Eq(SUCCEED));
  EXPECT_THAT(Vdetach(cdf_vg1), Eq(SUCCEED));
  DetachLeakedVsetAtoms();

  // 3. hdf_read_dims 0-record DIM_VALS01 VSseek error (cdf.c:1245)
  const int32_t cdf_vg2 = Vattach(dh->hdf_file, -1, "w");
  ASSERT_THAT(cdf_vg2, Ne(FAIL));
  EXPECT_THAT(Vsetclass(cdf_vg2, _HDF_CDF), Eq(SUCCEED));
  const int32_t dim_vg2 = Vattach(dh->hdf_file, -1, "w");
  ASSERT_THAT(dim_vg2, Ne(FAIL));
  EXPECT_THAT(Vsetclass(dim_vg2, _HDF_DIMENSION), Eq(SUCCEED));
  EXPECT_THAT(Vsetname(dim_vg2, "d_empty"), Eq(SUCCEED));
  const int32_t vs_empty_dim = VSattach(dh->hdf_file, -1, "w");
  ASSERT_THAT(vs_empty_dim, Ne(FAIL));
  EXPECT_THAT(VSsetclass(vs_empty_dim, DIM_VALS01), Eq(SUCCEED));
  char val_fld[] = "Values";
  EXPECT_THAT(VSfdefine(vs_empty_dim, val_fld, DFNT_INT32, 1), Eq(SUCCEED));
  EXPECT_THAT(VSsetfields(vs_empty_dim, val_fld), Eq(SUCCEED));
  const int32_t vs_dim_ref = VSQueryref(vs_empty_dim);
  EXPECT_THAT(VSdetach(vs_empty_dim), Eq(SUCCEED));
  EXPECT_THAT(Vaddtagref(dim_vg2, DFTAG_VH, vs_dim_ref), Ne(FAIL));
  EXPECT_THAT(Vinsert(cdf_vg2, dim_vg2), Ne(FAIL));
  EXPECT_THAT(hdf_read_dims(nullptr, dh, cdf_vg2), Eq(FAIL));
  EXPECT_THAT(Vdetach(dim_vg2), Eq(SUCCEED));
  EXPECT_THAT(Vdetach(cdf_vg2), Eq(SUCCEED));
  DetachLeakedVsetAtoms();

  // 4. hdf_read_attrs 0-record _HDF_ATTRIBUTE VSread error (cdf.c:1451)
  const int32_t attr_vg1 = Vattach(dh->hdf_file, -1, "w");
  ASSERT_THAT(attr_vg1, Ne(FAIL));
  EXPECT_THAT(Vsetclass(attr_vg1, _HDF_ATTRIBUTE), Eq(SUCCEED));
  EXPECT_THAT(Vsetname(attr_vg1, "a_empty"), Eq(SUCCEED));
  const int32_t vs_empty_attr = VSattach(dh->hdf_file, -1, "w");
  ASSERT_THAT(vs_empty_attr, Ne(FAIL));
  EXPECT_THAT(VSsetclass(vs_empty_attr, _HDF_ATTRIBUTE), Eq(SUCCEED));
  EXPECT_THAT(VSfdefine(vs_empty_attr, val_fld, DFNT_INT32, 1), Eq(SUCCEED));
  EXPECT_THAT(VSsetfields(vs_empty_attr, val_fld), Eq(SUCCEED));
  const int32_t vs_attr_ref = VSQueryref(vs_empty_attr);
  EXPECT_THAT(VSdetach(vs_empty_attr), Eq(SUCCEED));
  EXPECT_THAT(Vaddtagref(attr_vg1, DFTAG_VH, vs_attr_ref), Ne(FAIL));
  EXPECT_THAT(hdf_read_attrs(nullptr, dh, attr_vg1), IsNull());
  EXPECT_THAT(Vdetach(attr_vg1), Eq(SUCCEED));
  DetachLeakedVsetAtoms();

  // 5. hdf_read_vars subobject error branches
  // Case A: L1605-1606, 1809-1811
  dh->vars = NC_new_array(NC_VARIABLE, 0, nullptr);
  ASSERT_THAT(dh->vars, NotNull());
  const int32_t top_a = Vattach(dh->hdf_file, -1, "w");
  ASSERT_THAT(top_a, Ne(FAIL));
  const int32_t var_vg_a = Vattach(dh->hdf_file, -1, "w");
  ASSERT_THAT(var_vg_a, Ne(FAIL));
  EXPECT_THAT(Vsetclass(var_vg_a, _HDF_VARIABLE), Eq(SUCCEED));
  EXPECT_THAT(Vsetname(var_vg_a, "va"), Eq(SUCCEED));
  EXPECT_THAT(Vaddtagref(var_vg_a, DFTAG_VG, 9902), Ne(FAIL));
  EXPECT_THAT(Vinsert(top_a, var_vg_a), Ne(FAIL));
  EXPECT_THAT(hdf_read_vars(nullptr, dh, top_a), Eq(FAIL));
  dh->vars = nullptr;
  EXPECT_THAT(Vdetach(var_vg_a), Eq(SUCCEED));
  EXPECT_THAT(Vdetach(top_a), Eq(SUCCEED));
  DetachLeakedVsetAtoms();

  // Case B: L1624-1625 (dim not in dh->dims)
  dh->dims = NC_new_array(NC_DIMENSION, 0, nullptr);
  ASSERT_THAT(dh->dims, NotNull());
  const int32_t top_b = Vattach(dh->hdf_file, -1, "w");
  ASSERT_THAT(top_b, Ne(FAIL));
  const int32_t var_vg_b = Vattach(dh->hdf_file, -1, "w");
  ASSERT_THAT(var_vg_b, Ne(FAIL));
  EXPECT_THAT(Vsetclass(var_vg_b, _HDF_VARIABLE), Eq(SUCCEED));
  EXPECT_THAT(Vsetname(var_vg_b, "vb"), Eq(SUCCEED));
  const int32_t dim_vg_b = Vattach(dh->hdf_file, -1, "w");
  ASSERT_THAT(dim_vg_b, Ne(FAIL));
  EXPECT_THAT(Vsetclass(dim_vg_b, _HDF_DIMENSION), Eq(SUCCEED));
  EXPECT_THAT(Vsetname(dim_vg_b, "unregistered_dim"), Eq(SUCCEED));
  EXPECT_THAT(Vinsert(var_vg_b, dim_vg_b), Ne(FAIL));
  EXPECT_THAT(Vinsert(top_b, var_vg_b), Ne(FAIL));
  EXPECT_THAT(hdf_read_vars(nullptr, dh, top_b), Eq(FAIL));
  NC_free_array(dh->dims);
  dh->dims = nullptr;
  EXPECT_THAT(Vdetach(dim_vg_b), Eq(SUCCEED));
  EXPECT_THAT(Vdetach(var_vg_b), Eq(SUCCEED));
  EXPECT_THAT(Vdetach(top_b), Eq(SUCCEED));
  DetachLeakedVsetAtoms();

  // Case C: L1637 (VSattach == FAIL)
  const int32_t top_c = Vattach(dh->hdf_file, -1, "w");
  ASSERT_THAT(top_c, Ne(FAIL));
  const int32_t var_vg_c = Vattach(dh->hdf_file, -1, "w");
  ASSERT_THAT(var_vg_c, Ne(FAIL));
  EXPECT_THAT(Vsetclass(var_vg_c, _HDF_VARIABLE), Eq(SUCCEED));
  EXPECT_THAT(Vsetname(var_vg_c, "vc"), Eq(SUCCEED));
  EXPECT_THAT(Vaddtagref(var_vg_c, DFTAG_VH, 9903), Ne(FAIL));
  EXPECT_THAT(Vinsert(top_c, var_vg_c), Ne(FAIL));
  EXPECT_THAT(hdf_read_vars(nullptr, dh, top_c), Eq(FAIL));
  EXPECT_THAT(Vdetach(var_vg_c), Eq(SUCCEED));
  EXPECT_THAT(Vdetach(top_c), Eq(SUCCEED));
  DetachLeakedVsetAtoms();

  // Case D: L1669-1670 (Hgetelement on missing DFTAG_NT)
  const int32_t top_d = Vattach(dh->hdf_file, -1, "w");
  ASSERT_THAT(top_d, Ne(FAIL));
  const int32_t var_vg_d = Vattach(dh->hdf_file, -1, "w");
  ASSERT_THAT(var_vg_d, Ne(FAIL));
  EXPECT_THAT(Vsetclass(var_vg_d, _HDF_VARIABLE), Eq(SUCCEED));
  EXPECT_THAT(Vsetname(var_vg_d, "vd"), Eq(SUCCEED));
  EXPECT_THAT(Vaddtagref(var_vg_d, DFTAG_NT, 9904), Ne(FAIL));
  EXPECT_THAT(Vinsert(top_d, var_vg_d), Ne(FAIL));
  EXPECT_THAT(hdf_read_vars(nullptr, dh, top_d), Eq(FAIL));
  EXPECT_THAT(Vdetach(var_vg_d), Eq(SUCCEED));
  EXPECT_THAT(Vdetach(top_d), Eq(SUCCEED));
  DetachLeakedVsetAtoms();

  // Case E: L1674-1675 (hdf_unmap_type(99) == FAIL)
  const uint16_t bad_nt_ref = Hnewref(dh->hdf_file);
  const uint8_t bad_nt_bytes[4] = {DFNT_VERSION, 99, 32, DFNTF_IEEE};
  EXPECT_THAT(Hputelement(dh->hdf_file, DFTAG_NT, bad_nt_ref, bad_nt_bytes,
                          sizeof(bad_nt_bytes)),
              Eq(sizeof(bad_nt_bytes)));
  const int32_t top_e = Vattach(dh->hdf_file, -1, "w");
  ASSERT_THAT(top_e, Ne(FAIL));
  const int32_t var_vg_e = Vattach(dh->hdf_file, -1, "w");
  ASSERT_THAT(var_vg_e, Ne(FAIL));
  EXPECT_THAT(Vsetclass(var_vg_e, _HDF_VARIABLE), Eq(SUCCEED));
  EXPECT_THAT(Vsetname(var_vg_e, "ve"), Eq(SUCCEED));
  EXPECT_THAT(Vaddtagref(var_vg_e, DFTAG_NT, bad_nt_ref), Ne(FAIL));
  EXPECT_THAT(Vinsert(top_e, var_vg_e), Ne(FAIL));
  EXPECT_THAT(hdf_read_vars(nullptr, dh, top_e), Eq(FAIL));
  EXPECT_THAT(Vdetach(var_vg_e), Eq(SUCCEED));
  EXPECT_THAT(Vdetach(top_e), Eq(SUCCEED));
  DetachLeakedVsetAtoms();

  EXPECT_THAT(NC_free_cdf(dh), Eq(SUCCEED));
  dh = nullptr;

  // 6. hdf_read_xdr_cdf failure branches (cdf.c:1843, 1848, 1863)
  const std::string p_rx1 = GetTempFilePath("rx1.hdf");
  absl::Cleanup remove_rx1 = [&p_rx1] { std::filesystem::remove(p_rx1); };
  NC* h_rx1 = NC_new_cdf(p_rx1.c_str(), NC_CLOBBER);
  ASSERT_THAT(h_rx1, NotNull());
  const int32_t cvg1 = Vattach(h_rx1->hdf_file, -1, "w");
  ASSERT_THAT(cvg1, Ne(FAIL));
  EXPECT_THAT(Vsetclass(cvg1, _HDF_CDF), Eq(SUCCEED));
  const int32_t dvg1 = Vattach(h_rx1->hdf_file, -1, "w");
  ASSERT_THAT(dvg1, Ne(FAIL));
  EXPECT_THAT(Vsetclass(dvg1, _HDF_DIMENSION), Eq(SUCCEED));
  EXPECT_THAT(Vsetname(dvg1, "d1"), Eq(SUCCEED));
  EXPECT_THAT(Vaddtagref(dvg1, DFTAG_VH, 9905), Ne(FAIL));
  EXPECT_THAT(Vinsert(cvg1, dvg1), Ne(FAIL));
  EXPECT_THAT(Vdetach(dvg1), Eq(SUCCEED));
  EXPECT_THAT(Vdetach(cvg1), Eq(SUCCEED));
  EXPECT_THAT(hdf_read_xdr_cdf(nullptr, &h_rx1), Eq(FAIL));
  DetachLeakedVsetAtoms();
  EXPECT_THAT(NC_free_cdf(h_rx1), Eq(SUCCEED));

  const std::string p_rx2 = GetTempFilePath("rx2.hdf");
  absl::Cleanup remove_rx2 = [&p_rx2] { std::filesystem::remove(p_rx2); };
  NC* h_rx2 = NC_new_cdf(p_rx2.c_str(), NC_CLOBBER);
  ASSERT_THAT(h_rx2, NotNull());
  const int32_t cvg2 = Vattach(h_rx2->hdf_file, -1, "w");
  ASSERT_THAT(cvg2, Ne(FAIL));
  EXPECT_THAT(Vsetclass(cvg2, _HDF_CDF), Eq(SUCCEED));
  const int32_t vvg2 = Vattach(h_rx2->hdf_file, -1, "w");
  ASSERT_THAT(vvg2, Ne(FAIL));
  EXPECT_THAT(Vsetclass(vvg2, _HDF_VARIABLE), Eq(SUCCEED));
  EXPECT_THAT(Vsetname(vvg2, "v1"), Eq(SUCCEED));
  EXPECT_THAT(Vaddtagref(vvg2, DFTAG_VH, 9906), Ne(FAIL));
  EXPECT_THAT(Vinsert(cvg2, vvg2), Ne(FAIL));
  EXPECT_THAT(Vdetach(vvg2), Eq(SUCCEED));
  EXPECT_THAT(Vdetach(cvg2), Eq(SUCCEED));
  EXPECT_THAT(hdf_read_xdr_cdf(nullptr, &h_rx2), Eq(FAIL));
  DetachLeakedVsetAtoms();
  EXPECT_THAT(NC_free_cdf(h_rx2), Eq(SUCCEED));
}

TEST_F(CdfTest, ReadOnlyClobberAndHdfCloseErrorBranches) {
  const std::string clobber_path = GetTempFilePath("clobber_ro.hdf");
  absl::Cleanup remove_clobber = [&clobber_path] {
    std::filesystem::remove(clobber_path);
  };

  {
    NC* wh = NC_new_cdf(clobber_path.c_str(), NC_CLOBBER);
    ASSERT_THAT(wh, NotNull());
    const int32_t fid = wh->hdf_file;

    // vg_bad_dd: has DFTAG_VERSION, 9911 (not on disk -> Hdeldd fails)
    const int32_t vg_bad_dd = Vattach(fid, -1, "w");
    ASSERT_THAT(vg_bad_dd, Ne(FAIL));
    EXPECT_THAT(Vaddtagref(vg_bad_dd, DFTAG_VERSION, 9911), Ne(FAIL));
    const int32_t ref_bad_dd = VQueryref(vg_bad_dd);
    EXPECT_THAT(Vdetach(vg_bad_dd), Eq(SUCCEED));

    // vg_parent: inserts vg_bad_dd
    const int32_t vg_parent = Vattach(fid, -1, "w");
    ASSERT_THAT(vg_parent, Ne(FAIL));
    const int32_t re_bad = Vattach(fid, ref_bad_dd, "w");
    ASSERT_THAT(re_bad, Ne(FAIL));
    EXPECT_THAT(Vinsert(vg_parent, re_bad), Ne(FAIL));
    const int32_t ref_parent = VQueryref(vg_parent);
    EXPECT_THAT(Vdetach(re_bad), Eq(SUCCEED));
    EXPECT_THAT(Vdetach(vg_parent), Eq(SUCCEED));

    // vs_ok: delete DFTAG_VS DD so VSdelete fails at Hdeldd(DFTAG_VS)
    const int32_t vs_ok = VSattach(fid, -1, "w");
    ASSERT_THAT(vs_ok, Ne(FAIL));
    char fld_f[] = "F";
    EXPECT_THAT(VSfdefine(vs_ok, fld_f, DFNT_INT32, 1), Eq(SUCCEED));
    EXPECT_THAT(VSsetfields(vs_ok, fld_f), Eq(SUCCEED));
    const int32_t one_val = 42;
    EXPECT_THAT(VSwrite(vs_ok, reinterpret_cast<const uint8_t*>(&one_val), 1,
                        FULL_INTERLACE),
                Eq(1));
    const int32_t vs_ok_ref = VSQueryref(vs_ok);
    EXPECT_THAT(VSdetach(vs_ok), Eq(SUCCEED));
    EXPECT_THAT(Hdeldd(fid, DFTAG_VS, static_cast<uint16_t>(vs_ok_ref)),
                Eq(SUCCEED));

    // vg_with_vh
    const int32_t vg_with_vh = Vattach(fid, -1, "w");
    ASSERT_THAT(vg_with_vh, Ne(FAIL));
    EXPECT_THAT(Vaddtagref(vg_with_vh, DFTAG_VH, vs_ok_ref), Ne(FAIL));
    const int32_t ref_with_vh = VQueryref(vg_with_vh);
    EXPECT_THAT(Vdetach(vg_with_vh), Eq(SUCCEED));

    // vg_dangling_child_vg and vg_dangling_child_vh
    const int32_t vg_d_vg = Vattach(fid, -1, "w");
    ASSERT_THAT(vg_d_vg, Ne(FAIL));
    EXPECT_THAT(Vaddtagref(vg_d_vg, DFTAG_VG, 9914), Ne(FAIL));
    const int32_t ref_dangling_child_vg = VQueryref(vg_d_vg);
    EXPECT_THAT(Vdetach(vg_d_vg), Eq(SUCCEED));

    const int32_t vg_d_vh = Vattach(fid, -1, "w");
    ASSERT_THAT(vg_d_vh, Ne(FAIL));
    EXPECT_THAT(Vaddtagref(vg_d_vh, DFTAG_VH, 9915), Ne(FAIL));
    const int32_t ref_dangling_child_vh = VQueryref(vg_d_vh);
    EXPECT_THAT(Vdetach(vg_d_vh), Eq(SUCCEED));

    // vg_u_dangling_vg
    const int32_t vg_u_dangling_vg = Vattach(fid, -1, "w");
    ASSERT_THAT(vg_u_dangling_vg, Ne(FAIL));
    EXPECT_THAT(Vaddtagref(vg_u_dangling_vg, DFTAG_VG, 9912), Ne(FAIL));
    const int32_t ref_u_dangling_vg = VQueryref(vg_u_dangling_vg);
    EXPECT_THAT(Vdetach(vg_u_dangling_vg), Eq(SUCCEED));

    // udim_dangling_vh and vg_u_dangling_vh
    const int32_t udim_dangling_vh = Vattach(fid, -1, "w");
    ASSERT_THAT(udim_dangling_vh, Ne(FAIL));
    EXPECT_THAT(Vsetclass(udim_dangling_vh, _HDF_UDIMENSION), Eq(SUCCEED));
    EXPECT_THAT(Vaddtagref(udim_dangling_vh, DFTAG_VH, 9913), Ne(FAIL));
    const int32_t vg_u_dangling_vh = Vattach(fid, -1, "w");
    ASSERT_THAT(vg_u_dangling_vh, Ne(FAIL));
    EXPECT_THAT(Vinsert(vg_u_dangling_vh, udim_dangling_vh), Ne(FAIL));
    const int32_t ref_u_dangling_vh = VQueryref(vg_u_dangling_vh);
    EXPECT_THAT(Vdetach(udim_dangling_vh), Eq(SUCCEED));
    EXPECT_THAT(Vdetach(vg_u_dangling_vh), Eq(SUCCEED));

    // vs_wrong_fld, udim_wrong_fld, vg_u_wrong_fld
    const int32_t vs_wrong_fld = VSattach(fid, -1, "w");
    ASSERT_THAT(vs_wrong_fld, Ne(FAIL));
    EXPECT_THAT(VSsetclass(vs_wrong_fld, DIM_VALS), Eq(SUCCEED));
    char wrong_fld[] = "WrongField";
    EXPECT_THAT(VSfdefine(vs_wrong_fld, wrong_fld, DFNT_INT32, 1), Eq(SUCCEED));
    EXPECT_THAT(VSsetfields(vs_wrong_fld, wrong_fld), Eq(SUCCEED));
    EXPECT_THAT(
        VSwrite(vs_wrong_fld, reinterpret_cast<const uint8_t*>(&one_val), 1,
                FULL_INTERLACE),
        Eq(1));
    const int32_t vs_wrong_ref = VSQueryref(vs_wrong_fld);
    EXPECT_THAT(VSdetach(vs_wrong_fld), Eq(SUCCEED));

    const int32_t udim_wrong_fld = Vattach(fid, -1, "w");
    ASSERT_THAT(udim_wrong_fld, Ne(FAIL));
    EXPECT_THAT(Vsetclass(udim_wrong_fld, _HDF_UDIMENSION), Eq(SUCCEED));
    EXPECT_THAT(Vaddtagref(udim_wrong_fld, DFTAG_VH, vs_wrong_ref), Ne(FAIL));

    const int32_t vg_u_wrong_fld = Vattach(fid, -1, "w");
    ASSERT_THAT(vg_u_wrong_fld, Ne(FAIL));
    EXPECT_THAT(Vinsert(vg_u_wrong_fld, udim_wrong_fld), Ne(FAIL));
    const int32_t ref_u_wrong_fld = VQueryref(vg_u_wrong_fld);
    EXPECT_THAT(Vdetach(udim_wrong_fld), Eq(SUCCEED));
    EXPECT_THAT(Vdetach(vg_u_wrong_fld), Eq(SUCCEED));

    // 1. hdf_vg_clobber error paths (cdf.c:1958-1959, 1967-1968, 1979-1980)
    EXPECT_THAT(hdf_vg_clobber(wh, ref_bad_dd), Eq(FAIL));
    EXPECT_THAT(hdf_vg_clobber(wh, ref_parent), Eq(FAIL));
    EXPECT_THAT(hdf_vg_clobber(wh, ref_with_vh), Eq(FAIL));
    (void)Hdeldd(fid, DFTAG_VH, static_cast<uint16_t>(vs_ok_ref));

    // 2. hdf_cdf_clobber error paths (cdf.c:2017, 2043-2044, 2050-2051,
    // 2057-2058)
    wh->vgid = 9919;
    EXPECT_THAT(hdf_cdf_clobber(wh), Eq(FAIL));
    wh->vgid = ref_dangling_child_vg;
    EXPECT_THAT(hdf_cdf_clobber(wh), Eq(FAIL));
    wh->vgid = ref_dangling_child_vh;
    EXPECT_THAT(hdf_cdf_clobber(wh), Eq(FAIL));
    wh->vgid = ref_bad_dd;
    EXPECT_THAT(hdf_cdf_clobber(wh), Eq(FAIL));

    // 3. hdf_close error paths (cdf.c:2138-2139, 2153-2155, 2166-2167)
    wh->flags = NC_NDIRTY;
    wh->vgid = ref_u_dangling_vg;
    EXPECT_THAT(hdf_close(wh), Eq(FAIL));

    wh->flags = NC_NDIRTY;
    wh->vgid = ref_u_dangling_vh;
    EXPECT_THAT(hdf_close(wh), Eq(FAIL));

    wh->flags = NC_NDIRTY;
    wh->vgid = ref_u_wrong_fld;
    EXPECT_THAT(hdf_close(wh), Eq(FAIL));

    wh->flags = 0;
    wh->vgid = 0;
    DetachLeakedVsetAtoms();

    EXPECT_THAT(NC_free_cdf(wh), Eq(SUCCEED));
    HEclear();
  }

  // Now open read-only for UNKNOWN var Hputelement write failure (cdf.c:855)
  NC* roh = NC_new_cdf(clobber_path.c_str(), NC_NOWRITE);
  ASSERT_THAT(roh, NotNull());
  NC_var* rv = NC_new_var("rv", NC_LONG, 0, nullptr);
  ASSERT_THAT(rv, NotNull());
  rv->ndg_ref = Hnewref(roh->hdf_file);
  rv->var_type = UNKNOWN;
  EXPECT_THAT(hdf_write_var(nullptr, roh, &rv), Eq(FAIL));
  NC_free_var(rv);
  EXPECT_THAT(NC_free_cdf(roh), Eq(SUCCEED));
  HEclear();
}

TEST_F(CdfTest, VsetInMemoryFaultInjectionAndRemainingErrorBranches) {
  // 1. DFdiwrite < 0 in hdf_write_var (cdf.c:918)
  const std::string p_write_fail = GetTempFilePath("var_write_fail.hdf");
  absl::Cleanup remove_wf = [&p_write_fail] {
    std::filesystem::remove(p_write_fail);
  };
  NC* wh1 = NC_new_cdf(p_write_fail.c_str(), NC_CLOBBER);
  ASSERT_THAT(wh1, NotNull());
  NC_dim* d0 = NC_new_dim("d0", 4);
  ASSERT_THAT(d0, NotNull());
  EXPECT_THAT(hdf_write_dim(nullptr, wh1, &d0, 0), Ge(0));
  NC_dim* dim_ptrs[1] = {d0};
  NC_array dim_arr{};
  dim_arr.count = 1;
  dim_arr.szof = sizeof(NC_dim*);
  dim_arr.values = reinterpret_cast<uint8_t*>(dim_ptrs);
  wh1->dims = &dim_arr;

  int zero_dim = 0;
  NC_var* v_bad_ndg = NC_new_var("v_bad_ndg", NC_LONG, 1, &zero_dim);
  ASSERT_THAT(v_bad_ndg, NotNull());
  v_bad_ndg->cdf = wh1;
  ASSERT_THAT(NC_var_shape(v_bad_ndg, wh1->dims), Ne(-1));
  v_bad_ndg->data_ref = 100;
  v_bad_ndg->ndg_ref = 0;  // ref == 0 causes Hputelement in DFdiwrite to fail
  EXPECT_THAT(hdf_write_var(nullptr, wh1, &v_bad_ndg), Eq(FAIL));
  NC_free_var(v_bad_ndg);
  NC_free_dim(d0);
  wh1->dims = nullptr;
  EXPECT_THAT(NC_free_cdf(wh1), Eq(SUCCEED));
  HEclear();

  // 2. Vfindclass(_HDF_CDF) == FAIL in hdf_read_xdr_cdf (cdf.c:1836)
  const std::string p_nocdf = GetTempFilePath("no_cdf_vg.hdf");
  absl::Cleanup remove_nocdf = [&p_nocdf] { std::filesystem::remove(p_nocdf); };
  NC* fh = NC_new_cdf(p_nocdf.c_str(), NC_CLOBBER);
  ASSERT_THAT(fh, NotNull());
  EXPECT_THAT(hdf_read_xdr_cdf(nullptr, &fh), Eq(FAIL));
  EXPECT_THAT(NC_free_cdf(fh), Eq(SUCCEED));
  HEclear();

  // 3. Vdelete == FAIL in hdf_cdf_clobber on read-only file (cdf.c:2070)
  const std::string p_clobber_ro = GetTempFilePath("clobber_empty_ro.hdf");
  absl::Cleanup remove_clobber_ro = [&p_clobber_ro] {
    std::filesystem::remove(p_clobber_ro);
  };
  int32_t ref_empty_vg = 0;
  {
    NC* ch = NC_new_cdf(p_clobber_ro.c_str(), NC_CLOBBER);
    ASSERT_THAT(ch, NotNull());
    const int32_t vg_e = Vattach(ch->hdf_file, -1, "w");
    ASSERT_THAT(vg_e, Ne(FAIL));
    EXPECT_THAT(Vsetname(vg_e, "empty_vg"), Eq(SUCCEED));
    ref_empty_vg = VQueryref(vg_e);
    EXPECT_THAT(Vdetach(vg_e), Eq(SUCCEED));
    EXPECT_THAT(NC_free_cdf(ch), Eq(SUCCEED));
  }
  {
    NC* roh2 = NC_new_cdf(p_clobber_ro.c_str(), NC_NOWRITE);
    ASSERT_THAT(roh2, NotNull());
    roh2->vgid = ref_empty_vg;
    EXPECT_THAT(hdf_cdf_clobber(roh2), Eq(FAIL));
    roh2->vgid = 0;
    EXPECT_THAT(NC_free_cdf(roh2), Eq(SUCCEED));
    HEclear();
  }

  // 4. In-memory fault injection via VGROUP and VDATA otag / field mutation
  const std::string p_fault = GetTempFilePath("fault_inj.hdf");
  absl::Cleanup remove_fault = [&p_fault] { std::filesystem::remove(p_fault); };
  NC* dh = NC_new_cdf(p_fault.c_str(), NC_CLOBBER);
  ASSERT_THAT(dh, NotNull());
  const int32_t fid = dh->hdf_file;

  auto WithBadVgOtag = [&](uint16_t ref, auto&& fn) {
    vginstance_t* v = vginst(fid, ref);
    ASSERT_THAT(v, NotNull());
    v->vg->otag = 0;
    fn();
    v->vg->otag = DFTAG_VG;
    v->nattach = 0;
    DetachLeakedVsetAtoms();
  };

  auto WithBadVsOtag = [&](uint16_t vs_ref, auto&& fn) {
    vsinstance_t* w = vsinst(fid, vs_ref);
    ASSERT_THAT(w, NotNull());
    w->vs->otag = 0;
    fn();
    w->vs->otag = VSDESCTAG;
    w->vs->marked = 0;
    DetachLeakedVsetAtoms();
  };

  auto Make1RecVdata = [&](const char* vsclass, int32_t nt) -> uint16_t {
    const int32_t vs = VSattach(fid, -1, "w");
    EXPECT_THAT(vs, Ne(FAIL));
    EXPECT_THAT(VSsetclass(vs, vsclass), Eq(SUCCEED));
    char fld[] = "Values";
    EXPECT_THAT(VSfdefine(vs, fld, nt, 1), Eq(SUCCEED));
    EXPECT_THAT(VSsetfields(vs, fld), Eq(SUCCEED));
    int32_t one_val = 1;
    EXPECT_THAT(VSwrite(vs, reinterpret_cast<const uint8_t*>(&one_val), 1,
                        FULL_INTERLACE),
                Eq(1));
    const int32_t ref = VSQueryref(vs);
    EXPECT_THAT(VSdetach(vs), Eq(SUCCEED));
    return static_cast<uint16_t>(ref);
  };

  // 4a. Vinquire == FAIL in hdf_read_dims (cdf.c:1203)
  const int32_t top_d1 = Vattach(fid, -1, "w");
  ASSERT_THAT(top_d1, Ne(FAIL));
  EXPECT_THAT(Vsetclass(top_d1, _HDF_CDF), Eq(SUCCEED));
  const int32_t dim_vg = Vattach(fid, -1, "w");
  ASSERT_THAT(dim_vg, Ne(FAIL));
  EXPECT_THAT(Vsetclass(dim_vg, _HDF_DIMENSION), Eq(SUCCEED));
  EXPECT_THAT(Vsetname(dim_vg, "d_otag"), Eq(SUCCEED));
  const uint16_t ref_dim_vg = static_cast<uint16_t>(VQueryref(dim_vg));
  EXPECT_THAT(Vinsert(top_d1, dim_vg), Ne(FAIL));
  const int32_t ref_top_d1 = VQueryref(top_d1);
  EXPECT_THAT(Vdetach(dim_vg), Eq(SUCCEED));
  EXPECT_THAT(Vdetach(top_d1), Eq(SUCCEED));

  const int32_t top_d1_k = Vattach(fid, ref_top_d1, "r");
  ASSERT_THAT(top_d1_k, Ne(FAIL));
  WithBadVgOtag(ref_dim_vg, [&] {
    EXPECT_THAT(hdf_read_dims(nullptr, dh, top_d1_k), Eq(FAIL));
  });

  // 4b. Vdetach(dim/var) == FAIL (cdf.c:1305, 1793, 2190)
  const int32_t top_o = Vattach(fid, -1, "w");
  ASSERT_THAT(top_o, Ne(FAIL));
  EXPECT_THAT(Vsetclass(top_o, _HDF_CDF), Eq(SUCCEED));
  const int32_t other_vg = Vattach(fid, -1, "w");
  ASSERT_THAT(other_vg, Ne(FAIL));
  EXPECT_THAT(Vsetclass(other_vg, "OTHER_VG"), Eq(SUCCEED));
  EXPECT_THAT(Vsetname(other_vg, "other"), Eq(SUCCEED));
  const uint16_t ref_other_vg = static_cast<uint16_t>(VQueryref(other_vg));
  EXPECT_THAT(Vinsert(top_o, other_vg), Ne(FAIL));
  const int32_t ref_top_o = VQueryref(top_o);
  EXPECT_THAT(Vdetach(other_vg), Eq(SUCCEED));
  EXPECT_THAT(Vdetach(top_o), Eq(SUCCEED));

  const int32_t top_o_k1 = Vattach(fid, ref_top_o, "r");
  ASSERT_THAT(top_o_k1, Ne(FAIL));
  WithBadVgOtag(ref_other_vg, [&] {
    EXPECT_THAT(hdf_read_dims(nullptr, dh, top_o_k1), Eq(FAIL));
  });

  const int32_t top_o_k2 = Vattach(fid, ref_top_o, "r");
  ASSERT_THAT(top_o_k2, Ne(FAIL));
  WithBadVgOtag(ref_other_vg, [&] {
    EXPECT_THAT(hdf_read_vars(nullptr, dh, top_o_k2), Eq(FAIL));
  });

  dh->flags = NC_NDIRTY;
  dh->vgid = ref_top_o;
  WithBadVgOtag(ref_other_vg, [&] { EXPECT_THAT(hdf_close(dh), Eq(FAIL)); });
  dh->flags = 0;
  dh->vgid = 0;

  // 4c. Vntagrefs == FAIL (cdf.c:1556, 1941, 2022)
  const int32_t other_k = Vattach(fid, ref_other_vg, "r");
  ASSERT_THAT(other_k, Ne(FAIL));
  vginst(fid, ref_other_vg)->vg->otag = 0;
  EXPECT_THAT(hdf_read_vars(nullptr, dh, other_k), Eq(FAIL));
  vginst(fid, ref_other_vg)->vg->otag = DFTAG_VG;
  EXPECT_THAT(Vdetach(other_k), Eq(SUCCEED));

  WithBadVgOtag(ref_other_vg, [&] {
    EXPECT_THAT(hdf_vg_clobber(dh, ref_other_vg), Eq(FAIL));
  });

  dh->vgid = ref_other_vg;
  WithBadVgOtag(ref_other_vg,
                [&] { EXPECT_THAT(hdf_cdf_clobber(dh), Eq(FAIL)); });
  dh->vgid = 0;

  // 4d. Vinquire and Vdetach errors in variable hierarchy (cdf.c:1588, 1618,
  // 1630)
  const int32_t top_v1 = Vattach(fid, -1, "w");
  ASSERT_THAT(top_v1, Ne(FAIL));
  EXPECT_THAT(Vsetclass(top_v1, _HDF_CDF), Eq(SUCCEED));
  const int32_t var_vg = Vattach(fid, -1, "w");
  ASSERT_THAT(var_vg, Ne(FAIL));
  EXPECT_THAT(Vsetclass(var_vg, _HDF_VARIABLE), Eq(SUCCEED));
  EXPECT_THAT(Vsetname(var_vg, "v_otag"), Eq(SUCCEED));
  const int32_t sub_dim_vg = Vattach(fid, -1, "w");
  ASSERT_THAT(sub_dim_vg, Ne(FAIL));
  EXPECT_THAT(Vsetclass(sub_dim_vg, _HDF_DIMENSION), Eq(SUCCEED));
  EXPECT_THAT(Vsetname(sub_dim_vg, "sd"), Eq(SUCCEED));
  const uint16_t ref_sub_dim = static_cast<uint16_t>(VQueryref(sub_dim_vg));
  EXPECT_THAT(Vinsert(var_vg, sub_dim_vg), Ne(FAIL));
  const uint16_t ref_var_vg = static_cast<uint16_t>(VQueryref(var_vg));
  EXPECT_THAT(Vinsert(top_v1, var_vg), Ne(FAIL));
  const int32_t ref_top_v1 = VQueryref(top_v1);
  EXPECT_THAT(Vdetach(sub_dim_vg), Eq(SUCCEED));
  EXPECT_THAT(Vdetach(var_vg), Eq(SUCCEED));
  EXPECT_THAT(Vdetach(top_v1), Eq(SUCCEED));

  const int32_t top_v1_k1 = Vattach(fid, ref_top_v1, "r");
  ASSERT_THAT(top_v1_k1, Ne(FAIL));
  WithBadVgOtag(ref_var_vg, [&] {
    EXPECT_THAT(hdf_read_vars(nullptr, dh, top_v1_k1), Eq(FAIL));
  });

  const int32_t top_v1_k2 = Vattach(fid, ref_top_v1, "r");
  ASSERT_THAT(top_v1_k2, Ne(FAIL));
  WithBadVgOtag(ref_sub_dim, [&] {
    EXPECT_THAT(hdf_read_vars(nullptr, dh, top_v1_k2), Eq(FAIL));
  });

  const int32_t top_v2 = Vattach(fid, -1, "w");
  ASSERT_THAT(top_v2, Ne(FAIL));
  EXPECT_THAT(Vsetclass(top_v2, _HDF_CDF), Eq(SUCCEED));
  const int32_t var_vg2 = Vattach(fid, -1, "w");
  ASSERT_THAT(var_vg2, Ne(FAIL));
  EXPECT_THAT(Vsetclass(var_vg2, _HDF_VARIABLE), Eq(SUCCEED));
  EXPECT_THAT(Vsetname(var_vg2, "v_otag2"), Eq(SUCCEED));
  const int32_t sub_other = Vattach(fid, -1, "w");
  ASSERT_THAT(sub_other, Ne(FAIL));
  EXPECT_THAT(Vsetclass(sub_other, "OTHER_VG"), Eq(SUCCEED));
  EXPECT_THAT(Vsetname(sub_other, "so"), Eq(SUCCEED));
  const uint16_t ref_sub_other = static_cast<uint16_t>(VQueryref(sub_other));
  EXPECT_THAT(Vinsert(var_vg2, sub_other), Ne(FAIL));
  EXPECT_THAT(Vinsert(top_v2, var_vg2), Ne(FAIL));
  const int32_t ref_top_v2 = VQueryref(top_v2);
  EXPECT_THAT(Vdetach(sub_other), Eq(SUCCEED));
  EXPECT_THAT(Vdetach(var_vg2), Eq(SUCCEED));
  EXPECT_THAT(Vdetach(top_v2), Eq(SUCCEED));

  const int32_t top_v2_k = Vattach(fid, ref_top_v2, "r");
  ASSERT_THAT(top_v2_k, Ne(FAIL));
  WithBadVgOtag(ref_sub_other, [&] {
    EXPECT_THAT(hdf_read_vars(nullptr, dh, top_v2_k), Eq(FAIL));
  });

  // 4e. VDATA otag == 0 (cdf.c:1224, 1369, 1435, 1650, 2181)
  const uint16_t vs_dimvals = Make1RecVdata(DIM_VALS, DFNT_INT32);
  const int32_t top_dv = Vattach(fid, -1, "w");
  ASSERT_THAT(top_dv, Ne(FAIL));
  EXPECT_THAT(Vsetclass(top_dv, _HDF_CDF), Eq(SUCCEED));
  const int32_t dim_dv = Vattach(fid, -1, "w");
  ASSERT_THAT(dim_dv, Ne(FAIL));
  EXPECT_THAT(Vsetclass(dim_dv, _HDF_DIMENSION), Eq(SUCCEED));
  EXPECT_THAT(Vsetname(dim_dv, "d_dv"), Eq(SUCCEED));
  EXPECT_THAT(Vaddtagref(dim_dv, DFTAG_VH, vs_dimvals), Ne(FAIL));
  EXPECT_THAT(Vinsert(top_dv, dim_dv), Ne(FAIL));
  const int32_t ref_top_dv = VQueryref(top_dv);
  EXPECT_THAT(Vdetach(dim_dv), Eq(SUCCEED));
  EXPECT_THAT(Vdetach(top_dv), Eq(SUCCEED));

  const int32_t top_dv_k = Vattach(fid, ref_top_dv, "r");
  ASSERT_THAT(top_dv_k, Ne(FAIL));
  WithBadVsOtag(vs_dimvals, [&] {
    EXPECT_THAT(hdf_read_dims(nullptr, dh, top_dv_k), Eq(FAIL));
  });

  const std::string p_numattrs = GetTempFilePath("num_attrs.hdf");
  absl::Cleanup remove_na = [&p_numattrs] {
    std::filesystem::remove(p_numattrs);
  };
  NC* h_na = NC_new_cdf(p_numattrs.c_str(), NC_CLOBBER);
  ASSERT_THAT(h_na, NotNull());
  const int32_t vs_na = VSattach(h_na->hdf_file, -1, "w");
  ASSERT_THAT(vs_na, Ne(FAIL));
  EXPECT_THAT(VSsetclass(vs_na, _HDF_ATTRIBUTE), Eq(SUCCEED));
  char fld_na[] = "Values";
  EXPECT_THAT(VSfdefine(vs_na, fld_na, DFNT_INT32, 1), Eq(SUCCEED));
  EXPECT_THAT(VSsetfields(vs_na, fld_na), Eq(SUCCEED));
  int32_t na_val = 1;
  EXPECT_THAT(VSwrite(vs_na, reinterpret_cast<const uint8_t*>(&na_val), 1,
                      FULL_INTERLACE),
              Eq(1));
  const int32_t ref_vs_na = VSQueryref(vs_na);
  EXPECT_THAT(VSdetach(vs_na), Eq(SUCCEED));

  const int32_t cdf_vg_na = Vattach(h_na->hdf_file, -1, "w");
  ASSERT_THAT(cdf_vg_na, Ne(FAIL));
  EXPECT_THAT(Vsetclass(cdf_vg_na, _HDF_CDF), Eq(SUCCEED));
  EXPECT_THAT(Vaddtagref(cdf_vg_na, DFTAG_VH, ref_vs_na), Ne(FAIL));
  EXPECT_THAT(Vdetach(cdf_vg_na), Eq(SUCCEED));

  vsinstance_t* w_na = vsinst(h_na->hdf_file, static_cast<uint16_t>(ref_vs_na));
  ASSERT_THAT(w_na, NotNull());
  w_na->vs->otag = 0;
  EXPECT_THAT(hdf_read_xdr_cdf(nullptr, &h_na), Eq(SUCCEED));
  w_na->vs->otag = VSDESCTAG;
  DetachLeakedVsetAtoms();
  EXPECT_THAT(NC_free_cdf(h_na), Eq(SUCCEED));
  HEclear();

  const uint16_t vs_attr = Make1RecVdata(_HDF_ATTRIBUTE, DFNT_INT32);
  const int32_t attr_vg = Vattach(fid, -1, "w");
  ASSERT_THAT(attr_vg, Ne(FAIL));
  EXPECT_THAT(Vsetclass(attr_vg, _HDF_ATTRIBUTE), Eq(SUCCEED));
  EXPECT_THAT(Vaddtagref(attr_vg, DFTAG_VH, vs_attr), Ne(FAIL));
  const int32_t ref_attr_vg = VQueryref(attr_vg);
  EXPECT_THAT(Vdetach(attr_vg), Eq(SUCCEED));

  const int32_t attr_vg_k = Vattach(fid, ref_attr_vg, "r");
  ASSERT_THAT(attr_vg_k, Ne(FAIL));
  WithBadVsOtag(vs_attr, [&] {
    EXPECT_THAT(hdf_read_attrs(nullptr, dh, attr_vg_k), IsNull());
  });

  const uint16_t vs_sds = Make1RecVdata(_HDF_SDSVAR, DFNT_INT32);
  const int32_t top_sv = Vattach(fid, -1, "w");
  ASSERT_THAT(top_sv, Ne(FAIL));
  EXPECT_THAT(Vsetclass(top_sv, _HDF_CDF), Eq(SUCCEED));
  const int32_t var_sv = Vattach(fid, -1, "w");
  ASSERT_THAT(var_sv, Ne(FAIL));
  EXPECT_THAT(Vsetclass(var_sv, _HDF_VARIABLE), Eq(SUCCEED));
  EXPECT_THAT(Vsetname(var_sv, "v_sv"), Eq(SUCCEED));
  EXPECT_THAT(Vaddtagref(var_sv, DFTAG_VH, vs_sds), Ne(FAIL));
  EXPECT_THAT(Vinsert(top_sv, var_sv), Ne(FAIL));
  const int32_t ref_top_sv = VQueryref(top_sv);
  EXPECT_THAT(Vdetach(var_sv), Eq(SUCCEED));
  EXPECT_THAT(Vdetach(top_sv), Eq(SUCCEED));

  const int32_t top_sv_k = Vattach(fid, ref_top_sv, "r");
  ASSERT_THAT(top_sv_k, Ne(FAIL));
  WithBadVsOtag(vs_sds, [&] {
    EXPECT_THAT(hdf_read_vars(nullptr, dh, top_sv_k), Eq(FAIL));
  });

  const uint16_t vs_oth = Make1RecVdata("OTHER_VS", DFNT_INT32);
  const int32_t top_uo = Vattach(fid, -1, "w");
  ASSERT_THAT(top_uo, Ne(FAIL));
  EXPECT_THAT(Vsetclass(top_uo, _HDF_CDF), Eq(SUCCEED));
  const int32_t udim_uo = Vattach(fid, -1, "w");
  ASSERT_THAT(udim_uo, Ne(FAIL));
  EXPECT_THAT(Vsetclass(udim_uo, _HDF_UDIMENSION), Eq(SUCCEED));
  EXPECT_THAT(Vaddtagref(udim_uo, DFTAG_VH, vs_oth), Ne(FAIL));
  EXPECT_THAT(Vinsert(top_uo, udim_uo), Ne(FAIL));
  const int32_t ref_top_uo = VQueryref(top_uo);
  EXPECT_THAT(Vdetach(udim_uo), Eq(SUCCEED));
  EXPECT_THAT(Vdetach(top_uo), Eq(SUCCEED));

  dh->flags = NC_NDIRTY;
  dh->vgid = ref_top_uo;
  WithBadVsOtag(vs_oth, [&] { EXPECT_THAT(hdf_close(dh), Eq(FAIL)); });
  dh->flags = 0;
  dh->vgid = 0;

  // 4f. VSseek == FAIL on 0-field DIM_VALS01 (cdf.c:1245) and VSread != 1 in
  // hdf_close (cdf.c:2175)
  const uint16_t vs_dv01 = Make1RecVdata(DIM_VALS01, DFNT_INT32);
  const int32_t top_z1 = Vattach(fid, -1, "w");
  ASSERT_THAT(top_z1, Ne(FAIL));
  EXPECT_THAT(Vsetclass(top_z1, _HDF_CDF), Eq(SUCCEED));
  const int32_t dim_z = Vattach(fid, -1, "w");
  ASSERT_THAT(dim_z, Ne(FAIL));
  EXPECT_THAT(Vsetclass(dim_z, _HDF_DIMENSION), Eq(SUCCEED));
  EXPECT_THAT(Vsetname(dim_z, "d_z"), Eq(SUCCEED));
  EXPECT_THAT(Vaddtagref(dim_z, DFTAG_VH, vs_dv01), Ne(FAIL));
  EXPECT_THAT(Vinsert(top_z1, dim_z), Ne(FAIL));
  const int32_t ref_top_z1 = VQueryref(top_z1);
  EXPECT_THAT(Vdetach(dim_z), Eq(SUCCEED));
  EXPECT_THAT(Vdetach(top_z1), Eq(SUCCEED));

  vsinstance_t* w_z1 = vsinst(fid, vs_dv01);
  ASSERT_THAT(w_z1, NotNull());
  w_z1->vs->wlist.n = 0;  // VSseek checks vs->wlist.n <= 0 (vrw.c:121)
  const int32_t top_z1_k = Vattach(fid, ref_top_z1, "r");
  ASSERT_THAT(top_z1_k, Ne(FAIL));
  EXPECT_THAT(hdf_read_dims(nullptr, dh, top_z1_k), Eq(FAIL));
  w_z1->vs->wlist.n = 1;
  DetachLeakedVsetAtoms();

  // VSQuerycount == FAIL in hdf_read_dims (cdf.c:1224)
  vsinst(fid, vs_dimvals)->vs->nvertices = FAIL;
  const int32_t top_dv_cnt = Vattach(fid, ref_top_dv, "r");
  ASSERT_THAT(top_dv_cnt, Ne(FAIL));
  EXPECT_THAT(hdf_read_dims(nullptr, dh, top_dv_cnt), Eq(FAIL));
  vsinst(fid, vs_dimvals)->vs->nvertices = 1;
  DetachLeakedVsetAtoms();

  const int32_t top_z2 = Vattach(fid, -1, "w");
  ASSERT_THAT(top_z2, Ne(FAIL));
  EXPECT_THAT(Vsetclass(top_z2, _HDF_CDF), Eq(SUCCEED));
  const int32_t udim_z = Vattach(fid, -1, "w");
  ASSERT_THAT(udim_z, Ne(FAIL));
  EXPECT_THAT(Vsetclass(udim_z, _HDF_UDIMENSION), Eq(SUCCEED));
  EXPECT_THAT(Vaddtagref(udim_z, DFTAG_VH, vs_dv01), Ne(FAIL));
  EXPECT_THAT(Vinsert(top_z2, udim_z), Ne(FAIL));
  const int32_t ref_top_z2 = VQueryref(top_z2);
  EXPECT_THAT(Vdetach(udim_z), Eq(SUCCEED));
  EXPECT_THAT(Vdetach(top_z2), Eq(SUCCEED));

  w_z1->vs->nvertices = 0;
  dh->flags = NC_NDIRTY;
  dh->vgid = ref_top_z2;
  EXPECT_THAT(hdf_close(dh), Eq(FAIL));
  w_z1->vs->nvertices = 1;
  dh->flags = 0;
  dh->vgid = 0;
  DetachLeakedVsetAtoms();

  // 4g. VFfieldtype == FAIL, hdf_unmap_type == FAIL, and VSread == FAIL
  // (cdf.c:1438, 1441, 1451)
  const uint16_t vs_u16 = Make1RecVdata(_HDF_ATTRIBUTE, DFNT_INT32);
  const int32_t attr_u16_vg = Vattach(fid, -1, "w");
  ASSERT_THAT(attr_u16_vg, Ne(FAIL));
  EXPECT_THAT(Vsetclass(attr_u16_vg, _HDF_ATTRIBUTE), Eq(SUCCEED));
  EXPECT_THAT(Vaddtagref(attr_u16_vg, DFTAG_VH, vs_u16), Ne(FAIL));
  const int32_t ref_u16_vg = VQueryref(attr_u16_vg);
  EXPECT_THAT(Vdetach(attr_u16_vg), Eq(SUCCEED));

  const int32_t attr_u16_k1 = Vattach(fid, ref_u16_vg, "r");
  ASSERT_THAT(attr_u16_k1, Ne(FAIL));
  vsinstance_t* w_u16 = vsinst(fid, vs_u16);
  ASSERT_THAT(w_u16, NotNull());
  w_u16->vs->wlist.n = 0;
  EXPECT_THAT(hdf_read_attrs(nullptr, dh, attr_u16_k1), IsNull());
  w_u16->vs->wlist.n = 1;
  DetachLeakedVsetAtoms();

  const int32_t attr_u16_k2 = Vattach(fid, ref_u16_vg, "r");
  ASSERT_THAT(attr_u16_k2, Ne(FAIL));
  w_u16->vs->wlist.type[0] = 99;
  EXPECT_THAT(hdf_read_attrs(nullptr, dh, attr_u16_k2), IsNull());
  w_u16->vs->wlist.type[0] = DFNT_INT32;
  DetachLeakedVsetAtoms();

  const int32_t attr_u16_k3 = Vattach(fid, ref_u16_vg, "r");
  ASSERT_THAT(attr_u16_k3, Ne(FAIL));
  w_u16->vs->nvertices = 0;
  EXPECT_THAT(hdf_read_attrs(nullptr, dh, attr_u16_k3), IsNull());
  w_u16->vs->nvertices = 1;
  DetachLeakedVsetAtoms();

  // 4h. Vdetach(vg) == FAIL in hdf_close (cdf.c:2197)
  const int32_t empty_close_vg = Vattach(fid, -1, "w");
  ASSERT_THAT(empty_close_vg, Ne(FAIL));
  EXPECT_THAT(Vsetclass(empty_close_vg, _HDF_CDF), Eq(SUCCEED));
  const int32_t ref_empty_close = VQueryref(empty_close_vg);
  EXPECT_THAT(Vdetach(empty_close_vg), Eq(SUCCEED));
  dh->flags = NC_NDIRTY;
  dh->vgid = ref_empty_close;
  WithBadVgOtag(static_cast<uint16_t>(ref_empty_close),
                [&] { EXPECT_THAT(hdf_close(dh), Eq(FAIL)); });
  dh->flags = 0;
  dh->vgid = 0;

  // 4j. Hgetelement == FAIL in hdf_conv_scales (cdf.c:1122)
  const auto ext_dir =
      std::filesystem::path(::testing::TempDir()) / "cdf_conv_missing_ext";
  std::filesystem::create_directories(ext_dir);
  absl::Cleanup remove_ext_dir = [&ext_dir] {
    std::filesystem::remove_all(ext_dir);
  };
  const uint16_t ext_ref = Hnewref(fid);
  const int32_t ext_aid = HXcreate(
      fid, DFTAG_SDS, ext_ref, (ext_dir / "scale.bin").string().c_str(), 0, 8);
  ASSERT_THAT(ext_aid, Ne(FAIL));
  ASSERT_THAT(Hendaccess(ext_aid), Eq(SUCCEED));
  std::filesystem::remove_all(ext_dir);

  NC_var ext_v{};
  ext_v.data_tag = DFTAG_SDS;
  ext_v.data_ref = ext_ref;
  ext_v.ndg_ref = ext_ref + 1;
  ext_v.data_offset = 0;
  ext_v.len = 4;
  NC_var* ext_v_ptrs[1] = {&ext_v};
  NC_array ext_v_arr{};
  ext_v_arr.count = 1;
  ext_v_arr.szof = sizeof(NC_var*);
  ext_v_arr.values = reinterpret_cast<uint8_t*>(ext_v_ptrs);
  dh->vars = &ext_v_arr;
  EXPECT_THAT(hdf_conv_scales(&dh), Eq(FAIL));
  dh->vars = nullptr;

  EXPECT_THAT(NC_free_cdf(dh), Eq(SUCCEED));
  HEclear();
}

// TODO(schwehr): Uncovered lines in cdf.c (~94.6% covered) are defensive OOM
// and Vgroup/Vdata write/read error fallback paths.

}  // namespace
}  // namespace hdf4
