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

// Unit tests for //third_party/netcdf/ncdump/nc4printer.c.
//
// Exercises NC4print XML serialization across NetCDF-4 groups, dimensions,
// user-defined types, XML entity escaping, error paths, and variable
// attributes. Attribute-printing subtests run inside forked child processes
// because upstream readAttributeValues() intentionally leaks its temporary
// buffer. Note that global attributes, compound fields, vlen types, and
// subgroup-owned type/dimension FQN resolution trigger upstream crashes/aborts
// in nc4printer.c and are avoided here.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncdump/nc4printer.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/ncdump_globals_stub.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_dispatch.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

// Upstream nc4printer.c declares an uninitialized `union NUMVALUE numvalue` and
// passes it by value into `getNumericValue()` after `nc_inq_enum_member()`
// copies only `type->size` (< 8) bytes for narrow enums, tripping MSan's
// -fsanitize-memory-param-retval check. Keep wide 64-bit enums active
// everywhere and exercise narrow enums when MSan is disabled.
#if defined(MEMORY_SANITIZER)
#define NETCDF_NC4PRINTER_TEST_MSAN 1
#elif defined(__has_feature)
#if __has_feature(memory_sanitizer)
#define NETCDF_NC4PRINTER_TEST_MSAN 1
#endif
#endif

namespace netcdf {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;

TEST(Nc4PrinterTest, RejectsNullBufferAndInvalidNcid) {
  EXPECT_EQ(NC_EINVAL, NC4print(nullptr, 0));

  NCbytes* buf = ncbytesnew();
  ASSERT_NE(nullptr, buf);
  absl::Cleanup free_buf = [&] { ncbytesfree(buf); };

  EXPECT_EQ(NC_EBADID, NC4print(buf, -1));
}

TEST(Nc4PrinterTest, PrintsEmptyNetcdf4AndClassicFiles) {
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create_mem("empty4.nc", NC_NETCDF4, 4096, nc.ptr()));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    NCbytes* buf = ncbytesnew();
    ASSERT_NE(nullptr, buf);
    absl::Cleanup free_buf = [&] { ncbytesfree(buf); };

    EXPECT_EQ(NC_NOERR, NC4print(buf, nc));
    const std::string xml(ncbytescontents(buf));
    EXPECT_THAT(xml, HasSubstr("<Group name=\"/\">\n"));
    EXPECT_THAT(xml, HasSubstr("</Group>"));
  }

  {
    ScopedMemio memio;
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR,
                nc_create_mem("classic3.nc", NC_CLASSIC_MODEL, 4096, nc.ptr()));
      int dimid = -1;
      int varid = -1;
      ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "x", 2, &dimid));
      ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v", NC_INT, 1, &dimid, &varid));
      ASSERT_EQ(NC_NOERR, nc_enddef(nc));
      constexpr int kVals[2] = {10, 20};
      ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, varid, kVals));
      ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
    }

    ScopedNcFile reopened;
    ASSERT_EQ(NC_NOERR, nc_open_mem("classic3.nc", NC_NOWRITE, memio.size(),
                                    memio.get()->memory, reopened.ptr()));

    NCbytes* buf = ncbytesnew();
    ASSERT_NE(nullptr, buf);
    absl::Cleanup free_buf = [&] { ncbytesfree(buf); };

    EXPECT_EQ(NC_NOERR, NC4print(buf, reopened));
    const std::string xml(ncbytescontents(buf));
    EXPECT_THAT(xml, HasSubstr("<Dimension name=\"x\" size=\"2\"/>"));
    EXPECT_THAT(xml, HasSubstr("<Var name=\"v\" type=\"/Int\" rank=\"1\">"));
  }
}

TEST(Nc4PrinterTest, PrintsGroupsDimsTypesVarsAndEntityEscapes) {
  ScopedMemio memio;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("hierarchy.nc", NC_NETCDF4, 8192, nc.ptr()));

    int dim_x = -1;
    int dim_unlim = -1;
    int dim_esc = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "x", 3, &dim_x));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "unlim", NC_UNLIMITED, &dim_unlim));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "d&<>\"'q", 2, &dim_esc));

    nc_type opaque_tid = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_opaque(nc, 4, "op&<>\"'q", &opaque_tid));

    nc_type enum64_tid = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_enum(nc, NC_INT64, "e64", &enum64_tid));
    constexpr long long kM0 = 0;
    constexpr long long kM1 = -5;
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum64_tid, "M0", &kM0));
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum64_tid, "M&<>\"'x", &kM1));

    nc_type enum_u64_tid = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_enum(nc, NC_UINT64, "eu64", &enum_u64_tid));
    constexpr unsigned long long kU1 = 99ULL;
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum_u64_tid, "U1", &kU1));

