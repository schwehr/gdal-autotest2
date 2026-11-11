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

// Unit tests for //third_party/netcdf/libsrc4/nc4var.c (NetCDF-4 variable
// inquiries, chunk-cache/chunking helpers, type conversion, quantization, and
// fill-value / default chunksize calculations).
//
// Covers both public API entrypoints and direct internal entrypoints exposed
// via nc4internal.h / nc4dispatch.h / netcdf_f.h. All tests run hermetically in
// memory via nc_create_mem / nc_open_mem or using TempFile under
// ::testing::TempDir().

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <type_traits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
// clang-format off
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"
// clang-format on

extern "C" {
// clang-format off
#include "third_party/netcdf/include/nc4dispatch.h"
#include "third_party/netcdf/include/nc4internal.h"
#include "third_party/netcdf/include/netcdf_f.h"
int nc_var_par_access(int ncid, int varid, int par_access);
// clang-format on
}  // extern "C"

namespace netcdf {
namespace {

constexpr size_t kMemInitialBytes = 4096;

template <typename SrcT, typename DstT>
void ExpectConvertInRange(nc_type src_type, nc_type dst_type,
                          std::initializer_list<SrcT> inputs,
                          std::initializer_list<DstT> expected,
                          int strict_nc3 = 0) {
  std::vector<SrcT> src(inputs);
  std::vector<DstT> dst(src.size(), DstT{});
  int range_error = -1;
  ASSERT_EQ(NC_NOERR, nc4_convert_type(src.data(), dst.data(), src_type,
                                       dst_type, src.size(), &range_error,
                                       nullptr, strict_nc3, NC_NOQUANTIZE, 0));
  EXPECT_EQ(range_error, 0);
  ASSERT_EQ(dst.size(), expected.size());
  size_t i = 0;
  for (DstT exp : expected) {
    if constexpr (std::is_same_v<DstT, float>) {
      EXPECT_FLOAT_EQ(dst[i], exp) << "index=" << i;
    } else if constexpr (std::is_same_v<DstT, double>) {
      EXPECT_DOUBLE_EQ(dst[i], exp) << "index=" << i;
    } else {
      EXPECT_EQ(dst[i], exp) << "index=" << i;
    }
    ++i;
  }
}

template <typename SrcT, typename DstT>
void ExpectConvertOutOfRange(nc_type src_type, nc_type dst_type,
                             std::initializer_list<SrcT> inputs,
                             int expected_range_errors, int strict_nc3 = 0) {
  std::vector<SrcT> src(inputs);
  std::vector<DstT> dst(src.size(), DstT{});
  int range_error = -1;
  ASSERT_EQ(NC_NOERR, nc4_convert_type(src.data(), dst.data(), src_type,
                                       dst_type, src.size(), &range_error,
                                       nullptr, strict_nc3, NC_NOQUANTIZE, 0));
  EXPECT_EQ(range_error, expected_range_errors);
}

// Exercises all numeric destination switches and invalid dest_type branch for a
// given source type.
template <typename SrcT>
void ExpectInvalidDestTypesReturnBadType(nc_type src_type, SrcT sample_val) {
  SrcT src = sample_val;
  long long dummy = 0;
  int range_error = -1;
  EXPECT_EQ(NC_EBADTYPE,
            nc4_convert_type(&src, &dummy, src_type, NC_NAT, 1, &range_error,
                             nullptr, 0, NC_NOQUANTIZE, 0));
  EXPECT_EQ(range_error, 0);
  EXPECT_EQ(NC_EBADTYPE,
            nc4_convert_type(&src, &dummy, src_type, NC_STRING, 1, &range_error,
                             nullptr, 0, NC_NOQUANTIZE, 0));
}

template <typename FloatT>
uint64_t BitPattern64(FloatT value) {
  uint64_t bits = 0;
  static_assert(sizeof(FloatT) <= sizeof(bits));
  std::memcpy(&bits, &value, sizeof(FloatT));
  return bits;
}

// ============================================================================
// Chunk cache getters: NC4_get_var_chunk_cache & nc_get_var_chunk_cache_ints
// ============================================================================

TEST(Nc4VarChunkCacheTest, GetVarChunkCacheAndIntsWithOptionalOutputs) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR, nc_create_mem("var_chunk_cache.nc", NC_NETCDF4,
                                    kMemInitialBytes, file.ptr()));
  int dimid = -1;
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "x", 8, &dimid));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v", NC_INT, 1, &dimid, &varid));
  constexpr size_t kChunks[1] = {4};
  ASSERT_EQ(NC_NOERR, nc_def_var_chunking(file, varid, NC_CHUNKED, kChunks));
  constexpr size_t kCacheSize = 4 * static_cast<size_t>(MEGABYTE);
  constexpr size_t kCacheNelems = 521;
  constexpr float kPreemption = 0.5F;
  ASSERT_EQ(NC_NOERR, nc_set_var_chunk_cache(file, varid, kCacheSize,
                                             kCacheNelems, kPreemption));

  // Public API + direct dispatch entrypoint with all pointers non-null.
  size_t got_size = 0;
  size_t got_nelems = 0;
  float got_preemption = -1.0F;
  ASSERT_EQ(NC_NOERR, nc_get_var_chunk_cache(file, varid, &got_size,
                                             &got_nelems, &got_preemption));
  EXPECT_EQ(got_size, kCacheSize);
  EXPECT_EQ(got_nelems, kCacheNelems);
  EXPECT_FLOAT_EQ(got_preemption, kPreemption);

  // Direct call with every combination of NULL out pointers.
  EXPECT_EQ(NC_NOERR,
            NC4_get_var_chunk_cache(file, varid, nullptr, nullptr, nullptr));
  got_size = 0;
  EXPECT_EQ(NC_NOERR,
            NC4_get_var_chunk_cache(file, varid, &got_size, nullptr, nullptr));
  EXPECT_EQ(got_size, kCacheSize);
  got_nelems = 0;
  EXPECT_EQ(NC_NOERR, NC4_get_var_chunk_cache(file, varid, nullptr, &got_nelems,
                                              nullptr));
  EXPECT_EQ(got_nelems, kCacheNelems);
  got_preemption = -1.0F;
  EXPECT_EQ(NC_NOERR, NC4_get_var_chunk_cache(file, varid, nullptr, nullptr,
                                              &got_preemption));
  EXPECT_FLOAT_EQ(got_preemption, kPreemption);

  // Integer Fortran wrapper.
  int size_mb = -1;
  int nelems_i = -1;
  int preempt_pct = -1;
  ASSERT_EQ(NC_NOERR, nc_get_var_chunk_cache_ints(file, varid, &size_mb,
                                                  &nelems_i, &preempt_pct));
  EXPECT_EQ(size_mb, 4);
  EXPECT_EQ(nelems_i, 521);
  EXPECT_EQ(preempt_pct, 50);

  EXPECT_EQ(NC_NOERR, nc_get_var_chunk_cache_ints(file, varid, nullptr, nullptr,
                                                  nullptr));
}

TEST(Nc4VarChunkCacheTest, GetVarChunkCacheErrorPaths) {
  ScopedNcFile nc4_file;
  ASSERT_EQ(NC_NOERR, nc_create_mem("var_cache_err.nc", NC_NETCDF4,
                                    kMemInitialBytes, nc4_file.ptr()));
  int dimid = -1;
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc4_file, "x", 4, &dimid));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc4_file, "v", NC_INT, 1, &dimid, &varid));

  size_t size_out = 0;
  size_t nelems_out = 0;
  float preempt_out = 0.0F;
  int mb_out = 0;
  int nelems_i_out = 0;
  int preempt_i_out = 0;

  // Invalid file ID and invalid group sub-ID.
  EXPECT_EQ(NC_EBADID, NC4_get_var_chunk_cache(-1, 0, &size_out, &nelems_out,
                                               &preempt_out));
  const int bad_grpid = (nc4_file.get() & ~GRP_ID_MASK) | 0x0fff;
  EXPECT_EQ(NC_EBADID, NC4_get_var_chunk_cache(bad_grpid, 0, &size_out,
                                               &nelems_out, &preempt_out));
  EXPECT_EQ(NC_EBADID, nc_get_var_chunk_cache_ints(
                           -1, 0, &mb_out, &nelems_i_out, &preempt_i_out));

  // Invalid varid.
  EXPECT_EQ(NC_ENOTVAR, NC4_get_var_chunk_cache(nc4_file, -1, &size_out,
                                                &nelems_out, &preempt_out));
  EXPECT_EQ(NC_ENOTVAR, NC4_get_var_chunk_cache(nc4_file, 99, &size_out,
                                                &nelems_out, &preempt_out));
  EXPECT_EQ(NC_ENOTVAR,
            nc_get_var_chunk_cache_ints(nc4_file, 99, &mb_out, &nelems_i_out,
                                        &preempt_i_out));

  // Public dispatch rejects classic files with NC_ENOTNC4.
  ScopedNcFile classic_file;
  ASSERT_EQ(NC_NOERR, nc_create_mem("classic_cache.nc", NC_CLOBBER,
                                    kMemInitialBytes, classic_file.ptr()));
  int c_dimid = -1;
  int c_varid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(classic_file, "x", 2, &c_dimid));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(classic_file, "cv", NC_INT, 1, &c_dimid, &c_varid));
  EXPECT_EQ(NC_ENOTNC4, nc_get_var_chunk_cache(classic_file, c_varid, &size_out,
                                               &nelems_out, &preempt_out));
}

// ============================================================================
// Variable inquiries: NC4_inq_var_all & nc_inq_var_chunking_ints
// ============================================================================

