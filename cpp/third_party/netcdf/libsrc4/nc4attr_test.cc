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

// Unit tests for //third_party/netcdf/libsrc4/nc4attr.c.
//
// Covers nc4_get_att_ptrs(), nc4_get_att(), NC4_inq_att(), NC4_inq_attid(),
// NC4_inq_attname(), and NC4_get_att() across global and variable attributes,
// optional NULL pointers, zero-length attributes, NC_NAT handling, NC_ECHAR
// text/numeric checks, type conversion and NC_ERANGE/classic-model BYTE<->UBYTE
// exceptions, NC_STRING and compound/vlen reclamation, Unicode NFC/NFD
// normalization, and invalid ncid/varid/name/type/attnum paths.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libsrc4/nc4attr.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/libsrc4/nc4attr_shim.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

namespace netcdf {
namespace {

using ::testing::ElementsAre;

struct CompoundRecord {
  int32_t id;
  char* label;
};

TEST(Nc4AttrTest, MetadataAndZeroLengthReadsWorkAcrossGlobalAndVariableAtts) {
  ScopedMemio memio;
  {
    ScopedNcFile writer;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("nc4attr_meta.nc", NC_NETCDF4, 0, writer.ptr()));

    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(writer, "x", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(writer, "v", NC_INT, 1, &dimid, &varid));

    constexpr char kTitle[] = "sample";
    ASSERT_EQ(NC_NOERR, nc_put_att_text(writer, NC_GLOBAL, "title",
                                        sizeof(kTitle) - 1, kTitle));
    ASSERT_EQ(NC_NOERR,
              nc_put_att_text(writer, NC_GLOBAL, "empty_text", 0, ""));
    ASSERT_EQ(NC_NOERR, nc_put_att_int(writer, NC_GLOBAL, "empty_ints", NC_INT,
                                       0, nullptr));

    constexpr std::array<int32_t, 3> kVarInts = {11, 22, 33};
    ASSERT_EQ(NC_NOERR, nc_put_att_int(writer, varid, "weights", NC_INT,
                                       kVarInts.size(), kVarInts.data()));
    // NFC U+00E9 attribute name to verify Unicode normalization on lookup.
    constexpr double kScale = 2.5;
    ASSERT_EQ(NC_NOERR, nc_put_att_double(writer, varid, "r\xc3\xa9sum\xc3\xa9",
                                          NC_DOUBLE, 1, &kScale));

    // Verify direct libsrc4 calls on newly created in-memory attributes.
    nc_type xtype = NC_NAT;
    size_t len = 999;
    ASSERT_EQ(NC_NOERR,
              NetcdfTestNc4InqAtt(writer, NC_GLOBAL, "title", &xtype, &len));
    EXPECT_EQ(NC_CHAR, xtype);
    EXPECT_EQ(size_t{6}, len);
    EXPECT_EQ(NC_NOERR, NetcdfTestNc4InqAtt(writer, NC_GLOBAL, "title", nullptr,
                                            nullptr));

    int attnum = -1;
    ASSERT_EQ(NC_NOERR,
              NetcdfTestNc4InqAttid(writer, varid, "weights", &attnum));
    EXPECT_EQ(0, attnum);
    attnum = -1;
    ASSERT_EQ(NC_NOERR, NetcdfTestNc4InqAttid(
                            writer, varid, "re\xcc\x81sume\xcc\x81", &attnum));
    EXPECT_EQ(1, attnum);

    char att_name[NC_MAX_NAME + 1]{};
    ASSERT_EQ(NC_NOERR, NetcdfTestNc4InqAttname(writer, varid, 0, att_name));
    EXPECT_STREQ("weights", att_name);
    EXPECT_EQ(NC_NOERR, NetcdfTestNc4InqAttname(writer, varid, 0, nullptr));

    ASSERT_EQ(NC_NOERR, nc_close_memio(writer.release(), memio.get()));
  }

  ScopedNcFile reader;
  ASSERT_EQ(NC_NOERR, nc_open_mem("nc4attr_meta.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, reader.ptr()));
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(reader, "v", &varid));

  // Trigger HDF5 lazy attribute load on reopened file for both global and
  // variable lists before calling direct NC4_* shims.
  nc_type xtype = NC_NAT;
  size_t len = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_att(reader, NC_GLOBAL, "title", &xtype, &len));
  ASSERT_EQ(NC_NOERR, nc_inq_att(reader, varid, "weights", &xtype, &len));

