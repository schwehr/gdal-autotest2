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
// Tests MPI-IO VFD constants, disabled MPIO driver sentinels, ctl opcodes,
// FAPL/lookup rejection, and serial VFD fallback behavior related to
// third_party/hdf5/src/H5FDmpio.c.
// (LF=0 in default google3 non-MPI Linux build: all implementations in
// H5FDmpio.c are gated inside #ifdef H5_HAVE_PARALLEL.)
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FDmpio.c

// `:hdf5` is built with -DNDEBUG; match internal struct layout in C++ tests.
#ifndef NDEBUG
#define H5FDMPIO_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5FD_FRIEND
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#undef try
#define try h5fd_try_param
#include "third_party/hdf5/src/H5FDpkg.h"
#pragma pop_macro("try")
#pragma clang diagnostic pop
}  // extern "C"
#ifdef H5FDMPIO_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FDMPIO_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <array>
#include <cstdint>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

TEST(H5FDmpioTest, DefaultBuildDisablesParallelAndExportsMpioConstants) {
#ifdef H5_HAVE_PARALLEL
  ADD_FAILURE() << "Expected H5_HAVE_PARALLEL to be disabled in default build.";
#endif

  // Disabled MPIO VFD sentinel and library-reserved VFD value constants.
  EXPECT_EQ(H5I_INVALID_HID, H5FD_MPIO);
  EXPECT_EQ(static_cast<H5FD_class_value_t>(7), H5_VFD_MPIO);
  EXPECT_EQ(static_cast<H5FD_class_value_t>(-1), H5_VFD_INVALID);
  EXPECT_LT(H5_VFD_MPIO, H5_VFD_RESERVED);
  EXPECT_EQ(256, H5_VFD_RESERVED);
  EXPECT_EQ(65535, H5_VFD_MAX);

  // VFD class metadata constants and free-list dichotomy map used by H5FDmpio.
  EXPECT_EQ(0x01u, H5FD_CLASS_VERSION);
  EXPECT_EQ(2, H5F_CLOSE_SEMI);
  constexpr H5FD_mem_t kDichotomyMap[H5FD_MEM_NTYPES] = H5FD_FLMAP_DICHOTOMY;
  EXPECT_THAT(
      kDichotomyMap,
      ElementsAre(H5FD_MEM_SUPER, H5FD_MEM_SUPER, H5FD_MEM_SUPER, H5FD_MEM_DRAW,
                  H5FD_MEM_DRAW, H5FD_MEM_SUPER, H5FD_MEM_SUPER));

  // Feature flags checked/exported by parallel VFD query callbacks.
  EXPECT_EQ(0x00000001UL, H5FD_FEAT_AGGREGATE_METADATA);
  EXPECT_EQ(0x00000010UL, H5FD_FEAT_AGGREGATE_SMALLDATA);
  EXPECT_EQ(0x00000100UL, H5FD_FEAT_HAS_MPI);
  EXPECT_EQ(0x00000200UL, H5FD_FEAT_ALLOCATE_EARLY);
  EXPECT_EQ(0x00008000UL, H5FD_FEAT_DEFAULT_VFD_COMPATIBLE);

  // VFD ctl opcodes and flags defined for MPI state queries.
  EXPECT_EQ(512, H5FD_CTL_OPC_RESERVED);
  EXPECT_EQ(2, H5FD_CTL_GET_MPI_COMMUNICATOR_OPCODE);
  EXPECT_EQ(3, H5FD_CTL_GET_MPI_RANK_OPCODE);
  EXPECT_EQ(4, H5FD_CTL_GET_MPI_SIZE_OPCODE);
  EXPECT_EQ(8, H5FD_CTL_GET_MPI_FILE_SYNC_OPCODE);
  EXPECT_EQ(9, H5FD_CTL_GET_MPI_INFO_OPCODE);
  EXPECT_EQ(0x0001u, H5FD_CTL_FAIL_IF_UNKNOWN_FLAG);
  EXPECT_EQ(0x0002u, H5FD_CTL_ROUTE_TO_TERMINAL_VFD_FLAG);

  // Public MPI VFL thresholds and transfer/collective option enums (H5FDmpi.h).
  EXPECT_EQ(0, H5D_ONE_LINK_CHUNK_IO_THRESHOLD);
  EXPECT_EQ(60, H5D_MULTI_CHUNK_IO_COL_THRESHOLD);

  EXPECT_EQ(0, H5FD_MPIO_INDEPENDENT);
  EXPECT_EQ(1, H5FD_MPIO_COLLECTIVE);

  EXPECT_EQ(0, H5FD_MPIO_CHUNK_DEFAULT);
  EXPECT_EQ(1, H5FD_MPIO_CHUNK_ONE_IO);
  EXPECT_EQ(2, H5FD_MPIO_CHUNK_MULTI_IO);

  EXPECT_EQ(0, H5FD_MPIO_COLLECTIVE_IO);
  EXPECT_EQ(1, H5FD_MPIO_INDIVIDUAL_IO);
}