TEST(Nc4VarInqTest, InqVarAllCoversMetadataFiltersFillEndiannessAndErrors) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR, nc_create_mem("inq_var_all.nc", NC_NETCDF4,
                                    kMemInitialBytes, file.ptr()));
  int d0 = -1;
  int d1 = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d0", 4, &d0));
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d1", 6, &d1));

  // Global attributes for the NC4_inq_var_all(NC_GLOBAL, ...) fast path.
  const int gval[2] = {10, 20};
  ASSERT_EQ(NC_NOERR, nc_put_att_int(file, NC_GLOBAL, "g1", NC_INT, 2, gval));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(file, NC_GLOBAL, "g2", 3, "abc"));

  // 1. Chunked variable with shuffle+deflate, fletcher32, big-endian, custom
  // fill value, and variable attribute.
  const int dims2[2] = {d0, d1};
  int v_chunked = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(file, "v_chunked", NC_DOUBLE, 2, dims2, &v_chunked));
  constexpr size_t kChunks[2] = {2, 3};
  ASSERT_EQ(NC_NOERR,
            nc_def_var_chunking(file, v_chunked, NC_CHUNKED, kChunks));
  ASSERT_EQ(NC_NOERR, nc_def_var_deflate(file, v_chunked, 1, 1, 3));
  ASSERT_EQ(NC_NOERR, nc_def_var_fletcher32(file, v_chunked, NC_FLETCHER32));
  ASSERT_EQ(NC_NOERR, nc_def_var_endian(file, v_chunked, NC_ENDIAN_BIG));
  const double custom_fill = -999.5;
  ASSERT_EQ(NC_NOERR, nc_def_var_fill(file, v_chunked, NC_FILL, &custom_fill));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(file, v_chunked, "units", 1, "K"));

  // 2. Contiguous variable with default fill value, no filters, little-endian.
  int v_contig = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v_contig", NC_INT, 1, &d0, &v_contig));
  ASSERT_EQ(NC_NOERR, nc_def_var_endian(file, v_contig, NC_ENDIAN_LITTLE));

  // 3. Scalar variable with NC_NOFILL.
  int v_nofill = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(file, "v_nofill", NC_SHORT, 0, nullptr, &v_nofill));
  ASSERT_EQ(NC_NOERR, nc_def_var_fill(file, v_nofill, NC_NOFILL, nullptr));

  ASSERT_EQ(NC_NOERR, nc_enddef(file));

  // Query NC_GLOBAL via NC4_inq_var_all directly (public dispatch handles
  // NC_GLOBAL separately, so direct call exercises NC4_inq_var_all lines).
  int g_natts = -1;
  ASSERT_EQ(NC_NOERR,
            NC4_inq_var_all(file, NC_GLOBAL, nullptr, nullptr, nullptr, nullptr,
                            &g_natts, nullptr, nullptr, nullptr, nullptr,
                            nullptr, nullptr, nullptr, nullptr, nullptr,
                            nullptr, nullptr, nullptr));
  EXPECT_EQ(g_natts, 2);

  // NC_GLOBAL with nattsp == nullptr falls through to invalid varid check.
  EXPECT_EQ(NC_ENOTVAR,
            NC4_inq_var_all(file, NC_GLOBAL, nullptr, nullptr, nullptr, nullptr,
                            nullptr, nullptr, nullptr, nullptr, nullptr,
                            nullptr, nullptr, nullptr, nullptr, nullptr,
                            nullptr, nullptr, nullptr));

  // Query all supported fields of v_chunked simultaneously.
  char name_buf[NC_MAX_NAME + 1]{};
  nc_type xtype = NC_NAT;
  int ndims = -1;
  int dimids[2]{-1, -1};
  int natts = -1;
  int shuffle = -1;
  int fletcher32 = -1;
  int storage = -1;
  size_t chunks[2]{0, 0};
  int no_fill = -1;
  double fill_out = 0.0;
  int endian = -1;
  ASSERT_EQ(
      NC_NOERR,
      NC4_inq_var_all(file, v_chunked, name_buf, &xtype, &ndims, dimids, &natts,
                      &shuffle, nullptr, nullptr, &fletcher32, &storage, chunks,
                      &no_fill, &fill_out, &endian, nullptr, nullptr, nullptr));
  EXPECT_STREQ(name_buf, "v_chunked");
  EXPECT_EQ(xtype, NC_DOUBLE);
  EXPECT_EQ(ndims, 2);
  EXPECT_EQ(dimids[0], d0);
  EXPECT_EQ(dimids[1], d1);
  EXPECT_GE(natts, 2);  // "units" + NC_FillValue
  EXPECT_EQ(shuffle, 1);
  EXPECT_EQ(fletcher32, 1);
  EXPECT_EQ(storage, NC_CHUNKED);
  EXPECT_EQ(chunks[0], 2U);
  EXPECT_EQ(chunks[1], 3U);
  EXPECT_EQ(no_fill, 0);  // Boolean var->no_fill (0 = fill, 1 = no-fill).
  EXPECT_DOUBLE_EQ(fill_out, custom_fill);
  EXPECT_EQ(endian, NC_ENDIAN_BIG);

  // All optional output pointers NULL succeeds.
  EXPECT_EQ(NC_NOERR,
            NC4_inq_var_all(file, v_chunked, nullptr, nullptr, nullptr, nullptr,
                            nullptr, nullptr, nullptr, nullptr, nullptr,
                            nullptr, nullptr, nullptr, nullptr, nullptr,
                            nullptr, nullptr, nullptr));

  // Passing non-null deflatep or idp directly is explicitly rejected.
  int unused_deflate = 0;
  unsigned int unused_id = 0;
  EXPECT_EQ(NC_EFILTER,
            NC4_inq_var_all(file, v_chunked, nullptr, nullptr, nullptr, nullptr,
                            nullptr, nullptr, &unused_deflate, nullptr, nullptr,
                            nullptr, nullptr, nullptr, nullptr, nullptr,
                            nullptr, nullptr, nullptr));
  EXPECT_EQ(NC_EFILTER,
            NC4_inq_var_all(file, v_chunked, nullptr, nullptr, nullptr, nullptr,
                            nullptr, nullptr, nullptr, nullptr, nullptr,
                            nullptr, nullptr, nullptr, nullptr, nullptr,
                            &unused_id, nullptr, nullptr));

  // Query contiguous var with default fill value and no filters.
  int c_shuffle = -1;
  int c_fletch = -1;
  int c_storage = -1;
  size_t untouched_chunk = 777U;
  int c_no_fill = -1;
  int c_fill = 0;
  int c_endian = -1;
  ASSERT_EQ(NC_NOERR,
            NC4_inq_var_all(file, v_contig, nullptr, nullptr, nullptr, nullptr,
                            nullptr, &c_shuffle, nullptr, nullptr, &c_fletch,
                            &c_storage, &untouched_chunk, &c_no_fill, &c_fill,
                            &c_endian, nullptr, nullptr, nullptr));
  EXPECT_EQ(c_shuffle, 0);
  EXPECT_EQ(c_fletch, 0);
  EXPECT_EQ(c_storage, NC_CONTIGUOUS);
  EXPECT_EQ(untouched_chunk, 777U);
  EXPECT_EQ(c_no_fill, 0);
  EXPECT_EQ(c_fill, NC_FILL_INT);
  EXPECT_EQ(c_endian, NC_ENDIAN_LITTLE);

  // Query scalar no-fill var: fill_valuep must remain untouched.
  short s_fill_sentinel = 12345;
  int s_no_fill = -1;
  ASSERT_EQ(NC_NOERR,
            NC4_inq_var_all(file, v_nofill, nullptr, nullptr, nullptr, nullptr,
                            nullptr, nullptr, nullptr, nullptr, nullptr,
                            nullptr, nullptr, &s_no_fill, &s_fill_sentinel,
                            nullptr, nullptr, nullptr, nullptr));
  // NC4_inq_var_all returns boolean var->no_fill (1), not NC_NOFILL (0x100).
  EXPECT_EQ(s_no_fill, 1);
  EXPECT_EQ(s_fill_sentinel, 12345);

  // Also check public inquiry wrappers hit the expected metadata.
  int pub_shuffle = -1;
  int pub_deflate = -1;
  int pub_level = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_var_deflate(file, v_chunked, &pub_shuffle,
                                         &pub_deflate, &pub_level));
  EXPECT_EQ(pub_shuffle, 1);
  EXPECT_EQ(pub_deflate, 1);
  EXPECT_EQ(pub_level, 3);

  // Error paths: bad ncid and bad varid.
  EXPECT_EQ(
      NC_EBADID,
      NC4_inq_var_all(-1, 0, nullptr, nullptr, nullptr, nullptr, nullptr,
                      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
  EXPECT_EQ(
      NC_ENOTVAR,
      NC4_inq_var_all(file, 999, nullptr, nullptr, nullptr, nullptr, nullptr,
                      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
}

TEST(Nc4VarInqTest, InqVarAllStringAndVlenFillValuesReclaimCleanly) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR, nc_create_mem("inq_var_str_vlen_fill.nc", NC_NETCDF4,
                                    kMemInitialBytes, file.ptr()));
  int dimid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "x", 2, &dimid));

  nc_type vlen_type = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_vlen(file, "int_vlen_t", NC_INT, &vlen_type));

  int v_str_custom = -1;
  int v_str_default = -1;
  int v_vlen_custom = -1;
  int v_vlen_default = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v_str_custom", NC_STRING, 1, &dimid,
                                 &v_str_custom));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v_str_default", NC_STRING, 1, &dimid,
                                 &v_str_default));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v_vlen_custom", vlen_type, 1, &dimid,
                                 &v_vlen_custom));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v_vlen_default", vlen_type, 1, &dimid,
                                 &v_vlen_default));

  const char* custom_str = "missing";
  ASSERT_EQ(NC_NOERR,
            nc_def_var_fill(file, v_str_custom, NC_FILL, &custom_str));

  std::array<int, 2> vlen_elems = {11, 22};
  const nc_vlen_t custom_vlen = {vlen_elems.size(), vlen_elems.data()};
  ASSERT_EQ(NC_NOERR,
            nc_def_var_fill(file, v_vlen_custom, NC_FILL, &custom_vlen));

  ASSERT_EQ(NC_NOERR, nc_enddef(file));

  int no_fill = -1;
  char* got_str = nullptr;
  absl::Cleanup free_got_str = [&got_str] {
    if (got_str != nullptr) {
      nc_free_string(1, &got_str);
    }
  };
  ASSERT_EQ(NC_NOERR, nc_inq_var_fill(file, v_str_custom, &no_fill, &got_str));
  EXPECT_EQ(no_fill, NC_FILL);
  ASSERT_NE(got_str, nullptr);
  EXPECT_STREQ(got_str, "missing");
  nc_free_string(1, &got_str);
  got_str = nullptr;

  ASSERT_EQ(NC_NOERR, nc_inq_var_fill(file, v_str_default, &no_fill, &got_str));
  EXPECT_EQ(no_fill, NC_FILL);
  ASSERT_NE(got_str, nullptr);
  EXPECT_STREQ(got_str, "");

  nc_vlen_t got_vlen{};
  absl::Cleanup reclaim_got_vlen = [&] {
    nc_reclaim_data(file, vlen_type, &got_vlen, 1);
  };
  ASSERT_EQ(NC_NOERR,
            nc_inq_var_fill(file, v_vlen_custom, &no_fill, &got_vlen));
  EXPECT_EQ(no_fill, NC_FILL);
  EXPECT_EQ(got_vlen.len, 2U);
  ASSERT_NE(got_vlen.p, nullptr);
  const int* vlen_ints = static_cast<const int*>(got_vlen.p);
  EXPECT_EQ(vlen_ints[0], 11);
  EXPECT_EQ(vlen_ints[1], 22);
  ASSERT_EQ(NC_NOERR, nc_reclaim_data(file, vlen_type, &got_vlen, 1));
  got_vlen = nc_vlen_t{};

  ASSERT_EQ(NC_NOERR,
            nc_inq_var_fill(file, v_vlen_default, &no_fill, &got_vlen));
  EXPECT_EQ(no_fill, NC_FILL);
  EXPECT_EQ(got_vlen.len, 0U);
  EXPECT_EQ(got_vlen.p, nullptr);
}

