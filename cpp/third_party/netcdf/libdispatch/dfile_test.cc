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

// Unit tests for //third_party/netcdf/libdispatch/dfile.c.
//
// Covers user-defined format (UDF) registration and lookup across all 10
// slots, create/open wrappers and mode validation, in-memory and disk file
// lifecycles, define/data mode transitions, inquiry/fill/type APIs, and
// pseudo-fd / auto-initialization behavior.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/dfile.c

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <fstream>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
extern "C" {
#include "third_party/netcdf/include/ncdispatch.h"
int nc_initialize(void);
int nc_finalize(void);
}  // extern "C"
// clang-format on

namespace netcdf {
namespace {

struct UdfSlotDescriptor {
  int mode_flag;
  int formatx;
  const char* mode_tag;
};

constexpr std::array<UdfSlotDescriptor, NC_MAX_UDF_FORMATS> kUdfSlots = {{
    {NC_UDF0, NC_FORMATX_UDF0, "udf0"},
    {NC_UDF1, NC_FORMATX_UDF1, "udf1"},
    {NC_UDF2, NC_FORMATX_UDF2, "udf2"},
    {NC_UDF3, NC_FORMATX_UDF3, "udf3"},
    {NC_UDF4, NC_FORMATX_UDF4, "udf4"},
    {NC_UDF5, NC_FORMATX_UDF5, "udf5"},
    {NC_UDF6, NC_FORMATX_UDF6, "udf6"},
    {NC_UDF7, NC_FORMATX_UDF7, "udf7"},
    {NC_UDF8, NC_FORMATX_UDF8, "udf8"},
    {NC_UDF9, NC_FORMATX_UDF9, "udf9"},
}};

// Snapshots and restores the process-wide UDF dispatch and magic-number tables
// so UDF tests remain hermetic across --gunit_shuffle and --gunit_repeat.
class ScopedUdfState {
 public:
  ScopedUdfState() {
    for (int i = 0; i < NC_MAX_UDF_FORMATS; ++i) {
      saved_tables_[i] = UDF_dispatch_tables[i];
      std::memcpy(saved_magics_[i], UDF_magic_numbers[i],
                  sizeof(saved_magics_[i]));
    }
  }

  ~ScopedUdfState() {
    for (int i = 0; i < NC_MAX_UDF_FORMATS; ++i) {
      UDF_dispatch_tables[i] = saved_tables_[i];
      std::memcpy(UDF_magic_numbers[i], saved_magics_[i],
                  sizeof(UDF_magic_numbers[i]));
    }
  }

  ScopedUdfState(const ScopedUdfState&) = delete;
  ScopedUdfState& operator=(const ScopedUdfState&) = delete;

