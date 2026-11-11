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

// Unit tests for //third_party/netcdf/libdispatch/dinfermodel.c.

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"
// clang-format off
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
// clang-format on

extern "C" {
#include "third_party/netcdf/include/ncdispatch.h"
#include "third_party/netcdf/include/ncmodel.h"
#include "third_party/netcdf/include/netcdf_dispatch.h"
int nc_initialize(void);
}  // extern "C"

namespace netcdf {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;
using ::testing::StartsWith;

struct InferOutcome {
  int status = NC_NOERR;
  int omode = 0;
  NCmodel model{};
  std::string newpath;
  bool has_newpath = false;
};

InferOutcome CallInferModel(const char* path, int omode, int iscreate,
                            int useparallel = 0, void* params = nullptr,
                            bool request_newpath = true) {
  InferOutcome out;
  out.omode = omode;
  char* raw_newpath = nullptr;
  out.status =
      NC_infermodel(path, &out.omode, iscreate, useparallel, params, &out.model,
                    request_newpath ? &raw_newpath : nullptr);
  if (raw_newpath != nullptr) {
    out.has_newpath = true;
    out.newpath = raw_newpath;
    std::free(raw_newpath);
  }
  return out;
}

bool WriteBinaryFile(const TempFile& file, std::string_view bytes) {
  std::ofstream ofs(file.path(), std::ios::binary | std::ios::trunc);
  if (!ofs.is_open()) {
    return false;
  }
  ofs.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  ofs.close();
  return !ofs.fail();
}

std::string MakePaddedMagic(std::string_view magic, size_t total_size) {
  std::string buf(total_size, '\0');
  const size_t copy_len = magic.size() < total_size ? magic.size() : total_size;
  std::memcpy(buf.data(), magic.data(), copy_len);
  return buf;
}

// Saves and restores a single UDF dispatch/magic slot so tests remain isolated
// across --gunit_shuffle and --gunit_repeat.
class ScopedUdfRegistration {
 public:
  ScopedUdfRegistration(int mode_flag, int index, std::string_view magic)
      : index_(index) {
    prev_table_ = UDF_dispatch_tables[index_];
    std::memcpy(prev_magic_, UDF_magic_numbers[index_], sizeof(prev_magic_));
    table_ = {};
    table_.model = (index <= 1) ? (NC_FORMATX_UDF0 + index)
                                : (NC_FORMATX_UDF2 + index - 2);
    table_.dispatch_version = NC_DISPATCH_VERSION;
    char magic_buf[NC_MAX_MAGIC_NUMBER_LEN + 1]{};
    const size_t copy_len = magic.size() < NC_MAX_MAGIC_NUMBER_LEN
                                ? magic.size()
                                : NC_MAX_MAGIC_NUMBER_LEN;
    if (copy_len > 0) {
      std::memcpy(magic_buf, magic.data(), copy_len);
    }
    status_ = nc_def_user_format(mode_flag, &table_, magic_buf);
  }

  ~ScopedUdfRegistration() {
    UDF_dispatch_tables[index_] = prev_table_;
    std::memcpy(UDF_magic_numbers[index_], prev_magic_, sizeof(prev_magic_));
  }

  ScopedUdfRegistration(const ScopedUdfRegistration&) = delete;
  ScopedUdfRegistration& operator=(const ScopedUdfRegistration&) = delete;

  int status() const { return status_; }

 private:
  int index_;
  NC_Dispatch table_{};
  NC_Dispatch* prev_table_ = nullptr;
  char prev_magic_[NC_MAX_MAGIC_NUMBER_LEN + 1]{};
  int status_ = NC_NOERR;
};

class DinfermodelTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_EQ(NC_NOERR, nc_initialize()); }
};

TEST_F(DinfermodelTest, RejectsEmptyPathAndUnsupportedUrlProtocol) {
  const InferOutcome empty_out = CallInferModel("", NC_NOWRITE, /*iscreate=*/1);
  EXPECT_EQ(NC_EURL, empty_out.status);
  EXPECT_FALSE(empty_out.has_newpath);

  const InferOutcome bad_proto = CallInferModel(
      "ftp://example.invalid/archive/data.nc", NC_NOWRITE, /*iscreate=*/1);
  EXPECT_EQ(NC_EINVAL, bad_proto.status);
  EXPECT_FALSE(bad_proto.has_newpath);

  const InferOutcome plain_path =
      CallInferModel("plain_dataset.nc", NC_CLOBBER, /*iscreate=*/1);
  EXPECT_EQ(NC_NOERR, plain_path.status);
  EXPECT_FALSE(plain_path.has_newpath);
  EXPECT_EQ(NC_FORMATX_NC3, plain_path.model.impl);
  EXPECT_EQ(NC_FORMAT_CLASSIC, plain_path.model.format);
}

