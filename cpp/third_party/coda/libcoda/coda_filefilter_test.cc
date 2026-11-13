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
// Tests CODA file filter matching, directory recursion, symlink/FIFO handling,
// error reporting, and callback abort propagation (libcoda/coda-filefilter.c).
//
// google3 build caveats:
//   - WIN32 branches in coda-filefilter.c (FindFirstFile/FindNextFile) are
//     compiled out on Linux.
//   - Upstream coda-filefilter.c:146-149 returns without calling coda_close()
//     when coda_expression_eval_bool() fails; the test covering that error
//     branch uses absl::LeakCheckDisabler around that call.
//   - Upstream append_string_to_name_buffer() leaves name->buffer[0]
//     uninitialized before the first filepath is appended and allocates
//     new_room without accounting for the trailing NUL when total length is an
//     exact multiple of NAME_BLOCK_SIZE (1024); long-path tests here avoid
//     exact multiples of 1024 and seed an initial short filepath first.
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-filefilter.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda.h

#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/debugging/leak_check.h"
#include "third_party/stcorp_coda/google/util/netcdf_builder.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"
#include "third_party/stcorp_coda/google/util/zip_builder.h"
#include "third_party/stcorp_coda/libcoda/coda.h"

namespace stcorp_coda {
namespace {

using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;

constexpr std::string_view kFilterIndexXml = R"(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="FilterClass">
  <cd:ProductType name="FilterType">
    <cd:ProductDefinition id="FilterProd" format="binary" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">FFTEST01</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
</cd:ProductClass>
)";

constexpr std::string_view kFilterProductXml = R"(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="FilterProd" format="binary">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>8</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="val">
      <cd:Integer>
        <cd:BitSize>16</cd:BitSize>
        <cd:NativeType>uint16</cd:NativeType>
        <cd:LittleEndian/>
      </cd:Integer>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)";

constexpr std::string_view kValidXmlProduct =
    "<?xml version=\"1.0\"?><root><item>ok</item></root>";

constexpr std::string_view kCorruptXmlProduct =
    "<?xml version=\"1.0\"?><broken";

std::vector<uint8_t> MakeFilterCodadefBytes() {
  const std::array<std::pair<std::string_view, std::string_view>, 1> entries = {
      {{"products/FilterProd.xml", kFilterProductXml}},
  };
  return BuildCodadefBytes(kFilterIndexXml, entries);
}

std::array<uint8_t, 10> MakeBinaryProductBytes(uint16_t val) {
  return {
      'F',
      'F',
      'T',
      'E',
      'S',
      'T',
      '0',
      '1',
      static_cast<uint8_t>(val & 0xFFU),
      static_cast<uint8_t>((val >> 8U) & 0xFFU),
  };
}

std::vector<uint8_t> MakeMinimalNetcdfBytes() {
  NetcdfBuilder builder(1);
  builder.AddDim({.name = "x", .size = 2});
  builder.AddVar({
      .name = "v",
      .dim_ids = {0},
      .attrs = {},
      .type = NetcdfType::kInt,
      .vsize = 8,
      .data = {0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02},
  });
  return builder.Build();
}

struct FilterEvent {
  std::string path;
  coda_filefilter_status status;
  std::optional<std::string> error;
};

struct CallbackState {
  std::vector<FilterEvent> events;
  // When non-negative, returns `abort_code` on the 0-based `stop_on_index`
  // callback invocation to abort traversal.
  int stop_on_index = -1;
  int abort_code = 0;
};

int CollectFilterCallback(const char* filepath, coda_filefilter_status status,
                          const char* error, void* userdata) {
  auto* state = static_cast<CallbackState*>(userdata);
  const int index = static_cast<int>(state->events.size());
  state->events.push_back({
      .path = filepath != nullptr ? std::string(filepath) : std::string(),
      .status = status,
      .error =
          error != nullptr ? std::optional<std::string>(error) : std::nullopt,
  });
  if (state->stop_on_index >= 0 && index == state->stop_on_index) {
    return state->abort_code;
  }
  return 0;
}