 private:
  std::array<NC_Dispatch*, NC_MAX_UDF_FORMATS> saved_tables_{};
  char saved_magics_[NC_MAX_UDF_FORMATS][NC_MAX_MAGIC_NUMBER_LEN + 1]{};
};

// Mutable state shared with clean-room fake UDF C dispatch callbacks.
struct FakeUdfController {
  int create_status = NC_NOERR;
  int open_status = NC_NOERR;
  int fail_next_close_count = 0;
  int create_calls = 0;
  int open_calls = 0;
  int close_calls = 0;
  int abort_calls = 0;
  int last_model = NC_FORMATX_UNDEFINED;
  std::string last_path;
};

FakeUdfController& GetFakeUdfController() {
  static auto* controller = new FakeUdfController();
  return *controller;
}

class ScopedFakeUdfControllerReset {
 public:
  ScopedFakeUdfControllerReset() {
    GetFakeUdfController() = FakeUdfController{};
  }
  ~ScopedFakeUdfControllerReset() {
    GetFakeUdfController() = FakeUdfController{};
  }
  ScopedFakeUdfControllerReset(const ScopedFakeUdfControllerReset&) = delete;
  ScopedFakeUdfControllerReset& operator=(const ScopedFakeUdfControllerReset&) =
      delete;
};

extern "C" {

int FakeUdfCreate(const char* path, int cmode, size_t initialsz, int basepe,
                  size_t* chunksizehintp, void* parameters,
                  const NC_Dispatch* table, int ncid) {
  (void)cmode;
  (void)initialsz;
  (void)basepe;
  (void)chunksizehintp;
  (void)parameters;
  (void)ncid;
  FakeUdfController& ctrl = GetFakeUdfController();
  ++ctrl.create_calls;
  ctrl.last_model = table != nullptr ? table->model : NC_FORMATX_UNDEFINED;
  ctrl.last_path = path != nullptr ? path : "";
  return ctrl.create_status;
}

int FakeUdfOpen(const char* path, int mode, int basepe, size_t* chunksizehintp,
                void* parameters, const NC_Dispatch* table, int ncid) {
  (void)mode;
  (void)basepe;
  (void)chunksizehintp;
  (void)parameters;
  (void)ncid;
  FakeUdfController& ctrl = GetFakeUdfController();
  ++ctrl.open_calls;
  ctrl.last_model = table != nullptr ? table->model : NC_FORMATX_UNDEFINED;
  ctrl.last_path = path != nullptr ? path : "";
  return ctrl.open_status;
}

int FakeUdfRedef(int ncid) {
  (void)ncid;
  return NC_NOERR;
}

int FakeUdfEnddef(int ncid, size_t h_minfree, size_t v_align, size_t v_minfree,
                  size_t r_align) {
  (void)ncid;
  (void)h_minfree;
  (void)v_align;
  (void)v_minfree;
  (void)r_align;
  return NC_NOERR;
}

int FakeUdfSync(int ncid) {
  (void)ncid;
  return NC_NOERR;
}

int FakeUdfAbort(int ncid) {
  (void)ncid;
  ++GetFakeUdfController().abort_calls;
  return NC_NOERR;
}

int FakeUdfClose(int ncid, void* params) {
  (void)ncid;
  (void)params;
  FakeUdfController& ctrl = GetFakeUdfController();
  ++ctrl.close_calls;
  if (ctrl.fail_next_close_count > 0) {
    --ctrl.fail_next_close_count;
    return NC_EHDFERR;
  }
  return NC_NOERR;
}

int FakeUdfSetFill(int ncid, int fillmode, int* old_modep) {
  (void)ncid;
  (void)fillmode;
  if (old_modep != nullptr) {
    *old_modep = NC_FILL;
  }
  return NC_NOERR;
}

int FakeUdfInqFormat(int ncid, int* formatp) {
  (void)ncid;
  if (formatp != nullptr) {
    *formatp = NC_FORMAT_CLASSIC;
  }
  return NC_NOERR;
}

int FakeUdfInqFormatExtended(int ncid, int* formatp, int* modep) {
  NC* ncp = nullptr;
  const int stat = NC_check_id(ncid, &ncp);
  if (stat != NC_NOERR) {
    return stat;
  }
  if (formatp != nullptr) {
    *formatp = ncp->dispatch->model;
  }
  if (modep != nullptr) {
    *modep = ncp->mode;
  }
  return NC_NOERR;
}

int FakeUdfInq(int ncid, int* ndimsp, int* nvarsp, int* nattsp,
               int* unlimdimidp) {
  (void)ncid;
  if (ndimsp != nullptr) {
    *ndimsp = 0;
  }
  if (nvarsp != nullptr) {
    *nvarsp = 0;
  }
  if (nattsp != nullptr) {
    *nattsp = 0;
  }
  if (unlimdimidp != nullptr) {
    *unlimdimidp = -1;
  }
  return NC_NOERR;
}

int FakeUdfInqType(int ncid, nc_type xtype, char* name, size_t* size) {
  (void)ncid;
  (void)xtype;
  (void)name;
  (void)size;
  return NC_EBADTYPE;
}

}  // extern "C"

NC_Dispatch MakeFakeUdfDispatch(int model,
                                int dispatch_version = NC_DISPATCH_VERSION) {
  NC_Dispatch table{};
  table.model = model;
  table.dispatch_version = dispatch_version;
  table.create = FakeUdfCreate;
  table.open = FakeUdfOpen;
  table.redef = FakeUdfRedef;
  table._enddef = FakeUdfEnddef;
  table.sync = FakeUdfSync;
  table.abort = FakeUdfAbort;
  table.close = FakeUdfClose;
  table.set_fill = FakeUdfSetFill;
  table.inq_format = FakeUdfInqFormat;
  table.inq_format_extended = FakeUdfInqFormatExtended;
  table.inq = FakeUdfInq;
  table.inq_type = FakeUdfInqType;
  return table;
}

TEST(DfileTest, UdfRegistrationValidatesInputsAndQueriesAllTenSlots) {
  ScopedUdfState udf_guard;
  NC_Dispatch dummy_table = MakeFakeUdfDispatch(NC_FORMATX_UDF0);
  char short_magic[] = "UDF0";
  char overlong_magic[] = "123456789";

  EXPECT_EQ(NC_EINVAL, nc_def_user_format(NC_UDF0, nullptr, short_magic));
  EXPECT_EQ(NC_EINVAL,
            nc_def_user_format(NC_UDF0, &dummy_table, overlong_magic));

  NC_Dispatch wrong_ver_table =
      MakeFakeUdfDispatch(NC_FORMATX_UDF0, NC_DISPATCH_VERSION + 1);
  EXPECT_EQ(NC_EINVAL,
            nc_def_user_format(NC_UDF0, &wrong_ver_table, short_magic));

  // Magic numbers are disallowed with classic-family mode restrictions unless
  // NC_CLASSIC_MODEL is paired with NC_NETCDF4.
  EXPECT_EQ(NC_EINVAL, nc_def_user_format(NC_UDF0 | NC_64BIT_OFFSET,
                                          &dummy_table, short_magic));
  EXPECT_EQ(NC_EINVAL, nc_def_user_format(NC_UDF0 | NC_64BIT_DATA, &dummy_table,
                                          short_magic));
  EXPECT_EQ(NC_EINVAL, nc_def_user_format(NC_UDF0 | NC_CLASSIC_MODEL,
                                          &dummy_table, short_magic));

  // Null magic is permitted alongside NC_64BIT_OFFSET, and magic is allowed
  // when NC_CLASSIC_MODEL is paired with NC_NETCDF4.
  EXPECT_EQ(NC_NOERR, nc_def_user_format(NC_UDF0 | NC_64BIT_OFFSET,
                                         &dummy_table, nullptr));
  EXPECT_EQ(NC_NOERR,
            nc_def_user_format(NC_UDF0 | NC_CLASSIC_MODEL | NC_NETCDF4,
                               &dummy_table, short_magic));

  // Invalid or ambiguous UDF mode flags must be rejected by both APIs.
  EXPECT_EQ(NC_EINVAL, nc_def_user_format(0, &dummy_table, nullptr));
  EXPECT_EQ(NC_EINVAL, nc_def_user_format(NC_NETCDF4, &dummy_table, nullptr));
  EXPECT_EQ(NC_EINVAL,
            nc_def_user_format(NC_UDF0 | NC_UDF1, &dummy_table, nullptr));

  NC_Dispatch* queried_table = &dummy_table;
  char queried_magic[NC_MAX_MAGIC_NUMBER_LEN + 1]{};
  EXPECT_EQ(NC_EINVAL, nc_inq_user_format(0, &queried_table, queried_magic));
  EXPECT_EQ(NC_EINVAL,
            nc_inq_user_format(NC_NETCDF4, &queried_table, queried_magic));
  EXPECT_EQ(NC_EINVAL, nc_inq_user_format(NC_UDF0 | NC_UDF1, &queried_table,
                                          queried_magic));

  // Register and inspect all 10 UDF slots, including an exact 8-byte magic.
  std::array<NC_Dispatch, NC_MAX_UDF_FORMATS> tables{};
  for (int i = 0; i < NC_MAX_UDF_FORMATS; ++i) {
    tables[i] = MakeFakeUdfDispatch(kUdfSlots[i].formatx);
    std::string magic_str = absl::StrCat("UDFMAG0", i);
    ASSERT_EQ(static_cast<size_t>(NC_MAX_MAGIC_NUMBER_LEN), magic_str.size());
    char magic_buf[NC_MAX_MAGIC_NUMBER_LEN + 1]{};
    std::memcpy(magic_buf, magic_str.data(), magic_str.size());

    ASSERT_EQ(NC_NOERR, nc_def_user_format(kUdfSlots[i].mode_flag, &tables[i],
                                           magic_buf));

    NC_Dispatch* out_table = nullptr;
    char out_magic[NC_MAX_MAGIC_NUMBER_LEN + 1]{};
    EXPECT_EQ(NC_NOERR, nc_inq_user_format(kUdfSlots[i].mode_flag, &out_table,
                                           out_magic));
    EXPECT_EQ(&tables[i], out_table);
    EXPECT_STREQ(magic_str.c_str(), out_magic);

    // Null output pointers are ignored cleanly.
    EXPECT_EQ(NC_NOERR,
              nc_inq_user_format(kUdfSlots[i].mode_flag, nullptr, nullptr));
    EXPECT_EQ(NC_NOERR,
              nc_inq_user_format(kUdfSlots[i].mode_flag, &out_table, nullptr));
    char magic_only[NC_MAX_MAGIC_NUMBER_LEN + 1]{};
    EXPECT_EQ(NC_NOERR,
              nc_inq_user_format(kUdfSlots[i].mode_flag, nullptr, magic_only));
    EXPECT_STREQ(magic_str.c_str(), magic_only);
  }
}

TEST(DfileTest, UdfDispatchCreateOpenCloseAndErrorPathsAcrossAllTenSlots) {
  ScopedUdfState udf_guard;
  ScopedFakeUdfControllerReset ctrl_reset;
  FakeUdfController& ctrl = GetFakeUdfController();

  // Unregistered UDF slots fail creation with NC_ENOTBUILT.
  {
    ScopedNcFile nc;
    EXPECT_EQ(NC_ENOTBUILT,
              nc_create("unreg_udf0.nc", NC_UDF0 | NC_CLOBBER, nc.ptr()));
    EXPECT_EQ(NC_ENOTBUILT,
              nc_create("unreg_udf5.nc", NC_UDF5 | NC_CLOBBER, nc.ptr()));
  }

  std::array<NC_Dispatch, NC_MAX_UDF_FORMATS> tables{};
  for (int i = 0; i < NC_MAX_UDF_FORMATS; ++i) {
    tables[i] = MakeFakeUdfDispatch(kUdfSlots[i].formatx);
    char magic_buf[NC_MAX_MAGIC_NUMBER_LEN + 1]{};
    std::string magic_str = absl::StrCat("UDFM000", i);
    std::memcpy(magic_buf, magic_str.data(), magic_str.size());
    ASSERT_EQ(NC_NOERR, nc_def_user_format(kUdfSlots[i].mode_flag, &tables[i],
                                           magic_buf));

    // Create via UDF mode flag and verify format_extended reports the slot.
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR, nc_create(absl::StrCat("slot_", i, ".udf").c_str(),
                                    kUdfSlots[i].mode_flag, nc.ptr()));
      int ext_format = NC_FORMATX_UNDEFINED;
      int ext_mode = 0;
      EXPECT_EQ(NC_NOERR,
                nc_inq_format_extended(nc.get(), &ext_format, &ext_mode));
      EXPECT_EQ(kUdfSlots[i].formatx, ext_format);
      EXPECT_NE(0, ext_mode & kUdfSlots[i].mode_flag);
      EXPECT_EQ(kUdfSlots[i].formatx, ctrl.last_model);
    }

