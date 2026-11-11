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

// Unit tests for //third_party/netcdf/libhdf5/nc4info.c (provenance metadata).
//
// Covers global provenance initialization/finalization, new/read/write
// provenance lifecycle on netCDF-4 and raw HDF5 files, malformed or missing
// `_NCProperties` attributes, error paths, and ncprintprovenance/clear helpers.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libhdf5/nc4info.c

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/libhdf5/nc4info_shim.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

namespace netcdf {
namespace {

using ::testing::HasSubstr;

constexpr char kNcPropertiesAttr[] = "_NCProperties";
constexpr char kSuperblockAttr[] = "_SuperblockVersion";
constexpr char kIsNetcdf4Attr[] = "_IsNetcdf4";

TEST(Nc4InfoTest, Netcdf4FileWritesAndReadsProvenanceSpecialAttributes) {
  ScopedNcFile writer;
  ASSERT_EQ(NC_NOERR,
            nc_create_mem("nc4info_prov.nc", NC_NETCDF4, 0, writer.ptr()));
  constexpr int kAttrVal = 7;
  ASSERT_EQ(NC_NOERR,
            nc_put_att_int(writer, NC_GLOBAL, "marker", NC_INT, 1, &kAttrVal));
  // Calling sync twice exercises the existing-_NCProperties no-overwrite path.
  ASSERT_EQ(NC_NOERR, nc_sync(writer));
  ASSERT_EQ(NC_NOERR, nc_sync(writer));

  ScopedMemio memio;
  ASSERT_EQ(NC_NOERR, nc_close_memio(writer.release(), memio.get()));
  ASSERT_NE(memio.get()->memory, nullptr);
  ASSERT_GT(memio.size(), 0U);

  ScopedNcFile reader;
  ASSERT_EQ(NC_NOERR,
            nc_open_mem("nc4info_prov_ro.nc", NC_NOWRITE, memio.size(),
                        memio.get()->memory, reader.ptr()));

  nc_type xtype = NC_NAT;
  size_t len = 0;
  ASSERT_EQ(NC_NOERR,
            nc_inq_att(reader, NC_GLOBAL, kNcPropertiesAttr, &xtype, &len));
  EXPECT_EQ(NC_CHAR, xtype);
  ASSERT_GT(len, 0U);

  // nc4_get_att_special writes len + 1 bytes (including NUL terminator).
  std::vector<char> props_buf(len + 1, '\0');
  ASSERT_EQ(NC_NOERR, nc_get_att_text(reader, NC_GLOBAL, kNcPropertiesAttr,
                                      props_buf.data()));
  const std::string props(props_buf.data());
  EXPECT_THAT(props, HasSubstr("version=2"));
  EXPECT_THAT(props, HasSubstr("netcdf="));
  EXPECT_THAT(props, HasSubstr("hdf5="));

  int is_nc4 = -1;
  int superblock = -1;
  ASSERT_EQ(NC_NOERR,
            nc_get_att_int(reader, NC_GLOBAL, kIsNetcdf4Attr, &is_nc4));
  ASSERT_EQ(NC_NOERR,
            nc_get_att_int(reader, NC_GLOBAL, kSuperblockAttr, &superblock));
  EXPECT_EQ(1, is_nc4);
  EXPECT_GE(superblock, 0);
}

TEST(Nc4InfoTest, ReopenWritableNetcdf4FileSkipsOverwritingExistingProps) {
  TempFile file("nc4info_reopen_rw.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(file.c_str(), NC_NETCDF4, nc.ptr()));
    ASSERT_EQ(NC_NOERR, nc.close());
  }

  {
    ScopedNcFile rw;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_WRITE, rw.ptr()));
    constexpr int kVal = 123;
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(rw, NC_GLOBAL, "extra", NC_INT, 1, &kVal));
    ASSERT_EQ(NC_NOERR, rw.close());
  }

  ScopedNcFile ro;
  ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_NOWRITE, ro.ptr()));
  size_t len = 0;
  ASSERT_EQ(NC_NOERR, nc_inq_attlen(ro, NC_GLOBAL, kNcPropertiesAttr, &len));
  std::vector<char> props_buf(len + 1, '\0');
  ASSERT_EQ(NC_NOERR, nc_get_att_text(ro, NC_GLOBAL, kNcPropertiesAttr,
                                      props_buf.data()));
  const std::string props(props_buf.data());
  EXPECT_THAT(props, HasSubstr("version=2"));
}

TEST(Nc4InfoTest, RawHdf5WithoutNcPropertiesLeavesAttrAbsentOnWrite) {
  TempFile file("nc4info_raw_absent.h5");
  ASSERT_EQ(NC_NOERR, Nc4InfoShimCreateRawHdf5File(file.c_str(), 0, nullptr));

  {
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_NOWRITE, ro.ptr()));
    nc_type xtype = NC_NAT;
    size_t len = 0;
    EXPECT_EQ(NC_ENOTATT,
              nc_inq_att(ro, NC_GLOBAL, kNcPropertiesAttr, &xtype, &len));
  }

  {
    ScopedNcFile rw;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_WRITE, rw.ptr()));
    constexpr int kTag = 9;
    ASSERT_EQ(NC_NOERR,
              nc_put_att_int(rw, NC_GLOBAL, "user_tag", NC_INT, 1, &kTag));
    ASSERT_EQ(NC_NOERR, nc_sync(rw));
    ASSERT_EQ(NC_NOERR, rw.close());
  }

  int exists = -1;
  ASSERT_EQ(NC_NOERR, Nc4InfoShimHasRawNcPropertiesAttr(file.c_str(), &exists));
  EXPECT_EQ(0, exists);

  {
    ScopedNcFile ro2;
    ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_NOWRITE, ro2.ptr()));
    int tag_out = 0;
    ASSERT_EQ(NC_NOERR, nc_get_att_int(ro2, NC_GLOBAL, "user_tag", &tag_out));
    EXPECT_EQ(9, tag_out);
    nc_type xtype = NC_NAT;
    size_t len = 0;
    EXPECT_EQ(NC_ENOTATT,
              nc_inq_att(ro2, NC_GLOBAL, kNcPropertiesAttr, &xtype, &len));
  }
}

