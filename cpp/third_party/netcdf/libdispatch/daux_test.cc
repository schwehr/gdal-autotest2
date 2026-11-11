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

// Unit tests for //third_party/netcdf/libdispatch/daux.c (auxiliary helpers).
//
// Covers compound-builder lifecycle entry points and validation, type/class
// alignment queries, HDF5 filter spec parsing and 8-byte endian fixups,
// temporary-file read/write wrappers, ncaux_inq_any_type, ncaux_dump_data,
// ncaux_reclaim_data[_all] (with strict reclamation before closing ncid),
// NCPluginList path utilities, and _NCProperties provenance parsing.
//
// Note: Upstream daux.c contains bugs in ncaux_add_field/computefieldinfo
// (never assigning cmpd->fields before NULL check and dereferencing fields[0]
// in ncaux_end_compound) and ncaux_h5filterspec_parselist (shifting the heap
// pointer `spec` past valid "[N]" prefixes before `free(spec)`). Tests verify
// the safe error paths without invoking upstream UB/crashes.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/daux.c

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
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"

namespace netcdf {
namespace {

using ::testing::HasSubstr;

struct FreeDeleter {
  void operator()(void* ptr) const { std::free(ptr); }
};

using ScopedUintArray = std::unique_ptr<unsigned int[], FreeDeleter>;

struct AuxDumpRecord {
  int32_t code;
  int16_t pair[2];
  char* label;
};

struct AuxNestedCompound {
  double weight;
  uint8_t flags;
};

struct AuxVlenFirstCompound {
  nc_vlen_t items;
};

double DecodeFilterDouble(unsigned int w0, unsigned int w1) {
  std::array<unsigned int, 2> words = {w0, w1};
  std::array<unsigned char, 8> raw{};
  std::memcpy(raw.data(), words.data(), raw.size());
  ncaux_h5filterspec_fix8(raw.data(), /*decode=*/1);
  double val = 0.0;
  std::memcpy(&val, raw.data(), sizeof(val));
  return val;
}

long long DecodeFilterInt64(unsigned int w0, unsigned int w1) {
  std::array<unsigned int, 2> words = {w0, w1};
  std::array<unsigned char, 8> raw{};
  std::memcpy(raw.data(), words.data(), raw.size());
  ncaux_h5filterspec_fix8(raw.data(), /*decode=*/1);
  long long val = 0;
  std::memcpy(&val, raw.data(), sizeof(val));
  return val;
}

unsigned long long DecodeFilterUint64(unsigned int w0, unsigned int w1) {
  std::array<unsigned int, 2> words = {w0, w1};
  std::array<unsigned char, 8> raw{};
  std::memcpy(raw.data(), words.data(), raw.size());
  ncaux_h5filterspec_fix8(raw.data(), /*decode=*/1);
  unsigned long long val = 0;
  std::memcpy(&val, raw.data(), sizeof(val));
  return val;
}

TEST(DauxTest, CompoundBuilderValidationAndAbortLifecycle) {
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("daux_cmpd_builder.nc", NC_NETCDF4, 0, nc.ptr()));

  void* tag = nullptr;
  absl::Cleanup tag_guard([&]() {
    if (tag != nullptr) {
      ncaux_abort_compound(tag);
      tag = nullptr;
    }
  });

  ASSERT_EQ(NC_NOERR, ncaux_begin_compound(nc, "aux_rec", NCAUX_ALIGN_C, &tag));
  ASSERT_NE(nullptr, tag);

  // Null tag on add_field / abort_compound returns NC_NOERR; null tag on
  // end_compound returns NC_EINVAL.
  EXPECT_EQ(NC_NOERR, ncaux_add_field(nullptr, "ignored", NC_INT, 0, nullptr));
  EXPECT_EQ(NC_NOERR, ncaux_abort_compound(nullptr));
  nc_type dummy_tid = NC_NAT;
  EXPECT_EQ(NC_EINVAL, ncaux_end_compound(nullptr, &dummy_tid));

  // Invalid dimension counts and zero/negative dimension extents fail with
  // NC_EINVAL before allocation.
  EXPECT_EQ(NC_EINVAL, ncaux_add_field(tag, "neg_ndims", NC_INT, -1, nullptr));
  constexpr std::array<int, 2> kZeroDim = {2, 0};
  EXPECT_EQ(NC_EINVAL,
            ncaux_add_field(tag, "zero_dim", NC_INT, 2, kZeroDim.data()));
  constexpr std::array<int, 2> kNegDim = {3, -1};
  EXPECT_EQ(NC_EINVAL,
            ncaux_add_field(tag, "neg_dim", NC_INT, 2, kNegDim.data()));

  // Upstream daux.c checks `cmpd->fields == NULL` before updating it, causing
  // valid field additions to return NC_ENOMEM cleanly without leaking.
  EXPECT_EQ(NC_ENOMEM, ncaux_add_field(tag, "x", NC_INT, 0, nullptr));

  EXPECT_EQ(NC_NOERR, ncaux_abort_compound(tag));
  tag = nullptr;

