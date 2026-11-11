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

// Clean-room unit tests for third_party/netcdf/libdispatch/dvar.c.

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/libdispatch/dvar_shim.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"

extern "C" {
// clang-format off
#include "third_party/netcdf/include/ncdispatch.h"
// clang-format on
}

namespace netcdf {
namespace {

struct FreeDeleter {
  void operator()(void* ptr) const { std::free(ptr); }
};

template <typename T>
using ScopedCBuffer = std::unique_ptr<T[], FreeDeleter>;

class ScopedDvarUdfFault {
 public:
  explicit ScopedDvarUdfFault(int fault_mode) {
    netcdf_dvar_set_udf_fault(fault_mode);
  }
  ~ScopedDvarUdfFault() { netcdf_dvar_set_udf_fault(kDvarFaultNone); }

  ScopedDvarUdfFault(const ScopedDvarUdfFault&) = delete;
  ScopedDvarUdfFault& operator=(const ScopedDvarUdfFault&) = delete;
};

TEST(DvarTest, DefVarAndRenameVarClassicAndNetcdf4) {
  int varid = -1;
  EXPECT_EQ(NC_EBADID, nc_def_var(-1, "v", NC_INT, 0, nullptr, &varid));
  EXPECT_EQ(NC_EBADID, nc_rename_var(-1, 0, "new_name"));

  // Classic dataset definition and rename.
  {
    ScopedNcFile classic;
    ASSERT_EQ(NC_NOERR, nc_create("dvar_def_rename_nc3.nc",
                                  NC_CLOBBER | NC_DISKLESS, classic.ptr()));
    int d0 = -1;
    int d1 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(classic.get(), "d0", 3, &d0));
    ASSERT_EQ(NC_NOERR, nc_def_dim(classic.get(), "d1", 4, &d1));
    const int d2[2] = {d0, d1};

    int v_a = -1;
    int v_b = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(classic.get(), "alpha", NC_FLOAT, 2, d2, &v_a));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(classic.get(), "beta", NC_DOUBLE, 0, nullptr, &v_b));

    EXPECT_EQ(NC_ENAMEINUSE,
              nc_def_var(classic.get(), "alpha", NC_INT, 0, nullptr, &varid));
    EXPECT_EQ(NC_EBADNAME, nc_def_var(classic.get(), "bad/name", NC_INT, 0,
                                      nullptr, &varid));
    const int bad_dim = 99;
    EXPECT_EQ(NC_EBADDIM, nc_def_var(classic.get(), "bad_dim", NC_INT, 1,
                                     &bad_dim, &varid));

    ASSERT_EQ(NC_NOERR, nc_rename_var(classic.get(), v_a, "alpha_long"));
    EXPECT_EQ(NC_ENAMEINUSE, nc_rename_var(classic.get(), v_a, "beta"));
    EXPECT_EQ(NC_ENOTVAR, nc_rename_var(classic.get(), 42, "ghost"));

    ASSERT_EQ(NC_NOERR, nc_enddef(classic.get()));
    char got_name[NC_MAX_NAME + 1]{};
    ASSERT_EQ(NC_NOERR, nc_inq_varname(classic.get(), v_a, got_name));
    EXPECT_STREQ(got_name, "alpha_long");
  }

  // NetCDF-4 dataset definition and rename.
  {
    ScopedNcFile nc4;
    ASSERT_EQ(NC_NOERR,
              nc_create("dvar_def_rename_nc4.nc",
                        NC_CLOBBER | NC_NETCDF4 | NC_DISKLESS, nc4.ptr()));
    int dimid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc4.get(), "n", 5, &dimid));
    int v0 = -1;
    int v1 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(nc4.get(), "x", NC_INT64, 1, &dimid, &v0));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc4.get(), "y", NC_STRING, 1, &dimid, &v1));
    ASSERT_EQ(NC_NOERR, nc_rename_var(nc4.get(), v0, "x_renamed"));
    EXPECT_EQ(NC_ENOTVAR, nc_rename_var(nc4.get(), 99, "none"));
    EXPECT_EQ(NC_ENAMEINUSE, nc_rename_var(nc4.get(), v0, "y"));

    char got_name[NC_MAX_NAME + 1]{};
    ASSERT_EQ(NC_NOERR, nc_inq_varname(nc4.get(), v0, got_name));
    EXPECT_STREQ(got_name, "x_renamed");
  }
}