TEST_F(DinfermodelTest, NormalizesProtocolsFileUrlsAndNullNewpath) {
  const InferOutcome dods_out =
      CallInferModel("dods://example.invalid/thredds/dodsC/ds", NC_NOWRITE,
                     /*iscreate=*/0);
  EXPECT_EQ(NC_NOERR, dods_out.status);
  EXPECT_EQ(NC_FORMATX_DAP2, dods_out.model.impl);
  EXPECT_EQ(NC_FORMAT_CLASSIC, dods_out.model.format);
  EXPECT_EQ(0, dods_out.omode & (NC_NETCDF4 | NC_64BIT_OFFSET | NC_64BIT_DATA |
                                 NC_CLASSIC_MODEL));
  EXPECT_THAT(dods_out.newpath, StartsWith("http://example.invalid/"));
  EXPECT_THAT(dods_out.newpath, HasSubstr("#mode=dap2"));

  const InferOutcome dap4_out =
      CallInferModel("dap4://example.invalid/thredds/dap4/ds", NC_NOWRITE,
                     /*iscreate=*/0);
  EXPECT_EQ(NC_NOERR, dap4_out.status);
  EXPECT_EQ(NC_FORMATX_DAP4, dap4_out.model.impl);
  EXPECT_EQ(NC_FORMAT_NETCDF4, dap4_out.model.format);
  EXPECT_NE(0, dap4_out.omode & NC_NETCDF4);
  EXPECT_THAT(dap4_out.newpath, StartsWith("http://example.invalid/"));
  EXPECT_THAT(dap4_out.newpath, HasSubstr("#mode=dap4"));

  const InferOutcome https_out = CallInferModel(
      "https://example.invalid/data#mode=dap2", NC_NOWRITE, /*iscreate=*/0);
  EXPECT_EQ(NC_NOERR, https_out.status);
  EXPECT_EQ(NC_FORMATX_DAP2, https_out.model.impl);

  const InferOutcome s3_out =
      CallInferModel("s3://bucket.example.invalid/object", NC_NOWRITE,
                     /*iscreate=*/0);
  EXPECT_EQ(NC_NOERR, s3_out.status);
  EXPECT_EQ(NC_FORMATX_DAP2, s3_out.model.impl);
  EXPECT_THAT(s3_out.newpath, StartsWith("s3://bucket.example.invalid/"));
  EXPECT_THAT(s3_out.newpath, HasSubstr("#mode=s3"));

  const InferOutcome gs3_out =
      CallInferModel("gs3://bucket.example.invalid/object", NC_NOWRITE,
                     /*iscreate=*/0);
  EXPECT_EQ(NC_NOERR, gs3_out.status);
  EXPECT_EQ(NC_FORMATX_DAP2, gs3_out.model.impl);
  EXPECT_THAT(gs3_out.newpath, StartsWith("gs3://bucket.example.invalid/"));
  EXPECT_THAT(gs3_out.newpath, HasSubstr("#mode=gs3"));

  const InferOutcome abs_file =
      CallInferModel("file:///anchor_dir/sub/../target.nc#mode=netcdf-4",
                     NC_CLOBBER, /*iscreate=*/1);
  EXPECT_EQ(NC_NOERR, abs_file.status);
  EXPECT_EQ(NC_FORMATX_NC4, abs_file.model.impl);
  EXPECT_EQ(NC_FORMAT_NETCDF4, abs_file.model.format);
  EXPECT_NE(0, abs_file.omode & NC_NETCDF4);
  EXPECT_THAT(abs_file.newpath, StartsWith("file:///"));
  EXPECT_THAT(abs_file.newpath, HasSubstr("target.nc#mode=netcdf-4"));

  const InferOutcome rel_file = CallInferModel(
      "file://relative_sub/sample.nc#mode=classic", NC_CLOBBER, /*iscreate=*/1);
  EXPECT_EQ(NC_NOERR, rel_file.status);
  EXPECT_EQ(NC_FORMATX_NC3, rel_file.model.impl);
  EXPECT_THAT(rel_file.newpath, StartsWith("file:///"));
  EXPECT_THAT(rel_file.newpath,
              HasSubstr("relative_sub/sample.nc#mode=classic"));

  // Calling with newpathp == nullptr skips URI rebuilding cleanly.
  const InferOutcome no_newpath = CallInferModel(
      "http://example.invalid/ds#mode=dap4", NC_NOWRITE, /*iscreate=*/0,
      /*useparallel=*/0, /*params=*/nullptr, /*request_newpath=*/false);
  EXPECT_EQ(NC_NOERR, no_newpath.status);
  EXPECT_EQ(NC_FORMATX_DAP4, no_newpath.model.impl);
  EXPECT_FALSE(no_newpath.has_newpath);
}

TEST_F(DinfermodelTest, ExpandsFragmentMacrosAndCoalescesKeyValues) {
  struct MacroCase {
    const char* uri;
    int iscreate;
    int expected_impl;
    const char* expected_substr;
  };
  constexpr MacroCase kCases[] = {
      {"http://example.invalid/ds#dap2", 0, NC_FORMATX_DAP2, "#mode=dap2"},
      {"http://example.invalid/ds#dap4", 0, NC_FORMATX_DAP4, "#mode=dap4"},
      {"http://example.invalid/ds#bytes", 1, NC_FORMATX_NC4, "#mode=bytes"},
      {"http://example.invalid/ds#xarray", 0, NC_FORMATX_NCZARR,
       "#mode=zarr,nczarr"},
      {"http://example.invalid/ds#zarr", 0, NC_FORMATX_NCZARR, "#mode=nczarr"},
      {"http://example.invalid/ds#noxarray", 0, NC_FORMATX_NCZARR,
       "#mode=nczarr"},
      {"http://example.invalid/ds#s3", 0, NC_FORMATX_DAP2, "#mode=s3"},
      {"http://example.invalid/ds#gs3", 0, NC_FORMATX_DAP2, "#mode=gs3"},
  };
  for (const MacroCase& tc : kCases) {
    SCOPED_TRACE(tc.uri);
    const InferOutcome out = CallInferModel(tc.uri, NC_NOWRITE, tc.iscreate);
    EXPECT_EQ(NC_NOERR, out.status);
    EXPECT_EQ(tc.expected_impl, out.model.impl);
    EXPECT_THAT(out.newpath, HasSubstr(tc.expected_substr));
  }

  // Non-empty macro key ("dap2=custom") and unknown singleton pass through
  // unchanged; lacking a "mode" key triggers Phase 6 + Phase 7 DAP2 fallback.
  const InferOutcome non_mode = CallInferModel(
      "http://example.invalid/ds#dap2=custom&unknown_flag", NC_NOWRITE,
      /*iscreate=*/0);
  EXPECT_EQ(NC_NOERR, non_mode.status);
  EXPECT_EQ(NC_FORMATX_DAP2, non_mode.model.impl);
  EXPECT_EQ(NC_FORMAT_NC3, non_mode.model.format);
  EXPECT_THAT(non_mode.newpath, HasSubstr("dap2=custom&unknown_flag"));

  // No fragments at all hits envvlist2string empty fast path.
  const InferOutcome no_frags = CallInferModel("http://example.invalid/plain",
                                               NC_NOWRITE, /*iscreate=*/0);
  EXPECT_EQ(NC_NOERR, no_frags.status);
  EXPECT_EQ(NC_FORMATX_DAP2, no_frags.model.impl);
  EXPECT_EQ("http://example.invalid/plain", no_frags.newpath);

  // Empty key skipped by envvlist2string, duplicate case-insensitive keys,
  // comma-split empty segments, empty value pairs, and value-less flags.
  const InferOutcome coalesced = CallInferModel(
      "http://example.invalid/ds#=emptykey&foo=a,A,,b&FOO=b,c&emptyonly=&"
      "EMPTYONLY=,&flag",
      NC_NOWRITE, /*iscreate=*/0);
  EXPECT_EQ(NC_NOERR, coalesced.status);
  EXPECT_EQ(NC_FORMATX_DAP2, coalesced.model.impl);
  EXPECT_THAT(coalesced.newpath, Not(HasSubstr("emptykey")));
  EXPECT_THAT(coalesced.newpath, HasSubstr("foo="));
  EXPECT_THAT(coalesced.newpath, HasSubstr("flag"));
}