  // Verify NCAUX_ALIGN_UNIFORM begin + immediate abort also frees cleanly.
  ASSERT_EQ(NC_NOERR,
            ncaux_begin_compound(nc, "aux_uniform", NCAUX_ALIGN_UNIFORM, &tag));
  ASSERT_NE(nullptr, tag);
  EXPECT_EQ(NC_NOERR, ncaux_abort_compound(tag));
  tag = nullptr;
}

TEST(DauxTest, ClassAlignmentCoversSupportedAndRejectedClasses) {
  struct ClassProbe {
    int ncclass;
    size_t expected_align;
  };
  constexpr ClassProbe kSupported[] = {
      {NC_BYTE, alignof(signed char)},
      {NC_CHAR, alignof(char)},
      {NC_SHORT, alignof(short)},
      {NC_INT, alignof(int)},
      {NC_FLOAT, alignof(float)},
      {NC_DOUBLE, alignof(double)},
      {NC_UBYTE, alignof(unsigned char)},
      {NC_USHORT, alignof(unsigned short)},
      {NC_UINT, alignof(unsigned int)},
      {NC_INT64, alignof(long long)},
      {NC_UINT64, alignof(unsigned long long)},
      {NC_STRING, alignof(char*)},
      {NC_VLEN, alignof(nc_vlen_t)},
      {NC_OPAQUE, 1U},
  };

  for (const ClassProbe& probe : kSupported) {
    size_t align = 0;
    EXPECT_EQ(NC_NOERR, ncaux_class_alignment(probe.ncclass, &align));
    EXPECT_EQ(probe.expected_align, align);
  }

  // Null output pointer is ignored safely.
  EXPECT_EQ(NC_NOERR, ncaux_class_alignment(NC_INT, nullptr));

  // Unalignable or invalid classes fail with NC_EINVAL and write 0 when
  // non-null.
  for (int bad_class : {NC_NAT, -1, NC_ENUM, NC_COMPOUND, 999}) {
    size_t align = 123;
    EXPECT_EQ(NC_EINVAL, ncaux_class_alignment(bad_class, &align));
    EXPECT_EQ(0U, align);
  }
}

TEST(DauxTest, FilterSpecFix8AndParseParameterAllTypeTags) {
  std::array<unsigned char, 8> orig = {0x01, 0x23, 0x45, 0x67,
                                       0x89, 0xab, 0xcd, 0xef};
  std::array<unsigned char, 8> work = orig;
  ncaux_h5filterspec_fix8(work.data(), /*decode=*/0);
  ncaux_h5filterspec_fix8(work.data(), /*decode=*/1);
  EXPECT_EQ(orig, work);

  // Error / empty inputs on ncaux_h5filterspec_parse_parameter.
  // Note: upstream `while(strchr(" \t", *p) != NULL)` matches NUL on
  // whitespace-only strings (`strchr(..., '\0') != NULL`), so exercise
  // leading whitespace followed by an invalid token (" \t bad").
  size_t nuiparams = 0;
  std::array<unsigned int, 2> out{};
  EXPECT_EQ(NC_EINVAL, ncaux_h5filterspec_parse_parameter(nullptr, &nuiparams,
                                                          out.data()));
  EXPECT_EQ(NC_EINVAL,
            ncaux_h5filterspec_parse_parameter("", &nuiparams, out.data()));
  for (const char* invalid :
       {" \t bad", "ab", "-ab", "af", "ad", "al", "aul", "9x", "badtag", "u"}) {
    SCOPED_TRACE(invalid);
    EXPECT_EQ(NC_EINVAL, ncaux_h5filterspec_parse_parameter(invalid, &nuiparams,
                                                            out.data()));
  }

  // Optional null nuiparamsp succeeds.
  out.fill(0);
  EXPECT_EQ(NC_NOERR,
            ncaux_h5filterspec_parse_parameter("7", nullptr, out.data()));
  EXPECT_EQ(7U, out[0]);

  // Len 1 and 2 integer, unsigned, byte, short tags.
  struct IntParamCase {
    const char* text;
    unsigned int expected;
  };
  constexpr IntParamCase kIntCases[] = {
      {"5", 5U},
      {" \t 42", 42U},
      {"-7", static_cast<unsigned int>(-7)},
      {"-12345", static_cast<unsigned int>(-12345)},
      {"7u", 7U},
      {"7U", 7U},
      {"123u", 123U},
      {"123U", 123U},
      {"9b", 9U},
      {"9B", 9U},
      {"200ub", 200U},
      {"200UB", 200U},
      {"-5b", 251U},
      {"300b", 44U},
      {"9s", 9U},
      {"9S", 9U},
      {"40000us", 40000U},
      {"40000US", 40000U},
      {"-5s", 65531U},
      {"70000s", 4464U},
  };
  for (const IntParamCase& c : kIntCases) {
    SCOPED_TRACE(c.text);
    out.fill(0);
    nuiparams = 0;
    ASSERT_EQ(NC_NOERR, ncaux_h5filterspec_parse_parameter(c.text, &nuiparams,
                                                           out.data()));
    EXPECT_EQ(1U, nuiparams);
    EXPECT_EQ(c.expected, out[0]);
  }

  // Float tags ('f', 'F', '.') across len == 2 and len > 2.
  struct FloatParamCase {
    const char* text;
    float expected;
  };
  constexpr FloatParamCase kFloatCases[] = {
      {"9.", 9.0F},   {"9f", 9.0F},   {"9F", 9.0F},
      {"10.", 10.0F}, {"1.5f", 1.5F}, {"-2.5F", -2.5F},
  };
  for (const FloatParamCase& c : kFloatCases) {
    SCOPED_TRACE(c.text);
    out.fill(0);
    nuiparams = 0;
    ASSERT_EQ(NC_NOERR, ncaux_h5filterspec_parse_parameter(c.text, &nuiparams,
                                                           out.data()));
    EXPECT_EQ(1U, nuiparams);
    float decoded = 0.0F;
    std::memcpy(&decoded, &out[0], sizeof(decoded));
    EXPECT_FLOAT_EQ(c.expected, decoded);
  }

  // Double tags ('d', 'D') across len == 2 and len > 2.
  struct DoubleParamCase {
    const char* text;
    double expected;
  };
  constexpr DoubleParamCase kDoubleCases[] = {
      {"9d", 9.0},
      {"9D", 9.0},
      {"3.25d", 3.25},
      {"-0.125D", -0.125},
  };
  for (const DoubleParamCase& c : kDoubleCases) {
    SCOPED_TRACE(c.text);
    out.fill(0);
    nuiparams = 0;
    ASSERT_EQ(NC_NOERR, ncaux_h5filterspec_parse_parameter(c.text, &nuiparams,
                                                           out.data()));
    EXPECT_EQ(2U, nuiparams);
    EXPECT_DOUBLE_EQ(c.expected, DecodeFilterDouble(out[0], out[1]));
  }

  // Signed 64-bit integer tags ('l', 'L').
  struct Int64ParamCase {
    const char* text;
    long long expected;
  };
  constexpr Int64ParamCase kInt64Cases[] = {
      {"9l", 9LL},
      {"9L", 9LL},
      {"-5000000000000l", -5000000000000LL},
      {"5000000000000L", 5000000000000LL},
  };
  for (const Int64ParamCase& c : kInt64Cases) {
    SCOPED_TRACE(c.text);
    out.fill(0);
    nuiparams = 0;
    ASSERT_EQ(NC_NOERR, ncaux_h5filterspec_parse_parameter(c.text, &nuiparams,
                                                           out.data()));
    EXPECT_EQ(2U, nuiparams);
    EXPECT_EQ(c.expected, DecodeFilterInt64(out[0], out[1]));
  }

  // Unsigned 64-bit integer tags ('ul', 'UL').
  for (const char* text : {"5000000000000ul", "5000000000000UL"}) {
    SCOPED_TRACE(text);
    out.fill(0);
    nuiparams = 0;
    ASSERT_EQ(NC_NOERR,
              ncaux_h5filterspec_parse_parameter(text, &nuiparams, out.data()));
    EXPECT_EQ(2U, nuiparams);
    EXPECT_EQ(5000000000000ULL, DecodeFilterUint64(out[0], out[1]));
  }
}

TEST(DauxTest, FilterSpecParseAndParseList) {
  unsigned int filter_id = 0;
  size_t nparams = 0;
  unsigned int* raw_params = nullptr;
  ScopedUintArray params_guard;

  EXPECT_EQ(NC_EINVAL, ncaux_h5filterspec_parse(nullptr, &filter_id, &nparams,
                                                &raw_params));
  EXPECT_EQ(NC_EINVAL,
            ncaux_h5filterspec_parse("", &filter_id, &nparams, &raw_params));
  EXPECT_EQ(NC_EINVAL, ncaux_h5filterspec_parse("badid,1", &filter_id, &nparams,
                                                &raw_params));
  EXPECT_EQ(NC_EINVAL, ncaux_h5filterspec_parse("307,1,badparam", &filter_id,
                                                &nparams, &raw_params));
  // Avoid trailing empty segment ("307,") because upstream `strchr(" \t", *p)`
  // treats NUL as whitespace; exercise leading-whitespace error propagation.
  EXPECT_EQ(NC_EINVAL, ncaux_h5filterspec_parse("307,   badparam", &filter_id,
                                                &nparams, &raw_params));

  // Filter id with zero parameters succeeds.
  ASSERT_EQ(NC_NOERR,
            ncaux_h5filterspec_parse("307", &filter_id, &nparams, &raw_params));
  params_guard.reset(raw_params);
  raw_params = nullptr;
  EXPECT_EQ(307U, filter_id);
  EXPECT_EQ(0U, nparams);
  params_guard.reset();

  // Mixed 32-bit and 64-bit params with spaces and tabs.
  ASSERT_EQ(NC_NOERR,
            ncaux_h5filterspec_parse("32001, \t 9, 2.5d, 100ul", &filter_id,
                                     &nparams, &raw_params));
  params_guard.reset(raw_params);
  raw_params = nullptr;
  EXPECT_EQ(32001U, filter_id);
  ASSERT_EQ(5U, nparams);
  EXPECT_EQ(9U, params_guard[0]);
  EXPECT_DOUBLE_EQ(2.5, DecodeFilterDouble(params_guard[1], params_guard[2]));
  EXPECT_EQ(100ULL, DecodeFilterUint64(params_guard[3], params_guard[4]));
  params_guard.reset();

  // Null optional outputs clean up internally without leaking.
  EXPECT_EQ(NC_NOERR,
            ncaux_h5filterspec_parse("307,1,2", nullptr, nullptr, nullptr));

  // ncaux_h5filterspec_parselist error & edge cases.
  int format = -1;
  size_t nspecs = 99;
  NC_H5_Filterspec** specs = nullptr;
  EXPECT_EQ(NC_EINVAL,
            ncaux_h5filterspec_parselist(nullptr, &format, &nspecs, &specs));
  EXPECT_EQ(NC_EINVAL,
            ncaux_h5filterspec_parselist("[123", &format, &nspecs, &specs));
  EXPECT_EQ(NC_EINVAL,
            ncaux_h5filterspec_parselist("[bad]307", &format, &nspecs, &specs));
  EXPECT_EQ(NC_EINVAL, ncaux_h5filterspec_parselist("307,5|badspec", &format,
                                                    &nspecs, &specs));

  // Empty spec list yields 0 specs and null vector.
  ASSERT_EQ(NC_NOERR,
            ncaux_h5filterspec_parselist("", &format, &nspecs, &specs));
  EXPECT_EQ(0, format);
  EXPECT_EQ(0U, nspecs);
  EXPECT_EQ(nullptr, specs);

  // Valid pipe-separated list with multiple filters.
  ASSERT_EQ(NC_NOERR, ncaux_h5filterspec_parselist("307,5|32001,1,2|32015",
                                                   &format, &nspecs, &specs));
  absl::Cleanup specs_guard([&]() {
    if (specs != nullptr) {
      for (size_t i = 0; i < nspecs; ++i) {
        ncaux_h5filterspec_free(specs[i]);
      }
      std::free(specs);
      specs = nullptr;
    }
  });
  EXPECT_EQ(0, format);
  ASSERT_EQ(3U, nspecs);
  ASSERT_NE(nullptr, specs);
  EXPECT_EQ(307U, specs[0]->filterid);
  ASSERT_EQ(1U, specs[0]->nparams);
  EXPECT_EQ(5U, specs[0]->params[0]);
  EXPECT_EQ(32001U, specs[1]->filterid);
  ASSERT_EQ(2U, specs[1]->nparams);
  EXPECT_EQ(1U, specs[1]->params[0]);
  EXPECT_EQ(2U, specs[1]->params[1]);
  EXPECT_EQ(32015U, specs[2]->filterid);
  EXPECT_EQ(0U, specs[2]->nparams);

  // Null optional out-parameters on parselist and null free clean up cleanly.
  EXPECT_EQ(NC_NOERR, ncaux_h5filterspec_parselist("307,9|32015", nullptr,
                                                   nullptr, nullptr));
  ncaux_h5filterspec_free(nullptr);
}

TEST(DauxTest, ReadAndWriteFileRoundTripAndErrors) {
  TempFile data_file("daux_payload.txt");
  std::string payload = "line1=alpha\nline2=beta\nline3=gamma\n";
  ASSERT_EQ(NC_NOERR,
            ncaux_writefile(data_file.c_str(), payload.size(), payload.data()));

  size_t read_size = 0;
  void* raw_read = nullptr;
  ASSERT_EQ(NC_NOERR, ncaux_readfile(data_file.c_str(), &read_size, &raw_read));
  auto read_guard = MakeScopedCString(static_cast<char*>(raw_read));
  raw_read = nullptr;
  ASSERT_EQ(payload.size(), read_size);
  ASSERT_NE(nullptr, read_guard.get());
  EXPECT_EQ(payload, std::string(read_guard.get(), read_size));
  read_guard.reset();

  // Optional null sizep / datap work without leaking.
  ASSERT_EQ(NC_NOERR, ncaux_readfile(data_file.c_str(), nullptr, &raw_read));
  read_guard = MakeScopedCString(static_cast<char*>(raw_read));
  raw_read = nullptr;
  EXPECT_STREQ(payload.c_str(), read_guard.get());
  read_guard.reset();

  read_size = 0;
  ASSERT_EQ(NC_NOERR, ncaux_readfile(data_file.c_str(), &read_size, nullptr));
  EXPECT_EQ(payload.size(), read_size);

  // Null content writes an empty file (0 bytes).
  TempFile empty_file("daux_empty.txt");
  ASSERT_EQ(NC_NOERR, ncaux_writefile(empty_file.c_str(), 123, nullptr));
  ASSERT_EQ(NC_NOERR,
            ncaux_readfile(empty_file.c_str(), &read_size, &raw_read));
  read_guard = MakeScopedCString(static_cast<char*>(raw_read));
  raw_read = nullptr;
  EXPECT_EQ(0U, read_size);
  read_guard.reset();

  // Missing file / missing parent directory error paths.
  TempFile missing_file("daux_does_not_exist.txt");
  void* missing_data = nullptr;
  EXPECT_NE(NC_NOERR,
            ncaux_readfile(missing_file.c_str(), &read_size, &missing_data));
  EXPECT_EQ(nullptr, missing_data);

  TempDir missing_dir("daux_missing_parent");
  std::string bad_path = (missing_dir.path() / "no_such_subdir" / "f.bin");
  EXPECT_NE(NC_NOERR,
            ncaux_writefile(bad_path.c_str(), payload.size(), payload.data()));
}

TEST(DauxTest, TypeInquiryAlignmentDumpAndDeepReclamation) {
  ScopedNcFile classic_nc;
  ASSERT_EQ(NC_NOERR, nc_create("daux_classic.nc", NC_DISKLESS | NC_CLOBBER,
                                classic_nc.ptr()));
  // Classic files short-circuit NC_type_alignment after checking USENC3INFO.
  size_t classic_align = 0;
  EXPECT_EQ(NC_NOERR, ncaux_type_alignment(NC_INT, classic_nc, &classic_align));

  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR, nc_create_mem("daux_types.nc", NC_NETCDF4, 0, nc.ptr()));