TEST(Nc4VarInqTest,
     InqVarChunkingIntsHandlesScalarChunkedContigAndRangeOverflow) {
  TempFile temp("c_ints.nc");
  ASSERT_LT(temp.string().size(), static_cast<size_t>(NC_MAX_NAME));
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR, nc_create(temp.c_str(), NC_NETCDF4, file.ptr()));
  absl::Cleanup abort_huge = [&file] {
    if (file.valid()) {
      static_cast<void>(file.abort());
    }
  };
  int d0 = -1;
  int d1 = -1;
  int d_huge = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d0", 10, &d0));
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d1", 12, &d1));
  // Fits in unsigned 32-bit NC_MAX_UINT bytes for 1-byte NC_BYTE, > NC_MAX_INT.
  constexpr size_t kHugeChunk = static_cast<size_t>(NC_MAX_INT) + size_t{2};
  ASSERT_EQ(NC_NOERR,
            nc_def_dim(file, "huge", kHugeChunk + size_t{4}, &d_huge));

  const int dims2[2] = {d0, d1};
  int v_chunked = -1;
  int v_contig = -1;
  int v_scalar = -1;
  int v_huge_chunk = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(file, "v_chunked", NC_INT, 2, dims2, &v_chunked));
  constexpr size_t kChunks[2] = {3, 4};
  ASSERT_EQ(NC_NOERR,
            nc_def_var_chunking(file, v_chunked, NC_CHUNKED, kChunks));

  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v_contig", NC_INT, 1, &d0, &v_contig));
  ASSERT_EQ(NC_NOERR,
            nc_def_var_chunking(file, v_contig, NC_CONTIGUOUS, nullptr));

  ASSERT_EQ(NC_NOERR,
            nc_def_var(file, "v_scalar", NC_INT, 0, nullptr, &v_scalar));

  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v_huge_chunk", NC_BYTE, 1, &d_huge,
                                 &v_huge_chunk));
  ASSERT_EQ(NC_NOERR,
            nc_def_var_chunking(file, v_huge_chunk, NC_CHUNKED, &kHugeChunk));

  // Query chunked var with and without chunksizesp.
  int storage = -1;
  int chunks_out[2]{-1, -1};
  ASSERT_EQ(NC_NOERR,
            nc_inq_var_chunking_ints(file, v_chunked, &storage, chunks_out));
  EXPECT_EQ(storage, NC_CHUNKED);
  EXPECT_EQ(chunks_out[0], 3);
  EXPECT_EQ(chunks_out[1], 4);

  storage = -1;
  ASSERT_EQ(NC_NOERR,
            nc_inq_var_chunking_ints(file, v_chunked, &storage, nullptr));
  EXPECT_EQ(storage, NC_CHUNKED);

  // Query contiguous var with non-null chunksizesp (remains untouched).
  int untouched = 999;
  storage = -1;
  ASSERT_EQ(NC_NOERR,
            nc_inq_var_chunking_ints(file, v_contig, &storage, &untouched));
  EXPECT_EQ(storage, NC_CONTIGUOUS);
  EXPECT_EQ(untouched, 999);

  // Scalar var skips malloc/free of cs.
  storage = -1;
  ASSERT_EQ(NC_NOERR,
            nc_inq_var_chunking_ints(file, v_scalar, &storage, nullptr));
  EXPECT_EQ(storage, NC_CONTIGUOUS);

  // Chunksize > NC_MAX_INT triggers NC_ERANGE.
  int huge_out = 0;
  EXPECT_EQ(NC_ERANGE,
            nc_inq_var_chunking_ints(file, v_huge_chunk, &storage, &huge_out));
  EXPECT_EQ(storage, NC_CHUNKED);

  // Error propagation from nc4_find_grp_h5_var.
  EXPECT_EQ(NC_EBADID, nc_inq_var_chunking_ints(-1, 0, &storage, chunks_out));
  EXPECT_EQ(NC_ENOTVAR,
            nc_inq_var_chunking_ints(file, 99, &storage, chunks_out));

  // Stay in define mode and abort so HDF5 never allocates the huge dataset.
  ASSERT_EQ(NC_NOERR, file.abort());
}

// ============================================================================
// Name lookup and parallel stub: NC4_inq_varid & NC4_var_par_access
// ============================================================================

TEST(Nc4VarIdAndParAccessTest, InqVarIdChecksArgsUtf8AndMissingVars) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR, nc_create_mem("inq_varid.nc", NC_NETCDF4,
                                    kMemInitialBytes, file.ptr()));
  int dimid = -1;
  int var_ascii = -1;
  int var_utf8 = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "x", 2, &dimid));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(file, "temp", NC_FLOAT, 1, &dimid, &var_ascii));
  // NFC UTF-8 name ("caf\xc3\xa9") -> normalized lookup with NFD succeeds.
  ASSERT_EQ(NC_NOERR,
            nc_def_var(file, "caf\xc3\xa9", NC_FLOAT, 1, &dimid, &var_utf8));

  int found = -1;
  EXPECT_EQ(NC_EINVAL, NC4_inq_varid(file, nullptr, &found));
  EXPECT_EQ(NC_NOERR, NC4_inq_varid(file, "anything", nullptr));
  EXPECT_EQ(NC_EBADID, NC4_inq_varid(-1, "temp", &found));

  // Invalid UTF-8 byte sequence triggers nc4_normalize_name error.
  EXPECT_EQ(NC_EBADNAME, NC4_inq_varid(file, "\xff\xfe", &found));

  // Missing valid name.
  EXPECT_EQ(NC_ENOTVAR, NC4_inq_varid(file, "missing", &found));
  EXPECT_EQ(NC_ENOTVAR, nc_inq_varid(file, "missing", &found));

  // Existing ASCII and NFD-spelled UTF-8 variable names.
  ASSERT_EQ(NC_NOERR, nc_inq_varid(file, "temp", &found));
  EXPECT_EQ(found, var_ascii);
  found = -1;
  ASSERT_EQ(NC_NOERR, NC4_inq_varid(file, "caf\x65\xcc\x81", &found));
  EXPECT_EQ(found, var_utf8);
}

TEST(Nc4VarIdAndParAccessTest, VarParAccessReturnsNoParWithoutParallel) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR, nc_create_mem("par_access.nc", NC_NETCDF4,
                                    kMemInitialBytes, file.ptr()));
  int dimid = -1;
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "x", 2, &dimid));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v", NC_INT, 1, &dimid, &varid));

  constexpr int kIndependent = 0;
  constexpr int kCollective = 1;
  EXPECT_EQ(NC_ENOPAR, nc_var_par_access(file, varid, kCollective));
  EXPECT_EQ(NC_ENOPAR, NC4_var_par_access(file, varid, kIndependent));
  EXPECT_EQ(NC_ENOPAR, NC4_var_par_access(-1, varid, kCollective));
}

// ============================================================================
// Type conversion matrix: nc4_convert_type
// ============================================================================

TEST(Nc4VarConvertTypeTest, CharAndInvalidTypeConversions) {
  // Zero-length copy succeeds cleanly.
  int range_error = -1;
  EXPECT_EQ(NC_NOERR,
            nc4_convert_type(nullptr, nullptr, NC_INT, NC_DOUBLE, 0,
                             &range_error, nullptr, 0, NC_NOQUANTIZE, 0));
  EXPECT_EQ(range_error, 0);

  // NC_CHAR -> NC_CHAR copies characters verbatim.
  const std::array<char, 4> src_chars = {'N', 'e', 't', '4'};
  std::array<char, 4> dst_chars{};
  ASSERT_EQ(NC_NOERR,
            nc4_convert_type(src_chars.data(), dst_chars.data(), NC_CHAR,
                             NC_CHAR, src_chars.size(), &range_error, nullptr,
                             0, NC_NOQUANTIZE, 0));
  EXPECT_EQ(range_error, 0);
  EXPECT_EQ(dst_chars, src_chars);

  // NC_CHAR -> non-NC_CHAR hits the inner NC_CHAR default branch cleanly.
  int dummy_int = 7;
  EXPECT_EQ(NC_NOERR,
            nc4_convert_type(src_chars.data(), &dummy_int, NC_CHAR, NC_INT, 1,
                             &range_error, nullptr, 0, NC_NOQUANTIZE, 0));

  // Invalid src_type returns NC_EBADTYPE.
  EXPECT_EQ(NC_EBADTYPE,
            nc4_convert_type(&dummy_int, &dummy_int, NC_NAT, NC_INT, 1,
                             &range_error, nullptr, 0, NC_NOQUANTIZE, 0));
  EXPECT_EQ(NC_EBADTYPE,
            nc4_convert_type(&dummy_int, &dummy_int, NC_STRING, NC_INT, 1,
                             &range_error, nullptr, 0, NC_NOQUANTIZE, 0));

  // Invalid dest_type for all 10 numeric src_types.
  ExpectInvalidDestTypesReturnBadType<signed char>(NC_BYTE, -1);
  ExpectInvalidDestTypesReturnBadType<unsigned char>(NC_UBYTE, 1);
  ExpectInvalidDestTypesReturnBadType<short>(NC_SHORT, 1);
  ExpectInvalidDestTypesReturnBadType<unsigned short>(NC_USHORT, 1);
  ExpectInvalidDestTypesReturnBadType<int>(NC_INT, 1);
  ExpectInvalidDestTypesReturnBadType<unsigned int>(NC_UINT, 1);
  ExpectInvalidDestTypesReturnBadType<long long>(NC_INT64, 1);
  ExpectInvalidDestTypesReturnBadType<unsigned long long>(NC_UINT64, 1);
  ExpectInvalidDestTypesReturnBadType<float>(NC_FLOAT, 1.0F);
  ExpectInvalidDestTypesReturnBadType<double>(NC_DOUBLE, 1.0);
}

TEST(Nc4VarConvertTypeTest, ByteAndUbyteSourceMatrix) {
  // NC_BYTE source.
  ExpectConvertInRange<signed char, signed char>(
      NC_BYTE, NC_BYTE, {-128, 0, 127}, {-128, 0, 127});
  ExpectConvertInRange<signed char, unsigned char>(NC_BYTE, NC_UBYTE, {0, 127},
                                                   {0, 127});
  ExpectConvertOutOfRange<signed char, unsigned char>(NC_BYTE, NC_UBYTE, {-1},
                                                      1);
  ExpectConvertInRange<signed char, short>(NC_BYTE, NC_SHORT, {-128, 127},
                                           {-128, 127});
  ExpectConvertInRange<signed char, unsigned short>(NC_BYTE, NC_USHORT,
                                                    {0, 127}, {0, 127});
  ExpectConvertOutOfRange<signed char, unsigned short>(NC_BYTE, NC_USHORT, {-5},
                                                       1);
  ExpectConvertInRange<signed char, int>(NC_BYTE, NC_INT, {-128, 127},
                                         {-128, 127});
  ExpectConvertInRange<signed char, unsigned int>(NC_BYTE, NC_UINT, {0, 127},
                                                  {0, 127});
  ExpectConvertOutOfRange<signed char, unsigned int>(NC_BYTE, NC_UINT, {-3}, 1);
  ExpectConvertInRange<signed char, long long>(NC_BYTE, NC_INT64, {-128, 127},
                                               {-128, 127});
  ExpectConvertInRange<signed char, unsigned long long>(NC_BYTE, NC_UINT64,
                                                        {0, 127}, {0, 127});
  ExpectConvertOutOfRange<signed char, unsigned long long>(NC_BYTE, NC_UINT64,
                                                           {-1}, 1);
  ExpectConvertInRange<signed char, float>(NC_BYTE, NC_FLOAT, {-12, 42},
                                           {-12.0F, 42.0F});
  ExpectConvertInRange<signed char, double>(NC_BYTE, NC_DOUBLE, {-12, 42},
                                            {-12.0, 42.0});

  // NC_UBYTE source, including strict_nc3 flag handling for NC_BYTE.
  ExpectConvertInRange<unsigned char, signed char>(NC_UBYTE, NC_BYTE, {0, 127},
                                                   {0, 127}, /*strict_nc3=*/0);
  ExpectConvertOutOfRange<unsigned char, signed char>(NC_UBYTE, NC_BYTE, {200},
                                                      1, /*strict_nc3=*/0);
  ExpectConvertOutOfRange<unsigned char, signed char>(NC_UBYTE, NC_BYTE, {200},
                                                      0, /*strict_nc3=*/1);
  ExpectConvertInRange<unsigned char, unsigned char>(NC_UBYTE, NC_UBYTE,
                                                     {0, 255}, {0, 255});
  ExpectConvertInRange<unsigned char, short>(NC_UBYTE, NC_SHORT, {0, 255},
                                             {0, 255});
  ExpectConvertInRange<unsigned char, unsigned short>(NC_UBYTE, NC_USHORT,
                                                      {0, 255}, {0, 255});
  ExpectConvertInRange<unsigned char, int>(NC_UBYTE, NC_INT, {0, 255},
                                           {0, 255});
  ExpectConvertInRange<unsigned char, unsigned int>(NC_UBYTE, NC_UINT, {0, 255},
                                                    {0, 255});
  ExpectConvertInRange<unsigned char, long long>(NC_UBYTE, NC_INT64, {0, 255},
                                                 {0, 255});
  ExpectConvertInRange<unsigned char, unsigned long long>(NC_UBYTE, NC_UINT64,
                                                          {0, 255}, {0, 255});
  ExpectConvertInRange<unsigned char, float>(NC_UBYTE, NC_FLOAT, {0, 255},
                                             {0.0F, 255.0F});
  ExpectConvertInRange<unsigned char, double>(NC_UBYTE, NC_DOUBLE, {0, 255},
                                              {0.0, 255.0});
}