TEST(DvarTest, DefVarFillCoversBadIdGlobalClassicAndNetcdf4) {
  const int fill_i = 42;
  EXPECT_EQ(NC_EBADID, nc_def_var_fill(-1, 0, NC_FILL, &fill_i));

  {
    ScopedNcFile classic;
    ASSERT_EQ(NC_NOERR, nc_create("dvar_fill_nc3.nc", NC_CLOBBER | NC_DISKLESS,
                                  classic.ptr()));
    EXPECT_EQ(NC_EGLOBAL,
              nc_def_var_fill(classic.get(), NC_GLOBAL, NC_FILL, &fill_i));

    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(classic.get(), "n", 4, &dimid));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(classic.get(), "v", NC_INT, 1, &dimid, &varid));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_fill(classic.get(), varid, NC_FILL, &fill_i));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_fill(classic.get(), varid, NC_NOFILL, nullptr));
    EXPECT_EQ(NC_ENOTVAR,
              nc_def_var_fill(classic.get(), 99, NC_NOFILL, nullptr));
    ASSERT_EQ(NC_NOERR, nc_enddef(classic.get()));
    EXPECT_EQ(NC_ENOTINDEFINE,
              nc_def_var_fill(classic.get(), varid, NC_FILL, &fill_i));
  }

  {
    ScopedNcFile nc4;
    ASSERT_EQ(NC_NOERR,
              nc_create("dvar_fill_nc4.nc",
                        NC_CLOBBER | NC_NETCDF4 | NC_DISKLESS, nc4.ptr()));
    EXPECT_EQ(NC_EGLOBAL,
              nc_def_var_fill(nc4.get(), NC_GLOBAL, NC_FILL, &fill_i));

    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc4.get(), "n", 4, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc4.get(), "v", NC_INT, 1, &dimid, &varid));
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(nc4.get(), varid, NC_FILL, &fill_i));

    int no_fill = -1;
    int got_fill = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_var_fill(nc4.get(), varid, &no_fill, &got_fill));
    EXPECT_EQ(no_fill, 0);
    EXPECT_EQ(got_fill, fill_i);

    ASSERT_EQ(NC_NOERR, nc_def_var_fill(nc4.get(), varid, NC_NOFILL, nullptr));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc4.get()));
    EXPECT_EQ(NC_ELATEDEF, nc_def_var_fill(nc4.get(), varid, NC_FILL, &fill_i));
  }
}

