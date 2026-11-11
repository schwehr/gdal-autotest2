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

// Unit tests for //third_party/netcdf/libsrc/putget.c.
//
// Covers the full classic/64-bit-offset/CDF-5 conversion matrix across empty,
// multi-chunk, out-of-range, and truncated memory reads; fill-value handling;
// record growth (special single recvar, multi recvar, NC_NOFILL, NC_SHARE);
// multidimensional odometer slicing; and direct NC3 dispatch validation.

#include <array>
#include <cstddef>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

extern "C" {
#include "third_party/netcdf/include/nc3dispatch.h"
}  // extern "C"

namespace netcdf {
namespace {

struct NumericTypeDesc {
  nc_type xtype;
  const char* name;
  size_t ext_size;
  bool is_signed_int;
  bool is_unsigned_int;
};

constexpr std::array<NumericTypeDesc, 10> kNumericTypes = {{
    {NC_BYTE, "byte", 1, true, false},
    {NC_UBYTE, "ubyte", 1, false, true},
    {NC_SHORT, "short", 2, true, false},
    {NC_USHORT, "ushort", 2, false, true},
    {NC_INT, "int", 4, true, false},
    {NC_UINT, "uint", 4, false, true},
    {NC_INT64, "int64", 8, true, false},
    {NC_UINT64, "uint64", 8, false, true},
    {NC_FLOAT, "float", 4, false, false},
    {NC_DOUBLE, "double", 8, false, false},
}};

template <typename T>
int PutVaraMem(int ncid, int varid, const size_t* start, const size_t* count,
               const T* data);

template <>
int PutVaraMem<signed char>(int ncid, int varid, const size_t* start,
                            const size_t* count, const signed char* data) {
  return nc_put_vara_schar(ncid, varid, start, count, data);
}

template <>
int PutVaraMem<unsigned char>(int ncid, int varid, const size_t* start,
                              const size_t* count, const unsigned char* data) {
  return nc_put_vara_uchar(ncid, varid, start, count, data);
}

template <>
int PutVaraMem<short>(int ncid, int varid, const size_t* start,
                      const size_t* count, const short* data) {
  return nc_put_vara_short(ncid, varid, start, count, data);
}

template <>
int PutVaraMem<unsigned short>(int ncid, int varid, const size_t* start,
                               const size_t* count,
                               const unsigned short* data) {
  return nc_put_vara_ushort(ncid, varid, start, count, data);
}

template <>
int PutVaraMem<int>(int ncid, int varid, const size_t* start,
                    const size_t* count, const int* data) {
  return nc_put_vara_int(ncid, varid, start, count, data);
}

template <>
int PutVaraMem<unsigned int>(int ncid, int varid, const size_t* start,
                             const size_t* count, const unsigned int* data) {
  return nc_put_vara_uint(ncid, varid, start, count, data);
}

template <>
int PutVaraMem<long long>(int ncid, int varid, const size_t* start,
                          const size_t* count, const long long* data) {
  return nc_put_vara_longlong(ncid, varid, start, count, data);
}

template <>
int PutVaraMem<unsigned long long>(int ncid, int varid, const size_t* start,
                                   const size_t* count,
                                   const unsigned long long* data) {
  return nc_put_vara_ulonglong(ncid, varid, start, count, data);
}

template <>
int PutVaraMem<float>(int ncid, int varid, const size_t* start,
                      const size_t* count, const float* data) {
  return nc_put_vara_float(ncid, varid, start, count, data);
}

template <>
int PutVaraMem<double>(int ncid, int varid, const size_t* start,
                       const size_t* count, const double* data) {
  return nc_put_vara_double(ncid, varid, start, count, data);
}

template <typename T>
int GetVaraMem(int ncid, int varid, const size_t* start, const size_t* count,
               T* data);

template <>
int GetVaraMem<signed char>(int ncid, int varid, const size_t* start,
                            const size_t* count, signed char* data) {
  return nc_get_vara_schar(ncid, varid, start, count, data);
}

template <>
int GetVaraMem<unsigned char>(int ncid, int varid, const size_t* start,
                              const size_t* count, unsigned char* data) {
  return nc_get_vara_uchar(ncid, varid, start, count, data);
}

template <>
int GetVaraMem<short>(int ncid, int varid, const size_t* start,
                      const size_t* count, short* data) {
  return nc_get_vara_short(ncid, varid, start, count, data);
}

template <>
int GetVaraMem<unsigned short>(int ncid, int varid, const size_t* start,
                               const size_t* count, unsigned short* data) {
  return nc_get_vara_ushort(ncid, varid, start, count, data);
}

template <>
int GetVaraMem<int>(int ncid, int varid, const size_t* start,
                    const size_t* count, int* data) {
  return nc_get_vara_int(ncid, varid, start, count, data);
}

template <>
int GetVaraMem<unsigned int>(int ncid, int varid, const size_t* start,
                             const size_t* count, unsigned int* data) {
  return nc_get_vara_uint(ncid, varid, start, count, data);
}

template <>
int GetVaraMem<long long>(int ncid, int varid, const size_t* start,
                          const size_t* count, long long* data) {
  return nc_get_vara_longlong(ncid, varid, start, count, data);
}

template <>
int GetVaraMem<unsigned long long>(int ncid, int varid, const size_t* start,
                                   const size_t* count,
                                   unsigned long long* data) {
  return nc_get_vara_ulonglong(ncid, varid, start, count, data);
}

template <>
int GetVaraMem<float>(int ncid, int varid, const size_t* start,
                      const size_t* count, float* data) {
  return nc_get_vara_float(ncid, varid, start, count, data);
}

template <>
int GetVaraMem<double>(int ncid, int varid, const size_t* start,
                       const size_t* count, double* data) {
  return nc_get_vara_double(ncid, varid, start, count, data);
}

template <typename Fn>
void ForEachNumericMemType(Fn&& fn) {
  fn.template operator()<signed char>();
  fn.template operator()<unsigned char>();
  fn.template operator()<short>();
  fn.template operator()<unsigned short>();
  fn.template operator()<int>();
  fn.template operator()<unsigned int>();
  fn.template operator()<long long>();
  fn.template operator()<unsigned long long>();
  fn.template operator()<float>();
  fn.template operator()<double>();
}

// Returns true and sets `*bad_val` when writing `MemT` to `ext` can safely
// trigger NC_ERANGE without encountering float-cast-overflow in ncx when
// ERANGE_FILL is undefined.
template <typename MemT>
bool GetSafePutOverflowValue(const NumericTypeDesc& ext, MemT* bad_val) {
  if constexpr (!std::is_integral_v<MemT>) {
    return false;
  } else if (ext.is_unsigned_int) {
    if constexpr (std::is_signed_v<MemT>) {
      *bad_val = static_cast<MemT>(-1);
      return true;
    } else if (sizeof(MemT) > ext.ext_size) {
      if (ext.ext_size == 1) {
        if constexpr (sizeof(MemT) >= 2 && !std::is_signed_v<MemT>) {
          *bad_val = static_cast<MemT>(256);
          return true;
        }
      } else if (ext.ext_size == 2) {
        if constexpr (sizeof(MemT) >= 4 && !std::is_signed_v<MemT>) {
          *bad_val = static_cast<MemT>(65536);
          return true;
        }
      } else if (ext.ext_size == 4) {
        if constexpr (sizeof(MemT) >= 8 && !std::is_signed_v<MemT>) {
          *bad_val = static_cast<MemT>(4294967296ULL);
          return true;
        }
      }
    }
    return false;
  } else if (ext.is_signed_int) {
    const bool wider_or_same_unsigned =
        sizeof(MemT) > ext.ext_size ||
        (!std::is_signed_v<MemT> && sizeof(MemT) == ext.ext_size);
    if (!wider_or_same_unsigned) {
      return false;
    }
    if (ext.ext_size == 1) {
      if constexpr (!std::is_signed_v<MemT> || sizeof(MemT) >= 2) {
        *bad_val = static_cast<MemT>(128);
        return true;
      }
    } else if (ext.ext_size == 2) {
      if constexpr ((!std::is_signed_v<MemT> && sizeof(MemT) == 2) ||
                    sizeof(MemT) >= 4) {
        *bad_val = static_cast<MemT>(32768);
        return true;
      }
    } else if (ext.ext_size == 4) {
      if constexpr ((!std::is_signed_v<MemT> && sizeof(MemT) == 4) ||
                    sizeof(MemT) >= 8) {
        *bad_val = static_cast<MemT>(2147483648ULL);
        return true;
      }
    } else if (ext.ext_size == 8) {
      if constexpr (!std::is_signed_v<MemT> && sizeof(MemT) >= 8) {
        *bad_val = static_cast<MemT>(9223372036854775808ULL);
        return true;
      }
    }
    return false;
  }
  return false;
}

template <typename MemT>
bool ExpectGetOverflow(const NumericTypeDesc& ext) {
  if constexpr (!std::is_integral_v<MemT>) {
    if constexpr (std::is_same_v<MemT, float>) {
      return ext.xtype == NC_DOUBLE;
    }
    return false;
  } else {
    if (!ext.is_signed_int && !ext.is_unsigned_int) {
      // Stored extreme float/double values exceed all 64-bit integer bounds.
      return true;
    }
    if (ext.is_signed_int && !std::is_signed_v<MemT>) {
      // Negative sentinel in signed external var overflows all unsigned MemTs.
      return true;
    }
    if (ext.is_unsigned_int && std::is_signed_v<MemT> &&
        sizeof(MemT) <= ext.ext_size) {
      // Max unsigned sentinel overflows same-width or narrower signed MemTs.
      return true;
    }
    // Narrower unsigned/signed target where positive sentinel exceeds MemT max.
    return sizeof(MemT) < ext.ext_size;
  }
}

void WriteNativeSentinelPattern(int ncid, int varid, nc_type xtype,
                                size_t len) {
  ASSERT_GE(len, size_t{259});
  const size_t start = 0;
  const size_t count = len;
  switch (xtype) {
    case NC_BYTE: {
      std::vector<signed char> vals(len, 42);
      vals[0] = -1;
      vals[257] = -1;
      ASSERT_EQ(NC_NOERR,
                nc_put_vara_schar(ncid, varid, &start, &count, vals.data()));
      break;
    }
    case NC_UBYTE: {
      std::vector<unsigned char> vals(len, 42);
      vals[0] = 255;
      vals[257] = 255;
      ASSERT_EQ(NC_NOERR,
                nc_put_vara_uchar(ncid, varid, &start, &count, vals.data()));
      break;
    }
    case NC_SHORT: {
      std::vector<short> vals(len, 42);
      vals[0] = -1;
      vals[2] = 200;
      vals[257] = -1;
      vals[258] = 200;
      ASSERT_EQ(NC_NOERR,
                nc_put_vara_short(ncid, varid, &start, &count, vals.data()));
      break;
    }
    case NC_USHORT: {
      std::vector<unsigned short> vals(len, 42);
      vals[0] = 65535;
      vals[257] = 65535;
      ASSERT_EQ(NC_NOERR,
                nc_put_vara_ushort(ncid, varid, &start, &count, vals.data()));
      break;
    }
    case NC_INT: {
      std::vector<int> vals(len, 42);
      vals[0] = -1;
      vals[2] = 70000;
      vals[257] = -1;
      vals[258] = 70000;
      ASSERT_EQ(NC_NOERR,
                nc_put_vara_int(ncid, varid, &start, &count, vals.data()));
      break;
    }
    case NC_UINT: {
      std::vector<unsigned int> vals(len, 42U);
      vals[0] = 4294967295U;
      vals[257] = 4294967295U;
      ASSERT_EQ(NC_NOERR,
                nc_put_vara_uint(ncid, varid, &start, &count, vals.data()));
      break;
    }
    case NC_INT64: {
      std::vector<long long> vals(len, 42LL);
      vals[0] = -1LL;
      vals[2] = 5000000000LL;
      vals[257] = -1LL;
      vals[258] = 5000000000LL;
      ASSERT_EQ(NC_NOERR,
                nc_put_vara_longlong(ncid, varid, &start, &count, vals.data()));
      break;
    }
    case NC_UINT64: {
      std::vector<unsigned long long> vals(len, 42ULL);
      vals[0] = 10000000000000000000ULL;
      vals[257] = 10000000000000000000ULL;
      ASSERT_EQ(NC_NOERR, nc_put_vara_ulonglong(ncid, varid, &start, &count,
                                                vals.data()));
      break;
    }
    case NC_FLOAT: {
      std::vector<float> vals(len, 42.0F);
      vals[0] = 1.0e25F;
      vals[257] = 1.0e25F;
      ASSERT_EQ(NC_NOERR,
                nc_put_vara_float(ncid, varid, &start, &count, vals.data()));
      break;
    }
    case NC_DOUBLE: {
      std::vector<double> vals(len, 42.0);
      vals[0] = 1.0e40;
      vals[257] = 1.0e40;
      ASSERT_EQ(NC_NOERR,
                nc_put_vara_double(ncid, varid, &start, &count, vals.data()));
      break;
    }
    default:
      FAIL() << "Unexpected type: " << xtype;
  }
}

void ReadAndVerifyValue42Native(int ncid, int varid, nc_type xtype,
                                size_t index) {
  const size_t count = 1;
  switch (xtype) {
    case NC_BYTE: {
      signed char val = 0;
      EXPECT_EQ(NC_NOERR, nc_get_vara_schar(ncid, varid, &index, &count, &val));
      EXPECT_EQ(val, 42);
      break;
    }
    case NC_UBYTE: {
      unsigned char val = 0;
      EXPECT_EQ(NC_NOERR, nc_get_vara_uchar(ncid, varid, &index, &count, &val));
      EXPECT_EQ(val, 42U);
      break;
    }
    case NC_SHORT: {
      short val = 0;
      EXPECT_EQ(NC_NOERR, nc_get_vara_short(ncid, varid, &index, &count, &val));
      EXPECT_EQ(val, 42);
      break;
    }
    case NC_USHORT: {
      unsigned short val = 0;
      EXPECT_EQ(NC_NOERR,
                nc_get_vara_ushort(ncid, varid, &index, &count, &val));
      EXPECT_EQ(val, 42U);
      break;
    }
    case NC_INT: {
      int val = 0;
      EXPECT_EQ(NC_NOERR, nc_get_vara_int(ncid, varid, &index, &count, &val));
      EXPECT_EQ(val, 42);
      break;
    }
    case NC_UINT: {
      unsigned int val = 0;
      EXPECT_EQ(NC_NOERR, nc_get_vara_uint(ncid, varid, &index, &count, &val));
      EXPECT_EQ(val, 42U);
      break;
    }
    case NC_INT64: {
      long long val = 0;
      EXPECT_EQ(NC_NOERR,
                nc_get_vara_longlong(ncid, varid, &index, &count, &val));
      EXPECT_EQ(val, 42LL);
      break;
    }
    case NC_UINT64: {
      unsigned long long val = 0;
      EXPECT_EQ(NC_NOERR,
                nc_get_vara_ulonglong(ncid, varid, &index, &count, &val));
      EXPECT_EQ(val, 42ULL);
      break;
    }
    case NC_FLOAT: {
      float val = 0.0F;
      EXPECT_EQ(NC_NOERR, nc_get_vara_float(ncid, varid, &index, &count, &val));
      EXPECT_FLOAT_EQ(val, 42.0F);
      break;
    }
    case NC_DOUBLE: {
      double val = 0.0;
      EXPECT_EQ(NC_NOERR,
                nc_get_vara_double(ncid, varid, &index, &count, &val));
      EXPECT_DOUBLE_EQ(val, 42.0);
      break;
    }
    default:
      FAIL() << "Unexpected type: " << xtype;
  }
}

TEST(PutGetTest, EmptyCountAllTypePairsAndChar) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("empty_matrix.nc", NC_64BIT_DATA, 0, file.ptr()));

