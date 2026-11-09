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
// Tests Object Header Group Information (ginfo) messages: encoding, decoding,
// sizing, copying, freeing, debug formatting, corrupt buffer rejection, and
// group creation property list persistence (third_party/hdf5/src/H5Oginfo.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Oginfo.c

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
#define H5OGINFO_TEST_RESTORE_UNDEF_NDEBUG 1
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
#include "third_party/hdf5/src/H5Gprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#undef try
#pragma pop_macro("try")
#ifdef H5OGINFO_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5OGINFO_TEST_RESTORE_UNDEF_NDEBUG
#endif
// clang-format on

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

// RAII wrapper for HDF5 internal API context.
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

struct GinfoCase {
  bool store_link_phase_change;
  uint16_t max_compact;
  uint16_t min_dense;
  bool store_est_entry_info;
  uint16_t est_num_entries;
  uint16_t est_name_len;
  size_t expected_raw_size;
};

constexpr auto kGinfoRoundTripCases = std::to_array<GinfoCase>({
    {.store_link_phase_change = false,
     .max_compact = H5G_CRT_GINFO_MAX_COMPACT,
     .min_dense = H5G_CRT_GINFO_MIN_DENSE,
     .store_est_entry_info = false,
     .est_num_entries = H5G_CRT_GINFO_EST_NUM_ENTRIES,
     .est_name_len = H5G_CRT_GINFO_EST_NAME_LEN,
     .expected_raw_size = 2},
    {.store_link_phase_change = true,
     .max_compact = 16,
     .min_dense = 5,
     .store_est_entry_info = false,
     .est_num_entries = H5G_CRT_GINFO_EST_NUM_ENTRIES,
     .est_name_len = H5G_CRT_GINFO_EST_NAME_LEN,
     .expected_raw_size = 6},
    {.store_link_phase_change = false,
     .max_compact = H5G_CRT_GINFO_MAX_COMPACT,
     .min_dense = H5G_CRT_GINFO_MIN_DENSE,
     .store_est_entry_info = true,
     .est_num_entries = 42,
     .est_name_len = 31,
     .expected_raw_size = 6},
    {.store_link_phase_change = true,
     .max_compact = 20,
     .min_dense = 10,
     .store_est_entry_info = true,
     .est_num_entries = 100,
     .est_name_len = 64,
     .expected_raw_size = 10},
});

TEST(H5OginfoTest, EncodeDecodeSizeCopyFreeAllFlagCombinations) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  auto* f = static_cast<H5F_t*>(H5VL_object(file.get()));
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  for (const auto& tc : kGinfoRoundTripCases) {
    H5O_ginfo_t src{};
    src.lheap_size_hint = 128;
    src.store_link_phase_change = tc.store_link_phase_change;
    src.max_compact = tc.max_compact;
    src.min_dense = tc.min_dense;
    src.store_est_entry_info = tc.store_est_entry_info;
    src.est_num_entries = tc.est_num_entries;
    src.est_name_len = tc.est_name_len;

    const size_t raw_size =
        H5O_msg_raw_size(f, H5O_GINFO_ID, /*disable_shared=*/false, &src);
    ASSERT_EQ(raw_size, tc.expected_raw_size);

    std::vector<uint8_t> encoded(raw_size, 0xff);
    ASSERT_GE(H5O_msg_encode(f, H5O_GINFO_ID, /*disable_shared=*/false,
                             encoded.data(), &src),
              0);

    auto* decoded = static_cast<H5O_ginfo_t*>(H5O_msg_decode(
        f, /*open_oh=*/nullptr, H5O_GINFO_ID, encoded.size(), encoded.data()));
    ASSERT_NE(decoded, nullptr);
    const auto free_decoded =
        absl::MakeCleanup([decoded] { H5O_msg_free(H5O_GINFO_ID, decoded); });

    EXPECT_EQ(decoded->store_link_phase_change, tc.store_link_phase_change);
    EXPECT_EQ(decoded->store_est_entry_info, tc.store_est_entry_info);
    // When flags are unset, decode restores HDF5's group info defaults.
    EXPECT_EQ(decoded->max_compact, tc.store_link_phase_change
                                        ? tc.max_compact
                                        : H5G_CRT_GINFO_MAX_COMPACT);
    EXPECT_EQ(decoded->min_dense,
              tc.store_link_phase_change ? tc.min_dense : H5G_CRT_GINFO_MIN_DENSE);
    EXPECT_EQ(decoded->est_num_entries, tc.store_est_entry_info
                                            ? tc.est_num_entries
                                            : H5G_CRT_GINFO_EST_NUM_ENTRIES);
    EXPECT_EQ(decoded->est_name_len, tc.store_est_entry_info
                                         ? tc.est_name_len
                                         : H5G_CRT_GINFO_EST_NAME_LEN);

    // Exercise H5O__ginfo_copy both with an existing destination struct and
    // allocating a fresh destination struct.
    H5O_ginfo_t copy_in_place{};
    ASSERT_EQ(H5O_msg_copy(H5O_GINFO_ID, decoded, &copy_in_place),
              &copy_in_place);
    EXPECT_EQ(copy_in_place.max_compact, decoded->max_compact);
    EXPECT_EQ(copy_in_place.min_dense, decoded->min_dense);
    EXPECT_EQ(copy_in_place.est_num_entries, decoded->est_num_entries);
    EXPECT_EQ(copy_in_place.est_name_len, decoded->est_name_len);

    auto* copy_alloc =
        static_cast<H5O_ginfo_t*>(H5O_msg_copy(H5O_GINFO_ID, decoded, nullptr));
    ASSERT_NE(copy_alloc, nullptr);
    const auto free_copy = absl::MakeCleanup(
        [copy_alloc] { H5O_msg_free(H5O_GINFO_ID, copy_alloc); });
    EXPECT_EQ(copy_alloc->store_link_phase_change,
              decoded->store_link_phase_change);
    EXPECT_EQ(copy_alloc->store_est_entry_info, decoded->store_est_entry_info);
    EXPECT_EQ(copy_alloc->est_num_entries, decoded->est_num_entries);
  }
}