TEST_F(DinfermodelTest, ProcessesModeInferencesNegationsAndEdgeModes) {
  // Multi-hop BFS inference: xarray -> zarr -> nczarr.
  const InferOutcome bfs_out = CallInferModel(
      "http://example.invalid/ds#mode=xarray", NC_NOWRITE, /*iscreate=*/0);
  EXPECT_EQ(NC_NOERR, bfs_out.status);
  EXPECT_EQ(NC_FORMATX_NCZARR, bfs_out.model.impl);
  EXPECT_EQ("http://example.invalid/ds#mode=xarray,zarr,nczarr",
            bfs_out.newpath);

  // Duplicate case-insensitive mode and non-mode key before "mode".
  const InferOutcome dup_infer = CallInferModel(
      "http://example.invalid/ds#other=1&mode=xarray,XARRAY", NC_NOWRITE,
      /*iscreate=*/0);
  EXPECT_EQ(NC_NOERR, dup_infer.status);
  EXPECT_EQ(NC_FORMATX_NCZARR, dup_infer.model.impl);
  EXPECT_THAT(dup_infer.newpath, HasSubstr("other=1"));
  EXPECT_THAT(dup_infer.newpath, HasSubstr("mode=xarray,zarr,nczarr"));
  EXPECT_THAT(dup_infer.newpath, Not(HasSubstr("XARRAY")));

  // Negation: "bytes" negates inferred "nczarr" and "zarr".
  const InferOutcome bytes_neg =
      CallInferModel("http://example.invalid/ds#mode=bytes,xarray", NC_CLOBBER,
                     /*iscreate=*/1);
  EXPECT_EQ(NC_NOERR, bytes_neg.status);
  EXPECT_EQ(NC_FORMATX_NC4, bytes_neg.model.impl);
  EXPECT_EQ("http://example.invalid/ds#mode=bytes,xarray", bytes_neg.newpath);

  // Negation: "noxarray" infers nczarr/zarr and negates "xarray".
  const InferOutcome noxarray_neg = CallInferModel(
      "http://example.invalid/ds#mode=noxarray,xarray", NC_NOWRITE,
      /*iscreate=*/0);
  EXPECT_EQ(NC_NOERR, noxarray_neg.status);
  EXPECT_EQ(NC_FORMATX_NCZARR, noxarray_neg.model.impl);
  EXPECT_EQ("http://example.invalid/ds#mode=noxarray,nczarr,zarr",
            noxarray_neg.newpath);

  // Multiple modes inferring the same target ("noxarray" and "zarr" both
  // infer "nczarr") exercise infernext deduplication.
  const InferOutcome dedup_infer =
      CallInferModel("http://example.invalid/ds#mode=noxarray,zarr", NC_NOWRITE,
                     /*iscreate=*/0);
  EXPECT_EQ(NC_NOERR, dedup_infer.status);
  EXPECT_EQ(NC_FORMATX_NCZARR, dedup_infer.model.impl);
  EXPECT_EQ("http://example.invalid/ds#mode=noxarray,zarr,nczarr",
            dedup_infer.newpath);

  // Empty mode and comma-only mode hit parsemode/list2string empty paths and
  // fall through to Phase 7 DAP2.
  const InferOutcome empty_mode = CallInferModel(
      "http://example.invalid/ds#mode=", NC_NOWRITE, /*iscreate=*/0);
  EXPECT_EQ(NC_NOERR, empty_mode.status);
  EXPECT_EQ(NC_FORMATX_DAP2, empty_mode.model.impl);

  const InferOutcome comma_mode = CallInferModel(
      "http://example.invalid/ds#mode=,", NC_NOWRITE, /*iscreate=*/0);
  EXPECT_EQ(NC_NOERR, comma_mode.status);
  EXPECT_EQ(NC_FORMATX_DAP2, comma_mode.model.impl);
}

