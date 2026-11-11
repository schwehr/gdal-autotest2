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

// Clean-room unit tests for third_party/netcdf/libdispatch/dvlen.c.

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
// clang-format on

namespace netcdf {
namespace {

using ::testing::ElementsAre;

class ScopedVlenReclaim {
 public:
  ScopedVlenReclaim(int ncid, nc_type xtype, nc_vlen_t* data, size_t count)
      : ncid_(ncid), xtype_(xtype), data_(data), count_(count) {}
  ~ScopedVlenReclaim() {
    EXPECT_EQ(NC_NOERR, nc_reclaim_data(ncid_, xtype_, data_, count_));
  }

  ScopedVlenReclaim(const ScopedVlenReclaim&) = delete;
  ScopedVlenReclaim& operator=(const ScopedVlenReclaim&) = delete;

 private:
  int ncid_;
  nc_type xtype_;
  nc_vlen_t* data_;
  size_t count_;
};

TEST(DvlenTest, FreeVlenHandlesNullAndHeapPointers) {
  nc_vlen_t empty{};
  empty.len = 0;
  empty.p = nullptr;
  EXPECT_EQ(NC_NOERR, nc_free_vlen(&empty));
  EXPECT_EQ(empty.p, nullptr);

  nc_vlen_t allocated{};
  allocated.len = 4;
  allocated.p = std::malloc(4 * sizeof(int));
  ASSERT_NE(allocated.p, nullptr);
  EXPECT_EQ(NC_NOERR, nc_free_vlen(&allocated));
  allocated.p = nullptr;
}

TEST(DvlenTest, FreeVlensHandlesZeroCountAndMixedArray) {
  EXPECT_EQ(NC_NOERR, nc_free_vlens(0, nullptr));

  nc_vlen_t zero_arr[1]{};
  EXPECT_EQ(NC_NOERR, nc_free_vlens(0, zero_arr));

  nc_vlen_t vlens[3]{};
  vlens[0].len = 2;
  vlens[0].p = std::malloc(2 * sizeof(int));
  ASSERT_NE(vlens[0].p, nullptr);
  vlens[1].len = 0;
  vlens[1].p = nullptr;
  vlens[2].len = 1;
  vlens[2].p = std::malloc(sizeof(double));
  ASSERT_NE(vlens[2].p, nullptr);

  EXPECT_EQ(NC_NOERR, nc_free_vlens(3, vlens));
  for (nc_vlen_t& entry : vlens) {
    entry.p = nullptr;
  }
}

TEST(DvlenTest, DefVlenRejectsBadNcidAndClassicFormats) {
  nc_type vlen_type = NC_NAT;
  EXPECT_EQ(NC_EBADID, nc_def_vlen(-1, "vlen_invalid", NC_INT, &vlen_type));

  const int classic_modes[] = {
      NC_CLOBBER | NC_DISKLESS,
      NC_CLOBBER | NC_64BIT_OFFSET | NC_DISKLESS,
      NC_CLOBBER | NC_CDF5 | NC_DISKLESS,
  };
  for (size_t i = 0; i < sizeof(classic_modes) / sizeof(classic_modes[0]);
       ++i) {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc_create("dvlen_classic_inmem.nc", classic_modes[i],
                                  file.ptr()));
    EXPECT_EQ(NC_ENOTNC4,
              nc_def_vlen(file.get(), "vlen_nc3", NC_INT, &vlen_type));
  }

  ScopedNcFile strict_nc4;
  ASSERT_EQ(NC_NOERR,
            nc_create("dvlen_strict_nc4.nc",
                      NC_CLOBBER | NC_NETCDF4 | NC_CLASSIC_MODEL | NC_DISKLESS,
                      strict_nc4.ptr()));
  EXPECT_EQ(NC_ESTRICTNC3,
            nc_def_vlen(strict_nc4.get(), "vlen_strict", NC_INT, &vlen_type));