    // Open via magic number recognition in nc_open_mem (>= 9 bytes required by
    // NC_infermodel's magic reader).
    std::array<uint8_t, 16> mem_buf{};
    std::memcpy(mem_buf.data(), magic_str.data(), magic_str.size());
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR, nc_open_mem(absl::StrCat("open_", i, ".udf").c_str(),
                                      NC_NOWRITE, mem_buf.size(),
                                      mem_buf.data(), nc.ptr()));
      int ext_format = NC_FORMATX_UNDEFINED;
      EXPECT_EQ(NC_NOERR,
                nc_inq_format_extended(nc.get(), &ext_format, nullptr));
      EXPECT_EQ(kUdfSlots[i].formatx, ext_format);
    }
  }

  // Exercise URL rewriting in NC_create via file://...#mode=netcdf-4,udf0.
  // Specifying netcdf-4 before udf0 supplies a non-zero model.format alongside
  // model.impl = NC_FORMATX_UDF0 (avoiding NC_create's model.format assert).
  {
    TempDir url_dir("dfile_udf_url");
    const std::string mode_suffix =
        absl::StrCat("#mode=netcdf-4,", kUdfSlots[0].mode_tag);
    const std::string udf_url = absl::StrCat(
        "file://", url_dir.child("via_url.udf").string(), mode_suffix);
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(udf_url.c_str(), NC_CLOBBER, nc.ptr()));
    EXPECT_EQ(kUdfSlots[0].formatx, ctrl.last_model);
    EXPECT_THAT(ctrl.last_path, ::testing::HasSubstr(mode_suffix));
  }

  // Failing dispatcher->create / dispatcher->open cleanly unwind NC list entry.
  ctrl.create_status = NC_EPERM;
  {
    ScopedNcFile nc;
    EXPECT_EQ(NC_EPERM, nc_create("fail_udf.nc", NC_UDF0, nc.ptr()));
    EXPECT_FALSE(nc.valid());
  }
  ctrl.create_status = NC_NOERR;

  ctrl.open_status = NC_EIO;
  {
    std::array<uint8_t, 16> mem_buf{};
    std::memcpy(mem_buf.data(), "UDFM0000", 8);
    ScopedNcFile nc;
    EXPECT_EQ(NC_EIO, nc_open_mem("fail_open_udf.nc", NC_NOWRITE,
                                  mem_buf.size(), mem_buf.data(), nc.ptr()));
    EXPECT_FALSE(nc.valid());
  }
  ctrl.open_status = NC_NOERR;

  // Failing dispatcher->close in nc_close() and nc_close_memio() skips
  // del_from_NCList/free_NC on failure, allowing clean RAII close afterwards.
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create("fail_close_1.udf", NC_UDF0, nc.ptr()));
    ctrl.fail_next_close_count = 1;
    EXPECT_EQ(NC_EHDFERR, nc_close(nc.get()));
    // ScopedNcFile destructor now succeeds cleanly.
  }
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create("fail_close_2.udf", NC_UDF1, nc.ptr()));
    ctrl.fail_next_close_count = 1;
    NC_memio unused_memio{};
    EXPECT_EQ(NC_EHDFERR, nc_close_memio(nc.get(), &unused_memio));
    // ScopedNcFile destructor now succeeds cleanly.
  }
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create("abort_udf.udf", NC_UDF2, nc.ptr()));
    EXPECT_EQ(NC_NOERR, nc.abort());
    EXPECT_EQ(1, ctrl.abort_calls);
  }
  EXPECT_GE(ctrl.create_calls, NC_MAX_UDF_FORMATS);
  EXPECT_GE(ctrl.open_calls, NC_MAX_UDF_FORMATS);
  EXPECT_GE(ctrl.close_calls, NC_MAX_UDF_FORMATS * 2);
}

