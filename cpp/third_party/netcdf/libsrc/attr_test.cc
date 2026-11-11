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

// Unit tests for //third_party/netcdf/libsrc/attr.c (NetCDF-3 attributes).
//
// Covers the full NetCDF-3 numeric conversion matrix across CDF-1, CDF-2, and
// CDF-5, classic NC_BYTE/NC_UBYTE compatibility rules, text and zero-length
// attributes, attribute inquiry and ordering, define/data-mode overwrites,
// renames, deletions, redef/abort rollbacks, nc_copy_att across formats, API
// validation errors, and internal NC3_* / NC_attrarray helpers.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libsrc/attr.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"

extern "C" {
#include "third_party/netcdf/include/fbits.h"
#include "third_party/netcdf/include/nc.h"
#include "third_party/netcdf/include/nc3dispatch.h"
#include "third_party/netcdf/include/nc3internal.h"
#include "third_party/netcdf/include/ncdispatch.h"
}

namespace netcdf {
namespace {

using ::testing::ElementsAre;

constexpr std::array<nc_type, 10> kAllNumericTypes = {
    NC_BYTE,  NC_SHORT,  NC_INT,  NC_FLOAT, NC_DOUBLE,
    NC_UBYTE, NC_USHORT, NC_UINT, NC_INT64, NC_UINT64,
};

constexpr std::array<int, 3> kMatrixSample = {3, 17, 91};

int PutNumericAttrWithMemType(int ncid, int varid, const char* name,
                              nc_type mem_type, nc_type ext_type,
                              const std::array<int, 3>& values) {
  switch (mem_type) {
    case NC_BYTE: {
      const std::array<signed char, 3> buf = {
          static_cast<signed char>(values[0]),
          static_cast<signed char>(values[1]),
          static_cast<signed char>(values[2]),
      };
      return nc_put_att_schar(ncid, varid, name, ext_type, buf.size(),
                              buf.data());
    }
    case NC_UBYTE: {
      const std::array<unsigned char, 3> buf = {
          static_cast<unsigned char>(values[0]),
          static_cast<unsigned char>(values[1]),
          static_cast<unsigned char>(values[2]),
      };
      return nc_put_att_uchar(ncid, varid, name, ext_type, buf.size(),
                              buf.data());
    }
    case NC_SHORT: {
      const std::array<short, 3> buf = {
          static_cast<short>(values[0]),
          static_cast<short>(values[1]),
          static_cast<short>(values[2]),
      };
      return nc_put_att_short(ncid, varid, name, ext_type, buf.size(),
                              buf.data());
    }
    case NC_USHORT: {
      const std::array<unsigned short, 3> buf = {
          static_cast<unsigned short>(values[0]),
          static_cast<unsigned short>(values[1]),
          static_cast<unsigned short>(values[2]),
      };
      return nc_put_att_ushort(ncid, varid, name, ext_type, buf.size(),
                               buf.data());
    }
    case NC_INT:
      return nc_put_att_int(ncid, varid, name, ext_type, values.size(),
                            values.data());
    case NC_UINT: {
      const std::array<unsigned int, 3> buf = {
          static_cast<unsigned int>(values[0]),
          static_cast<unsigned int>(values[1]),
          static_cast<unsigned int>(values[2]),
      };
      return nc_put_att_uint(ncid, varid, name, ext_type, buf.size(),
                             buf.data());
    }
    case NC_INT64: {
      const std::array<long long, 3> buf = {
          static_cast<long long>(values[0]),
          static_cast<long long>(values[1]),
          static_cast<long long>(values[2]),
      };
      return nc_put_att_longlong(ncid, varid, name, ext_type, buf.size(),
                                 buf.data());
    }
    case NC_UINT64: {
      const std::array<unsigned long long, 3> buf = {
          static_cast<unsigned long long>(values[0]),
          static_cast<unsigned long long>(values[1]),
          static_cast<unsigned long long>(values[2]),
      };
      return nc_put_att_ulonglong(ncid, varid, name, ext_type, buf.size(),
                                  buf.data());
    }
    case NC_FLOAT: {
      const std::array<float, 3> buf = {
          static_cast<float>(values[0]),
          static_cast<float>(values[1]),
          static_cast<float>(values[2]),
      };
      return nc_put_att_float(ncid, varid, name, ext_type, buf.size(),
                              buf.data());
    }
    case NC_DOUBLE: {
      const std::array<double, 3> buf = {
          static_cast<double>(values[0]),
          static_cast<double>(values[1]),
          static_cast<double>(values[2]),
      };
      return nc_put_att_double(ncid, varid, name, ext_type, buf.size(),
                               buf.data());
    }
    default:
      return NC_EBADTYPE;
  }
}

int GetNumericAttrWithMemType(int ncid, int varid, const char* name,
                              nc_type mem_type, std::array<int, 3>* out) {
  switch (mem_type) {
    case NC_BYTE: {
      std::array<signed char, 3> buf = {-77, -77, -77};
      const int status = nc_get_att_schar(ncid, varid, name, buf.data());
      for (size_t i = 0; i < buf.size(); ++i) {
        (*out)[i] = static_cast<int>(buf[i]);
      }
      return status;
    }
    case NC_UBYTE: {
      std::array<unsigned char, 3> buf = {222, 222, 222};
      const int status = nc_get_att_uchar(ncid, varid, name, buf.data());
      for (size_t i = 0; i < buf.size(); ++i) {
        (*out)[i] = static_cast<int>(buf[i]);
      }
      return status;
    }
    case NC_SHORT: {
      std::array<short, 3> buf = {-77, -77, -77};
      const int status = nc_get_att_short(ncid, varid, name, buf.data());
      for (size_t i = 0; i < buf.size(); ++i) {
        (*out)[i] = static_cast<int>(buf[i]);
      }
      return status;
    }
    case NC_USHORT: {
      std::array<unsigned short, 3> buf = {222, 222, 222};
      const int status = nc_get_att_ushort(ncid, varid, name, buf.data());
      for (size_t i = 0; i < buf.size(); ++i) {
        (*out)[i] = static_cast<int>(buf[i]);
      }
      return status;
    }
    case NC_INT: {
      std::array<int, 3> buf = {-77, -77, -77};
      const int status = nc_get_att_int(ncid, varid, name, buf.data());
      *out = buf;
      return status;
    }
    case NC_UINT: {
      std::array<unsigned int, 3> buf = {222, 222, 222};
      const int status = nc_get_att_uint(ncid, varid, name, buf.data());
      for (size_t i = 0; i < buf.size(); ++i) {
        (*out)[i] = static_cast<int>(buf[i]);
      }
      return status;
    }
    case NC_INT64: {
      std::array<long long, 3> buf = {-77, -77, -77};
      const int status = nc_get_att_longlong(ncid, varid, name, buf.data());
      for (size_t i = 0; i < buf.size(); ++i) {
        (*out)[i] = static_cast<int>(buf[i]);
      }
      return status;
    }
    case NC_UINT64: {
      std::array<unsigned long long, 3> buf = {222, 222, 222};
      const int status = nc_get_att_ulonglong(ncid, varid, name, buf.data());
      for (size_t i = 0; i < buf.size(); ++i) {
        (*out)[i] = static_cast<int>(buf[i]);
      }
      return status;
    }
    case NC_FLOAT: {
      std::array<float, 3> buf = {-77.0F, -77.0F, -77.0F};
      const int status = nc_get_att_float(ncid, varid, name, buf.data());
      for (size_t i = 0; i < buf.size(); ++i) {
        (*out)[i] = static_cast<int>(buf[i]);
      }
      return status;
    }
    case NC_DOUBLE: {
      std::array<double, 3> buf = {-77.0, -77.0, -77.0};
      const int status = nc_get_att_double(ncid, varid, name, buf.data());
      for (size_t i = 0; i < buf.size(); ++i) {
        (*out)[i] = static_cast<int>(buf[i]);
      }
      return status;
    }
    default:
      return NC_EBADTYPE;
  }
}

TEST(AttrTest, PutAndGetFullNumericMatrixCdf5) {
  ScopedMemio memio;
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("matrix_cdf5.nc", NC_CDF5, 8192, file.ptr()));

