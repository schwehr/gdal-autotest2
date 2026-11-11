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

// Unit tests for //third_party/netcdf/ncdump/nctime0.c.
//
// Covers time-unit validation, bounds-attribute detection and list management,
// calendar/units parsing in get_timeinfo(), and CDL time-comment printing in
// print_att_times().

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/ncdump_globals_stub.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"

namespace netcdf {
namespace {

template <typename Fn>
CliResult RunChild(Fn&& fn) {
  auto entry = [&fn](int /*argc*/, char** /*argv*/) -> int {
    const int rc = std::forward<Fn>(fn)();
    ncdump_stub_reset_globals();
    return rc;
  };
  return RunCliInForkedChild(entry, {});
}

void FreeTimeinfo(ncvar_t* vp) {
  if (vp->timeinfo != nullptr) {
    if (vp->timeinfo->units != nullptr) {
      std::free(vp->timeinfo->units);
    }
    std::free(vp->timeinfo);
    vp->timeinfo = nullptr;
  }
}

TEST(Nctime0Test, IsValidTimeUnitAcceptsSupportedUnitsAndRejectsInvalid) {
  constexpr std::array<const char*, 24> kValidUnits = {
      "seconds since 2000-01-01 00:00:00",
      "SEC since 2000-01-01T12:30:00",
      "s since 2000-01-01",
      "minutes since 2000-01-01 01:00:00",
      "min since 2000-01-01T01:00",
      "mn since 2000-01-01",
      "hours since 2000-01-01 00:00",
      "hour since 2000-01-01T00:00",
      "hr since 2000-01-01",
      "days since 2000-01-01 00:00:00",
      "day since 2000-01-01T00:00:00",
      "dy since 2000-01-01",
      "weeks since 2000-01-01 00:00:00",
      "week since 2000-01-01",
      "wk since 2000-01-01",
      "months since 2000-01-01",
      "month since 2000-01-01T00:00:00",
      "mo since 2000-01-01",
      "season since 2000-01-01",
      "seasons since 2000-01-01 00:00:00",
      "years since 2000-01-01",
      "year since 2000-01-01T00:00:00",
      "yr since 2000-01-01",
      "DAYS since 1-1",
  };
  for (const char* units : kValidUnits) {
    SCOPED_TRACE(units);
    EXPECT_EQ(1, is_valid_time_unit(units));
  }

  constexpr std::array<const char*, 6> kInvalidUnits = {
      "",
      "meters",
      "days",
      "days since",
      "fortnights since 2000-01-01 00:00:00",
      "epochs since 2000-01-01T00:00:00",
  };
  for (const char* units : kInvalidUnits) {
    SCOPED_TRACE(units);
    EXPECT_EQ(0, is_valid_time_unit(units));
  }
}

TEST(Nctime0Test, IsBoundsAttDetectsCharAndStringBoundsAttributes) {
  char val_buf[] = "time_bnds";
  char* str_ptr = val_buf;

  auto make_att = [](nc_type type, const char* name, void* valgp) {
    ncatt_t att{};
    att.type = type;
    std::strncpy(att.name, name, NC_MAX_NAME - 1);
    att.valgp = valgp;
    return att;
  };

  ncatt_t char_ok = make_att(NC_CHAR, "bounds", val_buf);
  ncatt_t str_ok = make_att(NC_STRING, "bounds", &str_ptr);
  ncatt_t char_wrong_name = make_att(NC_CHAR, "units", val_buf);
  ncatt_t str_wrong_name = make_att(NC_STRING, "units", &str_ptr);
  ncatt_t char_null = make_att(NC_CHAR, "bounds", nullptr);
  ncatt_t str_null = make_att(NC_STRING, "bounds", nullptr);
  ncatt_t int_wrong_type = make_att(NC_INT, "bounds", val_buf);

  EXPECT_EQ(1, is_bounds_att(&char_ok));
  EXPECT_EQ(1, is_bounds_att(&str_ok));
  EXPECT_EQ(0, is_bounds_att(&char_wrong_name));
  EXPECT_EQ(0, is_bounds_att(&str_wrong_name));
  EXPECT_EQ(0, is_bounds_att(&char_null));
  EXPECT_EQ(0, is_bounds_att(&str_null));
  EXPECT_EQ(0, is_bounds_att(&int_wrong_type));
}

TEST(Nctime0Test, GetTimeinfoHandlesMissingInvalidValidAndAllCalendarTypes) {
  const CliResult res = RunChild([]() -> int {
    ScopedNcFile nc;
    if (nc_create_mem("timeinfo.nc", NC_NETCDF4, 16384, nc.ptr()) != NC_NOERR) {
      return 1;
    }
    int tdim = -1;
    if (nc_def_dim(nc.get(), "t", 2, &tdim) != NC_NOERR) return 1;

    auto add_var = [&](const char* name) -> int {
      int vid = -1;
      if (nc_def_var(nc.get(), name, NC_DOUBLE, 1, &tdim, &vid) != NC_NOERR) {
        return -1;
      }
      return vid;
    };
    auto put_text = [&](int vid, const char* att_name,
                        std::string_view val) -> bool {
      return nc_put_att_text(nc.get(), vid, att_name, val.size(), val.data()) ==
             NC_NOERR;
    };

    const int v_no_units = add_var("v_no_units");
    const int v_int_units = add_var("v_int_units");
    const int v_empty_units = add_var("v_empty_units");
    const int v_bad_units = add_var("v_bad_units");
    const int v_str_atts = add_var("v_str_atts");
    const int v_int_cal = add_var("v_int_cal");
    const int v_empty_cal = add_var("v_empty_cal");
    const int v_unknown_cal = add_var("v_unknown_cal");
    const int v_clim_years = add_var("v_clim_years");
    if (v_no_units < 0 || v_int_units < 0 || v_empty_units < 0 ||
        v_bad_units < 0 || v_str_atts < 0 || v_int_cal < 0 || v_empty_cal < 0 ||
        v_unknown_cal < 0 || v_clim_years < 0) {
      return 1;
    }

    const int int_val = 7;
    if (nc_put_att_int(nc.get(), v_int_units, "units", NC_INT, 1, &int_val) !=
            NC_NOERR ||
        !put_text(v_empty_units, "units", "") ||
        !put_text(v_bad_units, "units", "meters")) {
      return 1;
    }

    const char* str_units = "hours since 2020-05-01T06:00:00";
    const char* str_cal = "Proleptic_Gregorian";
    if (nc_put_att_string(nc.get(), v_str_atts, "units", 1, &str_units) !=
            NC_NOERR ||
        nc_put_att_string(nc.get(), v_str_atts, "calendar", 1, &str_cal) !=
            NC_NOERR) {
      return 1;
    }

    if (!put_text(v_int_cal, "units", "days since 2000-01-01") ||
        nc_put_att_int(nc.get(), v_int_cal, "calendar", NC_INT, 1, &int_val) !=
            NC_NOERR ||
        !put_text(v_empty_cal, "units", "days since 2000-01-01") ||
        !put_text(v_empty_cal, "calendar", "") ||
        !put_text(v_unknown_cal, "units", "days since 2000-01-01") ||
        !put_text(v_unknown_cal, "calendar", "mars_sol") ||
        !put_text(v_clim_years, "units", "years since 2000-01-01") ||
        !put_text(v_clim_years, "calendar", "none")) {
      return 1;
    }

    struct CalCase {
      const char* var_name;
      const char* calendar_att;
      const char* units_att;
      cdCalenType expected;
      int vid;
    };
    std::array<CalCase, 12> cal_cases = {{
        {"c0", "gregorian", "days since 2000-01-01", cdMixed, -1},
        {"c1", "standard", "days since 2000-01-01", cdMixed, -1},
        {"c2", "proleptic_gregorian", "days since 2000-01-01", cdStandard, -1},
        {"c3", "noleap", "days since 2000-01-01", cdNoLeap, -1},
        {"c4", "no_leap", "days since 2000-01-01", cdNoLeap, -1},
        {"c5", "365_day", "days since 2000-01-01", cdNoLeap, -1},
        {"c6", "allleap", "days since 2000-01-01", cd366, -1},
        {"c7", "all_leap", "days since 2000-01-01", cd366, -1},
        {"c8", "366_day", "days since 2000-01-01", cd366, -1},
        {"c9", "360_day", "days since 2000-01-01", cd360, -1},
        {"c10", "julian", "days since 2000-01-01", cdJulian, -1},
        {"c11", "none", "days since 1-1", cdClim, -1},
    }};
    for (CalCase& tc : cal_cases) {
      tc.vid = add_var(tc.var_name);
      if (tc.vid < 0 || !put_text(tc.vid, "units", tc.units_att) ||
          !put_text(tc.vid, "calendar", tc.calendar_att)) {
        return 1;
      }
    }

    if (nc_enddef(nc.get()) != NC_NOERR) return 1;
    init_types(nc.get());

    auto query = [&](int vid, const char* name, bool expect_valid,
                     cdCalenType expected_cal = cdMixed) -> bool {
      ncvar_t vp{};
      std::strncpy(vp.name, name, NC_MAX_NAME - 1);
      absl::Cleanup free_ti([&vp]() { FreeTimeinfo(&vp); });
      get_timeinfo(nc.get(), vid, &vp);
      if (vp.is_bnds_var != 0) return false;
      if (expect_valid) {
        if (vp.has_timeval != 1 || vp.timeinfo == nullptr ||
            vp.timeinfo->calendar != expected_cal ||
            vp.timeinfo->units == nullptr) {
          std::fprintf(
              stderr,
              "Expected valid timeinfo for %s (got valid=%d cal=%d "
              "exp=%d)\n",
              name, vp.has_timeval,
              vp.timeinfo ? static_cast<int>(vp.timeinfo->calendar) : -1,
              static_cast<int>(expected_cal));
          return false;
        }
      } else {
        if (vp.has_timeval != 0 || vp.timeinfo != nullptr) {
          std::fprintf(stderr, "Expected invalid timeinfo for %s\n", name);
          return false;
        }
      }
      return true;
    };

    if (!query(v_no_units, "v_no_units", false)) return 1;
    if (!query(v_int_units, "v_int_units", false)) return 1;
    if (!query(v_empty_units, "v_empty_units", false)) return 1;
    if (!query(v_bad_units, "v_bad_units", false)) return 1;
    if (!query(v_clim_years, "v_clim_years", false)) return 1;
    if (!query(v_str_atts, "v_str_atts", true, cdStandard)) return 1;
    if (!query(v_int_cal, "v_int_cal", true, cdMixed)) return 1;
    if (!query(v_empty_cal, "v_empty_cal", true, cdMixed)) return 1;
    if (!query(v_unknown_cal, "v_unknown_cal", true, cdMixed)) return 1;

    for (const CalCase& tc : cal_cases) {
      if (!query(tc.vid, tc.var_name, true, tc.expected)) return 1;
    }
    return 0;
  });
  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;
}

TEST(Nctime0Test, InsertBoundsInfoAssociatesBoundsVariableWithParentTimeVar) {
  const CliResult res = RunChild([]() -> int {
    ScopedNcFile nc;
    if (nc_create_mem("bounds.nc", NC_NETCDF4, 8192, nc.ptr()) != NC_NOERR) {
      return 1;
    }
    int tdim = -1;
    int bdim = -1;
    if (nc_def_dim(nc.get(), "t", 2, &tdim) != NC_NOERR ||
        nc_def_dim(nc.get(), "b", 2, &bdim) != NC_NOERR) {
      return 1;
    }
    int v_time = -1;
    int v_bnds = -1;
    int v_other = -1;
    const int bdims[2] = {tdim, bdim};
    if (nc_def_var(nc.get(), "time", NC_DOUBLE, 1, &tdim, &v_time) !=
            NC_NOERR ||
        nc_def_var(nc.get(), "time_bnds", NC_DOUBLE, 2, bdims, &v_bnds) !=
            NC_NOERR ||
        nc_def_var(nc.get(), "other", NC_DOUBLE, 1, &tdim, &v_other) !=
            NC_NOERR) {
      return 1;
    }

    constexpr std::string_view kUnits = "days since 2000-01-01";
    constexpr std::string_view kCal = "360_day";
    constexpr std::string_view kBnds = "time_bnds";
    if (nc_put_att_text(nc.get(), v_time, "units", kUnits.size(),
                        kUnits.data()) != NC_NOERR ||
        nc_put_att_text(nc.get(), v_time, "calendar", kCal.size(),
                        kCal.data()) != NC_NOERR ||
        nc_put_att_text(nc.get(), v_time, "bounds", kBnds.size(),
                        kBnds.data()) != NC_NOERR) {
      return 1;
    }
    if (nc_enddef(nc.get()) != NC_NOERR) return 1;
    init_types(nc.get());

    // Before insert_bounds_info, bounds_list is empty.
    ncvar_t vp_before{};
    std::strncpy(vp_before.name, "time_bnds", NC_MAX_NAME - 1);
    get_timeinfo(nc.get(), v_bnds, &vp_before);
    if (vp_before.is_bnds_var != 0 || vp_before.has_timeval != 0 ||
        vp_before.timeinfo != nullptr) {
      FreeTimeinfo(&vp_before);
      return 1;
    }

    char bname[] = "time_bnds";
    ncatt_t batt{};
    batt.type = NC_CHAR;
    batt.len = std::strlen(bname);
    batt.valgp = bname;
    std::strncpy(batt.name, "bounds", NC_MAX_NAME - 1);
    insert_bounds_info(nc.get(), v_time, &batt);

    // Non-matching variable walks the bounds list and stays unaffected.
    ncvar_t vp_other{};
    std::strncpy(vp_other.name, "other", NC_MAX_NAME - 1);
    get_timeinfo(nc.get(), v_other, &vp_other);
    if (vp_other.is_bnds_var != 0 || vp_other.has_timeval != 0 ||
        vp_other.timeinfo != nullptr) {
      FreeTimeinfo(&vp_other);
      return 1;
    }

    // Bounds variable inherits units and calendar from parent variable.
    ncvar_t vp_bnds{};
    std::strncpy(vp_bnds.name, "time_bnds", NC_MAX_NAME - 1);
    absl::Cleanup free_bnds([&vp_bnds]() { FreeTimeinfo(&vp_bnds); });
    get_timeinfo(nc.get(), v_bnds, &vp_bnds);
    if (vp_bnds.is_bnds_var != 1 || vp_bnds.has_timeval != 1 ||
        vp_bnds.timeinfo == nullptr || vp_bnds.timeinfo->calendar != cd360 ||
        vp_bnds.timeinfo->units == nullptr ||
        std::string_view(vp_bnds.timeinfo->units) != kUnits) {
      return 1;
    }
    return 0;
  });
  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;
}

TEST(Nctime0Test, PrintAttTimesSkipsDisqualifiedAndFormatsValidAttributes) {
  // Disqualified inputs produce no output.
  const CliResult skip_res = RunChild([]() -> int {
    ScopedNcFile nc;
    if (nc_create_mem("att_skip.nc", NC_NETCDF4, 8192, nc.ptr()) != NC_NOERR) {
      return 1;
    }
    int tdim = -1;
    int v_time = -1;
    int v_notime = -1;
    int v_badparse = -1;
    if (nc_def_dim(nc.get(), "t", 1, &tdim) != NC_NOERR ||
        nc_def_var(nc.get(), "time", NC_DOUBLE, 1, &tdim, &v_time) !=
            NC_NOERR ||
        nc_def_var(nc.get(), "notime", NC_DOUBLE, 1, &tdim, &v_notime) !=
            NC_NOERR ||
        nc_def_var(nc.get(), "badparse", NC_DOUBLE, 1, &tdim, &v_badparse) !=
            NC_NOERR) {
      return 1;
    }
    constexpr std::string_view kUnits = "days since 2000-01-01";
    constexpr std::string_view kBadYears = "years since 2000-01-01";
    constexpr std::string_view kNoneCal = "none";
    if (nc_put_att_text(nc.get(), v_time, "units", kUnits.size(),
                        kUnits.data()) != NC_NOERR ||
        nc_put_att_text(nc.get(), v_badparse, "units", kBadYears.size(),
                        kBadYears.data()) != NC_NOERR ||
        nc_put_att_text(nc.get(), v_badparse, "calendar", kNoneCal.size(),
                        kNoneCal.data()) != NC_NOERR) {
      return 1;
    }
    if (nc_enddef(nc.get()) != NC_NOERR) return 1;
    init_types(nc.get());
    set_max_len(80);
    indent_init();

    char text_val[] = "2000";
    char* str_val = text_val;
    double d_val = 1.0;

    ncatt_t a_char{};
    a_char.type = NC_CHAR;
    a_char.tinfo = get_typeinfo(NC_CHAR);
    a_char.len = 4;
    a_char.valgp = text_val;
    print_att_times(nc.get(), v_time, &a_char);

    ncatt_t a_str{};
    a_str.type = NC_STRING;
    a_str.tinfo = get_typeinfo(NC_STRING);
    a_str.len = 1;
    a_str.valgp = &str_val;
    print_att_times(nc.get(), v_time, &a_str);

    ncatt_t a_num{};
    a_num.type = NC_DOUBLE;
    a_num.tinfo = get_typeinfo(NC_DOUBLE);
    a_num.len = 1;
    a_num.valgp = &d_val;
    print_att_times(nc.get(), NC_GLOBAL, &a_num);
    print_att_times(nc.get(), v_notime, &a_num);
    print_att_times(nc.get(), v_badparse, &a_num);
    std::fflush(stdout);
    return 0;
  });
  ASSERT_TRUE(skip_res.ok()) << skip_res.harness_error;
  EXPECT_EQ(0, skip_res.exit_code) << skip_res.stderr_output;
  EXPECT_EQ("", skip_res.stdout_output);

  // Single and 2-element attributes keep output on the same line (wrap =
  // false).
  const CliResult pair_res = RunChild([]() -> int {
    ScopedNcFile nc;
    if (nc_create_mem("att_pair.nc", NC_NETCDF4, 8192, nc.ptr()) != NC_NOERR) {
      return 1;
    }
    int tdim = -1;
    int v_time = -1;
    if (nc_def_dim(nc.get(), "t", 1, &tdim) != NC_NOERR ||
        nc_def_var(nc.get(), "time", NC_DOUBLE, 1, &tdim, &v_time) !=
            NC_NOERR) {
      return 1;
    }
    constexpr std::string_view kUnits = "days since 2000-01-01 00:00:00";
    if (nc_put_att_text(nc.get(), v_time, "units", kUnits.size(),
                        kUnits.data()) != NC_NOERR) {
      return 1;
    }
    if (nc_enddef(nc.get()) != NC_NOERR) return 1;
    init_types(nc.get());
    set_max_len(80);
    indent_init();

    double one = 0.0;
    ncatt_t a1{};
    a1.type = NC_DOUBLE;
    a1.tinfo = get_typeinfo(NC_DOUBLE);
    a1.len = 1;
    a1.valgp = &one;
    print_att_times(nc.get(), v_time, &a1);

    std::fputs("|", stdout);

    std::array<double, 2> two = {1.0, 2.5};
    ncatt_t a2{};
    a2.type = NC_DOUBLE;
    a2.tinfo = get_typeinfo(NC_DOUBLE);
    a2.len = two.size();
    a2.valgp = two.data();
    print_att_times(nc.get(), v_time, &a2);
    std::fflush(stdout);
    return 0;
  });
  ASSERT_TRUE(pair_res.ok()) << pair_res.harness_error;
  EXPECT_EQ(0, pair_res.exit_code) << pair_res.stderr_output;
  EXPECT_EQ(" // \"2000-01-01\"| // \"2000-01-02\", \"2000-01-03 12\"",
            pair_res.stdout_output);

  // Multi-element attribute (> 2) enables wrapping across lines.
  const CliResult wrap_res = RunChild([]() -> int {
    ScopedNcFile nc;
    if (nc_create_mem("att_wrap.nc", NC_NETCDF4, 8192, nc.ptr()) != NC_NOERR) {
      return 1;
    }
    int tdim = -1;
    int v_time = -1;
    if (nc_def_dim(nc.get(), "t", 1, &tdim) != NC_NOERR ||
        nc_def_var(nc.get(), "time", NC_DOUBLE, 1, &tdim, &v_time) !=
            NC_NOERR) {
      return 1;
    }
    constexpr std::string_view kUnits = "days since 2000-01-01 00:00:00";
    if (nc_put_att_text(nc.get(), v_time, "units", kUnits.size(),
                        kUnits.data()) != NC_NOERR) {
      return 1;
    }
    if (nc_enddef(nc.get()) != NC_NOERR) return 1;
    init_types(nc.get());
    set_max_len(55);
    indent_init();

    std::array<double, 4> four = {0.0, 1.0, 2.0, 3.0};
    ncatt_t a4{};
    a4.type = NC_DOUBLE;
    a4.tinfo = get_typeinfo(NC_DOUBLE);
    a4.len = four.size();
    a4.valgp = four.data();
    print_att_times(nc.get(), v_time, &a4);
    std::fflush(stdout);
    return 0;
  });
  ASSERT_TRUE(wrap_res.ok()) << wrap_res.harness_error;
  EXPECT_EQ(0, wrap_res.exit_code) << wrap_res.stderr_output;
  EXPECT_EQ(
      "\n\t\t  // \"2000-01-01\", \"2000-01-02\",\n\t\t  // \"2000-01-03\", "
      "\"2000-01-04\"",
      wrap_res.stdout_output);

  // Exercise all 10 numeric attribute types, iso_separator, and NaN/Inf.
  const CliResult types_res = RunChild([]() -> int {
    ScopedNcFile nc;
    if (nc_create_mem("att_types.nc", NC_NETCDF4, 8192, nc.ptr()) != NC_NOERR) {
      return 1;
    }
    int tdim = -1;
    int v_time = -1;
    if (nc_def_dim(nc.get(), "t", 1, &tdim) != NC_NOERR ||
        nc_def_var(nc.get(), "time", NC_DOUBLE, 1, &tdim, &v_time) !=
            NC_NOERR) {
      return 1;
    }
    constexpr std::string_view kUnits = "days since 2000-01-01 00:00:00";
    if (nc_put_att_text(nc.get(), v_time, "units", kUnits.size(),
                        kUnits.data()) != NC_NOERR) {
      return 1;
    }
    if (nc_enddef(nc.get()) != NC_NOERR) return 1;
    init_types(nc.get());
    set_max_len(80);
    indent_init();
    formatting_specs.iso_separator = 1;

    auto print_one = [&](nc_type tid, void* valp, size_t len = 1) {
      ncatt_t att{};
      att.type = tid;
      att.tinfo = get_typeinfo(tid);
      att.len = len;
      att.valgp = valp;
      print_att_times(nc.get(), v_time, &att);
    };

    signed char b = 1;
    short s = 2;
    int i = 3;
    float f = 0.25F;
    double d = 1.5;
    unsigned char ub = 4;
    unsigned short us = 5;
    unsigned int ui = 6;
    long long i64 = 7;
    unsigned long long ui64 = 8;
    std::array<double, 2> special = {std::numeric_limits<double>::quiet_NaN(),
                                     std::numeric_limits<double>::infinity()};

    print_one(NC_BYTE, &b);
    print_one(NC_SHORT, &s);
    print_one(NC_INT, &i);
    print_one(NC_FLOAT, &f);
    print_one(NC_DOUBLE, &d);
    print_one(NC_UBYTE, &ub);
    print_one(NC_USHORT, &us);
    print_one(NC_UINT, &ui);
    print_one(NC_INT64, &i64);
    print_one(NC_UINT64, &ui64);
    print_one(NC_DOUBLE, special.data(), special.size());
    std::fflush(stdout);
    return 0;
  });
  ASSERT_TRUE(types_res.ok()) << types_res.harness_error;
  EXPECT_EQ(0, types_res.exit_code) << types_res.stderr_output;
  EXPECT_THAT(types_res.stdout_output,
              ::testing::HasSubstr("\"2000-01-01T06\""));
  EXPECT_THAT(types_res.stdout_output,
              ::testing::HasSubstr("\"2000-01-02T12\""));
  EXPECT_THAT(types_res.stdout_output, ::testing::HasSubstr("NaN, Infinity"));
}

}  // namespace
}  // namespace netcdf
