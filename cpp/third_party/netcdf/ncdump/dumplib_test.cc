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

// Unit tests for //third_party/netcdf/ncdump/dumplib.c.
//
// Covers safebuf string builders, format selection, type initialization across
// classic and netCDF-4 datasets, value equality and string-conversion
// callbacks, coordinate-variable checks, type-name formatting,
// unlimited-dimension lookup, and error exits inside isolated forked children.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/ncdump_globals_stub.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"

namespace netcdf {
namespace {

using ScopedSafebuf = std::unique_ptr<safebuf_t, decltype(&sbuf_free)>;

ScopedSafebuf MakeSafebuf() { return ScopedSafebuf(sbuf_new(), &sbuf_free); }

template <typename Fn>
CliResult RunChild(Fn&& fn) {
  auto entry = [&fn](int /*argc*/, char** /*argv*/) -> int {
    const int rc = std::forward<Fn>(fn)();
    ncdump_stub_reset_globals();
    return rc;
  };
  return RunCliInForkedChild(entry, {});
}

// Checks equality inside a forked child and prints diagnostics to stderr.
bool CheckStrEq(std::string_view label, const char* expected,
                const char* actual) {
  const std::string_view act = actual ? actual : "<null>";
  if (expected == nullptr || actual == nullptr ||
      std::strcmp(expected, actual) != 0) {
    std::fprintf(stderr, "Mismatch [%.*s]: expected='%s' actual='%.*s'\n",
                 static_cast<int>(label.size()), label.data(),
                 expected ? expected : "<null>", static_cast<int>(act.size()),
                 act.data());
    return false;
  }
  return true;
}

TEST(DumplibTest, SafebufCopyConcatAndGrowWorks) {
  ScopedSafebuf sb1 = MakeSafebuf();
  ASSERT_NE(nullptr, sb1.get());
  EXPECT_EQ(0, sbuf_len(sb1.get()));
  EXPECT_STREQ("", sbuf_str(sb1.get()));

  sbuf_cpy(sb1.get(), "alpha");
  EXPECT_EQ(5, sbuf_len(sb1.get()));
  EXPECT_STREQ("alpha", sbuf_str(sb1.get()));

  sbuf_cat(sb1.get(), "-beta");
  EXPECT_EQ(10, sbuf_len(sb1.get()));
  EXPECT_STREQ("alpha-beta", sbuf_str(sb1.get()));

  // Trigger single-step buffer growth past the 128-byte initial capacity.
  const std::string block_160(160, 'x');
  sbuf_cpy(sb1.get(), block_160.c_str());
  EXPECT_EQ(160, sbuf_len(sb1.get()));
  EXPECT_EQ(block_160, sbuf_str(sb1.get()));

  // Trigger multi-doubling growth when concatenating another large block.
  const std::string block_420(420, 'y');
  sbuf_cat(sb1.get(), block_420.c_str());
  EXPECT_EQ(580, sbuf_len(sb1.get()));
  EXPECT_EQ(block_160 + block_420, sbuf_str(sb1.get()));

  ScopedSafebuf sb2 = MakeSafebuf();
  sbuf_cpy(sb2.get(), "prefix:");
  sbuf_catb(sb2.get(), sb1.get());
  EXPECT_EQ(587, sbuf_len(sb2.get()));
  EXPECT_EQ("prefix:" + block_160 + block_420, sbuf_str(sb2.get()));
}

TEST(DumplibTest, SetFormatsAndGetFmtCoverDefaultsOverridesAndCFormat) {
  absl::Cleanup restore_globals([]() {
    float_precision_specified = 0;
    double_precision_specified = 0;
    set_formats(FLT_DIGITS, DBL_DIGITS);
    ncdump_stub_reset_globals();
  });

  set_formats(5, 11);
  EXPECT_STREQ("%.5g", float_var_fmt);
  EXPECT_STREQ("%.11g", double_var_fmt);
  EXPECT_STREQ("%#.5gf", float_att_fmt);
  EXPECT_STREQ("%#.5g", float_attx_fmt);
  EXPECT_STREQ("%#.11g", double_att_fmt);

  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR, nc_create_mem("get_fmt.nc", NC_NETCDF4, 4096, nc.ptr()));
  int dimid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "x", 2, &dimid));

  int v_plain = -1;
  int v_cfmt = -1;
  int v_cfmt_f = -1;
  int v_cfmt_d = -1;
  int v_bad_type = -1;
  int v_empty = -1;
  int v_too_long = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc.get(), "plain", NC_DOUBLE, 1, &dimid, &v_plain));
  ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "cfmt", NC_INT, 1, &dimid, &v_cfmt));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc.get(), "cfmt_f", NC_FLOAT, 1, &dimid, &v_cfmt_f));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc.get(), "cfmt_d", NC_DOUBLE, 1, &dimid, &v_cfmt_d));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc.get(), "bad_type", NC_INT, 1, &dimid, &v_bad_type));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc.get(), "empty", NC_INT, 1, &dimid, &v_empty));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc.get(), "too_long", NC_INT, 1, &dimid, &v_too_long));

  ASSERT_EQ(NC_NOERR, nc_put_att_text(nc.get(), v_cfmt, "C_format", 4, "%04x"));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(nc.get(), v_cfmt_f, "C_format", 4, "%.2f"));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(nc.get(), v_cfmt_d, "C_format", 4, "%.3f"));
  const int bad_fmt_val = 42;
  ASSERT_EQ(NC_NOERR, nc_put_att_int(nc.get(), v_bad_type, "C_format", NC_INT,
                                     1, &bad_fmt_val));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(nc.get(), v_empty, "C_format", 0, ""));
  const std::string hundred_chars(100, 'c');
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(nc.get(), v_too_long, "C_format",
                            hundred_chars.size(), hundred_chars.data()));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));

  struct DefaultFmtCase {
    nc_type type;
    const char* expected;
  };
  constexpr DefaultFmtCase kCases[] = {
      {NC_BYTE, "%d"},          {NC_CHAR, "%s"},     {NC_SHORT, "%d"},
      {NC_INT, "%d"},           {NC_FLOAT, "%.5g"},  {NC_DOUBLE, "%.11g"},
      {NC_UBYTE, "%u"},         {NC_USHORT, "%u"},   {NC_UINT, "%u"},
      {NC_INT64, "%lld"},       {NC_UINT64, "%llu"}, {NC_STRING, "\"%s\""},
      {NC_FIRSTUSERTYPEID, ""},
  };
  for (const DefaultFmtCase& tc : kCases) {
    EXPECT_STREQ(tc.expected, get_fmt(nc.get(), v_plain, tc.type));
  }

  EXPECT_STREQ("%04x", get_fmt(nc.get(), v_cfmt, NC_INT));
  EXPECT_STREQ("%d", get_fmt(nc.get(), v_bad_type, NC_INT));
  EXPECT_STREQ("%d", get_fmt(nc.get(), v_empty, NC_INT));
  EXPECT_STREQ("%d", get_fmt(nc.get(), v_too_long, NC_INT));

  // Explicit float/double precision flags override any C_format attribute.
  float_precision_specified = 1;
  EXPECT_STREQ("%.5g", get_fmt(nc.get(), v_cfmt_f, NC_FLOAT));
  EXPECT_STREQ("%.3f", get_fmt(nc.get(), v_cfmt_d, NC_DOUBLE));

  double_precision_specified = 1;
  EXPECT_STREQ("%.11g", get_fmt(nc.get(), v_cfmt_d, NC_DOUBLE));

  // Invalid ncid logs an inquiry error to stderr and falls back to default fmt.
  const CliResult err_res = RunChild([]() -> int {
    const char* fmt = get_fmt(-1, 0, NC_INT);
    return CheckStrEq("invalid_ncid_fmt", "%d", fmt) ? 0 : 1;
  });
  ASSERT_TRUE(err_res.ok()) << err_res.harness_error;
  EXPECT_EQ(0, err_res.exit_code) << err_res.stderr_output;
  EXPECT_THAT(err_res.stderr_output,
              ::testing::HasSubstr("Inquiring about 'C_format' attribute"));
}