TEST(DfileTest, CheckCreateModeRejectsConflictingFlags) {
  int ncid = -1;
  EXPECT_EQ(NC_EINVAL, nc_create("c1.nc", NC_NETCDF4 | NC_64BIT_OFFSET, &ncid));
  EXPECT_EQ(NC_EINVAL, nc_create("c2.nc", NC_NETCDF4 | NC_CDF5, &ncid));
  EXPECT_EQ(NC_EINVAL, nc_create("c3.nc", NC_64BIT_OFFSET | NC_CDF5, &ncid));
  EXPECT_EQ(NC_EDISKLESS, nc_create("c4.nc", NC_DISKLESS | NC_INMEMORY, &ncid));
  EXPECT_EQ(NC_EDISKLESS, nc_create("c5.nc", NC_DISKLESS | NC_MMAP, &ncid));
  EXPECT_EQ(NC_EINMEMORY, nc_create("c6.nc", NC_INMEMORY | NC_MMAP, &ncid));
  EXPECT_EQ(NC_EINVAL, nc_create("c7.nc", NC_MMAP | NC_NETCDF4, &ncid));
  EXPECT_EQ(NC_EINVAL, nc_create_mem("c8.nc", NC_MMAP, 0, &ncid));
  EXPECT_EQ(NC_EDISKLESS, nc_create_mem("c9.nc", NC_DISKLESS, 0, &ncid));
}

TEST(DfileTest, CreateAndOpenDiskVariantsAndPathHandling) {
  TempDir dir("dfile_disk_variants");

  // Null and blank paths (leading whitespace trims to "", which returns
  // NC_EURL in NC_infermodel's processuri).
  int bad_ncid = -1;
  EXPECT_EQ(NC_EINVAL, nc_create(nullptr, NC_CLOBBER, &bad_ncid));
  EXPECT_EQ(NC_EINVAL, nc_open(nullptr, NC_NOWRITE, &bad_ncid));
  EXPECT_EQ(NC_EURL, nc_create("   ", NC_CLOBBER, &bad_ncid));
  EXPECT_EQ(NC_EURL, nc_open("   ", NC_NOWRITE, &bad_ncid));

  // NC_open mode conflict checks.
  EXPECT_EQ(NC_EDISKLESS,
            nc_open("o1.nc", NC_DISKLESS | NC_INMEMORY, &bad_ncid));
  EXPECT_EQ(NC_EDISKLESS, nc_open("o2.nc", NC_DISKLESS | NC_MMAP, &bad_ncid));
  EXPECT_EQ(NC_EINMEMORY, nc_open("o3.nc", NC_INMEMORY | NC_MMAP, &bad_ncid));
  EXPECT_EQ(NC_EINVAL, nc_open("o4.nc", NC_MMAP | NC_NETCDF4, &bad_ncid));

  // Missing files (including UTF-8 non-ASCII lead byte and plain path).
  EXPECT_EQ(ENOENT, nc_open("\xc3\xa9_nonexistent.nc", NC_NOWRITE, &bad_ncid));
  EXPECT_EQ(ENOENT,
            nc_open(dir.child("missing.nc").c_str(), NC_NOWRITE, &bad_ncid));
  EXPECT_EQ(NC_EINVAL,
            nc_create("ftp://unsupported.invalid/a.nc", NC_CLOBBER, &bad_ncid));
  EXPECT_EQ(NC_EINVAL,
            nc_open("ftp://unsupported.invalid/a.nc", NC_NOWRITE, &bad_ncid));

  // Truncated non-netCDF file fails NC_infermodel with NC_ENOTNC.
  TempFile short_file("dfile_disk_variants/short.nc");
  {
    std::ofstream out(short_file.path(), std::ios::binary);
    out.write("CDF\x01\x00", 5);
  }
  EXPECT_EQ(NC_ENOTNC, nc_open(short_file.c_str(), NC_NOWRITE, &bad_ncid));

  // Create disk file via nc__create with leading whitespace, chunk hint, and
  // initialsz, then verify path normalization and NC_NOCLOBBER error handling.
  TempFile disk_file("dfile_disk_variants/classic_disk.nc");
  const std::string ws_disk_path = absl::StrCat(" \t", disk_file.string());
  size_t create_chunk_hint = 4096;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc__create(ws_disk_path.c_str(), NC_CLOBBER, 1024,
                                   &create_chunk_hint, nc.ptr()));
    size_t path_len = 0;
    std::string out_path(disk_file.string().size() + 16, '\0');
    EXPECT_EQ(NC_NOERR, nc_inq_path(nc.get(), &path_len, out_path.data()));
    out_path.resize(path_len);
    EXPECT_EQ(disk_file.string(), out_path);

    // Creating again with NC_NOCLOBBER fails dispatcher->create and cleans up.
    int clobber_id = -1;
    EXPECT_EQ(NC_EEXIST,
              nc_create(disk_file.c_str(), NC_NOCLOBBER, &clobber_id));
  }

  // Reopen with nc__open (with and without chunksizehintp, and leading spaces).
  {
    size_t open_chunk_hint = NC_SIZEHINT_DEFAULT;
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc__open(ws_disk_path.c_str(), NC_NOWRITE,
                                 &open_chunk_hint, nc.ptr()));
    size_t len_only = 0;
    EXPECT_EQ(NC_NOERR, nc_inq_path(nc.get(), &len_only, nullptr));
    EXPECT_EQ(disk_file.string().size(), len_only);
    std::string buf_only(len_only + 1, '\0');
    EXPECT_EQ(NC_NOERR, nc_inq_path(nc.get(), nullptr, buf_only.data()));
    EXPECT_STREQ(disk_file.c_str(), buf_only.c_str());
    EXPECT_EQ(NC_NOERR, nc_inq_path(nc.get(), nullptr, nullptr));
  }
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc__open(disk_file.c_str(), NC_NOWRITE, nullptr, nc.ptr()));
  }

  // Deprecated mp variants with basepe == 0 succeed and non-zero basepe fails.
  TempFile mp_file("dfile_disk_variants/mp_disk.nc");
  size_t mp_hint = 2048;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc__create_mp(mp_file.c_str(), NC_CLOBBER, 0, 0,
                                      &mp_hint, nc.ptr()));
  }
  {
    ScopedNcFile nc;
    EXPECT_EQ(NC_EINVAL, nc__create_mp(mp_file.c_str(), NC_CLOBBER, 0, 1,
                                       &mp_hint, nc.ptr()));
  }
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc__open_mp(mp_file.c_str(), NC_NOWRITE, 0, &mp_hint, nc.ptr()));
  }
  {
    ScopedNcFile nc;
    EXPECT_EQ(NC_EINVAL,
              nc__open_mp(mp_file.c_str(), NC_NOWRITE, 1, &mp_hint, nc.ptr()));
  }

  // Ephemeral NC_DISKLESS does not write a backing file; NC_DISKLESS|NC_PERSIST
  // writes one that can be reopened.
  TempFile ephemeral_nc("dfile_disk_variants/ephemeral.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(ephemeral_nc.c_str(),
                                  NC_DISKLESS | NC_CLOBBER, nc.ptr()));
  }
  EXPECT_FALSE(std::filesystem::exists(ephemeral_nc.path()));

  TempFile persist_nc("dfile_disk_variants/persist.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create(persist_nc.c_str(),
                        NC_DISKLESS | NC_PERSIST | NC_CLOBBER, nc.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "x", 1, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "v", NC_INT, 1, &dimid, &varid));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));
    constexpr int kVal = 42;
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc.get(), varid, &kVal));
  }
  EXPECT_TRUE(std::filesystem::exists(persist_nc.path()));
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_open(persist_nc.c_str(), NC_NOWRITE | NC_DISKLESS, nc.ptr()));
  }

  // Unbuilt formats / URL modes hit NC_create default & NC_open built checks.
  EXPECT_EQ(NC_ENOTBUILT, NC_create(disk_file.c_str(), NC_CLOBBER, 0, 0,
                                    nullptr, 1, nullptr, &bad_ncid));
  const std::string dap_url =
      absl::StrCat("file://", dir.child("dap.nc").string(), "#mode=dap2");
  const std::string nczarr_url =
      absl::StrCat("file://", dir.child("zarr.nc").string(), "#mode=nczarr");
  EXPECT_EQ(NC_ENOTNC, nc_create(dap_url.c_str(), NC_CLOBBER, &bad_ncid));
  EXPECT_EQ(NC_ENOTBUILT, nc_open(dap_url.c_str(), NC_NOWRITE, &bad_ncid));
  EXPECT_EQ(NC_ENOTBUILT, nc_open(nczarr_url.c_str(), NC_NOWRITE, &bad_ncid));
}