  int dimid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d", 4, &dimid));

  int char_varid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(file, "v_char", NC_CHAR, 1, &dimid, &char_varid));

  std::array<int, 10> varids{};
  for (size_t i = 0; i < kNumericTypes.size(); ++i) {
    const std::string vname = absl::StrCat("v_", kNumericTypes[i].name);
    ASSERT_EQ(NC_NOERR, nc_def_var(file, vname.c_str(), kNumericTypes[i].xtype,
                                   1, &dimid, &varids[i]));
  }
  ASSERT_EQ(NC_NOERR, nc_enddef(file));

  const size_t start = 0;
  const size_t zero_count = 0;

  const char text_buf[4] = {'a', 'b', 'c', 'd'};
  char text_out[4] = {'x', 'x', 'x', 'x'};
  EXPECT_EQ(NC_NOERR,
            nc_put_vara_text(file, char_varid, &start, &zero_count, text_buf));
  EXPECT_EQ(NC_NOERR,
            nc_get_vara_text(file, char_varid, &start, &zero_count, text_out));
  EXPECT_EQ(text_out[0], 'x');

  const unsigned char uchar_dummy = 0;
  unsigned char uchar_out = 0;
  EXPECT_EQ(NC_ECHAR, nc_put_vara_uchar(file, char_varid, &start, &zero_count,
                                        &uchar_dummy));
  EXPECT_EQ(NC_ECHAR, nc_get_vara_uchar(file, char_varid, &start, &zero_count,
                                        &uchar_out));

  for (size_t i = 0; i < kNumericTypes.size(); ++i) {
    ForEachNumericMemType([&]<typename MemT>() {
      const MemT in_dummy = static_cast<MemT>(0);
      MemT out_dummy = static_cast<MemT>(7);
      EXPECT_EQ(NC_NOERR, PutVaraMem<MemT>(file, varids[i], &start, &zero_count,
                                           &in_dummy))
          << "var=" << kNumericTypes[i].name;
      EXPECT_EQ(NC_NOERR, GetVaraMem<MemT>(file, varids[i], &start, &zero_count,
                                           &out_dummy))
          << "var=" << kNumericTypes[i].name;
      EXPECT_EQ(out_dummy, static_cast<MemT>(7));
    });
  }
  EXPECT_EQ(NC_NOERR, file.close());

  // Classic NC_BYTE with uchar memtype routes through
  // {put,get}NCvx_uchar_uchar.
  ScopedNcFile classic;
  ASSERT_EQ(NC_NOERR, nc_create("classic_empty.nc", NC_CLOBBER | NC_DISKLESS,
                                classic.ptr()));
  int cdim = -1;
  int cvar = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(classic, "d", 4, &cdim));
  ASSERT_EQ(NC_NOERR, nc_def_var(classic, "b", NC_BYTE, 1, &cdim, &cvar));
  ASSERT_EQ(NC_NOERR, nc_enddef(classic));
  EXPECT_EQ(NC_NOERR, nc_put_vara_uchar(classic, cvar, &start, &zero_count,
                                        &uchar_dummy));
  EXPECT_EQ(NC_NOERR,
            nc_get_vara_uchar(classic, cvar, &start, &zero_count, &uchar_out));
  EXPECT_EQ(NC_NOERR, classic.close());
}