std::map<std::string, FilterEvent> IndexEventsByPath(
    const std::vector<FilterEvent>& events) {
  std::map<std::string, FilterEvent> by_path;
  for (const FilterEvent& ev : events) {
    by_path.emplace(ev.path, ev);
  }
  return by_path;
}

TEST(CodaFilefilterTest, InvalidArgumentsReturnInvalidArgument) {
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  std::array<const char*, 1> dummy_paths = {"unused.dat"};
  CallbackState state;

  struct InvalidArgCase {
    std::string_view label;
    int num_filepaths;
    const char** filepathlist;
    int (*callbackfunc)(const char*, coda_filefilter_status, const char*,
                        void*);
  };
  const InvalidArgCase kCases[] = {
      {.label = "zero_filepaths",
       .num_filepaths = 0,
       .filepathlist = dummy_paths.data(),
       .callbackfunc = &CollectFilterCallback},
      {.label = "negative_filepaths",
       .num_filepaths = -2,
       .filepathlist = dummy_paths.data(),
       .callbackfunc = &CollectFilterCallback},
      {.label = "null_filepathlist",
       .num_filepaths = 1,
       .filepathlist = nullptr,
       .callbackfunc = &CollectFilterCallback},
      {.label = "null_callback",
       .num_filepaths = 1,
       .filepathlist = dummy_paths.data(),
       .callbackfunc = nullptr},
  };

  for (const InvalidArgCase& tc : kCases) {
    SCOPED_TRACE(tc.label);
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_match_filefilter("true", tc.num_filepaths, tc.filepathlist,
                                    tc.callbackfunc, &state));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  }
  EXPECT_THAT(state.events, IsEmpty());
}

TEST(CodaFilefilterTest, InvalidOrNonBooleanFilterExpressionsFail) {
  const ScopedCodaOptions restore_options;
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  const absl::Cleanup clear_def_path = [] {
    coda_set_definition_path(nullptr);
  };
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(nullptr));
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  std::array<const char*, 1> dummy_paths = {"unused.dat"};
  CallbackState state;

  // 1. Parse error in expression string fails in coda_expression_from_string().
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_match_filefilter("invalid((", 1, dummy_paths.data(),
                                      &CollectFilterCallback, &state));
  EXPECT_EQ(CODA_ERROR_EXPRESSION, coda_errno);

  // 2. Valid expressions whose result_type != coda_expression_boolean fail.
  constexpr const char* kNonBooleanFilters[] = {
      "42",
      "1.5",
      "\"hello\"",
      "/",
  };
  for (const char* filter : kNonBooleanFilters) {
    SCOPED_TRACE(filter);
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_match_filefilter(filter, 1, dummy_paths.data(),
                                        &CollectFilterCallback, &state));
    EXPECT_EQ(CODA_ERROR_EXPRESSION, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno),
                HasSubstr("does not result in a boolean value"));
  }
  EXPECT_THAT(state.events, IsEmpty());
}

TEST(CodaFilefilterTest, NullAndEmptyFilterMatchAllSupportedProducts) {
  const ScopedCodaOptions restore_options;
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  TempFile codadef("ff_null_empty.codadef");
  ASSERT_TRUE(codadef.WriteBytes(MakeFilterCodadefBytes()));
  TempFile bin_file("ff_null_empty_bin.dat");
  ASSERT_TRUE(bin_file.WriteBytes(MakeBinaryProductBytes(5)));
  TempFile xml_file("ff_null_empty.xml");
  ASSERT_TRUE(xml_file.WriteText(kValidXmlProduct));
  TempFile nc_file("ff_null_empty.nc");
  ASSERT_TRUE(nc_file.WriteBytes(MakeMinimalNetcdfBytes()));

  const absl::Cleanup clear_def_path = [] {
    coda_set_definition_path(nullptr);
  };
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef.c_str()));
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  std::array<const char*, 3> paths = {
      bin_file.c_str(),
      xml_file.c_str(),
      nc_file.c_str(),
  };

  for (const char* filter : {static_cast<const char*>(nullptr), ""}) {
    SCOPED_TRACE(filter == nullptr ? "<nullptr>" : "<empty>");
    CallbackState state;
    ASSERT_EQ(CODA_SUCCESS, coda_match_filefilter(
                                filter, static_cast<int>(paths.size()),
                                paths.data(), &CollectFilterCallback, &state));
    ASSERT_EQ(3U, state.events.size());
    for (size_t i = 0; i < paths.size(); ++i) {
      EXPECT_EQ(paths[i], state.events[i].path);
      EXPECT_EQ(coda_ffs_match, state.events[i].status);
      EXPECT_EQ(std::nullopt, state.events[i].error);
    }
  }
}

