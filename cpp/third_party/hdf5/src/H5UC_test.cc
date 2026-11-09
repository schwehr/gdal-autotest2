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
// Tests internal reference-counted object wrappers (H5UC_create, H5UC_decr,
// H5UC_INC, H5UC_DEC, H5UC_GET_OBJ) and public HDF5 callers that share v1
// B-tree metadata through H5UC_t (third_party/hdf5/src/H5UC.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5UC.c
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5UCprivate.h

// clang-format off
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
// clang-format on

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

extern "C" {
#include "third_party/hdf5/src/H5UCprivate.h"
}  // extern "C"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

struct TestPayload {
  int value = 0;
  int* free_calls = nullptr;
  void** last_freed = nullptr;
  herr_t free_rc = 0;
};

herr_t FreePayload(void* obj) {
  auto* payload = static_cast<TestPayload*>(obj);
  if (payload->free_calls != nullptr) {
    ++(*payload->free_calls);
  }
  if (payload->last_freed != nullptr) {
    *payload->last_freed = obj;
  }
  const herr_t rc = payload->free_rc;
  delete payload;
  return rc;
}

// Releases any remaining references on early ASSERT_* failure without
// double-decrementing once the final reference drops to zero.
class ScopedUseCount {
 public:
  explicit ScopedUseCount(H5UC_t* rc = nullptr) : rc_(rc) {}
  ~ScopedUseCount() { Reset(); }

  ScopedUseCount(const ScopedUseCount&) = delete;
  ScopedUseCount& operator=(const ScopedUseCount&) = delete;

  ScopedUseCount(ScopedUseCount&& other) noexcept
      : rc_(std::exchange(other.rc_, nullptr)) {}

  ScopedUseCount& operator=(ScopedUseCount&& other) noexcept {
    if (this != &other) {
      Reset();
      rc_ = std::exchange(other.rc_, nullptr);
    }
    return *this;
  }

  H5UC_t* get() const { return rc_; }

  void Inc() {
    if (rc_ != nullptr) {
      H5UC_INC(rc_);
    }
  }

  herr_t Dec() {
    if (rc_ == nullptr) {
      return -1;
    }
    const bool is_last = (rc_->n == 1);
    const herr_t status = H5UC_decr(rc_);
    if (is_last) {
      rc_ = nullptr;
    }
    return status;
  }

  herr_t DecMacro() {
    if (rc_ == nullptr) {
      return -1;
    }
    const bool is_last = (rc_->n == 1);
    const herr_t status = H5UC_DEC(rc_);
    if (is_last) {
      rc_ = nullptr;
    }
    return status;
  }

  void Reset() {
    while (rc_ != nullptr) {
      Dec();
    }
  }

 private:
  H5UC_t* rc_ = nullptr;
};

TEST(H5UcTest, CreateInitializesFieldsAndSingleDecrementFreesObject) {
  ASSERT_GE(H5open(), 0);

  int free_calls = 0;
  void* last_freed = nullptr;
  auto owned_payload = std::make_unique<TestPayload>(TestPayload{
      .value = 17,
      .free_calls = &free_calls,
      .last_freed = &last_freed,
      .free_rc = 0,
  });
  TestPayload* raw_payload = owned_payload.get();

  H5UC_t* raw_rc = H5UC_create(raw_payload, FreePayload);
  ASSERT_NE(raw_rc, nullptr);
  owned_payload.release();
  ScopedUseCount rc(raw_rc);

  EXPECT_EQ(H5UC_GET_OBJ(rc.get()), raw_payload);
  EXPECT_EQ(rc.get()->o, raw_payload);
  EXPECT_EQ(rc.get()->n, 1U);
  EXPECT_EQ(rc.get()->free_func, &FreePayload);
  EXPECT_EQ(free_calls, 0);

  EXPECT_GE(rc.Dec(), 0);
  EXPECT_EQ(rc.get(), nullptr);
  EXPECT_EQ(free_calls, 1);
  EXPECT_EQ(last_freed, raw_payload);
}

TEST(H5UcTest, IncrementAndDecrementDeferReleaseUntilZero) {
  ASSERT_GE(H5open(), 0);

  int free_calls = 0;
  void* last_freed = nullptr;
  auto owned_payload = std::make_unique<TestPayload>(TestPayload{
      .value = 42,
      .free_calls = &free_calls,
      .last_freed = &last_freed,
      .free_rc = 0,
  });
  TestPayload* raw_payload = owned_payload.get();

  H5UC_t* raw_rc = H5UC_create(raw_payload, FreePayload);
  ASSERT_NE(raw_rc, nullptr);
  owned_payload.release();
  ScopedUseCount rc(raw_rc);

  // 1. Increment twice to reach a reference count of 3.
  rc.Inc();
  EXPECT_EQ(rc.get()->n, 2U);
  rc.Inc();
  EXPECT_EQ(rc.get()->n, 3U);
  EXPECT_EQ(free_calls, 0);

  // 2. Non-final decrements leave the wrapped object intact.
  EXPECT_GE(rc.DecMacro(), 0);
  EXPECT_EQ(rc.get()->n, 2U);
  EXPECT_EQ(free_calls, 0);
  EXPECT_EQ(static_cast<const TestPayload*>(H5UC_GET_OBJ(rc.get()))->value, 42);

  EXPECT_GE(rc.Dec(), 0);
  EXPECT_EQ(rc.get()->n, 1U);
  EXPECT_EQ(free_calls, 0);
  EXPECT_EQ(static_cast<const TestPayload*>(H5UC_GET_OBJ(rc.get()))->value, 42);

  // 3. Dropping the final reference invokes free_func exactly once.
  EXPECT_GE(rc.DecMacro(), 0);
  EXPECT_EQ(rc.get(), nullptr);
  EXPECT_EQ(free_calls, 1);
  EXPECT_EQ(last_freed, raw_payload);
}