TEST(PutGetTest, MultiChunkFullTypeMatrixRoundTripCdf5) {
  constexpr size_t kLen = 260;
  constexpr size_t kChunkHint = 256;
  TempFile tmp("putget_matrix_roundtrip.nc");

  std::array<int, 10> varids{};
  int char_varid = -1;
  {
    size_t create_chunk = kChunkHint;
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc__create(tmp.c_str(), NC_CLOBBER | NC_64BIT_DATA, 0,
                                   &create_chunk, file.ptr()));
    ASSERT_EQ(create_chunk, kChunkHint);

    int dimid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d", kLen, &dimid));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file, "v_char", NC_CHAR, 1, &dimid, &char_varid));
    for (size_t i = 0; i < kNumericTypes.size(); ++i) {
      const std::string vname = absl::StrCat("v_", kNumericTypes[i].name);
      ASSERT_EQ(NC_NOERR,
                nc_def_var(file, vname.c_str(), kNumericTypes[i].xtype, 1,
                           &dimid, &varids[i]));
    }
    ASSERT_EQ(NC_NOERR, nc_enddef(file));

    const size_t start = 0;
    const size_t count = kLen;

    std::string text_in(kLen, '\0');
    for (size_t i = 0; i < kLen; ++i) {
      text_in[i] = static_cast<char>('A' + (i % 26));
    }
    ASSERT_EQ(NC_NOERR, nc_put_vara_text(file, char_varid, &start, &count,
                                         text_in.data()));
    std::string text_out(kLen, '\0');
    ASSERT_EQ(NC_NOERR, nc_get_vara_text(file, char_varid, &start, &count,
                                         text_out.data()));
    EXPECT_EQ(text_out, text_in);

    size_t mem_idx = 0;
    for (size_t i = 0; i < kNumericTypes.size(); ++i) {
      mem_idx = 0;
      ForEachNumericMemType([&]<typename MemT>() {
        std::vector<MemT> src(kLen);
        for (size_t k = 0; k < kLen; ++k) {
          src[k] = static_cast<MemT>((k + mem_idx) % 100);
        }
        ASSERT_EQ(NC_NOERR,
                  PutVaraMem<MemT>(file, varids[i], &start, &count, src.data()))
            << "var=" << kNumericTypes[i].name << " mem_idx=" << mem_idx;

        std::vector<MemT> dst(kLen, static_cast<MemT>(0));
        ASSERT_EQ(NC_NOERR,
                  GetVaraMem<MemT>(file, varids[i], &start, &count, dst.data()))
            << "var=" << kNumericTypes[i].name << " mem_idx=" << mem_idx;
        EXPECT_EQ(dst[0], src[0]);
        EXPECT_EQ(dst[127], src[127]);
        EXPECT_EQ(dst[256], src[256]);
        EXPECT_EQ(dst[kLen - 1], src[kLen - 1]);
        ++mem_idx;
      });
    }
    EXPECT_EQ(NC_NOERR, file.close());
  }

  // Reopen read-only with 256-byte chunk hint to exercise read-only chunked
  // I/O.
  size_t open_chunk = kChunkHint;
  ScopedNcFile reader;
  ASSERT_EQ(NC_NOERR,
            nc__open(tmp.c_str(), NC_NOWRITE, &open_chunk, reader.ptr()));
  ASSERT_EQ(open_chunk, kChunkHint);

  const size_t start = 0;
  const size_t count = kLen;
  std::string text_check(kLen, '\0');
  ASSERT_EQ(NC_NOERR, nc_get_vara_text(reader, char_varid, &start, &count,
                                       text_check.data()));
  EXPECT_EQ(text_check[0], 'A');
  EXPECT_EQ(text_check[259], static_cast<char>('A' + (259 % 26)));

  for (size_t i = 0; i < kNumericTypes.size(); ++i) {
    std::vector<double> vals(kLen, -1.0);
    ASSERT_EQ(NC_NOERR, GetVaraMem<double>(reader, varids[i], &start, &count,
                                           vals.data()));
    // Final memtype written to each variable was double (mem_idx == 9).
    EXPECT_DOUBLE_EQ(vals[0], 9.0);
    EXPECT_DOUBLE_EQ(vals[256], static_cast<double>((256 + 9) % 100));
    EXPECT_DOUBLE_EQ(vals[259], static_cast<double>((259 + 9) % 100));
  }
  EXPECT_EQ(NC_NOERR, reader.close());
}

TEST(PutGetTest, ConversionOverflowMultiChunkRangeChecksCdf5) {
  constexpr size_t kLen = 260;
  constexpr size_t kChunkHint = 256;
  TempFile tmp("putget_matrix_erange.nc");

  size_t chunk = kChunkHint;
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR, nc__create(tmp.c_str(), NC_CLOBBER | NC_64BIT_DATA, 0,
                                 &chunk, file.ptr()));
  int dimid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d", kLen, &dimid));

  std::array<int, 10> varids{};
  for (size_t i = 0; i < kNumericTypes.size(); ++i) {
    const std::string vname = absl::StrCat("e_", kNumericTypes[i].name);
    ASSERT_EQ(NC_NOERR, nc_def_var(file, vname.c_str(), kNumericTypes[i].xtype,
                                   1, &dimid, &varids[i]));
  }
  ASSERT_EQ(NC_NOERR, nc_enddef(file));

  const size_t start = 0;
  const size_t count = kLen;

  // Part A: putNCvx NC_ERANGE across chunk 0 and chunk >= 1 on safe integer
  // pairs.
  for (size_t i = 0; i < kNumericTypes.size(); ++i) {
    ForEachNumericMemType([&]<typename MemT>() {
      MemT bad_val = static_cast<MemT>(0);
      if (!GetSafePutOverflowValue<MemT>(kNumericTypes[i], &bad_val)) {
        return;
      }
      std::vector<MemT> src(kLen, static_cast<MemT>(42));
      src[0] = bad_val;    // First chunk error (status becomes NC_ERANGE).
      src[257] = bad_val;  // Later chunk error (status remains NC_ERANGE).
      EXPECT_EQ(NC_ERANGE,
                PutVaraMem<MemT>(file, varids[i], &start, &count, src.data()))
          << "put overflow ext=" << kNumericTypes[i].name;
      ReadAndVerifyValue42Native(file, varids[i], kNumericTypes[i].xtype, 1);
      ReadAndVerifyValue42Native(file, varids[i], kNumericTypes[i].xtype, 256);
    });
  }

  // Part B: getNCvx NC_ERANGE across chunk 0 and trailing chunks on all 55
  // pairs.
  for (size_t i = 0; i < kNumericTypes.size(); ++i) {
    WriteNativeSentinelPattern(file, varids[i], kNumericTypes[i].xtype, kLen);
    ForEachNumericMemType([&]<typename MemT>() {
      std::vector<MemT> dst(kLen, static_cast<MemT>(99));
      const int status =
          GetVaraMem<MemT>(file, varids[i], &start, &count, dst.data());
      const int expected =
          ExpectGetOverflow<MemT>(kNumericTypes[i]) ? NC_ERANGE : NC_NOERR;
      EXPECT_EQ(expected, status) << "get ext=" << kNumericTypes[i].name;
      EXPECT_EQ(dst[1], static_cast<MemT>(42));
      EXPECT_EQ(dst[256], static_cast<MemT>(42));
    });
  }
  EXPECT_EQ(NC_NOERR, file.close());
}

