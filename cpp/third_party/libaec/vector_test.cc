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
// Tests libaec internal dynamic size_t vector
// (third_party/libaec/src/vector.c). Note: Allocation failures invoke
// VECTOR_FATAL_ERROR (stderr + exit(1)).
//
// See also:
//   https://gitlab.dkrz.de/k202009/libaec/-/blob/main/src/vector.c
//   https://gitlab.dkrz.de/k202009/libaec/-/blob/main/src/vector.h

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <span>  // NOLINT
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"

// clang-format off
extern "C" {
#include "third_party/libaec/src/vector.h"
}  // extern "C"
// clang-format on

namespace libaec {
namespace {

using ::testing::ElementsAreArray;

// vector_destroy dereferences vec->values; std::unique_ptr avoids calling it on
// nullptr.
using VectorPtr =
    std::unique_ptr<vector_t, decltype([](vector_t* v) { vector_destroy(v); })>;

TEST(VectorTest, CreateAndDestroyStartsEmpty) {
  VectorPtr a(vector_create());
  VectorPtr b(vector_create());
  // Guard non-null for static analyzers; OOM aborts via VECTOR_FATAL_ERROR.
  ASSERT_NE(nullptr, a);
  ASSERT_NE(nullptr, b);

  EXPECT_EQ(0u, vector_size(a.get()));
  EXPECT_NE(nullptr, vector_data(a.get()));
  EXPECT_EQ(1, vector_equal(a.get(), b.get()));
}

TEST(VectorTest, PushBackGrowsPastInitialCapacity) {
  VectorPtr vec(vector_create());
  ASSERT_NE(nullptr, vec);

  // Exceed VECTOR_INITIAL_CAPACITY (128) and a second doubling (256) to
  // exercise both initial writes and multiple realloc expansions.
  constexpr size_t kNumElements = 300;
  std::vector<size_t> expected;
  expected.reserve(kNumElements);
  for (size_t i = 0; i < kNumElements; ++i) {
    const size_t value = i * 17 + 3;
    vector_push_back(vec.get(), value);
    expected.push_back(value);
  }

  ASSERT_EQ(kNumElements, vector_size(vec.get()));
  for (size_t i = 0; i < kNumElements; ++i) {
    EXPECT_EQ(expected[i], vector_at(vec.get(), i));
  }

  const std::span<const size_t> actual(vector_data(vec.get()),
                                       vector_size(vec.get()));
  EXPECT_THAT(actual, ElementsAreArray(expected));
}

TEST(VectorTest, EqualDetectsMatchesAndMismatches) {
  VectorPtr a(vector_create());
  VectorPtr b(vector_create());
  ASSERT_NE(nullptr, a);
  ASSERT_NE(nullptr, b);

  for (size_t v : {size_t{0}, size_t{42}, size_t{1024}}) {
    vector_push_back(a.get(), v);
    vector_push_back(b.get(), v);
  }
  EXPECT_EQ(1, vector_equal(a.get(), a.get()));
  EXPECT_EQ(1, vector_equal(a.get(), b.get()));

  // Mismatch on size alone.
  vector_push_back(b.get(), 9999);
  EXPECT_EQ(0, vector_equal(a.get(), b.get()));

  // Same size, mismatching middle element.
  VectorPtr c(vector_create());
  ASSERT_NE(nullptr, c);
  for (size_t v : {size_t{0}, size_t{43}, size_t{1024}, size_t{9999}}) {
    vector_push_back(c.get(), v);
  }
  EXPECT_EQ(0, vector_equal(b.get(), c.get()));
}

// TODO(schwehr): Cover VECTOR_FATAL_ERROR OOM branches via allocator fault
// injection if libaec adds a custom allocator hook.

}  // namespace
}  // namespace libaec