TEST(H5FDmpioTest, MpioDriverIsUnregisteredAndSerialDriversLackHasMpiFlag) {
  EXPECT_EQ(0, H5FDis_driver_registered_by_name("mpio"));
  EXPECT_EQ(0, H5FDis_driver_registered_by_value(H5_VFD_MPIO));

  {
    ScopedErrorSilence silence;
    unsigned long flags = 0;
    EXPECT_LT(H5FDdriver_query(H5FD_MPIO, &flags), 0);
  }

  struct SerialDriverSpec {
    const char* name;
    H5FD_class_value_t value;
    hid_t driver_id;
  };
  const std::array<SerialDriverSpec, 8> kSerialDrivers = {{
      {.name = "core", .value = H5_VFD_CORE, .driver_id = H5FD_CORE},
      {.name = "sec2", .value = H5_VFD_SEC2, .driver_id = H5FD_SEC2},
      {.name = "stdio", .value = H5_VFD_STDIO, .driver_id = H5FD_STDIO},
      {.name = "log", .value = H5_VFD_LOG, .driver_id = H5FD_LOG},
      {.name = "family", .value = H5_VFD_FAMILY, .driver_id = H5FD_FAMILY},
      {.name = "multi", .value = H5_VFD_MULTI, .driver_id = H5FD_MULTI},
      {.name = "splitter",
       .value = H5_VFD_SPLITTER,
       .driver_id = H5FD_SPLITTER},
      {.name = "onion", .value = H5_VFD_ONION, .driver_id = H5FD_ONION},
  }};

  for (const SerialDriverSpec& spec : kSerialDrivers) {
    EXPECT_GT(H5FDis_driver_registered_by_name(spec.name), 0) << spec.name;
    EXPECT_GT(H5FDis_driver_registered_by_value(spec.value), 0) << spec.name;
    ASSERT_NE(H5I_INVALID_HID, spec.driver_id) << spec.name;

    unsigned long flags = H5FD_FEAT_HAS_MPI;
    ASSERT_GE(H5FDdriver_query(spec.driver_id, &flags), 0) << spec.name;
    EXPECT_EQ(0UL, flags & H5FD_FEAT_HAS_MPI) << spec.name;
  }
}

TEST(H5FDmpioTest, FaplRejectsMpioDriverAndPreservesSerialCoreFallback) {
  ScopedPlist core_fapl = MakeCoreFapl();
  ASSERT_TRUE(core_fapl.valid());
  EXPECT_EQ(H5FD_CORE, H5Pget_driver(core_fapl.get()));

  ScopedPlist default_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(default_fapl.valid());
  const hid_t initial_default_driver = H5Pget_driver(default_fapl.get());
  ASSERT_NE(H5I_INVALID_HID, initial_default_driver);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Pset_driver(core_fapl.get(), H5FD_MPIO, nullptr), 0);
    EXPECT_LT(H5Pset_driver_by_name(core_fapl.get(), "mpio", nullptr), 0);
    EXPECT_LT(H5Pset_driver_by_name(core_fapl.get(), "mpio", "config"), 0);
    EXPECT_LT(H5Pset_driver_by_value(core_fapl.get(), H5_VFD_MPIO, nullptr), 0);

    EXPECT_LT(H5Pset_driver(default_fapl.get(), H5FD_MPIO, nullptr), 0);
    EXPECT_LT(H5Pset_driver_by_name(default_fapl.get(), "mpio", nullptr), 0);
    EXPECT_LT(H5Pset_driver_by_value(default_fapl.get(), H5_VFD_MPIO, nullptr),
              0);
  }

  // Failed MPIO driver assignments must leave prior FAPL drivers intact.
  EXPECT_EQ(H5FD_CORE, H5Pget_driver(core_fapl.get()));
  EXPECT_EQ(initial_default_driver, H5Pget_driver(default_fapl.get()));

  // Serial in-memory Core VFD file creation succeeds; missing/corrupt files
  // fail cleanly.
  ScopedFile created(H5Fcreate("h5fd_mpio_serial_fallback.h5", H5F_ACC_TRUNC,
                               H5P_DEFAULT, core_fapl.get()));
  ASSERT_TRUE(created.valid());
  ScopedPlist file_fapl(H5Fget_access_plist(created.get()));
  ASSERT_TRUE(file_fapl.valid());
  EXPECT_EQ(H5FD_CORE, H5Pget_driver(file_fapl.get()));
  created.reset();

  {
    ScopedErrorSilence silence;
    ScopedFile missing(H5Fopen("nonexistent_h5fd_mpio_serial.h5",
                               H5F_ACC_RDONLY, core_fapl.get()));
    EXPECT_EQ(H5I_INVALID_HID, missing.get());

    constexpr std::array<uint8_t, 32> kCorruptHeader = {0xde, 0xad, 0xbe, 0xef,
                                                        0x00, 0x01, 0x02, 0x03};
    ScopedPlist image_fapl = MakeCoreFapl();
    ASSERT_TRUE(image_fapl.valid());
    ASSERT_GE(H5Pset_file_image(image_fapl.get(),
                                const_cast<uint8_t*>(kCorruptHeader.data()),
                                kCorruptHeader.size()),
              0);
    ScopedFile corrupt(H5Fopen("corrupt_h5fd_mpio_serial.h5", H5F_ACC_RDONLY,
                               image_fapl.get()));
    EXPECT_EQ(H5I_INVALID_HID, corrupt.get());
  }
}