TEST(CodaFilefilterTest,
     ExpressionFilterDistinguishesMatchesNonMatchesAndFormats) {
  const ScopedCodaOptions restore_options;
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  TempFile codadef("ff_expr_match.codadef");
  ASSERT_TRUE(codadef.WriteBytes(MakeFilterCodadefBytes()));
  TempFile high_bin("ff_expr_high.dat");
  ASSERT_TRUE(high_bin.WriteBytes(MakeBinaryProductBytes(25)));
  TempFile low_bin("ff_expr_low.dat");
  ASSERT_TRUE(low_bin.WriteBytes(MakeBinaryProductBytes(5)));
  TempFile xml_file("ff_expr.xml");
  ASSERT_TRUE(xml_file.WriteText(kValidXmlProduct));
  TempFile nc_file("ff_expr.nc");
  ASSERT_TRUE(nc_file.WriteBytes(MakeMinimalNetcdfBytes()));
  TempFile unknown_file("ff_expr_unknown.dat");
  ASSERT_TRUE(unknown_file.WriteText("UNSUPPORTED_BIN_PAYLOAD"));

  const absl::Cleanup clear_def_path = [] {
    coda_set_definition_path(nullptr);
  };
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef.c_str()));
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // 1. Field-value filter across matching, non-matching, and unsupported files.
  std::array<const char*, 3> bin_paths = {
      high_bin.c_str(),
      low_bin.c_str(),
      unknown_file.c_str(),
  };
  CallbackState field_state;
  ASSERT_EQ(CODA_SUCCESS,
            coda_match_filefilter(
                "productclass() == \"FilterClass\" && int(/val) > 10",
                static_cast<int>(bin_paths.size()), bin_paths.data(),
                &CollectFilterCallback, &field_state));
  ASSERT_EQ(3U, field_state.events.size());
  EXPECT_EQ(coda_ffs_match, field_state.events[0].status);
  EXPECT_EQ(std::nullopt, field_state.events[0].error);
  EXPECT_EQ(coda_ffs_no_match, field_state.events[1].status);
  EXPECT_EQ(std::nullopt, field_state.events[1].error);
  EXPECT_EQ(coda_ffs_unsupported_file, field_state.events[2].status);
  EXPECT_EQ(std::nullopt, field_state.events[2].error);

  // 2. Format filter across NetCDF, XML, and binary products.
  std::array<const char*, 3> format_paths = {
      nc_file.c_str(),
      xml_file.c_str(),
      high_bin.c_str(),
  };
  CallbackState format_state;
  ASSERT_EQ(CODA_SUCCESS,
            coda_match_filefilter(
                "productformat() == \"netcdf\" || productformat() == \"xml\"",
                static_cast<int>(format_paths.size()), format_paths.data(),
                &CollectFilterCallback, &format_state));
  ASSERT_EQ(3U, format_state.events.size());
  EXPECT_EQ(coda_ffs_match, format_state.events[0].status);
  EXPECT_EQ(coda_ffs_match, format_state.events[1].status);
  EXPECT_EQ(coda_ffs_no_match, format_state.events[2].status);
}