TEST(DumplibTest, InitTypesCoversFormatsAndUserTypeHierarchy) {
  struct FormatCase {
    const char* name;
    int cmode;
    nc_type max_prim_type;
  };
  constexpr FormatCase kFormats[] = {
      {"classic", 0, NC_DOUBLE},
      {"offset64", NC_64BIT_OFFSET, NC_DOUBLE},
      {"cdf5", NC_CDF5, NC_UINT64},
      {"nc4_classic", NC_NETCDF4 | NC_CLASSIC_MODEL, NC_DOUBLE},
  };

  for (const FormatCase& tc : kFormats) {
    SCOPED_TRACE(tc.name);
    const CliResult res = RunChild([&tc]() -> int {
      ScopedNcFile nc;
      if (nc_create_mem("fmt.nc", tc.cmode, 4096, nc.ptr()) != NC_NOERR) {
        return 1;
      }
      init_types(nc.get());
      nctype_t* t_byte = get_typeinfo(NC_BYTE);
      nctype_t* t_max = get_typeinfo(tc.max_prim_type);
      if (t_byte == nullptr || t_max == nullptr) {
        return 1;
      }
      if (is_user_defined_type(tc.max_prim_type) != 0) {
        return 1;
      }
      return 0;
    });
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
  }

  // NetCDF-4 with nuser == 0 and a subgroup.
  const CliResult empty_nc4_res = RunChild([]() -> int {
    ScopedNcFile nc;
    if (nc_create_mem("empty_nc4.nc", NC_NETCDF4, 4096, nc.ptr()) != NC_NOERR) {
      return 1;
    }
    int subid = -1;
    if (nc_def_grp(nc.get(), "sub", &subid) != NC_NOERR) {
      return 1;
    }
    init_types(nc.get());
    return (get_typeinfo(NC_STRING) != nullptr &&
            is_user_defined_type(NC_STRING) == 0)
               ? 0
               : 1;
  });
  ASSERT_TRUE(empty_nc4_res.ok()) << empty_nc4_res.harness_error;
  EXPECT_EQ(0, empty_nc4_res.exit_code) << empty_nc4_res.stderr_output;

  // NetCDF-4 with user-defined types spread across root and nested subgroups.
  const CliResult nested_nc4_res = RunChild([]() -> int {
    struct NestedRec {
      int scalar;
      int vec[2];
    };
    static_assert(std::is_standard_layout_v<NestedRec>);

    ScopedNcFile nc;
    if (nc_create_mem("nested_nc4.nc", NC_NETCDF4, 4096, nc.ptr()) !=
        NC_NOERR) {
      return 1;
    }
    int grp1 = -1;
    int grp2 = -1;
    if (nc_def_grp(nc.get(), "grp1", &grp1) != NC_NOERR ||
        nc_def_grp(grp1, "grp2", &grp2) != NC_NOERR) {
      return 1;
    }
    nc_type root_enum = 0;
    nc_type grp1_opq = 0;
    nc_type grp2_vlen = 0;
    nc_type grp2_comp = 0;
    if (nc_def_enum(nc.get(), NC_INT, "e_root", &root_enum) != NC_NOERR) {
      return 1;
    }
    const int e_val = 1;
    if (nc_insert_enum(nc.get(), root_enum, "ONE", &e_val) != NC_NOERR ||
        nc_def_opaque(grp1, 3, "o_grp1", &grp1_opq) != NC_NOERR ||
        nc_def_vlen(grp2, "v_grp2", NC_INT, &grp2_vlen) != NC_NOERR ||
        nc_def_compound(grp2, sizeof(NestedRec), "c_grp2", &grp2_comp) !=
            NC_NOERR ||
        nc_insert_compound(grp2, grp2_comp, "scalar",
                           offsetof(NestedRec, scalar), NC_INT) != NC_NOERR) {
      return 1;
    }
    int dim_sizes[1] = {2};
    if (nc_insert_array_compound(grp2, grp2_comp, "vec",
                                 offsetof(NestedRec, vec), NC_INT, 1,
                                 dim_sizes) != NC_NOERR) {
      return 1;
    }

    init_types(nc.get());
    for (const nc_type tid : {root_enum, grp1_opq, grp2_vlen, grp2_comp}) {
      if (is_user_defined_type(tid) == 0 || get_typeinfo(tid) == nullptr) {
        return 1;
      }
    }
    return 0;
  });
  ASSERT_TRUE(nested_nc4_res.ok()) << nested_nc4_res.harness_error;
  EXPECT_EQ(0, nested_nc4_res.exit_code) << nested_nc4_res.stderr_output;

  // Invalid ncid in init_types prints diagnostic in max_typeid and exits 1.
  const CliResult bad_id_res = RunChild([]() -> int {
    init_types(-1);
    return 0;
  });
  ASSERT_TRUE(bad_id_res.ok()) << bad_id_res.harness_error;
  EXPECT_EQ(1, bad_id_res.exit_code);
  EXPECT_THAT(bad_id_res.stderr_output,
              ::testing::HasSubstr("Cannot get file format"));
}

