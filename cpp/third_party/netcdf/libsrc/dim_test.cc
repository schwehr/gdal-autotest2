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

// Unit tests for //third_party/netcdf/libsrc/dim.c.
//
// Exercises classic-model (CDF-1, CDF-2, CDF-5) dimension definition, lookup,
// inquiry, renaming, unlimited dimensions, UTF-8 normalization, format limits,
// dynamic array growth/duplication, and internal dim.c helper functions.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"

extern "C" {
#include "third_party/netcdf/include/nc3dispatch.h"
#include "third_party/netcdf/include/nc3internal.h"
}  // extern "C"

namespace netcdf {
namespace {

TEST(DimTest, InternalPrimitiveHelpersHandleNullAndEmptyArrays) {
  free_NC_dim(nullptr);

  NC_string* str = new_NC_string(4, "dim0");
  ASSERT_NE(str, nullptr);
  NC_dim* dim = new_x_NC_dim(str);
  ASSERT_NE(dim, nullptr);
  EXPECT_EQ(size_t{0}, dim->size);
  free_NC_dim(dim);

  NC_dimarray empty_arr{};
  free_NC_dimarrayV0(&empty_arr);
  free_NC_dimarrayV(&empty_arr);
  EXPECT_EQ(-1, find_NC_Udim(&empty_arr, nullptr));
  EXPECT_EQ(nullptr, elem_NC_dimarray(&empty_arr, 0));

  NC_dimarray dup_empty{};
  ASSERT_EQ(NC_NOERR, dup_NC_dimarrayV(&dup_empty, &empty_arr));
  EXPECT_EQ(size_t{0}, dup_empty.nelems);
  free_NC_dimarrayV(&dup_empty);
}

TEST(DimTest, DirectNc3DispatchBadNcidAndNullOutputPointers) {
  int id = -1;
  char name_buf[NC_MAX_NAME + 1]{};
  size_t len = 0;

  EXPECT_EQ(NC_EBADID, NC3_def_dim(-1, "d", 4, &id));
  EXPECT_EQ(NC_EBADID, NC3_inq_dimid(-1, "d", &id));
  EXPECT_EQ(NC_EBADID, NC3_inq_dim(-1, 0, name_buf, &len));
  EXPECT_EQ(NC_EBADID, NC3_rename_dim(-1, 0, "d2"));

  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create("dim_null_ptrs.nc", NC_DISKLESS | NC_CLOBBER, nc.ptr()));

  // Inquiries on empty dimarray.
  EXPECT_EQ(NC_EBADDIM, nc_inq_dimid(nc, "missing", &id));
  EXPECT_EQ(NC_EBADDIM, nc_inq_dim(nc, 0, name_buf, &len));
  int unlim_id = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_unlimdim(nc, &unlim_id));
  EXPECT_EQ(-1, unlim_id);

  // Define with dimidp == NULL, then look up with dimid_ptr == NULL.
  ASSERT_EQ(NC_NOERR, NC3_def_dim(nc, "anon_out", 12, nullptr));
  ASSERT_EQ(NC_NOERR, NC3_inq_dimid(nc, "anon_out", nullptr));
  ASSERT_EQ(NC_NOERR, NC3_inq_dimid(nc, "anon_out", &id));
  EXPECT_EQ(0, id);

  // Verify NC3_inq_dim with NULL name and NULL sizep independently.
  ASSERT_EQ(NC_NOERR, NC3_inq_dim(nc, id, nullptr, nullptr));
  ASSERT_EQ(NC_NOERR, NC3_inq_dim(nc, id, name_buf, nullptr));
  EXPECT_STREQ("anon_out", name_buf);
  ASSERT_EQ(NC_NOERR, NC3_inq_dim(nc, id, nullptr, &len));
  EXPECT_EQ(size_t{12}, len);

  // Invalid dimids on populated dimarray.
  EXPECT_EQ(NC_EBADDIM, NC3_inq_dim(nc, -1, name_buf, &len));
  EXPECT_EQ(NC_EBADDIM, NC3_inq_dim(nc, 1, name_buf, &len));
  EXPECT_EQ(NC_NOERR, nc.close());
}

