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
// Tests Object Header File Space Info (fsinfo) messages: v1 encode/decode/size,
// v0 deprecated decode mapping, copy/free, version bounds checks, debug
// formatting, and FCPL persistence (third_party/hdf5/src/H5Ofsinfo.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Ofsinfo.c

#include <stdio.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// clang-format off
// `:hdf5` is built with -DNDEBUG, omitting debug fields from cache/object
// header structs. Define NDEBUG around internal headers so struct offsets match.
#ifndef NDEBUG
#define H5OFSINFO_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
// H5Fpkg.h's H5F__efc_open uses `try` as a parameter name, which is both a C++
// keyword and macro-defined under -fno-exceptions.
#pragma push_macro("try")
#undef try
#define try h5f_try_param
extern "C" {
#define H5F_FRIEND
#define H5O_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#undef try
#pragma pop_macro("try")
#ifdef H5OFSINFO_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5OFSINFO_TEST_RESTORE_UNDEF_NDEBUG
#endif
// clang-format on

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

class ScopedApiContext {
 public:
  ScopedApiContext() {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
    }
  }

  ~ScopedApiContext() {
    if (pushed_) {
      H5CX_pop(false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

  bool valid() const { return pushed_; }

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

H5O_fsinfo_t MakeSampleFsinfo(H5F_fspace_strategy_t strategy, bool persist) {
  H5O_fsinfo_t info{};
  info.version = H5O_FSINFO_VERSION_1;
  info.strategy = strategy;
  info.persist = persist;
  info.threshold = 16;
  info.page_size = 4096;
  info.pgend_meta_thres = 128;
  info.eoa_pre_fsm_fsalloc = 2048;
  for (int i = 0; i < H5F_MEM_PAGE_NTYPES - 1; ++i) {
    info.fs_addr[i] = persist ? static_cast<haddr_t>(4096 + i * 64) : HADDR_UNDEF;
  }
  return info;
}

TEST(H5OfsinfoTest, SetAndCheckVersionBounds) {
  H5O_fsinfo_t fsinfo{};
  EXPECT_GE(
      H5O_fsinfo_set_version(H5F_LIBVER_EARLIEST, H5F_LIBVER_V110, &fsinfo),
      0);
  EXPECT_EQ(fsinfo.version, static_cast<unsigned>(H5O_FSINFO_VERSION_1));

  EXPECT_GE(H5O_fsinfo_set_version(H5F_LIBVER_V110, H5F_LIBVER_LATEST, &fsinfo),
            0);
  EXPECT_EQ(fsinfo.version, static_cast<unsigned>(H5O_FSINFO_VERSION_1));

  EXPECT_GE(H5O_fsinfo_check_version(H5F_LIBVER_V110, &fsinfo), 0);
  EXPECT_GE(H5O_fsinfo_check_version(H5F_LIBVER_LATEST, &fsinfo), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(
      H5O_fsinfo_set_version(H5F_LIBVER_EARLIEST, H5F_LIBVER_EARLIEST, &fsinfo),
      0);
  EXPECT_LT(H5O_fsinfo_set_version(H5F_LIBVER_V110, H5F_LIBVER_V18, &fsinfo),
            0);
  EXPECT_LT(H5O_fsinfo_check_version(H5F_LIBVER_EARLIEST, &fsinfo), 0);
  EXPECT_LT(H5O_fsinfo_check_version(H5F_LIBVER_V18, &fsinfo), 0);

  fsinfo.version = H5O_FSINFO_VERSION_1 + 1;
  EXPECT_LT(H5O_fsinfo_check_version(H5F_LIBVER_LATEST, &fsinfo), 0);
}

TEST(H5OfsinfoTest, EncodeDecodeSizeCopyFreeV1PersistentAndNonPersistent) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  auto* f = static_cast<H5F_t*>(H5VL_object(file.get()));
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  for (const bool persist : {false, true}) {
    const H5O_fsinfo_t src =
        MakeSampleFsinfo(H5F_FSPACE_STRATEGY_PAGE, persist);

    const size_t expected_size =
        3 + 2 * static_cast<size_t>(H5F_SIZEOF_SIZE(f)) + 2 +
        static_cast<size_t>(H5F_SIZEOF_ADDR(f)) +
        (persist ? (H5F_MEM_PAGE_NTYPES - 1) *
                       static_cast<size_t>(H5F_SIZEOF_ADDR(f))
                 : 0);
    const size_t raw_size =
        H5O_msg_raw_size(f, H5O_FSINFO_ID, /*disable_shared=*/false, &src);
    ASSERT_EQ(raw_size, expected_size);

    std::vector<uint8_t> encoded(raw_size, 0xff);
    ASSERT_GE(H5O_msg_encode(f, H5O_FSINFO_ID, /*disable_shared=*/false,
                             encoded.data(), &src),
              0);

    auto* decoded = static_cast<H5O_fsinfo_t*>(H5O_msg_decode(
        f, nullptr, H5O_FSINFO_ID, encoded.size(), encoded.data()));
    ASSERT_NE(decoded, nullptr);
    const auto free_decoded = absl::MakeCleanup(
        [decoded] { H5O_msg_free(H5O_FSINFO_ID, decoded); });

    EXPECT_EQ(decoded->version, static_cast<unsigned>(H5O_FSINFO_VERSION_1));
    EXPECT_EQ(decoded->strategy, src.strategy);
    EXPECT_EQ(decoded->persist, persist);
    EXPECT_EQ(decoded->threshold, src.threshold);
    EXPECT_EQ(decoded->page_size, src.page_size);
    EXPECT_EQ(decoded->pgend_meta_thres, src.pgend_meta_thres);
    EXPECT_EQ(decoded->eoa_pre_fsm_fsalloc, src.eoa_pre_fsm_fsalloc);
    EXPECT_FALSE(decoded->mapped);
    for (int i = 0; i < H5F_MEM_PAGE_NTYPES - 1; ++i) {
      EXPECT_EQ(decoded->fs_addr[i], src.fs_addr[i]);
    }

    H5O_fsinfo_t copy_in_place{};
    ASSERT_EQ(H5O_msg_copy(H5O_FSINFO_ID, decoded, &copy_in_place),
              &copy_in_place);
    EXPECT_EQ(copy_in_place.page_size, decoded->page_size);
    EXPECT_EQ(copy_in_place.persist, decoded->persist);

    auto* copy_alloc = static_cast<H5O_fsinfo_t*>(
        H5O_msg_copy(H5O_FSINFO_ID, decoded, nullptr));
    ASSERT_NE(copy_alloc, nullptr);
    const auto free_copy = absl::MakeCleanup(
        [copy_alloc] { H5O_msg_free(H5O_FSINFO_ID, copy_alloc); });
    EXPECT_EQ(copy_alloc->eoa_pre_fsm_fsalloc, decoded->eoa_pre_fsm_fsalloc);
  }
}

TEST(H5OfsinfoTest, DecodeDeprecatedV0MapsAllStrategiesAndRejectsInvalid) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  auto* f = static_cast<H5F_t*>(H5VL_object(file.get()));
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  const size_t sizeof_size = H5F_SIZEOF_SIZE(f);
  const size_t sizeof_addr = H5F_SIZEOF_ADDR(f);
  const haddr_t expected_eoa = H5F_get_eoa(f, H5FD_MEM_DEFAULT);
  ASSERT_NE(expected_eoa, HADDR_UNDEF);

  auto build_v0 = [&](uint8_t strategy_byte, hsize_t threshold,
                      bool with_persist_addrs) {
    std::vector<uint8_t> buf(
        2 + sizeof_size +
        (with_persist_addrs ? (H5FD_MEM_NTYPES - 1) * sizeof_addr : 0));
    uint8_t* p = buf.data();
    *p++ = H5O_FSINFO_VERSION_0;
    *p++ = strategy_byte;
    H5F_ENCODE_LENGTH(f, p, threshold);
    if (with_persist_addrs) {
      for (int t = H5FD_MEM_SUPER; t < H5FD_MEM_NTYPES; ++t) {
        H5F_addr_encode(f, &p, static_cast<haddr_t>(8192 + t * 128));
      }
    }
    return buf;
  };

  // 1. ALL_PERSIST maps to FSM_AGGR + persist=true + V1 + H5F_get_eoa + 6 addrs.
  {
    const std::vector<uint8_t> buf =
        build_v0(static_cast<uint8_t>(H5F_FILE_SPACE_ALL_PERSIST), 512, true);
    auto* decoded = static_cast<H5O_fsinfo_t*>(
        H5O_msg_decode(f, nullptr, H5O_FSINFO_ID, buf.size(), buf.data()));
    ASSERT_NE(decoded, nullptr);
    const auto cleanup = absl::MakeCleanup(
        [decoded] { H5O_msg_free(H5O_FSINFO_ID, decoded); });
    EXPECT_EQ(decoded->version, static_cast<unsigned>(H5O_FSINFO_VERSION_1));
    EXPECT_TRUE(decoded->mapped);
    EXPECT_EQ(decoded->strategy, H5F_FSPACE_STRATEGY_FSM_AGGR);
    EXPECT_TRUE(decoded->persist);
    EXPECT_EQ(decoded->threshold, 512u);
    EXPECT_EQ(decoded->eoa_pre_fsm_fsalloc, expected_eoa);
    for (int t = H5FD_MEM_SUPER; t < H5FD_MEM_NTYPES; ++t) {
      EXPECT_EQ(decoded->fs_addr[t - 1], static_cast<haddr_t>(8192 + t * 128));
    }

    // Truncating inside the persistent free-space manager addresses fails.
    ScopedErrorSilence silence;
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_FSINFO_ID, buf.size() - 1,
                             buf.data()),
              nullptr);
  }

  // 2. Remaining valid deprecated strategies map cleanly to v1 strategies.
  struct V0MapCase {
    H5F_file_space_type_t v0_strategy;
    H5F_fspace_strategy_t expected_v1;
    hsize_t expected_threshold;
  };
  constexpr auto kV0Cases = std::to_array<V0MapCase>({
      {.v0_strategy = H5F_FILE_SPACE_ALL,
       .expected_v1 = H5F_FSPACE_STRATEGY_FSM_AGGR,
       .expected_threshold = 256},
      {.v0_strategy = H5F_FILE_SPACE_AGGR_VFD,
       .expected_v1 = H5F_FSPACE_STRATEGY_AGGR,
       .expected_threshold = H5F_FREE_SPACE_THRESHOLD_DEF},
      {.v0_strategy = H5F_FILE_SPACE_VFD,
       .expected_v1 = H5F_FSPACE_STRATEGY_NONE,
       .expected_threshold = H5F_FREE_SPACE_THRESHOLD_DEF},
  });
  for (const auto& tc : kV0Cases) {
    const std::vector<uint8_t> buf =
        build_v0(static_cast<uint8_t>(tc.v0_strategy), 256, false);
    auto* decoded = static_cast<H5O_fsinfo_t*>(
        H5O_msg_decode(f, nullptr, H5O_FSINFO_ID, buf.size(), buf.data()));
    ASSERT_NE(decoded, nullptr);
    const auto cleanup = absl::MakeCleanup(
        [decoded] { H5O_msg_free(H5O_FSINFO_ID, decoded); });
    EXPECT_TRUE(decoded->mapped);
    EXPECT_EQ(decoded->strategy, tc.expected_v1);
    EXPECT_FALSE(decoded->persist);
    EXPECT_EQ(decoded->threshold, tc.expected_threshold);
  }

  // 3. Invalid deprecated strategies and truncated v0 buffers fail.
  ScopedErrorSilence silence;
  for (const uint8_t bad_strat : {static_cast<uint8_t>(H5F_FILE_SPACE_DEFAULT),
                                  static_cast<uint8_t>(H5F_FILE_SPACE_NTYPES),
                                  static_cast<uint8_t>(99)}) {
    const std::vector<uint8_t> buf = build_v0(bad_strat, 1, false);
    EXPECT_EQ(
        H5O_msg_decode(f, nullptr, H5O_FSINFO_ID, buf.size(), buf.data()),
        nullptr);
  }
  const std::vector<uint8_t> trunc_v0 =
      build_v0(static_cast<uint8_t>(H5F_FILE_SPACE_ALL), 1, false);
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_FSINFO_ID, trunc_v0.size() - 1,
                           trunc_v0.data()),
            nullptr);
}

