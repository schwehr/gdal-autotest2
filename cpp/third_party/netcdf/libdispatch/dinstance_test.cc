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

// Unit tests for //third_party/netcdf/libdispatch/dinstance.c.
//
// Exercises public and internal instance copy, reclaim, alignment, type
// inquiry, dump, and print operations across atomic and nested NetCDF-4 types.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/dinstance.c

#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"

extern "C" {
#include "third_party/netcdf/include/ncoffsets.h"

int nc_dump_data(int ncid, nc_type xtype, const void* memory, size_t count,
                 char** bufp);
int nc_print_data(int ncid, nc_type xtype, const void* memory, size_t count);
int NC_inq_any_type(int ncid, nc_type nc_typeid, char* name, size_t* size,
                    nc_type* basetypep, size_t* nfieldsp, int* classp);
}  // extern "C"

namespace netcdf {
namespace {

template <typename Fn>
std::string CaptureStderr(Fn&& fn) {
  std::fflush(stderr);
  int pipe_fds[2] = {-1, -1};
  if (pipe(pipe_fds) != 0) {
    return {};
  }
  int saved_stderr = dup(STDERR_FILENO);
  absl::Cleanup cleanup = [&]() {
    std::fflush(stderr);
    if (saved_stderr >= 0) {
      dup2(saved_stderr, STDERR_FILENO);
      close(saved_stderr);
      saved_stderr = -1;
    }
    if (pipe_fds[0] >= 0) {
      close(pipe_fds[0]);
      pipe_fds[0] = -1;
    }
    if (pipe_fds[1] >= 0) {
      close(pipe_fds[1]);
      pipe_fds[1] = -1;
    }
  };
  if (saved_stderr < 0 || dup2(pipe_fds[1], STDERR_FILENO) < 0) {
    return {};
  }
  close(pipe_fds[1]);
  pipe_fds[1] = -1;

  fn();
  std::fflush(stderr);

  // Restore stderr before reading so the pipe write end has no open FDs.
  dup2(saved_stderr, STDERR_FILENO);
  close(saved_stderr);
  saved_stderr = -1;

  std::string captured;
  char chunk[256];
  for (;;) {
    const ssize_t n = read(pipe_fds[0], chunk, sizeof(chunk));
    if (n > 0) {
      captured.append(chunk, static_cast<size_t>(n));
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }
  close(pipe_fds[0]);
  pipe_fds[0] = -1;
  return captured;
}

TEST(DinstanceTest, ParameterValidationAndNoOpPaths) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("dinstance_params.nc", NC_NETCDF4, 0, nc.ptr()));

  int stale_id = -1;
  {
    ScopedNcFile tmp;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("dinstance_stale.nc", NC_CLOBBER, 0, tmp.ptr()));
    stale_id = tmp.get();
  }

  int32_t src_val = 42;
  int32_t dst_val = 0;

  // nc_reclaim_data checks.
  EXPECT_EQ(NC_EINVAL, nc_reclaim_data(-1, NC_INT, &src_val, 1));
  EXPECT_EQ(NC_EINVAL, nc_reclaim_data(nc.get(), NC_NAT, &src_val, 1));
  EXPECT_EQ(NC_EINVAL, nc_reclaim_data(nc.get(), -3, &src_val, 1));
  EXPECT_EQ(NC_EINVAL, nc_reclaim_data(nc.get(), NC_INT, nullptr, 1));
  EXPECT_EQ(NC_NOERR, nc_reclaim_data(nc.get(), NC_INT, nullptr, 0));
  EXPECT_EQ(NC_NOERR, nc_reclaim_data(nc.get(), NC_INT, &src_val, 0));
  EXPECT_EQ(NC_EBADID, nc_reclaim_data(stale_id, NC_INT, &src_val, 1));

  // nc_reclaim_data_all error and NULL no-op.
  EXPECT_EQ(NC_EINVAL, nc_reclaim_data_all(-1, NC_INT, &src_val, 1));
  EXPECT_EQ(NC_NOERR, nc_reclaim_data_all(nc.get(), NC_INT, nullptr, 0));