TEST(DfileTest, InMemoryCreateOpenMemAndMemioLifecycleAndErrors) {
  std::array<uint8_t, 16> scratch{};
  int bad_ncid = -1;

  // nc_open_mem argument sanity checks.
  EXPECT_EQ(NC_EINVAL, nc_open_mem("m.nc", NC_NOWRITE, scratch.size(), nullptr,
                                   &bad_ncid));
  EXPECT_EQ(NC_EINVAL,
            nc_open_mem("m.nc", NC_NOWRITE, 7, scratch.data(), &bad_ncid));
  EXPECT_EQ(NC_EINVAL, nc_open_mem(nullptr, NC_NOWRITE, scratch.size(),
                                   scratch.data(), &bad_ncid));
  EXPECT_EQ(NC_EINVAL, nc_open_mem("m.nc", NC_WRITE, scratch.size(),
                                   scratch.data(), &bad_ncid));
  EXPECT_EQ(NC_EINVAL, nc_open_mem("m.nc", NC_MMAP, scratch.size(),
                                   scratch.data(), &bad_ncid));
  EXPECT_EQ(NC_EDISKLESS, nc_open_mem("m.nc", NC_DISKLESS, scratch.size(),
                                      scratch.data(), &bad_ncid));

  // nc_open_memio argument sanity checks.
  NC_memio bad_params{scratch.size(), scratch.data(), NC_MEMIO_LOCKED};
  EXPECT_EQ(NC_EINVAL,
            nc_open_memio(nullptr, NC_NOWRITE, &bad_params, &bad_ncid));
  EXPECT_EQ(NC_EINVAL, nc_open_memio("m.nc", NC_NOWRITE, nullptr, &bad_ncid));
  NC_memio null_mem{scratch.size(), nullptr, NC_MEMIO_LOCKED};
  EXPECT_EQ(NC_EINVAL, nc_open_memio("m.nc", NC_NOWRITE, &null_mem, &bad_ncid));
  NC_memio too_small{MAGIC_NUMBER_LEN - 1, scratch.data(), NC_MEMIO_LOCKED};
  EXPECT_EQ(NC_EINVAL,
            nc_open_memio("m.nc", NC_NOWRITE, &too_small, &bad_ncid));
  EXPECT_EQ(NC_EINVAL, nc_open_memio("m.nc", NC_MMAP, &bad_params, &bad_ncid));
  EXPECT_EQ(NC_EDISKLESS,
            nc_open_memio("m.nc", NC_DISKLESS, &bad_params, &bad_ncid));

  // Unbuilt HDF4 magic and corrupt Classic/HDF5 buffers in nc_open_mem.
  std::array<uint8_t, 16> hdf4_magic{0x0e, 0x03, 0x13, 0x01};
  EXPECT_EQ(NC_ENOTBUILT, nc_open_mem("hdf4.nc", NC_NOWRITE, hdf4_magic.size(),
                                      hdf4_magic.data(), &bad_ncid));

  // Use >=32 bytes so memio_open() sets chunk >= 16 ((alloc / 2) / 8 * 8) and
  // nc_get_NC() (v1hpg.c) has gs.pos < gs.end after reading magic + numrecs
  // before v1h_get_NC_dimarray() returns EINVAL on the invalid dim tag (99).
  std::array<uint8_t, 32> corrupt_nc3{'C', 'D', 'F', 0x01, 0, 0, 0, 0,
                                      0,   0,   0,   99,   0, 0, 0, 1};
  EXPECT_EQ(EINVAL,
            nc_open_mem("corrupt_nc3.nc", NC_NOWRITE, corrupt_nc3.size(),
                        corrupt_nc3.data(), &bad_ncid));

  std::array<uint8_t, 32> corrupt_hdf5{0x89, 'H',  'D',  'F',
                                       '\r', '\n', 0x1a, '\n'};
  EXPECT_EQ(NC_EHDFERR,
            nc_open_mem("corrupt_hdf5.nc", NC_NOWRITE, corrupt_hdf5.size(),
                        corrupt_hdf5.data(), &bad_ncid));

  // Round-trip Classic and NetCDF-4 datasets via nc_create_mem +
  // nc_close_memio.
  for (const int mode : {NC_CLOBBER, NC_NETCDF4 | NC_CLOBBER}) {
    ScopedMemio created_mem;
    int dimid = -1;
    int varid = -1;
    {
      ScopedNcFile nc;
      ASSERT_EQ(NC_NOERR,
                nc_create_mem("roundtrip_mem.nc", mode, 4096, nc.ptr()));
      ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "x", 3, &dimid));
      ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "v", NC_INT, 1, &dimid, &varid));
      ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));
      constexpr int kVals[3] = {11, 22, 33};
      ASSERT_EQ(NC_NOERR, nc_put_var_int(nc.get(), varid, kVals));
      ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), created_mem.get()));
    }
    ASSERT_NE(nullptr, created_mem.bytes());
    EXPECT_GE(created_mem.size(), static_cast<size_t>(MAGIC_NUMBER_LEN));

    // Reopen read-only via nc_open_mem.
    {
      ScopedNcFile reader;
      ASSERT_EQ(NC_NOERR,
                nc_open_mem("reopen_ro.nc", NC_NOWRITE, created_mem.size(),
                            created_mem.get()->memory, reader.ptr()));
      std::vector<int> out(3, -1);
      ASSERT_EQ(NC_NOERR, nc_get_var_int(reader.get(), 0, out.data()));
      EXPECT_THAT(out, ::testing::ElementsAre(11, 22, 33));
    }

    // Reopen read-only with NC_MEMIO_LOCKED via nc_open_memio.
    NC_memio locked_params = *created_mem.get();
    locked_params.flags = NC_MEMIO_LOCKED;
    {
      ScopedNcFile reader;
      ASSERT_EQ(NC_NOERR, nc_open_memio("reopen_locked.nc", NC_NOWRITE,
                                        &locked_params, reader.ptr()));
      int nvars = -1;
      EXPECT_EQ(NC_NOERR, nc_inq_nvars(reader.get(), &nvars));
      EXPECT_EQ(1, nvars);
    }

    // Classic image additionally exercises internal parallel rejection and
    // unlocked NC_WRITE ownership transfer in nc_open_memio.
    if ((mode & NC_NETCDF4) == 0) {
      EXPECT_EQ(NC_ENOTBUILT, NC_open("par_mem.nc", NC_NOWRITE | NC_INMEMORY, 0,
                                      nullptr, 1, &locked_params, &bad_ncid));

      void* heap_copy = std::malloc(created_mem.size());
      ASSERT_NE(nullptr, heap_copy);
      std::memcpy(heap_copy, created_mem.bytes(), created_mem.size());
      NC_memio rw_params{created_mem.size(), heap_copy, 0};
      ScopedMemio rw_out;
      {
        ScopedNcFile rw;
        ASSERT_EQ(NC_NOERR, nc_open_memio("reopen_rw.nc", NC_WRITE, &rw_params,
                                          rw.ptr()));
        // Classic memio_open clears rw_params.memory upon taking ownership.
        EXPECT_EQ(nullptr, rw_params.memory);
        constexpr int kUpdated[3] = {44, 55, 66};
        ASSERT_EQ(NC_NOERR, nc_put_var_int(rw.get(), 0, kUpdated));
        ASSERT_EQ(NC_NOERR, nc_close_memio(rw.release(), rw_out.get()));
      }
      ASSERT_NE(nullptr, rw_out.bytes());

      ScopedNcFile verify;
      ASSERT_EQ(NC_NOERR, nc_open_mem("verify_rw.nc", NC_NOWRITE, rw_out.size(),
                                      rw_out.get()->memory, verify.ptr()));
      std::vector<int> out(3, -1);
      ASSERT_EQ(NC_NOERR, nc_get_var_int(verify.get(), 0, out.data()));
      EXPECT_THAT(out, ::testing::ElementsAre(44, 55, 66));
    }
  }
}

