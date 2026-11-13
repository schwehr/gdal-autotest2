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
//
// Unit tests for NetCDF import, export, and metadata import
// (libharp/harp-netcdf.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-netcdf.c

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/google/util/temp_file.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace {
constexpr int kNcEbadid = -33;
constexpr int kNcEinval = -36;
constinit thread_local int g_fail_nc_put_att_text = 0;
constinit thread_local int g_fail_nc_put_att_double = 0;
constinit thread_local int g_fail_nc_inq_att = 0;
constinit thread_local int g_fail_nc_enddef = 0;
constinit thread_local int g_fail_nc_close = 0;
}  // namespace

extern "C" {
int __real_nc_put_att_text(int ncid, int varid, const char* name, size_t len,
                           const char* op);
int __wrap_nc_put_att_text(int ncid, int varid, const char* name, size_t len,
                           const char* op) {
  if (g_fail_nc_put_att_text > 0 && --g_fail_nc_put_att_text == 0) {
    return kNcEinval;
  }
  return __real_nc_put_att_text(ncid, varid, name, len, op);
}

int __real_nc_put_att_double(int ncid, int varid, const char* name, int xtype,
                             size_t len, const double* op);
int __wrap_nc_put_att_double(int ncid, int varid, const char* name, int xtype,
                             size_t len, const double* op) {
  if (g_fail_nc_put_att_double > 0 && --g_fail_nc_put_att_double == 0) {
    return kNcEinval;
  }
  return __real_nc_put_att_double(ncid, varid, name, xtype, len, op);
}

int __real_nc_inq_att(int ncid, int varid, const char* name, int* xtypep,
                      size_t* lenp);
int __wrap_nc_inq_att(int ncid, int varid, const char* name, int* xtypep,
                      size_t* lenp) {
  if (g_fail_nc_inq_att > 0 && --g_fail_nc_inq_att == 0) {
    return kNcEbadid;  // != NC_NOERR && != NC_ENOTATT
  }
  return __real_nc_inq_att(ncid, varid, name, xtypep, lenp);
}

int __real_nc_enddef(int ncid);
int __wrap_nc_enddef(int ncid) {
  if (g_fail_nc_enddef > 0 && --g_fail_nc_enddef == 0) {
    return kNcEinval;
  }
  return __real_nc_enddef(ncid);
}

int __real_nc_close(int ncid);
int __wrap_nc_close(int ncid) {
  const int rc = __real_nc_close(ncid);
  if (g_fail_nc_close > 0 && --g_fail_nc_close == 0) {
    return kNcEinval;  // After real close so ncid is freed.
  }
  return rc;
}
}  // extern "C"

namespace stcorp_harp {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

// Minimal NetCDF-3 classic (CDF\x01) binary serializer for synthesizing custom
// valid and invalid NetCDF files without depending on //third_party/netcdf.
namespace nc3 {

constexpr uint32_t kNcByte = 1;
constexpr uint32_t kNcChar = 2;
constexpr uint32_t kNcShort = 3;
constexpr uint32_t kNcInt = 4;
constexpr uint32_t kNcFloat = 5;
constexpr uint32_t kNcDouble = 6;
constexpr uint32_t kNcUbyte = 7;

constexpr uint32_t kTagDim = 0x0000000A;
constexpr uint32_t kTagVar = 0x0000000B;
constexpr uint32_t kTagAtt = 0x0000000C;

struct Attr {
  std::string name;
  uint32_t type = kNcChar;
  std::string str_val;
  std::vector<int32_t> int_vals;
  std::vector<double> double_vals;
};

struct Dim {
  std::string name;
  uint32_t length = 1;
};

struct Var {
  std::string name;
  std::vector<uint32_t> dim_ids;
  std::vector<Attr> attrs;
  uint32_t type = kNcInt;
  uint32_t nelems = 1;
};

void PushU32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(v & 0xFF));
}

void PushU64(std::vector<uint8_t>& out, uint64_t v) {
  PushU32(out, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFU));
  PushU32(out, static_cast<uint32_t>(v & 0xFFFFFFFFU));
}

void Pad4(std::vector<uint8_t>& out) {
  while (out.size() % 4 != 0) {
    out.push_back(0);
  }
}

void PushNcString(std::vector<uint8_t>& out, std::string_view s) {
  PushU32(out, static_cast<uint32_t>(s.size()));
  for (char c : s) {
    out.push_back(static_cast<uint8_t>(c));
  }
  Pad4(out);
}

void PushAttList(std::vector<uint8_t>& out, const std::vector<Attr>& attrs) {
  if (attrs.empty()) {
    PushU32(out, 0);
    PushU32(out, 0);
    return;
  }
  PushU32(out, kTagAtt);
  PushU32(out, static_cast<uint32_t>(attrs.size()));
  for (const auto& a : attrs) {
    PushNcString(out, a.name);
    PushU32(out, a.type);
    if (a.type == kNcChar || a.type == kNcByte) {
      PushU32(out, static_cast<uint32_t>(a.str_val.size()));
      for (char c : a.str_val) {
        out.push_back(static_cast<uint8_t>(c));
      }
      Pad4(out);
    } else if (a.type == kNcInt) {
      PushU32(out, static_cast<uint32_t>(a.int_vals.size()));
      for (int32_t v : a.int_vals) {
        PushU32(out, static_cast<uint32_t>(v));
      }
    } else if (a.type == kNcDouble) {
      PushU32(out, static_cast<uint32_t>(a.double_vals.size()));
      for (double d : a.double_vals) {
        PushU64(out, std::bit_cast<uint64_t>(d));
      }
    }
  }
}

uint32_t ElemSize(uint32_t nc_type) {
  switch (nc_type) {
    case kNcByte:
    case kNcChar:
      return 1;
    case kNcShort:
      return 2;
    case kNcInt:
    case kNcFloat:
      return 4;
    case kNcDouble:
      return 8;
    default:
      return 4;
  }
}