TEST(DumplibTest, PrimitiveValEqualsCoversAllTwelveTypesAndFloatEdgeCases) {
  const CliResult res = RunChild([]() -> int {
    ScopedNcFile nc;
    if (nc_create_mem("eq_prim.nc", NC_NETCDF4, 4096, nc.ptr()) != NC_NOERR) {
      return 1;
    }
    init_epsilons();
    init_types(nc.get());

    auto eq = [](nc_type tid, const void* a, const void* b) -> bool {
      nctype_t* t = get_typeinfo(tid);
      return t->val_equals(t, a, b) != 0;
    };

    const signed char b1 = -42, b2 = 42;
    const char c1 = 'A', c2 = 'B';
    const short s1 = -1234, s2 = 1234;
    const int i1 = -100000, i2 = 100000;
    const unsigned char ub1 = 200, ub2 = 201;
    const unsigned short us1 = 50000, us2 = 50001;
    const unsigned int ui1 = 3000000000U, ui2 = 3000000001U;
    const long long i64_1 = -9000000000000LL, i64_2 = 9000000000000LL;
    const unsigned long long ui64_1 = 9000000000000ULL,
                             ui64_2 = 9000000000001ULL;

    if (!eq(NC_BYTE, &b1, &b1) || eq(NC_BYTE, &b1, &b2)) return 1;
    if (!eq(NC_CHAR, &c1, &c1) || eq(NC_CHAR, &c1, &c2)) return 1;
    if (!eq(NC_SHORT, &s1, &s1) || eq(NC_SHORT, &s1, &s2)) return 1;
    if (!eq(NC_INT, &i1, &i1) || eq(NC_INT, &i1, &i2)) return 1;
    if (!eq(NC_UBYTE, &ub1, &ub1) || eq(NC_UBYTE, &ub1, &ub2)) return 1;
    if (!eq(NC_USHORT, &us1, &us1) || eq(NC_USHORT, &us1, &us2)) return 1;
    if (!eq(NC_UINT, &ui1, &ui1) || eq(NC_UINT, &ui1, &ui2)) return 1;
    if (!eq(NC_INT64, &i64_1, &i64_1) || eq(NC_INT64, &i64_1, &i64_2)) return 1;
    if (!eq(NC_UINT64, &ui64_1, &ui64_1) || eq(NC_UINT64, &ui64_1, &ui64_2))
      return 1;

    // Float comparisons: finite, epsilon-close, distinct, signs, NaN, +/-Inf.
    const float f_one = 1.0F;
    const float f_close = 1.0F + 0.5F * std::numeric_limits<float>::epsilon();
    const float f_far = 1.05F;
    const float f_neg = -1.0F;
    const float f_zero = 0.0F;
    const float f_nan = std::numeric_limits<float>::quiet_NaN();
    const float f_pinf = std::numeric_limits<float>::infinity();
    const float f_ninf = -std::numeric_limits<float>::infinity();
    if (!eq(NC_FLOAT, &f_one, &f_one)) return 1;
    if (!eq(NC_FLOAT, &f_one, &f_close)) return 1;
    if (eq(NC_FLOAT, &f_one, &f_far)) return 1;
    if (eq(NC_FLOAT, &f_one, &f_neg)) return 1;
    if (eq(NC_FLOAT, &f_zero, &f_one)) return 1;
    if (!eq(NC_FLOAT, &f_nan, &f_nan)) return 1;
    if (!eq(NC_FLOAT, &f_pinf, &f_pinf)) return 1;
    if (!eq(NC_FLOAT, &f_ninf, &f_ninf)) return 1;
    if (eq(NC_FLOAT, &f_pinf, &f_ninf)) return 1;
    if (eq(NC_FLOAT, &f_nan, &f_ninf)) return 1;
    if (eq(NC_FLOAT, &f_pinf, &f_one)) return 1;

    // Double comparisons: same matrix plus negative epsilon magnitude.
    const double d_one = 1.0;
    const double d_close = 1.0 + 0.5 * std::numeric_limits<double>::epsilon();
    const double d_far = 1.05;
    const double d_neg = -10.0;
    const double d_zero = 0.0;
    const double d_nan = std::numeric_limits<double>::quiet_NaN();
    const double d_pinf = std::numeric_limits<double>::infinity();
    const double d_ninf = -std::numeric_limits<double>::infinity();
    if (!eq(NC_DOUBLE, &d_one, &d_one)) return 1;
    if (!eq(NC_DOUBLE, &d_one, &d_close)) return 1;
    if (!eq(NC_DOUBLE, &d_neg, &d_neg)) return 1;
    if (eq(NC_DOUBLE, &d_one, &d_far)) return 1;
    if (eq(NC_DOUBLE, &d_one, &d_neg)) return 1;
    if (eq(NC_DOUBLE, &d_zero, &d_one)) return 1;
    if (!eq(NC_DOUBLE, &d_nan, &d_nan)) return 1;
    if (!eq(NC_DOUBLE, &d_pinf, &d_pinf)) return 1;
    if (!eq(NC_DOUBLE, &d_ninf, &d_ninf)) return 1;
    if (eq(NC_DOUBLE, &d_pinf, &d_ninf)) return 1;
    if (eq(NC_DOUBLE, &d_nan, &d_ninf)) return 1;
    if (eq(NC_DOUBLE, &d_pinf, &d_one)) return 1;

    // String comparisons across NULL combinations and content matches.
    char s_abc1[] = "abc";
    char s_abc2[] = "abc";
    char s_abd[] = "abd";
    char* p_null = nullptr;
    char* p_abc1 = s_abc1;
    char* p_abc2 = s_abc2;
    char* p_abd = s_abd;
    if (!eq(NC_STRING, &p_null, &p_null)) return 1;
    if (eq(NC_STRING, &p_abc1, &p_null)) return 1;
    if (eq(NC_STRING, &p_null, &p_abc1)) return 1;
    if (!eq(NC_STRING, &p_abc1, &p_abc2)) return 1;
    if (eq(NC_STRING, &p_abc1, &p_abd)) return 1;

    return 0;
  });
  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;
}