    for (size_t i = 0; i < kAllNumericTypes.size(); ++i) {
      for (size_t j = 0; j < kAllNumericTypes.size(); ++j) {
        const std::string name = absl::StrFormat("m_%zu_%zu", i, j);
        SCOPED_TRACE(name);
        ASSERT_EQ(NC_NOERR,
                  PutNumericAttrWithMemType(
                      file, NC_GLOBAL, name.c_str(), kAllNumericTypes[i],
                      kAllNumericTypes[j], kMatrixSample));
      }
    }

    const std::array<long, 3> long_vals = {3L, 17L, 91L};
    ASSERT_EQ(NC_NOERR, nc_put_att_long(file, NC_GLOBAL, "m_long", NC_INT64,
                                        long_vals.size(), long_vals.data()));

    const std::array<unsigned char, 3> ubyte_vals = {3, 17, 91};
    ASSERT_EQ(NC_NOERR, nc_put_att_ubyte(file, NC_GLOBAL, "m_ubyte", NC_UBYTE,
                                         ubyte_vals.size(), ubyte_vals.data()));

    const std::array<int, 3> generic_vals = {3, 17, 91};
    ASSERT_EQ(NC_NOERR, nc_put_att(file, NC_GLOBAL, "m_generic", NC_INT,
                                   generic_vals.size(), generic_vals.data()));

    const int writer_id = file.release();
    ASSERT_EQ(NC_NOERR, nc_close_memio(writer_id, memio.get()));
  }

  ScopedNcFile ro_file;
  ASSERT_EQ(NC_NOERR, nc_open_mem("matrix_cdf5.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, ro_file.ptr()));

  int natts = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_natts(ro_file, &natts));
  EXPECT_EQ(103, natts);

  for (size_t i = 0; i < kAllNumericTypes.size(); ++i) {
    for (size_t j = 0; j < kAllNumericTypes.size(); ++j) {
      const std::string name = absl::StrFormat("m_%zu_%zu", i, j);
      SCOPED_TRACE(name);
      nc_type xtype = NC_NAT;
      size_t len = 0;
      ASSERT_EQ(NC_NOERR,
                nc_inq_att(ro_file, NC_GLOBAL, name.c_str(), &xtype, &len));
      EXPECT_EQ(kAllNumericTypes[j], xtype);
      EXPECT_EQ(kMatrixSample.size(), len);

      for (size_t r = 0; r < kAllNumericTypes.size(); ++r) {
        std::array<int, 3> read_back = {-1, -1, -1};
        ASSERT_EQ(NC_NOERR,
                  GetNumericAttrWithMemType(ro_file, NC_GLOBAL, name.c_str(),
                                            kAllNumericTypes[r], &read_back));
        EXPECT_THAT(read_back, ElementsAre(3, 17, 91));
      }
    }
  }

  std::array<long, 3> long_out = {-1L, -1L, -1L};
  ASSERT_EQ(NC_NOERR,
            nc_get_att_long(ro_file, NC_GLOBAL, "m_long", long_out.data()));
  EXPECT_THAT(long_out, ElementsAre(3L, 17L, 91L));

  std::array<unsigned char, 3> ubyte_out = {222, 222, 222};
  ASSERT_EQ(NC_NOERR,
            nc_get_att_ubyte(ro_file, NC_GLOBAL, "m_ubyte", ubyte_out.data()));
  EXPECT_THAT(ubyte_out, ElementsAre(3, 17, 91));

  std::array<int, 3> generic_out = {-1, -1, -1};
  ASSERT_EQ(NC_NOERR,
            nc_get_att(ro_file, NC_GLOBAL, "m_generic", generic_out.data()));
  EXPECT_THAT(generic_out, ElementsAre(3, 17, 91));
}

TEST(AttrTest, ClassicAnd64BitOffsetByteUbyteCompatibility) {
  for (const int mode : {0, NC_64BIT_OFFSET}) {
    SCOPED_TRACE(absl::StrFormat("mode=%d", mode));
    ScopedMemio memio;
    {
      ScopedNcFile file;
      ASSERT_EQ(NC_NOERR, nc_create_mem("compat.nc", mode, 2048, file.ptr()));

      // In CDF-1 and CDF-2, NC_BYTE paired with uchar memtype writes unsigned
      // bytes without NC_ERANGE both in define mode and in data mode.
      const std::array<unsigned char, 3> define_bytes = {0, 128, 255};
      ASSERT_EQ(NC_NOERR,
                nc_put_att_uchar(file, NC_GLOBAL, "u8", NC_BYTE,
                                 define_bytes.size(), define_bytes.data()));
      const signed char signed_byte = -5;
      ASSERT_EQ(NC_NOERR, nc_put_att_schar(file, NC_GLOBAL, "s8", NC_BYTE, 1,
                                           &signed_byte));
      const unsigned char short_as_uchar = 42;
      ASSERT_EQ(NC_NOERR, nc_put_att_uchar(file, NC_GLOBAL, "u16", NC_SHORT, 1,
                                           &short_as_uchar));

      // CDF-5-only and NetCDF-4-only attribute types are rejected.
      const unsigned int u32 = 42U;
      EXPECT_EQ(NC_EBADTYPE,
                nc_put_att_uint(file, NC_GLOBAL, "bad_u32", NC_UINT, 1, &u32));
      const unsigned long long u64 = 42ULL;
      EXPECT_EQ(NC_EBADTYPE, nc_put_att_ulonglong(file, NC_GLOBAL, "bad_u64",
                                                  NC_UINT64, 1, &u64));
      EXPECT_EQ(NC_EBADTYPE,
                nc_put_att_ubyte(file, NC_GLOBAL, "bad_u8", NC_UBYTE, 1,
                                 define_bytes.data()));
      const char* s = "x";
      EXPECT_EQ(NC_EBADTYPE,
                nc_put_att_string(file, NC_GLOBAL, "bad_str", 1, &s));

      ASSERT_EQ(NC_NOERR, nc_enddef(file));

      std::array<unsigned char, 3> read_define = {1, 1, 1};
      ASSERT_EQ(NC_NOERR,
                nc_get_att_uchar(file, NC_GLOBAL, "u8", read_define.data()));
      EXPECT_THAT(read_define, ElementsAre(0, 128, 255));

      unsigned char u16_out = 0;
      ASSERT_EQ(NC_NOERR, nc_get_att_uchar(file, NC_GLOBAL, "u16", &u16_out));
      EXPECT_EQ(42, u16_out);

      const std::array<unsigned char, 3> data_bytes = {255, 42, 1};
      ASSERT_EQ(NC_NOERR,
                nc_put_att_uchar(file, NC_GLOBAL, "u8", NC_BYTE,
                                 data_bytes.size(), data_bytes.data()));
      const signed char signed_byte_data = -12;
      ASSERT_EQ(NC_NOERR, nc_put_att_schar(file, NC_GLOBAL, "s8", NC_BYTE, 1,
                                           &signed_byte_data));
      const unsigned char short_as_uchar_data = 77;
      ASSERT_EQ(NC_NOERR, nc_put_att_uchar(file, NC_GLOBAL, "u16", NC_SHORT, 1,
                                           &short_as_uchar_data));

      const int writer_id = file.release();
      ASSERT_EQ(NC_NOERR, nc_close_memio(writer_id, memio.get()));
    }

    ScopedNcFile ro_file;
    ASSERT_EQ(NC_NOERR, nc_open_mem("compat.nc", NC_NOWRITE, memio.size(),
                                    memio.get()->memory, ro_file.ptr()));

    nc_type xtype = NC_NAT;
    size_t len = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_att(ro_file, NC_GLOBAL, "u8", &xtype, &len));
    EXPECT_EQ(NC_BYTE, xtype);
    EXPECT_EQ(3U, len);

    std::array<unsigned char, 3> out_bytes = {7, 7, 7};
    ASSERT_EQ(NC_NOERR,
              nc_get_att_uchar(ro_file, NC_GLOBAL, "u8", out_bytes.data()));
    EXPECT_THAT(out_bytes, ElementsAre(255, 42, 1));

    signed char s8_out = 0;
    ASSERT_EQ(NC_NOERR, nc_get_att_schar(ro_file, NC_GLOBAL, "s8", &s8_out));
    EXPECT_EQ(-12, s8_out);

    unsigned char u16_ro_out = 0;
    ASSERT_EQ(NC_NOERR,
              nc_get_att_uchar(ro_file, NC_GLOBAL, "u16", &u16_ro_out));
    EXPECT_EQ(77, u16_ro_out);
  }