TEST(CodaFilefilterTest,
     DirectoryRecursionSymlinksFifoAndLongPathBufferGrowth) {
  const ScopedCodaOptions restore_options;
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  TempFile codadef("ff_dir_tree.codadef");
  ASSERT_TRUE(codadef.WriteBytes(MakeFilterCodadefBytes()));
  TempDir root("ff_dir_tree");
  TempDir external_target("ff_dir_external");

  const std::filesystem::path top_match = root.child("top_match.dat");
  const std::filesystem::path sub_dir = root.child("sub");
  const std::filesystem::path sub_nomatch = sub_dir / "sub_nomatch.dat";
  const std::filesystem::path sub_xml = sub_dir / "sub.xml";
  const std::filesystem::path ext_match = external_target.child("ext.dat");
  const std::filesystem::path ext_dir = external_target.child("linked_dir");
  const std::filesystem::path ext_dir_file = ext_dir / "inside_link.dat";
  const std::filesystem::path file_symlink = root.child("link_to_file.dat");
  const std::filesystem::path dir_symlink = root.child("link_to_dir");
  const std::filesystem::path broken_symlink = root.child("broken_link.dat");
  const std::filesystem::path dir_fifo = root.child("ignored_fifo");

  ASSERT_TRUE(WriteFileBytes(top_match, MakeBinaryProductBytes(20)));
  ASSERT_TRUE(WriteFileBytes(sub_nomatch, MakeBinaryProductBytes(3)));
  ASSERT_TRUE(WriteFileText(sub_xml, kValidXmlProduct));
  ASSERT_TRUE(WriteFileBytes(ext_match, MakeBinaryProductBytes(30)));
  ASSERT_TRUE(WriteFileBytes(ext_dir_file, MakeBinaryProductBytes(40)));

  std::error_code ec;
  std::filesystem::create_symlink(ext_match, file_symlink, ec);
  ASSERT_FALSE(ec) << ec.message();
  std::filesystem::create_symlink(ext_dir, dir_symlink, ec);
  ASSERT_FALSE(ec) << ec.message();
  std::filesystem::create_symlink(root.child("does_not_exist"), broken_symlink,
                                  ec);
  ASSERT_FALSE(ec) << ec.message();
  ASSERT_EQ(0, ::mkfifo(dir_fifo.c_str(), 0600));

  // Build nested subdirectories exceeding NAME_BLOCK_SIZE (1024) so
  // append_string_to_name_buffer() reallocates during recursion without hitting
  // an exact multiple of 1024.
  std::filesystem::path deep_dir = root.path();
  for (int depth = 0; depth < 8; ++depth) {
    std::string component(160, static_cast<char>('a' + depth));
    const size_t candidate_len =
        deep_dir.native().size() + 1 + component.size();
    if (candidate_len % 1024 == 0 || candidate_len % 1024 == 1023) {
      component.append("_pad");
    }
    deep_dir /= component;
  }
  ASSERT_GT(deep_dir.native().size(), 1200U);
  ASSERT_TRUE(std::filesystem::create_directories(deep_dir, ec))
      << ec.message();
  std::string deep_filename = "deep_match.dat";
  if ((deep_dir.native().size() + 1 + deep_filename.size()) % 1024 == 0) {
    deep_filename = "deep_match_x.dat";
  }
  const std::filesystem::path deep_file = deep_dir / deep_filename;
  ASSERT_TRUE(WriteFileBytes(deep_file, MakeBinaryProductBytes(50)));

  // Second top-level path (> 2100 bytes, < PATH_MAX, non-multiple of 1024,
  // with every component <= 255 bytes) exercises multi-block growth in
  // append_string_to_name_buffer() after iteration 0 resets
  // path_name.buffer[0] = '\0', while keeping stat() in the ENOENT branch.
  std::filesystem::path long_missing = root.path();
  const std::string missing_segment(150, 'z');
  while (long_missing.native().size() < 2200) {
    long_missing /= missing_segment;
  }
  long_missing /= "missing.dat";
  std::string long_missing_path = long_missing.native();
  if (long_missing_path.size() % 1024 == 0) {
    long_missing_path.append("_x");
  }

  const absl::Cleanup clear_def_path = [] {
    coda_set_definition_path(nullptr);
  };
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef.c_str()));
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  std::array<const char*, 2> roots = {
      root.c_str(),
      long_missing_path.c_str(),
  };
  CallbackState state;
  ASSERT_EQ(CODA_SUCCESS,
            coda_match_filefilter("exists(/val) && int(/val) > 10",
                                  static_cast<int>(roots.size()), roots.data(),
                                  &CollectFilterCallback, &state));

  const std::map<std::string, FilterEvent> by_path =
      IndexEventsByPath(state.events);
  ASSERT_EQ(8U, by_path.size());
  EXPECT_EQ(coda_ffs_match, by_path.at(top_match.native()).status);
  EXPECT_EQ(coda_ffs_no_match, by_path.at(sub_nomatch.native()).status);
  EXPECT_EQ(coda_ffs_no_match, by_path.at(sub_xml.native()).status);
  EXPECT_EQ(coda_ffs_match, by_path.at(file_symlink.native()).status);
  EXPECT_EQ(coda_ffs_match,
            by_path.at((dir_symlink / "inside_link.dat").native()).status);
  EXPECT_EQ(coda_ffs_match, by_path.at(deep_file.native()).status);

  const FilterEvent& broken_ev = by_path.at(broken_symlink.native());
  EXPECT_EQ(coda_ffs_error, broken_ev.status);
  ASSERT_TRUE(broken_ev.error.has_value());
  EXPECT_STREQ("no such file or directory", broken_ev.error->c_str());

  const FilterEvent& long_missing_ev = by_path.at(long_missing_path);
  EXPECT_EQ(coda_ffs_error, long_missing_ev.status);
  ASSERT_TRUE(long_missing_ev.error.has_value());
  EXPECT_STREQ("no such file or directory", long_missing_ev.error->c_str());

  // Non-regular, non-directory entries inside directories (FIFO) are ignored.
  EXPECT_EQ(by_path.end(), by_path.find(dir_fifo.native()));
}