  // Verify NC4_inq_att* and zero-length reads after reopen.
  int attnum = -1;
  ASSERT_EQ(NC_NOERR,
            NetcdfTestNc4InqAttid(reader, NC_GLOBAL, "empty_ints", &attnum));
  EXPECT_EQ(2, attnum);

  char global_name[NC_MAX_NAME + 1]{};
  ASSERT_EQ(NC_NOERR,
            NetcdfTestNc4InqAttname(reader, NC_GLOBAL, 0, global_name));
  EXPECT_STREQ("title", global_name);
  EXPECT_EQ(NC_NOERR, NetcdfTestNc4InqAttname(reader, NC_GLOBAL, 0, nullptr));

  // Zero-length attributes short-circuit before mem_type conversion/ECHAR
  // check.
  char char_sentinel = '\x5a';
  int32_t int_sentinel = -777;
  EXPECT_EQ(NC_NOERR,
            nc_get_att_text(reader, NC_GLOBAL, "empty_text", &char_sentinel));
  EXPECT_EQ('\x5a', char_sentinel);
  EXPECT_EQ(NC_NOERR, NetcdfTestNc4GetAtt(reader, NC_GLOBAL, "empty_text",
                                          &int_sentinel, NC_INT));
  EXPECT_EQ(-777, int_sentinel);
  EXPECT_EQ(NC_NOERR, NetcdfTestNc4GetAtt(reader, NC_GLOBAL, "empty_ints",
                                          &char_sentinel, NC_CHAR));
  EXPECT_EQ('\x5a', char_sentinel);

  // NC_NAT with non-NULL data uses the attribute's native file type.
  std::array<char, 8> text_buf{};
  ASSERT_EQ(NC_NOERR, NetcdfTestNc4GetAtt(reader, NC_GLOBAL, "title",
                                          text_buf.data(), NC_NAT));
  EXPECT_EQ("sample", std::string_view(text_buf.data(), 6));

  std::vector<int32_t> weights_out(3, -999);
  attnum = -1;
  xtype = NC_NAT;
  len = 0;
  ASSERT_EQ(NC_NOERR,
            NetcdfTestNc4GetAttRaw(reader, varid, "weights", &xtype, NC_NAT,
                                   &len, &attnum, weights_out.data()));
  EXPECT_EQ(NC_INT, xtype);
  EXPECT_EQ(size_t{3}, len);
  EXPECT_EQ(0, attnum);
  EXPECT_THAT(weights_out, ElementsAre(11, 22, 33));
}

TEST(Nc4AttrTest, TypeConversionsAndRangeAndCharErrorsBehaveCorrectly) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR, nc_create("nc4attr_convert.nc", NC_NETCDF4 | NC_DISKLESS,
                                file.ptr()));

  constexpr char kText[] = "abc";
  ASSERT_EQ(NC_NOERR, nc_put_att_text(file, NC_GLOBAL, "text_att", 3, kText));
  constexpr std::array<int32_t, 2> kLargeInts = {10, 100000};
  ASSERT_EQ(NC_NOERR, nc_put_att_int(file, NC_GLOBAL, "int_att", NC_INT,
                                     kLargeInts.size(), kLargeInts.data()));
  constexpr signed char kNegByte = -1;
  ASSERT_EQ(NC_NOERR, nc_put_att_schar(file, NC_GLOBAL, "byte_att", NC_BYTE, 1,
                                       &kNegByte));
  constexpr unsigned char kHighUbyte = 200;
  ASSERT_EQ(NC_NOERR, nc_put_att_uchar(file, NC_GLOBAL, "ubyte_att", NC_UBYTE,
                                       1, &kHighUbyte));
  const char* str_val = "not_an_int";
  ASSERT_EQ(NC_NOERR,
            nc_put_att_string(file, NC_GLOBAL, "str_att", 1, &str_val));

  // NC_ECHAR for text<->numeric mismatches on non-empty attributes.
  std::array<int32_t, 3> bad_ints = {-1, -1, -1};
  std::array<uint8_t, 3> bad_ubytes = {0, 0, 0};
  EXPECT_EQ(NC_ECHAR,
            nc_get_att_int(file, NC_GLOBAL, "text_att", bad_ints.data()));
  EXPECT_EQ(NC_ECHAR, NetcdfTestNc4GetAtt(file, NC_GLOBAL, "text_att",
                                          bad_ubytes.data(), NC_UBYTE));
  std::array<char, 4> bad_text{};
  EXPECT_EQ(NC_ECHAR,
            nc_get_att_text(file, NC_GLOBAL, "int_att", bad_text.data()));
  EXPECT_EQ(NC_ECHAR, NetcdfTestNc4GetAtt(file, NC_GLOBAL, "byte_att",
                                          bad_text.data(), NC_CHAR));

  // Lossless numeric conversion NC_INT -> NC_DOUBLE.
  std::vector<double> as_double(2, -999.0);
  ASSERT_EQ(NC_NOERR,
            nc_get_att_double(file, NC_GLOBAL, "int_att", as_double.data()));
  EXPECT_THAT(as_double, ElementsAre(10.0, 100000.0));

  // Range overflow triggers NC_ERANGE.
  std::vector<short> overflow_shorts(2, -1);
  EXPECT_EQ(NC_ERANGE, nc_get_att_short(file, NC_GLOBAL, "int_att",
                                        overflow_shorts.data()));

  // Non-classic NC_NETCDF4 rejects BYTE<->UBYTE out-of-range values.
  unsigned char ubyte_out = 0;
  signed char schar_out = 0;
  EXPECT_EQ(NC_ERANGE,
            nc_get_att_uchar(file, NC_GLOBAL, "byte_att", &ubyte_out));
  EXPECT_EQ(NC_ERANGE,
            nc_get_att_schar(file, NC_GLOBAL, "ubyte_att", &schar_out));

  // Unsupported conversion NC_STRING -> NC_INT frees conversion buffer cleanly
  // and returns NC_EBADTYPE.
  int32_t str_to_int = -1;
  EXPECT_EQ(NC_EBADTYPE, NetcdfTestNc4GetAtt(file, NC_GLOBAL, "str_att",
                                             &str_to_int, NC_INT));

  // Invalid memory type fails nc4_get_typelen_mem().
  int32_t unused_val = 0;
  EXPECT_EQ(NC_EBADTYPID,
            NetcdfTestNc4GetAtt(file, NC_GLOBAL, "int_att", &unused_val, 9999));
  EXPECT_EQ(NC_EINVAL,
            NetcdfTestNc4GetAtt(file, NC_GLOBAL, "int_att", &unused_val, -5));
}