TEST(H5UcTest, DecrementPropagatesFreeCallbackFailureAndReleasesWrapper) {
  ASSERT_GE(H5open(), 0);

  int free_calls = 0;
  void* last_freed = nullptr;
  auto owned_payload = std::make_unique<TestPayload>(TestPayload{
      .value = -7,
      .free_calls = &free_calls,
      .last_freed = &last_freed,
      .free_rc = -1,
  });
  TestPayload* raw_payload = owned_payload.get();

  H5UC_t* raw_rc = H5UC_create(raw_payload, FreePayload);
  ASSERT_NE(raw_rc, nullptr);
  owned_payload.release();
  ScopedUseCount rc(raw_rc);

  rc.Inc();
  EXPECT_GE(rc.Dec(), 0);
  EXPECT_EQ(rc.get()->n, 1U);
  EXPECT_EQ(free_calls, 0);

  // On final decrement, H5UC_decr frees the H5UC_t node even when free_func
  // fails, pushes an H5E_RS / H5E_CANTFREE error, and returns FAIL (< 0).
  ScopedErrorSilence silence;
  EXPECT_LT(rc.Dec(), 0);
  EXPECT_EQ(rc.get(), nullptr);
  EXPECT_EQ(free_calls, 1);
  EXPECT_EQ(last_freed, raw_payload);
  EXPECT_GT(H5Eget_num(H5E_DEFAULT), 0);
}

TEST(H5UcTest, FreeListReuseAndGarbageCollectionAcrossMultipleWrappers) {
  ASSERT_GE(H5open(), 0);

  constexpr size_t kBatchSize = 32;
  int free_calls = 0;
  std::vector<ScopedUseCount> active;
  active.reserve(kBatchSize);

  // 1. Allocate a batch concurrently so H5UC_t free list grows beyond one node.
  for (size_t i = 0; i < kBatchSize; ++i) {
    auto owned_payload = std::make_unique<TestPayload>(TestPayload{
        .value = static_cast<int>(i),
        .free_calls = &free_calls,
        .last_freed = nullptr,
        .free_rc = 0,
    });
    H5UC_t* raw_rc = H5UC_create(owned_payload.get(), FreePayload);
    ASSERT_NE(raw_rc, nullptr);
    owned_payload.release();
    active.emplace_back(raw_rc);
    EXPECT_EQ(static_cast<const TestPayload*>(H5UC_GET_OBJ(raw_rc))->value,
              static_cast<int>(i));
  }
  EXPECT_EQ(free_calls, 0);

  // 2. Release all nodes back onto H5UC's static free list, then reuse them.
  active.clear();
  EXPECT_EQ(free_calls, static_cast<int>(kBatchSize));

  for (size_t i = 0; i < kBatchSize; ++i) {
    auto owned_payload = std::make_unique<TestPayload>(TestPayload{
        .value = static_cast<int>(i + 100),
        .free_calls = &free_calls,
        .last_freed = nullptr,
        .free_rc = 0,
    });
    H5UC_t* raw_rc = H5UC_create(owned_payload.get(), FreePayload);
    ASSERT_NE(raw_rc, nullptr);
    owned_payload.release();
    ScopedUseCount rc(raw_rc);
    EXPECT_GE(rc.Dec(), 0);
  }
  EXPECT_EQ(free_calls, static_cast<int>(kBatchSize * 2));
  EXPECT_GE(H5garbage_collect(), 0);
}

TEST(H5UcTest, PublicGroupsAndChunkedDatasetsUseRefCountedSharedMetadata) {
  // Public v1 symbol-table groups (H5Gnode.c) and v1 B-tree chunk indices
  // (H5Dbtree.c) allocate and share H5UC_t objects across B-tree nodes.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_V18), 0);
  ScopedFile file(
      H5Fcreate("h5uc_v1_public.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  ScopedGroup grp_a(
      H5Gcreate2(file.get(), "/grp_a", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp_a.valid());
  ScopedGroup grp_b(
      H5Gcreate2(file.get(), "/grp_b", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp_b.valid());

  constexpr std::array<hsize_t, 1> kDims = {8};
  constexpr std::array<hsize_t, 1> kChunkDims = {4};
  ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, kChunkDims.data()), 0);

  ScopedDataset dset(H5Dcreate2(file.get(), "/chunked", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, dcpl.get(),
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  constexpr std::array<int32_t, 8> kInput = {10, 20, 30, 40, 50, 60, 70, 80};
  ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, kInput.data()),
            0);

  std::array<int32_t, 8> output = {};
  output.fill(-1);
  ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    output.data()),
            0);
  EXPECT_THAT(output, ElementsAre(10, 20, 30, 40, 50, 60, 70, 80));
}

}  // namespace
}  // namespace hdf5