TEST(PutGetTest, ClassicOffset64ByteUcharDisklessAndTruncatedMemio) {
  // Classic and 64-bit Offset treat NC_BYTE + uchar memtype as unsigned bytes.
  for (const int format_flag : {0, NC_64BIT_OFFSET}) {
    ScopedNcFile mem_ds;
    ASSERT_EQ(NC_NOERR,
              nc_create("classic_byte_uchar.nc",
                        NC_CLOBBER | NC_DISKLESS | format_flag, mem_ds.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(mem_ds, "d", 4, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(mem_ds, "b", NC_BYTE, 1, &dimid, &varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(mem_ds));

    const size_t start = 0;
    const size_t count = 4;
    const unsigned char in_bytes[4] = {0, 127, 128, 255};
    unsigned char out_bytes[4] = {0, 0, 0, 0};
    EXPECT_EQ(NC_NOERR,
              nc_put_vara_uchar(mem_ds, varid, &start, &count, in_bytes));
    EXPECT_EQ(NC_NOERR,
              nc_get_vara_uchar(mem_ds, varid, &start, &count, out_bytes));
    EXPECT_EQ(out_bytes[2], 128U);
    EXPECT_EQ(out_bytes[3], 255U);
    EXPECT_EQ(NC_NOERR, mem_ds.close());
  }

  // Create a CDF-5 memory dataset where all 11 typed variables follow a 4096B
  // spacer variable, then reopen a truncated slice so reads past EOF hit EPERM.
  constexpr size_t kSpacerCount = 512;  // 512 * 8 = 4096 bytes
  constexpr size_t kSmallCount = 16;    // Multiple of 16 -> zero alignment pad
  constexpr size_t kTypedVarsBytes =
      16 * (1 + 1 + 1 + 2 + 2 + 4 + 4 + 4 + 8 + 8 + 8);
  ScopedMemio memio;
  int char_var = -1;
  std::array<int, 10> varids{};
  int mat_var = -1;
  {
    ScopedNcFile src;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("trunc_src.nc", NC_64BIT_DATA, 0, src.ptr()));
    int d_spacer = -1;
    int d_small = -1;
    int d_mat_a = -1;
    int d_mat_b = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(src, "ds", kSpacerCount, &d_spacer));
    ASSERT_EQ(NC_NOERR, nc_def_dim(src, "d0", kSmallCount, &d_small));
    ASSERT_EQ(NC_NOERR, nc_def_dim(src, "ma", 4, &d_mat_a));
    ASSERT_EQ(NC_NOERR, nc_def_dim(src, "mb", 4, &d_mat_b));

    int spacer_var = -1;
    ASSERT_EQ(NC_NOERR,
              nc_def_var(src, "spacer", NC_DOUBLE, 1, &d_spacer, &spacer_var));
    ASSERT_EQ(NC_NOERR, nc_def_var(src, "vc", NC_CHAR, 1, &d_small, &char_var));

    const int mat_dims[2] = {d_mat_a, d_mat_b};
    for (size_t i = 0; i < kNumericTypes.size(); ++i) {
      const std::string vname = absl::StrCat("v", i);
      if (kNumericTypes[i].xtype == NC_INT) {
        ASSERT_EQ(NC_NOERR, nc_def_var(src, vname.c_str(), NC_INT, 2, mat_dims,
                                       &varids[i]));
        mat_var = varids[i];
      } else {
        ASSERT_EQ(NC_NOERR,
                  nc_def_var(src, vname.c_str(), kNumericTypes[i].xtype, 1,
                             &d_small, &varids[i]));
      }
    }
    ASSERT_EQ(NC_NOERR, nc_enddef(src));

    const std::vector<double> spacer_data(kSpacerCount, 1.5);
    const size_t s_start = 0;
    const size_t s_count = kSpacerCount;
    ASSERT_EQ(NC_NOERR, nc_put_vara_double(src, spacer_var, &s_start, &s_count,
                                           spacer_data.data()));

    const std::string cdata(kSmallCount, 'z');
    const size_t c_start = 0;
    const size_t c_count = kSmallCount;
    ASSERT_EQ(NC_NOERR, nc_put_vara_text(src, char_var, &c_start, &c_count,
                                         cdata.data()));

    for (size_t i = 0; i < kNumericTypes.size(); ++i) {
      const std::vector<int> ivals(kSmallCount, 7);
      if (varids[i] == mat_var) {
        const size_t m_start[2] = {0, 0};
        const size_t m_count[2] = {4, 4};
        ASSERT_EQ(NC_NOERR, nc_put_vara_int(src, varids[i], m_start, m_count,
                                            ivals.data()));
      } else {
        ASSERT_EQ(NC_NOERR, nc_put_vara_int(src, varids[i], &c_start, &c_count,
                                            ivals.data()));
      }
    }
    ASSERT_EQ(NC_NOERR, nc_close_memio(src.release(), memio.get()));
  }

  ASSERT_GT(memio.size(), kTypedVarsBytes + 64);

  // Sanity-check full memory open before testing the truncated view.
  {
    ScopedNcFile full;
    ASSERT_EQ(NC_NOERR, nc_open_mem("full.nc", NC_NOWRITE, memio.size(),
                                    memio.get()->memory, full.ptr()));
    char c_out[kSmallCount] = {};
    const size_t c_start = 0;
    const size_t c_count = kSmallCount;
    ASSERT_EQ(NC_NOERR,
              nc_get_vara_text(full, char_var, &c_start, &c_count, c_out));
    EXPECT_EQ(c_out[0], 'z');
    EXPECT_EQ(NC_NOERR, full.close());
  }

  const size_t trunc_size = memio.size() - kTypedVarsBytes - 64;
  ScopedNcFile trunc;
  ASSERT_EQ(NC_NOERR, nc_open_mem("trunc.nc", NC_NOWRITE, trunc_size,
                                  memio.get()->memory, trunc.ptr()));

  const size_t start1 = 0;
  const size_t count1 = 1;
  char char_out = 'x';
  EXPECT_NE(NC_NOERR,
            nc_get_vara_text(trunc, char_var, &start1, &count1, &char_out));

  // Trigger odometer fatal break on truncated 2D slice.
  const size_t m_start[2] = {0, 0};
  const size_t m_slice[2] = {2, 2};
  int mat_out[4] = {0, 0, 0, 0};
  EXPECT_NE(NC_NOERR,
            nc_get_vara_int(trunc, mat_var, m_start, m_slice, mat_out));

  // Hit the ncio_get error return in all 100 getNCvx_* matrix variants.
  for (size_t i = 0; i < kNumericTypes.size(); ++i) {
    ForEachNumericMemType([&]<typename MemT>() {
      MemT out_val = static_cast<MemT>(0);
      if (varids[i] == mat_var) {
        const size_t m1[2] = {0, 0};
        const size_t c1[2] = {1, 1};
        EXPECT_NE(NC_NOERR,
                  GetVaraMem<MemT>(trunc, varids[i], m1, c1, &out_val));
      } else {
        EXPECT_NE(NC_NOERR, GetVaraMem<MemT>(trunc, varids[i], &start1, &count1,
                                             &out_val));
      }
    });
  }
  EXPECT_EQ(NC_NOERR, trunc.close());
}

TEST(PutGetTest, FillAllDefaultTypesAndCustomAndInvalidFillValues) {
  constexpr size_t kChunkHint = 256;
  TempFile tmp("putget_fill.nc");
  {
    size_t chunk = kChunkHint;
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc__create(tmp.c_str(), NC_CLOBBER | NC_64BIT_DATA, 0,
                                   &chunk, file.ptr()));

    int d_small = -1;
    int d_big_int = -1;
    int d_big_dbl = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "small", 5, &d_small));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "big_int", 200, &d_big_int));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "big_dbl", 37, &d_big_dbl));

    int v_char = -1;
    int v_byte = -1;
    int v_ubyte = -1;
    int v_short = -1;
    int v_ushort = -1;
    int v_int = -1;
    int v_uint = -1;
    int v_int64 = -1;
    int v_uint64 = -1;
    int v_float = -1;
    int v_double = -1;
    int v_big_int = -1;
    int v_big_dbl = -1;
    int v_custom = -1;

    ASSERT_EQ(NC_NOERR, nc_def_var(file, "c", NC_CHAR, 1, &d_small, &v_char));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "b", NC_BYTE, 1, &d_small, &v_byte));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file, "ub", NC_UBYTE, 1, &d_small, &v_ubyte));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "s", NC_SHORT, 1, &d_small, &v_short));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file, "us", NC_USHORT, 1, &d_small, &v_ushort));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "i", NC_INT, 1, &d_small, &v_int));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "ui", NC_UINT, 1, &d_small, &v_uint));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "l", NC_INT64, 1, &d_small, &v_int64));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file, "ul", NC_UINT64, 1, &d_small, &v_uint64));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "f", NC_FLOAT, 1, &d_small, &v_float));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file, "d", NC_DOUBLE, 1, &d_small, &v_double));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file, "bi", NC_INT, 1, &d_big_int, &v_big_int));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file, "bd", NC_DOUBLE, 1, &d_big_dbl, &v_big_dbl));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file, "cust", NC_INT, 1, &d_big_int, &v_custom));

    const int custom_fill = -777;
    ASSERT_EQ(NC_NOERR, nc_def_var_fill(file, v_custom, NC_FILL, &custom_fill));
    ASSERT_EQ(NC_NOERR, nc_enddef(file));

    const size_t idx0 = 0;
    const size_t idx4 = 4;
    char c = 'x';
    signed char b = 0;
    unsigned char ub = 0;
    short s = 0;
    unsigned short us = 0;
    int iv = 0;
    unsigned int uiv = 0;
    long long l = 0;
    unsigned long long ul = 0;
    float f = 0.0F;
    double d = 0.0;

    EXPECT_EQ(NC_NOERR, nc_get_var1_text(file, v_char, &idx4, &c));
    EXPECT_EQ(c, NC_FILL_CHAR);
    EXPECT_EQ(NC_NOERR, nc_get_var1_schar(file, v_byte, &idx4, &b));
    EXPECT_EQ(b, NC_FILL_BYTE);
    EXPECT_EQ(NC_NOERR, nc_get_var1_ubyte(file, v_ubyte, &idx4, &ub));
    EXPECT_EQ(ub, static_cast<unsigned char>(NC_FILL_UBYTE));
    EXPECT_EQ(NC_NOERR, nc_get_var1_short(file, v_short, &idx4, &s));
    EXPECT_EQ(s, NC_FILL_SHORT);
    EXPECT_EQ(NC_NOERR, nc_get_var1_ushort(file, v_ushort, &idx4, &us));
    EXPECT_EQ(us, static_cast<unsigned short>(NC_FILL_USHORT));
    EXPECT_EQ(NC_NOERR, nc_get_var1_int(file, v_int, &idx4, &iv));
    EXPECT_EQ(iv, NC_FILL_INT);
    EXPECT_EQ(NC_NOERR, nc_get_var1_uint(file, v_uint, &idx4, &uiv));
    EXPECT_EQ(uiv, NC_FILL_UINT);
    EXPECT_EQ(NC_NOERR, nc_get_var1_longlong(file, v_int64, &idx4, &l));
    EXPECT_EQ(l, NC_FILL_INT64);
    EXPECT_EQ(NC_NOERR, nc_get_var1_ulonglong(file, v_uint64, &idx4, &ul));
    EXPECT_EQ(ul, NC_FILL_UINT64);
    EXPECT_EQ(NC_NOERR, nc_get_var1_float(file, v_float, &idx4, &f));
    EXPECT_FLOAT_EQ(f, NC_FILL_FLOAT);
    EXPECT_EQ(NC_NOERR, nc_get_var1_double(file, v_double, &idx4, &d));
    EXPECT_DOUBLE_EQ(d, NC_FILL_DOUBLE);

    // Multi-chunk default fill + remainder byte verification.
    const size_t big_last_int = 199;
    const size_t big_last_dbl = 36;
    EXPECT_EQ(NC_NOERR, nc_get_var1_int(file, v_big_int, &idx0, &iv));
    EXPECT_EQ(iv, NC_FILL_INT);
    EXPECT_EQ(NC_NOERR, nc_get_var1_int(file, v_big_int, &big_last_int, &iv));
    EXPECT_EQ(iv, NC_FILL_INT);
    EXPECT_EQ(NC_NOERR, nc_get_var1_double(file, v_big_dbl, &big_last_dbl, &d));
    EXPECT_DOUBLE_EQ(d, NC_FILL_DOUBLE);

    EXPECT_EQ(NC_NOERR, nc_get_var1_int(file, v_custom, &idx0, &iv));
    EXPECT_EQ(iv, custom_fill);
    EXPECT_EQ(NC_NOERR, nc_get_var1_int(file, v_custom, &big_last_int, &iv));
    EXPECT_EQ(iv, custom_fill);
    EXPECT_EQ(NC_NOERR, file.close());
  }

  // Invalid _FillValue type on fixed variable rejects at nc_enddef().
  {
    ScopedNcFile bad_type;
    ASSERT_EQ(NC_NOERR, nc_create_mem("bad_fill_type.nc", NC_64BIT_DATA, 0,
                                      bad_type.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(bad_type, "d", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(bad_type, "b", NC_BYTE, 1, &dimid, &varid));
    const int wrong_fill = 99;
    ASSERT_EQ(NC_NOERR, nc_put_att_int(bad_type, varid, NC_FillValue, NC_INT, 1,
                                       &wrong_fill));
    EXPECT_EQ(NC_EBADTYPE, nc_enddef(bad_type));
    ASSERT_EQ(NC_NOERR, bad_type.abort());
  }

  // Invalid _FillValue element count (nelems == 2) rejects at nc_enddef().
  {
    ScopedNcFile bad_len;
    ASSERT_EQ(NC_NOERR, nc_create_mem("bad_fill_len.nc", NC_64BIT_DATA, 0,
                                      bad_len.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(bad_len, "d", 2, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(bad_len, "i", NC_INT, 1, &dimid, &varid));
    const int two_fills[2] = {1, 2};
    ASSERT_EQ(NC_NOERR, nc_put_att_int(bad_len, varid, NC_FillValue, NC_INT, 2,
                                       two_fills));
    EXPECT_EQ(NC_EBADTYPE, nc_enddef(bad_len));
    ASSERT_EQ(NC_NOERR, bad_len.abort());
  }
}

TEST(PutGetTest, RecordFillSpecialMultiVarNoFillAndShare) {
  // Single 1-byte unpadded record variable -> NCfillspecialrecord().
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("rec_special.nc", NC_64BIT_DATA, 0, file.ptr()));
    int unlim = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "time", NC_UNLIMITED, &unlim));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "b", NC_BYTE, 1, &unlim, &varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(file));

    const size_t idx2 = 2;
    const signed char val = 77;
    ASSERT_EQ(NC_NOERR, nc_put_var1_schar(file, varid, &idx2, &val));

    // Overwriting an existing record skips record allocation growth.
    const size_t idx0 = 0;
    const signed char val0 = 55;
    ASSERT_EQ(NC_NOERR, nc_put_var1_schar(file, varid, &idx0, &val0));

    const size_t start = 0;
    const size_t count = 3;
    signed char out[3] = {0, 0, 0};
    ASSERT_EQ(NC_NOERR, nc_get_vara_schar(file, varid, &start, &count, out));
    EXPECT_EQ(out[0], 55);
    EXPECT_EQ(out[1], NC_FILL_BYTE);
    EXPECT_EQ(out[2], 77);
    EXPECT_EQ(NC_NOERR, file.close());
  }

  // NCfillspecialrecord() error propagation on bad _FillValue attribute.
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc_create_mem("rec_special_err.nc", NC_64BIT_DATA, 0,
                                      file.ptr()));
    int unlim = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "time", NC_UNLIMITED, &unlim));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "b", NC_BYTE, 1, &unlim, &varid));
    const int bad_fill = 5;
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(file, varid, NC_FillValue, NC_INT, 1, &bad_fill));
    ASSERT_EQ(NC_NOERR, nc_enddef(file));

    const size_t idx0 = 0;
    const signed char v = 1;
    EXPECT_EQ(NC_EBADTYPE, nc_put_var1_schar(file, varid, &idx0, &v));
    EXPECT_EQ(NC_NOERR, file.close());
  }

  // Fixed var + 2 record vars -> NCfillrecord() skips fixed var and fills both.
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("rec_multi.nc", NC_64BIT_DATA, 0, file.ptr()));
    int d_fix = -1;
    int unlim = -1;
    int v_fix = -1;
    int v_r1 = -1;
    int v_r2 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "fix", 2, &d_fix));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "time", NC_UNLIMITED, &unlim));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "f", NC_INT, 1, &d_fix, &v_fix));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "r1", NC_INT, 1, &unlim, &v_r1));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "r2", NC_DOUBLE, 1, &unlim, &v_r2));
    ASSERT_EQ(NC_NOERR, nc_enddef(file));

    const size_t idx2 = 2;
    const int v = 123;
    ASSERT_EQ(NC_NOERR, nc_put_var1_int(file, v_r1, &idx2, &v));
    const size_t idx0 = 0;
    double r2_fill = 0.0;
    ASSERT_EQ(NC_NOERR, nc_get_var1_double(file, v_r2, &idx0, &r2_fill));
    EXPECT_DOUBLE_EQ(r2_fill, NC_FILL_DOUBLE);
    EXPECT_EQ(NC_NOERR, file.close());
  }

  // NCfillrecord() error propagation when second recvar has bad _FillValue.
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("rec_multi_err.nc", NC_64BIT_DATA, 0, file.ptr()));
    int unlim = -1;
    int v_r1 = -1;
    int v_r2 = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "time", NC_UNLIMITED, &unlim));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "r1", NC_INT, 1, &unlim, &v_r1));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "r2", NC_SHORT, 1, &unlim, &v_r2));
    const short two_shorts[2] = {10, 20};
    ASSERT_EQ(NC_NOERR, nc_put_att_short(file, v_r2, NC_FillValue, NC_SHORT, 2,
                                         two_shorts));
    ASSERT_EQ(NC_NOERR, nc_enddef(file));

    const size_t idx0 = 0;
    const int v = 42;
    EXPECT_EQ(NC_EBADTYPE, nc_put_var1_int(file, v_r1, &idx0, &v));
    EXPECT_EQ(NC_NOERR, file.close());
  }

  // NC_NOFILL skips fill_NC_var inside NCvnrecs().
  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("rec_nofill.nc", NC_64BIT_DATA, 0, file.ptr()));
    int old_mode = -1;
    ASSERT_EQ(NC_NOERR, nc_set_fill(file, NC_NOFILL, &old_mode));
    EXPECT_EQ(old_mode, NC_FILL);
    int unlim = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "time", NC_UNLIMITED, &unlim));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "r", NC_INT, 1, &unlim, &varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(file));

    const size_t idx2 = 2;
    const int val = 999;
    ASSERT_EQ(NC_NOERR, nc_put_var1_int(file, varid, &idx2, &val));
    size_t nrecs = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_dimlen(file, unlim, &nrecs));
    EXPECT_EQ(nrecs, size_t{3});

    int read_val = 0;
    ASSERT_EQ(NC_NOERR, nc_get_var1_int(file, varid, &idx2, &read_val));
    EXPECT_EQ(read_val, 999);
    ASSERT_EQ(NC_NOERR, nc_set_fill(file, NC_FILL, &old_mode));
    EXPECT_EQ(old_mode, NC_NOFILL);
    EXPECT_EQ(NC_NOERR, file.close());
  }

  // NC_SHARE writer + NC_SHARE/non-NC_SHARE readers exercise
  // write/read_numrecs.
  {
    TempFile tmp("putget_share.nc");
    ScopedNcFile writer;
    ASSERT_EQ(NC_NOERR,
              nc_create(tmp.c_str(), NC_CLOBBER | NC_64BIT_DATA | NC_SHARE,
                        writer.ptr()));
    int unlim = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(writer, "time", NC_UNLIMITED, &unlim));
    ASSERT_EQ(NC_NOERR, nc_def_var(writer, "r", NC_INT, 1, &unlim, &varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(writer));

    ScopedNcFile share_reader;
    ASSERT_EQ(NC_NOERR,
              nc_open(tmp.c_str(), NC_NOWRITE | NC_SHARE, share_reader.ptr()));
    ScopedNcFile stale_reader;
    ASSERT_EQ(NC_NOERR, nc_open(tmp.c_str(), NC_NOWRITE, stale_reader.ptr()));

    const size_t start0 = 0;
    const size_t count2 = 2;
    const int rec_vals[2] = {11, 22};
    ASSERT_EQ(NC_NOERR,
              nc_put_vara_int(writer, varid, &start0, &count2, rec_vals));

    const size_t idx1 = 1;
    const size_t idx5 = 5;
    int got = 0;
    EXPECT_EQ(NC_EINVALCOORDS,
              nc_get_var1_int(stale_reader, varid, &idx1, &got));
    EXPECT_EQ(NC_NOERR, nc_get_var1_int(share_reader, varid, &idx1, &got));
    EXPECT_EQ(got, 22);
    EXPECT_EQ(NC_EINVALCOORDS,
              nc_get_var1_int(share_reader, varid, &idx5, &got));

    EXPECT_EQ(NC_NOERR, stale_reader.close());
    EXPECT_EQ(NC_NOERR, share_reader.close());
    EXPECT_EQ(NC_NOERR, writer.close());
  }
}