TEST(DumplibTest, UserDefinedValEqualsCoversOpaqueVlenAndCompound) {
  const CliResult res = RunChild([]() -> int {
    struct ScalarComp {
      int a;
      double b;
    };
    struct InnerArray {
      int arr[1];
    };
    struct OuterNested {
      InnerArray item;
    };
    static_assert(std::is_standard_layout_v<ScalarComp>);
    static_assert(std::is_standard_layout_v<InnerArray>);
    static_assert(std::is_standard_layout_v<OuterNested>);
    static_assert(sizeof(OuterNested) == sizeof(InnerArray));

    ScopedNcFile nc;
    if (nc_create_mem("eq_udt.nc", NC_NETCDF4, 4096, nc.ptr()) != NC_NOERR) {
      return 1;
    }

    nc_type opq_tid = 0;
    nc_type vlen_tid = 0;
    nc_type sc_tid = 0;
    nc_type inner_tid = 0;
    nc_type outer_tid = 0;
    if (nc_def_opaque(nc.get(), 4, "opq4", &opq_tid) != NC_NOERR ||
        nc_def_vlen(nc.get(), "vint", NC_INT, &vlen_tid) != NC_NOERR ||
        nc_def_compound(nc.get(), sizeof(ScalarComp), "ScalarComp", &sc_tid) !=
            NC_NOERR ||
        nc_insert_compound(nc.get(), sc_tid, "a", offsetof(ScalarComp, a),
                           NC_INT) != NC_NOERR ||
        nc_insert_compound(nc.get(), sc_tid, "b", offsetof(ScalarComp, b),
                           NC_DOUBLE) != NC_NOERR) {
      return 1;
    }

    int dim_one[1] = {1};
    if (nc_def_compound(nc.get(), sizeof(InnerArray), "InnerArray",
                        &inner_tid) != NC_NOERR ||
        nc_insert_array_compound(nc.get(), inner_tid, "arr",
                                 offsetof(InnerArray, arr), NC_INT, 1,
                                 dim_one) != NC_NOERR ||
        nc_def_compound(nc.get(), sizeof(OuterNested), "OuterNested",
                        &outer_tid) != NC_NOERR ||
        nc_insert_compound(nc.get(), outer_tid, "item",
                           offsetof(OuterNested, item),
                           inner_tid) != NC_NOERR) {
      return 1;
    }

    init_epsilons();
    init_types(nc.get());

    // Opaque comparison.
    const std::array<unsigned char, 4> opq_a = {0x01, 0x02, 0x03, 0x04};
    const std::array<unsigned char, 4> opq_b = {0x01, 0x02, 0xFF, 0x04};
    nctype_t* opq_info = get_typeinfo(opq_tid);
    if (opq_info->val_equals(opq_info, opq_a.data(), opq_a.data()) == 0 ||
        opq_info->val_equals(opq_info, opq_a.data(), opq_b.data()) != 0) {
      return 1;
    }

    // Vlen comparison using elements padded to sizeof(nc_vlen_t) matching
    // ncvlen_val_equals's stride.
    struct VlenSlot {
      int val;
      unsigned char pad[sizeof(nc_vlen_t) - sizeof(int)]{};
    };
    static_assert(sizeof(VlenSlot) == sizeof(nc_vlen_t));
    std::array<VlenSlot, 2> va{};
    std::array<VlenSlot, 2> vb{};
    std::memset(va.data(), 0, sizeof(va));
    std::memset(vb.data(), 0, sizeof(vb));
    va[0].val = 10;
    va[1].val = 20;
    vb[0].val = 10;
    vb[1].val = 20;

    nc_vlen_t v_len2_a{2, va.data()};
    nc_vlen_t v_len2_b{2, vb.data()};
    nc_vlen_t v_len1{1, va.data()};
    nctype_t* vlen_info = get_typeinfo(vlen_tid);
    if (vlen_info->val_equals(vlen_info, &v_len2_a, &v_len1) != 0) return 1;
    if (vlen_info->val_equals(vlen_info, &v_len2_a, &v_len2_b) == 0) return 1;
    vb[1].val = 99;
    if (vlen_info->val_equals(vlen_info, &v_len2_a, &v_len2_b) != 0) return 1;

    // Scalar compound comparison.
    ScalarComp s1{};
    ScalarComp s2{};
    std::memset(&s1, 0, sizeof(s1));
    std::memset(&s2, 0, sizeof(s2));
    s1.a = 5;
    s1.b = 3.5;
    s2 = s1;
    nctype_t* sc_info = get_typeinfo(sc_tid);
    if (sc_info->val_equals(sc_info, &s1, &s2) == 0) return 1;
    s2.b = 9.5;
    if (sc_info->val_equals(sc_info, &s1, &s2) != 0) return 1;

    // Nested compound with rank > 0 array field on InnerArray so
    // finfo->ranks != 0 && finfo->ranks[0] > 0 exercises the array branch
    // cleanly within 2-element backing storage.
    std::array<OuterNested, 2> o1{};
    std::array<OuterNested, 2> o2{};
    std::memset(o1.data(), 0, sizeof(o1));
    std::memset(o2.data(), 0, sizeof(o2));
    o1[0].item.arr[0] = 7;
    o2[0].item.arr[0] = 7;
    nctype_t* outer_info = get_typeinfo(outer_tid);
    if (outer_info->val_equals(outer_info, &o1[0], &o2[0]) == 0) return 1;
    o2[0].item.arr[0] = 8;
    if (outer_info->val_equals(outer_info, &o1[0], &o2[0]) != 0) return 1;

    return 0;
  });
  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;
}