#ifndef NETCDF_NC4PRINTER_TEST_MSAN
    nc_type enum_byte_tid = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_enum(nc, NC_BYTE, "eb", &enum_byte_tid));
    constexpr signed char kB1 = 7;
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum_byte_tid, "B1", &kB1));

    nc_type enum_short_tid = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_enum(nc, NC_SHORT, "es", &enum_short_tid));
    constexpr short kS1 = 300;
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum_short_tid, "S1", &kS1));

    nc_type enum_int_tid = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_enum(nc, NC_INT, "ei", &enum_int_tid));
    constexpr int kI1 = 42;
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum_int_tid, "I1", &kI1));

    nc_type enum_ubyte_tid = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_enum(nc, NC_UBYTE, "eub", &enum_ubyte_tid));
    constexpr unsigned char kUb1 = 11;
    ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum_ubyte_tid, "UB1", &kUb1));
#endif

    int v_scalar = -1;
    int v_arr = -1;
    int v_op = -1;
    int v_unlim = -1;
    const int arr_dims[2] = {dim_x, dim_esc};
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "scalar_i", NC_INT, 0, nullptr, &v_scalar));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "arr_f", NC_FLOAT, 2, arr_dims, &v_arr));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "op_v", opaque_tid, 1, &dim_x, &v_op));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(nc, "e_rec", enum64_tid, 1, &dim_unlim, &v_unlim));

    // Nested subgroups test recursive Group/indent printing while keeping all
    // variable-referenced dims and user types in root to avoid upstream's
    // fqnWalk NULL->id dereference on non-root parent paths.
    int sub_gid = -1;
    int grand_gid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(nc, "sub_g", &sub_gid));
    int sub_unused_dim = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(sub_gid, "sub_dim", 4, &sub_unused_dim));
    int sub_scalar = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(sub_gid, "sub_d", NC_DOUBLE, 0, nullptr, &sub_scalar));
    int sub_rootdim_var = -1;
    ASSERT_EQ(NC_NOERR, nc_def_var(sub_gid, "sub_b", NC_BYTE, 1, &dim_x,
                                   &sub_rootdim_var));

    ASSERT_EQ(NC_NOERR, nc_def_grp(sub_gid, "grand_g", &grand_gid));
    int grand_var = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(grand_gid, "gv", NC_STRING, 0, nullptr, &grand_var));

    ASSERT_EQ(NC_NOERR, nc_enddef(nc));

    constexpr size_t kStart[1] = {0};
    constexpr size_t kCount[1] = {2};
    constexpr long long kRecVals[2] = {kM0, kM1};
    ASSERT_EQ(NC_NOERR, nc_put_vara(nc, v_unlim, kStart, kCount, kRecVals));

    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
  }

  ScopedNcFile reopened;
  ASSERT_EQ(NC_NOERR, nc_open_mem("hierarchy.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, reopened.ptr()));

  NCbytes* buf = ncbytesnew();
  ASSERT_NE(nullptr, buf);
  absl::Cleanup free_buf = [&] { ncbytesfree(buf); };

  ASSERT_EQ(NC_NOERR, NC4print(buf, reopened));
  const std::string xml(ncbytescontents(buf));

  EXPECT_THAT(xml, HasSubstr("<Dimension name=\"x\" size=\"3\"/>"));
  EXPECT_THAT(xml, HasSubstr("<Dimension name=\"unlim\" size=\"2\"/>"));
  EXPECT_THAT(
      xml, HasSubstr(
               "<Dimension name=\"d&amp;&lt;&gt;&quot;&apos;q\" size=\"2\"/>"));
  EXPECT_THAT(
      xml,
      HasSubstr("<Opaque name=\"op&amp;&lt;&gt;&quot;&apos;q\" size=\"4\"/>"));
  EXPECT_THAT(xml, HasSubstr("<Enumeration name=\"e64\">"));
  EXPECT_THAT(xml, HasSubstr("<EnumConst name=\"M0\" value=\"0\"/>"));
  EXPECT_THAT(
      xml,
      HasSubstr(
          "<EnumConst name=\"M&amp;&lt;&gt;&quot;&apos;x\" value=\"-5\"/>"));
  EXPECT_THAT(xml, HasSubstr("<Enumeration name=\"eu64\">"));
  EXPECT_THAT(xml, HasSubstr("<EnumConst name=\"U1\" value=\"99\"/>"));
#ifndef NETCDF_NC4PRINTER_TEST_MSAN
  EXPECT_THAT(xml, HasSubstr("<EnumConst name=\"B1\" value=\"7\"/>"));
  EXPECT_THAT(xml, HasSubstr("<EnumConst name=\"S1\" value=\"300\"/>"));
  EXPECT_THAT(xml, HasSubstr("<EnumConst name=\"I1\" value=\"42\"/>"));
  EXPECT_THAT(xml, HasSubstr("<EnumConst name=\"UB1\" value=\"-1\"/>"));
#endif
  EXPECT_THAT(xml, HasSubstr("<Var name=\"scalar_i\" type=\"/Int\"/>"));
  EXPECT_THAT(xml,
              HasSubstr("<Var name=\"arr_f\" type=\"/Float\" rank=\"2\">"));
  EXPECT_THAT(xml, HasSubstr("<Dim name=\"/d&amp;&lt;&gt;&quot;&apos;q\"/>"));
  EXPECT_THAT(xml,
              HasSubstr("<Var name=\"op_v\" "
                        "type=\"/op&amp;&lt;&gt;&quot;&apos;q\" rank=\"1\">"));
  EXPECT_THAT(xml, HasSubstr(R"(<Var name="e_rec" type="/e64" rank="1">)"));
  EXPECT_THAT(xml, HasSubstr(R"(<Dim name="/unlim"/>)"));
  EXPECT_THAT(xml, HasSubstr("<Group name=\"sub_g\">"));
  EXPECT_THAT(xml, HasSubstr("<Dimension name=\"sub_dim\" size=\"4\"/>"));
  EXPECT_THAT(xml, HasSubstr("<Var name=\"sub_d\" type=\"/Double\"/>"));
  EXPECT_THAT(xml, HasSubstr(R"(<Var name="sub_b" type="/Byte" rank="1">)"));
  EXPECT_THAT(xml, HasSubstr(R"(<Dim name="/x"/>)"));
  EXPECT_THAT(xml, HasSubstr("<Group name=\"grand_g\">"));
  EXPECT_THAT(xml, HasSubstr("<Var name=\"gv\" type=\"/String\"/>"));
}

TEST(Nc4PrinterTest, PrintsZeroFieldCompoundInDefineMode) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("compound_def.nc", NC_NETCDF4, 4096, nc.ptr()));
  // End initial define mode and re-enter define mode via nc_redef() so
  // NC4_abort() treats this as a redef rather than calling remove() on the
  // virtual in-memory path (which fails with NC_ECANTREMOVE).
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));
  ASSERT_EQ(NC_NOERR, nc_redef(nc));
  nc_type cmp_tid = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_compound(nc, sizeof(int), "empty_cmp", &cmp_tid));

  NCbytes* buf = ncbytesnew();
  ASSERT_NE(nullptr, buf);
  absl::Cleanup free_buf = [&] { ncbytesfree(buf); };

  EXPECT_EQ(NC_NOERR, NC4print(buf, nc));
  const std::string xml(ncbytescontents(buf));
  EXPECT_THAT(xml, HasSubstr("<Compound name=\"empty_cmp\">\n"));
  EXPECT_THAT(xml, HasSubstr("  </Compound>"));

  // HDF5 rejects committing a 0-field compound on nc_close, so abort cleanly.
  EXPECT_EQ(NC_NOERR, nc.abort());
}