TEST(H5OginfoTest, DecodeRejectsTruncatedInvalidVersionAndBadFlags) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  auto* f = static_cast<H5F_t*>(H5VL_object(file.get()));
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  ScopedErrorSilence silence;

  // Empty buffer check (`p_end = p + 0 - 1` remains within backing array).
  constexpr std::array<uint8_t, 2> kDummy = {0, 0};
  EXPECT_EQ(
      H5O_msg_decode(f, nullptr, H5O_GINFO_ID, 0, kDummy.data() + 1),
      nullptr);

  // Bad message version (only version 0 is valid).
  constexpr auto kBadVersion = std::to_array<uint8_t>({1, 0});
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_GINFO_ID, kBadVersion.size(),
                           kBadVersion.data()),
            nullptr);

  // Truncated before flags byte (version byte only).
  constexpr auto kMissingFlags = std::to_array<uint8_t>({0});
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_GINFO_ID, kMissingFlags.size(),
                           kMissingFlags.data()),
            nullptr);

  // Invalid flag bit outside 0x01 | 0x02.
  constexpr auto kBadFlags = std::to_array<uint8_t>({0, 0x04});
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_GINFO_ID, kBadFlags.size(),
                           kBadFlags.data()),
            nullptr);

  // STORE_PHASE_CHANGE set, but fewer than 4 remaining bytes for uint16 pair.
  constexpr auto kTruncatedPhaseChange =
      std::to_array<uint8_t>({0, 0x01, 8, 0, 6});
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_GINFO_ID,
                           kTruncatedPhaseChange.size(),
                           kTruncatedPhaseChange.data()),
            nullptr);

  // STORE_EST_ENTRY_INFO set, but fewer than 4 remaining bytes for uint16 pair.
  constexpr auto kTruncatedEstInfo =
      std::to_array<uint8_t>({0, 0x02, 4, 0, 8});
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_GINFO_ID, kTruncatedEstInfo.size(),
                           kTruncatedEstInfo.data()),
            nullptr);

  // Both flags set, truncated during estimated entry info.
  constexpr auto kTruncatedBoth =
      std::to_array<uint8_t>({0, 0x03, 8, 0, 6, 0, 4, 0});
  EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_GINFO_ID, kTruncatedBoth.size(),
                           kTruncatedBoth.data()),
            nullptr);
}