TEST_F(DinfermodelTest, MapsAllFormatModeTagsAndHandlesUnreadableUrls) {
  struct ModeTagCase {
    const char* tag;
    int iscreate;
    int initial_omode;
    int expected_impl;
    int expected_format;
  };
  constexpr ModeTagCase kTagCases[] = {
      {"dap2", 0, NC_NETCDF4 | NC_64BIT_OFFSET, NC_FORMATX_DAP2,
       NC_FORMAT_CLASSIC},
      {"dap4", 0, NC_NOWRITE, NC_FORMATX_DAP4, NC_FORMAT_NETCDF4},
      {"nczarr", 0, NC_NOWRITE, NC_FORMATX_NCZARR, NC_FORMAT_NETCDF4},
      {"zarr", 0, NC_NOWRITE, NC_FORMATX_NCZARR, NC_FORMAT_NETCDF4},
      {"unrecognized_tag", 0, NC_NOWRITE, NC_FORMATX_DAP2, NC_FORMAT_NC3},
      {"netcdf-3", 1, NC_NETCDF4, NC_FORMATX_NC3, 0},
      {"classic", 1, NC_NETCDF4, NC_FORMATX_NC3, 0},
      {"netcdf-4", 1, NC_CLOBBER, NC_FORMATX_NC4, NC_FORMAT_NETCDF4},
      {"enhanced", 1, NC_CLOBBER, NC_FORMATX_NC4, NC_FORMAT_NETCDF4},
      {"bytes", 1, NC_CLOBBER, NC_FORMATX_NC4, NC_FORMAT_NETCDF4},
      {"udf0", 1, NC_CLOBBER, NC_FORMATX_UDF0, 0},
      {"udf1", 1, NC_CLOBBER, NC_FORMATX_UDF1, 0},
      {"udf2", 1, NC_CLOBBER, NC_FORMATX_UDF2, 0},
      {"udf3", 1, NC_CLOBBER, NC_FORMATX_UDF3, 0},
      {"udf4", 1, NC_CLOBBER, NC_FORMATX_UDF4, 0},
      {"udf5", 1, NC_CLOBBER, NC_FORMATX_UDF5, 0},
      {"udf6", 1, NC_CLOBBER, NC_FORMATX_UDF6, 0},
      {"udf7", 1, NC_CLOBBER, NC_FORMATX_UDF7, 0},
      {"udf8", 1, NC_CLOBBER, NC_FORMATX_UDF8, 0},
      {"udf9", 1, NC_CLOBBER, NC_FORMATX_UDF9, 0},
  };

  for (const ModeTagCase& tc : kTagCases) {
    SCOPED_TRACE(tc.tag);
    const std::string uri =
        std::string("http://example.invalid/ds#mode=") + tc.tag;
    const InferOutcome out =
        CallInferModel(uri.c_str(), tc.initial_omode, tc.iscreate);
    EXPECT_EQ(NC_NOERR, out.status);
    EXPECT_EQ(tc.expected_impl, out.model.impl);
    EXPECT_EQ(tc.expected_format, out.model.format);
    if (tc.expected_impl == NC_FORMATX_DAP2) {
      EXPECT_EQ(0, out.omode & (NC_NETCDF4 | NC_64BIT_OFFSET | NC_64BIT_DATA |
                                NC_CLASSIC_MODEL));
    } else if (tc.expected_impl == NC_FORMATX_NC3) {
      EXPECT_EQ(0, out.omode & NC_NETCDF4);
    } else if (tc.expected_impl == NC_FORMATX_NC4 ||
               tc.expected_impl == NC_FORMATX_DAP4 ||
               tc.expected_impl == NC_FORMATX_NCZARR) {
      EXPECT_NE(0, out.omode & NC_NETCDF4);
    }
  }

  // Legacy bracketed prefix syntax is converted into # fragments.
  const InferOutcome prefix_out = CallInferModel(
      "[mode=dap4]http://example.invalid/legacy", NC_NOWRITE, /*iscreate=*/0);
  EXPECT_EQ(NC_NOERR, prefix_out.status);
  EXPECT_EQ(NC_FORMATX_DAP4, prefix_out.model.impl);
  EXPECT_EQ("http://example.invalid/legacy#mode=dap4", prefix_out.newpath);

  // Readable URLs with iscreate=0 fail cleanly with NC_ENOTBUILT when byterange
  // HTTP reading is disabled.
  for (const char* uri : {"http://example.invalid/ds#mode=classic",
                          "http://example.invalid/ds#mode=netcdf-4",
                          "http://example.invalid/ds#mode=bytes",
                          "http://example.invalid/ds#mode=udf0"}) {
    SCOPED_TRACE(uri);
    const InferOutcome read_url =
        CallInferModel(uri, NC_NOWRITE, /*iscreate=*/0);
    EXPECT_EQ(NC_ENOTBUILT, read_url.status);
  }
}