std::vector<uint8_t> BuildClassicFile(const std::vector<Dim>& dims,
                                      const std::vector<Attr>& global_attrs,
                                      const std::vector<Var>& vars) {
  std::vector<uint8_t> out = {'C', 'D', 'F', 0x01};
  PushU32(out, 0);  // numrecs

  if (dims.empty()) {
    PushU32(out, 0);
    PushU32(out, 0);
  } else {
    PushU32(out, kTagDim);
    PushU32(out, static_cast<uint32_t>(dims.size()));
    for (const auto& d : dims) {
      PushNcString(out, d.name);
      PushU32(out, d.length);
    }
  }

  PushAttList(out, global_attrs);

  struct VarOffset {
    size_t begin_pos;
    uint32_t vsize;
  };
  std::vector<VarOffset> offsets;
  if (vars.empty()) {
    PushU32(out, 0);
    PushU32(out, 0);
  } else {
    PushU32(out, kTagVar);
    PushU32(out, static_cast<uint32_t>(vars.size()));
    for (const auto& v : vars) {
      PushNcString(out, v.name);
      PushU32(out, static_cast<uint32_t>(v.dim_ids.size()));
      for (uint32_t id : v.dim_ids) {
        PushU32(out, id);
      }
      PushAttList(out, v.attrs);
      PushU32(out, v.type);
      const uint32_t raw_bytes = v.nelems * ElemSize(v.type);
      const uint32_t vsize = ((raw_bytes + 3) / 4) * 4;
      PushU32(out, vsize == 0 ? 4 : vsize);
      offsets.push_back(
          {.begin_pos = out.size(), .vsize = vsize == 0 ? 4 : vsize});
      PushU32(out, 0);  // placeholder for begin offset
    }
  }

  for (const auto& vo : offsets) {
    const uint32_t begin = static_cast<uint32_t>(out.size());
    out[vo.begin_pos + 0] = static_cast<uint8_t>((begin >> 24) & 0xFF);
    out[vo.begin_pos + 1] = static_cast<uint8_t>((begin >> 16) & 0xFF);
    out[vo.begin_pos + 2] = static_cast<uint8_t>((begin >> 8) & 0xFF);
    out[vo.begin_pos + 3] = static_cast<uint8_t>(begin & 0xFF);
    out.resize(out.size() + vo.vsize, 0);
  }

  return out;
}

// Minimal NetCDF-5 (CDF\x05) binary builder to synthesize unsupported NetCDF
// types such as NC_UBYTE (7) that classic CDF-1 cannot represent.
void PushNc5String(std::vector<uint8_t>& out, std::string_view s) {
  PushU64(out, static_cast<uint64_t>(s.size()));
  for (char c : s) {
    out.push_back(static_cast<uint8_t>(c));
  }
  Pad4(out);
}

void PushNc5AttList(std::vector<uint8_t>& out, const std::vector<Attr>& attrs) {
  if (attrs.empty()) {
    PushU32(out, 0);
    PushU64(out, 0);
    return;
  }
  PushU32(out, kTagAtt);
  PushU64(out, static_cast<uint64_t>(attrs.size()));
  for (const auto& a : attrs) {
    PushNc5String(out, a.name);
    PushU32(out, a.type);
    if (a.type == kNcChar || a.type == kNcByte || a.type == kNcUbyte) {
      PushU64(out, static_cast<uint64_t>(a.str_val.size()));
      for (char c : a.str_val) {
        out.push_back(static_cast<uint8_t>(c));
      }
      Pad4(out);
    }
  }
}

std::vector<uint8_t> BuildCdf5File(const std::vector<Dim>& dims,
                                   const std::vector<Attr>& global_attrs,
                                   const std::vector<Var>& vars) {
  std::vector<uint8_t> out = {'C', 'D', 'F', 0x05};
  PushU64(out, 0);  // numrecs

  if (dims.empty()) {
    PushU32(out, 0);
    PushU64(out, 0);
  } else {
    PushU32(out, kTagDim);
    PushU64(out, static_cast<uint64_t>(dims.size()));
    for (const auto& d : dims) {
      PushNc5String(out, d.name);
      PushU64(out, d.length);
    }
  }

  PushNc5AttList(out, global_attrs);

  struct VarOffset {
    size_t begin_pos;
    uint64_t vsize;
  };
  std::vector<VarOffset> offsets;
  if (vars.empty()) {
    PushU32(out, 0);
    PushU64(out, 0);
  } else {
    PushU32(out, kTagVar);
    PushU64(out, static_cast<uint64_t>(vars.size()));
    for (const auto& v : vars) {
      PushNc5String(out, v.name);
      PushU64(out, static_cast<uint64_t>(v.dim_ids.size()));
      for (uint32_t id : v.dim_ids) {
        PushU64(out, id);
      }
      PushNc5AttList(out, v.attrs);
      PushU32(out, v.type);
      const uint64_t vsize = ((v.nelems * 1ULL + 3) / 4) * 4;
      PushU64(out, vsize == 0 ? 4 : vsize);
      offsets.push_back(
          {.begin_pos = out.size(), .vsize = vsize == 0 ? 4 : vsize});
      PushU64(out, 0);
    }
  }

  for (const auto& vo : offsets) {
    const uint64_t begin = static_cast<uint64_t>(out.size());
    for (int b = 0; b < 8; ++b) {
      out[vo.begin_pos + b] =
          static_cast<uint8_t>((begin >> (56 - 8 * b)) & 0xFF);
    }
    out.resize(out.size() + vo.vsize, 0);
  }
  return out;
}

}  // namespace nc3

