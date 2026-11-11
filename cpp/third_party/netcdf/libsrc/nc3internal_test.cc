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

// Clean-room unit tests for //third_party/netcdf/libsrc/nc3internal.c.

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>  // NOLINT
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"

extern "C" {
#include "third_party/netcdf/include/nc.h"
#include "third_party/netcdf/include/nc3dispatch.h"
#include "third_party/netcdf/include/nc3internal.h"
#include "third_party/netcdf/include/ncdispatch.h"
}  // extern "C"

namespace netcdf {
namespace {

uint32_t ReadBe32(const unsigned char* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void WriteBe32(unsigned char* p, uint32_t v) {
  p[0] = static_cast<unsigned char>((v >> 24) & 0xFFU);
  p[1] = static_cast<unsigned char>((v >> 16) & 0xFFU);
  p[2] = static_cast<unsigned char>((v >> 8) & 0xFFU);
  p[3] = static_cast<unsigned char>(v & 0xFFU);
}

NC3_INFO* GetNc3InfoOrNull(int ncid) {
  NC* nc = nullptr;
  if (NC_check_id(ncid, &nc) != NC_NOERR || nc == nullptr) {
    return nullptr;
  }
  return NC3_DATA(nc);
}

void CleanOrphanNcId(int ncid) {
  NC* nc = nullptr;
  if (NC_check_id(ncid, &nc) == NC_NOERR && nc != nullptr) {
    del_from_NCList(nc);
    free_NC(nc);
  }
}

void WriteBytesToFile(const std::filesystem::path& path,
                      const std::vector<unsigned char>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out.is_open());
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  ASSERT_TRUE(out.good());
}

TEST(Nc3InternalTest, CkTypeChecksClassicCdf2AndCdf5Formats) {
  ASSERT_EQ(NC_NOERR, nc_initialize());

  for (int mode : {0, NC_64BIT_OFFSET}) {
    EXPECT_EQ(NC_EBADTYPE, nc3_cktype(mode, NC_NAT));
    EXPECT_EQ(NC_EBADTYPE, nc3_cktype(mode, -1));
    for (nc_type t = NC_BYTE; t <= NC_DOUBLE; ++t) {
      EXPECT_EQ(NC_NOERR, nc3_cktype(mode, t));
    }
    for (nc_type t = NC_UBYTE; t <= NC_STRING; ++t) {
      EXPECT_EQ(NC_EBADTYPE, nc3_cktype(mode, t));
    }
    EXPECT_EQ(NC_EBADTYPE, nc3_cktype(mode, NC_STRING + 1));
  }

  EXPECT_EQ(NC_EBADTYPE, nc3_cktype(NC_CDF5, NC_NAT));
  EXPECT_EQ(NC_EBADTYPE, nc3_cktype(NC_CDF5, -1));
  for (nc_type t = NC_BYTE; t < NC_STRING; ++t) {
    EXPECT_EQ(NC_NOERR, nc3_cktype(NC_CDF5, t));
  }
  EXPECT_EQ(NC_EBADTYPE, nc3_cktype(NC_CDF5, NC_STRING));
  EXPECT_EQ(NC_EBADTYPE, nc3_cktype(NC_CDF5, NC_VLEN));
}

TEST(Nc3InternalTest, NcxHowmanyComputesElementCountsForAllTypes) {
  ASSERT_EQ(NC_NOERR, nc_initialize());

  constexpr size_t kBufBytes = 64;
  EXPECT_EQ(64U, ncx_howmany(NC_BYTE, kBufBytes));
  EXPECT_EQ(64U, ncx_howmany(NC_CHAR, kBufBytes));
  EXPECT_EQ(32U, ncx_howmany(NC_SHORT, kBufBytes));
  EXPECT_EQ(16U, ncx_howmany(NC_INT, kBufBytes));
  EXPECT_EQ(16U, ncx_howmany(NC_FLOAT, kBufBytes));
  EXPECT_EQ(8U, ncx_howmany(NC_DOUBLE, kBufBytes));
  EXPECT_EQ(64U, ncx_howmany(NC_UBYTE, kBufBytes));
  EXPECT_EQ(32U, ncx_howmany(NC_USHORT, kBufBytes));
  EXPECT_EQ(16U, ncx_howmany(NC_UINT, kBufBytes));
  EXPECT_EQ(8U, ncx_howmany(NC_INT64, kBufBytes));
  EXPECT_EQ(8U, ncx_howmany(NC_UINT64, kBufBytes));

  EXPECT_EQ(0U, ncx_howmany(NC_INT, 0));
  EXPECT_EQ(1U, ncx_howmany(NC_INT, 7));
  EXPECT_EQ(0U, ncx_howmany(NC_DOUBLE, 7));
}

TEST(Nc3InternalTest, CreateOpenLifecycleChunkHintsAndErrorPaths) {
  TempFile existing("nc3_existing.nc");
  TempDir missing_parent("nc3_missing_dir");
  std::filesystem::remove_all(missing_parent.path());

  size_t hint = 4096;
  EXPECT_EQ(NC_EBADID, NC3_create(existing.c_str(), NC_CLOBBER, 0, 0, &hint,
                                  nullptr, NC3_dispatch_table, -1));
  EXPECT_EQ(NC_EBADID, NC3_open(existing.c_str(), NC_NOWRITE, 0, &hint, nullptr,
                                NC3_dispatch_table, -1));

  int bad_id = -1;
  EXPECT_EQ(NC_EINVAL,
            nc__create_mp(existing.c_str(), NC_CLOBBER, 0, 1, &hint, &bad_id));

  for (int format_flag : {0, NC_64BIT_OFFSET, NC_CDF5}) {
    TempFile tf("nc3_fmt_create.nc");
    hint = 1024;
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc__create(tf.c_str(), NC_CLOBBER | format_flag, 2048,
                                   &hint, file.ptr()));
    EXPECT_GE(hint, 256U);
    ASSERT_EQ(NC_NOERR, file.close());

    hint = 512;
    ASSERT_EQ(NC_NOERR, nc__open(tf.c_str(), NC_NOWRITE, &hint, file.ptr()));
    EXPECT_GE(hint, 256U);
    // nc__open_mp with basepe != 0 hits NC3_open unwind_alloc with nc3 == NULL.
    EXPECT_EQ(NC_EINVAL,
              nc__open_mp(tf.c_str(), NC_NOWRITE, 1, &hint, &bad_id));
    ASSERT_EQ(NC_NOERR, file.close());
  }

  // NC_NOCLOBBER on an existing file translates EEXIST -> NC_EEXIST.
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc_create(existing.c_str(), NC_CLOBBER, file.ptr()));
    ASSERT_EQ(NC_NOERR, file.close());
    EXPECT_EQ(NC_EEXIST, nc_create(existing.c_str(), NC_NOCLOBBER, &bad_id));
  }

  // Non-EEXIST create error (missing parent directory).
  const std::filesystem::path bad_path = missing_parent.child("file.nc");
  EXPECT_NE(NC_NOERR, nc_create(bad_path.c_str(), NC_CLOBBER, &bad_id));

  // Direct NC3_open failure when underlying file does not exist.
  ASSERT_EQ(NC_NOERR, nc_initialize());
  {
    NC* tmp_nc = nullptr;
    ASSERT_EQ(NC_NOERR, new_NC(NC3_dispatch_table, bad_path.c_str(), NC_NOWRITE,
                               &tmp_nc));
    add_to_NCList(tmp_nc);
    absl::Cleanup cleanup_tmp_nc = [&] {
      del_from_NCList(tmp_nc);
      free_NC(tmp_nc);
    };
    EXPECT_NE(NC_NOERR, NC3_open(tmp_nc->path, NC_NOWRITE, 0, nullptr, nullptr,
                                 NC3_dispatch_table, tmp_nc->ext_ncid));
  }

  // Corrupt CDF-1 header > MAGIC_NUMBER_LEN with nelems != 0 hits NC3_open
  // unwind_ioc -> unwind_alloc in both POSIX I/O and memio without tripping
  // v1hpg's short-buffer assertion (`gs.pos < gs.end`, where memio sets
  // sizehint = ((size / 2) / 8) * 8) or v1h_get_NC_dimarray's nelems == 0
  // early return before type validation.
  std::array<unsigned char, 32> corrupt_cdf1 = {
      'C', 'D', 'F', 0x01,  // Magic
      0,   0,   0,   0,     // numrecs
      0,   0,   0,   99,    // invalid NC_DIMENSION tag
      0,   0,   0,   1,     // nelems != 0 triggers EINVAL tag check
  };
  TempFile corrupt_tf("nc3_corrupt.nc");
  WriteBytesToFile(corrupt_tf.path(),
                   {corrupt_cdf1.begin(), corrupt_cdf1.end()});
  int bad_posix_id = -1;
  EXPECT_EQ(EINVAL, nc_open(corrupt_tf.c_str(), NC_NOWRITE, &bad_posix_id));
  if (bad_posix_id >= 0) {
    static_cast<void>(nc_close(bad_posix_id));
  }
  int bad_mem_id = -1;
  EXPECT_EQ(EINVAL,
            nc_open_mem("corrupt_mem.nc", NC_NOWRITE, corrupt_cdf1.size(),
                        corrupt_cdf1.data(), &bad_mem_id));
  if (bad_mem_id >= 0) {
    static_cast<void>(nc_close(bad_mem_id));
  }
}