  // nc_copy_data checks.
  EXPECT_EQ(NC_EINVAL, nc_copy_data(-1, NC_INT, &src_val, 1, &dst_val));
  EXPECT_EQ(NC_EINVAL, nc_copy_data(nc.get(), NC_NAT, &src_val, 1, &dst_val));
  EXPECT_EQ(NC_EINVAL, nc_copy_data(nc.get(), -2, &src_val, 1, &dst_val));
  EXPECT_EQ(NC_EINVAL, nc_copy_data(nc.get(), NC_INT, nullptr, 1, &dst_val));
  EXPECT_EQ(NC_EINVAL, nc_copy_data(nc.get(), NC_INT, &src_val, 1, nullptr));
  EXPECT_EQ(NC_NOERR, nc_copy_data(nc.get(), NC_INT, nullptr, 0, &dst_val));
  EXPECT_EQ(NC_NOERR, nc_copy_data(nc.get(), NC_INT, &src_val, 0, nullptr));
  EXPECT_EQ(NC_EBADID, nc_copy_data(stale_id, NC_INT, &src_val, 1, &dst_val));

  // nc_copy_data_all type failure, count==0, and error cleanup branch.
  void* copy_out = &dst_val;
  EXPECT_EQ(NC_EBADTYPE,
            nc_copy_data_all(nc.get(), 9999, &src_val, 1, &copy_out));
  EXPECT_EQ(NC_NOERR,
            nc_copy_data_all(nc.get(), NC_INT, &src_val, 0, &copy_out));
  EXPECT_EQ(nullptr, copy_out);

  // When copyp is NULL and nc_copy_data fails, internal cleanup frees copy.
  EXPECT_EQ(NC_NOERR, nc_copy_data_all(nc.get(), NC_INT, nullptr, 1, nullptr));

  // When copyp is non-NULL and nc_copy_data fails, caller receives and frees
  // the zero-initialized top-level allocation.
  void* err_copy = nullptr;
  EXPECT_EQ(NC_EINVAL,
            nc_copy_data_all(nc.get(), NC_INT, nullptr, 1, &err_copy));
  ASSERT_NE(nullptr, err_copy);
  EXPECT_EQ(NC_NOERR, nc_reclaim_data_all(nc.get(), NC_INT, err_copy, 1));

  // nc_dump_data validation, no-op, and NULL bufp.
  char* dump_buf = nullptr;
  EXPECT_EQ(NC_EINVAL, nc_dump_data(-1, NC_INT, &src_val, 1, &dump_buf));
  EXPECT_EQ(NC_EINVAL, nc_dump_data(nc.get(), NC_NAT, &src_val, 1, &dump_buf));
  EXPECT_EQ(NC_EINVAL, nc_dump_data(nc.get(), NC_INT, nullptr, 1, &dump_buf));
  EXPECT_EQ(NC_NOERR, nc_dump_data(nc.get(), NC_INT, nullptr, 0, &dump_buf));
  EXPECT_EQ(nullptr, dump_buf);
  EXPECT_EQ(NC_NOERR, nc_dump_data(nc.get(), NC_INT, &src_val, 0, &dump_buf));
  EXPECT_EQ(nullptr, dump_buf);
  EXPECT_EQ(NC_NOERR, nc_dump_data(nc.get(), NC_INT, &src_val, 1, nullptr));
  EXPECT_EQ(NC_EBADTYPE, nc_dump_data(nc.get(), 9999, &src_val, 2, &dump_buf));
  EXPECT_EQ(nullptr, dump_buf);

  // nc_print_data error propagation.
  EXPECT_EQ(NC_EINVAL, nc_print_data(-1, NC_INT, &src_val, 1));
}