TEST_F(DinfermodelTest, InfersCreateModesFromDefaultFormatAndExplicitFlags) {
  struct DefaultCase {
    int default_format;
    int expected_impl;
    int expected_format;
    int expected_omode_bits;
  };
  constexpr DefaultCase kDefaultCases[] = {
      {NC_FORMAT_CLASSIC, NC_FORMATX_NC3, NC_FORMAT_CLASSIC, 0},
      {NC_FORMAT_64BIT_OFFSET, NC_FORMATX_NC3, NC_FORMAT_64BIT_OFFSET,
       NC_64BIT_OFFSET},
      {NC_FORMAT_64BIT_DATA, NC_FORMATX_NC3, NC_FORMAT_64BIT_DATA,
       NC_64BIT_DATA},
      {NC_FORMAT_NETCDF4, NC_FORMATX_NC4, NC_FORMAT_NETCDF4, NC_NETCDF4},
      {NC_FORMAT_NETCDF4_CLASSIC, NC_FORMATX_NC4, NC_FORMAT_NETCDF4_CLASSIC,
       NC_NETCDF4 | NC_CLASSIC_MODEL},
  };
  for (const DefaultCase& tc : kDefaultCases) {
    SCOPED_TRACE(tc.default_format);
    ScopedDefaultFormat default_guard(tc.default_format);
    const InferOutcome out =
        CallInferModel("create_default.nc", NC_CLOBBER, /*iscreate=*/1);
    EXPECT_EQ(NC_NOERR, out.status);
    EXPECT_EQ(tc.expected_impl, out.model.impl);
    EXPECT_EQ(tc.expected_format, out.model.format);
    EXPECT_EQ(tc.expected_omode_bits,
              out.omode & (NC_NETCDF4 | NC_CLASSIC_MODEL | NC_64BIT_OFFSET |
                           NC_64BIT_DATA));
  }

  ScopedDefaultFormat classic_guard(NC_FORMAT_CLASSIC);

  // Explicit flags without default fallback.
  const InferOutcome offset64 =
      CallInferModel("create_64.nc", NC_64BIT_OFFSET, /*iscreate=*/1);
  EXPECT_EQ(NC_NOERR, offset64.status);
  EXPECT_EQ(NC_FORMATX_NC3, offset64.model.impl);
  EXPECT_EQ(NC_FORMAT_64BIT_OFFSET, offset64.model.format);

  const InferOutcome cdf5 =
      CallInferModel("create_cdf5.nc", NC_64BIT_DATA, /*iscreate=*/1);
  EXPECT_EQ(NC_NOERR, cdf5.status);
  EXPECT_EQ(NC_FORMATX_NC3, cdf5.model.impl);
  EXPECT_EQ(NC_FORMAT_64BIT_DATA, cdf5.model.format);

  const InferOutcome nc4 =
      CallInferModel("create_nc4.nc", NC_NETCDF4, /*iscreate=*/1);
  EXPECT_EQ(NC_NOERR, nc4.status);
  EXPECT_EQ(NC_FORMATX_NC4, nc4.model.impl);
  EXPECT_EQ(NC_FORMAT_NETCDF4, nc4.model.format);

  const InferOutcome nc4_classic = CallInferModel(
      "create_nc4c.nc", NC_NETCDF4 | NC_CLASSIC_MODEL, /*iscreate=*/1);
  EXPECT_EQ(NC_NOERR, nc4_classic.status);
  EXPECT_EQ(NC_FORMATX_NC4, nc4_classic.model.impl);
  EXPECT_EQ(NC_FORMAT_NETCDF4_CLASSIC, nc4_classic.model.format);
  EXPECT_EQ(NC_NETCDF4 | NC_CLASSIC_MODEL,
            nc4_classic.omode & (NC_NETCDF4 | NC_CLASSIC_MODEL));

  // Parallel flag converts NC3 -> PNETCDF while leaving NC4 intact.
  const InferOutcome par_classic = CallInferModel(
      "par_classic.nc", NC_CLOBBER, /*iscreate=*/1, /*useparallel=*/1);
  EXPECT_EQ(NC_NOERR, par_classic.status);
  EXPECT_EQ(NC_FORMATX_PNETCDF, par_classic.model.impl);
  EXPECT_EQ(NC_FORMAT_CLASSIC, par_classic.model.format);

  const InferOutcome par_64 = CallInferModel("par_64.nc", NC_64BIT_OFFSET,
                                             /*iscreate=*/1, /*useparallel=*/1);
  EXPECT_EQ(NC_NOERR, par_64.status);
  EXPECT_EQ(NC_FORMATX_PNETCDF, par_64.model.impl);
  EXPECT_EQ(NC_FORMAT_64BIT_OFFSET, par_64.model.format);
  EXPECT_NE(0, par_64.omode & NC_64BIT_OFFSET);

  const InferOutcome par_cdf5 = CallInferModel(
      "par_cdf5.nc", NC_64BIT_DATA, /*iscreate=*/1, /*useparallel=*/1);
  EXPECT_EQ(NC_NOERR, par_cdf5.status);
  EXPECT_EQ(NC_FORMATX_PNETCDF, par_cdf5.model.impl);
  EXPECT_EQ(NC_FORMAT_64BIT_DATA, par_cdf5.model.format);
  EXPECT_NE(0, par_cdf5.omode & NC_64BIT_DATA);

  const InferOutcome par_nc4 = CallInferModel(
      "par_nc4.nc", NC_NETCDF4, /*iscreate=*/1, /*useparallel=*/1);
  EXPECT_EQ(NC_NOERR, par_nc4.status);
  EXPECT_EQ(NC_FORMATX_NC4, par_nc4.model.impl);
  EXPECT_EQ(NC_FORMAT_NETCDF4, par_nc4.model.format);

  // All 10 UDF flags map cleanly across the UDF1 -> UDF2 numbering gap.
  struct UdfCase {
    int mode_flag;
    int expected_impl;
  };
  constexpr UdfCase kUdfCases[] = {
      {NC_UDF0, NC_FORMATX_UDF0}, {NC_UDF1, NC_FORMATX_UDF1},
      {NC_UDF2, NC_FORMATX_UDF2}, {NC_UDF3, NC_FORMATX_UDF3},
      {NC_UDF4, NC_FORMATX_UDF4}, {NC_UDF5, NC_FORMATX_UDF5},
      {NC_UDF6, NC_FORMATX_UDF6}, {NC_UDF7, NC_FORMATX_UDF7},
      {NC_UDF8, NC_FORMATX_UDF8}, {NC_UDF9, NC_FORMATX_UDF9},
  };
  for (const UdfCase& tc : kUdfCases) {
    SCOPED_TRACE(tc.expected_impl);
    const InferOutcome udf_out =
        CallInferModel("udf_create.nc", tc.mode_flag, /*iscreate=*/1);
    EXPECT_EQ(NC_NOERR, udf_out.status);
    EXPECT_EQ(tc.expected_impl, udf_out.model.impl);
    EXPECT_EQ(NC_FORMAT_CLASSIC, udf_out.model.format);
  }

  // UDF combined with format flags updates model->format and omodep bits.
  const InferOutcome udf_64 =
      CallInferModel("udf_64.nc", NC_UDF0 | NC_64BIT_OFFSET, /*iscreate=*/1);
  EXPECT_EQ(NC_NOERR, udf_64.status);
  EXPECT_EQ(NC_FORMATX_UDF0, udf_64.model.impl);
  EXPECT_EQ(NC_FORMAT_64BIT_OFFSET, udf_64.model.format);
  EXPECT_NE(0, udf_64.omode & NC_64BIT_OFFSET);

  const InferOutcome udf_cdf5 =
      CallInferModel("udf_cdf5.nc", NC_UDF2 | NC_64BIT_DATA, /*iscreate=*/1);
  EXPECT_EQ(NC_NOERR, udf_cdf5.status);
  EXPECT_EQ(NC_FORMATX_UDF2, udf_cdf5.model.impl);
  EXPECT_EQ(NC_FORMAT_64BIT_DATA, udf_cdf5.model.format);
  EXPECT_NE(0, udf_cdf5.omode & NC_64BIT_DATA);

  const InferOutcome udf_nc4 =
      CallInferModel("udf_nc4.nc", NC_UDF5 | NC_NETCDF4, /*iscreate=*/1);
  EXPECT_EQ(NC_NOERR, udf_nc4.status);
  EXPECT_EQ(NC_FORMATX_UDF5, udf_nc4.model.impl);
  EXPECT_EQ(NC_FORMAT_NETCDF4, udf_nc4.model.format);
  EXPECT_NE(0, udf_nc4.omode & NC_NETCDF4);

  const InferOutcome udf_nc4c = CallInferModel(
      "udf_nc4c.nc", NC_UDF9 | NC_NETCDF4 | NC_CLASSIC_MODEL, /*iscreate=*/1);
  EXPECT_EQ(NC_NOERR, udf_nc4c.status);
  EXPECT_EQ(NC_FORMATX_UDF9, udf_nc4c.model.impl);
  EXPECT_EQ(NC_FORMAT_NETCDF4_CLASSIC, udf_nc4c.model.format);
  EXPECT_EQ(NC_NETCDF4 | NC_CLASSIC_MODEL,
            udf_nc4c.omode & (NC_NETCDF4 | NC_CLASSIC_MODEL));
}