TEST(Nc3InternalTest, InquiriesFormatAndTypeDirectDispatch) {
  ASSERT_EQ(NC_NOERR, nc_initialize());

  int fmt = 0;
  int mode = 0;
  int ndims = 0;
  int nvars = 0;
  int natts = 0;
  int unlim = 0;
  std::array<char, NC_MAX_NAME + 1> name{};
  size_t tsize = 0;

  EXPECT_EQ(NC_EBADID, NC3_inq(-1, &ndims, &nvars, &natts, &unlim));
  EXPECT_EQ(NC_EBADID, NC3_inq_unlimdim(-1, &unlim));
  EXPECT_EQ(NC_EBADID, NC3_inq_format(-1, &fmt));
  EXPECT_EQ(NC_EBADID, NC3_inq_format_extended(-1, &fmt, &mode));
  EXPECT_EQ(NC_EBADID, NC3_inq_type(-1, NC_INT, name.data(), &tsize));

  struct FormatCase {
    int create_flag;
    int expected_format;
  };
  constexpr std::array<FormatCase, 3> kCases = {{
      {0, NC_FORMAT_CLASSIC},
      {NC_64BIT_OFFSET, NC_FORMAT_64BIT_OFFSET},
      {NC_CDF5, NC_FORMAT_CDF5},
  }};

  for (const auto& tc : kCases) {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc_create("fmt_inq.nc", NC_DISKLESS | tc.create_flag,
                                  file.ptr()));
    EXPECT_EQ(NC_NOERR,
              NC3_inq(file.get(), nullptr, nullptr, nullptr, nullptr));
    EXPECT_EQ(NC_NOERR, NC3_inq_unlimdim(file.get(), nullptr));
    EXPECT_EQ(NC_NOERR, NC3_inq_format(file.get(), nullptr));
    EXPECT_EQ(NC_NOERR, NC3_inq_format_extended(file.get(), nullptr, nullptr));

    unlim = 99;
    EXPECT_EQ(NC_NOERR, NC3_inq(file.get(), &ndims, &nvars, &natts, &unlim));
    EXPECT_EQ(0, ndims);
    EXPECT_EQ(0, nvars);
    EXPECT_EQ(0, natts);
    EXPECT_EQ(-1, unlim);
    unlim = 99;
    EXPECT_EQ(NC_NOERR, NC3_inq_unlimdim(file.get(), &unlim));
    EXPECT_EQ(-1, unlim);

    int d_unlim = -1;
    int v_scalar = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "t", NC_UNLIMITED, &d_unlim));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "s", NC_INT, 0, nullptr, &v_scalar));
    const int gval = 7;
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(file.get(), NC_GLOBAL, "g", NC_INT, 1, &gval));

    EXPECT_EQ(NC_NOERR, NC3_inq(file.get(), &ndims, &nvars, &natts, &unlim));
    EXPECT_EQ(1, ndims);
    EXPECT_EQ(1, nvars);
    EXPECT_EQ(1, natts);
    EXPECT_EQ(d_unlim, unlim);

    unlim = -1;
    EXPECT_EQ(NC_NOERR, NC3_inq_unlimdim(file.get(), &unlim));
    EXPECT_EQ(d_unlim, unlim);

    fmt = 0;
    EXPECT_EQ(NC_NOERR, nc_inq_format(file.get(), &fmt));
    EXPECT_EQ(tc.expected_format, fmt);

    fmt = 0;
    mode = 0;
    EXPECT_EQ(NC_NOERR, nc_inq_format_extended(file.get(), &fmt, &mode));
    EXPECT_EQ(NC_FORMATX_NC3, fmt);
    EXPECT_NE(0, mode & NC_DISKLESS);

    // Call NC3_inq_type directly because libdispatch handles atomic types.
    EXPECT_EQ(NC_EBADTYPE,
              NC3_inq_type(file.get(), NC_NAT, name.data(), &tsize));
    EXPECT_EQ(NC_EBADTYPE,
              NC3_inq_type(file.get(), NC_STRING + 1, name.data(), &tsize));
    for (nc_type t = NC_BYTE; t <= NC_STRING; ++t) {
      name.fill('\0');
      tsize = 0;
      EXPECT_EQ(NC_NOERR, NC3_inq_type(file.get(), t, name.data(), &tsize));
      EXPECT_GT(std::strlen(name.data()), 0U);
      EXPECT_GT(tsize, 0U);
      EXPECT_EQ(NC_NOERR, NC3_inq_type(file.get(), t, nullptr, nullptr));
    }

    ASSERT_EQ(NC_NOERR, file.close());
  }
}

TEST(Nc3InternalTest, EnddefAlignmentChunkSingleUnalignedRecvarAndClamp) {
  ASSERT_EQ(NC_NOERR, nc_initialize());
  EXPECT_EQ(NC_EBADID, NC3__enddef(-1, 0, 1, 0, 1));

  // Empty variables dataset: enddef -> data mode error -> redef -> enddef.
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc_create("empty_vars.nc", NC_DISKLESS, file.ptr()));
    ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));
    EXPECT_EQ(NC_ENOTINDEFINE, nc_enddef(file.get()));
    EXPECT_EQ(NC_ENOTINDEFINE, NC3__enddef(file.get(), 0, 1, 0, 1));
    ASSERT_EQ(NC_NOERR, nc_redef(file.get()));
    const int attr_val = 42;
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(file.get(), NC_GLOBAL, "a", NC_INT, 1, &attr_val));
    ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));
    ASSERT_EQ(NC_NOERR, file.close());
  }

  // NC_ALIGN_CHUNK + record-only single 3-byte char var -> packed recsize == 3.
  {
    ScopedNcFile file;
    size_t chunk = 256;
    ASSERT_EQ(NC_NOERR, nc__create("rec_only_chunk.nc", NC_DISKLESS, 0, &chunk,
                                   file.ptr()));
    int d_rec = -1;
    int d_three = -1;
    int v_rec = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "rec", NC_UNLIMITED, &d_rec));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "three", 3, &d_three));
    const std::array<int, 2> dims = {d_rec, d_three};
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "c", NC_CHAR, 2, dims.data(), &v_rec));
    ASSERT_EQ(NC_NOERR,
              nc__enddef(file.get(), 0, NC_ALIGN_CHUNK, 0, NC_ALIGN_CHUNK));
    NC3_INFO* nc3 = GetNc3InfoOrNull(file.get());
    ASSERT_NE(nullptr, nc3);
    EXPECT_EQ(nc3->begin_rec, nc3->begin_var);
    EXPECT_EQ(0, nc3->begin_rec % static_cast<off_t>(nc3->chunk));
    EXPECT_EQ(3U, static_cast<size_t>(nc3->recsize));
    ASSERT_EQ(NC_NOERR, file.close());
  }

  // Exercise h_minfree/v_align/v_minfree/r_align recalculation and begin_var
  // clamp when redef alignment requests a larger v_align making computed
  // begin_var smaller than old->begin_var.
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create("clamp_begin_var.nc", NC_DISKLESS, file.ptr()));
    int d_rec = -1;
    int d_len = -1;
    int v_fix = -1;
    int v_rec = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "rec", NC_UNLIMITED, &d_rec));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "n", 3, &d_len));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "fx", NC_INT, 1, &d_len, &v_fix));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "rc", NC_INT, 1, &d_rec, &v_rec));

    // Ensure initial begin_var >= xsz + 64 and begin_var % 64 != 0 so that
    // re-entering define mode with h_minfree=0, v_align=64 computes a smaller
    // begin_var (<= D_RNDUP(xsz, 64) <= xsz + 63 < old->begin_var) and triggers
    // the NC_begins clamp to old->begin_var.
    ASSERT_EQ(NC_NOERR, nc__enddef(file.get(), 72, 8, 64, 8));
    NC3_INFO* nc3 = GetNc3InfoOrNull(file.get());
    ASSERT_NE(nullptr, nc3);
    EXPECT_GE(nc3->begin_var, static_cast<off_t>(nc3->xsz) + 64);

    if (nc3->begin_var % 64 == 0) {
      ASSERT_EQ(NC_NOERR, nc_redef(file.get()));
      const size_t bump =
          static_cast<size_t>(nc3->begin_var - static_cast<off_t>(nc3->xsz)) +
          8U;
      ASSERT_EQ(NC_NOERR, nc__enddef(file.get(), bump, 8, 0, 8));
    }
    ASSERT_EQ(NC_NOERR, nc_redef(file.get()));
    const off_t pre_clamp_begin_var = nc3->begin_var;
    ASSERT_GE(pre_clamp_begin_var, static_cast<off_t>(nc3->xsz) + 64);
    ASSERT_NE(0, pre_clamp_begin_var % 64);
    ASSERT_EQ(NC_NOERR, nc__enddef(file.get(), 0, 64, 0, 8));
    EXPECT_EQ(pre_clamp_begin_var, nc3->begin_var);
    ASSERT_EQ(NC_NOERR, file.close());
  }
}