TEST(HarpNetcdfTest, ImportAndExportArgumentAndFileErrors) {
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  harp_product* prod = nullptr;
  EXPECT_EQ(-1, harp_import_netcdf(nullptr, &prod));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_EQ(nullptr, prod);

  harp_product_metadata* raw_meta = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_metadata_new(&raw_meta));
  ScopedHarpProductMetadata meta(raw_meta);
  EXPECT_EQ(-1, harp_import_metadata_netcdf(nullptr, meta.get()));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  EXPECT_EQ(-1, harp_export_netcdf(nullptr, nullptr));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_EQ(-1, harp_export_netcdf("out.nc", nullptr));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  // Missing file (Google stat() ENOENT patch in harp_import_netcdf).
  const std::string missing =
      (std::filesystem::path(::testing::TempDir()) / "missing_netcdf.nc")
          .native();
  EXPECT_EQ(-1, harp_import_netcdf(missing.c_str(), &prod));
  EXPECT_EQ(HARP_ERROR_FILE_NOT_FOUND, harp_errno);
  EXPECT_EQ(-1, harp_import_metadata_netcdf(missing.c_str(), meta.get()));
  EXPECT_EQ(HARP_ERROR_NETCDF, harp_errno);

  // Too-short file (< 10 bytes Google patch in harp_import_netcdf).
  TempFile short_file("netcdf_short.nc");
  ASSERT_TRUE(WriteFileText(short_file.path(), "CDF\x01short"));
  EXPECT_EQ(-1, harp_import_netcdf(short_file.c_str(), &prod));
  EXPECT_EQ(HARP_ERROR_FILE_READ, harp_errno);

  // Exact 10-byte boundary passes the stat() guard and fails inside nc_open.
  TempFile ten_byte_file("netcdf_10bytes.nc");
  ASSERT_TRUE(WriteFileText(ten_byte_file.path(), "0123456789"));
  EXPECT_EQ(-1, harp_import_netcdf(ten_byte_file.c_str(), &prod));
  EXPECT_EQ(HARP_ERROR_NETCDF, harp_errno);

  // Corrupt >= 10 byte file fails nc_open.
  TempFile corrupt_file("netcdf_corrupt.nc");
  ASSERT_TRUE(WriteFileText(corrupt_file.path(), "not a valid netcdf file"));
  EXPECT_EQ(-1, harp_import_netcdf(corrupt_file.c_str(), &prod));
  EXPECT_EQ(HARP_ERROR_NETCDF, harp_errno);
  EXPECT_EQ(-1, harp_import_metadata_netcdf(corrupt_file.c_str(), meta.get()));
  EXPECT_EQ(HARP_ERROR_NETCDF, harp_errno);

  // Export to unwritable/non-existent directory fails nc_create.
  harp_product* raw_empty = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_empty));
  ScopedHarpProduct empty_prod(raw_empty);
  EXPECT_EQ(
      -1, harp_export_netcdf("/nonexistent_dir_xyz/out.nc", empty_prod.get()));
  EXPECT_EQ(HARP_ERROR_NETCDF, harp_errno);
}