TEST(DvarTest, DefVarStorageFiltersQuantizeEndianSzipAndChunkCache) {
  const size_t chunk1[1] = {4};
  size_t cache_size = 0;
  size_t cache_nelems = 0;
  float cache_preempt = 0.0F;

  EXPECT_EQ(NC_EBADID, nc_def_var_deflate(-1, 0, 1, 1, 4));
  EXPECT_EQ(NC_EBADID, nc_def_var_quantize(-1, 0, NC_QUANTIZE_BITGROOM, 3));
  EXPECT_EQ(NC_EBADID, nc_def_var_fletcher32(-1, 0, NC_FLETCHER32));
  EXPECT_EQ(NC_EBADID, nc_def_var_chunking(-1, 0, NC_CHUNKED, chunk1));
  EXPECT_EQ(NC_EBADID, nc_def_var_endian(-1, 0, NC_ENDIAN_LITTLE));
  EXPECT_EQ(NC_EBADID, nc_def_var_szip(-1, 0, NC_SZIP_EC, 8));
  EXPECT_EQ(NC_EBADID, nc_set_var_chunk_cache(-1, 0, 1024, 17, 0.5F));
  EXPECT_EQ(NC_EBADID, nc_get_var_chunk_cache(-1, 0, &cache_size, &cache_nelems,
                                              &cache_preempt));

  // Classic dataset rejects all NetCDF-4 storage/filter/cache operations.
  {
    ScopedNcFile classic;
    ASSERT_EQ(NC_NOERR, nc_create("dvar_nc3_rejects.nc",
                                  NC_CLOBBER | NC_DISKLESS, classic.ptr()));
    EXPECT_EQ(NC_EGLOBAL, nc_def_var_quantize(classic.get(), NC_GLOBAL,
                                              NC_QUANTIZE_BITGROOM, 3));

    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(classic.get(), "n", 8, &dimid));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(classic.get(), "v", NC_FLOAT, 1, &dimid, &varid));

    EXPECT_EQ(NC_ENOTNC4, nc_def_var_deflate(classic.get(), varid, 1, 1, 4));
    EXPECT_EQ(NC_ENOTNC4, nc_def_var_quantize(classic.get(), varid,
                                              NC_QUANTIZE_BITGROOM, 3));
    EXPECT_EQ(NC_ENOTNC4,
              nc_def_var_fletcher32(classic.get(), varid, NC_FLETCHER32));
    EXPECT_EQ(NC_ENOTNC4,
              nc_def_var_chunking(classic.get(), varid, NC_CHUNKED, chunk1));
    EXPECT_EQ(NC_ENOTNC4,
              nc_def_var_endian(classic.get(), varid, NC_ENDIAN_BIG));
    EXPECT_EQ(NC_ENOTNC4, nc_def_var_szip(classic.get(), varid, NC_SZIP_EC, 8));
    EXPECT_EQ(NC_ENOTNC4,
              nc_set_var_chunk_cache(classic.get(), varid, 1024, 17, 0.5F));
    EXPECT_EQ(NC_ENOTNC4,
              nc_get_var_chunk_cache(classic.get(), varid, &cache_size,
                                     &cache_nelems, &cache_preempt));
  }

  // NetCDF-4 positive paths and parameter validation.
  {
    ScopedNcFile nc4;
    ASSERT_EQ(NC_NOERR,
              nc_create("dvar_nc4_settings.nc",
                        NC_CLOBBER | NC_NETCDF4 | NC_DISKLESS, nc4.ptr()));
    EXPECT_EQ(NC_EGLOBAL, nc_def_var_quantize(nc4.get(), NC_GLOBAL,
                                              NC_QUANTIZE_BITGROOM, 3));

    int d0 = -1;
    int d1 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc4.get(), "r", 4, &d0));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc4.get(), "c", 8, &d1));
    const int d2[2] = {d0, d1};

    int v_defl = -1;
    int v_szip = -1;
    int v_compact = -1;
    int v_contig = -1;
    int v_quant = -1;
    int v_scalar = -1;
    int v_char = -1;
    int v_bad_defl = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc4.get(), "v_defl", NC_INT, 2, d2, &v_defl));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc4.get(), "v_szip", NC_INT, 2, d2, &v_szip));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc4.get(), "v_compact", NC_INT, 2, d2, &v_compact));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc4.get(), "v_contig", NC_DOUBLE, 2, d2, &v_contig));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc4.get(), "v_quant", NC_FLOAT, 1, &d0, &v_quant));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc4.get(), "v_scalar", NC_INT, 0, nullptr, &v_scalar));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc4.get(), "v_char", NC_CHAR, 1, &d0, &v_char));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc4.get(), "v_bad_defl", NC_INT, 2, d2, &v_bad_defl));

    // Deflate + chunking + fletcher32 + endian + per-variable chunk cache.
    const size_t chunks2[2] = {2, 4};
    ASSERT_EQ(NC_NOERR,
              nc_def_var_chunking(nc4.get(), v_defl, NC_CHUNKED, chunks2));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_fletcher32(nc4.get(), v_defl, NC_FLETCHER32));
    ASSERT_EQ(NC_NOERR, nc_def_var_deflate(nc4.get(), v_defl, 1, 1, 6));
    ASSERT_EQ(NC_NOERR, nc_def_var_endian(nc4.get(), v_defl, NC_ENDIAN_LITTLE));

    // NC4_hdf5_def_var_filter only validates deflate_level when the deflate
    // filter is not already defined on the variable.
    EXPECT_EQ(NC_EINVAL, nc_def_var_deflate(nc4.get(), v_bad_defl, 0, 1, 99));
    EXPECT_EQ(NC_EINVAL, nc_def_var_deflate(nc4.get(), v_bad_defl, 0, 1, -1));
    EXPECT_EQ(NC_EINVAL, nc_def_var_deflate(nc4.get(), v_scalar, 1, 0, 0));
    EXPECT_EQ(NC_EINVAL,
              nc_def_var_fletcher32(nc4.get(), v_scalar, NC_FLETCHER32));
    EXPECT_EQ(NC_ENOTVAR, nc_def_var_fletcher32(nc4.get(), 99, NC_FLETCHER32));
    EXPECT_EQ(NC_EINVAL,
              nc_def_var_chunking(nc4.get(), v_scalar, NC_CHUNKED, chunk1));
    const size_t bad_chunks[2] = {100, 4};
    EXPECT_EQ(NC_EBADCHUNK,
              nc_def_var_chunking(nc4.get(), v_contig, NC_CHUNKED, bad_chunks));

    // Compact, contiguous, endianness variants.
    ASSERT_EQ(NC_NOERR,
              nc_def_var_chunking(nc4.get(), v_compact, NC_COMPACT, nullptr));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_chunking(nc4.get(), v_contig, NC_CONTIGUOUS, nullptr));
    ASSERT_EQ(NC_NOERR, nc_def_var_endian(nc4.get(), v_contig, NC_ENDIAN_BIG));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_endian(nc4.get(), v_compact, NC_ENDIAN_NATIVE));
    EXPECT_EQ(NC_EINVAL, nc_def_var_endian(nc4.get(), v_char, NC_ENDIAN_BIG));
    EXPECT_EQ(NC_ENOTVAR, nc_def_var_endian(nc4.get(), 99, NC_ENDIAN_LITTLE));

    // Quantize modes and validation.
    ASSERT_EQ(NC_NOERR,
              nc_def_var_quantize(nc4.get(), v_quant, NC_QUANTIZE_BITGROOM, 4));
    ASSERT_EQ(NC_NOERR, nc_def_var_quantize(nc4.get(), v_quant,
                                            NC_QUANTIZE_GRANULARBR, 3));
    ASSERT_EQ(NC_NOERR, nc_def_var_quantize(nc4.get(), v_quant,
                                            NC_QUANTIZE_BITROUND, 10));
    ASSERT_EQ(NC_NOERR,
              nc_def_var_quantize(nc4.get(), v_quant, NC_NOQUANTIZE, 0));
    EXPECT_EQ(NC_EINVAL,
              nc_def_var_quantize(nc4.get(), v_defl, NC_QUANTIZE_BITGROOM, 3));
    EXPECT_EQ(NC_EINVAL, nc_def_var_quantize(nc4.get(), v_quant, 99, 3));
    EXPECT_EQ(NC_EINVAL, nc_def_var_quantize(nc4.get(), v_quant,
                                             NC_QUANTIZE_BITGROOM, 99));

    // Szip valid EC/NN settings and invalid pixels_per_block.
    EXPECT_EQ(NC_EINVAL, nc_def_var_szip(nc4.get(), v_szip, NC_SZIP_EC, 7));
    EXPECT_EQ(NC_EINVAL, nc_def_var_szip(nc4.get(), v_szip, NC_SZIP_EC, 64));
    EXPECT_EQ(NC_EINVAL, nc_def_var_szip(nc4.get(), v_defl, NC_SZIP_EC, 8));
    ASSERT_EQ(NC_NOERR, nc_def_var_szip(nc4.get(), v_szip, NC_SZIP_NN, 8));

    // Per-variable chunk cache set/get + error cases.
    EXPECT_EQ(NC_EINVAL,
              nc_set_var_chunk_cache(nc4.get(), v_defl, 1024, 17, -0.1F));
    EXPECT_EQ(NC_EINVAL,
              nc_set_var_chunk_cache(nc4.get(), v_defl, 1024, 17, 1.5F));
    EXPECT_EQ(NC_ENOTVAR,
              nc_set_var_chunk_cache(nc4.get(), 99, 1024, 17, 0.5F));
    EXPECT_EQ(NC_ENOTVAR,
              nc_get_var_chunk_cache(nc4.get(), 99, &cache_size, &cache_nelems,
                                     &cache_preempt));

    ASSERT_EQ(NC_NOERR,
              nc_set_var_chunk_cache(nc4.get(), v_defl, 65536, 13, 0.75F));
    ASSERT_EQ(NC_NOERR, nc_get_var_chunk_cache(nc4.get(), v_defl, &cache_size,
                                               &cache_nelems, &cache_preempt));
    EXPECT_EQ(cache_size, 65536U);
    EXPECT_EQ(cache_nelems, 13U);
    EXPECT_FLOAT_EQ(cache_preempt, 0.75F);
    EXPECT_EQ(NC_NOERR, nc_get_var_chunk_cache(nc4.get(), v_defl, nullptr,
                                               nullptr, nullptr));

    ASSERT_EQ(NC_NOERR, nc_enddef(nc4.get()));
    EXPECT_EQ(NC_ELATEDEF,
              nc_def_var_endian(nc4.get(), v_contig, NC_ENDIAN_LITTLE));
  }
}

