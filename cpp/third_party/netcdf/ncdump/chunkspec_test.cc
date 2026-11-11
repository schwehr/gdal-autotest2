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

// Unit tests for //third_party/netcdf/ncdump/chunkspec.c.
//
// Runs each chunkspec scenario inside an isolated forked child because
// dimchunkspecs and varchunkspecs hold process-global allocations with no
// deallocation API.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncdump/chunkspec.c

#include <array>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/ncdump_globals_stub.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
// clang-format on

extern "C" {
// Note: chunkspec.h misdeclares this function as varchunkspec_ndims();
// chunkspec.c defines varchunkspec_rank().
size_t varchunkspec_rank(int igrpid, int ivarid);
}  // extern "C"

namespace netcdf {
namespace {

constexpr std::array<std::string_view, 1> kChildArgv = {"chunkspec_test"};

TEST(ChunkspecTest, DefaultsEmptyNullAndOmitSpecs) {
  const CliResult res = RunCliInForkedChild(
      [](int, char**) -> int {
        ncdump_stub_reset_globals();

        // Accessors are safe even before chunkspecinit().
        if (dimchunkspec_ndims() != 0 || dimchunkspec_omit() != 0 ||
            dimchunkspec_size(0) != 0 || dimchunkspec_exists(0) != 0 ||
            varchunkspec_kind(0, 0) != NC_CONTIGUOUS ||
            varchunkspec_exists(0, 0) != 0 || varchunkspec_omit(0, 0) != 0 ||
            varchunkspec_chunksizes(0, 0) != nullptr ||
            varchunkspec_rank(0, 0) != 0) {
          std::fprintf(stderr, "unexpected pre-init defaults\n");
          return 1;
        }

        // Calling chunkspecinit() twice exercises both varchunkspecs branches.
        chunkspecinit();
        chunkspecinit();

        ScopedNcFile nc;
        if (nc_create("chunkspec_empty.nc", NC_NETCDF4 | NC_DISKLESS,
                      nc.ptr()) != NC_NOERR) {
          return 1;
        }
        int xid = -1;
        int varid = -1;
        if (nc_def_dim(nc.get(), "x", 6, &xid) != NC_NOERR ||
            nc_def_var(nc.get(), "v", NC_INT, 1, &xid, &varid) != NC_NOERR ||
            nc_enddef(nc.get()) != NC_NOERR) {
          return 1;
        }

        if (chunkspec_parse(nc.get(), nullptr) != NC_NOERR ||
            chunkspec_parse(nc.get(), "") != NC_NOERR) {
          std::fprintf(stderr, "empty/null spec failed\n");
          return 1;
        }
        if (dimchunkspec_omit() != 0 || dimchunkspec_ndims() != 0) {
          std::fprintf(stderr, "empty spec changed state\n");
          return 1;
        }

        if (chunkspec_parse(nc.get(), "/") != NC_NOERR ||
            dimchunkspec_omit() != 1 || dimchunkspec_ndims() != 0 ||
            varchunkspec_omit(nc.get(), varid) != 1) {
          std::fprintf(stderr, "slash omit spec failed\n");
          return 1;
        }

        if (chunkspec_parse(nc.get(), "//") != NC_NOERR ||
            dimchunkspec_omit() != 1 || dimchunkspec_ndims() != 0) {
          std::fprintf(stderr, "double-slash omit spec failed\n");
          return 1;
        }
        return 0;
      },
      kChildArgv);

  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;
}

TEST(ChunkspecTest, DimChunkspecParsesValidDimsEscapedCommaHexAndOmittedSizes) {
  const CliResult res1 = RunCliInForkedChild(
      [](int, char**) -> int {
        ncdump_stub_reset_globals();
        chunkspecinit();

        ScopedNcFile nc;
        if (nc_create("chunkspec_dims.nc", NC_NETCDF4 | NC_DISKLESS,
                      nc.ptr()) != NC_NOERR) {
          return 1;
        }
        int xid = -1;
        int yid = -1;
        int zid = -1;
        int abid = -1;
        int wid = -1;
        if (nc_def_dim(nc.get(), "x", 10, &xid) != NC_NOERR ||
            nc_def_dim(nc.get(), "y", 7, &yid) != NC_NOERR ||
            nc_def_dim(nc.get(), "z", 5, &zid) != NC_NOERR ||
            nc_def_dim(nc.get(), "a\\,b", 4, &abid) != NC_NOERR ||
            nc_def_dim(nc.get(), "w", 3, &wid) != NC_NOERR ||
            nc_enddef(nc.get()) != NC_NOERR) {
          return 1;
        }

        if (chunkspec_parse(nc.get(), "x/4,y/,z/0x3,a\\,b/2") != NC_NOERR) {
          std::fprintf(stderr, "valid dimchunkspec parse failed\n");
          return 1;
        }
        if (dimchunkspec_ndims() != 4 || dimchunkspec_omit() != 0) {
          std::fprintf(stderr, "unexpected ndims=%zu omit=%d\n",
                       dimchunkspec_ndims(), dimchunkspec_omit());
          return 1;
        }
        if (dimchunkspec_exists(xid) != 1 || dimchunkspec_size(xid) != 4 ||
            dimchunkspec_exists(yid) != 1 || dimchunkspec_size(yid) != 7 ||
            dimchunkspec_exists(zid) != 1 || dimchunkspec_size(zid) != 3 ||
            dimchunkspec_exists(abid) != 1 || dimchunkspec_size(abid) != 2 ||
            dimchunkspec_exists(wid) != 0 || dimchunkspec_size(wid) != 0) {
          std::fprintf(stderr, "dimchunkspec lookup mismatch\n");
          return 1;
        }
        return 0;
      },
      kChildArgv);
  ASSERT_TRUE(res1.ok()) << res1.harness_error;
  EXPECT_EQ(0, res1.exit_code) << res1.stderr_output;

  // Test trailing omitted size ('/') and chunksize exceeding dimlen.
  const CliResult res2 = RunCliInForkedChild(
      [](int, char**) -> int {
        ncdump_stub_reset_globals();
        chunkspecinit();

        ScopedNcFile nc;
        if (nc_create("chunkspec_dims2.nc", NC_NETCDF4 | NC_DISKLESS,
                      nc.ptr()) != NC_NOERR) {
          return 1;
        }
        int xid = -1;
        int yid = -1;
        if (nc_def_dim(nc.get(), "x", 6, &xid) != NC_NOERR ||
            nc_def_dim(nc.get(), "y", 9, &yid) != NC_NOERR ||
            nc_enddef(nc.get()) != NC_NOERR) {
          return 1;
        }
        if (chunkspec_parse(nc.get(), "x/20,y/") != NC_NOERR) {
          std::fprintf(stderr, "oversized/trailing-omitted spec failed\n");
          return 1;
        }
        if (dimchunkspec_ndims() != 2 || dimchunkspec_size(xid) != 20 ||
            dimchunkspec_size(yid) != 9) {
          std::fprintf(stderr, "unexpected sizes x=%zu y=%zu\n",
                       dimchunkspec_size(xid), dimchunkspec_size(yid));
          return 1;
        }
        return 0;
      },
      kChildArgv);
  ASSERT_TRUE(res2.ok()) << res2.harness_error;
  EXPECT_EQ(0, res2.exit_code) << res2.stderr_output;
}

TEST(ChunkspecTest, DimChunkspecRejectsMalformedAndMissingInputs) {
  struct ErrCase {
    const char* name;
    const char* spec;
    int expected_err;
  };
  constexpr ErrCase kCases[] = {
      {"consecutive_commas", "x/2,,y/3", NC_EINVAL},
      {"missing_slash", "x", NC_EINVAL},
      {"missing_slash_second", "x/2,y", NC_EINVAL},
      {"trailing_comma", "x/2,", NC_EINVAL},
      {"missing_dim", "nosuch/2", NC_EBADDIM},
      {"non_numeric_size", "x/abc", NC_EINVAL},
      {"zero_size", "x/0", NC_EINVAL},
      {"negative_size", "x/-3", NC_EINVAL},
      {"overflow_size", "x/999999999999999999999999999999", NC_EINVAL},
  };

  for (const ErrCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const CliResult res = RunCliInForkedChild(
        [&](int, char**) -> int {
          ncdump_stub_reset_globals();
          chunkspecinit();

          ScopedNcFile nc;
          if (nc_create("chunkspec_dimerr.nc", NC_NETCDF4 | NC_DISKLESS,
                        nc.ptr()) != NC_NOERR) {
            return 1;
          }
          int xid = -1;
          int yid = -1;
          if (nc_def_dim(nc.get(), "x", 6, &xid) != NC_NOERR ||
              nc_def_dim(nc.get(), "y", 6, &yid) != NC_NOERR ||
              nc_enddef(nc.get()) != NC_NOERR) {
            return 1;
          }
          const int actual = chunkspec_parse(nc.get(), tc.spec);
          if (actual != tc.expected_err) {
            std::fprintf(stderr, "spec='%s': expected %d, got %d\n", tc.spec,
                         tc.expected_err, actual);
            return 1;
          }
          return 0;
        },
        kChildArgv);
    ASSERT_TRUE(res.ok()) << res.harness_error;
    EXPECT_EQ(0, res.exit_code) << res.stderr_output;
  }
}

TEST(ChunkspecTest,
     VarChunkspecParsesChunkedCompactContiguousOmitAndSubgroups) {
  const CliResult res = RunCliInForkedChild(
      [](int, char**) -> int {
        ncdump_stub_reset_globals();
        chunkspecinit();

        ScopedNcFile nc;
        if (nc_create("chunkspec_vars.nc", NC_NETCDF4 | NC_DISKLESS,
                      nc.ptr()) != NC_NOERR) {
          return 1;
        }
        int xid = -1;
        int yid = -1;
        if (nc_def_dim(nc.get(), "x", 6, &xid) != NC_NOERR ||
            nc_def_dim(nc.get(), "y", 8, &yid) != NC_NOERR) {
          return 1;
        }
        const std::array<int, 2> dims2 = {xid, yid};
        int v1 = -1;
        int v2 = -1;
        int v3 = -1;
        int v4 = -1;
        int v_other = -1;
        int grpid = -1;
        int subv = -1;
        if (nc_def_var(nc.get(), "v1", NC_INT, 2, dims2.data(), &v1) !=
                NC_NOERR ||
            nc_def_var(nc.get(), "v2", NC_INT, 1, &xid, &v2) != NC_NOERR ||
            nc_def_var(nc.get(), "v3", NC_INT, 1, &xid, &v3) != NC_NOERR ||
            nc_def_var(nc.get(), "v4", NC_INT, 1, &xid, &v4) != NC_NOERR ||
            nc_def_var(nc.get(), "v_other", NC_INT, 1, &xid, &v_other) !=
                NC_NOERR ||
            nc_def_grp(nc.get(), "grp", &grpid) != NC_NOERR ||
            nc_def_var(grpid, "subv", NC_INT, 2, dims2.data(), &subv) !=
                NC_NOERR ||
            nc_enddef(nc.get()) != NC_NOERR) {
          return 1;
        }

        if (chunkspec_parse(nc.get(), "v1:2,4") != NC_NOERR ||
            chunkspec_parse(nc.get(), "v2:") != NC_NOERR ||
            chunkspec_parse(nc.get(), "v3:Compact") != NC_NOERR ||
            chunkspec_parse(nc.get(), "v4:CONTIGUOUS") != NC_NOERR ||
            chunkspec_parse(nc.get(), "/grp/subv:3,4") != NC_NOERR) {
          std::fprintf(stderr, "valid varchunkspec_parse call failed\n");
          return 1;
        }

        const size_t* v1_chunks = varchunkspec_chunksizes(nc.get(), v1);
        if (varchunkspec_exists(nc.get(), v1) != 1 ||
            varchunkspec_kind(nc.get(), v1) != NC_CHUNKED ||
            varchunkspec_omit(nc.get(), v1) != 0 ||
            varchunkspec_rank(nc.get(), v1) != 2 || v1_chunks == nullptr ||
            v1_chunks[0] != 2 || v1_chunks[1] != 4) {
          std::fprintf(stderr, "v1 metadata mismatch\n");
          return 1;
        }

        if (varchunkspec_exists(nc.get(), v2) != 1 ||
            varchunkspec_omit(nc.get(), v2) != 1) {
          std::fprintf(stderr, "v2 omit metadata mismatch\n");
          return 1;
        }

        if (varchunkspec_exists(nc.get(), v3) != 1 ||
            varchunkspec_kind(nc.get(), v3) != NC_COMPACT ||
            varchunkspec_omit(nc.get(), v3) != 0) {
          std::fprintf(stderr, "v3 compact metadata mismatch\n");
          return 1;
        }

        if (varchunkspec_exists(nc.get(), v4) != 1 ||
            varchunkspec_kind(nc.get(), v4) != NC_CONTIGUOUS ||
            varchunkspec_omit(nc.get(), v4) != 0) {
          std::fprintf(stderr, "v4 contiguous metadata mismatch\n");
          return 1;
        }

        const size_t* sub_chunks = varchunkspec_chunksizes(grpid, subv);
        if (varchunkspec_exists(grpid, subv) != 1 ||
            varchunkspec_kind(grpid, subv) != NC_CHUNKED ||
            varchunkspec_rank(grpid, subv) != 2 || sub_chunks == nullptr ||
            sub_chunks[0] != 3 || sub_chunks[1] != 4) {
          std::fprintf(stderr, "/grp/subv metadata mismatch\n");
          return 1;
        }

        if (varchunkspec_exists(nc.get(), v_other) != 0 ||
            varchunkspec_kind(nc.get(), v_other) != NC_CONTIGUOUS ||
            varchunkspec_omit(nc.get(), v_other) != 0 ||
            varchunkspec_chunksizes(nc.get(), v_other) != nullptr ||
            varchunkspec_rank(nc.get(), v_other) != 0) {
          std::fprintf(stderr, "unconfigured v_other defaults mismatch\n");
          return 1;
        }
        return 0;
      },
      kChildArgv);

  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;
}

TEST(ChunkspecTest, VarChunkspecRejectsInvalidInputs) {
  const CliResult res = RunCliInForkedChild(
      [](int, char**) -> int {
        ncdump_stub_reset_globals();
        chunkspecinit();

        ScopedNcFile nc;
        if (nc_create("chunkspec_varerr.nc", NC_NETCDF4 | NC_DISKLESS,
                      nc.ptr()) != NC_NOERR) {
          return 1;
        }
        int xid = -1;
        int yid = -1;
        if (nc_def_dim(nc.get(), "x", 4, &xid) != NC_NOERR ||
            nc_def_dim(nc.get(), "y", 6, &yid) != NC_NOERR) {
          return 1;
        }
        const std::array<int, 2> dims2 = {xid, yid};
        int vid = -1;
        if (nc_def_var(nc.get(), "v", NC_INT, 2, dims2.data(), &vid) !=
                NC_NOERR ||
            nc_enddef(nc.get()) != NC_NOERR) {
          return 1;
        }

        std::string too_many_ranks = "v:";
        for (int i = 0; i <= NC_MAX_VAR_DIMS; ++i) {
          if (i > 0) {
            too_many_ranks += ',';
          }
          too_many_ranks += '1';
        }

        struct ErrCase {
          const char* spec;
          int expected;
        };
        const std::array<ErrCase, 7> cases = {{
            {"missing:1,1", NC_ENOTVAR},
            {"v:2,bad", NC_EINVAL},
            {"v:2,,3", NC_EINVAL},
            {"v:2", NC_EINVAL},
            {"v:2,2,2", NC_EINVAL},
            {"v:5,2", NC_EBADCHUNK},
            {too_many_ranks.c_str(), NC_EINVAL},
        }};
        for (const ErrCase& tc : cases) {
          const int actual = chunkspec_parse(nc.get(), tc.spec);
          if (actual != tc.expected) {
            std::fprintf(stderr, "spec expected %d, got %d\n", tc.expected,
                         actual);
            return 1;
          }
          if (varchunkspec_exists(nc.get(), vid) != 0) {
            std::fprintf(stderr, "failed spec polluted varchunkspecs\n");
            return 1;
          }
        }
        return 0;
      },
      kChildArgv);

  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;
}

}  // namespace
}  // namespace netcdf
