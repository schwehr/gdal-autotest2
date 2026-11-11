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

// Unit tests for //third_party/netcdf/libhdf5/nc4memcb.c (file image
// callbacks).
//
// Covers NC4_image_init (create/open, initial size branches, 10% alloc_incr,
// failure paths), NC4_extract_file_image, NC4_image_finalize, and all branches
// of the internal file-image callback family (image_malloc, image_memcpy,
// image_realloc, image_free, udata_copy, udata_free) captured cleanly via a
// temporary pass-through HDF5 VOL connector in nc4memcb_shim.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libhdf5/nc4memcb.c

#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/libhdf5/nc4memcb_shim.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
#include "third_party/netcdf/include/netcdf_mem.h"
// clang-format on

#if !defined(ADDRESS_SANITIZER) && !defined(__SANITIZE_ADDRESS__) && \
    !defined(MEMORY_SANITIZER) && !defined(__SANITIZE_MEMORY__) &&   \
    !defined(THREAD_SANITIZER) && !defined(__SANITIZE_THREAD__)
#define NC4MEMCB_TEST_ALLOW_HUGE_REALLOC_OOM 1
#else
#define NC4MEMCB_TEST_ALLOW_HUGE_REALLOC_OOM 0
#endif

namespace netcdf {
namespace {

using ::testing::ElementsAre;

constexpr std::array<unsigned char, 8> kHdf5Magic = {
    0x89, 'H', 'D', 'F', '\r', '\n', 0x1a, '\n',
};

// RAII scope that installs the shim's capture VOL as the default HDF5 VOL
// connector and cleanly restores the environment and unregisters the connector
// upon destruction (declare BEFORE any ScopedNcFile created inside the scope).
class ScopedCaptureVol {
 public:
  ScopedCaptureVol() {
    Nc4MemcbShimEnsureHdf5Initialized();
    const char* prev = std::getenv("HDF5_VOL_CONNECTOR");
    if (prev != nullptr) {
      prev_env_ = prev;
    }
    Nc4MemcbShimResetCapturedCallbacks();
    registered_ = (Nc4MemcbShimRegisterCaptureVol() == 0);
    if (registered_) {
      if (setenv("HDF5_VOL_CONNECTOR", "nc4memcb_capture_vol", 1) == 0 &&
          Nc4MemcbShimReparseDefaultVol() == 0) {
        active_ = true;
      }
    }
  }

  ~ScopedCaptureVol() {
    Nc4MemcbShimSetFailNextCreate(0);
    if (active_) {
      if (prev_env_.has_value()) {
        setenv("HDF5_VOL_CONNECTOR", prev_env_->c_str(), 1);
      } else {
        unsetenv("HDF5_VOL_CONNECTOR");
      }
      static_cast<void>(Nc4MemcbShimReparseDefaultVol());
    }
    if (registered_) {
      static_cast<void>(Nc4MemcbShimUnregisterCaptureVol());
    }
    Nc4MemcbShimResetCapturedCallbacks();
  }

  ScopedCaptureVol(const ScopedCaptureVol&) = delete;
  ScopedCaptureVol& operator=(const ScopedCaptureVol&) = delete;

  bool valid() const { return active_; }