TEST(DvarTest, FreeStringZeroNonEmptyAndWithNullSlots) {
  EXPECT_EQ(NC_NOERR, nc_free_string(0, nullptr));

  char* empty_ptrs[1] = {nullptr};
  EXPECT_EQ(NC_NOERR, nc_free_string(0, empty_ptrs));

  char* ptrs[3] = {
      static_cast<char*>(std::malloc(6)),
      nullptr,
      static_cast<char*>(std::malloc(4)),
  };
  ASSERT_NE(ptrs[0], nullptr);
  ASSERT_NE(ptrs[2], nullptr);
  std::memcpy(ptrs[0], "hello", 6);
  std::memcpy(ptrs[2], "xyz", 4);
  EXPECT_EQ(NC_NOERR, nc_free_string(3, ptrs));
  ptrs[0] = ptrs[1] = ptrs[2] = nullptr;
}

TEST(DvarTest, AtomicTypeMetadataHelpersCoverAllTypesAndInvalid) {
  struct TypeExpectation {
    nc_type type;
    int typelen_ret;
    size_t atomic_len;
    const char* atomic_name;
  };
  const TypeExpectation kCases[] = {
      {NC_NAT, -1, 0, "undefined"},
      {NC_BYTE, static_cast<int>(sizeof(signed char)), sizeof(signed char),
       "byte"},
      {NC_CHAR, static_cast<int>(sizeof(char)), sizeof(char), "char"},
      {NC_SHORT, static_cast<int>(sizeof(short)), sizeof(short), "short"},
      {NC_INT, static_cast<int>(sizeof(int)), sizeof(int), "int"},
      {NC_FLOAT, static_cast<int>(sizeof(float)), sizeof(float), "float"},
      {NC_DOUBLE, static_cast<int>(sizeof(double)), sizeof(double), "double"},
      {NC_UBYTE, static_cast<int>(sizeof(unsigned char)), sizeof(unsigned char),
       "ubyte"},
      {NC_USHORT, static_cast<int>(sizeof(unsigned short)),
       sizeof(unsigned short), "ushort"},
      {NC_UINT, static_cast<int>(sizeof(unsigned int)), sizeof(unsigned int),
       "uint"},
      {NC_INT64, static_cast<int>(sizeof(signed long long)),
       sizeof(signed long long), "int64"},
      {NC_UINT64, static_cast<int>(sizeof(unsigned long long)),
       sizeof(unsigned long long), "uint64"},
      {NC_STRING, static_cast<int>(sizeof(char*)), sizeof(char*), "string"},
  };

  for (const auto& entry : kCases) {
    EXPECT_EQ(nctypelen(entry.type), entry.typelen_ret);
    EXPECT_EQ(NC_atomictypelen(entry.type), entry.atomic_len);
    const char* name = NC_atomictypename(entry.type);
    ASSERT_NE(name, nullptr);
    EXPECT_STREQ(name, entry.atomic_name);
  }

  EXPECT_EQ(nctypelen(999), -1);
  EXPECT_EQ(NC_atomictypelen(999), 0U);
  EXPECT_EQ(NC_atomictypename(999), nullptr);
}