TEST(Nc3InternalTest, EnddefRedefDataMovementAndFillAddedMatrix) {
  // Case A: header grows past begin_var AND begin_rec > old->begin_rec ->
  // invokes both move_recs_r and move_vars_r, plus fill_added &
  // fill_added_recs with both NC_FILL and NC_NOFILL added variables.
  {
    TempFile tf("redef_both_move.nc");
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc_create(tf.c_str(), NC_CLOBBER, file.ptr()));

    int d_rec = -1;
    int d_two = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "rec", NC_UNLIMITED, &d_rec));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "two", 2, &d_two));

    int v_fix0 = -1;
    int v_rec0 = -1;
    int v_rec1 = -1;
    const std::array<int, 2> rec2d = {d_rec, d_two};
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "fix0", NC_INT, 1, &d_two, &v_fix0));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "rec0", NC_INT, 1, &d_rec, &v_rec0));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "rec1", NC_INT, 2, rec2d.data(), &v_rec1));
    ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));

    const std::array<int, 2> fix_vals = {101, 202};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(file.get(), v_fix0, fix_vals.data()));
    const int r0_rec0 = 11;
    const int r0_rec1 = 22;
    const std::array<int, 2> r1_rec0 = {33, 44};
    const std::array<int, 2> r1_rec1 = {55, 66};
    const size_t idx0 = 0;
    const size_t idx1 = 1;
    const std::array<size_t, 2> start0 = {0, 0};
    const std::array<size_t, 2> start1 = {1, 0};
    const std::array<size_t, 2> count = {1, 2};
    ASSERT_EQ(NC_NOERR, nc_put_var1_int(file.get(), v_rec0, &idx0, &r0_rec0));
    ASSERT_EQ(NC_NOERR, nc_put_var1_int(file.get(), v_rec0, &idx1, &r0_rec1));
    ASSERT_EQ(NC_NOERR, nc_put_vara_int(file.get(), v_rec1, start0.data(),
                                        count.data(), r1_rec0.data()));
    ASSERT_EQ(NC_NOERR, nc_put_vara_int(file.get(), v_rec1, start1.data(),
                                        count.data(), r1_rec1.data()));

    ASSERT_EQ(NC_NOERR, nc_redef(file.get()));
    const std::string big_attr(200, 'Z');
    ASSERT_EQ(NC_NOERR, nc_put_att_text(file.get(), NC_GLOBAL, "grow",
                                        big_attr.size(), big_attr.c_str()));
    int v_fix_new = -1;
    int v_fix_nf = -1;
    int v_rec_new = -1;
    int v_rec_nf = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "fnew", NC_INT, 1, &d_two, &v_fix_new));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "fnf", NC_INT, 1, &d_two, &v_fix_nf));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_fill(file.get(), v_fix_nf, /*no_fill=*/1, nullptr));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "rnew", NC_INT, 1, &d_rec, &v_rec_new));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "rnf", NC_INT, 1, &d_rec, &v_rec_nf));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_fill(file.get(), v_rec_nf, /*no_fill=*/1, nullptr));
    ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));

    // Verify existing data survived both record and fixed variable moves.
    std::array<int, 2> fix_out{};
    std::array<int, 2> r0_out{};
    std::array<int, 2> r1_out0{};
    std::array<int, 2> r1_out1{};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(file.get(), v_fix0, fix_out.data()));
    EXPECT_EQ(fix_vals, fix_out);
    ASSERT_EQ(NC_NOERR, nc_get_var_int(file.get(), v_rec0, r0_out.data()));
    EXPECT_EQ(11, r0_out[0]);
    EXPECT_EQ(22, r0_out[1]);
    ASSERT_EQ(NC_NOERR, nc_get_vara_int(file.get(), v_rec1, start0.data(),
                                        count.data(), r1_out0.data()));
    ASSERT_EQ(NC_NOERR, nc_get_vara_int(file.get(), v_rec1, start1.data(),
                                        count.data(), r1_out1.data()));
    EXPECT_EQ(r1_rec0, r1_out0);
    EXPECT_EQ(r1_rec1, r1_out1);

    // Verify newly added filled fixed and record vars contain default fill.
    std::array<int, 2> fnew_out{};
    std::array<int, 2> rnew_out{};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(file.get(), v_fix_new, fnew_out.data()));
    EXPECT_EQ(NC_FILL_INT, fnew_out[0]);
    EXPECT_EQ(NC_FILL_INT, fnew_out[1]);
    ASSERT_EQ(NC_NOERR, nc_get_var_int(file.get(), v_rec_new, rnew_out.data()));
    EXPECT_EQ(NC_FILL_INT, rnew_out[0]);
    EXPECT_EQ(NC_FILL_INT, rnew_out[1]);

    ASSERT_EQ(NC_NOERR, file.close());
  }

  // Case B: begin_rec > old->begin_rec && begin_var == old->begin_var.
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create("move_recs_only.nc", NC_DISKLESS, file.ptr()));
    int d_rec = -1;
    int d_one = -1;
    int v_fix0 = -1;
    int v_rec0 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "rec", NC_UNLIMITED, &d_rec));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "one", 1, &d_one));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "fx0", NC_INT, 1, &d_one, &v_fix0));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "rc0", NC_INT, 1, &d_rec, &v_rec0));
    ASSERT_EQ(NC_NOERR, nc__enddef(file.get(), 512, 4, 0, 4));

    NC3_INFO* nc3 = GetNc3InfoOrNull(file.get());
    ASSERT_NE(nullptr, nc3);
    const off_t old_bv = nc3->begin_var;
    const off_t old_br = nc3->begin_rec;

    const int fv = 505;
    const int rv0 = 606;
    const int rv1 = 707;
    const size_t i0 = 0;
    const size_t i1 = 1;
    ASSERT_EQ(NC_NOERR, nc_put_var1_int(file.get(), v_fix0, &i0, &fv));
    ASSERT_EQ(NC_NOERR, nc_put_var1_int(file.get(), v_rec0, &i0, &rv0));
    ASSERT_EQ(NC_NOERR, nc_put_var1_int(file.get(), v_rec0, &i1, &rv1));

    ASSERT_EQ(NC_NOERR, nc_redef(file.get()));
    int v_fix1 = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "fx1", NC_INT, 1, &d_one, &v_fix1));
    ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));
    EXPECT_EQ(old_bv, nc3->begin_var);
    EXPECT_GT(nc3->begin_rec, old_br);

    int out = 0;
    ASSERT_EQ(NC_NOERR, nc_get_var1_int(file.get(), v_fix0, &i0, &out));
    EXPECT_EQ(fv, out);
    ASSERT_EQ(NC_NOERR, nc_get_var1_int(file.get(), v_rec0, &i0, &out));
    EXPECT_EQ(rv0, out);
    ASSERT_EQ(NC_NOERR, nc_get_var1_int(file.get(), v_rec0, &i1, &out));
    EXPECT_EQ(rv1, out);
    ASSERT_EQ(NC_NOERR, file.close());
  }

  // Case C: begin_rec == old->begin_rec && begin_var > old->begin_var.
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create("move_vars_only.nc", NC_DISKLESS, file.ptr()));
    int d_rec = -1;
    int d_one = -1;
    int v_fix0 = -1;
    int v_rec0 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "rec", NC_UNLIMITED, &d_rec));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "one", 1, &d_one));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "fx0", NC_INT, 1, &d_one, &v_fix0));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "rc0", NC_INT, 1, &d_rec, &v_rec0));
    ASSERT_EQ(NC_NOERR, nc__enddef(file.get(), 0, 4, 256, 4));

    NC3_INFO* nc3 = GetNc3InfoOrNull(file.get());
    ASSERT_NE(nullptr, nc3);
    const off_t old_bv = nc3->begin_var;
    const off_t old_br = nc3->begin_rec;

    const int fv = 808;
    const int rv = 909;
    const size_t i0 = 0;
    ASSERT_EQ(NC_NOERR, nc_put_var1_int(file.get(), v_fix0, &i0, &fv));
    ASSERT_EQ(NC_NOERR, nc_put_var1_int(file.get(), v_rec0, &i0, &rv));

    ASSERT_EQ(NC_NOERR, nc_redef(file.get()));
    const std::string small_attr(16, 'A');
    ASSERT_EQ(NC_NOERR, nc_put_att_text(file.get(), NC_GLOBAL, "bump",
                                        small_attr.size(), small_attr.c_str()));
    ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));
    EXPECT_GT(nc3->begin_var, old_bv);
    EXPECT_EQ(old_br, nc3->begin_rec);

    int out = 0;
    ASSERT_EQ(NC_NOERR, nc_get_var1_int(file.get(), v_fix0, &i0, &out));
    EXPECT_EQ(fv, out);
    ASSERT_EQ(NC_NOERR, nc_get_var1_int(file.get(), v_rec0, &i0, &out));
    EXPECT_EQ(rv, out);
    ASSERT_EQ(NC_NOERR, file.close());
  }

  // Case D & E: begin_var == old->begin_var && begin_rec == old->begin_rec,
  // first with recsize == old->recsize (attr tweak only), then recsize >
  // old->recsize (adding a record variable within h_minfree & v_minfree slack,
  // which also exercises gnu_off == old_off for recno 0 in move_recs_r).
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc_create("rec_grow_only.nc", NC_DISKLESS, file.ptr()));
    int d_rec = -1;
    int d_one = -1;
    int v_fix0 = -1;
    int v_rec0 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "rec", NC_UNLIMITED, &d_rec));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "one", 1, &d_one));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "fx0", NC_INT, 1, &d_one, &v_fix0));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "rc0", NC_INT, 1, &d_rec, &v_rec0));
    ASSERT_EQ(NC_NOERR, nc__enddef(file.get(), 256, 4, 256, 4));

    NC3_INFO* nc3 = GetNc3InfoOrNull(file.get());
    ASSERT_NE(nullptr, nc3);
    const off_t old_bv = nc3->begin_var;
    const off_t old_br = nc3->begin_rec;

    const int fv = 123;
    const int rv0 = 456;
    const int rv1 = 789;
    const size_t i0 = 0;
    const size_t i1 = 1;
    ASSERT_EQ(NC_NOERR, nc_put_var1_int(file.get(), v_fix0, &i0, &fv));
    ASSERT_EQ(NC_NOERR, nc_put_var1_int(file.get(), v_rec0, &i0, &rv0));
    ASSERT_EQ(NC_NOERR, nc_put_var1_int(file.get(), v_rec0, &i1, &rv1));

    // Case E: no offset or recsize change.
    ASSERT_EQ(NC_NOERR, nc_redef(file.get()));
    ASSERT_EQ(NC_NOERR, nc_put_att_text(file.get(), NC_GLOBAL, "k", 1, "x"));
    ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));
    EXPECT_EQ(old_bv, nc3->begin_var);
    EXPECT_EQ(old_br, nc3->begin_rec);

    // Case D: recsize grows while begin_var and begin_rec stay unchanged.
    ASSERT_EQ(NC_NOERR, nc_redef(file.get()));
    int v_rec1 = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "rc1", NC_INT, 1, &d_rec, &v_rec1));
    ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));
    EXPECT_EQ(old_bv, nc3->begin_var);
    EXPECT_EQ(old_br, nc3->begin_rec);
    EXPECT_EQ(8U, static_cast<size_t>(nc3->recsize));

    int out = 0;
    ASSERT_EQ(NC_NOERR, nc_get_var1_int(file.get(), v_fix0, &i0, &out));
    EXPECT_EQ(fv, out);
    ASSERT_EQ(NC_NOERR, nc_get_var1_int(file.get(), v_rec0, &i0, &out));
    EXPECT_EQ(rv0, out);
    ASSERT_EQ(NC_NOERR, nc_get_var1_int(file.get(), v_rec0, &i1, &out));
    EXPECT_EQ(rv1, out);
    ASSERT_EQ(NC_NOERR, nc_get_var1_int(file.get(), v_rec1, &i1, &out));
    EXPECT_EQ(NC_FILL_INT, out);
    ASSERT_EQ(NC_NOERR, file.close());
  }
}