TEST(DumplibTest, PrimitiveTypTostringAndValTostringCoverAllTypesAndEscapes) {
  const CliResult res = RunChild([]() -> int {
    ScopedNcFile nc;
    if (nc_create_mem("str_prim.nc", NC_NETCDF4, 4096, nc.ptr()) != NC_NOERR) {
      return 1;
    }
    set_formats(FLT_DIGITS, DBL_DIGITS);
    init_types(nc.get());
    ScopedSafebuf sb = MakeSafebuf();

    auto check_both = [&](nc_type tid, const void* valp, const char* expected,
                          const char* override_fmt = nullptr) -> bool {
      nctype_t* ti = get_typeinfo(tid);
      ti->typ_tostring(ti, sb.get(), valp);
      if (!CheckStrEq("typ", expected, sbuf_str(sb.get()))) return false;

      ncvar_t var{};
      var.type = tid;
      var.tinfo = ti;
      var.fmt = override_fmt ? override_fmt : get_fmt(nc.get(), NC_GLOBAL, tid);
      set_tostring_func(&var);
      var.val_tostring(&var, sb.get(), valp);
      return CheckStrEq("val", expected, sbuf_str(sb.get()));
    };

    const signed char v_b = -42;
    const short v_s = -1234;
    const int v_i = 987654;
    const float v_f = 1.5F;
    const float f_nan = std::numeric_limits<float>::quiet_NaN();
    const float f_pinf = std::numeric_limits<float>::infinity();
    const float f_ninf = -std::numeric_limits<float>::infinity();
    const double v_d = 2.25;
    const double d_nan = std::numeric_limits<double>::quiet_NaN();
    const double d_pinf = std::numeric_limits<double>::infinity();
    const double d_ninf = -std::numeric_limits<double>::infinity();
    const unsigned char v_ub = 250;
    const unsigned short v_us = 65000;
    const unsigned int v_ui = 4000000000U;
    const long long v_i64 = -9000000000000LL;
    const unsigned long long v_ui64 = 9000000000000ULL;

    if (!check_both(NC_BYTE, &v_b, "-42")) return 1;
    if (!check_both(NC_SHORT, &v_s, "-1234")) return 1;
    if (!check_both(NC_INT, &v_i, "987654")) return 1;
    if (!check_both(NC_FLOAT, &v_f, "1.5")) return 1;
    if (!check_both(NC_FLOAT, &f_nan, "NaNf")) return 1;
    if (!check_both(NC_FLOAT, &f_pinf, "Infinityf")) return 1;
    if (!check_both(NC_FLOAT, &f_ninf, "-Infinityf")) return 1;
    if (!check_both(NC_DOUBLE, &v_d, "2.25")) return 1;
    if (!check_both(NC_DOUBLE, &d_nan, "NaN")) return 1;
    if (!check_both(NC_DOUBLE, &d_pinf, "Infinity")) return 1;
    if (!check_both(NC_DOUBLE, &d_ninf, "-Infinity")) return 1;
    if (!check_both(NC_UBYTE, &v_ub, "250")) return 1;
    if (!check_both(NC_USHORT, &v_us, "65000")) return 1;
    if (!check_both(NC_UINT, &v_ui, "4000000000")) return 1;
    if (!check_both(NC_INT64, &v_i64, "-9000000000000")) return 1;
    if (!check_both(NC_UINT64, &v_ui64, "9000000000000")) return 1;

    // NC_CHAR: typ_tostring passes a 2-byte C-string for "%s"; val_tostring
    // passes a single char, so supply an explicit "%c" format for val_tostring.
    const char v_c = 'K';
    {
      nctype_t* ci = get_typeinfo(NC_CHAR);
      ci->typ_tostring(ci, sb.get(), &v_c);
      if (!CheckStrEq("char_typ", "K", sbuf_str(sb.get()))) return 1;
      ncvar_t cvar{};
      cvar.type = NC_CHAR;
      cvar.tinfo = ci;
      cvar.fmt = "%c";
      set_tostring_func(&cvar);
      cvar.val_tostring(&cvar, sb.get(), &v_c);
      if (!CheckStrEq("char_val", "K", sbuf_str(sb.get()))) return 1;
    }

    // NC_STRING: NULL -> NIL, empty, and all escape switches (including '\v'
    // mapped to "\n" in dumplib.c and iscntrl() octal formatting).
    char* null_sp = nullptr;
    if (!check_both(NC_STRING, &null_sp, "NIL")) return 1;

    char empty_buf[] = "";
    char* empty_sp = empty_buf;
    if (!check_both(NC_STRING, &empty_sp, "\"\"")) return 1;

    char esc_buf[] = "\b\f\n\r\t\v\\'\"A\x01\x7f";
    char* esc_sp = esc_buf;
    if (!check_both(NC_STRING, &esc_sp,
                    "\"\\b\\f\\n\\r\\t\\n\\\\\\'\\\"A\\001\\177\"")) {
      return 1;
    }

    return 0;
  });
  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;
}