TEST(DimTest, FormatSpecificDimensionSizeLimitsAndUnlimitedDim) {
  if constexpr (sizeof(size_t) > 4) {
    // Classic (CDF-1): max fixed dimension is X_INT_MAX - 3.
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR,
                nc_create("dim_cdf1.nc", NC_DISKLESS | NC_CLOBBER, nc.ptr()));
      constexpr size_t kMaxClassic =
          static_cast<size_t>(std::numeric_limits<int32_t>::max()) - 3U;
      int id = -1;
      EXPECT_EQ(NC_EDIMSIZE, nc_def_dim(nc, "too_big", kMaxClassic + 1U, &id));
      EXPECT_EQ(NC_NOERR, nc_def_dim(nc, "at_max", kMaxClassic, &id));
      EXPECT_EQ(NC_NOERR, nc.abort());
    }

    // 64-bit offset (CDF-2): max fixed dimension is X_UINT_MAX - 3.
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR, nc_create("dim_cdf2.nc",
                                    NC_DISKLESS | NC_CLOBBER | NC_64BIT_OFFSET,
                                    nc.ptr()));
      constexpr size_t kMaxCdf2 =
          static_cast<size_t>(std::numeric_limits<uint32_t>::max()) - 3U;
      int id = -1;
      EXPECT_EQ(NC_EDIMSIZE, nc_def_dim(nc, "too_big", kMaxCdf2 + 1U, &id));
      EXPECT_EQ(NC_NOERR, nc_def_dim(nc, "at_max", kMaxCdf2, &id));
      EXPECT_EQ(NC_NOERR, nc.abort());
    }

    // CDF-5 (NC_64BIT_DATA): max fixed dimension is X_UINT64_MAX - 3.
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR,
                nc_create("dim_cdf5.nc", NC_DISKLESS | NC_CLOBBER | NC_CDF5,
                          nc.ptr()));
      constexpr size_t kMaxCdf5 =
          static_cast<size_t>(std::numeric_limits<uint64_t>::max() - 3ULL);
      int id = -1;
      EXPECT_EQ(NC_EDIMSIZE, nc_def_dim(nc, "too_big", kMaxCdf5 + 1U, &id));
      EXPECT_EQ(NC_NOERR, nc_def_dim(nc, "at_max", kMaxCdf5, &id));
      EXPECT_EQ(NC_NOERR, nc.abort());
    }
  }

  // Unlimited dimension placed after fixed dims exercises find_NC_Udim loop.
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create("dim_unlim.nc", NC_DISKLESS | NC_CLOBBER, nc.ptr()));
  int fixed0 = -1;
  int fixed1 = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "lat", 4, &fixed0));
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "lon", 8, &fixed1));

  // No unlimited dim yet: search traverses all elements and returns -1.
  int unlim_id = 99;
  ASSERT_EQ(NC_NOERR, nc_inq_unlimdim(nc, &unlim_id));
  EXPECT_EQ(-1, unlim_id);

  int rec_id = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "time", NC_UNLIMITED, &rec_id));
  EXPECT_EQ(2, rec_id);
  ASSERT_EQ(NC_NOERR, nc_inq_unlimdim(nc, &unlim_id));
  EXPECT_EQ(rec_id, unlim_id);

  // Second unlimited dim rejected with NC_EUNLIMIT.
  int dup_unlim = -1;
  EXPECT_EQ(NC_EUNLIMIT, nc_def_dim(nc, "time2", NC_UNLIMITED, &dup_unlim));

  // Unlimited dim length reflects written records.
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "series", NC_INT, 1, &rec_id, &varid));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));

  size_t rec_len = 999;
  ASSERT_EQ(NC_NOERR, nc_inq_dimlen(nc, rec_id, &rec_len));
  EXPECT_EQ(size_t{0}, rec_len);

  for (size_t idx = 0; idx < 3; ++idx) {
    const int val = static_cast<int>(100 + idx);
    ASSERT_EQ(NC_NOERR, nc_put_var1_int(nc, varid, &idx, &val));
  }
  ASSERT_EQ(NC_NOERR, nc_inq_dimlen(nc, rec_id, &rec_len));
  EXPECT_EQ(size_t{3}, rec_len);
  EXPECT_EQ(NC_NOERR, nc.close());
}

TEST(DimTest, ArrayGrowthRedefDuplicationUtf8AndMaxLengthNames) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create("dim_growth.nc", NC_DISKLESS | NC_CLOBBER, nc.ptr()));

  // Redef on 0-dim dataset hits empty dup_NC_dimarrayV path.
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));
  ASSERT_EQ(NC_NOERR, nc_redef(nc));

  // Define >= 10 dims so incr_NC_dimarray grows past NC_ARRAY_GROWBY (4 -> 8 ->
  // 12), including NFD UTF-8 normalization and an exact NC_MAX_NAME name.
  for (int i = 0; i < 8; ++i) {
    int id = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, ("dim_" + std::to_string(i)).c_str(),
                                   static_cast<size_t>(i + 1), &id));
    EXPECT_EQ(i, id);
  }

  // Decomposed UTF-8 "caf\u0065\u0301" normalizes to composed "caf\u00e9".
  const char kNfdName[] = "cafe\xCC\x81";
  const char kNfcName[] = "caf\xC3\xA9";
  int utf8_id = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, kNfdName, 7, &utf8_id));
  int dup_id = -1;
  EXPECT_EQ(NC_ENAMEINUSE, nc_def_dim(nc, kNfcName, 7, &dup_id));
  int queried_id = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_dimid(nc, kNfcName, &queried_id));
  EXPECT_EQ(utf8_id, queried_id);

  // Invalid names and invalid UTF-8 lookup.
  EXPECT_EQ(NC_EBADNAME, nc_def_dim(nc, "", 5, &dup_id));
  EXPECT_EQ(NC_EBADNAME, nc_def_dim(nc, "bad/slash", 5, &dup_id));
  EXPECT_EQ(NC_EBADNAME, nc_def_dim(nc, "trailing ", 5, &dup_id));
  const std::string too_long_name(NC_MAX_NAME + 1, 'x');
  EXPECT_EQ(NC_EMAXNAME, nc_def_dim(nc, too_long_name.c_str(), 5, &dup_id));
  EXPECT_EQ(NC_EBADDIM, nc_inq_dimid(nc, "\xFF\xFE", &queried_id));

  // Exact NC_MAX_NAME length triggers false branch of nchars < NC_MAX_NAME.
  const std::string max_name(NC_MAX_NAME, 'm');
  int max_id = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, max_name.c_str(), 11, &max_id));

  char out_name[NC_MAX_NAME + 1]{};
  size_t out_len = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_dim(nc, max_id, out_name, &out_len));
  EXPECT_EQ(max_name, out_name);
  EXPECT_EQ(size_t{11}, out_len);

  // Finish define mode, then test data-mode error, populated redef(), and
  // abort().
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));
  EXPECT_EQ(NC_ENOTINDEFINE, nc_def_dim(nc, "late_dim", 3, &dup_id));

  ASSERT_EQ(NC_NOERR, nc_redef(nc));
  int extra_id = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "during_redef", 9, &extra_id));
  EXPECT_EQ(NC_NOERR, nc.abort());
}

