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

// Unit tests for //third_party/netcdf/ncgen/genbin.c.
//
// Drives genbin_netcdf() and genbin_close() in isolated forked children across
// NetCDF-4 / classic / CDF5 formats, groups, user-defined types, special
// variable properties (storage, chunksizes, deflate, shuffle, fletcher32,
// endianness, fill, filters, quantization), all primitive/user-defined
// attribute writers, header-only / nofill modes, and CHECK_ERR error paths,
// then verifies the generated binary files in-process.

#include <csetjmp>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/google/util/ncgen_main_shim.h"
// clang-format on

extern "C" {
extern FILE* ncgin;
}

namespace netcdf {
namespace {

constexpr int kQuantizeBitGroomAlgo = NC_QUANTIZE_BITGROOM;
constexpr int kQuantizeGranularBrAlgo = NC_QUANTIZE_GRANULARBR;
constexpr int kQuantizeBitRoundAlgo = NC_QUANTIZE_BITROUND;

enum class ExpectedOutcome {
  kSuccess,
  kFinalizeExit,
  kNonFatalError,
};

struct ChildRunResult {
  bool exited_via_finalize = false;
  int finalize_ret = -1;
  int parse_rc = -1;
  int error_count_after = -1;
};

// Keep sigsetjmp in a plain frame with no C++ destructors crossing siglongjmp.
void RunSemanticsAndGenbinPlain(bool catch_finalize, ChildRunResult* out) {
  sigjmp_buf env;
  int retcode = -1;
  if (catch_finalize) {
    if (sigsetjmp(env, 1) != 0) {
      out->exited_via_finalize = true;
      out->finalize_ret = retcode;
      ncgen_shim_set_finalize_jmp(nullptr, nullptr);
      return;
    }
    ncgen_shim_set_finalize_jmp(&env, &retcode);
  }

  processsemantics();
  genbin_netcdf();
  genbin_close();
  ncgen_shim_set_finalize_jmp(nullptr, nullptr);
  out->exited_via_finalize = false;
  out->error_count_after = error_count;
}

void ConfigureFormatGlobals(int k_mode) {
  k_flag = k_mode;
  switch (k_mode) {
    case NC_FORMAT_CLASSIC:
      usingclassic = 1;
      cmode_modifier = 0;
      break;
    case NC_FORMAT_64BIT_OFFSET:
      usingclassic = 1;
      cmode_modifier = NC_64BIT_OFFSET;
      break;
    case NC_FORMAT_64BIT_DATA:
      usingclassic = 1;
      cmode_modifier = NC_CDF5;
      break;
    case NC_FORMAT_NETCDF4_CLASSIC:
      usingclassic = 1;
      cmode_modifier = NC_NETCDF4 | NC_CLASSIC_MODEL;
      break;
    case NC_FORMAT_NETCDF4:
    default:
      usingclassic = 0;
      cmode_modifier = NC_NETCDF4;
      break;
  }
}

::testing::AssertionResult ExecuteCdlThroughGenbin(
    std::string_view cdl_text, const char* out_path, int k_mode, bool nofill,
    bool hdr_only, ExpectedOutcome expected_outcome) {
  TempFile cdl_file("genbin_input.cdl");
  {
    std::ofstream out(cdl_file.path(), std::ios::binary);
    out.write(cdl_text.data(), static_cast<std::streamsize>(cdl_text.size()));
    if (!out.good()) {
      return ::testing::AssertionFailure() << "Failed writing CDL temp file";
    }
  }

  const std::string cdl_path = cdl_file.string();
  const std::string out_path_str(out_path);

  return RunInForkedChild([&]() -> bool {
    static char kCdlName[] = "genbin_test.cdl";
    char mutable_out[4096]{};
    if (out_path_str.size() >= sizeof(mutable_out)) {
      return false;
    }
    std::memcpy(mutable_out, out_path_str.data(), out_path_str.size());

    ncgen_shim_reset_globals();
    cdlname = kCdlName;
    l_flag = L_BINARY;
    nofill_flag = nofill ? 1 : 0;
    header_only = hdr_only ? 1 : 0;
    netcdf_name = mutable_out;
    ConfigureFormatGlobals(k_mode);

    FILE* fp = std::fopen(cdl_path.c_str(), "r");
    if (fp == nullptr) {
      return false;
    }
    parse_init();
    ncgin = fp;
    const int parse_rc = ncgparse();
    std::fclose(fp);
    ncgin = nullptr;

    if (parse_rc != 0) {
      std::fprintf(stderr, "ncgparse() failed with rc=%d\n", parse_rc);
      return false;
    }

    ChildRunResult run_res{};
    run_res.parse_rc = parse_rc;
    const bool catch_finalize =
        (expected_outcome == ExpectedOutcome::kFinalizeExit);
    RunSemanticsAndGenbinPlain(catch_finalize, &run_res);

    switch (expected_outcome) {
      case ExpectedOutcome::kFinalizeExit:
        return run_res.exited_via_finalize && run_res.finalize_ret == 1;
      case ExpectedOutcome::kNonFatalError:
        if (run_res.exited_via_finalize || run_res.error_count_after <= 0) {
          return false;
        }
        cleanup();
        netcdf_name = nullptr;
        cdlname = nullptr;
        return true;
      case ExpectedOutcome::kSuccess:
        if (run_res.exited_via_finalize || run_res.error_count_after != 0) {
          return false;
        }
        cleanup();
        netcdf_name = nullptr;
        cdlname = nullptr;
        return true;
    }
    return false;
  });
}

TEST(GenbinTest, Netcdf4TypesGroupsSpecialsAttrsAndDataRoundTrip) {
  static constexpr char kRichCdl[] =
      "netcdf rich_genbin {\n"
      "types:\n"
      "  opaque(4) opq4_t;\n"
      "  int enum status_t { ST_IDLE = 0, ST_RUN = 1, ST_ERR = 2 };\n"
      "  int(*) ivlen_t;\n"
      "  compound rec_t {\n"
      "    short tag;\n"
      "    int vec(2, 2);\n"
      "    double score;\n"
      "  };\n"
      "dimensions:\n"
      "  n = 4;\n"
      "  m = 3;\n"
      "  u = UNLIMITED;\n"
      "variables:\n"
      "  int scalar_v;\n"
      "  int nodata_v(n);\n"
      "  int contig_v(n);\n"
      "  short compact_v(m);\n"
      "  int uncomp_chunked(n);\n"
      "  float chunked_flt(u, m);\n"
      "  double filtered_bg(n);\n"
      "  float quant_gbr(n);\n"
      "  double quant_br(n);\n"
      "  rec_t cmpd_v(m);\n"
      "  ivlen_t vlen_v(m);\n"
      "  opq4_t opq_v;\n"
      "  status_t enum_v(m);\n"
      "  string str_v(m);\n"
      "\n"
      "  // Storage & filter specials\n"
      "  contig_v:_Storage = \"contiguous\";\n"
      "  compact_v:_Storage = \"compact\";\n"
      "  compact_v:_Endianness = \"big\";\n"
      "  compact_v:_NoFill = \"true\";\n"
      "  uncomp_chunked:_Storage = \"chunked\";\n"
      "  uncomp_chunked:_ChunkSizes = 2;\n"
      "  uncomp_chunked:_DeflateLevel = -1;\n"
      "  uncomp_chunked:_Shuffle = \"false\";\n"
      "  chunked_flt:_Storage = \"chunked\";\n"
      "  chunked_flt:_ChunkSizes = 2, 3;\n"
      "  chunked_flt:_Fletcher32 = \"true\";\n"
      "  chunked_flt:_DeflateLevel = 4;\n"
      "  chunked_flt:_Shuffle = \"true\";\n"
      "  chunked_flt:_Endianness = \"little\";\n"
      "  chunked_flt:_NoFill = \"false\";\n"
      "  filtered_bg:_Storage = \"chunked\";\n"
      "  filtered_bg:_ChunkSizes = 4;\n"
      "  filtered_bg:_Filter = \"1, 5\";\n"
      "  filtered_bg:_QuantizeBitGroomNumberOfSignificantDigits = 4;\n"
      "  quant_gbr:_Storage = \"chunked\";\n"
      "  quant_gbr:_ChunkSizes = 4;\n"
      "  quant_gbr:_QuantizeGranularBitRoundNumberOfSignificantDigits = 3;\n"
      "  quant_br:_Storage = \"chunked\";\n"
      "  quant_br:_ChunkSizes = 4;\n"
      "  quant_br:_QuantizeBitRoundNumberOfSignificantDigits = 8;\n"
      "\n"
      "  // Per-variable attributes\n"
      "  scalar_v:v_note = \"var-attr\";\n"
      "  scalar_v:v_empty = \"\";\n"
      "  scalar_v:" NC_FillValue
      " = -999;\n"
      "\n"
      "  // Global attributes spanning all 12 primitive types + user types\n"
      "  byte :g_byte = -10b, 20b;\n"
      "  char :g_char = \"netcdf-genbin\";\n"
      "  char :g_empty_char = \"\";\n"
      "  short :g_short = -123s, 456s;\n"
      "  int :g_int = -1000, 2000;\n"
      "  float :g_float = 1.25f, -2.5f;\n"
      "  double :g_double = 3.5, -6.25;\n"
      "  string :g_string = \"alpha\", \"beta\";\n"
      "  ubyte :g_ubyte = 200UB, 255UB;\n"
      "  ushort :g_ushort = 40000US, 50000US;\n"
      "  uint :g_uint = 3000000000U, 4000000000U;\n"
      "  int64 :g_int64 = -9000000000LL, 9000000000LL;\n"
      "  uint64 :g_uint64 = 12000000000ULL, 18000000000ULL;\n"
      "  status_t :g_enum = ST_IDLE, ST_ERR;\n"
      "  opq4_t :g_opq = 0xDEADBEEF;\n"
      "  ivlen_t :g_vlen = {10, 20}, {30};\n"
      "\n"
      "data:\n"
      "  scalar_v = 42;\n"
      "  contig_v = 10, 20, 30, 40;\n"
      "  compact_v = -1s, 2s, -3s;\n"
      "  uncomp_chunked = 5, 6, 7, 8;\n"
      "  chunked_flt = 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f;\n"
      "  filtered_bg = 1.25, 2.5, 3.75, 5.0;\n"
      "  quant_gbr = 1.5f, 2.5f, 3.5f, 4.5f;\n"
      "  quant_br = 8.0, 9.0, 10.0, 11.0;\n"
      "  cmpd_v = {11s, {1, 2, 3, 4}, 1.5}, {22s, {5, 6, 7, 8}, 2.5}, "
      "{33s, {9, 10, 11, 12}, 3.5};\n"
      "  vlen_v = {100, 200}, {300}, {400, 500, 600};\n"
      "  opq_v = 0xCAFEBABE;\n"
      "  enum_v = ST_IDLE, ST_RUN, ST_ERR;\n"
      "  str_v = \"first\", \"second\", \"third\";\n"
      "\n"
      "group: subgrp {\n"
      "  dimensions:\n"
      "    z = 2;\n"
      "  variables:\n"
      "    int gvar(z);\n"
      "  data:\n"
      "    gvar = 7, 8;\n"
      "}\n"
      "}\n";

  TempFile out_nc("genbin_rich.nc");
  ASSERT_TRUE(ExecuteCdlThroughGenbin(
      kRichCdl, out_nc.c_str(), NC_FORMAT_NETCDF4, /*nofill=*/false,
      /*hdr_only=*/false, ExpectedOutcome::kSuccess));

  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR, nc_open(out_nc.c_str(), NC_NOWRITE, nc.ptr()));