  // Contrast with CDF-5: NC_BYTE is strictly signed, so 200 triggers NC_ERANGE.
  ScopedNcFile cdf5;
  ASSERT_EQ(NC_NOERR, nc_create_mem("cdf5_byte.nc", NC_CDF5, 1024, cdf5.ptr()));
  const unsigned char over = 200;
  EXPECT_EQ(NC_ERANGE, nc_put_att_uchar(cdf5, NC_GLOBAL, "signed_only", NC_BYTE,
                                        1, &over));
}

TEST(AttrTest, TextAndZeroLengthAttributes) {
  struct TextCase {
    const char* name;
    const char* data;
    size_t len;
  };
  constexpr std::array<TextCase, 8> kTextCases = {{
      {"t0", nullptr, 0},
      {"t1", "A", 1},
      {"t2", "AB", 2},
      {"t3", "ABC", 3},
      {"t4", "WXYZ", 4},
      {"t5", "ab\0cd", 5},
      {"t8", "12345678", 8},
      {"t13", "abcdefghijklm", 13},
  }};

  ScopedMemio memio;
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("text_zero.nc", NC_CDF5, 4096, file.ptr()));

    for (const auto& tc : kTextCases) {
      ASSERT_EQ(NC_NOERR,
                nc_put_att_text(file, NC_GLOBAL, tc.name, tc.len, tc.data));
    }

    // Zero-length numeric attributes for all CDF-5 numeric external types,
    // exercising xsz == 0 -> xvalue == NULL with both NULL and non-NULL inputs.
    const int dummy = 123;
    for (size_t i = 0; i < kAllNumericTypes.size(); ++i) {
      const std::string name = absl::StrFormat("z_%zu", i);
      ASSERT_EQ(NC_NOERR,
                nc_put_att(file, NC_GLOBAL, name.c_str(), kAllNumericTypes[i],
                           0, (i % 2 == 0) ? nullptr : &dummy));
    }

    const int writer_id = file.release();
    ASSERT_EQ(NC_NOERR, nc_close_memio(writer_id, memio.get()));
  }

  ScopedNcFile ro_file;
  ASSERT_EQ(NC_NOERR, nc_open_mem("text_zero.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, ro_file.ptr()));

  for (const auto& tc : kTextCases) {
    SCOPED_TRACE(tc.name);
    nc_type xtype = NC_NAT;
    size_t len = 999;
    ASSERT_EQ(NC_NOERR, nc_inq_att(ro_file, NC_GLOBAL, tc.name, &xtype, &len));
    EXPECT_EQ(NC_CHAR, xtype);
    EXPECT_EQ(tc.len, len);

    if (tc.len == 0) {
      EXPECT_EQ(NC_NOERR,
                nc_get_att_text(ro_file, NC_GLOBAL, tc.name, nullptr));
    } else {
      std::vector<char> buf(tc.len + 2, '@');
      ASSERT_EQ(NC_NOERR,
                nc_get_att_text(ro_file, NC_GLOBAL, tc.name, buf.data()));
      EXPECT_EQ(0, std::memcmp(buf.data(), tc.data, tc.len));
      // Surrounding sentinel bytes must remain intact.
      EXPECT_EQ('@', buf[tc.len]);
      EXPECT_EQ('@', buf[tc.len + 1]);
    }
  }

  for (size_t i = 0; i < kAllNumericTypes.size(); ++i) {
    const std::string name = absl::StrFormat("z_%zu", i);
    nc_type xtype = NC_NAT;
    size_t len = 999;
    ASSERT_EQ(NC_NOERR,
              nc_inq_att(ro_file, NC_GLOBAL, name.c_str(), &xtype, &len));
    EXPECT_EQ(kAllNumericTypes[i], xtype);
    EXPECT_EQ(0U, len);

    int sentinel = -999;
    EXPECT_EQ(NC_NOERR,
              nc_get_att_int(ro_file, NC_GLOBAL, name.c_str(), &sentinel));
    EXPECT_EQ(-999, sentinel);
    EXPECT_EQ(NC_NOERR,
              nc_get_att_int(ro_file, NC_GLOBAL, name.c_str(), nullptr));
  }
}

TEST(AttrTest, InquiryNamesIdsVariablesAndOrdering) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR, nc_create_mem("inq_order.nc", NC_CDF5, 4096, file.ptr()));

  int dimid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "x", 4, &dimid));
  int var0 = -1;
  int var1 = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v0", NC_INT, 1, &dimid, &var0));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v1", NC_DOUBLE, 1, &dimid, &var1));

  // Add 10 global attributes to exercise NC_ARRAY_GROWBY initial allocation (4)
  // and multiple reallocations (4 -> 8 -> 12).
  constexpr std::array<const char*, 10> kGlobalNames = {
      "alpha", "g_01", "g_02", "g_03", "g_04",
      "g_05",  "g_06", "g_07", "g_08", "g_09",
  };
  for (size_t i = 0; i < kGlobalNames.size(); ++i) {
    const int val = static_cast<int>(100 + i);
    ASSERT_EQ(NC_NOERR, nc_put_att_int(file, NC_GLOBAL, kGlobalNames[i], NC_INT,
                                       1, &val));
  }

  // Variable attributes on v0 including UTF-8 NFC "café", while v1 has none.
  const int v0_a = 7;
  const int v0_b = 9;
  ASSERT_EQ(NC_NOERR, nc_put_att_int(file, var0, "local_a", NC_INT, 1, &v0_a));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_int(file, var0, "caf\xC3\xA9", NC_INT, 1, &v0_b));

  int natts = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_natts(file, &natts));
  EXPECT_EQ(10, natts);
  int v0_natts = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varnatts(file, var0, &v0_natts));
  EXPECT_EQ(2, v0_natts);
  int v1_natts = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varnatts(file, var1, &v1_natts));
  EXPECT_EQ(0, v1_natts);

  for (int idx = 0; idx < natts; ++idx) {
    char qname[NC_MAX_NAME + 1]{};
    ASSERT_EQ(NC_NOERR, nc_inq_attname(file, NC_GLOBAL, idx, qname));
    EXPECT_STREQ(kGlobalNames[static_cast<size_t>(idx)], qname);
    int qid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_attid(file, NC_GLOBAL, qname, &qid));
    EXPECT_EQ(idx, qid);
  }

  // Nullable output pointers on nc_inq_att / nc_inq_attid.
  nc_type qtype = NC_NAT;
  size_t qlen = 0;
  EXPECT_EQ(NC_NOERR, nc_inq_att(file, NC_GLOBAL, "alpha", nullptr, nullptr));
  EXPECT_EQ(NC_NOERR, nc_inq_att(file, NC_GLOBAL, "alpha", &qtype, nullptr));
  EXPECT_EQ(NC_INT, qtype);
  EXPECT_EQ(NC_NOERR, nc_inq_att(file, NC_GLOBAL, "alpha", nullptr, &qlen));
  EXPECT_EQ(1U, qlen);
  EXPECT_EQ(NC_NOERR, nc_inq_atttype(file, NC_GLOBAL, "alpha", &qtype));
  EXPECT_EQ(NC_INT, qtype);
  EXPECT_EQ(NC_NOERR, nc_inq_attlen(file, NC_GLOBAL, "alpha", &qlen));
  EXPECT_EQ(1U, qlen);
  EXPECT_EQ(NC_NOERR, nc_inq_attid(file, NC_GLOBAL, "alpha", nullptr));

  // Prefix miss, same-length miss against "alpha", and longer miss.
  EXPECT_EQ(NC_ENOTATT, nc_inq_attid(file, NC_GLOBAL, "alp", nullptr));
  EXPECT_EQ(NC_ENOTATT, nc_inq_attid(file, NC_GLOBAL, "bbbbb", nullptr));
  EXPECT_EQ(NC_ENOTATT, nc_inq_attid(file, NC_GLOBAL, "alpha_", nullptr));

  // Attribute lookup on variable with 0 attributes hits nelems == 0 fast path.
  EXPECT_EQ(NC_ENOTATT, nc_inq_att(file, var1, "anything", &qtype, &qlen));

  // UTF-8 NFD query "cafe\u0301" matches NFC attribute "caf\u00e9".
  int utf8_id = -1;
  int utf8_val = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_attid(file, var0, "cafe\xCC\x81", &utf8_id));
  EXPECT_EQ(1, utf8_id);
  ASSERT_EQ(NC_NOERR, nc_get_att_int(file, var0, "cafe\xCC\x81", &utf8_val));
  EXPECT_EQ(9, utf8_val);
}