TEST(DfileTest, DefineModeSyncAbortAndCloseErrorBranches) {
  // All ncid-based entry points validate bad ncid (-1) -> NC_EBADID.
  size_t path_len = 0;
  char path_buf[64]{};
  int val = 0;
  int mode = 0;
  NC_memio memio{};
  EXPECT_EQ(NC_EBADID, nc_inq_path(-1, &path_len, path_buf));
  EXPECT_EQ(NC_EBADID, nc_redef(-1));
  EXPECT_EQ(NC_EBADID, nc_enddef(-1));
  EXPECT_EQ(NC_EBADID, nc__enddef(-1, 0, 4, 0, 4));
  EXPECT_EQ(NC_EBADID, nc_sync(-1));
  EXPECT_EQ(NC_EBADID, nc_abort(-1));
  EXPECT_EQ(NC_EBADID, nc_close(-1));
  EXPECT_EQ(NC_EBADID, nc_close_memio(-1, &memio));
  EXPECT_EQ(NC_EBADID, nc_set_fill(-1, NC_FILL, &val));
  EXPECT_EQ(NC_EBADID, nc_inq_base_pe(-1, &val));
  EXPECT_EQ(NC_EBADID, nc_set_base_pe(-1, 0));
  EXPECT_EQ(NC_EBADID, nc_inq_format(-1, &val));
  EXPECT_EQ(NC_EBADID, nc_inq_format_extended(-1, &val, &mode));
  EXPECT_EQ(NC_EBADID, nc_inq(-1, &val, &val, &val, &val));
  EXPECT_EQ(NC_EBADID, nc_inq_nvars(-1, &val));

  // Classic disk file: define/data transitions, nc__enddef, sync, redef, abort.
  TempDir dir("dfile_def_sync_abort");
  TempFile new_abort_nc("dfile_def_sync_abort/new_abort.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_create(new_abort_nc.c_str(), NC_CLOBBER, nc.ptr()));
    // Aborting a newly created classic file deletes it.
    ASSERT_EQ(NC_NOERR, nc.abort());
  }
  EXPECT_FALSE(std::filesystem::exists(new_abort_nc.path()));

  TempFile redef_abort_nc("dfile_def_sync_abort/redef_abort.nc");
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create(redef_abort_nc.c_str(), NC_CLOBBER, nc.ptr()));
    EXPECT_EQ(NC_EINDEFINE, nc_redef(nc.get()));
    EXPECT_EQ(NC_EINDEFINE, nc_sync(nc.get()));

    const int init_attr = 7;
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc.get(), NC_GLOBAL, "initial", NC_INT,
                                       1, &init_attr));
    ASSERT_EQ(NC_NOERR, nc__enddef(nc.get(), 64, 4, 64, NC_ALIGN_CHUNK));
    EXPECT_EQ(NC_ENOTINDEFINE, nc_enddef(nc.get()));
    EXPECT_EQ(NC_ENOTINDEFINE, nc__enddef(nc.get(), 0, 4, 0, 4));
    EXPECT_EQ(NC_NOERR, nc_sync(nc.get()));
  }

  // Reopen NC_WRITE, enter define mode, add an uncommitted attribute, then
  // roll back with nc_abort().
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_open(redef_abort_nc.c_str(), NC_WRITE, nc.ptr()));
    ASSERT_EQ(NC_NOERR, nc_redef(nc.get()));
    const int extra_attr = 99;
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc.get(), NC_GLOBAL, "rolled_back",
                                       NC_INT, 1, &extra_attr));
    ASSERT_EQ(NC_NOERR, nc.abort());
  }
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_open(redef_abort_nc.c_str(), NC_NOWRITE, nc.ptr()));
    int natts = -1;
    ASSERT_EQ(NC_NOERR, nc_inq(nc.get(), nullptr, nullptr, &natts, nullptr));
    EXPECT_EQ(1, natts);
    EXPECT_EQ(NC_EPERM, nc_redef(nc.get()));
    EXPECT_EQ(NC_NOERR, nc_sync(nc.get()));
  }

  // NetCDF-4 child group ID passed to nc_close() and nc_close_memio() fails
  // with NC_EBADGRPID without tearing down the root dataset.
  {
    ScopedNcFile root;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("nc4_child_close.nc", NC_NETCDF4 | NC_CLOBBER, 4096,
                            root.ptr()));
    int child_grpid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_grp(root.get(), "child", &child_grpid));
    EXPECT_EQ(NC_EBADGRPID, nc_close(child_grpid));
    NC_memio failed_memio{};
    EXPECT_EQ(NC_EBADGRPID, nc_close_memio(child_grpid, &failed_memio));
    EXPECT_EQ(nullptr, failed_memio.memory);

    EXPECT_EQ(NC_NOERR, root.close());
  }
}