  int fmt = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_format(nc.get(), &fmt));
  EXPECT_EQ(NC_FORMAT_NETCDF4, fmt);

  // Verify child group and variable.
  int subgrp_id = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_grp_ncid(nc.get(), "subgrp", &subgrp_id));
  int gvar_id = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_varid(subgrp_id, "gvar", &gvar_id));
  int gvar_vals[2]{};
  ASSERT_EQ(NC_NOERR, nc_get_var_int(subgrp_id, gvar_id, gvar_vals));
  EXPECT_THAT(gvar_vals, ::testing::ElementsAre(7, 8));

  // Verify user-defined types.
  nc_type opq_tid = 0;
  nc_type enum_tid = 0;
  nc_type vlen_tid = 0;
  nc_type rec_tid = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(nc.get(), "opq4_t", &opq_tid));
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(nc.get(), "status_t", &enum_tid));
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(nc.get(), "ivlen_t", &vlen_tid));
  ASSERT_EQ(NC_NOERR, nc_inq_typeid(nc.get(), "rec_t", &rec_tid));

  size_t opq_sz = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_opaque(nc.get(), opq_tid, nullptr, &opq_sz));
  EXPECT_EQ(4u, opq_sz);

  nc_type enum_base = 0;
  size_t enum_nmembers = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_enum(nc.get(), enum_tid, nullptr, &enum_base,
                                  nullptr, &enum_nmembers));
  EXPECT_EQ(NC_INT, enum_base);
  EXPECT_EQ(3u, enum_nmembers);

  nc_type vlen_base = 0;
  ASSERT_EQ(NC_NOERR,
            nc_inq_vlen(nc.get(), vlen_tid, nullptr, nullptr, &vlen_base));
  EXPECT_EQ(NC_INT, vlen_base);

  size_t rec_size = 0;
  size_t rec_nfields = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_compound(nc.get(), rec_tid, nullptr, &rec_size,
                                      &rec_nfields));
  EXPECT_EQ(3u, rec_nfields);
  int field1_ndims = 0;
  int field1_dims[2]{};
  ASSERT_EQ(NC_NOERR,
            nc_inq_compound_field(nc.get(), rec_tid, 1, nullptr, nullptr,
                                  nullptr, &field1_ndims, field1_dims));
  EXPECT_EQ(2, field1_ndims);
  EXPECT_THAT(field1_dims, ::testing::ElementsAre(2, 2));

  // Verify special attributes on variables.
  auto get_varid = [&](const char* name) {
    int id = -1;
    EXPECT_EQ(NC_NOERR, nc_inq_varid(nc.get(), name, &id)) << name;
    return id;
  };

  int contig_storage = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(nc.get(), get_varid("contig_v"),
                                          &contig_storage, nullptr));
  EXPECT_EQ(NC_CONTIGUOUS, contig_storage);

  const int compact_id = get_varid("compact_v");
  int compact_storage = -1;
  int compact_endian = -1;
  int compact_nofill = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(nc.get(), compact_id,
                                          &compact_storage, nullptr));
  ASSERT_EQ(NC_NOERR, nc_inq_var_endian(nc.get(), compact_id, &compact_endian));
  ASSERT_EQ(NC_NOERR,
            nc_inq_var_fill(nc.get(), compact_id, &compact_nofill, nullptr));
  EXPECT_EQ(NC_COMPACT, compact_storage);
  EXPECT_EQ(NC_ENDIAN_BIG, compact_endian);
  EXPECT_EQ(1, compact_nofill);

  const int uncomp_id = get_varid("uncomp_chunked");
  int shuffle = -1;
  int deflate = -1;
  int deflate_level = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_var_deflate(nc.get(), uncomp_id, &shuffle,
                                         &deflate, &deflate_level));
  EXPECT_EQ(0, shuffle);
  EXPECT_EQ(0, deflate);

  const int chunked_id = get_varid("chunked_flt");
  int chunked_storage = -1;
  size_t chunks[2]{};
  int fletcher32 = -1;
  int c_endian = -1;
  int c_nofill = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_var_chunking(nc.get(), chunked_id,
                                          &chunked_storage, chunks));
  ASSERT_EQ(NC_NOERR, nc_inq_var_fletcher32(nc.get(), chunked_id, &fletcher32));
  ASSERT_EQ(NC_NOERR, nc_inq_var_deflate(nc.get(), chunked_id, &shuffle,
                                         &deflate, &deflate_level));
  ASSERT_EQ(NC_NOERR, nc_inq_var_endian(nc.get(), chunked_id, &c_endian));
  ASSERT_EQ(NC_NOERR,
            nc_inq_var_fill(nc.get(), chunked_id, &c_nofill, nullptr));
  EXPECT_EQ(NC_CHUNKED, chunked_storage);
  EXPECT_THAT(chunks, ::testing::ElementsAre(2u, 3u));
  EXPECT_EQ(NC_FLETCHER32, fletcher32);
  EXPECT_EQ(1, shuffle);
  EXPECT_EQ(1, deflate);
  EXPECT_EQ(4, deflate_level);
  EXPECT_EQ(NC_ENDIAN_LITTLE, c_endian);
  EXPECT_EQ(0, c_nofill);

  const int filt_id = get_varid("filtered_bg");
  unsigned int fid = 0;
  size_t nparams = 0;
  unsigned int fparams[4]{};
  ASSERT_EQ(NC_NOERR,
            nc_inq_var_filter(nc.get(), filt_id, &fid, &nparams, fparams));
  EXPECT_EQ(1u, fid);
  EXPECT_EQ(1u, nparams);
  EXPECT_EQ(5u, fparams[0]);

  int q_mode = -1;
  int q_nsd = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_var_quantize(nc.get(), filt_id, &q_mode, &q_nsd));
  EXPECT_EQ(kQuantizeBitGroomAlgo, q_mode);
  EXPECT_EQ(4, q_nsd);

  ASSERT_EQ(NC_NOERR, nc_inq_var_quantize(nc.get(), get_varid("quant_gbr"),
                                          &q_mode, &q_nsd));
  EXPECT_EQ(kQuantizeGranularBrAlgo, q_mode);
  EXPECT_EQ(3, q_nsd);

  ASSERT_EQ(NC_NOERR, nc_inq_var_quantize(nc.get(), get_varid("quant_br"),
                                          &q_mode, &q_nsd));
  EXPECT_EQ(kQuantizeBitRoundAlgo, q_mode);
  EXPECT_EQ(8, q_nsd);

  // Verify scalar data and attributes.
  const int scalar_id = get_varid("scalar_v");
  int scalar_val = 0;
  ASSERT_EQ(NC_NOERR, nc_get_var_int(nc.get(), scalar_id, &scalar_val));
  EXPECT_EQ(42, scalar_val);
  int scalar_fill = 0;
  ASSERT_EQ(NC_NOERR,
            nc_get_att_int(nc.get(), scalar_id, NC_FillValue, &scalar_fill));
  EXPECT_EQ(-999, scalar_fill);
  char v_note[16]{};
  ASSERT_EQ(NC_NOERR, nc_get_att_text(nc.get(), scalar_id, "v_note", v_note));
  EXPECT_EQ(std::string_view(v_note, 8), "var-attr");
  size_t empty_len = 0;
  ASSERT_EQ(NC_NOERR,
            nc_inq_attlen(nc.get(), scalar_id, "v_empty", &empty_len));
  EXPECT_EQ(1u, empty_len);
  char v_empty[4]{};
  ASSERT_EQ(NC_NOERR, nc_get_att_text(nc.get(), scalar_id, "v_empty", v_empty));
  EXPECT_EQ('\0', v_empty[0]);

  // Verify global attributes across all 12 primitive types + user types.
  signed char g_byte[2]{};
  ASSERT_EQ(NC_NOERR, nc_get_att_schar(nc.get(), NC_GLOBAL, "g_byte", g_byte));
  EXPECT_THAT(g_byte, ::testing::ElementsAre(-10, 20));

  char g_char[32]{};
  ASSERT_EQ(NC_NOERR, nc_get_att_text(nc.get(), NC_GLOBAL, "g_char", g_char));
  EXPECT_EQ(std::string_view(g_char, 13), "netcdf-genbin");

  size_t g_empty_len = 0;
  ASSERT_EQ(NC_NOERR,
            nc_inq_attlen(nc.get(), NC_GLOBAL, "g_empty_char", &g_empty_len));
  EXPECT_EQ(1u, g_empty_len);

  short g_short[2]{};
  ASSERT_EQ(NC_NOERR,
            nc_get_att_short(nc.get(), NC_GLOBAL, "g_short", g_short));
  EXPECT_THAT(g_short, ::testing::ElementsAre(-123, 456));

  int g_int[2]{};
  ASSERT_EQ(NC_NOERR, nc_get_att_int(nc.get(), NC_GLOBAL, "g_int", g_int));
  EXPECT_THAT(g_int, ::testing::ElementsAre(-1000, 2000));

  float g_float[2]{};
  ASSERT_EQ(NC_NOERR,
            nc_get_att_float(nc.get(), NC_GLOBAL, "g_float", g_float));
  EXPECT_THAT(g_float, ::testing::ElementsAre(1.25F, -2.5F));

  double g_double[2]{};
  ASSERT_EQ(NC_NOERR,
            nc_get_att_double(nc.get(), NC_GLOBAL, "g_double", g_double));
  EXPECT_THAT(g_double, ::testing::ElementsAre(3.5, -6.25));

  {
    ScopedNcStrings g_strs(2);
    ASSERT_EQ(NC_NOERR, nc_get_att_string(nc.get(), NC_GLOBAL, "g_string",
                                          g_strs.data()));
    EXPECT_STREQ("alpha", g_strs[0]);
    EXPECT_STREQ("beta", g_strs[1]);
  }

  unsigned char g_ubyte[2]{};
  ASSERT_EQ(NC_NOERR,
            nc_get_att_uchar(nc.get(), NC_GLOBAL, "g_ubyte", g_ubyte));
  EXPECT_THAT(g_ubyte, ::testing::ElementsAre(200U, 255U));

  unsigned short g_ushort[2]{};
  ASSERT_EQ(NC_NOERR,
            nc_get_att_ushort(nc.get(), NC_GLOBAL, "g_ushort", g_ushort));
  EXPECT_THAT(g_ushort, ::testing::ElementsAre(40000U, 50000U));

  unsigned int g_uint[2]{};
  ASSERT_EQ(NC_NOERR, nc_get_att_uint(nc.get(), NC_GLOBAL, "g_uint", g_uint));
  EXPECT_THAT(g_uint, ::testing::ElementsAre(3000000000U, 4000000000U));

  long long g_int64[2]{};
  ASSERT_EQ(NC_NOERR,
            nc_get_att_longlong(nc.get(), NC_GLOBAL, "g_int64", g_int64));
  EXPECT_THAT(g_int64, ::testing::ElementsAre(-9000000000LL, 9000000000LL));

  unsigned long long g_uint64[2]{};
  ASSERT_EQ(NC_NOERR,
            nc_get_att_ulonglong(nc.get(), NC_GLOBAL, "g_uint64", g_uint64));
  EXPECT_THAT(g_uint64, ::testing::ElementsAre(12000000000ULL, 18000000000ULL));

  int g_enum_vals[2]{};
  ASSERT_EQ(NC_NOERR, nc_get_att(nc.get(), NC_GLOBAL, "g_enum", g_enum_vals));
  EXPECT_THAT(g_enum_vals, ::testing::ElementsAre(0, 2));

  unsigned char g_opq[4]{};
  ASSERT_EQ(NC_NOERR, nc_get_att(nc.get(), NC_GLOBAL, "g_opq", g_opq));
  EXPECT_THAT(g_opq, ::testing::ElementsAre(0xDE, 0xAD, 0xBE, 0xEF));

  {
    ScopedNcVlens g_vlen(2);
    ASSERT_EQ(NC_NOERR,
              nc_get_att(nc.get(), NC_GLOBAL, "g_vlen", g_vlen.data()));
    EXPECT_EQ(2u, g_vlen[0].len);
    EXPECT_EQ(1u, g_vlen[1].len);
    const auto* v0 = static_cast<const int*>(g_vlen[0].p);
    EXPECT_EQ(10, v0[0]);
    EXPECT_EQ(20, v0[1]);
    const auto* v1 = static_cast<const int*>(g_vlen[1].p);
    EXPECT_EQ(30, v1[0]);
  }

  // Verify variable data round-trip for user types, arrays, and strings.
  int contig_vals[4]{};
  ASSERT_EQ(NC_NOERR,
            nc_get_var_int(nc.get(), get_varid("contig_v"), contig_vals));
  EXPECT_THAT(contig_vals, ::testing::ElementsAre(10, 20, 30, 40));

  short compact_vals[3]{};
  ASSERT_EQ(NC_NOERR, nc_get_var_short(nc.get(), compact_id, compact_vals));
  EXPECT_THAT(compact_vals, ::testing::ElementsAre(-1, 2, -3));

  int uncomp_vals[4]{};
  ASSERT_EQ(NC_NOERR, nc_get_var_int(nc.get(), uncomp_id, uncomp_vals));
  EXPECT_THAT(uncomp_vals, ::testing::ElementsAre(5, 6, 7, 8));

  float chunked_vals[6]{};
  ASSERT_EQ(NC_NOERR, nc_get_var_float(nc.get(), chunked_id, chunked_vals));
  EXPECT_THAT(chunked_vals,
              ::testing::ElementsAre(1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F));

  unsigned char opq_val[4]{};
  ASSERT_EQ(NC_NOERR, nc_get_var(nc.get(), get_varid("opq_v"), opq_val));
  EXPECT_THAT(opq_val, ::testing::ElementsAre(0xCA, 0xFE, 0xBA, 0xBE));

  int enum_vals[3]{};
  ASSERT_EQ(NC_NOERR, nc_get_var(nc.get(), get_varid("enum_v"), enum_vals));
  EXPECT_THAT(enum_vals, ::testing::ElementsAre(0, 1, 2));

  {
    ScopedNcStrings str_vals(3);
    ASSERT_EQ(NC_NOERR,
              nc_get_var_string(nc.get(), get_varid("str_v"), str_vals.data()));
    EXPECT_STREQ("first", str_vals[0]);
    EXPECT_STREQ("second", str_vals[1]);
    EXPECT_STREQ("third", str_vals[2]);
  }

  {
    std::vector<nc_vlen_t> vlen_vals(3);
    auto reclaim_vlen = absl::MakeCleanup([&]() {
      EXPECT_EQ(NC_NOERR, nc_reclaim_data(nc.get(), vlen_tid, vlen_vals.data(),
                                          vlen_vals.size()));
    });
    ASSERT_EQ(NC_NOERR,
              nc_get_var(nc.get(), get_varid("vlen_v"), vlen_vals.data()));
    EXPECT_EQ(2u, vlen_vals[0].len);
    EXPECT_EQ(1u, vlen_vals[1].len);
    EXPECT_EQ(3u, vlen_vals[2].len);
  }

  size_t off0 = 0;
  size_t off1 = 0;
  size_t off2 = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_compound_fieldoffset(nc.get(), rec_tid, 0, &off0));
  ASSERT_EQ(NC_NOERR, nc_inq_compound_fieldoffset(nc.get(), rec_tid, 1, &off1));
  ASSERT_EQ(NC_NOERR, nc_inq_compound_fieldoffset(nc.get(), rec_tid, 2, &off2));
  std::vector<unsigned char> cmpd_buf(3 * rec_size);
  ASSERT_EQ(NC_NOERR,
            nc_get_var(nc.get(), get_varid("cmpd_v"), cmpd_buf.data()));
  short tag0 = 0;
  int vec0[4]{};
  double score2 = 0.0;
  std::memcpy(&tag0, cmpd_buf.data() + off0, sizeof(tag0));
  std::memcpy(vec0, cmpd_buf.data() + off1, sizeof(vec0));
  std::memcpy(&score2, cmpd_buf.data() + 2 * rec_size + off2, sizeof(score2));
  EXPECT_EQ(11, tag0);
  EXPECT_THAT(vec0, ::testing::ElementsAre(1, 2, 3, 4));
  EXPECT_DOUBLE_EQ(3.5, score2);
}