TEST_F(DinfermodelTest, DetectsDiskMagicNumbersUserBlocksAndErrors) {
  TempFile missing("dinfermodel_missing.nc");
  const InferOutcome missing_out =
      CallInferModel(missing.c_str(), NC_NOWRITE, /*iscreate=*/0);
  EXPECT_EQ(ENOENT, missing_out.status);

  TempFile empty_file("dinfermodel_empty.nc");
  ASSERT_TRUE(WriteBinaryFile(empty_file, ""));
  EXPECT_EQ(
      NC_ENOTNC,
      CallInferModel(empty_file.c_str(), NC_NOWRITE, /*iscreate=*/0).status);

  // Exactly MAGIC_NUMBER_LEN (8 bytes) fails `MAGIC_NUMBER_LEN >= filelen`.
  TempFile tiny_file("dinfermodel_tiny8.nc");
  ASSERT_TRUE(WriteBinaryFile(tiny_file, std::string("CDF\x01\0\0\0\0", 8)));
  EXPECT_EQ(
      NC_ENOTNC,
      CallInferModel(tiny_file.c_str(), NC_NOWRITE, /*iscreate=*/0).status);

  // 600 non-magic bytes: reads offset 0, checks 512, then hits size bound.
  TempFile bogus_file("dinfermodel_bogus600.nc");
  ASSERT_TRUE(WriteBinaryFile(bogus_file, std::string(600, '\x5a')));
  EXPECT_EQ(
      NC_ENOTNC,
      CallInferModel(bogus_file.c_str(), NC_NOWRITE, /*iscreate=*/0).status);

  // Classic CDF-1 signature (and hint override + parallel upgrade).
  TempFile cdf1_file("dinfermodel_cdf1.nc");
  ASSERT_TRUE(WriteBinaryFile(
      cdf1_file, MakePaddedMagic(std::string_view("CDF\x01", 4), 32)));
  const InferOutcome cdf1_out = CallInferModel(
      cdf1_file.c_str(), NC_NETCDF4, /*iscreate=*/0, /*useparallel=*/0);
  EXPECT_EQ(NC_NOERR, cdf1_out.status);
  EXPECT_EQ(NC_FORMATX_NC3, cdf1_out.model.impl);
  EXPECT_EQ(NC_FORMAT_CLASSIC, cdf1_out.model.format);
  EXPECT_EQ(0, cdf1_out.omode & NC_NETCDF4);

  const InferOutcome cdf1_par = CallInferModel(
      cdf1_file.c_str(), NC_NOWRITE, /*iscreate=*/0, /*useparallel=*/1);
  EXPECT_EQ(NC_NOERR, cdf1_par.status);
  EXPECT_EQ(NC_FORMATX_PNETCDF, cdf1_par.model.impl);
  EXPECT_EQ(NC_FORMAT_CLASSIC, cdf1_par.model.format);

  // 64-bit offset CDF-2 signature.
  TempFile cdf2_file("dinfermodel_cdf2.nc");
  ASSERT_TRUE(WriteBinaryFile(
      cdf2_file, MakePaddedMagic(std::string_view("CDF\x02", 4), 32)));
  const InferOutcome cdf2_out =
      CallInferModel(cdf2_file.c_str(), NC_NOWRITE, /*iscreate=*/0);
  EXPECT_EQ(NC_NOERR, cdf2_out.status);
  EXPECT_EQ(NC_FORMATX_NC3, cdf2_out.model.impl);
  EXPECT_EQ(NC_FORMAT_64BIT_OFFSET, cdf2_out.model.format);
  EXPECT_NE(0, cdf2_out.omode & NC_64BIT_OFFSET);

  // CDF-5 signature.
  TempFile cdf5_file("dinfermodel_cdf5.nc");
  ASSERT_TRUE(WriteBinaryFile(
      cdf5_file, MakePaddedMagic(std::string_view("CDF\x05", 4), 32)));
  const InferOutcome cdf5_out =
      CallInferModel(cdf5_file.c_str(), NC_NOWRITE, /*iscreate=*/0);
  EXPECT_EQ(NC_NOERR, cdf5_out.status);
  EXPECT_EQ(NC_FORMATX_NC3, cdf5_out.model.impl);
  EXPECT_EQ(NC_FORMAT_64BIT_DATA, cdf5_out.model.format);
  EXPECT_NE(0, cdf5_out.omode & NC_64BIT_DATA);

  // HDF4 magic signature.
  TempFile hdf4_file("dinfermodel_hdf4.nc");
  ASSERT_TRUE(WriteBinaryFile(
      hdf4_file, MakePaddedMagic(std::string_view("\x0e\x03\x13\x01", 4), 32)));
  const InferOutcome hdf4_out =
      CallInferModel(hdf4_file.c_str(), NC_NOWRITE, /*iscreate=*/0);
  EXPECT_EQ(NC_NOERR, hdf4_out.status);
  EXPECT_EQ(NC_FORMATX_NC_HDF4, hdf4_out.model.impl);
  EXPECT_EQ(NC_FORMAT_NETCDF4, hdf4_out.model.format);
  EXPECT_NE(0, hdf4_out.omode & NC_NETCDF4);

  // HDF5 magic signature at offset 0.
  const std::string kHdf5Magic("\x89HDF\r\n\x1a\n", 8);
  TempFile hdf5_zero_file("dinfermodel_hdf5_0.nc");
  ASSERT_TRUE(WriteBinaryFile(hdf5_zero_file, MakePaddedMagic(kHdf5Magic, 32)));
  const InferOutcome hdf5_zero =
      CallInferModel(hdf5_zero_file.c_str(), NC_NOWRITE, /*iscreate=*/0);
  EXPECT_EQ(NC_NOERR, hdf5_zero.status);
  EXPECT_EQ(NC_FORMATX_NC4, hdf5_zero.model.impl);
  EXPECT_EQ(NC_FORMAT_NETCDF4, hdf5_zero.model.format);

  // HDF5 magic signature at offset 1024 (tests 512 -> 1024 doubling search).
  TempFile hdf5_ub_file("dinfermodel_hdf5_1024.nc");
  std::string ub_bytes(1040, '\0');
  std::memcpy(ub_bytes.data() + 1024, kHdf5Magic.data(), kHdf5Magic.size());
  ASSERT_TRUE(WriteBinaryFile(hdf5_ub_file, ub_bytes));
  const InferOutcome hdf5_ub =
      CallInferModel(hdf5_ub_file.c_str(), NC_NOWRITE, /*iscreate=*/0);
  EXPECT_EQ(NC_NOERR, hdf5_ub.status);
  EXPECT_EQ(NC_FORMATX_NC4, hdf5_ub.model.impl);
  EXPECT_EQ(NC_FORMAT_NETCDF4, hdf5_ub.model.format);

  // Genuine NetCDF-4 file exercises H5Fis_accessible > 0 inside isdaoscontainer
  // and full end-to-end nc_create / nc_open inference.
  TempFile real_nc4("dinfermodel_real_nc4.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(real_nc4.c_str(),
                                  NC_NETCDF4 | NC_CLASSIC_MODEL, nc.ptr()));
  }
  const InferOutcome real_nc4_infer =
      CallInferModel(real_nc4.c_str(), NC_NOWRITE, /*iscreate=*/0);
  EXPECT_EQ(NC_NOERR, real_nc4_infer.status);
  EXPECT_EQ(NC_FORMATX_NC4, real_nc4_infer.model.impl);
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_open(real_nc4.c_str(), NC_NOWRITE, nc.ptr()));
    int fmt = -1;
    int ext_fmt = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_format(nc.get(), &fmt));
    ASSERT_EQ(NC_NOERR, nc_inq_format_extended(nc.get(), &ext_fmt, nullptr));
    EXPECT_EQ(NC_FORMAT_NETCDF4_CLASSIC, fmt);
    EXPECT_EQ(NC_FORMATX_NC_HDF5, ext_fmt);
  }

  // Public nc_open error paths routed through NC_infermodel.
  {
    ScopedNcFile nc;
    EXPECT_EQ(NC_EURL, nc_open("", NC_NOWRITE, nc.ptr()));
    EXPECT_EQ(NC_EINVAL,
              nc_open("ftp://example.invalid/a.nc", NC_NOWRITE, nc.ptr()));
    EXPECT_EQ(ENOENT, nc_open(missing.c_str(), NC_NOWRITE, nc.ptr()));
    EXPECT_EQ(NC_ENOTBUILT, nc_open("http://example.invalid/ds#mode=classic",
                                    NC_NOWRITE, nc.ptr()));
  }
}