TEST(Nc3InternalTest, BeginsReusesLargerOldOffsetsInGappedCdf1File) {
  // Build an interleaved [fx0, rc0, fx1, rc1] CDF-1 buffer in memory, patch
  // variable offsets to introduce gaps while maintaining valid ascending order,
  // and verify NC_begins preserves old->begin when computed begin is smaller.
  ScopedMemio mem;
  std::array<off_t, 4> orig_begins{};
  size_t header_sz = 0;
  {
    ScopedNcFile mem_file;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("gap_tpl.nc", NC_CLOBBER, 0, mem_file.ptr()));
    int d_fx = -1;
    int d_rc = -1;
    int v = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(mem_file.get(), "fxd", 1, &d_fx));
    ASSERT_EQ(NC_NOERR, nc_def_dim(mem_file.get(), "rcd", NC_UNLIMITED, &d_rc));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(mem_file.get(), "fx0", NC_INT, 1, &d_fx, &v));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(mem_file.get(), "rc0", NC_INT, 1, &d_rc, &v));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(mem_file.get(), "fx1", NC_INT, 1, &d_fx, &v));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(mem_file.get(), "rc1", NC_INT, 1, &d_rc, &v));
    ASSERT_EQ(NC_NOERR, nc_enddef(mem_file.get()));

    NC3_INFO* nc3 = GetNc3InfoOrNull(mem_file.get());
    ASSERT_NE(nullptr, nc3);
    ASSERT_EQ(4U, nc3->vars.nelems);
    header_sz = nc3->xsz;
    for (size_t i = 0; i < 4; ++i) {
      orig_begins[i] = nc3->vars.value[i]->begin;
    }
    ASSERT_EQ(NC_NOERR, nc_close_memio(mem_file.get(), mem.get()));
    mem_file.release();
  }

  // Exact CDF-1 layout for 2 dims ("fxd","rcd"), 0 attrs, 4 1-D vars
  // ("fx0","rc0","fx1","rc1"): header_sz == 200, var[i].begin at 56 + i*36
  // + 32.
  ASSERT_EQ(200U, header_sz);
  std::vector<unsigned char> buf(mem.bytes(), mem.bytes() + mem.size());
  ASSERT_GE(buf.size(), header_sz);
  for (size_t i = 0; i < 4; ++i) {
    const size_t pos = 56U + i * 36U + 32U;
    ASSERT_EQ(static_cast<uint32_t>(orig_begins[i]), ReadBe32(&buf[pos]));
  }

  // orig_begins[0] (fx0) = 200, [2] (fx1) = 204, [1] (rc0) = 208, [3] (rc1) =
  // 212.
  const uint32_t new_fx1 = static_cast<uint32_t>(orig_begins[2]) + 28U;  // 232
  const uint32_t new_rc0 = static_cast<uint32_t>(orig_begins[1]) + 40U;  // 248
  const uint32_t new_rc1 = static_cast<uint32_t>(orig_begins[3]) + 68U;  // 280
  WriteBe32(&buf[56U + 2U * 36U + 32U], new_fx1);
  WriteBe32(&buf[56U + 1U * 36U + 32U], new_rc0);
  WriteBe32(&buf[56U + 3U * 36U + 32U], new_rc1);
  if (buf.size() < new_rc1 + 64U) {
    buf.resize(new_rc1 + 64U, 0);
  }

  TempFile tf("gap_redef.nc");
  WriteBytesToFile(tf.path(), buf);

  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR, nc_open(tf.c_str(), NC_WRITE, file.ptr()));
  const size_t i0 = 0;
  const int v111 = 111;
  const int v222 = 222;
  const int v333 = 333;
  const int v444 = 444;
  ASSERT_EQ(NC_NOERR, nc_put_var1_int(file.get(), 0, &i0, &v111));
  ASSERT_EQ(NC_NOERR, nc_put_var1_int(file.get(), 1, &i0, &v333));
  ASSERT_EQ(NC_NOERR, nc_put_var1_int(file.get(), 2, &i0, &v222));
  ASSERT_EQ(NC_NOERR, nc_put_var1_int(file.get(), 3, &i0, &v444));

  // Redef 1 (no header growth): hits begin < old->vars.value[j]->begin for
  // both fx1 (L234-240) and rc1 (L303-305) while skipping interleaved vars.
  ASSERT_EQ(NC_NOERR, nc_redef(file.get()));
  ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));

  NC3_INFO* nc3 = GetNc3InfoOrNull(file.get());
  ASSERT_NE(nullptr, nc3);
  EXPECT_EQ(static_cast<off_t>(new_fx1), nc3->vars.value[2]->begin);
  EXPECT_EQ(static_cast<off_t>(new_rc1), nc3->vars.value[3]->begin);

  // Redef 2: small +20 byte global attr shifts fx0 into the gap while fx1
  // stays at new_fx1 (exercises both gnu_off > old_off and gnu_off == old_off
  // in move_vars_r).
  ASSERT_EQ(NC_NOERR, nc_redef(file.get()));
  const signed char bval = 9;
  ASSERT_EQ(NC_NOERR,
            nc_put_att_schar(file.get(), NC_GLOBAL, "a", NC_BYTE, 1, &bval));
  ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));
  EXPECT_EQ(static_cast<off_t>(new_fx1), nc3->vars.value[2]->begin);

  int out = 0;
  ASSERT_EQ(NC_NOERR, nc_get_var1_int(file.get(), 0, &i0, &out));
  EXPECT_EQ(111, out);
  ASSERT_EQ(NC_NOERR, nc_get_var1_int(file.get(), 1, &i0, &out));
  EXPECT_EQ(333, out);
  ASSERT_EQ(NC_NOERR, nc_get_var1_int(file.get(), 2, &i0, &out));
  EXPECT_EQ(222, out);
  ASSERT_EQ(NC_NOERR, nc_get_var1_int(file.get(), 3, &i0, &out));
  EXPECT_EQ(444, out);
  ASSERT_EQ(NC_NOERR, file.close());
}

TEST(Nc3InternalTest, BeginsCumulativeOffsetOverflowClassicFailsWithVarSize) {
  // 1) Fixed variable cumulative offset > X_OFF_MAX (2GiB - 1) in Classic.
  // Also tests NC3_close failing NC_endef while NC_INDEF and calling NC3_abort.
  {
    int ncid = -1;
    ASSERT_EQ(NC_NOERR, nc_create("ov_fix_classic.nc", NC_DISKLESS, &ncid));
    absl::Cleanup close_orphan = [&] {
      if (ncid >= 0) {
        if (nc_close(ncid) != NC_NOERR) {
          CleanOrphanNcId(ncid);
        }
      }
    };
    int d_big = -1;
    int d_one = -1;
    int v = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "big", 0x10000000U, &d_big));  // 1GiB
    ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "one", 1, &d_one));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "f0", NC_INT, 1, &d_big, &v));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "f1", NC_INT, 1, &d_big, &v));
    ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "f2", NC_INT, 1, &d_one, &v));

    EXPECT_EQ(NC_EVARSIZE, nc_enddef(ncid));
    // nc_close while still NC_INDEF hits NC3_close L1306-1309 (NC3_abort).
    EXPECT_EQ(NC_EVARSIZE, nc_close(ncid));
    CleanOrphanNcId(ncid);
    ncid = -1;
  }

  // 2) Record variable cumulative offset > X_OFF_MAX in Classic.
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create("ov_rec_classic.nc", NC_DISKLESS, file.ptr()));
    int d_rec = -1;
    int d_big = -1;
    int d_one = -1;
    int v = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "rec", NC_UNLIMITED, &d_rec));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "big", 0x10000000U, &d_big));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "one", 1, &d_one));
    const std::array<int, 2> rbig = {d_rec, d_big};
    const std::array<int, 2> rone = {d_rec, d_one};
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "r0", NC_INT, 2, rbig.data(), &v));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "r1", NC_INT, 2, rbig.data(), &v));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "r2", NC_INT, 2, rone.data(), &v));

    EXPECT_EQ(NC_EVARSIZE, nc_enddef(file.get()));
    ASSERT_EQ(NC_NOERR, file.abort());
  }
}