TEST(GenbinTest, EmptyDatasetClassicFormatsNoFillAndHeaderOnly) {
  static constexpr char kEmptyCdl[] = "netcdf empty_nc4 {}\n";
  TempFile empty_nc("genbin_empty.nc");
  ASSERT_TRUE(ExecuteCdlThroughGenbin(
      kEmptyCdl, empty_nc.c_str(), NC_FORMAT_NETCDF4, /*nofill=*/false,
      /*hdr_only=*/false, ExpectedOutcome::kSuccess));
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_open(empty_nc.c_str(), NC_NOWRITE, nc.ptr()));
    int ndims = -1;
    int nvars = -1;
    int ngatts = -1;
    ASSERT_EQ(NC_NOERR, nc_inq(nc.get(), &ndims, &nvars, &ngatts, nullptr));
    EXPECT_EQ(0, ndims);
    EXPECT_EQ(0, nvars);
    EXPECT_EQ(0, ngatts);
  }

  static constexpr char kClassicCdl[] =
      "netcdf classic_ds {\n"
      "dimensions:\n"
      "  u = UNLIMITED;\n"
      "  k = 2;\n"
      "variables:\n"
      "  int s;\n"
      "  int arr(u, k);\n"
      "  :title = \"classic\";\n"
      "data:\n"
      "  s = 99;\n"
      "  arr = 1, 2, 3, 4;\n"
      "}\n";

  // Header-only omits variable data writes so scalar remains at NC_FILL_INT.
  TempFile hdr_nc("genbin_hdr_only.nc");
  ASSERT_TRUE(ExecuteCdlThroughGenbin(
      kClassicCdl, hdr_nc.c_str(), NC_FORMAT_CLASSIC, /*nofill=*/false,
      /*hdr_only=*/true, ExpectedOutcome::kSuccess));
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_open(hdr_nc.c_str(), NC_NOWRITE, nc.ptr()));
    int fmt = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_format(nc.get(), &fmt));
    EXPECT_EQ(NC_FORMAT_CLASSIC, fmt);
    int sid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(nc.get(), "s", &sid));
    int sval = 0;
    ASSERT_EQ(NC_NOERR, nc_get_var_int(nc.get(), sid, &sval));
    EXPECT_EQ(NC_FILL_INT, sval);
  }

  struct FormatCase {
    const char* name;
    int k_mode;
    int expected_format;
  };
  constexpr FormatCase kFormats[] = {
      {"offset64", NC_FORMAT_64BIT_OFFSET, NC_FORMAT_64BIT_OFFSET},
      {"cdf5", NC_FORMAT_64BIT_DATA, NC_FORMAT_64BIT_DATA},
      {"nc4_classic", NC_FORMAT_NETCDF4_CLASSIC, NC_FORMAT_NETCDF4_CLASSIC},
  };
  for (const FormatCase& tc : kFormats) {
    SCOPED_TRACE(tc.name);
    TempFile out_nc(std::string("genbin_") + tc.name + ".nc");
    ASSERT_TRUE(ExecuteCdlThroughGenbin(
        kClassicCdl, out_nc.c_str(), tc.k_mode, /*nofill=*/true,
        /*hdr_only=*/false, ExpectedOutcome::kSuccess));
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_open(out_nc.c_str(), NC_NOWRITE, nc.ptr()));
    int fmt = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_format(nc.get(), &fmt));
    EXPECT_EQ(tc.expected_format, fmt);
    int sid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(nc.get(), "s", &sid));
    int sval = 0;
    ASSERT_EQ(NC_NOERR, nc_get_var_int(nc.get(), sid, &sval));
    EXPECT_EQ(99, sval);
  }
}