TEST(PutGetTest, OdometerMultidimShapesZeroEdgesAndSliceRangeErrors) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("odometer.nc", NC_64BIT_DATA, 0, file.ptr()));

  int d3a = -1;
  int d3b = -1;
  int d4 = -1;
  int du = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d3a", 3, &d3a));
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d3b", 3, &d3b));
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d4", 4, &d4));
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "du", NC_UNLIMITED, &du));

  int v_scalar = -1;
  int v_2d = -1;
  int v_3d = -1;
  int v_2d_short = -1;
  int v_rec_1d_a = -1;
  int v_rec_2d = -1;

  const int dims2[2] = {d3a, d4};
  const int dims3[3] = {d3a, d3b, d4};
  const int dims_rec2[2] = {du, d4};

  ASSERT_EQ(NC_NOERR, nc_def_var(file, "scal", NC_INT, 0, nullptr, &v_scalar));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "m2", NC_INT, 2, dims2, &v_2d));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "m3", NC_INT, 3, dims3, &v_3d));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "m2s", NC_SHORT, 2, dims2, &v_2d_short));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "ru", NC_INT, 1, &du, &v_rec_1d_a));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "r2", NC_INT, 2, dims_rec2, &v_rec_2d));
  ASSERT_EQ(NC_NOERR, nc_enddef(file));

  // 0D scalar with NC3_put_vara / NC3_get_vara (edges == nullptr, memtype ==
  // NC_NAT).
  const int s_in = 88;
  int s_out = 0;
  ASSERT_EQ(NC_NOERR,
            NC3_put_vara(file, v_scalar, nullptr, nullptr, &s_in, NC_NAT));
  ASSERT_EQ(NC_NOERR,
            NC3_get_vara(file, v_scalar, nullptr, nullptr, &s_out, NC_NAT));
  EXPECT_EQ(s_out, 88);

  // 2D full rightmost dim -> NCiocount returns ii == -1, iocount == 8.
  {
    const size_t st[2] = {1, 0};
    const size_t ct[2] = {2, 4};
    const int in[8] = {10, 11, 12, 13, 14, 15, 16, 17};
    int out[8] = {};
    ASSERT_EQ(NC_NOERR, nc_put_vara_int(file, v_2d, st, ct, in));
    ASSERT_EQ(NC_NOERR, nc_get_vara_int(file, v_2d, st, ct, out));
    EXPECT_EQ(out[0], 10);
    EXPECT_EQ(out[7], 17);
  }

  // 2D partial rightmost dim -> zedp == edp0 break in NCiocount, odometer loop.
  {
    const size_t st[2] = {0, 1};
    const size_t ct[2] = {2, 2};
    const int in[4] = {101, 102, 103, 104};
    int out[4] = {};
    ASSERT_EQ(NC_NOERR, nc_put_vara_int(file, v_2d, st, ct, in));
    ASSERT_EQ(NC_NOERR, nc_get_vara_int(file, v_2d, st, ct, out));
    EXPECT_EQ(out[0], 101);
    EXPECT_EQ(out[3], 104);
  }

  // 2D zero edge cases ({2, 0} and {0, 2}) -> NCiocount *zedp == 0 branch.
  {
    const size_t st[2] = {0, 1};
    const size_t ct_inner_zero[2] = {2, 0};
    const size_t ct_outer_zero[2] = {0, 2};
    int buf = 7;
    EXPECT_EQ(NC_NOERR, nc_put_vara_int(file, v_2d, st, ct_inner_zero, &buf));
    EXPECT_EQ(NC_NOERR, nc_get_vara_int(file, v_2d, st, ct_inner_zero, &buf));
    EXPECT_EQ(NC_NOERR, nc_put_vara_int(file, v_2d, st, ct_outer_zero, &buf));
    EXPECT_EQ(NC_NOERR, nc_get_vara_int(file, v_2d, st, ct_outer_zero, &buf));
    EXPECT_EQ(buf, 7);
  }

  // 3D rightmost 2 dims full -> iocount == 24, ii == -1.
  {
    const size_t st[3] = {1, 0, 0};
    const size_t ct[3] = {2, 3, 4};
    std::vector<int> in(24);
    for (size_t i = 0; i < 24; ++i) {
      in[i] = static_cast<int>(i + 1);
    }
    std::vector<int> out(24, 0);
    ASSERT_EQ(NC_NOERR, nc_put_vara_int(file, v_3d, st, ct, in.data()));
    ASSERT_EQ(NC_NOERR, nc_get_vara_int(file, v_3d, st, ct, out.data()));
    EXPECT_EQ(out[0], 1);
    EXPECT_EQ(out[23], 24);
  }

  // 3D partial inner slices -> recursive odo1 carry across dimensions.
  {
    const size_t st[3] = {0, 1, 1};
    const size_t ct[3] = {2, 2, 2};
    const int in[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    int out[8] = {};
    ASSERT_EQ(NC_NOERR, nc_put_vara_int(file, v_3d, st, ct, in));
    ASSERT_EQ(NC_NOERR, nc_get_vara_int(file, v_3d, st, ct, out));
    for (int i = 0; i < 8; ++i) {
      EXPECT_EQ(out[i], in[i]);
    }
  }

  // 1D multi-recvar and 2D recvar -> NC_varoffset and NCiocount recvar paths.
  {
    const size_t st1 = 0;
    const size_t ct1 = 2;
    const int r1[2] = {31, 32};
    int r1_out[2] = {};
    ASSERT_EQ(NC_NOERR, nc_put_vara_int(file, v_rec_1d_a, &st1, &ct1, r1));
    ASSERT_EQ(NC_NOERR, nc_get_vara_int(file, v_rec_1d_a, &st1, &ct1, r1_out));
    EXPECT_EQ(r1_out[0], 31);
    EXPECT_EQ(r1_out[1], 32);

    const size_t st2[2] = {0, 1};
    const size_t ct2[2] = {2, 2};
    const int r2_in[4] = {41, 42, 43, 44};
    int r2_out[4] = {};
    ASSERT_EQ(NC_NOERR, nc_put_vara_int(file, v_rec_2d, st2, ct2, r2_in));
    ASSERT_EQ(NC_NOERR, nc_get_vara_int(file, v_rec_2d, st2, ct2, r2_out));
    EXPECT_EQ(r2_out[0], 41);
    EXPECT_EQ(r2_out[3], 44);
  }

  // Multi-slice non-fatal NC_ERANGE across multiple odometer iterations.
  {
    const size_t st[2] = {0, 0};
    const size_t ct[2] = {3, 2};
    const int bad_puts[6] = {40000, 1, 40001, 2, 3, 4};
    EXPECT_EQ(NC_ERANGE, nc_put_vara_int(file, v_2d_short, st, ct, bad_puts));
    short verify_puts[6] = {};
    ASSERT_EQ(NC_NOERR,
              nc_get_vara_short(file, v_2d_short, st, ct, verify_puts));
    EXPECT_EQ(verify_puts[1], 1);
    EXPECT_EQ(verify_puts[3], 2);
    EXPECT_EQ(verify_puts[4], 3);
    EXPECT_EQ(verify_puts[5], 4);

    const short neg_src[6] = {-1, 10, -2, 20, 30, 40};
    ASSERT_EQ(NC_NOERR, nc_put_vara_short(file, v_2d_short, st, ct, neg_src));
    unsigned short out[6] = {99, 99, 99, 99, 99, 99};
    EXPECT_EQ(NC_ERANGE, nc_get_vara_ushort(file, v_2d_short, st, ct, out));
    EXPECT_EQ(out[1], 10U);
    EXPECT_EQ(out[3], 20U);
    EXPECT_EQ(out[4], 30U);
    EXPECT_EQ(out[5], 40U);
  }
  EXPECT_EQ(NC_NOERR, file.close());
}

