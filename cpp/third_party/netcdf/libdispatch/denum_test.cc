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

// Unit tests for //third_party/netcdf/libdispatch/denum.c (enum types).
//
// Covers nc_def_enum, nc_insert_enum, nc_inq_enum, nc_inq_enum_member, and
// nc_inq_enum_ident across integer base types, format restrictions, type-class
// mismatches, undefined member handling, committed/read-only errors, and
// in-memory persistence round-trips.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/denum.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

namespace netcdf {
namespace {

using ::testing::ElementsAre;

template <typename T>
T DecodeRaw(const std::array<unsigned char, 8>& raw) {
  T val{};
  std::memcpy(&val, raw.data(), sizeof(T));
  return val;
}

void ExpectMemberValueEq(nc_type base_type,
                         const std::array<unsigned char, 8>& raw,
                         long long expected_ll) {
  switch (base_type) {
    case NC_BYTE:
      EXPECT_EQ(static_cast<signed char>(expected_ll),
                DecodeRaw<signed char>(raw));
      break;
    case NC_UBYTE:
      EXPECT_EQ(static_cast<unsigned char>(expected_ll),
                DecodeRaw<unsigned char>(raw));
      break;
    case NC_SHORT:
      EXPECT_EQ(static_cast<short>(expected_ll), DecodeRaw<short>(raw));
      break;
    case NC_USHORT:
      EXPECT_EQ(static_cast<unsigned short>(expected_ll),
                DecodeRaw<unsigned short>(raw));
      break;
    case NC_INT:
      EXPECT_EQ(static_cast<int>(expected_ll), DecodeRaw<int>(raw));
      break;
    case NC_UINT:
      EXPECT_EQ(static_cast<unsigned int>(expected_ll),
                DecodeRaw<unsigned int>(raw));
      break;
    case NC_INT64:
      EXPECT_EQ(expected_ll, DecodeRaw<long long>(raw));
      break;
    case NC_UINT64:
      EXPECT_EQ(static_cast<unsigned long long>(expected_ll),
                DecodeRaw<unsigned long long>(raw));
      break;
    default:
      FAIL() << "Unexpected base type: " << base_type;
  }
}

TEST(DenumTest, RejectsInvalidNcid) {
  nc_type enum_type = NC_NAT;
  constexpr int kVal = 1;
  char name[NC_MAX_NAME + 1]{};
  nc_type base_type = NC_NAT;
  size_t base_size = 0;
  size_t num_members = 0;
  int out_val = 0;

  EXPECT_EQ(NC_EBADID, nc_def_enum(-1, NC_INT, "e", &enum_type));
  EXPECT_EQ(NC_EBADID, nc_insert_enum(-1, NC_FIRSTUSERTYPEID, "A", &kVal));
  EXPECT_EQ(NC_EBADID, nc_inq_enum(-1, NC_FIRSTUSERTYPEID, name, &base_type,
                                   &base_size, &num_members));
  EXPECT_EQ(NC_EBADID,
            nc_inq_enum_member(-1, NC_FIRSTUSERTYPEID, 0, name, &out_val));
  EXPECT_EQ(NC_EBADID, nc_inq_enum_ident(-1, NC_FIRSTUSERTYPEID, kVal, name));
}

TEST(DenumTest, RejectsClassicAndClassicModelFormats) {
  ScopedNcFile classic_nc;
  ASSERT_EQ(NC_NOERR, nc_create("denum_classic.nc", NC_DISKLESS | NC_CLOBBER,
                                classic_nc.ptr()));

  nc_type enum_type = NC_NAT;
  constexpr int kVal = 1;
  char name[NC_MAX_NAME + 1]{};
  nc_type base_type = NC_NAT;
  size_t base_size = 0;
  size_t num_members = 0;
  int out_val = 0;

  EXPECT_EQ(NC_ENOTNC4, nc_def_enum(classic_nc, NC_INT, "e", &enum_type));
  EXPECT_EQ(NC_ENOTNC4,
            nc_insert_enum(classic_nc, NC_FIRSTUSERTYPEID, "A", &kVal));
  EXPECT_EQ(NC_ENOTNC4, nc_inq_enum(classic_nc, NC_FIRSTUSERTYPEID, name,
                                    &base_type, &base_size, &num_members));
  EXPECT_EQ(NC_ENOTNC4, nc_inq_enum_member(classic_nc, NC_FIRSTUSERTYPEID, 0,
                                           name, &out_val));
  EXPECT_EQ(NC_ENOTNC4,
            nc_inq_enum_ident(classic_nc, NC_FIRSTUSERTYPEID, kVal, name));

  ScopedNcFile strict_nc;
  ASSERT_EQ(NC_NOERR,
            nc_create("denum_strict.nc",
                      NC_NETCDF4 | NC_CLASSIC_MODEL | NC_DISKLESS | NC_CLOBBER,
                      strict_nc.ptr()));
  EXPECT_EQ(NC_ESTRICTNC3, nc_def_enum(strict_nc, NC_INT, "e", &enum_type));
  EXPECT_EQ(NC_EBADTYPE, nc_inq_enum(strict_nc, NC_FIRSTUSERTYPEID, name,
                                     &base_type, &base_size, &num_members));
}

TEST(DenumTest, ValidatesNamesTypesMembersAndUndefinedZero) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("denum_validate.nc", NC_NETCDF4, 0, nc.ptr()));