TEST(DinstanceTest, InqAnyTypeAndTypeAlignmentCoverAllCategories) {
  ScopedNcFile nc3;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("dinstance_align3.nc", NC_CLOBBER, 0, nc3.ptr()));

  ScopedNcFile nc4;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("dinstance_align4.nc", NC_NETCDF4, 0, nc4.ptr()));

  nc_type opaque_id = NC_NAT;
  nc_type enum_id = NC_NAT;
  nc_type vlen_id = NC_NAT;
  nc_type cmpd_id = NC_NAT;
  struct Pair {
    int32_t a;
    double b;
  };
  ASSERT_EQ(NC_NOERR, nc_def_opaque(nc4.get(), 4, "op4", &opaque_id));
  ASSERT_EQ(NC_NOERR, nc_def_enum(nc4.get(), NC_INT, "color_e", &enum_id));
  const int enum_zero = 0;
  ASSERT_EQ(NC_NOERR, nc_insert_enum(nc4.get(), enum_id, "NONE", &enum_zero));
  ASSERT_EQ(NC_NOERR, nc_def_vlen(nc4.get(), "ivlen", NC_INT, &vlen_id));
  ASSERT_EQ(NC_NOERR,
            nc_def_compound(nc4.get(), sizeof(Pair), "pair_t", &cmpd_id));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(nc4.get(), cmpd_id, "a",
                                         NC_COMPOUND_OFFSET(Pair, a), NC_INT));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(nc4.get(), cmpd_id, "b",
                               NC_COMPOUND_OFFSET(Pair, b), NC_DOUBLE));

  // Query all atomic types with full out-parameters and NULL optional params.
  for (nc_type t = NC_BYTE; t <= NC_STRING; ++t) {
    char name[NC_MAX_NAME + 1]{};
    size_t sz = 0;
    nc_type base = NC_STRING;
    size_t nfields = 99;
    int klass = -1;
    EXPECT_EQ(NC_NOERR, NC_inq_any_type(nc4.get(), t, name, &sz, &base,
                                        &nfields, &klass));
    EXPECT_NE('\0', name[0]);
    EXPECT_GT(sz, 0U);
    EXPECT_EQ(NC_NAT, base);
    EXPECT_EQ(0U, nfields);
    EXPECT_EQ(t, klass);

    EXPECT_EQ(NC_NOERR, ncaux_inq_any_type(-1, t, nullptr, nullptr, nullptr,
                                           nullptr, nullptr));
  }

  // Query user types via both NC_inq_any_type and ncaux_inq_any_type.
  size_t sz = 0;
  nc_type base = NC_NAT;
  size_t nfields = 0;
  int klass = -1;
  ASSERT_EQ(NC_NOERR, NC_inq_any_type(nc4.get(), cmpd_id, nullptr, &sz, &base,
                                      &nfields, &klass));
  EXPECT_EQ(sizeof(Pair), sz);
  EXPECT_EQ(2U, nfields);
  EXPECT_EQ(NC_COMPOUND, klass);
  EXPECT_EQ(NC_NOERR, ncaux_inq_any_type(nc4.get(), vlen_id, nullptr, &sz,
                                         &base, &nfields, &klass));
  EXPECT_EQ(sizeof(nc_vlen_t), sz);
  EXPECT_EQ(NC_INT, base);
  EXPECT_EQ(NC_VLEN, klass);

  // Invalid type IDs and bad ncid for user types.
  EXPECT_EQ(NC_EBADTYPE, NC_inq_any_type(nc4.get(), NC_NAT, nullptr, nullptr,
                                         nullptr, nullptr, nullptr));
  EXPECT_EQ(NC_EBADTYPE, NC_inq_any_type(nc4.get(), -1, nullptr, nullptr,
                                         nullptr, nullptr, nullptr));
  EXPECT_EQ(NC_EBADTYPE,
            NC_inq_any_type(nc4.get(), NC_MAX_ATOMIC_TYPE + 1, nullptr, nullptr,
                            nullptr, nullptr, nullptr));
  EXPECT_EQ(NC_EBADTYPE, NC_inq_any_type(nc4.get(), 9999, nullptr, nullptr,
                                         nullptr, nullptr, nullptr));
  EXPECT_EQ(NC_EBADID, NC_inq_any_type(-1, cmpd_id, nullptr, nullptr, nullptr,
                                       nullptr, nullptr));

  // NC_type_alignment: invalid ID, NC3 early-exit, and NC4 types.
  size_t align = 0;
  EXPECT_EQ(NC_EBADID, NC_type_alignment(-1, NC_INT, &align));
  EXPECT_EQ(NC_NOERR, NC_type_alignment(nc3.get(), NC_INT, &align));
  EXPECT_EQ(NC_NOERR, NC_type_alignment(nc4.get(), NC_DOUBLE, &align));
  EXPECT_EQ(NC_NOERR, NC_type_alignment(nc4.get(), opaque_id, &align));
  EXPECT_GE(align, 1U);
  EXPECT_EQ(NC_NOERR, NC_type_alignment(nc4.get(), vlen_id, &align));
  EXPECT_EQ(alignof(nc_vlen_t), align);
  EXPECT_EQ(NC_NOERR, ncaux_type_alignment(cmpd_id, nc4.get(), &align));
}