TEST(AttrTest, OverwriteRenameDeleteAndRedefAbortLifecycle) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR, nc_create_mem("lifecycle.nc", NC_CDF5, 4096, file.ptr()));

  int dimid = -1;
  int varid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d", 2, &dimid));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "v", NC_INT, 1, &dimid, &varid));

  // Variable NC_FillValue attribute works cleanly in NetCDF-3.
  const int fill_v = -9999;
  ASSERT_EQ(NC_NOERR,
            nc_put_att_int(file, varid, NC_FillValue, NC_INT, 1, &fill_v));

  const double d2[2] = {1.5, 2.5};
  ASSERT_EQ(NC_NOERR,
            nc_put_att_double(file, NC_GLOBAL, "first", NC_DOUBLE, 2, d2));
  const int i1 = 11;
  ASSERT_EQ(NC_NOERR,
            nc_put_att_int(file, NC_GLOBAL, "middle", NC_INT, 1, &i1));
  const int i2 = 22;
  ASSERT_EQ(NC_NOERR, nc_put_att_int(file, NC_GLOBAL, "last", NC_INT, 1, &i2));

  // Overwrite in define mode frees old NC_attr and replaces slot in-place.
  const double d3[3] = {4.0, 5.0, 6.0};
  ASSERT_EQ(NC_NOERR,
            nc_put_att_double(file, NC_GLOBAL, "first", NC_DOUBLE, 3, d3));
  int first_id = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_attid(file, NC_GLOBAL, "first", &first_id));
  EXPECT_EQ(0, first_id);

  ASSERT_EQ(NC_NOERR, nc_enddef(file));

  // Data-mode attribute writes: growing xsz fails, <= xsz succeeds, new fails.
  const double d4[4] = {1.0, 2.0, 3.0, 4.0};
  EXPECT_EQ(NC_ENOTINDEFINE,
            nc_put_att_double(file, NC_GLOBAL, "first", NC_DOUBLE, 4, d4));
  EXPECT_EQ(NC_ENOTINDEFINE,
            nc_put_att_int(file, NC_GLOBAL, "new_in_data", NC_INT, 1, &i1));

  // Shrink NC_DOUBLE[3] (24 bytes) -> NC_INT[1] (4 bytes).
  const int shrunk = 77;
  ASSERT_EQ(NC_NOERR,
            nc_put_att_int(file, NC_GLOBAL, "first", NC_INT, 1, &shrunk));
  // Same xsz overwrite NC_INT[1] -> NC_FLOAT[1].
  const float same_sz = 3.25F;
  ASSERT_EQ(NC_NOERR,
            nc_put_att_float(file, NC_GLOBAL, "first", NC_FLOAT, 1, &same_sz));
  // Shrink to zero-length in data mode.
  ASSERT_EQ(NC_NOERR,
            nc_put_att_int(file, NC_GLOBAL, "middle", NC_INT, 0, nullptr));

  // Data-mode rename & delete rules.
  EXPECT_EQ(NC_ENOTINDEFINE, nc_del_att(file, NC_GLOBAL, "last"));
  EXPECT_EQ(NC_ENOTINDEFINE,
            nc_rename_att(file, NC_GLOBAL, "last", "last_longer"));
  ASSERT_EQ(NC_NOERR, nc_rename_att(file, NC_GLOBAL, "last", "lst"));
  // Expanding back to original capacity (4 chars from "last") succeeds in
  // data mode and keeps name->nchars == 4 for subsequent deletion in redef.
  ASSERT_EQ(NC_NOERR, nc_rename_att(file, NC_GLOBAL, "lst", "zedd"));

  ASSERT_EQ(NC_NOERR, nc_redef(file));

  // Rename collisions in define mode.
  EXPECT_EQ(NC_ENAMEINUSE, nc_rename_att(file, NC_GLOBAL, "first", "zedd"));
  EXPECT_EQ(NC_ENAMEINUSE, nc_rename_att(file, NC_GLOBAL, "first", "first"));
  // Growing rename succeeds in define mode.
  ASSERT_EQ(NC_NOERR,
            nc_rename_att(file, NC_GLOBAL, "first", "renamed_longer_first"));

  // Delete middle (shuffle last down), then first, then last.
  ASSERT_EQ(NC_NOERR, nc_del_att(file, NC_GLOBAL, "middle"));
  int zed_id = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_attid(file, NC_GLOBAL, "zedd", &zed_id));
  EXPECT_EQ(1, zed_id);
  ASSERT_EQ(NC_NOERR, nc_del_att(file, NC_GLOBAL, "renamed_longer_first"));
  ASSERT_EQ(NC_NOERR, nc_inq_attid(file, NC_GLOBAL, "zedd", &zed_id));
  EXPECT_EQ(0, zed_id);
  ASSERT_EQ(NC_NOERR, nc_del_att(file, NC_GLOBAL, "zedd"));

  int natts = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_natts(file, &natts));
  EXPECT_EQ(0, natts);

  // Separate dataset for redef() -> dup_NC3INFO/dup_NC_attrarrayV -> abort().
  ScopedNcFile redef_file;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("redef_abort.nc", NC_CDF5, 2048, redef_file.ptr()));
  int rdim = -1;
  int rvar_with_atts = -1;
  int rvar_empty = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(redef_file, "rd", 2, &rdim));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(redef_file, "va", NC_INT, 1, &rdim, &rvar_with_atts));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(redef_file, "vb", NC_INT, 1, &rdim, &rvar_empty));
  const int gv = 5;
  ASSERT_EQ(NC_NOERR, nc_put_att_int(redef_file, NC_GLOBAL, "g_nonempty",
                                     NC_INT, 1, &gv));
  ASSERT_EQ(NC_NOERR, nc_put_att_int(redef_file, NC_GLOBAL, "g_empty", NC_INT,
                                     0, nullptr));
  ASSERT_EQ(NC_NOERR, nc_put_att_int(redef_file, rvar_with_atts, "v_nonempty",
                                     NC_INT, 1, &gv));
  ASSERT_EQ(NC_NOERR, nc_enddef(redef_file));

  ASSERT_EQ(NC_NOERR, nc_redef(redef_file));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(redef_file, NC_GLOBAL, "discarded", 3, "tmp"));
  ASSERT_EQ(NC_NOERR, redef_file.abort());
}