TEST(Nc4AttrTest,
     ClassicModelPermitsSignedByteReadAsUbyteButReportsOtherRange) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR,
            nc_create("nc4attr_classic_model.nc",
                      NC_NETCDF4 | NC_CLASSIC_MODEL | NC_DISKLESS, file.ptr()));

  constexpr signed char kNegByte = -1;
  ASSERT_EQ(NC_NOERR, nc_put_att_schar(file, NC_GLOBAL, "signed_byte", NC_BYTE,
                                       1, &kNegByte));
  constexpr int32_t kTooBigForShort = 100000;
  ASSERT_EQ(NC_NOERR, nc_put_att_int(file, NC_GLOBAL, "big_int", NC_INT, 1,
                                     &kTooBigForShort));

  // Strict NC_CLASSIC_MODEL suppresses NC_ERANGE for NC_BYTE -> NC_UBYTE.
  unsigned char as_ubyte = 0;
  ASSERT_EQ(NC_NOERR,
            nc_get_att_uchar(file, NC_GLOBAL, "signed_byte", &as_ubyte));
  EXPECT_EQ(static_cast<unsigned char>(255), as_ubyte);

  // Other out-of-range conversions still return NC_ERANGE.
  short bad_short = 0;
  EXPECT_EQ(NC_ERANGE,
            nc_get_att_short(file, NC_GLOBAL, "big_int", &bad_short));
}