TEST(DvarTest, InternalRecordShapeAndCheckNullsHelpers) {
  size_t nrecs = 999;
  int nrecdims = -1;
  int is_recdim[NC_MAX_VAR_DIMS]{};
  size_t shape[NC_MAX_VAR_DIMS]{};
  size_t* count_ptr = nullptr;
  ptrdiff_t* stride_ptr = nullptr;

  EXPECT_EQ(NC_is_recvar(-1, 0, &nrecs), 0);
  EXPECT_NE(NC_NOERR, NC_inq_recvar(-1, 0, &nrecdims, is_recdim));
  EXPECT_EQ(NC_EBADID, NC_getshape(-1, 0, 1, shape));
  EXPECT_EQ(NC_EBADID, NC_check_nulls(-1, 0, nullptr, &count_ptr, &stride_ptr));

  // Classic dataset without an unlimited dimension (unlimid == -1 branch).
  {
    ScopedNcFile no_unlim;
    ASSERT_EQ(NC_NOERR, nc_create("dvar_no_unlim_nc3.nc",
                                  NC_CLOBBER | NC_DISKLESS, no_unlim.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(no_unlim.get(), "fix", 5, &dimid));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(no_unlim.get(), "v_fix", NC_INT, 1, &dimid, &varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(no_unlim.get()));

    is_recdim[0] = 7;
    nrecdims = 7;
    EXPECT_EQ(NC_NOERR,
              NC_inq_recvar(no_unlim.get(), varid, &nrecdims, is_recdim));
    EXPECT_EQ(is_recdim[0], 0);
    EXPECT_EQ(NC_is_recvar(no_unlim.get(), varid, &nrecs), 0);
  }

  // Classic dataset with scalar, non-record, and record variables.
  {
    ScopedNcFile classic;
    ASSERT_EQ(NC_NOERR, nc_create("dvar_helpers_nc3.nc",
                                  NC_CLOBBER | NC_DISKLESS, classic.ptr()));
    int d_rec = -1;
    int d_lat = -1;
    int d_lon = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_dim(classic.get(), "time", NC_UNLIMITED, &d_rec));
    ASSERT_EQ(NC_NOERR, nc_def_dim(classic.get(), "lat", 3, &d_lat));
    ASSERT_EQ(NC_NOERR, nc_def_dim(classic.get(), "lon", 4, &d_lon));

    int v_scalar = -1;
    int v_fix = -1;
    int v_rec = -1;
    const int fix_dims[2] = {d_lat, d_lon};
    const int rec_dims[2] = {d_rec, d_lat};
    ASSERT_EQ(NC_NOERR, nc_def_var(classic.get(), "scalar", NC_INT, 0, nullptr,
                                   &v_scalar));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(classic.get(), "grid", NC_INT, 2, fix_dims, &v_fix));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(classic.get(), "series", NC_INT, 2, rec_dims, &v_rec));
    ASSERT_EQ(NC_NOERR, nc_enddef(classic.get()));

    const size_t start_rec[2] = {0, 0};
    const size_t count_rec[2] = {2, 3};
    const int rec_vals[6] = {1, 2, 3, 4, 5, 6};
    ASSERT_EQ(NC_NOERR, nc_put_vara_int(classic.get(), v_rec, start_rec,
                                        count_rec, rec_vals));

    EXPECT_EQ(NC_is_recvar(classic.get(), 99, &nrecs), 0);
    EXPECT_EQ(NC_ENOTVAR,
              NC_inq_recvar(classic.get(), 99, &nrecdims, is_recdim));
    EXPECT_EQ(NC_ENOTVAR, NC_getshape(classic.get(), 99, 1, shape));
    EXPECT_EQ(NC_ENOTVAR, NC_check_nulls(classic.get(), 99, nullptr, &count_ptr,
                                         &stride_ptr));
    EXPECT_EQ(NC_is_recvar(classic.get(), v_scalar, &nrecs), 0);
    EXPECT_EQ(NC_is_recvar(classic.get(), v_fix, &nrecs), 0);
    nrecs = 0;
    EXPECT_EQ(NC_is_recvar(classic.get(), v_rec, &nrecs), 1);
    EXPECT_EQ(nrecs, 2U);

    nrecdims = 7;
    EXPECT_EQ(NC_NOERR,
              NC_inq_recvar(classic.get(), v_scalar, &nrecdims, is_recdim));
    EXPECT_EQ(NC_getshape(classic.get(), v_scalar, 0, shape), NC_NOERR);

    shape[0] = shape[1] = 0;
    ASSERT_EQ(NC_NOERR, NC_getshape(classic.get(), v_fix, 2, shape));
    EXPECT_EQ(shape[0], 3U);
    EXPECT_EQ(shape[1], 4U);

    // NC_check_nulls parameter paths.
    EXPECT_EQ(NC_EINVALCOORDS, NC_check_nulls(classic.get(), v_fix, nullptr,
                                              &count_ptr, nullptr));

    {
      const size_t start2[2] = {0, 0};
      size_t* raw_count = nullptr;
      ptrdiff_t* raw_stride = nullptr;
      ASSERT_EQ(NC_NOERR, NC_check_nulls(classic.get(), v_fix, start2,
                                         &raw_count, &raw_stride));
      ScopedCBuffer<size_t> count_guard(raw_count);
      ScopedCBuffer<ptrdiff_t> stride_guard(raw_stride);
      ASSERT_NE(count_guard.get(), nullptr);
      ASSERT_NE(stride_guard.get(), nullptr);
      EXPECT_EQ(count_guard[0], 3U);
      EXPECT_EQ(count_guard[1], 4U);
      EXPECT_EQ(stride_guard[0], 1);
      EXPECT_EQ(stride_guard[1], 1);
    }

    {
      const size_t start2[2] = {0, 0};
      size_t explicit_count[2] = {1, 2};
      ptrdiff_t explicit_stride[2] = {2, 2};
      size_t* count_alias = explicit_count;
      ptrdiff_t* stride_alias = explicit_stride;
      ASSERT_EQ(NC_NOERR, NC_check_nulls(classic.get(), v_fix, start2,
                                         &count_alias, nullptr));
      EXPECT_EQ(count_alias, explicit_count);
      ASSERT_EQ(NC_NOERR, NC_check_nulls(classic.get(), v_fix, start2,
                                         &count_alias, &stride_alias));
      EXPECT_EQ(stride_alias, explicit_stride);
    }

    {
      size_t* scalar_count = nullptr;
      ptrdiff_t* scalar_stride = nullptr;
      ASSERT_EQ(NC_NOERR, NC_check_nulls(classic.get(), v_scalar, nullptr,
                                         &scalar_count, &scalar_stride));
      ScopedCBuffer<size_t> count_guard(scalar_count);
      ScopedCBuffer<ptrdiff_t> stride_guard(scalar_stride);
    }
  }

  // NetCDF-4 multi-unlimited-dimension variable where unlimdims are not dim 0.
  {
    ScopedNcFile nc4;
    ASSERT_EQ(NC_NOERR,
              nc_create("dvar_helpers_nc4.nc",
                        NC_CLOBBER | NC_NETCDF4 | NC_DISKLESS, nc4.ptr()));
    int d_fix = -1;
    int d_u0 = -1;
    int d_u1 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc4.get(), "fix", 3, &d_fix));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc4.get(), "u0", NC_UNLIMITED, &d_u0));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc4.get(), "u1", NC_UNLIMITED, &d_u1));

    const int dims3[3] = {d_fix, d_u0, d_u1};
    int varid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc4.get(), "multi", NC_INT, 3, dims3, &varid));

    is_recdim[0] = is_recdim[1] = is_recdim[2] = -1;
    nrecdims = -1;
    ASSERT_EQ(NC_NOERR, NC_inq_recvar(nc4.get(), varid, &nrecdims, is_recdim));
    EXPECT_EQ(nrecdims, 2);
    EXPECT_EQ(is_recdim[0], 0);
    EXPECT_EQ(is_recdim[1], 1);
    EXPECT_EQ(is_recdim[2], 1);

    // Null nrecdimsp must also succeed cleanly.
    is_recdim[0] = is_recdim[1] = is_recdim[2] = -1;
    ASSERT_EQ(NC_NOERR, NC_inq_recvar(nc4.get(), varid, nullptr, is_recdim));
    EXPECT_EQ(is_recdim[0], 0);
    EXPECT_EQ(is_recdim[1], 1);
    EXPECT_EQ(is_recdim[2], 1);
  }
}