TEST(Nc3InternalTest, CheckVlensCoversClassicCdf2AndCdf5Limits) {
  // Empty variables dataset returns NC_NOERR.
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc_create("vlen_empty.nc", NC_DISKLESS, file.ptr()));
    NC3_INFO* nc3 = GetNc3InfoOrNull(file.get());
    ASSERT_NE(nullptr, nc3);
    EXPECT_EQ(NC_NOERR, NC_check_vlens(nc3));
    ASSERT_EQ(NC_NOERR, file.abort());
  }

  // Classic (vlen_max = 2^31 - 4): last large fixed allowed, two not allowed.
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc_create("vlen_cdf1.nc", NC_DISKLESS, file.ptr()));
    int d_big = -1;
    int v = -1;
    // 512M NC_INT = 2GiB (> 2^31 - 4).
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "d", 0x20000000U, &d_big));
    ASSERT_EQ(NC_NOERR, nc_def_var(file.get(), "v0", NC_INT, 1, &d_big, &v));
    NC3_INFO* nc3 = GetNc3InfoOrNull(file.get());
    ASSERT_NE(nullptr, nc3);
    EXPECT_EQ(NC_NOERR, NC_check_vlens(nc3));
    ASSERT_EQ(NC_NOERR, nc_def_var(file.get(), "v1", NC_INT, 1, &d_big, &v));
    EXPECT_EQ(NC_EVARSIZE, NC_check_vlens(nc3));
    // Also hit NC_check_vlens failure inside NC_endef.
    EXPECT_EQ(NC_EVARSIZE, nc_enddef(file.get()));
    ASSERT_EQ(NC_NOERR, file.abort());
  }

  // CDF-2 (vlen_max = 2^32 - 4): test all fixed and record variable rules.
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc_create("vlen_cdf2_a.nc",
                                  NC_DISKLESS | NC_64BIT_OFFSET, file.ptr()));
    int d_big = -1;
    int d_small = -1;
    int v = -1;
    // 512M NC_DOUBLE = 4GiB (> 2^32 - 4).
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "big", 0x20000000U, &d_big));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "small", 4, &d_small));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "huge_last", NC_DOUBLE, 1, &d_big, &v));
    NC3_INFO* nc3 = GetNc3InfoOrNull(file.get());
    ASSERT_NE(nullptr, nc3);
    EXPECT_EQ(NC_NOERR, NC_check_vlens(nc3));

    // Adding a small fixed var makes huge_last non-last -> NC_EVARSIZE.
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "after", NC_INT, 1, &d_small, &v));
    EXPECT_EQ(NC_EVARSIZE, NC_check_vlens(nc3));
    ASSERT_EQ(NC_NOERR, file.abort());
  }

  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc_create("vlen_cdf2_b.nc",
                                  NC_DISKLESS | NC_64BIT_OFFSET, file.ptr()));
    int d_rec = -1;
    int d_big = -1;
    int d_small = -1;
    int v = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "rec", NC_UNLIMITED, &d_rec));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "big", 0x20000000U, &d_big));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "small", 4, &d_small));

    // Huge last fixed var + a record var -> NC_EVARSIZE.
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "fhuge", NC_DOUBLE, 1, &d_big, &v));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "rsmall", NC_INT, 1, &d_rec, &v));
    NC3_INFO* nc3 = GetNc3InfoOrNull(file.get());
    ASSERT_NE(nullptr, nc3);
    EXPECT_EQ(NC_EVARSIZE, NC_check_vlens(nc3));
    ASSERT_EQ(NC_NOERR, file.abort());
  }

  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc_create("vlen_cdf2_rec.nc",
                                  NC_DISKLESS | NC_64BIT_OFFSET, file.ptr()));
    int d_rec = -1;
    int d_big = -1;
    int d_small = -1;
    int v = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "rec", NC_UNLIMITED, &d_rec));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "big", 0x20000000U, &d_big));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "small", 4, &d_small));
    const std::array<int, 2> rbig = {d_rec, d_big};
    const std::array<int, 2> rsmall = {d_rec, d_small};

    // Single huge last recvar is allowed.
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "rh0", NC_DOUBLE, 2, rbig.data(), &v));
    NC3_INFO* nc3 = GetNc3InfoOrNull(file.get());
    ASSERT_NE(nullptr, nc3);
    EXPECT_EQ(NC_NOERR, NC_check_vlens(nc3));

    // Non-last huge recvar (small recvar follows) -> NC_EVARSIZE.
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "rs", NC_INT, 2, rsmall.data(), &v));
    EXPECT_EQ(NC_EVARSIZE, NC_check_vlens(nc3));

    // Two huge recvars -> NC_EVARSIZE.
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "rh1", NC_DOUBLE, 2, rbig.data(), &v));
    EXPECT_EQ(NC_EVARSIZE, NC_check_vlens(nc3));
    ASSERT_EQ(NC_NOERR, file.abort());
  }

  // CDF-5 (vlen_max = X_INT64_MAX - 3): fixed and rec variables exceeding
  // limit. Define small unit dims first so nc_def_var -> NC_var_shape does not
  // compute OFF_T_MAX * xsz (signed 64-bit overflow under UBSan), then
  // temporarily patch shape dimensions to exercise NC_check_vlens safely.
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create("vlen_cdf5_fix.nc", NC_DISKLESS | NC_CDF5, file.ptr()));
    int d0 = -1;
    int d1 = -1;
    int v = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "d0", 1, &d0));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "d1", 1, &d1));
    const std::array<int, 2> dims = {d0, d1};
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "fhuge5", NC_INT, 2, dims.data(), &v));
    NC3_INFO* nc3 = GetNc3InfoOrNull(file.get());
    ASSERT_NE(nullptr, nc3);
    ASSERT_EQ(1U, nc3->vars.nelems);
    NC_var* const varp = nc3->vars.value[0];
    ASSERT_NE(nullptr, varp);
    ASSERT_EQ(2U, varp->ndims);
    ASSERT_NE(nullptr, varp->shape);
    const size_t saved_s0 = varp->shape[0];
    const size_t saved_s1 = varp->shape[1];
    absl::Cleanup restore_shape = [&] {
      varp->shape[0] = saved_s0;
      varp->shape[1] = saved_s1;
    };
    // 2^31 * 2^32 * 4 = 2^65 > X_INT64_MAX - 3.
    varp->shape[0] = static_cast<size_t>(1ULL << 31);
    varp->shape[1] = static_cast<size_t>(1ULL << 32);
    EXPECT_EQ(NC_EVARSIZE, NC_check_vlens(nc3));
    std::move(restore_shape).Invoke();
    ASSERT_EQ(NC_NOERR, file.abort());
  }

  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create("vlen_cdf5_rec.nc", NC_DISKLESS | NC_CDF5, file.ptr()));
    int d_rec = -1;
    int d0 = -1;
    int d1 = -1;
    int v = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "rec", NC_UNLIMITED, &d_rec));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "d0", 1, &d0));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "d1", 1, &d1));
    const std::array<int, 3> dims = {d_rec, d0, d1};
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "rhuge5", NC_INT, 3, dims.data(), &v));
    NC3_INFO* nc3 = GetNc3InfoOrNull(file.get());
    ASSERT_NE(nullptr, nc3);
    ASSERT_EQ(1U, nc3->vars.nelems);
    NC_var* const varp = nc3->vars.value[0];
    ASSERT_NE(nullptr, varp);
    ASSERT_EQ(3U, varp->ndims);
    ASSERT_NE(nullptr, varp->shape);
    const size_t saved_s1 = varp->shape[1];
    const size_t saved_s2 = varp->shape[2];
    absl::Cleanup restore_shape = [&] {
      varp->shape[1] = saved_s1;
      varp->shape[2] = saved_s2;
    };
    // 2^31 * 2^32 * 4 = 2^65 > X_INT64_MAX - 3.
    varp->shape[1] = static_cast<size_t>(1ULL << 31);
    varp->shape[2] = static_cast<size_t>(1ULL << 32);
    EXPECT_EQ(NC_EVARSIZE, NC_check_vlens(nc3));
    std::move(restore_shape).Invoke();
    ASSERT_EQ(NC_NOERR, file.abort());
  }
}

TEST(Nc3InternalTest, CheckVoffsDetectsOverlappingVariablesAndBadBeginRec) {
  // Direct checks: empty vars, valid mixed vars, and begin_rec < prev_off.
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc_create("voffs_direct.nc", NC_DISKLESS, file.ptr()));
    NC3_INFO* nc3 = GetNc3InfoOrNull(file.get());
    ASSERT_NE(nullptr, nc3);
    EXPECT_EQ(NC_NOERR, NC_check_voffs(nc3));

    int d_rec = -1;
    int d_one = -1;
    int v = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "rec", NC_UNLIMITED, &d_rec));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "one", 1, &d_one));
    ASSERT_EQ(NC_NOERR, nc_def_var(file.get(), "fx0", NC_INT, 1, &d_one, &v));
    ASSERT_EQ(NC_NOERR, nc_def_var(file.get(), "rc0", NC_INT, 1, &d_rec, &v));
    ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));
    EXPECT_EQ(NC_NOERR, NC_check_voffs(nc3));

    const off_t saved_begin_rec = nc3->begin_rec;
    absl::Cleanup restore = [&] { nc3->begin_rec = saved_begin_rec; };
    nc3->begin_rec = nc3->begin_var;
    EXPECT_EQ(NC_ENOTNC, NC_check_voffs(nc3));
    std::move(restore).Invoke();
    ASSERT_EQ(NC_NOERR, file.close());
  }

  // Fixed variable overlap detected on nc_open_mem (L809).
  {
    ScopedMemio mem;
    std::array<off_t, 3> orig_begins{};
    {
      ScopedNcFile mem_file;
      ASSERT_EQ(NC_NOERR,
                nc_create_mem("ov_fix_tpl.nc", NC_CLOBBER, 0, mem_file.ptr()));
      int d_fx = -1;
      int d_rc = -1;
      int v = -1;
      ASSERT_EQ(NC_NOERR, nc_def_dim(mem_file.get(), "fxd", 4, &d_fx));
      ASSERT_EQ(NC_NOERR,
                nc_def_dim(mem_file.get(), "rcd", NC_UNLIMITED, &d_rc));
      ASSERT_EQ(NC_NOERR,
                nc_def_var(mem_file.get(), "fx0", NC_INT, 1, &d_fx, &v));
      ASSERT_EQ(NC_NOERR,
                nc_def_var(mem_file.get(), "fx1", NC_INT, 1, &d_fx, &v));
      ASSERT_EQ(NC_NOERR,
                nc_def_var(mem_file.get(), "rc0", NC_INT, 1, &d_rc, &v));
      ASSERT_EQ(NC_NOERR, nc_enddef(mem_file.get()));
      NC3_INFO* nc3 = GetNc3InfoOrNull(mem_file.get());
      ASSERT_NE(nullptr, nc3);
      for (size_t i = 0; i < 3; ++i) {
        orig_begins[i] = nc3->vars.value[i]->begin;
      }
      ASSERT_EQ(NC_NOERR, nc_close_memio(mem_file.get(), mem.get()));
      mem_file.release();
    }

    std::vector<unsigned char> buf(mem.bytes(), mem.bytes() + mem.size());
    const size_t fx1_begin_pos = 56U + 1U * 36U + 32U;
    ASSERT_EQ(static_cast<uint32_t>(orig_begins[1]),
              ReadBe32(&buf[fx1_begin_pos]));
    // Pull fx1 back by 8 bytes into fx0 (length 16), while keeping rc0 valid.
    WriteBe32(&buf[fx1_begin_pos], static_cast<uint32_t>(orig_begins[1] - 8));
    // Pad locked read-only memio past initial header extent so nc_get_NC
    // finishes reading the header instead of failing with EPERM on memio_get.
    if (buf.size() < 512U) {
      buf.resize(512U, 0);
    }

    int bad_id = -1;
    EXPECT_EQ(NC_ENOTNC, nc_open_mem("ov_fix.nc", NC_NOWRITE, buf.size(),
                                     buf.data(), &bad_id));
    if (bad_id >= 0) {
      static_cast<void>(nc_close(bad_id));
    }
  }

  // Record variable overlap detected on nc_open_mem (L831).
  {
    ScopedMemio mem;
    std::array<off_t, 2> orig_begins{};
    {
      ScopedNcFile mem_file;
      ASSERT_EQ(NC_NOERR,
                nc_create_mem("ov_rec_tpl.nc", NC_CLOBBER, 0, mem_file.ptr()));
      int d_fx = -1;
      int d_rc = -1;
      int v = -1;
      ASSERT_EQ(NC_NOERR, nc_def_dim(mem_file.get(), "fxd", 4, &d_fx));
      ASSERT_EQ(NC_NOERR,
                nc_def_dim(mem_file.get(), "rcd", NC_UNLIMITED, &d_rc));
      const std::array<int, 2> dims = {d_rc, d_fx};
      ASSERT_EQ(NC_NOERR,
                nc_def_var(mem_file.get(), "rc0", NC_INT, 2, dims.data(), &v));
      ASSERT_EQ(NC_NOERR,
                nc_def_var(mem_file.get(), "rc1", NC_INT, 2, dims.data(), &v));
      ASSERT_EQ(NC_NOERR, nc_enddef(mem_file.get()));
      NC3_INFO* nc3 = GetNc3InfoOrNull(mem_file.get());
      ASSERT_NE(nullptr, nc3);
      for (size_t i = 0; i < 2; ++i) {
        orig_begins[i] = nc3->vars.value[i]->begin;
      }
      ASSERT_EQ(NC_NOERR, nc_close_memio(mem_file.get(), mem.get()));
      mem_file.release();
    }

    std::vector<unsigned char> buf(mem.bytes(), mem.bytes() + mem.size());
    // 2D var entry is 40 bytes; begin is at +36.
    const size_t rc1_begin_pos = 56U + 1U * 40U + 36U;
    ASSERT_EQ(static_cast<uint32_t>(orig_begins[1]),
              ReadBe32(&buf[rc1_begin_pos]));
    // Pull rc1 back by 8 bytes into rc0 (length 16).
    WriteBe32(&buf[rc1_begin_pos], static_cast<uint32_t>(orig_begins[1] - 8));
    // Pad locked read-only memio past initial header extent so nc_get_NC
    // finishes reading the header instead of failing with EPERM on memio_get.
    if (buf.size() < 512U) {
      buf.resize(512U, 0);
    }

    int bad_id = -1;
    EXPECT_EQ(NC_ENOTNC, nc_open_mem("ov_rec.nc", NC_NOWRITE, buf.size(),
                                     buf.data(), &bad_id));
    if (bad_id >= 0) {
      static_cast<void>(nc_close(bad_id));
    }
  }
}