TEST(DumplibTest, UserDefinedTypTostringAndValTostringCoverAllClasses) {
  const CliResult res = RunChild([]() -> int {
    struct FullRec {
      char tag;
      int id;
      char str1d[14];
      char grid2d[2][5];
      int nums[3];
    };
    static_assert(std::is_standard_layout_v<FullRec>);

    ScopedNcFile nc;
    if (nc_create_mem("str_udt.nc", NC_NETCDF4, 8192, nc.ptr()) != NC_NOERR) {
      return 1;
    }
    set_formats(FLT_DIGITS, DBL_DIGITS);

    // 1. Opaque
    nc_type opq_tid = 0;
    if (nc_def_opaque(nc.get(), 4, "opq4", &opq_tid) != NC_NOERR) return 1;

    // 2. All 8 integer enum base types.
    struct EnumSamples {
      // Non-negative NC_BYTE avoids signed/unsigned char mismatch between
      // ncenum_typ_tostring (*(signed char*)) and NC4_inq_enum_ident
      // (*(char*)).
      signed char b = 3;
      unsigned char ub = 7;
      short s = -200;
      unsigned short us = 200;
      int i = -4000;
      unsigned int ui = 4000;
      long long i64 = -80000LL;
      unsigned long long ui64 = 80000ULL;
    };
    const EnumSamples enum_samples;
    auto enum_val_ptr = [&enum_samples](nc_type base) -> const void* {
      switch (base) {
        case NC_BYTE:
          return &enum_samples.b;
        case NC_UBYTE:
          return &enum_samples.ub;
        case NC_SHORT:
          return &enum_samples.s;
        case NC_USHORT:
          return &enum_samples.us;
        case NC_INT:
          return &enum_samples.i;
        case NC_UINT:
          return &enum_samples.ui;
        case NC_INT64:
          return &enum_samples.i64;
        case NC_UINT64:
          return &enum_samples.ui64;
        default:
          return nullptr;
      }
    };
    struct EnumDef {
      nc_type base;
      const char* name;
      nc_type tid;
    };
    std::array<EnumDef, 8> enums = {{
        {NC_BYTE, "eb", 0},
        {NC_UBYTE, "eub", 0},
        {NC_SHORT, "es", 0},
        {NC_USHORT, "eus", 0},
        {NC_INT, "ei", 0},
        {NC_UINT, "eui", 0},
        {NC_INT64, "ei64", 0},
        {NC_UINT64, "eui64", 0},
    }};
    for (EnumDef& ed : enums) {
      if (nc_def_enum(nc.get(), ed.base, ed.name, &ed.tid) != NC_NOERR) {
        return 1;
      }
      const void* vp = enum_val_ptr(ed.base);
      if (vp == nullptr ||
          nc_insert_enum(nc.get(), ed.tid, "SYM", vp) != NC_NOERR) {
        return 1;
      }
    }

    // 3. Vlen of NC_INT.
    nc_type vlen_tid = 0;
    if (nc_def_vlen(nc.get(), "vint", NC_INT, &vlen_tid) != NC_NOERR) return 1;
    int vdim = -1;
    int vvar = -1;
    if (nc_def_dim(nc.get(), "d", 3, &vdim) != NC_NOERR ||
        nc_def_var(nc.get(), "vv", vlen_tid, 1, &vdim, &vvar) != NC_NOERR) {
      return 1;
    }

    // 4. Compound covering scalar char, scalar int, 1D char array, 2D char
    // array, and 1D numeric array.
    nc_type comp_tid = 0;
    if (nc_def_compound(nc.get(), sizeof(FullRec), "FullRec", &comp_tid) !=
            NC_NOERR ||
        nc_insert_compound(nc.get(), comp_tid, "tag", offsetof(FullRec, tag),
                           NC_CHAR) != NC_NOERR ||
        nc_insert_compound(nc.get(), comp_tid, "id", offsetof(FullRec, id),
                           NC_INT) != NC_NOERR) {
      return 1;
    }
    int d14[1] = {14};
    int d2x5[2] = {2, 5};
    int d3[1] = {3};
    if (nc_insert_array_compound(nc.get(), comp_tid, "str1d",
                                 offsetof(FullRec, str1d), NC_CHAR, 1,
                                 d14) != NC_NOERR ||
        nc_insert_array_compound(nc.get(), comp_tid, "grid2d",
                                 offsetof(FullRec, grid2d), NC_CHAR, 2,
                                 d2x5) != NC_NOERR ||
        nc_insert_array_compound(nc.get(), comp_tid, "nums",
                                 offsetof(FullRec, nums), NC_INT, 1,
                                 d3) != NC_NOERR) {
      return 1;
    }

    if (nc_enddef(nc.get()) != NC_NOERR) return 1;

    std::array<int, 1> v1_data = {42};
    std::array<int, 3> v3_data = {10, 20, 30};
    std::array<nc_vlen_t, 3> v_in = {{
        {0, nullptr},
        {v1_data.size(), v1_data.data()},
        {v3_data.size(), v3_data.data()},
    }};
    if (nc_put_var(nc.get(), vvar, v_in.data()) != NC_NOERR) return 1;

    init_types(nc.get());
    ScopedSafebuf sb = MakeSafebuf();

    // Check ncopaque_val_as_hex(0) and 4-byte opaque typ/val formatting.
    char hex_empty[16]{};
    const unsigned char dummy_byte = 0;
    if (ncopaque_val_as_hex(0, hex_empty, &dummy_byte) != 2 ||
        !CheckStrEq("opq_zero", "0X", hex_empty)) {
      return 1;
    }
    const std::array<unsigned char, 4> opq_val = {0x0A, 0xBC, 0x00, 0xFF};
    {
      nctype_t* ti = get_typeinfo(opq_tid);
      ti->typ_tostring(ti, sb.get(), opq_val.data());
      if (!CheckStrEq("opq_typ", "0X0ABC00FF", sbuf_str(sb.get()))) return 1;
      ncvar_t var{};
      var.type = opq_tid;
      var.tinfo = ti;
      set_tostring_func(&var);
      var.val_tostring(&var, sb.get(), opq_val.data());
      if (!CheckStrEq("opq_val", "0X0ABC00FF", sbuf_str(sb.get()))) return 1;
    }

    // Check all 8 enum base types.
    for (const EnumDef& ed : enums) {
      nctype_t* ti = get_typeinfo(ed.tid);
      ncvar_t var{};
      var.type = ed.tid;
      var.tinfo = ti;
      set_tostring_func(&var);

      const void* vp = enum_val_ptr(ed.base);
      if (vp == nullptr) return 1;
      ti->typ_tostring(ti, sb.get(), vp);
      if (!CheckStrEq(ed.name, "SYM", sbuf_str(sb.get()))) return 1;
      var.val_tostring(&var, sb.get(), vp);
      if (!CheckStrEq(ed.name, "SYM", sbuf_str(sb.get()))) return 1;
    }

    // Check vlen reading back from file (0, 1, 3 elements).
    ScopedNcVlens v_out(3);
    if (nc_get_var(nc.get(), vvar, v_out.data()) != NC_NOERR) return 1;
    nctype_t* vlen_info = get_typeinfo(vlen_tid);
    ncvar_t vlen_var{};
    vlen_var.type = vlen_tid;
    vlen_var.tinfo = vlen_info;
    set_tostring_func(&vlen_var);
    const std::array<const char*, 3> expected_vlen = {"{}", "{42}",
                                                      "{10, 20, 30}"};
    for (size_t i = 0; i < 3; ++i) {
      vlen_info->typ_tostring(vlen_info, sb.get(), &v_out[i]);
      if (!CheckStrEq("vlen_typ", expected_vlen[i], sbuf_str(sb.get())))
        return 1;
      vlen_var.val_tostring(&vlen_var, sb.get(), &v_out[i]);
      if (!CheckStrEq("vlen_val", expected_vlen[i], sbuf_str(sb.get())))
        return 1;
    }
    v_out.clear();

    // Check compound formatting (scalar char + int + 1D chars + 2D chars +
    // int array). Notice chars_tostring escapes special chars as '\' + raw uc
    // and non-printables as "\\%03o".
    FullRec rec{};
    std::memset(&rec, 0, sizeof(rec));
    rec.tag = 'X';
    rec.id = -7;
    const char src1d[] = "\b\f\n\r\t\v\\'\"\x01z\x7f";
    std::memcpy(rec.str1d, src1d, sizeof(src1d) - 1);  // leaves trailing '\0'
    std::memcpy(rec.grid2d[0], "hi", 2);
    std::memcpy(rec.grid2d[1], "yo", 2);
    rec.nums[0] = 1;
    rec.nums[1] = 2;
    rec.nums[2] = 3;

    const std::string expected_comp =
        "{\"X\", -7, {\"\\\b\\\f\\\n\\\r\\\t\\\v\\\\\\'\\\"\\001z\\177\"}, "
        "{\"hi\", \"yo\"}, {1, 2, 3}}";
    nctype_t* comp_info = get_typeinfo(comp_tid);
    comp_info->typ_tostring(comp_info, sb.get(), &rec);
    if (!CheckStrEq("comp_typ", expected_comp.c_str(), sbuf_str(sb.get()))) {
      return 1;
    }
    ncvar_t comp_var{};
    comp_var.type = comp_tid;
    comp_var.tinfo = comp_info;
    set_tostring_func(&comp_var);
    comp_var.val_tostring(&comp_var, sb.get(), &rec);
    if (!CheckStrEq("comp_val", expected_comp.c_str(), sbuf_str(sb.get()))) {
      return 1;
    }

    // All-NUL 1D char array trims down to length 0 -> {""}.
    std::memset(rec.str1d, 0, sizeof(rec.str1d));
    comp_info->typ_tostring(comp_info, sb.get(), &rec);
    const std::string expected_empty_chars =
        "{\"X\", -7, {\"\"}, {\"hi\", \"yo\"}, {1, 2, 3}}";
    if (!CheckStrEq("comp_nul_chars", expected_empty_chars.c_str(),
                    sbuf_str(sb.get()))) {
      return 1;
    }

    return 0;
  });
  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;
}