TEST_F(DinfermodelTest, DetectsInMemoryMagicNumbersAndIntegration) {
  struct MemFormatCase {
    const char* name;
    int cmode;
    int expected_impl;
    int expected_format;
  };
  constexpr MemFormatCase kMemCases[] = {
      {"classic", NC_CLOBBER, NC_FORMATX_NC3, NC_FORMAT_CLASSIC},
      {"offset64", NC_64BIT_OFFSET, NC_FORMATX_NC3, NC_FORMAT_64BIT_OFFSET},
      {"cdf5", NC_64BIT_DATA, NC_FORMATX_NC3, NC_FORMAT_64BIT_DATA},
      {"netcdf4", NC_NETCDF4, NC_FORMATX_NC4, NC_FORMAT_NETCDF4},
      {"netcdf4_classic", NC_NETCDF4 | NC_CLASSIC_MODEL, NC_FORMATX_NC4,
       NC_FORMAT_NETCDF4_CLASSIC},
  };

  for (const MemFormatCase& tc : kMemCases) {
    SCOPED_TRACE(tc.name);
    ScopedMemio memio;
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR, nc_create_mem(tc.name, tc.cmode, 4096, nc.ptr()));
      ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
    }

    const InferOutcome mem_infer = CallInferModel(
        tc.name, NC_INMEMORY, /*iscreate=*/0, /*useparallel=*/0, memio.get());
    EXPECT_EQ(NC_NOERR, mem_infer.status);
    EXPECT_EQ(tc.expected_impl, mem_infer.model.impl);
    if (tc.cmode == (NC_NETCDF4 | NC_CLASSIC_MODEL)) {
      EXPECT_EQ(NC_FORMAT_NETCDF4, mem_infer.model.format);
    } else {
      EXPECT_EQ(tc.expected_format, mem_infer.model.format);
    }

    ScopedNcFile reopened;
    ASSERT_EQ(NC_NOERR, nc_open_mem(tc.name, 0, memio.size(),
                                    memio.get()->memory, reopened.ptr()));
    int fmt = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_format(reopened.get(), &fmt));
    EXPECT_EQ(tc.expected_format, fmt);
  }

  // Short in-memory buffer fails len check.
  char short_buf[8] = {'C', 'D', 'F', '\x01', 0, 0, 0, 0};
  NC_memio short_mem{};
  short_mem.size = sizeof(short_buf);
  short_mem.memory = short_buf;
  EXPECT_EQ(NC_ENOTNC, CallInferModel("short.nc", NC_INMEMORY, /*iscreate=*/0,
                                      /*useparallel=*/0, &short_mem)
                           .status);

  // Non-magic 600-byte in-memory buffer exhausts offset search -> NC_ENOTNC.
  std::string bogus_mem_buf(600, '\x42');
  NC_memio bogus_mem{};
  bogus_mem.size = bogus_mem_buf.size();
  bogus_mem.memory = bogus_mem_buf.data();
  EXPECT_EQ(NC_ENOTNC,
            CallInferModel("bogus_mem.nc", NC_INMEMORY,
                           /*iscreate=*/0, /*useparallel=*/0, &bogus_mem)
                .status);

  // In-memory HDF5 signature at offset 1024 traverses 512 -> 1024 search loop.
  const std::string kHdf5Magic("\x89HDF\r\n\x1a\n", 8);
  std::string ub_mem_buf(1040, '\0');
  std::memcpy(ub_mem_buf.data() + 1024, kHdf5Magic.data(), kHdf5Magic.size());
  NC_memio ub_mem{};
  ub_mem.size = ub_mem_buf.size();
  ub_mem.memory = ub_mem_buf.data();
  const InferOutcome ub_out =
      CallInferModel("ub_mem.nc", NC_INMEMORY, /*iscreate=*/0,
                     /*useparallel=*/0, &ub_mem);
  EXPECT_EQ(NC_NOERR, ub_out.status);
  EXPECT_EQ(NC_FORMATX_NC4, ub_out.model.impl);
  EXPECT_EQ(NC_FORMAT_NETCDF4, ub_out.model.format);
}