TEST(Nc3InternalTest, CalcsizeComputesEmptyFixedHugeAndRecordSizes) {
  // 1) Empty variables dataset: calcsize == header xsz.
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc_create("calc_empty.nc", NC_DISKLESS, file.ptr()));
    ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));
    NC3_INFO* nc3 = GetNc3InfoOrNull(file.get());
    ASSERT_NE(nullptr, nc3);
    off_t sz = 0;
    EXPECT_EQ(NC_NOERR, NC_calcsize(nc3, &sz));
    EXPECT_EQ(static_cast<off_t>(nc3->xsz), sz);
    ASSERT_EQ(NC_NOERR, file.close());
  }

  // 2) Fixed-only normal and X_UINT_MAX (huge fixed var) branches.
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc_create("calc_fix.nc", NC_DISKLESS, file.ptr()));
    int d0 = -1;
    int d1 = -1;
    int v0 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "d0", 3, &d0));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "d1", 5, &d1));
    const std::array<int, 2> dims = {d0, d1};
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "a", NC_INT, 2, dims.data(), &v0));
    ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));
    NC3_INFO* nc3 = GetNc3InfoOrNull(file.get());
    ASSERT_NE(nullptr, nc3);

    off_t sz = 0;
    EXPECT_EQ(NC_NOERR, NC_calcsize(nc3, &sz));
    EXPECT_EQ(nc3->vars.value[0]->begin + nc3->vars.value[0]->len, sz);

    // Exercise last_fix->len == X_UINT_MAX branch with both valid shape and
    // NULL shape fallback.
    NC_var* varp = nc3->vars.value[0];
    const long long saved_len = varp->len;
    size_t* const saved_shape = varp->shape;
    absl::Cleanup restore = [&] {
      varp->len = saved_len;
      varp->shape = saved_shape;
    };

    varp->len = 4294967295LL;  // X_UINT_MAX
    EXPECT_EQ(NC_NOERR, NC_calcsize(nc3, &sz));
    EXPECT_EQ(varp->begin + 15, sz);

    varp->shape = nullptr;
    EXPECT_EQ(NC_NOERR, NC_calcsize(nc3, &sz));
    EXPECT_EQ(varp->begin + 1, sz);

    std::move(restore).Invoke();
    ASSERT_EQ(NC_NOERR, file.close());
  }

  // 3) Mixed fixed + record variables.
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc_create("calc_rec.nc", NC_DISKLESS, file.ptr()));
    int d_rec = -1;
    int d_one = -1;
    int v_fix = -1;
    int v_rec = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "rec", NC_UNLIMITED, &d_rec));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "one", 1, &d_one));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "fx", NC_INT, 1, &d_one, &v_fix));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "rc", NC_INT, 1, &d_rec, &v_rec));
    ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));
    NC3_INFO* nc3 = GetNc3InfoOrNull(file.get());
    ASSERT_NE(nullptr, nc3);

    off_t sz = 0;
    EXPECT_EQ(NC_NOERR, NC_calcsize(nc3, &sz));
    EXPECT_EQ(nc3->begin_rec, sz);

    const std::array<size_t, 1> i2 = {2};
    const int val = 99;
    ASSERT_EQ(NC_NOERR, nc_put_var1_int(file.get(), v_rec, i2.data(), &val));
    EXPECT_EQ(NC_NOERR, NC_calcsize(nc3, &sz));
    EXPECT_EQ(nc3->begin_rec + static_cast<off_t>(3U * nc3->recsize), sz);
    ASSERT_EQ(NC_NOERR, file.close());
  }
}

TEST(Nc3InternalTest, SharedNumrecsRedefAndSyncAcrossClassicAndCdf5) {
  ASSERT_EQ(NC_NOERR, nc_initialize());

  // Error checks on direct dispatch entry points.
  EXPECT_EQ(NC_EBADID, NC3_redef(-1));
  EXPECT_EQ(NC_EBADID, NC3_sync(-1));

  for (int format_flag : {0, NC_CDF5}) {
    TempFile tf(format_flag == 0 ? "share_cdf1.nc" : "share_cdf5.nc");
    ScopedNcFile writer;
    ASSERT_EQ(NC_NOERR,
              nc_create(tf.c_str(), NC_CLOBBER | NC_SHARE | format_flag,
                        writer.ptr()));
    int d_rec = -1;
    int v_rec = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(writer.get(), "rec", NC_UNLIMITED, &d_rec));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(writer.get(), "val", NC_INT, 1, &d_rec, &v_rec));
    // NC_EINDEFINE from NC3_redef and NC3_sync while still in define mode.
    EXPECT_EQ(NC_EINDEFINE, nc_redef(writer.get()));
    EXPECT_EQ(NC_EINDEFINE, nc_sync(writer.get()));
    ASSERT_EQ(NC_NOERR, nc_enddef(writer.get()));

    // Open concurrent shared reader before writer appends records.
    ScopedNcFile reader;
    ASSERT_EQ(NC_NOERR,
              nc_open(tf.c_str(), NC_NOWRITE | NC_SHARE, reader.ptr()));
    EXPECT_EQ(NC_EPERM, nc_redef(reader.get()));

    // Writer writes record 0 and record 1 -> write_numrecs on each write.
    const size_t r0 = 0;
    const size_t r1 = 1;
    const size_t r2 = 2;
    const int val0 = 10;
    const int val1 = 20;
    ASSERT_EQ(NC_NOERR, nc_put_var1_int(writer.get(), v_rec, &r0, &val0));
    ASSERT_EQ(NC_NOERR, nc_put_var1_int(writer.get(), v_rec, &r1, &val1));

    // Reader's numrecs is 0; reading coord 1 > 0 triggers read_numrecs
    // (old_nrecs != new_nrecs) and succeeds.
    int out = 0;
    ASSERT_EQ(NC_NOERR, nc_get_var1_int(reader.get(), v_rec, &r1, &out));
    EXPECT_EQ(20, out);
    // Out-of-bounds coord > 2 calls read_numrecs again with old == new.
    EXPECT_EQ(NC_EINVALCOORDS, nc_get_var1_int(reader.get(), v_rec, &r2, &out));

    // Writer adds an attribute via redef and enddef; reader calls readonly
    // nc_sync -> read_NC to pick up the updated header.
    ASSERT_EQ(NC_NOERR, nc_redef(writer.get()));
    const int tag = 77;
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(writer.get(), NC_GLOBAL, "tag", NC_INT, 1, &tag));
    ASSERT_EQ(NC_NOERR, nc_enddef(writer.get()));
    ASSERT_EQ(NC_NOERR, nc_sync(reader.get()));
    int tag_out = 0;
    ASSERT_EQ(NC_NOERR,
              nc_get_att_int(reader.get(), NC_GLOBAL, "tag", &tag_out));
    EXPECT_EQ(77, tag_out);

    ASSERT_EQ(NC_NOERR, reader.close());
    ASSERT_EQ(NC_NOERR, writer.close());

    // Reopen writable with NC_SHARE -> nc_redef invokes read_NC (L1377).
    ScopedNcFile rw_share;
    ASSERT_EQ(NC_NOERR,
              nc_open(tf.c_str(), NC_WRITE | NC_SHARE, rw_share.ptr()));
    ASSERT_EQ(NC_NOERR, nc_redef(rw_share.get()));
    ASSERT_EQ(NC_NOERR, nc_enddef(rw_share.get()));
    ASSERT_EQ(NC_NOERR, rw_share.close());
  }

  // Non-NC_SHARE writable sync paths: clean, ndirty (write_numrecs), and hdirty
  // (write_NC via same-length nc_rename_var in data mode).
  {
    TempFile tf("sync_dirty.nc");
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc_create(tf.c_str(), NC_CLOBBER, file.ptr()));
    int d_rec = -1;
    int v_rec = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "rec", NC_UNLIMITED, &d_rec));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file.get(), "v01", NC_INT, 1, &d_rec, &v_rec));
    ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));

    // Clean sync -> NC_NOERR.
    ASSERT_EQ(NC_NOERR, nc_sync(file.get()));

    // Dirty numrecs -> NC_sync calls write_numrecs.
    const size_t r0 = 0;
    const int val = 33;
    ASSERT_EQ(NC_NOERR, nc_put_var1_int(file.get(), v_rec, &r0, &val));
    ASSERT_EQ(NC_NOERR, nc_sync(file.get()));

    // Dirty header via same-length rename in data mode -> NC_sync calls
    // write_NC.
    ASSERT_EQ(NC_NOERR, nc_rename_var(file.get(), v_rec, "v02"));
    ASSERT_EQ(NC_NOERR, nc_sync(file.get()));
    ASSERT_EQ(NC_NOERR, file.close());
  }
}

