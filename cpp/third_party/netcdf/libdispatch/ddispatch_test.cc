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

// Unit tests for //third_party/netcdf/libdispatch/ddispatch.c.
//
// Covers NCDISPATCH_initialize()/NCDISPATCH_finalize() (via nc_initialize() and
// nc_finalize() inside isolated child processes), internal atomic type lookup
// helpers, and process-global alignment and HDF5 metadata block size settings.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/ddispatch.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"

extern "C" {
#include "third_party/netcdf/include/nc4internal.h"
#include "third_party/netcdf/include/ncdispatch.h"
#include "third_party/netcdf/include/ncglobal.h"
#include "third_party/netcdf/include/netcdf_f.h"
int nc_initialize(void);
int nc_finalize(void);
}  // extern "C"

namespace netcdf {
namespace {

// Preserves the complete global alignment and meta_block_size structs
// (including their `.defined` flags). Calling `nc_set_alignment(0, 0)` on
// teardown would leave `alignment.defined == 1` with `alignment == 0`, causing
// subsequent HDF5 creates/opens to fail in `H5Pset_alignment`.
class ScopedAlignmentAndMetaBlock {
 public:
  ScopedAlignmentAndMetaBlock()
      : gs_(NC_getglobalstate()),
        saved_alignment_(gs_->alignment),
        saved_meta_block_size_(gs_->meta_block_size) {}

  ~ScopedAlignmentAndMetaBlock() {
    gs_->alignment = saved_alignment_;
    gs_->meta_block_size = saved_meta_block_size_;
  }

  ScopedAlignmentAndMetaBlock(const ScopedAlignmentAndMetaBlock&) = delete;
  ScopedAlignmentAndMetaBlock& operator=(const ScopedAlignmentAndMetaBlock&) =
      delete;