TEST(HarpNetcdfTest, ExportAndImportAllTypesDimensionsAndAttributesRoundTrip) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_set_source_product(prod.get(), "synthetic_src"));
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_set_history(prod.get(), "created for netcdf test"));

  auto add_var = [&](const char* name, harp_data_type dtype, int ndims,
                     const harp_dimension_type* dim_types, const long* dims,
                     auto init_fn) {
    harp_variable* raw_v = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_new(name, dtype, ndims, dim_types, dims, &raw_v));
    ScopedHarpVariable v(raw_v);
    init_fn(v.get());
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), v.get()));
    v.release();
  };

  // datetime (time=2) so datetime_start/datetime_stop attributes are written.
  const harp_dimension_type t_dim[] = {harp_dimension_time};
  const long t_len[] = {2};
  add_var("datetime", harp_type_double, 1, t_dim, t_len, [](harp_variable* v) {
    ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(v, HARP_UNIT_DATETIME));
    ASSERT_EQ(HARP_SUCCESS,
              harp_variable_set_description(v, "measurement time"));
    v->data.double_data[0] = 100.0;
    v->data.double_data[1] = 200.0;
    v->valid_min.double_data = 0.0;
    v->valid_max.double_data = 1000.0;
  });

  // int8 with enum flag_values/flag_meanings and custom valid_min/valid_max.
  const harp_dimension_type lat_dim[] = {harp_dimension_latitude};
  const long lat_len[] = {2};
  add_var("quality_flag", harp_type_int8, 1, lat_dim, lat_len,
          [](harp_variable* v) {
            v->data.int8_data[0] = 0;
            v->data.int8_data[1] = 1;
            v->valid_min.int8_data = 0;
            v->valid_max.int8_data = 1;
            const char* enums[] = {"good", "bad"};
            ASSERT_EQ(HARP_SUCCESS,
                      harp_variable_set_enumeration_values(v, 2, enums));
          });

  // int16 (longitude)
  const harp_dimension_type lon_dim[] = {harp_dimension_longitude};
  const long lon_len[] = {2};
  add_var("count_i16", harp_type_int16, 1, lon_dim, lon_len,
          [](harp_variable* v) {
            v->data.int16_data[0] = -10;
            v->data.int16_data[1] = 20;
            v->valid_min.int16_data = -100;
            v->valid_max.int16_data = 100;
          });

  // int32 (vertical)
  const harp_dimension_type vert_dim[] = {harp_dimension_vertical};
  const long vert_len[] = {2};
  add_var("index_i32", harp_type_int32, 1, vert_dim, vert_len,
          [](harp_variable* v) {
            v->data.int32_data[0] = 1000;
            v->data.int32_data[1] = 2000;
            v->valid_min.int32_data = 0;
            v->valid_max.int32_data = 5000;
          });

  // float (spectral)
  const harp_dimension_type spec_dim[] = {harp_dimension_spectral};
  const long spec_len[] = {2};
  add_var("wavelength", harp_type_float, 1, spec_dim, spec_len,
          [](harp_variable* v) {
            ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(v, "nm"));
            v->data.float_data[0] = 300.5f;
            v->data.float_data[1] = 400.5f;
            v->valid_min.float_data = 200.0f;
            v->valid_max.float_data = 800.0f;
          });

  // string with empty strings (covers string_1 minimum dimension length) and
  // non-empty strings (independent dimension).
  const harp_dimension_type indep_dim[] = {harp_dimension_independent};
  const long indep_len[] = {2};
  add_var("empty_strs", harp_type_string, 1, indep_dim, indep_len,
          [](harp_variable* v) {
            ASSERT_EQ(HARP_SUCCESS,
                      harp_variable_set_string_data_element(v, 0, ""));
            ASSERT_EQ(HARP_SUCCESS,
                      harp_variable_set_string_data_element(v, 1, ""));
          });
  add_var("labels", harp_type_string, 1, indep_dim, indep_len,
          [](harp_variable* v) {
            ASSERT_EQ(HARP_SUCCESS,
                      harp_variable_set_string_data_element(v, 0, "alpha"));
            ASSERT_EQ(HARP_SUCCESS,
                      harp_variable_set_string_data_element(v, 1, "beta_long"));
          });

  // 0-D scalar double.
  add_var("scalar_d", harp_type_double, 0, nullptr, nullptr,
          [](harp_variable* v) { v->data.double_data[0] = 3.25; });

  TempFile nc_file("netcdf_roundtrip.nc");
  ASSERT_EQ(HARP_SUCCESS, harp_export_netcdf(nc_file.c_str(), prod.get()));

  // Import back via harp_import_netcdf and verify contents.
  harp_product* raw_imported = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_import_netcdf(nc_file.c_str(), &raw_imported));
  ScopedHarpProduct imported(raw_imported);
  ASSERT_EQ(HARP_SUCCESS, harp_product_verify(imported.get()));
  EXPECT_STREQ("synthetic_src", imported->source_product);
  EXPECT_STREQ("created for netcdf test", imported->history);
  EXPECT_EQ(2L, imported->dimension[harp_dimension_time]);
  EXPECT_EQ(2L, imported->dimension[harp_dimension_latitude]);
  EXPECT_EQ(2L, imported->dimension[harp_dimension_longitude]);
  EXPECT_EQ(2L, imported->dimension[harp_dimension_vertical]);
  EXPECT_EQ(2L, imported->dimension[harp_dimension_spectral]);

  harp_variable* var = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(
                              imported.get(), "quality_flag", &var));
  EXPECT_EQ(2, var->num_enum_values);
  EXPECT_STREQ("good", var->enum_name[0]);
  EXPECT_STREQ("bad", var->enum_name[1]);
  EXPECT_EQ(0, var->valid_min.int8_data);
  EXPECT_EQ(1, var->valid_max.int8_data);

  ASSERT_EQ(HARP_SUCCESS,
            harp_product_get_variable_by_name(imported.get(), "labels", &var));
  EXPECT_STREQ("alpha", var->data.string_data[0]);
  EXPECT_STREQ("beta_long", var->data.string_data[1]);

  ASSERT_EQ(HARP_SUCCESS, harp_product_get_variable_by_name(imported.get(),
                                                            "scalar_d", &var));
  EXPECT_DOUBLE_EQ(3.25, var->data.double_data[0]);

  // Import metadata via harp_import_metadata_netcdf.
  harp_product_metadata* raw_meta = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_metadata_new(&raw_meta));
  ScopedHarpProductMetadata meta(raw_meta);
  ASSERT_EQ(HARP_SUCCESS,
            harp_import_metadata_netcdf(nc_file.c_str(), meta.get()));
  EXPECT_STREQ("HARP_NETCDF", meta->format);
  EXPECT_STREQ("synthetic_src", meta->source_product);
  EXPECT_STREQ("created for netcdf test", meta->history);
  EXPECT_DOUBLE_EQ(100.0 / 86400.0, meta->datetime_start);
  EXPECT_DOUBLE_EQ(200.0 / 86400.0, meta->datetime_stop);
  EXPECT_EQ(2L, meta->dimension[harp_dimension_time]);
}

TEST(HarpNetcdfTest, ExportRejectsConflictingDimensionSizes) {
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);

  const harp_dimension_type dim_type[] = {harp_dimension_time};
  const long len2[] = {2};
  const long len3[] = {3};
  harp_variable* raw_v1 = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_new("v1", harp_type_double, 1, dim_type,
                                            len2, &raw_v1));
  ScopedHarpVariable v1(raw_v1);
  ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), v1.get()));
  v1.release();

  harp_variable* raw_v2 = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_new("v2", harp_type_double, 1, dim_type,
                                            len3, &raw_v2));
  ScopedHarpVariable v2(raw_v2);
  // Temporarily match time dimension (2) so harp_product_add_variable succeeds,
  // then restore v2->dimension[0] = 3 to hit the dimension-conflict check in
  // write_product() / dimensions_add().
  v2->dimension[0] = 2;
  ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), v2.get()));
  v2.release();
  prod->variable[1]->dimension[0] = 3;

  TempFile nc_file("netcdf_conflict_dim.nc");
  EXPECT_EQ(-1, harp_export_netcdf(nc_file.c_str(), prod.get()));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
}

