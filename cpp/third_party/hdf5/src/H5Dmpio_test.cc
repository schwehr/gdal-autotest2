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
// Tests MPI-IO dataset transfer constants, disabled parallel driver sentinels,
// DXPL parallel property registration/encoding, non-parallel serial dataset I/O
// fallback, and internal I/O info struct layout related to
// third_party/hdf5/src/H5Dmpio.c.
// (LF=0 in default google3 non-MPI Linux build: all implementations in
// H5Dmpio.c are gated inside #ifdef H5_HAVE_PARALLEL.)
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Dmpio.c

#include "third_party/hdf5/src/hdf5.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

// `:hdf5` is built with -DNDEBUG, which omits debug fields from internal
// metadata structures pulled by H5private.h and H5Dpkg.h.
#ifndef NDEBUG
#define H5DMPIO_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#pragma push_macro("try")
#undef try
#define try h5d_try_param
#define H5D_FRIEND
#include "third_party/hdf5/src/H5Dpkg.h"
#pragma pop_macro("try")
}  // extern "C"
#ifdef H5DMPIO_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5DMPIO_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

TEST(H5DmpioTest, DefaultBuildDisablesParallelAndExportsMpioConstants) {
#ifdef H5_HAVE_PARALLEL
  ADD_FAILURE() << "Expected H5_HAVE_PARALLEL to be disabled in default build.";
#endif
#ifdef H5_HAVE_PARALLEL_FILTERED_WRITES
  ADD_FAILURE() << "Expected H5_HAVE_PARALLEL_FILTERED_WRITES to be disabled.";
#endif

  // Disabled MPI-based driver sentinels and VFD identifier constants.
  EXPECT_EQ(H5FD_MPIO, H5I_INVALID_HID);
  EXPECT_EQ(H5FD_SUBFILING, H5I_INVALID_HID);
  EXPECT_EQ(H5FD_IOC, H5I_INVALID_HID);
  EXPECT_EQ(H5_VFD_MPIO, 7);

  // Chunk collective optimization thresholds (H5FDmpi.h).
  EXPECT_EQ(H5D_ONE_LINK_CHUNK_IO_THRESHOLD, 0);
  EXPECT_EQ(H5D_MULTI_CHUNK_IO_COL_THRESHOLD, 60);

  // MPI-IO transfer mode, chunk optimization, and collective optimization enums.
  EXPECT_EQ(H5FD_MPIO_INDEPENDENT, 0);
  EXPECT_EQ(H5FD_MPIO_COLLECTIVE, 1);

  EXPECT_EQ(H5FD_MPIO_CHUNK_DEFAULT, 0);
  EXPECT_EQ(H5FD_MPIO_CHUNK_ONE_IO, 1);
  EXPECT_EQ(H5FD_MPIO_CHUNK_MULTI_IO, 2);

  EXPECT_EQ(H5FD_MPIO_COLLECTIVE_IO, 0);
  EXPECT_EQ(H5FD_MPIO_INDIVIDUAL_IO, 1);

  // Actual chunk optimization mode enum (H5Ppublic.h).
  EXPECT_EQ(H5D_MPIO_NO_CHUNK_OPTIMIZATION, 0);
  EXPECT_EQ(H5D_MPIO_LINK_CHUNK, 1);
  EXPECT_EQ(H5D_MPIO_MULTI_CHUNK, 2);

  // Actual I/O mode bitfield enum (H5Ppublic.h).
  EXPECT_EQ(H5D_MPIO_NO_COLLECTIVE, 0x0);
  EXPECT_EQ(H5D_MPIO_CHUNK_INDEPENDENT, 0x1);
  EXPECT_EQ(H5D_MPIO_CHUNK_COLLECTIVE, 0x2);
  EXPECT_EQ(H5D_MPIO_CHUNK_MIXED,
            H5D_MPIO_CHUNK_INDEPENDENT | H5D_MPIO_CHUNK_COLLECTIVE);
  EXPECT_EQ(H5D_MPIO_CONTIGUOUS_COLLECTIVE, 0x4);

  // No-collective cause bitmask constants (H5Ppublic.h).
  EXPECT_EQ(H5D_MPIO_COLLECTIVE, 0x00);
  EXPECT_EQ(H5D_MPIO_SET_INDEPENDENT, 0x01);
  EXPECT_EQ(H5D_MPIO_DATATYPE_CONVERSION, 0x02);
  EXPECT_EQ(H5D_MPIO_DATA_TRANSFORMS, 0x04);
  EXPECT_EQ(H5D_MPIO_MPI_OPT_TYPES_ENV_VAR_DISABLED, 0x08);
  EXPECT_EQ(H5D_MPIO_NOT_SIMPLE_OR_SCALAR_DATASPACES, 0x10);
  EXPECT_EQ(H5D_MPIO_NOT_CONTIGUOUS_OR_CHUNKED_DATASET, 0x20);
  EXPECT_EQ(H5D_MPIO_PARALLEL_FILTERED_WRITES_DISABLED, 0x40);
  EXPECT_EQ(H5D_MPIO_ERROR_WHILE_CHECKING_COLLECTIVE_POSSIBLE, 0x80);
  EXPECT_EQ(H5D_MPIO_NO_SELECTION_IO, 0x100);
  EXPECT_EQ(H5D_MPIO_NO_COLLECTIVE_MAX_CAUSE, 0x200);

  // Selection I/O cause aggregation mask and actual selection I/O flags.
  EXPECT_EQ(H5D_MPIO_NO_SELECTION_IO_CAUSES,
            H5D_SEL_IO_DISABLE_BY_API | H5D_SEL_IO_TCONV_BUF_TOO_SMALL |
                H5D_SEL_IO_BKG_BUF_TOO_SMALL | H5D_SEL_IO_DATASET_FILTER |
                H5D_SEL_IO_CHUNK_CACHE);
  EXPECT_EQ(H5D_SCALAR_IO, 0x0001u);
  EXPECT_EQ(H5D_VECTOR_IO, 0x0002u);
  EXPECT_EQ(H5D_SELECTION_IO, 0x0004u);
}