TEST(Nc4AttrTest, StringsAndUserDefinedCompoundVlenEnumOpaqueRoundTrip) {
  ScopedMemio memio;
  {
    ScopedNcFile writer;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("nc4attr_types.nc", NC_NETCDF4, 0, writer.ptr()));

    // Enum attribute.
    nc_type enum_type = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_enum(writer, NC_INT, "StatusEnum", &enum_type));
    constexpr int32_t kEnumOk = 7;
    ASSERT_EQ(NC_NOERR, nc_insert_enum(writer, enum_type, "OK", &kEnumOk));
    ASSERT_EQ(NC_NOERR, nc_put_att(writer, NC_GLOBAL, "enum_att", enum_type, 1,
                                   &kEnumOk));

    // Opaque attribute.
    nc_type opaque_type = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_opaque(writer, 4, "Tag4", &opaque_type));
    constexpr std::array<uint8_t, 4> kOpaqueBytes = {0xDE, 0xAD, 0xBE, 0xEF};
    ASSERT_EQ(NC_NOERR, nc_put_att(writer, NC_GLOBAL, "opaque_att", opaque_type,
                                   1, kOpaqueBytes.data()));

    // VLEN attribute.
    nc_type vlen_type = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_vlen(writer, "IntVec", NC_INT, &vlen_type));
    std::array<int32_t, 3> vlen_data = {10, 20, 30};
    const nc_vlen_t vlen_in{vlen_data.size(), vlen_data.data()};
    ASSERT_EQ(NC_NOERR, nc_put_att(writer, NC_GLOBAL, "vlen_att", vlen_type, 1,
                                   &vlen_in));

    // Compound attribute with nested NC_STRING field.
    nc_type cmp_type = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_compound(writer, sizeof(CompoundRecord),
                                        "TaggedId", &cmp_type));
    ASSERT_EQ(NC_NOERR, nc_insert_compound(
                            writer, cmp_type, "id",
                            NC_COMPOUND_OFFSET(CompoundRecord, id), NC_INT));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(writer, cmp_type, "label",
                                 NC_COMPOUND_OFFSET(CompoundRecord, label),
                                 NC_STRING));
    char label_buf[] = "nested_compound";
    const CompoundRecord cmp_in{42, label_buf};
    ASSERT_EQ(NC_NOERR, nc_put_att(writer, NC_GLOBAL, "compound_att", cmp_type,
                                   1, &cmp_in));

    // Top-level NC_STRING array attribute.
    std::array<const char*, 2> str_array = {"alpha", "beta"};
    ASSERT_EQ(NC_NOERR, nc_put_att_string(writer, NC_GLOBAL, "strings_att",
                                          str_array.size(), str_array.data()));

    ASSERT_EQ(NC_NOERR, nc_close_memio(writer.release(), memio.get()));
  }

  ScopedNcFile reader;
  ASSERT_EQ(NC_NOERR, nc_open_mem("nc4attr_types.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, reader.ptr()));

  // Read NC_STRING array attribute via both public API and direct NC4_get_att.
  {
    ScopedNcStrings strings_out(2);
    ASSERT_EQ(NC_NOERR, nc_get_att_string(reader, NC_GLOBAL, "strings_att",
                                          strings_out.data()));
    EXPECT_STREQ("alpha", strings_out[0]);
    EXPECT_STREQ("beta", strings_out[1]);
  }
  {
    ScopedNcStrings direct_strings(2);
    ASSERT_EQ(NC_NOERR, NetcdfTestNc4GetAtt(reader, NC_GLOBAL, "strings_att",
                                            direct_strings.data(), NC_STRING));
    EXPECT_STREQ("alpha", direct_strings[0]);
    EXPECT_STREQ("beta", direct_strings[1]);
  }

  // Read Enum & Opaque attributes.
  int32_t enum_out = -1;
  ASSERT_EQ(NC_NOERR, nc_get_att(reader, NC_GLOBAL, "enum_att", &enum_out));
  EXPECT_EQ(7, enum_out);

  std::array<uint8_t, 4> opaque_out{};
  ASSERT_EQ(NC_NOERR,
            nc_get_att(reader, NC_GLOBAL, "opaque_att", opaque_out.data()));
  EXPECT_THAT(opaque_out, ElementsAre(0xDE, 0xAD, 0xBE, 0xEF));

  // Read VLEN attribute with RAII cleanup ordered before reader closes.
  {
    ScopedNcVlens vlen_out(1);
    ASSERT_EQ(NC_NOERR,
              nc_get_att(reader, NC_GLOBAL, "vlen_att", vlen_out.data()));
    ASSERT_EQ(size_t{3}, vlen_out[0].len);
    const auto* elems = static_cast<const int32_t*>(vlen_out[0].p);
    EXPECT_THAT(std::vector<int32_t>(elems, elems + vlen_out[0].len),
                ElementsAre(10, 20, 30));
  }

  // Read Compound attribute containing nested NC_STRING and reclaim BEFORE
  // ScopedNcFile closes.
  nc_type reopened_cmp_type = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(reader, "TaggedId", &reopened_cmp_type));
  CompoundRecord cmp_out{};
  absl::Cleanup reclaim_cmp = [&]() {
    nc_reclaim_data(reader, reopened_cmp_type, &cmp_out, 1);
  };
  ASSERT_EQ(NC_NOERR, nc_get_att(reader, NC_GLOBAL, "compound_att", &cmp_out));
  EXPECT_EQ(42, cmp_out.id);
  EXPECT_STREQ("nested_compound", cmp_out.label);
}

