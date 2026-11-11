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

// Unit tests for //third_party/netcdf/libdispatch/dopaque.c (opaque types).
//
// Covers nc_def_opaque and nc_inq_opaque validation, format restrictions,
// non-opaque user-type rejection, optional output parameters, and in-memory
// variable/attribute persistence round-trips.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/dopaque.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

namespace netcdf {
namespace {

using ::testing::ElementsAreArray;

TEST(DopaqueTest, RejectsInvalidNcid) {
  nc_type xtype = NC_NAT;
  char name[NC_MAX_NAME + 1]{};
  size_t size = 0;

  EXPECT_EQ(NC_EBADID, nc_def_opaque(-1, 8, "blob8", &xtype));
  EXPECT_EQ(NC_EBADID, nc_inq_opaque(-1, NC_FIRSTUSERTYPEID, name, &size));
}

TEST(DopaqueTest, RejectsClassicAndClassicModelFormats) {
  ScopedNcFile classic_nc;
  ASSERT_EQ(NC_NOERR, nc_create("dopaque_classic.nc", NC_DISKLESS | NC_CLOBBER,
                                classic_nc.ptr()));

  nc_type xtype = NC_NAT;
  char name[NC_MAX_NAME + 1]{};
  size_t size = 0;
  EXPECT_EQ(NC_ENOTNC4, nc_def_opaque(classic_nc, 8, "blob8", &xtype));
  EXPECT_EQ(NC_ENOTNC4,
            nc_inq_opaque(classic_nc, NC_FIRSTUSERTYPEID, name, &size));

  ScopedNcFile strict_nc;
  ASSERT_EQ(NC_NOERR,
            nc_create("dopaque_strict.nc",
                      NC_NETCDF4 | NC_CLASSIC_MODEL | NC_DISKLESS | NC_CLOBBER,
                      strict_nc.ptr()));
  EXPECT_EQ(NC_ESTRICTNC3, nc_def_opaque(strict_nc, 8, "blob8", &xtype));
  EXPECT_EQ(NC_EBADTYPE,
            nc_inq_opaque(strict_nc, NC_FIRSTUSERTYPEID, name, &size));
}

TEST(DopaqueTest, ValidatesSizeNamesAndTypeClass) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("dopaque_validate.nc", NC_NETCDF4, 0, nc.ptr()));

  nc_type xtype = NC_NAT;
  // Zero size is rejected.
  EXPECT_EQ(NC_EINVAL, nc_def_opaque(nc, 0, "zero_opaque", &xtype));

  // Invalid / too-long / duplicate names are rejected.
  EXPECT_EQ(NC_EBADNAME, nc_def_opaque(nc, 4, "bad/name", &xtype));
  const std::string too_long(NC_MAX_NAME + 1, 'a');
  EXPECT_EQ(NC_EMAXNAME, nc_def_opaque(nc, 4, too_long.c_str(), &xtype));

  // Valid creation and optional null xtypep.
  ASSERT_EQ(NC_NOERR, nc_def_opaque(nc, 4, "blob4", &xtype));
  EXPECT_EQ(NC_ENAMEINUSE, nc_def_opaque(nc, 8, "blob4", &xtype));
  ASSERT_EQ(NC_NOERR, nc_def_opaque(nc, 12, "blob12", nullptr));

  nc_type looked_up = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(nc, "blob12", &looked_up));

  char name[NC_MAX_NAME + 1]{};
  size_t size = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_opaque(nc, looked_up, name, &size));
  EXPECT_STREQ("blob12", name);
  EXPECT_EQ(12U, size);

  // Null optional out-parameters on nc_inq_opaque succeed.
  EXPECT_EQ(NC_NOERR, nc_inq_opaque(nc, xtype, nullptr, nullptr));

  // Invalid typeid / atomic type fails in nc_inq_user_type().
  EXPECT_EQ(NC_EBADTYPE, nc_inq_opaque(nc, NC_INT, name, &size));
  EXPECT_EQ(NC_EBADTYPE, nc_inq_opaque(nc, 99999, name, &size));

  // Non-opaque user type hits class != NC_OPAQUE check in nc_inq_opaque().
  nc_type enum_type = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_enum(nc, NC_INT, "color_enum", &enum_type));
  constexpr int kRed = 1;
  ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum_type, "RED", &kRed));
  EXPECT_EQ(NC_EBADTYPE, nc_inq_opaque(nc, enum_type, name, &size));

  nc_type cmpd_type = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_compound(nc, sizeof(int), "pair", &cmpd_type));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(nc, cmpd_type, "v", 0, NC_INT));
  EXPECT_EQ(NC_EBADTYPE, nc_inq_opaque(nc, cmpd_type, name, &size));
}

TEST(DopaqueTest, VariableAndAttributeRoundTripAndReadOnlyRejection) {
  constexpr size_t kBlobSize = 16;
  using Blob = std::array<uint8_t, kBlobSize>;
  constexpr std::array<Blob, 2> kInput = {{
      {0x00, 0x01, 0x02, 0x03, 0x10, 0x20, 0x30, 0x40, 0x55, 0x66, 0x77, 0x88,
       0x99, 0xaa, 0xbb, 0xff},
      {0xfe, 0xed, 0xfa, 0xce, 0xca, 0xfe, 0xba, 0xbe, 0x01, 0x23, 0x45, 0x67,
       0x89, 0xab, 0xcd, 0xef},
  }};

  ScopedMemio memio;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("dopaque_roundtrip.nc", NC_NETCDF4, 0, nc.ptr()));

    nc_type opaque_type = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_opaque(nc, kBlobSize, "digest16", &opaque_type));

    int dimid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "items", kInput.size(), &dimid));
    int varid = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "blobs", opaque_type, 1, &dimid, &varid));
    ASSERT_EQ(NC_NOERR, nc_put_att(nc, NC_GLOBAL, "signature", opaque_type, 1,
                                   kInput[0].data()));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));
    ASSERT_EQ(NC_NOERR, nc_put_var(nc, varid, kInput.data()));

    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
  }

  ScopedNcFile ro_nc;
  ASSERT_EQ(NC_NOERR, nc_open_mem("dopaque_ro.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, ro_nc.ptr()));

  // Defining a new opaque type on a read-only file returns NC_EPERM.
  nc_type dummy_type = NC_NAT;
  EXPECT_EQ(NC_EPERM, nc_def_opaque(ro_nc, 4, "forbidden", &dummy_type));

  nc_type reopened_type = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(ro_nc, "digest16", &reopened_type));

  char name[NC_MAX_NAME + 1]{};
  size_t size = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_opaque(ro_nc, reopened_type, name, &size));
  EXPECT_STREQ("digest16", name);
  EXPECT_EQ(kBlobSize, size);

  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc, "blobs", &varid));
  std::array<Blob, 2> output{};
  output[0].fill(0xA5);
  output[1].fill(0x5A);
  ASSERT_EQ(NC_NOERR, nc_get_var(ro_nc, varid, output.data()));
  EXPECT_THAT(output[0], ElementsAreArray(kInput[0]));
  EXPECT_THAT(output[1], ElementsAreArray(kInput[1]));

  Blob attr_out{};
  attr_out.fill(0x33);
  ASSERT_EQ(NC_NOERR,
            nc_get_att(ro_nc, NC_GLOBAL, "signature", attr_out.data()));
  EXPECT_THAT(attr_out, ElementsAreArray(kInput[0]));
}

}  // namespace
}  // namespace netcdf