TEST(CodaFilefilterTest,
     StatErrorsFifoAndCorruptOrUnsupportedFilesReportExpectedStatus) {
  const ScopedCodaOptions restore_options;
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  TempDir dir("ff_errors");
  const std::filesystem::path missing_path = dir.child("missing_file.dat");
  const std::filesystem::path regular_file = dir.child("regular.dat");
  const std::filesystem::path not_a_dir_child = regular_file / "child";
  const std::filesystem::path loop_a = dir.child("loop_a");
  const std::filesystem::path loop_b = dir.child("loop_b");
  const std::filesystem::path fifo_path = dir.child("direct_fifo");
  const std::filesystem::path corrupt_xml = dir.child("corrupt.xml");
  const std::filesystem::path hdf4_file = dir.child("fake.hdf");

  ASSERT_TRUE(WriteFileText(regular_file, "PLAIN_UNSUPPORTED_BYTES"));
  ASSERT_TRUE(WriteFileText(corrupt_xml, kCorruptXmlProduct));
  // HDF4 magic header (\016\003\023\001) triggers CODA_ERROR_NO_HDF4_SUPPORT.
  constexpr auto kFakeHdf4Bytes =
      std::to_array<uint8_t>({0x0E, 0x03, 0x13, 0x01, 0x00, 0x00, 0x00, 0x00});
  ASSERT_TRUE(WriteFileBytes(hdf4_file, kFakeHdf4Bytes));

  std::error_code ec;
  std::filesystem::create_symlink(loop_b, loop_a, ec);
  ASSERT_FALSE(ec) << ec.message();
  std::filesystem::create_symlink(loop_a, loop_b, ec);
  ASSERT_FALSE(ec) << ec.message();
  ASSERT_EQ(0, ::mkfifo(fifo_path.c_str(), 0600));

  const absl::Cleanup clear_def_path = [] {
    coda_set_definition_path(nullptr);
  };
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(nullptr));
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  std::array<const char*, 7> paths = {
      missing_path.c_str(), not_a_dir_child.c_str(), loop_a.c_str(),
      fifo_path.c_str(),    regular_file.c_str(),    corrupt_xml.c_str(),
      hdf4_file.c_str(),
  };

  CallbackState state;
  ASSERT_EQ(CODA_SUCCESS, coda_match_filefilter(
                              nullptr, static_cast<int>(paths.size()),
                              paths.data(), &CollectFilterCallback, &state));
  ASSERT_EQ(7U, state.events.size());

  // 1. ENOENT.
  EXPECT_EQ(coda_ffs_error, state.events[0].status);
  ASSERT_TRUE(state.events[0].error.has_value());
  EXPECT_STREQ("no such file or directory", state.events[0].error->c_str());

  // 2. ENOTDIR.
  EXPECT_EQ(coda_ffs_error, state.events[1].status);
  ASSERT_TRUE(state.events[1].error.has_value());
  EXPECT_STREQ("no such file or directory", state.events[1].error->c_str());

  // 3. ELOOP (non-ENOENT/ENOTDIR stat error -> strerror(errno)).
  EXPECT_EQ(coda_ffs_error, state.events[2].status);
  ASSERT_TRUE(state.events[2].error.has_value());
  EXPECT_THAT(*state.events[2].error, Not(IsEmpty()));

  // 4. Direct non-regular/non-dir path (FIFO with ignore_other_file_types ==
  // 0).
  EXPECT_EQ(coda_ffs_error, state.events[3].status);
  ASSERT_TRUE(state.events[3].error.has_value());
  EXPECT_STREQ("not a directory or regular file",
               state.events[3].error->c_str());

  // 5. Unrecognized binary file -> CODA_ERROR_UNSUPPORTED_PRODUCT.
  EXPECT_EQ(coda_ffs_unsupported_file, state.events[4].status);
  EXPECT_EQ(std::nullopt, state.events[4].error);

  // 6. Corrupt XML -> coda_ffs_could_not_open_file.
  EXPECT_EQ(coda_ffs_could_not_open_file, state.events[5].status);
  ASSERT_TRUE(state.events[5].error.has_value());
  EXPECT_THAT(*state.events[5].error, Not(IsEmpty()));

  // 7. HDF4 magic -> coda_ffs_could_not_open_file (no HDF4 support).
  EXPECT_EQ(coda_ffs_could_not_open_file, state.events[6].status);
  ASSERT_TRUE(state.events[6].error.has_value());
  EXPECT_THAT(*state.events[6].error, HasSubstr("HDF4"));
}