TEST(PutGetTest, DirectNc3DispatchValidationCoordAndEdgeBounds) {
  ScopedMemio memio;
  int v_scalar = -1;
  int v_char = -1;
  int v_fixed = -1;
  int v_mat = -1;
  int v_rec = -1;

  {
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("nc3_direct.nc", NC_64BIT_DATA, 0, file.ptr()));
    int d3 = -1;
    int du = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d3", 3, &d3));
    ASSERT_EQ(NC_NOERR, nc_def_dim(file, "du", NC_UNLIMITED, &du));
    ASSERT_EQ(NC_NOERR,
              nc_def_var(file, "sc", NC_DOUBLE, 0, nullptr, &v_scalar));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "vc", NC_CHAR, 1, &d3, &v_char));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "vf", NC_INT, 1, &d3, &v_fixed));
    const int mat_dims[2] = {d3, d3};
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "vm", NC_INT, 2, mat_dims, &v_mat));
    ASSERT_EQ(NC_NOERR, nc_def_var(file, "vr", NC_INT, 1, &du, &v_rec));

    // Define-mode guard checks via direct NC3 calls.
    const size_t start0 = 0;
    const size_t count1 = 1;
    int dummy = 0;
    EXPECT_EQ(NC_EINDEFINE,
              NC3_put_vara(file, v_fixed, &start0, &count1, &dummy, NC_INT));
    EXPECT_EQ(NC_EINDEFINE,
              NC3_get_vara(file, v_fixed, &start0, &count1, &dummy, NC_INT));

    ASSERT_EQ(NC_NOERR, nc_enddef(file));

    // Bad ncid and bad/global varid checks.
    EXPECT_EQ(NC_EBADID,
              NC3_put_vara(-1, v_fixed, &start0, &count1, &dummy, NC_INT));
    EXPECT_EQ(NC_EBADID,
              NC3_get_vara(-1, v_fixed, &start0, &count1, &dummy, NC_INT));
    EXPECT_EQ(NC_EGLOBAL,
              NC3_put_vara(file, NC_GLOBAL, &start0, &count1, &dummy, NC_INT));
    EXPECT_EQ(NC_EGLOBAL,
              NC3_get_vara(file, NC_GLOBAL, &start0, &count1, &dummy, NC_INT));
    EXPECT_EQ(NC_ENOTVAR,
              NC3_put_vara(file, 999, &start0, &count1, &dummy, NC_INT));
    EXPECT_EQ(NC_ENOTVAR,
              NC3_get_vara(file, 999, &start0, &count1, &dummy, NC_INT));

    // All four NC_ECHAR branches.
    char cbuf[3] = {'a', 'b', 'c'};
    int ibuf[3] = {10, 20, 30};
    EXPECT_EQ(NC_ECHAR,
              NC3_put_vara(file, v_fixed, &start0, &count1, cbuf, NC_CHAR));
    EXPECT_EQ(NC_ECHAR,
              NC3_put_vara(file, v_char, &start0, &count1, ibuf, NC_INT));
    EXPECT_EQ(NC_ECHAR,
              NC3_get_vara(file, v_fixed, &start0, &count1, cbuf, NC_CHAR));
    EXPECT_EQ(NC_ECHAR,
              NC3_get_vara(file, v_char, &start0, &count1, ibuf, NC_INT));

    // memtype == NC_NAT for scalar double and 1D char array.
    const double sc_in = 3.25;
    double sc_out = 0.0;
    EXPECT_EQ(NC_NOERR,
              NC3_put_vara(file, v_scalar, nullptr, nullptr, &sc_in, NC_NAT));
    EXPECT_EQ(NC_NOERR,
              NC3_get_vara(file, v_scalar, nullptr, nullptr, &sc_out, NC_NAT));
    EXPECT_DOUBLE_EQ(sc_out, 3.25);

    const size_t count3 = 3;
    char c_out[3] = {};
    EXPECT_EQ(NC_NOERR,
              NC3_put_vara(file, v_char, &start0, &count3, cbuf, NC_NAT));
    EXPECT_EQ(NC_NOERR,
              NC3_get_vara(file, v_char, &start0, &count3, c_out, NC_NAT));
    EXPECT_EQ(std::string(c_out, 3), "abc");

    // edges == nullptr directly on NC3_{put,get}_vara (fixed and record vars).
    int f_out[3] = {};
    EXPECT_EQ(NC_NOERR,
              NC3_put_vara(file, v_fixed, &start0, nullptr, ibuf, NC_INT));
    EXPECT_EQ(NC_NOERR,
              NC3_get_vara(file, v_fixed, &start0, nullptr, f_out, NC_INT));
    EXPECT_EQ(f_out[2], 30);

    const int rec0 = 77;
    EXPECT_EQ(NC_NOERR,
              NC3_put_vara(file, v_rec, &start0, &count1, &rec0, NC_INT));
    const int rec0_overwrite = 99;
    int rec0_out = 0;
    EXPECT_EQ(NC_NOERR, NC3_put_vara(file, v_rec, &start0, nullptr,
                                     &rec0_overwrite, NC_INT));
    EXPECT_EQ(NC_NOERR,
              NC3_get_vara(file, v_rec, &start0, nullptr, &rec0_out, NC_INT));
    EXPECT_EQ(rec0_out, 99);

    // memtype == NC_STRING on 2D sub-slice hits default NC_EBADTYPE and loop
    // break.
    const size_t m_st[2] = {0, 0};
    const size_t m_ct[2] = {2, 2};
    const char* s_in[4] = {};
    char* s_out[4] = {};
    EXPECT_EQ(NC_EBADTYPE,
              NC3_put_vara(file, v_mat, m_st, m_ct, s_in, NC_STRING));
    EXPECT_EQ(NC_EBADTYPE,
              NC3_get_vara(file, v_mat, m_st, m_ct, s_out, NC_STRING));

    // NCcoordck and NCedgeck boundary checks on writable dataset.
    const size_t over_uint_max =
        static_cast<size_t>(std::numeric_limits<unsigned int>::max()) + 1ULL;
    const size_t zero = 0;
    EXPECT_EQ(NC_EINVALCOORDS,
              NC3_put_vara(file, v_rec, &over_uint_max, &zero, ibuf, NC_INT));
    EXPECT_EQ(NC_EINVALCOORDS,
              NC3_get_vara(file, v_rec, &over_uint_max, &zero, ibuf, NC_INT));

    const size_t fix_over = 4;
    const size_t fix_at_end = 3;
    const size_t fix_in_bounds = 2;
    EXPECT_EQ(NC_EINVALCOORDS,
              NC3_get_vara(file, v_fixed, &fix_over, &zero, f_out, NC_INT));
    EXPECT_EQ(NC_EINVALCOORDS,
              NC3_get_vara(file, v_fixed, &fix_at_end, &count1, f_out, NC_INT));
    EXPECT_EQ(NC_NOERR,
              NC3_get_vara(file, v_fixed, &fix_at_end, &zero, f_out, NC_INT));
    EXPECT_EQ(NC_EEDGE,
              NC3_get_vara(file, v_fixed, &start0, &fix_over, f_out, NC_INT));
    EXPECT_EQ(NC_EEDGE, NC3_get_vara(file, v_fixed, &fix_in_bounds,
                                     &fix_in_bounds, f_out, NC_INT));

    ASSERT_EQ(NC_NOERR, nc_close_memio(file.release(), memio.get()));
  }

  // Read-only NC3 checks: NC_EPERM, record NCcoordck, NCedgeck, and NC_EEDGE.
  ScopedNcFile reader;
  ASSERT_EQ(NC_NOERR, nc_open_mem("nc3_ro.nc", NC_NOWRITE, memio.size(),
                                  memio.get()->memory, reader.ptr()));
  const size_t start0 = 0;
  const size_t start1 = 1;
  const size_t start2 = 2;
  const size_t zero = 0;
  const size_t one = 1;
  const size_t two = 2;
  int buf = 0;

  EXPECT_EQ(NC_EPERM,
            NC3_put_vara(reader, v_fixed, &start0, &one, &buf, NC_INT));
  EXPECT_EQ(NC_EINVALCOORDS,
            NC3_get_vara(reader, v_rec, &start2, &zero, &buf, NC_INT));
  EXPECT_EQ(NC_NOERR,
            NC3_get_vara(reader, v_rec, &start1, &zero, &buf, NC_INT));
  EXPECT_EQ(NC_EINVALCOORDS,
            NC3_get_vara(reader, v_rec, &start1, &one, &buf, NC_INT));
  EXPECT_EQ(NC_EEDGE, NC3_get_vara(reader, v_rec, &start0, &two, &buf, NC_INT));
  EXPECT_EQ(NC_NOERR, reader.close());
}