TEST(Nc4AttrTest, RejectsInvalidIdsVaridsNamesAttnumsAndMissingAttributes) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR, nc_create("nc4attr_errors.nc", NC_NETCDF4 | NC_DISKLESS,
                                file.ptr()));
  int dimid = -1;
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "x", 2, &dimid));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v", NC_INT, 1, &dimid, &varid));
  constexpr int32_t kVal = 1;
  ASSERT_EQ(NC_NOERR, nc_put_att_int(file, varid, "a", NC_INT, 1, &kVal));

  nc_type xtype = NC_NAT;
  size_t len = 0;
  int attnum = -1;
  char name_buf[NC_MAX_NAME + 1]{};
  int32_t val = 0;
  const int bogus_grpid = file.get() | 0x7FFF;

  // Invalid ncid (both -1 and bad group ID).
  EXPECT_EQ(NC_EBADID, NetcdfTestNc4InqAtt(-1, NC_GLOBAL, "a", &xtype, &len));
  EXPECT_EQ(NC_EBADID,
            NetcdfTestNc4InqAtt(bogus_grpid, NC_GLOBAL, "a", &xtype, &len));
  EXPECT_EQ(NC_EBADID, NetcdfTestNc4InqAttid(bogus_grpid, varid, "a", &attnum));
  EXPECT_EQ(NC_EBADID, NetcdfTestNc4InqAttname(-1, varid, 0, name_buf));
  EXPECT_EQ(NC_EBADID,
            NetcdfTestNc4InqAttname(bogus_grpid, varid, 0, name_buf));
  EXPECT_EQ(NC_EBADID,
            NetcdfTestNc4GetAtt(bogus_grpid, varid, "a", &val, NC_INT));

  // Invalid varid.
  EXPECT_EQ(NC_ENOTVAR, NetcdfTestNc4InqAtt(file, 99, "a", &xtype, &len));
  EXPECT_EQ(NC_ENOTVAR, NetcdfTestNc4InqAtt(file, -2, "a", &xtype, &len));
  EXPECT_EQ(NC_ENOTVAR, NetcdfTestNc4InqAttname(file, 99, 0, name_buf));
  EXPECT_EQ(NC_ENOTVAR, NetcdfTestNc4GetAtt(file, 99, "a", &val, NC_INT));

  // NULL name and direct nc4_get_att_ptrs() NULL-name guard.
  EXPECT_EQ(NC_EBADNAME,
            NetcdfTestNc4InqAtt(file, varid, nullptr, &xtype, &len));
  EXPECT_EQ(NC_EBADNAME, NetcdfTestNc4InqAttid(file, varid, nullptr, &attnum));
  EXPECT_EQ(NC_EBADNAME,
            NetcdfTestNc4GetAttPtrsNullName(file, NC_GLOBAL, &xtype, NC_NAT,
                                            &len, nullptr, nullptr));
  attnum = -1;
  EXPECT_EQ(NC_EBADNAME,
            NetcdfTestNc4GetAttPtrsNullName(file, varid, &xtype, NC_NAT, &len,
                                            &attnum, nullptr));

  // Name normalization errors: invalid UTF-8 and overlong name.
  EXPECT_EQ(NC_EBADNAME,
            NetcdfTestNc4InqAtt(file, varid, "\xff\xfe", &xtype, &len));
  const std::string overlong(NC_MAX_NAME + 1, 'a');
  EXPECT_EQ(NC_EMAXNAME,
            NetcdfTestNc4InqAtt(file, varid, overlong.c_str(), &xtype, &len));

  // Missing attribute by name and by attnum.
  EXPECT_EQ(NC_ENOTATT,
            NetcdfTestNc4InqAtt(file, varid, "no_such_att", &xtype, &len));
  EXPECT_EQ(NC_ENOTATT,
            NetcdfTestNc4InqAtt(file, NC_GLOBAL, "no_such_att", &xtype, &len));
  EXPECT_EQ(NC_ENOTATT, NetcdfTestNc4InqAttname(file, varid, -1, name_buf));
  EXPECT_EQ(NC_ENOTATT, NetcdfTestNc4InqAttname(file, varid, 99, name_buf));
}

}  // namespace
}  // namespace netcdf