 private:
  NCglobalstate* gs_;
  decltype(NCglobalstate::alignment) saved_alignment_;
  decltype(NCglobalstate::meta_block_size) saved_meta_block_size_;
};

TEST(DdispatchTest, InitializeWithHomeSetPopulatesGlobalsAndCoordVectors) {
  RunInExecveChild([]() -> bool {
    constexpr char kCustomHome[] = "/custom/netcdf/home";
    if (::setenv("HOME", kCustomHome, 1) != 0 ||
        ::setenv("NCRCENV_IGNORE", "1", 1) != 0) {
      return false;
    }
    if (nc_initialize() != NC_NOERR) {
      return false;
    }

    for (int i = 0; i < NC_MAX_VAR_DIMS; ++i) {
      if (NC_coord_zero[i] != 0 || NC_coord_one[i] != 1 ||
          NC_stride_one[i] != 1) {
        return false;
      }
    }

    NCglobalstate* gs = NC_getglobalstate();
    if (gs == nullptr || gs->tempdir == nullptr || gs->home == nullptr ||
        gs->cwd == nullptr) {
      return false;
    }
    if (std::strcmp(gs->tempdir, "/tmp") != 0 ||
        std::string_view(gs->home).find("custom/netcdf/home") ==
            std::string_view::npos ||
        gs->cwd[0] == '\0') {
      return false;
    }

    return nc_finalize() == NC_NOERR;
  });
}

TEST(DdispatchTest, InitializeWithUnsetHomeFallsBackToCwdAndFinalizeCleans) {
  RunInExecveChild([]() -> bool {
    ::unsetenv("HOME");
    if (::setenv("NCRCENV_IGNORE", "1", 1) != 0) {
      return false;
    }
    if (nc_initialize() != NC_NOERR) {
      return false;
    }

    NCglobalstate* gs = NC_getglobalstate();
    if (gs == nullptr || gs->home == nullptr || gs->cwd == nullptr) {
      return false;
    }
    if (gs->home[0] == '\0' || gs->cwd[0] == '\0') {
      return false;
    }

    if (nc_finalize() != NC_NOERR) {
      return false;
    }
    // Re-initialize after finalize to verify clean restart.
    if (nc_initialize() != NC_NOERR) {
      return false;
    }
    return nc_finalize() == NC_NOERR;
  });
}

TEST(DdispatchTest, AtomicTypeInquiryAndLookupCoverAllAtomicTypesAndErrors) {
  struct AtomicSpec {
    nc_type id;
    const char* name;
    const char* mixed_case_name;
    size_t size;
  };
  constexpr AtomicSpec kAtomicTypes[] = {
      {NC_NAT, "none", "NONE", 0},
      {NC_BYTE, "byte", "Byte", 1},
      {NC_CHAR, "char", "CHAR", sizeof(char)},
      {NC_SHORT, "short", "ShOrT", 2},
      {NC_INT, "int", "INT", 4},
      {NC_FLOAT, "float", "Float", 4},
      {NC_DOUBLE, "double", "DOUBLE", 8},
      {NC_UBYTE, "ubyte", "UByte", 1},
      {NC_USHORT, "ushort", "uShort", 2},
      {NC_UINT, "uint", "Uint", 4},
      {NC_INT64, "int64", "INT64", 8},
      {NC_UINT64, "uint64", "Uint64", 8},
      {NC_STRING, "string", "StRiNg", sizeof(char*)},
  };
  static_assert(sizeof(kAtomicTypes) / sizeof(kAtomicTypes[0]) ==
                NUM_ATOMIC_TYPES);

  for (const AtomicSpec& spec : kAtomicTypes) {
    SCOPED_TRACE(spec.name);
    char name_buf[NC_MAX_NAME + 1]{};
    size_t sz = 9999;
    EXPECT_EQ(NC_NOERR, NC4_inq_atomic_type(spec.id, name_buf, &sz));
    EXPECT_STREQ(spec.name, name_buf);
    EXPECT_EQ(spec.size, sz);

    // Null output pointers must also succeed cleanly.
    EXPECT_EQ(NC_NOERR, NC4_inq_atomic_type(spec.id, nullptr, nullptr));

    // Case-insensitive lookup by name.
    nc_type found_id = -1;
    size_t found_sz = 9999;
    EXPECT_EQ(NC_NOERR, NC4_lookup_atomic_type(spec.mixed_case_name, &found_id,
                                               &found_sz));
    EXPECT_EQ(spec.id, found_id);
    EXPECT_EQ(spec.size, found_sz);
    EXPECT_EQ(NC_NOERR, NC4_lookup_atomic_type(spec.name, nullptr, nullptr));

    // Exact-case typeid inquiry.
    nc_type exact_id = -1;
    EXPECT_EQ(NC_NOERR, NC4_inq_atomic_typeid(0, spec.name, &exact_id));
    EXPECT_EQ(spec.id, exact_id);
    EXPECT_EQ(NC_NOERR, NC4_inq_atomic_typeid(0, spec.name, nullptr));
  }

  // Invalid type IDs >= NUM_ATOMIC_TYPES.
  EXPECT_EQ(NC_EBADTYPE,
            NC4_inq_atomic_type(NUM_ATOMIC_TYPES, nullptr, nullptr));
  EXPECT_EQ(NC_EBADTYPE,
            NC4_inq_atomic_type(NC_FIRSTUSERTYPEID, nullptr, nullptr));

  // Lookup failures (NULL, empty, unknown).
  nc_type id_out = 99;
  size_t sz_out = 99;
  EXPECT_EQ(NC_EBADTYPE, NC4_lookup_atomic_type(nullptr, &id_out, &sz_out));
  EXPECT_EQ(NC_EBADTYPE, NC4_lookup_atomic_type("", &id_out, &sz_out));
  EXPECT_EQ(NC_EBADTYPE, NC4_lookup_atomic_type("unknown", &id_out, &sz_out));

  // Exact-case inquiry rejects uppercase/unknown names.
  EXPECT_EQ(NC_EBADTYPE, NC4_inq_atomic_typeid(0, "INT", &id_out));
  EXPECT_EQ(NC_EBADTYPE, NC4_inq_atomic_typeid(0, "", &id_out));
  EXPECT_EQ(NC_EBADTYPE, NC4_inq_atomic_typeid(0, "bogus", &id_out));
}

TEST(DdispatchTest, GetAtomicTypeclassMapsAllClassesAndRejectsOthers) {
  struct ClassCase {
    nc_type type;
    int expected_class;
  };
  constexpr ClassCase kValidCases[] = {
      {NC_BYTE, NC_INT},     {NC_UBYTE, NC_INT},  {NC_SHORT, NC_INT},
      {NC_USHORT, NC_INT},   {NC_INT, NC_INT},    {NC_UINT, NC_INT},
      {NC_INT64, NC_INT},    {NC_UINT64, NC_INT}, {NC_FLOAT, NC_FLOAT},
      {NC_DOUBLE, NC_FLOAT}, {NC_CHAR, NC_CHAR},  {NC_STRING, NC_STRING},
  };
  for (const ClassCase& tc : kValidCases) {
    SCOPED_TRACE(tc.type);
    int type_class = -1;
    EXPECT_EQ(NC_NOERR, NC4_get_atomic_typeclass(tc.type, &type_class));
    EXPECT_EQ(tc.expected_class, type_class);
  }

  constexpr nc_type kInvalidTypes[] = {
      NC_NAT, NC_VLEN, NC_OPAQUE, NC_ENUM, NC_COMPOUND, NUM_ATOMIC_TYPES, -1,
  };
  for (nc_type invalid : kInvalidTypes) {
    SCOPED_TRACE(invalid);
    int type_class = -1;
    EXPECT_EQ(NC_EBADTYPE, NC4_get_atomic_typeclass(invalid, &type_class));
  }
}

TEST(DdispatchTest, AlignmentAndMetaBlockSizeRoundTripAndApplyToNetcdf4) {
  ScopedAlignmentAndMetaBlock restore_globals;

  int threshold = -1;
  int alignment = -1;
  EXPECT_EQ(NC_NOERR, nc_get_alignment(nullptr, nullptr));
  EXPECT_EQ(NC_NOERR, nc_get_alignment(&threshold, nullptr));
  EXPECT_EQ(NC_NOERR, nc_get_alignment(nullptr, &alignment));
  EXPECT_EQ(NC_NOERR, nc_get_alignment(&threshold, &alignment));

  EXPECT_EQ(NC_NOERR, nc_set_alignment(256, 1024));
  EXPECT_EQ(NC_NOERR, nc_get_alignment(&threshold, &alignment));
  EXPECT_EQ(256, threshold);
  EXPECT_EQ(1024, alignment);
  EXPECT_EQ(1, NC_getglobalstate()->alignment.defined);

  // Meta block size APIs (size_t and Fortran-int compatibility wrappers).
  size_t meta_sz = 9999;
  EXPECT_EQ(NC_NOERR, nc_get_meta_block_size(nullptr));
  EXPECT_EQ(NC_NOERR, nc_set_meta_block_size(8192));
  EXPECT_EQ(NC_NOERR, nc_get_meta_block_size(&meta_sz));
  EXPECT_EQ(size_t{8192}, meta_sz);
  EXPECT_EQ(1, NC_getglobalstate()->meta_block_size.defined);

  int meta_isz = -1;
  EXPECT_EQ(NC_NOERR, nc_get_meta_block_size_ints(nullptr));
  EXPECT_EQ(NC_NOERR, nc_set_meta_block_size_ints(16384));
  EXPECT_EQ(NC_NOERR, nc_get_meta_block_size_ints(&meta_isz));
  EXPECT_EQ(16384, meta_isz);
  EXPECT_EQ(size_t{16384}, NC_getglobalstate()->meta_block_size.size);
  EXPECT_EQ(1, NC_getglobalstate()->meta_block_size.defined);

  // Create and reopen an in-memory NetCDF-4 dataset while active.
  ScopedMemio memio;
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR,
              nc_create_mem("align_meta.nc", NC_NETCDF4, 4096, nc.ptr()));
    int dimid = -1;
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_def_dim(nc, "x", 4, &dimid));
    ASSERT_EQ(NC_NOERR, nc_def_var(nc, "v", NC_INT, 1, &dimid, &varid));
    constexpr std::array<int, 4> kValues = {10, 20, 30, 40};
    ASSERT_EQ(NC_NOERR, nc_put_var_int(nc, varid, kValues.data()));
    ASSERT_EQ(NC_NOERR, nc_close_memio(nc.release(), memio.get()));
  }
  {
    ScopedNcFile nc;
    ASSERT_EQ(NC_NOERR, nc_open_mem("align_meta.nc", NC_NOWRITE, memio.size(),
                                    memio.get()->memory, nc.ptr()));
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(nc, "v", &varid));
    std::array<int, 4> out{};
    ASSERT_EQ(NC_NOERR, nc_get_var_int(nc, varid, out.data()));
    EXPECT_EQ((std::array<int, 4>{10, 20, 30, 40}), out);
  }

  // Setting size == 0 disables the meta block size override.
  EXPECT_EQ(NC_NOERR, nc_set_meta_block_size(0));
  EXPECT_EQ(NC_NOERR, nc_get_meta_block_size(&meta_sz));
  EXPECT_EQ(size_t{0}, meta_sz);
  EXPECT_EQ(0, NC_getglobalstate()->meta_block_size.defined);

  EXPECT_EQ(NC_NOERR, nc_set_meta_block_size_ints(4096));
  EXPECT_EQ(1, NC_getglobalstate()->meta_block_size.defined);
  EXPECT_EQ(NC_NOERR, nc_set_meta_block_size_ints(0));
  EXPECT_EQ(NC_NOERR, nc_get_meta_block_size_ints(&meta_isz));
  EXPECT_EQ(0, meta_isz);
  EXPECT_EQ(0, NC_getglobalstate()->meta_block_size.defined);
}

}  // namespace
}  // namespace netcdf