TEST(GenbinTest, FatalCheckErrAndSpecialValidationPaths) {
  static constexpr char kMinimalCdl[] =
      "netcdf min_err {\n"
      "dimensions:\n"
      "  n = 2;\n"
      "variables:\n"
      "  int v(n);\n"
      "data:\n"
      "  v = 1, 2;\n"
      "}\n";

  // Non-existent parent directory triggers CHECK_ERR on nc_create().
  EXPECT_TRUE(ExecuteCdlThroughGenbin(
      kMinimalCdl, "/nonexistent_genbin_parent_dir/bad.nc", NC_FORMAT_NETCDF4,
      /*nofill=*/false, /*hdr_only=*/false, ExpectedOutcome::kFinalizeExit));

  // Array NC_CHUNKED without _ChunkSizes logs derror() and defaults chunking.
  static constexpr char kChunkedNoSizesArrCdl[] =
      "netcdf bad_chunk_arr {\n"
      "dimensions:\n"
      "  n = 2;\n"
      "variables:\n"
      "  int v(n);\n"
      "  v:_Storage = \"chunked\";\n"
      "}\n";
  TempFile bad_chunk_arr_nc("genbin_bad_chunk_arr.nc");
  EXPECT_TRUE(ExecuteCdlThroughGenbin(
      kChunkedNoSizesArrCdl, bad_chunk_arr_nc.c_str(), NC_FORMAT_NETCDF4,
      /*nofill=*/false, /*hdr_only=*/false, ExpectedOutcome::kNonFatalError));

  // Scalar NC_CHUNKED without _ChunkSizes triggers derror() + CHECK_ERR().
  static constexpr char kChunkedScalarCdl[] =
      "netcdf bad_chunk_scalar {\n"
      "variables:\n"
      "  int s;\n"
      "  s:_Storage = \"chunked\";\n"
      "}\n";
  TempFile bad_chunk_scalar_nc("genbin_bad_chunk_scalar.nc");
  EXPECT_TRUE(ExecuteCdlThroughGenbin(
      kChunkedScalarCdl, bad_chunk_scalar_nc.c_str(), NC_FORMAT_NETCDF4,
      /*nofill=*/false, /*hdr_only=*/false, ExpectedOutcome::kFinalizeExit));

  // Unregistered filter ID within H5Z_FILTER_MAX (<= 65535) triggers
  // NC_ENOFILTER + derror() + fall-through CHECK_ERR().
  static constexpr char kMissingFilterCdl[] =
      "netcdf bad_filter {\n"
      "dimensions:\n"
      "  n = 4;\n"
      "variables:\n"
      "  int v(n);\n"
      "  v:_Storage = \"chunked\";\n"
      "  v:_ChunkSizes = 4;\n"
      "  v:_Filter = \"32013, 1\";\n"
      "}\n";
  TempFile bad_filt_nc("genbin_bad_filter.nc");
  EXPECT_TRUE(ExecuteCdlThroughGenbin(
      kMissingFilterCdl, bad_filt_nc.c_str(), NC_FORMAT_NETCDF4,
      /*nofill=*/false, /*hdr_only=*/false, ExpectedOutcome::kFinalizeExit));

  // Out-of-range HDF5 filter ID (> H5Z_FILTER_MAX) triggers NC_EHDFERR on
  // nc_inq_filter_avail() and hits the default CHECK_ERR() branch.
  static constexpr char kInvalidFilterIdCdl[] =
      "netcdf invalid_filter_id {\n"
      "dimensions:\n"
      "  n = 4;\n"
      "variables:\n"
      "  int v(n);\n"
      "  v:_Storage = \"chunked\";\n"
      "  v:_ChunkSizes = 4;\n"
      "  v:_Filter = \"99999, 1\";\n"
      "}\n";
  TempFile invalid_filt_nc("genbin_invalid_filter_id.nc");
  EXPECT_TRUE(ExecuteCdlThroughGenbin(
      kInvalidFilterIdCdl, invalid_filt_nc.c_str(), NC_FORMAT_NETCDF4,
      /*nofill=*/false, /*hdr_only=*/false, ExpectedOutcome::kFinalizeExit));
}

}  // namespace
}  // namespace netcdf