TEST(H5FDmpioTest, CoreVfdRejectsMpiCtlOpcodesWhenFailIfUnknownIsSet) {
  ScopedPlist core_fapl = MakeCoreFapl();
  ASSERT_TRUE(core_fapl.valid());

  H5FD_t* raw_file = H5FDopen("h5fd_mpio_ctl_probe.h5",
                              H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                              core_fapl.get(), HADDR_UNDEF);
  ASSERT_NE(nullptr, raw_file);
  absl::Cleanup close_raw_file = [&] {
    if (raw_file != nullptr) {
      EXPECT_GE(H5FDclose(raw_file), 0);
    }
  };

  unsigned long file_flags = H5FD_FEAT_HAS_MPI;
  ASSERT_GE(H5FDquery(raw_file, &file_flags), 0);
  EXPECT_EQ(0UL, file_flags & H5FD_FEAT_HAS_MPI);

  constexpr std::array<uint64_t, 5> kMpiOpcodes = {
      H5FD_CTL_GET_MPI_COMMUNICATOR_OPCODE, H5FD_CTL_GET_MPI_INFO_OPCODE,
      H5FD_CTL_GET_MPI_RANK_OPCODE,         H5FD_CTL_GET_MPI_SIZE_OPCODE,
      H5FD_CTL_GET_MPI_FILE_SYNC_OPCODE,
  };

  int sentinel = 42;
  void* output = &sentinel;
  for (const uint64_t opcode : kMpiOpcodes) {
    // Unknown opcodes without FAIL_IF_UNKNOWN succeed as no-ops on Core VFD.
    EXPECT_GE(H5FDctl(raw_file, opcode, 0, nullptr, &output), 0);
    EXPECT_EQ(&sentinel, output);

    ScopedErrorSilence silence;
    EXPECT_LT(H5FDctl(raw_file, opcode, H5FD_CTL_FAIL_IF_UNKNOWN_FLAG, nullptr,
                      &output),
              0);
    EXPECT_LT(H5FDctl(raw_file, opcode,
                      H5FD_CTL_FAIL_IF_UNKNOWN_FLAG |
                          H5FD_CTL_ROUTE_TO_TERMINAL_VFD_FLAG,
                      nullptr, &output),
              0);
  }

  // Opening a missing Core VFD file without H5F_ACC_CREAT fails.
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(nullptr, H5FDopen("nonexistent_raw_h5fd_mpio.h5", H5F_ACC_RDONLY,
                                core_fapl.get(), HADDR_UNDEF));
  }
}

TEST(H5FDmpioTest, InternalVfdLookupAndRegistrationRejectMpioInNonMpiBuild) {
  // Ensure H5FD package is initialized via a public API before calling
  // internal NOAPI routines.
  ASSERT_GE(H5open(), 0);

  hid_t reg_id = H5I_INVALID_HID;
  EXPECT_EQ(0, H5FD_is_driver_registered_by_name("mpio", &reg_id));
  EXPECT_EQ(H5I_INVALID_HID, reg_id);

  EXPECT_EQ(0, H5FD_is_driver_registered_by_value(H5_VFD_MPIO, &reg_id));
  EXPECT_EQ(H5I_INVALID_HID, reg_id);

  EXPECT_EQ(H5I_INVALID_HID, H5FD_get_driver_id_by_name("mpio", false));
  EXPECT_EQ(H5I_INVALID_HID, H5FD_get_driver_id_by_value(H5_VFD_MPIO, false));

  // Verify the same internal lookup finds built-in serial drivers cleanly.
  EXPECT_GT(H5FD_is_driver_registered_by_name("core", &reg_id), 0);
  EXPECT_EQ(H5FD_CORE, reg_id);

  reg_id = H5I_INVALID_HID;
  EXPECT_GT(H5FD_is_driver_registered_by_value(H5_VFD_CORE, &reg_id), 0);
  EXPECT_EQ(H5FD_CORE, reg_id);

  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5I_INVALID_HID, H5FD_register_driver_by_name("mpio", true));
    EXPECT_EQ(H5I_INVALID_HID,
              H5FD_register_driver_by_value(H5_VFD_MPIO, true));
  }
}

}  // namespace
}  // namespace hdf5