TEST(Nc4VarConvertTypeTest, ShortAndUshortSourceMatrix) {
  // NC_SHORT source.
  ExpectConvertInRange<short, unsigned char>(NC_SHORT, NC_UBYTE, {0, 255},
                                             {0, 255});
  ExpectConvertOutOfRange<short, unsigned char>(NC_SHORT, NC_UBYTE, {300, -1},
                                                2);
  ExpectConvertInRange<short, signed char>(NC_SHORT, NC_BYTE, {-128, 127},
                                           {-128, 127});
  ExpectConvertOutOfRange<short, signed char>(NC_SHORT, NC_BYTE, {200, -200},
                                              2);
  ExpectConvertInRange<short, short>(NC_SHORT, NC_SHORT, {-32768, 32767},
                                     {-32768, 32767});
  ExpectConvertInRange<short, unsigned short>(NC_SHORT, NC_USHORT, {0, 32767},
                                              {0, 32767});
  ExpectConvertOutOfRange<short, unsigned short>(NC_SHORT, NC_USHORT, {-1}, 1);
  ExpectConvertInRange<short, int>(NC_SHORT, NC_INT, {-32768, 32767},
                                   {-32768, 32767});
  ExpectConvertInRange<short, unsigned int>(NC_SHORT, NC_UINT, {0, 32767},
                                            {0U, 32767U});
  ExpectConvertOutOfRange<short, unsigned int>(NC_SHORT, NC_UINT, {-1}, 1);
  ExpectConvertInRange<short, long long>(NC_SHORT, NC_INT64, {-32768, 32767},
                                         {-32768, 32767});
  ExpectConvertInRange<short, unsigned long long>(NC_SHORT, NC_UINT64,
                                                  {0, 32767}, {0ULL, 32767ULL});
  ExpectConvertOutOfRange<short, unsigned long long>(NC_SHORT, NC_UINT64, {-1},
                                                     1);
  ExpectConvertInRange<short, float>(NC_SHORT, NC_FLOAT, {-100, 200},
                                     {-100.0F, 200.0F});
  ExpectConvertInRange<short, double>(NC_SHORT, NC_DOUBLE, {-100, 200},
                                      {-100.0, 200.0});

  // NC_USHORT source.
  ExpectConvertInRange<unsigned short, unsigned char>(NC_USHORT, NC_UBYTE,
                                                      {0, 255}, {0, 255});
  ExpectConvertOutOfRange<unsigned short, unsigned char>(NC_USHORT, NC_UBYTE,
                                                         {300}, 1);
  ExpectConvertInRange<unsigned short, signed char>(NC_USHORT, NC_BYTE,
                                                    {0, 127}, {0, 127});
  ExpectConvertOutOfRange<unsigned short, signed char>(NC_USHORT, NC_BYTE,
                                                       {200}, 1);
  ExpectConvertInRange<unsigned short, short>(NC_USHORT, NC_SHORT, {0, 32767},
                                              {0, 32767});
  ExpectConvertOutOfRange<unsigned short, short>(NC_USHORT, NC_SHORT, {40000},
                                                 1);
  ExpectConvertInRange<unsigned short, unsigned short>(NC_USHORT, NC_USHORT,
                                                       {0, 65535}, {0, 65535});
  ExpectConvertInRange<unsigned short, int>(NC_USHORT, NC_INT, {0, 65535},
                                            {0, 65535});
  ExpectConvertInRange<unsigned short, unsigned int>(NC_USHORT, NC_UINT,
                                                     {0, 65535}, {0U, 65535U});
  ExpectConvertInRange<unsigned short, long long>(NC_USHORT, NC_INT64,
                                                  {0, 65535}, {0LL, 65535LL});
  ExpectConvertInRange<unsigned short, unsigned long long>(
      NC_USHORT, NC_UINT64, {0, 65535}, {0ULL, 65535ULL});
  ExpectConvertInRange<unsigned short, float>(NC_USHORT, NC_FLOAT, {0, 65535},
                                              {0.0F, 65535.0F});
  ExpectConvertInRange<unsigned short, double>(NC_USHORT, NC_DOUBLE, {0, 65535},
                                               {0.0, 65535.0});
}

TEST(Nc4VarConvertTypeTest, IntAndUintSourceMatrix) {
  // NC_INT source.
  ExpectConvertInRange<int, unsigned char>(NC_INT, NC_UBYTE, {0, 255},
                                           {0, 255});
  ExpectConvertOutOfRange<int, unsigned char>(NC_INT, NC_UBYTE, {300, -1}, 2);
  ExpectConvertInRange<int, signed char>(NC_INT, NC_BYTE, {-128, 127},
                                         {-128, 127});
  ExpectConvertOutOfRange<int, signed char>(NC_INT, NC_BYTE, {200, -200}, 2);
  ExpectConvertInRange<int, short>(NC_INT, NC_SHORT, {-32768, 32767},
                                   {-32768, 32767});
  ExpectConvertOutOfRange<int, short>(NC_INT, NC_SHORT, {40000, -40000}, 2);
  ExpectConvertInRange<int, unsigned short>(NC_INT, NC_USHORT, {0, 65535},
                                            {0, 65535});
  ExpectConvertOutOfRange<int, unsigned short>(NC_INT, NC_USHORT, {70000, -1},
                                               2);
  ExpectConvertInRange<int, int>(NC_INT, NC_INT, {X_INT_MIN, 0, X_INT_MAX},
                                 {X_INT_MIN, 0, X_INT_MAX});
  ExpectConvertInRange<int, unsigned int>(
      NC_INT, NC_UINT, {0, X_INT_MAX},
      {0U, static_cast<unsigned int>(X_INT_MAX)});
  ExpectConvertOutOfRange<int, unsigned int>(NC_INT, NC_UINT, {-1}, 1);
  ExpectConvertInRange<int, long long>(NC_INT, NC_INT64, {-100, 200},
                                       {-100LL, 200LL});
  ExpectConvertInRange<int, unsigned long long>(NC_INT, NC_UINT64, {0, 200},
                                                {0ULL, 200ULL});
  ExpectConvertOutOfRange<int, unsigned long long>(NC_INT, NC_UINT64, {-5}, 1);
  ExpectConvertInRange<int, float>(NC_INT, NC_FLOAT, {-100, 200},
                                   {-100.0F, 200.0F});
  ExpectConvertInRange<int, double>(NC_INT, NC_DOUBLE, {-100, 200},
                                    {-100.0, 200.0});

  // NC_UINT source.
  ExpectConvertInRange<unsigned int, unsigned char>(NC_UINT, NC_UBYTE, {0, 255},
                                                    {0, 255});
  ExpectConvertOutOfRange<unsigned int, unsigned char>(NC_UINT, NC_UBYTE, {300},
                                                       1);
  ExpectConvertInRange<unsigned int, signed char>(NC_UINT, NC_BYTE, {0, 127},
                                                  {0, 127});
  ExpectConvertOutOfRange<unsigned int, signed char>(NC_UINT, NC_BYTE, {200},
                                                     1);
  ExpectConvertInRange<unsigned int, short>(NC_UINT, NC_SHORT, {0, 32767},
                                            {0, 32767});
  ExpectConvertOutOfRange<unsigned int, short>(NC_UINT, NC_SHORT, {40000}, 1);
  ExpectConvertInRange<unsigned int, unsigned short>(NC_UINT, NC_USHORT,
                                                     {0, 65535}, {0, 65535});
  ExpectConvertOutOfRange<unsigned int, unsigned short>(NC_UINT, NC_USHORT,
                                                        {70000}, 1);
  ExpectConvertInRange<unsigned int, int>(NC_UINT, NC_INT, {0, 2147483647U},
                                          {0, 2147483647});
  ExpectConvertOutOfRange<unsigned int, int>(NC_UINT, NC_INT, {3000000000U}, 1);
  ExpectConvertInRange<unsigned int, unsigned int>(
      NC_UINT, NC_UINT, {0U, X_UINT_MAX}, {0U, X_UINT_MAX});
  ExpectConvertInRange<unsigned int, long long>(
      NC_UINT, NC_INT64, {0U, X_UINT_MAX},
      {0LL, static_cast<long long>(X_UINT_MAX)});
  ExpectConvertInRange<unsigned int, unsigned long long>(
      NC_UINT, NC_UINT64, {0U, X_UINT_MAX},
      {0ULL, static_cast<unsigned long long>(X_UINT_MAX)});
  ExpectConvertInRange<unsigned int, float>(NC_UINT, NC_FLOAT, {0U, 1000U},
                                            {0.0F, 1000.0F});
  ExpectConvertInRange<unsigned int, double>(
      NC_UINT, NC_DOUBLE, {0U, X_UINT_MAX},
      {0.0, static_cast<double>(X_UINT_MAX)});
}

TEST(Nc4VarConvertTypeTest, Int64AndUint64SourceMatrix) {
  // NC_INT64 source.
  ExpectConvertInRange<long long, unsigned char>(NC_INT64, NC_UBYTE, {0, 255},
                                                 {0, 255});
  ExpectConvertOutOfRange<long long, unsigned char>(NC_INT64, NC_UBYTE,
                                                    {300LL, -1LL}, 2);
  ExpectConvertInRange<long long, signed char>(NC_INT64, NC_BYTE, {-128, 127},
                                               {-128, 127});
  ExpectConvertOutOfRange<long long, signed char>(NC_INT64, NC_BYTE,
                                                  {200LL, -200LL}, 2);
  ExpectConvertInRange<long long, short>(NC_INT64, NC_SHORT, {-32768, 32767},
                                         {-32768, 32767});
  ExpectConvertOutOfRange<long long, short>(NC_INT64, NC_SHORT,
                                            {40000LL, -40000LL}, 2);
  ExpectConvertInRange<long long, unsigned short>(NC_INT64, NC_USHORT,
                                                  {0, 65535}, {0, 65535});
  ExpectConvertOutOfRange<long long, unsigned short>(NC_INT64, NC_USHORT,
                                                     {70000LL, -1LL}, 2);
  ExpectConvertInRange<long long, unsigned int>(
      NC_INT64, NC_UINT, {0LL, static_cast<long long>(X_UINT_MAX)},
      {0U, X_UINT_MAX});
  ExpectConvertOutOfRange<long long, unsigned int>(NC_INT64, NC_UINT,
                                                   {5000000000LL, -1LL}, 2);
  ExpectConvertInRange<long long, int>(NC_INT64, NC_INT, {X_INT_MIN, X_INT_MAX},
                                       {X_INT_MIN, X_INT_MAX});
  ExpectConvertOutOfRange<long long, int>(NC_INT64, NC_INT,
                                          {5000000000LL, -5000000000LL}, 2);
  ExpectConvertInRange<long long, long long>(NC_INT64, NC_INT64,
                                             {X_INT64_MIN, X_INT64_MAX},
                                             {X_INT64_MIN, X_INT64_MAX});
  ExpectConvertInRange<long long, unsigned long long>(
      NC_INT64, NC_UINT64, {0LL, X_INT64_MAX},
      {0ULL, static_cast<unsigned long long>(X_INT64_MAX)});
  ExpectConvertOutOfRange<long long, unsigned long long>(NC_INT64, NC_UINT64,
                                                         {-1LL}, 1);
  ExpectConvertInRange<long long, float>(NC_INT64, NC_FLOAT, {-1024LL, 2048LL},
                                         {-1024.0F, 2048.0F});
  ExpectConvertInRange<long long, double>(NC_INT64, NC_DOUBLE,
                                          {-1024LL, 2048LL}, {-1024.0, 2048.0});

  // NC_UINT64 source.
  ExpectConvertInRange<unsigned long long, unsigned char>(NC_UINT64, NC_UBYTE,
                                                          {0, 255}, {0, 255});
  ExpectConvertOutOfRange<unsigned long long, unsigned char>(
      NC_UINT64, NC_UBYTE, {300}, 1);
  ExpectConvertInRange<unsigned long long, signed char>(NC_UINT64, NC_BYTE,
                                                        {0, 127}, {0, 127});
  ExpectConvertOutOfRange<unsigned long long, signed char>(NC_UINT64, NC_BYTE,
                                                           {200}, 1);
  ExpectConvertInRange<unsigned long long, short>(NC_UINT64, NC_SHORT,
                                                  {0, 32767}, {0, 32767});
  ExpectConvertOutOfRange<unsigned long long, short>(NC_UINT64, NC_SHORT,
                                                     {40000}, 1);
  ExpectConvertInRange<unsigned long long, unsigned short>(
      NC_UINT64, NC_USHORT, {0, 65535}, {0, 65535});
  ExpectConvertOutOfRange<unsigned long long, unsigned short>(
      NC_UINT64, NC_USHORT, {70000}, 1);
  ExpectConvertInRange<unsigned long long, unsigned int>(
      NC_UINT64, NC_UINT, {0ULL, X_UINT_MAX}, {0U, X_UINT_MAX});
  ExpectConvertOutOfRange<unsigned long long, unsigned int>(NC_UINT64, NC_UINT,
                                                            {5000000000ULL}, 1);
  ExpectConvertInRange<unsigned long long, int>(
      NC_UINT64, NC_INT, {0ULL, X_INT_MAX}, {0, X_INT_MAX});
  ExpectConvertOutOfRange<unsigned long long, int>(NC_UINT64, NC_INT,
                                                   {3000000000ULL}, 1);
  ExpectConvertInRange<unsigned long long, long long>(
      NC_UINT64, NC_INT64, {0ULL, static_cast<unsigned long long>(X_INT64_MAX)},
      {0LL, X_INT64_MAX});
  ExpectConvertOutOfRange<unsigned long long, long long>(
      NC_UINT64, NC_INT64, {10000000000000000000ULL}, 1);
  ExpectConvertInRange<unsigned long long, unsigned long long>(
      NC_UINT64, NC_UINT64, {0ULL, X_UINT64_MAX}, {0ULL, X_UINT64_MAX});
  ExpectConvertInRange<unsigned long long, float>(
      NC_UINT64, NC_FLOAT, {0ULL, 4096ULL}, {0.0F, 4096.0F});
  ExpectConvertInRange<unsigned long long, double>(
      NC_UINT64, NC_DOUBLE, {0ULL, 4096ULL}, {0.0, 4096.0});
}