TEST(DfileTest, InquiryFillAndTypeAcrossAllFormats) {
  struct FormatCase {
    const char* name;
    int create_mode;
    int expected_format;
    int expected_formatx;
  };
  constexpr FormatCase kFormats[] = {
      {"classic", NC_CLOBBER, NC_FORMAT_CLASSIC, NC_FORMATX_NC3},
      {"offset64", NC_64BIT_OFFSET | NC_CLOBBER, NC_FORMAT_64BIT_OFFSET,
       NC_FORMATX_NC3},
      {"cdf5", NC_CDF5 | NC_CLOBBER, NC_FORMAT_CDF5, NC_FORMATX_NC3},
      {"nc4", NC_NETCDF4 | NC_CLOBBER, NC_FORMAT_NETCDF4, NC_FORMATX_NC4},
      {"nc4_classic", NC_NETCDF4 | NC_CLASSIC_MODEL | NC_CLOBBER,
       NC_FORMAT_NETCDF4_CLASSIC, NC_FORMATX_NC4},
  };

  for (const FormatCase& tc : kFormats) {
    SCOPED_TRACE(tc.name);
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem(absl::StrCat("fmt_", tc.name, ".nc").c_str(),
                            tc.create_mode, 4096, nc.ptr()));

    int format = -1;
    int formatx = NC_FORMATX_UNDEFINED;
    int mode = 0;
    EXPECT_EQ(NC_NOERR, nc_inq_format(nc.get(), &format));
    EXPECT_EQ(tc.expected_format, format);
    EXPECT_EQ(NC_NOERR, nc_inq_format(nc.get(), nullptr));
    EXPECT_EQ(NC_NOERR, nc_inq_format_extended(nc.get(), &formatx, &mode));
    EXPECT_EQ(tc.expected_formatx, formatx);
    EXPECT_EQ(NC_INMEMORY, mode & NC_INMEMORY);
    EXPECT_EQ(NC_NOERR, nc_inq_format_extended(nc.get(), nullptr, nullptr));

    int base_pe = -1;
    EXPECT_EQ(NC_NOERR, nc_inq_base_pe(nc.get(), &base_pe));
    EXPECT_EQ(0, base_pe);
    EXPECT_EQ(NC_NOERR, nc_inq_base_pe(nc.get(), nullptr));
    EXPECT_EQ(NC_NOERR, nc_set_base_pe(nc.get(), 0));

    // Fill mode validation and toggling.
    EXPECT_EQ(NC_EINVAL, nc_set_fill(nc.get(), 999, nullptr));
    int old_fill = -1;
    EXPECT_EQ(NC_NOERR, nc_set_fill(nc.get(), NC_NOFILL, &old_fill));
    EXPECT_EQ(NC_FILL, old_fill);
    EXPECT_EQ(NC_NOERR, nc_set_fill(nc.get(), NC_FILL, &old_fill));
    EXPECT_EQ(NC_NOFILL, old_fill);
    EXPECT_EQ(NC_NOERR, nc_set_fill(nc.get(), NC_NOFILL, nullptr));

    // Define metadata and verify nc_inq() & nc_inq_nvars().
    int udim = -1;
    int fdim = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "time", NC_UNLIMITED, &udim));
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc.get(), "fixed", 2, &fdim));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc.get(), "v", NC_INT, 1, &fdim, &varid));
    const int attr_val = 42;
    ASSERT_EQ(NC_NOERR, nc_put_att_int(nc.get(), NC_GLOBAL, NC_FillValue,
                                       NC_INT, 1, &attr_val));
    ASSERT_EQ(NC_NOERR, nc_enddef(nc.get()));

    int ndims = -1;
    int nvars = -1;
    int ngatts = -1;
    int unlimid = -99;
    EXPECT_EQ(NC_NOERR, nc_inq(nc.get(), &ndims, &nvars, &ngatts, &unlimid));
    EXPECT_EQ(2, ndims);
    EXPECT_EQ(1, nvars);
    EXPECT_EQ(1, ngatts);
    EXPECT_EQ(udim, unlimid);
    EXPECT_EQ(NC_NOERR, nc_inq(nc.get(), nullptr, nullptr, nullptr, nullptr));

    int nvars_only = -1;
    EXPECT_EQ(NC_NOERR, nc_inq_nvars(nc.get(), &nvars_only));
    EXPECT_EQ(1, nvars_only);
    EXPECT_EQ(NC_NOERR, nc_inq_nvars(nc.get(), nullptr));
  }

  // Read-only nc_set_fill returns NC_EPERM.
  {
    ScopedMemio created;
    {
      ScopedNcFile writer;
      ASSERT_EQ(NC_NOERR,
                nc_create_mem("ro_fill.nc", NC_CLOBBER, 1024, writer.ptr()));
      ASSERT_EQ(NC_NOERR, nc_close_memio(writer.release(), created.get()));
    }
    ScopedNcFile reader;
    ASSERT_EQ(NC_NOERR, nc_open_mem("ro_fill.nc", NC_NOWRITE, created.size(),
                                    created.get()->memory, reader.ptr()));
    EXPECT_EQ(NC_EPERM, nc_set_fill(reader.get(), NC_NOFILL, nullptr));
  }

  // nc_inq_type: invalid types, all atomic types even with bad ncid, and NC4
  // user-defined types.
  char type_name[NC_MAX_NAME + 1]{};
  size_t type_size = 0;
  EXPECT_EQ(NC_EBADTYPE, nc_inq_type(-1, NC_NAT, type_name, &type_size));
  EXPECT_EQ(NC_EBADTYPE, nc_inq_type(-1, -5, type_name, &type_size));

  struct AtomicTypeCase {
    nc_type type_id;
    const char* expected_name;
    size_t expected_size;
  };
  constexpr AtomicTypeCase kAtomicTypes[] = {
      {NC_BYTE, "byte", 1},
      {NC_CHAR, "char", sizeof(char)},
      {NC_SHORT, "short", sizeof(short)},
      {NC_INT, "int", sizeof(int)},
      {NC_FLOAT, "float", sizeof(float)},
      {NC_DOUBLE, "double", sizeof(double)},
      {NC_UBYTE, "ubyte", 1},
      {NC_USHORT, "ushort", sizeof(unsigned short)},
      {NC_UINT, "uint", sizeof(unsigned int)},
      {NC_INT64, "int64", sizeof(long long)},
      {NC_UINT64, "uint64", sizeof(unsigned long long)},
      {NC_STRING, "string", sizeof(char*)},
  };
  for (const AtomicTypeCase& tc : kAtomicTypes) {
    char buf[NC_MAX_NAME + 1]{};
    size_t sz = 0;
    EXPECT_EQ(NC_NOERR, nc_inq_type(-1, tc.type_id, buf, &sz));
    EXPECT_STREQ(tc.expected_name, buf);
    EXPECT_EQ(tc.expected_size, sz);
  }
  EXPECT_EQ(NC_NOERR, nc_inq_type(-1, NC_INT, nullptr, nullptr));
  EXPECT_EQ(NC_EBADTYPE,
            nc_inq_type(-1, NC_FIRSTUSERTYPEID, type_name, &type_size));

  // Classic ncid rejects user-defined type ids.
  {
    ScopedNcFile nc3;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("nc3_type.nc", NC_CLOBBER, 1024, nc3.ptr()));
    EXPECT_EQ(NC_EBADTYPE, nc_inq_type(nc3.get(), NC_FIRSTUSERTYPEID, type_name,
                                       &type_size));
  }

  // NetCDF-4 ncid resolves defined user types and rejects bogus ones.
  {
    ScopedNcFile nc4;
    ASSERT_EQ(NC_NOERR, nc_create_mem("nc4_type.nc", NC_NETCDF4 | NC_CLOBBER,
                                      4096, nc4.ptr()));
    nc_type enum_type = NC_NAT;
    ASSERT_EQ(NC_NOERR,
              nc_def_enum(nc4.get(), NC_INT, "phase_enum", &enum_type));
    char enum_name[NC_MAX_NAME + 1]{};
    size_t enum_size = 0;
    EXPECT_EQ(NC_NOERR,
              nc_inq_type(nc4.get(), enum_type, enum_name, &enum_size));
    EXPECT_STREQ("phase_enum", enum_name);
    EXPECT_EQ(sizeof(int), enum_size);
    EXPECT_EQ(NC_NOERR, nc_inq_type(nc4.get(), enum_type, nullptr, nullptr));
    EXPECT_EQ(NC_EBADTYPE, nc_inq_type(nc4.get(), NC_FIRSTUSERTYPEID + 999,
                                       enum_name, &enum_size));
  }
}