TEST(H5OginfoTest, DebugFormatsAllFields) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  auto* f = static_cast<H5F_t*>(H5VL_object(file.get()));
  ASSERT_NE(f, nullptr);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  H5O_ginfo_t ginfo{};
  ginfo.store_link_phase_change = true;
  ginfo.max_compact = 15;
  ginfo.min_dense = 7;
  ginfo.store_est_entry_info = true;
  ginfo.est_num_entries = 12;
  ginfo.est_name_len = 28;

  std::array<char, 512> out_buf{};
  FILE* stream = fmemopen(out_buf.data(), out_buf.size(), "w");
  ASSERT_NE(stream, nullptr);
  const auto close_stream = absl::MakeCleanup([stream] { fclose(stream); });

  ASSERT_GE(H5O_debug_id(H5O_GINFO_ID, f, &ginfo, stream, /*indent=*/2,
                         /*fwidth=*/44),
            0);
  ASSERT_EQ(fflush(stream), 0);

  const std::string output(out_buf.data());
  EXPECT_THAT(output, HasSubstr("Max. compact links:"));
  EXPECT_THAT(output, HasSubstr("15"));
  EXPECT_THAT(output, HasSubstr("Min. dense links:"));
  EXPECT_THAT(output, HasSubstr("7"));
  EXPECT_THAT(output, HasSubstr("Estimated # of objects in group:"));
  EXPECT_THAT(output, HasSubstr("12"));
  EXPECT_THAT(output, HasSubstr("Estimated length of object in group's name:"));
  EXPECT_THAT(output, HasSubstr("28"));
}

TEST(H5OginfoTest, GroupCreatePlistPersistsAndReloadsGinfoMessage) {
  // 1. Configure Core VFD with V18+ libver bounds so groups use new-format
  // object headers carrying the H5O_GINFO_ID message.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST),
            0);

  ScopedFile file(H5Fcreate("ginfo_persist.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                            fapl.get()));
  ASSERT_TRUE(file.valid());

  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  ASSERT_GE(H5Pset_link_phase_change(gcpl.get(), 12, 4), 0);
  ASSERT_GE(H5Pset_est_link_info(gcpl.get(), 11, 25), 0);

  ScopedGroup custom_grp(
      H5Gcreate2(file.get(), "/custom", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(custom_grp.valid());
  ScopedGroup default_grp(
      H5Gcreate2(file.get(), "/default", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(default_grp.valid());

  custom_grp.reset();
  default_grp.reset();
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

  // 2. Capture the in-memory image before closing and reopen read-only.
  const ssize_t image_size = H5Fget_file_image(file.get(), nullptr, 0);
  ASSERT_GT(image_size, 0);
  std::vector<uint8_t> image(static_cast<size_t>(image_size));
  ASSERT_EQ(H5Fget_file_image(file.get(), image.data(), image.size()),
            image_size);
  file.reset();

  ScopedPlist ro_fapl = MakeCoreFapl();
  ASSERT_TRUE(ro_fapl.valid());
  ASSERT_GE(H5Pset_file_image(ro_fapl.get(), image.data(), image.size()), 0);
  ScopedFile ro_file(
      H5Fopen("ginfo_persist.h5", H5F_ACC_RDONLY, ro_fapl.get()));
  ASSERT_TRUE(ro_file.valid());

  // 3. Verify custom GCPL values round-tripped through file encoding/decoding.
  ScopedGroup ro_custom(H5Gopen2(ro_file.get(), "/custom", H5P_DEFAULT));
  ASSERT_TRUE(ro_custom.valid());
  ScopedPlist ro_gcpl(H5Gget_create_plist(ro_custom.get()));
  ASSERT_TRUE(ro_gcpl.valid());

  unsigned max_compact = 0;
  unsigned min_dense = 0;
  ASSERT_GE(H5Pget_link_phase_change(ro_gcpl.get(), &max_compact, &min_dense),
            0);
  EXPECT_EQ(max_compact, 12u);
  EXPECT_EQ(min_dense, 4u);

  unsigned est_entries = 0;
  unsigned est_name_len = 0;
  ASSERT_GE(H5Pget_est_link_info(ro_gcpl.get(), &est_entries, &est_name_len),
            0);
  EXPECT_EQ(est_entries, 11u);
  EXPECT_EQ(est_name_len, 25u);

  // 4. Exercise H5O_debug on the real group's object header (hits ginfo decode &
  // debug on disk).
  auto* f = static_cast<H5F_t*>(H5VL_object(ro_file.get()));
  H5O_loc_t* oloc = H5O_get_loc(ro_custom.get());
  ASSERT_NE(f, nullptr);
  ASSERT_NE(oloc, nullptr);
  std::vector<char> oh_buf(16384, '\0');
  FILE* stream = fmemopen(oh_buf.data(), oh_buf.size(), "w");
  ASSERT_NE(stream, nullptr);
  const auto close_stream = absl::MakeCleanup([stream] { fclose(stream); });

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  ASSERT_GE(H5O_debug(f, oloc->addr, stream, /*indent=*/0, /*fwidth=*/30), 0);
  ASSERT_EQ(fflush(stream), 0);
  EXPECT_THAT(std::string(oh_buf.data()), HasSubstr("Max. compact links:"));
}

}  // namespace
}  // namespace hdf5
