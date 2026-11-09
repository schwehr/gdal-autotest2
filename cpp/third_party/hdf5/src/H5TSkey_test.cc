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
// Tests internal thread-local key creation and deletion wrappers
// (third_party/hdf5/src/H5TSkey.c). Built on POSIX pthreads in google3.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5TSkey.c

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/h5ts_c_shim.h"

namespace hdf5 {
namespace {

struct ThreadKeyData {
  std::atomic<int>* dtor_calls;
  std::atomic<int>* dtor_sum;
  int value;
};

void CleanupThreadKeyData(void* ptr) {
  auto* data = static_cast<ThreadKeyData*>(ptr);
  data->dtor_sum->fetch_add(data->value, std::memory_order_relaxed);
  data->dtor_calls->fetch_add(1, std::memory_order_relaxed);
  delete data;
}

TEST(H5TSkeyTest, CreateWithNullKeyFails) {
  EXPECT_LT(H5TS_key_create(nullptr, nullptr), 0);
}

TEST(H5TSkeyTest, CreateSetGetAndDeleteKey) {
  H5TS_key_t key{};
  ASSERT_GE(H5TS_key_create(&key, nullptr), 0);
  bool deleted = false;
  absl::Cleanup delete_key = [&] {
    if (!deleted) {
      H5TS_key_delete(key);
    }
  };

  void* value = reinterpret_cast<void*>(0x1);
  ASSERT_GE(H5TS_shim_key_get_value(key, &value), 0);
  EXPECT_EQ(value, nullptr);

  int sentinel = 123;
  ASSERT_GE(H5TS_shim_key_set_value(key, &sentinel), 0);
  ASSERT_GE(H5TS_shim_key_get_value(key, &value), 0);
  EXPECT_EQ(value, &sentinel);

  ASSERT_GE(H5TS_shim_key_set_value(key, nullptr), 0);
  EXPECT_GE(H5TS_key_delete(key), 0);
  deleted = true;
}

TEST(H5TSkeyTest, PerThreadIsolationAndDestructorInvocation) {
  H5TS_key_t key{};
  ASSERT_GE(H5TS_key_create(&key, CleanupThreadKeyData), 0);
  absl::Cleanup delete_key = [&] { H5TS_key_delete(key); };

  constexpr int kActiveThreads = 4;
  std::atomic<int> dtor_calls{0};
  std::atomic<int> dtor_sum{0};

  std::vector<std::thread> threads;
  threads.reserve(kActiveThreads + 1);
  for (int i = 1; i <= kActiveThreads; ++i) {
    threads.emplace_back([key, i, &dtor_calls, &dtor_sum]() {
      auto* data = new ThreadKeyData{&dtor_calls, &dtor_sum, i * 10};
      ASSERT_GE(H5TS_shim_key_set_value(key, data), 0);
      void* got = nullptr;
      ASSERT_GE(H5TS_shim_key_get_value(key, &got), 0);
      EXPECT_EQ(got, data);
    });
  }
  // A thread that never sets a non-null key value should not invoke dtor.
  threads.emplace_back([key]() {
    void* got = reinterpret_cast<void*>(0x1);
    ASSERT_GE(H5TS_shim_key_get_value(key, &got), 0);
    EXPECT_EQ(got, nullptr);
  });

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(dtor_calls.load(std::memory_order_relaxed), kActiveThreads);
  EXPECT_EQ(dtor_sum.load(std::memory_order_relaxed), 100);
}

TEST(H5TSkeyTest, KeyExhaustionAndInvalidDeleteFail) {
  std::vector<H5TS_key_t> keys;
  absl::Cleanup free_keys = [&] {
    for (H5TS_key_t k : keys) {
      H5TS_key_delete(k);
    }
  };

  bool hit_limit = false;
  for (int i = 0; i < 4096; ++i) {
    H5TS_key_t key{};
    if (H5TS_key_create(&key, nullptr) < 0) {
      hit_limit = true;
      break;
    }
    keys.push_back(key);
  }
  EXPECT_TRUE(hit_limit);
  EXPECT_FALSE(keys.empty());

  // Exceeding the valid key table range causes pthread_key_delete to fail.
  EXPECT_LT(H5TS_key_delete(static_cast<H5TS_key_t>(-1)), 0);
}

}  // namespace
}  // namespace hdf5