TEST(DimTest, RenameDimDefineDataReadOnlyAndHsyncPaths) {
  TempFile disk_file("dim_rename.nc");

  // Create on disk, rename inside initial define mode (including longer name),
  // then transition to data mode.
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(disk_file.c_str(), NC_CLOBBER, nc.ptr()));
    int d0 = -1;
    int d1 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "short", 5, &d0));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "other_dimension_name", 6, &d1));

    // Invalid names / duplicate / bad dimid checks in define mode.
    EXPECT_EQ(NC_EBADNAME, nc_rename_dim(nc, d0, ""));
    EXPECT_EQ(NC_ENAMEINUSE, nc_rename_dim(nc, d0, "other_dimension_name"));
    EXPECT_EQ(NC_EBADDIM, nc_rename_dim(nc, -1, "valid_new"));
    EXPECT_EQ(NC_EBADDIM, nc_rename_dim(nc, 99, "valid_new"));

    // Rename in define mode to a longer UTF-8 name.
    ASSERT_EQ(NC_NOERR, nc_rename_dim(nc, d0, "longer_rename_cafe\xCC\x81"));
    int found = -1;
    EXPECT_EQ(NC_EBADDIM, nc_inq_dimid(nc, "short", &found));
    ASSERT_EQ(NC_NOERR, nc_inq_dimid(nc, "longer_rename_caf\xC3\xA9", &found));
    EXPECT_EQ(d0, found);

    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    // Data mode: longer rename fails with NC_ENOTINDEFINE.
    EXPECT_EQ(NC_ENOTINDEFINE,
              nc_rename_dim(nc, d1, "much_longer_than_other_dimension_name"));

    // Data mode: shorter rename succeeds and updates hashmap.
    ASSERT_EQ(NC_NOERR, nc_rename_dim(nc, d1, "other_v2"));
    EXPECT_EQ(NC_EBADDIM, nc_inq_dimid(nc, "other_dimension_name", &found));
    ASSERT_EQ(NC_NOERR, nc_inq_dimid(nc, "other_v2", &found));
    EXPECT_EQ(d1, found);

    // Enable NC_HSYNC on NC3_INFO to exercise header sync branch in data mode.
    NC* base_nc = nullptr;
    ASSERT_EQ(NC_NOERR, NC_check_id(nc, &base_nc));
    ASSERT_NE(base_nc, nullptr);
    NC3_INFO* nc3 = NC3_DATA(base_nc);
    ASSERT_NE(nc3, nullptr);
    nc3->state |= NC_HSYNC;

    ASSERT_EQ(NC_NOERR, nc_rename_dim(nc, d1, "synced"));
    ASSERT_EQ(NC_NOERR, nc.close());
  }

  // Reopen read-only, verify persisted names, and check NC_EPERM on rename.
  ScopedNcFile ro;
  ASSERT_EQ(NC_NOERR, nc_open(disk_file.c_str(), NC_NOWRITE, ro.ptr()));
  int synced_id = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_dimid(ro, "synced", &synced_id));
  EXPECT_EQ(1, synced_id);
  char synced_name[NC_MAX_NAME + 1]{};
  size_t synced_len = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_dim(ro, synced_id, synced_name, &synced_len));
  EXPECT_STREQ("synced", synced_name);
  EXPECT_EQ(size_t{6}, synced_len);
  EXPECT_EQ(NC_EPERM, nc_rename_dim(ro, synced_id, "blocked"));
  EXPECT_EQ(NC_NOERR, ro.close());
}

}  // namespace
}  // namespace netcdf