TEST(H5DmpioTest, DisabledMpioDriverAndSerialDriverFlagsReportNoMpi) {
  {
    ScopedErrorSilence silence;
    unsigned long flags = 0;
    EXPECT_LT(H5FDdriver_query(H5FD_MPIO, &flags), 0);
    EXPECT_LT(H5FDdriver_query(H5FD_SUBFILING, &flags), 0);
    EXPECT_LT(H5FDdriver_query(H5FD_IOC, &flags), 0);
  }

  const std::array serial_drivers = {
      H5FD_CORE,   H5FD_SEC2,  H5FD_STDIO,    H5FD_LOG,
      H5FD_FAMILY, H5FD_MULTI, H5FD_SPLITTER, H5FD_ONION,
  };
  for (const hid_t driver_id : serial_drivers) {
    ASSERT_NE(H5I_INVALID_HID, driver_id);
    unsigned long flags = H5FD_FEAT_HAS_MPI;
    ASSERT_GE(H5FDdriver_query(driver_id, &flags), 0);
    EXPECT_EQ(flags & H5FD_FEAT_HAS_MPI, 0UL);
  }
}

TEST(H5DmpioTest, DxplRegistersMpioPropertiesWithExpectedDefaults) {
  struct PropSpec {
    const char* name;
    std::size_t size;
  };
  constexpr std::array<PropSpec, 9> kMpioDxplProps = {{
      {.name = H5D_XFER_IO_XFER_MODE_NAME, .size = sizeof(H5FD_mpio_xfer_t)},
      {.name = H5D_XFER_MPIO_COLLECTIVE_OPT_NAME,
       .size = sizeof(H5FD_mpio_collective_opt_t)},
      {.name = H5D_XFER_MPIO_CHUNK_OPT_HARD_NAME,
       .size = sizeof(H5FD_mpio_chunk_opt_t)},
      {.name = H5D_XFER_MPIO_CHUNK_OPT_NUM_NAME, .size = sizeof(unsigned)},
      {.name = H5D_XFER_MPIO_CHUNK_OPT_RATIO_NAME, .size = sizeof(unsigned)},
      {.name = H5D_MPIO_ACTUAL_CHUNK_OPT_MODE_NAME,
       .size = sizeof(H5D_mpio_actual_chunk_opt_mode_t)},
      {.name = H5D_MPIO_ACTUAL_IO_MODE_NAME,
       .size = sizeof(H5D_mpio_actual_io_mode_t)},
      {.name = H5D_MPIO_LOCAL_NO_COLLECTIVE_CAUSE_NAME,
       .size = sizeof(uint32_t)},
      {.name = H5D_MPIO_GLOBAL_NO_COLLECTIVE_CAUSE_NAME,
       .size = sizeof(uint32_t)},
  }};

  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(dxpl.valid());
  ASSERT_TRUE(fapl.valid());

  for (const PropSpec& spec : kMpioDxplProps) {
    EXPECT_GT(H5Pexist(dxpl.get(), spec.name), 0) << spec.name;
    EXPECT_EQ(H5Pexist(fapl.get(), spec.name), 0) << spec.name;
    std::size_t actual_size = 0;
    ASSERT_GE(H5Pget_size(dxpl.get(), spec.name, &actual_size), 0) << spec.name;
    EXPECT_EQ(actual_size, spec.size) << spec.name;
  }

  H5FD_mpio_xfer_t xfer_mode = H5FD_MPIO_COLLECTIVE;
  ASSERT_GE(H5Pget(dxpl.get(), H5D_XFER_IO_XFER_MODE_NAME, &xfer_mode), 0);
  EXPECT_EQ(xfer_mode, H5FD_MPIO_INDEPENDENT);

  H5FD_mpio_collective_opt_t coll_opt = H5FD_MPIO_INDIVIDUAL_IO;
  ASSERT_GE(H5Pget(dxpl.get(), H5D_XFER_MPIO_COLLECTIVE_OPT_NAME, &coll_opt),
            0);
  EXPECT_EQ(coll_opt, H5FD_MPIO_COLLECTIVE_IO);

  H5FD_mpio_chunk_opt_t chunk_opt = H5FD_MPIO_CHUNK_MULTI_IO;
  ASSERT_GE(H5Pget(dxpl.get(), H5D_XFER_MPIO_CHUNK_OPT_HARD_NAME, &chunk_opt),
            0);
  EXPECT_EQ(chunk_opt, H5FD_MPIO_CHUNK_DEFAULT);

  unsigned chunk_num = 999;
  ASSERT_GE(H5Pget(dxpl.get(), H5D_XFER_MPIO_CHUNK_OPT_NUM_NAME, &chunk_num),
            0);
  EXPECT_EQ(chunk_num, static_cast<unsigned>(H5D_ONE_LINK_CHUNK_IO_THRESHOLD));

  unsigned chunk_ratio = 999;
  ASSERT_GE(
      H5Pget(dxpl.get(), H5D_XFER_MPIO_CHUNK_OPT_RATIO_NAME, &chunk_ratio), 0);
  EXPECT_EQ(chunk_ratio,
            static_cast<unsigned>(H5D_MULTI_CHUNK_IO_COL_THRESHOLD));

  H5D_mpio_actual_chunk_opt_mode_t actual_chunk_opt = H5D_MPIO_MULTI_CHUNK;
  ASSERT_GE(H5Pget(dxpl.get(), H5D_MPIO_ACTUAL_CHUNK_OPT_MODE_NAME,
                   &actual_chunk_opt),
            0);
  EXPECT_EQ(actual_chunk_opt, H5D_MPIO_NO_CHUNK_OPTIMIZATION);

  H5D_mpio_actual_io_mode_t actual_io_mode = H5D_MPIO_CONTIGUOUS_COLLECTIVE;
  ASSERT_GE(H5Pget(dxpl.get(), H5D_MPIO_ACTUAL_IO_MODE_NAME, &actual_io_mode),
            0);
  EXPECT_EQ(actual_io_mode, H5D_MPIO_NO_COLLECTIVE);

  uint32_t local_cause = 0xffu;
  uint32_t global_cause = 0xffu;
  ASSERT_GE(H5Pget(dxpl.get(), H5D_MPIO_LOCAL_NO_COLLECTIVE_CAUSE_NAME,
                   &local_cause),
            0);
  ASSERT_GE(H5Pget(dxpl.get(), H5D_MPIO_GLOBAL_NO_COLLECTIVE_CAUSE_NAME,
                   &global_cause),
            0);
  EXPECT_EQ(local_cause, static_cast<uint32_t>(H5D_MPIO_COLLECTIVE));
  EXPECT_EQ(global_cause, static_cast<uint32_t>(H5D_MPIO_COLLECTIVE));
}