  nc_type enum_type = NC_NAT;
  EXPECT_EQ(NC_EBADTYPE, nc_def_enum(nc, NC_NAT, "bad_base_nat", &enum_type));
  EXPECT_EQ(NC_EBADTYPID, nc_def_enum(nc, 99999, "bad_base_id", &enum_type));
  EXPECT_EQ(NC_EBADNAME, nc_def_enum(nc, NC_INT, "bad/enum", &enum_type));
  const std::string too_long(NC_MAX_NAME + 1, 'e');
  EXPECT_EQ(NC_EMAXNAME, nc_def_enum(nc, NC_INT, too_long.c_str(), &enum_type));

  // Optional null typeidp works and can be looked up by name.
  ASSERT_EQ(NC_NOERR, nc_def_enum(nc, NC_INT, "status_enum", nullptr));
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(nc, "status_enum", &enum_type));
  EXPECT_EQ(NC_ENAMEINUSE, nc_def_enum(nc, NC_INT, "status_enum", &enum_type));

  constexpr int kOk = 10;
  constexpr int kFail = -2;
  EXPECT_EQ(NC_EBADNAME, nc_insert_enum(nc, enum_type, "bad/member", &kOk));
  EXPECT_EQ(NC_EBADTYPE, nc_insert_enum(nc, NC_INT, "ATOMIC", &kOk));
  EXPECT_EQ(NC_EBADTYPID, nc_insert_enum(nc, 99999, "MISSING_TID", &kOk));

  ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum_type, "OK", &kOk));
  ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum_type, "FAIL", &kFail));

  // Non-enum user types fail consistently across enum inquiry/insert APIs.
  nc_type opaque_type = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_opaque(nc, 4, "blob4", &opaque_type));
  EXPECT_EQ(NC_EBADTYPE, nc_insert_enum(nc, opaque_type, "X", &kOk));

  char name[NC_MAX_NAME + 1]{};
  nc_type base_type = NC_NAT;
  size_t base_size = 0;
  size_t num_members = 0;
  int out_val = 0;

  EXPECT_EQ(NC_EBADTYPE, nc_inq_enum(nc, NC_INT, name, &base_type, &base_size,
                                     &num_members));
  EXPECT_EQ(NC_EBADTYPE,
            nc_inq_enum(nc, 99999, name, &base_type, &base_size, &num_members));
  EXPECT_EQ(NC_EBADTYPE, nc_inq_enum(nc, opaque_type, name, &base_type,
                                     &base_size, &num_members));

  EXPECT_EQ(NC_EBADTYPE,
            nc_inq_enum_member(nc, opaque_type, 0, name, &out_val));
  EXPECT_EQ(NC_EBADTYPE, nc_inq_enum_ident(nc, opaque_type, kOk, name));

  ASSERT_EQ(NC_NOERR, nc_inq_enum(nc, enum_type, name, &base_type, &base_size,
                                  &num_members));
  EXPECT_STREQ("status_enum", name);
  EXPECT_EQ(NC_INT, base_type);
  EXPECT_EQ(sizeof(int), base_size);
  EXPECT_EQ(2U, num_members);
  EXPECT_EQ(NC_NOERR,
            nc_inq_enum(nc, enum_type, nullptr, nullptr, nullptr, nullptr));

  // Out-of-range member indices fail with NC_EINVAL.
  EXPECT_EQ(NC_EINVAL, nc_inq_enum_member(nc, enum_type, -1, name, &out_val));
  EXPECT_EQ(NC_EINVAL, nc_inq_enum_member(nc, enum_type, 2, name, &out_val));

  // Valid member inquiries, including null optional out-parameters.
  ASSERT_EQ(NC_NOERR, nc_inq_enum_member(nc, enum_type, 0, name, &out_val));
  EXPECT_STREQ("OK", name);
  EXPECT_EQ(kOk, out_val);
  ASSERT_EQ(NC_NOERR, nc_inq_enum_member(nc, enum_type, 1, name, &out_val));
  EXPECT_STREQ("FAIL", name);
  EXPECT_EQ(kFail, out_val);
  EXPECT_EQ(NC_NOERR, nc_inq_enum_member(nc, enum_type, 0, nullptr, nullptr));

  // Matching identifiers succeed (including identifier == nullptr).
  std::memset(name, 0, sizeof(name));
  ASSERT_EQ(NC_NOERR, nc_inq_enum_ident(nc, enum_type, kFail, name));
  EXPECT_STREQ("FAIL", name);
  EXPECT_EQ(NC_NOERR, nc_inq_enum_ident(nc, enum_type, kOk, nullptr));

  // Missing zero defaults to NC_UNDEFINED_ENUM_IDENT ("_UNDEFINED");
  // missing non-zero values return NC_EINVAL.
  std::memset(name, 0, sizeof(name));
  ASSERT_EQ(NC_NOERR, nc_inq_enum_ident(nc, enum_type, 0, name));
  EXPECT_STREQ(NC_UNDEFINED_ENUM_IDENT, name);
  EXPECT_EQ(NC_EINVAL, nc_inq_enum_ident(nc, enum_type, 999, name));

  // Committing the file locks the enum against additional insertions.
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));
  constexpr int kLate = 3;
  EXPECT_EQ(NC_ETYPDEFINED, nc_insert_enum(nc, enum_type, "LATE", &kLate));
}