TEST(AttrTest, CopyAttributeBetweenDatasetsAndFormats) {
  ScopedNcFile cdf5_file;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("copy_src.nc", NC_CDF5, 4096, cdf5_file.ptr()));
  int src_dim = -1;
  int src_var = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(cdf5_file, "d", 3, &src_dim));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(cdf5_file, "vs", NC_INT, 1, &src_dim, &src_var));

  const std::array<int, 2> src_ints = {12, -34};
  ASSERT_EQ(NC_NOERR, nc_put_att_int(cdf5_file, NC_GLOBAL, "g_ints", NC_INT,
                                     src_ints.size(), src_ints.data()));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(cdf5_file, src_var, "units", 6, "meters"));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_int(cdf5_file, src_var, "empty", NC_INT, 0, nullptr));
  const unsigned long long src_u64 = 9000000000ULL;
  ASSERT_EQ(NC_NOERR, nc_put_att_ulonglong(cdf5_file, NC_GLOBAL, "u64_only",
                                           NC_UINT64, 1, &src_u64));

  // Same-file, same-var copy is a no-op returning NC_NOERR.
  EXPECT_EQ(NC_NOERR,
            nc_copy_att(cdf5_file, src_var, "units", cdf5_file, src_var));
  // Missing source attribute returns NC_ENOTATT.
  EXPECT_EQ(NC_ENOTATT,
            nc_copy_att(cdf5_file, src_var, "missing", cdf5_file, NC_GLOBAL));

  ScopedNcFile classic_file;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("copy_dst.nc", 0, 4096, classic_file.ptr()));
  int dst_dim = -1;
  int dst_var = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(classic_file, "d", 3, &dst_dim));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(classic_file, "vd", NC_INT, 1, &dst_dim, &dst_var));

  ASSERT_EQ(NC_NOERR, nc_copy_att(cdf5_file, NC_GLOBAL, "g_ints", classic_file,
                                  NC_GLOBAL));
  ASSERT_EQ(NC_NOERR,
            nc_copy_att(cdf5_file, src_var, "units", classic_file, dst_var));
  ASSERT_EQ(NC_NOERR,
            nc_copy_att(cdf5_file, src_var, "empty", classic_file, dst_var));
  // CDF-5 type NC_UINT64 cannot be copied into Classic format.
  EXPECT_EQ(NC_EBADTYPE, nc_copy_att(cdf5_file, NC_GLOBAL, "u64_only",
                                     classic_file, NC_GLOBAL));

  std::array<int, 2> dst_ints = {-1, -1};
  ASSERT_EQ(NC_NOERR,
            nc_get_att_int(classic_file, NC_GLOBAL, "g_ints", dst_ints.data()));
  EXPECT_THAT(dst_ints, ElementsAre(12, -34));

  char units_buf[7]{};
  ASSERT_EQ(NC_NOERR,
            nc_get_att_text(classic_file, dst_var, "units", units_buf));
  EXPECT_EQ(0, std::memcmp(units_buf, "meters", 6));

  ASSERT_EQ(NC_NOERR, nc_enddef(classic_file));
  // `cdf5_file` remains in define mode. Copy into existing attribute with
  // smaller/equal size succeeds in `classic_file` data mode; new or growing
  // copy fails with NC_ENOTINDEFINE.
  const std::array<int, 1> small_int = {99};
  ASSERT_EQ(NC_NOERR, nc_put_att_int(cdf5_file, NC_GLOBAL, "g_ints", NC_INT, 1,
                                     small_int.data()));
  ASSERT_EQ(NC_NOERR, nc_copy_att(cdf5_file, NC_GLOBAL, "g_ints", classic_file,
                                  NC_GLOBAL));
  int updated_int = -1;
  ASSERT_EQ(NC_NOERR,
            nc_get_att_int(classic_file, NC_GLOBAL, "g_ints", &updated_int));
  EXPECT_EQ(99, updated_int);
  EXPECT_EQ(NC_ENOTINDEFINE,
            nc_copy_att(cdf5_file, src_var, "units", classic_file, NC_GLOBAL));
}