TEST(H5DmpioTest, DxplMpioPropertiesRoundTripThroughSetCopyEncodeDecode) {
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ScopedPlist def_dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ASSERT_TRUE(def_dxpl.valid());

  const H5FD_mpio_xfer_t xfer_in = H5FD_MPIO_COLLECTIVE;
  const H5FD_mpio_collective_opt_t coll_opt_in = H5FD_MPIO_INDIVIDUAL_IO;
  const H5FD_mpio_chunk_opt_t chunk_opt_in = H5FD_MPIO_CHUNK_MULTI_IO;
  constexpr unsigned kChunkNumIn = 7;
  constexpr unsigned kChunkRatioIn = 85;

  ASSERT_GE(H5Pset(dxpl.get(), H5D_XFER_IO_XFER_MODE_NAME, &xfer_in), 0);
  ASSERT_GE(
      H5Pset(dxpl.get(), H5D_XFER_MPIO_COLLECTIVE_OPT_NAME, &coll_opt_in), 0);
  ASSERT_GE(
      H5Pset(dxpl.get(), H5D_XFER_MPIO_CHUNK_OPT_HARD_NAME, &chunk_opt_in), 0);
  ASSERT_GE(H5Pset(dxpl.get(), H5D_XFER_MPIO_CHUNK_OPT_NUM_NAME, &kChunkNumIn),
            0);
  ASSERT_GE(
      H5Pset(dxpl.get(), H5D_XFER_MPIO_CHUNK_OPT_RATIO_NAME, &kChunkRatioIn),
      0);

  EXPECT_EQ(H5Pequal(dxpl.get(), def_dxpl.get()), 0);

  ScopedPlist copied(H5Pcopy(dxpl.get()));
  ASSERT_TRUE(copied.valid());
  EXPECT_GT(H5Pequal(dxpl.get(), copied.get()), 0);

  // Set a return-only property (which intentionally has no encode/decode
  // callbacks in H5Pdxpl.c) on copied, and verify encoding drops it back to
  // default while preserving the five encodable MPIO configuration properties.
  const H5D_mpio_actual_io_mode_t non_enc_mode = H5D_MPIO_CONTIGUOUS_COLLECTIVE;
  ASSERT_GE(
      H5Pset(copied.get(), H5D_MPIO_ACTUAL_IO_MODE_NAME, &non_enc_mode), 0);
  EXPECT_EQ(H5Pequal(dxpl.get(), copied.get()), 0);

  std::size_t enc_size = 0;
  ASSERT_GE(H5Pencode2(copied.get(), nullptr, &enc_size, H5P_DEFAULT), 0);
  ASSERT_GT(enc_size, 0U);
  std::vector<uint8_t> encoded(enc_size);
  ASSERT_GE(H5Pencode2(copied.get(), encoded.data(), &enc_size, H5P_DEFAULT),
            0);

  ScopedPlist decoded(H5Pdecode(encoded.data()));
  ASSERT_TRUE(decoded.valid());
  EXPECT_GT(H5Pequal(dxpl.get(), decoded.get()), 0);

  H5FD_mpio_xfer_t xfer_out = H5FD_MPIO_INDEPENDENT;
  H5FD_mpio_collective_opt_t coll_opt_out = H5FD_MPIO_COLLECTIVE_IO;
  H5FD_mpio_chunk_opt_t chunk_opt_out = H5FD_MPIO_CHUNK_DEFAULT;
  unsigned chunk_num_out = 0;
  unsigned chunk_ratio_out = 0;
  H5D_mpio_actual_io_mode_t actual_io_out = H5D_MPIO_CONTIGUOUS_COLLECTIVE;

  ASSERT_GE(H5Pget(decoded.get(), H5D_XFER_IO_XFER_MODE_NAME, &xfer_out), 0);
  ASSERT_GE(
      H5Pget(decoded.get(), H5D_XFER_MPIO_COLLECTIVE_OPT_NAME, &coll_opt_out),
      0);
  ASSERT_GE(
      H5Pget(decoded.get(), H5D_XFER_MPIO_CHUNK_OPT_HARD_NAME, &chunk_opt_out),
      0);
  ASSERT_GE(
      H5Pget(decoded.get(), H5D_XFER_MPIO_CHUNK_OPT_NUM_NAME, &chunk_num_out),
      0);
  ASSERT_GE(H5Pget(decoded.get(), H5D_XFER_MPIO_CHUNK_OPT_RATIO_NAME,
                   &chunk_ratio_out),
            0);
  ASSERT_GE(
      H5Pget(decoded.get(), H5D_MPIO_ACTUAL_IO_MODE_NAME, &actual_io_out), 0);

  EXPECT_EQ(xfer_out, xfer_in);
  EXPECT_EQ(coll_opt_out, coll_opt_in);
  EXPECT_EQ(chunk_opt_out, chunk_opt_in);
  EXPECT_EQ(chunk_num_out, kChunkNumIn);
  EXPECT_EQ(chunk_ratio_out, kChunkRatioIn);
  EXPECT_EQ(actual_io_out, H5D_MPIO_NO_COLLECTIVE);
}