TEST(H5OfsinfoTest, DecodeV1RejectsEveryTruncatedOffsetAndInvalidPageSize) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  auto* f = static_cast<H5F_t*>(H5VL_object(file.get()));
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  ScopedErrorSilence silence;

  // Empty buffer check (`p_end = p + 0 - 1` stays within dummy backing array).
  constexpr std::array<uint8_t, 2> kDummy = {0, 0};
  EXPECT_EQ(
      H5O_msg_decode(f, nullptr, H5O_FSINFO_ID, 0, kDummy.data() + 1),
      nullptr);

  const H5O_fsinfo_t src = MakeSampleFsinfo(H5F_FSPACE_STRATEGY_PAGE, true);
  const size_t full_size = H5O_msg_raw_size(f, H5O_FSINFO_ID, false, &src);
  std::vector<uint8_t> encoded(full_size, 0);
  ASSERT_GE(H5O_msg_encode(f, H5O_FSINFO_ID, false, encoded.data(), &src), 0);

  const size_t sizeof_size = H5F_SIZEOF_SIZE(f);
  const size_t sizeof_addr = H5F_SIZEOF_ADDR(f);
  // Test every field boundary truncation in H5O__fsinfo_decode v1.
  const std::vector<size_t> trunc_lengths = {
      1,                                   // Before strategy & persist
      3 + sizeof_size - 1,                 // Before threshold
      3 + 2 * sizeof_size - 1,             // Before page_size
      3 + 2 * sizeof_size + 1,             // Before pgend_meta_thres
      3 + 2 * sizeof_size + 2 + sizeof_addr - 1,  // Before eoa_pre_fsm_fsalloc
      full_size - 1,                       // Inside fs_addr array
  };
  for (const size_t trunc : trunc_lengths) {
    EXPECT_EQ(
        H5O_msg_decode(f, nullptr, H5O_FSINFO_ID, trunc, encoded.data()),
        nullptr)
        << "trunc=" << trunc;
  }

  // Invalid page_size values (0 and > H5F_FILE_SPACE_PAGE_SIZE_MAX).
  for (const hsize_t bad_page_size :
       {static_cast<hsize_t>(0),
        static_cast<hsize_t>(H5F_FILE_SPACE_PAGE_SIZE_MAX) + 1}) {
    H5O_fsinfo_t bad_src = MakeSampleFsinfo(H5F_FSPACE_STRATEGY_PAGE, false);
    bad_src.page_size = bad_page_size;
    std::vector<uint8_t> bad_buf(
        H5O_msg_raw_size(f, H5O_FSINFO_ID, false, &bad_src));
    ASSERT_GE(
        H5O_msg_encode(f, H5O_FSINFO_ID, false, bad_buf.data(), &bad_src), 0);
    EXPECT_EQ(
        H5O_msg_decode(f, nullptr, H5O_FSINFO_ID, bad_buf.size(), bad_buf.data()),
        nullptr);
  }
}