TEST(DinstanceTest, DumpAndPrintDataCoverAllAtomicAndUserTypes) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("dinstance_dump.nc", NC_NETCDF4, 0, nc.ptr()));

  auto dump_to_string = [&](nc_type xtype, const void* data, size_t count,
                            std::string* out) {
    char* raw = nullptr;
    const int rc = nc_dump_data(nc.get(), xtype, data, count, &raw);
    ScopedCString guard = MakeScopedCString(raw);
    ASSERT_EQ(NC_NOERR, rc);
    ASSERT_NE(nullptr, raw);
    *out = raw;
  };

  std::string out;

  // All atomic scalar & vector types (using non-negative values for NC_BYTE
  // and <= INT_MAX values for NC_UINT for cross-arch signed-char parity).
  const std::array<char, 2> chars{'A', 'Z'};
  dump_to_string(NC_CHAR, chars.data(), chars.size(), &out);
  EXPECT_EQ("'A' 'Z'", out);

  const std::array<signed char, 2> sbytes{7, 42};
  dump_to_string(NC_BYTE, sbytes.data(), sbytes.size(), &out);
  EXPECT_EQ("7 42", out);

  const std::array<unsigned char, 2> ubytes{0, 255};
  dump_to_string(NC_UBYTE, ubytes.data(), ubytes.size(), &out);
  EXPECT_EQ("0 255", out);

  const std::array<short, 2> shorts{-321, 321};
  dump_to_string(NC_SHORT, shorts.data(), shorts.size(), &out);
  EXPECT_EQ("-321 321", out);

  const std::array<unsigned short, 2> ushorts{10, 65000};
  dump_to_string(NC_USHORT, ushorts.data(), ushorts.size(), &out);
  EXPECT_EQ("10 65000", out);

  const std::array<int, 2> ints{-42, 99};
  dump_to_string(NC_INT, ints.data(), ints.size(), &out);
  EXPECT_EQ("-42 99", out);

  const std::array<unsigned int, 2> uints{0U, 42000U};
  dump_to_string(NC_UINT, uints.data(), uints.size(), &out);
  EXPECT_EQ("0 42000", out);

  const float fval = 1.5F;
  dump_to_string(NC_FLOAT, &fval, 1, &out);
  EXPECT_THAT(out, ::testing::HasSubstr("1.5"));

  const int64_t i64val = -9000000000LL;
  dump_to_string(NC_INT64, &i64val, 1, &out);
  EXPECT_EQ("-9000000000", out);

  const uint64_t u64val = 9000000000ULL;
  dump_to_string(NC_UINT64, &u64val, 1, &out);
  EXPECT_EQ("9000000000", out);

  const double dval = 2.25;
  dump_to_string(NC_DOUBLE, &dval, 1, &out);
  EXPECT_THAT(out, ::testing::HasSubstr("2.25"));

  char str_a[] = "alpha";
  char str_b[] = "beta";
  char* str_ptrs[2] = {str_a, str_b};
  {
    char* aux_buf = nullptr;
    const int rc = ncaux_dump_data(nc.get(), NC_STRING, str_ptrs, 2, &aux_buf);
    ScopedCString guard = MakeScopedCString(aux_buf);
    ASSERT_EQ(NC_NOERR, rc);
    ASSERT_NE(nullptr, aux_buf);
    EXPECT_STREQ("\"alpha\" \"beta\"", aux_buf);
  }

  // Opaque type.
  nc_type opq_type = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_opaque(nc.get(), 3, "opq3", &opq_type));
  const std::array<unsigned char, 3> opq_bytes{0xab, 0xcd, 0xef};
  dump_to_string(opq_type, opq_bytes.data(), 1, &out);
  EXPECT_EQ("|abcdef|", out);

  // Enum type.
  nc_type enum_type = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_enum(nc.get(), NC_INT, "state_e", &enum_type));
  const int state_idle = 1;
  const int state_busy = 2;
  ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), enum_type, "IDLE", &state_idle));
  ASSERT_EQ(NC_NOERR, nc_insert_enum(nc.get(), enum_type, "BUSY", &state_busy));
  dump_to_string(enum_type, &state_busy, 1, &out);
  EXPECT_EQ("2", out);

  // VLEN types: empty, populated, nested vlen-of-vlen, and invalid NULL p.
  nc_type vlen_int = NC_NAT;
  nc_type vlen_vlen = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_vlen(nc.get(), "vi", NC_INT, &vlen_int));
  ASSERT_EQ(NC_NOERR, nc_def_vlen(nc.get(), "vvi", vlen_int, &vlen_vlen));

  std::array<int, 2> vlen_elems{10, 20};
  const std::array<nc_vlen_t, 2> vlens{
      nc_vlen_t{0, nullptr},
      nc_vlen_t{vlen_elems.size(), vlen_elems.data()},
  };
  dump_to_string(vlen_int, vlens.data(), vlens.size(), &out);
  EXPECT_EQ("{len=0,p=()} {len=2,p=(10 20)}", out);

  std::array<nc_vlen_t, 2> inner_vec{vlens[0], vlens[1]};
  const nc_vlen_t outer{inner_vec.size(), inner_vec.data()};
  dump_to_string(vlen_vlen, &outer, 1, &out);
  EXPECT_EQ("{len=2,p=({len=0,p=()} {len=2,p=(10 20)})}", out);

  const nc_vlen_t invalid_vlen{2, nullptr};
  {
    char* bad_buf = nullptr;
    const int rc = nc_dump_data(nc.get(), vlen_int, &invalid_vlen, 1, &bad_buf);
    ScopedCString bad_guard = MakeScopedCString(bad_buf);
    EXPECT_EQ(NC_EINVAL, rc);
    EXPECT_EQ(nullptr, bad_buf);
  }

  std::array<nc_vlen_t, 1> bad_inner{invalid_vlen};
  const nc_vlen_t bad_outer{bad_inner.size(), bad_inner.data()};
  {
    char* bad_buf = nullptr;
    const int rc = nc_dump_data(nc.get(), vlen_vlen, &bad_outer, 1, &bad_buf);
    ScopedCString bad_guard = MakeScopedCString(bad_buf);
    EXPECT_EQ(NC_EINVAL, rc);
    // nc_dump_data extracts any partial bytes buffered before a nested error.
    ASSERT_NE(nullptr, bad_buf);
    EXPECT_STREQ("{len=1,p=(", bad_buf);
  }

  // Compound type with scalar field, 2D array field, string, and vlen.
  struct RichRec {
    int32_t id;
    int32_t mat[2][2];
    char* label;
    nc_vlen_t nums;
  };
  nc_type cmpd_type = NC_NAT;
  ASSERT_EQ(NC_NOERR,
            nc_def_compound(nc.get(), sizeof(RichRec), "rich_t", &cmpd_type));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(nc.get(), cmpd_type, "id",
                               NC_COMPOUND_OFFSET(RichRec, id), NC_INT));
  const std::array<int, 2> mat_dims{2, 2};
  ASSERT_EQ(NC_NOERR, nc_insert_array_compound(nc.get(), cmpd_type, "mat",
                                               NC_COMPOUND_OFFSET(RichRec, mat),
                                               NC_INT, 2, mat_dims.data()));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(nc.get(), cmpd_type, "label",
                               NC_COMPOUND_OFFSET(RichRec, label), NC_STRING));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(nc.get(), cmpd_type, "nums",
                               NC_COMPOUND_OFFSET(RichRec, nums), vlen_int));

  char label_lit[] = "node0";
  std::array<int, 2> rec_nums{5, 6};
  RichRec rec{};
  rec.id = 7;
  rec.mat[0][0] = 1;
  rec.mat[0][1] = 2;
  rec.mat[1][0] = 3;
  rec.mat[1][1] = 4;
  rec.label = label_lit;
  rec.nums = nc_vlen_t{rec_nums.size(), rec_nums.data()};
  dump_to_string(cmpd_type, &rec, 1, &out);
  EXPECT_EQ("<id()7;mat(2,2)1 2 3 4;label()\"node0\";nums(){len=2,p=(5 6)}>",
            out);

  // Verify nc_print_data emits the formatted dump line to stderr.
  int print_rc = -1;
  const std::string printed = CaptureStderr(
      [&]() { print_rc = nc_print_data(nc.get(), cmpd_type, &rec, 1); });
  EXPECT_EQ(NC_NOERR, print_rc);
  EXPECT_EQ(out + "\n", printed);

  RichRec bad_rec{};
  bad_rec.id = 9;
  bad_rec.label = label_lit;
  bad_rec.nums = invalid_vlen;
  {
    char* bad_buf = nullptr;
    const int rc = nc_dump_data(nc.get(), cmpd_type, &bad_rec, 1, &bad_buf);
    ScopedCString bad_guard = MakeScopedCString(bad_buf);
    EXPECT_EQ(NC_EINVAL, rc);
    // nc_dump_data extracts any partial bytes buffered before a nested error.
    ASSERT_NE(nullptr, bad_buf);
    EXPECT_STREQ("<id()9;mat(2,2)0 0 0 0;label()\"node0\";nums()", bad_buf);
  }
}