  // Verify closed ncid fails NC_check_id cleanly.
  const int closed_id = strict_nc4.get();
  ASSERT_EQ(NC_NOERR, strict_nc4.close());
  EXPECT_EQ(NC_EBADID,
            nc_def_vlen(closed_id, "vlen_closed", NC_INT, &vlen_type));
}

TEST(DvlenTest, DefAndInqVlenAtomicNestedAndNullOutParams) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR,
            nc_create("dvlen_def_inq.nc", NC_CLOBBER | NC_NETCDF4 | NC_DISKLESS,
                      file.ptr()));

  nc_type bad_type = NC_NAT;
  EXPECT_EQ(NC_EINVAL, nc_def_vlen(file.get(), nullptr, NC_INT, &bad_type));
  EXPECT_EQ(NC_EBADNAME, nc_def_vlen(file.get(), "", NC_INT, &bad_type));
  EXPECT_EQ(NC_EBADNAME,
            nc_def_vlen(file.get(), "bad/vlen", NC_INT, &bad_type));

  nc_type int_vlen = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_vlen(file.get(), "int_vlen", NC_INT, &int_vlen));
  EXPECT_EQ(NC_ENAMEINUSE,
            nc_def_vlen(file.get(), "int_vlen", NC_INT, &bad_type));

  // NULL xtypep is accepted; query back by name via nc_inq_typeid.
  ASSERT_EQ(NC_NOERR, nc_def_vlen(file.get(), "short_vlen", NC_SHORT, nullptr));
  nc_type short_vlen = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(file.get(), "short_vlen", &short_vlen));

  nc_type nested_vlen = NC_NAT;
  ASSERT_EQ(NC_NOERR,
            nc_def_vlen(file.get(), "nested_vlen", int_vlen, &nested_vlen));

  char name[NC_MAX_NAME + 1]{};
  size_t datum_size = 0;
  nc_type base_type = NC_NAT;
  ASSERT_EQ(NC_NOERR,
            nc_inq_vlen(file.get(), int_vlen, name, &datum_size, &base_type));
  EXPECT_STREQ(name, "int_vlen");
  EXPECT_EQ(datum_size, sizeof(nc_vlen_t));
  EXPECT_EQ(base_type, NC_INT);

  std::memset(name, 0, sizeof(name));
  datum_size = 0;
  base_type = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_inq_vlen(file.get(), nested_vlen, name, &datum_size,
                                  &base_type));
  EXPECT_STREQ(name, "nested_vlen");
  EXPECT_EQ(datum_size, sizeof(nc_vlen_t));
  EXPECT_EQ(base_type, int_vlen);

  // Null optional out-parameters must be tolerated.
  EXPECT_EQ(NC_NOERR,
            nc_inq_vlen(file.get(), short_vlen, nullptr, nullptr, nullptr));
}

TEST(DvlenTest, InqVlenRejectsErrorsAndNonVlenUserTypes) {
  char name[NC_MAX_NAME + 1]{};
  size_t datum_size = 0;
  nc_type base_type = NC_NAT;
  EXPECT_EQ(NC_EBADID, nc_inq_vlen(-1, NC_INT, name, &datum_size, &base_type));

  ScopedNcFile classic;
  ASSERT_EQ(NC_NOERR, nc_create("dvlen_inq_classic.nc",
                                NC_CLOBBER | NC_DISKLESS, classic.ptr()));
  EXPECT_EQ(NC_ENOTNC4,
            nc_inq_vlen(classic.get(), NC_INT, name, &datum_size, &base_type));
  ASSERT_EQ(NC_NOERR, classic.close());

  ScopedNcFile nc4;
  ASSERT_EQ(NC_NOERR,
            nc_create("dvlen_inq_types.nc",
                      NC_CLOBBER | NC_NETCDF4 | NC_DISKLESS, nc4.ptr()));

  EXPECT_EQ(NC_EBADTYPE,
            nc_inq_vlen(nc4.get(), NC_NAT, name, &datum_size, &base_type));
  EXPECT_EQ(NC_EBADTYPE,
            nc_inq_vlen(nc4.get(), NC_INT, name, &datum_size, &base_type));
  EXPECT_EQ(NC_EBADTYPE,
            nc_inq_vlen(nc4.get(), 9999, name, &datum_size, &base_type));

  nc_type opaque_type = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_opaque(nc4.get(), 8, "opaque_t", &opaque_type));
  EXPECT_EQ(NC_EBADTYPE,
            nc_inq_vlen(nc4.get(), opaque_type, name, &datum_size, &base_type));

  nc_type enum_type = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_enum(nc4.get(), NC_INT, "enum_t", &enum_type));
  int enum_val = 1;
  ASSERT_EQ(NC_NOERR, nc_insert_enum(nc4.get(), enum_type, "kOne", &enum_val));
  EXPECT_EQ(NC_EBADTYPE,
            nc_inq_vlen(nc4.get(), enum_type, name, &datum_size, &base_type));

  nc_type compound_type = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_compound(nc4.get(), sizeof(int), "compound_t",
                                      &compound_type));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(nc4.get(), compound_type, "field", 0, NC_INT));
  EXPECT_EQ(NC_EBADTYPE, nc_inq_vlen(nc4.get(), compound_type, name,
                                     &datum_size, &base_type));
  ASSERT_EQ(NC_NOERR, nc4.close());
}