  nc_type enum_t = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_enum(nc, NC_INT, "mode_e", &enum_t));
  constexpr int kActive = 7;
  ASSERT_EQ(NC_NOERR, nc_insert_enum(nc, enum_t, "ACTIVE", &kActive));

  nc_type opaque_t = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_opaque(nc, 3, "bytes3", &opaque_t));

  nc_type vlen_t = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_vlen(nc, "int_list", NC_INT, &vlen_t));

  nc_type nested_t = NC_NAT;
  ASSERT_EQ(NC_NOERR, nc_def_compound(nc, sizeof(AuxNestedCompound), "nested_c",
                                      &nested_t));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(nc, nested_t, "weight",
                               NC_COMPOUND_OFFSET(AuxNestedCompound, weight),
                               NC_DOUBLE));
  ASSERT_EQ(NC_NOERR,
            nc_insert_compound(nc, nested_t, "flags",
                               NC_COMPOUND_OFFSET(AuxNestedCompound, flags),
                               NC_UBYTE));

  nc_type cmpd_t = NC_NAT;
  ASSERT_EQ(NC_NOERR,
            nc_def_compound(nc, sizeof(AuxDumpRecord), "rec_c", &cmpd_t));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(
                          nc, cmpd_t, "code",
                          NC_COMPOUND_OFFSET(AuxDumpRecord, code), NC_INT));
  constexpr std::array<int, 1> kPairDims = {2};
  ASSERT_EQ(NC_NOERR,
            nc_insert_array_compound(nc, cmpd_t, "pair",
                                     NC_COMPOUND_OFFSET(AuxDumpRecord, pair),
                                     NC_SHORT, 1, kPairDims.data()));
  ASSERT_EQ(NC_NOERR, nc_insert_compound(
                          nc, cmpd_t, "label",
                          NC_COMPOUND_OFFSET(AuxDumpRecord, label), NC_STRING));

  // Check ncaux_inq_any_type across atomic, user-defined, and invalid types.
  char tname[NC_MAX_NAME + 1]{};
  size_t tsize = 0;
  nc_type tbasetype = NC_NAT;
  size_t tnfields = 0;
  int tclass = -1;

  ASSERT_EQ(NC_NOERR, ncaux_inq_any_type(nc, NC_DOUBLE, tname, &tsize,
                                         &tbasetype, &tnfields, &tclass));
  EXPECT_STREQ("double", tname);
  EXPECT_EQ(sizeof(double), tsize);
  EXPECT_EQ(NC_NAT, tbasetype);
  EXPECT_EQ(0U, tnfields);
  EXPECT_EQ(NC_DOUBLE, tclass);

  ASSERT_EQ(NC_NOERR, ncaux_inq_any_type(nc, NC_STRING, nullptr, &tsize,
                                         nullptr, nullptr, nullptr));
  EXPECT_EQ(sizeof(char*), tsize);

  ASSERT_EQ(NC_NOERR, ncaux_inq_any_type(nc, cmpd_t, tname, &tsize, &tbasetype,
                                         &tnfields, &tclass));
  EXPECT_STREQ("rec_c", tname);
  EXPECT_EQ(sizeof(AuxDumpRecord), tsize);
  EXPECT_EQ(NC_NAT, tbasetype);
  EXPECT_EQ(3U, tnfields);
  EXPECT_EQ(NC_COMPOUND, tclass);

  EXPECT_EQ(NC_EBADTYPE, ncaux_inq_any_type(nc, NC_NAT, tname, &tsize,
                                            &tbasetype, &tnfields, &tclass));
  EXPECT_EQ(NC_EBADTYPE, ncaux_inq_any_type(nc, NC_COMPOUND, tname, &tsize,
                                            &tbasetype, &tnfields, &tclass));
  EXPECT_EQ(NC_EBADTYPE, ncaux_inq_any_type(nc, 99999, tname, &tsize,
                                            &tbasetype, &tnfields, &tclass));

  // Check ncaux_type_alignment across atomic, user, nested, and error cases.
  // Note: upstream NC_type_alignment_internal jumps over `*alignp = align` for
  // atomic types (`xtype <= NC_MAX_ATOMIC_TYPE`), leaving `*alignp` unchanged;
  // compounds whose first field is atomic inherit `0` from the recursive call,
  // whereas non-atomic first fields (e.g. NC_VLEN) propagate their alignment.
  size_t align = 777;
  EXPECT_EQ(NC_EBADID, ncaux_type_alignment(NC_INT, -1, &align));
  EXPECT_EQ(777U, align);
  EXPECT_EQ(NC_NOERR, ncaux_type_alignment(NC_DOUBLE, nc, &align));
  EXPECT_EQ(777U, align);
  EXPECT_EQ(NC_NOERR, ncaux_type_alignment(opaque_t, nc, &align));
  EXPECT_EQ(1U, align);
  EXPECT_EQ(NC_NOERR, ncaux_type_alignment(vlen_t, nc, &align));
  EXPECT_EQ(alignof(nc_vlen_t), align);
  EXPECT_EQ(NC_NOERR, ncaux_type_alignment(enum_t, nc, &align));
  EXPECT_EQ(0U, align);
  align = 777;
  EXPECT_EQ(NC_NOERR, ncaux_type_alignment(nested_t, nc, &align));
  EXPECT_EQ(0U, align);
  align = 777;
  EXPECT_EQ(NC_NOERR, ncaux_type_alignment(cmpd_t, nc, &align));
  EXPECT_EQ(0U, align);
  EXPECT_EQ(NC_NOERR, ncaux_type_alignment(cmpd_t, nc, nullptr));
  EXPECT_EQ(NC_EBADTYPID, ncaux_type_alignment(99999, nc, &align));

  // Keep uncommitted empty-compound and vlen-first compound checks in a
  // separate redef-aborted file so HDF5 never commits a zero-field compound.
  {
    ScopedNcFile aux_nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("daux_empty_cmpd.nc", NC_NETCDF4, 0, aux_nc.ptr()));
    ASSERT_EQ(NC_NOERR, nc_enddef(aux_nc));
    absl::Cleanup aux_abort_guard([&]() {
      if (aux_nc.valid()) {
        static_cast<void>(aux_nc.abort());
      }
    });

    nc_type empty_cmpd_t = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_compound(aux_nc, 4, "empty_c", &empty_cmpd_t));
    EXPECT_EQ(NC_EINVAL, ncaux_type_alignment(empty_cmpd_t, aux_nc, &align));

    nc_type aux_vlen_t = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_vlen(aux_nc, "aux_vlen", NC_INT, &aux_vlen_t));
    nc_type vlen_first_t = NC_NAT;
    ASSERT_EQ(NC_NOERR, nc_def_compound(aux_nc, sizeof(AuxVlenFirstCompound),
                                        "vlen_first_c", &vlen_first_t));
    ASSERT_EQ(NC_NOERR,
              nc_insert_compound(
                  aux_nc, vlen_first_t, "items",
                  NC_COMPOUND_OFFSET(AuxVlenFirstCompound, items), aux_vlen_t));
    align = 0;
    EXPECT_EQ(NC_NOERR, ncaux_type_alignment(vlen_first_t, aux_nc, &align));
    EXPECT_EQ(alignof(nc_vlen_t), align);
    EXPECT_EQ(NC_NOERR, aux_nc.abort());
  }

  // Define variables for compound/vlen persistence + reclaim round-trips.
  int dimid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "rows", 2, &dimid));
  int cmpd_varid = -1;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(nc, "records", cmpd_t, 1, &dimid, &cmpd_varid));
  int vlen_varid = -1;
  ASSERT_EQ(NC_NOERR, nc_def_var(nc, "lists", vlen_t, 1, &dimid, &vlen_varid));
  ASSERT_EQ(NC_NOERR, nc_enddef(nc));

  char lbl0[] = "ok";
  char lbl1[] = "warn";
  std::array<AuxDumpRecord, 2> rec_in{};
  rec_in[0].code = 42;
  rec_in[0].pair[0] = 10;
  rec_in[0].pair[1] = -20;
  rec_in[0].label = lbl0;
  rec_in[1].code = 99;
  rec_in[1].pair[0] = 1;
  rec_in[1].pair[1] = 2;
  rec_in[1].label = lbl1;
  ASSERT_EQ(NC_NOERR, nc_put_var(nc, cmpd_varid, rec_in.data()));

  std::array<int32_t, 3> vl_elems0 = {10, 20, 30};
  std::array<int32_t, 1> vl_elems1 = {40};
  std::array<nc_vlen_t, 2> vl_in{};
  vl_in[0].len = vl_elems0.size();
  vl_in[0].p = vl_elems0.data();
  vl_in[1].len = vl_elems1.size();
  vl_in[1].p = vl_elems1.data();
  ASSERT_EQ(NC_NOERR, nc_put_var(nc, vlen_varid, vl_in.data()));

  // Check ncaux_dump_data validation, empty count, and all type classes.
  int active = kActive;
  char* dump_buf = nullptr;
  EXPECT_EQ(NC_EINVAL, ncaux_dump_data(-1, NC_INT, &active, 1, &dump_buf));
  EXPECT_EQ(NC_EINVAL, ncaux_dump_data(nc, NC_NAT, &active, 1, &dump_buf));
  EXPECT_EQ(NC_EINVAL, ncaux_dump_data(nc, NC_INT, nullptr, 1, &dump_buf));
  EXPECT_EQ(NC_NOERR, ncaux_dump_data(nc, NC_INT, nullptr, 0, &dump_buf));
  EXPECT_EQ(nullptr, dump_buf);

  std::array<int32_t, 2> ints = {10, -20};
  ASSERT_EQ(NC_NOERR,
            ncaux_dump_data(nc, NC_INT, ints.data(), ints.size(), &dump_buf));
  EXPECT_STREQ("10 -20", MakeScopedCString(dump_buf).get());

  char* str_ptr = lbl0;
  ASSERT_EQ(NC_NOERR, ncaux_dump_data(nc, NC_STRING, &str_ptr, 1, &dump_buf));
  EXPECT_STREQ("\"ok\"", MakeScopedCString(dump_buf).get());

  int enum_val = kActive;
  ASSERT_EQ(NC_NOERR, ncaux_dump_data(nc, enum_t, &enum_val, 1, &dump_buf));
  EXPECT_STREQ("7", MakeScopedCString(dump_buf).get());

  std::array<uint8_t, 3> opq_bytes = {0x12, 0xab, 0xff};
  ASSERT_EQ(NC_NOERR,
            ncaux_dump_data(nc, opaque_t, opq_bytes.data(), 1, &dump_buf));
  EXPECT_STREQ("|12abff|", MakeScopedCString(dump_buf).get());

  ASSERT_EQ(NC_NOERR, ncaux_dump_data(nc, vlen_t, &vl_in[0], 1, &dump_buf));
  auto vlen_dump = MakeScopedCString(dump_buf);
  ASSERT_NE(nullptr, vlen_dump.get());
  EXPECT_THAT(vlen_dump.get(), HasSubstr("len=3,p=(10 20 30)"));

  ASSERT_EQ(NC_NOERR, ncaux_dump_data(nc, cmpd_t, &rec_in[0], 1, &dump_buf));
  auto cmpd_dump = MakeScopedCString(dump_buf);
  ASSERT_NE(nullptr, cmpd_dump.get());
  EXPECT_THAT(cmpd_dump.get(), HasSubstr("code()42"));
  EXPECT_THAT(cmpd_dump.get(), HasSubstr("pair(2)10 -20"));
  EXPECT_THAT(cmpd_dump.get(), HasSubstr("label()\"ok\""));

  // Check ncaux_reclaim_data and ncaux_reclaim_data_all (reclaiming while nc
  // is still open).
  EXPECT_EQ(NC_EINVAL, ncaux_reclaim_data(-1, NC_INT, nullptr, 0));
  EXPECT_EQ(NC_EINVAL, ncaux_reclaim_data(nc, NC_NAT, nullptr, 0));
  EXPECT_EQ(NC_EINVAL, ncaux_reclaim_data(nc, NC_INT, nullptr, 1));
  EXPECT_EQ(NC_NOERR, ncaux_reclaim_data(nc, NC_INT, nullptr, 0));
  EXPECT_EQ(NC_NOERR, ncaux_reclaim_data_all(nc, NC_INT, nullptr, 0));
  EXPECT_EQ(NC_NOERR,
            ncaux_reclaim_data_all(nc, NC_INT, std::malloc(sizeof(int)), 0));

  std::vector<AuxDumpRecord> rec_out(2, AuxDumpRecord{});
  absl::Cleanup rec_out_guard([&]() {
    if (nc.valid()) {
      ncaux_reclaim_data(nc, cmpd_t, rec_out.data(), rec_out.size());
    }
  });
  ASSERT_EQ(NC_NOERR, nc_get_var(nc, cmpd_varid, rec_out.data()));
  EXPECT_EQ(42, rec_out[0].code);
  EXPECT_STREQ("ok", rec_out[0].label);
  EXPECT_EQ(99, rec_out[1].code);
  EXPECT_STREQ("warn", rec_out[1].label);

  std::vector<nc_vlen_t> vl_out(2, nc_vlen_t{});
  absl::Cleanup vl_out_guard([&]() {
    if (nc.valid()) {
      ncaux_reclaim_data(nc, vlen_t, vl_out.data(), vl_out.size());
    }
  });
  ASSERT_EQ(NC_NOERR, nc_get_var(nc, vlen_varid, vl_out.data()));
  ASSERT_EQ(3U, vl_out[0].len);
  EXPECT_EQ(30, static_cast<const int32_t*>(vl_out[0].p)[2]);
  ASSERT_EQ(1U, vl_out[1].len);
  EXPECT_EQ(40, static_cast<const int32_t*>(vl_out[1].p)[0]);

  void* copied_cmpd = nullptr;
  absl::Cleanup copied_cmpd_guard([&]() {
    if (copied_cmpd != nullptr && nc.valid()) {
      ncaux_reclaim_data_all(nc, cmpd_t, copied_cmpd, rec_in.size());
      copied_cmpd = nullptr;
    }
  });
  ASSERT_EQ(NC_NOERR, nc_copy_data_all(nc, cmpd_t, rec_in.data(), rec_in.size(),
                                       &copied_cmpd));
  ASSERT_NE(nullptr, copied_cmpd);
  EXPECT_EQ(NC_NOERR,
            ncaux_reclaim_data_all(nc, cmpd_t, copied_cmpd, rec_in.size()));
  copied_cmpd = nullptr;

  void* copied_vlen = nullptr;
  absl::Cleanup copied_vlen_guard([&]() {
    if (copied_vlen != nullptr && nc.valid()) {
      ncaux_reclaim_data_all(nc, vlen_t, copied_vlen, vl_in.size());
      copied_vlen = nullptr;
    }
  });
  ASSERT_EQ(NC_NOERR, nc_copy_data_all(nc, vlen_t, vl_in.data(), vl_in.size(),
                                       &copied_vlen));
  ASSERT_NE(nullptr, copied_vlen);
  EXPECT_EQ(NC_NOERR,
            ncaux_reclaim_data_all(nc, vlen_t, copied_vlen, vl_in.size()));
  copied_vlen = nullptr;
}