TEST(DumplibTest, NctimeValTostringCoversNumericTypesIsoSeparatorAndNonFinite) {
  const CliResult res = RunChild([]() -> int {
    ScopedNcFile nc;
    if (nc_create_mem("nctime_val.nc", NC_NETCDF4, 4096, nc.ptr()) !=
        NC_NOERR) {
      return 1;
    }
    init_types(nc.get());
    ScopedSafebuf sb = MakeSafebuf();

    char units_buf[] = "days since 2000-01-01 00:00:00";
    timeinfo_t ti{};
    ti.calendar = cdMixed;
    ti.unit = cdDay;
    ti.units = units_buf;

    formatting_specs.string_times = 1;
    formatting_specs.iso_separator = 0;

    auto format_time = [&](nc_type tid, const void* valp,
                           const char* expected) -> bool {
      ncvar_t var{};
      var.type = tid;
      var.tinfo = get_typeinfo(tid);
      var.has_timeval = 1;
      var.timeinfo = &ti;
      set_tostring_func(&var);
      var.val_tostring(&var, sb.get(), valp);
      return CheckStrEq("time_val", expected, sbuf_str(sb.get()));
    };

    const signed char b = 1;
    const short s = 2;
    const int i = 3;
    const float f = 0.5F;
    const double d = 1.5;
    const unsigned char ub = 4;
    const unsigned short us = 5;
    const unsigned int ui = 6;
    const long long i64 = 7;
    const unsigned long long ui64 = 8;

    if (!format_time(NC_BYTE, &b, "\"2000-01-02\"")) return 1;
    if (!format_time(NC_SHORT, &s, "\"2000-01-03\"")) return 1;
    if (!format_time(NC_INT, &i, "\"2000-01-04\"")) return 1;
    if (!format_time(NC_FLOAT, &f, "\"2000-01-01 12\"")) return 1;
    if (!format_time(NC_DOUBLE, &d, "\"2000-01-02 12\"")) return 1;
    if (!format_time(NC_UBYTE, &ub, "\"2000-01-05\"")) return 1;
    if (!format_time(NC_USHORT, &us, "\"2000-01-06\"")) return 1;
    if (!format_time(NC_UINT, &ui, "\"2000-01-07\"")) return 1;
    if (!format_time(NC_INT64, &i64, "\"2000-01-08\"")) return 1;
    if (!format_time(NC_UINT64, &ui64, "\"2000-01-09\"")) return 1;

    formatting_specs.iso_separator = 1;
    if (!format_time(NC_DOUBLE, &d, "\"2000-01-02T12\"")) return 1;

    const double d_nan = std::numeric_limits<double>::quiet_NaN();
    const double d_pinf = std::numeric_limits<double>::infinity();
    const double d_ninf = -std::numeric_limits<double>::infinity();
    if (!format_time(NC_DOUBLE, &d_nan, "NaN")) return 1;
    if (!format_time(NC_DOUBLE, &d_pinf, "Infinity")) return 1;
    if (!format_time(NC_DOUBLE, &d_ninf, "-Infinity")) return 1;

    return 0;
  });
  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;
}

TEST(DumplibTest, FatalErrorPathsExitCleanlyInChild) {
  {
    const CliResult res = RunChild([]() -> int {
      get_typeinfo(-1);
      return 0;
    });
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stderr_output, ::testing::HasSubstr("invalid type id"));
  }
  {
    const CliResult res = RunChild([]() -> int {
      ScopedNcFile nc;
      if (nc_create_mem("bad_hi.nc", 0, 4096, nc.ptr()) != NC_NOERR) return 2;
      init_types(nc.get());
      get_typeinfo(999);
      return 0;
    });
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stderr_output, ::testing::HasSubstr("invalid type id"));
  }
  {
    // Non-user-defined slot with bogus tid triggers prim_type_name's default.
    const CliResult res = RunChild([]() -> int {
      ScopedNcFile nc;
      if (nc_create_mem("bad_prim.nc", NC_NETCDF4, 4096, nc.ptr()) !=
          NC_NOERR) {
        return 2;
      }
      init_types(nc.get());
      nctype_t bogus{};
      bogus.tid = 13;
      bogus.nc_class_field = 0;
      typeadd(&bogus);
      char name_buf[NC_MAX_NAME + 1]{};
      get_type_name(nc.get(), 13, name_buf);
      return 0;
    });
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stderr_output,
                ::testing::HasSubstr("prim_type_name: bad type"));
  }
  {
    const CliResult res = RunChild([]() -> int {
      ScopedNcFile nc;
      if (nc_create_mem("bad_enum.nc", NC_NETCDF4, 4096, nc.ptr()) !=
          NC_NOERR) {
        return 2;
      }
      nc_type etid = 0;
      if (nc_def_enum(nc.get(), NC_INT, "e", &etid) != NC_NOERR) return 2;
      init_types(nc.get());
      nctype_t* real_enum = get_typeinfo(etid);
      nctype_t fake_enum = *real_enum;
      fake_enum.base_tid = NC_FLOAT;
      ScopedSafebuf sb = MakeSafebuf();
      const int v = 0;
      fake_enum.typ_tostring(&fake_enum, sb.get(), &v);
      return 0;
    });
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stderr_output,
                ::testing::HasSubstr("bad base type for enum"));
  }
  {
    const CliResult res = RunChild([]() -> int {
      ncvar_t var{};
      var.type = NC_CHAR;
      ScopedSafebuf sb = MakeSafebuf();
      const char ch = 'a';
      nctime_val_tostring(&var, sb.get(), &ch);
      return 0;
    });
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stderr_output,
                ::testing::HasSubstr("type not numeric primitive"));
  }
  {
    const CliResult res = RunChild([]() -> int {
      ScopedNcFile nc;
      if (nc_create_mem("bad_cls.nc", NC_NETCDF4, 4096, nc.ptr()) != NC_NOERR) {
        return 2;
      }
      nc_type udt_tid = 0;
      if (nc_def_opaque(nc.get(), 4, "opq", &udt_tid) != NC_NOERR) return 2;
      init_types(nc.get());
      nctype_t fake_udt{};
      fake_udt.nc_class_field = 99;
      ncvar_t var{};
      var.type = udt_tid;
      var.tinfo = &fake_udt;
      set_tostring_func(&var);
      return 0;
    });
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
    EXPECT_THAT(res.stderr_output, ::testing::HasSubstr("unrecognized class"));
  }
  {
    const CliResult res = RunChild([]() -> int {
      static_cast<void>(is_unlim_dim(-1, 0));
      return 0;
    });
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(1, res.exit_code);
  }
}