TEST(H5OfsinfoTest, DebugFormatsAllStrategiesAndPersistAddresses) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  auto* f = static_cast<H5F_t*>(H5VL_object(file.get()));
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  struct DebugExpectation {
    H5F_fspace_strategy_t strategy;
    bool persist;
    const char* expected_text;
  };
  constexpr auto kDebugCases = std::to_array<DebugExpectation>({
      {.strategy = H5F_FSPACE_STRATEGY_FSM_AGGR,
       .persist = false,
       .expected_text = "H5F_FSPACE_STRATEGY_FSM_AGGR"},
      {.strategy = H5F_FSPACE_STRATEGY_PAGE,
       .persist = true,
       .expected_text = "H5F_FSPACE_STRATEGY_PAGE"},
      {.strategy = H5F_FSPACE_STRATEGY_AGGR,
       .persist = false,
       .expected_text = "H5F_FSPACE_STRATEGY_AGGR"},
      {.strategy = H5F_FSPACE_STRATEGY_NONE,
       .persist = false,
       .expected_text = "H5F_FSPACE_STRATEGY_NONE"},
      {.strategy = H5F_FSPACE_STRATEGY_NTYPES,
       .persist = false,
       .expected_text = "unknown"},
  });

  for (const auto& tc : kDebugCases) {
    const H5O_fsinfo_t fsinfo = MakeSampleFsinfo(tc.strategy, tc.persist);
    std::array<char, 2048> out_buf{};
    FILE* stream = fmemopen(out_buf.data(), out_buf.size(), "w");
    ASSERT_NE(stream, nullptr);
    const auto close_stream = absl::MakeCleanup([stream] { fclose(stream); });

    ASSERT_GE(H5O_debug_id(H5O_FSINFO_ID, f, &fsinfo, stream, /*indent=*/2,
                           /*fwidth=*/36),
              0);
    ASSERT_EQ(fflush(stream), 0);

    const std::string output(out_buf.data());
    EXPECT_THAT(output, HasSubstr(tc.expected_text));
    EXPECT_THAT(output, HasSubstr(tc.persist ? "TRUE" : "FALSE"));
    if (tc.persist) {
      EXPECT_THAT(output, HasSubstr("Free space manager address:"));
    }
  }
}