TEST(Nc4VarConvertTypeTest, FloatAndDoubleSourceMatrix) {
  // Use fractional overshoots whose truncated integer values remain in bounds
  // so UBSan (-fsanitize=float-cast-overflow) does not trap on C casts.
  // NC_FLOAT source.
  ExpectConvertInRange<float, unsigned char>(NC_FLOAT, NC_UBYTE, {0.0F, 255.0F},
                                             {0, 255});
  ExpectConvertOutOfRange<float, unsigned char>(NC_FLOAT, NC_UBYTE,
                                                {255.5F, -0.5F}, 2);
  ExpectConvertInRange<float, signed char>(NC_FLOAT, NC_BYTE, {-128.0F, 127.0F},
                                           {-128, 127});
  ExpectConvertOutOfRange<float, signed char>(NC_FLOAT, NC_BYTE,
                                              {127.5F, -128.5F}, 2);
  ExpectConvertInRange<float, short>(NC_FLOAT, NC_SHORT, {-32768.0F, 32767.0F},
                                     {-32768, 32767});
  ExpectConvertOutOfRange<float, short>(NC_FLOAT, NC_SHORT,
                                        {32767.5F, -32768.5F}, 2);
  ExpectConvertInRange<float, unsigned short>(NC_FLOAT, NC_USHORT,
                                              {0.0F, 65535.0F}, {0, 65535});
  ExpectConvertOutOfRange<float, unsigned short>(NC_FLOAT, NC_USHORT,
                                                 {65535.5F, -0.5F}, 2);
  ExpectConvertInRange<float, unsigned int>(NC_FLOAT, NC_UINT, {0.0F, 1000.0F},
                                            {0U, 1000U});
  // (float)X_UINT_MAX rounds to 2^32, so only negative fractional inputs stay
  // representable after truncation.
  ExpectConvertOutOfRange<float, unsigned int>(NC_FLOAT, NC_UINT, {-0.5F}, 1);
  ExpectConvertInRange<float, int>(NC_FLOAT, NC_INT, {-1000.0F, 2000.0F},
                                   {-1000, 2000});
  // Around +-2^31 float step is >= 256, so any out-of-range float also
  // overflows `(int)*fp++` under UBSan.
  ExpectConvertInRange<float, long long>(
      NC_FLOAT, NC_INT64, {-1024.0F, 2048.0F}, {-1024LL, 2048LL});
  // Any float triggering NC_INT64 range check also overflows `(long
  // long)*fp++`.
  ExpectConvertInRange<float, unsigned long long>(
      NC_FLOAT, NC_UINT64, {0.0F, 4096.0F}, {0ULL, 4096ULL});
  ExpectConvertOutOfRange<float, unsigned long long>(NC_FLOAT, NC_UINT64,
                                                     {-0.5F}, 1);
  ExpectConvertInRange<float, float>(NC_FLOAT, NC_FLOAT, {-1.5F, 3.25F},
                                     {-1.5F, 3.25F});
  ExpectConvertInRange<float, double>(NC_FLOAT, NC_DOUBLE, {-1.5F, 3.25F},
                                      {-1.5, 3.25});

  // NC_DOUBLE source.
  ExpectConvertInRange<double, unsigned char>(NC_DOUBLE, NC_UBYTE, {0.0, 255.0},
                                              {0, 255});
  ExpectConvertOutOfRange<double, unsigned char>(NC_DOUBLE, NC_UBYTE,
                                                 {255.5, -0.5}, 2);
  ExpectConvertInRange<double, signed char>(NC_DOUBLE, NC_BYTE, {-128.0, 127.0},
                                            {-128, 127});
  ExpectConvertOutOfRange<double, signed char>(NC_DOUBLE, NC_BYTE,
                                               {127.5, -128.5}, 2);
  ExpectConvertInRange<double, short>(NC_DOUBLE, NC_SHORT, {-32768.0, 32767.0},
                                      {-32768, 32767});
  ExpectConvertOutOfRange<double, short>(NC_DOUBLE, NC_SHORT,
                                         {32767.5, -32768.5}, 2);
  ExpectConvertInRange<double, unsigned short>(NC_DOUBLE, NC_USHORT,
                                               {0.0, 65535.0}, {0, 65535});
  ExpectConvertOutOfRange<double, unsigned short>(NC_DOUBLE, NC_USHORT,
                                                  {65535.5, -0.5}, 2);
  ExpectConvertInRange<double, unsigned int>(
      NC_DOUBLE, NC_UINT, {0.0, 4294967295.0}, {0U, X_UINT_MAX});
  ExpectConvertOutOfRange<double, unsigned int>(NC_DOUBLE, NC_UINT,
                                                {4294967295.5, -0.5}, 2);
  ExpectConvertInRange<double, int>(
      NC_DOUBLE, NC_INT, {-2147483648.0, 2147483647.0}, {X_INT_MIN, X_INT_MAX});
  ExpectConvertOutOfRange<double, int>(NC_DOUBLE, NC_INT,
                                       {2147483647.5, -2147483648.5}, 2);
  ExpectConvertInRange<double, long long>(
      NC_DOUBLE, NC_INT64, {-100000.0, 200000.0}, {-100000LL, 200000LL});
  // (double)X_INT64_MAX rounds to 2^63 and X_INT64_MIN == -2^63, so any
  // double triggering range_error overflows `(long long)*dp++`.
  ExpectConvertInRange<double, unsigned long long>(
      NC_DOUBLE, NC_UINT64, {0.0, 500000.0}, {0ULL, 500000ULL});
  ExpectConvertOutOfRange<double, unsigned long long>(NC_DOUBLE, NC_UINT64,
                                                      {-0.5}, 1);
  ExpectConvertInRange<double, float>(NC_DOUBLE, NC_FLOAT, {-12.5, 42.25},
                                      {-12.5F, 42.25F});
  ExpectConvertOutOfRange<double, float>(NC_DOUBLE, NC_FLOAT,
                                         {3.4028235e+38, -3.4028235e+38}, 2);
  ExpectConvertInRange<double, double>(NC_DOUBLE, NC_DOUBLE, {-12.5, 42.25},
                                       {-12.5, 42.25});
}

TEST(Nc4VarConvertTypeTest, VariableIoCrossTypeAndStrictClassicRules) {
  // Verify cross-type writes/reads through real NetCDF-4 datasets hit
  // nc4_convert_type including strict_nc3 UBYTE/BYTE handling.
  ScopedNcFile nc4_file;
  ASSERT_EQ(NC_NOERR, nc_create_mem("convert_io.nc", NC_NETCDF4,
                                    kMemInitialBytes, nc4_file.ptr()));
  int dimid = -1;
  int var_int = -1;
  int var_schar = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc4_file, "n", 2, &dimid));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc4_file, "vi", NC_INT, 1, &dimid, &var_int));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc4_file, "vb", NC_BYTE, 1, &dimid, &var_schar));

  const double d_in[2] = {10.0, -25.0};
  ASSERT_EQ(NC_NOERR, nc_put_var_double(nc4_file, var_int, d_in));
  float f_out[2]{};
  ASSERT_EQ(NC_NOERR, nc_get_var_float(nc4_file, var_int, f_out));
  EXPECT_FLOAT_EQ(f_out[0], 10.0F);
  EXPECT_FLOAT_EQ(f_out[1], -25.0F);

  const unsigned char ub_in[2] = {12, 200};
  EXPECT_EQ(NC_ERANGE, nc_put_var_uchar(nc4_file, var_schar, ub_in));

  // Classic-model NetCDF-4 ignores UBYTE<->BYTE sign range errors.
  ScopedNcFile strict_file;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("convert_strict.nc", NC_NETCDF4 | NC_CLASSIC_MODEL,
                          kMemInitialBytes, strict_file.ptr()));
  int s_dim = -1;
  int s_var = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(strict_file, "n", 2, &s_dim));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(strict_file, "vb", NC_BYTE, 1, &s_dim, &s_var));
  ASSERT_EQ(NC_NOERR, nc_enddef(strict_file));
  EXPECT_EQ(NC_NOERR, nc_put_var_uchar(strict_file, s_var, ub_in));
}

// ============================================================================
// Quantization paths in nc4_convert_type
// ============================================================================