TEST(DumplibTest, IscoordvarDetectsCoordinateVarsAndParentDimensions) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR, nc_create_mem("coord.nc", NC_NETCDF4, 4096, nc.ptr()));
  int dim_x = -1;
  int dim_y = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "x", 3, &dim_x));
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "y", 2, &dim_y));

  int v_scalar = -1;
  int v_other = -1;
  int v_2d_same = -1;
  int v_coord_y = -1;
  const int dims_xy[2] = {dim_x, dim_y};
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc.get(), "scalar", NC_INT, 0, nullptr, &v_scalar));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc.get(), "other", NC_INT, 1, &dim_x, &v_other));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc.get(), "x", NC_INT, 2, dims_xy, &v_2d_same));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc.get(), "y", NC_DOUBLE, 1, &dim_y, &v_coord_y));

  int subgrp = -1;
  int v_sub_coord_x = -1;
  ASSERT_EQ(NC_NOERR, nc_def_grp(nc.get(), "sub", &subgrp));
  ASSERT_EQ(NC_NOERR,
            nc_def_var(subgrp, "x", NC_INT, 1, &dim_x, &v_sub_coord_x));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));

  EXPECT_EQ(0, iscoordvar(nc.get(), v_scalar));
  EXPECT_EQ(0, iscoordvar(nc.get(), v_other));
  EXPECT_EQ(0, iscoordvar(nc.get(), v_2d_same));
  EXPECT_EQ(1, iscoordvar(nc.get(), v_coord_y));
  EXPECT_EQ(1, iscoordvar(subgrp, v_sub_coord_x));
}

TEST(DumplibTest, GetTypeNameAndPrintTypeNameHandleInheritanceAndPaths) {
  const CliResult res = RunChild([]() -> int {
    ScopedNcFile nc;
    if (nc_create_mem("type_names.nc", NC_NETCDF4, 4096, nc.ptr()) !=
        NC_NOERR) {
      return 1;
    }
    int grp_a = -1;
    int grp_b = -1;
    if (nc_def_grp(nc.get(), "grp_a", &grp_a) != NC_NOERR ||
        nc_def_grp(nc.get(), "grp b", &grp_b) != NC_NOERR) {
      return 1;
    }
    nc_type root_enum = 0;
    nc_type sib_enum = 0;
    if (nc_def_enum(nc.get(), NC_INT, "root_e", &root_enum) != NC_NOERR ||
        nc_def_enum(grp_b, NC_INT, "sib type", &sib_enum) != NC_NOERR) {
      return 1;
    }

    init_types(nc.get());

    char name_buf[NC_MAX_NAME + 1]{};
    get_type_name(nc.get(), NC_STRING, name_buf);
    if (!CheckStrEq("prim_name", "string", name_buf)) return 1;
    get_type_name(grp_b, sib_enum, name_buf);
    if (!CheckStrEq("udt_name", "sib type", name_buf)) return 1;

    print_type_name(nc.get(), NC_INT);
    std::fputs("|", stdout);
    print_type_name(nc.get(), root_enum);
    std::fputs("|", stdout);
    // Inherited from root ancestor: prints unqualified simple name.
    print_type_name(grp_a, root_enum);
    std::fputs("|", stdout);
    // Non-ancestor sibling group: prints escaped absolute group path + name.
    print_type_name(grp_a, sib_enum);
    std::fflush(stdout);
    return 0;
  });
  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;
  EXPECT_EQ("int|root_e|root_e|/grp\\ b/sib\\ type", res.stdout_output);
}

TEST(DumplibTest, IsUnlimDimHandlesRootAndSubgroupFixedAndUnlimitedDims) {
  const CliResult res = RunChild([]() -> int {
    ScopedNcFile nc;
    if (nc_create_mem("unlim.nc", NC_NETCDF4, 4096, nc.ptr()) != NC_NOERR) {
      return 1;
    }
    int r_fixed = -1;
    int r_unlim = -1;
    if (nc_def_dim(nc.get(), "lat", 4, &r_fixed) != NC_NOERR ||
        nc_def_dim(nc.get(), "time", NC_UNLIMITED, &r_unlim) != NC_NOERR) {
      return 1;
    }
    int sub = -1;
    int s_fixed = -1;
    int s_unlim = -1;
    if (nc_def_grp(nc.get(), "sub", &sub) != NC_NOERR ||
        nc_def_dim(sub, "z", 2, &s_fixed) != NC_NOERR ||
        nc_def_dim(sub, "rec", NC_UNLIMITED, &s_unlim) != NC_NOERR) {
      return 1;
    }

    // Call first with a subgroup grpid so init_is_unlim walks to root group.
    if (is_unlim_dim(sub, s_unlim) != 1) return 1;
    if (is_unlim_dim(sub, s_fixed) != 0) return 1;
    if (is_unlim_dim(nc.get(), r_fixed) != 0) return 1;
    if (is_unlim_dim(nc.get(), r_unlim) != 1) return 1;
    return 0;
  });
  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;
}

}  // namespace
}  // namespace netcdf