TEST(DfileTest, UninitializedLibraryAutoInitializesAndPseudofdIsMonotonic) {
  const int fd_a = nc__pseudofd();
  const int fd_b = nc__pseudofd();
  EXPECT_GT(fd_a, 0);
  EXPECT_EQ(fd_a + 1, fd_b);

  // Run first-call pseudofd initialization and finalize -> auto-initialize
  // paths inside a clean execve'd child so order/repeats never mask
  // NC_initialized==0.
  RunInExecveChild([]() -> bool {
    const int p1 = nc__pseudofd();
    const int p2 = nc__pseudofd();
    if (p1 <= 0 || p2 != p1 + 1) {
      return false;
    }

    if (nc_initialize() != NC_NOERR) {
      return false;
    }

    // Create a minimal classic image, close it, then finalize to reset
    // NC_initialized back to 0.
    ScopedMemio sample;
    {
      ScopedNcFile nc;
      if (nc_create_mem("child_sample.nc", NC_CLOBBER, 512, nc.ptr()) !=
          NC_NOERR) {
        return false;
      }
      if (nc_close_memio(nc.release(), sample.get()) != NC_NOERR) {
        return false;
      }
    }
    if (nc_finalize() != NC_NOERR || NC_initialized != 0) {
      return false;
    }

    // NC_create must auto-initialize when !NC_initialized.
    {
      ScopedNcFile nc;
      if (nc_create_mem("child_auto_create.nc", NC_CLOBBER, 512, nc.ptr()) !=
          NC_NOERR) {
        return false;
      }
      if (nc.close() != NC_NOERR) {
        return false;
      }
    }
    if (NC_initialized == 0 || nc_finalize() != NC_NOERR ||
        NC_initialized != 0) {
      return false;
    }

    // NC_open must also auto-initialize when !NC_initialized.
    {
      ScopedNcFile nc;
      if (nc_open_mem("child_auto_open.nc", NC_NOWRITE, sample.size(),
                      sample.get()->memory, nc.ptr()) != NC_NOERR) {
        return false;
      }
      if (nc.close() != NC_NOERR) {
        return false;
      }
    }
    if (NC_initialized == 0) {
      return false;
    }

    sample.reset();
    return nc_finalize() == NC_NOERR;
  });
}

}  // namespace
}  // namespace netcdf