TEST(AttrTest, ParameterValidationAndErrorPaths) {
  ScopedMemio ro_memio;
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("validation.nc", NC_CDF5, 4096, file.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "v", NC_INT, 1, &dimid, &varid));

    const int i32 = 42;
    ASSERT_EQ(NC_NOERR, nc_put_att_int(file, NC_GLOBAL, "i", NC_INT, 1, &i32));
    ASSERT_EQ(NC_NOERR, nc_put_att_text(file, NC_GLOBAL, "c", 3, "abc"));

    // NC_EBADID through public API dispatchers.
    char name_buf[NC_MAX_NAME + 1]{};
    nc_type qtype = NC_NAT;
    size_t qlen = 0;
    int qid = -1;
    int val = 0;
    EXPECT_EQ(NC_EBADID, nc_inq_attname(-1, NC_GLOBAL, 0, name_buf));
    EXPECT_EQ(NC_EBADID, nc_inq_attid(-1, NC_GLOBAL, "i", &qid));
    EXPECT_EQ(NC_EBADID, nc_inq_att(-1, NC_GLOBAL, "i", &qtype, &qlen));
    EXPECT_EQ(NC_EBADID, nc_rename_att(-1, NC_GLOBAL, "i", "j"));
    EXPECT_EQ(NC_EBADID, nc_del_att(-1, NC_GLOBAL, "i"));
    EXPECT_EQ(NC_EBADID, nc_put_att_int(-1, NC_GLOBAL, "i", NC_INT, 1, &val));
    EXPECT_EQ(NC_EBADID, nc_get_att_int(-1, NC_GLOBAL, "i", &val));

    // Invalid varid (-2 and 99) across all attribute operations.
    for (const int bad_varid : {-2, 99}) {
      EXPECT_EQ(NC_ENOTVAR, nc_inq_attname(file, bad_varid, 0, name_buf));
      EXPECT_EQ(NC_ENOTVAR, nc_inq_attid(file, bad_varid, "i", &qid));
      EXPECT_EQ(NC_ENOTVAR, nc_inq_att(file, bad_varid, "i", &qtype, &qlen));
      EXPECT_EQ(NC_ENOTVAR, nc_rename_att(file, bad_varid, "i", "j"));
      EXPECT_EQ(NC_ENOTVAR, nc_del_att(file, bad_varid, "i"));
      EXPECT_EQ(NC_ENOTVAR,
                nc_put_att_int(file, bad_varid, "i", NC_INT, 1, &val));
      EXPECT_EQ(NC_ENOTVAR, nc_get_att_int(file, bad_varid, "i", &val));
    }

    // Out-of-bounds attnum and missing attribute name.
    EXPECT_EQ(NC_ENOTATT, nc_inq_attname(file, NC_GLOBAL, -1, name_buf));
    EXPECT_EQ(NC_ENOTATT, nc_inq_attname(file, NC_GLOBAL, 2, name_buf));
    EXPECT_EQ(NC_ENOTATT, nc_inq_attid(file, NC_GLOBAL, "missing", &qid));
    EXPECT_EQ(NC_ENOTATT,
              nc_inq_att(file, NC_GLOBAL, "missing", &qtype, &qlen));
    EXPECT_EQ(NC_ENOTATT, nc_rename_att(file, NC_GLOBAL, "missing", "newname"));
    EXPECT_EQ(NC_ENOTATT, nc_del_att(file, NC_GLOBAL, "missing"));
    EXPECT_EQ(NC_ENOTATT, nc_get_att_int(file, NC_GLOBAL, "missing", &val));

    // Safe NULL attribute name checks (NC_lookupattr and NC3_put_att).
    EXPECT_EQ(NC_EBADNAME, nc_inq_att(file, NC_GLOBAL, nullptr, &qtype, &qlen));
    EXPECT_EQ(NC_EBADNAME, nc_get_att_int(file, NC_GLOBAL, nullptr, &val));
    EXPECT_EQ(NC_EBADNAME,
              nc_put_att_int(file, NC_GLOBAL, nullptr, NC_INT, 1, &val));

    // Invalid names and invalid UTF-8 sequences.
    for (const char* bad_name : {"", "/slash", " lead", "trail ", "\xFF\xFE"}) {
      SCOPED_TRACE(bad_name);
      EXPECT_EQ(NC_EBADNAME,
                nc_put_att_int(file, NC_GLOBAL, bad_name, NC_INT, 1, &val));
      EXPECT_EQ(NC_EBADNAME, nc_rename_att(file, NC_GLOBAL, "i", bad_name));
    }
    const std::string max_name(NC_MAX_NAME, 'a');
    ASSERT_EQ(NC_NOERR, nc_put_att_int(file, NC_GLOBAL, max_name.c_str(),
                                       NC_INT, 1, &val));
    char max_out[NC_MAX_NAME + 1]{};
    ASSERT_EQ(NC_NOERR, nc_inq_attname(file, NC_GLOBAL, 2, max_out));
    EXPECT_EQ(max_name, max_out);

    const std::string over_max_name(NC_MAX_NAME + 1, 'a');
    EXPECT_EQ(NC_EMAXNAME,
              nc_put_att_int(file, NC_GLOBAL, over_max_name.c_str(), NC_INT, 1,
                             &val));
    EXPECT_EQ(NC_EMAXNAME,
              nc_rename_att(file, NC_GLOBAL, "i", over_max_name.c_str()));

    // Invalid UTF-8 on lookup/delete hits nc_utf8_normalize() failure branches.
    EXPECT_EQ(NC_ENOTATT, nc_inq_attid(file, NC_GLOBAL, "\xFF\xFE", &qid));
    EXPECT_EQ(NC_ENOTATT,
              nc_rename_att(file, NC_GLOBAL, "\xFF\xFE", "valid_target"));
    EXPECT_EQ(NC_EBADNAME, nc_del_att(file, NC_GLOBAL, "\xFF\xFE"));
    // Same-length non-matching attribute name on delete.
    EXPECT_EQ(NC_ENOTATT, nc_del_att(file, NC_GLOBAL, "z"));

    // Invalid types in CDF-5.
    const char* str_ptr = "abc";
    EXPECT_EQ(NC_EBADTYPE,
              nc_put_att_string(file, NC_GLOBAL, "bad_str", 1, &str_ptr));
    EXPECT_EQ(NC_EBADTYPE,
              nc_put_att(file, NC_GLOBAL, "bad_nat", NC_NAT, 1, &val));

    // NC_ECHAR conversions on put and get.
    EXPECT_EQ(NC_ECHAR,
              nc_put_att_int(file, NC_GLOBAL, "bad_c1", NC_CHAR, 1, &val));
    EXPECT_EQ(NC_ECHAR, nc_get_att_int(file, NC_GLOBAL, "c", &val));
    EXPECT_EQ(NC_ECHAR, nc_get_att_text(file, NC_GLOBAL, "i", name_buf));

    // nelems > X_INT_MAX and NULL value with nelems > 0.
    EXPECT_EQ(NC_EINVAL,
              nc_put_att_int(file, NC_GLOBAL, "too_many", NC_INT,
                             static_cast<size_t>(X_INT_MAX) + 1U, &val));
    EXPECT_EQ(NC_EINVAL,
              nc_put_att_int(file, NC_GLOBAL, "null_val", NC_INT, 2, nullptr));

    // Safe NC_ERANGE checks (avoid float-to-int overflow UB when ERANGE_FILL
    // is disabled).
    const int big_int = 300;
    EXPECT_EQ(NC_ERANGE,
              nc_put_att_int(file, NC_GLOBAL, "ovf_i8", NC_BYTE, 1, &big_int));
    const signed char neg_i8 = -5;
    EXPECT_EQ(NC_ERANGE, nc_put_att_schar(file, NC_GLOBAL, "ovf_u8", NC_UBYTE,
                                          1, &neg_i8));
    const double huge_f64 = 1e100;
    EXPECT_EQ(NC_ERANGE, nc_put_att_double(file, NC_GLOBAL, "ovf_f32", NC_FLOAT,
                                           1, &huge_f64));

    const int big_i32 = 70000;
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(file, NC_GLOBAL, "big32", NC_INT, 1, &big_i32));
    short short_out = 0;
    EXPECT_EQ(NC_ERANGE,
              nc_get_att_short(file, NC_GLOBAL, "big32", &short_out));

    const short neg_i16 = -5;
    ASSERT_EQ(NC_NOERR, nc_put_att_short(file, NC_GLOBAL, "neg16", NC_SHORT, 1,
                                         &neg_i16));
    unsigned short ushort_out = 0;
    EXPECT_EQ(NC_ERANGE,
              nc_get_att_ushort(file, NC_GLOBAL, "neg16", &ushort_out));

    // NC_STRING memtype read on NetCDF-3 attribute returns NC_EBADTYPE.
    char* str_out = nullptr;
    EXPECT_EQ(NC_EBADTYPE, nc_get_att_string(file, NC_GLOBAL, "i", &str_out));
    EXPECT_EQ(nullptr, str_out);

    const int writer_id = file.release();
    ASSERT_EQ(NC_NOERR, nc_close_memio(writer_id, ro_memio.get()));
  }

  // Read-only NC_NOWRITE dataset rejects put/rename with NC_EPERM, while
  // NC3_del_att checks !NC_indef() first and returns NC_ENOTINDEFINE.
  ScopedNcFile ro_file;
  ASSERT_EQ(NC_NOERR, nc_open_mem("validation.nc", NC_NOWRITE, ro_memio.size(),
                                  ro_memio.get()->memory, ro_file.ptr()));
  const int v = 1;
  EXPECT_EQ(NC_EPERM, nc_put_att_int(ro_file, NC_GLOBAL, "ro", NC_INT, 1, &v));
  EXPECT_EQ(NC_EPERM, nc_rename_att(ro_file, NC_GLOBAL, "i", "i2"));
  EXPECT_EQ(NC_ENOTINDEFINE, nc_del_att(ro_file, NC_GLOBAL, "i"));
}