TEST(Nc4VarQuantizeTest, DirectConvertQuantizeAllModesFloatAndDouble) {
  const float kCustomFillF = -999.0F;
  const double kCustomFillD = -999.0;
  const float kNanF = std::numeric_limits<float>::quiet_NaN();
  const double kNanD = std::numeric_limits<double>::quiet_NaN();

  struct ModeCase {
    int mode;
    int float_nsd;
    int double_nsd;
  };
  constexpr ModeCase kModes[] = {
      {NC_QUANTIZE_BITGROOM, 3, 5},
      {NC_QUANTIZE_BITROUND, 10, 20},
      {NC_QUANTIZE_GRANULARBR, 3, 5},
  };

  for (const ModeCase& mc : kModes) {
    for (bool use_custom_fill : {false, true}) {
      const float fill_f = use_custom_fill ? kCustomFillF : NC_FILL_FLOAT;
      const double fill_d = use_custom_fill ? kCustomFillD : NC_FILL_DOUBLE;
      const void* fill_ptr_f = use_custom_fill ? &kCustomFillF : nullptr;
      const void* fill_ptr_d = use_custom_fill ? &kCustomFillD : nullptr;

      // Put fill, +/-0.0, NaN at both even and odd indices so BitGroom's even
      // (shave) and odd (set) loops both cover all guard branches.
      const std::array<float, 10> in_f = {
          3.14159265F, -2.7182818F, fill_f, fill_f,      0.0F,
          -0.0F,       kNanF,       kNanF,  -0.0042195F, 123.456789F,
      };
      std::array<float, 10> out_f{};
      int range_error = -1;
      ASSERT_EQ(NC_NOERR,
                nc4_convert_type(in_f.data(), out_f.data(), NC_FLOAT, NC_FLOAT,
                                 in_f.size(), &range_error, fill_ptr_f, 0,
                                 mc.mode, mc.float_nsd));
      EXPECT_EQ(range_error, 0);
      EXPECT_EQ(BitPattern64(out_f[2]), BitPattern64(fill_f));
      EXPECT_EQ(BitPattern64(out_f[3]), BitPattern64(fill_f));
      EXPECT_EQ(BitPattern64(out_f[4]), BitPattern64(0.0F));
      EXPECT_EQ(BitPattern64(out_f[5]), BitPattern64(-0.0F));
      EXPECT_TRUE(std::isnan(out_f[6]));
      EXPECT_TRUE(std::isnan(out_f[7]));
      EXPECT_NEAR(out_f[0], in_f[0], 0.01F);
      EXPECT_NEAR(out_f[1], in_f[1], 0.01F);
      EXPECT_NEAR(out_f[8], in_f[8], 1e-4F);
      EXPECT_NEAR(out_f[9], in_f[9], 0.5F);

      const std::array<double, 10> in_d = {
          3.14159265358979,
          -2.71828182845904,
          fill_d,
          fill_d,
          0.0,
          -0.0,
          kNanD,
          kNanD,
          -0.004219512,
          123.456789123,
      };
      std::array<double, 10> out_d{};
      range_error = -1;
      ASSERT_EQ(NC_NOERR,
                nc4_convert_type(in_d.data(), out_d.data(), NC_DOUBLE,
                                 NC_DOUBLE, in_d.size(), &range_error,
                                 fill_ptr_d, 0, mc.mode, mc.double_nsd));
      EXPECT_EQ(range_error, 0);
      EXPECT_EQ(BitPattern64(out_d[2]), BitPattern64(fill_d));
      EXPECT_EQ(BitPattern64(out_d[3]), BitPattern64(fill_d));
      EXPECT_EQ(BitPattern64(out_d[4]), BitPattern64(0.0));
      EXPECT_EQ(BitPattern64(out_d[5]), BitPattern64(-0.0));
      EXPECT_TRUE(std::isnan(out_d[6]));
      EXPECT_TRUE(std::isnan(out_d[7]));
      EXPECT_NEAR(out_d[0], in_d[0], 1e-4);
      EXPECT_NEAR(out_d[1], in_d[1], 1e-4);
      EXPECT_NEAR(out_d[8], in_d[8], 1e-6);
      EXPECT_NEAR(out_d[9], in_d[9], 0.01);
    }
  }

  // Non-identity type conversion composed with quantization.
  const std::array<double, 4> src_dbl = {1.234567, -9.876543, 0.0, 42.5};
  std::array<float, 4> dst_flt{};
  int range_error = -1;
  ASSERT_EQ(NC_NOERR,
            nc4_convert_type(src_dbl.data(), dst_flt.data(), NC_DOUBLE,
                             NC_FLOAT, src_dbl.size(), &range_error, nullptr, 0,
                             NC_QUANTIZE_BITROUND, 12));
  EXPECT_EQ(range_error, 0);
  EXPECT_EQ(BitPattern64(dst_flt[2]), BitPattern64(0.0F));
  EXPECT_NEAR(dst_flt[0], 1.234567F, 1e-3F);
}

TEST(Nc4VarQuantizeTest, VariableEndToEndQuantizeRoundTrip) {
  ScopedMemio memio;
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc_create_mem("quant_e2e.nc", NC_NETCDF4,
                                      kMemInitialBytes, file.ptr()));
    int dimid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "x", 6, &dimid));

    int v_bg_f = -1;
    int v_br_d = -1;
    int v_gbr_f = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "bg_f", NC_FLOAT, 1, &dimid, &v_bg_f));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file, "br_d", NC_DOUBLE, 1, &dimid, &v_br_d));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file, "gbr_f", NC_FLOAT, 1, &dimid, &v_gbr_f));

    ASSERT_EQ(NC_NOERR,
              nc_def_var_quantize(file, v_bg_f, NC_QUANTIZE_BITGROOM, 3));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_quantize(file, v_br_d, NC_QUANTIZE_BITROUND, 18));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_quantize(file, v_gbr_f, NC_QUANTIZE_GRANULARBR, 3));

    const float custom_fill = -888.0F;
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(file, v_bg_f, NC_FILL, &custom_fill));

    const std::array<float, 6> f_vals = {3.1415927F, -2.7182817F, custom_fill,
                                         0.0F,       100.1234F,   -0.56789F};
    const std::array<double, 6> d_vals = {
        3.1415926535, -2.7182818284, NC_FILL_DOUBLE, 0.0, 100.123456, -0.56789};

    ASSERT_EQ(NC_NOERR, nc_put_var_float(file, v_bg_f, f_vals.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_double(file, v_br_d, d_vals.data()));
    ASSERT_EQ(NC_NOERR, nc_put_var_float(file, v_gbr_f, f_vals.data()));

    ASSERT_EQ(NC_NOERR, nc_close_memio(file.release(), memio.get()));
  }

  ScopedNcFile reopened;
  ASSERT_EQ(NC_NOERR, nc_open_mem("quant_e2e.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, reopened.ptr()));

  int v_bg_f = -1;
  int v_br_d = -1;
  int v_gbr_f = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(reopened, "bg_f", &v_bg_f));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(reopened, "br_d", &v_br_d));
  ASSERT_EQ(NC_NOERR, nc_inq_varid(reopened, "gbr_f", &v_gbr_f));

  int mode = -1;
  int nsd = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_var_quantize(reopened, v_bg_f, &mode, &nsd));
  EXPECT_EQ(mode, NC_QUANTIZE_BITGROOM);
  EXPECT_EQ(nsd, 3);

  std::array<float, 6> bg_out{};
  std::array<double, 6> br_out{};
  std::array<float, 6> gbr_out{};
  ASSERT_EQ(NC_NOERR, nc_get_var_float(reopened, v_bg_f, bg_out.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_double(reopened, v_br_d, br_out.data()));
  ASSERT_EQ(NC_NOERR, nc_get_var_float(reopened, v_gbr_f, gbr_out.data()));

  EXPECT_FLOAT_EQ(bg_out[2], -888.0F);
  EXPECT_FLOAT_EQ(bg_out[3], 0.0F);
  EXPECT_NEAR(bg_out[0], 3.1415927F, 0.01F);
  EXPECT_DOUBLE_EQ(br_out[2], NC_FILL_DOUBLE);
  EXPECT_NEAR(br_out[0], 3.1415926535, 1e-4);
  EXPECT_NEAR(gbr_out[0], 3.1415927F, 0.01F);
}

// ============================================================================
// Type length, default fill values, and nc4_get_fill_value
// ============================================================================

TEST(Nc4VarFillAndTypeLenTest, GetTypeLenMemCoversAtomicUserAndInvalidTypes) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR, nc_create_mem("typelen.nc", NC_NETCDF4, kMemInitialBytes,
                                    file.ptr()));
  nc_type enum_t = NC_NAT;
  nc_type opaque_t = NC_NAT;
  nc_type vlen_t = NC_NAT;
  nc_type cmp_t = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_enum(file, NC_INT, "color_e", &enum_t));
  const int cval = 1;
  ASSERT_EQ(NC_NOERR, nc_insert_enum(file, enum_t, "RED", &cval));
  ASSERT_EQ(NC_NOERR, nc_def_opaque(file, 13, "raw13_t", &opaque_t));
  ASSERT_EQ(NC_NOERR, nc_def_vlen(file, "vlen_flt_t", NC_FLOAT, &vlen_t));
  ASSERT_EQ(NC_NOERR, nc_def_compound(file, 16, "pair_t", &cmp_t));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(file, cmp_t, "a", 0, NC_DOUBLE));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(file, cmp_t, "b", 8, NC_INT));

  NC_FILE_INFO_T* h5 = nullptr;
  ASSERT_EQ(NC_NOERR, nc4_find_grp_h5(file, nullptr, &h5));
  ASSERT_NE(h5, nullptr);

  struct LenCase {
    nc_type xtype;
    size_t expected;
  };
  const LenCase kCases[] = {
      {NC_BYTE, sizeof(char)},        {NC_CHAR, sizeof(char)},
      {NC_UBYTE, sizeof(char)},       {NC_SHORT, sizeof(short)},
      {NC_USHORT, sizeof(short)},     {NC_INT, sizeof(int)},
      {NC_UINT, sizeof(int)},         {NC_FLOAT, sizeof(float)},
      {NC_DOUBLE, sizeof(double)},    {NC_INT64, sizeof(long long)},
      {NC_UINT64, sizeof(long long)}, {NC_STRING, sizeof(char*)},
      {enum_t, sizeof(int)},          {opaque_t, 13},
      {vlen_t, sizeof(nc_vlen_t)},    {cmp_t, 16},
  };

  for (const LenCase& tc : kCases) {
    size_t len = 0;
    EXPECT_EQ(NC_NOERR, nc4_get_typelen_mem(h5, tc.xtype, &len))
        << "xtype=" << tc.xtype;
    EXPECT_EQ(len, tc.expected) << "xtype=" << tc.xtype;
  }

  // NC_NAT passes nc4_find_type (<= NC_STRING) with type == NULL ->
  // NC_EBADTYPE. Negative and unknown user typeids fail in nc4_find_type.
  size_t len = 0;
  EXPECT_EQ(NC_EBADTYPE, nc4_get_typelen_mem(h5, NC_NAT, &len));
  EXPECT_EQ(NC_EINVAL, nc4_get_typelen_mem(h5, static_cast<nc_type>(-1), &len));
  EXPECT_EQ(NC_EBADTYPID,
            nc4_get_typelen_mem(h5, NC_FIRSTUSERTYPEID + 500, &len));
}

