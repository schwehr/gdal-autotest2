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

// Unit tests for //third_party/netcdf/libdispatch/derror.c.
//
// Covers nc_inq_libvers() version formatting/stability and nc_strerror()
// mapping across all netCDF status codes, aliases, positive system errnos,
// and unmapped default values.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/derror.c

#include <cerrno>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
// clang-format on

namespace netcdf {
namespace {

TEST(DerrorTest, InqLibversMatchesPackageVersionAndIsStable) {
  const char* vers1 = nc_inq_libvers();
  const char* vers2 = nc_inq_libvers();
  ASSERT_NE(vers1, nullptr);
  EXPECT_EQ(vers1, vers2);

  const std::string_view sv(vers1);
  EXPECT_THAT(sv, ::testing::StartsWith(PACKAGE_VERSION " of "));
  EXPECT_THAT(sv, ::testing::EndsWith(" $"));
}

TEST(DerrorTest, StrerrorMapsPositiveSystemErrnos) {
  constexpr int kErrnos[] = {
      EPERM, ENOENT, EIO, EBADF, ENOMEM, EACCES, EEXIST, ENOTDIR, EINVAL,
  };
  for (int err : kErrnos) {
    SCOPED_TRACE(err);
    ASSERT_GT(err, 0);
    ASSERT_TRUE(NC_ISSYSERR(err));
    const char* msg = nc_strerror(err);
    ASSERT_NE(msg, nullptr);
    const std::string msg_copy(msg);
    EXPECT_EQ(std::strerror(err), msg_copy);
    EXPECT_FALSE(msg_copy.empty());
  }

  // Positive value outside standard errno table still delegates to strerror().
  constexpr int kLargePositiveErr = 999999;
  ASSERT_TRUE(NC_ISSYSERR(kLargePositiveErr));
  const char* large_msg = nc_strerror(kLargePositiveErr);
  ASSERT_NE(large_msg, nullptr);
  const std::string large_msg_copy(large_msg);
  EXPECT_EQ(std::strerror(kLargePositiveErr), large_msg_copy);
}

TEST(DerrorTest, StrerrorMapsAllNetcdfStatusCodesToExpectedMessages) {
  struct ErrorCase {
    int code;
    const char* name;
    const char* expected;
  };
  constexpr ErrorCase kCases[] = {
      {NC_NOERR, "NC_NOERR", "No error"},
      {NC_EBADID, "NC_EBADID", "NetCDF: Not a valid ID"},
      {NC_ENFILE, "NC_ENFILE", "NetCDF: Too many files open"},
      {NC_EEXIST, "NC_EEXIST", "NetCDF: File exists && NC_NOCLOBBER"},
      {NC_EINVAL, "NC_EINVAL", "NetCDF: Invalid argument"},
      {NC_EPERM, "NC_EPERM", "NetCDF: Write to read only"},
      {NC_ENOTINDEFINE, "NC_ENOTINDEFINE",
       "NetCDF: Operation not allowed in data mode"},
      {NC_EINDEFINE, "NC_EINDEFINE",
       "NetCDF: Operation not allowed in define mode"},
      {NC_EINVALCOORDS, "NC_EINVALCOORDS",
       "NetCDF: Index exceeds dimension bound"},
      {NC_EMAXDIMS, "NC_EMAXDIMS", "NetCDF: NC_MAX_DIMS exceeded"},
      {NC_ENAMEINUSE, "NC_ENAMEINUSE", "NetCDF: String match to name in use"},
      {NC_ENOTATT, "NC_ENOTATT", "NetCDF: Attribute not found"},
      {NC_EMAXATTS, "NC_EMAXATTS", "NetCDF: NC_MAX_ATTRS exceeded"},
      {NC_EBADTYPE, "NC_EBADTYPE",
       "NetCDF: Not a valid data type or " NC_FillValue " type mismatch"},
      {NC_EBADDIM, "NC_EBADDIM", "NetCDF: Invalid dimension ID or name"},
      {NC_EUNLIMPOS, "NC_EUNLIMPOS", "NetCDF: NC_UNLIMITED in the wrong index"},
      {NC_EMAXVARS, "NC_EMAXVARS", "NetCDF: NC_MAX_VARS exceeded"},
      {NC_ENOTVAR, "NC_ENOTVAR", "NetCDF: Variable not found"},
      {NC_EGLOBAL, "NC_EGLOBAL",
       "NetCDF: Action prohibited on NC_GLOBAL varid"},
      {NC_ENOTNC, "NC_ENOTNC", "NetCDF: Unknown file format"},
      {NC_ESTS, "NC_ESTS", "NetCDF: In Fortran, string too short"},
      {NC_EMAXNAME, "NC_EMAXNAME", "NetCDF: NC_MAX_NAME exceeded"},
      {NC_EUNLIMIT, "NC_EUNLIMIT", "NetCDF: NC_UNLIMITED size already in use"},
      {NC_ENORECVARS, "NC_ENORECVARS",
       "NetCDF: nc_rec op when there are no record vars"},
      {NC_ECHAR, "NC_ECHAR",
       "NetCDF: Attempt to convert between text & numbers"},
      {NC_EEDGE, "NC_EEDGE", "NetCDF: Start+count exceeds dimension bound"},
      {NC_ESTRIDE, "NC_ESTRIDE", "NetCDF: Illegal stride"},
      {NC_EBADNAME, "NC_EBADNAME", "NetCDF: Name contains illegal characters"},
      {NC_ERANGE, "NC_ERANGE", "NetCDF: Numeric conversion not representable"},
      {NC_ENOMEM, "NC_ENOMEM", "NetCDF: Memory allocation (malloc) failure"},
      {NC_EVARSIZE, "NC_EVARSIZE",
       "NetCDF: One or more variable sizes violate format constraints"},
      {NC_EDIMSIZE, "NC_EDIMSIZE", "NetCDF: Invalid dimension size"},
      {NC_ETRUNC, "NC_ETRUNC",
       "NetCDF: File likely truncated or possibly corrupted"},
      {NC_EAXISTYPE, "NC_EAXISTYPE", "NetCDF: Illegal axis type"},
      {NC_EDAP, "NC_EDAP", "NetCDF: DAP failure"},
      {NC_ECURL, "NC_ECURL", "NetCDF: libcurl failure"},
      {NC_EIO, "NC_EIO", "NetCDF: I/O failure"},
      {NC_ENODATA, "NC_ENODATA", "NetCDF: Variable has no data"},
      {NC_EDAPSVC, "NC_EDAPSVC", "NetCDF: DAP server error"},
      {NC_EDAS, "NC_EDAS", "NetCDF: Malformed or inaccessible DAP DAS"},
      {NC_EDDS, "NC_EDDS",
       "NetCDF: Malformed or inaccessible DAP2 DDS or DAP4 DMR response"},
      {NC_EDATADDS, "NC_EDATADDS",
       "NetCDF: Malformed or inaccessible DAP2 DATADDS or DAP4 DAP response"},
      {NC_EDAPURL, "NC_EDAPURL", "NetCDF: Malformed URL"},
      {NC_EDAPCONSTRAINT, "NC_EDAPCONSTRAINT",
       "NetCDF: Malformed or unexpected Constraint"},
      {NC_ETRANSLATION, "NC_ETRANSLATION", "NetCDF: Untranslatable construct"},
      {NC_EACCESS, "NC_EACCESS", "NetCDF: Access failure"},
      {NC_EAUTH, "NC_EAUTH", "NetCDF: Authorization failure"},
      {NC_ENOTFOUND, "NC_ENOTFOUND", "NetCDF: file not found"},
      {NC_ECANTREMOVE, "NC_ECANTREMOVE", "NetCDF: cannot delete file"},
      {NC_EINTERNAL, "NC_EINTERNAL",
       "NetCDF: internal library error; Please contact Unidata support"},
      {NC_EPNETCDF, "NC_EPNETCDF", "NetCDF: PnetCDF error"},
      {NC_EHDFERR, "NC_EHDFERR", "NetCDF: HDF error"},
      {NC_ECANTREAD, "NC_ECANTREAD", "NetCDF: Can't read file"},
      {NC_ECANTWRITE, "NC_ECANTWRITE", "NetCDF: Can't write file"},
      {NC_ECANTCREATE, "NC_ECANTCREATE", "NetCDF: Can't create file"},
      {NC_EFILEMETA, "NC_EFILEMETA", "NetCDF: Can't add HDF5 file metadata"},
      {NC_EDIMMETA, "NC_EDIMMETA", "NetCDF: Can't define dimensional metadata"},
      {NC_EATTMETA, "NC_EATTMETA", "NetCDF: Can't open HDF5 attribute"},
      {NC_EVARMETA, "NC_EVARMETA", "NetCDF: Problem with variable metadata."},
      {NC_ENOCOMPOUND, "NC_ENOCOMPOUND",
       "NetCDF: Can't create HDF5 compound type"},
      {NC_EATTEXISTS, "NC_EATTEXISTS",
       "NetCDF: Attempt to create attribute that already exists"},
      {NC_ENOTNC4, "NC_ENOTNC4",
       "NetCDF: Attempting netcdf-4 operation on netcdf-3 file"},
      {NC_ESTRICTNC3, "NC_ESTRICTNC3",
       "NetCDF: Attempting netcdf-4 operation on strict nc3 netcdf-4 file"},
      {NC_ENOTNC3, "NC_ENOTNC3",
       "NetCDF: Attempting netcdf-3 operation on netcdf-4 file"},
      {NC_ENOPAR, "NC_ENOPAR",
       "NetCDF: Parallel operation on file opened for non-parallel access"},
      {NC_EPARINIT, "NC_EPARINIT",
       "NetCDF: Error initializing for parallel access"},
      {NC_EBADGRPID, "NC_EBADGRPID", "NetCDF: Bad group ID"},
      {NC_EBADTYPID, "NC_EBADTYPID", "NetCDF: Bad type ID"},
      {NC_ETYPDEFINED, "NC_ETYPDEFINED",
       "NetCDF: Type has already been defined and may not be edited"},
      {NC_EBADFIELD, "NC_EBADFIELD", "NetCDF: Bad field ID"},
      {NC_EBADCLASS, "NC_EBADCLASS", "NetCDF: Bad class"},
      {NC_EMAPTYPE, "NC_EMAPTYPE",
       "NetCDF: Mapped access for atomic types only"},
      {NC_ELATEFILL, "NC_ELATEFILL",
       "NetCDF: Attempt to define fill value when data already exists."},
      {NC_ELATEDEF, "NC_ELATEDEF",
       "NetCDF: Attempt to define var properties, like deflate, after enddef."},
      {NC_EDIMSCALE, "NC_EDIMSCALE", "NetCDF: Problem with HDF5 dimscales."},
      {NC_ENOGRP, "NC_ENOGRP", "NetCDF: No group found."},
      {NC_ESTORAGE, "NC_ESTORAGE",
       "NetCDF: Cannot specify both contiguous and chunking."},
      {NC_EBADCHUNK, "NC_EBADCHUNK", "NetCDF: Bad chunk sizes."},
      {NC_ENOTBUILT, "NC_ENOTBUILT",
       "NetCDF: Attempt to use feature that was not turned on "
       "when netCDF was built."},
      {NC_EDISKLESS, "NC_EDISKLESS", "NetCDF: Error in using diskless access"},
      {NC_ECANTEXTEND, "NC_ECANTEXTEND",
       "NetCDF: Attempt to extend dataset during NC_INDEPENDENT I/O operation. "
       "Use nc_var_par_access to set mode NC_COLLECTIVE before extending "
       "variable."},
      {NC_EMPI, "NC_EMPI", "NetCDF: MPI operation failed."},
      {NC_EFILTER, "NC_EFILTER",
       "NetCDF: Filter error: bad id or parameters or duplicate filter"},
      {NC_ERCFILE, "NC_ERCFILE", "NetCDF: RC File Failure."},
      {NC_ENULLPAD, "NC_ENULLPAD",
       "NetCDF: File fails strict Null-Byte Header check."},
      {NC_EINMEMORY, "NC_EINMEMORY",
       "NetCDF: In-memory File operation failed."},
      {NC_ENOFILTER, "NC_ENOFILTER",
       "NetCDF: Filter error: undefined filter encountered"},
      {NC_ENCZARR, "NC_ENCZARR", "NetCDF: NCZarr error"},
      {NC_ES3, "NC_ES3", "NetCDF: S3 error"},
      {NC_EEMPTY, "NC_EEMPTY", "NetCDF: Attempt to read empty NCZarr map key"},
      {NC_EOBJECT, "NC_EOBJECT",
       "NetCDF: Some object exists when it should not"},
      {NC_ENOOBJECT, "NC_ENOOBJECT", "NetCDF: Some object not found"},
      {NC_EPLUGIN, "NC_EPLUGIN",
       "NetCDF: Unclassified failure in accessing a dynamically loaded plugin"},
  };

  std::unordered_set<std::string> seen_messages;
  for (const ErrorCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const char* msg = nc_strerror(tc.code);
    ASSERT_NE(msg, nullptr);
    EXPECT_STREQ(tc.expected, msg);
    EXPECT_STRNE("Unknown Error", msg);
    EXPECT_TRUE(seen_messages.insert(msg).second)
        << "Duplicate message for " << tc.name;
  }

  // Verify backward-compatibility and protocol error code aliases.
  EXPECT_STREQ(nc_strerror(NC_EMAXNAME), nc_strerror(NC_ENTOOL));
  EXPECT_STREQ(nc_strerror(NC_EDDS), nc_strerror(NC_EDMR));
  EXPECT_STREQ(nc_strerror(NC_EDATADDS), nc_strerror(NC_EDATADAP));
  EXPECT_STREQ(nc_strerror(NC_EDAPURL), nc_strerror(NC_EURL));
  EXPECT_STREQ(nc_strerror(NC_EDAPCONSTRAINT), nc_strerror(NC_ECONSTRAINT));
}

TEST(DerrorTest, StrerrorReturnsUnknownErrorForUnmappedNegativeValues) {
  constexpr int kUnknownCodes[] = {
      NC2_ERR,     -2,           NC_SYSERR, NC_EXDR,
      -79,         -89,          -94,       NC4_FIRST_ERROR,
      NC_ENOTZARR, NC_EZARRMETA, -9999,     std::numeric_limits<int>::min(),
  };
  for (int code : kUnknownCodes) {
    SCOPED_TRACE(code);
    EXPECT_FALSE(NC_ISSYSERR(code));
    const char* msg = nc_strerror(code);
    ASSERT_NE(msg, nullptr);
    EXPECT_STREQ("Unknown Error", msg);
  }
}

}  // namespace
}  // namespace netcdf