TEST(H5OfsinfoTest, FcplFileSpaceStrategyRoundTripsThroughFile) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(
      H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_PAGE, true, 16),
      0);
  ASSERT_GE(H5Pset_file_space_page_size(fcpl.get(), 1024), 0);

  ScopedFile file =
      CreateInMemoryFile("fsinfo_page.h5", H5F_ACC_TRUNC, fcpl.get());
  ASSERT_TRUE(file.valid());

  const std::array<hsize_t, 1> dims = {64};
  ScopedSpace space(H5Screate_simple(1, dims.data(), nullptr));
  ASSERT_TRUE(space.valid());
  std::vector<int32_t> data(64, 7);
  {
    ScopedDataset dset1(H5Dcreate2(file.get(), "/d1", H5T_NATIVE_INT32,
                                   space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT));
    ASSERT_TRUE(dset1.valid());
    ASSERT_GE(H5Dwrite(dset1.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, data.data()),
              0);
  }
  // Delete a dataset to generate persistent free space tracked by fsinfo.
  ASSERT_GE(H5Ldelete(file.get(), "/d1", H5P_DEFAULT), 0);
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

  const ssize_t img_len = H5Fget_file_image(file.get(), nullptr, 0);
  ASSERT_GT(img_len, 0);
  std::vector<uint8_t> image(static_cast<size_t>(img_len));
  ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()), img_len);
  file.reset();

  ScopedPlist ro_fapl = MakeCoreFapl();
  ASSERT_TRUE(ro_fapl.valid());
  ASSERT_GE(H5Pset_file_image(ro_fapl.get(), image.data(), image.size()), 0);
  ScopedFile ro_file(H5Fopen("fsinfo_page.h5", H5F_ACC_RDONLY, ro_fapl.get()));
  ASSERT_TRUE(ro_file.valid());

  ScopedPlist ro_fcpl(H5Fget_create_plist(ro_file.get()));
  ASSERT_TRUE(ro_fcpl.valid());
  H5F_fspace_strategy_t strategy = H5F_FSPACE_STRATEGY_NTYPES;
  hbool_t persist = false;
  hsize_t threshold = 0;
  hsize_t page_size = 0;
  ASSERT_GE(
      H5Pget_file_space_strategy(ro_fcpl.get(), &strategy, &persist, &threshold),
      0);
  ASSERT_GE(H5Pget_file_space_page_size(ro_fcpl.get(), &page_size), 0);
  EXPECT_EQ(strategy, H5F_FSPACE_STRATEGY_PAGE);
  EXPECT_TRUE(persist);
  EXPECT_EQ(threshold, 16u);
  EXPECT_EQ(page_size, 1024u);
}

}  // namespace
}  // namespace hdf5