TEST(DauxTest, PluginPathParseToStringAppendPrependClearAndReclaim) {
  // Invalid / empty arguments on parsen and parse.
  NCPluginList dirs{};
  absl::Cleanup dirs_guard([&]() { ncaux_plugin_path_clear(&dirs); });

  EXPECT_EQ(NC_EINVAL, ncaux_plugin_path_parsen(4, "/opt", ':', nullptr));
  ASSERT_EQ(NC_NOERR, ncaux_plugin_path_parsen(0, "/opt", ':', &dirs));
  EXPECT_EQ(0U, dirs.ndirs);
  ASSERT_EQ(NC_NOERR, ncaux_plugin_path_parsen(4, nullptr, ':', &dirs));
  EXPECT_EQ(0U, dirs.ndirs);
  ASSERT_EQ(NC_NOERR, ncaux_plugin_path_parse(nullptr, ':', &dirs));
  EXPECT_EQ(0U, dirs.ndirs);

  // Slice parsing via ncaux_plugin_path_parsen ignores bytes after pathlen.
  const std::string sliced = "/alpha:/beta:/ignored";
  ASSERT_EQ(NC_NOERR, ncaux_plugin_path_parsen(12, sliced.data(), ':', &dirs));
  ASSERT_EQ(2U, dirs.ndirs);
  EXPECT_STREQ("/alpha", dirs.dirs[0]);
  EXPECT_STREQ("/beta", dirs.dirs[1]);
  ASSERT_EQ(NC_NOERR, ncaux_plugin_path_clear(&dirs));
  EXPECT_EQ(0U, dirs.ndirs);
  EXPECT_EQ(nullptr, dirs.dirs);

  // Single directory with zero separators parses to one entry.
  ASSERT_EQ(NC_NOERR, ncaux_plugin_path_parse("/solo", ':', &dirs));
  ASSERT_EQ(1U, dirs.ndirs);
  EXPECT_STREQ("/solo", dirs.dirs[0]);
  ASSERT_EQ(NC_NOERR, ncaux_plugin_path_clear(&dirs));

  // Platform default separator (sep = 0 -> ':' on POSIX) and ';'.
  ASSERT_EQ(NC_NOERR, ncaux_plugin_path_parse("/p1:/p2:/p3", '\0', &dirs));
  ASSERT_EQ(3U, dirs.ndirs);
  char* joined = nullptr;
  ASSERT_EQ(NC_NOERR, ncaux_plugin_path_tostring(&dirs, ';', &joined));
  EXPECT_STREQ("/p1;/p2;/p3", MakeScopedCString(joined).get());
  ASSERT_EQ(NC_NOERR, ncaux_plugin_path_tostring(&dirs, '\0', &joined));
  EXPECT_STREQ("/p1:/p2:/p3", MakeScopedCString(joined).get());
  // Null catp still succeeds without leaking.
  EXPECT_EQ(NC_NOERR, ncaux_plugin_path_tostring(&dirs, ':', nullptr));
  ASSERT_EQ(NC_NOERR, ncaux_plugin_path_clear(&dirs));

  // Pre-allocated dirs->dirs skips internal calloc in parsen.
  dirs.dirs = static_cast<char**>(std::calloc(2, sizeof(char*)));
  ASSERT_NE(nullptr, dirs.dirs);
  ASSERT_EQ(NC_NOERR, ncaux_plugin_path_parse("/x;/y", ';', &dirs));
  ASSERT_EQ(2U, dirs.ndirs);
  EXPECT_STREQ("/x", dirs.dirs[0]);
  EXPECT_STREQ("/y", dirs.dirs[1]);
  ASSERT_EQ(NC_NOERR, ncaux_plugin_path_clear(&dirs));

  // tostring validation & empty list & null-entry sparse handling.
  EXPECT_EQ(NC_EINVAL, ncaux_plugin_path_tostring(nullptr, ':', &joined));
  NCPluginList bad_list{2, nullptr};
  EXPECT_EQ(NC_EINVAL, ncaux_plugin_path_tostring(&bad_list, ':', &joined));
  NCPluginList empty_list{};
  ASSERT_EQ(NC_NOERR, ncaux_plugin_path_tostring(&empty_list, ':', &joined));
  EXPECT_STREQ("", MakeScopedCString(joined).get());

  // Append & prepend (both empty and non-empty), including null-arg errors.
  EXPECT_EQ(NC_EINVAL, ncaux_plugin_path_append(nullptr, "/a"));
  EXPECT_EQ(NC_EINVAL, ncaux_plugin_path_append(&dirs, nullptr));
  EXPECT_EQ(NC_EINVAL, ncaux_plugin_path_prepend(nullptr, "/a"));
  EXPECT_EQ(NC_EINVAL, ncaux_plugin_path_prepend(&dirs, nullptr));

  ASSERT_EQ(NC_NOERR, ncaux_plugin_path_prepend(&dirs, "/head"));
  ASSERT_EQ(NC_NOERR, ncaux_plugin_path_append(&dirs, "/mid"));
  ASSERT_EQ(NC_NOERR, ncaux_plugin_path_append(&dirs, "/tail"));
  ASSERT_EQ(NC_NOERR, ncaux_plugin_path_prepend(&dirs, "/new_head"));
  ASSERT_EQ(4U, dirs.ndirs);
  EXPECT_STREQ("/new_head", dirs.dirs[0]);
  EXPECT_STREQ("/head", dirs.dirs[1]);
  EXPECT_STREQ("/mid", dirs.dirs[2]);
  EXPECT_STREQ("/tail", dirs.dirs[3]);

  // Clear and tostring when an interior element is nullptr.
  std::free(dirs.dirs[1]);
  dirs.dirs[1] = nullptr;
  ASSERT_EQ(NC_NOERR, ncaux_plugin_path_tostring(&dirs, ';', &joined));
  EXPECT_STREQ("/new_head;;/mid;/tail", MakeScopedCString(joined).get());
  ASSERT_EQ(NC_NOERR, ncaux_plugin_path_clear(&dirs));
  EXPECT_EQ(NC_NOERR, ncaux_plugin_path_clear(nullptr));

  // Heap-allocated NCPluginList reclaimed via ncaux_plugin_path_reclaim.
  NCPluginList* heap_dirs =
      static_cast<NCPluginList*>(std::calloc(1, sizeof(NCPluginList)));
  ASSERT_NE(nullptr, heap_dirs);
  absl::Cleanup heap_guard([&]() {
    if (heap_dirs != nullptr) {
      ncaux_plugin_path_reclaim(heap_dirs);
      heap_dirs = nullptr;
    }
  });
  ASSERT_EQ(NC_NOERR, ncaux_plugin_path_parse("/one:/two", ':', heap_dirs));
  ASSERT_EQ(NC_NOERR, ncaux_plugin_path_reclaim(heap_dirs));
  heap_dirs = nullptr;
  EXPECT_EQ(NC_NOERR, ncaux_plugin_path_reclaim(nullptr));
}