TEST_F(DinfermodelTest, HandlesUdfMagicMatchingAndPreservation) {
  ScopedUdfRegistration udf1(NC_UDF1, 1, "MYUDF1!");
  ASSERT_EQ(NC_NOERR, udf1.status());
  ScopedUdfRegistration udf9(NC_UDF9, 9, "MYUDF9!");
  ASSERT_EQ(NC_NOERR, udf9.status());

  TempFile udf1_file("dinfermodel_udf1.dat");
  ASSERT_TRUE(WriteBinaryFile(udf1_file, MakePaddedMagic("MYUDF1!", 32)));
  const InferOutcome out1 =
      CallInferModel(udf1_file.c_str(), NC_NOWRITE, /*iscreate=*/0);
  EXPECT_EQ(NC_NOERR, out1.status);
  EXPECT_EQ(NC_FORMATX_UDF1, out1.model.impl);

  TempFile udf9_file("dinfermodel_udf9.dat");
  ASSERT_TRUE(WriteBinaryFile(udf9_file, MakePaddedMagic("MYUDF9!", 32)));
  const InferOutcome out9 =
      CallInferModel(udf9_file.c_str(), NC_NOWRITE, /*iscreate=*/0);
  EXPECT_EQ(NC_NOERR, out9.status);
  EXPECT_EQ(NC_FORMATX_UDF9, out9.model.impl);

  // Explicit UDF mode flags on non-matching bytes preserve tmpimpl across
  // magic interpretation for both UDF0..1 and UDF2..9 ranges.
  TempFile custom_file("dinfermodel_udf_custom.dat");
  ASSERT_TRUE(WriteBinaryFile(custom_file, MakePaddedMagic("CUSTOM00", 32)));

  const InferOutcome keep0 =
      CallInferModel(custom_file.c_str(), NC_UDF0, /*iscreate=*/0);
  EXPECT_EQ(NC_NOERR, keep0.status);
  EXPECT_EQ(NC_FORMATX_UDF0, keep0.model.impl);

  const InferOutcome keep8 =
      CallInferModel(custom_file.c_str(), NC_UDF8, /*iscreate=*/0);
  EXPECT_EQ(NC_NOERR, keep8.status);
  EXPECT_EQ(NC_FORMATX_UDF8, keep8.model.impl);
}

TEST_F(DinfermodelTest, DisklessCreateRespectsDefaultFormat) {
  ScopedDefaultFormat default_nc4(NC_FORMAT_NETCDF4);
  TempFile diskless_file("dinfermodel_diskless.nc");
  ScopedNcFile nc;
  ASSERT_EQ(NC_NOERR, nc_create(diskless_file.c_str(), NC_CLOBBER | NC_DISKLESS,
                                nc.ptr()));
  int fmt = -1;
  ASSERT_EQ(NC_NOERR, nc_inq_format(nc.get(), &fmt));
  EXPECT_EQ(NC_FORMAT_NETCDF4, fmt);
}

}  // namespace
}  // namespace netcdf