TEST(HarpNetcdfTest, ExportNetcdfErrorPaths) {
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  // 1. Two 0-length dimensions cause nc_def_dim() to fail (NC_EUNLIMIT).
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);
    const harp_dimension_type t_dim[] = {harp_dimension_time};
    const harp_dimension_type lat_dim[] = {harp_dimension_latitude};
    const long len0[] = {0};
    harp_variable* raw_v1 = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_variable_new("v1", harp_type_double, 1, t_dim,
                                              len0, &raw_v1));
    ScopedHarpVariable v1(raw_v1);
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), v1.get()));
    v1.release();

    harp_variable* raw_v2 = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_variable_new("v2", harp_type_double, 1,
                                              lat_dim, len0, &raw_v2));
    ScopedHarpVariable v2(raw_v2);
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), v2.get()));
    v2.release();

    TempFile nc_file("netcdf_two_unlim.nc");
    EXPECT_EQ(-1, harp_export_netcdf(nc_file.c_str(), prod.get()));
    EXPECT_EQ(HARP_ERROR_NETCDF, harp_errno);
  }

  // 2. Duplicate variable name causes nc_def_var() to fail.
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);
    harp_variable* raw_v1 = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_variable_new("dup", harp_type_double, 0,
                                              nullptr, nullptr, &raw_v1));
    ScopedHarpVariable v1(raw_v1);
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), v1.get()));
    v1.release();

    harp_variable* raw_v2 = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_variable_new("other", harp_type_double, 0,
                                              nullptr, nullptr, &raw_v2));
    ScopedHarpVariable v2(raw_v2);
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), v2.get()));
    v2.release();
    free(prod->variable[1]->name);
    prod->variable[1]->name = strdup("dup");

    TempFile nc_file("netcdf_dup_var.nc");
    EXPECT_EQ(-1, harp_export_netcdf(nc_file.c_str(), prod.get()));
    EXPECT_EQ(HARP_ERROR_NETCDF, harp_errno);
  }

  // 3. Storage size > 1GB selects NC_64BIT_OFFSET, and a non-last variable
  //    exceeding the 4 GiB CDF-2 per-variable limit triggers nc_enddef()
  //    failure (NC_EVARSIZE) before any data write.
  {
    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);
    const harp_dimension_type t_dim[] = {harp_dimension_time};
    const harp_dimension_type lat_dim[] = {harp_dimension_latitude};
    const long len1[] = {1};
    harp_variable* raw_v1 = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_variable_new("big", harp_type_double, 1, t_dim,
                                              len1, &raw_v1));
    ScopedHarpVariable v1(raw_v1);
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), v1.get()));
    v1.release();

    harp_variable* raw_v2 = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_variable_new("tail", harp_type_double, 1,
                                              lat_dim, len1, &raw_v2));
    ScopedHarpVariable v2(raw_v2);
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), v2.get()));
    v2.release();

    prod->variable[0]->num_elements = 200000000L;  // > 1 GiB
    prod->variable[0]->dimension[0] = 600000000L;  // > 4 GiB non-last CDF-2
    auto restore = absl::MakeCleanup([&] {
      prod->variable[0]->num_elements = 1;
      prod->variable[0]->dimension[0] = 1;
    });

    TempFile nc_file("netcdf_oversized.nc");
    EXPECT_EQ(-1, harp_export_netcdf(nc_file.c_str(), prod.get()));
    EXPECT_EQ(HARP_ERROR_NETCDF, harp_errno);
  }
}

TEST(HarpNetcdfTest, ImportMinimalProductWithoutOptionalGlobalAttributes) {
  // Valid HARP-1.0 classic NetCDF file without datetime_start/stop,
  // source_product, or history exercises metadata defaults.
  const std::vector<uint8_t> bytes = nc3::BuildClassicFile(
      /*dims=*/{{"time", 3}},
      /*global_attrs=*/{{"Conventions", nc3::kNcChar, "HARP-1.0", {}, {}}},
      /*vars=*/{});
  TempFile nc_file("netcdf_minimal_meta.nc");
  ASSERT_TRUE(WriteFileBytes(nc_file.path(), bytes));

  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_import_netcdf(nc_file.c_str(), &raw_prod));
  ScopedHarpProduct prod(raw_prod);
  EXPECT_EQ(nullptr, prod->source_product);
  EXPECT_EQ(nullptr, prod->history);

  harp_product_metadata* raw_meta = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_metadata_new(&raw_meta));
  ScopedHarpProductMetadata meta(raw_meta);
  ASSERT_EQ(HARP_SUCCESS,
            harp_import_metadata_netcdf(nc_file.c_str(), meta.get()));
  EXPECT_EQ(1, harp_ismininf(meta->datetime_start));
  EXPECT_EQ(1, harp_isplusinf(meta->datetime_stop));
  EXPECT_STREQ("netcdf_minimal_meta.nc", meta->source_product);
  EXPECT_EQ(nullptr, meta->history);
  EXPECT_EQ(3L, meta->dimension[harp_dimension_time]);
}