TEST(DinstanceTest, CopyAndReclaimHandleNestedTypesAndFileRoundTrip) {
  // Verify NC3 atomic copy & reclaim path first.
  {
    ScopedNcFile nc3;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("dinstance_nc3.nc", NC_CLOBBER, 0, nc3.ptr()));
    const std::array<double, 3> src{1.25, -2.5, 3.75};
    std::array<double, 3> dst{};
    ASSERT_EQ(NC_NOERR, nc_copy_data(nc3.get(), NC_DOUBLE, src.data(),
                                     src.size(), dst.data()));
    EXPECT_THAT(dst, ::testing::ElementsAre(1.25, -2.5, 3.75));
    EXPECT_EQ(NC_NOERR,
              nc_reclaim_data(nc3.get(), NC_DOUBLE, dst.data(), dst.size()));
  }

  // Create a NetCDF-4 dataset with a variable of nested compound{string, vlen}.
  struct SampleRec {
    int32_t id;
    char* name;
    nc_vlen_t vals;
  };

  ScopedMemio memio;
  {
    ScopedNcFile nc_out;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("dinstance_rt.nc", NC_NETCDF4, 0, nc_out.ptr()));
    int dim_id = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc_out.get(), "n", 2, &dim_id));

    nc_type vlen_id = NC_NAT;
    nc_type rec_type = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_vlen(nc_out.get(), "int_seq", NC_INT, &vlen_id));
    ASSERT_EQ(NC_NOERR, nc_def_compound(nc_out.get(), sizeof(SampleRec),
                                        "sample_rec", &rec_type));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc_out.get(), rec_type, "id",
                                 NC_COMPOUND_OFFSET(SampleRec, id), NC_INT));
    ASSERT_EQ(NC_NOERR, nc_insert_compound(nc_out.get(), rec_type, "name",
                                           NC_COMPOUND_OFFSET(SampleRec, name),
                                           NC_STRING));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(nc_out.get(), rec_type, "vals",
                                 NC_COMPOUND_OFFSET(SampleRec, vals), vlen_id));

    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(nc_out.get(), "samples", rec_type, 1,
                                   &dim_id, &varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc_out.get()));

    char s0[] = "first";
    char s1[] = "second";
    std::array<int32_t, 3> v0{10, 20, 30};
    std::array<SampleRec, 2> records{};
    records[0].id = 1;
    records[0].name = s0;
    records[0].vals = nc_vlen_t{v0.size(), v0.data()};
    records[1].id = 2;
    records[1].name = s1;
    records[1].vals = nc_vlen_t{0, nullptr};
    ASSERT_EQ(NC_NOERR, nc_put_var(nc_out.get(), varid, records.data()));

    // Exercise nc_copy_data / ncaux_reclaim_data before closing.
    {
      std::array<SampleRec, 2> aux_copy{};
      absl::Cleanup reclaim_aux = [&]() {
        EXPECT_EQ(NC_NOERR,
                  ncaux_reclaim_data(nc_out.get(), rec_type, aux_copy.data(),
                                     aux_copy.size()));
      };
      ASSERT_EQ(NC_NOERR, nc_copy_data(nc_out.get(), rec_type, records.data(),
                                       records.size(), aux_copy.data()));
      EXPECT_NE(s0, aux_copy[0].name);
      EXPECT_STREQ("first", aux_copy[0].name);
      EXPECT_EQ(3U, aux_copy[0].vals.len);
      EXPECT_NE(v0.data(), aux_copy[0].vals.p);
      EXPECT_EQ(0U, aux_copy[1].vals.len);
      EXPECT_EQ(nullptr, aux_copy[1].vals.p);
    }

    ASSERT_EQ(NC_NOERR, nc_close_memio(nc_out.get(), memio.get()));
    nc_out.release();
  }

  // Reopen the in-memory dataset read-only, read back, deep-copy, dump,
  // reclaim.
  ScopedNcFile nc_in;
  ASSERT_EQ(NC_NOERR, nc_open_mem("dinstance_rt.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, nc_in.ptr()));

  nc_type rec_type = NC_NAT;
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(nc_in.get(), "sample_rec", &rec_type));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(nc_in.get(), "samples", &varid));

  std::vector<SampleRec> read_recs(2);
  absl::Cleanup reclaim_read = [&]() {
    if (nc_in.valid()) {
      EXPECT_EQ(NC_NOERR, nc_reclaim_data(nc_in.get(), rec_type,
                                          read_recs.data(), read_recs.size()));
    }
  };
  ASSERT_EQ(NC_NOERR, nc_get_var(nc_in.get(), varid, read_recs.data()));
  EXPECT_EQ(1, read_recs[0].id);
  EXPECT_STREQ("first", read_recs[0].name);
  ASSERT_EQ(3U, read_recs[0].vals.len);
  const int32_t* read_v0 = static_cast<const int32_t*>(read_recs[0].vals.p);
  EXPECT_EQ(10, read_v0[0]);
  EXPECT_EQ(20, read_v0[1]);
  EXPECT_EQ(30, read_v0[2]);
  EXPECT_EQ(2, read_recs[1].id);
  EXPECT_STREQ("second", read_recs[1].name);
  EXPECT_EQ(0U, read_recs[1].vals.len);

  // Deep copy via nc_copy_data_all -> verify -> nc_reclaim_data_all.
  void* all_copy_raw = nullptr;
  absl::Cleanup reclaim_all_copy = [&]() {
    if (all_copy_raw != nullptr && nc_in.valid()) {
      void* p = all_copy_raw;
      all_copy_raw = nullptr;
      EXPECT_EQ(NC_NOERR, nc_reclaim_data_all(nc_in.get(), rec_type, p, 2));
    }
  };
  ASSERT_EQ(NC_NOERR, nc_copy_data_all(nc_in.get(), rec_type, read_recs.data(),
                                       2, &all_copy_raw));
  ASSERT_NE(nullptr, all_copy_raw);
  const auto* copied = static_cast<const SampleRec*>(all_copy_raw);
  EXPECT_NE(read_recs[0].name, copied[0].name);
  EXPECT_STREQ("first", copied[0].name);
  EXPECT_NE(read_recs[0].vals.p, copied[0].vals.p);

  char* dump_raw = nullptr;
  const int dump_rc = nc_dump_data(nc_in.get(), rec_type, copied, 2, &dump_raw);
  ScopedCString dump_guard = MakeScopedCString(dump_raw);
  ASSERT_EQ(NC_NOERR, dump_rc);
  ASSERT_NE(nullptr, dump_raw);
  EXPECT_THAT(dump_raw,
              ::testing::HasSubstr(
                  "<id()1;name()\"first\";vals(){len=3,p=(10 20 30)}>"));
  EXPECT_THAT(dump_raw, ::testing::HasSubstr(
                            "<id()2;name()\"second\";vals(){len=0,p=()}>"));

  // Exercise nc_copy_data_all + ncaux_reclaim_data_all with NC_STRING array.
  {
    char str_lit0[] = "str0";
    char* raw_strs[2] = {str_lit0, nullptr};
    void* aux_strs_copy = nullptr;
    absl::Cleanup reclaim_aux_strs = [&]() {
      if (aux_strs_copy != nullptr && nc_in.valid()) {
        void* p = aux_strs_copy;
        aux_strs_copy = nullptr;
        EXPECT_EQ(NC_NOERR,
                  ncaux_reclaim_data_all(nc_in.get(), NC_STRING, p, 2));
      }
    };
    ASSERT_EQ(NC_NOERR, nc_copy_data_all(nc_in.get(), NC_STRING, raw_strs, 2,
                                         &aux_strs_copy));
    ASSERT_NE(nullptr, aux_strs_copy);
    char** strs_out = static_cast<char**>(aux_strs_copy);
    EXPECT_STREQ("str0", strs_out[0]);
    EXPECT_EQ(nullptr, strs_out[1]);
  }
}

}  // namespace
}  // namespace netcdf