TEST(Nc4PrinterTest, PrintsAllAtomicEnumAndOpaqueVariableAttributes) {
  constexpr std::string_view kArgs[] = {"nc4printer_attrs"};
  const CliResult res = RunCliInForkedChild(
      [](int /*argc*/, char** /*argv*/) -> int {
        ScopedMemio memio;
        {
          ScopedNcFile nc;
          if (nc_create_mem("attrs.nc", NC_NETCDF4, 8192, nc.ptr()) !=
              NC_NOERR) {
            return 1;
          }

          nc_type enum_tid = NC_NAT;
          if (nc_def_enum(nc, NC_INT64, "att_enum", &enum_tid) != NC_NOERR) {
            return 2;
          }
          constexpr long long kEnumVal = 123;
          if (nc_insert_enum(nc, enum_tid, "VAL_123", &kEnumVal) != NC_NOERR) {
            return 3;
          }

          nc_type opaque_tid = NC_NAT;
          if (nc_def_opaque(nc, sizeof(void*), "att_opaque", &opaque_tid) !=
              NC_NOERR) {
            return 4;
          }

          int varid = -1;
          if (nc_def_var(nc, "holder", NC_INT, 0, nullptr, &varid) !=
              NC_NOERR) {
            return 5;
          }

          constexpr char kChars[] = "a\\\"z";
          if (nc_put_att_text(nc, varid, "a_char", 4, kChars) != NC_NOERR) {
            return 6;
          }
          constexpr signed char kBytes[2] = {42, 43};
          if (nc_put_att_schar(nc, varid, "a_byte", NC_BYTE, 2, kBytes) !=
              NC_NOERR) {
            return 7;
          }
          constexpr unsigned char kUbyte = 200;
          if (nc_put_att_uchar(nc, varid, "a_ubyte", NC_UBYTE, 1, &kUbyte) !=
              NC_NOERR) {
            return 8;
          }
          constexpr short kShorts[2] = {-123, 456};
          if (nc_put_att_short(nc, varid, "a_short", NC_SHORT, 2, kShorts) !=
              NC_NOERR) {
            return 9;
          }
          constexpr unsigned short kUshort = 50000;
          if (nc_put_att_ushort(nc, varid, "a_ushort", NC_USHORT, 1,
                                &kUshort) != NC_NOERR) {
            return 10;
          }
          constexpr int kInt = -100000;
          if (nc_put_att_int(nc, varid, "a_int", NC_INT, 1, &kInt) !=
              NC_NOERR) {
            return 11;
          }
          constexpr unsigned int kUint = 3000000000U;
          if (nc_put_att_uint(nc, varid, "a_uint", NC_UINT, 1, &kUint) !=
              NC_NOERR) {
            return 12;
          }
          constexpr long long kI64 = -9000000000LL;
          if (nc_put_att_longlong(nc, varid, "a_i64", NC_INT64, 1, &kI64) !=
              NC_NOERR) {
            return 13;
          }
          constexpr unsigned long long kU64 = 9000000000ULL;
          if (nc_put_att_ulonglong(nc, varid, "a_u64", NC_UINT64, 1, &kU64) !=
              NC_NOERR) {
            return 14;
          }
          constexpr float kFlt = 1.5F;
          if (nc_put_att_float(nc, varid, "a_flt", NC_FLOAT, 1, &kFlt) !=
              NC_NOERR) {
            return 15;
          }
          constexpr double kDbl = 2.25;
          if (nc_put_att_double(nc, varid, "a_dbl", NC_DOUBLE, 1, &kDbl) !=
              NC_NOERR) {
            return 16;
          }
          const char* kStrs[2] = {"plain", "esc\\q\"end"};
          if (nc_put_att_string(nc, varid, "a_str", 2, kStrs) != NC_NOERR) {
            return 17;
          }
          if (nc_put_att(nc, varid, "a_enum", enum_tid, 1, &kEnumVal) !=
              NC_NOERR) {
            return 18;
          }
          // Upstream getPrintValue() switches on basetype->id rather than
          // basetype->usertype.kind, so user-defined opaque types (id >=
          // NC_FIRSTUSERTYPEID) fall into default: after ncbytesclear() without
          // resetting content[0], preserving the prior tmp2 value ("123").
          const std::array<unsigned char, sizeof(void*)> kZeroOpaque{};
          if (nc_put_att(nc, varid, "a_op", opaque_tid, 1,
                         kZeroOpaque.data()) != NC_NOERR) {
            return 19;
          }

          if (nc_enddef(nc) != NC_NOERR) {
            return 20;
          }
          if (nc_close_memio(nc.release(), memio.get()) != NC_NOERR) {
            return 21;
          }
        }

        ScopedNcFile reopened;
        if (nc_open_mem("attrs.nc", NC_NOWRITE, memio.size(),
                        memio.get()->memory, reopened.ptr()) != NC_NOERR) {
          return 22;
        }

        NCbytes* buf = ncbytesnew();
        if (buf == nullptr) {
          return 23;
        }
        absl::Cleanup free_buf = [&] { ncbytesfree(buf); };

        const int status = NC4print(buf, reopened);
        if (status != NC_NOERR) {
          std::fprintf(stderr, "NC4print failed: %s\n", nc_strerror(status));
          return 24;
        }
        std::fputs(ncbytescontents(buf), stdout);
        return 0;
      },
      kArgs);

  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;

  EXPECT_THAT(res.stdout_output, HasSubstr("<Attribute name=\"a_char\">"));
  EXPECT_THAT(res.stdout_output, HasSubstr(R"(<Value value="'a'"/>)"));
  EXPECT_THAT(res.stdout_output, HasSubstr(R"(<Value value="'\\'"/>)"));
  EXPECT_THAT(res.stdout_output, HasSubstr(R"(<Value value="'\"'"/>)"));
  EXPECT_THAT(res.stdout_output, HasSubstr("<Attribute name=\"a_byte\">"));
  EXPECT_THAT(res.stdout_output, HasSubstr("<Value value=\"42\"/>"));
  EXPECT_THAT(res.stdout_output, HasSubstr("<Value value=\"43\"/>"));
  EXPECT_THAT(res.stdout_output, HasSubstr("<Value value=\"200\"/>"));
  EXPECT_THAT(res.stdout_output, HasSubstr("<Value value=\"-123\"/>"));
  EXPECT_THAT(res.stdout_output, HasSubstr("<Value value=\"456\"/>"));
  EXPECT_THAT(res.stdout_output, HasSubstr("<Value value=\"50000\"/>"));
  EXPECT_THAT(res.stdout_output, HasSubstr("<Value value=\"-100000\"/>"));
  EXPECT_THAT(res.stdout_output, HasSubstr("<Value value=\"3000000000\"/>"));
  EXPECT_THAT(res.stdout_output, HasSubstr("<Value value=\"-9000000000\"/>"));
  EXPECT_THAT(res.stdout_output, HasSubstr("<Value value=\"9000000000\"/>"));
  EXPECT_THAT(res.stdout_output, HasSubstr("<Value value=\"1.5\"/>"));
  EXPECT_THAT(res.stdout_output, HasSubstr("<Value value=\"2.25\"/>"));
  EXPECT_THAT(res.stdout_output, HasSubstr("<Attribute name=\"a_str\">"));
  EXPECT_THAT(res.stdout_output, HasSubstr("<Value value=\"plain\"/>"));
  EXPECT_THAT(res.stdout_output,
              HasSubstr(R"(<Value value="esc\\\\q\\\"end"/>)"));
  EXPECT_THAT(res.stdout_output, HasSubstr("<Attribute name=\"a_enum\">"));
  EXPECT_THAT(res.stdout_output, HasSubstr("<Value value=\"123\"/>"));
  EXPECT_THAT(res.stdout_output, HasSubstr("<Attribute name=\"a_op\">\n"
                                           "        <Value value=\"123\"/>\n"
                                           "      </Attribute>"));
}

// Scenarios backed by an isolated NC_UDF9 dispatch table inside a forked child
// to reach nc4printer.c branches unreachable via HDF5 user types (type ids
// equal to NC_OPAQUE / NC_ENUM and early inquiry errors).
enum class UdfScenario {
  kOpaqueAndEnumAliasAttrs,
  kGlobalAttrNameError,
  kCompoundFieldError,
};

UdfScenario g_udf_scenario = UdfScenario::kOpaqueAndEnumAliasAttrs;

int UdfCreate(const char* /*path*/, int /*cmode*/, size_t /*initialsz*/,
              int /*basepe*/, size_t* /*chunksizehintp*/, void* /*parameters*/,
              const NC_Dispatch* /*table*/, int /*ncid*/) {
  return NC_NOERR;
}

int UdfAbort(int /*ncid*/) { return NC_NOERR; }

int UdfClose(int /*ncid*/, void* /*params*/) { return NC_NOERR; }

int UdfInqFormat(int /*ncid*/, int* formatp) {
  if (formatp != nullptr) {
    *formatp = NC_FORMAT_NETCDF4;
  }
  return NC_NOERR;
}

int UdfInqFormatExtended(int /*ncid*/, int* formatp, int* modep) {
  if (formatp != nullptr) {
    *formatp = NC_FORMATX_UDF9;
  }
  if (modep != nullptr) {
    *modep = NC_UDF9;
  }
  return NC_NOERR;
}

int UdfInqType(int /*ncid*/, nc_type /*xtype*/, char* /*name*/,
               size_t* /*size*/) {
  return NC_EBADTYPE;
}

int UdfInqDimids(int /*ncid*/, int* ndims, int* /*dimids*/,
                 int /*include_parents*/) {
  if (ndims != nullptr) {
    *ndims = 0;
  }
  return NC_NOERR;
}

int UdfInqGrps(int /*ncid*/, int* numgrps, int* /*ncids*/) {
  if (numgrps != nullptr) {
    *numgrps = 0;
  }
  return NC_NOERR;
}

int UdfInqGrpname(int /*ncid*/, char* name) {
  if (name != nullptr) {
    std::snprintf(name, NC_MAX_NAME + 1, "%s", "/");
  }
  return NC_NOERR;
}

int UdfInq(int /*ncid*/, int* ndimsp, int* nvarsp, int* nattsp,
           int* unlimdimidp) {
  if (ndimsp != nullptr) {
    *ndimsp = 0;
  }
  if (unlimdimidp != nullptr) {
    *unlimdimidp = -1;
  }
  switch (g_udf_scenario) {
    case UdfScenario::kOpaqueAndEnumAliasAttrs:
      if (nvarsp != nullptr) *nvarsp = 1;
      if (nattsp != nullptr) *nattsp = 0;
      return NC_NOERR;
    case UdfScenario::kGlobalAttrNameError:
      if (nvarsp != nullptr) *nvarsp = 0;
      if (nattsp != nullptr) *nattsp = 1;
      return NC_NOERR;
    case UdfScenario::kCompoundFieldError:
      if (nvarsp != nullptr) *nvarsp = 0;
      if (nattsp != nullptr) *nattsp = 0;
      return NC_NOERR;
  }
  return NC_EINVAL;
}

int UdfInqTypeids(int /*ncid*/, int* ntypes, int* typeids) {
  switch (g_udf_scenario) {
    case UdfScenario::kOpaqueAndEnumAliasAttrs:
      if (ntypes != nullptr) {
        *ntypes = 2;
      }
      if (typeids != nullptr) {
        typeids[0] = NC_OPAQUE;
        typeids[1] = NC_ENUM;
      }
      return NC_NOERR;
    case UdfScenario::kGlobalAttrNameError:
      if (ntypes != nullptr) {
        *ntypes = 0;
      }
      return NC_NOERR;
    case UdfScenario::kCompoundFieldError:
      if (ntypes != nullptr) {
        *ntypes = 1;
      }
      if (typeids != nullptr) {
        typeids[0] = NC_FIRSTUSERTYPEID;
      }
      return NC_NOERR;
  }
  return NC_EINVAL;
}

int UdfInqUserType(int /*ncid*/, nc_type xtype, char* name, size_t* size,
                   nc_type* base_nc_typep, size_t* nfieldsp, int* classp) {
  switch (g_udf_scenario) {
    case UdfScenario::kOpaqueAndEnumAliasAttrs:
      if (xtype == NC_OPAQUE) {
        if (name != nullptr) {
          std::snprintf(name, NC_MAX_NAME + 1, "%s", "op_alias");
        }
        if (size != nullptr) *size = sizeof(unsigned char*);
        if (base_nc_typep != nullptr) *base_nc_typep = NC_NAT;
        if (nfieldsp != nullptr) *nfieldsp = 0;
        if (classp != nullptr) *classp = NC_OPAQUE;
        return NC_NOERR;
      }
      if (xtype == NC_ENUM) {
        if (name != nullptr) {
          std::snprintf(name, NC_MAX_NAME + 1, "%s", "en_alias");
        }
        if (size != nullptr) *size = sizeof(int);
        if (base_nc_typep != nullptr) *base_nc_typep = NC_INT;
        if (nfieldsp != nullptr) *nfieldsp = 0;
        // Keep usertype.kind != NC_ENUM so printValue() does not unwrap
        // basetype before getPrintValue() dispatches on basetype->id ==
        // NC_ENUM.
        if (classp != nullptr) *classp = NC_OPAQUE;
        return NC_NOERR;
      }
      return NC_EBADTYPE;
    case UdfScenario::kCompoundFieldError:
      if (xtype == NC_FIRSTUSERTYPEID) {
        if (name != nullptr) {
          std::snprintf(name, NC_MAX_NAME + 1, "%s", "bad_cmp");
        }
        if (size != nullptr) *size = sizeof(int);
        if (base_nc_typep != nullptr) *base_nc_typep = NC_NAT;
        if (nfieldsp != nullptr) *nfieldsp = 1;
        if (classp != nullptr) *classp = NC_COMPOUND;
        return NC_NOERR;
      }
      return NC_EBADTYPE;
    case UdfScenario::kGlobalAttrNameError:
      return NC_EBADTYPE;
  }
  return NC_EBADTYPE;
}

int UdfInqCompoundField(int /*ncid*/, nc_type /*xtype*/, int /*fieldid*/,
                        char* /*name*/, size_t* /*offsetp*/,
                        nc_type* /*field_typeidp*/, int* /*ndimsp*/,
                        int* /*dim_sizesp*/) {
  return NC_EBADFIELD;
}

int UdfInqVarids(int /*ncid*/, int* nvars, int* varids) {
  if (g_udf_scenario == UdfScenario::kOpaqueAndEnumAliasAttrs) {
    if (nvars != nullptr) {
      *nvars = 1;
    }
    if (varids != nullptr) {
      varids[0] = 0;
    }
    return NC_NOERR;
  }
  if (nvars != nullptr) {
    *nvars = 0;
  }
  return NC_NOERR;
}

int UdfInqVarAll(int /*ncid*/, int varid, char* name, nc_type* xtypep,
                 int* ndimsp, int* /*dimidsp*/, int* nattsp, int* /*shufflep*/,
                 int* /*deflatep*/, int* /*deflate_levelp*/,
                 int* /*fletcher32p*/, int* /*contiguousp*/,
                 size_t* /*chunksizesp*/, int* /*no_fill*/,
                 void* /*fill_valuep*/, int* /*endiannessp*/,
                 unsigned int* /*idp*/, size_t* /*nparamsp*/,
                 unsigned int* /*params*/) {
  if (g_udf_scenario != UdfScenario::kOpaqueAndEnumAliasAttrs || varid != 0) {
    return NC_ENOTVAR;
  }
  if (name != nullptr) {
    std::snprintf(name, NC_MAX_NAME + 1, "%s", "v");
  }
  if (xtypep != nullptr) {
    *xtypep = NC_INT;
  }
  if (ndimsp != nullptr) {
    *ndimsp = 0;
  }
  if (nattsp != nullptr) {
    *nattsp = 2;
  }
  return NC_NOERR;
}

int UdfInqAttname(int /*ncid*/, int varid, int attnum, char* name) {
  if (g_udf_scenario == UdfScenario::kGlobalAttrNameError &&
      varid == NC_GLOBAL) {
    return NC_ENOTATT;
  }
  if (g_udf_scenario != UdfScenario::kOpaqueAndEnumAliasAttrs || varid != 0) {
    return NC_ENOTATT;
  }
  if (attnum == 0) {
    if (name != nullptr) {
      std::snprintf(name, NC_MAX_NAME + 1, "%s", "op_att");
    }
    return NC_NOERR;
  }
  if (attnum == 1) {
    if (name != nullptr) {
      std::snprintf(name, NC_MAX_NAME + 1, "%s", "enum_att");
    }
    return NC_NOERR;
  }
  return NC_ENOTATT;
}

int UdfInqAtt(int /*ncid*/, int varid, const char* name, nc_type* xtypep,
              size_t* lenp) {
  if (g_udf_scenario != UdfScenario::kOpaqueAndEnumAliasAttrs || varid != 0 ||
      name == nullptr) {
    return NC_ENOTATT;
  }
  if (std::strcmp(name, "op_att") == 0) {
    if (xtypep != nullptr) *xtypep = NC_OPAQUE;
    if (lenp != nullptr) *lenp = 2;
    return NC_NOERR;
  }
  if (std::strcmp(name, "enum_att") == 0) {
    if (xtypep != nullptr) *xtypep = NC_ENUM;
    if (lenp != nullptr) *lenp = 1;
    return NC_NOERR;
  }
  return NC_ENOTATT;
}

int UdfGetAtt(int /*ncid*/, int varid, const char* name, void* value,
              nc_type /*memtype*/) {
  if (g_udf_scenario != UdfScenario::kOpaqueAndEnumAliasAttrs || varid != 0 ||
      name == nullptr || value == nullptr) {
    return NC_ENOTATT;
  }
  if (std::strcmp(name, "op_att") == 0) {
    static constexpr std::array<unsigned char, 16> kOpaqueBytes = {
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
        0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe};
    const unsigned char* ptrs[2] = {kOpaqueBytes.data(), nullptr};
    std::memcpy(value, ptrs, sizeof(ptrs));
    return NC_NOERR;
  }
  if (std::strcmp(name, "enum_att") == 0) {
    constexpr int kEnumIntVal = 77;
    std::memcpy(value, &kEnumIntVal, sizeof(kEnumIntVal));
    return NC_NOERR;
  }
  return NC_ENOTATT;
}

NC_Dispatch BuildUdf9Dispatch() {
  NC_Dispatch dispatch{};
  dispatch.model = NC_FORMATX_UDF9;
  dispatch.dispatch_version = NC_DISPATCH_VERSION;
  dispatch.create = &UdfCreate;
  dispatch.abort = &UdfAbort;
  dispatch.close = &UdfClose;
  dispatch.inq_format = &UdfInqFormat;
  dispatch.inq_format_extended = &UdfInqFormatExtended;
  dispatch.inq = &UdfInq;
  dispatch.inq_type = &UdfInqType;
  dispatch.inq_dimids = &UdfInqDimids;
  dispatch.inq_grps = &UdfInqGrps;
  dispatch.inq_grpname = &UdfInqGrpname;
  dispatch.inq_varids = &UdfInqVarids;
  dispatch.inq_typeids = &UdfInqTypeids;
  dispatch.inq_user_type = &UdfInqUserType;
  dispatch.inq_compound_field = &UdfInqCompoundField;
  dispatch.inq_var_all = &UdfInqVarAll;
  dispatch.inq_attname = &UdfInqAttname;
  dispatch.inq_att = &UdfInqAtt;
  dispatch.get_att = &UdfGetAtt;
  return dispatch;
}

CliResult RunNc4PrintInUdf9Child(UdfScenario scenario, const char* temp_name,
                                 int expected_status) {
  TempFile dataset_path(temp_name);
  constexpr std::string_view kArgs[] = {"nc4printer_udf9"};
  return RunCliInForkedChild(
      [&dataset_path, scenario, expected_status](int /*argc*/,
                                                 char** /*argv*/) -> int {
        g_udf_scenario = scenario;
        static NC_Dispatch dispatch = BuildUdf9Dispatch();
        if (nc_def_user_format(NC_UDF9, &dispatch, nullptr) != NC_NOERR) {
          std::fprintf(stderr, "nc_def_user_format(NC_UDF9) failed\n");
          return 1;
        }
        ScopedNcFile nc;
        if (nc_create(dataset_path.c_str(), NC_UDF9 | NC_NETCDF4, nc.ptr()) !=
            NC_NOERR) {
          std::fprintf(stderr, "nc_create(NC_UDF9) failed\n");
          return 2;
        }
        NCbytes* buf = ncbytesnew();
        if (buf == nullptr) {
          return 3;
        }
        absl::Cleanup free_buf = [&] { ncbytesfree(buf); };

        const int status = NC4print(buf, nc);
        if (status != expected_status) {
          std::fprintf(stderr, "NC4print returned %d (%s), expected %d (%s)\n",
                       status, nc_strerror(status), expected_status,
                       nc_strerror(expected_status));
          return 4;
        }
        ncbytesnull(buf);
        std::fputs(ncbytescontents(buf), stdout);
        return 0;
      },
      kArgs);
}

TEST(Nc4PrinterTest, PrintsOpaqueAndEnumAliasVariableAttributesViaUdf) {
  const CliResult res =
      RunNc4PrintInUdf9Child(UdfScenario::kOpaqueAndEnumAliasAttrs,
                             "nc4printer_udf9_attrs.nc", NC_NOERR);

  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;

  EXPECT_THAT(res.stdout_output, HasSubstr("<Opaque name=\"op_alias\""));
  EXPECT_THAT(res.stdout_output, HasSubstr("<Opaque name=\"en_alias\""));
  EXPECT_THAT(res.stdout_output, HasSubstr("<Attribute name=\"op_att\">"));
  EXPECT_THAT(res.stdout_output, HasSubstr("<Value value=\"0x01234567"));
  EXPECT_THAT(res.stdout_output, HasSubstr(R"(<Value value="0x00"/>)"));
  EXPECT_THAT(res.stdout_output, HasSubstr("<Attribute name=\"enum_att\">"));
  EXPECT_THAT(res.stdout_output, HasSubstr(R"(<Value value="77"/>)"));
}

TEST(Nc4PrinterTest, ReturnsErrorWhenGlobalAttrNameInquiryFailsViaUdf) {
  const CliResult res =
      RunNc4PrintInUdf9Child(UdfScenario::kGlobalAttrNameError,
                             "nc4printer_udf9_gattr.nc", NC_ENOTATT);

  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;
  EXPECT_THAT(res.stdout_output, HasSubstr("<Group name=\"/\">\n"));
}

TEST(Nc4PrinterTest, StopsCompoundPrintingWhenFieldInquiryFailsViaUdf) {
  const CliResult res =
      RunNc4PrintInUdf9Child(UdfScenario::kCompoundFieldError,
                             "nc4printer_udf9_cmpfield.nc", NC_NOERR);

  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;
  EXPECT_THAT(res.stdout_output, HasSubstr("<Compound name=\"bad_cmp\">\n"));
  EXPECT_THAT(res.stdout_output, Not(HasSubstr("</Compound>")));
}

}  // namespace
}  // namespace netcdf