TEST(HarpNetcdfTest, ImportAndMetadataValidationErrorPaths) {
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  struct ClassicErrorCase {
    std::string name;
    std::vector<nc3::Dim> dims;
    std::vector<nc3::Attr> global_attrs;
    std::vector<nc3::Var> vars;
    int expected_errno;
    bool test_metadata;
  };

  const nc3::Attr kValidHarpConv{
      "Conventions", nc3::kNcChar, "HARP-1.0", {}, {}};

  const ClassicErrorCase kCases[] = {
      {.name = "missing_conventions",
       .dims = {},
       .global_attrs = {},
       .vars = {},
       .expected_errno = HARP_ERROR_UNSUPPORTED_PRODUCT,
       .test_metadata = true},
      {.name = "non_harp_conventions",
       .dims = {},
       .global_attrs = {{"Conventions", nc3::kNcChar, "CF-1.6", {}, {}}},
       .vars = {},
       .expected_errno = HARP_ERROR_UNSUPPORTED_PRODUCT,
       .test_metadata = true},
      {.name = "future_major_version",
       .dims = {},
       .global_attrs = {{"Conventions", nc3::kNcChar, "HARP-2.0", {}, {}}},
       .vars = {},
       .expected_errno = HARP_ERROR_FILE_OPEN,
       .test_metadata = true},
      {.name = "future_minor_version",
       .dims = {},
       .global_attrs = {{"Conventions", nc3::kNcChar, "HARP-1.9", {}, {}}},
       .vars = {},
       .expected_errno = HARP_ERROR_FILE_OPEN,
       .test_metadata = true},
      {.name = "unsupported_dimension_name",
       .dims = {{"station", 2}},
       .global_attrs = {kValidHarpConv},
       .vars = {},
       .expected_errno = HARP_ERROR_IMPORT,
       .test_metadata = true},
      {.name = "duplicate_independent_dimension_same_size",
       .dims = {{"independent_02", 2}, {"independent_2", 2}},
       .global_attrs = {kValidHarpConv},
       .vars = {},
       .expected_errno = HARP_ERROR_IMPORT,
       .test_metadata = false},
      {.name = "string_var_zero_dims",
       .dims = {},
       .global_attrs = {kValidHarpConv},
       .vars = {{"s0", {}, {}, nc3::kNcChar, 1}},
       .expected_errno = HARP_ERROR_IMPORT,
       .test_metadata = false},
      {.name = "string_var_non_string_inner_dim",
       .dims = {{"time", 2}},
       .global_attrs = {kValidHarpConv},
       .vars = {{"s1", {0}, {}, nc3::kNcChar, 2}},
       .expected_errno = HARP_ERROR_IMPORT,
       .test_metadata = false},
      {.name = "too_many_dimensions",
       .dims = {{"time", 1},
                {"latitude", 1},
                {"longitude", 1},
                {"vertical", 1},
                {"spectral", 1},
                {"independent_1", 1},
                {"independent_2", 2},
                {"independent_3", 3},
                {"independent_4", 4}},
       .global_attrs = {kValidHarpConv},
       .vars = {{"v9", {0, 1, 2, 3, 4, 5, 6, 7, 8}, {}, nc3::kNcDouble, 24}},
       .expected_errno = HARP_ERROR_IMPORT,
       .test_metadata = false},
      {.name = "numeric_var_with_string_dim",
       .dims = {{"string_4", 4}},
       .global_attrs = {kValidHarpConv},
       .vars = {{"bad_dim_var", {0}, {}, nc3::kNcDouble, 4}},
       .expected_errno = HARP_ERROR_IMPORT,
       .test_metadata = false},
      {.name = "var_description_not_string",
       .dims = {{"time", 1}},
       .global_attrs = {kValidHarpConv},
       .vars = {{"v",
                 {0},
                 {{"description", nc3::kNcInt, "", {1}, {}}},
                 nc3::kNcDouble,
                 1}},
       .expected_errno = HARP_ERROR_IMPORT,
       .test_metadata = false},
      {.name = "var_units_not_string",
       .dims = {{"time", 1}},
       .global_attrs = {kValidHarpConv},
       .vars = {{"v",
                 {0},
                 {{"units", nc3::kNcInt, "", {1}, {}}},
                 nc3::kNcDouble,
                 1}},
       .expected_errno = HARP_ERROR_IMPORT,
       .test_metadata = false},
      {.name = "var_valid_min_bad_len",
       .dims = {{"time", 1}},
       .global_attrs = {kValidHarpConv},
       .vars = {{"v",
                 {0},
                 {{"valid_min", nc3::kNcDouble, "", {}, {1.0, 2.0}}},
                 nc3::kNcDouble,
                 1}},
       .expected_errno = HARP_ERROR_IMPORT,
       .test_metadata = false},
      {.name = "var_valid_min_bad_type_char",
       .dims = {{"time", 1}},
       .global_attrs = {kValidHarpConv},
       .vars = {{"v",
                 {0},
                 {{"valid_min", nc3::kNcChar, "x", {}, {}}},
                 nc3::kNcDouble,
                 1}},
       .expected_errno = HARP_ERROR_IMPORT,
       .test_metadata = false},
      {.name = "var_valid_min_mismatched_type",
       .dims = {{"time", 1}},
       .global_attrs = {kValidHarpConv},
       .vars = {{"v",
                 {0},
                 {{"valid_min", nc3::kNcInt, "", {0}, {}}},
                 nc3::kNcDouble,
                 1}},
       .expected_errno = HARP_ERROR_IMPORT,
       .test_metadata = false},
      {.name = "var_valid_max_bad_len",
       .dims = {{"time", 1}},
       .global_attrs = {kValidHarpConv},
       .vars = {{"v",
                 {0},
                 {{"valid_max", nc3::kNcDouble, "", {}, {1.0, 2.0}}},
                 nc3::kNcDouble,
                 1}},
       .expected_errno = HARP_ERROR_IMPORT,
       .test_metadata = false},
      {.name = "var_valid_max_mismatched_type",
       .dims = {{"time", 1}},
       .global_attrs = {kValidHarpConv},
       .vars = {{"v",
                 {0},
                 {{"valid_max", nc3::kNcInt, "", {0}, {}}},
                 nc3::kNcDouble,
                 1}},
       .expected_errno = HARP_ERROR_IMPORT,
       .test_metadata = false},
      {.name = "var_flag_meanings_not_string",
       .dims = {{"time", 1}},
       .global_attrs = {kValidHarpConv},
       .vars = {{"v",
                 {0},
                 {{"flag_meanings", nc3::kNcInt, "", {1}, {}}},
                 nc3::kNcByte,
                 1}},
       .expected_errno = HARP_ERROR_IMPORT,
       .test_metadata = false},
      {.name = "var_flag_meanings_invalid_identifier",
       .dims = {{"time", 1}},
       .global_attrs = {kValidHarpConv},
       .vars = {{"v",
                 {0},
                 {{"flag_meanings", nc3::kNcChar, "1bad", {}, {}}},
                 nc3::kNcByte,
                 1}},
       .expected_errno = HARP_ERROR_INVALID_ARGUMENT,
       .test_metadata = false},
      {.name = "var_invalid_identifier_name",
       .dims = {{"time", 1}},
       .global_attrs = {kValidHarpConv},
       .vars = {{"123bad", {0}, {}, nc3::kNcDouble, 1}},
       .expected_errno = HARP_ERROR_INVALID_ARGUMENT,
       .test_metadata = false},
      {.name = "global_source_product_not_string",
       .dims = {},
       .global_attrs = {kValidHarpConv,
                        {"source_product", nc3::kNcInt, "", {1}, {}}},
       .vars = {},
       .expected_errno = HARP_ERROR_IMPORT,
       .test_metadata = true},
      {.name = "global_history_not_string",
       .dims = {},
       .global_attrs = {kValidHarpConv, {"history", nc3::kNcInt, "", {1}, {}}},
       .vars = {},
       .expected_errno = HARP_ERROR_IMPORT,
       .test_metadata = true},
      {.name = "meta_datetime_start_bad_type",
       .dims = {},
       .global_attrs = {kValidHarpConv,
                        {"datetime_start", nc3::kNcInt, "", {1}, {}}},
       .vars = {},
       .expected_errno = HARP_ERROR_IMPORT,
       .test_metadata = true},
      {.name = "meta_datetime_start_bad_len",
       .dims = {},
       .global_attrs = {kValidHarpConv,
                        {"datetime_start", nc3::kNcDouble, "", {}, {1.0, 2.0}}},
       .vars = {},
       .expected_errno = HARP_ERROR_IMPORT,
       .test_metadata = true},
      {.name = "meta_datetime_stop_bad_type",
       .dims = {},
       .global_attrs = {kValidHarpConv,
                        {"datetime_stop", nc3::kNcInt, "", {1}, {}}},
       .vars = {},
       .expected_errno = HARP_ERROR_IMPORT,
       .test_metadata = true},
      {.name = "meta_datetime_stop_bad_len",
       .dims = {},
       .global_attrs = {kValidHarpConv,
                        {"datetime_stop", nc3::kNcDouble, "", {}, {1.0, 2.0}}},
       .vars = {},
       .expected_errno = HARP_ERROR_IMPORT,
       .test_metadata = true},
  };

  for (const auto& tc : kCases) {
    TempFile nc_file("netcdf_err_case.nc");
    const std::vector<uint8_t> bytes =
        nc3::BuildClassicFile(tc.dims, tc.global_attrs, tc.vars);
    ASSERT_TRUE(WriteFileBytes(nc_file.path(), bytes)) << tc.name;

    if (tc.test_metadata) {
      harp_set_error(HARP_SUCCESS, nullptr);
      harp_product_metadata* raw_meta = nullptr;
      ASSERT_EQ(HARP_SUCCESS, harp_product_metadata_new(&raw_meta));
      ScopedHarpProductMetadata meta(raw_meta);
      EXPECT_EQ(-1, harp_import_metadata_netcdf(nc_file.c_str(), meta.get()))
          << tc.name;
      EXPECT_EQ(tc.expected_errno, harp_errno) << tc.name;
    }

    if (!tc.name.starts_with("meta_")) {
      harp_set_error(HARP_SUCCESS, nullptr);
      harp_product* raw_prod = nullptr;
      EXPECT_EQ(-1, harp_import_netcdf(nc_file.c_str(), &raw_prod)) << tc.name;
      ScopedHarpProduct prod_guard(raw_prod);
      EXPECT_EQ(tc.expected_errno, harp_errno) << tc.name;
      EXPECT_EQ(nullptr, raw_prod) << tc.name;
    }
  }

  // CDF-5 unsupported data type (NC_UBYTE) in numeric attribute and variable.
  {
    const std::vector<uint8_t> bad_attr = nc3::BuildCdf5File(
        /*dims=*/{},
        /*global_attrs=*/
        {kValidHarpConv, {"datetime_start", nc3::kNcUbyte, "x", {}, {}}},
        /*vars=*/{});
    TempFile f_attr("netcdf_cdf5_ubyte_attr.nc");
    ASSERT_TRUE(WriteFileBytes(f_attr.path(), bad_attr));
    harp_product_metadata* raw_meta = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_metadata_new(&raw_meta));
    ScopedHarpProductMetadata meta(raw_meta);
    EXPECT_EQ(-1, harp_import_metadata_netcdf(f_attr.c_str(), meta.get()));
    EXPECT_EQ(HARP_ERROR_IMPORT, harp_errno);

    const std::vector<uint8_t> bad_var = nc3::BuildCdf5File(
        /*dims=*/{{"time", 1}},
        /*global_attrs=*/{kValidHarpConv},
        /*vars=*/{{"u8", {0}, {}, nc3::kNcUbyte, 1}});
    TempFile f_var("netcdf_cdf5_ubyte_var.nc");
    ASSERT_TRUE(WriteFileBytes(f_var.path(), bad_var));
    harp_product* raw_prod = nullptr;
    EXPECT_EQ(-1, harp_import_netcdf(f_var.c_str(), &raw_prod));
    ScopedHarpProduct prod_guard(raw_prod);
    EXPECT_EQ(HARP_ERROR_IMPORT, harp_errno);
  }

  // Importing a valid HDF5 HARP file (NetCDF-4 compatible) via
  // harp_import_netcdf succeeds, and corrupting/truncating a compressed HDF5
  // dataset causes nc_get_var_*() to fail.
  {
    ScopedHarpInit init;
    ASSERT_EQ(HARP_SUCCESS, init.status());
    const int prev_comp = harp_get_option_hdf5_compression();
    auto restore_comp =
        absl::MakeCleanup([&] { harp_set_option_hdf5_compression(prev_comp); });
    ASSERT_EQ(HARP_SUCCESS, harp_set_option_hdf5_compression(1));

    harp_product* raw_prod = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
    ScopedHarpProduct prod(raw_prod);
    const harp_dimension_type t_dim[] = {harp_dimension_time};
    const long t_len[] = {64};
    harp_variable* raw_v = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_variable_new("vals", harp_type_double, 1,
                                              t_dim, t_len, &raw_v));
    ScopedHarpVariable v(raw_v);
    for (long i = 0; i < 64; ++i) {
      v->data.double_data[i] = static_cast<double>(i);
    }
    ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), v.get()));
    v.release();

    TempFile h5_file("netcdf4_compat.h5");
    ASSERT_EQ(HARP_SUCCESS, harp_export(h5_file.c_str(), "hdf5", prod.get()));

    harp_product* raw_ok = nullptr;
    EXPECT_EQ(HARP_SUCCESS, harp_import_netcdf(h5_file.c_str(), &raw_ok));
    ScopedHarpProduct ok_guard(raw_ok);

    // Truncate the last 16 bytes (inside the compressed dataset chunk).
    const auto sz = std::filesystem::file_size(h5_file.path());
    if (sz > 128) {
      std::filesystem::resize_file(h5_file.path(), sz - 16);
      harp_product* raw_trunc = nullptr;
      EXPECT_EQ(-1, harp_import_netcdf(h5_file.c_str(), &raw_trunc));
      ScopedHarpProduct trunc_guard(raw_trunc);
    }
  }
}