TEST(H5DmpioTest, NonParallelCoreDatasetIoKeepsSerialFallbackDefaults) {
  ScopedFile file = CreateInMemoryFile("h5d_mpio_serial.h5");
  ASSERT_TRUE(file.valid());

  constexpr std::array<hsize_t, 1> kDims = {8};
  constexpr std::array<hsize_t, 1> kChunkDims = {4};
  ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  ScopedPlist chunk_dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(chunk_dcpl.valid());
  ASSERT_GE(H5Pset_chunk(chunk_dcpl.get(), 1, kChunkDims.data()), 0);

  ScopedDataset contig_dset(H5Dcreate2(file.get(), "contig", H5T_NATIVE_INT32,
                                       space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                       H5P_DEFAULT));
  ScopedDataset chunked_dset(H5Dcreate2(file.get(), "chunked", H5T_NATIVE_INT32,
                                        space.get(), H5P_DEFAULT,
                                        chunk_dcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(contig_dset.valid());
  ASSERT_TRUE(chunked_dset.valid());

  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  const H5FD_mpio_xfer_t xfer = H5FD_MPIO_COLLECTIVE;
  const H5FD_mpio_collective_opt_t coll_opt = H5FD_MPIO_INDIVIDUAL_IO;
  const H5FD_mpio_chunk_opt_t chunk_opt = H5FD_MPIO_CHUNK_ONE_IO;
  constexpr unsigned kChunkNum = 2;
  constexpr unsigned kChunkRatio = 50;
  ASSERT_GE(H5Pset(dxpl.get(), H5D_XFER_IO_XFER_MODE_NAME, &xfer), 0);
  ASSERT_GE(H5Pset(dxpl.get(), H5D_XFER_MPIO_COLLECTIVE_OPT_NAME, &coll_opt),
            0);
  ASSERT_GE(H5Pset(dxpl.get(), H5D_XFER_MPIO_CHUNK_OPT_HARD_NAME, &chunk_opt),
            0);
  ASSERT_GE(H5Pset(dxpl.get(), H5D_XFER_MPIO_CHUNK_OPT_NUM_NAME, &kChunkNum),
            0);
  ASSERT_GE(
      H5Pset(dxpl.get(), H5D_XFER_MPIO_CHUNK_OPT_RATIO_NAME, &kChunkRatio), 0);

  constexpr std::array<int32_t, 8> kWriteData = {10, 20, 30, 40,
                                                 50, 60, 70, 80};
  ASSERT_GE(H5Dwrite(contig_dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     dxpl.get(), kWriteData.data()),
            0);
  ASSERT_GE(H5Dwrite(chunked_dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     dxpl.get(), kWriteData.data()),
            0);

  std::array<int32_t, 8> contig_read;
  contig_read.fill(-1);
  std::array<int32_t, 8> chunked_read;
  chunked_read.fill(-1);
  ASSERT_GE(H5Dread(contig_dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                    dxpl.get(), contig_read.data()),
            0);
  ASSERT_GE(H5Dread(chunked_dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                    dxpl.get(), chunked_read.data()),
            0);

  EXPECT_THAT(contig_read, ElementsAre(10, 20, 30, 40, 50, 60, 70, 80));
  EXPECT_THAT(chunked_read, ElementsAre(10, 20, 30, 40, 50, 60, 70, 80));

  H5D_mpio_actual_chunk_opt_mode_t actual_chunk_opt = H5D_MPIO_LINK_CHUNK;
  H5D_mpio_actual_io_mode_t actual_io_mode = H5D_MPIO_CHUNK_COLLECTIVE;
  uint32_t local_cause = 0xffu;
  uint32_t global_cause = 0xffu;
  ASSERT_GE(H5Pget(dxpl.get(), H5D_MPIO_ACTUAL_CHUNK_OPT_MODE_NAME,
                   &actual_chunk_opt),
            0);
  ASSERT_GE(H5Pget(dxpl.get(), H5D_MPIO_ACTUAL_IO_MODE_NAME, &actual_io_mode),
            0);
  ASSERT_GE(H5Pget(dxpl.get(), H5D_MPIO_LOCAL_NO_COLLECTIVE_CAUSE_NAME,
                   &local_cause),
            0);
  ASSERT_GE(H5Pget(dxpl.get(), H5D_MPIO_GLOBAL_NO_COLLECTIVE_CAUSE_NAME,
                   &global_cause),
            0);

  EXPECT_EQ(actual_chunk_opt, H5D_MPIO_NO_CHUNK_OPTIMIZATION);
  EXPECT_EQ(actual_io_mode, H5D_MPIO_NO_COLLECTIVE);
  EXPECT_EQ(local_cause, static_cast<uint32_t>(H5D_MPIO_COLLECTIVE));
  EXPECT_EQ(global_cause, static_cast<uint32_t>(H5D_MPIO_COLLECTIVE));
}

template <typename T>
concept HasMpioComm = requires(const T& info) { info.comm; };

template <typename T>
concept HasUsingMpiVfd = requires(const T& info) { info.using_mpi_vfd; };

template <typename T>
concept HasActualIoMode = requires(const T& info) { info.actual_io_mode; };

template <typename T>
concept HasIoInfoBaseFields = requires(const T& info) {
  info.f_sh;
  info.md_io_ops;
  info.count;
  info.use_select_io;
};

TEST(H5DmpioTest, InternalIoInfoLayoutOmitsParallelFieldsInNonParallelBuild) {
  static_assert(HasIoInfoBaseFields<H5D_io_info_t>);
  static_assert(!HasMpioComm<H5D_io_info_t>);
  static_assert(!HasUsingMpiVfd<H5D_io_info_t>);
  static_assert(!HasActualIoMode<H5D_io_info_t>);

  const H5D_io_info_t io_info{};
  EXPECT_EQ(io_info.f_sh, nullptr);
  EXPECT_EQ(io_info.count, 0U);
  EXPECT_EQ(io_info.md_io_ops.multi_read_md, nullptr);
  EXPECT_EQ(io_info.md_io_ops.multi_write_md, nullptr);
  EXPECT_EQ(io_info.md_io_ops.single_read_md, nullptr);
  EXPECT_EQ(io_info.md_io_ops.single_write_md, nullptr);
  EXPECT_EQ(io_info.use_select_io, H5D_SELECTION_IO_MODE_DEFAULT);
}

}  // namespace
}  // namespace hdf5