TEST(DauxTest, ParseProvenanceHandlesSeparatorsEscapesAndErrors) {
  auto collect_and_free = [](char** pairs) {
    std::vector<std::string> out;
    if (pairs == nullptr) {
      return out;
    }
    for (size_t i = 0; pairs[i] != nullptr; ++i) {
      out.emplace_back(pairs[i]);
      std::free(pairs[i]);
    }
    std::free(pairs);
    return out;
  };

  // Null pairsp and empty / null input strings.
  EXPECT_EQ(NC_NOERR, ncaux_parse_provenance("a=1", nullptr));

  char* sentinel = nullptr;
  char** pairs = &sentinel;
  EXPECT_EQ(NC_NOERR, ncaux_parse_provenance(nullptr, &pairs));
  EXPECT_EQ(nullptr, pairs);

  pairs = &sentinel;
  EXPECT_EQ(NC_NOERR, ncaux_parse_provenance("", &pairs));
  EXPECT_EQ(nullptr, pairs);

  // Comma-separated pairs plus key with no '=' separator.
  ASSERT_EQ(NC_NOERR,
            ncaux_parse_provenance("version=2,netcdf=4.10.1,flagonly", &pairs));
  EXPECT_THAT(collect_and_free(pairs),
              ::testing::ElementsAre("version", "2", "netcdf", "4.10.1",
                                     "flagonly", ""));

  // Pipe-separated pairs with escaped delimiters, equals signs, and
  // backslashes. Note: upstream `deescape` places `break;` outside `if(*p ==
  // '\0')`, so interior doubled backslashes (`\\end`) are both skipped (`cend`)
  // while a trailing backslash (`val\`) is preserved (`val\\`).
  ASSERT_EQ(NC_NOERR,
            ncaux_parse_provenance(
                R"(k\=1\,2\|3=v\=a\,b\|c\\end|plain=ok|trail=val\)", &pairs));
  EXPECT_THAT(collect_and_free(pairs),
              ::testing::ElementsAre("k=1,2|3", "v=a,b|cend", "plain", "ok",
                                     "trail", "val\\"));

  // Invalid empty keys rejected cleanly without leaking.
  for (const char* bad_prop : {",a=1", "a=1,", "a=1||b=2"}) {
    SCOPED_TRACE(bad_prop);
    pairs = nullptr;
    EXPECT_EQ(NC_EINVAL, ncaux_parse_provenance(bad_prop, &pairs));
    EXPECT_EQ(nullptr, pairs);
  }
}

}  // namespace
}  // namespace netcdf