TEST(Nc4VarFillAndTypeLenTest, GetDefaultFillValueCoversAllAtomicAndUserTypes) {
  char c = 0;
  ASSERT_EQ(NC_NOERR, nc4_get_default_atomic_fill_value(NC_CHAR, &c));
  EXPECT_EQ(c, NC_FILL_CHAR);

  char* str = nullptr;
  absl::Cleanup free_str = [&str] { std::free(str); };
  ASSERT_EQ(NC_NOERR, nc4_get_default_atomic_fill_value(NC_STRING, &str));
  ASSERT_NE(str, nullptr);
  EXPECT_STREQ(str, NC_FILL_STRING);

  signed char b = 0;
  ASSERT_EQ(NC_NOERR, nc4_get_default_atomic_fill_value(NC_BYTE, &b));
  EXPECT_EQ(b, NC_FILL_BYTE);

  short s = 0;
  ASSERT_EQ(NC_NOERR, nc4_get_default_atomic_fill_value(NC_SHORT, &s));
  EXPECT_EQ(s, NC_FILL_SHORT);

  int i = 0;
  ASSERT_EQ(NC_NOERR, nc4_get_default_atomic_fill_value(NC_INT, &i));
  EXPECT_EQ(i, NC_FILL_INT);

  unsigned char ub = 0;
  ASSERT_EQ(NC_NOERR, nc4_get_default_atomic_fill_value(NC_UBYTE, &ub));
  EXPECT_EQ(ub, NC_FILL_UBYTE);

  unsigned short us = 0;
  ASSERT_EQ(NC_NOERR, nc4_get_default_atomic_fill_value(NC_USHORT, &us));
  EXPECT_EQ(us, NC_FILL_USHORT);

  unsigned int ui = 0;
  ASSERT_EQ(NC_NOERR, nc4_get_default_atomic_fill_value(NC_UINT, &ui));
  EXPECT_EQ(ui, NC_FILL_UINT);

  long long i64 = 0;
  ASSERT_EQ(NC_NOERR, nc4_get_default_atomic_fill_value(NC_INT64, &i64));
  EXPECT_EQ(i64, NC_FILL_INT64);

  unsigned long long ui64 = 0;
  ASSERT_EQ(NC_NOERR, nc4_get_default_atomic_fill_value(NC_UINT64, &ui64));
  EXPECT_EQ(ui64, NC_FILL_UINT64);

  float f = 0.0F;
  ASSERT_EQ(NC_NOERR, nc4_get_default_atomic_fill_value(NC_FLOAT, &f));
  EXPECT_FLOAT_EQ(f, NC_FILL_FLOAT);

  double d = 0.0;
  ASSERT_EQ(NC_NOERR, nc4_get_default_atomic_fill_value(NC_DOUBLE, &d));
  EXPECT_DOUBLE_EQ(d, NC_FILL_DOUBLE);

  EXPECT_EQ(NC_EINVAL, nc4_get_default_atomic_fill_value(NC_NAT, &i));

  // User-defined types + synthetic invalid class check in
  // nc4_get_default_fill_value.
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR, nc_create_mem("def_fill.nc", NC_NETCDF4, kMemInitialBytes,
                                    file.ptr()));
  nc_type enum_t = NC_NAT;
  nc_type opaque_t = NC_NAT;
  nc_type vlen_t = NC_NAT;
  nc_type cmp_t = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_enum(file, NC_SHORT, "e16_t", &enum_t));
  ASSERT_EQ(NC_NOERR, nc_def_opaque(file, 4, "opq4_t", &opaque_t));
  ASSERT_EQ(NC_NOERR, nc_def_vlen(file, "vl_t", NC_INT, &vlen_t));
  ASSERT_EQ(NC_NOERR, nc_def_compound(file, 8, "cmp8_t", &cmp_t));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(file, cmp_t, "x", 0, NC_INT));

  NC_FILE_INFO_T* h5 = nullptr;
  ASSERT_EQ(NC_NOERR, nc4_find_grp_h5(file, nullptr, &h5));
  NC_TYPE_INFO_T* ti_enum = nullptr;
  NC_TYPE_INFO_T* ti_opq = nullptr;
  NC_TYPE_INFO_T* ti_vlen = nullptr;
  NC_TYPE_INFO_T* ti_cmp = nullptr;
  ASSERT_EQ(NC_NOERR, nc4_find_type(h5, enum_t, &ti_enum));
  ASSERT_EQ(NC_NOERR, nc4_find_type(h5, opaque_t, &ti_opq));
  ASSERT_EQ(NC_NOERR, nc4_find_type(h5, vlen_t, &ti_vlen));
  ASSERT_EQ(NC_NOERR, nc4_find_type(h5, cmp_t, &ti_cmp));

  short e_fill = 0;
  ASSERT_EQ(NC_NOERR, nc4_get_default_fill_value(ti_enum, &e_fill));
  EXPECT_EQ(e_fill, NC_FILL_SHORT);

  std::array<unsigned char, 4> opq_fill = {0xAA, 0xBB, 0xCC, 0xDD};
  ASSERT_EQ(NC_NOERR, nc4_get_default_fill_value(ti_opq, opq_fill.data()));
  EXPECT_THAT(opq_fill, ::testing::ElementsAre(0, 0, 0, 0));

  nc_vlen_t vl_fill{123, &e_fill};
  ASSERT_EQ(NC_NOERR, nc4_get_default_fill_value(ti_vlen, &vl_fill));
  EXPECT_EQ(vl_fill.len, 0U);
  EXPECT_EQ(vl_fill.p, nullptr);

  std::array<unsigned char, 8> cmp_fill;
  cmp_fill.fill(0x55);
  ASSERT_EQ(NC_NOERR, nc4_get_default_fill_value(ti_cmp, cmp_fill.data()));
  EXPECT_THAT(cmp_fill, ::testing::Each(0));

  // Passing nullptr fill_value for compound is allowed by line 1858 guard.
  EXPECT_EQ(NC_NOERR, nc4_get_default_fill_value(ti_cmp, nullptr));

  // Atomic id path inside nc4_get_default_fill_value + default NC_EBADTYPE.
  NC_TYPE_INFO_T atomic_ti{};
  atomic_ti.hdr.id = NC_INT;
  int atomic_fill = 0;
  ASSERT_EQ(NC_NOERR, nc4_get_default_fill_value(&atomic_ti, &atomic_fill));
  EXPECT_EQ(atomic_fill, NC_FILL_INT);

  NC_TYPE_INFO_T bad_ti = *ti_opq;
  bad_ti.nc_type_class = static_cast<nc_type>(999);
  EXPECT_EQ(NC_EBADTYPE, nc4_get_default_fill_value(&bad_ti, &atomic_fill));
}

TEST(Nc4VarFillAndTypeLenTest, GetFillValueDirectExplicitNullAndErrorPaths) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR, nc_create_mem("get_fill.nc", NC_NETCDF4, kMemInitialBytes,
                                    file.ptr()));
  int dimid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "x", 2, &dimid));
  nc_type vlen_t = NC_NAT;
  nc_type opq_t = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_vlen(file, "vl_t", NC_INT, &vlen_t));
  ASSERT_EQ(NC_NOERR, nc_def_opaque(file, 8, "opq8_t", &opq_t));

  int v_vlen = -1;
  int v_str = -1;
  int v_dbl = -1;
  int v_def = -1;
  int v_opq = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v_vlen", vlen_t, 1, &dimid, &v_vlen));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v_str", NC_STRING, 1, &dimid, &v_str));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v_dbl", NC_DOUBLE, 1, &dimid, &v_dbl));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v_def", NC_INT, 1, &dimid, &v_def));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v_opq", opq_t, 1, &dimid, &v_opq));

  std::array<int, 2> vl_data = {7, 8};
  const nc_vlen_t vl_in = {vl_data.size(), vl_data.data()};
  ASSERT_EQ(NC_NOERR, nc_def_var_fill(file, v_vlen, NC_FILL, &vl_in));
  const char* str_in = "hello";
  ASSERT_EQ(NC_NOERR, nc_def_var_fill(file, v_str, NC_FILL, &str_in));
  const double dbl_in = -42.5;
  ASSERT_EQ(NC_NOERR, nc_def_var_fill(file, v_dbl, NC_FILL, &dbl_in));

  NC_FILE_INFO_T* h5 = nullptr;
  NC_VAR_INFO_T* var_vlen = nullptr;
  NC_VAR_INFO_T* var_str = nullptr;
  NC_VAR_INFO_T* var_dbl = nullptr;
  NC_VAR_INFO_T* var_def = nullptr;
  NC_VAR_INFO_T* var_opq = nullptr;
  ASSERT_EQ(NC_NOERR,
            nc4_find_grp_h5_var(file, v_vlen, &h5, nullptr, &var_vlen));
  ASSERT_EQ(NC_NOERR,
            nc4_find_grp_h5_var(file, v_str, nullptr, nullptr, &var_str));
  ASSERT_EQ(NC_NOERR,
            nc4_find_grp_h5_var(file, v_dbl, nullptr, nullptr, &var_dbl));
  ASSERT_EQ(NC_NOERR,
            nc4_find_grp_h5_var(file, v_def, nullptr, nullptr, &var_def));
  ASSERT_EQ(NC_NOERR,
            nc4_find_grp_h5_var(file, v_opq, nullptr, nullptr, &var_opq));

  // Explicit VLEN fill value copies element buffer cleanly.
  void* fillp = nullptr;
  ASSERT_EQ(NC_NOERR, nc4_get_fill_value(h5, var_vlen, &fillp));
  ASSERT_NE(fillp, nullptr);
  {
    absl::Cleanup free_vl = [&] {
      nc_reclaim_data_all(file, vlen_t, fillp, 1);
      fillp = nullptr;
    };
    const auto* got_vl = static_cast<const nc_vlen_t*>(fillp);
    EXPECT_EQ(got_vl->len, 2U);
    ASSERT_NE(got_vl->p, nullptr);
    const int* ints = static_cast<const int*>(got_vl->p);
    EXPECT_EQ(ints[0], 7);
    EXPECT_EQ(ints[1], 8);
  }

  // Explicit non-null NC_STRING fill value duplicates string.
  ASSERT_EQ(NC_NOERR, nc4_get_fill_value(h5, var_str, &fillp));
  ASSERT_NE(fillp, nullptr);
  {
    absl::Cleanup free_s = [&fillp] {
      std::free(*static_cast<char**>(fillp));
      std::free(fillp);
      fillp = nullptr;
    };
    ASSERT_NE(*static_cast<char**>(fillp), nullptr);
    EXPECT_STREQ(*static_cast<char**>(fillp), "hello");
  }

  // Explicit NULL NC_STRING pointer skips strdup safely.
  char* null_str_val = nullptr;
  NC_VAR_INFO_T synth_null_str = *var_str;
  synth_null_str.fill_value = &null_str_val;
  ASSERT_EQ(NC_NOERR, nc4_get_fill_value(h5, &synth_null_str, &fillp));
  ASSERT_NE(fillp, nullptr);
  EXPECT_EQ(*static_cast<char**>(fillp), nullptr);
  std::free(fillp);
  fillp = nullptr;

  // Explicit numeric fill value copies sizeof(double) bytes.
  ASSERT_EQ(NC_NOERR, nc4_get_fill_value(h5, var_dbl, &fillp));
  ASSERT_NE(fillp, nullptr);
  EXPECT_DOUBLE_EQ(*static_cast<const double*>(fillp), dbl_in);
  std::free(fillp);
  fillp = nullptr;

  // Unset fill_value falls back to nc4_get_default_fill_value.
  ASSERT_EQ(NC_NOERR, nc4_get_fill_value(h5, var_def, &fillp));
  ASSERT_NE(fillp, nullptr);
  EXPECT_EQ(*static_cast<const int*>(fillp), NC_FILL_INT);
  std::free(fillp);
  fillp = nullptr;

  // Unknown typeid returns nc4_get_typelen_mem error before allocation.
  NC_TYPE_INFO_T bad_id_ti = *var_def->type_info;
  bad_id_ti.hdr.id = NC_FIRSTUSERTYPEID + 999;
  bad_id_ti.nc_type_class = NC_INT;
  NC_VAR_INFO_T synth_bad_id = *var_def;
  synth_bad_id.type_info = &bad_id_ti;
  EXPECT_EQ(NC_EBADTYPID, nc4_get_fill_value(h5, &synth_bad_id, &fillp));

  // Valid user typeid for typelen but invalid nc_type_class triggers
  // nc4_get_default_fill_value failure cleanup (free(*fillp); *fillp = NULL;).
  NC_TYPE_INFO_T bad_class_ti = *var_opq->type_info;
  bad_class_ti.nc_type_class = static_cast<nc_type>(999);
  NC_VAR_INFO_T synth_bad_class = *var_opq;
  synth_bad_class.type_info = &bad_class_ti;
  synth_bad_class.fill_value = nullptr;
  EXPECT_EQ(NC_NOERR, nc4_get_fill_value(h5, &synth_bad_class, &fillp));
  EXPECT_EQ(fillp, nullptr);
}

// ============================================================================
// Chunk size checking and defaults: nc4_check_chunksizes &
// nc4_find_default_chunksizes2
// ============================================================================