TEST(PutGetTest, PublicVarVar1VarsVarmUbyteAndLongSmoke) {
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("public_wrappers.nc", NC_64BIT_DATA, 0, file.ptr()));

  int dimid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(file, "d", 4, &dimid));
  int v_ubyte = -1;
  int v_long = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "ub", NC_UBYTE, 1, &dimid, &v_ubyte));
  ASSERT_EQ(NC_NOERR, nc_def_var(file, "lg", NC_INT, 1, &dimid, &v_long));
  ASSERT_EQ(NC_NOERR, nc_enddef(file));

  const size_t start0 = 0;
  const size_t count4 = 4;
  const unsigned char ub_in[4] = {1, 2, 200, 254};
  unsigned char ub_out[4] = {};
  EXPECT_EQ(NC_NOERR,
            nc_put_vara_ubyte(file, v_ubyte, &start0, &count4, ub_in));
  EXPECT_EQ(NC_NOERR,
            nc_get_vara_ubyte(file, v_ubyte, &start0, &count4, ub_out));
  EXPECT_EQ(ub_out[2], 200U);

  const long l_in[4] = {10L, -20L, 30L, -40L};
  long l_out[4] = {};
  EXPECT_EQ(NC_NOERR, nc_put_vara_long(file, v_long, &start0, &count4, l_in));
  EXPECT_EQ(NC_NOERR, nc_get_vara_long(file, v_long, &start0, &count4, l_out));
  EXPECT_EQ(l_out[1], -20L);

  // var1 / var / vars / varm wrappers.
  const size_t idx3 = 3;
  const long l1_in = 99L;
  long l1_out = 0;
  EXPECT_EQ(NC_NOERR, nc_put_var1_long(file, v_long, &idx3, &l1_in));
  EXPECT_EQ(NC_NOERR, nc_get_var1_long(file, v_long, &idx3, &l1_out));
  EXPECT_EQ(l1_out, 99L);

  const size_t count2 = 2;
  const ptrdiff_t stride2 = 2;
  const long s_in[2] = {111L, 222L};
  long s_out[2] = {};
  EXPECT_EQ(NC_NOERR,
            nc_put_vars_long(file, v_long, &start0, &count2, &stride2, s_in));
  EXPECT_EQ(NC_NOERR,
            nc_get_vars_long(file, v_long, &start0, &count2, &stride2, s_out));
  EXPECT_EQ(s_out[0], 111L);
  EXPECT_EQ(s_out[1], 222L);

  const ptrdiff_t stride1 = 1;
  const ptrdiff_t imap_elem = 1;
  const unsigned char m_in[2] = {7, 8};
  unsigned char m_out[2] = {};
  EXPECT_EQ(NC_NOERR, nc_put_varm_ubyte(file, v_ubyte, &start0, &count2,
                                        &stride1, &imap_elem, m_in));
  EXPECT_EQ(NC_NOERR, nc_get_varm_ubyte(file, v_ubyte, &start0, &count2,
                                        &stride1, &imap_elem, m_out));
  EXPECT_EQ(m_out[0], 7U);
  EXPECT_EQ(m_out[1], 8U);

  unsigned char full_ub[4] = {};
  EXPECT_EQ(NC_NOERR, nc_put_var_ubyte(file, v_ubyte, ub_in));
  EXPECT_EQ(NC_NOERR, nc_get_var_ubyte(file, v_ubyte, full_ub));
  EXPECT_EQ(full_ub[3], 254U);
  EXPECT_EQ(NC_NOERR, file.close());
}

}  // namespace
}  // namespace netcdf