TEST(Nc4InfoTest, RawHdf5WithNonStringNcPropertiesIgnoresInvalidAttr) {
  TempFile file("nc4info_raw_int_prop.h5");
  ASSERT_EQ(NC_NOERR, Nc4InfoShimCreateRawHdf5File(file.c_str(), 1, nullptr));

  ScopedNcFile ro;
  ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_NOWRITE, ro.ptr()));
  nc_type xtype = NC_NAT;
  size_t len = 0;
  EXPECT_EQ(NC_ENOTATT,
            nc_inq_att(ro, NC_GLOBAL, kNcPropertiesAttr, &xtype, &len));
}

TEST(Nc4InfoTest, RawHdf5WithValidStringNcPropertiesPreservesValue) {
  TempFile file("nc4info_raw_str_prop.h5");
  constexpr char kCustomProps[] = "version=1|netcdf=4.5.0|hdf5=1.10.0";
  ASSERT_EQ(NC_NOERR,
            Nc4InfoShimCreateRawHdf5File(file.c_str(), 2, kCustomProps));

  ScopedNcFile ro;
  ASSERT_EQ(NC_NOERR, nc_open(file.c_str(), NC_NOWRITE, ro.ptr()));
  nc_type xtype = NC_NAT;
  size_t len = 0;
  ASSERT_EQ(NC_NOERR,
            nc_inq_att(ro, NC_GLOBAL, kNcPropertiesAttr, &xtype, &len));
  EXPECT_EQ(NC_CHAR, xtype);
  ASSERT_EQ(std::string_view(kCustomProps).size(), len);

  std::vector<char> read_back_buf(len + 1, '\0');
  ASSERT_EQ(NC_NOERR, nc_get_att_text(ro, NC_GLOBAL, kNcPropertiesAttr,
                                      read_back_buf.data()));
  EXPECT_EQ(kCustomProps, std::string_view(read_back_buf.data()));
}

TEST(Nc4InfoTest, ProvenanceInitIsIdempotent) {
  EXPECT_EQ(NC_NOERR, Nc4InfoShimProvenanceInit());
  EXPECT_EQ(NC_NOERR, Nc4InfoShimProvenanceInit());
}

TEST(Nc4InfoTest, NewAndReadProvenanceHandleInvalidHdf5IdGracefully) {
  EXPECT_EQ(NC_NOERR, Nc4InfoShimNewProvenanceBadHdfid());
  EXPECT_EQ(NC_NOERR, Nc4InfoShimReadProvenanceBadHdfid());
}

TEST(Nc4InfoTest, WriteProvenanceHandlesReadOnlyBadGroupAndEmptyString) {
  TempFile empty_prop_file("nc4info_empty_props.h5");
  EXPECT_EQ(NC_EPERM, Nc4InfoShimWriteProvenanceError(nullptr, 1, 0, 0));
  EXPECT_EQ(NC_EFILEMETA, Nc4InfoShimWriteProvenanceError(nullptr, 0, 1, 0));
  EXPECT_EQ(NC_EFILEMETA,
            Nc4InfoShimWriteProvenanceError(empty_prop_file.c_str(), 0, 0, 1));
}

TEST(Nc4InfoTest, ClearProvenanceHandlesNullZeroedAndAllocatedStructs) {
  EXPECT_EQ(NC_NOERR, Nc4InfoShimTestClearProvenance());
}

TEST(Nc4InfoTest, NewProvenanceAfterFinalizeInIsolatedChild) {
  TempFile file("nc4info_post_finalize.h5");
  EXPECT_TRUE(RunInForkedChild([&] {
    int has_props = -1;
    int version = -1;
    int superblock = -1;
    const int stat = Nc4InfoShimNewProvenanceAfterFinalize(
        file.c_str(), &has_props, &version, &superblock);
    return stat == NC_NOERR && has_props == 0 && version == 0 &&
           superblock >= 0;
  }));

  int exists = -1;
  ASSERT_EQ(NC_NOERR, Nc4InfoShimHasRawNcPropertiesAttr(file.c_str(), &exists));
  EXPECT_EQ(0, exists);
}

TEST(Nc4InfoTest, NcprintprovenanceFormatsNonNullAndNullProperties) {
  const CliResult res = RunCliInForkedChild(
      [](int /*argc*/, char** /*argv*/) -> int {
        const int r1 =
            Nc4InfoShimPrintProvenance(2, 0, "version=2,netcdf=4.10.1");
        const int r2 = Nc4InfoShimPrintProvenance(1, 3, nullptr);
        return (r1 == NC_NOERR && r2 == NC_NOERR) ? 0 : 1;
      },
      {});
  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code);
  EXPECT_THAT(res.stderr_output,
              HasSubstr("version=2 superblockversion=0 "
                        "ncproperties=|version=2,netcdf=4.10.1|"));
  EXPECT_THAT(res.stderr_output,
              HasSubstr("version=1 superblockversion=3 ncproperties=||"));
}

}  // namespace
}  // namespace netcdf