TEST(AttrTest, DirectInternalDispatchAndAttrHelpers) {
  // Direct NC3_* dispatch checks for NC_check_id failure paths.
  char tmp_name[NC_MAX_NAME + 1]{};
  nc_type tmp_type = NC_NAT;
  size_t tmp_len = 0;
  int tmp_attid = -1;
  int tmp_val = 0;
  EXPECT_EQ(NC_EBADID, NC3_inq_attname(-1, NC_GLOBAL, 0, tmp_name));
  EXPECT_EQ(NC_EBADID, NC3_inq_attid(-1, NC_GLOBAL, "a", &tmp_attid));
  EXPECT_EQ(NC_EBADID, NC3_inq_att(-1, NC_GLOBAL, "a", &tmp_type, &tmp_len));
  EXPECT_EQ(NC_EBADID, NC3_rename_att(-1, NC_GLOBAL, "a", "b"));
  EXPECT_EQ(NC_EBADID, NC3_del_att(-1, NC_GLOBAL, "a"));
  EXPECT_EQ(NC_EBADID,
            NC3_put_att(-1, NC_GLOBAL, "a", NC_INT, 1, &tmp_val, NC_INT));
  EXPECT_EQ(NC_EBADID, NC3_get_att(-1, NC_GLOBAL, "a", &tmp_val, NC_INT));

  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("direct_nc3.nc", NC_CDF5, 4096, file.ptr()));

    // Direct NC3_put_att error & NC_NAT branches bypassed by public wrappers.
    const char ch = 'z';
    EXPECT_EQ(NC_ECHAR, NC3_put_att(file, NC_GLOBAL, "bad_memchar", NC_INT, 1,
                                    &ch, NC_CHAR));
    const std::array<int, 2> nat_in = {15, -25};
    ASSERT_EQ(NC_NOERR, NC3_put_att(file, NC_GLOBAL, "nat_pair", NC_INT,
                                    nat_in.size(), nat_in.data(), NC_NAT));

    ASSERT_EQ(NC_NOERR, nc_enddef(file));

    std::array<int, 2> nat_out = {-1, -1};
    ASSERT_EQ(NC_NOERR,
              NC3_get_att(file, NC_GLOBAL, "nat_pair", nat_out.data(), NC_NAT));
    EXPECT_THAT(nat_out, ElementsAre(15, -25));

    // Overwriting an existing initialized NC_INT attribute in data mode with
    // memtype == NC_STRING exercises dispatchput()'s default: NC_EBADTYPE.
    const char* ignored = "str";
    EXPECT_EQ(NC_EBADTYPE, NC3_put_att(file, NC_GLOBAL, "nat_pair", NC_INT, 2,
                                       &ignored, NC_STRING));

    // Re-write valid initialized data, then enable NC_HSYNC to exercise
    // NC_doHsync() -> NC_sync() inside NC3_put_att and NC3_rename_att.
    NC* nc = nullptr;
    ASSERT_EQ(NC_NOERR, NC_check_id(file, &nc));
    ASSERT_NE(nullptr, nc);
    fSet(NC3_DATA(nc)->state, NC_HSYNC);
    absl::Cleanup clear_hsync = [nc]() { fClr(NC3_DATA(nc)->state, NC_HSYNC); };

    const std::array<int, 2> updated = {101, 202};
    ASSERT_EQ(NC_NOERR, nc_put_att_int(file, NC_GLOBAL, "nat_pair", NC_INT,
                                       updated.size(), updated.data()));
    EXPECT_EQ(0, NC_hdirty(NC3_DATA(nc)));

    ASSERT_EQ(NC_NOERR, nc_rename_att(file, NC_GLOBAL, "nat_pair", "nat2"));
    EXPECT_EQ(0, NC_hdirty(NC3_DATA(nc)));

    std::array<int, 2> synced_out = {-1, -1};
    ASSERT_EQ(NC_NOERR,
              nc_get_att_int(file, NC_GLOBAL, "nat2", synced_out.data()));
    EXPECT_THAT(synced_out, ElementsAre(101, 202));
  }

  // Pure attr.c internal helpers: free_NC_attr(NULL), size_t overflow guard in
  // new_x_NC_attr(), and NC_attrarray lifecycle functions.
  free_NC_attr(nullptr);

  NC_string* overflow_str = new_NC_string(4, "ovfl");
  ASSERT_NE(nullptr, overflow_str);
  std::unique_ptr<NC_string, decltype(&free_NC_string)> overflow_str_guard(
      overflow_str, &free_NC_string);
  EXPECT_EQ(nullptr, new_x_NC_attr(overflow_str, NC_DOUBLE, SIZE_MAX / 8));

  NC_attrarray empty_arr{};
  absl::Cleanup free_empty_arr = [&empty_arr]() {
    free_NC_attrarrayV(&empty_arr);
  };
  EXPECT_EQ(nullptr, elem_NC_attrarray(&empty_arr, 0));
  EXPECT_EQ(nullptr, NC_findattr(&empty_arr, "missing"));

  NC_attrarray empty_dup{};
  absl::Cleanup free_empty_dup = [&empty_dup]() {
    free_NC_attrarrayV(&empty_dup);
  };
  ASSERT_EQ(NC_NOERR, dup_NC_attrarrayV(&empty_dup, &empty_arr));
  EXPECT_EQ(0U, empty_dup.nelems);
  free_NC_attrarrayV0(&empty_dup);
  free_NC_attrarrayV(&empty_dup);

  NC_attrarray src_arr{};
  absl::Cleanup free_src_arr = [&src_arr]() { free_NC_attrarrayV(&src_arr); };
  src_arr.value = static_cast<NC_attr**>(std::calloc(2, sizeof(NC_attr*)));
  ASSERT_NE(nullptr, src_arr.value);
  src_arr.nalloc = 2;

  NC_string* s0 = new_NC_string(5, "alpha");
  ASSERT_NE(nullptr, s0);
  NC_attr* a0 = new_x_NC_attr(s0, NC_INT, 1);
  if (a0 == nullptr) {
    free_NC_string(s0);
    FAIL() << "new_x_NC_attr failed";
  }
  std::memset(a0->xvalue, 0, a0->xsz);
  src_arr.value[src_arr.nelems++] = a0;

  NC_string* s1 = new_NC_string(4, "zero");
  ASSERT_NE(nullptr, s1);
  NC_attr* a1 = new_x_NC_attr(s1, NC_DOUBLE, 0);
  if (a1 == nullptr) {
    free_NC_string(s1);
    FAIL() << "new_x_NC_attr failed";
  }
  src_arr.value[src_arr.nelems++] = a1;

  EXPECT_EQ(a0, elem_NC_attrarray(&src_arr, 0));
  EXPECT_EQ(a1, elem_NC_attrarray(&src_arr, 1));
  EXPECT_EQ(nullptr, elem_NC_attrarray(&src_arr, 2));
  EXPECT_NE(nullptr, NC_findattr(&src_arr, "alpha"));
  EXPECT_EQ(nullptr, NC_findattr(&src_arr, "bravo"));
  EXPECT_EQ(nullptr, NC_findattr(&src_arr, "\xFF\xFE"));

  NC_attrarray src_dup{};
  absl::Cleanup free_src_dup = [&src_dup]() { free_NC_attrarrayV(&src_dup); };
  ASSERT_EQ(NC_NOERR, dup_NC_attrarrayV(&src_dup, &src_arr));
  EXPECT_EQ(2U, src_dup.nelems);
  EXPECT_NE(nullptr, elem_NC_attrarray(&src_dup, 0));
  EXPECT_NE(nullptr, elem_NC_attrarray(&src_dup, 1));

  free_NC_attrarrayV0(&src_dup);
  EXPECT_EQ(0U, src_dup.nelems);
  free_NC_attrarrayV0(&src_dup);
  free_NC_attrarrayV(&src_dup);
  free_NC_attrarrayV(&src_dup);
}

}  // namespace
}  // namespace netcdf