TEST(Nc3InternalTest, SetFillAbortCloseNofillPaddingAndDelete) {
  ASSERT_EQ(NC_NOERR, nc_initialize());

  EXPECT_EQ(NC_EBADID, NC3_set_fill(-1, NC_FILL, nullptr));
  EXPECT_EQ(NC_EBADID, NC3_abort(-1));
  EXPECT_EQ(NC_EBADID, NC3_close(-1, nullptr));

  // NC3_abort during initial create unlinks the file on disk.
  {
    TempFile tf("abort_create.nc");
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc_create(tf.c_str(), NC_CLOBBER, file.ptr()));
    EXPECT_TRUE(std::filesystem::exists(tf.path()));
    ASSERT_EQ(NC_NOERR, file.abort());
    EXPECT_FALSE(std::filesystem::exists(tf.path()));
  }

  // NC3_abort during redef rolls back schema edits, and NC3_set_fill exercises
  // valid/invalid transitions, NULL old_modep, multi-var updates, and NC_sync.
  {
    TempFile tf("setfill_abort.nc");
    {
      ScopedNcFile file;
      ASSERT_EQ(NC_NOERR, nc_create(tf.c_str(), NC_CLOBBER, file.ptr()));
      int d_rec = -1;
      int d_len = -1;
      int v_rec = -1;
      int v_fix = -1;
      ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "rec", NC_UNLIMITED, &d_rec));
      ASSERT_EQ(NC_NOERR, nc_def_dim(file.get(), "len", 64, &d_len));
      ASSERT_EQ(NC_NOERR,
                nc_def_var(file.get(), "r0", NC_INT, 1, &d_rec, &v_rec));
      ASSERT_EQ(NC_NOERR,
                nc_def_var(file.get(), "f0", NC_INT, 1, &d_len, &v_fix));

      int old_mode = -1;
      EXPECT_EQ(NC_EINVAL, nc_set_fill(file.get(), 999, &old_mode));
      ASSERT_EQ(NC_NOERR, nc_set_fill(file.get(), NC_FILL, &old_mode));
      EXPECT_EQ(NC_FILL, old_mode);
      ASSERT_EQ(NC_NOERR, nc_set_fill(file.get(), NC_NOFILL, &old_mode));
      EXPECT_EQ(NC_FILL, old_mode);
      ASSERT_EQ(NC_NOERR, nc_set_fill(file.get(), NC_NOFILL, nullptr));

      // Leave define mode in NC_NOFILL so filesize < calcsize, then close ->
      // triggers ncio_pad_length in NC3_close (L1333-1337).
      ASSERT_EQ(NC_NOERR, nc_enddef(file.get()));
      NC3_INFO* nc3 = GetNc3InfoOrNull(file.get());
      ASSERT_NE(nullptr, nc3);
      off_t expected_size = 0;
      ASSERT_EQ(NC_NOERR, NC_calcsize(nc3, &expected_size));
      ASSERT_EQ(NC_NOERR, file.close());
      EXPECT_GE(static_cast<off_t>(std::filesystem::file_size(tf.path())),
                expected_size);
    }

    // Reopen RW: test NC_NOFILL -> NC_FILL while ndirty, then redef + abort.
    {
      ScopedNcFile file;
      ASSERT_EQ(NC_NOERR, nc_open(tf.c_str(), NC_WRITE, file.ptr()));
      int old_mode = -1;
      ASSERT_EQ(NC_NOERR, nc_set_fill(file.get(), NC_NOFILL, &old_mode));
      const size_t i0 = 0;
      const int v = 5;
      ASSERT_EQ(NC_NOERR, nc_put_var1_int(file.get(), 0, &i0, &v));
      // Switching back to NC_FILL calls NC_sync while NC_NDIRTY is set.
      ASSERT_EQ(NC_NOERR, nc_set_fill(file.get(), NC_FILL, &old_mode));
      EXPECT_EQ(NC_NOFILL, old_mode);

      ASSERT_EQ(NC_NOERR, nc_redef(file.get()));
      int v_extra = -1;
      ASSERT_EQ(NC_NOERR,
                nc_def_var(file.get(), "tmp", NC_INT, 0, nullptr, &v_extra));
      ASSERT_EQ(NC_NOERR, file.abort());
    }

    // Verify rolled back, plus rw dirty abort and readonly
    // abort/set_fill/close.
    {
      ScopedNcFile file;
      ASSERT_EQ(NC_NOERR, nc_open(tf.c_str(), NC_NOWRITE, file.ptr()));
      int nvars = 0;
      ASSERT_EQ(NC_NOERR, nc_inq_nvars(file.get(), &nvars));
      EXPECT_EQ(2, nvars);
      EXPECT_EQ(NC_EPERM, nc_set_fill(file.get(), NC_NOFILL, nullptr));
      ASSERT_EQ(NC_NOERR, file.abort());

      ASSERT_EQ(NC_NOERR, nc_open(tf.c_str(), NC_NOWRITE, file.ptr()));
      ASSERT_EQ(NC_NOERR, file.close());
    }

    {
      ScopedNcFile file;
      ASSERT_EQ(NC_NOERR, nc_open(tf.c_str(), NC_WRITE, file.ptr()));
      const size_t i1 = 1;
      const int v = 6;
      ASSERT_EQ(NC_NOERR, nc_put_var1_int(file.get(), 0, &i1, &v));
      // Abort in data-mode rw calls NC_sync on dirty numrecs.
      ASSERT_EQ(NC_NOERR, file.abort());
    }

    // NC_INMEMORY close via nc_close_memio.
    {
      ScopedNcFile mem_file;
      ASSERT_EQ(NC_NOERR,
                nc_create_mem("mem_close.nc", NC_CLOBBER, 0, mem_file.ptr()));
      ASSERT_EQ(NC_NOERR, nc_enddef(mem_file.get()));
      ScopedMemio mem;
      ASSERT_EQ(NC_NOERR, nc_close_memio(mem_file.get(), mem.get()));
      mem_file.release();
      EXPECT_NE(nullptr, mem.bytes());
      EXPECT_GE(mem.size(), 32U);
    }

    // nc_delete_mp with invalid basepe != 0 returns NC_EINVAL (and leaves an
    // internal open handle that we clean up explicitly).
    EXPECT_EQ(NC_EINVAL, nc_delete_mp(tf.c_str(), 1));
    if (NC* leaked = find_in_NCList_by_name(tf.c_str()); leaked != nullptr) {
      EXPECT_EQ(NC_NOERR, nc_close(leaked->ext_ncid));
    }

    // nc_delete deletes the file; repeating fails on missing path.
    EXPECT_EQ(NC_NOERR, nc_delete(tf.c_str()));
    EXPECT_FALSE(std::filesystem::exists(tf.path()));
    EXPECT_NE(NC_NOERR, nc_delete(tf.c_str()));
  }

  // Test unlink failure -> NC_EIO when directory denies write (non-root).
  if (geteuid() != 0) {
    TempDir ro_dir("nc3_ro_unlink");
    const std::filesystem::path ro_file = ro_dir.child("locked.nc");
    {
      ScopedNcFile file;
      ASSERT_EQ(NC_NOERR, nc_create(ro_file.c_str(), NC_CLOBBER, file.ptr()));
      ASSERT_EQ(NC_NOERR, file.close());
    }
    std::error_code ec;
    std::filesystem::permissions(ro_dir.path(),
                                 std::filesystem::perms::owner_write |
                                     std::filesystem::perms::group_write |
                                     std::filesystem::perms::others_write,
                                 std::filesystem::perm_options::remove, ec);
    ASSERT_FALSE(ec);
    absl::Cleanup restore_perm = [&] {
      std::error_code restore_ec;
      std::filesystem::permissions(
          ro_dir.path(), std::filesystem::perms::owner_write,
          std::filesystem::perm_options::add, restore_ec);
    };
    EXPECT_EQ(NC_EIO, nc_delete_mp(ro_file.c_str(), 0));
    EXPECT_TRUE(std::filesystem::exists(ro_file));
  }
}