 private:
  std::optional<std::string> prev_env_;
  bool registered_ = false;
  bool active_ = false;
};

ScopedMemio BuildNetcdf4Image(size_t initial_size, size_t payload_ints) {
  ScopedNcFile nc;
  if (nc_create_mem("nc4memcb_build.nc", NC_NETCDF4, initial_size, nc.ptr()) !=
      NC_NOERR) {
    return ScopedMemio();
  }
  int dimid = -1;
  int varid = -1;
  if (nc_def_dim(nc, "n", payload_ints, &dimid) != NC_NOERR ||
      nc_def_var(nc, "data", NC_INT, 1, &dimid, &varid) != NC_NOERR ||
      nc_enddef(nc) != NC_NOERR) {
    return ScopedMemio();
  }
  std::vector<int> vals(payload_ints, 42);
  if (nc_put_var_int(nc, varid, vals.data()) != NC_NOERR) {
    return ScopedMemio();
  }
  ScopedMemio out;
  if (nc_close_memio(nc.release(), out.get()) != NC_NOERR) {
    return ScopedMemio();
  }
  return out;
}

TEST(Nc4MemcbTest, ImageInitCoversSmallDefaultAndLargePercentAllocIncr) {
  // initialsz == 0 -> uses DEFAULT_CREATE_MEMSIZE (65536).
  {
    ScopedMemio def_img = BuildNetcdf4Image(/*initial_size=*/0, 8);
    ASSERT_NE(def_img.get()->memory, nullptr);
    ASSERT_GT(def_img.size(), 8U);

    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open_mem("ro_def.nc", NC_NOWRITE, def_img.size(),
                                    def_img.get()->memory, ro.ptr()));
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "data", &varid));
    std::vector<int> vals(8, 0);
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, varid, vals.data()));
    EXPECT_EQ(vals[0], 42);
  }

  // initialsz == 1 MiB (> 655360) -> hits the 10% alloc_incr branch in
  // NC4_image_init() and triggers multiple realloc growths when writing >1 MiB.
  {
    ScopedMemio large_img =
        BuildNetcdf4Image(/*initial_size=*/1048576, /*payload_ints=*/300000);
    ASSERT_NE(large_img.get()->memory, nullptr);
    EXPECT_GT(large_img.size(), 1048576U);

    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open_mem("ro_large.nc", NC_NOWRITE, large_img.size(),
                                    large_img.get()->memory, ro.ptr()));
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "data", &varid));
    std::vector<int> vals(300000, -1);
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, varid, vals.data()));
    EXPECT_EQ(42, vals.front());
    EXPECT_EQ(42, vals.back());
  }
}

TEST(Nc4MemcbTest, ImageInitAndExtractBoundaryErrorCases) {
  Nc4MemcbShimEnsureHdf5Initialized();
  EXPECT_LT(Nc4MemcbShimImageInitMissingInput(), 0);
  EXPECT_LT(Nc4MemcbShimImageInitZeroSize(), 0);
  Nc4MemcbShimFinalizeNull();
  EXPECT_EQ(NC_EHDFERR, Nc4MemcbShimExtractAbortNullUdata());

  // Public H5Fopen failure path in NC4_image_init.
  std::vector<unsigned char> corrupt(64, 0);
  std::memcpy(corrupt.data(), kHdf5Magic.data(), kHdf5Magic.size());
  ScopedNcFile bad;
  EXPECT_EQ(NC_EHDFERR, nc_open_mem("bad_cb.nc", NC_NOWRITE, corrupt.size(),
                                    corrupt.data(), bad.ptr()));
}