TEST(DvarTest, UdfDispatchCoversInternalHelperErrorBranches) {
  ASSERT_EQ(NC_NOERR, netcdf_dvar_register_udf());

  ScopedNcFile udf;
  ASSERT_EQ(NC_NOERR, nc_create("dvar_udf_faults.nc",
                                NC_CLOBBER | NC_DISKLESS | NC_UDF8, udf.ptr()));
  int d_unlim = -1;
  int d_fix = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(udf.get(), "rec", NC_UNLIMITED, &d_unlim));
  ASSERT_EQ(NC_NOERR, nc_def_dim(udf.get(), "x", 4, &d_fix));
  const int rec_dims[2] = {d_unlim, d_fix};
  int varid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(udf.get(), "v_rec", NC_INT, 2, rec_dims, &varid));
  ASSERT_EQ(NC_NOERR, nc_enddef(udf.get()));

  size_t nrecs = 0;
  int nrecdims = 0;
  int is_recdim[2]{};
  size_t shape[2]{};
  const size_t start[2] = {0, 0};

  {
    ScopedDvarUdfFault fault(kDvarFaultFailVardimid);
    EXPECT_EQ(NC_is_recvar(udf.get(), varid, &nrecs), 0);
    EXPECT_EQ(NC_ENOTVAR,
              NC_inq_recvar(udf.get(), varid, &nrecdims, is_recdim));
  }

  {
    ScopedDvarUdfFault fault(kDvarFaultFailInqDim);
    EXPECT_EQ(NC_is_recvar(udf.get(), varid, &nrecs), 0);
    EXPECT_EQ(NC_EBADDIM, NC_getshape(udf.get(), varid, 2, shape));
    size_t* raw_count = nullptr;
    EXPECT_EQ(NC_EBADDIM,
              NC_check_nulls(udf.get(), varid, start, &raw_count, nullptr));
    EXPECT_EQ(raw_count, nullptr);
  }

  {
    ScopedDvarUdfFault fault(kDvarFaultFailUnlimdim);
    EXPECT_EQ(NC_is_recvar(udf.get(), varid, &nrecs), 0);
    EXPECT_EQ(NC_EBADDIM,
              NC_inq_recvar(udf.get(), varid, &nrecdims, is_recdim));
  }

  {
    ScopedDvarUdfFault fault(kDvarFaultFailUnlimdimsCount);
    EXPECT_EQ(NC_EBADDIM,
              NC_inq_recvar(udf.get(), varid, &nrecdims, is_recdim));
  }

  {
    ScopedDvarUdfFault fault(kDvarFaultZeroUnlimdims);
    EXPECT_EQ(NC_NOERR, NC_inq_recvar(udf.get(), varid, &nrecdims, is_recdim));
  }

  {
    ScopedDvarUdfFault fault(kDvarFaultFailUnlimdimsIds);
    EXPECT_EQ(NC_EBADDIM,
              NC_inq_recvar(udf.get(), varid, &nrecdims, is_recdim));
  }
}

}  // namespace
}  // namespace netcdf