TEST(Nc3InternalTest, InqDefaultFillValueAndInqVarFillAllTypesAndErrors) {
  ASSERT_EQ(NC_NOERR, nc_initialize());

  // 1) NC3_inq_default_fill_value: NULL fillp, invalid types, all 11 types.
  EXPECT_EQ(NC_NOERR, NC3_inq_default_fill_value(NC_INT, nullptr));
  int unused_buf = 0;
  EXPECT_EQ(NC_EBADTYPE, NC3_inq_default_fill_value(NC_NAT, &unused_buf));
  EXPECT_EQ(NC_EBADTYPE, NC3_inq_default_fill_value(NC_STRING, &unused_buf));

  char c_fill = 0;
  signed char b_fill = 0;
  short s_fill = 0;
  int i_fill = 0;
  float f_fill = 0.0F;
  double d_fill = 0.0;
  unsigned char ub_fill = 0;
  unsigned short us_fill = 0;
  unsigned int ui_fill = 0;
  long long i64_fill = 0;
  unsigned long long ui64_fill = 0;

  EXPECT_EQ(NC_NOERR, NC3_inq_default_fill_value(NC_CHAR, &c_fill));
  EXPECT_EQ(NC_FILL_CHAR, c_fill);
  EXPECT_EQ(NC_NOERR, NC3_inq_default_fill_value(NC_BYTE, &b_fill));
  EXPECT_EQ(NC_FILL_BYTE, b_fill);
  EXPECT_EQ(NC_NOERR, NC3_inq_default_fill_value(NC_SHORT, &s_fill));
  EXPECT_EQ(NC_FILL_SHORT, s_fill);
  EXPECT_EQ(NC_NOERR, NC3_inq_default_fill_value(NC_INT, &i_fill));
  EXPECT_EQ(NC_FILL_INT, i_fill);
  EXPECT_EQ(NC_NOERR, NC3_inq_default_fill_value(NC_FLOAT, &f_fill));
  EXPECT_EQ(NC_FILL_FLOAT, f_fill);
  EXPECT_EQ(NC_NOERR, NC3_inq_default_fill_value(NC_DOUBLE, &d_fill));
  EXPECT_EQ(NC_FILL_DOUBLE, d_fill);
  EXPECT_EQ(NC_NOERR, NC3_inq_default_fill_value(NC_UBYTE, &ub_fill));
  EXPECT_EQ(NC_FILL_UBYTE, ub_fill);
  EXPECT_EQ(NC_NOERR, NC3_inq_default_fill_value(NC_USHORT, &us_fill));
  EXPECT_EQ(NC_FILL_USHORT, us_fill);
  EXPECT_EQ(NC_NOERR, NC3_inq_default_fill_value(NC_UINT, &ui_fill));
  EXPECT_EQ(NC_FILL_UINT, ui_fill);
  EXPECT_EQ(NC_NOERR, NC3_inq_default_fill_value(NC_INT64, &i64_fill));
  EXPECT_EQ(NC_FILL_INT64, i64_fill);
  EXPECT_EQ(NC_NOERR, NC3_inq_default_fill_value(NC_UINT64, &ui64_fill));
  EXPECT_EQ(NC_FILL_UINT64, ui64_fill);

  // 2) NC3_inq_var_fill: default values for all 11 CDF-5 variable types.
  struct TypeSpec {
    const char* name;
    nc_type type;
  };
  constexpr std::array<TypeSpec, 11> kTypes = {{
      {"vc", NC_CHAR},
      {"vb", NC_BYTE},
      {"vs", NC_SHORT},
      {"vi", NC_INT},
      {"vf", NC_FLOAT},
      {"vd", NC_DOUBLE},
      {"vub", NC_UBYTE},
      {"vus", NC_USHORT},
      {"vui", NC_UINT},
      {"vi64", NC_INT64},
      {"vu64", NC_UINT64},
  }};

  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create("varfill_def.nc", NC_DISKLESS | NC_CDF5, file.ptr()));
    std::array<int, 11> vids{};
    for (size_t i = 0; i < kTypes.size(); ++i) {
      ASSERT_EQ(NC_NOERR, nc_def_var(file.get(), kTypes[i].name, kTypes[i].type,
                                     0, nullptr, &vids[i]));
    }
    NC3_INFO* nc3 = GetNc3InfoOrNull(file.get());
    ASSERT_NE(nullptr, nc3);
    EXPECT_EQ(NC_EINVAL, NC3_inq_var_fill(nc3->vars.value[0], nullptr));

    EXPECT_EQ(NC_NOERR, NC3_inq_var_fill(nc3->vars.value[0], &c_fill));
    EXPECT_EQ(NC_FILL_CHAR, c_fill);
    EXPECT_EQ(NC_NOERR, NC3_inq_var_fill(nc3->vars.value[1], &b_fill));
    EXPECT_EQ(NC_FILL_BYTE, b_fill);
    EXPECT_EQ(NC_NOERR, NC3_inq_var_fill(nc3->vars.value[2], &s_fill));
    EXPECT_EQ(NC_FILL_SHORT, s_fill);
    EXPECT_EQ(NC_NOERR, NC3_inq_var_fill(nc3->vars.value[3], &i_fill));
    EXPECT_EQ(NC_FILL_INT, i_fill);
    EXPECT_EQ(NC_NOERR, NC3_inq_var_fill(nc3->vars.value[4], &f_fill));
    EXPECT_EQ(NC_FILL_FLOAT, f_fill);
    EXPECT_EQ(NC_NOERR, NC3_inq_var_fill(nc3->vars.value[5], &d_fill));
    EXPECT_EQ(NC_FILL_DOUBLE, d_fill);
    EXPECT_EQ(NC_NOERR, NC3_inq_var_fill(nc3->vars.value[6], &ub_fill));
    EXPECT_EQ(NC_FILL_UBYTE, ub_fill);
    EXPECT_EQ(NC_NOERR, NC3_inq_var_fill(nc3->vars.value[7], &us_fill));
    EXPECT_EQ(NC_FILL_USHORT, us_fill);
    EXPECT_EQ(NC_NOERR, NC3_inq_var_fill(nc3->vars.value[8], &ui_fill));
    EXPECT_EQ(NC_FILL_UINT, ui_fill);
    EXPECT_EQ(NC_NOERR, NC3_inq_var_fill(nc3->vars.value[9], &i64_fill));
    EXPECT_EQ(NC_FILL_INT64, i64_fill);
    EXPECT_EQ(NC_NOERR, NC3_inq_var_fill(nc3->vars.value[10], &ui64_fill));
    EXPECT_EQ(NC_FILL_UINT64, ui64_fill);
    ASSERT_EQ(NC_NOERR, file.close());
  }

  // 3) NC3_inq_var_fill: custom NC_FillValue for all 11 types.
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc_create("varfill_custom.nc", NC_DISKLESS | NC_CDF5,
                                  file.ptr()));
    std::array<int, 11> vids{};
    for (size_t i = 0; i < kTypes.size(); ++i) {
      ASSERT_EQ(NC_NOERR, nc_def_var(file.get(), kTypes[i].name, kTypes[i].type,
                                     0, nullptr, &vids[i]));
    }

    const char c_custom = 'Q';
    const signed char b_custom = -42;
    const short s_custom = -1234;
    const int i_custom = -987654;
    const float f_custom = 3.25F;
    const double d_custom = -6.5;
    const unsigned char ub_custom = 200U;
    const unsigned short us_custom = 50000U;
    const unsigned int ui_custom = 3000000000U;
    const long long i64_custom = -1234567890123LL;
    const unsigned long long ui64_custom = 9876543210987ULL;

    ASSERT_EQ(NC_NOERR, nc_def_var_fill(file.get(), vids[0], 0, &c_custom));
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(file.get(), vids[1], 0, &b_custom));
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(file.get(), vids[2], 0, &s_custom));
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(file.get(), vids[3], 0, &i_custom));
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(file.get(), vids[4], 0, &f_custom));
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(file.get(), vids[5], 0, &d_custom));
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(file.get(), vids[6], 0, &ub_custom));
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(file.get(), vids[7], 0, &us_custom));
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(file.get(), vids[8], 0, &ui_custom));
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(file.get(), vids[9], 0, &i64_custom));
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(file.get(), vids[10], 0, &ui64_custom));

    NC3_INFO* nc3 = GetNc3InfoOrNull(file.get());
    ASSERT_NE(nullptr, nc3);

    c_fill = 0;
    b_fill = 0;
    s_fill = 0;
    i_fill = 0;
    f_fill = 0.0F;
    d_fill = 0.0;
    ub_fill = 0;
    us_fill = 0;
    ui_fill = 0;
    i64_fill = 0;
    ui64_fill = 0;

    EXPECT_EQ(NC_NOERR, NC3_inq_var_fill(nc3->vars.value[0], &c_fill));
    EXPECT_EQ(c_custom, c_fill);
    EXPECT_EQ(NC_NOERR, NC3_inq_var_fill(nc3->vars.value[1], &b_fill));
    EXPECT_EQ(b_custom, b_fill);
    EXPECT_EQ(NC_NOERR, NC3_inq_var_fill(nc3->vars.value[2], &s_fill));
    EXPECT_EQ(s_custom, s_fill);
    EXPECT_EQ(NC_NOERR, NC3_inq_var_fill(nc3->vars.value[3], &i_fill));
    EXPECT_EQ(i_custom, i_fill);
    EXPECT_EQ(NC_NOERR, NC3_inq_var_fill(nc3->vars.value[4], &f_fill));
    EXPECT_FLOAT_EQ(f_custom, f_fill);
    EXPECT_EQ(NC_NOERR, NC3_inq_var_fill(nc3->vars.value[5], &d_fill));
    EXPECT_DOUBLE_EQ(d_custom, d_fill);
    EXPECT_EQ(NC_NOERR, NC3_inq_var_fill(nc3->vars.value[6], &ub_fill));
    EXPECT_EQ(ub_custom, ub_fill);
    EXPECT_EQ(NC_NOERR, NC3_inq_var_fill(nc3->vars.value[7], &us_fill));
    EXPECT_EQ(us_custom, us_fill);
    EXPECT_EQ(NC_NOERR, NC3_inq_var_fill(nc3->vars.value[8], &ui_fill));
    EXPECT_EQ(ui_custom, ui_fill);
    EXPECT_EQ(NC_NOERR, NC3_inq_var_fill(nc3->vars.value[9], &i64_fill));
    EXPECT_EQ(i64_custom, i64_fill);
    EXPECT_EQ(NC_NOERR, NC3_inq_var_fill(nc3->vars.value[10], &ui64_fill));
    EXPECT_EQ(ui64_custom, ui64_fill);

    ASSERT_EQ(NC_NOERR, file.close());
  }

  // 4) NC3_inq_var_fill + fillerup error: bad NC_FillValue type/length attrs.
  {
    ScopedNcFile bad_file;
    ASSERT_EQ(NC_NOERR, nc_create("varfill_bad_attr.nc", NC_DISKLESS | NC_CDF5,
                                  bad_file.ptr()));
    int bad_t = -1;
    int bad_n = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(bad_file.get(), "bad_t", NC_INT, 0, nullptr, &bad_t));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(bad_file.get(), "bad_n", NC_INT, 0, nullptr, &bad_n));
    NC3_INFO* bad_nc3 = GetNc3InfoOrNull(bad_file.get());
    ASSERT_NE(nullptr, bad_nc3);
    ASSERT_GE(bad_t, 0);
    ASSERT_GE(bad_n, 0);
    ASSERT_LT(static_cast<size_t>(bad_t), bad_nc3->vars.nelems);
    ASSERT_LT(static_cast<size_t>(bad_n), bad_nc3->vars.nelems);

    const float wrong_type_val = 1.5F;
    EXPECT_EQ(NC_NOERR, nc_put_att_float(bad_file.get(), bad_t, NC_FillValue,
                                         NC_FLOAT, 1, &wrong_type_val));
    const std::array<int, 2> two_vals = {1, 2};
    EXPECT_EQ(NC_NOERR, nc_put_att_int(bad_file.get(), bad_n, NC_FillValue,
                                       NC_INT, 2, two_vals.data()));

    int err_fill = 0;
    EXPECT_EQ(NC_EBADTYPE,
              NC3_inq_var_fill(bad_nc3->vars.value[bad_t], &err_fill));
    EXPECT_EQ(NC_EBADTYPE,
              NC3_inq_var_fill(bad_nc3->vars.value[bad_n], &err_fill));
    // Invalid NC_FillValue makes nc_enddef/fillerup fail with NC_EBADTYPE;
    // abort instead of close so NC3_close does not orphan the NC entry.
    EXPECT_EQ(NC_EBADTYPE, nc_enddef(bad_file.get()));
    ASSERT_EQ(NC_NOERR, bad_file.abort());
  }

  // 5) Stack-constructed invalid var types: default -> NC_EINVAL,
  // with matching invalid-type NC_FillValue attribute -> NC_EBADTYPE.
  {
    NC_var bad_var{};
    bad_var.type = NC_STRING;
    uint64_t dummy_out = 0;
    EXPECT_EQ(NC_EINVAL, NC3_inq_var_fill(&bad_var, &dummy_out));

    constexpr std::string_view kFillName = NC_FillValue;
    std::array<char, 16> attr_name_buf{};
    std::copy(kFillName.begin(), kFillName.end(), attr_name_buf.begin());
    NC_string attr_name{};
    attr_name.nchars = kFillName.size();
    attr_name.cp = attr_name_buf.data();

    NC_attr bad_attr{};
    bad_attr.name = &attr_name;
    bad_attr.type = NC_STRING;
    bad_attr.nelems = 1;
    bad_attr.xvalue = &dummy_out;

    NC_attr* attr_ptrs[1] = {&bad_attr};
    bad_var.attrs.nalloc = 1;
    bad_var.attrs.nelems = 1;
    bad_var.attrs.value = attr_ptrs;
    EXPECT_EQ(NC_EBADTYPE, NC3_inq_var_fill(&bad_var, &dummy_out));
  }
}

}  // namespace
}  // namespace netcdf
