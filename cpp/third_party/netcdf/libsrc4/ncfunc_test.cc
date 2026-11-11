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

// Unit tests for //third_party/netcdf/libsrc4/ncfunc.c.
//
// Covers NC4_inq_format() and NC4_inq_format_extended() across NC_NETCDF4 and
// NC_NETCDF4 | NC_CLASSIC_MODEL datasets, subgroups, optional NULL
// out-pointers, in-memory and POSIX read/write open modes, and invalid
// file/group IDs.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libsrc4/ncfunc.c

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

namespace netcdf {
namespace {

TEST(NcfuncTest, InqFormatReportsNetcdf4AndClassicModelAcrossGroupsAndReopen) {
  struct FormatCase {
    const char* label;
    int cmode;
    int expected_format;
  };
  constexpr FormatCase kCases[] = {
      {"netcdf4", NC_NETCDF4, NC_FORMAT_NETCDF4},
      {"netcdf4_classic", NC_NETCDF4 | NC_CLASSIC_MODEL,
       NC_FORMAT_NETCDF4_CLASSIC},
  };

  for (const FormatCase& tc : kCases) {
    SCOPED_TRACE(tc.label);
    ScopedMemio memio;
    {
      ScopedNcFile writer;
      ASSERT_EQ(NC_NOERR, nc_create_mem(tc.label, tc.cmode, 0, writer.ptr()));

      // NULL formatp must be tolerated and return NC_NOERR.
      EXPECT_EQ(NC_NOERR, nc_inq_format(writer, nullptr));

      int format = -1;
      ASSERT_EQ(NC_NOERR, nc_inq_format(writer, &format));
      EXPECT_EQ(tc.expected_format, format);

      if ((tc.cmode & NC_CLASSIC_MODEL) == 0) {
        int subid = -1;
        ASSERT_EQ(NC_NOERR, nc_def_grp(writer, "sub", &subid));
        int sub_format = -1;
        ASSERT_EQ(NC_NOERR, nc_inq_format(subid, &sub_format));
        EXPECT_EQ(tc.expected_format, sub_format);
      }

      ASSERT_EQ(NC_NOERR, nc_close_memio(writer.release(), memio.get()));
    }

    ScopedNcFile reader;
    ASSERT_EQ(NC_NOERR, nc_open_mem(tc.label, NC_NOWRITE, memio.size(),
                                    memio.get()->memory, reader.ptr()));
    int reopened_format = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_format(reader, &reopened_format));
    EXPECT_EQ(tc.expected_format, reopened_format);
  }
}

TEST(NcfuncTest,
     InqFormatExtendedReportsTrueFormatAndModeWithOptionalPointers) {
  struct ModeCase {
    const char* label;
    int cmode;
  };
  constexpr ModeCase kCases[] = {
      {"netcdf4_diskless", NC_NETCDF4 | NC_DISKLESS},
      {"netcdf4_classic_diskless", NC_NETCDF4 | NC_CLASSIC_MODEL | NC_DISKLESS},
  };

  for (const ModeCase& tc : kCases) {
    SCOPED_TRACE(tc.label);
    ScopedNcFile file;
    ASSERT_EQ(NC_NOERR, nc_create(tc.label, tc.cmode, file.ptr()));

    int format = -1;
    int mode = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_format_extended(file, &format, &mode));
    EXPECT_EQ(NC_FORMATX_NC_HDF5, format);
    EXPECT_EQ(tc.cmode, mode & tc.cmode);

    // Verify independent NULL pointer branches in NC4_inq_format_extended().
    int only_mode = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_format_extended(file, nullptr, &only_mode));
    EXPECT_EQ(tc.cmode, only_mode & tc.cmode);

    int only_format = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_format_extended(file, &only_format, nullptr));
    EXPECT_EQ(NC_FORMATX_NC_HDF5, only_format);

    EXPECT_EQ(NC_NOERR, nc_inq_format_extended(file, nullptr, nullptr));

    if ((tc.cmode & NC_CLASSIC_MODEL) == 0) {
      int grpid = -1;
      ASSERT_EQ(NC_NOERR, nc_def_grp(file, "nested", &grpid));
      int grp_format = -1;
      int grp_mode = 0;
      ASSERT_EQ(NC_NOERR,
                nc_inq_format_extended(grpid, &grp_format, &grp_mode));
      EXPECT_EQ(NC_FORMATX_NC_HDF5, grp_format);
      EXPECT_EQ(NC_NETCDF4, grp_mode & NC_NETCDF4);
    }
  }
}

TEST(NcfuncTest, InqFormatExtendedReflectsPosixReadOnlyAndReadWriteOpenModes) {
  TempFile nc_file("ncfunc_modes.nc");
  {
    ScopedNcFile creator;
    ASSERT_EQ(NC_NOERR, nc_create(nc_file.c_str(), NC_NETCDF4 | NC_CLOBBER,
                                  creator.ptr()));
    ASSERT_EQ(NC_NOERR, nc_enddef(creator));
  }

  {
    ScopedNcFile ro_file;
    ASSERT_EQ(NC_NOERR, nc_open(nc_file.c_str(), NC_NOWRITE, ro_file.ptr()));
    int format = -1;
    int mode = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_format_extended(ro_file, &format, &mode));
    EXPECT_EQ(NC_FORMATX_NC_HDF5, format);
    EXPECT_NE(0, mode & NC_NETCDF4);
    EXPECT_EQ(0, mode & NC_WRITE);
  }

  {
    ScopedNcFile rw_file;
    ASSERT_EQ(NC_NOERR, nc_open(nc_file.c_str(), NC_WRITE, rw_file.ptr()));
    int format = -1;
    int mode = 0;
    ASSERT_EQ(NC_NOERR, nc_inq_format_extended(rw_file, &format, &mode));
    EXPECT_EQ(NC_FORMATX_NC_HDF5, format);
    EXPECT_NE(0, mode & NC_NETCDF4);
    EXPECT_EQ(NC_WRITE, mode & NC_WRITE);
  }
}

TEST(NcfuncTest, InqFormatAndExtendedRejectInvalidFileAndGroupIds) {
  int format = -1;
  int mode = 0;
  EXPECT_EQ(NC_EBADID, nc_inq_format(-1, &format));
  EXPECT_EQ(NC_EBADID, nc_inq_format_extended(-1, &format, &mode));

  // Passing an open HDF5 root ncid OR'd with an unused 16-bit group slot passes
  // top-level NC_check_id() and exercises nc4_find_nc_grp_h5() failure inside
  // NC4_inq_format() and NC4_inq_format_extended().
  ScopedNcFile file;
  ASSERT_EQ(NC_NOERR, nc_create("ncfunc_bad_grp.nc", NC_NETCDF4 | NC_DISKLESS,
                                file.ptr()));
  const int bogus_grpid = file.get() | 0x7FFF;
  EXPECT_EQ(NC_EBADID, nc_inq_format(bogus_grpid, &format));
  EXPECT_EQ(NC_EBADID, nc_inq_format_extended(bogus_grpid, &format, &mode));
}

}  // namespace
}  // namespace netcdf