TEST(DvlenTest, PutAndGetVlenElementErrorsAndRoundTrip) {
  static_assert(sizeof(int) == 4,
                "NC4_get_vlen_element copies 4 bytes per element");

  nc_vlen_t element{};
  std::vector<int> input{10, 20, 30, -40};
  std::vector<int> output(input.size(), 0);
  size_t out_len = 0;

  EXPECT_EQ(NC_EBADID,
            nc_put_vlen_element(-1, 0, &element, input.size(), input.data()));
  EXPECT_EQ(NC_EBADID,
            nc_get_vlen_element(-1, 0, &element, &out_len, output.data()));

  ScopedNcFile classic;
  ASSERT_EQ(NC_NOERR, nc_create("dvlen_elem_classic.nc",
                                NC_CLOBBER | NC_DISKLESS, classic.ptr()));
  EXPECT_EQ(NC_ENOTNC4, nc_put_vlen_element(classic.get(), 0, &element,
                                            input.size(), input.data()));
  EXPECT_EQ(NC_ENOTNC4, nc_get_vlen_element(classic.get(), 0, &element,
                                            &out_len, output.data()));
  ASSERT_EQ(NC_NOERR, classic.close());

  ScopedNcFile nc4;
  ASSERT_EQ(NC_NOERR,
            nc_create("dvlen_elem_nc4.nc",
                      NC_CLOBBER | NC_NETCDF4 | NC_DISKLESS, nc4.ptr()));
  nc_type vlen_type = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_vlen(nc4.get(), "int_vlen", NC_INT, &vlen_type));

  // NC4_put_vlen_element borrows the caller's pointer without copying.
  ASSERT_EQ(NC_NOERR, nc_put_vlen_element(nc4.get(), vlen_type, &element,
                                          input.size(), input.data()));
  EXPECT_EQ(element.len, input.size());
  EXPECT_EQ(element.p, input.data());

  ASSERT_EQ(NC_NOERR, nc_get_vlen_element(nc4.get(), vlen_type, &element,
                                          &out_len, output.data()));
  EXPECT_EQ(out_len, input.size());
  EXPECT_THAT(output, ElementsAre(10, 20, 30, -40));
}

TEST(DvlenTest, VlenVariableWriteReadAndReclaimRoundTrip) {
  ScopedNcFile nc4;
  ASSERT_EQ(NC_NOERR,
            nc_create("dvlen_var_roundtrip.nc",
                      NC_CLOBBER | NC_NETCDF4 | NC_DISKLESS, nc4.ptr()));

  nc_type vlen_type = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_vlen(nc4.get(), "vlen_i32", NC_INT, &vlen_type));

  int dimid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc4.get(), "n", 2, &dimid));
  int varid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc4.get(), "rows", vlen_type, 1, &dimid, &varid));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc4.get()));

  std::vector<int> row0{1, 2, 3};
  std::vector<int> row1{42};
  nc_vlen_t write_buf[2]{
      {row0.size(), row0.data()},
      {row1.size(), row1.data()},
  };
  ASSERT_EQ(NC_NOERR, nc_put_var(nc4.get(), varid, write_buf));

  {
    ScopedNcVlens read_buf(2);
    ASSERT_EQ(NC_NOERR, nc_get_var(nc4.get(), varid, read_buf.data()));
    EXPECT_EQ(read_buf[0].len, 3U);
    ASSERT_NE(read_buf[0].p, nullptr);
    const int* r0 = static_cast<const int*>(read_buf[0].p);
    EXPECT_EQ(r0[0], 1);
    EXPECT_EQ(r0[1], 2);
    EXPECT_EQ(r0[2], 3);

    EXPECT_EQ(read_buf[1].len, 1U);
    ASSERT_NE(read_buf[1].p, nullptr);
    EXPECT_EQ(static_cast<const int*>(read_buf[1].p)[0], 42);
  }

  // Also validate nc_reclaim_data before closing ncid.
  nc_vlen_t reclaim_buf[2]{};
  ScopedVlenReclaim reclaim_guard(nc4.get(), vlen_type, reclaim_buf, 2);
  ASSERT_EQ(NC_NOERR, nc_get_var(nc4.get(), varid, reclaim_buf));
  EXPECT_EQ(reclaim_buf[0].len, 3U);
  EXPECT_EQ(reclaim_buf[1].len, 1U);
}

}  // namespace
}  // namespace netcdf