TEST(DenumTest, SupportsAllIntegerBaseTypesAndPersistsAcrossReopen) {
  struct BaseTypeProbe {
    const char* name;
    nc_type base_type;
    size_t expected_size;
    long long expected_ll;
  };
  constexpr BaseTypeProbe kProbes[] = {
      // Upstream NC4_inq_enum_ident() reads NC_BYTE via *(char *), which
      // treats bytes >= 128 as positive under -funsigned-char, so probe
      // nc_inq_enum_ident() with a non-negative NC_BYTE value here and verify
      // negative NC_BYTE member persistence via nc_inq_enum_member().
      {"e_byte", NC_BYTE, sizeof(signed char), 5LL},
      {"e_ubyte", NC_UBYTE, sizeof(unsigned char), 250LL},
      {"e_short", NC_SHORT, sizeof(short), -300LL},
      {"e_ushort", NC_USHORT, sizeof(unsigned short), 65000LL},
      {"e_int", NC_INT, sizeof(int), -100000LL},
      {"e_uint", NC_UINT, sizeof(unsigned int), 3000000000LL},
      {"e_int64", NC_INT64, sizeof(long long), -9000000000000LL},
      {"e_uint64", NC_UINT64, sizeof(unsigned long long),
       9000000000000000000LL},
  };

  ScopedMemio memio;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("denum_basetypes.nc", NC_NETCDF4, 0, nc.ptr()));

    for (const BaseTypeProbe& probe : kProbes) {
      SCOPED_TRACE(probe.name);
      nc_type tid = NC_NAT;
      ASSERT_EQ(NC_NOERR, nc_def_enum(nc, probe.base_type, probe.name, &tid));
      switch (probe.base_type) {
        case NC_BYTE: {
          const signed char v = static_cast<signed char>(probe.expected_ll);
          ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, tid, "VAL", &v));
          break;
        }
        case NC_UBYTE: {
          const unsigned char v = static_cast<unsigned char>(probe.expected_ll);
          ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, tid, "VAL", &v));
          break;
        }
        case NC_SHORT: {
          const short v = static_cast<short>(probe.expected_ll);
          ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, tid, "VAL", &v));
          break;
        }
        case NC_USHORT: {
          const unsigned short v =
              static_cast<unsigned short>(probe.expected_ll);
          ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, tid, "VAL", &v));
          break;
        }
        case NC_INT: {
          const int v = static_cast<int>(probe.expected_ll);
          ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, tid, "VAL", &v));
          break;
        }
        case NC_UINT: {
          const unsigned int v = static_cast<unsigned int>(probe.expected_ll);
          ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, tid, "VAL", &v));
          break;
        }
        case NC_INT64: {
          const long long v = probe.expected_ll;
          ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, tid, "VAL", &v));
          break;
        }
        case NC_UINT64: {
          const unsigned long long v =
              static_cast<unsigned long long>(probe.expected_ll);
          ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, tid, "VAL", &v));
          break;
        }
        default:
          FAIL() << "Unexpected base type";
      }
    }

    // Also verify negative signed-byte enum members survive persistence.
    nc_type byte_enum = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_inq_typeid(nc, "e_byte", &byte_enum));
    constexpr signed char kNegByte = -5;
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, byte_enum, "NEG", &kNegByte));

    // Write a variable and global attribute using the signed 16-bit enum.
    nc_type short_enum = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_inq_typeid(nc, "e_short", &short_enum));
    constexpr int16_t kZero = 0;
    constexpr int16_t kPos = 42;
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, short_enum, "ZERO", &kZero));
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, short_enum, "POS", &kPos));

    int dimid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "n", 3, &dimid));
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "flags", short_enum, 1, &dimid, &varid));
    constexpr std::array<int16_t, 3> kVarData = {-300, 0, 42};
    ASSERT_EQ(NC_NOERR,
              nc_put_att(nc, NC_GLOBAL, "mode", short_enum, 1, &kPos));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));
    ASSERT_EQ(NC_NOERR, nc_put_var(nc, varid, kVarData.data()));

    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
  }

  ScopedNcFile ro_nc;
  ASSERT_EQ(NC_NOERR, nc_open_mem("denum_ro.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, ro_nc.ptr()));

  nc_type dummy_tid = NC_NAT;
  EXPECT_EQ(NC_EPERM, nc_def_enum(ro_nc, NC_INT, "forbidden", &dummy_tid));

  for (const BaseTypeProbe& probe : kProbes) {
    SCOPED_TRACE(probe.name);
    nc_type tid = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_inq_typeid(ro_nc, probe.name, &tid));

    char qname[NC_MAX_NAME + 1]{};
    nc_type base_type = NC_NAT;
    size_t base_size = 0;
    size_t nmembers = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_enum(ro_nc, tid, qname, &base_type, &base_size,
                                    &nmembers));
    EXPECT_STREQ(probe.name, qname);
    EXPECT_EQ(probe.base_type, base_type);
    EXPECT_EQ(probe.expected_size, base_size);
    EXPECT_GE(nmembers, 1U);

    char ident[NC_MAX_NAME + 1]{};
    ASSERT_EQ(NC_NOERR,
              nc_inq_enum_ident(ro_nc, tid, probe.expected_ll, ident));
    EXPECT_STREQ("VAL", ident);

    std::array<unsigned char, 8> raw_val{};
    char mname[NC_MAX_NAME + 1]{};
    ASSERT_EQ(NC_NOERR,
              nc_inq_enum_member(ro_nc, tid, 0, mname, raw_val.data()));
    EXPECT_STREQ("VAL", mname);
    ExpectMemberValueEq(probe.base_type, raw_val, probe.expected_ll);
  }

  nc_type byte_enum = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(ro_nc, "e_byte", &byte_enum));
  char neg_name[NC_MAX_NAME + 1]{};
  signed char neg_val = 0;
  ASSERT_EQ(NC_NOERR,
            nc_inq_enum_member(ro_nc, byte_enum, 1, neg_name, &neg_val));
  EXPECT_STREQ("NEG", neg_name);
  EXPECT_EQ(static_cast<signed char>(-5), neg_val);

  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(ro_nc, "flags", &varid));
  std::vector<int16_t> out(3, -999);
  ASSERT_EQ(NC_NOERR, nc_get_var(ro_nc, varid, out.data()));
  EXPECT_THAT(out, ElementsAre(-300, 0, 42));

  int16_t att_out = -999;
  ASSERT_EQ(NC_NOERR, nc_get_att(ro_nc, NC_GLOBAL, "mode", &att_out));
  EXPECT_EQ(42, att_out);
}

}  // namespace
}  // namespace netcdf