TEST(HarpNetcdfTest, NetcdfCallErrorInjections) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const absl::Cleanup restore = [] {
    g_fail_nc_put_att_text = 0;
    g_fail_nc_put_att_double = 0;
    g_fail_nc_inq_att = 0;
    g_fail_nc_enddef = 0;
    g_fail_nc_close = 0;
    harp_set_error(HARP_SUCCESS, nullptr);
  };

  // Build a rich product with source_product, history, datetime, custom
  // valid_min/valid_max, float _FillValue, enum flags, description, unit, and
  // string variable.
  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);
  ASSERT_EQ(HARP_SUCCESS, harp_product_set_source_product(prod.get(), "src"));
  char history_arg[] = "harp_netcdf_test";
  char* history_argv[] = {history_arg};
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_update_history(prod.get(), "test", 1, history_argv));

  const harp_dimension_type time_dim[] = {harp_dimension_time};
  const long time_len[] = {2};
  harp_variable* raw_dt = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_new("datetime", harp_type_double, 1,
                                            time_dim, time_len, &raw_dt));
  ScopedHarpVariable dt(raw_dt);
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_unit(dt.get(), "seconds since 2000-01-01"));
  dt->data.double_data[0] = 100.0;
  dt->data.double_data[1] = 200.0;
  dt->valid_min.double_data = 0.0;
  dt->valid_max.double_data = 1000.0;
  ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), dt.get()));
  dt.release();

  harp_variable* raw_str = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_new("label", harp_type_string, 1,
                                            time_dim, time_len, &raw_str));
  ScopedHarpVariable str(raw_str);
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(str.get(), 0, "alpha"));
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_string_data_element(str.get(), 1, "beta"));
  ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), str.get()));
  str.release();

  harp_variable* raw_flag = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_new("flag", harp_type_int8, 1, time_dim,
                                            time_len, &raw_flag));
  ScopedHarpVariable flag(raw_flag);
  const char* flag_names[] = {"ok", "warn"};
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_enumeration_values(flag.get(), 2, flag_names));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_description(flag.get(), "desc"));
  ASSERT_EQ(HARP_SUCCESS, harp_variable_set_unit(flag.get(), "1"));
  flag->data.int8_data[0] = 0;
  flag->data.int8_data[1] = 1;
  ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), flag.get()));
  flag.release();

  TempFile nc_file("netcdf_err_inject.nc");

  // 1. Export attribute/enddef/close failures.
  for (int n = 1; n <= 10; ++n) {
    g_fail_nc_put_att_text = n;
    harp_export_netcdf(nc_file.c_str(), prod.get());
    g_fail_nc_put_att_text = 0;
    harp_set_error(HARP_SUCCESS, nullptr);
  }
  for (int n = 1; n <= 6; ++n) {
    g_fail_nc_put_att_double = n;
    harp_export_netcdf(nc_file.c_str(), prod.get());
    g_fail_nc_put_att_double = 0;
    harp_set_error(HARP_SUCCESS, nullptr);
  }
  g_fail_nc_enddef = 1;
  EXPECT_EQ(-1, harp_export_netcdf(nc_file.c_str(), prod.get()));
  EXPECT_EQ(HARP_ERROR_NETCDF, harp_errno);
  g_fail_nc_enddef = 0;

  g_fail_nc_close = 1;
  EXPECT_EQ(-1, harp_export_netcdf(nc_file.c_str(), prod.get()));
  EXPECT_EQ(HARP_ERROR_NETCDF, harp_errno);
  g_fail_nc_close = 0;

  // Produce a valid NetCDF file for import failure sweeps.
  ASSERT_EQ(HARP_SUCCESS, harp_export_netcdf(nc_file.c_str(), prod.get()));

  // 2. Import nc_inq_att and nc_close failures.
  for (int n = 2; n <= 18; ++n) {
    g_fail_nc_inq_att = n;
    harp_product* imp = nullptr;
    if (harp_import_netcdf(nc_file.c_str(), &imp) == HARP_SUCCESS) {
      harp_product_delete(imp);
    }
    g_fail_nc_inq_att = 0;
    harp_set_error(HARP_SUCCESS, nullptr);
  }
  g_fail_nc_close = 1;
  {
    harp_product* imp = nullptr;
    EXPECT_EQ(-1, harp_import_netcdf(nc_file.c_str(), &imp));
    EXPECT_EQ(HARP_ERROR_NETCDF, harp_errno);
    EXPECT_EQ(nullptr, imp);
  }
  g_fail_nc_close = 0;

  // 3. Metadata import nc_inq_att and nc_close failures.
  for (int n = 2; n <= 8; ++n) {
    harp_product_metadata* meta = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_metadata_new(&meta));
    ScopedHarpProductMetadata meta_guard(meta);
    g_fail_nc_inq_att = n;
    harp_import_metadata_netcdf(nc_file.c_str(), meta);
    g_fail_nc_inq_att = 0;
    harp_set_error(HARP_SUCCESS, nullptr);
  }
  {
    harp_product_metadata* meta = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_metadata_new(&meta));
    ScopedHarpProductMetadata meta_guard(meta);
    g_fail_nc_close = 1;
    EXPECT_EQ(-1, harp_import_metadata_netcdf(nc_file.c_str(), meta));
    EXPECT_EQ(HARP_ERROR_NETCDF, harp_errno);
    g_fail_nc_close = 0;
  }
}

}  // namespace
}  // namespace stcorp_harp