TEST(CodaFilefilterTest, ExpressionEvaluationErrorReportsErrorStatus) {
  const ScopedCodaOptions restore_options;
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  TempFile codadef("ff_eval_err.codadef");
  ASSERT_TRUE(codadef.WriteBytes(MakeFilterCodadefBytes()));
  TempFile prod("ff_eval_err.dat");
  ASSERT_TRUE(prod.WriteBytes(MakeBinaryProductBytes(15)));

  const absl::Cleanup clear_def_path = [] {
    coda_set_definition_path(nullptr);
  };
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef.c_str()));
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  std::array<const char*, 1> paths = {prod.c_str()};
  CallbackState state;
  int rc = -1;
  {
    // Upstream coda-filefilter.c:146-149 returns without calling
    // coda_close(product) when coda_expression_eval_bool() returns non-zero.
    absl::LeakCheckDisabler leak_disabler;
    rc = coda_match_filefilter("(1 / 0) == 1", 1, paths.data(),
                               &CollectFilterCallback, &state);
  }
  EXPECT_EQ(CODA_SUCCESS, rc);
  ASSERT_EQ(1U, state.events.size());
  EXPECT_EQ(prod.string(), state.events[0].path);
  EXPECT_EQ(coda_ffs_error, state.events[0].status);
  ASSERT_TRUE(state.events[0].error.has_value());
  EXPECT_THAT(*state.events[0].error, Not(IsEmpty()));
}