TEST(Nc4MemcbTest, CaptureVolDrivesCreateFailureAndAllImageCallbackBranches) {
  ScopedCaptureVol cap;
  ASSERT_TRUE(cap.valid());

  // 1. Trigger NC4_image_init() H5Fcreate failure cleanly (no heap leak).
  EXPECT_LT(Nc4MemcbShimImageInitCreateFail(), 0);
  EXPECT_EQ(1, Nc4MemcbShimHasCapturedCallbacks());

  // 2. Drive real create + reopen through capture VOL (exercises H5Pcopy,
  //    PROPERTY_LIST_COPY, PROPERTY_LIST_CLOSE, FILE_OPEN, FILE_RESIZE,
  //    FILE_CLOSE, udata_copy, udata_free).
  {
    ScopedMemio sample = BuildNetcdf4Image(/*initial_size=*/0, 4);
    ASSERT_NE(sample.get()->memory, nullptr);
    ScopedNcFile ro;
    ASSERT_EQ(NC_NOERR, nc_open_mem("cap_ro.nc", NC_NOWRITE, sample.size(),
                                    sample.get()->memory, ro.ptr()));
    int varid = -1;
    ASSERT_EQ(NC_NOERR, nc_inq_varid(ro, "data", &varid));
    std::vector<int> got(4, -1);
    ASSERT_EQ(NC_NOERR, nc_get_var_int(ro, varid, got.data()));
    EXPECT_THAT(got, ElementsAre(42, 42, 42, 42));
  }
  ASSERT_EQ(1, Nc4MemcbShimHasCapturedCallbacks());

  // 3. Exhaustively exercise all guard and switch branches of the captured
  //    local_image_* and local_udata_* callbacks using a layout-compatible
  //    udata mirror.
  std::array<unsigned char, 64> app_buf{};
  std::array<unsigned char, 64> alt_buf{};
  std::array<unsigned char, 64> dummy_buf{};
  for (size_t i = 0; i < app_buf.size(); ++i) {
    app_buf[i] = static_cast<unsigned char>(i + 1);
  }

  // udata_copy / udata_free
  {
    nc4memcb_udata_mirror_t ud{};
    EXPECT_EQ(nullptr, Nc4MemcbShimCallUdataCopy(&ud));
    EXPECT_LT(Nc4MemcbShimCallUdataFree(&ud), 0);
    ud.ref_count = 1;
    EXPECT_EQ(&ud, Nc4MemcbShimCallUdataCopy(&ud));
    EXPECT_EQ(2, ud.ref_count);
    EXPECT_GE(Nc4MemcbShimCallUdataFree(&ud), 0);
    EXPECT_EQ(1, ud.ref_count);
    EXPECT_GE(Nc4MemcbShimCallUdataFree(&ud), 0);
    EXPECT_EQ(0, ud.ref_count);
  }

  // local_image_malloc: all cases & guards.
  {
    nc4memcb_udata_mirror_t ud{};
    EXPECT_EQ(nullptr,
              Nc4MemcbShimCallMalloc(64, kNc4MemcbOpPropertyListSet, &ud));

    ud.app_image_ptr = app_buf.data();
    ud.app_image_size = 64;
    EXPECT_EQ(nullptr,
              Nc4MemcbShimCallMalloc(32, kNc4MemcbOpPropertyListSet, &ud));

    // Assert at nc4memcb.c:261 requires fapl_image_ptr == app_image_ptr if
    // non-null.
    ud.fapl_image_ptr = app_buf.data();
    EXPECT_EQ(nullptr,
              Nc4MemcbShimCallMalloc(64, kNc4MemcbOpPropertyListSet, &ud));
    ud.fapl_image_ptr = nullptr;
    ud.fapl_image_size = 64;
    EXPECT_EQ(nullptr,
              Nc4MemcbShimCallMalloc(64, kNc4MemcbOpPropertyListSet, &ud));
    ud.fapl_image_size = 0;
    ud.fapl_ref_count = 1;
    EXPECT_EQ(nullptr,
              Nc4MemcbShimCallMalloc(64, kNc4MemcbOpPropertyListSet, &ud));

    ud.fapl_ref_count = 0;
    EXPECT_EQ(app_buf.data(),
              Nc4MemcbShimCallMalloc(64, kNc4MemcbOpPropertyListSet, &ud));
    EXPECT_EQ(1, ud.fapl_ref_count);

    // PROPERTY_LIST_COPY guards & success
    ud.fapl_image_ptr = nullptr;
    EXPECT_EQ(nullptr,
              Nc4MemcbShimCallMalloc(64, kNc4MemcbOpPropertyListCopy, &ud));
    ud.fapl_image_ptr = app_buf.data();
    EXPECT_EQ(nullptr,
              Nc4MemcbShimCallMalloc(32, kNc4MemcbOpPropertyListCopy, &ud));
    ud.fapl_ref_count = 0;
    EXPECT_EQ(nullptr,
              Nc4MemcbShimCallMalloc(64, kNc4MemcbOpPropertyListCopy, &ud));
    ud.fapl_ref_count = 1;
    EXPECT_EQ(app_buf.data(),
              Nc4MemcbShimCallMalloc(64, kNc4MemcbOpPropertyListCopy, &ud));
    EXPECT_EQ(2, ud.fapl_ref_count);

    // PROPERTY_LIST_GET guard & success
    ud.fapl_image_ptr = nullptr;
    EXPECT_EQ(nullptr,
              Nc4MemcbShimCallMalloc(64, kNc4MemcbOpPropertyListGet, &ud));
    ud.fapl_image_ptr = app_buf.data();
    EXPECT_EQ(app_buf.data(),
              Nc4MemcbShimCallMalloc(64, kNc4MemcbOpPropertyListGet, &ud));

    // FILE_OPEN guards & success
    ud.vfd_image_ptr = dummy_buf.data();
    EXPECT_EQ(nullptr, Nc4MemcbShimCallMalloc(64, kNc4MemcbOpFileOpen, &ud));
    ud.vfd_image_ptr = nullptr;
    ud.vfd_image_size = 64;
    EXPECT_EQ(nullptr, Nc4MemcbShimCallMalloc(64, kNc4MemcbOpFileOpen, &ud));
    ud.vfd_image_size = 0;
    ud.vfd_ref_count = 1;
    EXPECT_EQ(nullptr, Nc4MemcbShimCallMalloc(64, kNc4MemcbOpFileOpen, &ud));
    ud.vfd_ref_count = 0;
    ud.fapl_image_ptr = nullptr;
    EXPECT_EQ(nullptr, Nc4MemcbShimCallMalloc(64, kNc4MemcbOpFileOpen, &ud));
    ud.fapl_image_ptr = app_buf.data();
    EXPECT_EQ(nullptr, Nc4MemcbShimCallMalloc(32, kNc4MemcbOpFileOpen, &ud));
    ud.fapl_ref_count = 0;
    EXPECT_EQ(nullptr, Nc4MemcbShimCallMalloc(64, kNc4MemcbOpFileOpen, &ud));
    ud.fapl_ref_count = 1;
    EXPECT_EQ(app_buf.data(),
              Nc4MemcbShimCallMalloc(64, kNc4MemcbOpFileOpen, &ud));
    EXPECT_EQ(1, ud.vfd_ref_count);
    EXPECT_EQ(64U, ud.vfd_image_size);

    // Unused/default op labels
    EXPECT_EQ(nullptr, Nc4MemcbShimCallMalloc(64, kNc4MemcbOpNoOp, &ud));
    EXPECT_EQ(nullptr,
              Nc4MemcbShimCallMalloc(64, kNc4MemcbOpPropertyListClose, &ud));
    EXPECT_EQ(nullptr, Nc4MemcbShimCallMalloc(64, kNc4MemcbOpFileResize, &ud));
    EXPECT_EQ(nullptr, Nc4MemcbShimCallMalloc(64, kNc4MemcbOpFileClose, &ud));
  }

  // local_image_memcpy: all cases & guards, including real memcpy(dest != src).
  {
    nc4memcb_udata_mirror_t ud{};
    ud.app_image_ptr = app_buf.data();
    ud.app_image_size = 64;
    ud.fapl_image_ptr = alt_buf.data();
    ud.fapl_image_size = 64;
    ud.fapl_ref_count = 1;

    EXPECT_EQ(nullptr,
              Nc4MemcbShimCallMemcpy(dummy_buf.data(), app_buf.data(), 64,
                                     kNc4MemcbOpPropertyListSet, &ud));
    EXPECT_EQ(nullptr,
              Nc4MemcbShimCallMemcpy(alt_buf.data(), dummy_buf.data(), 64,
                                     kNc4MemcbOpPropertyListSet, &ud));
    EXPECT_EQ(nullptr,
              Nc4MemcbShimCallMemcpy(alt_buf.data(), app_buf.data(), 32,
                                     kNc4MemcbOpPropertyListSet, &ud));
    ud.app_image_size = 32;
    EXPECT_EQ(nullptr,
              Nc4MemcbShimCallMemcpy(alt_buf.data(), app_buf.data(), 64,
                                     kNc4MemcbOpPropertyListSet, &ud));
    ud.app_image_size = 64;
    ud.fapl_ref_count = 0;
    EXPECT_EQ(nullptr,
              Nc4MemcbShimCallMemcpy(alt_buf.data(), app_buf.data(), 64,
                                     kNc4MemcbOpPropertyListSet, &ud));

    // Success with !DONT_COPY and src != dest copies bytes into alt_buf.
    ud.fapl_ref_count = 1;
    EXPECT_EQ(alt_buf.data(),
              Nc4MemcbShimCallMemcpy(alt_buf.data(), app_buf.data(), 64,
                                     kNc4MemcbOpPropertyListSet, &ud));
    EXPECT_EQ(app_buf, alt_buf);

    // PROPERTY_LIST_COPY guards & success
    EXPECT_EQ(nullptr,
              Nc4MemcbShimCallMemcpy(dummy_buf.data(), alt_buf.data(), 64,
                                     kNc4MemcbOpPropertyListCopy, &ud));
    EXPECT_EQ(nullptr,
              Nc4MemcbShimCallMemcpy(alt_buf.data(), dummy_buf.data(), 64,
                                     kNc4MemcbOpPropertyListCopy, &ud));
    EXPECT_EQ(nullptr,
              Nc4MemcbShimCallMemcpy(alt_buf.data(), alt_buf.data(), 32,
                                     kNc4MemcbOpPropertyListCopy, &ud));
    EXPECT_EQ(nullptr, Nc4MemcbShimCallMemcpy(alt_buf.data(), alt_buf.data(),
                                              64, kNc4MemcbOpPropertyListCopy,
                                              &ud));  // ref_count < 2
    ud.fapl_ref_count = 2;
    EXPECT_EQ(alt_buf.data(),
              Nc4MemcbShimCallMemcpy(alt_buf.data(), alt_buf.data(), 64,
                                     kNc4MemcbOpPropertyListCopy, &ud));

    // PROPERTY_LIST_GET guards & success
    EXPECT_EQ(nullptr, Nc4MemcbShimCallMemcpy(alt_buf.data(), alt_buf.data(),
                                              64, kNc4MemcbOpPropertyListGet,
                                              &ud));  // !DONT_COPY
    ud.flags = kNc4MemcbFlagDontCopy | kNc4MemcbFlagDontRelease;
    EXPECT_EQ(nullptr,
              Nc4MemcbShimCallMemcpy(dummy_buf.data(), alt_buf.data(), 64,
                                     kNc4MemcbOpPropertyListGet, &ud));
    EXPECT_EQ(nullptr,
              Nc4MemcbShimCallMemcpy(dummy_buf.data(), dummy_buf.data(), 64,
                                     kNc4MemcbOpPropertyListGet, &ud));
    EXPECT_EQ(alt_buf.data(),
              Nc4MemcbShimCallMemcpy(alt_buf.data(), alt_buf.data(), 64,
                                     kNc4MemcbOpPropertyListGet, &ud));

    // FILE_OPEN guards & success
    ud.vfd_image_ptr = alt_buf.data();
    ud.vfd_image_size = 64;
    ud.vfd_ref_count = 1;
    EXPECT_EQ(nullptr, Nc4MemcbShimCallMemcpy(dummy_buf.data(), alt_buf.data(),
                                              64, kNc4MemcbOpFileOpen, &ud));
    EXPECT_EQ(nullptr, Nc4MemcbShimCallMemcpy(alt_buf.data(), dummy_buf.data(),
                                              64, kNc4MemcbOpFileOpen, &ud));
    EXPECT_EQ(nullptr, Nc4MemcbShimCallMemcpy(alt_buf.data(), alt_buf.data(),
                                              32, kNc4MemcbOpFileOpen, &ud));
    ud.fapl_image_size = 32;
    EXPECT_EQ(nullptr, Nc4MemcbShimCallMemcpy(alt_buf.data(), alt_buf.data(),
                                              64, kNc4MemcbOpFileOpen, &ud));
    ud.fapl_image_size = 64;
    ud.fapl_ref_count = 0;
    EXPECT_EQ(nullptr, Nc4MemcbShimCallMemcpy(alt_buf.data(), alt_buf.data(),
                                              64, kNc4MemcbOpFileOpen, &ud));
    ud.fapl_ref_count = 1;
    ud.vfd_ref_count = 2;
    EXPECT_EQ(nullptr, Nc4MemcbShimCallMemcpy(alt_buf.data(), alt_buf.data(),
                                              64, kNc4MemcbOpFileOpen, &ud));
    ud.vfd_ref_count = 1;
    EXPECT_EQ(alt_buf.data(),
              Nc4MemcbShimCallMemcpy(alt_buf.data(), alt_buf.data(), 64,
                                     kNc4MemcbOpFileOpen, &ud));

    // Unused/default labels
    EXPECT_EQ(nullptr, Nc4MemcbShimCallMemcpy(alt_buf.data(), alt_buf.data(),
                                              64, kNc4MemcbOpNoOp, &ud));
  }

  // local_image_realloc: read-only, wrong op, DONT_COPY shrink/overflow,
  // ptr == NULL (both app_image_ptr == NULL and != NULL), ptr != NULL guards
  // and growth.
  {
    nc4memcb_udata_mirror_t ud{};
    ud.vfd_image_ptr = app_buf.data();
    ud.vfd_image_size = 64;
    ud.vfd_ref_count = 1;
    EXPECT_EQ(nullptr, Nc4MemcbShimCallRealloc(app_buf.data(), 128,
                                               kNc4MemcbOpFileResize, &ud));

    ud.flags =
        kNc4MemcbFlagOpenRw | kNc4MemcbFlagDontCopy | kNc4MemcbFlagDontRelease;
    EXPECT_EQ(nullptr, Nc4MemcbShimCallRealloc(app_buf.data(), 64,
                                               kNc4MemcbOpNoOp, &ud));
    EXPECT_EQ(app_buf.data(),
              Nc4MemcbShimCallRealloc(app_buf.data(), 32, kNc4MemcbOpFileResize,
                                      &ud));
    EXPECT_EQ(32U, ud.vfd_image_size);
    EXPECT_EQ(nullptr, Nc4MemcbShimCallRealloc(app_buf.data(), 128,
                                               kNc4MemcbOpFileResize, &ud));

    // Editable mode (DONT_COPY unset): ptr == NULL with prior app_image_ptr
    // frees app_image_ptr and allocates the replacement VFD buffer.
    ud = {};
    ud.flags = kNc4MemcbFlagOpenRw;
    ud.app_image_ptr = std::malloc(32);
    ASSERT_NE(nullptr, ud.app_image_ptr);
    void* replaced =
        Nc4MemcbShimCallRealloc(nullptr, 48, kNc4MemcbOpFileResize, &ud);
    ASSERT_NE(nullptr, replaced);
    EXPECT_EQ(replaced, ud.vfd_image_ptr);
    EXPECT_EQ(replaced, ud.app_image_ptr);
    EXPECT_EQ(48U, ud.vfd_image_size);
    EXPECT_EQ(1, ud.vfd_ref_count);
    std::free(replaced);

    // Editable mode (DONT_COPY unset): ptr == NULL without prior app_image_ptr.
    ud = {};
    ud.flags = kNc4MemcbFlagOpenRw;
    void* fresh =
        Nc4MemcbShimCallRealloc(nullptr, 64, kNc4MemcbOpFileResize, &ud);
    ASSERT_NE(nullptr, fresh);
    EXPECT_EQ(fresh, ud.vfd_image_ptr);
    EXPECT_EQ(1, ud.vfd_ref_count);

    // Guards on ptr != NULL
    EXPECT_EQ(nullptr, Nc4MemcbShimCallRealloc(dummy_buf.data(), 128,
                                               kNc4MemcbOpFileResize, &ud));
    ud.vfd_ref_count = 2;
    EXPECT_EQ(nullptr,
              Nc4MemcbShimCallRealloc(fresh, 128, kNc4MemcbOpFileResize, &ud));
    ud.vfd_ref_count = 1;

    void* grown =
        Nc4MemcbShimCallRealloc(fresh, 128, kNc4MemcbOpFileResize, &ud);
    ASSERT_NE(nullptr, grown);
    EXPECT_EQ(128U, ud.vfd_image_size);

#if NC4MEMCB_TEST_ALLOW_HUGE_REALLOC_OOM
    void* pre_oom = grown;
    EXPECT_EQ(nullptr, Nc4MemcbShimCallRealloc(
                           pre_oom, std::numeric_limits<size_t>::max() / 2,
                           kNc4MemcbOpFileResize, &ud));
    // Realloc failure leaves previous buffer unfreed; free pre_oom cleanly.
    std::free(pre_oom);
    grown = nullptr;
#else
    std::free(grown);
    grown = nullptr;
#endif
  }

  // local_image_free: all cases & guards.
  {
    nc4memcb_udata_mirror_t ud{};
    ud.app_image_ptr = app_buf.data();
    ud.fapl_image_ptr = app_buf.data();
    ud.fapl_ref_count = 1;
    EXPECT_LT(Nc4MemcbShimCallFree(dummy_buf.data(),
                                   kNc4MemcbOpPropertyListClose, &ud),
              0);
    ud.fapl_ref_count = 0;
    EXPECT_LT(
        Nc4MemcbShimCallFree(app_buf.data(), kNc4MemcbOpPropertyListClose, &ud),
        0);
    ud.fapl_ref_count = 1;
    EXPECT_GE(
        Nc4MemcbShimCallFree(app_buf.data(), kNc4MemcbOpPropertyListClose, &ud),
        0);
    EXPECT_EQ(0, ud.fapl_ref_count);

    ud.vfd_image_ptr = app_buf.data();
    ud.vfd_ref_count = 1;
    EXPECT_LT(Nc4MemcbShimCallFree(dummy_buf.data(), kNc4MemcbOpFileClose, &ud),
              0);
    ud.vfd_ref_count = 2;
    EXPECT_LT(Nc4MemcbShimCallFree(app_buf.data(), kNc4MemcbOpFileClose, &ud),
              0);
    ud.vfd_ref_count = 1;
    EXPECT_GE(Nc4MemcbShimCallFree(app_buf.data(), kNc4MemcbOpFileClose, &ud),
              0);
    EXPECT_EQ(0, ud.vfd_ref_count);

    EXPECT_LT(Nc4MemcbShimCallFree(app_buf.data(), kNc4MemcbOpNoOp, &ud), 0);
  }
}

}  // namespace
}  // namespace netcdf