TEST(Nc4VarChunksizeTest, CheckChunksizesCoversAtomicVlenOverflowAndBadType) {
  TempFile temp("chk.nc");
  ASSERT_LT(temp.string().size(), static_cast<size_t>(NC_MAX_NAME));
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR, nc_create(temp.c_str(), NC_NETCDF4, file.ptr()));
  absl::Cleanup abort_huge = [&file] {
    if (file.valid()) {
      static_cast<void>(file.abort());
    }
  };
  int d0 = -1;
  int d1 = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d0", 100000000ULL, &d0));
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d1", 100ULL, &d1));

  nc_type vlen_t = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_vlen(file, "vl_t", NC_INT, &vlen_t));

  const int dims[2] = {d0, d1};
  int v_dbl = -1;
  int v_vlen = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v_dbl", NC_DOUBLE, 2, dims, &v_dbl));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v_vlen", vlen_t, 1, &d0, &v_vlen));

  NC_GRP_INFO_T* grp = nullptr;
  NC_VAR_INFO_T* var_dbl = nullptr;
  NC_VAR_INFO_T* var_vlen = nullptr;
  ASSERT_EQ(NC_NOERR,
            nc4_find_grp_h5_var(file, v_dbl, nullptr, &grp, &var_dbl));
  ASSERT_EQ(NC_NOERR,
            nc4_find_grp_h5_var(file, v_vlen, nullptr, nullptr, &var_vlen));

  constexpr size_t kOkChunks[2] = {1000, 10};
  EXPECT_EQ(NC_NOERR, nc4_check_chunksizes(grp, var_dbl, kOkChunks));

  // Product 8 * 100000000 * 10 = 8e9 > NC_MAX_UINT (~4.29e9) -> NC_EBADCHUNK.
  constexpr size_t kBadChunks[2] = {100000000, 10};
  EXPECT_EQ(NC_EBADCHUNK, nc4_check_chunksizes(grp, var_dbl, kBadChunks));
  EXPECT_EQ(NC_EBADCHUNK,
            nc_def_var_chunking(file, v_dbl, NC_CHUNKED, kBadChunks));

  // NC_VLEN branch multiplies by sizeof(nc_vlen_t).
  constexpr size_t kVlenOk = 100;
  constexpr size_t kVlenTooBig = 500000000;
  EXPECT_EQ(NC_NOERR, nc4_check_chunksizes(grp, var_vlen, &kVlenOk));
  EXPECT_EQ(NC_EBADCHUNK, nc4_check_chunksizes(grp, var_vlen, &kVlenTooBig));

  // Invalid typeid propagates nc4_get_typelen_mem error.
  NC_TYPE_INFO_T bad_ti = *var_dbl->type_info;
  bad_ti.hdr.id = NC_NAT;
  NC_VAR_INFO_T synth_bad = *var_dbl;
  synth_bad.type_info = &bad_ti;
  EXPECT_EQ(NC_EBADTYPE, nc4_check_chunksizes(grp, &synth_bad, kOkChunks));

  ASSERT_EQ(NC_NOERR, file.abort());
}

TEST(Nc4VarChunksizeTest, FindDefaultChunksizesCoversGeometriesAndHalvingLoop) {
  TempFile temp("def_chk.nc");
  ASSERT_LT(temp.string().size(), static_cast<size_t>(NC_MAX_NAME));
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR, nc_create(temp.c_str(), NC_NETCDF4, file.ptr()));
  absl::Cleanup abort_huge = [&file] {
    if (file.valid()) {
      static_cast<void>(file.abort());
    }
  };
  int d_unlim1 = -1;
  int d_unlim2 = -1;
  int d_small = -1;
  int d_medium = -1;
  int d_fixed_a = -1;
  int d_fixed_b = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "u1", NC_UNLIMITED, &d_unlim1));
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "u2", NC_UNLIMITED, &d_unlim2));
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "small", 5, &d_small));
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "med", 2, &d_medium));
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "fa", size_t{50000}, &d_fixed_a));
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "fb", size_t{50000}, &d_fixed_b));

  nc_type opq2k_t = NC_NAT;
  nc_type opq5m_t = NC_NAT;
  nc_type opq2m_t = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_opaque(file, 2048, "opq2k_t", &opq2k_t));
  ASSERT_EQ(NC_NOERR, nc_def_opaque(file, 5000000, "opq5m_t", &opq5m_t));
  ASSERT_EQ(NC_NOERR, nc_def_opaque(file, 2097152, "opq2m_t", &opq2m_t));

  // 1D unlimited small atomic type (DEFAULT_CHUNK_SIZE / type_size > 4096).
  int v_1d_unlim_int = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v_1d_unlim_int", NC_INT, 1, &d_unlim1,
                                 &v_1d_unlim_int));

  // 1D unlimited medium opaque (type_size=2048 -> DEFAULT_CHUNK_SIZE / 2048 <=
  // 4096, suggested_size / type_size == 1).
  int v_1d_unlim_opq = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v_1d_unlim_opq", opq2k_t, 1, &d_unlim1,
                                 &v_1d_unlim_opq));

  // 2D all-unlimited atomic type + huge opaque (suggested_size == 0 -> 1).
  const int dims_2u[2] = {d_unlim1, d_unlim2};
  int v_2d_unlim_int = -1;
  int v_2d_unlim_big = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v_2d_unlim_int", NC_INT, 2, dims_2u,
                                 &v_2d_unlim_int));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v_2d_unlim_big", opq5m_t, 2, dims_2u,
                                 &v_2d_unlim_big));

  // Mixed fixed + unlimited dims + overhang + NC_STRING type_size branch.
  const int dims_mix[2] = {d_unlim1, d_small};
  int v_str_mix = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(file, "v_str_mix", NC_STRING, 2, dims_mix, &v_str_mix));

  // Small fixed dims with huge opaque type -> suggested_size == 0 -> 1.
  const int dims_tiny_fix[2] = {d_small, d_medium};
  int v_tiny_big = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v_tiny_big", opq2m_t, 2, dims_tiny_fix,
                                 &v_tiny_big));

  // Large fixed 2D variable for overhang + NULL-chunksizes + NC_EBADCHUNK
  // halving loop.
  const int dims_huge_fix[2] = {d_fixed_a, d_fixed_b};
  int v_huge_fix = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v_huge_fix", NC_INT, 2, dims_huge_fix,
                                 &v_huge_fix));

  // Verify default chunksizes produced during nc_def_var.
  int storage = -1;
  size_t cs1 = 0;
  ASSERT_EQ(NC_NOERR,
            nc_inq_var_chunking(file, v_1d_unlim_int, &storage, &cs1));
  EXPECT_EQ(storage, NC_CHUNKED);
  EXPECT_EQ(cs1, 1024U);

  storage = -1;
  cs1 = 0;
  ASSERT_EQ(NC_NOERR,
            nc_inq_var_chunking(file, v_1d_unlim_opq, &storage, &cs1));
  EXPECT_EQ(storage, NC_CHUNKED);
  EXPECT_EQ(cs1, 1U);

  storage = -1;
  size_t cs2[2]{0, 0};
  ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(file, v_2d_unlim_int, &storage, cs2));
  EXPECT_EQ(storage, NC_CHUNKED);
  EXPECT_EQ(cs2[0], 1024U);
  EXPECT_EQ(cs2[1], 1024U);

  storage = -1;
  cs2[0] = 0;
  cs2[1] = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(file, v_2d_unlim_big, &storage, cs2));
  EXPECT_EQ(storage, NC_CHUNKED);
  EXPECT_EQ(cs2[0], 1U);
  EXPECT_EQ(cs2[1], 1U);

  storage = -1;
  cs2[0] = 0;
  cs2[1] = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(file, v_str_mix, &storage, cs2));
  EXPECT_EQ(storage, NC_CHUNKED);
  EXPECT_EQ(cs2[0], 1U);
  EXPECT_EQ(cs2[1], 5U);

  // Fixed-dim variables without filters default to NC_CONTIGUOUS, so
  // nc_inq_var_chunking() leaves chunksizesp untouched; inspect the default
  // chunksizes computed during nc_def_var directly on NC_VAR_INFO_T.
  storage = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(file, v_tiny_big, &storage, nullptr));
  EXPECT_EQ(storage, NC_CONTIGUOUS);
  NC_GRP_INFO_T* grp = nullptr;
  NC_VAR_INFO_T* var_tiny = nullptr;
  ASSERT_EQ(NC_NOERR,
            nc4_find_grp_h5_var(file, v_tiny_big, nullptr, &grp, &var_tiny));
  ASSERT_NE(var_tiny->chunksizes, nullptr);
  EXPECT_EQ(var_tiny->chunksizes[0], 1U);
  EXPECT_EQ(var_tiny->chunksizes[1], 1U);
  var_tiny->chunksizes[0] = 0;
  var_tiny->chunksizes[1] = 0;
  ASSERT_EQ(NC_NOERR, nc4_find_default_chunksizes2(grp, var_tiny));
  EXPECT_EQ(var_tiny->chunksizes[0], 1U);
  EXPECT_EQ(var_tiny->chunksizes[1], 1U);

  // Direct call with var->chunksizes == NULL allocates inside
  // nc4_find_default_chunksizes2 and exercises the fixed-dim overhang path
  // (pow(4194304 / (2.5e9 * 4), 0.5) * 50000 - 0.5 = 1023.5 -> 1023,
  // overhang 49 * 1023 - 50000 = 127 -> 1023 - 127 / 49 = 1021).
  NC_VAR_INFO_T* var_huge = nullptr;
  ASSERT_EQ(NC_NOERR,
            nc4_find_grp_h5_var(file, v_huge_fix, nullptr, &grp, &var_huge));
  std::free(var_huge->chunksizes);
  var_huge->chunksizes = nullptr;
  ASSERT_EQ(NC_NOERR, nc4_find_default_chunksizes2(grp, var_huge));
  ASSERT_NE(var_huge->chunksizes, nullptr);
  EXPECT_EQ(var_huge->chunksizes[0], 1021U);
  EXPECT_EQ(var_huge->chunksizes[1], 1021U);

  // Pre-seed chunksizes whose product exceeds NC_MAX_UINT to trigger the
  // retval == NC_EBADCHUNK halving loop cleanly without large allocations.
  var_huge->chunksizes[0] = size_t{50000};
  var_huge->chunksizes[1] = size_t{50000};
  ASSERT_EQ(NC_NOERR, nc4_find_default_chunksizes2(grp, var_huge));
  EXPECT_EQ(NC_NOERR,
            nc4_check_chunksizes(grp, var_huge, var_huge->chunksizes));
  EXPECT_EQ(var_huge->chunksizes[0], 25000U);
  EXPECT_EQ(var_huge->chunksizes[1], 25000U);

  // Also trigger the `chunksizes[d]/2 ? ... : 1` fallback for unit chunk dim.
  var_huge->chunksizes[0] = size_t{2000000000};
  var_huge->chunksizes[1] = size_t{1};
  ASSERT_EQ(NC_NOERR, nc4_find_default_chunksizes2(grp, var_huge));
  EXPECT_EQ(NC_NOERR,
            nc4_check_chunksizes(grp, var_huge, var_huge->chunksizes));
  EXPECT_EQ(var_huge->chunksizes[0], 50000U);
  EXPECT_EQ(var_huge->chunksizes[1], 1U);

  // Propagate non-NC_EBADCHUNK error from nc4_check_chunksizes inside
  // nc4_find_default_chunksizes2.
  NC_TYPE_INFO_T bad_ti = *var_huge->type_info;
  bad_ti.hdr.id = NC_NAT;
  NC_VAR_INFO_T synth_bad = *var_huge;
  synth_bad.type_info = &bad_ti;
  std::array<size_t, 2> synth_cs = {10, 10};
  synth_bad.chunksizes = synth_cs.data();
  EXPECT_EQ(NC_EBADTYPE, nc4_find_default_chunksizes2(grp, &synth_bad));

  // Stay in define mode and abort cleanly without creating large datasets.
  ASSERT_EQ(NC_NOERR, file.abort());
}

}  // namespace
}  // namespace netcdf