TEST(CodaFilefilterTest, UnreadableFileAndDirectoryTriggerPermissionErrors) {
  if (::geteuid() == 0) {
    GTEST_SKIP() << "chmod(0000) does not deny access for root";
  }

  const ScopedCodaOptions restore_options;
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  TempDir root("ff_perm");
  const std::filesystem::path unreadable_file = root.child("no_read.dat");
  const std::filesystem::path unreadable_dir = root.child("no_read_dir");
  ASSERT_TRUE(WriteFileBytes(unreadable_file, MakeBinaryProductBytes(1)));
  std::error_code ec;
  ASSERT_TRUE(std::filesystem::create_directory(unreadable_dir, ec))
      << ec.message();

  ASSERT_EQ(0, ::chmod(unreadable_file.c_str(), 0000));
  const absl::Cleanup restore_file_perms = [&unreadable_file] {
    static_cast<void>(::chmod(unreadable_file.c_str(), 0600));
  };
  ASSERT_EQ(0, ::chmod(unreadable_dir.c_str(), 0000));
  const absl::Cleanup restore_dir_perms = [&unreadable_dir] {
    static_cast<void>(::chmod(unreadable_dir.c_str(), 0700));
  };

  const absl::Cleanup clear_def_path = [] {
    coda_set_definition_path(nullptr);
  };
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(nullptr));
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // Start with use_mmap=1 so coda_open() hits CODA_ERROR_FILE_OPEN, exercises
  // the fallback retry with use_mmap=0, and restores use_mmap=1.
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(1));
  std::array<const char*, 2> paths = {
      unreadable_file.c_str(),
      unreadable_dir.c_str(),
  };
  CallbackState state;
  ASSERT_EQ(CODA_SUCCESS, coda_match_filefilter(
                              nullptr, static_cast<int>(paths.size()),
                              paths.data(), &CollectFilterCallback, &state));
  EXPECT_EQ(1, coda_get_option_use_mmap());

  ASSERT_EQ(2U, state.events.size());
  EXPECT_EQ(unreadable_file.native(), state.events[0].path);
  EXPECT_EQ(coda_ffs_could_not_open_file, state.events[0].status);
  ASSERT_TRUE(state.events[0].error.has_value());
  EXPECT_THAT(*state.events[0].error, Not(IsEmpty()));

  EXPECT_EQ(unreadable_dir.native(), state.events[1].path);
  EXPECT_EQ(coda_ffs_could_not_access_directory, state.events[1].status);
  ASSERT_TRUE(state.events[1].error.has_value());
  EXPECT_STREQ("could not recurse into directory",
               state.events[1].error->c_str());
}

TEST(CodaFilefilterTest, CallbackAbortPropagatesReturnValueAndStopsTraversal) {
  const ScopedCodaOptions restore_options;
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  TempDir dir("ff_abort_dir");
  const std::filesystem::path f1 = dir.child("a.xml");
  const std::filesystem::path f2 = dir.child("b.xml");
  const std::filesystem::path f3 = dir.child("c.xml");
  ASSERT_TRUE(WriteFileText(f1, kValidXmlProduct));
  ASSERT_TRUE(WriteFileText(f2, kValidXmlProduct));
  ASSERT_TRUE(WriteFileText(f3, kValidXmlProduct));

  const absl::Cleanup clear_def_path = [] {
    coda_set_definition_path(nullptr);
  };
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(nullptr));
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // 1. Callback returns non-zero inside directory recursion -> closedir() and
  //    early return with exact callback return code.
  {
    std::array<const char*, 1> paths = {dir.c_str()};
    CallbackState state;
    state.stop_on_index = 0;
    state.abort_code = 7;
    EXPECT_EQ(7, coda_match_filefilter(nullptr, 1, paths.data(),
                                       &CollectFilterCallback, &state));
    EXPECT_EQ(1U, state.events.size());
  }

  // 2. Callback returns non-zero on second entry of top-level filepathlist.
  {
    std::array<const char*, 3> paths = {
        f1.c_str(),
        f2.c_str(),
        f3.c_str(),
    };
    CallbackState state;
    state.stop_on_index = 1;
    state.abort_code = 42;
    EXPECT_EQ(42, coda_match_filefilter(nullptr, static_cast<int>(paths.size()),
                                        paths.data(), &CollectFilterCallback,
                                        &state));
    ASSERT_EQ(2U, state.events.size());
    EXPECT_EQ(f1.native(), state.events[0].path);
    EXPECT_EQ(f2.native(), state.events[1].path);
  }
}

// TODO(schwehr): 97.0% line coverage in libcoda/coda-filefilter.c (129/133);
// remaining 4 missed lines (143-144, 401-402) are strdup OOM and directory-open
// error guards.

}  // namespace
}  // namespace stcorp_coda
